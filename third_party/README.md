# Vendored runtimes (self-contained plugin)

Layout (OS-specific packages):

```
third_party/
  linux/
    DDMod/          DepotDownloaderMod.dll + deps
    SLSsteam/       SLSsteam.so, library-inject.so
    dotnet/         portable .NET 9 runtime
  windows/
    DDMod/          DepotDownloaderMod.dll + deps
    LumaCore/       dwmapi.dll / LumaCore DLLs (optional; else downloaded at runtime)
    dotnet/         portable .NET 9 runtime
```

## Fetch

```bash
# Linux host packaging a Linux .arach
./tools/fetch_third_party.sh linux

# Cross-prepare Windows runtime from Linux (optional)
./tools/fetch_third_party.sh windows
```

```powershell
.\tools\fetch_third_party.ps1
```

CMake copies `third_party/<host-os>/` into the plugin bundle. Large `dotnet/` trees are gitignored — run the fetch script before release builds.

DepotDownloaderMod is GPL-2. SLSsteam / LumaCore — see `NOTICE.md`.
