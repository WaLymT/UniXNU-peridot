// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native sheng SM8550 UFS bring-up. Default-off: no MMIO is touched unless
// native-ufs-probe=1 is present. This is a register probe, not a storage driver.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/crypto/sha2.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
/* Newer XNU keeps the kernel memory and map protection interfaces in these headers. */
#if __has_include(<vm/vm_kern_xnu.h>)
#include <vm/vm_kern_xnu.h>
#endif
#include <mach/mach_vm.h>
#include <sys/param.h>
#include <sys/conf.h>
// DKIOCGETBLOCKSIZE / DKIOCSETBLOCKSIZE / DKIOCGETBLOCKCOUNT: the block-device
// contract spec_open() checks before it lets a bdevsw vnode open.
#include <sys/disk.h>
#include <sys/buf.h>
#include <sys/mount.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
#include <sys/proc.h>
#include <miscfs/devfs/devfs.h>
#ifndef DEVFS_BLOCK
#define DEVFS_BLOCK 1
#endif
#ifdef __cplusplus
extern "C" void buf_biodone (struct buf *);
#endif
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
#include "verified_root.h"
#include "VerifiedBlockPolicy.h"
extern "C" {
extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t verified_root_key[];
}
#endif
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSIterator.h>
#include <IOKit/IOBSD.h>
#include <kern/thread.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include "GccUfs.h"
#include "RootDiagnostics.h"
#include "TransferPoll.h"
#include "SmmuTakeover.h"

// pe_init.c (peridot-last-panic-print.patch): render the previous boot's log tail.
extern "C" void pe_panic_last_log (void);
// pe_init.c panic region state, so one boot proves whether panic-base took effect.
extern "C" vm_offset_t gPanicBase;
extern "C" unsigned int gPanicSize;

static UfsRootDiagnostics gRootDiagnostic;
static bool gRootDiagnosticEnabled;
// native-ufs-allow-unverified=1: bring-up publication without the owner policy, the same
// mode the muyu port uses. The windows are published on their own geometry and the
// publisher waits for any media below each window instead of a signed volume identity.
static bool gAllowUnverifiedRoot;
static void rootDiagnosticFailure (const char *reason);

extern "C" {
// Optional passive observer; absent in ordinary builds. It may run while a
// window lock is held, so its contract forbids blocking or transport access.
#if defined(NATIVE_UFS_SOURCE_API) && NATIVE_UFS_SOURCE_API
// The source runtime archive carries a weak no-op definition; a weak
// reference here would become an unresolved pointer-auth import instead.
void boot_progress_poll (void);
#else
void boot_progress_poll (void) __attribute__((weak));
#endif
volatile uint32_t native_ufs_probe_calls = 0;
volatile uint32_t native_ufs_matched = 0;
volatile uint32_t native_ufs_probe_requested = 0;
volatile uint32_t native_ufs_mapped = 0;
volatile uint32_t native_ufs_status = 0;
// Root start: 1 entered, 2 host up, 3 layout found; the last soft-failure reason (a static string), for the boot page.
volatile uint32_t native_ufs_root_step = 0;
const char *volatile native_ufs_root_failure = nullptr;
volatile uint32_t native_ufs_cap = 0;
volatile uint32_t native_ufs_version = 0;
volatile uint32_t native_ufs_hcs = 0;
volatile uint32_t native_ufs_hce = 0;
volatile uint32_t native_ufs_utrl_ready = 0;
volatile uint32_t native_ufs_uic_status = 0;
volatile uint32_t native_ufs_uic_result = 0;
}

class NativeUfs : public IOService {
  OSDeclareDefaultStructors (NativeUfs)
  OSSharedPtr<IOMemoryMap> fMap;
  OSSharedPtr<IOBufferMemoryDescriptor> fCmdDesc;
  void inspect (void);
  void adoptAndRead (unsigned level);
  void dumpFirmwareList (void);
  void inspectSmmu (void);
  void inspectContextBanks (void);
  bool startRoot (IOService *provider);
  static bool waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
};

OSDefineMetaClassAndStructors (NativeUfs, IOService)

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (base + offset) = value;
}

static bool contains (const char *data, size_t length, const char *needle)
{
  size_t wanted = 0;
  while (needle[wanted]) {
    wanted++;
  }
  for (size_t at = 0; at + wanted <= length; at++) {
    size_t index = 0;
    while (index < wanted && data[at + index] == needle[index]) {
      index++;
    }
    if (index == wanted) {
      return true;
    }
  }
  return false;
}

IOService *NativeUfs::probe (IOService *provider, SInt32 *score)
{
  IOLog ("NUPR\n");
  native_ufs_probe_calls = native_ufs_probe_calls + 1;
  if (OSDynamicCast (IOPlatformExpertDevice, provider)) {
    // Second personality (KernelConfigTables: IOMatchCategory NativeUfsRoot): attaches to the
    // root device, which starts before the platform expert publishes IOResources, so the
    // hardware probes run before the original AMFI (whose start panics without a PMGR).
    native_ufs_matched = 1;
    if (score) {
      *score = 500;
    }
    return this;
  }
  OSObject *property = provider->getProperty ("compatible");
  OSData *data = OSDynamicCast (OSData, property);
  OSString *string = OSDynamicCast (OSString, property);
  bool match = false;
  if (data) {
    const char *text = static_cast<const char *> (data->getBytesNoCopy ());
    match = text && contains (text, data->getLength (), "qcom,sm8550-ufshc");
  } else if (string) {
    const char *text = string->getCStringNoCopy ();
    match = text && contains (text, string->getLength (), "qcom,sm8550-ufshc");
  }
  if (!match) {
    return nullptr;
  }
  native_ufs_matched = 1;
  if (score) {
    *score = 1000;
  }
  return this;
}

bool NativeUfs::waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops)
{
  for (unsigned index = 0; index < loops; index++) {
    if ((read32 (base, offset) & mask) == mask) {
      return true;
    }
    IODelay (1);
  }
  return false;
}

// Read-only inspection (native-ufs-probe=2): reports what firmware left behind.
// The physical windows are the sheng board DT's own GCC (0x100000) and UFS host
// (0x1d84000, 0x3000) ranges. The controller is read only if the GCC says its
// GDSC is powered and the AXI/AHB branches are running; nothing is written.
static uintptr_t mapPhys (uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, length);
  if (!memory) {
    return 0;
  }
  keep = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  return keep ? static_cast<uintptr_t> (keep->getVirtualAddress ()) : 0;
}

void NativeUfs::inspect (void)
{
  OSSharedPtr<IOMemoryMap> gccMap, memPhyMap, hcMap;
  uintptr_t gcc = mapPhys (GCC_BASE + 0x77000, 0x1000, gccMap);
  uintptr_t memPhy = mapPhys (GCC_BASE + 0x9e000, 0x1000, memPhyMap);
  if (!gcc || !memPhy) {
    panic ("NATIVE_UFS inspect: GCC map failed gcc=%p memphy=%p", (void *) gcc, (void *) memPhy);
  }
  uint32_t gdsc = read32 (gcc, GCC_UFS_PHY_GDSCR - 0x77000);
  uint32_t axi = read32 (gcc, GCC_UFS_PHY_AXI_ENABLE - 0x77000);
  uint32_t ahb = read32 (gcc, GCC_UFS_PHY_AHB_ENABLE - 0x77000);
  uint32_t unipro = read32 (gcc, GCC_UFS_PHY_UNIPRO_CORE_ENABLE - 0x77000);
  uint32_t txs = read32 (gcc, GCC_UFS_PHY_TX_SYMBOL0_ENABLE - 0x77000);
  uint32_t rxs = read32 (gcc, GCC_UFS_PHY_RX_SYMBOL0_ENABLE - 0x77000);
  uint32_t aux = read32 (gcc, GCC_UFS_PHY_PHY_AUX_ENABLE - 0x77000);
  uint32_t memGdsc = read32 (memPhy, 0);
  // GDSCR bit31 PWR_ON; CBCR bit31 CLK_OFF.
  bool powered = (gdsc & 0x80000000U) != 0;
  // Hardware 2026-10-17: AXI running, AHB enabled but CLK_OFF (bit31). With bit1 (HW gating)
  // set the branch wakes on access, so enable bits, not CLK_OFF, gate the read-only probe.
  bool busClocks = !(axi & 0x80000000U) && (ahb & 0x1U) && (axi & 0x1U);
  uint32_t cap = 0, ver = 0, hcs = 0, hce = 0, utrlba = 0, is = 0;
  unsigned readHc = 0;
  if (powered && busClocks) {
    uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
    if (hc) {
      cap = read32 (hc, 0x00);
      ver = read32 (hc, 0x08);
      is = read32 (hc, 0x20);
      hcs = read32 (hc, 0x30);
      hce = read32 (hc, 0x34);
      utrlba = read32 (hc, 0x50);
      readHc = 1;
    }
  }
  panic ("NATIVE_UFS inspect\n"
         "gdsc=%08x memphygdsc=%08x\n"
         "axi=%08x ahb=%08x unipro=%08x\n"
         "txsym=%08x rxsym=%08x aux=%08x\n"
         "hc_read=%u cap=%08x ver=%08x\n"
         "is=%08x hcs=%08x hce=%08x utrlba=%08x",
         gdsc, memGdsc, axi, ahb, unipro, txs, rxs, aux, readHc, cap, ver, is, hcs, hce, utrlba);
}

// Adopt the controller the firmware left running (native-ufs-probe=3) and issue
// read-only SCSI commands through one UTP transfer request slot. Nothing is written
// to the storage. The controller is not reset and the link is not restarted; the
// firmware's request list is stopped (refused if a request is outstanding) and replaced
// by ours. Interrupts stay masked and completion is polled.
namespace {
struct UfsResult {
  uint32_t ocs;      // UTRD overall command status; 0xf = never completed
  uint32_t response; // UPIU response byte
  uint32_t scsi;     // SCSI status byte
  uint32_t sense;    // sense key<<16 | ASC<<8 | ASCQ from the response UPIU's autosense data
  uint32_t senseLen; // sense data length reported by the device
  uint32_t seg;      // UPIU data segment length
};

static void be32 (uint8_t *at, uint32_t value)
{
  at[0] = value >> 24; at[1] = value >> 16; at[2] = value >> 8; at[3] = value;
}

static uint32_t getBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}
}

static void cacheClean (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc cvac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void cacheCleanInvalidate (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// The region is mapped as device memory: only aligned element-sized stores are allowed
// (bzero's block zeroing faulted on hardware, 2026-10-17).
static void zero32 (void *start, size_t length)
{
  volatile uint32_t *word = static_cast<volatile uint32_t *> (start);
  for (size_t index = 0; index < length / 4; index++) {
    word[index] = 0;
  }
}


// Give the UFS stream a pass-through mapping the way Linux does on this firmware.
// Hardware 2026-10-17: after hand-off apps_smmu has sCR0 = 0x002f0406 (USFCFG=1, unmatched
// streams fault), no stream-match entry for the UFS host (ID 0x60, `iommus = <&apps_smmu 0x60
// 0x0>` in the board DT), a TYPE=BYPASS S2CR reads back as FAULT, and sCR0 writes are ignored.
// Linux (drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c, qcom_smmu_cfg_probe + qcom_smmu_write_s2cr,
// "bypass_quirk") meets exactly this: it reserves the LAST context bank (num_context_banks-1),
// writes SCTLR=0 (stage 1 disabled) and CBAR.TYPE=S1_TRANS_S2_BYPASS there, and maps a bypass
// stream as S2CR TYPE=TRANSLATE with CBNDX = that bank. S2CR is written before SMR
// (arm_smmu_write_sme). ARM IHI 0062D.c table 3-1 additionally requires VA64=1
// for inputs above 32 bits even with SCTLR.M=0. Storage DMA path only; this
// is physical pass-through, not a page-table or Adreno 4 KiB qualification.
extern "C" volatile uint32_t native_ufs_extra_stream;
extern "C" uint8_t *native_ufs_extra_va;
extern "C" volatile uint32_t native_ufs_extra_uncached;
extern "C" volatile uint32_t native_ufs_extra_iova, native_ufs_extra_bytes;
extern "C" volatile uint32_t native_ufs_extra_atos[8];
extern "C" volatile uint32_t native_ufs_dpu_smmu[24];
extern "C" volatile uint32_t native_ufs_dpu_state;
extern "C" volatile uint32_t native_ufs_dpu_check[8];
// The display bank's stage 1 table: L1, then L2 for the first GiB, then kDpuSlots * kDpuSlotTables L3 tables (one per 2 MiB of a slot).
constexpr uint32_t kDpuWindow = 0x10000000U, kDpuSlots = 4, kDpuSlotBytes = 32U << 20, kDpuSlotTables = kDpuSlotBytes >> 21;
static uint64_t *gDpuTables;
static uintptr_t gDpuCb;
static IOLock *gDpuLock;
static uintptr_t gDpuLiveCb;
// FSR fault bits (TF .. UUT, SS, MULTI); 0x600 is the format field.
constexpr uint32_t kDpuFaults = 0xc00001feU;
static void dpuClean (const void *start, size_t length)
{
  for (size_t off = 0; off < length; off += 64)
    __asm__ volatile ("dc civac, %0" :: "r" (static_cast<const uint8_t *> (start) + off) : "memory");
  __asm__ volatile ("dsb sy" ::: "memory");
}
static bool dpuTlbFlush (uintptr_t cb)
{
  write32 (cb, 0x618, 0); write32 (cb, 0x7f0, 0);   // TLBIALL, TLBSYNC
  __asm__ volatile ("dsb sy" ::: "memory");
  for (unsigned spin = 0; spin < 100000; ++spin) if (!(read32 (cb, 0x7f4) & 1)) return true;
  return false;
}
static void dpuEnable (uintptr_t smmu, uintptr_t cb, uint32_t bank, uint32_t oldSctlr)
{
  (void) smmu;
  const size_t bytes = (2 + kDpuSlots * kDpuSlotTables) * 4096;
  IOBufferMemoryDescriptor *tables = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone), bytes, 4096);
  gDpuLock = IOLockAlloc ();
  if (!tables || !gDpuLock) {native_ufs_dpu_state = 0xdead0002U; return;}
  uint64_t *l1 = static_cast<uint64_t *> (tables->getBytesNoCopy ()), *l2 = l1 + 512;
  const uint64_t pa = tables->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
  bzero (l1, bytes);
  // Normal non-cacheable (attribute 0 = 0x44), not shareable, access flag, readable and writable at both privilege levels.
  const uint64_t attrs = (1ULL << 10) | (1ULL << 6);
  // i564: the first fetch faulted at once with FSYNR0 level 0, so the walk may start at level 0 although T0SZ 32 means level 1. One table
  // serves every level: [0] points back at itself (level 0 -> level 1, level 1 -> level 2 for the window), [2] and [3] are the 1 GiB
  // blocks, [128 + t] the window's level 3 tables. The second page stays unused.
  (void) l2;
  l1[0] = pa | 3;
  l1[2] = 0x80000000ULL | attrs | 1;
  l1[3] = 0xc0000000ULL | attrs | 1;
  for (uint32_t t = 0; t < kDpuSlots * kDpuSlotTables; ++t) l1[(kDpuWindow >> 21) + t] = (pa + (2 + t) * 4096ULL) | 3;
  dpuClean (l1, bytes);
  const uint32_t old[5] = {read32 (cb, 0x20), read32 (cb, 0x24), read32 (cb, 0x10), read32 (cb, 0x30), read32 (cb, 0x38)};
  gDpuLiveCb = cb;
  write32 (cb, 0x20, static_cast<uint32_t> (pa)); write32 (cb, 0x24, static_cast<uint32_t> (pa >> 32));
  // TCR2 (PASIZE bits 2:0, SEP bits 17:15): 48-bit output and SEP 7 (the sign extension comes from the upstream address, as arm-smmu's AArch64 contexts set it). SEP 0 takes
  // IA[31] as the sign bit, so every address with bit 31 set but zero above it (the display's own framebuffer at 0xb90f7500: i564, and the
  // first fetch of every earlier try) is out of TTBR0's range and faults at level 0.
  write32 (cb, 0x10, 5U | (7U << 15));
  write32 (cb, 0x30, 32U | (1U << 23));               // T0SZ 32, non-cacheable walks, 4 KiB granule, no TTBR1
  write32 (cb, 0x38, 0x44);                           // MAIR attribute 0: normal non-cacheable
  __asm__ volatile ("dsb sy" ::: "memory");
  volatile uint32_t *c = native_ufs_dpu_check;
  c[0] = read32 (cb, 0x20); c[1] = read32 (cb, 0x30); c[2] = read32 (cb, 0x58);
  const auto undo = [&] (uint32_t why) {
    write32 (cb, 0x0, oldSctlr);
    __asm__ volatile ("dsb sy; isb" ::: "memory");
    write32 (cb, 0x20, old[0]); write32 (cb, 0x24, old[1]); write32 (cb, 0x10, old[2]); write32 (cb, 0x30, old[3]); write32 (cb, 0x38, old[4]);
    const uint32_t fsr = read32 (cb, 0x58);
    if (fsr & (1U << 30)) write32 (cb, 0x8, 1);        // RESUME: terminate a stalled transaction
    if (fsr & kDpuFaults) write32 (cb, 0x58, fsr);     // write one to clear
    dpuTlbFlush (cb);
    native_ufs_dpu_state = why;
  };
  if (c[0] != static_cast<uint32_t> (pa) || c[1] != (32U | (1U << 23))) {undo (0xdead0003U); return;}   // the bank ignores our writes
  if (!dpuTlbFlush (cb)) {undo (0xdead0004U); return;}
  write32 (cb, 0x0, 1U | (1U << 5) | (1U << 6));      // SCTLR: M, CFRE, CFIE; a fault terminates instead of stalling the display
  __asm__ volatile ("dsb sy; isb" ::: "memory");
  // No check translation here (i563: IDR0 bit 26 ATOSNS, the SMMU has no ATOS), so watch the display's own fetches for 50 ms (6 frames
  // at 120 Hz) and turn translation off again at the first fault.
  uint32_t fsr = 0, us = 0;
  for (; us < 50000; us += 500) {
    fsr = read32 (cb, 0x58);
    if (fsr & kDpuFaults) break;
    IODelay (500);
  }
  c[3] = fsr; c[7] = us;
  if (fsr & kDpuFaults) {
    c[4] = read32 (cb, 0x60); c[5] = read32 (cb, 0x64); c[6] = read32 (cb, 0x68);
    undo (0xdead0005U); return;
  }
  gDpuTables = l1; gDpuCb = cb;
  native_ufs_dpu_state = 0x80000000U | bank;
}
static uintptr_t gExtraSmmu, gExtraCb;
static unsigned gExtraBank;

static void smmuBypassStream (uint16_t stream, OSSharedPtr<IOMemoryMap> &keep, uint32_t *report,
                              uint64_t dma, uint64_t bytes)
{
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, keep);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  UfsSmmuPlan plan = planUfsSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });
  gRootDiagnostic.smmuId1 = read32 (smmu, 0x24);
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.smmuSlot = plan.slot;
  if (plan.error) {
    gRootDiagnostic.stage = "SMMU planning";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    gRootDiagnostic.smr = plan.smr; gRootDiagnostic.s2cr = plan.s2cr;
    rootDiagnosticFailure (plan.error);
    panic ("NATIVE_UFS smmu: %s slot=%u smr=%x s2cr=%x", plan.error, plan.slot, plan.smr, plan.s2cr);
  }
  if (plan.adopt) {
    // Nothing is written: the firmware's own mapping already passes physical
    // addresses, so the transport can use it exactly as it stands.
    report[0] = plan.slot | (plan.bank << 16);
    report[1] = read32 (smmu, 0x1000 + 4 * plan.bank);
    report[2] = read32 (smmu + (plan.pages + plan.bank) * 4096, 0);
    return;
  }
  unsigned bank = plan.bank, index = plan.slot;
  uintptr_t cb = smmu + (plan.pages + bank) * 4096;
  const char *failure = configureUfsSmmu (plan, stream,
    [smmu](unsigned offset) { return read32 (smmu, offset); },
    [smmu](unsigned offset, uint32_t value) {
      write32 (smmu, offset, value);
      __asm__ volatile ("dsb sy" ::: "memory");
    });
  uint32_t smr = read32 (smmu, 0x800 + 4 * index), s2cr = read32 (smmu, 0xc00 + 4 * index);
  uint32_t cbar = read32 (smmu, 0x1000 + 4 * bank), sctlr = read32 (cb, 0x0);
  gRootDiagnostic.smr = smr; gRootDiagnostic.s2cr = s2cr;
  gRootDiagnostic.cbar = cbar; gRootDiagnostic.sctlr = sctlr;
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.cba2r = read32 (smmu, 0x1800 + 4 * bank);
  if (failure) {
    gRootDiagnostic.stage = "SMMU readback";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    rootDiagnosticFailure (failure);
    panic ("NATIVE_UFS smmu: %s idx=%u bank=%u cba2r=%x", failure, index, bank,
           gRootDiagnostic.cba2r);
  }
  report[0] = index | (bank << 16);
  report[1] = cbar;
  report[2] = sctlr;
}

void NativeUfs::adoptAndRead (unsigned level)
{
  OSSharedPtr<IOMemoryMap> hcMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS adopt: controller map failed");
  }
  // The controller's own request list is the one region its DMA is known to reach
  // (hardware 2026-10-17: a kernel-allocated buffer at 0xf080f000 reset the system when the
  // first command was issued, and nothing below 0xc8000000 is allocatable). The firmware
  // zeroed it; use 16 KiB from UTRLBA as ordinary cached memory (the DT marks the host
  // dma-coherent). Test instrument only: the kernel may also own these pages.
  uint64_t fwList = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  if (!fwList || (fwList & 0xfff)) {
    panic ("NATIVE_UFS adopt: no usable firmware list base=%llx", fwList);
  }
  IOMemoryDescriptor *region = IOMemoryDescriptor::withPhysicalAddress (fwList, 16384, kIODirectionInOut);
  OSSharedPtr<IOMemoryMap> regionMap;
  if (region) {
    regionMap = OSSharedPtr<IOMemoryMap> (region->map (kIOMapAnywhere), OSNoRetain);
    region->release ();
  }
  if (!regionMap) {
    panic ("NATIVE_UFS adopt: firmware list map failed base=%llx", fwList);
  }
  uint8_t *virt = reinterpret_cast<uint8_t *> (regionMap->getVirtualAddress ());
  uint64_t phys = fwList;
  zero32 (virt, 16384);
  cacheClean (virt, 16384);
  // Layout: [0,4096) request list (slot 0 used), [4096,5120) command + response UPIU +
  // PRDT, [8192,16384) data.
  const uint32_t ucdOffset = 4096, rspOffset = 512, prdtOffset = 1024, dataOffset = 8192;
  uint32_t hcsBefore = read32 (hc, 0x30);
  uint32_t doorbellBefore = read32 (hc, 0x58);
  if (doorbellBefore != 0 || read32 (hc, 0x78) != 0) {
    panic ("NATIVE_UFS adopt: firmware request outstanding doorbell=%08x hcs=%08x", doorbellBefore, hcsBefore);
  }
  OSSharedPtr<IOMemoryMap> smmuMap;
  uint32_t smmuReport[3] = { 0, 0, 0 };
  write32 (hc, 0x24, 0);            // mask interrupts
  write32 (hc, 0x60, 0);            // stop the firmware's request list
  __asm__ volatile ("dsb sy" ::: "memory");
  unsigned stopLoops = 10000;
  while ((read32 (hc, 0x60) & 1) && stopLoops--) IODelay (10);
  if (read32 (hc, 0x60) & 1) panic ("NATIVE_UFS adopt: request list did not stop");
  smmuBypassStream (0x60, smmuMap, smmuReport, phys, 16384);
  write32 (hc, 0x50, static_cast<uint32_t> (phys));
  write32 (hc, 0x54, static_cast<uint32_t> (phys >> 32));
  write32 (hc, 0x20, 0xffffffffU);  // clear stale status
  write32 (hc, 0x60, 1);
  if (!waitSet (hc, 0x30, 0x2, 100000)) {
    panic ("NATIVE_UFS adopt: request list not ready hcs=%08x", read32 (hc, 0x30));
  }
  if (level == 3) {
    panic ("NATIVE_UFS adopt: list replaced\n"
           "phys=%llx hcs=%08x->%08x\n"
           "is=%08x db=%08x rsr=%08x ba=%08x bau=%08x ie=%08x",
           phys, hcsBefore, read32 (hc, 0x30), read32 (hc, 0x20), read32 (hc, 0x58),
           read32 (hc, 0x60), read32 (hc, 0x50), read32 (hc, 0x54), read32 (hc, 0x24));
  }

  auto command = [&] (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t length) -> UfsResult {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    zero32 (virt + dataOffset, 8192);
    ucd[0] = 0x01;                  // COMMAND UPIU
    ucd[1] = toHost ? 0x40 : 0x00;  // flags: read
    ucd[2] = lun;
    ucd[3] = 0;                     // task tag
    be32 (ucd + 12, length);        // expected data transfer length
    for (unsigned index = 0; index < cdbLength; index++) {
      ucd[16 + index] = cdb[index];
    }
    uint64_t ucdPhys = phys + ucdOffset;
    uint32_t direction = length ? (toHost ? 2U : 1U) : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x10000000U | (direction << 25);
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;     // OCS: not completed
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = (512U >> 2) | ((rspOffset >> 2) << 16);   // response length | offset (dwords)
    uint32_t prdEntries = length ? 1U : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 28) = prdEntries | ((prdtOffset >> 2) << 16);
    if (length) {
      uint64_t dataPhys = phys + dataOffset;
      uint8_t *prd = ucd + prdtOffset;
      *reinterpret_cast<uint32_t *> (prd + 0) = static_cast<uint32_t> (dataPhys);
      *reinterpret_cast<uint32_t *> (prd + 4) = static_cast<uint32_t> (dataPhys >> 32);
      *reinterpret_cast<uint32_t *> (prd + 12) = length - 1;
    }
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    write32 (hc, 0x58, 1);          // ring slot 0
    UfsResult result = { 0xf, 0xff, 0xff, 0, 0, 0 };
    bool done = false;
    for (unsigned loops = 0; loops < 100000; loops++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return result;
    }
    result.ocs = *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
    const uint8_t *rsp = ucd + rspOffset;
    result.response = rsp[6];
    result.scsi = rsp[7];
    // Response UPIU: 12-byte header, residual count, 16 reserved bytes, then the data segment
    // (2-byte sense length + sense data) at offset 32.
    result.seg = (uint32_t (rsp[10]) << 8) | rsp[11];
    result.senseLen = (uint32_t (rsp[32]) << 8) | rsp[33];
    result.sense = ((rsp[36] & 0xfU) << 16) | (uint32_t (rsp[46]) << 8) | rsp[47];
    return result;
  };

  // NOP OUT (kind 0) and Query read requests (kind 1: opcode 0x05 READ FLAG, 0x03 READ ATTRIBUTE).
  // Read-only: no data segment, no state change on the device. Returns the UPIU response byte and
  // the 32-bit value field (attributes) or flag byte via *value; ocs 0xf means no completion.
  auto query = [&] (unsigned kind, uint8_t opcode, uint8_t idn, uint32_t *value, uint32_t *respByte) -> uint32_t {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    if (kind == 0) {
      ucd[0] = 0x00;                // NOP OUT
    } else {
      ucd[0] = 0x16;                // QUERY REQUEST
      ucd[5] = 0x01;                // query function (header dword 1, byte 1): standard read request
      ucd[12] = opcode;
      ucd[13] = idn;
    }
    ucd[3] = 1;                     // task tag
    uint64_t ucdPhys = phys + ucdOffset;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x10000000U;
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = (512U >> 2) | ((rspOffset >> 2) << 16);
    *reinterpret_cast<uint32_t *> (utrd + 28) = (prdtOffset >> 2) << 16;
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    write32 (hc, 0x58, 1);
    bool done = false;
    for (unsigned loops = 0; loops < 100000; loops++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return 0xf;
    }
    const uint8_t *rsp = ucd + rspOffset;
    *respByte = (uint32_t (rsp[0]) << 8) | rsp[6];     // response type << 8 | query/NOP response
    *value = getBe32 (rsp + 20);
    return *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
  };
  // Device state before waking it. Hardware 2026-10-17: NOP OUT answers, fDeviceInit = 0 (done),
  // bCurrentPowerMode = 0x33 (UFS-PowerDown) and every LU returns NOT READY. Linux resumes the
  // device with START STOP UNIT (power condition ACTIVE) to the UFS Device well-known LU 0xD0
  // (ufshcd_set_dev_pwr_mode); no user data is touched.
  uint32_t powerBefore = 0, powerAfter = 0, queryResp = 0, ignoredValue = 0;
  uint32_t wakeStatus = 0xee;
  if (level == 5 || level == 9) {
    query (1, 0x03, 0x02, &powerBefore, &queryResp);     // READ ATTRIBUTE bCurrentPowerMode
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };          // START STOP UNIT, power condition ACTIVE
    UfsResult w = command (0xd0, ssu, 6, false, 0);
    wakeStatus = (w.ocs << 16) | (w.response << 8) | w.scsi;
    for (unsigned settle = 0; settle < 50; settle++) {
      IODelay (1000);
    }
    query (1, 0x03, 0x02, &powerAfter, &queryResp);
    (void) ignoredValue;
  }
  if (level == 4) {
    uint8_t tur0[6] = { 0, 0, 0, 0, 0, 0 };
    UfsResult t = command (0, tur0, 6, false, 0);
    const uint8_t *rsp0 = virt + ucdOffset + rspOffset;
    panic ("NATIVE_UFS adopt: TEST UNIT READY lun0\n"
           "ocs=%x resp=%02x scsi=%02x\n"
           "phys=%llx hcs=%08x->%08x smmu idx/bank=%08x cbar=%08x\n"
           "is=%08x db=%08x rsr=%08x\n"
           "rsp=%08x %08x",
           t.ocs, t.response, t.scsi, phys, hcsBefore, read32 (hc, 0x30), smmuReport[0], smmuReport[1],
           read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), getBe32 (rsp0), getBe32 (rsp0 + 4));
  }
  uint32_t hcsAfter = 0;
  const unsigned maxLun = 6;
  uint32_t status[maxLun], last[maxLun], blockSize[maxLun], senseOf[maxLun];
  for (unsigned lun = 0; lun < maxLun; lun++) {
    status[lun] = 0xee; last[lun] = 0; blockSize[lun] = 0; senseOf[lun] = 0;
    uint8_t tur[6] = { 0x00, 0, 0, 0, 0, 0 };
    UfsResult r = { 0xf, 0xff, 0xff, 0, 0, 0 };
    for (unsigned attempt = 0; attempt < 3; attempt++) {
      r = command (static_cast<uint8_t> (lun), tur, 6, false, 0);
      if (lun == 0 && attempt == 0 && r.ocs == 0xf) {
        // Never completed: stop here and report instead of waiting out every command.
        const uint8_t *rsp = virt + ucdOffset + rspOffset;
        panic ("NATIVE_UFS adopt: first command did not complete\n"
               "phys=%llx hcs=%08x->%08x\n"
               "is=%08x db=%08x rsr=%08x ba=%08x\n"
               "utrd2=%08x rsp=%08x %08x",
               phys, hcsBefore, read32 (hc, 0x30),
               read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), read32 (hc, 0x50),
               *reinterpret_cast<volatile uint32_t *> (virt + 8), getBe32 (rsp), getBe32 (rsp + 4));
      }
      if (r.ocs == 0 && r.scsi == 0) {
        break;
      }
    }
    uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    r = command (static_cast<uint8_t> (lun), cap, 10, true, 8);
    status[lun] = (r.ocs << 16) | (r.response << 8) | r.scsi;
    senseOf[lun] = r.sense;
    if (r.ocs == 0 && r.scsi == 0) {
      last[lun] = getBe32 (virt + dataOffset);
      blockSize[lun] = getBe32 (virt + dataOffset + 4);
    }
  }
  if (level == 9) {
    // Read-only GPT survey of LUN 0 (4096-byte blocks): header fields and CRCs for before/after
    // comparison, then every non-empty entry among the first 64. Nothing is written.
    if (blockSize[0] != 4096) {
      panic ("NATIVE_UFS gpt: unexpected LUN0 block size %08x", blockSize[0]);
    }
    uint8_t rd[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult h = command (0, rd, 10, true, 4096);
    if (h.ocs != 0 || h.scsi != 0) {
      panic ("NATIVE_UFS gpt: header read failed ocs=%x resp=%x scsi=%x", h.ocs, h.response, h.scsi);
    }
    const uint8_t *hdr = virt + dataOffset;
    auto le32 = [] (const uint8_t *at) -> uint32_t {
      return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
    };
    auto le64 = [&] (const uint8_t *at) -> uint64_t {
      return uint64_t (le32 (at)) | (uint64_t (le32 (at + 4)) << 32);
    };
    uint32_t sig0 = le32 (hdr), sig1 = le32 (hdr + 4), hdrCrc = le32 (hdr + 16), entCrc = le32 (hdr + 88);
    uint64_t curLba = le64 (hdr + 24), bakLba = le64 (hdr + 32), firstUsable = le64 (hdr + 40), lastUsable = le64 (hdr + 48);
    uint64_t entLba = le64 (hdr + 72);
    uint32_t entCount = le32 (hdr + 80), entSize = le32 (hdr + 84);
    uint32_t guid0 = le32 (hdr + 56), guid1 = le32 (hdr + 60), guid2 = le32 (hdr + 64), guid3 = le32 (hdr + 68);
    if (sig0 != 0x20494645U || sig1 != 0x54524150U || entSize != 128 || entLba > 64) {
      panic ("NATIVE_UFS gpt: unexpected header sig=%08x %08x esz=%u elba=%llx", sig0, sig1, entSize, entLba);
    }
    uint32_t shown = entCount > 64 ? 64 : entCount;
    uint32_t blocks = (shown * 128 + 4095) / 4096;
    uint8_t rd2[10] = { 0x28, 0, 0, 0, 0, uint8_t (entLba), 0, 0, uint8_t (blocks), 0 };
    UfsResult e = command (0, rd2, 10, true, blocks * 4096);
    if (e.ocs != 0 || e.scsi != 0) {
      panic ("NATIVE_UFS gpt: entries read failed ocs=%x scsi=%x", e.ocs, e.scsi);
    }
    const uint8_t *ents = virt + dataOffset;
    static char text[3400];
    int used = snprintf (text, sizeof (text),
                         "NATIVE_UFS gpt LUN0 hdr crc=%08x ents crc=%08x cnt=%u\n"
                         "cur=%llx backup=%llx usable=%llx..%llx elba=%llx\n"
                         "disk guid %08x %08x %08x %08x last=%08x\n",
                         hdrCrc, entCrc, entCount, curLba, bakLba, firstUsable, lastUsable, entLba,
                         guid0, guid1, guid2, guid3, last[0]);
    unsigned column = 0, listed = 0;
    for (uint32_t index = 0; index < shown && used < int (sizeof (text)) - 80; index++) {
      const uint8_t *entry = ents + index * 128;
      bool empty = true;
      for (unsigned b = 0; b < 16; b++) {
        if (entry[b]) { empty = false; break; }
      }
      if (empty) {
        continue;
      }
      char name[14];
      for (unsigned c = 0; c < 13; c++) {
        uint8_t lo = entry[56 + 2 * c], hi = entry[57 + 2 * c];
        name[c] = (hi == 0 && lo >= 32 && lo < 127) ? char (lo) : (lo ? '?' : 0);
        if (!name[c]) { name[c] = 0; break; }
      }
      name[13] = 0;
      used += snprintf (text + used, sizeof (text) - used, "%u:%s %llx-%llx%s", index, name,
                        le64 (entry + 32), le64 (entry + 40), (++column % 3) ? "  " : "\n");
      listed++;
    }
    snprintf (text + used, sizeof (text) - used, "\nlisted=%u", listed);
    panic ("%s", text);
  }
  // INQUIRY of LUN 0: vendor/product bytes 8..23.
  uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
  UfsResult inq = command (0, inquiry, 6, true, 36);
  uint32_t inqWord[4] = { 0, 0, 0, 0 };
  for (unsigned index = 0; index < 4; index++) {
    inqWord[index] = getBe32 (virt + dataOffset + 8 + index * 4);
  }
  // LBA 1 of LUN 0 holds the GPT header when the block size is 4096 (or 512).
  uint32_t gptStatus = 0xee, sig0 = 0, sig1 = 0;
  if (blockSize[0] >= 512 && blockSize[0] <= 4096) {
    uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult g = command (0, read10, 10, true, blockSize[0]);
    gptStatus = (g.ocs << 16) | (g.response << 8) | g.scsi;
    if (g.ocs == 0 && g.scsi == 0) {
      sig0 = *reinterpret_cast<uint32_t *> (virt + dataOffset);
      sig1 = *reinterpret_cast<uint32_t *> (virt + dataOffset + 4);
    }
  }
  hcsAfter = read32 (hc, 0x30);
  panic ("NATIVE_UFS adopt+read\n"
         "hcs %08x->%08x pwr %08x->%08x ssu=%06x\n"
         "L0 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L1 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L2 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L3 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L4 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L5 st=%06x last=%08x bs=%08x sns=%06x\n"
         "inq st=%06x %08x %08x %08x %08x\n"
         "inq sns=%06x slen=%x seg=%x\n"
         "rsp0 %08x %08x %08x %08x\n"
         "rsp8 %08x %08x %08x %08x\n"
         "gpt st=%06x sig=%08x %08x",
         hcsBefore, hcsAfter, powerBefore, powerAfter, wakeStatus,
         status[0], last[0], blockSize[0], senseOf[0], status[1], last[1], blockSize[1], senseOf[1],
         status[2], last[2], blockSize[2], senseOf[2], status[3], last[3], blockSize[3], senseOf[3],
         status[4], last[4], blockSize[4], senseOf[4], status[5], last[5], blockSize[5], senseOf[5],
         (inq.ocs << 16) | (inq.response << 8) | inq.scsi, inqWord[0], inqWord[1], inqWord[2], inqWord[3],
         inq.sense, inq.senseLen, inq.seg,
         getBe32 (virt + ucdOffset + rspOffset), getBe32 (virt + ucdOffset + rspOffset + 4),
         getBe32 (virt + ucdOffset + rspOffset + 8), getBe32 (virt + ucdOffset + rspOffset + 12),
         getBe32 (virt + ucdOffset + rspOffset + 32), getBe32 (virt + ucdOffset + rspOffset + 36),
         getBe32 (virt + ucdOffset + rspOffset + 40), getBe32 (virt + ucdOffset + rspOffset + 44),
         gptStatus, sig0, sig1);
}

// Read-only dump of the request list the firmware left (native-ufs-probe=6): shows where
// the firmware placed its descriptors, to compare with an allocation the controller's DMA
// is known to reach. Nothing is written and no register is touched except reads.
static uint32_t dumpWord (const volatile uint32_t *base, unsigned index)
{
  return base[index];
}

void NativeUfs::dumpFirmwareList (void)
{
  OSSharedPtr<IOMemoryMap> hcMap, listMap, ucdMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS dump: controller map failed");
  }
  uint64_t listPhys = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  uint32_t db = read32 (hc, 0x58), rsr = read32 (hc, 0x60), hcs = read32 (hc, 0x30);
  IOMemoryDescriptor *listDesc = IOMemoryDescriptor::withPhysicalAddress (listPhys, 4096, kIODirectionIn);
  if (!listDesc) {
    panic ("NATIVE_UFS dump: list descriptor failed phys=%llx", listPhys);
  }
  listMap = OSSharedPtr<IOMemoryMap> (listDesc->map (kIOMapAnywhere), OSNoRetain);
  listDesc->release ();
  if (!listMap) {
    panic ("NATIVE_UFS dump: list map failed phys=%llx", listPhys);
  }
  const volatile uint32_t *utrd = reinterpret_cast<const volatile uint32_t *> (listMap->getVirtualAddress ());
  uint64_t ucdPhys = (uint64_t (dumpWord (utrd, 5)) << 32) | dumpWord (utrd, 4);
  uint32_t ucd0 = 0, ucd1 = 0, ucd2 = 0, ucd3 = 0;
  if (ucdPhys) {
    IOMemoryDescriptor *ucdDesc = IOMemoryDescriptor::withPhysicalAddress (ucdPhys, 4096, kIODirectionIn);
    if (ucdDesc) {
      ucdMap = OSSharedPtr<IOMemoryMap> (ucdDesc->map (kIOMapAnywhere), OSNoRetain);
      ucdDesc->release ();
      if (ucdMap) {
        const volatile uint32_t *ucd = reinterpret_cast<const volatile uint32_t *> (ucdMap->getVirtualAddress ());
        ucd0 = dumpWord (ucd, 0); ucd1 = dumpWord (ucd, 1); ucd2 = dumpWord (ucd, 2); ucd3 = dumpWord (ucd, 3);
      }
    }
  }
  panic ("NATIVE_UFS dump: firmware list\n"
         "list=%llx hcs=%08x db=%08x rsr=%08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd1 %08x %08x %08x %08x\n"
         "ucd=%llx %08x %08x %08x %08x",
         listPhys, hcs, db, rsr,
         dumpWord (utrd, 0), dumpWord (utrd, 1), dumpWord (utrd, 2), dumpWord (utrd, 3),
         dumpWord (utrd, 4), dumpWord (utrd, 5), dumpWord (utrd, 6), dumpWord (utrd, 7),
         dumpWord (utrd, 8), dumpWord (utrd, 9), dumpWord (utrd, 10), dumpWord (utrd, 11),
         ucdPhys, ucd0, ucd1, ucd2, ucd3);
}

// Read-only dump of the apps SMMU (apps_smmu@15000000) global register space and the stream
// match entry for the UFS stream ID 0x60 (native-ufs-probe=7). SMMUv2 layout: sCR0 0x0,
// IDR0 0x20, sGFSR 0x48, SMR(n) 0x800+4n, S2CR(n) 0xc00+4n. Nothing is written.
void NativeUfs::inspectSmmu (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0x1000, map);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  uint32_t scr0 = read32 (smmu, 0x00), idr0 = read32 (smmu, 0x20), idr1 = read32 (smmu, 0x24);
  uint32_t gfsr = read32 (smmu, 0x48);
  unsigned count = idr0 & 0xff;
  if (count > 128) {
    count = 128;
  }
  unsigned match = 0xffff, valid = 0, typeCount[4] = { 0, 0, 0, 0 };
  uint32_t matchSmr = 0, matchS2cr = 0;
  for (unsigned index = 0; index < count; index++) {
    uint32_t smr = read32 (smmu, 0x800 + 4 * index);
    uint32_t s2cr = read32 (smmu, 0xc00 + 4 * index);
    if (smr & 0x80000000U) {
      valid++;
      typeCount[(s2cr >> 16) & 3]++;
      uint32_t mask = (smr >> 16) & 0x7fff;
      if (match == 0xffff && (((smr ^ 0x60U) & 0x7fffU) & ~mask) == 0) {
        match = index;
        matchSmr = smr;
        matchS2cr = s2cr;
      }
    }
  }
  unsigned matched = (match == 0xffffU) ? 0U : (matchS2cr & 0xffU);
  unsigned pages = 1U << (((idr1 >> 28) & 7) + 1);
  uint32_t matchedCbar = read32 (smmu, 0x1000 + 4 * matched);
  uint32_t matchedSctlr = read32 (smmu + (pages + matched) * 4096, 0);
  (void)gfsr; (void)valid; (void)typeCount; (void)scr0; (void)matchSmr;
  panic ("NATIVE_UFS bank=%u cbar=%x sctlr=%x idr1=%x",
         matched, matchedCbar, matchedSctlr, idr1);
}

// Read-only context-bank survey (native-ufs-probe=8): CBAR/CBA2R in gr1 and SCTLR/TTBR0 in the
// context banks, to find a bank the hypervisor leaves to us. Nothing is written.
void NativeUfs::inspectContextBanks (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, map);
  if (!smmu) {
    panic ("NATIVE_UFS cb: map failed");
  }
  uint32_t idr1 = read32 (smmu, 0x24);
  unsigned numCb = idr1 & 0xff;
  unsigned globalPages = 1U << (((idr1 >> 28) & 7) + 1);
  if (globalPages > 128 || numCb > 128 || (globalPages + numCb) * 0x1000 > 0xd0000) {
    panic ("NATIVE_UFS cb: unexpected geometry idr1=%08x", idr1);
  }
  unsigned cbarZero = 0, sctlrZero = 0, mSet = 0, bothZero = 0;
  uint32_t firstZero = 0xffff;
  for (unsigned index = 0; index < numCb; index++) {
    uint32_t cbar = read32 (smmu, 0x1000 + 4 * index);
    uint32_t sctlr = read32 (smmu + (globalPages + index) * 0x1000, 0x0);
    cbarZero += cbar == 0;
    sctlrZero += sctlr == 0;
    mSet += (sctlr & 1) != 0;
    if (cbar == 0 && sctlr == 0) {
      bothZero++;
      if (firstZero == 0xffff) {
        firstZero = index;
      }
    }
  }
  uintptr_t cb0 = smmu + globalPages * 0x1000, cb1 = cb0 + 0x1000, cbLast = cb0 + (numCb - 1) * 0x1000;
  panic ("NATIVE_UFS cb survey\n"
         "ncb=%u pages=%u cbar0=%u sctlr0=%u msset=%u both0=%u first0=%x\n"
         "cbar[0]=%08x cbar[1]=%08x cbar[%u]=%08x\n"
         "sctlr[0]=%08x sctlr[1]=%08x sctlr[last]=%08x\n"
         "cba2r[0]=%08x cba2r[1]=%08x",
         numCb, globalPages, cbarZero, sctlrZero, mSet, bothZero, firstZero,
         read32 (smmu, 0x1000), read32 (smmu, 0x1004), numCb - 1, read32 (smmu, 0x1000 + 4 * (numCb - 1)),
         read32 (cb0, 0), read32 (cb1, 0), read32 (cbLast, 0),
         read32 (smmu, 0x1800), read32 (smmu, 0x1804));
}

// ======================================================================================
// Root media: the live controller as a read-only block device.
//
// One shared host (a single UTP request slot, polled; interrupts stay masked) serves every
// block device. The DMA region is ordinary cached, physically contiguous kernel memory with
// explicit cache maintenance around each command (the stream is mapped through the reserved
// apps_smmu bypass context bank by smmuBypassStream, so physical addresses are used as-is).
// Nothing here writes to the storage: only INQUIRY-class and READ(10) commands are built, plus
// the single START STOP UNIT that wakes the device from the power-down the firmware left.
// ======================================================================================
namespace {
#include "RootTransportLayout.h"

struct UfsHost {
  OSSharedPtr<IOMemoryMap> hcMap, smmuMap;
  uintptr_t hc;
  IOBufferMemoryDescriptor *dma;
  uint8_t *virt;
  uint64_t phys;
  IOLock *lock;
  bool ready;
  bool poisoned = false;
  uintptr_t smmu = 0, cb = 0;
};
UfsHost gHost = { OSSharedPtr<IOMemoryMap> (), OSSharedPtr<IOMemoryMap> (), 0, nullptr, nullptr, 0, nullptr, false };

static IOReturn recordTransport (IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  gRootDiagnostic.ocs = ocs;
  if (result != kIOReturnSuccess && gRootDiagnosticEnabled) {
    // Read only the already mapped HC's status/registers used by this driver.
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
    gRootDiagnostic.doorbell = read32 (gHost.hc, 0x58);
    gRootDiagnostic.interrupt = read32 (gHost.hc, 0x20);
    gRootDiagnostic.list = uint64_t (read32 (gHost.hc, 0x50)) |
                          (uint64_t (read32 (gHost.hc, 0x54)) << 32);
    gRootDiagnostic.runStop = read32 (gHost.hc, 0x60);
    gRootDiagnostic.ahit = read32 (gHost.hc, 0x18);
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32 (gHost.hc, 0x38 + 4 * i);
    if (gHost.smmu && gHost.cb) {
      gRootDiagnostic.gfsr = read32 (gHost.smmu, 0x48);
      gRootDiagnostic.gsyn = read32 (gHost.smmu, 0x54);
      gRootDiagnostic.fsr = read32 (gHost.cb, 0x58);
      gRootDiagnostic.fsyn = read32 (gHost.cb, 0x68);
      gRootDiagnostic.far = uint64_t (read32 (gHost.cb, 0x60)) |
                            (uint64_t (read32 (gHost.cb, 0x64)) << 32);
    }
  }
  return result;
}

static void cacheCivac (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Device-owned cache lines must not be written back over DMA results. All
// callers use complete 64-byte lines from the dedicated page-aligned buffer.
static void cacheInvalidate (const void *start, size_t length)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  uintptr_t address = reinterpret_cast<uintptr_t> (start);
  for (uintptr_t end = address + length; address < end; address += 64) {
    __asm__ volatile ("dc ivac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void hostWrite (unsigned offset, uint32_t value)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gHost.hc, offset, value);
  __asm__ volatile ("dsb sy" ::: "memory");
}

#include "RootQueue.h"

#include "RootCommands.h"

// Bring the live controller up for reading: map, SMMU pass-through for the UFS stream, take over
// the request list, wake the device from UFS-PowerDown and wait for LUN 0 to be ready.
// `why` receives a short reason on failure.
static bool hostBringUp (const char **why)
{
  if (gHost.poisoned) {
    *why = "transport quarantined";
    return false;
  }
  if (gHost.ready) {
    return true;
  }
  gRootDiagnostic.stage = "controller map";
  gHost.hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, gHost.hcMap);
  if (!gHost.hc) {
    *why = "controller map";
    return false;
  }
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.cap = read32 (gHost.hc, 0x00);
    gRootDiagnostic.version = read32 (gHost.hc, 0x08);
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
  }
  gRootDiagnostic.stage = "DMA allocation";
  gHost.dma = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone),
    kDmaBytes, 4096);
  gHost.lock = IOLockAlloc ();
  if (!gHost.dma || !gHost.lock) {
    *why = "dma buffer";
    return false;
  }
  gHost.virt = static_cast<uint8_t *> (gHost.dma->getBytesNoCopy ());
  IOByteCount contiguous = 0;
  gHost.phys = gHost.dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  gRootDiagnostic.dma = gHost.phys;
  gRootDiagnostic.contiguous = contiguous;
  if (!gHost.virt || !gHost.phys || (gHost.phys & 4095) || contiguous < kDmaBytes ||
      (!(read32 (gHost.hc, 0) & 0x01000000) && gHost.phys + kDmaBytes > 0x100000000ULL)) {
    *why = "DMA address/contiguity";
    return false;
  }
  bzero (gHost.virt, kDmaBytes);
  cacheCivac (gHost.virt, kDmaBytes);
  uint32_t report[3] = { 0, 0, 0 };
  gRootDiagnostic.stage = "SMMU and request list";
  if (!hostStopQueue (why)) return false;
  smmuBypassStream (0x60, gHost.smmuMap, report, gHost.phys, kDmaBytes);
  gHost.smmu = gHost.smmuMap->getVirtualAddress ();
  // The board's WiFi endpoint (sheng-wifi=2 or more) needs a pass-through stream of its own, set up here with the same code and at the same moment as the
  // storage stream: the registers of the stream matching group are not safe to touch from another thread later (a read of the first one never returned).
  uint32_t wifi = 0;
  if (PE_parse_boot_argn ("sheng-wifi", &wifi, sizeof (wifi)) && wifi >= 2) {
    static OSSharedPtr<IOMemoryMap> extraMap;   // kept: the fault registers are read from it later
    uint32_t extraReport[3] = { 0, 0, 0 };
    smmuBypassStream (0x1401, extraMap, extraReport, 0x9c900000ULL, 8ULL << 20);
    // The endpoint only produces 32-bit addresses and the memory here is above 4 GiB: give its context bank a stage 1 table (4 KiB granule,
    // 32-bit input, L1 -> L2 with 2 MiB blocks) that maps kExtraIova upward onto one contiguous buffer. Own bank only: refused when it is the storage's.
    constexpr uint32_t kExtraIova = 0x10000000U, kExtraBlocks = 24, kBlock = 2U << 20;
    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithOptions (
      kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
      static_cast<IOOptionBits> (kIOMemoryMapperNone) , (kExtraBlocks + 1) * kBlock, kBlock);
    IOBufferMemoryDescriptor *tables = IOBufferMemoryDescriptor::inTaskWithOptions (
      kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
      static_cast<IOOptionBits> (kIOMemoryMapperNone), 8192, 4096);
    if (buffer && tables && (extraReport[0] >> 16) != (report[0] >> 16)) {
      uint8_t *bva = static_cast<uint8_t *> (buffer->getBytesNoCopy ());
      uint64_t bpa = buffer->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
      uint64_t shift = (kBlock - (bpa & (kBlock - 1))) & (kBlock - 1);
      bva += shift; bpa += shift;
      bzero (buffer->getBytesNoCopy (), (kExtraBlocks + 1) * kBlock);
      uint64_t *l1 = static_cast<uint64_t *> (tables->getBytesNoCopy ());
      uint64_t *l2 = l1 + 512;
      bzero (l1, 8192);
      const uint64_t tablesPa = tables->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
      l1[0] = (tablesPa + 4096) | 3;
      uint32_t attr = 0;
      PE_parse_boot_argn ("sheng-wifi-attr", &attr, sizeof (attr));   // 1: normal non-cacheable, non-shareable memory for the endpoint's accesses
      const uint64_t share = attr == 1 ? 0 : (3ULL << 8);
      for (uint32_t i = 0; i < kExtraBlocks; ++i)
        l2[(kExtraIova >> 21) + i] = (bpa + i * kBlock) | (1ULL << 11) | (1ULL << 10) | share | (1ULL << 6) | 1;   // block, non-global, AF, inner shareable, readable and writable by unprivileged requesters, attribute index 0
      for (uint32_t off = 0; off < 8192; off += 64) __asm__ volatile ("dc civac, %0" :: "r" (reinterpret_cast<uint8_t *> (l1) + off) : "memory");
      __asm__ volatile ("dsb sy" ::: "memory");
      uintptr_t smmuBase = extraMap->getVirtualAddress ();
      unsigned pages = 1U << (((read32 (smmuBase, 0x24) >> 28) & 7) + 1);
      uintptr_t cb = smmuBase + (pages + (extraReport[0] >> 16)) * 4096;
      write32 (cb, 0x0, 0);                               // stage 1 off while the table is set
      write32 (cb, 0x20, static_cast<uint32_t> (tablesPa)); write32 (cb, 0x24, static_cast<uint32_t> (tablesPa >> 32));
      write32 (cb, 0x10, 5);                              // TCR2: 48-bit output
      write32 (cb, 0x30, 32U | (1U << 23));               // T0SZ 32, non-cacheable walks, 4 KiB granule, no TTBR1
      write32 (cb, 0x38, attr == 1 ? 0x44 : 0xff);        // MAIR attribute 0: write-back (or non-cacheable)
      write32 (cb, 0x618, 0); write32 (cb, 0x7f0, 0);     // TLB invalidate all, sync
      __asm__ volatile ("dsb sy" ::: "memory");
      write32 (cb, 0x0, 1U | (1U << 5) | (1U << 6));      // SCTLR: M, CFRE, CFIE
      __asm__ volatile ("dsb sy; isb" ::: "memory");
      gExtraSmmu = smmuBase; gExtraCb = cb; gExtraBank = extraReport[0] >> 16;
      // Ask the context bank to translate every page of the window (stage 1, privileged read): count the pages that fault or land elsewhere than the table says.
      {
        uint32_t tested = 0, faults = 0, wrong = 0, first = 0;
        for (uint64_t iova = kExtraIova; iova < kExtraIova + static_cast<uint64_t> (kExtraBlocks) * kBlock; iova += 4096) {
          write32 (cb, 0x800, static_cast<uint32_t> (iova)); write32 (cb, 0x804, 0);
          uint32_t spin = 0;
          while ((read32 (cb, 0x8f0) & 1) && ++spin < 100000) { }
          if (spin >= 100000) { native_ufs_extra_atos[3] = 0xdead0000U | tested; break; }
          const uint32_t parLo = read32 (cb, 0x50), parHi = read32 (cb, 0x54);
          ++tested;
          if (tested == 1) { native_ufs_extra_atos[4] = parLo; native_ufs_extra_atos[5] = parHi; native_ufs_extra_atos[6] = static_cast<uint32_t> (bpa); native_ufs_extra_atos[7] = static_cast<uint32_t> (bpa >> 32); }
          if (parLo & 1) { if (!faults++) first = static_cast<uint32_t> (iova); continue; }
          const uint64_t pa = ((static_cast<uint64_t> (parHi) << 32) | parLo) & 0x0000fffffffff000ULL;
          if (pa != ((bpa + (iova - kExtraIova)) & ~0xfffULL)) { if (!wrong++ && !first) first = static_cast<uint32_t> (iova); }
        }
        native_ufs_extra_atos[0] = tested; native_ufs_extra_atos[1] = faults; native_ufs_extra_atos[2] = wrong;
        if (native_ufs_extra_atos[3] == 0) native_ufs_extra_atos[3] = first;
      }
      // The CPU reaches the buffer through a second mapping that is not cached (the buffer's own kernel mapping is cached), so what the CPU writes is in
      // memory when the endpoint reads it and what the endpoint writes is seen at once, whether or not the link is cache coherent. The cached lines are
      // written back and dropped first, and that mapping is not used again.
      for (uint64_t off = 0; off < (kExtraBlocks + 1) * static_cast<uint64_t> (kBlock); off += 64)
        __asm__ volatile ("dc civac, %0" :: "r" (static_cast<uint8_t *> (buffer->getBytesNoCopy ()) + off) : "memory");
      __asm__ volatile ("dsb sy" ::: "memory");
      IOMemoryMap *wc = buffer->createMappingInTask (kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
      native_ufs_extra_va = wc ? reinterpret_cast<uint8_t *> (wc->getVirtualAddress ()) + (bva - static_cast<uint8_t *> (buffer->getBytesNoCopy ())) : bva;
      native_ufs_extra_uncached = wc != nullptr;
      native_ufs_extra_bytes = kExtraBlocks * kBlock - 0;
      native_ufs_extra_iova = kExtraIova;
    }
    native_ufs_extra_stream = 0x80000000U | extraReport[0];
  }
  // The display's streams (sheng-dpu=2 or more, or sheng-dpu-scanout), read here for the same reason: what the boot loader left for 0x1c00 and 0x1c02 (`iommus =
  // <&apps_smmu 0x1c00 0x2>`), the first step of letting the display fetch the compositor's pages. Nothing is written.
  uint32_t dpu = 0, scanout = 0;
  PE_parse_boot_argn ("sheng-dpu", &dpu, sizeof (dpu));
  PE_parse_boot_argn ("sheng-dpu-scanout", &scanout, sizeof (scanout));
  if (dpu >= 2 || scanout) {
    const uintptr_t s = gHost.smmu;
    const uint32_t idr0 = read32 (s, 0x20), idr1 = read32 (s, 0x24);
    const unsigned groups = (idr0 & 255) > 128 ? 128 : (idr0 & 255), banks = idr1 & 255;
    const unsigned pages = 1U << (((idr1 >> 28) & 7) + 1);
    native_ufs_dpu_smmu[1] = read32 (s, 0x0); native_ufs_dpu_smmu[2] = idr0; native_ufs_dpu_smmu[3] = idr1;
    unsigned valid = 0;
    for (unsigned k = 0; k < 2; ++k) native_ufs_dpu_smmu[4 + 9 * k] = 0xffffffffU;
    for (unsigned i = 0; i < groups; ++i) {
      const uint32_t smr = read32 (s, 0x800 + 4 * i);
      if (!(smr & 0x80000000U)) continue;
      ++valid;
      const uint32_t mask = (smr >> 16) & 0x7fff;
      for (unsigned k = 0; k < 2; ++k) {
        volatile uint32_t *r = &native_ufs_dpu_smmu[4 + 9 * k];
        if (r[0] != 0xffffffffU || ((smr ^ (0x1c00U + 2 * k)) & 0x7fff & ~mask)) continue;
        const uint32_t s2cr = read32 (s, 0xc00 + 4 * i), bank = s2cr & 255;
        r[0] = i | (bank << 16); r[1] = smr; r[2] = s2cr;
        if (((s2cr >> 16) & 3) == 0 && bank < banks && (pages + banks) * 4096 <= 0xd0000) {
          const uintptr_t cb = s + (pages + bank) * 4096;
          r[3] = read32 (s, 0x1000 + 4 * bank); r[4] = read32 (s, 0x1800 + 4 * bank);
          r[5] = read32 (cb, 0x0); r[6] = read32 (cb, 0x20); r[7] = read32 (cb, 0x24); r[8] = read32 (cb, 0x30);
          native_ufs_dpu_smmu[22 + k] = read32 (cb, 0x58);
        }
      }
    }
    native_ufs_dpu_smmu[0] = 0x80000000U | groups | (valid << 8) | (banks << 16);
    // sheng-dpu-scanout=1 or more: stage 1 for the display's bank (i562: bank 3, CBAR type 1, SCTLR 0xe0 = translation off, so the display
    // fetched physical addresses; its pipe address registers are 32-bit and every kernel allocation is above 4 GiB). The table keeps
    // 0x80000000..0xffffffff as it was (two 1 GiB identity blocks, so the boot loader's scanout at 0xb8000000 carries on unchanged) and adds
    // kDpuSlots windows of kDpuSlotBytes at kDpuWindow for the compositor's surfaces, filled by native_ufs_dpu_map. Undone at once when the
    // bank does not take the table or a check translation of 0xb8000000 does not come back identical.
    const uint32_t found = native_ufs_dpu_smmu[4], bank = found >> 16;
    if (scanout && found != 0xffffffffU && bank < banks && ((native_ufs_dpu_smmu[6] >> 16) & 3) == 0 && !(native_ufs_dpu_smmu[9] & 1) &&
        ((native_ufs_dpu_smmu[7] >> 16) & 3) == 1) {
      dpuEnable (s, s + (pages + bank) * 4096, bank, native_ufs_dpu_smmu[9]);
    } else if (scanout) {
      native_ufs_dpu_state = 0xdead0001U;
    }
  }
  uint32_t idr1 = read32 (gHost.smmu, 0x24);
  gHost.cb = gHost.smmu + ((1U << (((idr1 >> 28) & 7) + 1)) + (report[0] >> 16)) * 4096;
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.gfsrBefore = read32 (gHost.smmu, 0x48);
    gRootDiagnostic.fsrBefore = read32 (gHost.cb, 0x58);
    gRootDiagnostic.smmuId2 = read32 (gHost.smmu, 0x28);
    gRootDiagnostic.cba2r = read32 (gHost.smmu, 0x1800 + 4 * (report[0] >> 16));
  }
  if (!hostStartQueue (why)) return false;
  gRootDiagnostic.stage = "NOP OUT";
  // NOP OUT proves the transport before touching the SCSI layer.
  if (hostNop () != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "NOP OUT";
    return false;
  }
  uint32_t power = 0;
  gRootDiagnostic.stage = "power mode query";
  if (hostQueryRead (0x03, 0x02, &power) != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "power mode query";
    return false;
  }
  gRootDiagnostic.power = power;
  if ((power & 0xff) != 0x11) {
    gRootDiagnostic.stage = "device wake";
    // START STOP UNIT, power condition ACTIVE, to the UFS Device well-known LU (as Linux resumes).
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0xd0, ssu, 6, false, 0, 500000, &scsi) != kIOReturnSuccess) {
      gHost.ready = false;
      *why = "wake (START STOP UNIT)";
      return false;
    }
    IOSleep (50);
  }
  gRootDiagnostic.stage = "LUN ready";
  for (unsigned attempt = 0; attempt < 20; attempt++) {
    uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0, tur, 6, false, 0, 200000, &scsi) == kIOReturnSuccess) {
      gHost.ready = true;
      return true;
    }
    if (gHost.poisoned) break;
    IOSleep (10);
  }
  gHost.ready = false;
  *why = "LUN 0 not ready";
  return false;
}
}

namespace {
static uint32_t be32w (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

#include "RootLayout.h"
#include "RootWindowRead.h"
#include "RootPublication.h"

}

// ---- Persistent Data store window (sheng-data-persist) ----------------------------------------------------------
// The only part of this driver that ever writes. The consumer (NativeRamBlock's overlay) asks for a window of the given
// size at the END of the userdata partition. Mode 1 (boot argument sheng-data-persist=1) only reads: it proves the window and
// lets the consumer inspect its header. Mode 2 additionally opens the write window. A window is refused unless it lies inside
// the partition, clear of the flashed root image plus a 4 MiB guard, and is at most a quarter of the partition.
namespace {
struct StoreGeometry {
  bool valid;
  uint64_t first, last, imageBytes, entriesLba;
  uint32_t headerCrc, entriesCrc, entryCount;
};
StoreGeometry gStoreGeometry;
uint64_t gStoreRegionFirst, gStoreRegionBlocks;
// The diagnostic log (sheng-log=1): always the last 32 MiB of userdata, so the Data store's place does not depend on logging.
constexpr uint64_t kLogReserveBlocks = 8192;
// The log's export region: the 256 MiB just below the log window (the render server's pipeline caches, kits/sheng BootLog.c). It is
// part of the log's write window, so the store ends below both.
constexpr uint64_t kExportBlocks = 65536;
uint64_t gLogRegionFirst, gLogRegionBlocks;
// The reserve at the end of userdata that the Data store stays clear of: the log and its export region sit inside it (sheng-log=1), the rest
// is unused. sheng-log-reserve=<MiB>, default 1024; 0 gives all the room to Data and is only accepted with the log off. The store's place
// depends on it, so an image keeps one value for its whole life (a different value moves the store: the old one is read as blank).
constexpr uint32_t kDefaultReserveMiB = 1024;
static uint64_t reserveBlocks (void)
{
  static int64_t cached = -1;
  if (cached < 0) {
    uint32_t mib = kDefaultReserveMiB;
    if (!PE_parse_boot_argn ("sheng-log-reserve", &mib, sizeof (mib)) || mib > 16384) mib = kDefaultReserveMiB;
    cached = static_cast<int64_t> (mib) * 256;   // 4096-byte blocks
  }
  return static_cast<uint64_t> (cached);
}

static unsigned storeMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("sheng-data-persist", &value, sizeof (value)) || value > 2) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}
}

extern "C" unsigned native_ufs_store_mode (void) { return storeMode (); }

// Returns 1 and the first absolute 4096-byte LBA of the window, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_store_open (uint64_t blocks, uint64_t *firstLba, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!storeMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gStoreRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    const uint64_t reserve = reserveBlocks ();
    if (!blocks || reserve >= partition || blocks > partition - reserve) reason = "size";
    else if (partition - blocks - reserve < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      // The reserve at the end of the partition belongs to the diagnostic log (its last 32 MiB, with the export region below them);
      // the store ends just below it.
      uint64_t first = gStoreGeometry.last + 1 - reserve - blocks;
      if (storeMode () == 2 && !hostStoreOpen (first, blocks)) reason = "window refused";
      else {
        gStoreRegionFirst = first;
        gStoreRegionBlocks = blocks;
        if (firstLba) *firstLba = first;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_store_read (uint64_t rel, uint32_t blocks, void *dst)
{
  if (!gStoreRegionBlocks || !dst || !blocks || rel >= gStoreRegionBlocks || blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostRead4k (0, gStoreRegionFirst + rel, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (storeMode () != 2 || !gStoreRegionBlocks || !src || !blocks || rel >= gStoreRegionBlocks ||
      blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    // Forced unit access: the block is on the medium when the command completes, so the overlay needs no cache flush.
    if (hostWrite4k (0, gStoreRegionFirst + rel, chunk, src, true) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_sync (void)
{
  return storeMode () == 2 && gStoreRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// ---- Read-only access to other partitions, by GPT name ------------------------------------------------------------
// The firmware the board's own boot stages left on the modem partition is read from there (the audio DSP's images), so it never has to be
// part of the boot image. Nothing here writes: only reads of 4096-byte blocks of the LUN the name was found on.
// The stream-match slot | context bank << 16 of the extra pass-through stream, bit 31 set once it exists (0: not requested).
extern "C" volatile uint32_t native_ufs_extra_stream = 0;
// The endpoint's DMA buffer: physically contiguous, seen by the endpoint at native_ufs_extra_iova (it only addresses 32 bits).
extern "C" uint8_t *native_ufs_extra_va = nullptr;
extern "C" volatile uint32_t native_ufs_extra_uncached = 0;
extern "C" volatile uint32_t native_ufs_extra_iova = 0, native_ufs_extra_bytes = 0;
extern "C" volatile uint32_t native_ufs_extra_atos[8] = { 0, 0, 0, 0 };
// The display streams' state as the boot loader left it (sheng-dpu=2): [0] bit 31 read, stream-match groups | valid ones << 8 | context banks << 16;
// [1] sCR0, [2] IDR0, [3] IDR1; for 0x1c00 at [4] and 0x1c02 at [13]: group | bank << 16 (all ones: no match), SMR, S2CR, CBAR, CBA2R, SCTLR,
// TTBR0 low, high, TCR; [22], [23] the two banks' FSR.
extern "C" volatile uint32_t native_ufs_dpu_smmu[24] = {};
// The display bank's stage 1 (sheng-dpu-scanout): bit 31 on, low bits the bank; 0xdead000n the step that refused it (1 bank not as expected,
// 2 no memory, 3 the bank ignored the table registers, 4 TLB sync timeout, 5 the display's fetches faulted with translation on).
extern "C" volatile uint32_t native_ufs_dpu_state = 0;
// TTBR0 low and TCR read back, FSR before and after the 50 ms watch, FAR low and high and FSYNR0 of the first fault, us watched.
extern "C" volatile uint32_t native_ufs_dpu_check[8] = {};
// The display bank's SCTLR (reg 0) or FSR (reg 1) now, for the log; all ones before the bank was touched.
extern "C" uint32_t native_ufs_dpu_live (unsigned reg)
{
  return gDpuLiveCb ? read32 (gDpuLiveCb, reg ? 0x58 : 0x0) : 0xffffffffU;
}

// Points slot (0 .. kDpuSlots-1) of the display's window at count 4 KiB pages and returns the slot's address for the display, 0 when
// translation is off or the pages do not fit. The caller makes sure the display is not fetching from the slot while it changes.
extern "C" uint32_t native_ufs_dpu_map (unsigned slot, const uint64_t *pages, unsigned count)
{
  if (!gDpuTables || slot >= kDpuSlots || count > kDpuSlotBytes / 4096) return 0;
  uint64_t *l3 = gDpuTables + 1024 + static_cast<size_t> (slot) * kDpuSlotTables * 512;
  const uint64_t attrs = (1ULL << 10) | (1ULL << 6);
  for (unsigned i = 0; i < kDpuSlotTables * 512; ++i) {
    if (i < count && (pages[i] & 0xfff)) return 0;
    l3[i] = i < count ? (pages[i] | attrs | 3) : 0;
  }
  dpuClean (l3, kDpuSlotTables * 4096);
  IOLockLock (gDpuLock);
  const bool synced = dpuTlbFlush (gDpuCb);
  IOLockUnlock (gDpuLock);
  return synced ? kDpuWindow + slot * kDpuSlotBytes : 0;
}

extern "C" int native_ufs_part_find (const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)
{
  if (!gHost.ready || !name || !lun || !first || !blocks) return 0;
  uint8_t *buffer = static_cast<uint8_t *> (IOMalloc (4096));
  if (!buffer) return 0;
  int found = 0;
  const size_t length = strlen (name);
  for (uint32_t l = 0; l < 6 && !found; ++l) {
    if (hostRead4k (static_cast<uint8_t> (l), 1, 1, buffer) != kIOReturnSuccess || memcmp (buffer, "EFI PART", 8)) continue;
    uint64_t entriesLba;
    uint32_t count, size;
    memcpy (&entriesLba, buffer + 72, 8);
    memcpy (&count, buffer + 80, 4);
    memcpy (&size, buffer + 84, 4);
    if (size < 128 || size > 4096 || count > 512) continue;
    for (uint32_t i = 0; i < count && !found; ++i) {
      const uint64_t at = static_cast<uint64_t> (i) * size;
      if (i == 0 || at % 4096 < size) {
        if (hostRead4k (static_cast<uint8_t> (l), entriesLba + at / 4096, 1, buffer) != kIOReturnSuccess) break;
      }
      const uint8_t *entry = buffer + at % 4096;
      uint64_t firstLba, lastLba;
      memcpy (&firstLba, entry + 32, 8);
      memcpy (&lastLba, entry + 40, 8);
      if (!firstLba || lastLba < firstLba) continue;
      size_t n = 0;
      while (n < length && n < 36 && entry[56 + 2 * n] == static_cast<uint8_t> (name[n]) && entry[57 + 2 * n] == 0) ++n;
      if (n == length && (n == 36 || (entry[56 + 2 * n] == 0 && entry[57 + 2 * n] == 0))) {
        *lun = l;
        *first = firstLba;
        *blocks = lastLba - firstLba + 1;
        found = 1;
      }
    }
  }
  IOFree (buffer, 4096);
  return found;
}

// Reads `blocks` 4096-byte blocks from lba of the LUN (absolute, as native_ufs_part_find returned them).
extern "C" int native_ufs_part_read (uint32_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
  if (!gHost.ready || !dst || !blocks || lun > 5) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostRead4k (static_cast<uint8_t> (lun), lba, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    lba += chunk;
    blocks -= chunk;
  }
  return 1;
}

// The fault registers of the endpoint's stream: global fault status and syndrome, then the context bank's FSR, FAR (low, high) and FSYNR0.
extern "C" int native_ufs_extra_fault (uint32_t *out)
{
  if (!gExtraSmmu || !gExtraCb) return 0;
  out[0] = read32 (gExtraSmmu, 0x48); out[1] = read32 (gExtraSmmu, 0x50);
  out[2] = read32 (gExtraCb, 0x58); out[3] = read32 (gExtraCb, 0x60); out[4] = read32 (gExtraCb, 0x64); out[5] = read32 (gExtraCb, 0x68);
  out[6] = read32 (gExtraCb, 0x0); out[7] = read32 (gExtraCb, 0x30); out[8] = read32 (gExtraCb, 0x20); out[9] = read32 (gExtraSmmu, 0x1000 + 4 * gExtraBank);
  return 1;
}

// ---- Diagnostic log window (sheng-log=1) --------------------------------------------------------------------------
// The board's log writer (kits/sheng BootLog.c) gets the last kLogReserveBlocks of userdata and the kExportBlocks below them, refused
// unless the partition holds them clear of the flashed root image plus a 4 MiB guard. Off without the boot argument.
static unsigned logMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("sheng-log", &value, sizeof (value)) || value > 1) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}

// Returns 1 with the window's first absolute LBA and size in 4096-byte blocks, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_log_open (uint64_t *firstLba, uint64_t *blocks, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!logMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gLogRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    if (reserveBlocks () < kLogReserveBlocks + kExportBlocks) reason = "reserve too small for the log";
    else if (partition - reserveBlocks () < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      uint64_t first = gStoreGeometry.last + 1 - kLogReserveBlocks;
      // One write window covers the export region and the log: [first - kExportBlocks, first + kLogReserveBlocks).
      if (!hostLogOpen (first - kExportBlocks, kExportBlocks + kLogReserveBlocks)) reason = "window refused";
      else {
        gLogRegionFirst = first;
        gLogRegionBlocks = kLogReserveBlocks;
        if (firstLba) *firstLba = first;
        if (blocks) *blocks = kLogReserveBlocks;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_log_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= gLogRegionBlocks || blocks > gLogRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, gLogRegionFirst + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

// The export region below the log window: its size in blocks once the log is open, else 0; writes at a block offset inside it.
extern "C" uint64_t native_ufs_export_blocks (void) { return gLogRegionBlocks ? kExportBlocks : 0; }

extern "C" int native_ufs_export_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= kExportBlocks || blocks > kExportBlocks - rel) return 0;
  const uint64_t base = gLogRegionFirst - kExportBlocks;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, base + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_log_sync (void)
{
  return gLogRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// From a panic, before the log writes that keep its message: later host commands no longer wait on the host lock. 1 = the log is open.
extern "C" int native_ufs_log_panic (void)
{
  gHostPanic = true;
  return gLogRegionBlocks != 0;
}

// Re-read the GPT header and entry array and compare their CRCs with what was recorded at root start. 0 = unchanged.
extern "C" unsigned native_ufs_store_gpt_check (void)
{
  if (!gStoreGeometry.valid || !gHost.ready) return 1;
  static uint8_t block[4096];
  if (hostRead4k (0, 1, 1, block) != kIOReturnSuccess) return 2;
  uint32_t headerBytes = le32 (block + 12);
  if (headerBytes < 92 || headerBytes > 4096 || rootCrc32 (block, headerBytes, true) != le32 (block + 16) ||
      le32 (block + 16) != gStoreGeometry.headerCrc) return 3;
  uint32_t blocks = (gStoreGeometry.entryCount * 128 + 4095) / 4096;
  static uint8_t entries[4096 * 4];
  if (blocks > 4 || hostRead4k (0, gStoreGeometry.entriesLba, blocks, entries) != kIOReturnSuccess) return 4;
  return rootCrc32 (entries, gStoreGeometry.entryCount * 128) == gStoreGeometry.entriesCrc ? 0 : 5;
}

// One read-only window of the root image as a block device: 512-byte sectors on top of 4096-byte
// UFS blocks, so the unchanged dmg disk images (GPT with 512-byte sectors) parse as they do on
// the virtio path.
// Publication checkpoints and media-creation probes, read by the report thread.
static volatile unsigned gStartCheckpoint;
static volatile unsigned gDeviceCalls[6];
static volatile int gBdevSlot = -2;
static volatile int gBdevTestError = -1;
static volatile uint32_t gBdevMagic = 0;
static volatile int gBdevScan = -1;
static volatile int gRootMountError = -100;
static volatile uintptr_t gBdevNode = 0;
// bdevsw window geometry and the block-device ioctl answers, kept here so the mount
// thread can report them before anything it calls can panic.
static volatile uint64_t gBdevAbsolute = 0, gBdevBytes = 0;
static unsigned gBdevIoctlCalls, gBdevIoctlSize, gBdevIoctlSetSize, gBdevIoctlCount, gBdevIoctlOther;
static uint32_t gBdevBlockSize = 512;
static void publishNativeBdev (bool claimRoot = true);
extern "C" int vfs_mountroot (void);
// bsd/kern/bsd_init.c: the device vfs_mountroot() hands to bdevvp(). Nothing sets it
// on this port (setconf() never runs), so it is still major 0 when we mount.
extern "C" dev_t rootdev;
static volatile int gRootMountDev = -1;
static volatile int gRootMountMajor = -1;
static int nativeBdevRead (uint64_t, uint32_t, uint8_t *);

static void mountRootThread (void *, wait_result_t)
{
  rootdev = makedev (gBdevSlot, 0);
  gRootMountDev = static_cast<int> (rootdev);
  gRootMountMajor = major (rootdev);
  uint8_t *head = static_cast<uint8_t *> (IOMalloc (4096));
  int headRead = head ? nativeBdevRead (0, 4096, head) : -1;
  IOLog ("NATIVE_UFS mountroot: rootdev=%d major=%d minor=%d slot=%d abs=%llx bytes=%llx "
         "read=%d head=%02x%02x%02x%02x at32=%02x%02x%02x%02x\n",
         static_cast<int> (rootdev), major (rootdev), minor (rootdev), gBdevSlot,
         static_cast<unsigned long long> (gBdevAbsolute),
         static_cast<unsigned long long> (gBdevBytes), headRead,
         head ? head[0] : 0, head ? head[1] : 0, head ? head[2] : 0, head ? head[3] : 0,
         head ? head[32] : 0, head ? head[33] : 0, head ? head[34] : 0, head ? head[35] : 0);
  if (head) IOFree (head, 4096);
  gRootMountError = vfs_mountroot ();
  IOLog ("NATIVE_UFS mountroot: vfs_mountroot returned %d\n", gRootMountError);
}
static int nativeBdevRead (uint64_t, uint32_t, uint8_t *);

static void bdevTestThread (void *, wait_result_t)
{
  uint8_t *data = static_cast<uint8_t *> (IOMalloc (65536));
  if (!data) return;
  gBdevTestError = nativeBdevRead (0, 65536, data);
  if (gBdevTestError == 0) {
    gBdevMagic = (uint32_t (data[0]) << 24) | (uint32_t (data[1]) << 16) |
                 (uint32_t (data[2]) << 8) | uint32_t (data[3]);
    for (unsigned off = 0; off + 4 <= 65536; ++off) {
      if (data[off] == 'N' && data[off + 1] == 'X' && data[off + 2] == 'S' && data[off + 3] == 'B') {
        gBdevScan = static_cast<int> (off);
        break;
      }
    }
  }
  IOFree (data, 65536);
  IOLog ("NATIVE_UFS bdev: read=%d magic=%08x scan=%d bytes=%llx\n", gBdevTestError, gBdevMagic,
         gBdevScan, static_cast<unsigned long long> (gBdevBytes));
}


class NativeUfsBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeUfsBlock)
  uint64_t absolute = 0;      // byte offset of the window on LUN 0
  uint64_t bytes = 0;
  char label[32] = {};
  uint8_t *bounce = nullptr;
  IOLock *windowLock = nullptr;
  bool online = false;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  struct vr_view verified = {};
  vm_offset_t verifiedStorage = 0;
  vm_size_t verifiedStorageBytes = 0;
  bool configureVerification ();
#endif
public:
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  bool authenticatesVolume (const char *uuid, uint8_t *manifestDigest = nullptr) const;
  bool signedVolumeUuid (char out[37]) const;
#endif
  const char *windowLabel () const { return label; }
  bool configure (const char *name, uint64_t absoluteOffset, uint64_t size);
  bool start (IOService *provider) override;
  bool handleOpen (IOService *client, IOOptionBits options, void *argument) override;
  void handleClose (IOService *client, IOOptionBits options) override;
  void free () override;
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnSuccess; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { gDeviceCalls[4] = gDeviceCalls[4] + 1; IOLog ("NUV vendor\n"); return const_cast<char *> ("Native"); }
  char *getProductString () override { IOLog ("NUV product %s\n", label); return label; }
  char *getRevisionString () override { IOLog ("NUV revision\n"); return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override { IOLog ("NUV info\n"); return const_cast<char *> ("UFS LUN 0 userdata window, read-write"); }
  IOReturn reportBlockSize (UInt64 *value) override {
    gDeviceCalls[0] = gDeviceCalls[0] + 1;
    IOLog ("NUV blocksize\n");
    *value = 512;
    return kIOReturnSuccess;
  }
  IOReturn reportEjectability (bool *value) override { IOLog ("NUV ejectability\n"); *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override {
    gDeviceCalls[1] = gDeviceCalls[1] + 1;
    IOLog ("NUV maxblock\n");
    *value = bytes / 512 - 1;
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    gDeviceCalls[2] = gDeviceCalls[2] + 1;
    IOLog ("NUV mediastate\n");
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { IOLog ("NUV removability\n"); *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override {
    gDeviceCalls[3] = gDeviceCalls[3] + 1;
    IOLog ("NUV writeprotect\n");
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn doSynchronizeCache () override { IOLog ("NUV sync\n"); return online ? kIOReturnSuccess : kIOReturnNotReady; }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors (NativeUfsBlock, IOBlockStorageDevice)

#if defined(NATIVE_UFS_SOURCE_API) && NATIVE_UFS_SOURCE_API
// The source runtime owns registration and attachment after IOStorageFamily
// has loaded. No second IOKit personality starts this service in that mode.
extern "C" IOService *native_ufs_source_create (void)
{
  return new NativeUfs;
}
#endif

extern "C" {
// Read by peridot-bdevsw-root.patch when the kernel adopts this disk as the root.
volatile uint64_t native_ufs_bdev_absolute = 0;
volatile uint64_t native_ufs_bdev_bytes = 0;
// Set by the probe that wants the published bdevsw disk adopted as the kernel root
// (native-ufs-probe=36, fix A in peridot/audit/2026-11-15-enotblk.md section 7). The kernel
// side checks it in IOFindBSDRoot before the rd=/rootdev wait, so no other probe changes.
int native_ufs_bdev_claim_root = 0;
// The bdevsw slot registered by publishNativeBdev(). IOFindBSDRoot builds the root dev_t from
// this instead of the rootdev global: the kernel resets rootdev to NODEV between IOKit start-up
// and setconf (p364 showed claim=1 but rootdev=-1).
int native_ufs_bdev_slot = -1;
volatile uint32_t native_ufs_blocks_published = 0;
volatile uint64_t native_ufs_block_reads = 0;
volatile uint64_t native_ufs_block_failures = 0;
}

static char gRootDiagnosticText[2500];
static char gRootMediaText[1000];
// Passive diagnostic reader: cached software values only. It must remain safe
// when called from an IRQ while the transport is busy: no locks, allocations,
// MMIO reads, queue resets, or changes to the command/trust state. Fields can
// represent adjacent updates; this is explicitly not an atomic HW snapshot.
extern "C" void native_ufs_progress_text (char *out, size_t size)
{
  UfsRootDiagnostics sample = gRootDiagnostic;
  sample.published = native_ufs_blocks_published;
  sample.reads = native_ufs_block_reads;
  sample.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (out, size, sample, "cached values; not a new HW fault sample", "UFS CACHED SNAPSHOT");
}

static void rootDiagnosticFailure (const char *reason)
{
  // Kept even when the panic diagnostic is off, so the boot page can say why nothing was published.
  native_ufs_root_failure = reason;
  if (!gRootDiagnosticEnabled) return;
  gRootDiagnostic.published = native_ufs_blocks_published;
  gRootDiagnostic.reads = native_ufs_block_reads;
  gRootDiagnostic.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (gRootDiagnosticText, sizeof (gRootDiagnosticText), gRootDiagnostic, reason);
  strlcat (gRootDiagnosticText, gRootMediaText, sizeof (gRootDiagnosticText));
  panic ("%s", gRootDiagnosticText);
}

bool NativeUfsBlock::configure (const char *name, uint64_t absoluteOffset, uint64_t size)
{
  if (!name || !ufsWindowGeometry (absoluteOffset, size)) {
    return false;
  }
  strlcpy (label, name, sizeof (label));
  absolute = absoluteOffset;
  bytes = size;
  windowLock = IOLockAlloc ();
  if (!windowLock) return false;
  bounce = static_cast<uint8_t *> (IOMalloc (kDataMax));
  if (!bounce) {
    return false;
  }
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  if (gAllowUnverifiedRoot) return true;
  // Reads are served only after each 4096-byte integrity block verifies against the owner-signed
  // policy; without a matching policy the device is never published.
  return configureVerification ();
#else
  return true;
#endif
}

// Data objects of the root image (version 2 manifest), recorded by startRoot before windows are configured.
static volatile unsigned gPrepStage = 0;
static RootWindow gRootObject[4];
static uint32_t gRootObjects;
static uint64_t gRootObjectBase;

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
volatile uint64_t native_ufs_verified_blocks = 0;

// Read the hash table of a header-only policy from the root image object "<window>.vroot" into `table`.
// The bytes are untrusted here; vr_open checks them against the digest in the signed header.
static bool readPolicyTable (const char *window, uint8_t *table, uint64_t tableBytes)
{
  char name[32];
  if (snprintf (name, sizeof (name), "%s.vroot", window) >= static_cast<int> (sizeof (name))) {
    panic ("VRFAIL window name too long: %s", window);
    return false;
  }
  const RootWindow *object = nullptr;
  for (unsigned i = 0; i < gRootObjects; ++i)
    if (!strcmp (gRootObject[i].name, name)) object = &gRootObject[i];
  if (!object) {
    panic ("VRFAIL no object %s (objects=%u)", name, gRootObjects);
    return false;
  }
  // The generator pads each object to a whole 4096-byte block, while the signed
  // header counts 32-byte hashes; only tableBytes is covered by the signature.
  const uint64_t aligned = (tableBytes + 4095) & ~static_cast<uint64_t> (4095);
  if (object->size != tableBytes && object->size != aligned) {
    panic ("VRFAIL %s size=%llx expected=%llx aligned=%llx objects=%u", name,
           (unsigned long long) object->size, (unsigned long long) tableBytes,
           (unsigned long long) aligned, gRootObjects);
    return false;
  }
  uint64_t lba = (gRootObjectBase + object->offset) / 4096, done = 0;
  while (done < tableBytes) {
    uint64_t left = tableBytes - done;
    uint32_t blocks = 1;
    if (hostRead4k (0, lba, blocks, gScratch) != kIOReturnSuccess) {
      panic ("VRFAIL table read lba=%llx done=%llu of %llu window=%s",
             (unsigned long long) lba, (unsigned long long) done,
             (unsigned long long) tableBytes, window);
      return false;
    }
    uint64_t take = left < blocks * 4096ULL ? left : blocks * 4096ULL;
    memcpy (table + done, gScratch, take);
    done += take;
    lba += blocks;
  }
  return true;
}
// Time spent hashing/verifying and reading from the UFS transport, in generic-timer ticks (diagnostic only).
volatile uint64_t native_ufs_verify_ticks = 0, native_ufs_read_ticks = 0;
static inline uint64_t timerTicks (void) {uint64_t v; __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (v)); return v;}
volatile uint32_t native_ufs_integrity_failures = 0;

bool NativeUfsBlock::configureVerification ()
{
  gRootDiagnostic.stage = "policy selection";
  gRootDiagnostic.windowBytes = bytes;
  gRootDiagnostic.policyCount = verified_root_policy_count;
  gRootDiagnostic.verify = 0xffffffff;
  // Every rejection below panics instead of returning false: a silent false cannot be
  // told apart from a crash further along the failure path, and the screen only ever
  // shows the tail of a panic.
  auto reject = [&] (const char *why) -> bool {
    panic ("VRFAIL %s window=%s bytes=%llx policies=%llu",
           why, label, (unsigned long long) bytes,
           (unsigned long long) verified_root_policy_count);
    return false;
  };
  // Select the one policy whose signed image size equals this window; two matches are refused.
  const uint8_t *manifest = nullptr;
  uint64_t manifestBytes = 0;
  if (!verified_root_policy_count || verified_root_policy_count > 16) {
    return reject ("policy count invalid");
  }
  for (uint64_t index = 0; index < verified_root_policy_count; ++index) {
    const NativeVerifiedBlockPolicy &policy = verified_root_policies[index];
    if (!policy.manifest || policy.manifestBytes < VR_HEADER_BYTES + VR_SIGNATURE_BYTES) {
      return reject ("policy envelope invalid");
    }
    uint64_t imageBytes = 0;
    for (unsigned i = 0; i < 8; ++i) {
      imageBytes |= static_cast<uint64_t> (policy.manifest[24 + i]) << (8 * i);
    }
    if (imageBytes != bytes) {
      continue;
    }
    if (manifest) {
      return reject ("duplicate capacity-matching policy");
    }
    manifest = policy.manifest;
    manifestBytes = policy.manifestBytes;
  }
  if (!manifest) {
    return reject ("no capacity-matching policy");
  }
  // A header-only policy carries its hash table in the root image instead of the boot image.
  const bool headerOnly = manifestBytes == VR_HEADER_BYTES + VR_SIGNATURE_BYTES;
  uint64_t hashBytes = 0;
  if (headerOnly) {
    uint64_t count = 0;
    for (unsigned i = 0; i < 8; ++i) count |= static_cast<uint64_t> (manifest[32 + i]) << (8 * i);
    if (!count || count > (UINT64_MAX - manifestBytes) / 32) {
      return reject ("policy header block count");
    }
    hashBytes = count * 32;
    manifestBytes += hashBytes;
  }
  gRootDiagnostic.stage = "policy storage";
  vm_size_t tableBytes = round_page (static_cast<vm_size_t> (manifestBytes));
  if (tableBytes < manifestBytes || tableBytes > UINT64_MAX - 4096) {
    return reject ("policy size overflow");
  }
  verifiedStorageBytes = tableBytes + 4096;
  gRootDiagnostic.stage = "policy allocation";
  if (kmem_alloc (kernel_map, &verifiedStorage, verifiedStorageBytes,
                  static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
    return reject ("policy allocation");
  }
  if (headerOnly) {
    memcpy (reinterpret_cast<void *> (verifiedStorage), manifest, VR_HEADER_BYTES + VR_SIGNATURE_BYTES);
    gRootDiagnostic.stage = "policy table object read";
    if (!readPolicyTable (label, reinterpret_cast<uint8_t *> (verifiedStorage) + VR_HEADER_BYTES + VR_SIGNATURE_BYTES,
                          hashBytes)) {
      return reject ("policy table object read");
    }
  } else {
    memcpy (reinterpret_cast<void *> (verifiedStorage), manifest, manifestBytes);
  }
  uint8_t *key = reinterpret_cast<uint8_t *> (verifiedStorage + tableBytes);
  memcpy (key, verified_root_key, VR_KEY_BYTES);
  gRootDiagnostic.stage = "policy read-only protection";
  if (mach_vm_protect (kernel_map, verifiedStorage, verifiedStorageBytes,
                       TRUE, VM_PROT_READ) != KERN_SUCCESS) {
    return reject ("policy read-only protection");
  }
  gRootDiagnostic.stage = "owner signature and hash table";
  int status = vr_open (reinterpret_cast<const uint8_t *> (verifiedStorage), manifestBytes, key, 1,
                        &vr_xnu_crypto, &verified);
  gRootDiagnostic.verify = static_cast<uint32_t> (status);
  if (status != VR_OK || verified.image_bytes != bytes) {
    panic ("VRFAIL vr_open status=%d image_bytes=%llx bytes=%llx window=%s",
           status, (unsigned long long) verified.image_bytes, (unsigned long long) bytes, label);
    return false;
  }
  return true;
}

bool NativeUfsBlock::authenticatesVolume (const char *uuid, uint8_t *manifestDigest) const
{
  if (!uuid || !online || !verifiedStorage || !verified.hashes) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  char expected[37];
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      expected[at++] = '-';
    }
    expected[at++] = hex[verified.volume_uuid[i] >> 4];
    expected[at++] = hex[verified.volume_uuid[i] & 15];
  }
  expected[at] = 0;
  if (strcmp (uuid, expected)) {
    return false;
  }
  if (manifestDigest) {
    // Hash only the authenticated envelope, excluding allocation padding and the key.
    const uint64_t envelope = VR_HEADER_BYTES + VR_SIGNATURE_BYTES + verified.block_count * 32;
    SHA384_CTX context;
    SHA384_Init (&context);
    SHA384_Update (&context, reinterpret_cast<const void *> (verifiedStorage), envelope);
    SHA384_Final (manifestDigest, &context);
  }
  return true;
}

bool NativeUfsBlock::signedVolumeUuid (char out[37]) const
{
  if (!verifiedStorage || !verified.hashes) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out[at++] = '-';
    }
    out[at++] = hex[verified.volume_uuid[i] >> 4];
    out[at++] = hex[verified.volume_uuid[i] & 15];
  }
  out[at] = 0;
  return true;
}

// Called from the root authorization query in NativeVirtioBlock: -1 when `provider` is not a
// UFS window, otherwise 1 if its signed volume UUID equals `uuid`, else 0.
extern "C" int native_ufs_block_authenticates (IOService *provider, const char *uuid, uint8_t *digest)
{
  NativeUfsBlock *block = OSDynamicCast (NativeUfsBlock, provider);
  if (!block) {
    return -1;
  }
  return block->authenticatesVolume (uuid, digest) ? 1 : 0;
}
#endif


bool NativeUfsBlock::start (IOService *provider)
{
  gStartCheckpoint = 1;
  // The family start blocks for at least 30 s in this context (p84/p88), and
  // skipping it leaves IOBlockStorageDriver unable to instantiate. Publication
  // therefore goes through bdevsw now; this device only feeds the transport.
  if (!bounce || !windowLock || !IOService::start (provider)) {
    return false;
  }
  gStartCheckpoint = 2;
  online = true;
  // IOBlockStorageDriver reads this property to classify the transport; without
  // it the driver opens the device (NU8) but never calls any report method.
  setProperty (kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric);
  // registerService() silently refuses an object that is not attached to the
  // IOService plane root ("not registry member"), and without it no personality is
  // ever probed; the two markers say whether the call was entered and returned.
  IOLog ("NU6 1 %s\n", label);
  registerService ();
  gStartCheckpoint = 3;
  IOLog ("NU6 2 %s\n", label);
  return true;
}

// The storage family opens this device before it starts: seeing the call proves the
// personality matched and IOBlockStorageDriver::start() ran.
bool NativeUfsBlock::handleOpen (IOService *client, IOOptionBits options, void *argument)
{
  // The prelinked IOStorageFamily dispatches this hook through the class vtable; delegating
  // to the parent implementation is not bound on this port, so return success directly and
  // log both the caller and the decision (the driver's whole start path depends on it).
  IOLog ("NU8 open %s cls=%s ret=1\n", client ? client->getName () : "-", (client && client->getMetaClass () && client->getMetaClass ()->getClassName ()) ? client->getMetaClass ()->getClassName () : "-");
  return true;
}

// A close right after the open means the storage family gave up on the device, which is the
// difference between 'driver still starting' and 'driver failed' on the boot screen.
void NativeUfsBlock::handleClose (IOService *client, IOOptionBits options)
{
  IOLog ("NU8 close %s\n", client ? client->getName () : "-");
  (void) options;
}

IOReturn NativeUfsBlock::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                           IOStorageAttributes *, IOStorageCompletion *completion)
{
  gDeviceCalls[5] = gDeviceCalls[5] + 1;
  if (!buffer || !completion || !completion->action || !count || block >= bytes / 512 ||
      count > bytes / 512 - block || buffer->getLength () < count * 512) {
    return kIOReturnBadArgument;
  }
  const IODirection direction = buffer->getDirection ();
  if (!(direction & (kIODirectionIn | kIODirectionOut))) {
    return kIOReturnBadArgument;
  }
  if (direction & kIODirectionOut) {
    struct WriteOperations {
      IOLock *mutex;
      IOMemoryDescriptor *buffer;
      void lock () { IOLockLock (mutex); }
      void unlock () { IOLockUnlock (mutex); }
      IOReturn read (uint64_t lba, uint32_t blocks, void *out) {
        return hostRead4k (0, lba, blocks, out);
      }
      IOReturn write (uint64_t lba, uint32_t blocks, const void *in) {
        // Forced unit access: WRITE(10) with FUA puts the block on the medium before it completes,
        // so the block-device path does not depend on SYNCHRONIZE CACHE. This is the exact write
        // form probe 33 verified on the device (p356: match=1, restore=0/0, ok=1); the previous
        // form wrote through hostSyncCache(), whose window gate only knew the store/log windows
        // and made every IOBlockStorageDevice write report kIOReturnNotReady (audit P0-1).
        return hostWrite4k (0, lba, blocks, in, true);
      }
      uint64_t gather (uint64_t offset, void *dst, uint64_t length) {
        return buffer->readBytes (offset, dst, length);
      }
    } writeOperations = { windowLock, buffer };
    UfsWindowResult writeOutcome = ufsWriteWindow (writeOperations, absolute, bytes, block, count, bounce);
    if (writeOutcome.status != kIOReturnSuccess) {
      __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
      gRootDiagnostic.result = static_cast<uint32_t> (writeOutcome.status);
      rootDiagnosticFailure ("root window write failed");
    }
    completion->action (completion->target, completion->parameter, writeOutcome.status, writeOutcome.done);
    return kIOReturnSuccess;
  }
  if (!online) {
    return kIOReturnNotReady;
  }
  struct Operations {
    IOLock *mutex;
    IOMemoryDescriptor *buffer;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    const vr_view *view;
#endif
    void lock () { IOLockLock (mutex); }
    void unlock () { IOLockUnlock (mutex); }
    IOReturn read (uint64_t lba, uint32_t blocks, void *out) {
      uint64_t t0 = timerTicks ();
      IOReturn r = hostRead4k (0, lba, blocks, out);
      __atomic_fetch_add (&native_ufs_read_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      return r;
    }
    IOReturn verify (uint64_t index, const void *data, size_t length) {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      // Unverified bring-up: there is no policy, so accept the block the transport read.
      if (gAllowUnverifiedRoot) {
        (void) index; (void) data; (void) length;
        return kIOReturnSuccess;
      }
      uint64_t t0 = timerTicks ();
      int status = vr_verify_block (view, index, data, length, &vr_xnu_crypto);
      __atomic_fetch_add (&native_ufs_verify_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      if (status != VR_OK) {
        gRootDiagnostic.stage = "read block integrity";
        gRootDiagnostic.verify = static_cast<uint32_t> (status);
        __atomic_fetch_add (&native_ufs_integrity_failures, 1, __ATOMIC_RELAXED);
        return kIOReturnNotPrivileged;
      }
      __atomic_fetch_add (&native_ufs_verified_blocks, 1, __ATOMIC_RELAXED);
#if defined(NATIVE_UFS_SOURCE_API) && NATIVE_UFS_SOURCE_API
      boot_progress_poll ();
#else
      if (boot_progress_poll) boot_progress_poll ();
#endif
#else
      (void) index; (void) data; (void) length;
#endif
      return kIOReturnSuccess;
    }
    uint64_t copy (uint64_t offset, const void *data, uint64_t length) {
      uint64_t copied = buffer->writeBytes (offset, data, length);
      if (copied == length) __atomic_fetch_add (&native_ufs_block_reads, 1, __ATOMIC_RELAXED);
      return copied;
    }
  } operations = { windowLock, buffer
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    , &verified
#endif
  };
  UfsWindowResult outcome = ufsReadWindow (operations, absolute, bytes, block, count, bounce);
  IOReturn result = outcome.status;
  uint64_t done = outcome.done;
  if (result != kIOReturnSuccess) {
    __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
    gRootDiagnostic.result = static_cast<uint32_t> (result);
    rootDiagnosticFailure ("root window read failed");
  }
  completion->action (completion->target, completion->parameter, result, done);
  return kIOReturnSuccess;
}

void NativeUfsBlock::free ()
{
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  if (verifiedStorage) {
    kmem_free (kernel_map, verifiedStorage, verifiedStorageBytes);
    verifiedStorage = 0;
  }
#endif
  if (bounce) {
    IOFree (bounce, kDataMax);
    bounce = nullptr;
  }
  if (windowLock) {
    IOLockFree (windowLock);
    windowLock = nullptr;
  }
  IOBlockStorageDevice::free ();
}

// Ordered publication of the root windows. The original owner-trust code resolves the SystemOS
// volume by UUID immediately after the root is mounted, so SystemOS must already have published its
// volume media by then; the OS window (whose volume is the root being waited for) goes last.
static NativeUfsBlock *gPendingDevice[4];
// The object each window attached to. start() must be called with the same provider,
// otherwise the parent link and the start argument disagree and the storage family
// never matches the device.
static IOService *gPendingHost;
// Publisher heartbeat: a wait() that never returns looks identical to a wait() that
// times out, so count the loop iterations and the match attempts.
static volatile unsigned gWaitTicks, gMatchCalls;
static unsigned gPending;
static bool gPendingAttached[4];
static char gPendingUuid[4][37];
static UfsRootPlan gPublicationPlan;
static NativeUfs *gPendingProvider;

static bool mediaWithUuidExists (const char *uuid, NativeUfsBlock *window)
{
  const bool anyMedia = !uuid || !uuid[0];
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity (1);
  OSString *wanted = anyMedia ? nullptr : OSString::withCString (uuid);
  bool found = false;
  const bool filtered = anyMedia || (matching && properties && wanted &&
                                     properties->setObject ("UUID", wanted) &&
                                     matching->setObject ("IOPropertyMatch", properties));
  if (matching && filtered) {
    OSIterator *iterator = IOService::getMatchingServices (matching);
    if (iterator) {
      for (unsigned matches = 0; matches < 64 && !found; ++matches) {
        IORegistryEntry *media = OSDynamicCast (IORegistryEntry, iterator->getNextObject ());
        if (!media) break;
        IORegistryIterator *parents = IORegistryIterator::iterateOver (
          media, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
        if (parents) {
          found = ufsMediaHasAncestor (*parents, window);
          parents->release ();
        }
      }
      iterator->release ();
    }
  }
  OSSafeReleaseNULL (wanted);
  OSSafeReleaseNULL (properties);
  OSSafeReleaseNULL (matching);
  return found;
}


// Count what the registry holds right now, without matching the parent chain: a
// publisher that waits forever looks the same whether the media never appeared or
// only the ancestry check fails.
//
// Why nothing matches (p16): the storage stack is a prelinked Apple kext, and its
// IOKit personalities reach the IOCatalogue only when the kext is loaded. kextd does
// that on a normal boot; this kernel stops before any userland runs, so the catalogue
// holds only the built-in personalities and no IOBlockStorageDriver is ever created
// for a window device. Without a driver there is no IOMedia, so the partition scheme
// and APFS have nothing to match either. The workaround loads the family and, when
// the catalogue still has no match for this device, publishes the two personalities
// the publication chain needs. Every step is counted so the boot screen can say which
// one failed.
static volatile unsigned gStorageKextFlags;   // 1 found, 2 loaded, 4 started, 8 kernel component
static volatile unsigned gStorageCatalogueBefore, gStorageCatalogueAfter;
static volatile unsigned gStorageAdded, gStorageLoadResult, gStorageDeviceType;
static volatile unsigned gApfsService;
static bool gStorageBootstrapped;
static unsigned countService (const char *className);

static unsigned kextFlags (const char *identifier)
{
  auto kext = OSKext::lookupKextWithIdentifier (identifier);
  if (!kext) return 0;
  unsigned flags = 1;
  if (kext->isLoaded ()) flags |= 2;
  if (kext->isStarted ()) flags |= 4;
  if (kext->isKernelComponent ()) flags |= 8;
  return flags;
}

// Ask the catalogue which personalities would match this device right now. This is
// the exact query driver matching performs, so a zero here is the proof that the
// break is before IOBlockStorageDriver::start, not inside it.
// Whether a kext class is registered, and how many instances exist. A personality
// that never instantiates and a class that was never registered look identical in
// the registry, so both numbers are printed side by side.
static unsigned classInstances (const char *name, unsigned *registered)
{
  const OSSymbol *symbol = OSSymbol::withCString (name);
  const OSMetaClass *meta = symbol ? OSMetaClass::getMetaClassWithName (symbol) : nullptr;
  OSSafeReleaseNULL (symbol);
  if (registered) *registered = meta ? 1u : 0u;
  return meta ? meta->getInstanceCount () : 0;
}

static unsigned catalogueMatches (IOService *device)
{
  if (!gIOCatalogue || !device) return 0xffffffff;
  // findDrivers() ends with "*generationCount = getGenerationCount()" and does not
  // check the pointer: passing nullptr stored to address 0, which is the
  // "Kernel data abort, far=0, esr=0x96000045" that every build since p29 hit.
  SInt32 generation = 0;
  auto drivers = gIOCatalogue->findDrivers (device, &generation);
  return drivers ? static_cast<unsigned> (drivers->getCount ()) : 0;
}

static void addStringProperty (OSDictionary *dict, const char *key, const char *value)
{
  OSString *string = OSString::withCString (value);
  if (string) {
    dict->setObject (key, string);
    string->release ();
  }
}

static bool addPersonality (OSDictionary *personality)
{
  OSArray *array = OSArray::withCapacity (1);
  bool added = false;
  if (gIOCatalogue && personality && array) {
    array->setObject (personality);
    added = gIOCatalogue->addDrivers (array, true);
  }
  OSSafeReleaseNULL (array);
  return added;
}

// The two personalities this chain needs, copied from the family's Info.plist.
static bool addBlockStorageDriverPersonality ()
{
  OSDictionary *personality = OSDictionary::withCapacity (5);
  OSDictionary *propertyMatch = OSDictionary::withCapacity (1);
  bool added = false;
  if (personality && propertyMatch) {
    addStringProperty (personality, "CFBundleIdentifier", "com.apple.iokit.IOStorageFamily");
    addStringProperty (personality, "IOClass", "IOBlockStorageDriver");
    addStringProperty (personality, "IOProviderClass", "IOBlockStorageDevice");
    addStringProperty (propertyMatch, "device-type", "Generic");
    personality->setObject ("IOPropertyMatch", propertyMatch);
    added = addPersonality (personality);
  }
  OSSafeReleaseNULL (propertyMatch);
  OSSafeReleaseNULL (personality);
  return added;
}

static bool addGuidSchemePersonality ()
{
  OSDictionary *personality = OSDictionary::withCapacity (6);
  OSDictionary *propertyMatch = OSDictionary::withCapacity (1);
  bool added = false;
  if (personality && propertyMatch) {
    addStringProperty (personality, "CFBundleIdentifier", "com.apple.iokit.IOStorageFamily");
    addStringProperty (personality, "IOClass", "IOGUIDPartitionScheme");
    addStringProperty (personality, "IOProviderClass", "IOMedia");
    addStringProperty (personality, "IOMatchCategory", "IOStorage");
    addStringProperty (personality, "Content Mask", "GUID_partition_scheme");
    propertyMatch->setObject ("Whole", kOSBooleanTrue);
    personality->setObject ("IOPropertyMatch", propertyMatch);
    added = addPersonality (personality);
  }
  OSSafeReleaseNULL (propertyMatch);
  OSSafeReleaseNULL (personality);
  return added;
}

// The filesystem side of the same chain: a GPT partition whose type is the APFS
// container GUID is matched by AppleAPFSContainerScheme, which then publishes the
// container and its volumes. Both personalities come from the apfs kext's plist.
static bool addContentPersonality (const char *bundle, const char *ioClass, const char *contentHint)
{
  OSDictionary *personality = OSDictionary::withCapacity (6);
  OSDictionary *propertyMatch = OSDictionary::withCapacity (1);
  bool added = false;
  if (personality && propertyMatch) {
    addStringProperty (personality, "CFBundleIdentifier", bundle);
    addStringProperty (personality, "IOClass", ioClass);
    addStringProperty (personality, "IOProviderClass", "IOMedia");
    addStringProperty (personality, "IOMatchCategory", "IOStorage");
    addStringProperty (propertyMatch, "Content Hint", contentHint);
    personality->setObject ("IOPropertyMatch", propertyMatch);
    added = addPersonality (personality);
  }
  OSSafeReleaseNULL (propertyMatch);
  OSSafeReleaseNULL (personality);
  return added;
}

// p16 panicked with a data abort (write, far=0) and the panic text scrolled away, so
// the bring-up is now split into stages that each announce themselves before acting.
// The panic dump leaves only the last few log lines on screen, so the last "stage N"
// line names the step that died. native-ufs-storage=<bits> bisects without a kernel
// rebuild (only the payload carries boot arguments): 1 load the kext,
// 2 IOBlockStorageDriver, 4 IOGUIDPartitionScheme, 8 APFS container personalities
// (255 = everything).
static volatile unsigned gStorageStage, gStorageLevel, gStorageWindow;
// 1 when the parent meta class really is IOBlockStorageDevice: the bound vtable/metaclass
// addresses come from the prelinked image, so the class identity has to be checked by
// name rather than assumed.
static volatile unsigned gStorageParentOk;

// A panic dump is longer than the screen, so the panic text itself always scrolls off;
// only the log lines printed immediately before it survive. Running the bring-up at
// the very end of the report thread therefore makes its stage line readable even when
// the step crashes: nothing else is printed between it and the dump.
static bool storageLatePhase ()
{
  unsigned phase = 0;
  PE_parse_boot_argn ("native-ufs-storage-phase", &phase, sizeof (phase));
  return phase == 1;
}

// The panic dump is longer than the screen, so a one-shot log line is always pushed
// out of view before anyone can photograph it. This thread repeats the storage state
// every two seconds instead: whatever the kernel prints next (a panic dump included),
// the last line above it is the state we need.
// Numbers are unreadable in the doubled console font; a name is not.
static const char *storageStageName (unsigned stage)
{
  switch (stage) {
    case 0: return "none";
    case 20: return "host";
    case 21: return "layout";
    case 22: return "manifest";
    case 23: return "geom";
    case 24: return "svcstart";
    case 25: return "prep";
    case 30: return "palloc";
    case 31: return "pinit";
    case 32: return "pprop";
    case 33: return "pcfg";
    case 34: return "phost";
    case 35: return "pattch";
    case 36: return "attached";
    case 40: return "ident";
    case 42: return "sched";
    case 51: return "st1probe";
    case 52: return "st2load";
    case 53: return "st3driver";
    case 54: return "st4guid";
    case 55: return "st5apfs";
    case 59: return "stdone";
    default: return "other";
  }
}

static void storageHeartbeat (void *, wait_result_t)
{
  for (;;) {
    // Four lines a second: a panic dump is taller than the screen, so this line has to
    // still be inside the visible window when the dump starts.
    // The console truncates long lines, so keep every line well under 20 columns and
    // put the stage name first: the number was unreadable in the doubled font and the
    // trailing name was cut off.
    // One value per line: the doubled console font makes multi-field lines unreadable
    // from a photo, but a single letter plus a single digit always survives.
    IOLog ("NUS %s\n", storageStageName (gStorageStage));
    IOLog ("NUK %x\n", gStorageKextFlags);
    IOLog ("NUL %d\n", gStorageLoadResult);
    IOLog ("NUC %u\n", gStorageCatalogueAfter);
    IOLog ("NUA %x\n", gStorageAdded);
    IOLog ("NUX %u\n", gStorageParentOk);
    IOSleep (400);
  }
}

static void storageHaltIfRequested ()
{
  unsigned halt = 0;
  PE_parse_boot_argn ("native-ufs-storage-halt", &halt, sizeof (halt));
  if (!halt) return;
  IOLog ("NativeUfs: storage halt k%x c%u/%u a%x l%d d%u p%u s%u\n",
         gStorageKextFlags, gStorageCatalogueBefore, gStorageCatalogueAfter,
         gStorageAdded, gStorageLoadResult, gStorageDeviceType, gApfsService, gStorageStage);
  for (;;) IOSleep (1000);
}

static bool gStorageHeartbeatStarted;

static void startStorageHeartbeat ()
{
  if (gStorageHeartbeatStarted) return;
  // The heartbeat is an instrument, and a second thread logging every 400 ms is exactly
  // the kind of thing that moves a crash around. Off by default: opt in with
  // native-ufs-heartbeat=1 only when a reading is needed.
  unsigned enable = 0;
  PE_parse_boot_argn ("native-ufs-heartbeat", &enable, sizeof (enable));
  if (!enable) {
    gStorageHeartbeatStarted = true;
    return;
  }
  gStorageHeartbeatStarted = true;
  thread_t thread = nullptr;
  if (kernel_thread_start (storageHeartbeat, nullptr, &thread) == KERN_SUCCESS) {
    thread_deallocate (thread);
  }
}

// Kext probe/load, runnable before any window device exists: the window construction
// crashes (Kernel data abort inside prepare), and a load attempt scheduled after it
// never executes. This part answers "can the family be loaded at all" on its own.
static bool gStorageKextProbed;

static void bootstrapStorageKext ()
{
  if (gStorageKextProbed) return;
  gStorageKextProbed = true;
  unsigned level = 255;
  PE_parse_boot_argn ("native-ufs-storage", &level, sizeof (level));
  gStorageLevel = level;
  startStorageHeartbeat ();
  gStorageStage = 51;
  gStorageKextFlags = kextFlags ("com.apple.iokit.IOStorageFamily");
  gApfsService = countService ("com_apple_filesystems_apfs");
  IOLog ("NativeUfs: st1 kext=%x apfs=%u\n", gStorageKextFlags, gApfsService);
  if (!level) {
    gStorageStage = 59;
    return;
  }
  // The family is loaded but its personalities never reached the catalogue (the kernel
  // has no kextd to send them), so publish the ones the chain needs. The classes are
  // registered now, which is what makes this safe.
  if (level & 2) {
    gStorageStage = 53;
    if (addBlockStorageDriverPersonality ()) gStorageAdded |= 1;
    if (addGuidSchemePersonality ()) gStorageAdded |= 2;
    if (!gApfsService) {
      if (addContentPersonality ("com.apple.filesystems.apfs", "AppleAPFSContainerScheme",
                                 "7C3457EF-0000-11AA-AA11-00306543ECAC")) gStorageAdded |= 4;
      if (addContentPersonality ("com.apple.filesystems.apfs", "AppleAPFSContainer",
                                 "7C3457EF-0000-11AA-AA11-00306543ECAC")) gStorageAdded |= 8;
    }
    IOLog ("NativeUfs: st3 added=%x\n", gStorageAdded);
    gStorageStage = 59;
    return;
  }
  if (gStorageKextFlags & 2) {
    gStorageStage = 59;
    return;
  }
  gStorageStage = 52;
  // allowDefer=false: a deferred load reports success and never happens here, because
  // the kextd that would service the request never runs in this kernel.
  gStorageLoadResult = static_cast<unsigned> (OSKext::loadKextWithIdentifier ("com.apple.iokit.IOStorageFamily", false));
  gStorageKextFlags = kextFlags ("com.apple.iokit.IOStorageFamily");
  IOLog ("NativeUfs: st2 load=%d kext=%x\n", gStorageLoadResult, gStorageKextFlags);
  gStorageStage = 59;
}

static void bootstrapStorageMatching (NativeUfsBlock *device)
{
  if (gStorageBootstrapped) return;
  gStorageBootstrapped = true;
  unsigned level = 255;
  PE_parse_boot_argn ("native-ufs-storage", &level, sizeof (level));
  gStorageLevel = level;
  startStorageHeartbeat ();
  gStorageStage = 51;
  IOLog ("NativeUfs: stage 1 probe level=%u\n", level);
  if (device) {
    OSString *type = OSString::withCString ("Generic");
    if (type) {
      device->setProperty ("device-type", type);
      type->release ();
    }
  }
  gStorageDeviceType = (device && device->getProperty ("device-type")) ? 1u : 0u;
  gStorageKextFlags = kextFlags ("com.apple.iokit.IOStorageFamily");
  gApfsService = countService ("com_apple_filesystems_apfs");
  gStorageCatalogueBefore = catalogueMatches (device);
  IOLog ("NativeUfs: stage 1 kext=%x cat=%u devtype=%u apfs=%u\n",
         gStorageKextFlags, gStorageCatalogueBefore, gStorageDeviceType, gApfsService);
  if (!level || gStorageCatalogueBefore) return;

  if ((level & 1) && !(gStorageKextFlags & 2)) {
    gStorageStage = 52;
    IOLog ("NativeUfs: stage 2 load kext\n");
    gStorageLoadResult = static_cast<unsigned> (OSKext::loadKextWithIdentifier ("com.apple.iokit.IOStorageFamily", false));
    gStorageKextFlags = kextFlags ("com.apple.iokit.IOStorageFamily");
    IOLog ("NativeUfs: stage 2 load=%d kext=%x\n", gStorageLoadResult, gStorageKextFlags);
  }
  // A match here proves the personality is in the catalogue; whether a driver object
  // appears is then decided by instantiation, which the wait loop counts.
  if (level & 2) {
    gStorageStage = 53;
    IOLog ("NativeUfs: stage 3 block driver\n");
    if (addBlockStorageDriverPersonality ()) gStorageAdded |= 1;
    IOLog ("NativeUfs: stage 3 cat=%u added=%u\n", catalogueMatches (device), gStorageAdded);
  }
  if (level & 4) {
    gStorageStage = 54;
    IOLog ("NativeUfs: stage 4 guid scheme\n");
    if (addGuidSchemePersonality ()) gStorageAdded |= 2;
    IOLog ("NativeUfs: stage 4 added=%u\n", gStorageAdded);
  }
  // Only when APFS does not already answer its own IOResources personality: then the
  // filesystem kext never ran and its container personalities are missing as well.
  if ((level & 8) && !gApfsService) {
    gStorageStage = 55;
    IOLog ("NativeUfs: stage 5 apfs\n");
    if (addContentPersonality ("com.apple.filesystems.apfs", "AppleAPFSContainerScheme",
                               "7C3457EF-0000-11AA-AA11-00306543ECAC")) gStorageAdded |= 4;
    if (addContentPersonality ("com.apple.filesystems.apfs", "AppleAPFSContainer",
                               "7C3457EF-0000-11AA-AA11-00306543ECAC")) gStorageAdded |= 8;
    IOLog ("NativeUfs: stage 5 added=%u\n", gStorageAdded);
  }
  gStorageStage = 59;
  gStorageCatalogueAfter = catalogueMatches (device);
  IOLog ("NativeUfs: storage done cat=%u/%u added=%u\n",
         gStorageCatalogueBefore, gStorageCatalogueAfter, gStorageAdded);
  storageHaltIfRequested ();
}

static unsigned countService (const char *className)
{
  unsigned total = 0;
  OSDictionary *matching = IOService::serviceMatching (className);
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  while (iterator && total < 64) {
    if (!iterator->getNextObject ()) break;
    total++;
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
  return total;
}

static unsigned countMedia (const char *uuid, unsigned *uuidMatches)
{
  unsigned total = 0, matches = 0;
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  while (iterator && total < 64) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) break;
    total++;
    OSString *id = OSDynamicCast (OSString, media->getProperty ("UUID"));
    if (uuid && id && !strcmp (id->getCStringNoCopy (), uuid)) matches++;
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
  if (uuidMatches) *uuidMatches = matches;
  return total;
}

static void captureMissingMedia (const char *uuid)
{
  int used = snprintf (gRootMediaText, sizeof (gRootMediaText), "Waiting for signed UUID %s\n", uuid);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  unsigned count = 0;
  while (iterator && used < int (sizeof (gRootMediaText)) - 180 && count++ < 8) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) break;
    OSString *id = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    used += snprintf (gRootMediaText + used, sizeof (gRootMediaText) - used,
                      "media %.24s bytes=%llx uuid=%.36s\n",
                      bsd ? bsd->getCStringNoCopy () : "-", size ? size->unsigned64BitValue () : 0ULL,
                      id ? id->getCStringNoCopy () : "-");
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
}

// Used only for objects that were never started/published. Started objects
// retain their owner references even on a later failure; clients can have I/O.
static void discardPendingWindow (unsigned index)
{
  NativeUfsBlock *device = gPendingDevice[index];
  if (!device) return;
  if (gPendingAttached[index]) device->detach (gPendingProvider);
  gPendingAttached[index] = false;
  gPendingDevice[index] = nullptr;
  device->release ();
}

static void publishThread (void *, wait_result_t)
{
  struct Operations {
    bool start (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      gRootDiagnostic.stage = "root window service start";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Start);
      gRootDiagnostic.publicationWindow = index;
      IOService *host = gPendingHost ? gPendingHost : gPendingProvider;
      // Virtual call: NativeUfsBlock::start() is what registers the service. The
      // qualified base call tried while hunting the write fault, but that fault was
      // findDrivers(NULL) and had nothing to do with dispatch; without registerService()
      // the storage family can never match this device.
      if (!device->start (host)) return false;
      publishNativeBdev ();
      // Register the storage personalities only after the device completed its
      // own start: loading them first let the driver attach from inside start and
      // deadlock the publisher (p84, start=1 for 30 s).
      if (!storageLatePhase ()) bootstrapStorageMatching (device);
      native_ufs_blocks_published = native_ufs_blocks_published + 1;
      IOLog ("NU4 i%u c%u/%u a%x k%x l%d d%u p%u s%u\n", index,
             gStorageCatalogueBefore, gStorageCatalogueAfter, gStorageAdded, gStorageKextFlags,
             gStorageLoadResult, gStorageDeviceType, gApfsService, gStorageStage);
      IOLog ("NativeUfs: published %s\n", device->windowLabel ());
      return true;
    }
    bool wait (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      const char *uuid = gPendingUuid[index];
      gRootDiagnostic.stage = "media publication";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Media);
      gRootDiagnostic.publicationWindow = index;
      bool ready = false;
      // Diagnostic mode affects reporting only, never the dependency gate.
      // native-ufs-wait-ticks shortens the media wait while bisecting (the published
      // windows never appear as IOMedia on this port, so the full wait only delays
      // the bdevsw root handoff).
      unsigned limit = 300;
      PE_parse_boot_argn ("native-ufs-wait-ticks", &limit, sizeof (limit));
      if (limit > 300) limit = 300;
      for (unsigned tick = 0; tick <= limit; ++tick) {
        gWaitTicks = gWaitTicks + 1;
        gMatchCalls = gMatchCalls + 1;
        // One short line every ten seconds: enough to show the loop is alive, and quiet
        // enough that the earlier mount handoff lines stay on screen for a photograph.
        if ((tick % 10) == 0) {
          IOLog ("NU5 i%u t%u c%u/%u a%x s%u\n", index, tick,
                 gStorageCatalogueBefore, gStorageCatalogueAfter, gStorageAdded, gStorageStage);
        }
        ready = mediaWithUuidExists (uuid, device);
        if (!ready && (tick % 10) == 9) {
          unsigned uuidMatches = 0;
          unsigned driverRegistered = 0, schemeRegistered = 0;
          unsigned driverInstances = classInstances ("IOBlockStorageDriver", &driverRegistered);
          unsigned schemeInstances = classInstances ("IOGUIDPartitionScheme", &schemeRegistered);
          IOLog ("NU9 cls=%u/%u inst=%u/%u\n", driverRegistered, schemeRegistered,
                 driverInstances, schemeInstances);
          unsigned total = countMedia (uuid, &uuidMatches);
          IOLog ("NativeUfs: waiting %s %us media=%u uuidMatches=%u drivers=%u schemes=%u devices=%u cat=%u kext=%x\n",
                 uuid, (tick + 1) / 10, total, uuidMatches,
                 countService ("IOBlockStorageDriver"), countService ("IOGUIDPartitionScheme"),
                 countService ("IOBlockStorageDevice"), catalogueMatches (device), gStorageKextFlags);
        }
        if (ready || tick == limit) break;
        IOSleep (1000);
      }
      IOLog ("NativeUfs: %s volume %s %s\n", device->windowLabel (), uuid, ready ? "ready" : "not ready");
      if (!ready) captureMissingMedia (uuid);
      return ready;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations;
  UfsRootPublicationResult result = ufsPublishRootWindows (operations, gPublicationPlan);
  if (result.phase != UfsRootPhase::Complete) {
    gRootDiagnostic.result = result.phase == UfsRootPhase::Start ? kIOReturnIOError : kIOReturnTimeout;
    const char *reason = result.phase == UfsRootPhase::Start ? "root window service start failed" :
      (!strcmp (gPendingDevice[result.index]->windowLabel (), "SystemOS") ?
       "SystemOS signed volume not found" : "signed root window volume not found");
    IOLog ("NativeUfs: root publisher stopped at window %u; published=%u\n", result.index, result.published);
    // Start failures have already released their unpublished object; do not
    // dereference it here. The media-failure object is deliberately retained.
    rootDiagnosticFailure (reason);
  }
  gPendingProvider->release ();  // Balanced with successful thread dispatch.
}

// Memory snapshot (native-ufs-probe=13): root mode as usual, then, after a delay, the kernel's own
// page counters on the panic screen. Observation only.
extern "C" {
extern unsigned int vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
  vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count;
extern uint32_t vm_page_pages;
extern unsigned int memorystatus_available_pages;
extern int nprocs;
}

static void statsThread (void *argument, wait_result_t)
{
  unsigned delay = static_cast<unsigned> (reinterpret_cast<uintptr_t> (argument));
  IOSleep (delay * 1000);
  panic ("NATIVE_UFS memory snapshot after %u s\n"
         "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
         "available=%u max_mem=%llx procs=%d blocks=%llu\n",
         delay, vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
         vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
         memorystatus_available_pages, max_mem, nprocs, native_ufs_block_reads);
}

// Publication snapshot (native-ufs-probe=15): root mode, then a thread that sleeps a
// few seconds and panics with the publication state. A stalled publisher produces no
// log line at all, so the only way to see where it stopped is to interrupt it from
// another thread.
static void publicationReportThread (void *, wait_result_t)
{
  unsigned delay = 10;
  PE_parse_boot_argn ("native-ufs-report-delay", &delay, sizeof (delay));
  IOSleep (delay * 1000);
  if (storageLatePhase ()) {
    NativeUfsBlock *device = gPendingDevice[0] ? gPendingDevice[0] : gPendingDevice[1];
    bootstrapStorageMatching (device);
    panic ("NATIVE_UFS storage report\nstg=%u cat=%u/%u added=%u kext=%x load=%d devtype=%u apfs=%u lvl=%u\n",
           gStorageStage, gStorageCatalogueBefore, gStorageCatalogueAfter, gStorageAdded,
           gStorageKextFlags, gStorageLoadResult, gStorageDeviceType, gApfsService, gStorageLevel);
  }
  unsigned window = gRootDiagnostic.publicationWindow & 3;
  char wanted[37] = {};
  strlcpy (wanted, gPendingUuid[window], sizeof (wanted));
  // Enumerate from this thread: if the publisher is wedged inside the registry, this
  // thread can still report what the registry holds.
  captureMissingMedia (wanted);
  panic ("NATIVE_UFS publication report\n"
         "pubPhase=%u pubWindow=%u published=%u pending=%u prepared=%u\n"
         "windowBytes=%llx reads=%llu failures=%llu stage=%s\n"
         "waitTicks=%u matchCalls=%u diag=%u start=%u wanted=%s\n"
         "dev=%u,%u,%u,%u,%u,%u bdev=%d node=%lx test=%d magic=%x scan=%d mnt=%d\n"
         "bdevio=%u/%u/%u/%u/%u rootdev=%d major=%d bounce=%d blocks=%llx\n"
         "kext=%x cat=%u/%u added=%u load=%d devtype=%u apfs=%u stg=%u lvl=%u\n%s",
         gRootDiagnostic.publicationPhase, gRootDiagnostic.publicationWindow,
         native_ufs_blocks_published, gPending, gRootDiagnostic.prepared,
         (unsigned long long) gRootDiagnostic.windowBytes,
         (unsigned long long) native_ufs_block_reads,
         (unsigned long long) native_ufs_block_failures, gRootDiagnostic.stage,
         gWaitTicks, gMatchCalls, gRootDiagnosticEnabled ? 1u : 0u, gStartCheckpoint, wanted,
         gDeviceCalls[0], gDeviceCalls[1], gDeviceCalls[2], gDeviceCalls[3], gDeviceCalls[4],
         gDeviceCalls[5], gBdevSlot, (unsigned long) gBdevNode, gBdevTestError, gBdevMagic, gBdevScan, gRootMountError,
         gBdevIoctlSize, gBdevIoctlSetSize, gBdevIoctlCount, gBdevIoctlCalls, gBdevIoctlOther,
         gRootMountDev, gRootMountMajor, gBdevSlot >= 0 ? 1 : 0, (unsigned long long) gBdevBytes,
         gStorageKextFlags, gStorageCatalogueBefore, gStorageCatalogueAfter,
         gStorageAdded, gStorageLoadResult, gStorageDeviceType, gApfsService,
         gStorageStage, gStorageLevel, gRootMediaText);
}

// IOKit class/catalogue state (native-ufs-probe=19): no hardware work. Reports which of the
// storage classes are registered and whether any instance/media exists, which decides whether
// publishing a device can produce an IOMedia for the APFS container scheme to match.
static void iokitStateThread (void *argument, wait_result_t)
{
  unsigned delay = static_cast<unsigned> (reinterpret_cast<uintptr_t> (argument));
  IOSleep (delay * 1000);
  unsigned regDriver = 0, regMedia = 0, regGuid = 0, regScheme = 0, regContainer = 0, regDevice = 0;
  unsigned instDriver = classInstances ("IOBlockStorageDriver", &regDriver);
  unsigned instMedia = classInstances ("IOMedia", &regMedia);
  unsigned instGuid = classInstances ("IOGUIDPartitionScheme", &regGuid);
  unsigned instScheme = classInstances ("AppleAPFSContainerScheme", &regScheme);
  unsigned instContainer = classInstances ("AppleAPFSContainer", &regContainer);
  unsigned instDevice = classInstances ("IOBlockStorageDevice", &regDevice);
  unsigned storageFlags = kextFlags ("com.apple.iokit.IOStorageFamily");
  unsigned apfsFlags = kextFlags ("com.apple.filesystems.apfs");
  unsigned mediaCount = countService ("IOMedia");
  unsigned driverCount = countService ("IOBlockStorageDriver");
  panic ("NATIVE_UFS iokit state\n"
         "storage=%x apfs=%x cat=%u media=%u driver=%u\n"
         "regDrv=%u/%u regMedia=%u/%u regGuid=%u/%u\n"
         "regScheme=%u/%u regContainer=%u/%u regDevice=%u/%u",
         storageFlags, apfsFlags, gIOCatalogue ? 1u : 0u, mediaCount, driverCount,
         regDriver, instDriver, regMedia, instMedia, regGuid, instGuid,
         regScheme, instScheme, regContainer, instContainer, regDevice, instDevice);
}

// Storage discovery report (native-ufs-probe=12): a separate thread waits for the original
// IOStorageFamily to publish IOMedia for the root windows, then shows what exists on the screen.
// It must not run inside the matching thread that is starting NativeUfs.
// BSD block device publication. IOStorageFamily is a prelinked binary whose
// start either blocks the publisher or leaves the driver without an instance,
// so the window is exposed through bdevsw and devfs instead.

static int nativeBdevOpen (dev_t, int, int, struct proc *) { return 0; }
static int nativeBdevClose (dev_t, int, int, struct proc *) { return 0; }
// spec_open() (bsd/miscfs/specfs/spec_vnops.c) opens a block device and then asks it
// for its block size, switches it to 512 to read the block count, and switches it
// back. A GET or SET that fails turns the open into ENXIO, which is what made
// bdevvp() panic and what kept vfs_mountroot() from ever running.
static int nativeBdevIoctl (dev_t, u_long command, caddr_t data, int, struct proc *)
{
  if (!data) return EINVAL;
  gBdevIoctlCalls++;
  switch (command) {
    case DKIOCGETBLOCKSIZE:
      *reinterpret_cast<uint32_t *> (data) = gBdevBlockSize;
      gBdevIoctlSize++;
      return 0;
    case DKIOCSETBLOCKSIZE: {
      uint32_t requested = *reinterpret_cast<uint32_t *> (data);
      gBdevIoctlSetSize++;
      // spec_open only asks for 512 (its DEV_BSIZE) and then back to the size we
      // reported; anything else would silently change the sector geometry.
      return (requested == 512 || requested == gBdevBlockSize) ? 0 : EINVAL;
    }
    case DKIOCGETBLOCKCOUNT:
      *reinterpret_cast<uint64_t *> (data) = gBdevBytes / 512;
      gBdevIoctlCount++;
      return 0;
    default:
      gBdevIoctlOther++;
      return EINVAL;
  }
}
static int nativeBdevPsize (dev_t) { return gBdevBlockSize; }
static uint8_t *gBdevBounce = nullptr;

// The iOS OS window is a whole-disk image: a GPT whose single Apple_APFS partition
// holds the container the kernel mounts. The bdevsw device must start at that
// partition, not at the partition table in front of it, or APFS reads the GPT and
// reports ENOENT. Windows whose first blocks are not such a GPT are left alone.
// The disk image inside the window uses 512-byte GPT sectors (the dmg format), while
// hostRead4k moves 4096-byte blocks: the GPT header sits at byte 512 of the window and
// the entry array at byte 1024, both inside the first 4 KiB block.
static void skipLeadingApfsGpt (uint64_t *absolute, uint64_t *bytes)
{
  static const uint8_t apfsType[16] = { 0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
                                        0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac };
  if (!absolute || !bytes || !*bytes || (*absolute & 4095)) return;
  const uint64_t baseLba = *absolute / 4096;
  if (hostRead4k (0, baseLba, 1, gScratch) != kIOReturnSuccess) {
    IOLog ("NativeUfs: gpt probe: first block read failed\n");
    return;
  }
  if (memcmp (gScratch + 512, "EFI PART", 8) != 0) return;
  uint64_t entriesLba = le64 (gScratch + 512 + 72);
  uint32_t count = le32 (gScratch + 512 + 80), size = le32 (gScratch + 512 + 84);
  if (!count || !size || size > 512 || entriesLba < 2) return;
  uint64_t entriesByte = entriesLba * 512;
  uint64_t needBytes = entriesByte + uint64_t (count) * size;
  uint32_t blocks = static_cast<uint32_t> ((needBytes + 4095) / 4096);
  if (!blocks || blocks > kDataMax / 4096) return;
  if (hostRead4k (0, baseLba, blocks, gScratch) != kIOReturnSuccess) return;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *entry = gScratch + entriesByte + uint64_t (i) * size;
    if (memcmp (entry, apfsType, 16) != 0) continue;
    uint64_t first = le64 (entry + 32), last = le64 (entry + 40);
    if (last <= first) return;
    uint64_t offset = first * 512, length = (last - first + 1) * 512;
    if (offset >= *bytes || length > *bytes - offset) return;
    IOLog ("NativeUfs: bdev skips GPT, APFS partition at +%llx (%llu bytes)\n",
           static_cast<unsigned long long> (offset), static_cast<unsigned long long> (length));
    *absolute += offset;
    *bytes = length;
    return;
  }
  IOLog ("NativeUfs: gpt probe: no Apple_APFS entry among %u\n", count);
}


// One lock for the strategy read and write paths: the bounce buffer and the geometry are shared
// (audit 2026-11-15 P1-6), and a bdevsw disk is called concurrently by the page cache.
static IOLock *gBdevLock = nullptr;
static volatile uint64_t gBdevLastFailLba = 0;
static volatile uint32_t gBdevLastFailBytes = 0;
static volatile int32_t gBdevLastFailStatus = 0;
static volatile uint32_t gBdevLastFailWrite = 0;
static volatile unsigned gBdevReads, gBdevWrites, gBdevFailures;

static int nativeBdevRead (uint64_t block, uint32_t count, uint8_t *data)
{
  const bool invalid = !gBdevBounce || !data || !count || (count & 511) ||
      block >= gBdevBytes / 512 || count > gBdevBytes - block * 512;
  if (invalid) {
    // One line that says which precondition failed and with what geometry (EINVAL otherwise has
    // no visible cause on this board; p366 showed "op=0 err=22 lba=15ee8 bytes=4096").
    IOLog ("NATIVE_UFS bdev: invalid bounce=%d data=%d count=%u align=%u block=%llx"
           " limit=%llx bytes=%llx\n",
           gBdevBounce ? 1 : 0, data ? 1 : 0, count, count & 511,
           static_cast<unsigned long long> (block),
           static_cast<unsigned long long> (gBdevBytes / 512),
           static_cast<unsigned long long> (gBdevBytes));
    return EINVAL;
  }
  uint64_t start = block * 512, done = 0;
  while (done < count) {
    uint64_t lba = (gBdevAbsolute + start + done) / 4096;
    uint64_t skip = (gBdevAbsolute + start + done) % 4096;
    uint64_t blocks = (skip + count - done + 4095) / 4096;
    if (blocks > kDataMax / 4096) blocks = kDataMax / 4096;
    if (hostRead4k (0, lba, static_cast<uint32_t> (blocks), gBdevBounce) != kIOReturnSuccess)
      return EIO;
    uint64_t usable = blocks * 4096 - skip;
    if (usable > count - done) usable = count - done;
    memcpy (data + done, gBdevBounce + skip, static_cast<size_t> (usable));
    done += usable;
    __atomic_fetch_add (&native_ufs_block_reads, 1, __ATOMIC_RELAXED);
  }
  return 0;
}

static void nativeBdevStrategy (struct buf *bp)
{
  const uint64_t block = buf_blkno (bp);
  const uint32_t count = buf_count (bp);
  const bool writing = (buf_flags (bp) & B_READ) == 0;
  int error = 0;

  // The page cache hands a strategy routine a B_PAGEIO buffer whose data area is a UPL, not a
  // kernel pointer: buf_dataptr() is NULL for those (p367: "invalid bounce=1 data=0"). Map the
  // buffer first, exactly like the memory-device driver (bsd/dev/memdev.c mdevstrategy).
  caddr_t vaddr = nullptr;
  if (buf_map (bp, &vaddr) != 0 || vaddr == nullptr) {
    buf_seterror (bp, EIO);
    buf_setresid (bp, count);
    __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
    buf_biodone (bp);
    return;
  }

  if (writing) {
    // The UFS window is writable (hostMediaOpen in the probe), so serve writes through the same
    // window writer the IOBlockStorageDevice path uses: WRITE(10) with FUA, read-modify-write
    // for non-4096-byte requests. Without this a bdevsw root (fix A) can only read, and APFS
    // cannot replay its journal or update the container it mounted read-write.
    struct WriteOperations {
      caddr_t data;
      void lock () { if (gBdevLock) IOLockLock (gBdevLock); }
      void unlock () { if (gBdevLock) IOLockUnlock (gBdevLock); }
      IOReturn read (uint64_t lba, uint32_t blocks, void *out) {
        return hostRead4k (0, lba, blocks, out);
      }
      IOReturn write (uint64_t lba, uint32_t blocks, const void *in) {
        return hostWrite4k (0, lba, blocks, in, true);
      }
      uint64_t gather (uint64_t offset, void *dst, uint64_t length) {
        memcpy (dst, reinterpret_cast<const uint8_t *> (data) + offset, length);
        return length;
      }
    } writeOperations = { vaddr };
    UfsWindowResult outcome = ufsWriteWindow (writeOperations, gBdevAbsolute, gBdevBytes, block, count, gBdevBounce);
    if (outcome.status != kIOReturnSuccess) error = EIO;
  } else {
    if (gBdevLock) IOLockLock (gBdevLock);
    error = nativeBdevRead (block, count, reinterpret_cast<uint8_t *> (vaddr));
    if (gBdevLock) IOLockUnlock (gBdevLock);
  }
  buf_unmap (bp);

  if (error) {
    __atomic_fetch_add (&gBdevFailures, 1, __ATOMIC_RELAXED);
    gBdevLastFailLba = block;
    gBdevLastFailBytes = count;
    gBdevLastFailStatus = error;
    gBdevLastFailWrite = writing ? 1 : 0;
    // The screen is the only channel; log the failing request so a photo shows where it stops.
    IOLog ("NATIVE_UFS bdev: op=%u err=%d lba=%llx bytes=%u\n", writing ? 1u : 0u, error,
           static_cast<unsigned long long> (block), count);
    buf_seterror (bp, error);
    buf_setresid (bp, count);
    __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
  } else {
    if (writing) __atomic_fetch_add (&gBdevWrites, 1, __ATOMIC_RELAXED);
    else __atomic_fetch_add (&gBdevReads, 1, __ATOMIC_RELAXED);
    buf_setresid (bp, 0);
  }
  buf_biodone (bp);
}

static void publishNativeBdev (bool claimRoot)
{
  if (gBdevSlot != -2) return;
  static struct bdevsw operations = { nativeBdevOpen, nativeBdevClose, nativeBdevStrategy,
                                      nativeBdevIoctl, enodev, nativeBdevPsize, D_DISK };
  int slot = bdevsw_add (-1, &operations);
  gBdevSlot = slot;
  if (slot < 0) return;
  // Export the slot for the kernel's IOFindBSDRoot (peridot-bdevsw-root.patch).
  native_ufs_bdev_slot = slot;
  // gBdevAbsolute/gBdevBytes must be set by the caller before publishing: they are the window
  // this bdevsw disk serves. publishNativeBdev() no longer guesses them (probe 18 used to hand
  // the kernel a 0/0 device; audit 2026-11-15, P1-1).
  gBdevBounce = static_cast<uint8_t *> (IOMalloc (kDataMax));
  if (!gBdevLock) gBdevLock = IOLockAlloc ();
  thread_t thread = nullptr;
  if (kernel_thread_start (bdevTestThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate (thread);
  dev_t dev = makedev (slot, 0);
  // Only the root-handoff paths publish this device as the kernel's root; IOFindBSDRoot
  // (peridot-bdevsw-root.patch) adopts rootdev when it is a registered bdevsw disk. The media
  // publication (probe 18/20) keeps the root it already has - the md ramdisk with the APFS
  // container - instead of replacing it with this window (audit P1-1).
  if (claimRoot) rootdev = dev;
  native_ufs_bdev_absolute = gBdevAbsolute;
  native_ufs_bdev_bytes = gBdevBytes;
  // "disk0" belongs to the IOMedia chain published by startApfsMediaRoot(); a second node with
  // that name fails with EEXIST (seen on p361: "devfs: disk0: name slot allocation failed").
  gBdevNode = reinterpret_cast<uintptr_t> (devfs_make_node (dev, DEVFS_BLOCK, 0, 0, 0644, "nu0"));
  if (!gBdevNode) IOLog ("NATIVE_UFS bdev: devfs_make_node(nu0) failed\n");
  // native-ufs-mountroot=1 runs the real mountroot once the bdevsw contract is met.
  // Off by default: bdevvp() panics when the open is refused and a panic here resets
  // the device.
  unsigned mountroot = 0;
  PE_parse_boot_argn ("native-ufs-mountroot", &mountroot, sizeof (mountroot));
  if (mountroot == 1) {
    if (kernel_thread_start (mountRootThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate (thread);
  }
}

// ---- APFS root from memory over the userdata partition (native-ufs-probe=18) -----
// The iPadOS 17.7.11 RestoreRamDisk is a bare APFS container whose only volume carries
// no role and no root snapshot, so APFS refuses it on an ordinary block device. The real
// device mounts it through the restore path, which APFS takes for a memory device (the
// ramdisk iBoot hands over as rd=md0). A boot image cannot carry the 184 MiB container:
// the bootloader caps the boot-image ramdisk at the 96 MiB boot partition. So the kernel
// reads the container from the start of the userdata partition into wired memory, registers
// that memory as md(N) and leaves it as rootdev; IOFindBSDRoot adopts it (peridot-bdevsw-root
// .patch) and bsd_init's own vfs_mountroot mounts APFS from it.
extern "C" dev_t mdevadd (int devid, uint64_t base, unsigned int size, int phys);
static const uint64_t kApfsMdBytes = 184549376ULL;      // 4096 x 45056, the whole container
static uint8_t *gApfsMdMemory = nullptr;

static bool startApfsMdRoot ()
{
  const char *why = "";
  if (!hostBringUp (&why)) {
    panic ("NATIVE_UFS apfs-md: bring-up failed: %s", why);
  }
  uint32_t lun = 0;
  uint64_t first = 0, blocks = 0;
  if (!native_ufs_part_find ("userdata", &lun, &first, &blocks)) {
    panic ("NATIVE_UFS apfs-md: no userdata partition in the GPT");
  }
  IOLog ("NATIVE_UFS apfs-md: userdata lun=%u first=%llx blocks=%llx\n", lun,
         static_cast<unsigned long long> (first), static_cast<unsigned long long> (blocks));
  if (blocks * 4096ULL < kApfsMdBytes) {
    panic ("NATIVE_UFS apfs-md: userdata is smaller than the container");
  }
  gApfsMdMemory = static_cast<uint8_t *> (IOMalloc (kApfsMdBytes + 4096));
  if (!gApfsMdMemory) {
    panic ("NATIVE_UFS apfs-md: cannot allocate %llu bytes",
           static_cast<unsigned long long> (kApfsMdBytes));
  }
  uint8_t *memory = reinterpret_cast<uint8_t *> ((reinterpret_cast<uint64_t> (gApfsMdMemory) + 4095) & ~4095ULL);
  if (!native_ufs_part_read (lun, first, static_cast<uint32_t> (kApfsMdBytes / 4096), memory)) {
    panic ("NATIVE_UFS apfs-md: reading the container from userdata failed");
  }
  if (le32 (memory + 0x20) != 0x4253584e) {
    panic ("NATIVE_UFS apfs-md: no NXSB at +0x20 (magic %08x)", le32 (memory + 0x20));
  }
  // Register only the container range, not the whole partition: hostInWindow() is the last
  // guard before a WRITE(10), and handing it all ~481 GiB of userdata would let any offset error
  // below reach the GPT, the container or another partition (audit 2026-11-15, P1-4).
  if (!hostMediaOpen (first, kApfsMdBytes / 4096)) {
    panic ("NATIVE_UFS apfs-md: cannot open the writable media window");
  }
  dev_t device = mdevadd (-1, reinterpret_cast<uint64_t> (memory) >> 12,
                          static_cast<unsigned> (kApfsMdBytes >> 12), 0);
  if (device == static_cast<dev_t> (-1)) {
    panic ("NATIVE_UFS apfs-md: mdevadd refused the memory device");
  }
  rootdev = device;
  IOLog ("NATIVE_UFS apfs-md: container at %llx read into md%d dev=%d major=%d minor=%d, rootdev set\n",
         static_cast<unsigned long long> (first * 4096ULL), static_cast<int> (minor (device)),
         static_cast<int> (device), major (device), minor (device));
  return true;
}

// ---- APFS container as a published IOBlockStorageDevice (native-ufs-probe=20) ----
// p324 proved every storage class is registered once offline-apfs-load=1 loads the APFS
// kext and IOStorageFamily (regDrv=1 regMedia=1 regScheme=1 ... with zero instances). So the
// chain only needs a device instance: publish the APFS container window as an
// IOBlockStorageDevice, let IOBlockStorageDriver create the IOMedia, and report what appears,
// including the APFS container scheme instances and the media's BSD name.
static NativeUfsBlock *gApfsMediaDevice = nullptr;
// Absolute first LBA of the published window, recorded for the probe 34 data-path test
// (the block-device block numbers are relative to the window, the kernel transport wants
// absolute LBAs).
static uint64_t gApfsMediaFirstLba = 0;
// The install target (probe 37): a second, large userdata window published as its own
// IOBlockStorageDevice, used to prove writes/throughput before asr/newfs_apfs drive it.
static NativeUfsBlock *gInstallTargetDevice = nullptr;
static uint64_t gInstallTargetFirstLba = 0, gInstallTargetBytes = 0;
// Probe 38: copy the 184 MiB APFS container from the start of userdata to the install target.
static uint64_t gInstallCopySourceLba = 0, gInstallCopyTargetLba = 0;

static void apfsMediaReportThread (void *argument, wait_result_t)
{
  IOSleep (static_cast<unsigned> (reinterpret_cast<uintptr_t> (argument)) * 1000);
  IOLog ("NUA checkpoint=%u calls=%u,%u,%u,%u,%u,%u reads=%llu failures=%llu\n", gStartCheckpoint,
         gDeviceCalls[0], gDeviceCalls[1], gDeviceCalls[2], gDeviceCalls[3], gDeviceCalls[4], gDeviceCalls[5],
         (unsigned long long) native_ufs_block_reads, (unsigned long long) native_ufs_block_failures);
  unsigned driverReg = 0, mediaReg = 0, schemeReg = 0, containerReg = 0;
  unsigned driverInst = classInstances ("IOBlockStorageDriver", &driverReg);
  unsigned mediaInst = classInstances ("IOMedia", &mediaReg);
  unsigned schemeInst = classInstances ("AppleAPFSContainerScheme", &schemeReg);
  unsigned containerInst = classInstances ("AppleAPFSContainer", &containerReg);
  unsigned mediaCount = countService ("IOMedia");
  unsigned driverCount = countService ("IOBlockStorageDriver");
  char *text = static_cast<char *> (IOMalloc (1800));
  int used = 0;
  if (text) {
    used = snprintf (text, 1800, "NATIVE_UFS apfs media\npublished=%u media=%u driver=%u\n"
                     "regDrv=%u/%u regMedia=%u/%u regScheme=%u/%u regCont=%u/%u\n",
                     native_ufs_blocks_published, mediaCount, driverCount,
                     driverReg, driverInst, mediaReg, mediaInst, schemeReg, schemeInst,
                     containerReg, containerInst);
    OSDictionary *matching = IOService::serviceMatching ("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
    while (iterator && used < 1500) {
      IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
      if (!media) break;
      OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
      OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
      OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
      used += snprintf (text + used, 1800 - used, "%s %s size=%llx\n",
                        bsd ? bsd->getCStringNoCopy () : "-",
                        content ? content->getCStringNoCopy () : "-",
                        size ? size->unsigned64BitValue () : 0ULL);
    }
    if (iterator) iterator->release ();
    if (matching) matching->release ();
    panic ("%s", text);
  }
  panic ("NATIVE_UFS apfs media: no report buffer");
}

// Publish the APFS container window and start the storage chain. Returns false when the
// transport or the device object cannot be prepared.
// Kernel block-device helpers used by APFS's container_rootmount: it bdevvp()s the device,
// then VNOP_FSYNC()s it and maps any error to ENOTBLK. The probe repeats those calls on the
// media device we publish, so the real error is visible instead of the mapped 15.
struct vnode;
struct vfs_context;
extern "C" int bdevvp (dev_t, struct vnode **);
extern "C" int VNOP_FSYNC (struct vnode *, int, struct vfs_context *);
extern "C" struct vfs_context *vfs_context_kernel (void);
// Runtime trust cache query (native-ufs-probe=31): the kernel exports query_trust_cache()
// (kern_trustcache.c), which is exactly what pmap/AMFI use for cdhash membership. Querying the
// dyld and launchd hashes shows whether the DarwinBoot bundle really reached the runtime cache.
struct PeridotQueryToken { const void *trustCache; const void *trustCacheEntry; };
extern "C" unsigned int query_trust_cache (unsigned char query_type, const unsigned char *cdhash,
                                            void *query_token);
static const unsigned char kDyldCdhash[20] = { 0xb4, 0x79, 0xf9, 0x0e, 0x05, 0x54, 0x53, 0x14, 0xcb, 0x84,
                                               0x34, 0x10, 0xc3, 0xd9, 0x8e, 0xf8, 0x4d, 0x92, 0x67, 0x10 };
static const unsigned char kLaunchdCdhash[20] = { 0x1d, 0x52, 0xc6, 0x97, 0x47, 0xdf, 0xfc, 0x4b, 0x19, 0x37,
                                                  0xd2, 0x41, 0xbd, 0xbc, 0x9c, 0xee, 0x3b, 0x22, 0xdf, 0x18 };
static const unsigned char kBogusCdhash[20] = { 0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef, 0xde, 0xad,
                                                0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef };
static void trustQueryThread (void *, wait_result_t)
{
  IOSleep (3000);
  char *text = static_cast<char *> (IOMalloc (900));
  if (!text) panic ("NATIVE_UFS trustq: no buffer");
  int used = 0;
  used += snprintf (text + used, 900, "NATIVE_UFS trustq\n");
  for (unsigned type = 0; type <= 2 && used < 800; type++) {
    PeridotQueryToken token = { nullptr, nullptr };
    unsigned int rDyld = query_trust_cache (static_cast<unsigned char> (type), kDyldCdhash, &token);
    PeridotQueryToken token2 = { nullptr, nullptr };
    unsigned int rLaunchd = query_trust_cache (static_cast<unsigned char> (type), kLaunchdCdhash, &token2);
    PeridotQueryToken token3 = { nullptr, nullptr };
    unsigned int rBogus = query_trust_cache (static_cast<unsigned char> (type), kBogusCdhash, &token3);
    used += snprintf (text + used, 900 - used, "t%u dyld=%u launchd=%u bogus=%u\n",
                      type, rDyld, rLaunchd, rBogus);
  }
  panic ("%s", text);
}
// Root filesystem report (native-ufs-probe=32): the boot screen cannot tell mockfs from apfs,
// so this asks the exported vfs helpers which filesystem actually owns the root vnode.
extern "C" {
// <sys/mount.h> provides vfs_name()/vfs_statfs(); vnode_mount is kernel-internal.
extern struct vnode *rootvnode;
extern struct mount *vnode_mount (struct vnode *);
}
static void rootFsReportThread (void *, wait_result_t)
{
  IOSleep (15000);
  char fsname[64] = "?";
  struct vnode *rv = rootvnode;
  struct mount *mp = rv ? vnode_mount (rv) : nullptr;
  if (mp) vfs_name (mp, fsname);
  panic ("NATIVE_UFS rootfs fs=%s rootvnode=%p mount=%p", fsname,
         reinterpret_cast<void *> (rv), reinterpret_cast<void *> (mp));
}

// ---- Block-device data path self test (native-ufs-probe=34) ---------------------------
// The p356 write test called hostWrite4k() directly - the kernel transport, not the path
// userland and IOBlockStorageDriver use. The install goal needs the other path: a write
// that arrives through IOBlockStorageDevice::doAsyncReadWrite(). This probe drives that
// exact entry with an IOMemoryDescriptor, reads the block back through the kernel transport
// and restores the original bytes. It is the regression test for audit 2026-11-15 P0-1
// (the hostSyncCache gate made every such write return kIOReturnNotReady).
struct DeviceIoCompletion { volatile unsigned status; volatile unsigned done; };
static void deviceIoComplete (void *, void *parameter, IOReturn status, UInt64 actual)
{
  DeviceIoCompletion *completion = static_cast<DeviceIoCompletion *> (parameter);
  completion->status = static_cast<unsigned> (status);
  completion->done = static_cast<unsigned> (actual);
}
static IOReturn deviceWriteBlock (NativeUfsBlock *device, uint64_t block, uint32_t bytes, const void *source)
{
  IOMemoryDescriptor *buffer = IOMemoryDescriptor::withAddress (const_cast<void *> (source), bytes, kIODirectionOut);
  if (!buffer) return kIOReturnNoMemory;
  IOReturn status = buffer->prepare (kIODirectionOut);
  if (status != kIOReturnSuccess) { buffer->release (); return status; }
  DeviceIoCompletion done = { 0xffffffffU, 0 };
  IOStorageCompletion completion;
  completion.target = nullptr;
  completion.action = deviceIoComplete;
  completion.parameter = &done;
  status = device->doAsyncReadWrite (buffer, block, bytes / 512U, nullptr, &completion);
  buffer->complete ();
  buffer->release ();
  if (status != kIOReturnSuccess) return status;
  return static_cast<IOReturn> (done.status);
}
static void deviceWriteTestThread (void *, wait_result_t)
{
  IOSleep (6000);                                   // let IOBlockStorageDriver open the device
  NativeUfsBlock *device = gApfsMediaDevice;
  if (!device || !gApfsMediaFirstLba) panic ("NATIVE_UFS devio: no published device");
  const uint32_t bytes = 4096;
  const uint64_t block = 64;                        // 32 KiB into the window = first_lba + 8 (probe 33 block)
  const uint64_t lba = gApfsMediaFirstLba + block / 8;
  uint8_t *original = static_cast<uint8_t *> (IOMalloc (bytes));
  uint8_t *work = static_cast<uint8_t *> (IOMalloc (bytes));
  uint8_t *readback = static_cast<uint8_t *> (IOMalloc (bytes));
  if (!original || !work || !readback) panic ("NATIVE_UFS devio: no buffers");
  IOReturn readRc = hostRead4k (0, lba, 1, original);
  memcpy (work, original, bytes);
  for (uint32_t i = 0; i < bytes; i += 64) work[i] = static_cast<uint8_t> (work[i] ^ 0x5a);
  IOReturn writeRc = deviceWriteBlock (device, block, bytes, work);
  memset (readback, 0, bytes);
  IOReturn backRc = hostRead4k (0, lba, 1, readback);
  int matched = (backRc == kIOReturnSuccess) && !memcmp (readback, work, bytes);
  IOReturn restoreRc = deviceWriteBlock (device, block, bytes, original);
  memset (readback, 0, bytes);
  IOReturn checkRc = hostRead4k (0, lba, 1, readback);
  int restored = (checkRc == kIOReturnSuccess) && !memcmp (readback, original, bytes);
  panic ("NATIVE_UFS devio block=%d read=%d write=%d reread=%d match=%d restore=%d verify=%d ok=%d",
         static_cast<int> (block), static_cast<int> (readRc), static_cast<int> (writeRc),
         static_cast<int> (backRc), matched, static_cast<int> (restoreRc), static_cast<int> (checkRc),
         (readRc == 0 && writeRc == 0 && matched && restored) ? 1 : 0);
}

// ---- Install target window + write throughput + root flags (native-ufs-probe=37) -------
// The next stage of the goal: a large, dedicated target the install tools can format and
// restore into. This probe registers a second UFS window (1 GiB into userdata, up to 8 GiB),
// publishes it as an IOBlockStorageDevice, writes/reads/restores a page and 4 MiB through
// that device entry, and reports the root mount flags so we know whether the UFS root is
// writable by userland yet. Everything is reported in one panic line (32-bit specifiers).
static void installTargetTestThread (void *, wait_result_t)
{
  IOSleep (15000);                                  // root mounted + launchd started
  uint32_t rootFlags = 0xdeadbeefU;
  struct vnode *rv = rootvnode;
  struct mount *mp = rv ? vnode_mount (rv) : nullptr;
  if (mp) {
    struct vfsstatfs *sf = vfs_statfs (mp);
    if (sf) rootFlags = static_cast<uint32_t> (sf->f_flags);
  }
  NativeUfsBlock *device = gInstallTargetDevice;
  if (!device || !gInstallTargetBytes) panic ("NATIVE_UFS install: no target device");
  const uint32_t page = 4096;
  uint8_t *original = static_cast<uint8_t *> (IOMalloc (page));
  uint8_t *work = static_cast<uint8_t *> (IOMalloc (page));
  uint8_t *readback = static_cast<uint8_t *> (IOMalloc (page));
  if (!original || !work || !readback) panic ("NATIVE_UFS install: no page buffers");
  IOReturn pageRead = hostRead4k (0, gInstallTargetFirstLba, 1, original);
  memcpy (work, original, page);
  for (uint32_t i = 0; i < page; i += 64) work[i] = static_cast<uint8_t> (work[i] ^ 0x5a);
  IOReturn pageWrite = deviceWriteBlock (device, 0, page, work);
  memset (readback, 0, page);
  IOReturn pageBack = hostRead4k (0, gInstallTargetFirstLba, 1, readback);
  int pageMatch = pageBack == kIOReturnSuccess && !memcmp (readback, work, page);
  IOReturn pageRestore = deviceWriteBlock (device, 0, page, original);
  memset (readback, 0, page);
  IOReturn pageVerify = hostRead4k (0, gInstallTargetFirstLba, 1, readback);
  int pageRestored = pageVerify == kIOReturnSuccess && !memcmp (readback, original, page);
  const uint32_t bulk = 4U << 20;
  const uint32_t chunk = 128U << 10;
  uint8_t *bulkOriginal = static_cast<uint8_t *> (IOMalloc (bulk));
  uint8_t *bulkWork = static_cast<uint8_t *> (IOMalloc (bulk));
  uint8_t *bulkBack = static_cast<uint8_t *> (IOMalloc (chunk));
  if (!bulkOriginal || !bulkWork || !bulkBack) panic ("NATIVE_UFS install: no bulk buffers");
  int bulkReadOk = 1;
  for (uint32_t off = 0; off < bulk; off += chunk) {
    if (hostRead4k (0, gInstallTargetFirstLba + off / 4096, chunk / 4096, bulkOriginal + off) != kIOReturnSuccess) {
      bulkReadOk = 0;
      break;
    }
  }
  memcpy (bulkWork, bulkOriginal, bulk);
  for (uint32_t i = 0; i < bulk; i += 64) bulkWork[i] = static_cast<uint8_t> (bulkWork[i] ^ 0xa5);
  uint64_t t0 = mach_absolute_time ();
  IOReturn bulkWrite = bulkReadOk ? deviceWriteBlock (device, 0, bulk, bulkWork) : kIOReturnError;
  uint64_t t1 = mach_absolute_time ();
  uint64_t ns = 0;
  absolutetime_to_nanoseconds (t1 - t0, &ns);
  uint32_t bulkMs = static_cast<uint32_t> (ns / 1000000ULL);
  int bulkMatch = 0, bulkRestored = 0;
  IOReturn bulkRestore = kIOReturnError;
  if (bulkReadOk) {
    bulkMatch = 1;
    for (uint32_t off = 0; off < bulk; off += chunk) {
      if (hostRead4k (0, gInstallTargetFirstLba + off / 4096, chunk / 4096, bulkBack) != kIOReturnSuccess ||
          memcmp (bulkBack, bulkWork + off, chunk)) {
        bulkMatch = 0;
        break;
      }
    }
    bulkRestore = deviceWriteBlock (device, 0, bulk, bulkOriginal);
    if (bulkRestore == kIOReturnSuccess) {
      bulkRestored = 1;
      for (uint32_t off = 0; off < bulk; off += chunk) {
        if (hostRead4k (0, gInstallTargetFirstLba + off / 4096, chunk / 4096, bulkBack) != kIOReturnSuccess ||
            memcmp (bulkBack, bulkOriginal + off, chunk)) {
          bulkRestored = 0;
          break;
        }
      }
    }
  }
  panic ("NATIVE_UFS install mib=%d lba=%d page=%d/%d/%d/%d rootflags=%x bulkread=%d bulkwrite=%d bulkms=%d bulkmatch=%d bulkrestore=%d ok=%d",
         static_cast<int> (gInstallTargetBytes >> 20), static_cast<int> (gInstallTargetFirstLba),
         static_cast<int> (pageRead), static_cast<int> (pageWrite), static_cast<int> (pageBack),
         static_cast<int> (pageRestore), rootFlags, bulkReadOk, static_cast<int> (bulkWrite),
         static_cast<int> (bulkMs), bulkMatch, bulkRestored,
         (pageRead == 0 && pageWrite == 0 && pageMatch && pageRestored && bulkReadOk &&
          bulkWrite == 0 && bulkMs > 0 && bulkMatch && bulkRestored) ? 1 : 0);
}

// ---- Install target copy (native-ufs-probe=38) ---------------------------------------
// Copy the whole 184 MiB RestoreRamDisk APFS container from the start of userdata to the
// install-target window (1 GiB into userdata), verify every 128 KiB chunk and report the
// elapsed time. Probe 39 then boots from that target window.
static void installCopyThread (void *, wait_result_t)
{
  IOSleep (12000);
  uint8_t *source = static_cast<uint8_t *> (IOMalloc (kDataMax));
  uint8_t *target = static_cast<uint8_t *> (IOMalloc (kDataMax));
  if (!source || !target) panic ("NATIVE_UFS install copy: no buffers");
  const uint64_t chunkBlocks = kDataMax / 4096;
  const uint64_t chunks = kApfsMdBytes / kDataMax;
  unsigned copyErrors = 0, verifyErrors = 0;
  uint64_t t0 = mach_absolute_time ();
  for (uint64_t i = 0; i < chunks; i++) {
    const uint64_t off = i * chunkBlocks;
    if (hostRead4k (0, gInstallCopySourceLba + off, static_cast<uint32_t> (chunkBlocks), source) != kIOReturnSuccess ||
        hostWrite4k (0, gInstallCopyTargetLba + off, static_cast<uint32_t> (chunkBlocks), source, true) != kIOReturnSuccess) {
      copyErrors++;
      break;
    }
  }
  uint64_t t1 = mach_absolute_time ();
  for (uint64_t i = 0; i < chunks; i++) {
    const uint64_t off = i * chunkBlocks;
    if (hostRead4k (0, gInstallCopySourceLba + off, static_cast<uint32_t> (chunkBlocks), source) != kIOReturnSuccess ||
        hostRead4k (0, gInstallCopyTargetLba + off, static_cast<uint32_t> (chunkBlocks), target) != kIOReturnSuccess ||
        memcmp (source, target, kDataMax)) {
      verifyErrors++;
      break;
    }
  }
  uint64_t ns = 0;
  absolutetime_to_nanoseconds (t1 - t0, &ns);
  panic ("NATIVE_UFS install copy mib=%d chunks=%d copyerr=%d verifyerr=%d ms=%d ok=%d",
         static_cast<int> (kApfsMdBytes >> 20), static_cast<int> (chunks), static_cast<int> (copyErrors),
         static_cast<int> (verifyErrors), static_cast<int> (ns / 1000000ULL),
         (copyErrors == 0 && verifyErrors == 0) ? 1 : 0);
}

// ---- Install tool run (native-ufs-probe=40) -------------------------------------------
// The init program is taken from the peridot-init-path boot argument (normally
// /System/Library/Filesystems/apfs.fs/newfs_apfs, with peridot-init-arg1=/dev/nu0). This thread
// waits for a fresh APFS container superblock on the install target and reports it, because the
// init program has no console file descriptors.
static void initToolVerifyThread (void *, wait_result_t)
{
  IOSleep (18000);
  uint8_t *block = static_cast<uint8_t *> (IOMalloc (4096));
  if (!block) panic ("NATIVE_UFS init: no verify buffer");
  uint32_t found = 0, offset = 0, head = 0;
  uint32_t readErrors = 0;
  for (int attempt = 0; attempt < 12 && !found; attempt++) {
    if (hostRead4k (0, gBdevAbsolute / 4096, 1, block) != kIOReturnSuccess) {
      readErrors++;
      IOSleep (2000);
      continue;
    }
    head = (static_cast<uint32_t> (block[0]) << 24) | (static_cast<uint32_t> (block[1]) << 16) |
           (static_cast<uint32_t> (block[2]) << 8) | static_cast<uint32_t> (block[3]);
    for (uint32_t i = 0; i + 3 < 4096; i++) {
      if (block[i] == 'N' && block[i + 1] == 'X' && block[i + 2] == 'S' && block[i + 3] == 'B') {
        found = 1;
        offset = i;
        break;
      }
    }
    if (!found) IOSleep (2000);
  }
  panic ("NATIVE_UFS init: found=%d off=%d head=%08x reads=%d ok=%d",
         static_cast<int> (found), static_cast<int> (offset), head, static_cast<int> (readErrors),
         found ? 1 : 0);
}


// ---- Install target verify (native-ufs-probe=41) --------------------------------------
// Read-only check used after probe 40 ran an Apple tool (newfs_apfs) against the install
// target: the boot image has no init override, so this probe only brings the transport up and
// reports whether a fresh APFS container superblock is on the target.
static void targetCheckThread (void *, wait_result_t)
{
  IOSleep (3000);
  uint8_t *block = static_cast<uint8_t *> (IOMalloc (4096));
  if (!block) panic ("NATIVE_UFS check: no buffer");
  uint32_t found = 0, offset = 0, head = 0;
  for (int attempt = 0; attempt < 8 && !found; attempt++) {
    if (hostRead4k (0, gBdevAbsolute / 4096, 1, block) != kIOReturnSuccess) {
      IOSleep (1000);
      continue;
    }
    head = (static_cast<uint32_t> (block[0]) << 24) | (static_cast<uint32_t> (block[1]) << 16) |
           (static_cast<uint32_t> (block[2]) << 8) | static_cast<uint32_t> (block[3]);
    for (uint32_t i = 0; i + 3 < 4096; i++) {
      if (block[i] == 'N' && block[i + 1] == 'X' && block[i + 2] == 'S' && block[i + 3] == 'B') {
        found = 1;
        offset = i;
        break;
      }
    }
    if (!found) IOSleep (1000);
  }
  panic ("NATIVE_UFS check: found=%d off=%d head=%08x ok=%d",
         static_cast<int> (found), static_cast<int> (offset), head, found ? 1 : 0);
}
static bool startApfsMediaRoot (IOService *host, bool rootMode, bool diagnose, bool report = true)
{
  const char *why = "";
  if (!hostBringUp (&why)) {
    panic ("NATIVE_UFS apfs-media: bring-up failed: %s", why);
  }
  uint32_t lun = 0;
  uint64_t first = 0, blocks = 0;
  if (!native_ufs_part_find ("userdata", &lun, &first, &blocks)) {
    panic ("NATIVE_UFS apfs-media: no userdata partition in the GPT");
  }
  if (blocks * 4096ULL < kApfsMdBytes) {
    panic ("NATIVE_UFS apfs-media: userdata is smaller than the container");
  }
  gApfsMediaFirstLba = first;
  uint64_t absolute = first * 4096ULL;
  IOLog ("NATIVE_UFS apfs-media: userdata abs=%llx bytes=%llx\n",
         static_cast<unsigned long long> (absolute),
         static_cast<unsigned long long> (kApfsMdBytes));
  gAllowUnverifiedRoot = true;
  gPendingHost = host;
  NativeUfsBlock *device = OSTypeAlloc (NativeUfsBlock);
  if (!device || !device->init (static_cast<OSDictionary *> (nullptr))) {
    panic ("NATIVE_UFS apfs-media: device allocation failed");
  }
  OSString *generic = OSString::withCString ("Generic");
  if (generic) {
    device->setProperty ("device-type", generic);
    generic->release ();
  }
  if (!device->configure ("APFSContainer", absolute, kApfsMdBytes)) {
    panic ("NATIVE_UFS apfs-media: configure failed");
  }
  if (!device->attach (host)) {
    panic ("NATIVE_UFS apfs-media: attach failed");
  }
  gApfsMediaDevice = device;
  gStorageCatalogueAfter = catalogueMatches (device);
  IOLog ("NATIVE_UFS apfs-media: cat=%u\n", gStorageCatalogueAfter);
  if (!device->start (host)) {
    panic ("NATIVE_UFS apfs-media: start failed");
  }
  // The bdevsw disk is this window, so give it the container geometry before publishing it.
  gBdevAbsolute = absolute;
  gBdevBytes = kApfsMdBytes;
  // Only the probe 21/22 root handoff makes this disk the kernel root; probe 18 keeps the md
  // ramdisk it just registered.
  publishNativeBdev (rootMode);
  native_ufs_blocks_published = native_ufs_blocks_published + 1;
  IOLog ("NATIVE_UFS apfs-media: published %s\n", device->windowLabel ());
  if (rootMode) {
    dev_t mediaDev = static_cast<dev_t> (-1);
    for (unsigned tick = 0; tick < 150 && mediaDev == static_cast<dev_t> (-1); ++tick) {
      OSDictionary *matching = IOService::serviceMatching ("IOMedia");
      OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
      while (iterator) {
        IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
        if (!media) break;
        OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
        OSNumber *maj = OSDynamicCast (OSNumber, media->getProperty ("BSD Major"));
        OSNumber *min = OSDynamicCast (OSNumber, media->getProperty ("BSD Minor"));
        const char *bsdName = bsd ? bsd->getCStringNoCopy () : nullptr;
        if (bsd && maj && min && bsdName && strncmp (bsdName, "disk", 4) == 0) {
          mediaDev = makedev (maj->unsigned32BitValue (), min->unsigned32BitValue ());
          IOLog ("NATIVE_UFS apfs-root: media %s dev=%d (%u/%u)\n", bsdName,
                 static_cast<int> (mediaDev), maj->unsigned32BitValue (), min->unsigned32BitValue ());
          break;
        }
      }
      if (iterator) iterator->release ();
      if (matching) matching->release ();
      if (mediaDev == static_cast<dev_t> (-1)) IOSleep (100);
    }
    if (diagnose) {
      // Every IOMedia property that can identify or locate its BSD device. The partition media
      // reports no BSD Major/Minor on this chain, so the table has to show what it does carry.
      char *text = static_cast<char *> (IOMalloc (1800));
      int used = 0;
      if (text) {
        used = snprintf (text, 1800, "NATIVE_UFS apfs media table\nrootdev=%d catalogue=%u\n",
                         static_cast<int> (rootdev), gStorageCatalogueAfter);
        OSDictionary *matching = IOService::serviceMatching ("IOMedia");
        OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
        while (iterator && used < 1650) {
          IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
          if (!media) break;
          OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
          OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
          OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
          OSNumber *maj = OSDynamicCast (OSNumber, media->getProperty ("BSD Major"));
          OSNumber *min = OSDynamicCast (OSNumber, media->getProperty ("BSD Minor"));
          OSNumber *unit = OSDynamicCast (OSNumber, media->getProperty ("BSD Unit"));
          OSNumber *part = OSDynamicCast (OSNumber, media->getProperty ("BSD Partition"));
          OSNumber *whole = OSDynamicCast (OSNumber, media->getProperty ("Whole"));
          OSNumber *leaf = OSDynamicCast (OSNumber, media->getProperty ("Leaf"));
          used += snprintf (text + used, 1800 - used,
                            "%s c=%s sz=%llx maj=%u min=%u dv=%d u=%u p=%u w=%u l=%u\n",
                            bsd ? bsd->getCStringNoCopy () : "-",
                            content ? content->getCStringNoCopy () : "-",
                            size ? size->unsigned64BitValue () : 0ULL,
                            maj ? maj->unsigned32BitValue () : 0xffffffffu,
                            min ? min->unsigned32BitValue () : 0xffffffffu,
                            (maj && min) ? static_cast<int> (makedev (maj->unsigned32BitValue (), min->unsigned32BitValue ())) : -1,
                            unit ? unit->unsigned32BitValue () : 0xffffffffu,
                            part ? part->unsigned32BitValue () : 0xffffffffu,
                            whole ? whole->unsigned32BitValue () : 0xffffffffu,
                            leaf ? leaf->unsigned32BitValue () : 0xffffffffu);
        }
        if (iterator) iterator->release ();
        if (matching) matching->release ();
        if (mediaDev != static_cast<dev_t> (-1) && used < 1500) {
          struct vnode *dvp = nullptr;
          int bdError = bdevvp (mediaDev, &dvp);
          int fsError = -1;
          if (dvp) {
            fsError = VNOP_FSYNC (dvp, 1 /* MNT_WAIT */, vfs_context_kernel ());
          }
          used += snprintf (text + used, 1800 - used,
                            "probe: dev=%d bdevvp=%d vp=%p fsync=%d\n",
                            static_cast<int> (mediaDev), bdError,
                            reinterpret_cast<void *> (dvp), fsError);
        }
        panic ("%s", text);
      }
      panic ("NATIVE_UFS apfs media table: no buffer");
    }
    if (mediaDev != static_cast<dev_t> (-1)) {
      rootdev = mediaDev;
      native_ufs_bdev_absolute = absolute;
      native_ufs_bdev_bytes = kApfsMdBytes;
      IOLog ("NATIVE_UFS apfs-root: rootdev set to %d\n", static_cast<int> (mediaDev));
    } else {
      IOLog ("NATIVE_UFS apfs-root: no media device appeared\n");
    }
    return true;
  }
  if (!report) {
    return true;
  }
  unsigned reportDelay = 40;
  PE_parse_boot_argn ("native-ufs-report-delay", &reportDelay, sizeof (reportDelay));
  thread_t thread = nullptr;
  if (kernel_thread_start (apfsMediaReportThread, reinterpret_cast<void *> (static_cast<uintptr_t> (reportDelay)), &thread) != KERN_SUCCESS) {
    panic ("NATIVE_UFS apfs-media: cannot start the report thread");
  }
  thread_deallocate (thread);
  return true;
}

static char gMediaReport[3000];

static void mediaReportThread (void *, wait_result_t)
{
  unsigned count = 0;
  for (unsigned tick = 0; tick < 200; tick++) {      // up to 20 s
    OSDictionary *matching = IOService::serviceMatching ("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
    count = 0;
    if (iterator) {
      while (iterator->getNextObject ()) {
        count++;
      }
      iterator->release ();
    }
    if (matching) {
      matching->release ();
    }
    if (count >= 4) {
      break;
    }
    IOSleep (100);
  }
  IOSleep (500);
  int used = snprintf (gMediaReport, sizeof (gMediaReport),
                       "NATIVE_UFS media report: published=%u reads=%llu failures=%llu media=%u\n",
                       native_ufs_blocks_published, native_ufs_block_reads, native_ufs_block_failures, count);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  while (iterator && used < int (sizeof (gMediaReport)) - 200) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) {
      break;
    }
    OSString *uuid = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    used += snprintf (gMediaReport + used, sizeof (gMediaReport) - used, "%s %s size=%llx uuid=%s\n",
                      bsd ? bsd->getCStringNoCopy () : "-", content ? content->getCStringNoCopy () : "-",
                      size ? size->unsigned64BitValue () : 0ULL, uuid ? uuid->getCStringNoCopy () : "-");
  }
  if (iterator) {
    iterator->release ();
  }
  if (matching) {
    matching->release ();
  }
  panic ("%s", gMediaReport);
}

bool NativeUfs::start (IOService *provider)
{
  unsigned probe = 0, rootMode = 0;
  PE_parse_boot_argn ("native-ufs-probe", &probe, sizeof (probe));
  PE_parse_boot_argn ("native-ufs-root", &rootMode, sizeof (rootMode));
  native_ufs_probe_requested = probe != 0;
  unsigned allowUnverified = 0;
  PE_parse_boot_argn ("native-ufs-allow-unverified", &allowUnverified, sizeof (allowUnverified));
  gAllowUnverifiedRoot = allowUnverified == 1;
  // Default: match for observability only, never touch the controller. Hardware work runs once,
  // from the root-device personality; the ufshc nub match stays observation-only.
  if ((!probe && !rootMode) || !OSDynamicCast (IOPlatformExpertDevice, provider)) {
    return false;
  }
  unsigned diagnostic = 0;
  PE_parse_boot_argn ("native-ufs-diagnostics", &diagnostic, sizeof (diagnostic));
  gRootDiagnosticEnabled = diagnostic == 1;
  if (probe == 2) {
    inspect ();
  }
  if (probe == 8) {
    inspectContextBanks ();
  }
  if (probe == 7) {
    inspectSmmu ();
  }
  if (probe == 6) {
    dumpFirmwareList ();
  }
  if ((probe >= 3 && probe <= 5) || probe == 9) {
    adoptAndRead (probe);
  }
  if (probe == 10) {
    // Read-only check before anything is flashed: LUN 0 GPT -> "userdata" -> its first block.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root check: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root check: %s", why);
    }
    panic ("NATIVE_UFS root check\n"
           "userdata first=%llx last=%llx\n"
           "block0 magic %08x %08x ext4magic=%04x ext4blocks=%u\n"
           "manifest status=%u windows=%u",
           layout->userdataFirst, layout->userdataLast, layout->magic0, layout->magic1,
           layout->ext4Magic, layout->ext4Blocks, layout->manifestStatus, layout->windows);
  }
  if (probe == 11) {
    // After the root image is flashed: manifest, then each window's own GPT (sector 1 of the
    // attached dmg disk image carries "EFI PART"). Read-only.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root windows: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root windows: %s", why);
    }
    uint32_t sig[4][2] = {};
    uint32_t seen[4] = {};
    for (uint32_t index = 0; index < layout->windows && index < 4; index++) {
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      // The published window is what /dev/disk0 exposes: its first bytes hold the
      // APFS container superblock, not the SHNGROOT manifest.
      gBdevAbsolute = absolute;
      gBdevBytes = layout->window[index].size;
      if (hostRead4k (0, absolute / 4096, 1, gScratch) == kIOReturnSuccess) {
        seen[index] = 1;
        sig[index][0] = le32 (gScratch + 512);
        sig[index][1] = le32 (gScratch + 516);
      }
    }
    panic ("NATIVE_UFS root windows\n"
           "userdata first=%llx last=%llx manifest status=%u windows=%u\n"
           "w0 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w1 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w0 sha %08x %08x %08x %08x\n"
           "w1 sha %08x %08x %08x %08x",
           layout->userdataFirst, layout->userdataLast, layout->manifestStatus, layout->windows,
           layout->window[0].name, layout->window[0].offset, layout->window[0].size, sig[0][0], sig[0][1], seen[0],
           layout->window[1].name, layout->window[1].offset, layout->window[1].size, sig[1][0], sig[1][1], seen[1],
           be32w (layout->window[0].sha256), be32w (layout->window[0].sha256 + 4), be32w (layout->window[0].sha256 + 8), be32w (layout->window[0].sha256 + 12),
           be32w (layout->window[1].sha256), be32w (layout->window[1].sha256 + 4), be32w (layout->window[1].sha256 + 8), be32w (layout->window[1].sha256 + 12));
  }
  if (probe == 14) {
    // Immediate memory snapshot at root-device start, before the userspace that triggers jetsam.
    panic ("NATIVE_UFS early memory snapshot\n"
           "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
           "available=%u max_mem=%llx procs=%d",
           vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
           vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
           memorystatus_available_pages, max_mem, nprocs);
  }
  if (probe == 17) {
    // bdevsw contract test that does not need the root image: bring the transport up,
    // publish one window as /dev/disk0 (default: the 128 MiB payload region at the end of
    // userdata, which the UEFI side already reads as FAT) and, with
    // native-ufs-mountroot=1, hand it to vfs_mountroot().
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS bdev probe: bring-up failed: %s", why);
    }
    unsigned windowIndex = 0;
    PE_parse_boot_argn ("native-ufs-bdev-window", &windowIndex, sizeof (windowIndex));
    gBdevAbsolute = 516746620928ULL;
    gBdevBytes = 134217728ULL;
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (layout && findRootLayout (layout, &why) && layout->manifestStatus == 0 &&
        windowIndex < layout->windows) {
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[windowIndex].offset;
      uint64_t bytes = layout->window[windowIndex].size;
      skipLeadingApfsGpt (&absolute, &bytes);
      gBdevAbsolute = absolute;
      gBdevBytes = bytes;
      IOLog ("NATIVE_UFS bdev probe: window %u '%s' abs=%llx bytes=%llx\n", windowIndex,
             layout->window[windowIndex].name,
             static_cast<unsigned long long> (absolute),
             static_cast<unsigned long long> (bytes));
    } else {
      IOLog ("NATIVE_UFS bdev probe: no usable manifest, publishing the payload region\n");
    }
    if (layout) IOFree (layout, sizeof (RootLayout));
    publishNativeBdev ();
    // APFS reads metadata scattered across the whole container, so prove the transport
    // serves it beyond the first block before blaming the filesystem.
    static const uint64_t probeOffsets[4] = { 0, 4ULL << 20, 64ULL << 20, 1ULL << 30 };
    for (unsigned probe = 0; probe < 4; probe++) {
      uint8_t *buf = static_cast<uint8_t *> (IOMalloc (4096));
      if (!buf) break;
      int result = nativeBdevRead (probeOffsets[probe] / 512, 4096, buf);
      IOLog ("NATIVE_UFS bdev read +%llx: status=%d first=%02x%02x%02x%02x\n",
             static_cast<unsigned long long> (probeOffsets[probe]), result,
             buf[0], buf[1], buf[2], buf[3]);
      IOFree (buf, 4096);
    }
    return true;
  }
  if (probe == 20) {
    // Publish the APFS container as an IOBlockStorageDevice and report the storage chain.
    return startApfsMediaRoot (this, false, false);
  }
  if (probe == 21) {
    // Publish it, wait for the partition media, and hand its bdevsw device to rootdev.
    return startApfsMediaRoot (this, true, false);
  }
  if (probe == 22) {
    // Same handoff as probe 21, but report the state instead of letting mountroot retry.
    return startApfsMediaRoot (this, true, true);
  }
  if (probe == 30) {
    // Read what DarwinBoot wrote into /chosen/memory-map: the trust cache handoff either
    // exists (address and size non-zero) or the payload/firmware never delivered it.
    IORegistryEntry *map = IORegistryEntry::fromPath ("/chosen/memory-map", gIODTPlane);
    OSData *tc = map ? OSDynamicCast (OSData, map->getProperty ("TrustCache")) : nullptr;
    OSData *rd = map ? OSDynamicCast (OSData, map->getProperty ("RAMDisk")) : nullptr;
    uint64_t tcAddr = 0, tcSize = 0, rdAddr = 0, rdSize = 0;
    if (tc && tc->getLength () >= 16) {
      const uint64_t *v = static_cast<const uint64_t *> (tc->getBytesNoCopy ());
      tcAddr = v[0]; tcSize = v[1];
    }
    if (rd && rd->getLength () >= 16) {
      const uint64_t *v = static_cast<const uint64_t *> (rd->getBytesNoCopy ());
      rdAddr = v[0]; rdSize = v[1];
    }
    IORegistryEntry *chosen = IORegistryEntry::fromPath ("/chosen", gIODTPlane);
    OSData *bootargs = chosen ? OSDynamicCast (OSData, chosen->getProperty ("boot-args")) : nullptr;
    int argsLen = bootargs ? static_cast<int> (bootargs->getLength ()) : -1;
    panic ("NATIVE_UFS dt: map=%d tc=%d(%llx/%llx) rd=%d(%llx/%llx) chosen=%d args=%d",
           map ? 1 : 0, tc ? 1 : 0,
           static_cast<unsigned long long> (tcAddr), static_cast<unsigned long long> (tcSize),
           rd ? 1 : 0,
           static_cast<unsigned long long> (rdAddr), static_cast<unsigned long long> (rdSize),
           chosen ? 1 : 0, argsLen);
  }
  if (probe == 31) {
    thread_t thread = nullptr;
    if (kernel_thread_start (trustQueryThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS trustq: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 32) {
    thread_t thread = nullptr;
    if (kernel_thread_start (rootFsReportThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS rootfs: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 33) {
    // Write-path self test: flip bytes in one userdata block, read it back, then restore the
    // original bytes and verify the restore. The root device is the md ramdisk, so this block
    // is not referenced by any mounted filesystem.
    const char *why = "";
    if (!hostBringUp (&why)) panic ("NATIVE_UFS writetest: bring-up failed: %s", why);
    uint32_t lun = 0;
    uint64_t first = 0, blocks = 0;
    if (!native_ufs_part_find ("userdata", &lun, &first, &blocks))
      panic ("NATIVE_UFS writetest: no userdata partition");
    uint8_t *orig = static_cast<uint8_t *> (IOMalloc (4096));
    uint8_t *work = static_cast<uint8_t *> (IOMalloc (4096));
    uint8_t *back = static_cast<uint8_t *> (IOMalloc (4096));
    if (!orig || !work || !back) panic ("NATIVE_UFS writetest: no buffers");
    bool mediaOpened = hostMediaOpen (first, kApfsMdBytes / 4096);
    uint64_t lba = first + 8;
    IOReturn readRc = hostRead4k (lun, lba, 1, orig);
    memcpy (work, orig, 4096);
    for (unsigned i = 0; i < 4096; i += 64) work[i] = static_cast<uint8_t> (work[i] ^ 0xa5);
    IOReturn writeRc = hostWrite4k (lun, lba, 1, work, true);
    IOReturn backRc = hostRead4k (lun, lba, 1, back);
    int matched = (backRc == kIOReturnSuccess) && !memcmp (back, work, 4096);
    IOReturn restoreRc = hostWrite4k (lun, lba, 1, orig, true);
    IOReturn checkRc = hostRead4k (lun, lba, 1, back);
    int restored = (checkRc == kIOReturnSuccess) && !memcmp (back, orig, 4096);
    panic ("NATIVE_UFS writetest lba=%llx window=%d read=%d write=%d reread=%d match=%d restore=%d/%d ok=%d",
           static_cast<unsigned long long> (lba), mediaOpened ? 1 : 0, static_cast<int> (readRc), static_cast<int> (writeRc),
           static_cast<int> (backRc), matched, static_cast<int> (restoreRc), static_cast<int> (checkRc),
           (readRc == 0 && writeRc == 0 && matched && restored) ? 1 : 0);
  }
  if (probe == 41) {
    // Read-only verification of the install target (run after probe 40 formatted it).
    const char *why = "";
    if (!hostBringUp (&why)) panic ("NATIVE_UFS check: bring-up failed: %s", why);
    uint32_t lun = 0;
    uint64_t first = 0, blocks = 0;
    if (!native_ufs_part_find ("userdata", &lun, &first, &blocks)) panic ("NATIVE_UFS check: no userdata partition");
    const uint64_t kOffsetBlocks = (1ULL << 30) / 4096;
    if (blocks < kOffsetBlocks + 16) panic ("NATIVE_UFS check: userdata too small");
    gBdevAbsolute = (first + kOffsetBlocks) * 4096ULL;
    IOLog ("NATIVE_UFS check: target lba=%llx\n", static_cast<unsigned long long> (gBdevAbsolute / 4096));
    thread_t thread = nullptr;
    if (kernel_thread_start (targetCheckThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS check: cannot start the check thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 40) {
    // Install tool run: the root comes from the in-memory APFS ramdisk (startApfsMdRoot creates
    // md1), while the bdevsw disk is the 8 GiB install target (devfs /dev/nu0). The init program
    // from the boot arguments (peridot-init-path, e.g. newfs_apfs) formats that target; a thread
    // then reports whether a fresh APFS container superblock appeared.
    if (!startApfsMdRoot ()) return false;
    uint32_t lun = 0;
    uint64_t first = 0, blocks = 0;
    if (!native_ufs_part_find ("userdata", &lun, &first, &blocks)) panic ("NATIVE_UFS init: no userdata partition");
    const uint64_t kOffsetBlocks = (1ULL << 30) / 4096;
    const uint64_t kTargetBlocks = (8ULL << 30) / 4096;
    if (blocks < kOffsetBlocks + kTargetBlocks + 16) panic ("NATIVE_UFS init: userdata too small");
    uint64_t targetFirst = first + kOffsetBlocks;
    if (!hostTargetOpen (targetFirst, kTargetBlocks)) panic ("NATIVE_UFS init: cannot open the target window");
    gBdevAbsolute = targetFirst * 4096ULL;
    gBdevBytes = kTargetBlocks * 4096ULL;
    publishNativeBdev (false);
    if (gBdevSlot < 0) panic ("NATIVE_UFS init: no bdevsw slot");
    uint8_t *zero = static_cast<uint8_t *> (IOMalloc (kDataMax));
    if (!zero) panic ("NATIVE_UFS init: no wipe buffer");
    memset (zero, 0, kDataMax);
    int wipeErrors = 0;
    const uint64_t wipeChunks = (1024ULL * 1024ULL) / kDataMax;
    for (uint64_t off = 0; off < wipeChunks; off++) {
      if (hostWrite4k (0, targetFirst + off * (kDataMax / 4096), static_cast<uint32_t> (kDataMax / 4096), zero, true) != kIOReturnSuccess) {
        wipeErrors++;
        break;
      }
    }
    IOFree (zero, kDataMax);
    IOLog ("NATIVE_UFS init: nu0 lba=%llx bytes=%llx wipe=%d\n",
           static_cast<unsigned long long> (targetFirst),
           static_cast<unsigned long long> (gBdevBytes), wipeErrors);
    thread_t thread = nullptr;
    if (kernel_thread_start (initToolVerifyThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS init: cannot start the verify thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 39) {
    // Boot from the installed target: the bdevsw disk is the install-target window (written by
    // probe 38), claimed as root. The rd=md1 boot arg only makes APFS take its ramdisk restore
    // branch; the container is mounted from the target window on UFS.
    const char *why = "";
    if (!hostBringUp (&why)) panic ("NATIVE_UFS target-root: bring-up failed: %s", why);
    uint32_t lun = 0;
    uint64_t first = 0, blocks = 0;
    if (!native_ufs_part_find ("userdata", &lun, &first, &blocks)) panic ("NATIVE_UFS target-root: no userdata partition");
    const uint64_t kOffsetBlocks = (1ULL << 30) / 4096;
    if (blocks <= kOffsetBlocks + 4096) panic ("NATIVE_UFS target-root: userdata too small");
    uint64_t targetFirst = first + kOffsetBlocks;
    if (!hostTargetOpen (targetFirst, kApfsMdBytes / 4096)) panic ("NATIVE_UFS target-root: cannot open the target window");
    gBdevAbsolute = targetFirst * 4096ULL;
    gBdevBytes = kApfsMdBytes;
    publishNativeBdev (false);
    if (gBdevSlot < 0) panic ("NATIVE_UFS target-root: no bdevsw slot");
    rootdev = makedev (gBdevSlot, 0);
    native_ufs_bdev_claim_root = 1;
    IOLog ("NATIVE_UFS target-root: lba=%llx dev=%d claim=1\n",
           static_cast<unsigned long long> (targetFirst), static_cast<int> (rootdev));
    return true;
  }
  if (probe == 38) {
    // Copy the 184 MiB APFS container from userdata offset 0 to the install-target window.
    const char *why = "";
    if (!hostBringUp (&why)) panic ("NATIVE_UFS install copy: bring-up failed: %s", why);
    uint32_t lun = 0;
    uint64_t first = 0, blocks = 0;
    if (!native_ufs_part_find ("userdata", &lun, &first, &blocks)) panic ("NATIVE_UFS install copy: no userdata partition");
    const uint64_t kOffsetBlocks = (1ULL << 30) / 4096;
    if (blocks <= kOffsetBlocks + (kApfsMdBytes / 4096) + 16) panic ("NATIVE_UFS install copy: userdata too small");
    uint64_t targetFirst = first + kOffsetBlocks;
    if (!hostTargetOpen (targetFirst, kApfsMdBytes / 4096)) panic ("NATIVE_UFS install copy: cannot open the target window");
    gInstallCopySourceLba = first;
    gInstallCopyTargetLba = targetFirst;
    IOLog ("NATIVE_UFS install copy: src=%llx dst=%llx bytes=%llx\n",
           static_cast<unsigned long long> (first), static_cast<unsigned long long> (targetFirst),
           static_cast<unsigned long long> (kApfsMdBytes));
    thread_t thread = nullptr;
    if (kernel_thread_start (installCopyThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS install copy: cannot start the copy thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 37) {
    // Probe 36's UFS root plus the install target window: publish a second, large userdata
    // window as its own IOBlockStorageDevice (1 GiB into userdata, up to 8 GiB), then test page
    // and 4 MiB writes through that device and report the root mount flags.
    if (!startApfsMediaRoot (this, false, false, false)) return false;
    if (gBdevSlot < 0) panic ("NATIVE_UFS install: no bdevsw slot");
    uint32_t lun = 0;
    uint64_t first = 0, blocks = 0;
    if (!native_ufs_part_find ("userdata", &lun, &first, &blocks)) panic ("NATIVE_UFS install: no userdata partition");
    if (!hostMediaOpen (first, kApfsMdBytes / 4096)) panic ("NATIVE_UFS install: cannot open the media window");
    rootdev = makedev (gBdevSlot, 0);
    native_ufs_bdev_claim_root = 1;
    const uint64_t kOffsetBlocks = (1ULL << 30) / 4096;          // 1 GiB into userdata
    const uint64_t kMaxTargetBlocks = (8ULL << 30) / 4096;       // 8 GiB target
    if (blocks <= kOffsetBlocks + 4096) panic ("NATIVE_UFS install: userdata too small");
    uint64_t targetFirst = first + kOffsetBlocks;
    uint64_t targetBlocks = blocks - kOffsetBlocks - 16;
    if (targetBlocks > kMaxTargetBlocks) targetBlocks = kMaxTargetBlocks;
    if (!hostTargetOpen (targetFirst, targetBlocks)) panic ("NATIVE_UFS install: cannot open the target window");
    gInstallTargetFirstLba = targetFirst;
    gInstallTargetBytes = targetBlocks * 4096ULL;
    NativeUfsBlock *target = OSTypeAlloc (NativeUfsBlock);
    if (!target || !target->init (static_cast<OSDictionary *> (nullptr))) panic ("NATIVE_UFS install: target allocation failed");
    OSString *generic = OSString::withCString ("Generic");
    if (generic) {
      target->setProperty ("device-type", generic);
      generic->release ();
    }
    if (!target->configure ("InstallTarget", targetFirst * 4096ULL, gInstallTargetBytes)) panic ("NATIVE_UFS install: target configure failed");
    if (!target->attach (this)) panic ("NATIVE_UFS install: target attach failed");
    gInstallTargetDevice = target;
    if (!target->start (this)) panic ("NATIVE_UFS install: target start failed");
    IOLog ("NATIVE_UFS install: target lba=%llx mib=%llu\n",
           static_cast<unsigned long long> (targetFirst),
           static_cast<unsigned long long> (gInstallTargetBytes >> 20));
    thread_t thread = nullptr;
    if (kernel_thread_start (installTargetTestThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS install: cannot start the test thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 36) {
    // Fix A from peridot/audit/2026-11-15-enotblk.md section 7: root the APFS container from
    // the real UFS block device. The UFS window is published as the bdevsw disk and claimed as
    // rootdev; the kernel patch makes IOFindBSDRoot adopt it before the rd=/rootdev wait, while
    // the rd=md1 boot arg remains only APFS's ramdisk-branch ticket (no md device is created).
    // APFS container_rootmount then hands this devvp to container_mount, so the mounted root
    // lives on UFS instead of the memory copy and writes can reach the disk.
    if (!startApfsMediaRoot (this, false, false, false)) return false;
    if (gBdevSlot < 0) panic ("NATIVE_UFS ufs-root: no bdevsw slot");
    uint32_t lun = 0;
    uint64_t first = 0, blocks = 0;
    if (!native_ufs_part_find ("userdata", &lun, &first, &blocks)) panic ("NATIVE_UFS ufs-root: no userdata partition");
    if (!hostMediaOpen (first, kApfsMdBytes / 4096)) panic ("NATIVE_UFS ufs-root: cannot open the writable media window");
    rootdev = makedev (gBdevSlot, 0);
    native_ufs_bdev_claim_root = 1;
    IOLog ("NATIVE_UFS ufs-root: rootdev=%d maj=%u min=%u bytes=%llx claim=1\n",
           static_cast<int> (rootdev), static_cast<unsigned> (major (rootdev)),
           static_cast<unsigned> (minor (rootdev)), static_cast<unsigned long long> (kApfsMdBytes));
    return true;
  }
  if (probe == 34) {
    // Same root and media publication as probe 18, then a write through the block-device data
    // path (IOBlockStorageDevice::doAsyncReadWrite) with read-back and restore. This is the
    // regression test for audit 2026-11-15 P0-1.
    if (!startApfsMdRoot ()) return false;
    if (!startApfsMediaRoot (this, false, false, false)) return false;
    thread_t thread = nullptr;
    if (kernel_thread_start (deviceWriteTestThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS devio: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 18) {
    // Root = the APFS container at the start of userdata, read into a memory device.
    if (!startApfsMdRoot ()) {
      return false;
    }
    // Publish the same window as a writable IOBlockStorageDevice, so the storage stack also
    // exposes /dev/disk0s1 with write protection off while the root stays on the md ramdisk.
    // No diagnostic report thread here: this boot is meant to stay up and run userspace.
    return startApfsMediaRoot (this, false, false, false);
  }
  if (probe == 19) {
    // No hardware: report the IOKit class/catalogue state the APFS container chain needs.
    unsigned delay = 3;
    PE_parse_boot_argn ("native-ufs-report-delay", &delay, sizeof (delay));
    thread_t thread = nullptr;
    if (kernel_thread_start (iokitStateThread, reinterpret_cast<void *> (static_cast<uintptr_t> (delay)), &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS iokit state: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 13) {
    unsigned delay = 12;
    PE_parse_boot_argn ("native-ufs-stats-delay", &delay, sizeof (delay));
    if (!startRoot (provider)) {
      panic ("NATIVE_UFS memory snapshot: root start failed");
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (statsThread, reinterpret_cast<void *> (static_cast<uintptr_t> (delay)), &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS memory snapshot: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  IOLog ("NU0 p%u\n", probe);
  if (probe == 16) {
    // Diagnostics first: render the previous boot's log tail through the panic screen.
    // Panic text after the first line starts at column 0, so it is not cut by the console
    // width, unlike a plain printf of the same text.
    unsigned printLog = 0;
    PE_parse_boot_argn ("native-ufs-print-last-log", &printLog, sizeof (printLog));
    if (printLog) {
      // Short lines first: these fit the console even when the panic dump does not.
      IOLog ("NUPRG %u\n", (unsigned) gPanicSize);
      IOLog ("NUPM %x\n", (gPanicBase && gPanicSize >= 4) ? *(volatile unsigned int *) gPanicBase : 0u);
      pe_panic_last_log ();
    }
    // Unverified bring-up publication (muyu's mode): publish the root windows without the
    // owner policy and let the publisher accept any media below them.
    gAllowUnverifiedRoot = true;
    return startRoot (provider);
  }
  if (probe == 15) {
    bootstrapStorageKext ();
    if (!startRoot (provider)) {
      panic ("NATIVE_UFS publication report: root start failed (blocks=%u)", native_ufs_blocks_published);
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (publicationReportThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS publication report: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 12) {
    bool published = startRoot (provider);
    if (!published) {
      panic ("NATIVE_UFS media report: root start failed (blocks=%u)", native_ufs_blocks_published);
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (mediaReportThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS media report: cannot start the report thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (rootMode && !probe) {
    return startRoot (provider);
  }
  return false;
}

// Boot the root image's windows as block devices. Soft-fails (logs, publishes nothing).
bool NativeUfs::startRoot (IOService *provider)
{
  const char *why = "";
  native_ufs_root_step = 1;
  gStorageStage = 20;
  if (!hostBringUp (&why)) {
    rootDiagnosticFailure (why);
    IOLog ("NativeUfs: root bring-up failed: %s\n", why);
    return false;
  }
  native_ufs_root_step = 2;
  gStorageStage = 21;
  RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
  if (!layout || !findRootLayout (layout, &why)) {
    rootDiagnosticFailure (layout ? why : "root layout allocation failed");
    IOLog ("NativeUfs: root layout failed: %s\n", why);
    if (layout) IOFree (layout, sizeof (RootLayout));
    return false;
  }
  native_ufs_root_step = 3;
  gStorageStage = 22;
  if (layout->manifestStatus != 0) {
    rootDiagnosticFailure ("root manifest rejected");
    IOLog ("NativeUfs: no valid root manifest in userdata (status %u, magic %08x %08x)\n",
           layout->manifestStatus, layout->magic0, layout->magic1);
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  gStorageStage = 23;
  gStoreGeometry = { true, layout->userdataFirst, layout->userdataLast, layout->imageBytes, layout->gptEntriesLba,
                     layout->gptHeaderCrc, layout->gptEntriesCrc, layout->gptEntryCount };
  gRootObjects = layout->objects;
  for (unsigned i = 0; i < layout->objects; ++i) gRootObject[i] = layout->object[i];
  gRootObjectBase = layout->userdataFirst * 4096ULL;

  const char *names[4] = {};
  for (unsigned i = 0; i < layout->windows; ++i) names[i] = layout->window[i].name;
  if (!ufsBuildRootPlan (names, layout->windows, gPublicationPlan)) {
    rootDiagnosticFailure ("root windows require distinct OS and SystemOS");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  gStorageStage = 24;
  if (!IOService::start (provider)) {
    rootDiagnosticFailure ("root IOService start failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  // Prepare all windows or publish none. Diagnostics never decide policy.
  gStorageStage = 25;
  gPending = 0;
  gPendingProvider = this;
  struct Operations {
    NativeUfs *owner;
    RootLayout *layout;
    bool prepare (unsigned index) {
      gStorageWindow = index;
      gStorageStage = 30;
      gPrepStage = 0;
      gRootDiagnostic.stage = "root window allocation/configuration";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Prepare);
      gRootDiagnostic.publicationWindow = index;
      gRootDiagnostic.windowBytes = layout->window[index].size;
      NativeUfsBlock *device = OSTypeAlloc (NativeUfsBlock);
      gPendingDevice[index] = device;
      gPendingAttached[index] = false;
      gPendingUuid[index][0] = 0;
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      {
        // Publish the container, not the partition table in front of it: the iOS OS
        // window is a whole-disk image whose first Apple_APFS partition holds the
        // container APFS has to mount.
        uint64_t bdevAbsolute = absolute;
        uint64_t bdevBytes = layout->window[index].size;
        skipLeadingApfsGpt (&bdevAbsolute, &bdevBytes);
        gBdevAbsolute = bdevAbsolute;
        gBdevBytes = bdevBytes;
      }
      // Back to the p15e form: the extra property table and the explicit device-type
      // write are the only differences from the boot that did reach "published SystemOS".
      // The property is set in the storage bring-up instead, right before matching.
      if (!device || !device->init (static_cast<OSDictionary *> (nullptr))) return false;
      gStorageStage = 31;
      // The personality matches on this exact property (Info.plist IOPropertyMatch).
      // Use an OSString: the plain const char* overload left the property missing, which
      // is why the catalogue match stayed at 0 even with the personalities registered.
      OSString *generic = OSString::withCString ("Generic");
      if (generic) {
        device->setProperty ("device-type", generic);
        generic->release ();
      }
      gStorageCatalogueAfter = catalogueMatches (device);
      const OSMetaClass *meta = device->getMetaClass ();
      const OSMetaClass *parent = meta ? meta->getSuperClass () : nullptr;
      gStorageParentOk = (parent && parent->getClassName () && !strcmp (parent->getClassName (), "IOBlockStorageDevice")) ? 1u : 0u;
      IOLog ("NativeUfs: dt=%u match=%u\n",
             device->getProperty ("device-type") ? 1u : 0u, gStorageCatalogueAfter);
      gStorageStage = 32;
      gPrepStage = 1;
      // native-ufs-prepare bisects the three calls that make a window device:
      // 1 = init+configure+attach, 2 = init+attach, 3 = init only.
      unsigned mode = 1;
      PE_parse_boot_argn ("native-ufs-prepare", &mode, sizeof (mode));
      if (mode <= 1 &&
          !device->configure (layout->window[index].name, absolute, layout->window[index].size)) return false;
      gStorageStage = 33;
      gPrepStage = 2;
      if (mode >= 3) {
        gStorageStage = 36;
        return true;
      }
      gRootDiagnostic.stage = "root window attachment";
      // muyu's chain: the window is a child of this NativeUfs, and start() is called with
      // the same object. Attaching to IOPlatformExpert instead broke that chain and is the
      // one structural difference from the port that reaches the UI.
      IOService *host = owner;
      gStorageStage = 34;
      gPrepStage = 3;
      if (mode <= 2 && (!host || !device->attach (host))) return false;
      // Walk to the root of the IOService plane: registerService() requires it, and a
      // detached object looks the same as a refused one in the log.
      {
        unsigned depth = 0;
        IORegistryEntry *entry = device;
        while (entry && depth < 24) {
          entry = entry->getParentEntry (gIOServicePlane);
          depth++;
        }
        IOLog ("NU7 d%u r%u\n", depth, entry ? 1u : 0u);
      }
      gStorageStage = 35;
      gPendingHost = host;
      gPrepStage = 4;
      gStorageStage = 36;
      gPendingAttached[index] = true;
      ++gPending;
      gRootDiagnostic.prepared = gPending;
      return true;
    }
    bool validate () {
      gStorageStage = 40;
      gRootDiagnostic.stage = "signed window identities";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Identity);
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      if (gAllowUnverifiedRoot) {
        // Bring-up: no signed identity. Empty UUIDs make the publisher wait for any media
        // descendant of each window, exactly like the muyu port's unverified mode.
        for (unsigned i = 0; i < gPending; ++i) gPendingUuid[i][0] = 0;
        return true;
      }
      for (unsigned i = 0; i < gPending; ++i) {
        gRootDiagnostic.publicationWindow = i;
        if (!gPendingDevice[i]->signedVolumeUuid (gPendingUuid[i])) {
          panic ("VRFAIL signedVolumeUuid window=%u files=%u", i, gPending);
          return false;
        }
        for (unsigned j = 0; j < i; ++j) {
          if (!strcmp (gPendingUuid[i], gPendingUuid[j])) {
            panic ("VRFAIL duplicate uuid w%u=%s w%u=%s", i, gPendingUuid[i], j, gPendingUuid[j]);
            return false;
          }
        }
      }
      return true;
#else
      return false; // Root publication requires owner verification in every build.
#endif
    }
    bool schedule () {
      gStorageStage = 42;
      gRootDiagnostic.stage = "root publisher thread";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Schedule);
      gRootDiagnostic.publicationWindow = 0xffffffff;
      thread_t thread = nullptr;
      owner->retain ();
      if (kernel_thread_start (publishThread, nullptr, &thread) != KERN_SUCCESS) {
        owner->release ();
        return false;
      }
      thread_deallocate (thread);
      return true;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations = { this, layout };
  UfsRootPublicationResult result = ufsPrepareRootWindows (operations, layout->windows);
  IOFree (layout, sizeof (RootLayout));
  if (result.phase != UfsRootPhase::Complete) {
    gPending = 0;
    gRootDiagnostic.result = result.phase == UfsRootPhase::Identity ? kIOReturnNotPrivileged :
      result.phase == UfsRootPhase::Schedule ? kIOReturnNoResources : kIOReturnError;
    IOLog ("NativeUfs: root preparation failed phase=%u window=%u; nothing published\n",
           static_cast<unsigned> (result.phase), result.index);
  panic ("PUBFAIL phase=%u index=%u stage=%u reason=%s verify=%x policies=%llu bytes=%llx",
         static_cast<unsigned> (result.phase), result.index, gPrepStage, gRootDiagnostic.stage,
         gRootDiagnostic.verify, (unsigned long long) gRootDiagnostic.policyCount,
         (unsigned long long) gRootDiagnostic.windowBytes);
    IOService::stop (provider);
    rootDiagnosticFailure (result.phase == UfsRootPhase::Schedule ? "root publisher thread creation failed" :
                          result.phase == UfsRootPhase::Identity ? "signed window identities rejected" :
                          "root window preparation failed");
    return false;
  }
  return true;
}

void NativeUfs::stop (IOService *provider)
{
  fCmdDesc.reset ();
  fMap.reset ();
  IOService::stop (provider);
}
