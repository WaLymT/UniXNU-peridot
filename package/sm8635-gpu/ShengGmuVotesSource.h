/* Generated from scripts/data/sm8635-gmu-votes.json. */
#include <stdint.h>
#define SHENG_GMU_VOTES_CHIP_ID 0x43030b00u
typedef struct {uint64_t hz;uint32_t level,kbps,acd_present,acd;} ShengGmuOpp;
#define SHENG_GPU_OPPS 12u
static const ShengGmuOpp sheng_gpu_opps[]={
{0,0,0,0,0},
{255000000u,56u,2136718u,0,0},
{353000000u,64u,6074218u,0,0},
{500000000u,128u,6671875u,0,0},
{633000000u,192u,12449218u,0,0},
{684000000u,224u,12449218u,0,0},
{736000000u,256u,12449218u,0,0},
{835000000u,320u,14398437u,0,0},
{900000000u,384u,16500000u,0,0},
{950000000u,400u,16500000u,0,0},
{1000000000u,416u,16500000u,0,0},
{1100000000u,432u,16500000u,0,0},
};
#define SHENG_GMU_OPPS 3u
static const ShengGmuOpp sheng_gmu_opps[]={
{0,0,0,0,0},
{220000000u,64u,0u,0,0},
{550000000u,128u,0u,0,0},
};
#define SHENG_GPU_BANDWIDTHS 6u
static const uint32_t sheng_gpu_bandwidths[]={2136718,6074218,6671875,12449218,14398437,16500000};
static const AdrenoHfiBcm sheng_bcm_policy[3]={
{.buswidth=16,.fixed=0,.perfmode=0,.perfmode_kbps=0},
{.buswidth=4,.fixed=0,.perfmode=0,.perfmode_kbps=0},
{.buswidth=0,.fixed=1,.perfmode=4,.perfmode_kbps=10687500},
};
