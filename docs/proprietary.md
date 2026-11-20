# Proprietary and local inputs

This repository is a public snapshot of a bring-up effort. It contains only
code and documentation written by the contributors, plus device-tree material
that is kept for reference. It deliberately does not contain Apple firmware,
IPSW images, signed trust caches, Apple-signed root hashes, cryptex metadata,
vendor GPU firmware, boot images, logs, screenshots or any local machine paths.

## Not in the repository, and why

| Input | Reason | How to supply it locally |
| --- | --- | --- |
| IPSW (`*.ipsw`) | Apple distribution | Set the IPSW in `local.toml` / pass `--ipsw` to the build tool. `tools/udif-extract.py` can read UDIF images on Linux |
| Decompressed kernelcache | Apple binary | `--kernelcache` / `local.toml`; the prelink step needs it |
| Trust cache | Apple signed | `--trust-cache` / `local.toml` |
| Data seed / device seed | Device-specific | `--data-ramdisk` and `--unique-chip-id`; see the UniXNU payload docs |
| Owner-root policies (`drivers/ShengUfs/OwnerRootPolicies.s`) | Apple-signed verified-root key and policy headers | The file in this repository is a zeroed, fail-closed stub. Regenerate it with `tools/emit-owner-policies.py` from signed headers produced from your own IPSW; keep the result out of Git |
| Cryptex graft metadata (`patches/xnu/cryptex/*.root_hash`, `*.trustcache`) | Apple signed data | Not committed. The array bytes in `patches/xnu/peridot-cryptex-graft.patch` are zeroed placeholders. Regenerate the patch with `tools/patch-cryptex-graft.py` and the files extracted from your own IPSW (`XNU_FEED` must point at the prepared XNU feed source) |
| GPU firmware (`package/sm8635-gpu/GmuFirmwareImage.c`) | Vendor firmware (`gen71100_gmu.bin`) | The committed file is a placeholder with a zero-length image. Generate the real array from your own firmware dump; the array name and byte count are the two symbols the Adreno 735 driver expects |

`patches/xnu/cryptex/` is ignored by Git (see `.gitignore`) so regenerated
metadata cannot be committed by accident.

## Local paths

All commands in the documentation use placeholders (`<unixnu-checkout>`,
`<Mu-Silicium>`, `<work>`, `<python>`) instead of the maintainer's machine
paths. Set the real values in `local.toml` (ignored by Git) or in the shell.

## Reporting

If you find a credential, personal path or proprietary blob in this repository,
please open an issue. The intended policy is that none of them are committed.
