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

## 2026-10-28 下午：控制台是最后一个启动期障碍

### 结论

用入口探针逐段验证，内核启动路径到 `machine_startup` 为止全部通过：

```
✅ kernel_startup_initialize_upto
✅ kprintf("kprintf initialized")      第一句打印正常返回
✅ PE_create_console                    需 peridot-defer-console.patch
✅ PE_init_printf                      同上
✅ arm_vm_init                          建页表
✅ ml_parse_cpu_topology                需 MAX_CPUS 8 + MAX_CPU_CLUSTERS 2
✅ machine_startup                      进入 BSD 初始化（此调用不返回，探针不应触发）
```

`machine_startup` 是 `_arm_init` 的最后一句，之后内核作为主线程运行 `kernel_bootstrap` →
`bsd_init`。**启动期的初始化链已经走通**，剩下的工作属于驱动层。

### 控制台为什么必须延后

`PE_create_console()` → `PE_initialize_console()` 需要有效的 `PE_state.video`，
而 peridot 还没有显示驱动，于是在里面挂死。`peridot-defer-console.patch` 让它和
`PE_init_printf()` 直接返回，代价是内核暂时没有输出通道（探针仍可用）。

真正的显示要等 `GopFramebuffer` 组件——注意它的
`build/native-legacy-display/libNativeLegacyDisplay.late.a` 在当前 UniXNU 版本里
**只被引用、没有构建步骤**，所以 `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` 目前必然失败。

### 下一步：驱动层

内核已经到 BSD 初始化，接下来卡点不再是启动流程，而是：

| 缺少的驱动 | 影响 |
| --- | --- |
| UFS（cliffs PHY + MCQ） | 没有根设备，`bsd_init` 最终会因拿不到 root 而 panic |
| 显示（GopFramebuffer） | 没有控制台，内核与 panic 都不可见 |
| 输入 | 无法交互 |

这三个都在 `board.toml` 的组件列表和 `porting.toml` 的待办里。

### 记录一个环境事实

`xnu_env()` 只从 **device defconfig** 导出 `XNU_*` 变量给构建脚本，而内核**编译**用的是
OS profile 的 `ipados.config`。两者不一致时会出现"编译引用了符号、链接却没带归档"。
本板已在 `configs/peridot_defconfig` 显式写 `CONFIG_XNU_SOURCE_MOCKFS=y` 对齐。

## 2026-10-28 下午续：引导线程逐段验证到 IOKit

补丁 `peridot-defer-console.patch` 让 `PE_create_console()` 和 `PE_init_printf()` 直接返回，
之后用探针（`--in _kernel_bootstrap_thread`）逐段推进，全部通过：

```
✅ idle_thread_create
✅ sched_startup
✅ clock_service_create
✅ device_service_create
✅ bootprofile_init（以及其间的 phys_carveout_init / kdp_init / hv_support_init）
❌ PE_init_iokit
❌ bsd_init（未到）
```

**结论：内核的启动路径到 IOKit 初始化为止是完好的**，卡在 `PE_init_iokit()` 内部。

### 重要提醒：这个卡点可能是我们自己造成的

`PE_init_iokit()` 的第一行就是 `PE_init_printf(TRUE);`——正是被 deferral 补丁改成
直接返回的那个函数。之后它立刻 `printf("iBoot version: ...")`。如果 `PE_putc`
因为跳过了初始化而未被设置，这里的 printf 就可能踩空。

所以下一步**不要直接去查 IOKit 的设备树**，先验证这一点：

1. 把 `PE_init_printf` 的提前返回改成"只跳过视频部分、仍然设置 `PE_putc`"，
   或者在 deferral 里给 `PE_putc` 一个哑函数
2. 再测 `PE_init_iokit` 之后是否通过

只有排除掉这个次生因素，才能确定 IOKit 本身是否有问题。

### 待办（按优先级）

| 事项 | 目的 |
| --- | --- |
| 修 deferral 里的 `PE_putc` | 排除 printf 踩空，避免误判 |
| 探针切进 `PE_init_iokit` 内部（`pe_prepare_images`、`DTInit`…） | 定位真正的卡点 |
| ADT 补设备节点 | IOKit 需要从 ADT 建出设备树 |
| UFS 驱动 | 没有根设备，`bsd_init` 之后必然停住 |
| 显示驱动 | 没有控制台，一切只能靠探针 |

### 2026-10-28 傍晚：排除"自伤"可能，IOKit 是真的卡点

补丁改进版（`peridot-defer-console.patch` 第二版）：

- `PE_create_console()` 仍然直接返回（视频控制台创建会死锁）
- `PE_init_printf()` 只跳过 `vcattach()`，**保留 `PE_putc = console_write_char`**
  ——这样后续 `printf` 不会踩空

用改进版重建后，探针在 `_PE_init_iokit` 之后**仍然不触发**。

结论：**不是我们的控制台补丁造成的**，`PE_init_iokit()` 内部确实卡住。
下一步要在它内部继续二分（`pe_prepare_images`、`DTInit`、registry 构建…）。

### 顺带解决：mockfs 链接问题

`CONFIG_XNU_SOURCE_MOCKFS` 在 profile 里是 `y`，导致 `vfs_conf.o` 引用三个 mockfs 符号；
而链接行没有带上 fixture 归档。在 `peridot_defconfig` **末尾**追加
`# CONFIG_XNU_SOURCE_MOCKFS is not set`（解析器后出现的行优先）之后，编译不再引用
这些符号，链接通过，`kernel.macho` sha256 `ab195a7f…`。

本板没有存储驱动，离线根镜像本来也用不上。

### 注意：构建日志会累积

`logs/kernel-bootstrap.log` 是追加写的，多轮构建后里面的 `Undefined symbols`
可能是**上一轮**的残留。判断要看最后一轮的结尾，或直接看 `unixnu` 自己的
`== kernel: … sha256 …` 输出。
