# Building the firmware on Windows

`peridotPkg` builds on Windows without WSL, but EDK2 hits five environment
problems on the way. Each one produces a misleading error, so they are listed
with the symptom first.

## Toolchain

| Tool | Where it came from | Note |
| --- | --- | --- |
| clang / lld / llvm-ar | llvm-mingw `20261020-ucrt-x86_64.zip` | portable, no installer |
| `lld-link.exe` | copy of `ld.lld.exe` in the same `bin` | lld dispatches on `argv[0]` |
| GNU make | w64devkit `make.exe` | must be copied **into the clang `bin` dir**, see below |
| iasl | `edk2-acpica-iasl` from the Project Mu NuGet feed | `Windows-x86` build works on x64 |
| EDK2 Python deps | `pip install --target` | needs an elevated shell, see below |

## 1. MAX_PATH

**Symptom:** `FatalError: 65535` from `PerformAutoGen`, no message, zero `.obj`
files produced.

The real error is `WinError 3` inside an AutoGen worker:
`...\ReportStatusCodeRouter\RuntimeDxe\ReportStatusCodeRouterRuntimeDxe\OUTPUT\...`
reaches 258 characters under a deep checkout path.

**Fix:** build from a short root. A real short directory works; a junction does
not, because `os.getcwd()` resolves it back to the long path and EDK2 then
computes drive-relative paths (`No rule to make target '\Silicon\...'`).

## 2. `make` must live in the clang bin directory

**Symptom:** `error 7000: Failed to execute command` for the first module, with
no compiler output.

`Conf/tools_def.txt` defines `*_CLANGPDB_*_MAKE_PATH = ENV(CLANG_HOST_BIN)make`,
so make is looked up next to the compiler, not on `PATH`. A `make.exe` that only
handles the BootShim rules also fails here: EDK2 generates GNU makefiles.

**Fix:** put a real GNU make at `$(CLANG_HOST_BIN)make.exe`.

## 3. `Trim.exe` is missing from Mu-Basetools

**Symptom:** `'Trim' is not recognized as an internal or external command`.

The Mu-Basetools bundle ships `GenFw`, `GenFfs`, `VfrCompile` and friends but no
`Trim`. `edk2-basetools` on PyPI contains a Python implementation without a
console script when installed with `--target`.

**Fix:** add a `Trim.bat` to the BaseTools bin directory:

```bat
@echo off
"C:\Python313\python.exe" "<pylibs>\edk2basetools\Trim\Trim.py" %*
```

## 4. clang cannot create temporary files

**Symptom:** `clang-23: error: unable to make temporary file: Permission denied`
while assembling `BootShim.S`.

**Fix:** point `TEMP` and `TMP` at a directory the build user can write.

## 5. `build_uefi.py` keeps its paths relative

**Symptom:** `parse_device_config` returns `None` even though
`Resources/Configs/peridot.toml` exists.

`RESOURCE_CONFIGS_PATH` and `BUILD_PATH` are relative strings, so the script
must run with the Mu-Silicium checkout as the working directory.

## Elevated installs and ACLs

`pip install --target` and `stuart_update` need an elevated shell (EDK2 uses
`multiprocessing.Queue`, which needs named pipes). Files created that way are
not readable by the normal user afterwards:

```powershell
icacls <dir> /grant "Users:(OI)(CI)(RX)" /T
```

## Reproducing the build

```powershell
peridot\tools\run-build-direct.ps1 -Clean
```

It sets `WORKSPACE`, `PACKAGES_PATH`, `EDK_TOOLS_BIN`, `PYTHONPATH`, `TEMP` and
`PATH`, then calls `BaseTools/Source/Python/build/build.py` directly. Calling
`stuart` instead swallows the compiler output, which is why the direct call is
preferred while debugging.

Output: `Build/peridotPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd` (3 MiB).

Package it for the device with header v0 and page size 4096 — see
[UEFI bring-up](uefi-bringup.md).
