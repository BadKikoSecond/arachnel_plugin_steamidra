# Steam plugin for Arachnel

User-facing name: **Steam**. The internal id remains `steamidra` so existing installations and settings keep working.

GPL-3 source plugin for [Arachnel](https://github.com/BadKiko/Arachnel): **self-contained** Steam depot installs via bundled **DepotDownloaderMod** + **.NET 9**, with **LumaCore** (Windows) / **SLSsteam** (Linux) when Steam is present.

Inspired by [SteaMidra / SFF](https://github.com/Midrags/SFF) (GPL-3). This repo does **not** vendor SFF Python modules.

## Catalog (installable only)

Daily CI rebuilds [`catalog/games-manifest-catalog.json`](catalog/games-manifest-catalog.json) — **only games we can actually download**: packaged manifests on MH3, revobd, or oureveryday (same mirrors as install). Not the full ~94k keys-only list.

Raw URL (used by the plugin):

```
https://gitlab.com/BadKiko/arachnel-plugin-steamidra/-/raw/master/catalog/games-manifest-catalog.json
```

`uploadDate` / `version` drive **sort by newest** and **library update detection**.

Rebuild locally:

```bash
# add newly key'd apps + any new mirror hits
python tools/build_manifest_catalog.py --incremental --steam-dates --verbose
# full re-probe of everything not yet in catalog:
python tools/build_manifest_catalog.py --steam-dates --verbose
```

GitLab: **CI/CD → Schedules** → cron `0 4 * * *` on branch `master`, set `CATALOG_PUSH_TOKEN` (project access token, `write_repository`). Optional: `HUBCAP_API_KEY`.

## Release

Push a semantic version tag to build and publish a universal `steam.arach`:

```bash
git tag v0.3.8
git push origin v0.3.8
```

GitLab builds Windows and Linux bundles, merges their platform runtimes, uploads the
package, and creates a Release. The toolchain is pinned in
`scripts/ci/launcher-toolchain.env`.

## What is bundled

| Component | Linux `.arach` | Windows `.arach` |
|-----------|----------------|------------------|
| DepotDownloaderMod | yes | yes |
| Portable .NET 9 | yes | yes |
| SLSsteam | yes | — |
| LumaCore | — | yes (or downloaded on first use) |

Fetch before release builds:

```bash
./tools/fetch_third_party.sh linux
# optional cross-prep:
./tools/fetch_third_party.sh windows
```

## Requirements

- CMake 3.20+, C++20, Qt 6.8+ (Core, Network), Ninja
- Python 3.10+ on PATH
- Arachnel checkout (`ARACHNEL_SDK_DIR`)
- **Steam optional** for download; recommended for library / Proton launch

## Build & install

```bash
export ARACHNEL_SDK_DIR=~/PetWork/Arachnel   # or D:\Work\Arachnel
./tools/fetch_third_party.sh linux           # or tools\fetch_third_party.ps1 on Windows
./run.sh                                     # or .\run.ps1
```

## Settings

| Key | Meaning |
|-----|---------|
| `preferMode` | `ddmod` (default), `native`, or `auto` |
| `libraryFolder` | Override install root when Steam is absent |
| `includeDlc` | Also download DLC depots (default `false`) |
| `hubcapApiKey` | Optional Hubcap `smm…` key for richer library dates |
| `manifestCatalogUrl` | Override catalog JSON URL |
| `manifestCatalogTtlHours` | Refresh interval (default `24`) |
| `steamPath` | Override Steam root |

## Modes

1. **ddmod (default)** — download depots with bundled DDMod. Writes ACF + SLS/Luma unlock when Steam exists.
2. **native** — prepare keys/lua and queue `steam://install/{appid}`.

## Limits

Needs a **depot key + manifest**. Catalog lists only titles with a known packaged manifest.

## License

GPL-3.0-or-later — [LICENSE](LICENSE). Third-party: [NOTICE.md](NOTICE.md).

Requires Arachnel **plugin API v3** (`owns_download`).
