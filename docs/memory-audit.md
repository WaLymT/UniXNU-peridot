# 内存范围核实（2026-11-20）

本记录只做静态核对；在拿到本次改动后的 UEFI memory map 前，不打包、不刷写。
本分支（wip/ufs-smmu）的修正同步自 main，仍需在同一份固件上重新取 map。

## 当前配置

| 项 | 值 | 来源 |
| --- | --- | --- |
| XNU DRAM 窗口 | 0x80000000 + 0x80000000 (2 GiB) | dts/peridot.dts memory node |
| payload.dram_size | 0x80000000 | board.toml |
| payload.adt.owned_memory_size | 0x80000000 (本次从 4 GiB 下调) | board.toml |
| UEFI FD | 0xa7100000 + 0x300000 | Mu-Silicium MemoryMapLib.c |
| FD reserved | 0xa7000000 + 0x100000 | 同上 |
| UEFI FD reserved | 0xa7400000 + 0x200000 | 同上 |
| DXE heap | 0xa8000000 + 0x1ba00000 | 同上 |
| Display_Reserved | 0xe3940000 + 0x2b00000 | 同上 / DTS splash |
| pram (本次修改) | 0xe6440000 + 0x400000 | board.toml / dts/peridot.dts |
| 物理 DDR | 12 GiB (ddr_size = 0x0c) | stock DTS；ADT 只声明低 2 GiB 连续窗口 |

## 发现

1. 旧值 owned_memory_size = 0x100000000 (4 GiB) 超出 DTS 的 2 GiB 窗口，不再使用。
2. dram_size = 0x80000000 与 DTS 一致，保留。
3. 旧 pram 0xa7000000 + 0x400000 与 FD_Reserved 0xa7000000 + 0x100000、UEFI_FD 0xa7100000 + 0x300000 完全重叠。
4. pram 已移到 0xe6440000 + 0x400000：紧接 Display_Reserved 结束地址，结束于 0xe6840000，仍在 2 GiB 窗口内。
5. 新地址已在 2026-11-20 的 live UEFI map 上确认（见下节），TODO(verify) 关闭。

## 结论

- owned_memory_size 已修正为 2 GiB，wip/ufs-smmu 与 main 取值一致；
- pram 已避开 UEFI FD，但需要下一次启动的 UEFI map 确认；
- live map 确认前不打包、不 fastboot boot/flash；
- 确认后把 UEFI map 关键区间和本次 revision 写入 porting evidence，再恢复打包/刷写。

## Live 验证（2026-11-20 13:20）

p553（kernel args 加 `pramcheck=1`）在 DarwinBoot fastboot 分支打印 live UEFI map 后停住，
屏幕结果：

```
PRAM check, live UEFI map, window e6440000-e6840000:
PRAM E6440000+19BC0000 type 7
```

- type 7 = `EfiConventionalMemory`；
- 该描述符从 0xe6440000 起长 0x19bc0000 字节，pram 窗口 0xe6440000-0xe6840000 完全落在其中；
- 结论：新 pram 地址没有和 UEFI 任何保留区重叠，静态审计的推论成立。

证据形式为屏幕拍照：本轮 USB fastboot 枚举失败（未改动的 p539 同样失败），`oem log`
取不到，所以这段 map 没有走日志通道。完整 map dump 仍待 USB 修好后补。
