# Host tools

Small host-side helpers used while building and debugging the peridot port.
None of them are invoked by the kernel build; the build steps live in
`../scripts/`.

| Tool | Purpose |
| --- | --- |
| `udif-extract.py` | Decode a UDIF (`.dmg`) disk image to its raw byte stream on Linux. Used to stage the iPadOS OS/System containers for the first UFS boot tests |
| `build-userdata-sparse.py` | Pack raw images into a sparse image for `fastboot flash userdata` (the bring-up-era path; the official install image is built by `unixnu build userdata`, which writes the SHNGROOT format) |
| `emit-owner-policies.py` | Regenerate `drivers/ShengUfs/OwnerRootPolicies.s` from signed policy headers and a public key. The signed inputs are not in the repository |
| `patch-cryptex-graft.py` | Regenerate `patches/xnu/peridot-cryptex-graft.patch` with real Apple-signed cryptex metadata. `XNU_FEED` must point at the prepared XNU feed source; the metadata files are extracted from your own IPSW and are ignored by Git |

## Not included

The signed inputs, firmware blobs and generated outputs are intentionally absent;
see [../docs/proprietary.md](../docs/proprietary.md).
