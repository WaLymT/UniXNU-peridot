#!/usr/bin/env python3
"""patch-cryptex-graft.py - kernel-side helpers for the SystemOS cryptex bring-up.

Adds to bsd/vfs/vfs_syscalls.c:

  int peridot_path_probe(path, *size, *ino)        namei + vn_stat probe
  int peridot_cryptex_graft(dev, mnt, type, 4cc, flags)
                                                    APFS graft ioctl attempt
  int peridot_apfs_mount(dev, mnt)                  kernel_mount() attempt

and to bsd/vfs/kpi_vfs.c:

  APFS mount-argument logging in VFS_MOUNT (peridot mountargs ...), used to
  capture the argument blob that the regular mount(2) path hands to APFS
  (mount_apfs), so the kernel-side attempts can be compared field by field.

Both files are generated against the feed source; nothing else in the series
touches them.
"""
import difflib
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
_feed = os.environ.get("XNU_FEED")
if not _feed:
    raise SystemExit("set XNU_FEED to the prepared XNU feed source directory")
FEED = pathlib.Path(_feed)
TARGET = 'bsd/vfs/vfs_syscalls.c'
META = ROOT / 'patches/xnu/cryptex'
baseline_file = FEED / TARGET
if not baseline_file.is_file():
    raise SystemExit('feed source missing: ' + str(baseline_file))
baseline = baseline_file.read_text()


def carray(name, data):
    rows = []
    for i in range(0, len(data), 12):
        rows.append('\t' + ', '.join('0x%02x' % b for b in data[i:i + 12]) + ',')
    return 'static const uint8_t %s[] = {\n%s\n};\n' % (name, '\n'.join(rows))


root_hash = (META / '044-76034-038.root_hash').read_bytes()
trustcache = (META / '044-76034-038.trustcache').read_bytes()

anchor = '''/*
 * Ungraft a cryptex disk image (via mount dir FD)
 * { int ungraftdmg(const char *mountdir, uint64_t flags); }
 */'''

helpers = '''/*
 * peridot: kernel-side helpers for the UFS-root bring-up. launchd cannot run
 * userland helpers on the staged iPadOS System volume (dyld has no shared cache
 * until the SystemOS cryptex is grafted), so the driver calls these directly.
 */

extern errno_t vnode_open(const char *path, int mode, int cmode, int flags, vnode_t *vpp,
    vfs_context_t ctx);
extern int vnode_close(vnode_t vp, int flags, vfs_context_t ctx);
extern errno_t vnode_lookup(const char *path, int flags, vnode_t *vpp, vfs_context_t ctx);

__AUTH_ARRAY__

__PAYLOAD_ARRAY__

/* namei + vn_stat of an absolute path on the current root. */
int
peridot_path_probe(const char *path, uint64_t *out_size, unsigned *out_ino)
{
	vfs_context_t ctx = vfs_context_current();
	struct nameidata nd = {};
	vnode_t vp = NULLVP;
	struct stat64 sb = {};
	int error;

	if (!path || !path[0]) {
		return EINVAL;
	}
	NDINIT(&nd, LOOKUP, OP_LOOKUP, (FOLLOW | AUDITVNPATH1), UIO_SYSSPACE,
	    (user_addr_t)(uintptr_t)path, ctx);
	error = namei(&nd);
	if (error) {
		return error;
	}
	vp = nd.ni_vp;
	nameidone(&nd);
	if (vp == NULLVP) {
		return ENOENT;
	}
	error = vn_stat(vp, &sb, NULL, 1, 0, ctx);
	if (!error) {
		if (out_size) {
			*out_size = (uint64_t)sb.st_size;
		}
		if (out_ino) {
			*out_ino = (unsigned)sb.st_ino;
		}
	}
	vnode_put(vp);
	return error;
}

/* Attach a cryptex image/device to a directory with APFS's graft ioctl. */
int
peridot_cryptex_graft(const char *dev_path, const char *mount_path, uint32_t graft_type,
    uint32_t graft_4cc, uint64_t graft_flags)
{
	vfs_context_t ctx = vfs_context_current();
	vnode_t dmg_vp = NULLVP;
	vnode_t mnt_vp = NULLVP;
	struct stat64 sb = {};
	fsioc_graft_fs_t gfs = {};
	uint64_t dir_ino = 0;
	int error;

	if (!dev_path || !dev_path[0]) {
		return EINVAL;
	}
	error = vnode_open(dev_path, FREAD, 0, 0, &dmg_vp, ctx);
	if (error) {
		return error;
	}
	if (mount_path && mount_path[0]) {
		error = vnode_lookup(mount_path, 0, &mnt_vp, ctx);
		if (error) {
			vnode_close(dmg_vp, FREAD, ctx);
			return error;
		}
		if (mnt_vp->v_type != VDIR) {
			vnode_put(mnt_vp);
			vnode_close(dmg_vp, FREAD, ctx);
			return ENOTDIR;
		}
		error = vn_stat(mnt_vp, &sb, NULL, 1, 0, ctx);
		if (error) {
			vnode_put(mnt_vp);
			vnode_close(dmg_vp, FREAD, ctx);
			return error;
		}
		dir_ino = (uint64_t)sb.st_ino;
	}
	gfs.graft_version = FSIOC_GRAFT_VERSION;
	gfs.graft_type = graft_type;
	gfs.graft_4cc = graft_4cc;
	gfs.graft_flags = graft_flags;
	gfs.dir_ino = dir_ino;
	gfs.authentic_manifest = (void *)peridot_auth_manifest;
	gfs.authentic_manifest_size = sizeof(peridot_auth_manifest);
	gfs.payload = (void *)peridot_payload;
	gfs.payload_size = sizeof(peridot_payload);
	error = VNOP_IOCTL(dmg_vp, FSIOC_GRAFT_FS, (caddr_t)&gfs, 0, ctx);
	if (mnt_vp) {
		vnode_put(mnt_vp);
	}
	vnode_close(dmg_vp, FREAD, ctx);
	return error;
}

/*
 * Mount an APFS container exposed as a block device (e.g. /dev/nu1).
 * The mount arguments are built by the caller in user memory; both the plain
 * mount() path (MACF refuses it for pid 1, p388) and this kernel_mount() path
 * are used during bring-up (p389+).
 */
int
peridot_apfs_mount(const char *dev_path, const char *mount_path)
{
	extern int kernel_mount(const char *fstype, vnode_t pvp, vnode_t vp, const char *path,
	    void *data, size_t datalen, int syscall_flags, uint32_t kern_flags,
	    vfs_context_t ctx);
	vfs_context_t ctx = vfs_context_kernel();
	int error;

	if (!mount_path || !mount_path[0]) {
		return EINVAL;
	}
	if (!dev_path || !dev_path[0]) {
		return kernel_mount("apfs", NULLVP, NULLVP, mount_path, NULL, 0, MNT_RDONLY,
		    KERNEL_MOUNT_NOAUTH, ctx);
	}
	char *dev_name = (char *)(uintptr_t)dev_path;
	char **dev_name_p = &dev_name;
	error = kernel_mount("apfs", NULLVP, NULLVP, mount_path, (void *)dev_name_p,
	    sizeof(dev_name_p), MNT_RDONLY,
	    KERNEL_MOUNT_NOAUTH | KERNEL_MOUNT_BASESYSTEMROOT, ctx);
	printf("peridot-cryptex: kernel_mount apfs dev=%s mnt=%s -> %d", dev_path,
	    mount_path, error);
	return error;
}
'''.replace('__AUTH_ARRAY__', carray('peridot_auth_manifest', root_hash)).replace(
    '__PAYLOAD_ARRAY__', carray('peridot_payload', trustcache))

patched = baseline
if 'peridot_path_probe' not in patched:
    if anchor not in patched:
        raise SystemExit('anchor for the helpers not found')
    patched = patched.replace(anchor, helpers + anchor, 1)
    fwd_anchor = '#define GRAFTDMG_ENTITLEMENT  "com.apple.private.vfs.graftdmg"'
    if fwd_anchor not in patched:
        raise SystemExit('entitlement anchor not found')
    patched = patched.replace(fwd_anchor,
        'int peridot_path_probe(const char *path, uint64_t *out_size, unsigned *out_ino);\n'
        'int peridot_cryptex_graft(const char *dev_path, const char *mount_path, uint32_t graft_type,\n'
        '    uint32_t graft_4cc, uint64_t graft_flags);\n'
        'int peridot_apfs_mount(const char *dev_path, const char *mount_path);\n\n'
        + fwd_anchor, 1)

patch_text = ''.join(difflib.unified_diff(
    baseline.splitlines(keepends=True), patched.splitlines(keepends=True),
    fromfile='a/' + TARGET, tofile='b/' + TARGET, n=3))

# --- kpi_vfs.c: log the APFS mount argument blob ---------------------------------
TARGET_MOUNT = 'bsd/vfs/kpi_vfs.c'
mount_baseline_file = FEED / TARGET_MOUNT
mount_baseline = mount_baseline_file.read_text()
mount_anchor = ('VFS_MOUNT(mount_t mp, vnode_t devvp, user_addr_t data, vfs_context_t ctx)\n'
                '{\n'
                '\tint error;\n')

mount_log = mount_anchor + '''	extern int peridotDumpMountArgs;
	/* peridot: log the APFS mount arguments for the cryptex bring-up. The
	 * regular mount(2) path hands APFS a user-space argument blob; probes
	 * 388-391 build the same structure from kernel memory, so log what APFS
	 * actually receives (p392). */
	if (data && mp->mnt_op->vfs_mount &&
	    strncmp(mp->mnt_vfsstat.f_fstypename, "apfs", 4) == 0) {
				extern int peridotDumpMountArgs;
		if (peridotDumpMountArgs == 1) {
			uint64_t peridotDump[16] = {};
			copyin(data, peridotDump, sizeof(peridotDump));
			panic("peridot mountargs dump data=%p devvp=%p "
			    "w0=%llx w1=%llx w2=%llx w3=%llx w4=%llx w5=%llx "
			    "w6=%llx w7=%llx w8=%llx w9=%llx",
			    (void *)(uintptr_t)data, devvp,
			    (unsigned long long)peridotDump[0],
			    (unsigned long long)peridotDump[1],
			    (unsigned long long)peridotDump[2],
			    (unsigned long long)peridotDump[3],
			    (unsigned long long)peridotDump[4],
			    (unsigned long long)peridotDump[5],
			    (unsigned long long)peridotDump[6],
			    (unsigned long long)peridotDump[7],
			    (unsigned long long)peridotDump[8],
			    (unsigned long long)peridotDump[9]);
		}
uint64_t peridotWords[4] = {};
		char peridotName14[48] = {};
		char peridotName1c[48] = {};
		if (copyin(data, peridotWords, sizeof(peridotWords)) == 0) {
			copyin(data + 0x14, peridotName14, sizeof(peridotName14) - 1);
			copyin(data + 0x1c, peridotName1c, sizeof(peridotName1c) - 1);
			printf("peridot mountargs data=%p devvp=%p w0=%llx w1=%llx w2=%llx "
			    "w3=%llx n14=%s n1c=%s\\n",
			    (void *)(uintptr_t)data, devvp,
			    (unsigned long long)peridotWords[0],
			    (unsigned long long)peridotWords[1],
			    (unsigned long long)peridotWords[2],
			    (unsigned long long)peridotWords[3],
			    peridotName14, peridotName1c);
		}
	}

'''

mount_patched = mount_baseline
if 'peridot mountargs' not in mount_patched:
    if mount_anchor not in mount_patched:
        raise SystemExit('VFS_MOUNT anchor missing in kpi_vfs.c')
    mount_patched = mount_patched.replace(mount_anchor, mount_log, 1)
result_anchor = chr(10) + chr(9) + 'return error;' + chr(10) + '}' + chr(10) + chr(10) + 'int' + chr(10) + 'VFS_START'
if result_anchor not in mount_patched:
    raise SystemExit('VFS_MOUNT result anchor missing')
mount_patched = mount_patched.replace(result_anchor,
    chr(10) + chr(9) + 'if (peridotDumpMountArgs == 2) {' + chr(10)
    + chr(9) + chr(9) + 'panic("peridot mountargs result err=%d", error);' + chr(10)
    + chr(9) + '}' + chr(10) + result_anchor, 1)
mount_patch_text = ''.join(difflib.unified_diff(
    mount_baseline.splitlines(keepends=True), mount_patched.splitlines(keepends=True),
    fromfile='a/' + TARGET_MOUNT, tofile='b/' + TARGET_MOUNT, n=3))

target = ROOT / 'patches/xnu/peridot-cryptex-graft.patch'
target.write_text(patch_text + mount_patch_text)

check_dir = pathlib.Path('/tmp/cryptex-graft-check')
if check_dir.exists():
    shutil.rmtree(check_dir)
(check_dir / TARGET).parent.mkdir(parents=True)
(check_dir / TARGET_MOUNT).parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(baseline_file, check_dir / TARGET)
shutil.copyfile(mount_baseline_file, check_dir / TARGET_MOUNT)
check = subprocess.run(['patch', '-p1', '--dry-run', '--forward', '-i', str(target)],
                       cwd=str(check_dir), capture_output=True, text=True)
print('wrote', target, len(patch_text) + len(mount_patch_text), 'bytes; dry-run rc', check.returncode)
print(check.stdout[-300:])
raise SystemExit(0 if check.returncode == 0 else 1)
