// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native SM8635 (Adreno 735) GPU bring-up: skeleton.
//
// Default-off: with no boot argument the driver matches nothing, so the kernel
// behaves exactly as before this file existed. With peridot-gpu=1 it maps the
// five windows the platform kit describes, binds them and stops before any GMU
// register is written. The remaining stages are marked TODO and each one needs a
// hardware fact that is not in this tree yet.
//
// The kit this file calls (drivers/Sm8635Platform) carries the GMU, GPU SMMU and
// command-DB code; the generated ShengGmuSource.h carries every register offset,
// the firmware hash and the 19-write non-A740 RSC sequence.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include <libkern/c++/OSData.h>
#include <vm/vm_kern.h>
#include <kern/clock.h>

extern "C" vm_offset_t ml_io_map (mach_vm_address_t phys_addr, vm_size_t size);
// printf() writes to the framebuffer console, so a diagnostic line survives the
// later panic instead of being replaced by a panic dump.
extern "C" int printf (const char *format, ...);
// GmuFirmwareImage.c: the pinned gen71100 image, hashed by the kit before use.
extern "C" const unsigned char sheng_gmu_firmware_image[];
extern "C" const unsigned int sheng_gmu_firmware_image_bytes;

extern "C" {
#include "ShengGmu.h"
#include "ShengGmuSource.h"
}

// Boot argument, same shape as the other peridot probes: absent means do nothing.
static bool gGpuProbeRequested;
static bool gGpuPanicOnResult;
static volatile uint32_t gGpuStep;          // last completed stage, for the boot page
static volatile int32_t gGpuLastStatus;     // ShengGmuStatus of the last kit call

class NativeAdreno735 : public IOService
{
	OSDeclareDefaultStructors (NativeAdreno735)

public:
	bool     init (OSDictionary *dictionary = nullptr) override;
	void     free (void) override;
	IOService *probe (IOService *provider, SInt32 *score) override;
	bool     start (IOService *provider) override;
	void     stop (IOService *provider) override;

private:
	bool     bringUp (void);
	void     releaseWindows (void);

	ShengGmuMmio  fMmio {};
	ShengGmuIo    fIo {};
	IOMemoryMap  *fMaps[5] {};
	uint32_t      fCoreId;
};

OSDefineMetaClassAndStructors (NativeAdreno735, IOService)

extern "C" {

// Kernel services the kit requires. Each one performs the real operation: the kit
// validates the firmware hash before any register write, so a stub here would be
// caught (and a permissive stub would let a wrong sequence run).
// A local SHA-256: the kernel's ccdigest glue is not initialised this early (p67
// data-aborted inside SHA256_Init at Boot Stage 2), and the kit needs the digest
// before it writes any register.
typedef struct {
	uint32_t state[8];
	uint64_t bits;
	uint8_t block[64];
	size_t used;
} GmuSha256;

static const uint32_t gmu_sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t
gmu_ror (uint32_t v, unsigned n)
{
	return (v >> n) | (v << (32 - n));
}

static void
gmu_sha256_block (GmuSha256 *c, const uint8_t *p)
{
	uint32_t w[64];
	for (unsigned i = 0; i < 16; ++i) {
		w[i] = ((uint32_t) p[4 * i] << 24) | ((uint32_t) p[4 * i + 1] << 16) |
		    ((uint32_t) p[4 * i + 2] << 8) | p[4 * i + 3];
	}
	for (unsigned i = 16; i < 64; ++i) {
		uint32_t s0 = gmu_ror (w[i - 15], 7) ^ gmu_ror (w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = gmu_ror (w[i - 2], 17) ^ gmu_ror (w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
	uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
	for (unsigned i = 0; i < 64; ++i) {
		uint32_t s1 = gmu_ror (e, 6) ^ gmu_ror (e, 11) ^ gmu_ror (e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + s1 + ch + gmu_sha256_k[i] + w[i];
		uint32_t s0 = gmu_ror (a, 2) ^ gmu_ror (a, 13) ^ gmu_ror (a, 22);
		uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
		uint32_t t2 = s0 + maj;
		h = g; g = f; f = e; e = d + t1;
		d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
	c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void
gmu_sha256_init (GmuSha256 *c)
{
	static const uint32_t initial[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	for (unsigned i = 0; i < 8; ++i) {
		c->state[i] = initial[i];
	}
	c->bits = 0;
	c->used = 0;
}

static void
gmu_sha256_update (GmuSha256 *c, const uint8_t *p, size_t n)
{
	c->bits += (uint64_t) n * 8;
	while (n) {
		size_t take = 64 - c->used;
		if (take > n) {
			take = n;
		}
		for (size_t i = 0; i < take; ++i) {
			c->block[c->used + i] = p[i];
		}
		c->used += take;
		p += take;
		n -= take;
		if (c->used == 64) {
			gmu_sha256_block (c, c->block);
			c->used = 0;
		}
	}
}

static void
gmu_sha256_final (GmuSha256 *c, uint8_t digest[32])
{
	uint64_t bits = c->bits;
	uint8_t pad[72];
	size_t padlen = (c->used < 56) ? (56 - c->used) : (120 - c->used);
	pad[0] = 0x80;
	for (size_t i = 1; i < padlen; ++i) {
		pad[i] = 0;
	}
	gmu_sha256_update (c, pad, padlen);
	c->bits = bits;
	uint8_t length[8];
	for (unsigned i = 0; i < 8; ++i) {
		length[i] = (uint8_t) (bits >> (56 - 8 * i));
	}
	c->bits = 0;
	gmu_sha256_update (c, length, 8);
	for (unsigned i = 0; i < 8; ++i) {
		digest[4 * i] = (uint8_t) (c->state[i] >> 24);
		digest[4 * i + 1] = (uint8_t) (c->state[i] >> 16);
		digest[4 * i + 2] = (uint8_t) (c->state[i] >> 8);
		digest[4 * i + 3] = (uint8_t) c->state[i];
	}
}

static int
gmu_sha256 (void *, const void *data, size_t bytes, uint8_t digest[32])
{
	GmuSha256 ctx;
	gmu_sha256_init (&ctx);
	gmu_sha256_update (&ctx, (const uint8_t *) data, bytes);
	gmu_sha256_final (&ctx, digest);
	return 0;
}

static int
gmu_now_us (void *, uint64_t *us)
{
	// clock_get_uptime returns nanoseconds since boot; the kit only needs a
	// monotonic clock with microsecond resolution.
	uint64_t nanoseconds = 0;
	clock_get_uptime (&nanoseconds);
	*us = nanoseconds / 1000;
	return 0;
}

static int
gmu_wait_us (void *, unsigned us)
{
	delay (us);
	return 0;
}

static bool
gmu_allocate (ShengGmuMemory *memory, size_t bytes, uint64_t iova)
{
	vm_address_t address = 0;
	if (vm_allocate (kernel_map, &address, bytes, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
		return false;
	}
	memory->cpu = (volatile uint32_t *) address;
	memory->bytes = bytes;
	memory->iova = iova;
	return true;
}

// Stage 1-3 run from here because this kernel installs no personality for built-in
// drivers (p61-p65). Stage 1-2 is device-verified; stage 3 loads the pinned GMU
// firmware and reports exactly how far the kit got.
__attribute__((constructor)) void
peridot_gpu_early_probe (void)
{
	bool requested = false;
	PE_parse_boot_argn ("peridot-gpu", &requested, sizeof (requested));
	if (!requested) {
		return;
	}

	ShengGmuMmio mmio {};
	ShengGmuIo io {};
	unsigned mapped = 0;
	for (unsigned i = 0; i < 5; ++i) {
		mmio.windows[i].physical = sheng_gmu_window_physical[i];
		mmio.windows[i].bytes = sheng_gmu_window_bytes[i];
		mmio.windows[i].cpu = (volatile unsigned char *)
		    ml_io_map ((mach_vm_address_t) mmio.windows[i].physical, mmio.windows[i].bytes);
		if (mmio.windows[i].cpu) {
			mapped++;
		}
	}
	if (mapped != 5) {
		panic ("NU735 s1 mapped=%u\n", mapped);
	}
	int bind = (int) ShengGmuBindMmio (&mmio, &io);
	if (bind != (int) ShengGmuSuccess) {
		panic ("NU735 s2 bind=%d\n", bind);
	}

	// RPMh first: p79 showed the GPUCC AHB block faults once its RCG runs, and the
	// vendor GPUCC driver defines no GDSC, so the CX domain is voted through the
	// RSC. Read the AP RSC (0x17a00000, three DRV blocks) before writing anything:
	// the DRV id register carries the layout version, and the TCS state says
	// whether the bootloader left commands behind.
	volatile uint32_t *rsc = (volatile uint32_t *)
	    ml_io_map ((mach_vm_address_t) 0x17a00000, 0x10000);
	if (!rsc) {
		panic ("NU735 rsc map failed\n");
	}
	uint32_t rsc_id = rsc[0x00 / 4];
	uint32_t major = (rsc_id >> 16) & 0xff;
	uint32_t minor = (rsc_id >> 8) & 0xff;
	uint32_t solver = rsc[0x04 / 4];
	uint32_t child = rsc[0x0c / 4];
	bool v3 = major >= 3;
	uint32_t tcs_wait = rsc[(0x10 + (v3 ? 0x20 : 0x10)) / 4];
	uint32_t tcs_control = rsc[(0x10 + (v3 ? 0x24 : 0x14)) / 4];
	uint32_t tcs_status = rsc[(0x10 + (v3 ? 0x28 : 0x18)) / 4];
	uint32_t tcs_enable = rsc[(0x10 + (v3 ? 0x2c : 0x1c)) / 4];
	uint32_t cmd_msgid = rsc[(0x10 + (v3 ? 0x34 : 0x30)) / 4];
	uint32_t cmd_addr = rsc[(0x10 + (v3 ? 0x38 : 0x34)) / 4];
	uint32_t cmd_data = rsc[(0x10 + (v3 ? 0x3c : 0x38)) / 4];
	panic ("NU735 rsc id=0x%x v%u.%u solver=0x%x child=0x%x w=0x%x c=0x%x s=0x%x e=0x%x msg=0x%x a=0x%x d=0x%x\n",
	    rsc_id, major, minor, solver, child, tcs_wait, tcs_control, tcs_status, tcs_enable,
	    cmd_msgid, cmd_addr, cmd_data);

	// Stage 3: five GMU memories at the IOVAs the kit pins, then the firmware.
	ShengGmuDevice device {};
	ShengGmuConfig config {};
	ShengGmuResult result {};
	device.io = io;
	device.services.cookie = nullptr;
	device.services.sha256 = gmu_sha256;
	device.services.now_us = gmu_now_us;
	device.services.wait_us = gmu_wait_us;
	config.chip_id = SHENG_GMU_CHIP_ID;
	config.firmware = sheng_gmu_firmware_image;
	config.firmware_bytes = SHENG_GMU_FIRMWARE_BYTES;
	config.poll_budget = 100;
	if (!gmu_allocate (&config.icache, 0xffc000, 0x4000) ||
	    !gmu_allocate (&config.dummy, 0x4000, 0x60000000) ||
	    !gmu_allocate (&config.debug, 0x7000, 0x60400000) ||
	    !gmu_allocate (&config.log, 0x4000, 0x61000000) ||
	    !gmu_allocate (&config.hfi, 0x4000, 0x61800000)) {
		panic ("NU735 s3 alloc failed\n");
	}
	ShengGmuStatus status = ShengGmuLoadAndStartHfi (&device, &config, &result);
	panic ("NU735 s3 status=%d phase=%u hash=%u fw=%u hfi=%u\n",
	    (int) status, result.phase, result.complete_hash_verified,
	    result.firmware_init_result, result.hfi_status);
}

}

bool
NativeAdreno735::init (OSDictionary *dictionary)
{
	if (!IOService::init (dictionary)) {
		return false;
	}
	gGpuProbeRequested = false;
	PE_parse_boot_argn ("peridot-gpu", &gGpuProbeRequested, sizeof (gGpuProbeRequested));
	PE_parse_boot_argn ("peridot-gpu-panic", &gGpuPanicOnResult, sizeof (gGpuPanicOnResult));
	if (gGpuPanicOnResult) {
		// Stage -1 marker: this runs when the kext loads, before any provider match.
		panic ("NU735 init kext loaded\n");
	}
	return true;
}

void
NativeAdreno735::free (void)
{
	releaseWindows ();
	IOService::free ();
}

static bool
isGpuNode (IOService *provider)
{
	const char *name = provider ? provider->getName () : nullptr;
	if (name && (strcmp (name, "gpu") == 0 || strncmp (name, "gpu@", 4) == 0)) {
		return true;
	}
	// The ADT carries the node's reg cells big-endian; the GPU window is 0x3d00000.
	OSData *reg = provider ? OSDynamicCast (OSData, provider->getProperty ("reg")) : nullptr;
	if (reg && reg->getLength () >= 8) {
		const uint8_t *p = (const uint8_t *) reg->getBytesNoCopy ();
		uint32_t address = ((uint32_t) p[4] << 24) | ((uint32_t) p[5] << 16) |
		    ((uint32_t) p[6] << 8) | (uint32_t) p[7];
		if (address == 0x03d00000) {
			return true;
		}
	}
	return false;
}

IOService *
NativeAdreno735::probe (IOService *provider, SInt32 *score)
{
	// Nothing is claimed unless the probe was asked for; a default boot must not
	// gain a new GPU service, even an idle one.
	if (!gGpuProbeRequested || !isGpuNode (provider)) {
		return nullptr;
	}
	if (score) {
		*score = 1000;
	}
	return this;
}

void
NativeAdreno735::releaseWindows (void)
{
	for (unsigned i = 0; i < 5; ++i) {
		if (fMaps[i]) {
			fMaps[i]->release ();
			fMaps[i] = nullptr;
		}
		fMmio.windows[i].cpu = nullptr;
	}
}

bool
NativeAdreno735::start (IOService *provider)
{
	if (!IOService::start (provider)) {
		return false;
	}
	if (!gGpuProbeRequested) {
		return true;
	}
	if (gGpuPanicOnResult) {
		// Stage 0 marker: reaching this line proves the personality matched the
		// provider and start() ran. Absent it, the driver never claimed the node.
		panic ("NU735 enter provider=%s\n", provider && provider->getName () ? provider->getName () : "?");
	}
	if (!bringUp ()) {
		return false;
	}
	setProperty ("gpu-step", gGpuStep, 32);
	setProperty ("gpu-status", (uint32_t) gGpuLastStatus, 32);
	IOLog ("NativeAdreno735: step=%u status=%d core=0x%x\n", gGpuStep, gGpuLastStatus, fCoreId);
	return true;
}

void
NativeAdreno735::stop (IOService *provider)
{
	releaseWindows ();
	IOService::stop (provider);
}

// Stage 1: map the five windows exactly as the platform kit describes them and
// bind them. No register is read or written here.
bool
NativeAdreno735::bringUp (void)
{
	static const char *names[5] = { "gmu", "rscc", "pdc", "gpu", "cx_misc" };

	for (unsigned i = 0; i < 5; ++i) {
		ShengGmuMmioWindow *w = &fMmio.windows[i];
		w->physical = sheng_gmu_window_physical[i];
		w->bytes    = sheng_gmu_window_bytes[i];
		IOMemoryDescriptor *mem = IOMemoryDescriptor::withPhysicalAddress (
		    (IOPhysicalAddress) w->physical, w->bytes, kIODirectionNone);
		if (!mem) {
			IOLog ("NativeAdreno735: no descriptor for %s at 0x%llx\n", names[i], (unsigned long long) w->physical);
			return false;
		}
		IOMemoryMap *map = mem->map ();
		mem->release ();
		if (!map || !map->getVirtualAddress ()) {
			if (map) {
				map->release ();
			}
			IOLog ("NativeAdreno735: cannot map %s at 0x%llx\n", names[i], (unsigned long long) w->physical);
			return false;
		}
		fMaps[i] = map;
		w->cpu = (volatile unsigned char *) map->getVirtualAddress ();
	}
	gGpuStep = 1;

	if (ShengGmuBindMmio (&fMmio, &fIo) != ShengGmuSuccess) {
		IOLog ("NativeAdreno735: ShengGmuBindMmio rejected the window map\n");
		return false;
	}
	gGpuStep = 2;

	// TODO(stage 3): the GMU firmware. The image is gen71100_gmu.bin (80,680
	// bytes, sha256 435c7bac...), which has to be embedded or read from the
	// payload before ShengGmuLoadAndStartHfi can run; SHENG_GMU_FIRMWARE_BYTES
	// and sheng_gmu_firmware_sha256 are already in the generated header.
	//
	// TODO(stage 4): the five GMU memories (icache, dummy, debug, log, hfi) with
	// the sizes and IOVAs the kit validates, allocated from owned, coherent,
	// non-overlapping memory and mapped through the GPU SMMU context.
	//
	// TODO(stage 5): the services the kit requires (sha256, now_us, wait_us) and
	// RFI. Until those exist ShengGmuLoadAndStartHfi correctly refuses to run.
	fCoreId = 0;
	gGpuLastStatus = ShengGmuStateMismatch;
	gGpuStep = 2;
	if (gGpuPanicOnResult) {
		/* Diagnostic build: the screen keeps these four values, and the next boot prints
		 * them again through peridot-last-panic-print.patch. */
		panic ("NU735 gpu step=%u status=%d\n", gGpuStep, gGpuLastStatus);
	}
	return true;
}