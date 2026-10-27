# 引导卡死定位（2026-10-27）

真机上 XNU 在 `arm_init` 早期卡死，屏幕停在 UniXNU 菜单最后一帧、按键无响应。
本文记录定位方法与每一步的结论，便于继续。

## 现象

```
fastboot boot Mu-peridot-xnu.img   → OKAY
之后：不在 fastboot、不起 adb、按键无响应、永不自动重启
```

## 工具一：入口色带

UniXNU 的 entry probe 会在帧缓冲顶部画三条色带（见上游
`docs/guide/debugging.md#color-bars`）：

| 色带 | 含义 |
| --- | --- |
| 绿 | DarwinBoot `DarwinEnter`：已退出 boot services |
| 蓝 | 内核入口已在 MMU 关闭状态下执行 |
| 紫 | 内核到达开 MMU 之前的最后一点 |

**实测：三色齐全** → 固件交接、内核入口、MMU 前的代码都正常执行了。

## 工具二：定时重启探针

`entry_probe_reset_delay` 让内核在指定位置之后等 N 秒，然后用 PSCI
SYSTEM_RESET 重启。**会自动重启** = 那个位置执行过了；**不重启** = 没到或卡在里面。

`entry_probe_reset_after` 指定函数名，探针插在该函数调用返回之后；
再加 `entry_probe_reset_before = true` 则插在调用点之前。

### 二分结果（全部实测）

| 探针位置 | 自动重启 | 结论 |
| --- | --- | --- |
| MMU 打开之后（默认） | ✅ | MMU 启用成功 |
| `_arm_vm_init` 之后 | ❌ | 没到 `arm_vm_init` 返回 |
| `_arm_vm_init` 调用点之前 | ❌ | 连这个调用都没到 |
| `_cpu_timebase_init` 之后 | ❌ | |
| `_cpu_timebase_init` 调用点之前 | ❌ | 连这个调用都没到 |
| `_PE_init_platform`（第 1 次，`arm_init.c:371`）之后 | ✅ | 平台专家初始化通过 |
| `_ml_parse_cpu_topology` 之后 | ❌ | 没到它返回 |
| `_wfe_timeout_init` 之后 | ✅ | WFE 超时初始化通过 |

### 当前结论

卡点在 `arm_init.c` 的 **`wfe_timeout_init()` 返回之后** 到
**`ml_parse_cpu_topology()` 返回之前**。这一段只剩：

```c
configure_misc_apple_boot_args();   // 空函数
configure_misc_apple_regs(true);    // 空函数
SecureDTLookupEntry(NULL, "/chosen", &chosen);
SecureDTGetProperty(chosen, "xnu_platform_stall", ...);
platform_stall_panic_or_spin(PLATFORM_STALL_XNU_LOCATION_ARM_INIT);
// 页大小选择（几行赋值）
ml_parse_cpu_topology();
```

`ml_parse_cpu_topology()` 用一个 `while (kSuccess == SecureDTIterateEntries(...))`
遍历 `/cpus` 的子节点；**ADT 结构异常时该循环不会结束**，与"静默卡死"一致。
已确认 payload 里的 `kernel.macho.adt`（14,700 字节）的 `/cpus` 有 8 个节点、
结构完好，所以下一步要么查迭代器在该 ADT 上的实际行为，要么把探针插到
`ml_parse_cpu_topology` 内部的调用（如 `ml_is_boot_cpu`）。

## 已排除的因素

| 假设 | 实验 | 结果 |
| --- | --- | --- |
| 内存描述太大 | `dram_size` 12 GiB → 2 GiB | 表现相同 |
| 串口阻塞 | 去掉 `serial=3` | 表现相同 |
| 页表建立在 `arm_vm_init` | 探针二分 | 根本没执行到 |
| MPIDR 写错 | 对比原厂 DTS 的 `cpu@0/100/…` | 一致 |
| Apple 寄存器配置 | 读源码 | 非 Apple 构建下是空函数 |
| WFE 超时初始化 | 探针 | 已通过 |

## 复现命令

```bash
# 1. 改 board.toml 的 [payload] 段
entry_probe_reset_delay = 6
entry_probe_reset_after = "_函数名"
# entry_probe_reset_before = true   # 可选：插在调用点之前

# 2. 重建 payload
cd /root/work/UniXNU
./unixnu payload -D ../UniXNU-peridot \
  --unique-chip-id 0x1A2B3C4D5E6F7788 --uefi /mnt/c/ms

# 3. 确认探针装上了
tail -1 out/peridot/*/*/logs/payload-patch.log

# 4. 打包并启动（Windows 侧）
python peridot/tools/dtbinitrd.py C:\ms\Resources\DTBs\peridot.dtb out.dtb 0x11000000 0x1D000000
# kernel = gzip(BootShim.bin + SILICIUM_UEFI.fd) + 上述 DTB
python Resources\Scripts\mkbootimg.py --kernel <kernel> --ramdisk payload.img \
  --header_version 0 --pagesize 4096 -o probe.img
fastboot boot probe.img
```

判据：启动后 45 秒内 adb 是否重新出现（出现 = 自动重启过）。

## 2026-10-28 续：找到并修复了两个上限

`ml_parse_cpu_topology()` 里两个数组上限卡住了 8 核的 peridot：
machine config `BCM2837` 声明 `MAX_CPUS 4` 和 `MAX_CPU_CLUSTERS 1`。

| 现象 | 实际原因 |
| --- | --- |
| 1/2/3 个 CPU 通过，4 个失败 | 不是核数：cpu3 是第一个 **P** 核，`phys_cluster_id = 1` 越界 |
| 8 个 CPU 全部失败 | `topology_info.cpus[]` 只有 4 项 |

修复（都在设备仓 `patches/xnu/`）：

1. `native-smp-secondary.patch`（UniXNU 自带，原本没进任何 series）：
   `MAX_CPUS 4 -> 8`，并让 `start.s` 在 BCM2837 上按 Aff0+Aff1 取核号。
2. `peridot-cpu-clusters.patch`（本板新增）：
   `MAX_CPU_CLUSTERS 1 -> 2`，容纳 E 簇和 P 簇。

配套：`dts/sm8635.dtsi` 的 CPU `reg` 改为单集群 0x0–0x7
（`MAX_CPU_CLUSTERS` 很小时 MPIDR 的 Aff1 不能再当簇号用）。

### 验证后的进度（真机、探针二分）

```
✅ ml_parse_cpu_topology     （MAX_CPUS 8 + MAX_CPU_CLUSTERS 2 之后通过）
✅ arm_vm_init               （建初始页表；修复前完全到不了）
✅ kernel_startup_initialize_upto（kprintf 机制初始化）
❓ PE_init_printf            （下一步：通过后屏幕应能出字）
```

### 下一步

探针放在 `_PE_init_printf` 之后没有触发，说明卡在
`kernel_startup_initialize_upto` 与 `PE_init_printf` 之间那一小段（含
`PE_create_console` 和串口探测）。这一段的输入是 `serial=` 启动参数和
`PE_consistent_debug_inherit()`，是明天最先要看的地方。

### 一个教训

prelink 会跳过"可执行字节未变"的候选，而 `MAX_CPUS`/`MAX_CPU_CLUSTERS` 只改
BSS 布局，所以改完这两个上限后 `kernel.macho` 不会自动更新——必须删掉它再跑
`./unixnu kernel`，否则上机的还是旧内核。
