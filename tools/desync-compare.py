#!/usr/bin/env python3
"""Compare the memory snapshots two PCs wrote at the same desync (build 121+).

After a desync online, each PC saves the fight state of one agreed frame and keeps the
parts that differ from the other PC in desync_<date>_<time>_P<n>_f<frame>.bin next to the
mod (Pack logs includes it). Give this script the file of each PC:

    python desync-compare.py desync_..._P1_f1234.bin desync_..._P2_f1234.bin

It lists every 4-byte value that differs, by place in memory, leaving out values that look
like pointers on either PC (heap addresses are different on every PC).
"""
import struct
import sys

FP_BASE, CHAR_SIZE = 0x05130CD8, 0xA520   # the two characters (net_session.inc)


def read(path):
    with open(path, "rb") as f:
        b = f.read()
    if not b.startswith(b"ARSNAP01"):
        sys.exit(f"{path}: not a desync snapshot file")
    meta = struct.unpack_from("<8I", b, 32)
    pos = 64
    chunks = {}
    while pos < len(b):
        lst, idx, addr, ln, bstart, bsize = struct.unpack_from("<BIIIII", b, pos)
        pos += 21
        data = b[pos:pos + ln]
        pos += ln
        nmask = (ln // 4 + 31) // 32
        mask = struct.unpack_from(f"<{nmask}I", b, pos)
        pos += nmask * 4
        chunks[(lst, idx)] = (addr, ln, bstart, bsize, data, mask)
    return {"side": meta[0], "epoch": meta[1], "frame": meta[2], "chunks": chunks, "header": b[:32].rstrip(b"\0").decode()}


def masked(mask, w):
    return (mask[w // 32] >> (w % 32)) & 1


def where(lst, addr, bstart, bsize):
    if lst == 0:
        if FP_BASE <= addr < FP_BASE + 2 * CHAR_SIZE:
            o = addr - FP_BASE
            return f"P{o // CHAR_SIZE + 1}+0x{o % CHAR_SIZE:X} (.data 0x{addr:08X})"
        return f".data 0x{addr:08X}"
    return f"heap block {bsize} bytes +0x{addr - bstart:X}"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    a, b = read(sys.argv[1]), read(sys.argv[2])
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    print(f"A: {sys.argv[1]} ({a['header']}, player {a['side']}, screen {a['epoch']}, frame {a['frame']})")
    print(f"B: {sys.argv[2]} ({b['header']}, player {b['side']}, screen {b['epoch']}, frame {b['frame']})")
    if a["frame"] != b["frame"]:
        print("warning: the two files are of different frames")
    keys = sorted(set(a["chunks"]) | set(b["chunks"]))
    total = 0
    for k in keys:
        if k not in a["chunks"] or k not in b["chunks"]:
            print(f"chunk {k}: only in {'A' if k in a['chunks'] else 'B'} (the other file was full)")
            continue
        aa, la, bsa, bza, da, ma = a["chunks"][k]
        ab, lb, bsb, bzb, db, mb = b["chunks"][k]
        if la != lb:
            print(f"chunk {k}: different sizes ({la} / {lb})")
            continue
        diffs = []
        for w in range(la // 4):
            va, = struct.unpack_from("<I", da, w * 4)
            vb, = struct.unpack_from("<I", db, w * 4)
            if va != vb and not masked(ma, w) and not masked(mb, w):
                diffs.append((w * 4, va, vb))
        if not diffs:
            continue
        total += len(diffs)
        head = where(k[0], aa, bsa, bza)
        extra = f" (A block at 0x{bsa:08X}, B at 0x{bsb:08X})" if k[0] == 1 else ""
        print(f"\n{head}{extra}: {len(diffs)} values differ")
        for o, va, vb in diffs[:limit]:
            print(f"  {where(k[0], aa + o, bsa, bza):40s} A {va:08X}  B {vb:08X}  ({va - vb if abs(va - vb) < 1 << 20 else '':})")
        if len(diffs) > limit:
            print(f"  ... {len(diffs) - limit} more")
    print(f"\n{total} values differ in all (pointers left out)")


if __name__ == "__main__":
    main()
