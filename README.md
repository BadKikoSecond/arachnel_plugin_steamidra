# Steam plugin for Arachnel

User-facing name: **Steam**. The internal id remains `steamidra` so existing installations and settings keep working.

GPL-3 source plugin for [Arachnel](https://github.com/BadKiko/Arachnel): Steam depot installs via linked **[depotkit](https://github.com/BadKiko/depotkit)**, with optional **SLSsteam** on Linux when Steam is present.

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
git tag v0.6.0
git push origin v0.6.0
```

GitLab builds Windows and Linux bundles, merges their platform runtimes, uploads the
package, and creates a Release. The toolchain is pinned in
`scripts/ci/launcher-toolchain.env`.

## What is bundled

| Component | Linux `.arach` | Windows `.arach` |
|-----------|----------------|------------------|
| depotkit (static in plugin) | yes | yes |
| SLSsteam | optional | — |
| Online Fix kit | yes | yes |

No DepotDownloaderMod / .NET / LumaCore.

## Requirements

- CMake 3.20+, C++20, Qt 6.8+ (Core, Network), Ninja
- Arachnel checkout (`ARACHNEL_SDK_DIR`)
- [depotkit](https://github.com/BadKiko/depotkit) checkout (`DEPOTKIT_DIR`, default `../depotkit`)
- **Steam optional** for download; recommended for library / Proton launch

Python is only used in CI/dev for catalog rebuild tools.

## Build & install

```bash
export ARACHNEL_SDK_DIR=~/path/to/Arachnel
export DEPOTKIT_DIR=~/path/to/depotkit
./tools/fetch_third_party.sh linux           # or tools\fetch_third_party.ps1 on Windows
./run.sh                                     # or .\run.ps1
```

## Settings

| Key | Meaning |
|-----|---------|
| `preferMode` | Legacy; ignored. |
| `depotOs` | `windows` (default) / `linux` / `macos` - which platform depots to keep. |
| `libraryFolder` | Fallback install root if `--target` is omitted (never Steam `steamapps/common`) |
| `includeDlc` | Also download DLC depots (default `false`) |
| `hubcapApiKey` | Optional Hubcap `smm…` key for richer library dates |
| `manifestCatalogUrl` | Override catalog JSON URL |
| `manifestCatalogTtlMinutes` | Disk cache TTL before re-download (default `5`) |
| `manifestCatalogTtlHours` | Legacy; used only if minutes key is unset |
| `steamPath` | Override Steam root |
| `autoEmbedOnlineFix` | After install, embed SteamFix when Steam Store has Online PvP/Co-op/MMO (default `true`) |
| `onlineFixOnWeakMultiplayer` | Also embed on weak Multi-player/Co-op tags (default `false`) |
| `onlineFixSourceDir` | Folder with `SteamFix64.dll` + `winmm.dll` (else harvest from FreeTP installs into `online_fix_kit/`) |

## When is an online fix needed?

| Class | Steam Store anchors | Action |
|-------|---------------------|--------|
| **none** | No Online PvP/Co-op/MMO/Cross-Platform (SP only, or local/LAN/split only) | Do not embed |
| **steam** | Category **36** Online PvP, **38** Online Co-op, **20** MMO, and/or **27** Cross-Platform | Embed SteamFix (+ winmm) |
| **steam + eos** | Same as steam, **and** install has **RedpointEOS** (not plain EOSSDK alone) | Also embed EpicFix (`NoAuth=1`) |

EpicFix is **never** decided from the Store alone. Plain `EOSSDK*.dll` (e.g. Pratfall) often needs **SteamFix only** — FreeTP does not ship EpicFix there; forcing EpicFix opens freetp.org and can black-screen the game.

## Online fix layout

Binaries are harvested into `online_fix_kit/` (not shipped in git):
- **Unity** — next to `<Game>.exe` + `*_Data`
- **Unreal** — only `<Game>/Binaries/Win64` (never `Engine/`, never install root)
- **EOS** — `EpicFix64.dll` + `EpicFix.ini` `[EOS] NoAuth=true`; `winmm.txt` loads both DLLs

## Modes

**depotkit** — download depots with the linked CDN client.

## Limits

Needs a **depot key + local manifest**. Catalog lists only titles with a known packaged manifest.

## License

GPL-3.0-or-later — [LICENSE](LICENSE). Third-party: [NOTICE.md](NOTICE.md).

Requires Arachnel **plugin API v4** (`owns_download`).
