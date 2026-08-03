# Vendored runtimes (self-contained plugin)

Depot downloads use **depotkit** (linked into `steamidra_plugin`). This tree is only for Steam injectors.

```
third_party/
  linux/
    SLSsteam/       SLSsteam.so, library-inject.so
  windows/
    LumaCore/       dwmapi.dll / LumaCore DLLs (optional; else downloaded at runtime)
```

## Fetch

```bash
./tools/fetch_third_party.sh linux
./tools/fetch_third_party.sh windows   # optional cross-prep
```

```powershell
.\tools\fetch_third_party.ps1
```

CMake copies `third_party/<host-os>/` into the plugin bundle (skips legacy `DDMod` / `dotnet` if still on disk).

Build also needs a [depotkit](https://github.com/BadKiko/depotkit) checkout (`DEPOTKIT_DIR`, default sibling `../depotkit`).
