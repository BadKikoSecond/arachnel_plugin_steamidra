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
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path
from typing import Any

# Windows consoles often default to cp1251/cp1252 — force UTF-8 for JSON IPC.
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HUBCAP_BASE = "https://hubcapmanifest.com/api/v1"
MH3_LUA_URL = "https://steamtools.games/api/files/{appid}/lua"
# Federated installable-manifest catalog (GitLab raw). Override via settings.manifestCatalogUrl.
DEFAULT_MANIFEST_CATALOG_URL = (
    "https://gitlab.com/BadKiko/arachnel-plugin-steamidra/-/raw/main/catalog/games-manifest-catalog.json"
)


def _emit_progress(enabled: bool, **kwargs: Any) -> None:
    if not enabled:
        return
    payload = {"type": "progress", **kwargs}
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


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


def _fmt_speed(bps: float) -> str:
    if bps <= 0:
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
    return total <= max(downloaded * 8, downloaded + 512 * 1024 * 1024)


def transfer_detail(downloaded: int, total: int, bps: float) -> str:
    """Size · speed · ETA. Omit fake totals — only show total when trustworthy."""
    if _trustworthy_total(downloaded, total):
        return (
            f"{_fmt_bytes(downloaded)} / {_fmt_bytes(total)} · "
            f"{_fmt_speed(bps)} · ETA {_fmt_eta(max(0, total - downloaded), bps)}"
        )
    if downloaded > 0:
        return f"{_fmt_bytes(downloaded)} · {_fmt_speed(bps)}"
    return "Downloading…"


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
        "totalBytes": int(total),
        "downloadRateBps": int(max(0, bps)),
    }

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
        "hubcapApiKey": "",
        "ryuuApiKey": "",
        "steamPath": "",
        "preferMode": "ddmod",  # ddmod (default) | native | auto
        "libraryFolder": "",
        "includeDlc": False,
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


def hubcap_headers(settings: dict[str, Any]) -> dict[str, str] | None:
    key = (settings.get("hubcapApiKey") or "").strip()
    if not (key.startswith("smm") and len(key) > 10):
        return None
    return {
        "Authorization": f"Bearer {key}",
        "User-Agent": "Arachnel-SteaMidra/0.1",
        "Accept": "application/json",
    }


def find_steam_path(settings: dict[str, Any]) -> Path | None:
    configured = (settings.get("steamPath") or "").strip()
    if configured and Path(configured).is_dir():
        return Path(configured)

    home = Path.home()
    candidates: list[Path] = []
    if platform.system() == "Windows":
        for env in ("PROGRAMFILES(X86)", "PROGRAMFILES"):
            root = os.environ.get(env)
            if root:
                candidates.append(Path(root) / "Steam")
        candidates.append(home / "Steam")
        # Registry-less fallback: common Steam Library folders
        for drive in ("C:", "D:", "E:"):
            candidates.append(Path(f"{drive}/Steam"))
            candidates.append(Path(f"{drive}/Program Files (x86)/Steam"))
    else:
        candidates.extend(
            [
                home / ".steam" / "steam",
                home / ".steam" / "debian-installation",
                home / ".local" / "share" / "Steam",
                home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
            ]
        )
    for c in candidates:
        if (c / "steam.sh").exists() or (c / "steam.exe").exists() or (c / "steamapps").is_dir():
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


def entry_from_hubcap_item(item: dict[str, Any]) -> dict[str, Any] | None:
    appid = str(item.get("game_id") or item.get("appid") or item.get("id") or "")
    if not appid or not appid.isdigit():
        return None
    title = item.get("game_name") or item.get("name") or item.get("title") or f"App {appid}"
    size = item.get("manifest_size") or item.get("size") or 0
    size_label = ""
    try:
        n = int(size)
        if n > 0:
            size_label = f"{n / (1024 ** 3):.1f} GB" if n > 1024**2 else f"{n // 1024} KB"
    except Exception:
        pass
    uploaded = str(item.get("uploaded_date") or item.get("last_updated") or "").strip()
    version = uploaded[:10] if uploaded else ""
    return {
        "id": f"steam-{appid}",
        "title": title,
        "steamAppId": appid,
        "coverUrl": f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg",
        "description": "",
        "sizeLabel": size_label,
        "uploadDate": uploaded,
        "version": version,
        "hasManifest": True,
        "manifestSource": "hubcap",
        "delivery": "steam-manifest",
        "sourceId": "steamidra",
    }


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
            else:
                parsed = entry_from_hubcap_item(it)
                if parsed:
                    out.append(parsed)
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
    return {
        "id": f"steam-{appid}",
        "title": d.get("name") or f"App {appid}",
        "steamAppId": str(appid),
        "coverUrl": f"https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg",
        "description": (d.get("short_description") or "")[:2000],
        "sizeLabel": "",
        "version": str(d.get("steam_appid") or appid),
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


def hubcap_library(settings: dict[str, Any], search: str | None = None, limit: int = 40) -> list[dict[str, Any]]:
    headers = hubcap_headers(settings)
    if not headers:
        return []
    params: dict[str, Any] = {"limit": limit, "offset": 0, "sort_by": "updated"}
    if search:
        params["search"] = search
    url = f"{HUBCAP_BASE}/library?" + urllib.parse.urlencode(params)
    try:
        data = http_get_json(url, headers=headers)
    except Exception:
        return []
    games: list[dict[str, Any]] = []
    for it in data.get("games") or []:
        e = entry_from_hubcap_item(it)
        if e:
            games.append(e)
    return games


def hubcap_search(settings: dict[str, Any], query: str, limit: int = 40) -> list[dict[str, Any]]:
    headers = hubcap_headers(settings)
    if not headers:
        return []
    # Numeric query → treat as appid lookup via search
    params = {"q": query, "limit": limit}
    if query.strip().isdigit():
        params["appid"] = "true"
    url = f"{HUBCAP_BASE}/search?" + urllib.parse.urlencode(params)
    try:
        data = http_get_json(url, headers=headers)
    except Exception:
        return hubcap_library(settings, search=query, limit=limit)
    games: list[dict[str, Any]] = []
    items = data.get("results") or data.get("games") or data.get("items") or []
    if isinstance(data, list):
        items = data
    for it in items:
        e = entry_from_hubcap_item(it)
        if e:
            games.append(e)
    return games


def hubcap_library_pages(settings: dict[str, Any], max_pages: int = 10, page_size: int = 100) -> list[dict[str, Any]]:
    """Pull multiple Hubcap /library pages (installable manifests), not the full Steam store."""
    headers = hubcap_headers(settings)
    if not headers:
        return []
    games: list[dict[str, Any]] = []
    seen: set[str] = set()
    for page in range(max_pages):
        params = {"limit": page_size, "offset": page * page_size, "sort_by": "updated"}
        url = f"{HUBCAP_BASE}/library?" + urllib.parse.urlencode(params)
        try:
            data = http_get_json(url, headers=headers)
        except Exception:
            break
        batch = data.get("games") or []
        if not batch:
            break
        for it in batch:
            e = entry_from_hubcap_item(it)
            if not e:
                continue
            appid = e["steamAppId"]
            if appid in seen:
                continue
            seen.add(appid)
            games.append(e)
        total = int(data.get("total_count") or 0)
        if total and len(games) >= total:
            break
        if len(batch) < page_size:
            break
    return games


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
        return Path(configured).parent / "games-manifest-catalog.json"
    return plugin_root / "config" / "games-manifest-catalog.json"


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
    return {
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
    }


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
        age = time.monotonic() - dest.stat().st_mtime
        if age < max_age_s:
            return True
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "Arachnel-SteaMidra/0.2"},
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
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
    doc = data if isinstance(data, dict) else {"entries": games}
    if isinstance(doc, dict) and "entries" not in doc and "games" not in doc:
        doc = {
            "schemaVersion": 1,
            "catalogKind": "installable-manifest",
            "updatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "count": len(games),
            "entries": games,
        }
    dest.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")
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
    """Installable games only — manifest packages confirmed in federated JSON."""
    cache = manifest_catalog_cache_path(settings, plugin_root)
    bundled = plugin_root / "config" / "games-manifest-catalog.json"
    url = str(settings.get("manifestCatalogUrl") or DEFAULT_MANIFEST_CATALOG_URL).strip()
    ttl = float(settings.get("manifestCatalogTtlHours") or 24) * 3600
    if force_download and url:
        download_manifest_catalog(url, cache, max_age_s=0)
    elif url:
        download_manifest_catalog(url, cache, max_age_s=ttl if not force_download else 0)

    games = load_manifest_catalog_file(cache, min_entries=1)
    if games:
        return games
    games = load_manifest_catalog_file(bundled, min_entries=1)
    if games:
        return games
    return []


def cmd_catalog_sync(settings: dict[str, Any], plugin_root: Path, *, force: bool = False) -> dict[str, Any]:
    games = load_or_sync_manifest_catalog(settings, plugin_root, force_download=force)
    cache = manifest_catalog_cache_path(settings, plugin_root)
    if games:
        return {
            "ok": True,
            "catalogPath": str(cache if cache.is_file() else plugin_root / "config" / "games-manifest-catalog.json"),
            "source": "manifest-catalog",
            "count": len(games),
            "games": [],
        }
    return {"ok": False, "error": "Manifest catalog unavailable — publish data/games-manifest-catalog.json or run tools/build_manifest_catalog.py"}


def cmd_catalog_seed(settings: dict[str, Any], plugin_root: Path) -> dict[str, Any]:
    """Installable catalog = federated manifest JSON (GitLab). Keys-only depot DB is fallback."""
    manifest = load_or_sync_manifest_catalog(settings, plugin_root)
    if manifest:
        cache = manifest_catalog_cache_path(settings, plugin_root)
        path = cache if cache.is_file() else plugin_root / "config" / "games-manifest-catalog.json"
        return {
            "ok": True,
            "games": [],
            "catalogPath": str(path),
            "source": "manifest-catalog",
            "count": len(manifest),
        }

    depot_err = ""
    try:
        games = load_or_build_depotkeys_catalog(settings, plugin_root)
        if games:
            cache = games_catalog_cache_path(settings, plugin_root)
            return {
                "ok": True,
                "games": [],
                "catalogPath": str(cache),
                "source": "depotkeys",
                "count": len(games),
                "warning": "Manifest catalog missing — showing keys-only titles (download may fail)",
            }
    except Exception as exc:
        depot_err = str(exc)

    # Optional Hubcap library (richer metadata) when API key is set.
    hub = hubcap_library_pages(settings, max_pages=30, page_size=100)
    if hub:
        return {
            "ok": True,
            "games": hub,
            "source": "hubcap",
            "count": len(hub),
            "warning": depot_err or None,
        }

    local = load_local_games_json(plugin_root)
    if local:
        return {
            "ok": True,
            "games": local,
            "source": "games.json",
            "count": len(local),
            "warning": depot_err or None,
        }

    featured = steam_featured_fallback()
    return {
        "ok": True,
        "games": featured,
        "source": "steam-featured",
        "count": len(featured),
        "warning": depot_err or None,
    }

def cmd_search(query: str, settings: dict[str, Any], plugin_root: Path) -> dict[str, Any]:
    q = query.strip()
    if not q:
        return cmd_catalog_seed(settings, plugin_root)

    # Exact appid
    if q.isdigit():
        try:
            g = steam_app_details(q)
            if g:
                return {"ok": True, "games": [g], "source": "steam-appid"}
        except Exception:
            pass

    hub = hubcap_search(settings, q)
    if hub:
        return {"ok": True, "games": hub, "source": "hubcap"}

    ql = q.lower()
    catalog: list[dict[str, Any]] = load_or_sync_manifest_catalog(settings, plugin_root)
    if not catalog:
        try:
            catalog = load_or_build_depotkeys_catalog(settings, plugin_root)
        except Exception:
            catalog = load_local_games_json(plugin_root)
    if catalog:
        filtered = [
            g
            for g in catalog
            if ql in (g.get("title") or "").lower() or q == g.get("steamAppId")
        ]
        if filtered:
            return {"ok": True, "games": filtered[:80], "source": "depotkeys"}

    return {"ok": True, "games": steam_store_search(q), "source": "steam-store"}


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
    return Path.home() / ".local" / "share" / "PetWork" / "Arachnel" / "plugins" / "steamidra" / "fallback_depotkeys.json"


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
            f"({len(depot_ids)} depot(s) known). Set hubcapApiKey or ryuuApiKey in plugin settings."
        )
    return "\n".join(lines) + "\n"


def fetch_lua(
    appid: str,
    settings: dict[str, Any],
    staging: Path,
    progress: bool = False,
) -> str:
    """Fetch stplug-in lua + extract any bundled .manifest files into staging.

    Prefer packaged mirrors (revobd/Hubcap) when they include manifests — Steam now
    returns HTTP 401 for anonymous manifest CDN downloads. Depot-key DB is the
    fast offline fallback for keys, then we still probe packages for manifests.
    Prep phase always reports 0%.
    """
    headers = hubcap_headers(settings)
    errors: list[str] = []
    probe_timeout = 8

    def _prep(msg: str) -> None:
        _emit_progress(progress, percent=0, detail=msg, status="downloading")

    def _from_package(raw: bytes) -> str | None:
        lua = extract_lua_and_manifests(raw, staging)
        if not lua or "addappid" not in lua.lower():
            return None
        return lua

    def _package_has_manifests() -> bool:
        return any(staging.glob("*.manifest"))

    _prep("Getting game info…")

    # 1) Packaged sources first (lua + .manifest) — required since Steam 401 on CDN.
    if headers:
        for path in (f"/lua/{appid}", f"/manifest/{appid}"):
            try:
                raw = http_get_bytes(
                    f"{HUBCAP_BASE}{path}", headers=headers, timeout=probe_timeout
                )
                lua = _from_package(raw)
                if lua and (_package_has_manifests() or "setmanifestid" in lua.lower()):
                    return lua
                if lua:
                    errors.append(f"hubcap{path}: lua without manifests")
            except Exception as exc:
                errors.append(f"hubcap{path}: {exc}")

    try:
        raw = http_get_bytes(
            f"https://api.luagen.revobd.club/{appid}.zip", timeout=probe_timeout
        )
        lua = _from_package(raw)
        if lua and (_package_has_manifests() or "setmanifestid" in lua.lower()):
            return lua
        if lua:
            # Keep lua/keys; manifests may still be missing (e.g. MECCHA package).
            errors.append("revobd: lua without manifests")
            packaged_lua = lua
        else:
            packaged_lua = None
            errors.append("revobd: no lua in zip")
    except Exception as exc:
        packaged_lua = None
        errors.append(f"revobd: {exc}")

    ryuu_key = (settings.get("ryuuApiKey") or "").strip()
    try:
        url = f"https://generator.ryuu.lol/secure_download?appid={appid}"
        if ryuu_key:
            url += f"&auth_code={urllib.parse.quote(ryuu_key)}"
        raw = http_get_bytes(url, timeout=probe_timeout)
        lua = _from_package(raw)
        if lua and (_package_has_manifests() or "setmanifestid" in lua.lower()):
            return lua
        if lua:
            packaged_lua = packaged_lua or lua
            errors.append("ryuu: lua without manifests")
    except Exception as exc:
        errors.append(f"ryuu: {exc}")

    for url in (
        f"https://cdn.jsdelivr.net/gh/oureveryday/Steam-Depot-Manifest-Database@main/depots/{appid}.lua",
        f"https://raw.githubusercontent.com/oureveryday/Steam-Depot-Manifest-Database/main/depots/{appid}.lua",
    ):
        try:
            raw = http_get_bytes(url, timeout=probe_timeout)
            text = raw.decode("utf-8", errors="replace")
            if "addappid" in text.lower() and "setmanifestid" in text.lower():
                return text
            if "addappid" in text.lower():
                packaged_lua = packaged_lua or text
        except Exception as exc:
            errors.append(f"mirror: {exc}")

    # 2) Offline depot-key DB (keys only — Steam will 401 without local manifests).
    depotkeys_lua: str | None = None
    no_keys = False
    try:
        depotkeys_lua = build_lua_from_depotkeys(appid, settings, progress=progress)
    except Exception as exc:
        errors.append(f"depotkeys: {exc}")
        no_keys = "No depot decryption keys" in str(exc)

    # Prefer package lua if it has keys; otherwise depotkeys. Manifests already in staging.
    chosen = packaged_lua or depotkeys_lua
    if chosen and (_package_has_manifests() or "setmanifestid" in chosen.lower()):
        return chosen

    # 3) steamcmd enrichment (often down) — only if we still lack manifests.
    if depotkeys_lua and not _package_has_manifests():
        try:
            enriched = build_lua_from_steamcmd(appid, settings, progress=progress)
            if enriched:
                return enriched
        except Exception as exc:
            errors.append(f"steamcmd: {exc}")

    if chosen:
        # May still fail at DDMod with Steam 401 — caller surfaces a clear error.
        return chosen

    if no_keys:
        raise RuntimeError(
            "This game is not available for download right now. Try another title."
        )

    raise RuntimeError(
        "Could not prepare this game for download. Try again later or pick another title."
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


def setup_lumacore(steam: Path, progress: bool, plugin_root: Path | None = None) -> None:
    (steam / "config" / "stplug-in").mkdir(parents=True, exist_ok=True)
    bundled = None
    if plugin_root:
        for cand in (
            plugin_root / "third_party" / "LumaCore",
            plugin_root / "third_party" / "windows" / "LumaCore",
        ):
            if cand.is_dir():
                bundled = cand
                break
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
    asset_url = None
    for a in rel.get("assets") or []:
        name = (a.get("name") or "").lower()
        if name.endswith(".zip") or "lumacore" in name:
            asset_url = a.get("browser_download_url")
            if asset_url:
                break
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
            json.dumps({"tag": rel.get("tag_name"), "installed": True}, indent=2), encoding="utf-8"
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


def ensure_inject(steam: Path | None, progress: bool, plugin_root: Path | None = None) -> None:
    """Install SLS/Luma when Steam is present; no-op if Steam absent (ddmod-only)."""
    if steam is None:
        _emit_progress(progress, percent=0, detail="Preparing…", status="downloading")
        return
    if platform.system() == "Windows":
        if not (steam / "LumaCore.dll").exists() and not (steam / "dwmapi.dll").exists():
            setup_lumacore(steam, progress, plugin_root=plugin_root)
        else:
            _emit_progress(progress, percent=0, detail="Preparing Steam…", status="downloading")
        (steam / "config" / "stplug-in").mkdir(parents=True, exist_ok=True)
    else:
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


def write_acf(
    steam: Path,
    appid: str,
    title: str,
    installdir: str,
    size: int = 0,
    manifests: dict[str, str] | None = None,
) -> Path:
    """Write a usable appmanifest (Fully Installed) after ddmod download."""
    import time

    lib = steam / "steamapps"
    lib.mkdir(parents=True, exist_ok=True)
    common = lib / "common" / installdir
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
\t"StateFlags"\t\t"4"
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


def write_acf_stub(steam: Path, appid: str, title: str, installdir: str, size: int = 0) -> Path:
    return write_acf(steam, appid, title, installdir, size=size, manifests=None)


def find_ddmod(plugin_root: Path) -> Path | None:
    candidates = [
        plugin_root / "third_party" / "DDMod" / "DepotDownloaderMod.dll",
        plugin_root / "third_party" / "linux" / "DDMod" / "DepotDownloaderMod.dll",
        plugin_root / "third_party" / "windows" / "DDMod" / "DepotDownloaderMod.dll",
        plugin_root / "third_party" / "DepotDownloaderMod.dll",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def find_dotnet(plugin_root: Path | None = None) -> str | None:
    cands: list[str] = []
    if plugin_root:
        for rel in (
            "third_party/dotnet/dotnet",
            "third_party/dotnet/dotnet.exe",
            "third_party/linux/dotnet/dotnet",
            "third_party/windows/dotnet/dotnet.exe",
            "third_party/windows/dotnet/dotnet",
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


def resolve_depot_os(appid: str) -> str:
    """Pick DepotDownloader -os from Steam appinfo (Windows-only titles need windows on Linux)."""
    if platform.system() == "Windows":
        return "windows"
    try:
        raw = http_get_bytes(f"https://api.steamcmd.net/v1/info/{appid}", timeout=20)
        info = json.loads(raw.decode("utf-8", errors="replace"))
        common = ((info.get("data") or {}).get(str(appid)) or {}).get("common") or {}
        oslist = str(common.get("oslist") or "").lower()
        if oslist and "linux" not in oslist and "windows" in oslist:
            return "windows"
    except Exception:
        pass
    return "linux"


def is_steamworks_redistributable(depot_id: str) -> bool:
    """Steamworks shared redistributables (VC++/DirectX) — not the game itself."""
    try:
        n = int(depot_id)
    except ValueError:
        return False
    # Common Steamworks redistributable depot id band.
    return 228980 <= n <= 229099


def filter_primary_depots(
    appid: str,
    depots: dict[str, str],
    manifests: dict[str, str],
    include_dlc: bool,
) -> tuple[dict[str, str], dict[str, str]]:
    """Keep game depots only: drop the app id itself, redistributables, and DLC unless asked."""
    if not depots:
        return depots, manifests
    dlc_ids: set[str] = set()
    if not include_dlc:
        try:
            info = steamcmd_app_info(appid) or {}
            ext = info.get("extended") or {}
            for part in str(ext.get("listofdlc") or "").split(","):
                part = part.strip()
                if part.isdigit():
                    dlc_ids.add(part)
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


def default_library_root(settings: dict[str, Any]) -> Path:
    lib = (settings.get("libraryFolder") or "").strip()
    if lib:
        return Path(lib)
    return Path.home() / "Games" / "Arachnel" / "steamidra"


def find_game_exe(install_path: Path) -> Path | None:
    exes = sorted(install_path.rglob("*.exe"))
    if not exes:
        return None
    # Prefer top-level / non-UnityCrashHandler
    preferred = [
        e
        for e in exes
        if e.parent == install_path and "crash" not in e.name.lower() and "unity" not in e.name.lower()
    ]
    if preferred:
        return preferred[0]
    for e in exes:
        if "crash" not in e.name.lower():
            return e
    return exes[0]


def launch_hint(steam: Path | None, appid: str, install_path: Path) -> dict[str, Any]:
    if steam is not None and (steam / "steamapps" / f"appmanifest_{appid}.acf").is_file():
        return {"type": "steam", "uri": f"steam://rungameid/{appid}"}
    exe = find_game_exe(install_path)
    if exe:
        return {"type": "exe", "path": str(exe)}
    return {"type": "folder", "path": str(install_path)}


def run_ddmod(
    plugin_root: Path,
    appid: str,
    depot_keys: dict[str, str],
    manifests: dict[str, str],
    dest: Path,
    progress: bool,
    steam: Path | None = None,
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
    depot_os = resolve_depot_os(appid)
    total = max(1, len(depots))
    pct_re = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*%")
    size_origin = dir_byte_size(dest) if dest.is_dir() else 0
    size_tracker = DirSizeTracker(dest, interval_s=0.15)
    size_tracker.start()
    estimated_total_bytes = max(0, size_origin)
    t0 = time.monotonic()
    last_bytes = 0
    last_t = t0
    speed = 0.0
    err_tail: list[str] = []

    def _current_downloaded() -> int:
        return max(0, size_tracker.snapshot() - size_origin)

    def _update_speed(now: float, downloaded: int) -> None:
        nonlocal last_bytes, last_t, speed
        dt = now - last_t
        if dt < 0.15 or downloaded <= last_bytes:
            return
        delta = downloaded - last_bytes
        if delta < 80 * 1024 * 1024:
            speed = delta / dt
        last_bytes = downloaded
        last_t = now

    def _resolve_progress(
        overall_pct: int, raw_downloaded: int
    ) -> tuple[int, int, int]:
        nonlocal estimated_total_bytes
        downloaded = max(0, raw_downloaded)
        # DDMod prints per-file percent — ignore low % when inferring total size.
        if overall_pct >= 8 and downloaded > 0:
            candidate = int(downloaded * 100 / overall_pct)
            if _trustworthy_total(downloaded, candidate):
                estimated_total_bytes = max(estimated_total_bytes, candidate)
        total = estimated_total_bytes
        if not _trustworthy_total(downloaded, total):
            total = 0
            estimated_total_bytes = 0
        if downloaded <= 0 and total > 0 and overall_pct > 0:
            downloaded = int(total * overall_pct / 100)
        if total > 0 and downloaded > 0:
            byte_pct = max(1, min(99, int(downloaded * 100 / total)))
            overall_pct = max(overall_pct, byte_pct)
        return overall_pct, downloaded, total

    def _emit_download(
        overall_pct: int,
        raw_downloaded: int,
        *,
        detail: str | None = None,
    ) -> None:
        overall_pct, downloaded, total = _resolve_progress(overall_pct, raw_downloaded)
        _emit_progress(
            progress,
            **progress_kwargs(
                percent=overall_pct,
                detail=detail or transfer_detail(downloaded, total, speed),
                downloaded=downloaded,
                total=total,
                bps=speed,
            ),
        )

    try:
        for i, depot_id in enumerate(depots):
            frac0 = i / total
            _emit_download(
                max(1, int(99 * frac0)),
                _current_downloaded(),
                detail="Downloading…",
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

            local_pct = 0.0
            last_emit = 0.0
            downloaded = 0
            while proc.poll() is None or not line_queue.empty():
                try:
                    line = line_queue.get(timeout=0.08)
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
                        except ValueError:
                            pass

                now = time.monotonic()
                if now - last_emit < 0.08 and proc.poll() is None:
                    continue
                last_emit = now

                downloaded = _current_downloaded()
                _update_speed(now, downloaded)
                frac = (i + min(local_pct, 100.0) / 100.0) / total
                overall = max(1, min(99, int(99 * frac)))
                _emit_download(overall, downloaded)

            rc = proc.wait()
            if rc != 0:
                tail = "\n".join(err_tail)
                sys.stderr.write(tail[-800:] + "\n")
                if "401" in tail or "No manifest request code" in tail:
                    raise RuntimeError(
                        "Steam blocked downloading game files (need a packaged manifest). "
                        "Try another title, or set hubcapApiKey in plugin settings."
                    )
                raise RuntimeError("Download failed. Try again or pick another game.")

            downloaded = _current_downloaded()
            _update_speed(time.monotonic(), downloaded)
            done_frac = (i + 1) / total
            _emit_download(max(1, min(99, int(99 * done_frac))), downloaded)
    finally:
        size_tracker.stop()


def install_game(
    plugin_root: Path,
    appid: str,
    title: str,
    target: str,
    settings: dict[str, Any],
    mode: str,
    progress: bool,
) -> dict[str, Any]:
    steam = find_steam_path(settings)
    prefer = mode if mode not in ("", "auto") else (settings.get("preferMode") or "ddmod")
    ensure_inject(steam, progress, plugin_root=plugin_root)

    staging = Path(tempfile.mkdtemp(prefix=f"arachnel_sm_{appid}_"))
    try:
        _emit_progress(progress, percent=0, detail="Getting game info…", status="downloading")
        try:
            lua = fetch_lua(appid, settings, staging, progress=progress)
        except Exception as exc:
            return {"ok": False, "error": str(exc)}

        parsed = parse_lua(lua)
        # Manifests bundled next to lua (revobd/Hubcap) — recover IDs from filenames
        # even when the lua has keys only.
        for mf in staging.glob("*.manifest"):
            stem = mf.stem  # e.g. 391541_3413030246586915448
            if "_" not in stem:
                continue
            depot_id, mid = stem.split("_", 1)
            if depot_id.isdigit() and mid.isdigit():
                parsed["manifests"].setdefault(depot_id, mid)
        depots, manifests = filter_primary_depots(
            appid,
            parsed["depots"],
            parsed["manifests"],
            include_dlc=bool(settings.get("includeDlc")),
        )
        parsed = {**parsed, "depots": depots, "manifests": manifests}

        if steam:
            write_stplugin_lua(steam, appid, lua)
            patch_config_vdf_keys(steam, parsed["depots"])
            copy_manifests_to_depotcache(steam, staging)
            try_download_manifests(steam, parsed["manifests"], staging)
            if platform.system() != "Windows":
                register_sls_app(appid, parsed["manifests"])
                create_steam_cfg(steam)
        else:
            # Still fetch manifests into staging for -manifestfile
            try_download_manifests(None, parsed["manifests"], staging)
            for mf in staging.glob("*.manifest"):
                norm = normalize_depot_manifest_bytes(mf.read_bytes())
                if norm:
                    mf.write_bytes(norm)
                    shutil.copy2(mf, Path(tempfile.gettempdir()) / mf.name)

        installdir = re.sub(r"[^\w\- ]+", "", title).strip() or f"App{appid}"
        installdir = installdir.replace(" ", "")[:40] or f"App{appid}"

        lib_folder = (settings.get("libraryFolder") or "").strip()
        if target:
            install_path = Path(target)
        elif lib_folder:
            install_path = Path(lib_folder) / installdir
        elif steam:
            install_path = steam / "steamapps" / "common" / installdir
        else:
            install_path = default_library_root(settings) / f"{appid}-{installdir}"

        force_ddmod = prefer == "ddmod"
        force_native = prefer == "native"
        can_ddmod = find_ddmod(plugin_root) is not None

        def do_ddmod() -> dict[str, Any]:
            _emit_progress(progress, percent=0, detail="Downloading…", status="downloading")
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
            )
            size = sum(f.stat().st_size for f in install_path.rglob("*") if f.is_file())
            if steam:
                # Prefer Steam common path for ACF installdir
                steam_common = steam / "steamapps" / "common" / installdir
                if install_path.resolve() != steam_common.resolve():
                    steam_common.parent.mkdir(parents=True, exist_ok=True)
                    if not steam_common.exists():
                        shutil.copytree(install_path, steam_common, dirs_exist_ok=True)
                    install_final = steam_common
                else:
                    install_final = install_path
                write_acf(
                    steam,
                    appid,
                    title,
                    installdir,
                    size=size,
                    manifests=parsed["manifests"],
                )
            else:
                install_final = install_path
            marker = install_final / ".arachnel-steamidra"
            hint = launch_hint(steam, appid, install_final)
            marker.write_text(
                json.dumps({"appid": appid, "mode": "ddmod", "launch": hint}, indent=2),
                encoding="utf-8",
            )
            _emit_progress(progress, percent=100, detail="Installed", status="completed")
            return {
                "ok": True,
                "installPath": str(install_final),
                "mode": "ddmod",
                "launch": hint,
                "steamPath": str(steam) if steam else "",
            }

        def do_native() -> dict[str, Any]:
            if not steam:
                return {
                    "ok": False,
                    "error": "native mode requires Steam — use preferMode ddmod or install Steam",
                }
            common = write_acf_stub(steam, appid, title, installdir, size=0)
            detail = "Queued in Steam — restart Steam, then open the game there"
            try:
                if platform.system() != "Windows":
                    subprocess.Popen(
                        ["steam", f"steam://install/{appid}"],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                else:
                    subprocess.Popen(
                        ["cmd", "/c", "start", "", f"steam://install/{appid}"],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
            except Exception:
                pass
            _emit_progress(progress, percent=90, detail=detail, status="downloading")
            path = common if common.exists() else install_path
            path.mkdir(parents=True, exist_ok=True)
            hint = launch_hint(steam, appid, path)
            marker = path / ".arachnel-steamidra"
            marker.write_text(
                json.dumps({"appid": appid, "mode": "native", "launch": hint}, indent=2),
                encoding="utf-8",
            )
            _emit_progress(
                progress, percent=100, detail="Prepared — Steam is downloading", status="completed"
            )
            return {
                "ok": True,
                "installPath": str(path),
                "mode": "native",
                "steamPath": str(steam),
                "launch": hint,
            }

        if force_ddmod or (prefer == "auto" and can_ddmod):
            if force_native:
                return do_native()
            try:
                return do_ddmod()
            except Exception as exc:
                if prefer == "auto" and steam:
                    try:
                        return do_native()
                    except Exception as native_exc:
                        return {
                            "ok": False,
                            "error": f"ddmod failed: {exc}; native failed: {native_exc}",
                        }
                return {"ok": False, "error": str(exc)}

        if force_native or not can_ddmod:
            if not can_ddmod and not force_native:
                return {
                    "ok": False,
                    "error": "DepotDownloaderMod missing — run tools/fetch_third_party.sh and rebuild",
                }
            return do_native()

        return do_ddmod()
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--progress-json", action="store_true")
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
    p_inst.add_argument("--mode", default="auto")

    p_setup = sub.add_parser("setup-inject")
    p_setup.add_argument("--settings", default="")

    sub.add_parser("check-dotnet")

    args = parser.parse_args()
    progress = bool(args.progress_json)
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
            ensure_inject(steam, progress, plugin_root=plugin_root)
            result = {"ok": True, "steamPath": str(steam) if steam else "", "os": platform.system()}
        elif args.cmd == "install":
            result = install_game(
                plugin_root,
                args.appid,
                args.title or f"App {args.appid}",
                getattr(args, "target", "") or "",
                settings,
                args.mode,
                progress,
            )
        else:
            result = {"ok": False, "error": "unknown command"}
    except Exception as exc:
        result = {"ok": False, "error": str(exc)}

    _emit_result(result, progress)
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
