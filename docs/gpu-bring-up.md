# GPU bring-up on peridot

Recorded on 2026-11-09/10 against a Redmi Turbo 3 (peridot, SM8635) with an
unlocked bootloader. Every result below comes from a real `fastboot boot` on the
device. The driver is not a working GPU yet: it maps the registers, binds the
platform kit and stops.

## The part

| Property | Value | Source |
| --- | --- | --- |
| chip id | `0x43030b00` | stock DT `qcom,chipid`, palawan catalog |
| family | Adreno 735, `gen7_11_0` | Turnip FD735 |
| GMU window | `0x3d68000`, `0x37000` | peridot stock DT |
| GPU window | `0x3d00000`, `0x40000` | peridot stock DT |
| RSCC window | `0x3d50000`, `0x10000` | kgsl `reg` entry |
| PDC window | `0xb280000`, `0x10000` | GMU `gmu_pdc` entry |
| CX misc window | `0x3d9e000`, `0x1000` | kgsl `cx_misc` entry |
| GPU SMMU | `0x3da0000`, `0x40000`, streams {0,2} | `kgsl-iommu` children |
| firmware | `gen71100_gmu.bin`, 80,680 B, sha256 `435c7bac…` | palawan catalog |
| GMEM | 1.5 MiB (`0x180000`) | catalog `.gmem` |
| CGC mode | `0x00020202` | catalog `.gmu_cgc_mode` |

The GMU base matters: peridot's stock DT puts it at `0x3d68000`, while the
palawan mainline kernel uses `0x3d6a000`. Both windows end at `0x3d9f000`. The
board keeps the stock address.

## What is built

| Piece | Where | Notes |
| --- | --- | --- |
| platform kit | `package/sm8635-gpu/` | ShengGmu, ShengGmuCommandDb, ShengGpuSmmu, ShengGmuVotes |
| generated headers | same directory | four sources from `prepare-sm8635-gmu.py` in the SoC repository |
| driver | `drivers/Adreno735/NativeAdreno735.cpp` | skeleton, constructor entry point |
| archive | `build/adreno735/libAdreno735.a` | 18 objects: driver, 4 kit, 4 AdrenoFirmware, 8 ArmSmmu4K, 1 AdrenoPM4 |
| link | `build-xnu-4k-baseline.sh` | `-Wl,-force_load,$ADRENO735_ARCHIVE` |

`component:Adreno735` builds the driver object, `scripts/build-adreno735-kits.py`
compiles the kit and dependency-kit objects with the kernel's own `.o.json`
recipes and appends them to the same archive, and the kernel link force-loads it.

## Entry point: this kernel installs no personality

The first six images were a search for how a built-in driver is ever started.
Each step changed one thing:

| Image | Change | Screen | Conclusion |
| --- | --- | --- | --- |
| p61 | driver linked, GPU nodes still `disabled` | UFS root failure | no NU735: nodes not in the ADT |
| p62 | `gpu`/`gmu`/`gpu_smmu` set `okay` | same | publishing the nodes does not disturb boot |
| p63 | panic at `start()` | same | `start()` never ran |
| p64 | panic at `init()` | same | no IOService instance was created |
| p65 | panic in a Mach-O constructor | `NU735 ctor gpu=1` | the constructor runs and reads boot arguments |
| p66 | stage 1-2 inside the constructor | `NU735 ctor gpu=1 mapped=5 bind=0` | window map and kit bind succeed |

The `Info.plist` personality in `drivers/Adreno735` never matches: personalities
are only registered by kexts that start, and this kernel does not start
built-in components as kexts. The constructor in the kernel's
`__mod_init_func` is the usable entry point, which is what sheng's Adreno740
comment means by "no personality is installed; the board probe calls the
bring-up directly".

At constructor time `ml_io_map` is safe to call (p66 proves it). It is declared in
`osfmk/arm/machine_routines.h`; the exported `machine/machine_routines.h` path does
not carry it, so the driver declares it itself.

## Stage 1-2 result

```
panic(cpu 0 caller 0xfffffff0073d21dc): NU735 ctor gpu=1 mapped=5 bind=0
@NativeAdreno735.cpp:92
```

`mapped=5` is the five windows; `bind=0` is `ShengGmuSuccess` from
`ShengGmuBindMmio`. Nothing was read from or written to the GMU: the driver
stops before the firmware stage on purpose.

## Reproduce

```sh
./unixnu kernel -D ../UniXNU-peridot
bash peridot/tools/wsl-pack-p53.sh p66 15
usbipd attach --wsl Ubuntu-24.04 --busid <bootloader busid>
fastboot boot out/Mu-peridot-p66.img
```

`kernel_args` in `board.toml` carry `peridot-gpu=1` and `peridot-gpu-panic=1`;
without them the constructor returns immediately and the boot is unchanged.

## What remains

| Stage | Needs | Where it comes from |
| --- | --- | --- |
| 3 | the 80,680-byte GMU firmware, five GMU memories, and the sha256/time/wait services | embedded blob or payload file; `vm_allocate`; kernel `sha2.h` |
| 4 | the GPU SMMU context for streams {0,2} | `ShengGpuSmmuSource.h` already has base, size and SIDs |
| 5 | the GMU clocks and GPU_CC_CX/GX power domains | palawan.dtsi nodes, `reference/palawan-gpu-nodes.dtsi` |

The area stays `implemented` rather than `device-verified` until a GPU provider
renders a frame, which is the completion criterion in `porting.toml`.
## Where stage 3 stops: the power gate

Stages 1 and 2 are device-verified. Stage 3 (firmware load and HFI) is written and
runs, but every path to the GMU now ends at the same wall: the GPU power domain
cannot be raised from this boot context.

| Image | Question | Result |
| --- | --- | --- |
| p67 | can the kit hash the firmware at constructor time? | data abort inside `gmu_sha256`: the kernel ccdigest glue is not initialised that early |
| p68 | with a local SHA-256? | `status=4 phase=1 hash=1`: hash verified, kit then fails an IO check |
| p69 | is the register offset right? | offset fixed to window + 0x14000; the access now reaches the bus and aborts there |
| p70 | is GPUCC readable? | yes: `cx=0x80000000 gx=0x0 hw=0x80000000` |
| p71 | do the sm8650 clock offsets work? | no: all five read back zero |
| p72 | do the vendor gpucc-cliffs offsets work? | four of five enable with bit 31 set |
| p73 | can the GMU be read with those clocks? | no: external abort at the GMU window |
| p74 | why is `gpu_cc_ahb_clk` stuck? | it reads zero and ignores writes while the CX branches answer |
| p75 | do the GCC GPU clocks help? | they enable (qmip 0x80000003) but the AHB branch stays zero |
| p76-77 | does a printf harness work? | no: the framebuffer console is not up at constructor time |
| p78 | can the hub RCG be programmed? | yes: `cmd=0x0->0x2`, ROOT_EN stays set |
| p79 | is the AHB branch reachable now? | no: accessing 0x90c8 now aborts on the bus |
| p80 | can the AP RSC be read? | no: DRV_ID at 0x17a00000 aborts |

### Why this is not a register problem

The vendor `gpucc-cliffs.c` defines **no GDSCs at all** (`grep -i gdsc` returns
nothing), while the sm8650 driver defines two. That means the CX and GX domains
on this part are not software GDSCs: they are hardware domains voted through
RPMh. The same DT shows `rsc@17a00000` carrying a `power-domains` property, so
the mailbox used for voting needs power that is itself voted through that
mailbox. Linux escapes this because its boot chain leaves the RSC available; a
UEFI to `fastboot boot` to XNU start does not.

### Options

| Path | Work | Note |
| --- | --- | --- |
| Firmware-side vote | teach the UEFI payload to raise CX before hand-off | Mu-Silicium has no RPMh or AOP code (searched `Silicon/Qualcomm`), so this is new firmware work |
| Port the power framework | RPMh, its resource map, and the GDSC equivalents inside XNU | weeks of work, and it still assumes the RSC answers |
| Inherit the state | boot from a context where the vendor kernel already voted | fastest way to prove the GMU code, not a shippable path |

Until one of these is chosen, the GPU record stays `implemented`: the driver,
the kit and the firmware image are in the kernel, and the only missing piece is
permission to touch the hardware.
