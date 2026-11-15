#!/usr/bin/env python3
"""Derive the IOStorageFamily bindings and fold them into libShengUfs.a.

Run from the build tree with no arguments; board.toml lists it as an archive step.

Two jobs:

1. Audit. Locate IOBlockStorageDevice's dispatch table in the prelinked kernel.
   The upstream audit matches vtable slots against kext symbol *names*, which
   peridot cannot do: the original kernelcache strips them and our prelinked
   kernel exports none of them. Instead the table is found structurally - 162
   slots equal to __ZTV9IOService's entries at the same index, 14 overrides
   inside IOStorageFamily's __TEXT_EXEC extent, and 14 pure-virtual slots sharing
   one trap outside it - which is the same 190-slot shape the upstream audit
   reports. Exactly one candidate is accepted.

2. Bind. NativeUfsBlock imports IOBlockStorageDevice's vtable, constructor,
   destructor, metaclass and seven virtual methods. Each address is recovered
   from the audited table or from the kext text with a pattern check: the
   constructor must store the table and forward to IOService::IOService, the
   destructor must forward to the real IOService D2, the metaclass getter must
   have the adrp/add form. A wrong address here is written to during start().

peridot has no signed-volume policy and no boot-progress layer, so both are
stubbed: every read through the verified path is refused rather than accepted.
That is the safe direction, and the read-only probes (native-ufs-probe=2 and
=10) are what to run first.
"""
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

TREE = Path.cwd()
sys.path.insert(0, str(TREE / "kits/XnuPrelink"))
import native_prelink as np  # noqa: E402

PRE_LINKED = TREE.parent / "kernel/kernel.macho"
ORIGINAL = next(Path("/root/work/UniXNU/out/ipsw").glob("*/kernelcache.decompressed"), None)
if ORIGINAL is None:
    raise SystemExit("no kernelcache.decompressed under out/ipsw")

ROOTS = """com.apple.AppleFSCompression.AppleFSCompressionTypeZlib
com.apple.driver.AppleARMPlatform
com.apple.driver.AppleARMWatchdogTimer
com.apple.driver.AppleAudioClockLibs
com.apple.driver.AppleEffaceableStorage
com.apple.driver.AppleIISController
com.apple.driver.AppleM2ScalerCSCDriver
com.apple.driver.AppleMobileFileIntegrity
com.apple.driver.ApplePMGR
com.apple.driver.IODARTFamily
com.apple.filesystems.apfs
com.apple.iokit.CoreAnalyticsFamily
com.apple.iokit.IOAVFamily
com.apple.iokit.IOAudio2Family
com.apple.iokit.IOCECFamily
com.apple.iokit.IOCryptoAcceleratorFamily
com.apple.iokit.IOGPUFamily
com.apple.iokit.IOHDCPFamily
com.apple.iokit.IOMobileGraphicsFamily
com.apple.iokit.IOReportFamily
com.apple.iokit.IOSlowAdaptiveClockingFamily
com.apple.iokit.IOStorageFamily
com.apple.iokit.IOSurface
com.apple.kec.Libm
com.apple.kec.corecrypto
com.apple.kext.AppleMatch
com.apple.kext.CoreTrust
com.apple.security.AppleImage4
com.apple.security.sandbox
com.apple.iokit.IOHIDFamily
com.apple.iokit.IOHIDEventDriver""".split("\n")

CFLAGS_EXTRA = ["-I", str(TREE / "build/storage-abi/include"),
                "-I", str(TREE / ".cache/xnu-work/osfmk"),
                "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-Wno-deprecated-declarations", "-Wno-c99-designator",
                "-Wno-reorder-init-list", "-Wno-unnecessary-virtual-specifier",
                "-Wno-deprecated-enum-enum-conversion",
                "-Wno-missing-designated-field-initializers"]

OUT_DIR = TREE / "build/storage-abi"
OUT_DIR.mkdir(parents=True, exist_ok=True)


def reference_methods():
    """Compile the public class and read its dispatch order from the object."""
    source = TREE / "refer/IOStorageFamily/IOBlockStorageDevice.cpp"
    record = json.loads((TREE / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/"
                                "IOPlatformExpert.cpo.json").read_text().strip().rstrip(","))
    obj_path = OUT_DIR / "IOBlockStorageDevice-reference.o"
    command, index, arguments = [], 0, record["arguments"]
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(source))
        elif value == "-o":
            command += ["-o", str(obj_path)]
            index += 1
        elif value in ("-MF", "-MJ"):
            index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif not value.startswith("-W") and value not in ("-MD", "-MP"):
            command.append(value)
        index += 1
    command += CFLAGS_EXTRA
    result = subprocess.run(command, cwd=record["directory"], capture_output=True, text=True)
    (OUT_DIR / "reference-compile.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit((result.stdout + result.stderr)[-3000:])

    obj = obj_path.read_bytes()
    image = np.macho(obj)
    offset = next(at for cmd, at, size in image["commands"] if cmd == 2)
    symoff, count, stroff, _ = struct.unpack_from("<4I", obj, offset + 8)
    symbols = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from("<IBBHQ", obj, symoff + i * 16)
        symbols.append((obj[stroff + ix:obj.index(b"\0", stroff + ix)].decode(), value, kind))
    vtable = next(v for name, v, kind in symbols if name == "__ZTV20IOBlockStorageDevice")
    end = min(v for name, v, kind in symbols if not kind & 0xE0 and kind & 0xE == 0xE and v > vtable)
    relocations = {}
    for segment in image["segments"]:
        for section in segment["sections"]:
            if section["va"] <= vtable < section["va"] + section["size"]:
                ro, rc = struct.unpack_from("<II", obj, section["command_offset"] + 56)
                for i in range(rc):
                    address, word = struct.unpack_from("<II", obj, ro + i * 8)
                    if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                        relocations[section["va"] + address] = symbols[word & 0xFFFFFF][0]
    methods = []
    for at in range(vtable + 16, end, 8):
        if at not in relocations:
            break
        methods.append(relocations[at])
    return obj, methods


def main():
    obj, methods = reference_methods()
    if len(methods) != 190 or methods.count("___cxa_pure_virtual") != 14:
        raise SystemExit(f"reference dispatch shape changed: {len(methods)} slots")

    prelinked_bytes = PRE_LINKED.read_bytes()
    prelinked = np.macho(prelinked_bytes)
    exports = np.exports(prelinked_bytes, prelinked)
    by_name = {name: va for va, names in exports.items() for name in names}
    parent_vtable = by_name["__ZTV9IOService"]

    def read(data_image, data, va, length):
        at = np.backed_offset(data_image["segments"], va, length, len(data))
        return data[at:at + length]

    parent_slots = struct.unpack_from("<190Q", read(prelinked, prelinked_bytes, parent_vtable + 16, 190 * 8))
    original_bytes = ORIGINAL.read_bytes()
    native_path = next((TREE / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837").glob("mach.development.bcm2837.unstripped*"))
    plan = np.plan(native_path.read_bytes(), original_bytes, ROOTS, -0x1000000)
    extents = [e for e in plan["extents"] if e["module"] == "com.apple.iokit.IOStorageFamily"]
    const = next(e for e in extents if e["segment"] == "__DATA_CONST")
    text = next(e for e in extents if e["segment"] == "__TEXT_EXEC")
    blob = read(prelinked, prelinked_bytes, const["va"], const["size"])
    text_lo, text_hi = text["va"], text["va"] + text["size"]
    found = []
    for at in range(0, len(blob) - 192 * 8 + 1, 8):
        if blob[at:at + 16] != bytes(16):
            continue
        pointers = struct.unpack_from("<190Q", blob, at + 16)
        matched = sum(pointers[i] == parent_slots[i] for i in range(190))
        rest = [p for i, p in enumerate(pointers) if pointers[i] != parent_slots[i]]
        overrides = [p for p in rest if text_lo <= p < text_hi]
        traps = [p for p in rest if not text_lo <= p < text_hi]
        if matched == 162 and len(overrides) == 14 and len(traps) == 14 and len(set(traps)) == 1:
            found.append((const["va"] + at, pointers))
    if len(found) != 1:
        raise SystemExit(f"expected one dispatch table, found {len(found)}")
    prefix, pointers = found[0]
    table = prefix + 16
    print(f"dispatch table {table:#x}: 162 parent slots, 14 overrides, 14 traps")

    audit = {
        "source": "peridot: scripts/build-ufs-bindings.py (structural derivation)",
        "method": "parent-slot matching against __ZTV9IOService; the upstream name-matching candidate does not exist on this board",
        "reference_object_sha256": hashlib.sha256(obj).hexdigest(),
        "reference_object_executed_or_linked": False,
        "prelinked_kernel_sha256": hashlib.sha256(prelinked_bytes).hexdigest(),
        "original_cache_sha256": hashlib.sha256(original_bytes).hexdigest(),
        "parent_vtable": hex(parent_vtable),
        "method_slots": 190,
        "pure_virtual_slots": 14,
        "kernel_bindings": 162,
        "vtable": hex(prefix),
        "storage_extents": {e["segment"]: {"va": hex(e["va"]), "size": hex(e["size"])} for e in extents},
        "upstream_qualifications_not_performed": [
            "name-matched vtable discovery against a qualified sheng prelink candidate",
            "live GDB observation of the metaclass and instance sizes",
            "macOS SDK platform header pin",
        ],
        "methods": [dict(slot=i, reference_name=name, candidate_target=hex(pointers[i]))
                    for i, name in enumerate(methods)],
        "disk_read_tested": False,
        "transport_implemented": False,
        "hardware_tested": False,
    }
    (OUT_DIR / "interface-bindings.json").write_text(json.dumps(audit, indent=2) + "\n")

    def readp(va, length):
        return read(prelinked, prelinked_bytes, va, length)

    def relative_branch(address, instruction):
        immediate = instruction & 0x3FFFFFF
        if immediate & 0x2000000:
            immediate -= 0x4000000
        return address + immediate * 4

    def thunk_names(address):
        a, load, branch = struct.unpack("<3I", readp(address, 12))
        if a & 0x9F00001F not in (0x90000010, 0x90000000):
            return []
        if load & 0xFFC003FF not in (0xF9400210, 0xF9400010):
            return []
        if branch not in (0xD61F0200, 0xD65F03C0):
            return []
        immediate = ((a >> 5 & 0x7FFFF) << 2) | (a >> 29 & 3)
        if immediate & 0x100000:
            immediate -= 0x200000
        slot = (address & ~4095) + immediate * 4096 + (load >> 10 & 4095) * 8
        return exports.get(struct.unpack("<Q", readp(slot, 8))[0], [])

    destructor = pointers[0] - 4
    word = struct.unpack("<I", readp(destructor, 4))[0]
    if word & 0xFC000000 != 0x14000000 or "__ZN9IOServiceD2Ev" not in thunk_names(relative_branch(destructor, word)):
        raise SystemExit("the base destructor does not forward to IOService D2")
    constructors = []
    for address in range(text_lo, text_hi - 32, 4):
        words = struct.unpack("<8I", readp(address, 32))
        if (words[:2] != (0xA9BF7BFD, 0x910003FD) or words[2] & 0xFC000000 != 0x94000000 or
                words[3] & 0x9F00001F != 0x90000008 or words[4] & 0xFFC003FF != 0x91000108 or
                words[5:] != (0xF9000008, 0xA8C17BFD, 0xD65F03C0)):
            continue
        immediate = ((words[3] >> 5 & 0x7FFFF) << 2) | (words[3] >> 29 & 3)
        if immediate & 0x100000:
            immediate -= 0x200000
        stored = ((address + 12) & ~4095) + immediate * 4096 + (words[4] >> 10 & 4095)
        if stored == table:
            constructors.append((address, relative_branch(address + 8, words[2])))
    if len(constructors) != 1:
        raise SystemExit(f"expected one constructor, found {len(constructors)}")
    constructor, forward = constructors[0]
    if "__ZN9IOServiceC2EPK11OSMetaClass" not in thunk_names(forward):
        raise SystemExit("the protected constructor does not forward to IOService::IOService")
    function = pointers[7]
    a, add, ret = struct.unpack("<3I", readp(function, 12))
    if a & 0x9F00001F != 0x90000000 or add & 0xFFC003FF != 0x91000000 or ret != 0xD65F03C0:
        raise SystemExit("the metaclass getter has an unexpected shape")
    immediate = ((a >> 5 & 0x7FFFF) << 2) | (a >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    metaclass = (function & ~4095) + immediate * 4096 + (add >> 10 & 4095)
    print(f"ctor {constructor:#x}  dtor {destructor:#x}  meta {metaclass:#x}")

    object_path = TREE / "build/sheng-ufs/NativeUfs.o"
    if not object_path.is_file():
        # UniXNU names the object after the catalog id, which became ShengUfs.
        object_path = TREE / "build/sheng-ufs/ShengUfs.o"
    obj_bytes = object_path.read_bytes()
    image = np.macho(obj_bytes)
    symtab = next(at for cmd, at, size in image["commands"] if cmd == 2)
    symoff, count, stroff, _ = struct.unpack_from("<4I", obj_bytes, symtab + 8)
    undefined = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from("<IBBHQ", obj_bytes, symoff + i * 16)
        name = obj_bytes[stroff + ix:obj_bytes.index(b"\0", stroff + ix)].decode()
        if kind == 1 and "IOBlockStorageDevice" in name:
            undefined.append(name)
    known = {row["reference_name"]: int(row["candidate_target"], 16) for row in audit["methods"]}
    special = {"__ZTV20IOBlockStorageDevice": table,
               "__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass": constructor,
               "__ZN20IOBlockStorageDeviceD2Ev": destructor,
               "__ZN20IOBlockStorageDevice10gMetaClassE": metaclass}
    bindings = {}
    for name in undefined:
        if name in special:
            bindings[name] = special[name]
        elif name in known:
            bindings[name] = known[name]
        else:
            raise SystemExit(f"no qualified original for {name}")

    assembly = TREE / "build/sheng-ufs/libShengUfs.S"
    lines = [f".globl {name}\n.set {name}, {value:#x}\n" for name, value in sorted(bindings.items())]
    # VerifiedRoot is a signed-volume policy kit and peridot has no signed
    # volume, so every entry point refuses: vr_open and vr_verify_block return
    # -1, the crypto table is zeroed and no policy is published. The verified
    # read path therefore fails closed; the plain transport is unaffected, and
    # the read-only probes are what to run first.
    lines.append(
        ".globl _boot_progress_poll\n.section __TEXT,__text\n.p2align 2\n_boot_progress_poll:\n    ret\n"
    )
    # Refusals for the verified read path: this board publishes no signed volume, so
    # vr_open/vr_verify_block fail and the crypto table stays zero. They were linked
    # from kits/VerifiedRoot before; the board no longer selects that kit.
    lines.append(
        ".globl _vr_open\n_vr_open:\n    mov w0, #-1\n    ret\n"
        ".globl _vr_verify_block\n_vr_verify_block:\n    mov w0, #-1\n    ret\n"
        ".globl _vr_xnu_crypto\n.section __DATA,__data\n.p2align 3\n_vr_xnu_crypto:\n    .space 512\n"
    )
    owner = TREE / "drivers/ShengUfs/OwnerRootPolicies.s"
    if owner.exists():
        lines.append(owner.read_text())
    assembly.write_text("".join(lines))
    bindings_object = TREE / "build/sheng-ufs/original-bindings.o"
    subprocess.run(["clang", "-target", "arm64-apple-ios17.0", "-c", str(assembly), "-o", str(bindings_object)], check=True)
    archive = TREE / "build/sheng-ufs/libShengUfs.a"
    archive.unlink(missing_ok=True)
    extras = [str(other) for other in archive.parent.glob("*.o")
           if other.name not in (object_path.name, bindings_object.name)]
    subprocess.run(["llvm-ar", "rcs", str(archive), str(object_path), str(bindings_object)] + extras, check=True)
    print(f"bound {len(bindings)} imports into {archive}")


main()
