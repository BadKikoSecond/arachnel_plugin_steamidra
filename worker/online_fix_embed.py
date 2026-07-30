"""Embed FreeTP-compatible SteamFix overlay into a SteaMidra install.

Anchors learned from FreeTP / Hydra installs on disk
----------------------------------------------------
Unity (Cat Mail, Ore Factory, Dont Panic, Gamble…):
  - Overlay next to `<Game>.exe` + `<Game>_Data/`
  - Files: SteamFix64.dll + winmm.dll + winmm.txt("SteamFix64.dll") + SteamFix.ini
  - No steam_appid.txt, no EpicFix

Unreal without EOS (Into the Radius 2, Chopping Trees):
  - Overlay in `<Game>/Binaries/Win64` next to `*-Win64-Shipping.exe`
  - Same SteamFix trio as Unity; never under `Engine/`

Unreal with EOS / RedpointEOS (MECCHA CHAMELEON):
  - Same Win64 shipping dir ONLY (not install root, not Engine/Steamworks)
  - Extra: EpicFix64.dll + EpicFix.ini with `[EOS]\\nNoAuth=true`
  - winmm.txt loads BOTH: SteamFix64.dll then EpicFix64.dll
  - SteamFix.ini: FakeWorkshop=true + Workshop=true when Store has Workshop

32-bit / classic Steamworks (Binding of Isaac, old Unity x86):
  - SteamFix32.dll + x86 winmm (from kit `winmm32.dll`) + winmm.txt("SteamFix32.dll")
  - Keep Valve steam_api.dll (classic SteamAPI_Init exports) - never flat-API cracks
  - Always steam_appid.txt=480; skip EpicFix64 (Win32 EOSSDK is not x64)
  - Steamless unpack on launch exes with a `.bind` section (SteamStub → Steam license UI)

Binaries are harvested into `online_fix_kit/` (SteamFix + EpicFix packaged in releases).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

# Steam Store category IDs (ajaxgetstorecategories / appdetails.categories).
#
# Decision tree (anchors validated against FreeTP/Hydra installs + Store samples):
#
#   SteamFix REQUIRED  ← any of:
#       36 Online PvP, 38 Online Co-op, 20 MMO, 27 Cross-Platform Multiplayer
#       (every FreeTP online pack we checked had 36 and/or 38)
#
#   SteamFix NONE      ← no strong tags above. Typical cases:
#       - Single-player only (Portal, Hollow Knight, Hades)
#       - Local / LAN / split-screen only (Isaac: Multi-player + Shared Screen,
#         but NO Online PvP/Co-op → no Steam online-fix)
#
#   SteamFix OPTIONAL  ← weak parents only (1 Multi-player / 9 Co-op / 49 PvP)
#       without strong Online-* tags. Default: do NOT embed
#       (enable onlineFixOnWeakMultiplayer to force).
#
#   EpicFix            ← NOT a Store signal. Meccha has Online PvP but NOT
#       Cross-Platform; EOS is only known after install via EOSSDK*/RedpointEOS.
#       Policy: probe on disk after depot download; if present → require EpicFix.
STRONG_ONLINE_CATEGORY_IDS = frozenset({36, 38, 27, 20})  # Online PvP/Co-op, Cross-Platform, MMO
WEAK_MP_CATEGORY_IDS = frozenset({1, 9, 49})  # Multi-player / Co-op / PvP parents
LOCAL_OR_LAN_CATEGORY_IDS = frozenset({24, 37, 39, 47, 48})  # Local / split / LAN
WORKSHOP_CATEGORY_ID = 30

KIT_REQUIRED = ("SteamFix64.dll", "winmm.dll")
KIT_OPTIONAL = ("SteamFix32.dll", "winmm32.dll", "EpicFix64.dll", "EpicFix.ini")


def analyze_store_categories(store_data: dict[str, Any]) -> dict[str, Any]:
    """Parse appdetails `data` → SteamFix need class (EpicFix is install-time only)."""
    cats = store_data.get("categories") or []
    ids: set[int] = set()
    labels: list[str] = []
    for cat in cats:
        if not isinstance(cat, dict):
            continue
        try:
            cid = int(cat.get("id"))
        except (TypeError, ValueError):
            continue
        ids.add(cid)
        desc = str(cat.get("description") or "").strip()
        if desc and desc not in labels:
            labels.append(desc)

    strong_hits = sorted(ids & STRONG_ONLINE_CATEGORY_IDS)
    weak_hits = sorted(ids & WEAK_MP_CATEGORY_IDS)
    local_hits = sorted(ids & LOCAL_OR_LAN_CATEGORY_IDS)
    strong = bool(strong_hits)
    weak = bool(weak_hits)
    local_only = bool(local_hits) and not strong

    genres = store_data.get("genres") or []
    genre_names = [
        str(g.get("description") or "").strip()
        for g in genres
        if isinstance(g, dict) and g.get("description")
    ]
    if any("massively multiplayer" in g.lower() for g in genre_names):
        strong = True
        if 20 not in strong_hits:
            strong_hits = sorted(set(strong_hits) | {20})

    # steamFix: required | optional | none
    if strong:
        steam_fix = "required"
        reason = "store has Online PvP/Co-op, Cross-Platform, or MMO"
    elif weak and not local_only:
        steam_fix = "optional"
        reason = "only weak Multi-player/Co-op/PvP tags (no Online-*); skip by default"
    elif local_only:
        steam_fix = "none"
        reason = "local/LAN/split-screen only — no Steam online-fix"
    else:
        steam_fix = "none"
        reason = "no multiplayer store tags — single-player / offline"

    # EpicFix cannot be decided from Store (Meccha has no Cross-Platform tag).
    epic_fix = "probe_after_install" if steam_fix == "required" else "none"

    return {
        "categoryIds": sorted(ids),
        "categoryLabels": labels,
        "genreLabels": genre_names,
        "hasStrongOnline": strong,
        "hasWeakMultiplayer": weak,
        "localOrLanOnly": local_only,
        "hasWorkshop": WORKSHOP_CATEGORY_ID in ids,
        "strongCategoryIds": strong_hits,
        "weakCategoryIds": weak_hits,
        "localCategoryIds": local_hits,
        "steamFix": steam_fix,
        "epicFix": epic_fix,
        "reason": reason,
        # Back-compat for embed / catalog installKind
        "needsOnlineFix": steam_fix == "required",
    }


def merge_genre_string(existing: str, analysis: dict[str, Any]) -> str:
    parts = [p.strip() for p in (existing or "").split(",") if p.strip()]
    for label in analysis.get("categoryLabels") or []:
        if label not in parts:
            parts.append(label)
    for label in analysis.get("genreLabels") or []:
        if label not in parts:
            parts.append(label)
    if analysis.get("needsOnlineFix") and "Online Fix" not in parts:
        parts.append("Online Fix")
    return ", ".join(parts)


def _kit_usable(path: Path) -> bool:
    if (path / "OnlineFix64.dll").is_file() or (path / "OnlineFix.dll").is_file():
        return True
    return (path / "SteamFix64.dll").is_file() and (path / "winmm.dll").is_file()


def _pick_best_kit(root: Path) -> Path:
    if _kit_usable(root):
        return root
    host = "windows" if os.name == "nt" else "linux"
    host_dir = root / host
    if _kit_usable(host_dir):
        return host_dir
    if os.name != "nt":
        linux_dir = root / "linux"
        linux_zip = root / "linux.zip"
        if not _kit_usable(linux_dir) and linux_zip.is_file():
            linux_dir.mkdir(parents=True, exist_ok=True)
            try:
                import zipfile

                with zipfile.ZipFile(linux_zip, "r") as zf:
                    zf.extractall(linux_dir)
            except OSError as exc:
                sys.stderr.write(f"online_fix: failed to extract linux.zip: {exc}\n")
        if _kit_usable(linux_dir):
            return linux_dir
        win = root / "windows"
        if _kit_usable(win):
            return win
    return root


def kit_dir(plugin_root: Path, settings: dict[str, Any]) -> Path:
    configured = (settings.get("onlineFixSourceDir") or "").strip()
    if configured:
        cfg = Path(configured).expanduser().resolve()
        if _kit_usable(cfg):
            return cfg
    # Prefer kit shipped next to the plugin (steam.arach / online_fix_kit).
    bundled = _pick_best_kit((plugin_root / "online_fix_kit").resolve())
    if _kit_usable(bundled):
        return bundled
    settings_path = (settings.get("_settingsPath") or "").strip()
    if settings_path:
        beside_settings = Path(settings_path).expanduser().resolve().parent / "online_fix_kit"
        picked = _pick_best_kit(beside_settings)
        if _kit_usable(picked):
            return picked
        if not _kit_usable(bundled):
            return beside_settings
    return bundled


_FILE_ATTRIBUTE_REPARSE_POINT = 0x400
_HARVEST_SKIP_DIRS = {
    ".depotdownloader",
    "staging",
    "intermediate",
    "saved",
    "deriveddatacache",
    "thirdparty",
    "downloads",
    "cover-cache",
    "torrent_resume",
    "proton",
    "node_modules",
    ".git",
}


def _is_reparse_point(path: Path) -> bool:
    """True for Windows junctions/symlinks (pathlib.rglob follows these → RecursionError)."""
    try:
        st = os.lstat(path)
        attrs = getattr(st, "st_file_attributes", 0)
        return bool(attrs & _FILE_ATTRIBUTE_REPARSE_POINT)
    except OSError:
        return path.is_symlink()


def _candidate_harvest_roots(settings: dict[str, Any]) -> list[Path]:
    roots: list[Path] = []
    for key in ("onlineFixSourceDir", "libraryFolder"):
        raw = (settings.get(key) or "").strip()
        if raw:
            roots.append(Path(raw).expanduser())
    home = Path.home()
    # Prefer known game libraries first — they usually hold FreeTP overlays.
    extras = [
        Path(r"D:\HydraGames"),
        Path(r"D:\ArachnelGame"),
        Path(r"C:\Games\Arachnel"),
        Path(r"D:\Games\Arachnel"),
        home / "Games",
        home / "Games" / "Online-Fix",
        home / "Games" / "Hydra",
        home / ".var" / "app" / "org.badkiko.sofl" / "data" / "games",
        Path("/mnt/games"),
        Path("/mnt/Games"),
    ]
    # Arachnel library folders (Linux + Windows).
    xdg = os.environ.get("XDG_DATA_HOME") or str(home / ".local" / "share")
    extras.append(Path(xdg) / "Arachnel" / "Arachnel" / "library")
    extras.append(Path(xdg) / "Arachnel" / "library")
    for extra in extras:
        roots.append(extra)
    appdata = os.environ.get("APPDATA") or ""
    if appdata:
        # Do NOT walk all of Roaming\Arachnel — junctions there break deep scans.
        roots.append(Path(appdata) / "Arachnel" / "Arachnel" / "plugins")
        roots.append(Path(appdata) / "Arachnel" / "plugins")
        roots.append(Path(appdata) / "Arachnel" / "Arachnel" / "library")
    out: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        try:
            key = str(root.resolve()) if root.exists() else str(root)
        except OSError:
            key = str(root)
        if key in seen:
            continue
        seen.add(key)
        out.append(root)
    return out


def _iter_files_named(root: Path, name: str, *, max_depth: int = 10):
    """Yield files named `name` under root without following junctions/symlinks."""
    if not root.is_dir():
        return
    direct = root / name
    if direct.is_file():
        yield direct
    try:
        for dirpath, dirnames, filenames in os.walk(root, topdown=True, followlinks=False):
            try:
                rel = Path(dirpath).relative_to(root)
                depth = 0 if str(rel) == "." else len(rel.parts)
            except ValueError:
                depth = 0
            keep: list[str] = []
            for d in dirnames:
                if depth + 1 > max_depth:
                    continue
                child = Path(dirpath) / d
                if d.lower() in _HARVEST_SKIP_DIRS or _is_reparse_point(child):
                    continue
                keep.append(d)
            dirnames[:] = keep
            if name in filenames:
                yield Path(dirpath) / name
    except OSError:
        return


def _harvest_named_files(
    settings: dict[str, Any], names: tuple[str, ...], *, prefer_with: str | None = None
) -> dict[str, Path]:
    """Find kit files under library roots. Prefer a parent that also has prefer_with."""
    found: dict[str, Path] = {}
    preferred: dict[str, Path] = {}
    for root in _candidate_harvest_roots(settings):
        if not root.is_dir():
            continue
        try:
            for path in _iter_files_named(root, names[0]):
                parent = path.parent
                if _skip_overlay_path(parent):
                    continue
                batch: dict[str, Path] = {}
                for name in names:
                    src = parent / name
                    if src.is_file():
                        batch[name] = src
                if names[0] not in batch:
                    continue
                if prefer_with and (parent / prefer_with).is_file():
                    for k, v in batch.items():
                        preferred.setdefault(k, v)
                for k, v in batch.items():
                    found.setdefault(k, v)
                if prefer_with and prefer_with in preferred:
                    return preferred
        except OSError:
            continue
    return preferred or found


def harvest_online_fix_kit(plugin_root: Path, settings: dict[str, Any]) -> Path | None:
    """Copy SteamFix (+ EpicFix when found) from an existing FreeTP-style install."""
    dest = kit_dir(plugin_root, settings)
    dest.mkdir(parents=True, exist_ok=True)

    steam = _harvest_named_files(
        settings, ("SteamFix64.dll", "winmm.dll", "SteamFix32.dll", "winmm32.dll", "winmm.txt")
    )
    if "SteamFix64.dll" not in steam or "winmm.dll" not in steam:
        return None if not ((dest / "SteamFix64.dll").is_file() and (dest / "winmm.dll").is_file()) else dest

    for name, src in steam.items():
        try:
            shutil.copy2(src, dest / name)
        except OSError as exc:
            sys.stderr.write(f"online_fix: failed to copy {src}: {exc}\n")
            return None

    # If we harvested an x86 winmm (Isaac / FreeTP 32-bit packs), keep winmm32.dll
    # so x64 kits can still ship a separate winmm.dll for AMD64 titles.
    harvested_winmm = dest / "winmm.dll"
    if harvested_winmm.is_file() and _pe_machine(harvested_winmm) == _PE_I386:
        try:
            shutil.copy2(harvested_winmm, dest / "winmm32.dll")
        except OSError as exc:
            sys.stderr.write(f"online_fix: winmm32 harvest: {exc}\n")
    # Prefer keeping kit winmm.dll as x64 when both exist.
    # (Caller may already have x64 winmm; don't overwrite with x86.)
    elif (dest / "winmm32.dll").is_file() and _pe_machine(dest / "winmm32.dll") != _PE_I386:
        try:
            (dest / "winmm32.dll").unlink()
        except OSError:
            pass

    # Prefer a FreeTP pack that already bundles EpicFix (Meccha-style).
    epic = _harvest_named_files(
        settings, ("EpicFix64.dll", "EpicFix.ini"), prefer_with="SteamFix64.dll"
    )
    for name, src in epic.items():
        try:
            shutil.copy2(src, dest / name)
        except OSError as exc:
            sys.stderr.write(f"online_fix: failed to copy {src}: {exc}\n")

    if not (dest / "winmm.txt").is_file():
        (dest / "winmm.txt").write_text("SteamFix64.dll\n", encoding="utf-8")
    if (dest / "EpicFix64.dll").is_file() and not (dest / "EpicFix.ini").is_file():
        write_epic_fix_ini(dest / "EpicFix.ini")
    return dest


def ensure_online_fix_kit(plugin_root: Path, settings: dict[str, Any]) -> Path | None:
    dest = kit_dir(plugin_root, settings)
    dest.mkdir(parents=True, exist_ok=True)
    if (dest / "OnlineFix64.dll").is_file() or (dest / "OnlineFix.dll").is_file():
        return dest
    have_steam = (dest / "SteamFix64.dll").is_file() and (dest / "winmm.dll").is_file()
    have_epic = (dest / "EpicFix64.dll").is_file()
    if have_steam and have_epic:
        return dest
    # Refresh / fill missing pieces (especially EpicFix) from FreeTP installs.
    harvested = harvest_online_fix_kit(plugin_root, settings)
    if harvested is not None:
        return harvested
    return dest if have_steam else None


def _is_ue_shipping_exe(path: Path) -> bool:
    name = path.name.lower()
    if not name.endswith(".exe"):
        return False
    skip = (
        "crashreport",
        "crashpad",
        "ue4prereq",
        "ue5prereq",
        "bootstrap",
        "redist",
        "unrealversionselector",
        "dotnet",
    )
    return not any(s in name for s in skip)


def _skip_overlay_path(path: Path) -> bool:
    """Ignore DepotDownloader staging / engine tooling trees."""
    parts_l = [p.lower() for p in path.parts]
    skip = {
        ".depotdownloader",
        "staging",
        "intermediate",
        "saved",
        "deriveddatacache",
        "thirdparty",
        "downloads",
    }
    if any(p in skip for p in parts_l):
        return True
    try:
        idx = parts_l.index("engine")
        if path.parts[idx] == "Engine":
            return True
    except ValueError:
        pass
    return False


def find_ue_shipping_dirs(install_root: Path) -> list[Path]:
    root = install_root.resolve()
    out: list[Path] = []
    seen: set[str] = set()
    for win_name in ("Win64", "Win32"):
        for win_dir in root.rglob(win_name):
            if not win_dir.is_dir() or win_dir.name != win_name:
                continue
            if win_dir.parent.name != "Binaries":
                continue
            if _skip_overlay_path(win_dir):
                continue
            if "Engine" in win_dir.parts or "ThirdParty" in win_dir.parts:
                continue
            exes = [p for p in win_dir.glob("*.exe") if _is_ue_shipping_exe(p)]
            if not exes:
                continue
            key = str(win_dir.resolve())
            if key in seen:
                continue
            seen.add(key)
            out.append(win_dir)
    return out


def find_unity_roots(install_root: Path) -> list[Path]:
    root = install_root.resolve()
    out: list[Path] = []
    seen: set[str] = set()
    for data_dir in root.rglob("*_Data"):
        if not data_dir.is_dir() or _skip_overlay_path(data_dir):
            continue
        parent = data_dir.parent
        stem = data_dir.name[: -len("_Data")]
        if not (parent / f"{stem}.exe").is_file():
            continue
        key = str(parent.resolve())
        if key in seen:
            continue
        seen.add(key)
        out.append(parent)
    return out


def find_overlay_targets(install_root: Path) -> list[Path]:
    """Return the single FreeTP-style overlay directory (UE shipping > Unity > root)."""
    root = install_root.resolve()
    if not root.is_dir():
        return []

    ue = find_ue_shipping_dirs(root)
    if ue:
        # Prefer dirs that already contain EOSSDK / shipping with Redpoint (Meccha).
        def score(p: Path) -> tuple[int, int]:
            eos = 1 if game_has_eos(p) or (p / "RedpointEOS").is_dir() else 0
            shipping = 1 if list(p.glob("*Shipping*.exe")) else 0
            return (eos, shipping)

        ue.sort(key=score, reverse=True)
        return [ue[0]]

    unity = find_unity_roots(root)
    if unity:
        return [unity[0]]

    return [root]


def game_has_eos(install_root: Path) -> bool:
    """True if EOSSDK / Redpoint bits exist (may still not need EpicFix)."""
    try:
        if (install_root / "RedpointEOS").is_dir():
            return True
        for pattern in (
            "EOSSDK*.dll",
            "*EOSSDK*.dll",
            "EpicFix64.dll",
            "RedpointEOS",
        ):
            for path in install_root.rglob(pattern):
                if _skip_overlay_path(path):
                    continue
                if path.is_dir() and path.name.lower().startswith("redpointeos"):
                    return True
                if path.is_file():
                    return True
    except OSError:
        return False
    return False


def _pe_machine(path: Path) -> int:
    """PE Machine field: 0x14C = i386, 0x8664 = AMD64, 0 = unknown."""
    try:
        data = path.read_bytes()
    except OSError:
        return 0
    if len(data) < 0x40 or data[0:2] != b"MZ":
        return 0
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if pe + 6 >= len(data) or data[pe : pe + 4] != b"PE\0\0":
        return 0
    return int.from_bytes(data[pe + 4 : pe + 6], "little")


def _pe_has_export_name(path: Path, export_name: str) -> bool:
    """True when the DLL export table lists export_name (ASCII)."""
    try:
        data = path.read_bytes()
    except OSError:
        return False
    if len(data) < 0x40 or data[0:2] != b"MZ":
        return False
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if pe + 24 >= len(data) or data[pe : pe + 4] != b"PE\0\0":
        return False
    magic = int.from_bytes(data[pe + 24 : pe + 26], "little")
    if magic == 0x10B:
        dd = pe + 24 + 96
    elif magic == 0x20B:
        dd = pe + 24 + 112
    else:
        return False
    if dd + 8 > len(data):
        return False
    export_rva = int.from_bytes(data[dd : dd + 4], "little")
    if export_rva == 0:
        return False
    num_sections = int.from_bytes(data[pe + 6 : pe + 8], "little")
    opt_size = int.from_bytes(data[pe + 20 : pe + 22], "little")
    sec = pe + 24 + opt_size

    def rva_to_off(rva: int) -> int:
        for i in range(num_sections):
            s = sec + i * 40
            if s + 40 > len(data):
                break
            va = int.from_bytes(data[s + 12 : s + 16], "little")
            raw_size = int.from_bytes(data[s + 16 : s + 20], "little")
            raw_ptr = int.from_bytes(data[s + 20 : s + 24], "little")
            virt_size = int.from_bytes(data[s + 8 : s + 12], "little")
            span = max(raw_size, virt_size)
            if va <= rva < va + span and raw_ptr:
                return rva - va + raw_ptr
        return -1

    exp = rva_to_off(export_rva)
    if exp < 0 or exp + 40 > len(data):
        return False
    names_count = int.from_bytes(data[exp + 24 : exp + 28], "little")
    names_rva = int.from_bytes(data[exp + 32 : exp + 36], "little")
    names_off = rva_to_off(names_rva)
    if names_off < 0:
        return False
    needle = export_name.encode("ascii")
    for i in range(min(names_count, 4096)):
        entry = names_off + i * 4
        if entry + 4 > len(data):
            break
        name_rva = int.from_bytes(data[entry : entry + 4], "little")
        name_off = rva_to_off(name_rva)
        if name_off < 0 or name_off >= len(data):
            continue
        end = data.find(b"\0", name_off)
        if end < 0:
            continue
        if data[name_off:end] == needle:
            return True
    return False


_PE_I386 = 0x14C
_PE_AMD64 = 0x8664


def _resolve_kit_winmm32(kit: Path) -> Path | None:
    named = kit / "winmm32.dll"
    if named.is_file() and _pe_machine(named) == _PE_I386:
        return named
    plain = kit / "winmm.dll"
    if plain.is_file() and _pe_machine(plain) == _PE_I386:
        return plain
    return None


def _pe_section_names(path: Path) -> list[str]:
    try:
        data = path.read_bytes()
    except OSError:
        return []
    if len(data) < 0x40 or data[0:2] != b"MZ":
        return []
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if pe + 24 >= len(data) or data[pe : pe + 4] != b"PE\0\0":
        return []
    num_sections = int.from_bytes(data[pe + 6 : pe + 8], "little")
    opt_size = int.from_bytes(data[pe + 20 : pe + 22], "little")
    sec = pe + 24 + opt_size
    names: list[str] = []
    for i in range(num_sections):
        s = sec + i * 40
        if s + 8 > len(data):
            break
        raw = data[s : s + 8]
        names.append(raw.split(b"\0", 1)[0].decode("ascii", errors="ignore"))
    return names


def _looks_steamstubbed(path: Path) -> bool:
    """True when the PE still has SteamStub's `.bind` section."""
    return ".bind" in _pe_section_names(path)


def try_unpack_steamstub(exe: Path, kit: Path) -> bool:
    """Run Steamless on a SteamStubbed launch exe (Windows only).

    SteamStub talks to Steam with the real AppId *before* winmm/SteamFix load.
    That opens the Steam install/license UI even when steam_appid.txt=480.
    """
    if os.name != "nt":
        return False
    if not exe.is_file() or not _looks_steamstubbed(exe):
        return False
    cli = kit / "steamless" / "Steamless.CLI.exe"
    if not cli.is_file():
        sys.stderr.write(
            "online_fix: Steamless.CLI.exe missing in online_fix_kit/steamless; "
            f"SteamStub on {exe.name} will hit Steam license checks\n"
        )
        return False
    unpacked = Path(str(exe) + ".unpacked.exe")
    if unpacked.is_file():
        try:
            unpacked.unlink()
        except OSError:
            pass
    try:
        proc = subprocess.run(
            [str(cli), str(exe)],
            cwd=str(exe.parent),
            capture_output=True,
            text=True,
            timeout=180,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        sys.stderr.write(f"online_fix: Steamless failed on {exe.name}: {exc}\n")
        return False
    if not unpacked.is_file():
        tail = (proc.stdout or proc.stderr or "").strip().splitlines()[-3:]
        sys.stderr.write(
            f"online_fix: Steamless did not unpack {exe.name}"
            + (f" ({'; '.join(tail)})" if tail else "")
            + "\n"
        )
        return False
    bak = Path(str(exe) + ".steamstub-bak")
    try:
        if not bak.is_file():
            shutil.copy2(exe, bak)
        shutil.copy2(unpacked, exe)
        sys.stderr.write(f"online_fix: unpacked SteamStub on {exe.name}\n")
        return True
    except OSError as exc:
        sys.stderr.write(f"online_fix: replace unpacked {exe.name}: {exc}\n")
        return False


def _ensure_classic_steam_api(target: Path) -> None:
    """Keep Valve-style steam_api.dll when a modern flat-API crack replaced it."""
    api = target / "steam_api.dll"
    if not api.is_file():
        return
    if _pe_has_export_name(api, "SteamAPI_Init"):
        return
    for bak_name in (
        "steam_api.dll.valve-bak",
        "steam_api.dll.arachnel-valve-bak",
        "steam_api.dll.bak",
    ):
        bak = target / bak_name
        if bak.is_file() and _pe_has_export_name(bak, "SteamAPI_Init"):
            modern = target / "steam_api.dll.arachnel-modern-bak"
            try:
                if modern.exists():
                    modern.unlink()
                api.replace(modern)
                shutil.copy2(bak, api)
            except OSError as exc:
                sys.stderr.write(f"online_fix: restore classic steam_api: {exc}\n")
            return


def _pe_mentions_dll(exe: Path, dll_name: str) -> bool:
    """Cheap PE scan: true if the binary likely imports/loads dll_name."""
    try:
        data = exe.read_bytes()
    except OSError:
        return False
    needle = dll_name.encode("ascii", errors="ignore").lower()
    return needle in data.lower()


def _steamfix_bootstrap_src(plugin_root: Path) -> Path | None:
    here = Path(__file__).resolve().parent
    for cand in (
        here / "assets" / "steamfix_bootstrap.exe",
        plugin_root / "worker" / "assets" / "steamfix_bootstrap.exe",
        plugin_root / "assets" / "steamfix_bootstrap.exe",
    ):
        if cand.is_file():
            return cand
    return None


def _write_fake_steam_appid(target: Path, fake_appid: str = "480") -> None:
    """Point Steamworks at an owned app (Spacewar) when winmm/SteamFix cannot load."""
    path = target / "steam_appid.txt"
    try:
        path.write_text(str(fake_appid).strip() + "\n", encoding="ascii")
    except OSError as exc:
        sys.stderr.write(f"online_fix: steam_appid.txt: {exc}\n")


def _install_steamfix_bootstrap(
    target: Path, plugin_root: Path, game_exe: Path | None = None
) -> Path | None:
    """Ensure non-winmm Steamworks titles get steam_appid.txt=480; no exe bootstrap."""
    del plugin_root  # reserved for future injector asset
    exe = game_exe or _find_launch_exe_for_overlay(target)
    if exe is None:
        return None
    if (target / f"{exe.stem}_Data").is_dir():
        return None
    if list(target.glob("*Shipping*.exe")):
        return None
    if _pe_mentions_dll(exe, "winmm.dll"):
        return None
    if not (
        game_has_steam_api(target)
        or (target / "steam_api64.dll").is_file()
        or (target / "steam_api.dll").is_file()
    ):
        return None
    _write_fake_steam_appid(target, "480")
    sys.stderr.write(
        f"online_fix: wrote steam_appid.txt=480 for non-winmm Steamworks exe {exe.name}\n"
    )
    return None


def game_has_steam_api(install_root: Path) -> bool:
    """True when the install ships Valve Steamworks (needs SteamFix without a license)."""
    try:
        for name in ("steam_api64.dll", "steam_api.dll", "libsteam_api.so"):
            for path in install_root.rglob(name):
                if _skip_overlay_path(path):
                    continue
                if path.is_file():
                    return True
    except OSError:
        return False
    return False


def game_needs_epic_fix(install_root: Path) -> bool:
    """EpicFix64 when the install talks to x64 EOS (Redpoint or EOSSDK).

    Win32 EOSSDK (Isaac etc.) must not require EpicFix64 - wrong arch and it
    used to abort the whole SteamFix embed.
    """
    try:
        for path in install_root.rglob("RedpointEOS"):
            if path.is_dir() and not _skip_overlay_path(path):
                return True
        for path in install_root.rglob("RedpointEOS*.dll"):
            if path.is_file() and not _skip_overlay_path(path):
                mach = _pe_machine(path)
                if mach in (0, _PE_AMD64):
                    return True
        for path in install_root.rglob("EOSSDK*.dll"):
            if not path.is_file() or _skip_overlay_path(path):
                continue
            if "win32" in path.name.lower():
                continue
            if _pe_machine(path) == _PE_I386:
                continue
            return True
    except OSError:
        return False
    return False


def write_steam_fix_ini(path: Path, appid: str, *, workshop: bool = False,
                         freetp_id: str | None = None,
                         dlc_lines: list[str] | None = None) -> None:
    """Match FreeTP SteamFix.ini layout (HydraGames Pratfall / Cat Mail)."""
    workshop_flag = "true" if workshop else "false"
    # FreeTP online packs rarely set FakeWorkshop; keep ini close to theirs.
    fake_ws = ""
    if dlc_lines:
        dlc = "".join(f"{line}\n" for line in dlc_lines)
    elif workshop:
        dlc = "0=dlc\n"
    else:
        dlc = ""
    freetp = ""
    if freetp_id:
        freetp = f"\n[FreeTP]\nId={freetp_id}"
    text = (
        "[Main]\r\n"
        f"RealAppId={appid}\r\n"
        "FakeAppId=480\r\n"
        "#Language=english\r\n"
        "BuildId=0\r\n"
        "\r\n"
        "[Misc]\r\n"
        # Overlay=true enables friends/invite UI. Promo site is suppressed by FreeTP/.hash.
        "Overlay=true\r\n"
        "FilterLobby=false\r\n"
        f"{fake_ws}"
        "\r\n"
        "[Interfaces]\r\n"
        "Apps=true\r\n"
        "Friends=true\r\n"
        "User=true\r\n"
        "Inventory=true\r\n"
        "Stats=true\r\n"
        "Storage=true\r\n"
        "Utils=true\r\n"
        f"Workshop={workshop_flag}\r\n"
        "\r\n"
        "[DLC]\r\n"
        f"{dlc.replace(chr(10), chr(13)+chr(10)) if dlc else ''}"
        f"{freetp.replace(chr(10), chr(13)+chr(10)) if freetp else ''}"
    )
    # Normalize: prefer CRLF FreeTP style without trailing junk.
    path.write_bytes(text.encode("ascii", errors="replace"))


def _find_freetp_overlay_dir_for_appid(appid: str, settings: dict[str, Any]) -> Path | None:
    """Directory that contains SteamFix.ini + winmm for this RealAppId (FreeTP pack)."""
    appid = str(appid).strip()
    if not appid:
        return None
    for root in _candidate_harvest_roots(settings):
        if not root.is_dir():
            continue
        try:
            for ini in root.rglob("SteamFix.ini"):
                if _skip_overlay_path(ini):
                    continue
                try:
                    text = ini.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                real = ""
                for line in text.splitlines():
                    if line.startswith("RealAppId="):
                        real = line.split("=", 1)[1].strip()
                        break
                if real != appid:
                    continue
                parent = ini.parent
                if (parent / "SteamFix64.dll").is_file() and (parent / "winmm.dll").is_file():
                    return parent
        except OSError:
            continue
    return None


def _find_launch_exe_for_overlay(target: Path) -> Path | None:
    """Pick the game exe that loads winmm/SteamFix from this overlay directory."""
    if not target.is_dir():
        return None
    best: Path | None = None
    best_score = -1
    try:
        exes = [p for p in target.iterdir() if p.is_file() and p.suffix.lower() == ".exe"]
    except OSError:
        return None
    for exe in exes:
        lower = exe.name.lower()
        if lower == "cmd.exe":
            continue
        if any(s in lower for s in ("crash", "unins", "redist", "prereq", "crashpad")):
            continue
        score = exe.stat().st_size
        if "launcher" in lower or "bootstrap" in lower:
            score -= 50 * 1024 * 1024
        stem = exe.stem
        if (target / f"{stem}.pck").is_file():
            score += 200 * 1024 * 1024
        if (target / f"{stem}_Data").is_dir():
            score += 200 * 1024 * 1024
        if "shipping" in lower or "win64" in lower:
            score += 100 * 1024 * 1024
        if score > best_score:
            best_score = score
            best = exe
    return best


def _freetp_hash_is_foreign(
    target: Path, appid: str, settings: dict[str, Any]
) -> bool:
    """True when FreeTP/.hash was copied from another install path (invalid ack)."""
    hash_path = target / "FreeTP" / ".hash"
    if not hash_path.is_file() or hash_path.stat().st_size != 4:
        return False
    donor = _find_freetp_overlay_dir_for_appid(appid, settings)
    if donor is None:
        return False
    try:
        if donor.resolve() == target.resolve():
            return False
    except OSError:
        return False
    donor_hash = donor / "FreeTP" / ".hash"
    if not donor_hash.is_file() or donor_hash.stat().st_size != 4:
        return False
    try:
        return hash_path.read_bytes() == donor_hash.read_bytes()
    except OSError:
        return False


def _cmd_stub_path(plugin_root: Path) -> Path | None:
    here = Path(__file__).resolve().parent
    for cand in (
        here / "assets" / "cmd_stub.exe",
        plugin_root / "worker" / "assets" / "cmd_stub.exe",
        plugin_root / "assets" / "cmd_stub.exe",
        plugin_root / "online_fix_kit" / "cmd_stub.exe",
    ):
        if cand.is_file():
            return cand
    return None


def _prime_freetp_hash(target: Path, plugin_root: Path) -> bool:
    """Best-effort FreeTP/.hash + native cmd.exe stub (do not patch SteamFix DLLs).

    SteamFix computes a path-bound 4-byte FreeTP/.hash inside VMProtect — we cannot
    forge it offline, and byte-patching SteamFix*.dll trips integrity ("File corrupted!").

    Plant a native no-op cmd.exe next to the game so CreateProcess("cmd.exe /c start …")
    hits the stub first (app dir before System32) and the promo browser never opens.
    On Linux/Proton we only plant the stub; on Windows we briefly launch the game
    so SteamFix can write a real path-local .hash.
    """
    hash_path = target / "FreeTP" / ".hash"
    if hash_path.is_file() and hash_path.stat().st_size == 4:
        return True
    stub_src = _cmd_stub_path(plugin_root)
    if stub_src is None:
        sys.stderr.write("online_fix: cannot prime FreeTP/.hash (cmd stub missing)\n")
        return False

    (target / "FreeTP").mkdir(exist_ok=True)
    (target / "FreeTP" / "UserData").mkdir(exist_ok=True)
    anon = target / "FreeTP" / "UserData" / "AnonFolderSave.txt"
    if not anon.is_file():
        try:
            anon.write_text("", encoding="utf-8")
        except OSError:
            pass

    stub_dst = target / "cmd.exe"
    try:
        shutil.copy2(stub_src, stub_dst)
    except OSError as exc:
        sys.stderr.write(f"online_fix: FreeTP stub plant failed: {exc}\n")
        return False

    if sys.platform != "win32":
        sys.stderr.write(
            "online_fix: planted cmd.exe stub for Proton "
            "(FreeTP/.hash primes on first launch)\n"
        )
        return True

    exe = _find_launch_exe_for_overlay(target)
    if exe is None:
        # Keep stub so the first real launch still cannot ShellExecute freetp.org.
        sys.stderr.write(
            "online_fix: cannot prime FreeTP/.hash yet (exe missing); "
            "left cmd.exe stub to block promo\n"
        )
        return False

    proc: subprocess.Popen[bytes] | None = None
    primed = False
    try:
        args = [str(exe)]
        if (target / f"{exe.stem}.pck").is_file():
            args.append("--steam")
        proc = subprocess.Popen(
            args,
            cwd=str(target),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.time() + 35.0
        while time.time() < deadline:
            if hash_path.is_file() and hash_path.stat().st_size == 4:
                primed = True
                break
            if proc.poll() is not None:
                break
            time.sleep(0.25)
        if not primed:
            primed = hash_path.is_file() and hash_path.stat().st_size == 4
        return primed
    except OSError as exc:
        sys.stderr.write(f"online_fix: FreeTP hash prime failed: {exc}\n")
        return False
    finally:
        if proc is not None and proc.poll() is None:
            try:
                proc.kill()
            except OSError:
                pass
            try:
                proc.wait(timeout=5)
            except Exception:
                pass
        try:
            subprocess.run(
                ["taskkill", "/F", "/IM", exe.name],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
        except OSError:
            pass
        # Only remove the stub after a successful path-local .hash. Otherwise the
        # next launch will ShellExecute freetp.org through System32\cmd.exe.
        hash_ok = hash_path.is_file() and hash_path.stat().st_size == 4
        if hash_ok and stub_dst.is_file():
            # Keep stub permanently — cheap insurance if SteamFix re-checks ack.
            # (Deleting it after prime was causing intermittent freetp.org opens.)
            sys.stderr.write(
                f"online_fix: FreeTP/.hash primed; keeping cmd.exe stub at {stub_dst}\n"
            )
        elif not hash_ok:
            sys.stderr.write(
                f"online_fix: FreeTP/.hash NOT primed; keeping cmd.exe stub at {stub_dst}\n"
            )


def _ensure_freetp_ack_folder(
    target: Path, appid: str, settings: dict[str, Any], plugin_root: Path
) -> None:
    """Create FreeTP/UserData and a path-local .hash so promo site is not opened.

    Never copy .hash from another FreeTP install — it is path-bound. Doing so makes
    SteamFix open freetp.org on every launch until the file is rewritten locally.
    """
    (target / "FreeTP").mkdir(exist_ok=True)
    userdata = target / "FreeTP" / "UserData"
    userdata.mkdir(exist_ok=True)
    anon = userdata / "AnonFolderSave.txt"
    if not anon.is_file():
        anon.write_text("", encoding="utf-8")
    hash_path = target / "FreeTP" / ".hash"
    if _freetp_hash_is_foreign(target, appid, settings):
        try:
            hash_path.unlink()
        except OSError:
            pass
    if hash_path.is_file() and hash_path.stat().st_size == 4:
        # Still ensure stub exists — blocks promo if SteamFix re-validates.
        stub_src = _cmd_stub_path(plugin_root)
        if stub_src is not None:
            try:
                shutil.copy2(stub_src, target / "cmd.exe")
            except OSError:
                pass
        return
    if not _prime_freetp_hash(target, plugin_root):
        # Last resort: disable SteamFix overlay promo until ack exists.
        ini = target / "SteamFix.ini"
        if ini.is_file():
            try:
                text = ini.read_text(encoding="utf-8", errors="replace")
                if "Overlay=true" in text:
                    ini.write_text(
                        text.replace("Overlay=true", "Overlay=false"),
                        encoding="utf-8",
                    )
                    sys.stderr.write(
                        "online_fix: set Overlay=false until FreeTP/.hash is primed\n"
                    )
            except OSError as exc:
                sys.stderr.write(f"online_fix: Overlay=false fallback failed: {exc}\n")


def write_epic_fix_ini(path: Path) -> None:
    # FreeTP EpicFix: NoAuth=0 → ShellExecute browser auth (often freetp.org).
    # Use numeric 1 — some builds parse true/false poorly (NoAuth=true can fail).
    path.write_bytes(b"[EOS]\r\nNoAuth=1\r\nDeviceAuth=true\r\n")


def write_winmm_txt(path: Path, *, include_epic: bool, use32: bool = False) -> None:
    # FreeTP packs often omit the trailing newline.
    # EpicFix64 is x64-only - never list it for 32-bit loaders.
    lines = ["SteamFix32.dll" if use32 else "SteamFix64.dll"]
    if include_epic and not use32:
        lines.append("EpicFix64.dll")
    path.write_bytes("\r\n".join(lines).encode("ascii"))


def overlay_complete(target: Path, *, need_epic: bool, want32: bool = False) -> bool:
    if not (target / "SteamFix.ini").is_file():
        return False
    if want32:
        if not (target / "SteamFix32.dll").is_file():
            return False
        if not (target / "winmm.dll").is_file():
            return False
        if _pe_machine(target / "winmm.dll") != _PE_I386:
            return False
        if not (target / "winmm.txt").is_file():
            return False
        txt = (target / "winmm.txt").read_text(encoding="utf-8", errors="replace")
        return "SteamFix32.dll" in txt
    if not (target / "SteamFix64.dll").is_file():
        return False
    if not (target / "winmm.dll").is_file():
        return False
    if not (target / "winmm.txt").is_file():
        return False
    txt = (target / "winmm.txt").read_text(encoding="utf-8", errors="replace")
    if "SteamFix64.dll" not in txt:
        return False
    if need_epic and not (target / "EpicFix64.dll").is_file():
        return False
    if need_epic:
        if "EpicFix64.dll" not in txt:
            return False
        if not (target / "EpicFix.ini").is_file():
            return False
    return True


def _ensure_epic_beside_eos(install_root: Path, kit: Path) -> int:
    """Place EpicFix next to every EOSSDK*.dll (FreeTP Meccha: RedpointEOS/).

    SteamFix+EpicFix in Binaries/Win64 alone is not enough — Redpoint loads
    EOSSDK from its own folder and needs EpicFix64.dll beside it, or Join/Play
    hits a null EOS session and CTDs (UE EXCEPTION_ACCESS_VIOLATION).
    """
    if not (kit / "EpicFix64.dll").is_file():
        return 0
    placed = 0
    try:
        eos_dlls = list(install_root.rglob("EOSSDK*.dll"))
    except OSError:
        return 0
    for eos_dll in eos_dlls:
        if _skip_overlay_path(eos_dll):
            continue
        if "win32" in eos_dll.name.lower() or _pe_machine(eos_dll) == _PE_I386:
            continue
        eos_dir = eos_dll.parent
        try:
            _copy_file_best_effort(kit / "EpicFix64.dll", eos_dir / "EpicFix64.dll")
            write_epic_fix_ini(eos_dir / "EpicFix.ini")
            placed += 1
        except OSError as exc:
            sys.stderr.write(f"online_fix: EpicFix beside {eos_dll.name}: {exc}\n")
    return placed


def _cleanup_stray_root_overlay(install_root: Path, primary: Path) -> None:
    """Remove mistaken SteamFix copies at install root when UE shipping is the real target."""
    try:
        root = install_root.resolve()
        primary = primary.resolve()
    except OSError:
        return
    if root == primary:
        return
    # Only strip root leftovers if root is not a Unity/exe overlay target itself.
    if any(p.is_dir() and p.name.endswith("_Data") for p in root.iterdir()):
        return
    for name in (
        "SteamFix64.dll",
        "SteamFix32.dll",
        "SteamFix.ini",
        "winmm.dll",
        "winmm.txt",
        "steam_appid.txt",
        "EpicFix64.dll",
        "EpicFix.ini",
    ):
        path = root / name
        if path.is_file():
            try:
                path.unlink()
            except OSError:
                pass


def _copy_file_best_effort(src: Path, dst: Path) -> None:
    """Copy binary; if destination is locked (game running), keep existing file."""
    try:
        shutil.copy2(src, dst)
    except OSError as exc:
        if dst.is_file() and dst.stat().st_size > 0:
            sys.stderr.write(f"online_fix: keep existing {dst.name} ({exc})\n")
            return
        raise


def embed_online_fix(
    install_path: str | Path,
    appid: str,
    plugin_root: Path,
    settings: dict[str, Any],
    *,
    analysis: dict[str, Any] | None = None,
    force: bool = False,
) -> dict[str, Any]:
    """Embed SteamFix (+ EpicFix when EOS) the way FreeTP packs do."""
    auto = settings.get("autoEmbedOnlineFix")
    if auto is None:
        auto = True
    if not auto and not force:
        return {"ok": True, "skipped": True, "reason": "disabled"}

    if analysis is None:
        analysis = {"needsOnlineFix": force}
    weak_ok = bool(settings.get("onlineFixOnWeakMultiplayer"))
    steam_class = str(analysis.get("steamFix") or "")
    if not steam_class:
        steam_class = "required" if analysis.get("needsOnlineFix") else (
            "optional" if analysis.get("hasWeakMultiplayer") else "none"
        )
    needs = steam_class == "required" or (weak_ok and steam_class == "optional") or (
        bool(analysis.get("needsOnlineFix")) and steam_class != "none"
    )
    root = Path(install_path)
    # Depot installs of Steamworks titles (even single-player) fail SteamAPI_Init
    # with "No licenses" unless FakeAppId overlay is present — store tags alone miss this.
    if not needs and not force and root.is_dir() and game_has_steam_api(root):
        needs = True
        steam_class = "required"
        analysis = {
            **analysis,
            "steamFix": "required",
            "needsOnlineFix": True,
            "reason": "steam_api present — SteamFix required without Steam license",
            "hasSteamApi": True,
        }
    if force:
        needs = True
    if not needs and not force:
        return {
            "ok": True,
            "skipped": True,
            "reason": analysis.get("reason") or "no-online-categories",
            "steamFix": steam_class,
            "epicFix": analysis.get("epicFix") or "none",
            "analysis": analysis,
        }

    if not root.is_dir():
        return {"ok": False, "error": f"install path missing: {root}"}

    kit = ensure_online_fix_kit(plugin_root, settings)
    if kit is None:
        return {
            "ok": False,
            "error": (
                "Online-Fix kit missing or quarantined (need SteamFix64+winmm or OnlineFix64). "
                "Restore the plugin kit, harvest FreeTP/OF.me binaries, or add a Windows Security "
                "exclusion for the Arachnel folder."
            ),
            "analysis": analysis,
        }

    targets = find_overlay_targets(root)
    if not targets:
        return {"ok": False, "error": "no overlay target directories found", "analysis": analysis}
    primary = targets[0]

    need_epic = game_needs_epic_fix(root) or game_needs_epic_fix(primary)
    if need_epic and not (kit / "EpicFix64.dll").is_file():
        # One more harvest pass focused on EpicFix (e.g. HydraGames Meccha).
        ensure_online_fix_kit(plugin_root, settings)
        kit = kit_dir(plugin_root, settings)
    use_epic = need_epic and (kit / "EpicFix64.dll").is_file()
    if need_epic and not use_epic:
        return {
            "ok": False,
            "error": (
                "Game uses RedpointEOS but EpicFix64.dll is not in online_fix_kit. "
                "Point onlineFixSourceDir at a FreeTP EOS fix folder "
                "(e.g. HydraGames\\MECCHA CHAMELEON\\Chameleon\\Binaries\\Win64)."
            ),
            "analysis": analysis,
            "target": str(primary),
        }

    workshop = bool(analysis.get("hasWorkshop"))
    applied: list[str] = []
    used_epic = False
    for target in targets:
        try:
            game_exe = _find_launch_exe_for_overlay(target)
            if game_exe is not None:
                try_unpack_steamstub(game_exe, kit)
                # Re-resolve in case Steamless rewrote the path's contents.
                game_exe = _find_launch_exe_for_overlay(target) or game_exe
            want32 = bool(game_exe and _pe_machine(game_exe) == _PE_I386)
            # Win32 titles never get EpicFix64.
            target_use_epic = use_epic and not want32
            _ensure_classic_steam_api(target)

            if not (overlay_complete(target, need_epic=target_use_epic, want32=want32) and not force):
                # Prefer byte-exact FreeTP overlay for this AppId (ini + dlls).
                # Do NOT copy FreeTP/.hash — it is path-bound and causes promo opens.
                donor = _find_freetp_overlay_dir_for_appid(str(appid), settings)
                if donor is not None and donor.resolve() != target.resolve():
                    names = [
                        "SteamFix.ini",
                        "winmm.txt",
                        "SteamFix32.dll" if want32 else "SteamFix64.dll",
                        "winmm.dll",
                    ]
                    if want32 and (donor / "SteamFix32.dll").is_file():
                        names.append("SteamFix32.dll")
                    if not want32:
                        names.extend(["SteamFix64.dll", "SteamFix32.dll"])
                        if target_use_epic:
                            names.extend(["EpicFix64.dll", "EpicFix.ini"])
                    for name in names:
                        src = donor / name
                        if src.is_file():
                            _copy_file_best_effort(src, target / name)
                    # Donor winmm must match exe arch.
                    donor_winmm = donor / "winmm.dll"
                    if want32 and donor_winmm.is_file() and _pe_machine(donor_winmm) != _PE_I386:
                        kit_w32 = _resolve_kit_winmm32(kit)
                        if kit_w32 is not None:
                            _copy_file_best_effort(kit_w32, target / "winmm.dll")
                else:
                    write_steam_fix_ini(target / "SteamFix.ini", str(appid), workshop=workshop)
                    if want32:
                        # Strip wrong-arch leftovers from a prior x64 embed.
                        for bad in ("SteamFix64.dll", "EpicFix64.dll", "EpicFix.ini"):
                            bad_path = target / bad
                            if bad_path.is_file():
                                try:
                                    bad_path.unlink()
                                except OSError:
                                    pass
                        if (kit / "SteamFix32.dll").is_file():
                            _copy_file_best_effort(kit / "SteamFix32.dll", target / "SteamFix32.dll")
                        kit_w32 = _resolve_kit_winmm32(kit)
                        if kit_w32 is not None:
                            _copy_file_best_effort(kit_w32, target / "winmm.dll")
                        else:
                            # No x86 winmm in kit - don't leave a broken x64 winmm.
                            winmm = target / "winmm.dll"
                            if winmm.is_file() and _pe_machine(winmm) == _PE_AMD64:
                                try:
                                    winmm.unlink()
                                except OSError:
                                    pass
                    else:
                        _copy_file_best_effort(kit / "SteamFix64.dll", target / "SteamFix64.dll")
                        _copy_file_best_effort(kit / "winmm.dll", target / "winmm.dll")
                        if (kit / "SteamFix32.dll").is_file():
                            _copy_file_best_effort(kit / "SteamFix32.dll", target / "SteamFix32.dll")

                # Always write FakeAppId for classic steam_api / Steamworks.NET.
                # Critical for x86 (Isaac) where winmm+SteamFix32 hooks Valve steam_api.
                _write_fake_steam_appid(target, "480")

            write_winmm_txt(
                target / "winmm.txt", include_epic=target_use_epic, use32=want32
            )
            if target_use_epic:
                epic_src = kit / "EpicFix64.dll"
                donor_epic = target / "EpicFix64.dll"
                if epic_src.is_file():
                    _copy_file_best_effort(epic_src, target / "EpicFix64.dll")
                elif not donor_epic.is_file():
                    return {
                        "ok": False,
                        "error": "EpicFix64.dll missing from kit and overlay target",
                        "applied": applied,
                        "analysis": analysis,
                    }
                write_epic_fix_ini(target / "EpicFix.ini")
                _ensure_epic_beside_eos(root, kit if (kit / "EpicFix64.dll").is_file() else target)
                used_epic = True
            elif want32:
                # Ensure x64 EpicFix is not sitting next to a 32-bit game.
                for bad in ("EpicFix64.dll", "EpicFix.ini", "SteamFix64.dll"):
                    bad_path = target / bad
                    if bad_path.is_file():
                        try:
                            bad_path.unlink()
                        except OSError:
                            pass

            _ensure_freetp_ack_folder(target, str(appid), settings, plugin_root)
            applied.append(str(target))
        except OSError as exc:
            return {"ok": False, "error": str(exc), "applied": applied, "analysis": analysis}

    _cleanup_stray_root_overlay(root, primary)

    bootstrap: Path | None = None
    try:
        bootstrap = _install_steamfix_bootstrap(primary, plugin_root)
    except Exception as exc:
        sys.stderr.write(f"online_fix: bootstrap: {exc}\n")

    marker = root / ".arachnel-steamidra"
    try:
        payload: dict[str, Any] = {}
        if marker.is_file():
            payload = json.loads(marker.read_text(encoding="utf-8"))
        # Prefer real game exe; steam_appid.txt handles non-winmm Steamworks.
        launch_exe = _find_launch_exe_for_overlay(primary)
        if launch_exe is not None:
            payload["launch"] = {"type": "exe", "path": str(launch_exe)}
        payload["onlineFix"] = {
            "embedded": True,
            "targets": applied,
            "epic": used_epic,
            "steamAppIdFile": (primary / "steam_appid.txt").is_file(),
            "layout": "ue-shipping" if primary.name in ("Win64", "Win32") else "unity-or-root",
            "analysis": {
                "needsOnlineFix": analysis.get("needsOnlineFix"),
                "hasWorkshop": analysis.get("hasWorkshop"),
                "categoryIds": analysis.get("categoryIds"),
                "categoryLabels": analysis.get("categoryLabels"),
                "hasSteamApi": analysis.get("hasSteamApi"),
            },
        }
        marker.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    except Exception:
        pass

    return {
        "ok": True,
        "embedded": True,
        "targets": applied,
        "epic": used_epic,
        "steamAppIdFile": (primary / "steam_appid.txt").is_file(),
        "kit": str(kit),
        "analysis": analysis,
    }
