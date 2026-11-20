# UniXNU peridot

Device support for the Xiaomi Redmi Turbo 3 and POCO F6, codename `peridot`,
Qualcomm SM8635 ("cliffs"). The board selects
[UniXNU-sm8635-common](https://github.com/OWNER/UniXNU-sm8635-common) and the
`ipados-17.7.11-21H461` OS profile in UniXNU.

This repository is a scaffold. The configuration is complete enough to resolve
and inspect; no build or boot result exists yet. Every subsystem is recorded as
`planned` in [porting.toml](porting.toml).

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

- `payload.dram_size` and `payload.adt.owned_memory_size`: the owned DRAM
  window (2 GiB since 2026-11-20, matching the single DTS memory node).
- `userdata.first_lba` and `userdata.last_lba`: from this phone's GPT.
- `payload.adt.chip_id`: currently the impersonated platform's id.
- `payload.adt.root_match_volume_uuid`: a property of the selected iPadOS image.

Do not fill these by guesswork; each one either defines a memory window or
selects a filesystem.

## Credits

Borrowed code and repository sources are listed in [docs/ATTRIBUTION.md](docs/ATTRIBUTION.md).

## Documentation

- [Device repository](https://unixnu.com/porting/device-repository)
- [Device trees](https://unixnu.com/porting/device-tree)
- [Porting record](https://unixnu.com/porting/records)
- [Prelink](https://unixnu.com/concepts/prelink)
