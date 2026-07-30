# Online-Fix kits (platform-split)

Packaging copies **one host-OS kit** into the flat `online_fix_kit/` inside `steam.arach`.
Universal merges keep the Windows kit flat under `online_fix_kit/windows/` and ship the
Linux kit as **`online_fix_kit/linux.zip` only** (no loose OnlineFix\*.dll) so Windows
Defender does not scan OF.me PE when the universal package is installed under `%AppData%`.
On Linux the plugin extracts `linux.zip` → `linux/` on first embed.

| Path | Layout | Used on |
|------|--------|---------|
| `windows/` | FreeTP SteamFix + winmm (+ EpicFix, Steamless) | Windows builds / Proton fallback |
| `linux/` | online-fix.me (`OnlineFix*`, overlays, `dlllist.txt`) | Linux builds (Proton) |

## Windows (`windows/`)

Required in the default payload (x64 only):

- `SteamFix64.dll`, `winmm.dll`
- `EpicFix64.dll` + `EpicFix.ini` (EOS/Redpoint titles)
- `steamless/` (SteamStub unpack on Windows)
- `cmd_stub.exe` (FreeTP promo stub → game `cmd.exe`)

32-bit is **not** unpacked in the plugin tree by default (Defender Dllhijack hits):

- Source PE: `windows/x86_optional/SteamFix32.dll` + `winmm32.dll`
- Packaged as: `windows/x86_optional.zip` (extracted only when a 32-bit game needs embed)

## Linux (`linux/`)

Harvest from online-fix.me Fix Repair packs (see `tools/harvest_onlinefix_me_kit.*`
and `linux/README.md`):

- Default: `OnlineFix64.dll`, `SteamOverlay64.dll`, `StubDRM64.dll`, `dlllist.txt`, `OnlineFix.ini`
- 32-bit (Unity / Galaxy Burger style): **`x86_optional.zip`** with `OnlineFix.dll` (PE32) +
  `SteamOverlay32.dll` + `winmm32.dll` — there is no `OnlineFix32.dll` on OF.me

If `linux/` has no OF.me binaries yet, CMake falls back to the Windows SteamFix **x64** kit
(no 32-bit PE folder) so Proton embeds still work.

Unpacked `linux/x86_optional/` is for regenerating the zip only; packaging ships the zip.

**EOS / Redpoint:** EpicFix is FreeTP-only and must **not** live under `linux/`. Linux packages
still ship a sibling `online_fix_kit/windows/EpicFix64.dll` (+ ini) so RedpointEOS titles
get EpicFix beside OF.me under Proton. Plain `EOSSDK*.dll` without Redpoint does **not** get
EpicFix.

## Notes

- Do not byte-patch SteamFix\*.dll (VMProtect → "File corrupted!").
- Embed writes `steam_appid.txt=480` (FakeAppId).
- Arachnel core already applies `WINEDLLOVERRIDES` for both FreeTP and OF.me layouts.
