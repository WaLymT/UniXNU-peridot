# UniXNU peridot

Device support for the Xiaomi Redmi Turbo 3 and POCO F6, codename `peridot`,
Qualcomm SM8635 ("cliffs"). The board selects
[UniXNU-sm8635-common](https://github.com/OWNER/UniXNU-sm8635-common) and the
`ipados-17.7.11-21H461` OS profile in UniXNU.

## Current status

`main` is the public snapshot and its porting-record CI passes. It selects only
published catalog components; the private `Adreno735` driver and its catalog
registration live on `wip/ufs-smmu` and are not part of this branch yet.

The device evidence recorded in [porting.toml](porting.toml) is split into two
classes:

- **current evidence**: the recorded revision is an ancestor of `main` and the
tracked paths are unchanged since that revision;
- **historical evidence**: the device result exists, but the revision predates
`main` or the referenced files have changed. Those areas are marked
`implemented`, not `device-verified`.

Therefore a `device-verified` status means the evidence still applies to the
current tracked code; it does not claim that this exact `main` commit has been
rebuilt and reflashed. A clean rebuild and flash test is not part of CI yet.

Historical firmware images such as p538/p539 and the Fastboot work remain valid
device observations, but they belong to the `wip/ufs-smmu` line until the private
components and firmware sources are published here.

## Evidence status on main

| Area | Status | Evidence interpretation |
| --- | --- | --- |
| cpu | device-verified | current path evidence |
| memory | implemented | 2 GiB window changed; live UEFI map verification pending |
| display | device-verified | current path evidence |
| userspace | device-verified | current path evidence |
| firmware | implemented | historical device evidence; not reproducible from main as-is |
| storage | implemented | historical device evidence; not reproducible from main as-is |
| diagnostics | implemented | historical device evidence; not reproducible from main as-is |
| packaging | implemented | historical host evidence; current CI only |
| gpu | implemented | private Adreno735 work is on wip/ufs-smmu |
| input/usb/network/audio/power | planned | no current evidence |

Detailed current/historical evidence counts are in [docs/evidence-status.md](docs/evidence-status.md). Memory ranges are audited in [docs/memory-audit.md](docs/memory-audit.md). Branch responsibilities and sync rules are in [docs/BRANCHES.md](docs/BRANCHES.md).

## Structure

| Path | Contents |
| --- | --- |
| `porting.toml` | Subsystem inputs, findings, evidence and completion criteria |
| `board.toml` | Board configuration, component selection, firmware and payload inputs |
| `feeds.toml` | Pinned SoC source |
| `configs/peridot_defconfig` | Board kernel settings |
| `dts/peridot.dts` | Memory, CPU clusters, reservations and enabled peripherals |
| `dts/vendor/peridot-stock.dts` | The phone's own decompiled DTS, kept as a reference only |
| `patches/xnu/series` | Board patches |
| `proprietary-files.txt` | Required firmware inputs |
| `package/` | Board package, exposed as `kits/peridot` in the build tree |

## What is already known

| Item | Value | Source |
| --- | --- | --- |
| SoC | SM8635, vendor SKU `cliffs` | on-device `ro.boot.product.vendor.sku` |
| Page size | 4096 | on-device `ro.boot.hardware.cpu.pagesize` |
| Current slot | `_a` | on-device `ro.boot.slot_suffix` |
| Panel | 1220x2712, 32 bpp | UEFI `peridot.dsc` and `wm size` |
| Firmware | Mu-Silicium `Platforms/Xiaomi/peridotPkg` | upstream, marked Inactive |
| UFS | host 0x1d84000, PHY 0x1d80000, stream 0x60 | stock DTS |
| GPU | Adreno 735, chip id 0x43030b00 | stock DTS |
| Boot image | header v0, pagesize 4096 only | measured, see [UEFI bring-up](docs/uefi-bringup.md) |
| UEFI | Silicium menu appears on the panel | measured, see [UEFI bring-up](docs/uefi-bringup.md) |

The firmware chain is verified on hardware: the stock `peridotPkg` firmware
builds to a 3 MiB volume, boots through `fastboot boot`, and shows the Silicium
UEFI menu. The volume has no bootable entry yet; adding UniXNU to it is the next
step. Every other subsystem is still `planned` in [porting.toml](porting.toml).

## Build

Requirements and toolchains are in
[Requirements](https://unixnu.com/guide/requirements). Run from the UniXNU
checkout:

```sh
./unixnu defconfig peridot_defconfig -D ../UniXNU-peridot
./unixnu fetch -D ../UniXNU-peridot
./unixnu status --json -D ../UniXNU-peridot
```

Then follow [Building](https://unixnu.com/building) for kernel, package,
firmware, payload and boot image. Firmware needs a Mu-Silicium checkout passed
as `--uefi`. The kernelcache, trust cache, Data seed and platform seed are local
inputs and never enter this repository.

## Values still to verify

`board.toml` marks every unverified value with `TODO(verify)` or `TODO(REQUIRED)`:

- `payload.dram_size` and `payload.adt.owned_memory_size`: the 12 GiB window.
- `userdata.first_lba` and `userdata.last_lba`: from this phone's GPT.
- `payload.adt.chip_id`: currently the impersonated platform's id.
- `payload.adt.root_match_volume_uuid`: a property of the selected iPadOS image.

Do not fill these by guesswork; each one either defines a memory window or
selects a filesystem.

## Handoff

- [2026-11-20 handoff: local and remote repositories](docs/HANDOFF-2026-11-20.md)

## Documentation

- [Device repository](https://unixnu.com/porting/device-repository)
- [Device trees](https://unixnu.com/porting/device-tree)
- [Porting record](https://unixnu.com/porting/records)
- [Prelink](https://unixnu.com/concepts/prelink)
