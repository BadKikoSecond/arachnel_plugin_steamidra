# Linux kit (online-fix.me)

Harvested from Online-Fix.me *Fix Repair* archives.

## x64 (default payload)

Consensus DLLs from ~20 Steam_Generic Fix Repair packs:

| File | Notes |
|------|--------|
| `OnlineFix64.dll` | Majority hash across Steam_Generic fixes |
| `SteamOverlay64.dll` | Shared overlay loader |
| `StubDRM64.dll` | Present in a subset of fixes |
| `dlllist.txt` | `OnlineFix64` + `SteamOverlay64` + `StubDRM64` |
| `OnlineFix.ini` | Generic `FakeAppId=480` (game-specific DLC lists stripped) |

## x86 (on demand)

OF.me does **not** ship `OnlineFix32.dll`. 32-bit Unity titles (e.g. Galaxy Burger) use:

| File | Notes |
|------|--------|
| `OnlineFix.dll` | PE32 (i386) — unversioned name |
| `SteamOverlay32.dll` | Overlay loader |
| `winmm32.dll` | Injector (copied to game as `winmm.dll`) |

Packaged as **`x86_optional.zip`** only (never loose PE next to the plugin — Defender hygiene).
Unpacked `x86_optional/` is kept in the repo for regenerating the zip; `CopyOnlineFixKit.cmake` skips that directory and ships the zip.

Embed writes `dlllist.txt` as `SteamOverlay32.dll` then `OnlineFix.dll` (Galaxy Burger order).

**Not covered by the generic kit:** game-specific Fix Repair extras such as Unity `Plugins/x86/steam_api.dll`, custom `GameAssembly.dll` / `Custom.dll` entries, or per-title `RealAppId` / DLC lists in `OnlineFix.ini`. Those still need a game-specific fix when the generic overlay is not enough.

## Re-harvest

CDN `uploads.online-fix.me:2053` often fails TLS from some hosts; use a machine that can reach it (e.g. kikopi via SSH MCP):

```bash
# download Fix Repair/*.rar with Referer = Fix Repair folder URL
# password: online-fix.me
./tools/merge_onlinefix_me_archives.sh ~/ofme_harvest/dl/fixes
# for x86 sample (Galaxy Burger etc.):
# extract OnlineFix.dll + SteamOverlay32.dll + winmm.dll → x86_optional/
# zip -j x86_optional.zip OnlineFix.dll SteamOverlay32.dll winmm32.dll
```

Password for archives: `online-fix.me`
