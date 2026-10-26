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
