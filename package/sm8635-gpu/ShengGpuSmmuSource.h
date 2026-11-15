/* Derived from /soc/kgsl-smmu@3da0000 by scripts/prepare-sm8635-gmu.py. */
#include <stdint.h>
#define SHENG_GPU_SMMU_BASE UINT64_C(0x3da0000)
#define SHENG_GPU_SMMU_BYTES 0x40000u
#define SHENG_GPU_SMMU_GPU_STREAMS 2u
#define SHENG_GPU_SMMU_GMU_SID 5u
static const uint16_t sheng_gpu_smmu_sids[2]={0,2};
