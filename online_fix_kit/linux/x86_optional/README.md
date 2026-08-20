# x86 optional (online-fix.me)

OF.me 32-bit layout (no `OnlineFix32.dll`):

- `OnlineFix.dll` (PE32)
- `SteamOverlay32.dll`
- `winmm32.dll` (copied to the game as `winmm.dll`)

Kept unpacked here to regenerate `../x86_optional.zip`. Packaging skips this
directory and ships only the zip (Defender hygiene).
