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
5. 新地址尚未通过本次固件的 live UEFI memory map 验证，已标 TODO(verify)。

## 结论

- owned_memory_size 已修正为 2 GiB，wip/ufs-smmu 与 main 取值一致；
- pram 已避开 UEFI FD，但需要下一次启动的 UEFI map 确认；
- live map 确认前不打包、不 fastboot boot/flash；
- 确认后把 UEFI map 关键区间和本次 revision 写入 porting evidence，再恢复打包/刷写。
