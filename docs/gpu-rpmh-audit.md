# GPU / RPMh reference audit (2026-11-19)

The four device repositories below were checked because they either share the
SM8635/cliffs platform or already crossed the peridot GPU power gate. None is a
drop-in replacement for this tree; the useful part is the logic and the
counter-evidence.

## Repositories

| Repository | Why it matters |
| --- | --- |
| https://github.com/yehua123-oos/UniXNU-muyu | SM8635/cliffs; has a CPU-side RPMh ARC voter (MuyuRpmhRsc.c/h) and a per-register GPUCC cliffs map. |
| https://github.com/tattwamashi/UniXNU-braveark | SM8635, derived from uke; GPU power/SMMU/PM4 host checks. |
| https://github.com/Olegu621/xlamiOS-xnu-kernel | Peridot-specific; reports a CP_MEM_WRITE completion, PAS13 ZAP authentication and RSC/GX collapse on the actual POCO F6. |
| https://github.com/Allenwdk/UniXNU-uke | SM7675, iPadOS 27 arm64e; the validated GPU stack this project has been comparing against. |

## RPMh audit

MuyuRpmhRsc passes its host tests (MuyuRpmhRsc host tests OK) but must not be
copied into this tree:

1. It addresses the AP RSC through IO-peer window index 5. The current
   ShengGmu MMIO table has five windows (indices 0-4); AP RSC is not one of
   them. Porting the logic requires a dedicated map of 0x17a20000, not a
   sixth window in the existing table.
2. It votes on gfx.lvl and mxc.lvl. Peridot's stock DT contains both, plus
   mx.lvl; mx.lvl is a different rail and must not be substituted.
3. Its TCS assignment assumes ACTIVE TCS ids {0,1,2}. Peridot's DT
   qcom,tcs-config = <2 3 0 2 1 2 3 0 4 1> has the same shape, but the code
   should derive the active range from DRV_PRNT_CHLD_CONFIG rather than assume
   it.
4. The synchronous write helper can poll for one second per register. In a
   constructor or pre-emption-disabled context this is an unbounded stall; an
   equivalent port should use a much smaller bounded budget and return a
   status instead of spinning.
5. The cmd-db walk stops at a zero level. That is fine for the synthetic test
   table, but it is not proof that a real zero is padding; validate against the
   actual AOP snapshot before treating it as a terminator.
6. The host tests cover success, corrupt header, missing resource, timeout,
   busy TCS, version and config failures. They do not cover a real peridot
   cmd-db snapshot or a DRV2 read; those remain device tests.

Current status in this project: there is no RPMh voter. NativeAdreno735 maps
the AP RSC and reads DRV2 for diagnostics, but performs no TCS write. The p80
historical conclusion was based on DRV0 and is corrected in
docs/gpu-bring-up.md.

## GPUCC cliffs corrections

The Muyu cliffs map confirms the offsets already used by p72, and adds two
checks this project was missing:

- gcc_gpu_cfg_ahb_clk at GCC 0x71004 bit 0 is not a clk_branch; it must be
  confirmed/set before touching GPUCC. The driver now does that.
- gcc_ddrss_gpu_axi_clk is 0x7115c on cliffs, not 0x71154.
- GPUCC CX/GX GDSCR are 0x908c/0x9034; 0x9108 is a clock branch, not a GDSC.
- GMU RCGR is 0x9168 and hub RCGR 0x919c.

These values should come from the peridot DT/vendor source, never from the
SM8650/sheng table.

## Peridot-specific evidence from xlamiOS

The xlamiOS notes claim a real CP_MEM_WRITE completion, PAS13 ZAP authentication,
unsecure switch, HFI ACK/idle and GPU/GMU context shutdown on the POCO F6. That
does not prove a persistent Metal/display path, but it is evidence that the GPU
core can be owned once the power path is correct. It is a useful cross-check
against the claim that the GPU has never been touched on this board.

## Next

1. DRV2 is confirmed: p481 reports id=0x30200 v3.2, solver=0x4010100 and
   child=0x800c8104. p484 then voted gfx.lvl/mxc.lvl and returned
   vote=0/0 (TCS0). p485 continued into the GMU stage and took a kernel data
   abort, so the ARC vote alone did not make the GPU blocks safe to touch.
2. The next check is the GPUCC/CX clock path, not another RPMh copy. p487 shows
   the cx.lvl cmd-db entry with address 0 in this snapshot (db=-5), so the CX
   rail still needs a valid resource or a GPUCC clock/GDSC path.
2. If DRV2 aborts, stop the RPMh route and return to the power/mapping question.
3. Keep the GCC cfg-AHB precondition in the GPU constructor.

## 2026-11-19 p488/p489

p488 enabled the GPUCC clocks before the GMU stage and still took the same
external abort at far=0xfffffff960184000 (GMU window + 0x14000). p489 switched
the third ARC vote to mmcx.lvl; the device then showed a white screen, so that
path is not trusted. The next safe step is the GPUCC CX/GX GDSC and a far-address
triage, not more blind ARC writes.

