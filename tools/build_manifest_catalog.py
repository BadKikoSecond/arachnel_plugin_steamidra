#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build installable-manifest catalog for SteaMidra (Arachnel plugin).

Only games with a packaged MH3/Hubcap lua (downloadable manifests).
Output: catalog/games-manifest-catalog.json — published via GitLab CI daily.

Schema (schemaVersion 1):
  entries[].delivery: "steam-manifest"
  entries[].sourceId: "steamidra"
  entries[].uploadDate: ISO-8601 (sort + update detection)
  entries[].version: date part of uploadDate

Usage:
  python tools/build_manifest_catalog.py --verbose
  python tools/build_manifest_catalog.py --incremental --verbose
  HUBCAP_API_KEY=smm… python tools/build_manifest_catalog.py --hubcap-pages 500
"""

from __future__ import annotations

import argparse
import email.utils
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
HUBCAP_BASE = "https://hubcapmanifest.com/api/v1"
MH3_LUA_URL = "https://steamtools.games/api/files/{appid}/lua"
STEAM_HEADER = "https://cdn.akamai.steamstatic.com/steam/apps/{appid}/header.jpg"
DEPOTKEYS_URLS = (
    "https://pub-d3ba7941fdf24c2c84da530b93221e1c.r2.dev/fallback_depotkeys.json",
    "https://raw.githubusercontent.com/KoriaPolis/Steam-Depot/main/fallback_depotkeys.json",
)
DEFAULT_OUTPUT = ROOT / "catalog" / "games-manifest-catalog.json"
ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}")


def http_request(
    url: str,
    *,
    method: str = "GET",
    headers: dict[str, str] | None = None,
    timeout: float = 60,
) -> tuple[int, dict[str, str], bytes]:
    req = urllib.request.Request(
        url,
        method=method,
        headers={"User-Agent": "Arachnel-SteaMidra-Catalog/1.0", **(headers or {})},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, dict(resp.headers.items()), resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, dict(exc.headers.items()), exc.read()


def parse_http_date(value: str) -> str:
    if not value:
        return ""
    try:
        dt = email.utils.parsedate_to_datetime(value)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    except Exception:
        return ""


def normalize_upload_date(raw: str) -> str:
    text = (raw or "").strip()
    if not text:
        return ""
    if "T" in text:
        return text.replace("+00:00", "Z")
    if ISO_RE.match(text):
        return f"{text}T00:00:00Z"
    return text


def fmt_size_label(n: int) -> str:
    if n <= 0:
        return ""
    units = ("B", "KB", "MB", "GB", "TB")
    v = float(n)
    u = 0
    while v >= 1024 and u < len(units) - 1:
        v /= 1024
        u += 1
    if u == 0:
        return f"{int(v)} {units[u]}"
    return f"{v:.1f} {units[u]}"


def now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def load_existing_catalog(path: Path) -> dict[str, dict[str, Any]]:
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    rows = data if isinstance(data, list) else data.get("entries") or data.get("games") or []
    out: dict[str, dict[str, Any]] = {}
    for item in rows:
        if not isinstance(item, dict):
            continue
        appid = str(item.get("steamAppId") or "").strip()
        if not appid.isdigit():
            continue
        out[appid] = item
    return out


def download_depotkeys_db(cache: Path) -> dict[str, Any]:
    cache.parent.mkdir(parents=True, exist_ok=True)
    if cache.is_file() and cache.stat().st_size > 1_000_000:
        age_h = (time.time() - cache.stat().st_mtime) / 3600
        if age_h < 24:
            return json.loads(cache.read_text(encoding="utf-8"))
    last_err = ""
    for url in DEPOTKEYS_URLS:
        try:
            status, _, body = http_request(url, timeout=300)
            if status != 200 or len(body) < 1_000_000:
                last_err = f"{url} status={status} size={len(body)}"
                continue
            cache.write_bytes(body)
            return json.loads(body.decode("utf-8", errors="replace"))
        except Exception as exc:
            last_err = str(exc)
    raise RuntimeError(f"Could not download depotkeys DB: {last_err}")


def build_depotkeys_games(keys_db: dict[str, Any]) -> dict[str, dict[str, Any]]:
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
            title and not title.startswith("App ") and len(title) >= len(prev.get("title") or "")
        ):
            apps[appid] = {
                "id": f"steam-{appid}",
                "title": title,
                "steamAppId": appid,
                "coverUrl": STEAM_HEADER.format(appid=appid),
            }
    return apps


def load_depotkeys_games_file(path: Path) -> dict[str, dict[str, Any]]:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    games = data if isinstance(data, list) else data.get("games") or []
    out: dict[str, dict[str, Any]] = {}
    for item in games:
        if not isinstance(item, dict):
            continue
        appid = str(item.get("steamAppId") or "").strip()
        if not appid.isdigit():
            sid = str(item.get("id") or "")
            if sid.startswith("steam-"):
                appid = sid[6:]
        if not appid.isdigit():
            continue
        out[appid] = item
    return out


def hubcap_headers(api_key: str) -> dict[str, str]:
    key = api_key.strip()
    if not key:
        return {}
    return {"Authorization": f"Bearer {key}"}


def fetch_hubcap_library(api_key: str, max_pages: int, page_size: int) -> list[dict[str, Any]]:
    headers = hubcap_headers(api_key)
    if not headers:
        return []
    entries: list[dict[str, Any]] = []
    seen: set[str] = set()
    for page in range(max_pages):
        params = {"limit": page_size, "offset": page * page_size, "sort_by": "updated"}
        url = f"{HUBCAP_BASE}/library?" + urllib.parse.urlencode(params)
        status, _, body = http_request(url, headers=headers)
        if status != 200:
            break
        data = json.loads(body.decode("utf-8", errors="replace"))
        batch = data.get("games") or []
        if not batch:
            break
        for item in batch:
            appid = str(item.get("game_id") or item.get("appid") or item.get("id") or "")
            if not appid.isdigit() or appid in seen:
                continue
            seen.add(appid)
            uploaded = normalize_upload_date(
                str(item.get("uploaded_date") or item.get("last_updated") or "")
            )
            size_raw = item.get("manifest_size") or item.get("size") or 0
            try:
                size_n = int(size_raw)
            except Exception:
                size_n = 0
            title = item.get("game_name") or item.get("name") or item.get("title") or f"App {appid}"
            entries.append(
                {
                    "id": f"steam-{appid}",
                    "sourceId": "steamidra",
                    "delivery": "steam-manifest",
                    "title": title,
                    "steamAppId": appid,
                    "coverUrl": STEAM_HEADER.format(appid=appid),
                    "uploadDate": uploaded,
                    "version": uploaded[:10] if uploaded else "",
                    "fileSize": fmt_size_label(size_n),
                    "hasManifest": True,
                    "manifestSource": "hubcap",
                }
            )
        total = int(data.get("total_count") or 0)
        if total and len(entries) >= total:
            break
        if len(batch) < page_size:
            break
    return entries


def probe_mh3_lua(appid: str) -> tuple[str, str]:
    url = MH3_LUA_URL.format(appid=appid)
    status, headers, _ = http_request(url, method="HEAD", timeout=12)
    if status != 200:
        return "", ""
    # Prefer Last-Modified when present; Date is just response time (weak).
    uploaded = parse_http_date(headers.get("Last-Modified", "")) or parse_http_date(
        headers.get("Date", "")
    )
    return appid, uploaded


def fetch_steam_release_date(appid: str) -> str:
    url = f"https://store.steampowered.com/api/appdetails?appids={appid}&l=english"
    try:
        status, _, body = http_request(url, timeout=15)
        if status != 200:
            return ""
        data = json.loads(body.decode("utf-8", errors="replace"))
        block = data.get(str(appid)) or {}
        if not block.get("success"):
            return ""
        release = (block.get("data") or {}).get("release_date") or {}
        if release.get("coming_soon"):
            return ""
        date_text = str(release.get("date") or "").strip()
        if not date_text:
            return ""
        for fmt in ("%d %b, %Y", "%b %d, %Y", "%d %B, %Y", "%B %d, %Y"):
            try:
                dt = datetime.strptime(date_text, fmt).replace(tzinfo=timezone.utc)
                return dt.strftime("%Y-%m-%dT00:00:00Z")
            except ValueError:
                continue
    except Exception:
        pass
    return ""


def merge_entry(
    base: dict[str, Any],
    *,
    upload_date: str,
    manifest_source: str,
    file_size: str = "",
) -> dict[str, Any]:
    entry = dict(base)
    entry["delivery"] = "steam-manifest"
    entry["sourceId"] = "steamidra"
    entry["hasManifest"] = True
    entry["manifestSource"] = manifest_source
    if upload_date:
        entry["uploadDate"] = normalize_upload_date(upload_date)
        entry["version"] = entry["uploadDate"][:10]
    if file_size and not entry.get("fileSize"):
        entry["fileSize"] = file_size
    if not entry.get("coverUrl") and entry.get("steamAppId"):
        entry["coverUrl"] = STEAM_HEADER.format(appid=entry["steamAppId"])
    if not entry.get("id") and entry.get("steamAppId"):
        entry["id"] = f"steam-{entry['steamAppId']}"
    return entry


def build_catalog(
    depotkeys: dict[str, dict[str, Any]],
    *,
    existing: dict[str, dict[str, Any]],
    hubcap_key: str,
    hubcap_pages: int,
    workers: int,
    max_probe: int,
    incremental: bool,
    steam_dates: bool,
    verbose: bool,
) -> dict[str, Any]:
    by_appid: dict[str, dict[str, Any]] = dict(existing)

    if hubcap_key:
        if verbose:
            print("Fetching Hubcap library…", file=sys.stderr)
        for entry in fetch_hubcap_library(hubcap_key, hubcap_pages, 100):
            appid = entry["steamAppId"]
            prev = by_appid.get(appid)
            # Hubcap dates are authoritative for updates when newer/present.
            if not prev or (entry.get("uploadDate") and entry["uploadDate"] >= (prev.get("uploadDate") or "")):
                by_appid[appid] = entry
        if verbose:
            print(f"  after Hubcap: {len(by_appid)}", file=sys.stderr)

    appids = list(depotkeys.keys())
    if max_probe > 0:
        appids = appids[:max_probe]

    if incremental:
        to_probe = [a for a in appids if a not in by_appid]
        if verbose:
            print(
                f"Incremental: {len(existing)} known, probing {len(to_probe)} new appids…",
                file=sys.stderr,
            )
    else:
        to_probe = [a for a in appids if a not in by_appid or not by_appid[a].get("hasManifest")]
        if verbose:
            print(f"Probing MH3 lua for {len(to_probe)} appids ({workers} workers)…", file=sys.stderr)

    mh3_hits = 0
    t0 = time.monotonic()
    with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        futures = {pool.submit(probe_mh3_lua, appid): appid for appid in to_probe}
        done = 0
        for fut in as_completed(futures):
            done += 1
            appid, weak_date = fut.result()
            if not appid:
                continue
            mh3_hits += 1
            base = depotkeys.get(appid) or by_appid.get(appid) or {
                "id": f"steam-{appid}",
                "title": f"App {appid}",
                "steamAppId": appid,
            }
            prev = by_appid.get(appid)
            # Keep a better previous uploadDate if we only got a weak HTTP Date.
            upload = weak_date
            if prev and prev.get("uploadDate") and prev.get("manifestSource") == "hubcap":
                upload = prev["uploadDate"]
            elif prev and prev.get("uploadDate") and (not upload or prev.get("dateKind") == "steam-release"):
                if prev.get("manifestSource") == "mh3" and prev.get("dateKind") != "steam-release":
                    upload = prev["uploadDate"]
            entry = merge_entry(
                {
                    "id": base.get("id") or f"steam-{appid}",
                    "title": base.get("title") or (prev or {}).get("title") or f"App {appid}",
                    "steamAppId": appid,
                    "coverUrl": base.get("coverUrl") or STEAM_HEADER.format(appid=appid),
                    "fileSize": base.get("sizeLabel") or base.get("fileSize") or (prev or {}).get("fileSize") or "",
                },
                upload_date=upload,
                manifest_source="mh3" if not (prev and prev.get("manifestSource") == "hubcap") else "hubcap",
            )
            by_appid[appid] = entry
            if verbose and done % 5000 == 0:
                elapsed = time.monotonic() - t0
                print(
                    f"  …{done}/{len(to_probe)} probed, {mh3_hits} hits ({elapsed:.0f}s)",
                    file=sys.stderr,
                )

    if verbose:
        print(f"  MH3 new hits: {mh3_hits} ({time.monotonic() - t0:.0f}s)", file=sys.stderr)

    if steam_dates:
        need = [a for a, e in by_appid.items() if not e.get("uploadDate")]
        if verbose:
            print(f"Steam release dates for {len(need)} undated entries…", file=sys.stderr)
        with ThreadPoolExecutor(max_workers=min(8, workers)) as pool:
            futs = {pool.submit(fetch_steam_release_date, a): a for a in need}
            for fut in as_completed(futs):
                appid = futs[fut]
                release = fut.result()
                if not release:
                    continue
                entry = by_appid[appid]
                entry["uploadDate"] = release
                entry["version"] = release[:10]
                entry["dateKind"] = "steam-release"

    # Drop entries that somehow lost hasManifest
    entries = [e for e in by_appid.values() if e.get("hasManifest")]
    entries.sort(key=lambda e: (e.get("uploadDate") or "", e.get("title") or ""), reverse=True)

    return {
        "schemaVersion": 1,
        "catalogKind": "installable-manifest",
        "updatedAt": now_iso(),
        "builtBy": "arachnel-plugin-steamidra/tools/build_manifest_catalog.py",
        "count": len(entries),
        "entries": entries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Build SteaMidra installable manifest catalog")
    parser.add_argument("--depotkeys-catalog", type=Path, default=None)
    parser.add_argument("--depotkeys-cache", type=Path, default=ROOT / "catalog" / ".cache" / "fallback_depotkeys.json")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--hubcap-key", default=os.environ.get("HUBCAP_API_KEY", ""))
    parser.add_argument("--hubcap-pages", type=int, default=500)
    parser.add_argument("--workers", type=int, default=48)
    parser.add_argument("--max-probe", type=int, default=0)
    parser.add_argument("--incremental", action="store_true", help="Only probe appids missing from existing catalog")
    parser.add_argument("--steam-dates", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    existing = load_existing_catalog(args.output)

    depotkeys: dict[str, dict[str, Any]] = {}
    if args.depotkeys_catalog and args.depotkeys_catalog.is_file():
        depotkeys = load_depotkeys_games_file(args.depotkeys_catalog)
        if args.verbose:
            print(f"Loaded depotkeys catalog: {len(depotkeys)}", file=sys.stderr)
    else:
        if args.verbose:
            print("Downloading fallback_depotkeys.json…", file=sys.stderr)
        keys_db = download_depotkeys_db(args.depotkeys_cache)
        depotkeys = build_depotkeys_games(keys_db)
        if args.verbose:
            print(f"Built from depotkeys DB: {len(depotkeys)} apps", file=sys.stderr)

    if not depotkeys:
        print("No depotkeys apps — abort", file=sys.stderr)
        return 1

    catalog = build_catalog(
        depotkeys,
        existing=existing,
        hubcap_key=args.hubcap_key,
        hubcap_pages=args.hubcap_pages,
        workers=args.workers,
        max_probe=args.max_probe,
        incremental=args.incremental,
        steam_dates=args.steam_dates,
        verbose=args.verbose,
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(catalog, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    # Keep a readable copy next to plugin config for offline bootstrap.
    bundled = ROOT / "config" / "games-manifest-catalog.json"
    bundled.parent.mkdir(parents=True, exist_ok=True)
    bundled.write_text(args.output.read_text(encoding="utf-8"), encoding="utf-8")
    print(f"Wrote {catalog['count']} entries → {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
