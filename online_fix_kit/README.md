# SteamFix / Online-Fix overlay binaries (not redistributed as source).
# Required: SteamFix64.dll, winmm.dll
# Required for EOS/Redpoint titles (Meccha etc.): EpicFix64.dll + EpicFix.ini
# Optional: SteamFix32.dll, winmm.txt
#
# Used by worker/online_fix_embed.py after depot installs (Windows + Linux/Proton).
# CI packages this folder into steam.arach for both platforms.
# EpicFix must also be copied beside EOSSDK*.dll (e.g. RedpointEOS/).
