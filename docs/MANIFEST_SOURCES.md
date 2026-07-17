# Steam manifest sources (SteaMidra)

What we need to **install**: depot **keys** + packaged **`.manifest`** (or lua with `setManifestid` **and** local manifests).  
Keys alone are **not** enough — Steam CDN often returns **401** for anonymous manifest download.

## Quick matrix

| Source | Free? | Has `.manifest`? | Bulk catalog? | Notes |
|--------|-------|------------------|---------------|-------|
| **MH3** [steamtools.games](https://steamtools.games) | Yes | Yes (zip/lua) | Yes (throttle) | Best free primary. Rate limit / Cloudflare 429 |
| **revobd** `api.luagen.revobd.club/{id}.zip` | Yes | Yes when hit | Yes (HEAD) | Partial cache (MECCHA yes, Portal/Cat Mail often no) |
| **ManifestHub2** `SSMGAlt/ManifestHub2` branch zip | Yes | Yes | Yes (+ GH token) | ~62k branches, **frozen ~2025-07** |
| **Auiowu/ManifestAutoUpdate** branch zip | Yes | Yes | Yes | CN-style mirror of MH |
| **Walftech proxy** | Yes | Yes when hit | Optional | CN CDN proxy; uneven |
| **oureveryday** `depots/{id}.lua` | Was | lua only | No | **404 dead** (2026-07) |
| **Internet Archive `manifest-hub-repo`** | Yes | Yes (bulk) | Offline seed | ~19 GB dump — seed once, not daily HEAD |
| **Sudama / fallback_depotkeys** | Yes | **No** (keys only) | Keys catalog only | Never mark `hasManifest` from these alone |
| **steamcmd.net / SteamDB GIDs** | Yes | **No** binary | No | Knows manifest id → still 401 without file |
| **Hubcap** `hubcapmanifest.com` | **Paid / key** | Yes | Yes with secret | Best freshness (Cat Mail, new releases) |
| **Ryuu** `generator.ryuu.lol` | Freemium | Yes | Weak | Discord / daily quota |
| **Morrenus** | Discord | — | — | Merged into Hubcap lineage; old API dead |

## Verified examples (2026-07-17)

| App | Free mirrors | Hubcap |
|-----|--------------|--------|
| Portal **400** | MH3, ManifestHub2, Auiowu | yes |
| MECCHA **4704690** | **revobd** | — |
| Cat Mail **4380490** | **none** free | **yes** (`status: available`) |

Cat Mail CLI with Hubcap key: **install OK** → `CatMailCo.exe`.

## Runtime order (`fetch_lua`)

1. Hubcap (if `hubcapApiKey`)
2. MH3 zip/lua
3. ManifestHub2 / Auiowu branch zip
4. revobd zip
5. Walftech (optional)
6. Ryuu (if key)
7. Keys-only last resort → expect Steam 401

## Catalog CI order

1. MH3 HEAD (low concurrency, backoff on 429)
2. ManifestHub2 + Auiowu HEAD (GitHub token)
3. revobd HEAD
4. Optional Hubcap library dump if `HUBCAP_API_KEY` in CI secrets → include brand-new titles

Never set `hasManifest: true` from depotkeys-only.

## User settings

| Key | Effect |
|-----|--------|
| `hubcapApiKey` (`smm_…`) | Unlock Hubcap library + Cat Mail–class titles |
| `ryuuApiKey` | Extra paid/freemium mirror |
| `manifestCatalogUrl` | Federated installable JSON (GitLab) |
