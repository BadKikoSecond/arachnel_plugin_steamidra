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
| `onlineFixSourceDir` | Folder with FreeTP (`SteamFix64`+`winmm`) or OF.me (`OnlineFix64`) kit (else harvest into `online_fix_kit/windows` or `linux/`) |

## When is an online fix needed?

| Class | Steam Store anchors | Action |
|-------|---------------------|--------|
| **none** | No Online PvP/Co-op/MMO/Cross-Platform (SP only, or local/LAN/split only) | Do not embed |
| **steam** | Category **36** Online PvP, **38** Online Co-op, **20** MMO, and/or **27** Cross-Platform | Embed SteamFix (+ winmm) |
| **steam + eos** | Same as steam, **and** install has **RedpointEOS** (not plain EOSSDK alone) | Also embed EpicFix (`NoAuth=1`) |

EpicFix is **never** decided from the Store alone. Plain `EOSSDK*.dll` (e.g. Pratfall) often needs **SteamFix only** — FreeTP does not ship EpicFix there; forcing EpicFix opens freetp.org and can black-screen the game.

## Online fix layout

Platform kits under `online_fix_kit/`:

- **Windows** — FreeTP SteamFix x64 (+ `x86_optional.zip` for 32-bit on demand)
- **Linux** — online-fix.me x64 + `x86_optional.zip` (`OnlineFix.dll` PE32 + Overlay32 + winmm32)
- **Universal** — Windows kit flat; Linux kit as `linux.zip` only (no loose OF.me PE for Defender)

Harvest:

```powershell
.\tools\harvest_online_fix_kit.ps1          # FreeTP → online_fix_kit/windows
.\tools\harvest_onlinefix_me_kit.ps1        # OF.me → online_fix_kit/linux
```

```bash
./tools/harvest_onlinefix_me_kit.sh
```

Embed targets:

- **Unity** — next to `<Game>.exe` + `*_Data`
- **Unreal** — only `<Game>/Binaries/Win64` (never `Engine/`, never install root)
- **EOS** — only when install has **RedpointEOS**: `EpicFix64.dll` + `EpicFix.ini` (`NoAuth=1`).
  On Linux (OF.me kit) EpicFix is loaded from sibling `online_fix_kit/windows/`; on Windows
  from the FreeTP kit. Listed in `winmm.txt` (SteamFix) or `dlllist.txt` (OF.me). Also copied
  next to every `EOSSDK*.dll`. Plain EOSSDK without Redpoint → no EpicFix.

Defender note: loose `SteamFix32.dll` is not shipped in the default Windows plugin payload.

## Modes

**depotkit** — download depots with the linked CDN client.

## Limits

Needs a **depot key + local manifest**. Catalog lists only titles with a known packaged manifest.

## License

GPL-3.0-or-later — [LICENSE](LICENSE). Third-party: [NOTICE.md](NOTICE.md).

Requires Arachnel **plugin API v4** (`owns_download`).
