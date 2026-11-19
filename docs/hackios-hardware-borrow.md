# 借鉴 HackiOS 硬件驱动的边界与顺序

主线仍是 **UniXNU 设备仓库 + prelinked XNU + SHNGROOT userdata + NativeUfs**。
HackiOS（`hoshikv/xnu-kernel-peridot`、`xnu-userland-peridot`）只作为 **peridot 硬件编程参考**，
不替换 UniXNU 的 APFS/cryptex/userland/启动链。

## 不借的部分

- `bsd/miscfs/apfs`、`ramfs`、`bindfs`（会替换我们已工作的 Apple APFS 根挂载路径）
- `hakios.ufs.c` 的 "UEFI UFSDxe + XNULoader 共享页" 存储模型（需要 UEFI 常驻，和 Mu-Silicium/DarwinBoot 不兼容）
- XNULoader / `xl` RAM 会话、`darwin` 分区安装、HackiOS 用户态、iOS 16.5 profile
- `miniamfi` 整套替换（保留我们现有 amfi bootargs/trust cache 路径，按需局部参考）

## 可借的硬件驱动（按优先级）

| 优先级 | HackiOS 文件 | 借用内容 | 接到 UniXNU 的位置 |
| --- | --- | --- | --- |
| P0 | `bsd/dev/hakios.cpufreq.c` | EPSS 频率表/顶频 row、DDR RPMh vote 的寄存器序列；先只做顶频提升，验证拷贝速度 | 新增 `drivers/PeridotPower`（或先放 `NativeUfs.cpp` 的 probe 辅助函数） |
| P0 | `bsd/dev/hakios.thermal.c` + `sys/hakios.thermal.h` | TSENS TM 状态字读取、peridot zone 表、过温降频/保护 | 同上；先只读监控，sysctl/日志输出 |
| P1 | `bsd/dev/hakios.buttons.c` | PMIC arbiter v7 读通道、PON/GPIO 按键位、SPMI 观察通道 | 独立 `drivers/PeridotButtons`；后续接 IOHID |
| P1 | `bsd/dev/hakios.goodix.c` | Goodix GT9916 BerlinD：SPI GENI、RPMh LDO、TLMM 复位/IRQ、report 解析 | 新 IOKit HID 驱动（不是 `/dev/hakios-touch`） |
| P2 | `bsd/dev/hakios.display.c` | INTF 扫描线/frame 计数、dispcc/GCC 时钟 | 当前 fb console 不动；接 Mini/Apple 显示驱动时再用 |
| P2 | `bsd/dev/hakios.adreno.c` + `iokit/Kernel/hakios.gpu.cpp` | A735/GMU/SMMU 初始化、GPU 固件流程 | 我们的 `drivers/Adreno735` 骨架；工作量很大 |
| P3 | `hakios.battery.c`、`hakios.audio.c`、`hakios.wifi.c`、`hakios.mpss.c` | 电池/音频 DSP/WCN6750/Modem | 后续外设阶段 |
| 参考 | `hakios.ufshc.c` | UFS 32-slot 命令表、read whitelist、doorbell/quiesce 细节 | 只对照 `NativeUfs.cpp`，不替换现有存储路径 |

## 可直接采用的 peridot 硬件常量

来源：`common/devices/peridot.conf`、`peridot.platform.c`、`hakios.board.c` 的默认值。

```
GIC dist            0x17100000   redist 0x17180000  size 0x200000
UFS host            0x1d84000    PHY 0x1d80000     apps SMMU 0x15000000 (stream 0x60)
EPSS                0x17d91000   cmd_db 0x81c60000  RSC 0x17a20000
TSENS0/1/2          0xc222000    0xc223000         0xc224000
SPMI arbiter        core 0xc400000  cnfg 0xc42d000  obsrvr 0xc440000
TLMM                0xf100000    GCC 0x100000       DISPCC 0xaf00000
DSI INTF            0xae36000    UART GENI0 0xa94000
AOSS                0xc300000    IPCC 0x406000      SMEM 0x81d00000
GPU/GMU/GPU SMMU    0x3d00000    0x3d68000          0x3da0000
Framebuffer         0xe3940000   1220x2712
```

## 集成方式

- 借用的 C 代码放独立目录/组件（例如 `drivers/PeridotPower`、`drivers/PeridotButtons`），
  在 `feeds.toml` 里声明为本地 driver source，在 `board.toml` 的 `[kernel].archives` 里编译；
  通过一个新的 kernel patch（或在 `bsd_init` 的 peridot 钩子）调用初始化。
- 把 HackiOS 的 `hakios_board_addr()` / boot args 替换成常量或 UniXNU 的 board 输入，
  去掉 `hakios_tl`、loader log、`/dev/hakios-touch` 依赖；输入设备改为 IOHID。
- 先在 QEMU/现有 probe 上验证寄存器读写，再上真机；任何 PMIC/RPMh 写都必须先读回确认。

## 许可证

- HackiOS 内核是 XNU 的 **APSL-2.0** 衍生作品；用户态是 MIT。
- 若直接拷贝驱动代码，必须保留 APSL 许可与声明，放在独立目录并附 `LICENSE.APSL`；
  不能把 APSL 代码直接改标成 BSD-2-Clause-Patent。
- 若只借鉴寄存器地址、位域和流程（事实性信息），按参考注明来源即可。

## 与当前主线的关系

硬件驱动不改变当前软件主线：`probe 56 + peridot-cryptex-probe=5` 的挂载/拷贝/graft 实验仍是
解决 dyld shared cache 的最近路径；P0 的 EPSS/TSENS 先做只读/顶频提升，目的是让 3.3 GB
cryptex 拷贝更快并避免过热，不替换任何 UniXNU 组件。
