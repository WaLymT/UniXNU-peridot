# 借鉴代码与仓库来源

本仓库的 peridot 移植在以下项目的基础上完成。此处明确区分“直接依赖/二进制”、
“逻辑等价改写”和“只读审计参考”。未列出许可证的代码不直接复制；从其他仓库
改写的逻辑在本仓库对应文件中保留来源注释。

## 直接依赖或二进制来源

| 来源 | 用途 | 本仓库位置 | 许可证/说明 |
| --- | --- | --- | --- |
| [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) | UEFI 基座、peridotPkg 平台包、Silicium DXE 基础设施 | C:\ms 构建树；本仓库 board.toml、porting.toml | BSD-2-Clause-Patent；peridotPkg 上游标记 Inactive |
| [hoshikv/UniXNU-peridot](https://github.com/hoshikv/UniXNU-peridot) | DarwinBootPkg（含 Fastboot）、UniXNUPkg、patched vendor driver 组合、固件构建脚本 | C:\ms\DarwinBootPkg、C:\ms\Binaries\peridot\QcomPkg\Drivers、C:\ms\UniXNUPkg | BSD-2-Clause-Patent；patched .efi 按其 INF 引用 |
| [edk2-porting/edk2-msm](https://github.com/edk2-porting/edk2-msm) | Qualcomm UEFI 平台/DXE 驱动谱系 | Mu-Silicium 基座 | BSD-2-Clause-Patent |
| [edk2-porting/edk2-msm-binary](https://github.com/edk2-porting/edk2-msm-binary) | 早期 vendor DXE 二进制来源 | vendor driver 谱系 | 按其仓库说明使用 |

## 逻辑等价改写（不是文件复制）

| 来源 | 借鉴内容 | 本仓库对应位置 |
| --- | --- | --- |
| [yehua123-oos/UniXNU-muyu](https://github.com/yehua123-oos/UniXNU-muyu) | MuyuRpmhRsc.c/h 的 AP RSC ARC vote 流程；GPUCC cliffs 寄存器表 | drivers/Adreno735/NativeAdreno735.cpp 中的 RSC/TCS/GPUCC 逻辑；见 docs/gpu-rpmh-audit.md |
| [DinnerABC/UniXNU-cheeseburger](https://github.com/DinnerABC/UniXNU-cheeseburger) | UEFI EFI_CLOCK_PROTOCOL 的 GPU 时钟/电源域清点与 VDD_GPU_CX/GX 上电顺序 | C:\ms\DarwinBootPkg\Driver\DarwinBoot.c 的 Clock 协议探针；参考 rebuild/ipados-up1/gpu-clock/ |
| [tattwamashi/UniXNU-braveark](https://github.com/tattwamashi/UniXNU-braveark) | SM8635 GPU 电源/SMMU/PM4 host 检查 | docs/gpu-rpmh-audit.md 的对照项 |
| [Allenwdk/UniXNU-uke](https://github.com/Allenwdk/UniXNU-uke) | SM7675/iPadOS 27 arm64e GPU 栈对照 | docs/gpu-rpmh-audit.md 的对照项 |
| [Olegu621/xlamiOS-xnu-kernel](https://github.com/Olegu621/xlamiOS-xnu-kernel) | peridot 真机 GPU/RSC 行为对照 | docs/gpu-rpmh-audit.md 的对照项 |
| [Lux12138/NativeDwc3](https://github.com/Lux12138/NativeDwc3) | nabu/SM8150 DWC3 DRD 驱动思路 | docs/hackios-hardware-borrow.md 的 USB 条目；未直接使用 |
| [Linux kernel: gpucc-cliffs.c / qcom DTSI](https://git.kernel.org/) | GPUCC/GDSC 寄存器偏移、clock/power-domain 名称 | drivers/Adreno735/NativeAdreno735.cpp；名称和偏移按 GPL-2.0 上游资料核对 |

## 明确未直接复制的内容

- Apple XNU/iPadOS kernelcache 仅作为目标运行环境，未复制其代码；
- Mu-Silicium/hoshikv 的 vendor .efi 二进制按其仓库许可证和 INF 引用使用；
- 上述“逻辑等价改写”均按本仓库代码结构重写，不包含逐文件复制；
- 如后续引入新的第三方代码，必须在本文件补充来源、许可证和对应本地路径。

## 维护规则

1. 新增借鉴代码时，在本文件追加一行；
2. 源文件头部或相邻注释写明来源仓库和原始文件名；
3. 不把 GPL 代码复制进 BSD-2-Clause-Patent 文件；
4. 二进制文件变更需记录来源 commit 或 tar 校验值。
