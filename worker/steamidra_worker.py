#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Arachnel SteaMidra worker — Steam depot prep / download (no torrent).

Pipeline inspired by SteaMidra/SFF (GPL-3), reimplemented for Arachnel plugin IPC.
Does not vendor SFF Python modules.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import platform
import queue
import re
import shutil
import subprocess
import sys
from collections import deque
from typing import Any
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

from online_fix_embed import (
    analyze_store_categories,
    embed_online_fix,
    merge_genre_string,
)

# Windows consoles often default to cp1251/cp1252 — force UTF-8 for JSON IPC.
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# Arachnel Ryuu relay only (catalog + manifests). Override via settings.relayBaseUrl.
DEFAULT_RELAY_BASE = "https://ryuu.badkiko.ru"
DEFAULT_MANIFEST_CATALOG_URL = f"{DEFAULT_RELAY_BASE}/v1/catalog"


# When True, also print a 1 Hz human ticker on stderr (CLI testing + debug).
_cli_ticker = False
_cli_last_print = 0.0
_pct_eta_last_pct = 0.0
_pct_eta_last_t = 0.0
_pct_eta_rate = 0.0  # percent points per second
_progress_log_path: Path | None = None
_progress_phase = "download"  # download | verify


def _append_progress_log(line: str) -> None:
    """Append one full line (no \\r) so CLI tests can be audited after the run."""
    if _progress_log_path is None:
        return
    try:
        _progress_log_path.parent.mkdir(parents=True, exist_ok=True)
        ts = time.strftime("%H:%M:%S")
        with _progress_log_path.open("a", encoding="utf-8") as fh:
            fh.write(f"{ts} {line.rstrip()}\n")
    except OSError:
        pass


def _fmt_bytes(n: int | float) -> str:
    value = float(max(0, n))
    units = ("B", "KB", "MB", "GB", "TB")
    unit = 0
    while value >= 1024.0 and unit < len(units) - 1:
        value /= 1024.0
        unit += 1
    if unit == 0:
        return f"{int(value)} {units[unit]}"
    return f"{value:.1f} {units[unit]}"


# Below this, treat as idle — never show "1 B/s" from EMA decay crumbs.
MIN_DISPLAY_BPS = 8 * 1024


def _fmt_speed(bps: float) -> str:
    if bps < MIN_DISPLAY_BPS:
        return "—"
    return f"{_fmt_bytes(bps)}/s"


def _fmt_eta(remaining: int, bps: float) -> str:
    if bps <= 0 or remaining <= 0:
        return "—"
    seconds = int(remaining / bps)
    if seconds < 60:
        return f"{seconds} s"
    if seconds < 3600:
        return f"{seconds // 60} min"
    return f"{seconds // 3600} h"


def _trustworthy_total(downloaded: int, total: int) -> bool:
    if total <= 0 or downloaded < 0 or total < downloaded:
        return False
    # Never trust invented totals that blow past ~2× downloaded early on.
    return total <= max(downloaded * 2, downloaded + 256 * 1024 * 1024)


def transfer_detail(downloaded: int, total: int, bps: float, *, percent: int = 0) -> str:
    """Size · speed · ETA. Never invent a total — only show it when known+sane."""
    global _pct_eta_last_pct, _pct_eta_last_t, _pct_eta_rate
    now = time.monotonic()
    # Only estimate ETA from % when bytes are actually moving (real download).
    moving = bps >= MIN_DISPLAY_BPS
    if percent > 0 and moving:
        if _pct_eta_last_t > 0 and percent > _pct_eta_last_pct:
            dt = now - _pct_eta_last_t
            if dt >= 0.9:
                inst = (percent - _pct_eta_last_pct) / dt
                _pct_eta_rate = inst if _pct_eta_rate <= 0 else _pct_eta_rate * 0.6 + inst * 0.4
                _pct_eta_last_pct = float(percent)
                _pct_eta_last_t = now
        elif _pct_eta_last_t <= 0:
            _pct_eta_last_pct = float(percent)
            _pct_eta_last_t = now
        elif now - _pct_eta_last_t > 8.0:
            _pct_eta_rate = 0.0
    else:
        _pct_eta_rate = 0.0

    eta_from_pct = "—"
    if moving and _pct_eta_rate > 0.01 and percent < 100:
        rem_s = int(max(0.0, (100.0 - percent) / _pct_eta_rate))
        if rem_s < 60:
            eta_from_pct = f"{rem_s} s"
        elif rem_s < 3600:
            eta_from_pct = f"{rem_s // 60} min"
        else:
            eta_from_pct = f"{rem_s // 3600} h"

    if _trustworthy_total(downloaded, total):
        parts = [f"{_fmt_bytes(downloaded)} / {_fmt_bytes(total)}"]
        if bps >= MIN_DISPLAY_BPS:
            parts.append(_fmt_speed(bps))
            parts.append(f"ETA {_fmt_eta(max(0, total - downloaded), bps)}")
        elif eta_from_pct != "—":
            parts.append(f"ETA {eta_from_pct}")
        return " · ".join(parts)
    if downloaded > 0:
        parts = [_fmt_bytes(downloaded)]
        if bps >= MIN_DISPLAY_BPS:
            parts.append(_fmt_speed(bps))
            if eta_from_pct != "—":
                parts.append(f"ETA {eta_from_pct}")
        return " · ".join(parts)
    return "Downloading..."


def progress_kwargs(
    *,
    percent: int,
    detail: str,
    status: str = "downloading",
    downloaded: int = 0,
    total: int = 0,
    bps: float = 0.0,
) -> dict[str, Any]:
    return {
        "percent": percent,
        "detail": detail,
        "status": status,
        "bytesDownloaded": int(downloaded),
        "totalBytes": int(total) if _trustworthy_total(downloaded, total) else 0,
        "downloadRateBps": int(bps) if bps >= MIN_DISPLAY_BPS else 0,
    }


def _print_cli_ticker(
    percent: int,
    downloaded: int,
    total: int,
    bps: float,
    *,
    detail: str | None = None,
    force: bool = False,
) -> None:
    """Live \\r line on stderr + one appended line in progress log."""
    global _cli_last_print
    now = time.monotonic()
    if not force and now - _cli_last_print < 0.95:
        return
    _cli_last_print = now
    text = detail or transfer_detail(downloaded, total, bps, percent=percent)
    line = f"[{percent:3d}%] {text}"
    # Live terminal: overwrite same line.
    sys.stderr.write(f"\r{line:<90}")
    sys.stderr.flush()
    # Audit log: every tick is a NEW line (what you asked for).
    rate = int(bps) if bps >= MIN_DISPLAY_BPS else 0
    _append_progress_log(
        f"pct={percent} bytes={int(downloaded)} bps={rate} phase={_progress_phase} | {line}"
    )


def _emit_progress(enabled: bool, **kwargs: Any) -> None:
    if enabled:
        payload = {"type": "progress", **kwargs}
        sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
        sys.stdout.flush()
    if _cli_ticker or enabled or _progress_log_path is not None:
        _print_cli_ticker(
            int(kwargs.get("percent") or 0),
            int(kwargs.get("bytesDownloaded") or 0),
            int(kwargs.get("totalBytes") or 0),
            float(kwargs.get("downloadRateBps") or 0),
            detail=str(kwargs.get("detail") or "") or None,
            force=bool(kwargs.get("status") == "completed")
            or int(kwargs.get("percent") or 0) >= 100,
        )

def dir_byte_size(path: Path) -> int:
    """One-shot directory size (slow on large trees — prefer DirSizeTracker)."""
    total = 0
    stack = [path]
    while stack:
        current = stack.pop()
        try:
            with os.scandir(current) as entries:
                for entry in entries:
                    try:
                        if entry.is_file(follow_symlinks=False):
                            total += entry.stat(follow_symlinks=False).st_size
                        elif entry.is_dir(follow_symlinks=False):
                            stack.append(Path(entry.path))
                    except OSError:
                        pass
        except OSError:
            pass
    return total


def process_io_bytes(pid: int) -> tuple[int, int] | None:
    """Return process read/write transfer bytes without third-party modules."""
    if platform.system() == "Windows":
        try:
            import ctypes
            from ctypes import wintypes

            class IoCounters(ctypes.Structure):
                _fields_ = [
                    ("ReadOperationCount", ctypes.c_ulonglong),
                    ("WriteOperationCount", ctypes.c_ulonglong),
                    ("OtherOperationCount", ctypes.c_ulonglong),
                    ("ReadTransferCount", ctypes.c_ulonglong),
                    ("WriteTransferCount", ctypes.c_ulonglong),
                    ("OtherTransferCount", ctypes.c_ulonglong),
                ]

            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel32.OpenProcess.restype = wintypes.HANDLE
            kernel32.GetProcessIoCounters.argtypes = [
                wintypes.HANDLE,
                ctypes.POINTER(IoCounters),
            ]
            kernel32.GetProcessIoCounters.restype = wintypes.BOOL
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            handle = kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
            if not handle:
                return None
            try:
                counters = IoCounters()
                if not kernel32.GetProcessIoCounters(handle, ctypes.byref(counters)):
                    return None
                return int(counters.ReadTransferCount), int(counters.WriteTransferCount)
            finally:
                kernel32.CloseHandle(handle)
        except (AttributeError, OSError, ValueError):
            return None
    if platform.system() == "Linux":
        try:
            values: dict[str, int] = {}
            for line in Path(f"/proc/{pid}/io").read_text(encoding="ascii").splitlines():
                key, value = line.split(":", 1)
                values[key] = int(value.strip())
            return values.get("read_bytes", 0), values.get("write_bytes", 0)
        except (OSError, ValueError):
            return None
    return None


class DirSizeTracker:
    """Background folder size sampler with incremental file cache."""

    def __init__(self, root: Path, interval_s: float = 0.35) -> None:
        self._root = root
        self._interval_s = interval_s
        self._lock = threading.Lock()
        self._size = 0
        self._files: dict[str, int] = {}
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._worker, name="dir-size", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread.is_alive():
            self._thread.join(timeout=2.0)

    def snapshot(self) -> int:
        with self._lock:
            return self._size

    def _measure(self) -> int:
        if not self._root.is_dir():
            return 0
        seen: set[str] = set()
        total = 0
        updates = 0
        try:
            for dirpath, _dirnames, filenames in os.walk(self._root):
                for name in filenames:
                    path = os.path.join(dirpath, name)
                    seen.add(path)
                    try:
                        size = os.stat(path, follow_symlinks=False).st_size
                    except OSError:
                        continue
                    prev = self._files.get(path)
                    if prev is None:
                        self._files[path] = size
                        total += size
                    elif prev != size:
                        total += size - prev
                        self._files[path] = size
                    updates += 1
                    if updates % 32 == 0:
                        with self._lock:
                            self._size = sum(self._files.values())
            for path in list(self._files):
                if path not in seen:
                    total -= self._files.pop(path)
        except OSError:
            pass
        with self._lock:
            self._size = sum(self._files.values())
        return self._size

    def _worker(self) -> None:
        while not self._stop.is_set():
            try:
                measured = self._measure()
                with self._lock:
                    self._size = measured
            except Exception:
                pass
            self._stop.wait(self._interval_s)


def _emit_result(obj: dict[str, Any], progress_mode: bool) -> None:
    text = json.dumps(obj, ensure_ascii=False)
    if progress_mode:
        sys.stdout.write("RESULT:" + text + "\n")
    else:
        sys.stdout.write(text + "\n")
    sys.stdout.flush()


def load_settings(path: str | None) -> dict[str, Any]:
    defaults = {
        "relayBaseUrl": DEFAULT_RELAY_BASE,
        "relayClientToken": "",
        "manifestCatalogUrl": DEFAULT_MANIFEST_CATALOG_URL,
        "manifestCatalogTtlHours": 24,
        "steamPath": "",
        "preferMode": "ddmod",  # legacy; installs always use DDMod
        "depotOs": "windows",  # windows | linux — Linux host defaults to windows (Proton)
        "libraryFolder": "",
        "includeDlc": False,
        # Auto-embed FreeTP-style SteamFix when Store categories say Online PvP/Co-op/MMO.
        "autoEmbedOnlineFix": True,
        "onlineFixOnWeakMultiplayer": False,
        "onlineFixSourceDir": "",
    }
    if path:
        defaults["_settingsPath"] = path
    if not path:
        return defaults
    p = Path(path)
    if not p.is_file():
        p.parent.mkdir(parents=True, exist_ok=True)
        # Don't persist internal keys
        public = {k: v for k, v in defaults.items() if not k.startswith("_")}
        p.write_text(json.dumps(public, indent=2), encoding="utf-8")
        return defaults
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
        defaults.update({k: data.get(k, defaults[k]) for k in defaults if not k.startswith("_")})
    except Exception:
        pass
    return defaults



def _steam_path_looks_valid(path: Path) -> bool:
    return (path / "steam.sh").exists() or (path / "steam.exe").exists() or (path / "steamapps").is_dir()


def _steam_paths_from_windows_registry() -> list[Path]:
    """Resolve custom Steam installs (e.g. D:\\Media\\Steam) via Valve registry keys."""
    try:
        import winreg
    except ImportError:
        return []
    found: list[Path] = []
    keys = (
        (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", ("SteamPath", "InstallPath")),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", ("InstallPath", "SteamPath")),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", ("InstallPath", "SteamPath")),
    )
    for hive, subkey, names in keys:
        try:
            with winreg.OpenKey(hive, subkey) as key:
                for name in names:
                    try:
                        raw, _ = winreg.QueryValueEx(key, name)
                    except OSError:
                        continue
                    text = str(raw or "").strip()
                    if not text:
                        continue
                    path = Path(text)
                    if path not in found:
                        found.append(path)
        except OSError:
            continue
    return found


def find_steam_path(settings: dict[str, Any]) -> Path | None:
    configured = (settings.get("steamPath") or "").strip()
    if configured:
        cfg = Path(configured)
        if _steam_path_looks_valid(cfg):
            return cfg
        # Wrong manual path — keep searching (empty/unknown is fine).

    home = Path.home()
    candidates: list[Path] = []
    if platform.system() == "Windows":
        candidates.extend(_steam_paths_from_windows_registry())
        for env in ("PROGRAMFILES(X86)", "PROGRAMFILES"):
            root = os.environ.get(env)
            if root:
                candidates.append(Path(root) / "Steam")
        candidates.append(home / "Steam")
        # Registry-less fallback: common Steam Library folders
        for drive in ("C:", "D:", "E:"):
            candidates.append(Path(f"{drive}/Steam"))
            candidates.append(Path(f"{drive}/Program Files (x86)/Steam"))
            candidates.append(Path(f"{drive}/Media/Steam"))
    else:
        candidates.extend(
            [
                home / ".steam" / "steam",
                home / ".steam" / "debian-installation",
                home / ".local" / "share" / "Steam",
                home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
            ]
        )
    seen: set[Path] = set()
    for c in candidates:
        try:
            key = c.resolve()
        except OSError:
            key = c
        if key in seen:
            continue
        seen.add(key)
        if _steam_path_looks_valid(c):
            return c
    return None


def http_get_json(url: str, headers: dict[str, str] | None = None, timeout: int = 30) -> Any:
    req = urllib.request.Request(url, headers=headers or {"User-Agent": "Arachnel-SteaMidra/0.1"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", errors="replace"))


def http_get_bytes(url: str, headers: dict[str, str] | None = None, timeout: int = 120) -> bytes:
    req = urllib.request.Request(url, headers=headers or {"User-Agent": "Arachnel-SteaMidra/0.1"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()



def load_local_games_json(plugin_root: Path) -> list[dict[str, Any]]:
    for rel in ("config/games.json", "games.json", "worker/games.json"):
        path = plugin_root / rel
        if not path.is_file():
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            continue
        items = data if isinstance(data, list) else data.get("games") or []
        out: list[dict[str, Any]] = []
        for it in items:
            if not isinstance(it, dict):
                continue
            if it.get("steamAppId") or it.get("appid"):
                appid = str(it.get("steamAppId") or it.get("appid"))
                out.append(
                    {
                        "id": it.get("id") or f"steam-{appid}",
                        "title": it.get("title") or it.get("name") or f"App {appid}",
                        "steamAppId": appid,
                        "coverUrl": it.get("coverUrl")
                        or f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg",
                        "description": it.get("description") or "",
                        "sizeLabel": it.get("sizeLabel") or "",
                        "uploadDate": it.get("uploadDate") or "",
                        "version": it.get("version") or "",
                    }
                )
        if out:
            return out
    return []


def steam_store_search(query: str, limit: int = 20) -> list[dict[str, Any]]:
    q = urllib.parse.quote(query)
    url = f"https://store.steampowered.com/api/storesearch/?term={q}&l=english&cc=US"
    data = http_get_json(url)
    items = data.get("items") or []
    out: list[dict[str, Any]] = []
    for it in items[:limit]:
        appid = str(it.get("id") or "")
        if not appid:
            continue
        out.append(
            {
                "id": f"steam-{appid}",
                "title": it.get("name") or f"App {appid}",
                "steamAppId": appid,
                "coverUrl": f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg",
                "description": "",
                "sizeLabel": "",
                "version": "",
            }
        )
    return out


def steam_app_details(appid: str) -> dict[str, Any] | None:
    url = f"https://store.steampowered.com/api/appdetails?appids={appid}&l=english"
    data = http_get_json(url)
    block = data.get(str(appid)) or data.get(appid)
    if not block or not block.get("success"):
        return None
    d = block.get("data") or {}
    analysis = analyze_store_categories(d)
    genres = merge_genre_string("", analysis)
    # BundledFix (2) when we will auto-embed SteamFix; else PortableArchive (0).
    install_kind = 2 if analysis.get("needsOnlineFix") else 0
    return {
        "id": f"steam-{appid}",
        "title": d.get("name") or f"App {appid}",
        "steamAppId": str(appid),
        "coverUrl": f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg",
        "description": (d.get("short_description") or "")[:2000],
        "sizeLabel": "",
        "version": str(d.get("steam_appid") or appid),
        "genres": genres,
        "needsOnlineFix": bool(analysis.get("needsOnlineFix")),
        "steamFix": analysis.get("steamFix") or "none",
        "epicFix": analysis.get("epicFix") or "none",
        "onlineFixReason": analysis.get("reason") or "",
        "installKind": install_kind,
        "onlineAnalysis": analysis,
        "categoryLabels": list(analysis.get("categoryLabels") or []),
    }


SEED_APPIDS = [
    "570",
    "730",
    "440",
    "620",
    "400",
    "550",
    "105600",
    "892970",
]



def steam_featured_fallback(limit: int = 60) -> list[dict[str, Any]]:
    """Popular Steam titles when Hubcap key / local snapshot are unavailable."""
    out: list[dict[str, Any]] = []
    seen: set[str] = set()
    try:
        data = http_get_json("https://store.steampowered.com/api/featuredcategories/?l=english&cc=US")
        for key in ("top_sellers", "new_releases", "coming_soon", "specials"):
            block = data.get(key) or {}
            for it in block.get("items") or []:
                appid = str(it.get("id") or "")
                if not appid or appid in seen:
                    continue
                seen.add(appid)
                out.append(
                    {
                        "id": f"steam-{appid}",
                        "title": it.get("name") or f"App {appid}",
                        "steamAppId": appid,
                        "coverUrl": f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg",
                        "description": "",
                        "sizeLabel": "",
                        "version": "",
                    }
                )
                if len(out) >= limit:
                    return out
    except Exception:
        pass
    for appid in SEED_APPIDS:
        if appid in seen:
            continue
        try:
            g = steam_app_details(appid)
            if g:
                out.append(g)
                continue
        except Exception:
            pass
        out.append(
            {
                "id": f"steam-{appid}",
                "title": f"Steam App {appid}",
                "steamAppId": appid,
                "coverUrl": f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg",
            }
        )
    return out


def games_catalog_cache_path(settings: dict[str, Any], plugin_root: Path) -> Path:
    configured = (settings.get("_settingsPath") or "").strip()
    if configured:
        return Path(configured).parent / "games-depotkeys.json"
    return plugin_root / "config" / "games-depotkeys.json"


def build_catalog_from_depotkeys(settings: dict[str, Any]) -> list[dict[str, Any]]:
    """All Steam apps that have at least one public depot decryption key."""
    keys_db = ensure_depotkeys_db(settings)
    apps: dict[str, dict[str, Any]] = {}
    for _did, ent in keys_db.items():
        if not isinstance(ent, dict):
            continue
        key = str(ent.get("key") or "")
        if len(key) != 64:
            continue
        appid = str(ent.get("parent_appid") or "")
        if not appid.isdigit():
            continue
        title = str(ent.get("parent_name") or "").strip() or f"App {appid}"
        prev = apps.get(appid)
        if prev is None or (
            title
            and not title.startswith("App ")
            and len(title) >= len(prev.get("title") or "")
        ):
            apps[appid] = {
                "id": f"steam-{appid}",
                "title": title,
                "steamAppId": appid,
                "coverUrl": f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg",
                "description": "",
                "sizeLabel": "",
                "version": "",
            }
    return sorted(apps.values(), key=lambda g: (g.get("title") or "").lower())


def load_or_build_depotkeys_catalog(
    settings: dict[str, Any], plugin_root: Path, force: bool = False
) -> list[dict[str, Any]]:
    cache = games_catalog_cache_path(settings, plugin_root)
    keys_cache = depotkeys_cache_path(settings)
    if (
        not force
        and cache.is_file()
        and cache.stat().st_size > 100_000
        and keys_cache.is_file()
        and cache.stat().st_mtime >= keys_cache.stat().st_mtime
    ):
        try:
            data = json.loads(cache.read_text(encoding="utf-8"))
            games = data if isinstance(data, list) else data.get("games") or []
            if isinstance(games, list) and len(games) > 1000:
                return [g for g in games if isinstance(g, dict)]
        except Exception:
            pass

    games = build_catalog_from_depotkeys(settings)
    try:
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(
            json.dumps(
                {"games": games, "source": "depotkeys", "count": len(games)},
                ensure_ascii=False,
                separators=(",", ":"),
            ),
            encoding="utf-8",
        )
    except Exception:
        pass
    return games


def manifest_catalog_cache_path(settings: dict[str, Any], plugin_root: Path) -> Path:
    configured = (settings.get("_settingsPath") or "").strip()
    if configured:
        return Path(configured).expanduser().resolve().parent / "games-manifest-catalog.json"
    return (plugin_root / "games-manifest-catalog.json").resolve()


def normalize_catalog_game(raw: dict[str, Any]) -> dict[str, Any] | None:
    """Map federated catalog entry → SteaMidra plugin game row."""
    if not isinstance(raw, dict):
        return None
    delivery = str(raw.get("delivery") or "").strip().lower()
    source_id = str(raw.get("sourceId") or raw.get("source") or "").strip().lower()
    if delivery == "torrent" or source_id == "online-fix":
        return None
    appid = str(raw.get("steamAppId") or raw.get("appid") or "").strip()
    if not appid.isdigit():
        entry_id = str(raw.get("id") or "")
        if entry_id.startswith("steam-"):
            appid = entry_id[6:]
    if not appid.isdigit():
        return None
    if raw.get("hasManifest") is False:
        return None
    upload = str(raw.get("uploadDate") or raw.get("uploaded_date") or "").strip()
    version = str(raw.get("version") or "").strip() or (upload[:10] if upload else "")
    size = str(raw.get("fileSize") or raw.get("sizeLabel") or "").strip()
    title = str(raw.get("title") or raw.get("name") or f"App {appid}").strip()
    entry: dict[str, Any] = {
        "id": str(raw.get("id") or f"steam-{appid}"),
        "title": title,
        "steamAppId": appid,
        "coverUrl": raw.get("coverUrl")
        or f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg",
        "description": str(raw.get("description") or ""),
        "sizeLabel": size,
        "uploadDate": upload,
        "version": version,
        "hasManifest": True,
        "manifestSource": str(raw.get("manifestSource") or raw.get("manifest_source") or ""),
        "delivery": delivery or "steam-manifest",
        "sourceId": source_id or "steamidra",
        "genres": str(raw.get("genres") or ""),
    }
    needs_fix = raw.get("needsOnlineFix")
    if needs_fix is None:
        genres_l = entry["genres"].lower()
        needs_fix = "online fix" in genres_l or "online co-op" in genres_l or "online pvp" in genres_l
    entry["needsOnlineFix"] = bool(needs_fix)
    try:
        entry["installKind"] = int(raw.get("installKind"))
    except (TypeError, ValueError):
        entry["installKind"] = 2 if entry["needsOnlineFix"] else 0
    addons_in = raw.get("addons")
    if isinstance(addons_in, list) and addons_in:
        addons_out: list[dict[str, Any]] = []
        for addon in addons_in:
            if not isinstance(addon, dict):
                continue
            uris = addon.get("uris") if isinstance(addon.get("uris"), list) else []
            uris = [str(u) for u in uris if u]
            addons_out.append(
                {
                    "id": str(addon.get("id") or ""),
                    "title": str(addon.get("title") or "Fix"),
                    "kind": str(addon.get("kind") or "addon"),
                    "delivery": str(addon.get("delivery") or "direct"),
                    "fileSize": str(addon.get("fileSize") or ""),
                    "uploadDate": str(addon.get("uploadDate") or ""),
                    "uris": uris,
                    "referer": str(addon.get("referer") or ""),
                    "getfileUrl": str(addon.get("getfileUrl") or ""),
                    "optional": bool(addon.get("optional", True)),
                }
            )
        if addons_out:
            entry["addons"] = addons_out
    return entry


def parse_manifest_catalog_payload(data: Any) -> list[dict[str, Any]]:
    """Accept schema v1 {entries:[]} or Hydra {downloads:[]} or {games:[]}."""
    rows: list[Any] = []
    if isinstance(data, list):
        rows = data
    elif isinstance(data, dict):
        if isinstance(data.get("entries"), list):
            rows = data["entries"]
        elif isinstance(data.get("games"), list):
            rows = data["games"]
        elif isinstance(data.get("downloads"), list):
            rows = data["downloads"]
    games: list[dict[str, Any]] = []
    for raw in rows:
        if not isinstance(raw, dict):
            continue
        entry = normalize_catalog_game(raw)
        if entry:
            games.append(entry)
    return games


def download_manifest_catalog(url: str, dest: Path, *, max_age_s: float = 3600) -> bool:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.is_file() and max_age_s > 0:
        age = time.time() - dest.stat().st_mtime
        if age < max_age_s:
            return True
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "Arachnel-SteaMidra/0.3"},
    )
    try:
        # Relay catalog is ~20MB+; allow a long first fetch.
        with urllib.request.urlopen(req, timeout=300) as resp:
            payload = resp.read()
    except Exception:
        return dest.is_file()
    try:
        data = json.loads(payload.decode("utf-8", errors="replace"))
    except Exception:
        return dest.is_file()
    games = parse_manifest_catalog_payload(data)
    if len(games) < 10:
        return dest.is_file()
    doc = {
        "schemaVersion": 1,
        "catalogKind": "ryuu-relay" if "ryuu" in url else "installable-manifest",
        "updatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "count": len(games),
        "entries": games,
    }
    dest.write_text(json.dumps(doc, ensure_ascii=False), encoding="utf-8")
    return True


def load_manifest_catalog_file(path: Path, *, min_entries: int = 10) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return []
    games = parse_manifest_catalog_payload(data)
    return games if len(games) >= min_entries else []


def load_or_sync_manifest_catalog(
    settings: dict[str, Any], plugin_root: Path, *, force_download: bool = False
) -> list[dict[str, Any]]:
    """Fetch catalog from Arachnel Ryuu relay into local cache (no stale bundled dump)."""
    cache = manifest_catalog_cache_path(settings, plugin_root)
    url = str(settings.get("manifestCatalogUrl") or DEFAULT_MANIFEST_CATALOG_URL).strip()
    ttl = float(settings.get("manifestCatalogTtlHours") or 24) * 3600
    if not url:
        return load_manifest_catalog_file(cache, min_entries=1)

    # Reject stale GitLab dump left in cache from older plugin builds.
    if cache.is_file():
        try:
            peek = json.loads(cache.read_text(encoding="utf-8"))
            kind = str(peek.get("catalogKind") or "") if isinstance(peek, dict) else ""
            entries = peek.get("entries") if isinstance(peek, dict) else None
            count = len(entries) if isinstance(entries, list) else 0
            if kind in ("installable-manifest",) and count < 10000:
                cache.unlink(missing_ok=True)
                force_download = True
        except Exception:
            pass

    if force_download:
        download_manifest_catalog(url, cache, max_age_s=0)
    else:
        download_manifest_catalog(url, cache, max_age_s=ttl)

    return load_manifest_catalog_file(cache, min_entries=1)


def cmd_catalog_sync(settings: dict[str, Any], plugin_root: Path, *, force: bool = False) -> dict[str, Any]:
    games = load_or_sync_manifest_catalog(settings, plugin_root, force_download=force)
    cache = manifest_catalog_cache_path(settings, plugin_root)
    if games and cache.is_file():
        return {
            "ok": True,
            "catalogPath": str(cache.resolve()),
            "source": "ryuu-relay",
            "count": len(games),
            "games": [],
        }
    return {
        "ok": False,
        "error": "Relay catalog unavailable. Check relayBaseUrl / network.",
        "count": 0,
        "games": [],
    }


def cmd_catalog_seed(settings: dict[str, Any], plugin_root: Path) -> dict[str, Any]:
    """Pull catalog from Arachnel Ryuu relay."""
    manifest = load_or_sync_manifest_catalog(settings, plugin_root, force_download=False)
    if not manifest:
        manifest = load_or_sync_manifest_catalog(settings, plugin_root, force_download=True)
    if manifest:
        cache = manifest_catalog_cache_path(settings, plugin_root)
        path = cache if cache.is_file() else (plugin_root / "games-manifest-catalog.json")
        return {
            "ok": True,
            "games": [],
            "catalogPath": str(path.resolve()),
            "source": "ryuu-relay",
            "count": len(manifest),
        }
    return {
        "ok": False,
        "error": "Relay catalog unavailable. Check relayBaseUrl / network.",
        "games": [],
        "count": 0,
    }


def cmd_search(query: str, settings: dict[str, Any], plugin_root: Path) -> dict[str, Any]:
    q = query.strip()
    catalog = load_or_sync_manifest_catalog(settings, plugin_root)
    if not q:
        return {
            "ok": True,
            "games": catalog[:200],
            "source": "ryuu-relay",
            "count": len(catalog),
        }
    if q.isdigit():
        hit = [g for g in catalog if g.get("steamAppId") == q]
        if hit:
            return {"ok": True, "games": hit, "source": "ryuu-relay"}
        try:
            g = steam_app_details(q)
            if g:
                return {"ok": True, "games": [g], "source": "steam-appid"}
        except Exception:
            pass
    ql = q.lower()
    filtered = [
        g
        for g in catalog
        if ql in (g.get("title") or "").lower() or q == g.get("steamAppId")
    ]
    return {"ok": True, "games": filtered[:80], "source": "ryuu-relay"}


def cmd_app_details(appid: str) -> dict[str, Any]:
    g = steam_app_details(appid)
    if not g:
        return {"ok": False, "error": f"App {appid} not found"}
    return {"ok": True, "game": g}


# --- Lua / keys / manifests ---

ADDAPPID_RE = re.compile(
    r'addappid\(\s*(\d+)\s*(?:,\s*(\d+)\s*)?(?:,\s*["\']([0-9a-fA-F]{64})["\']\s*)?\)',
    re.I,
)
SETMANIFEST_RE = re.compile(
    r'setManifestid\(\s*(\d+)\s*,\s*["\']?(\d+)["\']?\s*(?:,|\))',
    re.I,
)


def parse_lua(text: str) -> dict[str, Any]:
    depots: dict[str, str] = {}
    manifests: dict[str, str] = {}
    appids: list[str] = []
    for m in ADDAPPID_RE.finditer(text):
        depot_id, _flag, key = m.group(1), m.group(2), m.group(3)
        appids.append(depot_id)
        if key:
            depots[depot_id] = key.lower()
    for m in SETMANIFEST_RE.finditer(text):
        manifests[m.group(1)] = m.group(2)
    return {"depots": depots, "manifests": manifests, "appids": appids, "raw": text}


def io_bytes_to_tmp(data: bytes, suffix: str = ".zip") -> Path:
    fd, name = tempfile.mkstemp(suffix=suffix)
    os.close(fd)
    p = Path(name)
    p.write_bytes(data)
    return p


def extract_lua_and_manifests(raw: bytes, staging: Path) -> str | None:
    """Return lua text if found; extract .manifest files into staging."""
    staging.mkdir(parents=True, exist_ok=True)
    lua_text: str | None = None
    if raw[:2] == b"PK":
        zpath = io_bytes_to_tmp(raw)
        try:
            with zipfile.ZipFile(zpath) as zf:
                for name in zf.namelist():
                    lower = name.lower()
                    data = zf.read(name)
                    base = Path(name).name
                    if lower.endswith(".lua") and lua_text is None:
                        lua_text = data.decode("utf-8", errors="replace")
                    if lower.endswith(".manifest"):
                        (staging / base).write_bytes(data)
        finally:
            zpath.unlink(missing_ok=True)
        return lua_text
    text = raw.decode("utf-8", errors="replace")
    if "addappid" in text.lower():
        return text
    return None


DEPOTKEYS_URLS = [
    "https://pub-d3ba7941fdf24c2c84da530b93221e1c.r2.dev/fallback_depotkeys.json",
    "https://raw.githubusercontent.com/KoriaPolis/Steam-Depot/main/fallback_depotkeys.json",
]


def depotkeys_cache_path(settings: dict[str, Any]) -> Path:
    # Prefer next to settings.json when possible
    configured = (settings.get("_settingsPath") or "").strip()
    if configured:
        return Path(configured).parent / "fallback_depotkeys.json"
    if platform.system() == "Windows":
        base = os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
        return Path(base) / "Arachnel" / "Arachnel" / "plugins" / "steamidra" / "fallback_depotkeys.json"
    xdg = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(xdg) / "Arachnel" / "plugins" / "steamidra" / "fallback_depotkeys.json"


def ensure_depotkeys_db(
    settings: dict[str, Any], progress: bool = False
) -> dict[str, Any]:
    """Load (and cache) the public depot decryption-key database used by oureveryday/SFF."""
    cache = depotkeys_cache_path(settings)
    cache.parent.mkdir(parents=True, exist_ok=True)
    if cache.is_file() and cache.stat().st_size > 1_000_000:
        try:
            return json.loads(cache.read_text(encoding="utf-8"))
        except Exception:
            pass
    _emit_progress(
        progress, percent=0, detail="Getting game info…", status="downloading"
    )
    last_err: Exception | None = None
    for url in DEPOTKEYS_URLS:
        try:
            raw = http_get_bytes(url, timeout=120)
            cache.write_bytes(raw)
            return json.loads(raw.decode("utf-8"))
        except Exception as exc:
            last_err = exc
            continue
    raise RuntimeError(f"Could not download depot key database: {last_err}")


def steamcmd_app_info(appid: str) -> dict[str, Any] | None:
    url = f"https://api.steamcmd.net/v1/info/{appid}"
    data = http_get_json(url, timeout=12)
    block = (data.get("data") or {}).get(str(appid)) or (data.get("data") or {}).get(appid)
    return block if isinstance(block, dict) else None


def build_lua_from_depotkeys(
    appid: str, settings: dict[str, Any], progress: bool = False
) -> str:
    """Build lua from the cached public depot-key DB only (no steamcmd.net).

    Fast and offline once fallback_depotkeys.json is on disk. Manifests are
    optional — DepotDownloader can resolve them when given depot keys.
    """
    keys_db = ensure_depotkeys_db(settings, progress=progress)
    title = f"App {appid}"
    keyed: list[tuple[str, str]] = []
    for did, ent in keys_db.items():
        if not isinstance(ent, dict):
            continue
        if str(ent.get("parent_appid") or "") != str(appid):
            continue
        key = str(ent.get("key") or "")
        if len(key) != 64:
            continue
        name = str(ent.get("parent_name") or "").strip()
        if name:
            title = name
        keyed.append((str(did), key))
    if not keyed:
        raise RuntimeError(
            f"No depot decryption keys for app {appid} in public key DB"
        )
    lines = [
        "-- MAIN APPLICATION",
        f"addappid({appid}) -- {title}",
        "",
        "-- MAIN APP DEPOTS",
    ]
    for did, key in sorted(keyed, key=lambda x: int(x[0]) if x[0].isdigit() else 0):
        lines.append(f'addappid({did}, 1, "{key}")')
    return "\n".join(lines) + "\n"


def build_lua_from_steamcmd(
    appid: str, settings: dict[str, Any], progress: bool = False
) -> str:
    """Generate stplug-in lua from Steam appinfo + public depot key DB (oureveryday-style)."""
    info = steamcmd_app_info(appid)
    if not info:
        raise RuntimeError("steamcmd.net: app info unavailable")

    common = info.get("common") or {}
    app_name = str(common.get("name") or f"App {appid}")
    depots_block = info.get("depots") or {}
    depot_ids = [str(d) for d in depots_block.keys() if str(d).isdigit()]
    if not depot_ids:
        raise RuntimeError("steamcmd.net: no depots for app")

    manifest_map: dict[str, str] = {}
    for did in depot_ids:
        dinfo = depots_block.get(did) or depots_block.get(int(did)) or {}
        if not isinstance(dinfo, dict):
            continue
        manifests = dinfo.get("manifests") or {}
        public = manifests.get("public") if isinstance(manifests, dict) else None
        if isinstance(public, dict) and str(public.get("gid") or "").isdigit():
            manifest_map[did] = str(public["gid"])
        elif isinstance(public, str) and public.isdigit():
            manifest_map[did] = public

    keys_db = ensure_depotkeys_db(settings, progress=progress)
    lines = ["-- MAIN APPLICATION", f"addappid({appid}) -- {app_name}", "", "-- MAIN APP DEPOTS"]
    keyed = 0
    for did in sorted(depot_ids, key=lambda x: int(x)):
        ent = keys_db.get(did)
        key = ""
        name = f"Depot {did}"
        if isinstance(ent, dict):
            key = str(ent.get("key") or "").lower()
            name = str(ent.get("name") or name)
        elif isinstance(ent, str):
            key = ent.lower()
        if not key or len(key) != 64:
            continue
        lines.append(f'addappid({did}, 1, "{key}") -- {name}')
        if did in manifest_map:
            lines.append(f'setManifestid({did}, "{manifest_map[did]}")')
        keyed += 1

    # Depot-less DLCs
    extended = info.get("extended") or {}
    listofdlc = extended.get("listofdlc") if isinstance(extended, dict) else ""
    dlc_ids = []
    if isinstance(listofdlc, str) and listofdlc.strip():
        dlc_ids = [x.strip() for x in listofdlc.split(",") if x.strip().isdigit() and x.strip() != appid]

    if dlc_ids:
        lines.extend(["", "-- DLC (no depot)"])
        for dlc in dlc_ids:
            if dlc in depot_ids:
                continue
            lines.append(f"addappid({dlc})")

    if keyed == 0:
        raise RuntimeError(
            f"No depot decryption keys for app {appid} in public key DB "
            f"({len(depot_ids)} depot(s) known)."
        )
    return "\n".join(lines) + "\n"


def fetch_lua(
    appid: str,
    settings: dict[str, Any],
    staging: Path,
    progress: bool = False,
) -> str:
    """Fetch lua + .manifest package from Arachnel Ryuu relay only."""

    def _prep(msg: str) -> None:
        _emit_progress(progress, percent=0, detail=msg, status="downloading")

    def _from_package(raw: bytes) -> str | None:
        lua = extract_lua_and_manifests(raw, staging)
        if not lua or "addappid" not in lua.lower():
            return None
        return lua

    def _has_manifests() -> bool:
        return any(staging.glob("*.manifest"))

    relay = str(settings.get("relayBaseUrl") or DEFAULT_RELAY_BASE).strip().rstrip("/")
    if not relay:
        raise RuntimeError("relayBaseUrl is empty")

    headers = {"User-Agent": "Arachnel-SteaMidra/0.3", "Accept": "*/*"}
    token = str(settings.get("relayClientToken") or "").strip()
    if token:
        headers["X-Arachnel-Token"] = token

    errors: list[str] = []
    _prep("Relay…")
    for file_type in ("manifest", "lua"):
        try:
            _prep(f"Relay ({file_type})…")
            raw = http_get_bytes(
                f"{relay}/v1/download/{appid}?file_type={file_type}",
                headers=headers,
                timeout=120,
            )
            if file_type == "lua":
                text_lua = raw.decode("utf-8", errors="replace")
                if "addappid" not in text_lua.lower():
                    errors.append("relay lua: invalid")
                    continue
                (staging / f"{appid}.lua").write_text(text_lua, encoding="utf-8")
                if _has_manifests() or "setmanifestid" in text_lua.lower():
                    return text_lua
                errors.append("relay lua: no manifests")
                continue
            lua = _from_package(raw)
            if lua and (_has_manifests() or "setmanifestid" in lua.lower()):
                return lua
            if lua:
                errors.append("relay zip: lua without manifests")
            else:
                errors.append("relay zip: no lua")
        except Exception as exc:
            errors.append(f"relay {file_type}: {exc}")

    detail = "; ".join(errors[-4:]) if errors else "unknown"
    raise RuntimeError(
        f"Relay could not prepare app {appid} ({detail}). "
        "Check https://ryuu.badkiko.ru and RYUU_AUTH_KEY on the relay host."
    )



def write_stplugin_lua(steam: Path, appid: str, lua: str) -> Path:
    dest_dir = steam / "config" / "stplug-in"
    dest_dir.mkdir(parents=True, exist_ok=True)
    path = dest_dir / f"{appid}.lua"
    path.write_text(lua, encoding="utf-8")
    return path


def patch_config_vdf_keys(steam: Path, depot_keys: dict[str, str]) -> int:
    """Insert DecryptionKey entries under InstallConfigStore/.../depots in config.vdf."""
    cfg = steam / "config" / "config.vdf"
    if not depot_keys:
        return 0
    # Always write sidecar for DDMod
    keys_file = steam / "config" / "arachnel_depot_keys.txt"
    keys_file.write_text(
        "\n".join(f"{did};{key}" for did, key in depot_keys.items()) + "\n", encoding="utf-8"
    )
    if not cfg.exists():
        return 0

    text = cfg.read_text(encoding="utf-8", errors="replace")
    backup = steam / "config" / "config.vdf.arachnel-backup"
    if not backup.exists():
        shutil.copy2(cfg, backup)

    added = 0
    for depot_id, key in depot_keys.items():
        # Skip if already present
        if re.search(rf'"{re.escape(depot_id)}"\s*\{{[^}}]*"DecryptionKey"', text, re.I | re.S):
            continue
        block = f'\t\t\t\t"{depot_id}"\n\t\t\t\t{{\n\t\t\t\t\t"DecryptionKey"\t\t"{key}"\n\t\t\t\t}}\n'
        # Find "depots" section
        m = re.search(r'("depots"\s*\{)', text, re.I)
        if not m:
            # Insert under Steam { ... }
            steam_m = re.search(r'("Steam"\s*\{)', text, re.I)
            if not steam_m:
                continue
            insert_at = steam_m.end()
            text = text[:insert_at] + '\n\t\t\t"depots"\n\t\t\t{\n' + block + "\t\t\t}\n" + text[insert_at:]
            added += 1
            continue
        insert_at = m.end()
        text = text[:insert_at] + "\n" + block + text[insert_at:]
        added += 1

    if added:
        cfg.write_text(text, encoding="utf-8")
    return added


def copy_manifests_to_depotcache(steam: Path, staging: Path) -> int:
    targets = [
        steam / "steamapps" / "depotcache",
        steam / "config" / "depotcache",
        steam / "depotcache",
    ]
    for t in targets:
        t.mkdir(parents=True, exist_ok=True)
    count = 0
    for src in staging.glob("*.manifest"):
        for t in targets:
            shutil.copy2(src, t / src.name)
        count += 1
    return count


# --- Inject setup ---


def _copy_lumacore_dlls(src_root: Path, steam: Path) -> int:
    wanted = {"dwmapi.dll", "xinput1_4.dll", "lumacore.dll", "lumacorepayload.dll"}
    found = 0
    for p in src_root.rglob("*"):
        if p.is_file() and p.name.lower() in wanted:
            shutil.copy2(p, steam / p.name)
            found += 1
    return found


def _lumacore_bundle_dir(plugin_root: Path | None) -> Path | None:
    """Return a bundled LumaCore dir that actually contains DLLs (not just README)."""
    if not plugin_root:
        return None
    wanted = {"dwmapi.dll", "xinput1_4.dll", "lumacore.dll", "lumacorepayload.dll"}
    for cand in (
        plugin_root / "third_party" / "LumaCore",
        plugin_root / "third_party" / "windows" / "LumaCore",
    ):
        if not cand.is_dir():
            continue
        names = {p.name.lower() for p in cand.rglob("*") if p.is_file()}
        if names & wanted:
            return cand
    return None


def _pick_lumacore_asset_url(assets: list[Any]) -> str | None:
    """Prefer Release.zip; never grab Debug.zip first just because it is listed earlier."""
    zips: list[tuple[int, str]] = []
    for a in assets or []:
        name = (a.get("name") or "").lower()
        url = a.get("browser_download_url")
        if not url:
            continue
        if not (name.endswith(".zip") or "lumacore" in name):
            continue
        if "debug" in name:
            rank = 3
        elif "release" in name:
            rank = 0
        elif "lumacore" in name:
            rank = 1
        else:
            rank = 2
        zips.append((rank, str(url)))
    if not zips:
        return None
    zips.sort(key=lambda t: t[0])
    return zips[0][1]


def setup_lumacore(steam: Path, progress: bool, plugin_root: Path | None = None) -> None:
    (steam / "config" / "stplug-in").mkdir(parents=True, exist_ok=True)
    bundled = _lumacore_bundle_dir(plugin_root)
    if bundled and _copy_lumacore_dlls(bundled, steam) > 0:
        (steam / "config" / "arachnel_lumacore.json").write_text(
            json.dumps({"source": "bundled", "installed": True}, indent=2), encoding="utf-8"
        )
        _emit_progress(progress, percent=0, detail="Preparing Steam…", status="downloading")
        return

    _emit_progress(progress, percent=0, detail="Preparing Steam…", status="downloading")
    api = "https://api.github.com/repos/KoriaPolis/LumaCore/releases/latest"
    try:
        rel = http_get_json(
            api, headers={"User-Agent": "Arachnel-SteaMidra", "Accept": "application/vnd.github+json"}
        )
    except Exception as exc:
        raise RuntimeError(f"LumaCore release fetch failed: {exc}") from exc
    asset_url = _pick_lumacore_asset_url(list(rel.get("assets") or []))
    if not asset_url:
        raise RuntimeError("No LumaCore asset on latest release")
    raw = http_get_bytes(asset_url)
    tmp = Path(tempfile.mkdtemp(prefix="lumacore_"))
    try:
        zpath = tmp / "lc.zip"
        zpath.write_bytes(raw)
        with zipfile.ZipFile(zpath) as zf:
            zf.extractall(tmp / "out")
        if _copy_lumacore_dlls(tmp / "out", steam) == 0:
            raise RuntimeError("LumaCore zip had no expected DLLs")
        marker = steam / "config" / "arachnel_lumacore.json"
        marker.write_text(
            json.dumps(
                {
                    "tag": rel.get("tag_name"),
                    "installed": True,
                    "source": "github",
                    "asset": asset_url.rsplit("/", 1)[-1],
                },
                indent=2,
            ),
            encoding="utf-8",
        )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    _emit_progress(progress, percent=0, detail="Preparing Steam…", status="downloading")


SLSSTEAM_CONFIG_TEMPLATE = """# Arachnel-managed SLSsteam config (from AceSLS template)
DisableFamilyShareLock: yes
UseWhitelist: no
AppIds:
AdditionalApps:
DlcData:
AppTokens:
FakeOffline:
FakeAppIds:
ManifestIds:
DepotBlacklist:
IdleStatus:
  AppId: 0
  Title: ""
GameTitles:
SubscriptionTimestamps:
DenuvoGames:
MaxSchemaTries: 10
SafeMode: no
Notifications: yes
WarnHashMissmatch: no
NotifyInit: yes
API: no
DisableCloud: yes
DisableUpdates: no
FakeEmail: ""
FakeWalletBalance: 0
LogLevel: 2
ExtendedLogging: no
"""


def slssteam_paths() -> tuple[Path, Path]:
    share = Path.home() / ".local" / "share" / "SLSsteam"
    cfg_dir = Path.home() / ".config" / "SLSsteam"
    return share, cfg_dir


def ensure_slssteam_config() -> Path:
    """Ensure a complete AceSLS-compatible config.yaml exists (fixes Missing key(s))."""
    _, cfg_dir = slssteam_paths()
    cfg_dir.mkdir(parents=True, exist_ok=True)
    cfg = cfg_dir / "config.yaml"
    required = (
        "DisableFamilyShareLock",
        "AdditionalApps",
        "AppIds",
        "ManifestIds",
        "SafeMode",
        "NotifyInit",
        "Notifications",
    )
    text = cfg.read_text(encoding="utf-8") if cfg.is_file() else ""
    old_apps = re.findall(r"^\s*-\s*(\d+)\s*$", text, re.M) if text else []
    missing = [k for k in required if k not in text]
    stub = "PlayNotOwnedGames" in text and "AdditionalApps" not in text
    if missing or stub or not text.strip():
        cfg.write_text(SLSSTEAM_CONFIG_TEMPLATE, encoding="utf-8")
        text = cfg.read_text(encoding="utf-8")
        if old_apps:
            text = _yaml_ensure_list_ids(text, "AppIds", old_apps)
            text = _yaml_ensure_list_ids(text, "AdditionalApps", old_apps)
    replacements = {
        r"^SafeMode:\s*.*$": "SafeMode: no",
        r"^Notifications:\s*.*$": "Notifications: yes",
        r"^NotifyInit:\s*.*$": "NotifyInit: yes",
        r"^DisableFamilyShareLock:\s*.*$": "DisableFamilyShareLock: yes",
        r"^DisableUpdates:\s*.*$": "DisableUpdates: no",
    }
    for pattern, repl in replacements.items():
        if re.search(pattern, text, re.M):
            text = re.sub(pattern, repl, text, flags=re.M)
        else:
            text = text.rstrip() + "\n" + repl + "\n"
    cfg.write_text(text, encoding="utf-8")
    return cfg


def _yaml_ensure_list_ids(text: str, key: str, appids: list[str]) -> str:
    """Insert numeric ids under a top-level YAML list key (AppIds / AdditionalApps)."""
    m = re.search(rf"(^{re.escape(key)}:\s*\n)((?:^[ \t]+-.*\n)*)", text, re.M)
    if not m:
        m2 = re.search(rf"^{re.escape(key)}:\s*$", text, re.M)
        if not m2:
            text = text.rstrip() + f"\n{key}:\n"
            m2 = re.search(rf"^{re.escape(key)}:\s*$", text, re.M)
        insert_at = m2.end()
        block = "".join(f"  - {a}\n" for a in appids)
        return text[:insert_at] + "\n" + block + text[insert_at:]

    existing = set(re.findall(r"^\s*-\s*(\d+)\s*$", m.group(2), re.M))
    to_add = [a for a in appids if a not in existing]
    if not to_add:
        return text
    block = m.group(1) + m.group(2) + "".join(f"  - {a}\n" for a in to_add)
    return text[: m.start()] + block + text[m.end() :]


def _yaml_set_manifest_ids(text: str, manifests: dict[str, str]) -> str:
    if not manifests:
        return text
    m = re.search(r"^ManifestIds:\s*\n((?:^[ \t]+\d+\s*:.*\n)*)", text, re.M)
    existing: dict[str, str] = {}
    if m:
        for line in m.group(1).splitlines():
            mm = re.match(r"^\s*(\d+)\s*:\s*[\"']?(\d+)[\"']?\s*$", line)
            if mm:
                existing[mm.group(1)] = mm.group(2)
        start, end = m.start(), m.end()
    else:
        if not re.search(r"^ManifestIds:\s*$", text, re.M):
            text = text.rstrip() + "\nManifestIds:\n"
        m2 = re.search(r"^ManifestIds:\s*$", text, re.M)
        start, end = m2.start(), m2.end()
        if end < len(text) and text[end] == "\n":
            end += 1
        else:
            text = text[:end] + "\n" + text[end:]
            end += 1

    existing.update({str(k): str(v) for k, v in manifests.items()})
    body = "ManifestIds:\n" + "".join(
        f'  {k}: "{v}"\n' for k, v in sorted(existing.items(), key=lambda x: int(x[0]))
    )
    return text[:start] + body + text[end:]


def register_sls_app(appid: str, manifests: dict[str, str]) -> None:
    """Register app in SLSsteam AdditionalApps/AppIds + ManifestIds (required for licenses)."""
    cfg = ensure_slssteam_config()
    text = cfg.read_text(encoding="utf-8")
    text = _yaml_ensure_list_ids(text, "AppIds", [str(appid)])
    text = _yaml_ensure_list_ids(text, "AdditionalApps", [str(appid)])
    text = _yaml_set_manifest_ids(text, {str(k): str(v) for k, v in manifests.items()})
    cfg.write_text(text, encoding="utf-8")


def create_steam_cfg(steam: Path) -> None:
    cfg = steam / "steam.cfg"
    if cfg.exists():
        return
    cfg.write_text(
        "BootStrapperInhibitAll=enable\nBootStrapperForceSelfUpdate=disable\n",
        encoding="utf-8",
    )


def normalize_depot_manifest_bytes(raw: bytes) -> bytes | None:
    """CDN often returns a ZIP with one entry; unwrap to SteamKit v4 protobuf bytes."""
    if not raw or len(raw) < 16:
        return None
    data = raw
    if data[:2] == b"PK":
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as zf:
                names = [n for n in zf.namelist() if not n.endswith("/")]
                if not names:
                    return None
                data = zf.read(names[0])
        except Exception:
            return None
    # SteamKit PROTOBUF_PAYLOAD_MAGIC = 0x71F617D0 (v4)
    if len(data) > 8 and data[:4] in (b"\xd0\x17\xf6\x71", b"\xd5\x17\xf6\x71"):
        return data
    if len(data) > 100:
        return data
    return None


def try_download_manifests(steam: Path | None, manifests: dict[str, str], staging: Path) -> int:
    """Best-effort public mirrors for .manifest files into staging (+ Steam depotcache if present)."""
    if not manifests:
        return 0
    staging.mkdir(parents=True, exist_ok=True)
    got = 0
    for depot_id, manifest_id in manifests.items():
        dest_name = f"{depot_id}_{manifest_id}.manifest"
        existing = staging / dest_name
        if existing.is_file() and existing.stat().st_size > 100:
            # Re-normalize if a ZIP was cached earlier
            norm = normalize_depot_manifest_bytes(existing.read_bytes())
            if norm and norm != existing.read_bytes():
                existing.write_bytes(norm)
            got += 1
            continue
        raw = None
        # 1) request code mirrors → Steam CDN
        for url in (
            f"http://gmrc.wudrm.com/manifest/{manifest_id}",
            f"https://manifest.opensteamtool.com/{manifest_id}",
            f"https://manifest.steam.run/api/manifest/{manifest_id}",
        ):
            try:
                data = http_get_bytes(url, timeout=20)
            except Exception:
                continue
            text = data.decode("utf-8", errors="replace").strip()
            if text.isdigit() and len(text) < 40:
                for cdn in (
                    f"http://steampipe.akamaized.net/depot/{depot_id}/manifest/{manifest_id}/5/{text}",
                    f"https://steampipe.akamaized.net/depot/{depot_id}/manifest/{manifest_id}/5/{text}",
                ):
                    try:
                        payload = http_get_bytes(cdn, timeout=60)
                        if len(payload) > 100:
                            raw = payload
                            break
                    except Exception:
                        continue
                if raw:
                    break
            elif len(data) > 100 and not text.lower().startswith("<!doctype"):
                raw = data
                break
        if raw:
            normalized = normalize_depot_manifest_bytes(raw)
            if normalized:
                (staging / dest_name).write_bytes(normalized)
                got += 1
    if got and steam is not None:
        copy_manifests_to_depotcache(steam, staging)
    return got


def install_sls_wrappers() -> None:
    """Create AceSLS-style PATH wrappers so Steam always starts with LD_AUDIT."""
    share, _ = slssteam_paths()
    path_dir = share / "path"
    path_dir.mkdir(parents=True, exist_ok=True)
    inject = share / "library-inject.so"
    sls = share / "SLSsteam.so"
    if not inject.exists() or not sls.exists():
        return
    for name in ("steam", "steam-runtime", "steam-native"):
        target = None
        for cand in (Path("/usr/games") / name, Path("/usr/bin") / name):
            if cand.is_file() and cand.resolve().parent != path_dir:
                target = cand
                break
        if not target:
            continue
        wrapper = path_dir / name
        wrapper.write_text(
            "#!/bin/sh\n"
            f'export LD_AUDIT="{inject}:{sls}"\n'
            f'exec "{target}" "$@"\n',
            encoding="utf-8",
        )
        wrapper.chmod(0o755)
    # User desktop entry
    apps = Path.home() / ".local" / "share" / "applications"
    sys_desktop = Path("/usr/share/applications/steam.desktop")
    if sys_desktop.is_file():
        apps.mkdir(parents=True, exist_ok=True)
        dest = apps / "steam.desktop"
        text = sys_desktop.read_text(encoding="utf-8", errors="replace")
        text = re.sub(
            r"^Exec=.*$",
            f"Exec=env LD_AUDIT={inject}:{sls} /usr/games/steam %U",
            text,
            count=1,
            flags=re.M,
        )
        dest.write_text(text, encoding="utf-8")


def _install_sls_from_dir(src: Path, share: Path) -> int:
    copied = 0
    for name in ("SLSsteam.so", "library-inject.so"):
        p = src / name
        if p.is_file():
            shutil.copy2(p, share / name)
            copied += 1
    if copied:
        return copied
    for so in src.rglob("*.so"):
        shutil.copy2(so, share / so.name)
        copied += 1
    return copied


def _patch_steam_sh_ld_audit(steam: Path, inject_so: Path, sls_so: Path) -> None:
    steam_sh = steam / "steam.sh"
    if not steam_sh.is_file():
        return
    text = steam_sh.read_text(encoding="utf-8", errors="replace")
    marker = "# arachnel-steamidra-sls"
    ld_line = f"export LD_AUDIT={inject_so}:{sls_so}"
    if marker not in text:
        inject = f"{marker}\n{ld_line}\n"
        if text.startswith("#!"):
            nl = text.find("\n")
            text = text[: nl + 1] + inject + text[nl + 1 :]
        else:
            text = inject + text
        backup = steam / "steam.sh.arachnel-backup"
        if not backup.exists():
            shutil.copy2(steam_sh, backup)
        steam_sh.write_text(text, encoding="utf-8")
    elif "LD_AUDIT" not in text:
        text = text.replace(marker, marker + "\n" + ld_line)
        steam_sh.write_text(text, encoding="utf-8")


def setup_slssteam(steam: Path, progress: bool, plugin_root: Path | None = None) -> None:
    share, cfg_dir = slssteam_paths()
    share.mkdir(parents=True, exist_ok=True)
    cfg_dir.mkdir(parents=True, exist_ok=True)

    bundled = None
    if plugin_root:
        for cand in (
            plugin_root / "third_party" / "SLSsteam",
            plugin_root / "third_party" / "linux" / "SLSsteam",
        ):
            if cand.is_dir() and (cand / "SLSsteam.so").is_file():
                bundled = cand
                break

    if bundled and _install_sls_from_dir(bundled, share) > 0:
        _emit_progress(progress, percent=0, detail="Preparing Steam…", status="downloading")
    else:
        _emit_progress(progress, percent=0, detail="Preparing Steam…", status="downloading")
        api = "https://api.github.com/repos/AceSLS/SLSsteam/releases/latest"
        rel = http_get_json(
            api, headers={"User-Agent": "Arachnel-SteaMidra", "Accept": "application/vnd.github+json"}
        )
        asset_url = None
        for a in rel.get("assets") or []:
            name = a.get("name") or ""
            if "Any" in name and (name.endswith(".7z") or name.endswith(".zip")):
                asset_url = a.get("browser_download_url")
                break
            if name.endswith(".zip") and asset_url is None:
                asset_url = a.get("browser_download_url")
        if not asset_url:
            raise RuntimeError("No SLSsteam asset on latest release")
        raw = http_get_bytes(asset_url)
        tmp = Path(tempfile.mkdtemp(prefix="sls_"))
        try:
            archive = tmp / "sls.bin"
            archive.write_bytes(raw)
            out = tmp / "out"
            out.mkdir()
            if raw[:2] == b"PK":
                with zipfile.ZipFile(archive) as zf:
                    zf.extractall(out)
            else:
                seven = shutil.which("7z") or shutil.which("7za")
                if not seven:
                    raise RuntimeError("SLSsteam release is 7z — install p7zip (7z) to extract")
                subprocess.run([seven, "x", str(archive), f"-o{out}", "-y"], check=False, capture_output=True)
            if _install_sls_from_dir(out, share) == 0:
                raise RuntimeError("SLSsteam archive had no .so files")
            template = next(out.rglob("res/config.yaml"), None)
            cfg = cfg_dir / "config.yaml"
            if template and template.is_file() and (
                not cfg.exists()
                or "AdditionalApps" not in cfg.read_text(encoding="utf-8", errors="replace")
            ):
                shutil.copy2(template, cfg)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    ensure_slssteam_config()
    create_steam_cfg(steam)
    install_sls_wrappers()
    inject_so = share / "library-inject.so"
    sls_so = share / "SLSsteam.so"
    if inject_so.exists() and sls_so.exists():
        _patch_steam_sh_ld_audit(steam, inject_so, sls_so)
    _emit_progress(progress, percent=0, detail="Preparing Steam…", status="downloading")


def steam_tree_writable(steam: Path) -> bool:
    """True when Arachnel can create files under Steam (not Flatpak/read-only)."""
    try:
        cfg = steam / "config"
        cfg.mkdir(parents=True, exist_ok=True)
        probe = cfg / ".arachnel_write_probe"
        probe.write_text("ok", encoding="utf-8")
        probe.unlink(missing_ok=True)
        return True
    except OSError:
        return False


def ensure_inject(steam: Path | None, progress: bool, plugin_root: Path | None = None) -> bool:
    """Install SLS/Luma when Steam is present; no-op if Steam absent (ddmod-only)."""
    if steam is None:
        _emit_progress(progress, percent=0, detail="Preparing…", status="downloading")
        return False
    if not steam_tree_writable(steam):
        sys.stderr.write(f"ensure_inject: Steam path not writable, skipping inject: {steam}\n")
        _emit_progress(progress, percent=0, detail="Preparing…", status="downloading")
        return False
    try:
        if platform.system() == "Windows":
            freshly_installed = False
            if not (steam / "LumaCore.dll").exists() and not (steam / "dwmapi.dll").exists():
                setup_lumacore(steam, progress, plugin_root=plugin_root)
                freshly_installed = True
            else:
                _emit_progress(progress, percent=0, detail="Preparing Steam…", status="downloading")
            (steam / "config" / "stplug-in").mkdir(parents=True, exist_ok=True)
            return freshly_installed

        share, _ = slssteam_paths()
        if not (share / "SLSsteam.so").exists():
            setup_slssteam(steam, progress, plugin_root=plugin_root)
        else:
            # Refresh from bundled copies if newer/missing inject
            if plugin_root:
                for cand in (
                    plugin_root / "third_party" / "SLSsteam",
                    plugin_root / "third_party" / "linux" / "SLSsteam",
                ):
                    if cand.is_dir() and (cand / "SLSsteam.so").is_file():
                        _install_sls_from_dir(cand, share)
                        break
            ensure_slssteam_config()
            create_steam_cfg(steam)
            install_sls_wrappers()
            inject_so = share / "library-inject.so"
            sls_so = share / "SLSsteam.so"
            if inject_so.exists() and sls_so.exists():
                _patch_steam_sh_ld_audit(steam, inject_so, sls_so)
            _emit_progress(progress, percent=0, detail="Preparing Steam…", status="downloading")
        return False
    except Exception as exc:
        sys.stderr.write(f"ensure_inject: {exc}\n")
        _emit_progress(progress, percent=0, detail="Preparing…", status="downloading")
        return False


def write_acf(
    steam: Path,
    appid: str,
    title: str,
    installdir: str,
    size: int = 0,
    manifests: dict[str, str] | None = None,
    *,
    state_flags: str = "4",
    create_common_dir: bool = True,
) -> Path:
    """Write appmanifest. Default StateFlags 4 = Fully Installed (post-ddmod).

    When create_common_dir is False, only the .acf is written (game files live in
    the Arachnel library, not under steamapps/common).
    """
    import time

    lib = steam / "steamapps"
    lib.mkdir(parents=True, exist_ok=True)
    common = lib / "common" / installdir
    if create_common_dir:
        common.mkdir(parents=True, exist_ok=True)
    acf = lib / f"appmanifest_{appid}.acf"
    safe_title = title.replace('"', "'")
    depot_block = ""
    for depot_id, mid in (manifests or {}).items():
        depot_block += (
            f'\t\t"{depot_id}"\n\t\t{{\n'
            f'\t\t\t"manifest"\t\t"{mid}"\n'
            f'\t\t\t"size"\t\t"{size}"\n'
            f"\t\t}}\n"
        )
    body = f"""
"AppState"
{{
\t"appid"\t\t"{appid}"
\t"Universe"\t\t"1"
\t"name"\t\t"{safe_title}"
\t"StateFlags"\t\t"{state_flags}"
\t"installdir"\t\t"{installdir}"
\t"LastUpdated"\t\t"{int(time.time())}"
\t"SizeOnDisk"\t\t"{size}"
\t"BytesToDownload"\t\t"0"
\t"BytesDownloaded"\t\t"0"
\t"InstalledDepots"
\t{{
{depot_block}\t}}
\t"UserConfig"
\t{{
\t\t"language"\t\t"english"
\t}}
}}
""".strip()
    acf.write_text(body + "\n", encoding="utf-8")
    return common


def find_ddmod(plugin_root: Path) -> Path | None:
    os_dir = "windows" if platform.system() == "Windows" else "linux"
    candidates = [
        plugin_root / "third_party" / os_dir / "DDMod" / "DepotDownloaderMod.dll",
        plugin_root / "third_party" / "DDMod" / "DepotDownloaderMod.dll",
        plugin_root / "third_party" / "DepotDownloaderMod.dll",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def find_dotnet(plugin_root: Path | None = None) -> str | None:
    cands: list[str] = []
    if plugin_root:
        if platform.system() == "Windows":
            platform_dotnet = ("third_party/windows/dotnet/dotnet.exe",)
        else:
            platform_dotnet = ("third_party/linux/dotnet/dotnet",)
        for rel in (
            *platform_dotnet,
            "third_party/dotnet/dotnet",
            "third_party/dotnet/dotnet.exe",
        ):
            cands.append(str(plugin_root / rel))
    cands.extend(
        [
            str(Path.home() / ".dotnet" / "dotnet"),
            shutil.which("dotnet") or "",
            r"C:\Program Files\dotnet\dotnet.exe",
        ]
    )
    for cand in cands:
        if cand and Path(cand).exists():
            return cand
    return None


def check_dotnet9(plugin_root: Path | None = None) -> dict[str, Any]:
    dotnet = find_dotnet(plugin_root)
    if not dotnet:
        return {
            "ok": False,
            "dotnet": None,
            "hasNet9": False,
            "error": "dotnet not found (bundle third_party/dotnet or install .NET 9)",
        }
    try:
        env = os.environ.copy()
        # Prefer bundled DOTNET_ROOT next to the binary
        root = str(Path(dotnet).resolve().parent)
        env["DOTNET_ROOT"] = root
        proc = subprocess.run(
            [dotnet, "--list-runtimes"], capture_output=True, text=True, timeout=15, env=env
        )
        out = (proc.stdout or "") + (proc.stderr or "")
        has9 = any(
            "Microsoft.NETCore.App 9." in line or "Microsoft.AspNetCore.App 9." in line
            for line in out.splitlines()
        )
        # Bundled runtime-only install always has the shared framework beside dotnet
        if not has9 and (Path(root) / "shared" / "Microsoft.NETCore.App").is_dir():
            for child in (Path(root) / "shared" / "Microsoft.NETCore.App").iterdir():
                if child.name.startswith("9."):
                    has9 = True
                    break
        return {"ok": has9, "dotnet": dotnet, "hasNet9": has9, "runtimes": out.strip()}
    except Exception as exc:
        return {"ok": False, "dotnet": dotnet, "hasNet9": False, "error": str(exc)}


def resolve_depot_os(appid: str, settings: dict[str, Any] | None = None) -> str:
    """Pick DepotDownloader -os.

    Arachnel on Linux launches Windows builds via Proton, so default is always
    ``windows`` unless settings override ``depotOs``.
    """
    _ = appid
    configured = str((settings or {}).get("depotOs") or "").strip().lower()
    if configured in ("windows", "linux", "macos"):
        return configured
    return "windows"


def is_steamworks_redistributable(depot_id: str) -> bool:
    """Steamworks shared redistributables (VC++/DirectX) — not the game itself."""
    try:
        n = int(depot_id)
    except ValueError:
        return False
    # Common Steamworks redistributable depot id band.
    return 228980 <= n <= 229099


def _depot_oslist(depot_info: dict[str, Any]) -> str:
    cfg = depot_info.get("config") if isinstance(depot_info, dict) else None
    if isinstance(cfg, dict):
        return str(cfg.get("oslist") or "").lower()
    return ""


def filter_primary_depots(
    appid: str,
    depots: dict[str, str],
    manifests: dict[str, str],
    include_dlc: bool,
    *,
    depot_os: str | None = None,
    settings: dict[str, Any] | None = None,
) -> tuple[dict[str, str], dict[str, str]]:
    """Keep game depots only: drop the app id itself, redistributables, and DLC unless asked.

    Also drop depots whose Steam oslist does not match the chosen depot OS
    (Proton on Linux still wants windows depots — not host OS).
    """
    if not depots:
        return depots, manifests
    dlc_ids: set[str] = set()
    depot_meta: dict[str, Any] = {}
    target_os = (depot_os or resolve_depot_os(appid, settings)).lower()
    try:
        info = steamcmd_app_info(appid) or {}
        if not include_dlc:
            ext = info.get("extended") or {}
            for part in str(ext.get("listofdlc") or "").split(","):
                part = part.strip()
                if part.isdigit():
                    dlc_ids.add(part)
        raw_depots = info.get("depots") or {}
        if isinstance(raw_depots, dict):
            depot_meta = {str(k): v for k, v in raw_depots.items() if isinstance(v, dict)}
    except Exception:
        pass
    out_d: dict[str, str] = {}
    for k, v in depots.items():
        if k == appid:
            continue
        if is_steamworks_redistributable(k):
            continue
        if k in dlc_ids:
            continue
        oslist = _depot_oslist(depot_meta.get(k) or {})
        if oslist:
            # Shared / unmarked depots have empty oslist — keep them.
            if target_os not in oslist.split(","):
                continue
        out_d[k] = v
    out_m = {k: v for k, v in manifests.items() if k in out_d}
    if not out_d:
        # Fall back to original minus redistribs only — better than downloading nothing.
        out_d = {
            k: v
            for k, v in depots.items()
            if k != appid and not is_steamworks_redistributable(k)
        }
        out_m = {k: v for k, v in manifests.items() if k in out_d}
    if not out_d:
        return depots, manifests
    return out_d, out_m or {k: v for k, v in manifests.items() if k in out_d}


def strip_foreign_platform_content(
    install_path: Path, *, content_os: str | None = None
) -> list[str]:
    """Remove other-OS UE IoStore/paks that break the installed content OS.

    Repro: Chopping Trees (4512570) with ChoppingTrees-Linux.{pak,ucas,utoc} beside
    Windows IoStore → LowLevelFatalError null GTSM / DefaultPostProcessMaterial.

    ``content_os`` is the depot OS that was downloaded (usually windows under Proton),
    not necessarily the host OS.
    """
    root = Path(install_path)
    if not root.is_dir():
        return []
    os_name = (content_os or "windows").strip().lower()
    if os_name == "windows":
        foreign = ("-linux.", "_linux.", "-mac.", "_mac.", "-macos.", "_macos.")
    elif os_name == "linux":
        foreign = ("-windows.", "_windows.", "-win64.", "_win64.", "-mac.", "_mac.")
    else:
        return []

    removed: list[str] = []
    try:
        pak_dirs = list(root.rglob("Content"))
    except OSError:
        return []
    for content in pak_dirs:
        paks = content / "Paks"
        if not paks.is_dir():
            continue
        if "Engine" in content.parts and content.name == "Content":
            # Skip Engine/Content noise; game Content/Paks is what mounts at runtime.
            pass
        try:
            files = list(paks.iterdir())
        except OSError:
            continue
        for path in files:
            if not path.is_file():
                continue
            if path.suffix.lower() not in (".pak", ".ucas", ".utoc"):
                continue
            low = path.name.lower()
            if any(token in low for token in foreign):
                try:
                    path.unlink()
                    removed.append(str(path.relative_to(root)))
                except OSError as exc:
                    sys.stderr.write(f"strip_platform: keep {path.name} ({exc})\n")
    if removed:
        sys.stderr.write(
            "strip_platform: removed foreign IoStore/pak: " + ", ".join(removed) + "\n"
        )
    return removed


def default_library_root(settings: dict[str, Any]) -> Path:
    lib = (settings.get("libraryFolder") or "").strip()
    if lib:
        return Path(lib)
    return Path.home() / "Games" / "Arachnel" / "steamidra"


def find_game_exe(install_path: Path) -> Path | None:
    """Pick the real game binary (not GameLauncher / crash helpers)."""
    root = install_path.resolve()
    exes = [e for e in root.rglob("*.exe") if e.is_file()]
    if not exes:
        return None

    skip_sub = (
        "crash",
        "crashpad",
        "unitycrash",
        "redist",
        "prereq",
        "unins",
        "setup",
        "dotnet",
        "vcredist",
        "easyanticheat",
        "beclient",
        "uninstall",
        "arachnel",
        "createdump",
        "steamfix_bootstrap",
    )
    deprioritize = ("gamelauncher", "launcher", "bootstrap", "start", "play")

    def score(path: Path) -> tuple:
        name = path.name.lower()
        stem = path.stem.lower()
        if any(s in name for s in skip_sub):
            return (99, 0, 0, name)
        depth = len(path.relative_to(root).parts)
        # Prefer top-level game exe that matches folder / .pck / *_Data name.
        name_bonus = 0
        folder = root.name.lower().replace("steam-", "")
        if folder.isdigit():
            folder = ""
        if folder and (folder in stem or stem in folder):
            name_bonus += 50
        # Pratfall.pck / Sex With Friends_Data style anchors
        if (root / f"{path.stem}.pck").is_file():
            name_bonus += 80
        if (root / f"{path.stem}_Data").is_dir():
            name_bonus += 80
        if path.parent == root and list(path.parent.glob("*_Data")):
            # Unity root: prefer exe whose _Data exists
            if (root / f"{path.stem}_Data").is_dir():
                name_bonus += 40
        if any(s in stem for s in deprioritize):
            name_bonus -= 60
        if "shipping" in stem:
            name_bonus += 30
        try:
            size = path.stat().st_size
        except OSError:
            size = 0
        # Lower is better for depth; higher size/name_bonus wins via negatives.
        return (0 if path.parent == root else 1, depth, -name_bonus, -size, name)

    exes.sort(key=score)
    for e in exes:
        name = e.name.lower()
        if any(s in name for s in skip_sub):
            continue
        return e
    return exes[0]


# Partial downloads are usually far smaller; skip DDMod verify theater above this.
_ALREADY_INSTALLED_MIN_BYTES = 80 * 1024 * 1024


def already_installed_hint(install_path: Path, appid: str) -> dict[str, Any] | None:
    """Return meta if target looks fully installed (marker and/or exe + size)."""
    if not install_path.is_dir():
        return None
    exe = find_game_exe(install_path)
    if exe is None:
        return None
    marker = install_path / ".arachnel-steamidra"
    meta: dict[str, Any] = {}
    if marker.is_file():
        try:
            meta = json.loads(marker.read_text(encoding="utf-8"))
        except Exception:
            meta = {}
        if str(meta.get("appid") or "") not in ("", str(appid)):
            return None
        if str(meta.get("appid") or "") == str(appid):
            return meta
    # Interrupted install left files but no marker — don't re-verify for minutes.
    size = dir_byte_size(install_path)
    if size < _ALREADY_INSTALLED_MIN_BYTES:
        return None
    return {
        "appid": str(appid),
        "mode": "ddmod",
        "launch": {"type": "exe", "path": str(exe)},
        "inferred": True,
        "bytes": size,
    }


def launch_hint(steam: Path | None, appid: str, install_path: Path) -> dict[str, Any]:
    # Prefer local exe for depot/online-fix installs (steam:// skips winmm).
    exe = find_game_exe(install_path)
    if exe:
        args: list[str] = []
        # FreeTP Godot packs (Pratfall): "Pratfall.exe --steam"
        if (exe.parent / f"{exe.stem}.pck").is_file():
            args.append("--steam")
        hint: dict[str, Any] = {"type": "exe", "path": str(exe)}
        if args:
            hint["args"] = args
        return hint
    if steam is not None and (steam / "steamapps" / f"appmanifest_{appid}.acf").is_file():
        return {"type": "steam", "uri": f"steam://rungameid/{appid}"}
    return {"type": "folder", "path": str(install_path)}


def try_embed_online_fix(
    plugin_root: Path,
    install_path: Path,
    appid: str,
    settings: dict[str, Any],
    *,
    force: bool = False,
) -> dict[str, Any]:
    """Best-effort SteamFix overlay after depot install (UE Binaries/Win64 or Unity root)."""
    content_os = resolve_depot_os(appid, settings)
    try:
        strip_foreign_platform_content(Path(install_path), content_os=content_os)
    except Exception as exc:
        sys.stderr.write(f"strip_platform: {exc}\n")
    analysis: dict[str, Any] | None = None
    try:
        details = steam_app_details(str(appid))
        if details:
            analysis = details.get("onlineAnalysis")
    except Exception as exc:
        sys.stderr.write(f"online_fix: store lookup failed: {exc}\n")
    try:
        result = embed_online_fix(
            install_path,
            str(appid),
            plugin_root,
            settings,
            analysis=analysis,
            force=force,
        )
    except Exception as exc:
        sys.stderr.write(f"online_fix: embed failed: {exc}\n")
        return {"ok": False, "error": str(exc)}
    if result.get("error"):
        sys.stderr.write(f"online_fix: {result['error']}\n")
        # Persist failure on marker so UI/logs can show why fix is missing.
        try:
            marker = Path(install_path) / ".arachnel-steamidra"
            payload: dict[str, Any] = {}
            if marker.is_file():
                payload = json.loads(marker.read_text(encoding="utf-8"))
            payload["onlineFix"] = {"embedded": False, "error": result.get("error")}
            marker.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        except Exception:
            pass
    elif result.get("embedded"):
        n = len(result.get("targets") or [])
        epic = " + EpicFix" if result.get("epic") else ""
        sys.stderr.write(f"online_fix: embedded SteamFix{epic} into {n} path(s)\n")
    elif result.get("skipped"):
        sys.stderr.write(f"online_fix: skipped ({result.get('reason')})\n")
    return result


def run_ddmod(
    plugin_root: Path,
    appid: str,
    depot_keys: dict[str, str],
    manifests: dict[str, str],
    dest: Path,
    progress: bool,
    steam: Path | None = None,
    *,
    depot_os: str | None = None,
    settings: dict[str, Any] | None = None,
) -> None:
    dll = find_ddmod(plugin_root)
    if not dll:
        raise RuntimeError(
            "DepotDownloaderMod.dll missing — run tools/fetch_third_party.sh and rebuild the plugin"
        )
    net = check_dotnet9(plugin_root)
    if not net.get("hasNet9"):
        raise RuntimeError(
            "Bundled .NET 9 runtime missing — run tools/fetch_third_party.sh "
            f"({net.get('error') or 'Microsoft.NETCore.App 9.x not found'})"
        )
    dotnet = net["dotnet"]
    env = os.environ.copy()
    env["DOTNET_ROOT"] = str(Path(dotnet).resolve().parent)
    keys_tmp = Path(tempfile.gettempdir()) / "arachnel_mistwalker_keys.vdf"
    keys_tmp.write_text("\n".join(f"{d};{k}" for d, k in depot_keys.items()) + "\n", encoding="utf-8")
    dest.mkdir(parents=True, exist_ok=True)
    depots = list(depot_keys.keys()) or list(manifests.keys())
    if not depots:
        raise RuntimeError("No depot IDs — cannot run DepotDownloaderMod")
    depot_os = depot_os or resolve_depot_os(appid, settings)
    total = max(1, len(depots))
    pct_re = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*%")
    # Folder size is only a total-size hint: DDMod preallocates large files, so
    # logical size must never be used as downloaded bytes or download speed.
    size_tracker = DirSizeTracker(dest, interval_s=0.2)
    size_tracker.start()
    # Actual downloaded bytes/s come from the child process I/O counters.
    # Percent: DDMod is per-file — rate-limit display to +1%/s (no 12→26 jumps).
    speed_samples: deque[tuple[float, int]] = deque(maxlen=16)
    speed_samples.append((time.monotonic(), 0))
    speed_ema = 0.0
    pct_smooth = 1.0
    pct_last_t = time.monotonic()
    err_tail: list[str] = []
    last_emit_downloaded = -1
    depot_peak_local = 0.0
    completed_written = 0

    def _update_speed(now: float, downloaded: int) -> float:
        nonlocal speed_ema
        speed_samples.append((now, downloaded))
        while len(speed_samples) >= 2 and now - speed_samples[0][0] > 5.0:
            speed_samples.popleft()
        if len(speed_samples) < 2:
            return speed_ema if speed_ema >= MIN_DISPLAY_BPS else 0.0
        t0, b0 = speed_samples[0]
        t1, b1 = speed_samples[-1]
        dt = t1 - t0
        if dt < 0.9:
            return speed_ema if speed_ema >= MIN_DISPLAY_BPS else 0.0
        delta = b1 - b0
        if delta <= 0:
            # Idle / verifying — clear crumb speeds (avoids perpetual "1 B/s").
            speed_ema = 0.0
            return 0.0
        inst = delta / dt
        alpha = min(1.0, dt / 2.5)
        speed_ema = inst if speed_ema < MIN_DISPLAY_BPS else speed_ema * (1.0 - alpha) + inst * alpha
        if speed_ema < MIN_DISPLAY_BPS:
            speed_ema = 0.0
        return speed_ema

    def _smooth_pct(raw: float, *, verifying: bool = False) -> int:
        """Smooth DDMod %. Download: ~1.5%/s. Verify: catch up fast (no 100s crawl)."""
        nonlocal pct_smooth, pct_last_t
        now = time.monotonic()
        dt = max(0.05, now - pct_last_t)
        pct_last_t = now
        target = max(1.0, min(99.0, float(raw)))
        # Verifying already-on-disk files must not crawl at 1%/s for minutes.
        max_step = (20.0 if verifying else 1.5) * dt
        if target > pct_smooth:
            pct_smooth = min(target, pct_smooth + max_step)
        elif target < pct_smooth:
            # Never drop from DDMod per-file reset.
            pct_smooth = max(target, pct_smooth - 0.15 * dt)
        return max(1, min(99, int(round(pct_smooth))))

    def _emit_download(
        overall_pct: int,
        raw_downloaded: int,
        *,
        detail: str | None = None,
        reading: bool = False,
    ) -> None:
        global _progress_phase
        nonlocal last_emit_downloaded
        downloaded = max(0, raw_downloaded)
        bps = _update_speed(time.monotonic(), downloaded)
        stagnant = last_emit_downloaded >= 0 and downloaded <= last_emit_downloaded
        last_emit_downloaded = downloaded
        verifying = stagnant and reading and bps < MIN_DISPLAY_BPS
        _progress_phase = "verify" if verifying else "download"
        percent = _smooth_pct(overall_pct, verifying=verifying)
        total_hint = max(downloaded, size_tracker.snapshot())
        # Unlike downloaded bytes, DDMod's preallocated logical file size is a
        # useful total. Prefer byte progress once that total becomes available.
        if total_hint >= 64 * 1024 * 1024 and 0 < downloaded <= total_hint:
            percent = max(percent, min(99, int(downloaded * 99 / total_hint)))
        if detail is None:
            if verifying and percent >= 3:
                auto_detail = f"{_fmt_bytes(downloaded)} · verifying..."
            else:
                auto_detail = transfer_detail(downloaded, total_hint, bps, percent=percent)
        else:
            auto_detail = detail
        _emit_progress(
            progress,
            **progress_kwargs(
                percent=percent,
                detail=auto_detail,
                downloaded=downloaded,
                total=total_hint,
                bps=bps,
            ),
        )

    try:
        for i, depot_id in enumerate(depots):
            frac0 = i / total
            _emit_download(
                max(1, int(99 * frac0)),
                completed_written,
                detail="Downloading...",
            )
            cmd = [
                str(dotnet),
                str(dll),
                "-app",
                appid,
                "-depot",
                depot_id,
                "-depotkeys",
                str(keys_tmp),
                "-max-downloads",
                "64",
                "-dir",
                str(dest),
                "-os",
                depot_os,
            ]
            if depot_id in manifests:
                cmd += ["-manifest", manifests[depot_id]]
            # Always prefer a local .manifest when present (Steam CDN is 401 for anonymous).
            local_manifest: Path | None = None
            search_roots: list[Path] = []
            if steam:
                search_roots.append(steam / "steamapps" / "depotcache")
            for steam_root in (
                Path.home() / ".steam/steam",
                Path.home() / ".steam/debian-installation",
            ):
                search_roots.append(steam_root / "steamapps" / "depotcache")
            search_roots.append(Path(tempfile.gettempdir()))
            names: list[str] = [f"arachnel_dd_{depot_id}.manifest"]
            if depot_id in manifests:
                names.insert(0, f"{depot_id}_{manifests[depot_id]}.manifest")
            for root in search_roots:
                for name in names:
                    cand = root / name
                    if cand.is_file():
                        local_manifest = cand
                        break
                if local_manifest:
                    break
            if not local_manifest:
                for cand in Path(tempfile.gettempdir()).glob(f"{depot_id}_*.manifest"):
                    local_manifest = cand
                    break
            if local_manifest:
                norm = normalize_depot_manifest_bytes(local_manifest.read_bytes())
                if norm:
                    mf = Path(tempfile.gettempdir()) / f"arachnel_dd_{depot_id}.manifest"
                    mf.write_bytes(norm)
                    if depot_id not in manifests:
                        # Infer manifest id from filename when lua had no setManifestid.
                        stem = local_manifest.stem
                        if "_" in stem:
                            maybe = stem.split("_", 1)[1]
                            if maybe.isdigit():
                                cmd += ["-manifest", maybe]
                    elif "-manifest" not in cmd:
                        cmd += ["-manifest", manifests[depot_id]]
                    cmd += ["-manifestfile", str(mf)]

            # Stream stdout on a thread so UI ticks are not blocked by readline().
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=env,
                bufsize=1,
            )
            assert proc.stdout is not None
            line_queue: queue.Queue[str | None] = queue.Queue()

            def _stdout_reader() -> None:
                for out_line in proc.stdout:
                    line_queue.put(out_line)
                line_queue.put(None)

            threading.Thread(target=_stdout_reader, name="ddmod-stdout", daemon=True).start()

            io_start = process_io_bytes(proc.pid)
            read_start, write_start = io_start if io_start is not None else (0, 0)
            last_read = read_start
            proc_written_max = 0
            local_pct = 0.0
            depot_peak_local = 0.0
            last_emit = 0.0
            downloaded = 0
            while proc.poll() is None or not line_queue.empty():
                try:
                    line = line_queue.get(timeout=0.25)
                except queue.Empty:
                    line = ""

                if line is None:
                    break

                if line:
                    err_tail.append(line.rstrip())
                    if len(err_tail) > 40:
                        err_tail.pop(0)
                    m = pct_re.match(line)
                    if m:
                        try:
                            local_pct = float(m.group(1))
                            # DDMod resets to 0% on each file — keep peak so overall
                            # doesn't thrash within the depot.
                            depot_peak_local = max(depot_peak_local, min(local_pct, 100.0))
                        except ValueError:
                            pass

                now = time.monotonic()
                # Steady 1 Hz ticks — smooth % / speed for UI + CLI.
                if now - last_emit < 1.0 and proc.poll() is None:
                    continue
                last_emit = now

                io_now = process_io_bytes(proc.pid)
                reading = False
                if io_now is not None:
                    read_now, write_now = io_now
                    reading = read_now > last_read
                    last_read = read_now
                    proc_written_max = max(proc_written_max, max(0, write_now - write_start))
                downloaded = completed_written + proc_written_max
                # Blend depot index with a damped in-depot peak (not raw per-file %).
                frac = (i + depot_peak_local / 100.0) / total
                overall = max(1, min(99, int(99 * frac)))
                _emit_download(overall, downloaded, reading=reading)

            rc = proc.wait()
            if rc != 0:
                tail = "\n".join(err_tail)
                sys.stderr.write(("\n" if _cli_ticker or progress else "") + tail[-800:] + "\n")
                if "401" in tail or "No manifest request code" in tail:
                    raise RuntimeError(
                        "Steam blocked downloading game files (need a packaged manifest). "
                        "Try another title, or check the Ryuu relay."
                    )
                raise RuntimeError("Download failed. Try again or pick another game.")

            completed_written += proc_written_max
            downloaded = completed_written
            done_frac = (i + 1) / total
            _emit_download(max(1, min(99, int(99 * done_frac))), downloaded)
    finally:
        size_tracker.stop()
        if _cli_ticker or progress:
            sys.stderr.write("\n")
            sys.stderr.flush()


def cmd_install_addon(zip_path: str, dest_path: str) -> dict[str, Any]:
    """Extract a Ryuu fix zip into the game install directory (overlay)."""
    src = Path(zip_path)
    dest = Path(dest_path)
    if not src.is_file():
        return {"ok": False, "error": f"Add-on archive not found: {zip_path}"}
    if not dest.is_dir():
        return {"ok": False, "error": f"Game install path missing: {dest_path}"}
    try:
        if zipfile.is_zipfile(src):
            with zipfile.ZipFile(src) as zf:
                zf.extractall(dest)
        else:
            shutil.copy2(src, dest / src.name)
    except Exception as exc:
        return {"ok": False, "error": str(exc)}
    return {"ok": True, "installPath": str(dest), "addon": src.name}


def install_game(
    plugin_root: Path,
    appid: str,
    title: str,
    target: str,
    settings: dict[str, Any],
    mode: str,
    progress: bool,
    *,
    force: bool = False,
) -> dict[str, Any]:
    # mode kept for CLI/ABI compatibility; installs always use DDMod (Steam CDN).
    _ = mode
    steam = find_steam_path(settings)

    installdir = re.sub(r"[^\w\- ]+", "", title).strip() or f"App{appid}"
    installdir = installdir.replace(" ", "")[:40] or f"App{appid}"
    lib_folder = (settings.get("libraryFolder") or "").strip()
    if target:
        install_path_early = Path(target)
    elif lib_folder:
        install_path_early = Path(lib_folder) / installdir
    else:
        # Never default into Steam's C: library — use Arachnel library / ~/Games.
        install_path_early = default_library_root(settings) / f"{appid}-{installdir}"

    # Skip DepotDownloader verify theater when the game is already on disk.
    # DDMod re-checks every file (minutes for multi-GB) even when nothing downloads.
    if not force:
        prior = already_installed_hint(install_path_early, appid)
        if prior is not None:
            size = int(prior.get("bytes") or 0) or dir_byte_size(install_path_early)
            hint = launch_hint(steam, appid, install_path_early)
            marker = install_path_early / ".arachnel-steamidra"
            if not marker.is_file() or prior.get("inferred"):
                marker.write_text(
                    json.dumps(
                        {"appid": str(appid), "mode": "ddmod", "launch": hint},
                        indent=2,
                    ),
                    encoding="utf-8",
                )
            detail = f"Already installed · {_fmt_bytes(size)}"
            sys.stderr.write(detail + " (skip verify; use --force to re-download)\n")
            _emit_progress(
                progress,
                percent=100,
                detail=detail,
                status="completed",
                bytesDownloaded=size,
                totalBytes=size,
                downloadRateBps=0,
            )
            fix = try_embed_online_fix(plugin_root, install_path_early, appid, settings)
            out = {
                "ok": True,
                "installPath": str(install_path_early),
                "mode": "already-installed",
                "launch": hint,
                "steamPath": str(steam) if steam else "",
                "skippedDownload": True,
            }
            if fix:
                out["onlineFix"] = fix
            return out

    try:
        ensure_inject(steam, progress, plugin_root=plugin_root)
    except Exception as exc:
        sys.stderr.write(f"ensure_inject: {exc}\n")

    staging = Path(tempfile.mkdtemp(prefix=f"arachnel_sm_{appid}_"))
    try:
        _emit_progress(progress, percent=0, detail="Getting game info…", status="downloading")
        try:
            lua = fetch_lua(appid, settings, staging, progress=progress)
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

        parsed = parse_lua(lua)
        # Manifests bundled next to lua (relay zip) — recover IDs from filenames
        # even when the lua has keys only.
        for mf in staging.glob("*.manifest"):
            stem = mf.stem  # e.g. 391541_3413030246586915448
            if "_" not in stem:
                continue
            depot_id, mid = stem.split("_", 1)
            if depot_id.isdigit() and mid.isdigit():
                parsed["manifests"].setdefault(depot_id, mid)
        depot_os = resolve_depot_os(appid, settings)
        depots, manifests = filter_primary_depots(
            appid,
            parsed["depots"],
            parsed["manifests"],
            include_dlc=bool(settings.get("includeDlc")),
            depot_os=depot_os,
            settings=settings,
        )
        parsed = {**parsed, "depots": depots, "manifests": manifests}

        if steam:
            if steam_tree_writable(steam):
                try:
                    write_stplugin_lua(steam, appid, lua)
                    patch_config_vdf_keys(steam, parsed["depots"])
                    copy_manifests_to_depotcache(steam, staging)
                    try_download_manifests(steam, parsed["manifests"], staging)
                    if platform.system() != "Windows":
                        register_sls_app(appid, parsed["manifests"])
                        create_steam_cfg(steam)
                except Exception as exc:
                    sys.stderr.write(f"steam metadata write skipped: {exc}\n")
            else:
                sys.stderr.write(f"steam metadata write skipped (not writable): {steam}\n")
                try_download_manifests(None, parsed["manifests"], staging)
                for mf in staging.glob("*.manifest"):
                    norm = normalize_depot_manifest_bytes(mf.read_bytes())
                    if norm:
                        mf.write_bytes(norm)
                        shutil.copy2(mf, Path(tempfile.gettempdir()) / mf.name)
        else:
            # Still fetch manifests into staging for -manifestfile
            try_download_manifests(None, parsed["manifests"], staging)
            for mf in staging.glob("*.manifest"):
                norm = normalize_depot_manifest_bytes(mf.read_bytes())
                if norm:
                    mf.write_bytes(norm)
                    shutil.copy2(mf, Path(tempfile.gettempdir()) / mf.name)

        install_path = install_path_early
        if find_ddmod(plugin_root) is None:
            return {
                "ok": False,
                "error": "DepotDownloaderMod missing — run tools/fetch_third_party.sh and rebuild",
            }

        _emit_progress(progress, percent=0, detail="Downloading...", status="downloading")
        for mf in staging.glob("*.manifest"):
            try:
                shutil.copy2(mf, Path(tempfile.gettempdir()) / mf.name)
            except Exception:
                pass
        run_ddmod(
            plugin_root,
            appid,
            parsed["depots"],
            parsed["manifests"],
            install_path,
            progress,
            steam=steam,
            depot_os=depot_os,
            settings=settings,
        )
        try:
            strip_foreign_platform_content(install_path, content_os=depot_os)
        except Exception as exc:
            sys.stderr.write(f"strip_platform: {exc}\n")
        size = sum(f.stat().st_size for f in install_path.rglob("*") if f.is_file())
        # Keep the game in the user-selected Arachnel library. Do NOT copy into
        # Steam's steamapps/common (usually C:) — that doubled disk use and hijacked installPath.
        install_final = install_path
        # Optional: unlock metadata in Steam tree only — never move/copy game files there.
        # Skip appmanifest when the game lives outside steamapps/common (avoids empty
        # Steam library entries pointing at a missing installdir).
        if steam and steam_tree_writable(steam):
            try:
                under_steam = False
                try:
                    under_steam = install_path.resolve().is_relative_to(
                        (steam / "steamapps" / "common").resolve()
                    )
                except (OSError, ValueError, AttributeError):
                    under_steam = False
                if under_steam:
                    write_acf(
                        steam,
                        appid,
                        title,
                        installdir,
                        size=size,
                        manifests=parsed["manifests"],
                    )
            except Exception as exc:
                sys.stderr.write(f"steam acf skipped: {exc}\n")
        marker = install_final / ".arachnel-steamidra"
        hint = launch_hint(steam, appid, install_final)
        marker.write_text(
            json.dumps({"appid": appid, "mode": "ddmod", "launch": hint}, indent=2),
            encoding="utf-8",
        )
        fix = try_embed_online_fix(plugin_root, install_final, appid, settings)
        _emit_progress(progress, percent=100, detail="Installed", status="completed")
        out = {
            "ok": True,
            "installPath": str(install_final),
            "mode": "ddmod",
            "launch": hint,
            "steamPath": str(steam) if steam else "",
        }
        if fix:
            out["onlineFix"] = fix
        return out
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--progress-json", action="store_true")
    parser.add_argument(
        "--progress-log",
        default="",
        help="Append each progress tick as a new line (for debugging CLI speed/%%)",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_seed = sub.add_parser("catalog-seed")
    p_seed.add_argument("--settings", default="")

    p_sync = sub.add_parser("catalog-sync")
    p_sync.add_argument("--settings", default="")
    p_sync.add_argument("--force", action="store_true")

    p_search = sub.add_parser("search")
    p_search.add_argument("--query", required=True)
    p_search.add_argument("--settings", default="")

    p_app = sub.add_parser("app-details")
    p_app.add_argument("--appid", required=True)
    p_app.add_argument("--settings", default="")

    p_inst = sub.add_parser("install")
    p_inst.add_argument("--appid", required=True)
    p_inst.add_argument("--title", default="")
    p_inst.add_argument("--target", default="")
    p_inst.add_argument("--downloads", default="")
    p_inst.add_argument("--settings", default="")
    p_inst.add_argument("--mode", default="ddmod", help="Ignored; always downloads via DDMod")
    p_inst.add_argument(
        "--force",
        action="store_true",
        help="Re-download even if .arachnel-steamidra marker says already installed",
    )

    p_addon = sub.add_parser("install-addon")
    p_addon.add_argument("--zip", required=True)
    p_addon.add_argument("--dest", required=True)

    p_embed = sub.add_parser("embed-online-fix")
    p_embed.add_argument("--appid", required=True)
    p_embed.add_argument("--path", required=True, help="Game install directory")
    p_embed.add_argument("--settings", default="")
    p_embed.add_argument(
        "--force",
        action="store_true",
        help="Embed even without strong online categories / overwrite existing overlay",
    )

    p_setup = sub.add_parser("setup-inject")
    p_setup.add_argument("--settings", default="")

    sub.add_parser("check-dotnet")

    args = parser.parse_args()
    progress = bool(args.progress_json)
    global _cli_ticker, _progress_log_path
    # Always show 1 Hz human ticker on stderr for install (CLI testing).
    _cli_ticker = args.cmd == "install"
    log_arg = (getattr(args, "progress_log", "") or "").strip()
    if log_arg:
        _progress_log_path = Path(log_arg)
    elif args.cmd == "install":
        appid = getattr(args, "appid", "game")
        _progress_log_path = Path.cwd() / f"progress-{appid}.log"
    else:
        _progress_log_path = None
    if _progress_log_path is not None:
        try:
            _progress_log_path.write_text("", encoding="utf-8")  # truncate
            sys.stderr.write(f"Progress log: {_progress_log_path.resolve()}\n")
        except OSError:
            pass
    settings = load_settings(getattr(args, "settings", "") or None)
    plugin_root = Path(__file__).resolve().parent.parent

    try:
        if args.cmd == "catalog-seed":
            result = cmd_catalog_seed(settings, plugin_root)
        elif args.cmd == "catalog-sync":
            result = cmd_catalog_sync(settings, plugin_root, force=bool(args.force))
        elif args.cmd == "search":
            result = cmd_search(args.query, settings, plugin_root)
        elif args.cmd == "app-details":
            result = cmd_app_details(args.appid)
        elif args.cmd == "check-dotnet":
            result = check_dotnet9(plugin_root)
            result["ddmod"] = str(find_ddmod(plugin_root) or "")
            result["ok"] = bool(result.get("hasNet9") and find_ddmod(plugin_root))
        elif args.cmd == "setup-inject":
            steam = find_steam_path(settings)
            if steam is None:
                result = {
                    "ok": False,
                    "error": (
                        "Steam folder not found. Set steamPath in plugin settings "
                        "(path that contains steam.exe)."
                    ),
                    "steamPath": "",
                    "os": platform.system(),
                }
            else:
                try:
                    fresh = ensure_inject(steam, progress, plugin_root=plugin_root)
                    result = {
                        "ok": True,
                        "steamPath": str(steam),
                        "os": platform.system(),
                        "lumacorePresent": lumacore_present(steam),
                        "freshInstall": bool(fresh),
                    }
                except Exception as exc:
                    result = {
                        "ok": False,
                        "error": str(exc),
                        "steamPath": str(steam),
                        "os": platform.system(),
                    }
        elif args.cmd == "install-addon":
            result = cmd_install_addon(args.zip, args.dest)
        elif args.cmd == "embed-online-fix":
            result = try_embed_online_fix(
                plugin_root,
                Path(args.path),
                args.appid,
                settings,
                force=bool(args.force),
            )
            if result.get("ok") is None and not result.get("error"):
                result["ok"] = True
        elif args.cmd == "install":
            result = install_game(
                plugin_root,
                args.appid,
                args.title or f"App {args.appid}",
                getattr(args, "target", "") or "",
                settings,
                args.mode,
                progress,
                force=bool(getattr(args, "force", False)),
            )
        else:
            result = {"ok": False, "error": "unknown command"}
    except Exception as exc:
        result = {"ok": False, "error": str(exc)}

    _emit_result(result, progress)
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
