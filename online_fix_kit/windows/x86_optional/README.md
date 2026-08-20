# Optional 32-bit SteamFix pair

`SteamFix32.dll` + `winmm32.dll` are kept here for regenerating `../x86_optional.zip`.
Packaging copies **only the zip** into the plugin payload (not these PE files), so
Defender is less likely to quarantine the AppData plugin tree on sight.

Embed extracts the zip into a temp dir when the game exe is PE i386, copies into the
game folder, then discards the temp files (does not leave 32-bit PE next to the plugin).
