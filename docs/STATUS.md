# Bring-up status

This page is the short version of [porting.toml](../porting.toml). The record is
authoritative: every finding there carries the command, revision and result it
came from. Screen photographs referenced by the record are not committed; they
contain only the device screen and boot logs.

## Verified on hardware

| Stage | Result |
| --- | --- |
| UEFI | The stock Mu-Silicium `peridotPkg` firmware boots from `fastboot boot`, shows the Silicium menu, and the UniXNU menu follows |
| Kernel | XNU starts from the prelinked kernel, installs the device-tree platform, brings up the secondary CPUs and draws a framebuffer console |
| Interrupts / PMGR | GIC, PSCI, SMP start and the platform power hooks are alive; panics and the last-panic screen work |
| UFS | Native UFS host/PHY/ICE/MCQ bring-up; the GPT is read through the block interface at 4 KiB logical blocks |
| Root device | The UFS window is published as a bdevsw disk, claimed as root, and APFS mounts it through the restore/ramdisk branch |
| iPadOS System container | The 6,077,546,496-byte APFS container from the OS image mounts as root and `/sbin/launchd` is exec'ed from it (p368, p383) |
| Install target | Apple `newfs_apfs` formats the 8 GiB staging volume on the UFS install target (p372-p381, p401) |
| SHNGROOT userdata | The official `unixnu build userdata` image is flashed; the OS window (GPT + Apple_APFS partition at +0x104400) is mounted as root and `launchd` starts from it (p418) |

## Does not work yet

- **dyld shared cache.** `launchd` starts but dyld aborts with `Library not loaded: /usr/lib/libSystem.B.dylib` because the SystemOS cryptex has no graft. The cryptex graft prototype (file-vnode graft plus VFS mount-argument work) is in the patch series but the signed cryptex metadata is not redistributed (see [proprietary inputs](proprietary.md)).
- **Writable Data volume.** The Data volume from the payload seed is still read-only plumbing; there is no working persistent store.
- **Input, network, USB, cellular, audio, power.** Declared as `planned` in the porting record.
- **GPU.** The Adreno 735 driver and the SM8635 GMU kit are skeletons. The component is not selected in the public build because the vendor GPU firmware is not redistributed; the sources stay in the tree for later work.

## Next steps

1. Provide the dyld shared cache: mount the SystemOS crypto volume, graft it over `/System/Library/Caches` (or mount the cryptex as the OS profile expects), and confirm that dyld resolves `libSystem`.
2. Make the Data volume writable and persistent so launchd can create `/private/var` state.
3. Then enable the remaining board peripherals in the order of the porting record.

## Reproducing a boot

See the [README](../README.md#building) for the build steps. A normal round
needs a cold boot into `fastboot`, `fastboot flash userdata` with the image
produced by `unixnu build userdata`, and `fastboot boot` of the boot image
assembled from the kernel, payload and DTB.
