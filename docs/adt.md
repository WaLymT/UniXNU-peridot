# Generating the ADT on Windows

`kernel.macho.adt` is the Apple device tree the kernel reads. UniXNU generates it
from the hardware FDT plus the OS profile, and that pipeline runs on Windows as
long as `dtc` is available. Only the kernel build itself needs Linux.

## Inputs

| Input | Path |
| --- | --- |
| Board DTS | `dts/peridot.dts` (includes the SoC DTSI) |
| SoC DTSI | `../UniXNU-sm8635-common/dts/sm8635.dtsi` |
| OS profile | `profiles/os/ipados-17.7.11-21H461/profile.toml` |
| Board config | `board.toml` |

## dtc

MSYS2 publishes `mingw-w64-x86_64-dtc`; the binary needs the mingw runtime DLLs
next to it (`libyaml-0-2`, `libiconv-2`, `libcharset-1`, `libwinpthread-1`,
`libfdt-1`). Without them `dtc.exe` exits with `0xC0000135`
(`STATUS_DLL_NOT_FOUND`) and prints nothing at all.

## Compile and generate

```powershell
dtc -I dts -O dtb -i <sm8635-common>/dts -o hardware.dtb <device>/dts/peridot.dts

python repos/UniXNU/scripts/make-adt.py `
  --dtb hardware.dtb `
  --os-profile repos/UniXNU/profiles/os/ipados-17.7.11-21H461/profile.toml `
  --board board.toml `
  --unique-chip-id 0x1A2B3C4D5E6F7788 `
  --data-ramdisk --trust-cache `
  --output kernel.macho.adt
```

`--unique-chip-id` is the platform seed; it must come from the device at build
time and must not be reused across boards.

## Constraints the generator enforces

These are checked, not guessed; each one failed once before the tree compiled.

| Check | Requirement |
| --- | --- |
| `payload.dram_size` | must equal the memory node's **size** cell, `0x300000000` for 12 GiB at `0x80000000` |
| `payload.adt.owned_memory_size` | 256 MiB to 8 GiB, aligned to 2 MiB |
| interrupt specifiers | cell count must match the controller's `#interrupt-cells` (4 here) |
| `unixnu,adt-name` | only nodes carrying it are exported under `/arm-io` |

## Result

A successful run produces a tree with the nodes ArmDTPlatform requires:

```text
device-tree
  compatible = 'J120AP'        # OS profile identity
  model / target-type
  chosen        dram-base, dram-size, nvram proxy, unique-chip-id, chip-id,
                darwinboot-memory-size, root-matching, memory-map
                (PackedRAMDisk and TrustCache placeholders)
  cpus          one node per CPU with reg, cluster-type, timebase-frequency
  psci
  arm-io        nodes exported from the hardware tree
  defaults
  options
  product
  buttons
```

`peridot/tools/adtdump.py` prints the tree, which is the quickest way to check
that a change did what it was supposed to.
