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

## 构建树的恢复（重要，踩过一次坑）

**不要 `rm -rf build/xnu`，也不要 `rm -rf <tree>`。**

`unixnu kernel` 的流水线是这样排的：

```
渲染树 → 应用补丁 → archive-01..08（需要 link.filelist）
       → 内核编译+链接（产生 link.filelist）
```

`link.filelist` 由内核链接阶段写在
`tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/link.filelist`，
而归档步骤排在链接**之前**、却要读它。也就是说这个文件必须**从上一次成功的构建里留下来**。

一旦删掉，就会陷入：

```
No XNU symbol names: run a kernel pass first (it writes link.filelist)
```

而且重跑无法自愈（每次都卡在同一处），删整棵树也一样——因为新树同样没有这个文件。

**恢复办法**：保留 `out/` 下除 `build/` 之外的一切，只让 unixnu 重新走完整流水线；
如果已经删了，只能等一次真正干净的 `out/peridot/` 重建（会重新下载 IPSW，比较慢）。

换句话说：**改补丁可以，清缓存不行。** 构建慢一点远比重建树安全。
