# arachnel-plugin-steamidra

GPL-3 source plugin for [Arachnel](https://github.com/PetWork/Arachnel): **self-contained** Steam depot installs via bundled **DepotDownloaderMod** + **.NET 9**, with **LumaCore** (Windows) / **SLSsteam** (Linux) when Steam is present.

Inspired by [SteaMidra / SFF](https://github.com/Midrags/SFF) (GPL-3). This repo does **not** vendor SFF Python modules.

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

## Build & install (Linux)

```bash
export ARACHNEL_SDK_DIR=~/PetWork/Arachnel
./tools/fetch_third_party.sh linux
chmod +x run.sh
./run.sh
```

## Settings

| Key | Meaning |
|-----|---------|
| `preferMode` | `ddmod` (default), `native`, or `auto` |
| `libraryFolder` | Override install root when Steam is absent |
| `includeDlc` | Also download DLC depots (default `false`) |
| `hubcapApiKey` | Optional Hubcap `smm…` key for full lua library |
| `steamPath` | Override Steam root |

## Modes

1. **ddmod (default)** — download depots with bundled DDMod into Steam library (or `~/Games/Arachnel/steamidra/`). Writes ACF + SLS/Luma unlock when Steam exists.
2. **native** — prepare keys/lua and queue `steam://install/{appid}` (often blocked by ownership ticket for unowned apps).

## Limits

Needs a **depot key + manifest** for the title. Denuvo / private branches / missing keys → install fails with an error. Catalog without Hubcap = Steam featured/search, not every app.

## License

GPL-3.0-or-later — [LICENSE](LICENSE). Third-party: [NOTICE.md](NOTICE.md).

Requires Arachnel **plugin API v3** (`owns_download`).
