# Vendored runtimes (self-contained plugin)

Depot downloads use **depotkit** (linked into `steamidra_plugin`). Optional Linux Steam helper:

```
third_party/
  linux/
    SLSsteam/       SLSsteam.so, library-inject.so (optional)
```

Windows has no third_party injectors - no LumaCore.

## Fetch

```bash
./tools/fetch_third_party.sh linux
```

```powershell
.\tools\fetch_third_party.ps1
```

Build also needs a [depotkit](https://github.com/BadKiko/depotkit) checkout (`DEPOTKIT_DIR`, default sibling `../depotkit`).
