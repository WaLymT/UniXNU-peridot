# NativeAdreno735

SM8635 (Adreno 735) GPU bring-up. **Skeleton**: nothing runs unless `peridot-gpu=1`
is on the command line, and even then only the window map is exercised.

## What the skeleton does

1. `probe()` claims nothing and `start()` returns early when the boot argument is
   absent, so a default boot gains no new service.
2. With `peridot-gpu=1`, `bringUp()` maps the five windows named by the generated
   `ShengGmuSource.h` (gmu, rscc, pdc, gpu, cx_misc) through `ml_io_map` and binds
   them with `ShengGmuBindMmio`.
3. It then stops. Stages 3-5 are TODO on purpose: the GMU firmware image, the five
   GMU memories and the sha256/time services the kit demands do not exist here yet.

```
peridot-gpu=1   run the window map and the kit bind
```

## Why it stops at stage 2

`ShengGmuLoadAndStartHfi()` validates everything before touching a register:
the chip id, the 80,680-byte firmware and its sha256, the five memory extents
(including the 0xffc000-byte icache), and three real services. Handing it a stub
would either fail validation (correct) or, if the stub were permissive, write a
wrong sequence to the GMU (incorrect). The skeleton keeps the wrong half out.

## What the next stages need

| stage | input needed | where it comes from |
|---|---|---|
| 3 | GMU firmware `gen71100_gmu.bin` | embedded blob or payload file; hash is already in the header |
| 4 | five GMU memories and their IOVAs, mapped through the GPU SMMU | GPU SMMU context from `ShengGpuSmmuStart`, memory from `IOBufferMemoryDescriptor` |
| 5 | sha256, monotonic time and a sleep service | kernel `sha2.h`, `mach_absolute_time`, `delay` |
| 6 | GPU SMMU context for streams {0,2} at 0x3da0000 | `ShengGpuSmmuSource.h` already has the numbers |
| 7 | power: GPU_CC_CX/GX GDSCs and the seven GMU clocks | palawan.dtsi nodes, see `reference/palawan-gpu-nodes.dtsi` |

## Build

The driver and the three kit sources are compiled by
`scripts/build-adreno735-kits.py`, which reuses the kernel's own `.o.json`
recipes so the objects match the kernel the archive is linked into.
