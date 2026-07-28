# SteamFix / Online-Fix overlay binaries (not redistributed as source).

Required:
- `SteamFix64.dll`, `winmm.dll` (x64) - Unity/UE Win64 shipping

Optional but needed for 32-bit titles (Isaac, old Unity x86):
- `SteamFix32.dll`
- `winmm32.dll` (x86 FreeTP winmm proxy; copied to `winmm.dll` next to the game)

Required for EOS/Redpoint titles (Meccha etc.):
- `EpicFix64.dll` + `EpicFix.ini`

SteamStub DRM (Windows):
- `steamless/` with `Steamless.CLI.exe` + plugins
- Embed runs Steamless on launch exes that still have a `.bind` section.
  Without this, SteamStub talks to Steam with the real AppId before winmm loads
  and pops the Steam install / license dialog (even when `steam_appid.txt=480`).

Notes:
- Old games often need the Valve `steam_api.dll` (classic `SteamAPI_Init` exports).
  Do not replace it with a modern flat-API crack; SteamFix hooks the Valve DLL.
- Win32 `EOSSDK` must not force `EpicFix64` (wrong arch).
- Embed always writes `steam_appid.txt=480` (FakeAppId) for Steamworks fallback.

Used by `worker/online_fix_embed.py` and `src/online_fix_embed.cpp` after depot installs.
CI packages this folder into `steam.arach` for both platforms.
EpicFix must also be copied beside x64 `EOSSDK*.dll` (e.g. `RedpointEOS/`).
