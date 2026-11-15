# Sm8635Platform - GPU platform operations for SM8635 ("cliffs")

Status: **skeleton**. Nothing in this directory has been compiled, and none of it
has run on hardware. It exists so the SM8635-side GPU work has a fixed shape and
so the values that are still unknown are written down in one place.

## What is here

| File | Origin | State |
|---|---|---|
| `ShengGmu.c` / `.h` | SM8550 kit | byte-identical, needs `ShengGmuSource.h` |
| `ShengGpuSmmu.c` / `.h` | SM8550 kit | byte-identical |
| `ShengGmuCommandDb.c` / `.h` | SM8550 kit | byte-identical |
| `ShengGmuVotes.h` | SM8550 kit | byte-identical, implementation stays in the device repo |

Copied from `UniXNU-sm8550-common` `kits/Sm8550Platform` at
`a9f45dc60c035084b95463781538380f4c3606c9`; hashes were compared after copying
(`ShengGmu.c` e5905a86, `ShengGmu.h` d11224f2, `ShengGpuSmmu.c` 4e49504b,
`ShengGpuSmmu.h` 4cba68d4, `ShengGmuCommandDb.c` 36a605f9,
`ShengGmuCommandDb.h` a7ae5e86, `ShengGmuVotes.h` 1018d182).

The body is SoC-agnostic by construction: every register offset, window address,
chip id and firmware hash arrives through the generated `ShengGmuSource.h` and
the caller's `ShengGmuConfig`. The SM8635 delta is therefore confined to that
header - which does not exist yet.

## Build

`scripts/build-sm8635-gmu-kits.py` compiles the three kit sources for aarch64
against the generated headers. Run on 2026-11-09 it produced:

| object | bytes |
|---|---|
| `ShengGmu.o` | 10,344 |
| `ShengGmuCommandDb.o` | 2,184 |
| `ShengGpuSmmu.o` | 4,504 |

The objects are **not linked into the kernel yet**: there is no Adreno735 driver
to call them, no catalog entry to select them and no `board.toml` archive line.
The script uses a freestanding `--target=aarch64-none-elf` compile; once the
objects go into the kernel they have to be built with the kernel's own
`.o.json` recipe flags, the way sheng's `build-adreno740-kits.py` does.

## The window map (derived, not guessed)

`scripts/prepare-sm8635-gmu.py` reads the compiled device tree and produces
`build/sm8635-gmu/windows.json`; once the pinned sources are in place it also
writes `source-report.json` with the register set, the fatal masks and the whole
RSC/PDC table:

| window | base | bytes | DT source |
|---|---|---|---|
| Core | `0x3d68000` | `0x37000` | `/soc/qcom,gmu@3d68000` reg `gmu` |
| Rscc | `0x3d50000` | `0x10000` | `/soc/qcom,kgsl-3d0@3d00000` reg `rscc` |
| Pdc | `0xb280000` | `0x10000` | gmu reg `gmu_pdc` |
| Gpu | `0x3d00000` | `0x40000` | kgsl reg `kgsl_3d0_reg_memory` |
| CxMisc | `0x3d9e000` | `0x1000` | kgsl reg `cx_misc` |

Chip id `0x43030b00` (`qcom,chipid`), model `Adreno735`, core-to-gpu delta
`0x68000` (= `0x3d68000 - 0x3d00000`).

The same run writes the other two kit headers:

| header | values | source |
|---|---|---|
| `ShengGpuSmmuSource.h` | base `0x3da0000`, `0x40000`, streams `{0,2}`, GMU SID `5` | `kgsl-iommu` children and the GMU node |
| `ShengGmuCommandDbSource.h` | base `0x81c60000`, `131072` bytes | `/reserved-memory/aop_cmd_db_region` |

peridot's GPU streams are `{0,2}` (`gfx3d_user` 0, `gfx3d_secure` 2) where sheng
uses `{0,1}`; the stock node keeps them on the `kgsl-iommu` children, so the
generator reads those when the `kgsl` node has no `iommus` property of its own.

The GPU node in `dts/sm8635.dtsi` now carries all four stock ranges; before this
change it was missing `rscc` and called the misc block `cx_mem`
(peridot-stock.dts:13566-13567 is the source).

## The RSC/PDC sequence, and why it is not sheng's

The table is extracted from `a6xx_gmu_rpmh_init()` in pinned `a6xx_gmu.c`
(torvalds/linux v6.16, `scripts/data/sm8635-adreno-inputs.json`) along the
**non-A740** path. A735 is an a7xx part that is not `adreno_is_a740_family`, so
three things differ from the SM8550 generator's assumptions:

1. `adreno_is_a7xx()` sets `pdc_in_aop`, so the PDC microcode block is skipped by
   the `goto setup_pdc` and only the two `setup_pdc` writes remain. The generator
   proves the skip is still there before it extracts anything.
2. The sequencer register stays `REG_A6XX_RSCC_SEQ_MEM_0_DRV0` at word `0x180`;
   the A740-only override to `RSCC_SEQ_MEM_0_DRV0_A740` (word `0x154`) is present
   in the source and deliberately not taken.
3. The one ternary in the RSC block resolves to the else value `0x80000000`, not
   the A740 value `0x80000021`.

Result: 19 writes, and the difference is visible in the output - byte `0x600`
instead of `0x550` for the sequencer, and `0x80000000` at byte `0x48`.

```
write  window   offset     value
0      Rscc     0x10       0x01000000     RSC_STATUS0 = BIT(24)
1      Rscc     0x2c       0x00000001
2      Rscc     0x38       0x00000000
3      Rscc     0x34       0x00000000
4      Rscc     0x40       0x00000000
5      Rscc     0x3c       0x00000000
6      Rscc     0x48       0x80000000     (A740 would be 0x80000021)
7      Rscc     0x44       0x00000000
8      Rscc     0x400      0x00000000
9      Rscc     0x20       0x00004520
10     Rscc     0x24       0x00004510
11     Rscc     0x28       0x00004514
12-16  Rscc     0x600..    0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2, 0x0020edad
17     Pdc      0x4520     0x00000000
18     Pdc      0x4500     0x80000001
```

## What `ShengGmuSource.h` needs, and what is already derived

1. **Register offsets and fatal masks** - done: read from pinned `a6xx.xml` and
   `a6xx_gmu.xml`. The A6XX domain is shared with SM8550; `GMU_GENERAL_8/9/10`,
   `CX_MISC_TCM_RET_CNTL` and the A740 sequencer register carry A7XX variants.
2. **Firmware identity** - resolved on 2026-11-09: **`gen71100_gmu.bin`**,
   80,680 bytes, sha256 `435c7bac9a328b8a100df930b84b65b4d579e42be44785d0e60e3729291bb864`.
   It is pinned in `scripts/data/sm8635-adreno-inputs.json` and the generator
   refuses any other image. The other candidate, `gmu_gen70900.bin`, belongs to
   the 70900 GPU bin: `/odm/firmware` ships `gen70900_zap.mbn` and
   `gen71100_zap.mbn` side by side, and a clean boot loads **`gen71100_zap.mbn`**
   (`kgsl-3d0: Direct firmware load for gen71100_zap.mbn`, then ueventd loads it
   from `/odm/firmware` at 6.87 s). The driver's own string blob lists
   `gen71100_gmu.bin` and `gen71100_sqe.fw` next to
   `qcom,adreno-gpu-gen7-11-0`, which matches the DTS revision.
3. **GMU firmware chip id** - **confirmed**: `0x070b0000`.
   Upstream v6.16 has no A735 entry (0x43030b00 never appears in
   `a6xx_catalog.c`), and the vendor `msm_kgsl.ko` does not carry it either: the
   module is ET_REL, its string table has
   `gen71100_gmu.bin`/`gen71100_sqe.fw`/`qcom,adreno-gpu-gen7-11-0`, but no
   byte order of 0x43030b00 exists anywhere in the file - the model and chip id
   come from the device tree.

   The value was first derived from the GEN7_x_y encoding (upstream commit
   `2c4c53f`: a740 gen7-2-1 = `0x07020100`, a750 gen7-9-1 = `0x07090100`;
   this part is gen7-11-0 with no `qcom,patchid`), giving `0x070b0000`. It was
   then confirmed by the **palawan-mainline** tree, whose catalog entry for
   `0x43030b00` carries `.gmu_chipid = 0x70b0000` next to
   `gen71100_gmu.bin`/`gen71100_sqe.fw`/`gen71100_zap.mbn`
   (codeberg.org/palawan-mainline/linux, branch `palawan/v7.1`).

   The generator uses the catalog when the pinned tree has the chip, otherwise
   the pinned `gmu_chip_id` block, and stamps the provenance into the header.

## What the palawan entry also settles

The same catalog entry carries four values the kit will need before the GMU can
be brought up, none of which exist in the XNU tree yet:

| field | value | where it goes |
|---|---|---|
| `.gmem` | `SZ_1M + SZ_512K` (1.5 MiB) | GPU reserved memory; the DT carve-out `gpu_microcode_region` is only 8 KiB |
| `.gmu_cgc_mode` | `0x00020202` | written during GMU bring-up |
| `.family` | `ADRENO_7XX_GEN2` | explains why `.hwcg` reuses `a740_hwcg` while the rest differs |
| `.bcms` | SH0 width 16, MC0 width 4, ACV fixed with perfmode `BIT(2)` at 10,687,500 | the three BCM votes the GMU startup sends |

`.preempt_record_size = 3572 * SZ_1K` and `.quirks = CACHED_COHERENT | HW_APRIV |
PREEMPTION` also differ from the A740 entry.

To re-derive the firmware choice: `adb reboot`, then immediately
`adb shell dmesg | grep -i zap` before the ring buffer rotates (the boot log is
gone after a few minutes; pstore is empty on this ROM).

## SM8635 against SM8550, for the record

| item | SM8635 (A735) | SM8550 (A740) |
|---|---|---|
| GMU base | `0x3d68000`, `qcom,gen7-gmu` | `0x3d6a000`, `qcom,adreno-gmu-740.1` |
| RSCC window | second range of the kgsl node, `0x3d50000` | same shape |
| CX misc window name | `cx_misc` | `cx_mem` |
| chip id | `0x43030b00` | `0x43050a01` |
| GPU SMMU | `0x3da0000`, streams 0/1/5 | `0x3da0000` |

## Rules for whoever picks this up

- Do not fill a value in by hand. If it is not derivable from the DTB, the pinned
  kernel sources or the firmware file, it stays missing.
- The copied body has never executed against an A735. Treat the RSC sequence and
  the GMU firmware handshake as unverified until the A740-only assertions in the
  generator have been replaced and the result has run on the device.
- Keep `build/` out of anything packaged; it is generated.
