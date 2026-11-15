#!/usr/bin/env python3
"""Build an Android sparse image for fastboot from (offset, file) regions.

  build-userdata-sparse.py <out.sparse.img> OFF=FILE [OFF=FILE ...]

Each region is placed at byte offset OFF in the partition; everything else is
a DON'T CARE chunk (holes). Used to stage the extracted iPadOS APFS container
into userdata without transferring hundreds of gigabytes: the 1 GiB offset that
the kernel probe (native-ufs-probe=43) expects plus the restore container kept
at userdata offset 0.

Sparse format: 28-byte header, 12-byte chunk headers, 4 KiB blocks; RAW chunks
for non-zero data, DON'T CARE chunks for zero/hole regions.
"""
import pathlib
import struct
import sys

SPARSE_MAGIC = 0xed26ff3a
CHUNK_RAW = 0xCAC1
CHUNK_DONT_CARE = 0xCAC3
BLK = 4096
BLOCK_LIMIT = 0x1000000          # u32 chunk_sz safety limit
MAX_CHUNK_BYTES = 4 * 1024 * 1024


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    out = pathlib.Path(argv[1])
    regions = []
    for spec in argv[2:]:
        off, _, path = spec.partition('=')
        regions.append((int(off, 0), pathlib.Path(path)))
    regions.sort()
    total_bytes = 0
    for off, path in regions:
        if path.stat().st_size == 0:
            raise SystemExit('empty region: ' + str(path))
        total_bytes = max(total_bytes, off + path.stat().st_size)
    total_bytes = (total_bytes + BLK - 1) // BLK * BLK
    total_blocks = total_bytes // BLK
    print('total bytes', total_bytes, 'blocks', total_blocks, flush=True)
    chunks = 0
    cursor = 0
    zero = bytes(MAX_CHUNK_BYTES)
    with out.open('wb') as sink:
        sink.write(b'\0' * 28)

        def dont_care(nblocks):
            nonlocal chunks, cursor
            while nblocks > 0:
                n = min(nblocks, BLOCK_LIMIT)
                sink.write(struct.pack('<HHII', CHUNK_DONT_CARE, 0, n, 12))
                chunks += 1
                nblocks -= n
            cursor += 0

        for off, path in regions:
            gap_blocks = (off - (cursor * BLK)) // BLK
            if gap_blocks > 0:
                dont_care(gap_blocks)
                cursor += gap_blocks
            with path.open('rb') as src:
                remaining = path.stat().st_size
                while remaining > 0:
                    n = min(remaining, MAX_CHUNK_BYTES)
                    data = src.read(n)
                    if len(data) != n:
                        raise SystemExit('short read: ' + str(path))
                    if data == zero[:n]:
                        sink.write(struct.pack('<HHII', CHUNK_DONT_CARE, 0, n // BLK, 12))
                    else:
                        sink.write(struct.pack('<HHII', CHUNK_RAW, 0, n // BLK, 12 + n))
                        sink.write(data)
                    chunks += 1
                    cursor += n // BLK
                    remaining -= n
        pad_blocks = total_blocks - cursor
        if pad_blocks > 0:
            dont_care(pad_blocks)
        sink.seek(0)
        sink.write(struct.pack('<IHHHHIIII', SPARSE_MAGIC, 1, 0, 28, 12, BLK,
                               total_blocks, chunks, 0))
    print('chunks', chunks, 'file size', out.stat().st_size, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
