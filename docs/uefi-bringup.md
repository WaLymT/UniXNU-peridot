# UEFI bring-up on peridot

Recorded on 2026-10-26/27 against a Redmi Turbo 3 (peridot, SM8635) with an
unlocked bootloader. Every result below comes from a real `fastboot boot` on the
device, not from reading upstream code.

## What was built

Mu-Silicium `Platforms/Xiaomi/peridotPkg`, RELEASE, CLANGPDB, AARCH64:

| Artifact | Size | Note |
| --- | --- | --- |
| `SILICIUM_UEFI.fd` | 3.00 MiB (0x300000) | equals `[uefi_fd] size` in the device config |
| BootShim | 112 B | header carries `text_offset = 0xA7100000`, `image_size = 0x300000` |
| `Mu-peridot.img` | 1.72 MiB | BootShim + gzip(FD) + appended DTB |

## Boot image header: v0 + 4096 only

The device rejects every signed-header variant and accepts the legacy layout.

| Image | Bootloader reply |
| --- | --- |
| header v4 | `Failed to load/authenticate boot image: Invalid Parameter` |
| header v4 with `os_version`/`os_patch_level` | same |
| header v1 | same |
| header v0, default page size 2048 | same |
| **header v0, `--pagesize 4096`** | **`Booting OKAY`, UEFI menu appears** |

The v4 failures are an authentication failure, not a parse failure: a v4 boot
image carries a boot signature, and the one produced here has a zero-filled
signature area. The device does not fall back to unsigned v4 images.

Consequence for this board: `[bootimg.image]` in `board.toml` must pin
`header_version = 0` and `pagesize = 4096`, and the DTB is appended to the
compressed kernel rather than passed as a v2+ DTB field.

## What the device showed

The Silicium UEFI menu appears on the panel. Display, BDS and the menu are
working; the port is usable as a firmware environment.

The menu does not stay up indefinitely: with no bootable entry it returns to
the bootloader after a while. Adding UniXNU to the firmware volume is the next
step that gives it something to boot.

## Bootloader behaviour worth knowing

- After repeated failed `fastboot boot` attempts the bootloader marks the
  active slot unbootable and only returns to `fastboot`. `fastboot
  --set-active=a` clears that flag; no reflash is required.
- A 96 MiB partition write can fail with
  `AdbWriteEndpointSync failed` (ERROR_SEM_TIMEOUT) on a marginal cable or a
  front-panel port. Re-plugging the cable recovers it. Prefer a rear USB 2.0
  port for large transfers.
- `fastboot continue` reports `Failed to load image from partition` when the
  system partition is unusable, which distinguishes a broken system from a
  broken bootloader.

## Reproducing

```powershell
# build (Windows, short build root; see the porting record)
peridot\tools\run-build-direct.ps1 -Clean

# package for fastboot: v0 + 4096 is mandatory on this board
cd <tree>
python Resources\Scripts\mkbootimg.py `
  --kernel Build\peridotPkg\peridot-kernel `
  --ramdisk Resources\ramdisk `
  --header_version 0 --pagesize 4096 `
  -o Mu-peridot.img

fastboot boot Mu-peridot.img
```

## Payload volume

UniXNU reads its configuration from a FAT volume that the firmware publishes as
a block device. Getting there took three separate fixes; all three are required.

### 1. The entry-argument HOB has no producer in Mu-Silicium

`UniXNURamDiskDxeEntry` starts with

```c
Hob = GetFirstGuidHob (&mEntryArgHobGuid);   // DAEE82E6-8090-4E72-BF B9 ...
if (Hob == NULL) {
  return EFI_SUCCESS;                        // silently does nothing
}
```

Mu-Silicium ships the consumer but never creates that HOB, so the driver returns
before publishing anything. `uefi-sheng` publishes it in
`SiliciumPkg/Sec/Sec.c`. `peridot/tools/inject-entryarg-hob.ps1` adds the four
pieces: the GUID in `SiliciumPkg.dec`, `Sec.inf` `[Guids]`, saving the incoming
`x0` in `Sec/AArch64/ModuleEntryPoint.S`, and the `BuildGuidDataHob` call.

The call must run **after** `ProcessLibraryConstructorList ()`. Placed before it,
the HOB list is not ready yet and DXE dispatch breaks: the firmware then reports
`Failed to Find an Operating System!` because UniXNUDxe never registers a boot
option.

### 2. The DTB carries stale initrd addresses

`UniXNURamDiskDxe` publishes whatever `/chosen/linux,initrd-start` and `-end`
point at. The stock `Resources/DTBs/peridot.dtb` still holds the addresses from
Xiaomi's own boot image, and nothing in the Mu-Silicium path rewrites them.

The address that works is the one the boot image header implies:

```
initrd = base + ramdisk_offset = 0x10000000 + 0x01000000 = 0x11000000
```

So the DTB must be patched before it is appended to the kernel. Use
`peridot/tools/dtbinitrd.py`, which walks the FDT structure block instead of
substituting bytes, and set `start = 0x11000000`, `end = start + payload size`.
The length must be a whole number of 512-byte sectors or the driver refuses it.

### 3. The file name needs VFAT long names

`unixnu.toml` does not fit 8.3: a plain short entry truncates to `UNIXNU.TOM` and
the boot manager never matches it. The volume therefore needs long-name slots
(with the VFAT checksum over the short alias) for that file.
`peridot/tools/mkpayload.py` writes them; `peridot/tools/mkpayload2.py` is the
pyfatfs variant.

### Verified result

With all three in place the menu lists the entry defined in the payload:

```toml
version = 1
[boot]
timeout = 30
default = "xnu"
[[entry]]
id = "xnu"
title = "UniXNU"
type = "xnu"
path = "Payloads/kernel.macho"
args = "-v serial=3"
```

`no boot entry found` is gone. Selecting the entry will still fail: the kernel,
ADT and trust cache are not in the payload yet.

### Package the image

```powershell
# 16 MiB FAT volume with EFI/UniXNU/unixnu.toml
python peridot\tools\mkpayload.py peridot\out\payload.img

# patch the DTB, then build kernel = gzip(BootShim + FD) + DTB
python peridot\tools\dtbinitrd.py <stock.dtb> out.dtb 0x11000000 0x12000000

python Resources\Scripts\mkbootimg.py --kernel <kernel> --ramdisk <payload.img> `
  --header_version 0 --pagesize 4096 -o Mu-peridot.img
```
---

## Bootloader control block: clear it from the firmware

ABL starts `recovery_a` with `boot-recovery` still in the `misc` BCB, and the UEFI side never
reports completion the way a real recovery does. The consequence on hardware (2026-11-21/22) was
that every plain reboot after a UniXNU session went back to the UniXNU menu instead of Android;
the only workaround was `fastboot erase misc` from a host.

`DarwinBoot` now zeroes the command block of the GPT partition named `misc` as soon as the loader
runs (`ClearBootloaderMessage()` in `DarwinBootPkg/Driver/DarwinBoot.c`, via the `PartitionInfo`
protocol) and prints `DARWINBOOT: bcb cleared`. Entering recovery with the key combo makes ABL
write the BCB again, so the second system stays reachable.

Verified: `fastboot reboot recovery` → `bcb cleared` → XNU → long-press power → plain power on →
Android.

## Fastboot USB RX queueing

`FastbootUsbRead` used to queue the OUT transfer once and give up on `EFI_NOT_READY`, which showed
up as an endless `usb rx queue failed: Not Ready` on screen and kept host commands from reaching
`FastbootCommand`. It now aborts the endpoint, stalls 1 ms and requeues up to four times, and only
for `EFI_NOT_READY`; `DEVICE_STATE_CHANGE` aborts a leftover transfer instead of only clearing the
flag. `usbcheck=1` and `usblog=1` boot arguments dump the stack state and the Advanced Logger lines
that mention USB, so a unit whose host never enumerates can still be diagnosed from a photo.

The retry loop absorbs the error (p555: `send64=Success`), but host enumeration remains open — the
vendor stack reaches its device tree through a DAL config GUID, not `gFdtTableGuid`; see
`notes-2026-11-20-usb-dt-service-re.md` in the workspace.
