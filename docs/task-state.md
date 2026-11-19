# 任务状态（2026-11-19 23:20）

## 当前主线结论

- **p530**：p490 FD + 当前 GDSC 内核，XNU panic 正常，是可靠基线。
- **p538**：hoshikv DarwinBoot + 全套 hoshikv patched 驱动 + RELEASE；无 fastboot 参数，XNU GDSC panic 正常。
- **p539**：p538 基线 + `fastbootdebug=1`；DARWINBOOT Fastboot UI 正常，主机枚举成功：
  - `fastboot devices` -> `bf7c781b fastboot`
  - `fastboot getvar product` -> `peridot`
- **p540**：payload `timeout = -1`，UniXNU 菜单稳定停留。
- **p541**：`timeout = 10 + fastbootdebug=1`；启动后主机未出现 UEFI Fastboot，未得到 `oem log` 结果。

## 当前源码/构建状态

- `C:\ms\DarwinBootPkg\Driver`：hoshikv DarwinBoot 基线（Fastboot 由 `fastbootdebug=1` 控制）。
- `C:\ms\Binaries\peridot\QcomPkg\Drivers`：8 个 hoshikv patched 驱动，INF 指向 `.patched.efi`：
  - ButtonsDxe、ClockDxe、PmicDxe、SPMDxe、TzDxeLA、UFSDxe、UsbConfigDxe、UsbMsdDxe
- `peridot/UniXNU-peridot/board.toml`：
  - 默认 `fastbootdebug=1` 已移除；
  - payload `timeout = 10`（当前实验值；p540 使用过 `-1`）。
- `peridot.dsc` / `peridot.fdf`：AdvancedLog 原始配置已恢复。
- `peridot/UniXNU-peridot/docs/historical-leftovers.md`：历史遗留盘点已写入。

## 已确认根因

A 路线黑屏不是单个 Fastboot/GDSC/AdvancedLog 问题，而是：

- 旧 DarwinBoot 解析器；
- 未使用 hoshikv patched 驱动；
- DEBUG/RELEASE 与驱动组合不一致。

p538 的组合是可启动基线。

## 待办

1. 在 UniXNU 菜单增加显式 `fastboot` 条目或 action，直接调用 `DarwinFastbootMode()`；
2. 保持菜单不自动启动，主机执行 `fastboot oem log` 验证；
3. 确认 p541 未进入 Fastboot 的原因（菜单未按 Enter / Fastboot 未触发）；
4. 将 `board.toml` 的 `timeout` 固定到最终产品值（当前 10 为实验值）；
5. 清理 C 类备份前先列清单，不动 Build 树。

## 设备

- 最近一次停在 p541 启动尝试；主机未枚举到 UEFI Fastboot。
- boot_a 未写入未签名镜像；可用 stock boot 恢复。
## 2026-11-19 23:30 ׷��

- p542: explicit Fastboot entry injected; menu shows entry but numeric/arrow input did not switch.
- p543: Fastboot entry made default via build option FASTBOOT_ENTRY=1; config timeout=3/default=fastboot.
- p543 host still `fastboot devices` empty; Fastboot entry does not enumerate on this run.
- build-apfs-md.sh now supports FASTBOOT_ENTRY=1 (append Fastboot entry + make default).
- Current board.toml timeout=3.
- p538/p539 remain the known-good XNU/Fastboot baseline.
## 2026-11-19 23:50 ׷��

- p544: explicit Fastboot entry visible; selected -> `Fastboot returned: Unsupported`.
- p546: early Fastboot-only branch with zero context -> synchronous exception.
- p547: relaxed input validation; host still no UEFI Fastboot.
- p548: Fastboot entry copies full XNU args + initrd; host still no enumeration.
- Conclusion: explicit entry does not reproduce p539 USB path. p538/p539 remain the only known-good baseline; do not use p543-p548 as mainline.
## 2026-11-19 23:58 ׷��

- p545/p547 screenshots show DARWINBOOT Fastboot UI active: USB host connected, 65540 bytes staged.
- However `usb rx queue failed: Not Ready` repeats; `fastboot devices` is empty and `oem log` returns unknown.
- Root cause is now the Fastboot USB OUT transfer queue, not the menu timeout or Fastboot entry.
- Fastboot.c FastbootUsbRead calls mUsb->Send(USB_EP_OUT) and returns on Not Ready; the endpoint is not rearmed/retried.
- Next: fix the USB RX transfer rearm/retry, then re-test fastboot oem log.
