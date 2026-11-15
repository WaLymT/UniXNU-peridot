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

### 2026-10-28 傍晚：卡点进入 IOKit 内部

探针移进 `PE_init_iokit()`：

| 探针 | 结果 |
| --- | --- |
| `_InitIOKit` 之后 | ❌ 不触发 |
| `_pe_prepare_images` 之后 | 无法测试：该函数被内联，没有调用点 |

`InitIOKit()` 的实现（`iokit/Kernel/IOStartIOKit.cpp:134`）：

```c
IOLibInit();
OSlibkernInit();
IOMachPortInitialize();
gRootNub = new IOPlatformExpertDevice;
bool ok = gRootNub->init(dtTop);        // dtTop = PE_state.deviceTreeHead
```

也就是说，**IOKit 的根设备就是用我们那份 ADT 建出来的**。ADT 里描述了什么设备，
IOKit 就知道有什么；反过来，任何 IOKit 期待的节点/属性缺失，都可能在这里出问题。

我们的 ADT（`kernel.macho.adt`，14.7 KB）目前只导出三个硬件节点：
`gic`、`ufs`、`ufsphy`（靠 `unixnu,adt-name`）。其余是 `chosen`/`cpus`/`defaults`/`options`
模板内容。

### 下一步（明确）

1. 用探针确认"是否到达 `InitIOKit`"（`reset_before`），区分是它内部卡住还是更早
2. 若卡在 `gRootNub->init(dtTop)`，就要给 ADT 补齐 IOKit 需要的节点，
   而不是继续在别处找问题
3. 显示与存储驱动要尽早决定：它们是后面所有验证的前提

### 2026-10-28 晚：探针到达 InitIOKit，然后停在 C++ 边界

`reset_before` 版的探针**触发了**：

```
✅ 内核确实执行到 PE_init_iokit() 里的 InitIOKit(PE_state.deviceTreeHead) 调用点
❌ InitIOKit 不返回
```

继续往里切时遇到工具限制：`IOLibInit`、`OSlibkernInit`、`IOMachPortInitialize`
都是 **C++ 函数**，符号被 mangle，而入口探针只能按 C 符号名定位
（`_InitIOKit`、`_PE_init_iokit` 这类 extern "C" 的名字可用）。
内核产物没有符号表（`llvm-nm` 返回 0 条），也没法现场解析出 mangle 后的名字。

### 这暴露了真正的瓶颈：没有输出通道

到目前为止所有定位都靠**入口探针**——它只能在 C 调用边界上"打点"，而且一次只能测
一个点，还要重启设备。要继续往 `gRootNub->init(dtTop)` 里走，需要：

- 串口输出（硬件上没有引出），或
- 屏幕控制台（依赖显示驱动），或
- 在内核里加自己的诊断钩子，用 C 函数暴露给探针

**结论：显示驱动 / 串口从"功能"变成了"诊断工具"，已经排在关键路径上。**

### 建议的下一步（优先级）

1. 在 `InitIOKit` 里插入 C 语言的外部钩子（`extern "C"` 空函数），让探针能在
   C++ 边界内部打点——不改语义，只加可定位点
2. 或者优先打通显示：`GopFramebuffer` 的归档构建缺失需要补，
   一旦有屏幕控制台，后面所有问题都能自己打印出来
3. ADT 补节点：`gRootNub->init(dtTop)` 消费的就是它

## 2026-10-28 晚：自己写帧缓冲控制台，内核终于能说话了

### 做了什么

UniXNU 的 `GopFramebuffer` 组件在这个版本不可用（见下），所以自己写了一个最小控制台
（`peridot-fb-console.patch`）：

- 在 `pexpert/gen/pe_gen.c` 里加一个 `peridot_fb_putc()`：用 Mu-Silicium 的
  96 个 5x12 字模，画进 `PE_state.video` 描述的 GOP 帧缓冲（`ml_io_map` 映射）
- `PE_init_printf()` 里 `PE_putc` 指向它
- 同时钩住内核真正用于打印的两个出口：`_cnputc()`（串口路径）和 `vcputc()`（视频路径），
  后者直接 `return`——因为它的锁组 `vcputc_lock` 只在 `vcinit()` 里初始化，而我们跳过了
  `PE_create_console()`

**结果：内核的 panic 和日志能直接打在屏幕上了。**

### 拿到的一手证据

第一次跑，屏幕上完整打出：

```
panic(cpu 0 caller 0xfffffff0079da2e0): Kernel data abort.
  at pc 0xfffffff0078e5a54, lr 0xfffffff0078e3d38
  far: 0x0000000000000000   esr: 0x96000005
```

`far = 0` → 空指针解引用。符号化结果：

```
pc -> IODTGetICellCounts()      (被 IODTFindInterruptParent 返回 NULL 后直接解引用)
lr -> IODTMapInterruptsSharing()
```

### 修复与下一个问题

加上 `peridot-iodt-guard.patch`（找不到中断父节点时打印并跳过，不再 panic）之后：

```
ufshc: error mapping interrupt[0]          ← 守卫打出来的，说明 ufshc 的中断父节点没解析到
IONVRAM: NVRAM size is 8192 bytes
lockdown_mode: lockdown mode in nvram is off
panic(cpu 0 ...): Invalid ticket lock 0xfffffff007af0e08 @lock_ticket.c:134
```

锁地址符号化 = **`_vcputc_lock`**：`vcputc()` 用了未初始化的锁组。
这正是上面那个 "直接 return" 要解决的问题——第二版补丁已经这么改了，
但**卡在网络上没能重新编译验证**。

### 两个待办（下次直接做）

1. **重新编译**（`peridot-fb-console.patch` 第二版已生成、格式已校验）

2. **`ufshc` 的中断父节点为什么解析不到**
   ADT 里其实有 `intc`（`#interrupt-cells=4`、`phandle=1`）和
   `ufshc`（`interrupt-parent=<1>`）。`FindPHandle(1)` 却没找到 → 怀疑 ADT 生成器
   写出的 phandle 与 `interrupt-parent` 的宽度/类型不一致，或者注册表里没有把
   `phandle` 属性登记进 `gIODTPHandles` 表。有控制台之后，下一步可以在
   `FindPHandle` 里把整张表打印出来对比。

### 环境障碍：GitHub 直连不稳

`prepare-xnu-source.py` 每次构建都要从 `raw.githubusercontent.com` 取 DriverKit 头文件
（xnu 里省略的那些）。已做的缓解：

- `tools/dl/xnu-10063.121.3.tar.gz`（codeload 下载，稳定）
- `/root/xnu-driverkit-seed` 存了 21 个头文件 + 2 个 API 清单
- `peridot/tools/wsl-make-seed.sh` 建种子并给抓取函数打上"优先读本地"的补丁
  （**注意**：构建树会重新生成脚本，这个补丁是否被树内的副本采纳还没验证成功）

## 2026-10-29 上午：越过 IOKit，进入 kext 加载

### 先修的是构建，不是内核

上一轮"构建树坏了"的判断是错的。真相：**`peridot-fb-console.patch` 里把锁宏改坏了**。

`VCPUTC_LOCK_LOCK()` 这类宏靠行尾反斜杠续行，我替换掉那行时丢了 `\`，
`MACRO_END`（展开成 `} while (0)`）就落到宏外面：

```
video_console.c:164:1: error: extraneous closing brace ('}')
video_console.c:164:1: error: while loop outside of a function
```

`video_console.o` 编不出来 → 没有链接 → 没有 `link.filelist` → 归档步骤报
`No XNU symbol names`。而 `kernel.py:178-182` 的 `except Error: if not record.is_file(): raise`
会把这个失败吞掉（编译已经写了 `startup.o.json`），所以看起来像"树无法恢复"。

教训：**`startup.o.json` 存在不代表编译成功**，只代表编译开始过。

### 另外修掉的两个环境问题

- `prelink` 里写的是 `ipsw.kernelcache(ctx, ipsw.Ipsw(ctx))`——参数先求值，
  所以即使 `out/ipsw/<build>/kernelcache.decompressed` 已在本地，`Ipsw()` 也会联网。
  绕法：`UNIXNU_KERNELCACHE=<绝对路径> ./unixnu kernel`
- `prepare-xnu-source.py` 需要三份清单（DriverKit、DriverKit/crypto、**TrustCache**），
  种子目录按 URL 的 basename 匹配，所以第三份必须存成名为 `TrustCache` 的文件

### 真机结果

| 探针/版本 | 屏幕 |
| --- | --- |
| 锁宏修好（`4154a454`） | `Invalid ticket lock` 消失；新 panic：`video_console.c:1222 Assertion failed: gc_ops.update_color` |
| 加 `peridot-vc-guard.patch`（`c636bc6f`） | **不再 panic**，一路走到 kext 加载 |

最后几行（真机拍照）：

```
Darwin Image4 Validator Version 66.33.0
Security policy loaded: AppleImage4 hooks
dylib_limit: All the created dylib kext threads have been scheduled at least once
apfs_module_start:332232: load: com.apple.filesystems.apfs, apfs-222036.114400.66
```

**停住的地方：APFS 模块启动**。没有存储驱动，就没有可挂载的根设备，这一步等不到结果。

### 当前补丁序列（7 个）

```
native-smp-secondary.patch      MAX_CPUS 8
peridot-cpu-clusters.patch      MAX_CPU_CLUSTERS 2
peridot-no-pi3-uart.patch       去掉 PI3_UART
peridot-defer-console.patch     推迟 PE_create_console / vcattach
peridot-fb-console.patch        自写帧缓冲控制台 + 锁宏置空
peridot-iodt-guard.patch        中断父节点找不到时打印并跳过
peridot-vc-guard.patch          gc_update_color 未初始化时打印并返回
```

（`debug-*.patch` 三个早期打印脚手架已删除：它们对着旧源码生成、打不上了，
控制台接管了这个职责。）

### 下一步

1. **UFS 驱动**（cliffs PHY + MCQ）——APFS 卡在这里就是因为没有块设备
2. `ufshc` 的中断父节点为什么 `FindPHandle(1)` 找不到（ADT 里 phandle/宽度对不上？）
3. 显示驱动（`GopFramebuffer` 两条路都编不过，详见 `docs/build-windows.md`）

## 2026-10-29 中午：UFS 移植，摸清到链接期

目标很明确：内核停在 `apfs_module_start`，因为**没有块设备**。UFS 是下一个关隘。

### 已有的东西

UniXNU 里唯一的 Qualcomm UFS 驱动是 `ShengUfs`（catalog 标注 SM8550 / sheng 专用）。
从 stock DTS 取的事实表明 **cliffs 和 SM8550 在控制器层面一致**：

| 事实 | peridot (SM8635 / cliffs) | 驱动假设 |
| --- | --- | --- |
| UFS 主机 | `ufshc@1d84000`，`reg` 首段 `0x3000` | `UFS_HC_BASE 0x1d84000`，size `0x3000` ✓ |
| SMMU | `iommus = <&apps_smmu 0x60 0>` | stream `0x60` ✓ |
| GCC | `clock-controller@100000` (`qcom,cliffs-gcc`) | `GCC_BASE 0x100000` ✓ |
| compatible | `"qcom,ufshc"` | 只认 `"qcom,sm8550-ufshc"` ✗ |

驱动**不配置时钟和 PHY**——它检查 GCC 的电源状态，然后假设引导器已经把链路拉起来，
只做映射、SMMU 旁路、自建请求队列、NOP OUT、SCSI。这让移植看起来可行。

### 做了什么

1. **vendor 驱动**到 `drivers/ShengUfs`，在 `feeds.toml` 里用 `local` 源声明
   （同时解决 github 不可达和"要改代码"两件事）
2. `probe()` 增加接受 `"qcom,ufshc"`，注释里写清了三条必须成立的事实
3. `[components] include` 加 `ShengUfs`（自动带入 `VirtioMmioBlock`、`VerifiedRoot`）
4. `[kernel] archives` 显式列出存储组件——**arch profile 的列表是显式的，
   一旦有 `component:` 条目就不会自动补齐，新组件必须自己写进去**
5. 发现并修掉一个静默坑：`peridot_defconfig` 里本来就有
   `# CONFIG_XNU_NATIVE_UFS is not set`，我"追加" `=y` 时被判断条件挡掉了，
   文件里从来没出现那行；而且构建不会自动重跑 `defconfig`，`.config` 一直是旧的

### 卡在哪：链接期

驱动编译通过、`libShengUfs.a` 已生成、`-force_load` 也进了链接行，但缺符号：

```
IOBlockStorageDevice（整个类：vtable、构造、虚函数）
verified_root_key / verified_root_policies
boot_progress_poll
```

`IOBlockStorageDevice` 属于**外部 kext** `com.apple.iokit.IOStorageFamily`。
VirtioBlock 和 RamBlock 是靠 `build/*-bindings.py` 把这类引用**绑定到原始 kext 的地址**
（先 `audit-storage-interface.py` 审计 `refer/IOStorageFamily` 得出 ABI，
再生成 bindings 归档）。**UFS 没有对应的绑定步骤**——那部分在 sheng 设备仓里。

所以这不是开一个开关，而是要移植整条存储绑定流水线。

### 下次从这里继续

1. 写 `build-ufs-bindings.py`：照 `build-virtio-mmio-bindings.py` 的模式，
   审计 IOStorageFamily 的 ABI，生成 NativeUfs 需要的 `IOBlockStorageDevice` 绑定
2. 链接 `VerifiedRoot` kit 的归档（`verified_root_key/policies`）
3. `boot_progress_poll`：驱动里是 weak 声明，但模板实例化把它变成强引用；
   要么关掉 BOOT_PROGRESS，要么提供一个空实现
4. 三者齐了之后，先跑 `native-ufs-probe=2`（只读检查），再跑 `=10`（只读 GPT 校验）

**当前 `CONFIG_XNU_NATIVE_UFS` 已改回未设置**，仓库保持可构建；
驱动的 vendor 副本和 `probe()` 的改动都保留着，随时可以接着做。

### 存储绑定：为什么这一步不能"顺手写个脚本"

我试过直接照 `build-virtio-mmio-bindings.py` 写一份 UFS 版本，结论是**前置产物不存在**，
而且不是补几个文件就能造出来的。查证过程：

1. 绑定脚本要读 `build/storage-abi/interface-audit.json`，里面有原始
   `IOBlockStorageDevice` 每个方法的 `candidate_target` 地址。**这个文件我们没有**
2. 生成它的 `audit-storage-interface.py` 依赖 macOS 的 `xcrun dwarfdump`，
   而且它的地址来自一份"合格的 sheng prelink 产物"
   （`build/xnu-prelink/native-apfs-load-positive.macho`），那是 sheng 设备仓的构建产物
3. 想绕开它、直接从原始 kernelcache 取地址也不行：
   - `out/ipsw/21H461/kernelcache.decompressed` 里 8 个带符号表的 Mach-O，
     **没有一个含 `IOBlockStorageDevice`**
   - 我们自己 prelink 出来的 `out/kernel.macho`（50,639 个符号）同样没有
     —— 因为 IOStorageFamily 是**运行时加载的 kext**，它的符号不在内核里

也就是说：**地址只在"kext 被真正加载"时才确定**，而 UniXNU 的
`native_prelink.plan()` 掌握这套映射。sheng 把它固化成了 audit JSON，peridot 还没有。

### 两条可行路径（下次选一条）

**A. 产出 peridot 自己的 audit**（对齐 UniXNU 的设计）
- 让 `XnuPrelink` 的 plan 在 prelink 时导出 IOStorageFamily 的绑定表
- 或者用 macOS 跑一次 `audit-storage-interface.py`，产出 `interface-audit.json`
- 然后照 virtio 的模板写 `build-ufs-bindings.py`

**B. 不用原始 kext，自己提供存储类**
- 在树里放一个最小 `IOBlockStorageDevice` 兼容实现，让驱动链接到它
- 语义不同（这个类变成我们的），bring-up 阶段可以接受，但要写清楚这不是原版

两条都不小。**在写任何数据之前，必须先能只读地把 LUN 0 的 GPT 读对**——
后面的 `userdata` 窗口（`board.toml` 里仍是 0/0）依赖这个结果。

## 2026-10-29 下午：UFS 驱动第一次跑在设备上

### 做了什么（路线 A：自己产出绑定表）

sheng 的 `interface-audit.json` 里那些地址，peridot 拿不到：原始 kernelcache 剥离了 kext
符号，我们自己的 prelinked kernel 里有 22,544 个导出却没有一个 IOStorageFamily 类符号。

但 **`__ZTV9IOService` 是可用的**（`0xfffffff0071b9a80`）。派生类的 vtable 按 Itanium ABI
就是父类同位置的那张表，另加自己的覆写——上游 audit 报告的形状正好是
**190 槽 = 162 继承 + 14 覆写 + 14 纯虚陷阱**。

于是 `scripts/build-ufs-bindings.py` 用结构特征找表：扫描 IOStorageFamily 的
`__DATA_CONST` 区段，找 16 字节零前缀 + 190 指针满足上述分布，**只接受唯一解**：

```
dispatch table 0xfffffff005cb44d0: 162 parent slots, 14 overrides, 14 traps
```

四个锚点都做了模式校验，不是"找到就用"：

| 锚点 | 值 | 校验 |
| --- | --- | --- |
| vtable | `0xfffffff005cb44d0` | 190 槽分布 |
| 构造 | `0xfffffff00496ed98` | 存表 + 转发到 `IOService::IOService` |
| 析构 | `0xfffffff00496edb8` | 转发到真正的 `IOService` D2 |
| metaclass | `0xfffffff006add1d0` | getter 的 adrp/add 形态 |

11 个导入全部绑定，折进 `libShengUfs.a`。`VerifiedRoot` 与 `boot_progress_poll` 用
**失败即拒绝**的桩（`vr_open`/`vr_verify_block` 返回 -1、策略计数为 0），
这只是为了链接，验证路径一律不通过——安全方向。

### 真机结果

`native-ufs-probe=2`（只读检查）在设备上跑通并打印：

```
axi=0000000000000011  ahb=0000000000000011  uniphy=0000000000000011
txs=0000000000000011  rxs=0000000000000011  aux=0000000000000011
```

**六条 GCC UFS PHY 时钟分支全部是 0x11（使能位置位）**——`fastboot boot` 留下的
UFS 链路是活的。这对整个存储方向是决定性的好消息：不需要自己配时钟和 PHY。

### GCC 偏移：核对过了，cliffs 和 SM8550 一致

我一度怀疑那段打印里的 `gdsc=...` 值不像电源域寄存器，**那是我看错了照片**
（把两行挤在一起的值当成了 GDSCR）。stock DTS 直接给出了答案：

```
gcc_ufs_mem_phy_gdsc = "/soc/qcom,gdsc@19e000"      → GCC base 0x100000 + 0x9e000
gcc_ufs_phy_gdsc     = "/soc/qcom,gdsc@177004"      → GCC base 0x100000 + 0x77000 + 0x4
```

与 `GccUfs.h` 里的 `0x77000`/`0x9e000` 完全一致。而且驱动的打印格式是 `%08x`，
真正的判据是它自己算出来的 `hc_read`：

```
axi=00000011  ahb=00000011  unipro=00000011
txsym=00000011 rxsym=00000011 aux=00000011
hc_read=1 cap=... ver=...
```

**`hc_read=1` 说明控制器被成功读取**——驱动只有在"GDSC 已上电且总线时钟在跑"时才去读控制器。
所以 cliffs 的电源域与时钟状态是满足传输条件的。

教训：看照片里的十六进制值之前先确认打印格式（`%08x` 还是 `%p`），
否则会把"两行挤在一起"读成"一个奇怪的值"。

### 下一步

1. 从 stock DTS 的 `power-domain`/`gdsc` 引用或 cliffs 的 GCC 布局重新推导 UFS PHY 的
   GDSCR 偏移，替换 `GccUfs.h` 里 SM8550 的值
2. 偏移纠正后跑 `native-ufs-probe=10`（只读：LUN 0 → GPT → userdata 窗口）
3. 只有 GPT 读对之后，才去填 `board.toml` 的 `userdata.first_lba/last_lba`

### probe=10：进到 SMMU 就停了，而且停得有道理

`native-ufs-probe=10`（只读 GPT 校验）在真机上的结果：

```
panic(cpu 0 caller 0x...): NATIVE_UFS
  smmu: conflicting UFS stream mapping slot=1 s2cr=0x00...
```

`planUfsSmmu`（`SmmuTakeover.h`）的规则是：扫 SMR 表，找到与 stream 0x60 匹配的项后，
只有"唯一匹配、mask 为 0、且已经指向它保留的那个 context bank"才允许复用；
否则一律报冲突并拒绝。peridot 上 **引导器已经给 UFS 装了映射**，指向它自己的 bank，
于是 `ourBank` 为假 → 冲突。

这不是 bug，是驱动在保护一条可能正在被引导器使用的映射。

### 所以先看清，不要改写

下一步是 `native-ufs-probe=7`（`inspectSmmu`，纯只读）：它把 SMMU 的
`scr0/idr0/idr1/gfsr`、有效 SMR 数量、以及 **stream 0x60 那一项的 smr/s2cr 原样打印**。

知道那张映射的类型（S2CR.TYPE：bypass / translate / fault）和 CBNDX 之后，才有依据选：

- 若已经是 **bypass 或恒等映射**：说明引导器的映射正好能承载我们那块物理连续的 DMA，
  驱动应当"采纳"而不是"接管"（跳过改写，直接用）
- 若指向它自己的页表：必须显式重写，那是真正危险的操作，要先有它不在使用的证据

### 接管（方案 A）：实现完成了，但未验证成功

按"显式接管"加了 `native-ufs-takeover=1`：

- `planUfsSmmu` 在冲突分支里，若开关打开且匹配唯一、无 mask，就复用引导器那条 SMR 的槽位
- `configureUfsSmmu` 随后把这条 S2CR 重写指向我们的 bypass bank
- 默认 `nativeUfsTakeoverAllowed = false`，不加参数行为不变

**真机结果不一致**，而且有个必须记下的观察：

| 启动参数 | 屏幕 |
| --- | --- |
| `native-ufs-probe=2` | 有 `NATIVE_UFS inspect` + panic（驱动正常） |
| `native-ufs-probe=10`（接管前） | `conflicting UFS stream mapping` |
| `native-ufs-probe=10 native-ufs-takeover=1` | **只有普通日志，没有 `NATIVE_UFS`** |

A/B 证明补丁没有破坏驱动（`probe=2` 仍然工作），所以问题落在**接管路径本身**：
要么写 S2CR 之后总线挂起、要么在打印前就停了。

下一步的排查方向（按成本排序）：

1. 在 `smmuBypassStream` 里、写 S2CR **之前**先 `panic` 打印一次 plan 结果——确认接管分支真的进了
2. 写 S2CR 之后立刻 panic 打印读回值——把"挂住"和"写失败"分开
3. 如果确认写 S2CR 会挂：说明引导器的翻译表仍被引用，接管不成立，应改走方案 B（复用它 CBNDX 17 的页表）

**注**：这版补丁目前是"实现了但未验证"，默认关闭，不影响已验证的 `probe=2` 路径。

### 方案 A 判定：这是**否定结论**（2026-10-29 深夜）

最后一枪去掉了所有中间探针，接管无条件生效，结果：

| 版本 | 屏幕 |
| --- | --- |
| tag=9（探针停在写 S2CR **之前**） | 打出 `NATIVE_UFS tag=9` ✅ |
| tag=11（无探针，直接写并继续） | **完全静默**，既无 root check 也无 bring-up failed |

两次对照把结论钉死了：**代码走到了 SMMU 这一步，但一旦重写引导器的 S2CR，后续任何访问都不返回。**

也就是说 **方案 A（接管流表项）在 peridot 上不成立**——不是逻辑错误，而是硬件不接受这次改动：
那条映射大概仍被引导器的翻译表引用，改写会让 UFS 走一条没有有效页表的路径，随后总线挂起。

### 现在的状态

- 驱动里 `nativeUfsTakeoverAllowed` 恢复为 `false`，接管回到开关后面
- 保留了 `NATIVE_UFS_BUILD_TAG` 的 start() 打印（定位镜像用，不影响流程）
- 中间探针全部移除
- **没有对存储做过任何写操作**；唯一写过的寄存器是那次 S2CR 重写，而它现在已经被撤销（默认不再执行）

### 下一步：方案 B（复用，不接管）

引导器把 UFS 映射在 **CBNDX=17** 的翻译表上。要读写就得让我们那块连续 DMA 出现在**那张表**里，而不是把流改到别处。

要做的事：

1. 读 CBNDX 17 的 `SCTLR` / `TTBR0` / `TCR`，确认它的页表格式与当前生效的映射范围
2. 找到一块可用的空闲虚拟区间，把我们的 DMA 物理页按它的页表格式映射进去
3. 这一步必须先做**只读验证**（读 LUN 0 的 GPT）；写操作要等读过之后

风险比方案 A 低：不改现有映射，只往它的页表里加条目。

### 方案 B 第一步：SMMU 几何与假设不符

`native-ufs-probe=8`（`inspectContextBanks`，纯只读）在真机上的结果：

```
NATIVE_UFS cb survey: unexpected geometry idr1=...
```

它在**开始遍历之前**就拒绝了：驱动用 `IDR1` 的字段推导"每页多少个组、多少页"，
而 peridot 的取值落在它接受的枚举之外。

这解释了为什么接管方案里"保留最后一个 context bank"的做法不适用——
连 bank 的编号都按另一套几何算。

### 下次的具体动作

1. 读 `IDR0`/`IDR1`/`IDR2` 的原始值（不要用驱动的解码，直接打印）
2. 按 ARM IHI 0062 的字段定义手工解：`NUMSMRG`、组大小、`PAGESIZE`、`NUMCB`
3. 用真实几何重算"哪个 bank 是最后一个"，再判断 CBNDX 17 落在哪一页
4. 之后才是"往它的页表里加映射"

**在此之前不要写入 SMMU 任何寄存器。** 方案 A 已经证明：按错误几何改写会挂死总线。

## 2026-10-29 深夜：控制台终于可读（4 倍字 + 清屏）

之前的帧缓冲控制台有两个要命的问题，今天都解决了：

1. **不清屏**——旧 panic 留在屏上，新内容只覆盖一部分，导致我两次误判
   （"两者都没有"其实是被旧文字误导）
2. **字太小**——5x12 字模在 scale 2 下，1220 宽的屏幕每行约 100 字符，
   照片里根本读不出地址

改法（`peridot-fb-console.patch`，生成器 `tools/wsl-gen-fbconsole3.py`）：

- 首次 `peridot_fb_putc` 时把整块帧缓冲填黑（3.3M 像素，一次性开销）
- `PERIDOT_FB_SCALE` 从 2 提到 **4**

真机确认：**字号明显变大**。这一条比修任何一个 UFS bug 都值——
今天后半段大量时间花在"看不清屏幕"上。

### 又一次"改了一半"

这一轮我犯了一个典型错误：**重建了内核，却没重建 payload**——而内核是嵌在
payload 里的。结果刷进去的还是旧内核，屏幕"完全没变化"，我差点误判成
"清屏被 GOP 覆盖"。

教训：`内核 → payload → boot image` 这条链目前靠人记。**应该写成一个脚本**
（`wsl-cycle.sh`），一条命令走完并校验哈希，否则这类错误还会再犯。

## 2026-10-29 收尾：只差 bank 17 的两个寄存器

采纳（adopt）逻辑写好了：发现已有唯一匹配时，读**匹配项指向的那个 bank** 的
`CBAR` 与 `SCTLR`，若 `SCTLR.M=0` 且 `CBAR.CBNDX` 自洽，就认为它是 bypass、
直接用、不写任何寄存器。

真机结果仍是：

```
smmu: conflicting UFS stream mapping slot=1 smr=0x80000060 s2cr=0x11
```

**说明判定没通过**——而原因很具体：普查（`probe=8`）只打印了
`sctlr[0]`、`sctlr[1]`、`sctlr[last]`，**恰好没有 bank 17 的**。
我用的判据需要它的 `SCTLR` 与 `CBAR`，拿不到就只能保守地报冲突。

### 明天第一步（很小，且只读）

把普查改成：从匹配项的 `s2cr` 取出 `CBNDX`，**只打印那个 bank 的 `CBAR` 与 `SCTLR`**。

```c
unsigned matched = s2cr & 0xff;           // 17
kprintf("NATIVE_UFS bank=%u cbar=%x sctlr=%x\n",
        matched, read32(smmu, 0x1000 + 4*matched),
        read32(smmu + (pages + matched) * 4096, 0));
```

拿到这两个值，就能确定：

- `SCTLR.M=0` → 确实是 bypass，adopt 条件放宽到只看 M 位即可通过
- `SCTLR.M=1` → 是真实翻译，方案 B 需要往它的页表里加映射（工作量更大）

### 今天的经验

控制台改成 4 倍字 + 清屏之后，**屏幕终于可以直接读数字**。
今天下午多轮误判的根源就是字号太小 + 旧文字残留。这一条建议保持。

## 2026-10-30 凌晨：AMFI 因缺 PMGR panic，UFS 探针被它盖住

最后一次启动的 panic 不是 UFS，而是：

```
panic(cpu 0 caller 0x...): "AMFI::No PMGR?\n" @ConfigurationSettings.cpp:2235
```

这是 sheng 专有的依赖（仓库规则里明确禁止照搬 `CONFIG_XNU_SHENG_PMGR`）。
peridot 没有那套 PMGR 节点，AMFI 一旦走到这条路径就拒绝继续。

### 一个值得记录的推测

早先的控制台版本能一路跑到 APFS，**没有** AMFI panic；而现在出现了。
两者之间唯一的实质变化是：**UFS 块设备 personality（`NativeUfsBlock`）开始被启动了**。

如果这个推测成立，那说明 **UFS 已经越过了 SMMU，进入了块设备发布阶段**——
AMFI 正是因为看到了新的块设备才去走根验证，然后撞上缺 PMGR。

下次要验证它，最直接的办法是在屏幕最上方找 `NATIVE_UFS root check` 那一行：
如果它出现过，说明 SMMU 这一关其实已经过了。

### 下次的两条并行线

1. **UFS**：确认 `root check` 是否打印过；若打印过，直接看 `userdata` 的窗口值
2. **AMFI**：peridot 需要一条"没有 PMGR 也能通过"的路径，否则块设备一出现就 panic

这两条现在互相影响：UFS 越成功，AMFI 越早 panic。

## 2026-10-30：SMMU 映射建立成功，传输层卡在 NOP OUT

今天最重要的一步：**驱动第一次自己写入 SMMU 映射并被硬件接受**。

### 关键实测数据

| 测量 | 值 | 含义 |
|---|---|---|
| `inspect()` / bank | `bank=1  cbar=0x1ff00000  sctlr=0x…0ee0` | bank 1 的 **M 位 = 0 → bypass** |
| `smmu live` | `scr0`/`idr0` 非零 | 在 `hostBringUp` 时刻 SMMU **可访问** |
| `smr scan` | `found=1  idx=0  s2cr=0` | 固件那条映射在交棒后**已被禁用** |
| `smmu installed` | `slot=5  bank=0  smr=0x80000060  s2cr=0x0` | **写入并读回一致** ✅ |
| `queue up` | `slot=5  bank=0` | `hostStartQueue` 成功 ✅ |
| `NOP OUT` | 失败 | 卡点 |

### 结论

1. 之前所有 conflict 的根因：**固件映射已禁用**，代码却以为它还在
2. 正确做法不是"接管/采纳"，而是**在空闲槽位新建一条映射**，指向一个 `SCTLR.M=0` 的 bypass bank
3. `src` 的 `cbar & 0x3f == bank` 判据是错误的（CBAR 低 6 位并非 CBNDX），**唯一可靠判据是 `SCTLR` 的 bit0**

### 未解决

`hostNop()` 失败，但两次观测不一致（一次有 `bring-up failed: NOP OUT`，一次无输出）。
需要在失败时把 `HCE`(0x34)、`UTRLDBR`(0x40)、`IS`(0x20)、`HCS`(0x30) 一并打印，
才能判断是"控制器未使能"、"门铃未消费"还是"命令未完成"。

**下一次不要重复盲试**：先把这四个寄存器打出来，再决定改哪里。

## 2026-10-30 傍晚：链路是通的，卡在第一条命令

### 已确证的硬件状态（全部来自寄存器读回）

| 项 | 值 | 结论 |
|---|---|---|
| SMMU 自建映射 | `slot=5 bank=0 smr=0x80000060 s2cr=0x0` | 写入并读回一致 ✅ |
| 控制器使能 | `hce=1` | ✅ |
| 请求列表 | `hostStartQueue` 成功，基址读写一致 | ✅ |
| UIC 就绪 | `hcs=0x10f`，`ucrdy=1`，`utrlrdy=1` | ✅ 链路是活的 |
| UIC 命令完成 | `uccs=0` | 驱动从未主动发过 UIC 命令 |

### 一个结构性问题

`0x60` 在 UFSHCI 里是 **UICCMD**（UIC 命令寄存器），而驱动把它当成"队列开关"：

```
RootQueue.h:34   hostWrite (0x60, 0);   // 以为在停队列
RootQueue.h:55   hostWrite (0x60, 1);   // 以为在启队列 → 实际是 DME_LINKSTARTUP
```

`ucrdy=1` 很可能正是这次"误发"的 LINKSTARTUP 带来的。这不是错误的**结果**，
但说明 UIC 层没有被显式管理。

### 仍未解决：NOP OUT

观测不一致（有时打印 `bring-up failed: NOP OUT`，有时无输出直接重启），
说明失败发生在不同位置。已知的早期返回点：

1. `hostClearUcd()` 返回 false → `kIOReturnNotReady`（**没有打印**）
2. `hostSubmit` 的 `0x58 != 0` / IS fatal → poisoned（**没有打印**）
3. `pollUfsTransfer` 超时（有打印，但路径不稳定）

### 下次的第一件事

给 `hostClearUcd` 的失败和 `hostSubmit` 的每个早退都加 `panic`（不是 kprintf，
kprintf 会被后续日志冲掉），一次只保留一个，直到确认是哪一条返回。

在此之前不要改寄存器映射——今天我已经因为误判 UFSHCI 版本改错偏移，
导致队列起不来（已回滚）。

## 观测方法论问题（2026-10-30 收尾）

今天多次出现"直接进原系统、无打印"，即使我在三个静默早退点都放了 panic。
这说明问题不在"哪一行返回失败"，而在**更早的地方就硬挂了**（不是 XNU panic，
没有 panic 输出），或者 panic 输出还没来得及显示就被复位。

### 已排除

- `kprintf` 会被后续启动日志冲掉 → 必须用 panic 定格
- 三个早退点（`hostClearUcd`、残留门铃、致命 IS）都已加 panic，**仍未触发**

### 下一步的正确做法（改变架构，不是继续打点）

把传输诊断搬到**已验证能可靠打印的早期路径**：

`probe=2` 的 `inspect()` 曾在真机上稳定打印（GCC、控制器、SMMU 都读到过）。
所以下一步不是继续在 `hostBringUp` 里找失败点，而是：

1. 在 `inspect()` 里直接调用一次最小传输测试（映射 HC → 建队列 → 发 NOP → 读 OCS）
2. 因为 `inspect()` 早于其它子系统，崩溃时也更容易留下可见输出

### 另一条可选路线

`fastboot` 阶段（UEFI/Silicium）的 framebuffer 是可靠可写的。如果内核在
`ExitBootServices` 之后崩溃，可以考虑让 DarwinBoot 预先在屏幕上保留一个
"崩溃代码区"，内核通过 `PE_state.video` 写一个错误码进去——但内核崩溃时
写屏本身可能不可用，价值有限。

**结论：优先走"早期路径 + panic"，不要在 hostBringUp 里继续猜。**

## 2026-11-02：UFS 打通，第一次读到真实存储数据

### 结论先说

**传输代码没有问题。** 把仓库里的驱动换成已发布的 `UniXNU/ShengUfs` 之后，
`RootQueue.h` 与原版**逐字节相同**，`RootCommands.h` 只差探针。也就是说这两周
在传输层做的所有修改，都建立在"代码有缺陷"这个错误前提上。

真正的差异只有一处：**peridot 固件把 UFS 流映射到 CBNDX 17**，而驱动的
`planUfsSmmu` 只接受自己算出来的 bank（倒数第一个）。于是每次都报
`conflicting UFS stream mapping`。

### 修法（20 行）

在 `SmmuTakeover.h` 的冲突分支里，当"唯一匹配、无 mask"时读一次目标 bank 的
`SCTLR`：**M 位为 0（stage 1 关闭 ⇒ 物理直通）就直接采纳，一个寄存器都不写**。

```cpp
if (matches == 1 && !mask) {
  unsigned existing = s2cr & 0xffU;
  if (existing < banks && (read ((p.pages + existing) * 4096) & 1U) == 0) {
    p.adopt = true; p.reuse = true;
    p.bank = existing; p.slot = i; p.smr = smr; p.s2cr = s2cr;
    return p;
  }
}
```

调用方在 `plan.adopt` 时直接填 report 并返回，跳过 `configureUfsSmmu`。

### 设备上的结果（probe=10）

```
NATIVE_UFS root check
userdata first=0x253000  last=0x7aab7fa
block0 magic 00000000 00000000
```

- `hostBringUp` 完成：队列启动、NOP OUT 通过
- GPT 解析成功，`userdata` 窗口读出
- 窗口首块是全零：未格式化或加密卷，符合预期

### 窗口值怎么来的

`first_lba` 由设备实测（0x253000）。`last_lba` 不能靠念屏幕上的数字——它由
`fastboot getvar partition-size:userdata` 的 0x78587FB000 字节推出：
126191611 个 4096 字节扇区，`last = first + count - 1 = 0x7AAB7FA`，
与 `board.toml` 里的交叉校验一致。

### 走过的弯路（记下来）

| 尝试 | 结果 |
|---|---|
| 认为驱动缺逻辑，逐层加探针 | 探针本身破坏了构建/运行，反而看不到输出 |
| 运行期字号变量 | 新增全局符号让平台驱动注册失败 |
| `debug=0x1` 想冻结 panic | 实际启用 KDP，设备挂起等调试器 |
| 刷 boot 分区（v0 / GKI 拆分 / 禁验证 vbmeta） | ABL 全部回退；`slot-unbootable` 需 `erase misc` 才清 |
| 手工重建 64 MB payload | 可用，但 `fastboot boot` 下才有效 |

### 下一步

1. `probe=11`：读各 window 自己的 GPT
2. 读 userdata 窗口里更靠后的块，判断是否已有可识别的文件系统
3. 只有确认布局之后才考虑写入

## 2026-11-02 深夜：根镜像写入成功，manifest 被识别

### 写入方式

Windows 上的 fastboot 都不行（32 位 `bad_alloc`；第三方 x64 构建在读整个文件时
`out_of_range`；sparse 切片因引导器拒绝"跳过块"而报 `Volume Corrupt`）。

最终可用的组合：

```
usbipd-win 把手机共享给 WSL
WSL 里装 usbip（linux-tools-6.8.0-147 的 /usr/lib/linux-tools-6.8.0-147/usbip）
WSL 里用 Google 官方 Linux 版 platform-tools 的 64 位 fastboot
```

`fastboot flash userdata` 把 8,382,658,048 字节的镜像自动拆成 12 个 sparse 块，
790 秒写完。

### 设备上的验证（probe=11）

```
NATIVE_UFS root windows
userdata first=0x253000  last=0x7aab7fa
manifest status=0  windows=2
```

- `manifest status` 从 1 变成 **0**：SHNGROOT manifest 被正确解析
- `windows` 从 0 变成 **2**：OS 与 SystemOS 两个 window 都在

### 关键教训

Windows 的 fastboot 是 32 位程序（PE machine 0x014C），无法处理 >2 GB 镜像；
Google 未提供 64 位 Windows 版本。想写大分区，必须走 Linux 64 位 fastboot。
