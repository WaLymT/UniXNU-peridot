#!/usr/bin/env python3
"""Extract an Apple UDIF dmg whose data fork is LZFSE-compressed.

The iPadOS restore images (e.g. the "OS" entry in BuildManifest.plist) are UDIF
containers: a koly trailer at the end, an XML resource fork before it, and a data
fork made of LZFSE streams. This tool reassembles the requested partition's APFS
container into a raw image:

  udif-extract.py <dmg> <output.img> [--blkx NAME] [--lzfse PATH]

--blkx selects the blkx table (default "Apple_APFS"); NAME may be omitted to
extract the whole logical disk. --lzfse points at a working lzfse CLI
(e.g. /opt/unixnu-linux/bin/lzfse).

Blkx descriptor layout (big endian, all confirmed on 17.7.11 21H461):
  header 204 bytes, then 40-byte descriptors:
  type u32, comment u32, sectorNumber u64, sectorCount u64,
  compressedOffset u64 (absolute in the dmg), compressedLength u64.
Types: 0x1 raw, 0x2 zero, 0x80000007 LZFSE, 0xffffffff end sentinel.
"""
import pathlib
import plistlib
import struct
import subprocess
import sys
import tempfile

HEADER = 204
TYPE_RAW = 0x1
TYPE_ZERO = 0x2
TYPE_LZFSE = 0x80000007
TYPE_END = 0xffffffff


def parse(dmg):
    size = dmg.stat().st_size
    with dmg.open('rb') as f:
        f.seek(size - 512)
        koly = f.read(512)
        if koly[:4] != b'koly':
            raise SystemExit('no koly trailer: not a UDIF dmg')
        xml_off, xml_len = struct.unpack('>QQ', koly[216:232])
        f.seek(xml_off)
        xml = f.read(xml_len)
    plist = plistlib.loads(xml)
    return plist['resource-fork']['blkx']


def descriptors(blkx):
    d = blkx['Data']
    for k in range((len(d) - HEADER) // 40):
        o = HEADER + k * 40
        t, cmt, sector, count, off, length = struct.unpack('>IIQQQQ', d[o:o + 40])
        if t == TYPE_END:
            continue
        yield t, sector, count, off, length


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    dmg = pathlib.Path(argv[1])
    out = pathlib.Path(argv[2])
    blkx_name = 'Apple_APFS'
    lzfse = '/opt/unixnu-linux/bin/lzfse'
    i = 3
    while i < len(argv):
        if argv[i] == '--blkx':
            i += 1
            blkx_name = argv[i]
        elif argv[i] == '--lzfse':
            i += 1
            lzfse = argv[i]
        else:
            raise SystemExit('unknown argument ' + argv[i])
        i += 1
    tables = parse(dmg)
    if blkx_name == '':
        selected = tables
        base_sector = 0
    else:
        selected = [t for t in tables if t.get('Name') == blkx_name]
        if not selected:
            raise SystemExit('no blkx named ' + blkx_name)
        base_sector = 0
    descs = []
    for t in selected:
        for d in descriptors(t):
            descs.append(d)
    total_sectors = max(sector + count for t, sector, count, off, length in descs)
    print('descriptors', len(descs), 'sectors', total_sectors, flush=True)
    tmp_in = pathlib.Path(tempfile.mkstemp(prefix='udif-', suffix='.lz')[1])
    tmp_out = pathlib.Path(tempfile.mkstemp(prefix='udif-', suffix='.raw')[1])
    errors = 0
    with dmg.open('rb') as src, out.open('wb') as dst:
        for idx, (t, sector, count, off, length) in enumerate(descs):
            pos = sector * 512
            size = count * 512
            if t == TYPE_ZERO:
                continue
            src.seek(off)
            data = src.read(length)
            if len(data) != length:
                raise SystemExit('short read in the data fork')
            if t == TYPE_RAW:
                raw = data
            elif t == TYPE_LZFSE:
                tmp_in.write_bytes(data)
                rc = subprocess.run([lzfse, '-decode', '-i', str(tmp_in), '-o', str(tmp_out)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if rc.returncode != 0:
                    print('lzfse failed at descriptor', idx, flush=True)
                    errors += 1
                    break
                raw = tmp_out.read_bytes()
            else:
                print('unsupported descriptor type', hex(t), flush=True)
                errors += 1
                break
            if len(raw) != size:
                print('decoded size mismatch at', idx, len(raw), '!=', size, flush=True)
                errors += 1
                break
            dst.seek(pos)
            dst.write(raw)
            if (idx % 500) == 0:
                print('...', idx, flush=True)
        dst.truncate(total_sectors * 512)
    tmp_in.unlink(missing_ok=True)
    tmp_out.unlink(missing_ok=True)
    print('done errors=%d size=%d' % (errors, out.stat().st_size), flush=True)
    return 0 if errors == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
