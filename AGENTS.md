# Agent instructions

This repository owns peridot board configuration, firmware selection, payload
inputs and packaging. Shared SM8635 resources belong to `UniXNU-sm8635-common`.
Build tools and the component catalog belong to UniXNU.

Read before editing:

| Task | Required reading |
| --- | --- |
| Every port | https://unixnu.com/porting/ and https://unixnu.com/porting/device-repository |
| Hardware | https://unixnu.com/porting/device-tree and the SoC repository DTSI |
| Kernel | the selected OS profile and https://unixnu.com/xnu/source-and-patches |
| Images | https://unixnu.com/concepts/prelink and https://unixnu.com/concepts/payload |
| Verification | https://unixnu.com/building and https://unixnu.com/porting/bring-up |

Rules for this board:

1. Do not restate `[ipsw]` or `[kernel.prelink]` in `board.toml`. Both are
   inherited from the OS profile and must stay consistent with the kernelcache.
2. Do not copy sheng values. `CONFIG_XNU_SHENG_PMGR`, the sheng Data volume
   UUID, the sheng defconfig and the `xiaomi,sheng` ADT model are board
   specific and wrong here.
3. Every unverified hardware value carries a `TODO(verify)` comment naming what
   to measure. Keep that convention.
4. Update the matching `porting.toml` area with any configuration or code
   change, and move a status only when the evidence exists.
5. Keep `dts/vendor/peridot-stock.dts` as a reference. The build consumes
   `dts/peridot.dts`.

Run from the UniXNU checkout:

```sh
./unixnu status --json -D ../UniXNU-peridot
./unixnu tree -D ../UniXNU-peridot
python3 scripts/porting-record.py check --device ../UniXNU-peridot
```
