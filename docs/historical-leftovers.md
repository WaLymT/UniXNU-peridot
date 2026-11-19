# 历史遗留复盘（2026-11-19）

本文只做盘点，不删除构建树或源码备份。分类：

- **A：主线路障，必须处理**
- **B：实验代码/镜像，保留但需显式开关**
- **C：备份/临时文件，可安全清理**
- **D：有效基线，保留**

## A. 主线路障

### A1. DarwinBoot 无条件进入 Fastboot

文件：`C:\ms\DarwinBootPkg\Driver\DarwinBoot.c`

位置：`Boot()` 末尾，约 757 行：

```c
if (TRUE) {
  Print (L"\fDARWINBOOT: before fastboot\n");
  gBS->Stall (2000000);
  Status = DarwinFastbootMode (...);
  if (EFI_ERROR (Status)) {
    Print (L"DARWINBOOT: after fastboot status=%r\n", Status);
    CpuDeadLoop ();
  }
  CpuDeadLoop ();
}
Status = GetMap (...);
```

影响：

- 正常 XNU 启动被 Fastboot 常驻路径截断；
- 该块在 `GetMap` 之前，属于 Fastboot 常驻实验；
- `if (TRUE)`、打印和 `Stall` 是测试残留。

处理建议：改为 boot arg 控制（如 `peridot-fastboot=1`），默认关闭；正式主线删除无条件块。

### A2. UEFI 裸 MMIO CX 探针

文件：`C:\ms\DarwinBootPkg\Driver\DarwinBoot.c`

函数：`CxUefiTouch()`

影响：

- 直接写 GCC/GPUCC/GDSC，p522 黑屏；
- 与 A1 一起属于实验路径；
- 不应进入正常固件。

处理建议：移到实验分支或删除；保留 `CxClockProtocol` 的只读/协议路径。

### A3. Clock 协议探针位置

文件：`C:\ms\DarwinBootPkg\Driver\DarwinBoot.c`

函数：`CxClockProtocol()`

当前在 DarwinBoot 早期执行；p524 黑屏。

处理建议：先改成只读 `GetClockID` / `GetClockPowerDomainID`，并把 Enable 移到 `UniXNUDxe` 的显示/触摸供电之后。

## B. 实验代码/镜像

### B1. Adreno735 逐步 GDSC/CX 探针

文件：`peridot/UniXNU-peridot/drivers/Adreno735/NativeAdreno735.cpp`

boot arg：

- `peridot-gdsc=1..5`
- `peridot-rsc-cx=2`

状态：

- gated，默认不执行；
- p516 mode=1 成功；
- p517 mode=2 无变化；
- p518 mode=3 CX GDSC 写不进去；
- p521 `mmcx.lvl` 白屏。

建议保留为显式实验开关；默认启动路径不调用。

### B2. 无效/实验镜像

`peridot/out` 中以下镜像不再作为主线：

- p495：USB2-only descriptor，descriptor 无效；
- p500：hoshikv 驱动顺序，黑屏；
- p501：bootloader 认证失败；
- p502：组合 GDSC 写，黑屏；
- p503-p507：投票前 GDSC 探针，黑屏；
- p509-p515：投票后 GDSC 探针，黑屏；
- p521：mmcx 投票，白屏；
- p522：UEFI 裸 MMIO，黑屏；
- p523：bootloader 认证失败；
- p524：VDD_GPU_CX/GX 协议早期 Enable，黑屏。

### B3. hoshikv 参考文件

目录：`peridot/.tmp/ref-hoshikv/`

包含：

- `UniXNUGpuClock.c` / README（DinnerABC，不是 hoshikv）
- hoshikv 驱动/固件脚本
- hoshikv tar

属于本地参考，不是构建输入。

## C. 备份/临时文件

### C1. C:\ms 平台备份

- `Platforms/Xiaomi/peridotPkg/Include/APRIORI.inc.bak-order`
- `Platforms/Xiaomi/peridotPkg/Include/APRIORI.inc.bak-order2`
- `Platforms/Xiaomi/peridotPkg/Include/DXE.inc.bak-order`
- `Platforms/Xiaomi/peridotPkg/peridot.dsc.bak-advancedlog`
- `Platforms/Xiaomi/peridotPkg/peridot.fdf.bak-advancedlog`
- `Resources/DTBs/peridot.dtb.bak-usb`
- `Resources/DTBs/peridot.dts.bak-usb`
- `Silicon/Silicium/SiliciumPkg/Sec/**/*.orig`

### C2. Binaries 残留

- 8 个 `*.patched.efi`（Buttons/Clock/Pmic/SPMI/Tz/UFS/UsbConfig/UsbMsd）
- 8 个 `*.inf.bak`

当前 INF 已恢复 stock（`PE32|UsbConfigDxe.efi`），这些文件只是未跟踪残留，导致 `Binaries` submodule 脏。

### C3. 构建树备份

`C:\ms\Build\peridotPkg\DEBUG_CLANGPDB.bak-*` 共 23 个。

不要直接删除；它们包含不可再生构建状态。清理需先列清单并人工确认。

## D. 有效基线

- `p490`：GDSC 只读基线；
- `p516`：p490 FD + mode=1 只读，成功；
- `p517`：mode=2 GCC 写，无变化；
- `p518`：mode=3 CX GDSC 写，确认写不进；
- `p494`：DARWINBOOT UI 可用（USB 不枚举）。

## 优先处理顺序

1. 把 A1 的 `if (TRUE)` Fastboot 改为 boot arg 控制；
2. 把 A2/A3 的裸 MMIO/协议探针移出正常路径；
3. 用 p516 的 p490 FD 继续协议只读探针；
4. C 类只清理明确备份，不动 Build 树。
