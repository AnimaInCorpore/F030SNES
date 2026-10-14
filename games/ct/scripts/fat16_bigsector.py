#!/usr/bin/env python3
"""Minimal FAT16 reader/writer for GEMDOS volumes with large logical sectors
(e.g. 16384-byte sectors, which mtools and macOS refuse).

Usage:
  fat16.py IMAGE ls [PATH]          list recursively (PATH like /SCUMMVM)
  fat16.py IMAGE check              verify FAT chains, cross links, sizes
  fat16.py IMAGE put SRC DSTPATH    copy a host file to an 8.3 path (dirs made)
  fat16.py IMAGE get SRCPATH DST    copy a file out
IMAGE may be IMAGE@@OFFSET.
"""
import os
import struct
import sys
import time


class Vol:
    def __init__(self, spec, write=False):
        path, _, off = spec.partition("@@")
        self.off = int(off) if off else 0
        self.f = open(path, "r+b" if write else "rb")
        self.f.seek(self.off)
        bs = self.f.read(64)
        (self.bps, self.spc, self.res, self.nfats, self.nroot, ts16, _media,
         self.fatsz, _spt, _heads, _hidden, ts32) = struct.unpack_from("<HBHBHHBHHHII", bs, 11)
        self.total = ts16 or ts32
        self.csize = self.bps * self.spc
        self.fat_start = self.res * self.bps
        self.root_start = self.fat_start + self.nfats * self.fatsz * self.bps
        self.root_bytes = self.nroot * 32
        self.data_start = self.root_start + -(-self.root_bytes // self.bps) * self.bps
        self.nclusters = (self.total * self.bps - self.data_start) // self.csize
        if self.nclusters < 4085 or self.nclusters >= 65525:
            raise SystemExit(f"not FAT16 ({self.nclusters} clusters)")
        self.f.seek(self.off + self.fat_start)
        raw = self.f.read(self.fatsz * self.bps)
        self.fat = list(struct.unpack(f"<{len(raw)//2}H", raw))
        self.fat_dirty = False

    def rd(self, pos, n):
        self.f.seek(self.off + pos)
        return self.f.read(n)

    def wr(self, pos, data):
        self.f.seek(self.off + pos)
        self.f.write(data)

    def cpos(self, c):
        return self.data_start + (c - 2) * self.csize

    def chain(self, c):
        out, seen = [], set()
        while 2 <= c < 0xFFF8:
            if c in seen or c >= self.nclusters + 2:
                raise SystemExit(f"bad chain at {c}")
            seen.add(c)
            out.append(c)
            c = self.fat[c]
        return out

    # directories: (read entries, location list for writing back)
    def dir_slots(self, first):
        if first == 0:
            return [(self.root_start, self.root_bytes)]
        return [(self.cpos(c), self.csize) for c in self.chain(first)]

    def entries(self, first):
        for base, size in self.dir_slots(first):
            data = self.rd(base, size)
            for i in range(0, size, 32):
                e = data[i:i + 32]
                if e[0] == 0:
                    return
                yield base + i, e

    def listdir(self, first):
        out = []
        for pos, e in self.entries(first):
            if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
                continue
            name = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            full = name + ("." + ext if ext else "")
            if full in (".", ".."):
                continue
            hi, lo, size = struct.unpack_from("<H", e, 20)[0], struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]
            out.append((full, e[11], lo | (hi << 16), size, pos))
        return out

    def lookup(self, path):
        cur = 0
        parts = [p for p in path.strip("/").split("/") if p]
        ent = None
        for p in parts:
            hit = [x for x in self.listdir(cur) if x[0] == p.upper()]
            if not hit:
                return None
            ent = hit[0]
            cur = ent[2]
        return ent if parts else ("", 0x10, 0, 0, None)

    def free_clusters(self):
        return [c for c in range(2, self.nclusters + 2) if self.fat[c] == 0]

    def alloc(self, n):
        free = self.free_clusters()
        if len(free) < n:
            raise SystemExit(f"disk full: need {n} clusters, {len(free)} free")
        got = free[:n]
        for a, b in zip(got, got[1:] + [0xFFFF]):
            self.fat[a] = b
        self.fat_dirty = True
        return got

    def flush_fat(self):
        if not self.fat_dirty:
            return
        raw = struct.pack(f"<{len(self.fat)}H", *self.fat)
        for i in range(self.nfats):
            self.wr(self.fat_start + i * self.fatsz * self.bps, raw)
        self.fat_dirty = False

    @staticmethod
    def name83(name):
        base, _, ext = name.upper().partition(".")
        if not (1 <= len(base) <= 8 and len(ext) <= 3) or "." in ext:
            raise SystemExit(f"not an 8.3 name: {name}")
        return base.ljust(8).encode() + ext.ljust(3).encode()

    @staticmethod
    def stamp(t):
        lt = time.localtime(t)
        d = ((lt.tm_year - 1980) << 9) | (lt.tm_mon << 5) | lt.tm_mday
        tm = (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2)
        return tm, d

    def make_entry(self, name, attr, first, size, mtime):
        tm, d = self.stamp(mtime)
        return (self.name83(name) + bytes([attr, 0, 0]) + struct.pack("<HHHHHHHI", tm, d, d, first >> 16, tm, d, first & 0xFFFF, size))

    def free_slot(self, first):
        for base, size in self.dir_slots(first):
            data = self.rd(base, size)
            for i in range(0, size, 32):
                if data[i] in (0, 0xE5):
                    return base + i
        if first == 0:
            raise SystemExit("root directory full")
        c = self.alloc(1)[0]
        last = self.chain(first)[-1]
        self.fat[last] = c
        self.wr(self.cpos(c), bytes(self.csize))
        return self.cpos(c)

    def mkdir(self, parent, name, mtime):
        c = self.alloc(1)[0]
        blk = bytearray(self.csize)
        blk[0:32] = self.make_entry("X", 0x10, c, 0, mtime)
        blk[0:11] = b".          "
        blk[32:64] = self.make_entry("X", 0x10, parent, 0, mtime)
        blk[32:43] = b"..         "
        self.wr(self.cpos(c), bytes(blk))
        self.wr(self.free_slot(parent), self.make_entry(name, 0x10, c, 0, mtime))
        return c

    def ensure_dir(self, path):
        cur = 0
        for p in [p for p in path.strip("/").split("/") if p]:
            hit = [x for x in self.listdir(cur) if x[0] == p.upper()]
            if hit:
                if not hit[0][1] & 0x10:
                    raise SystemExit(f"{p} is a file")
                cur = hit[0][2]
            else:
                cur = self.mkdir(cur, p, time.time())
        return cur

    def put(self, src, dst):
        d, _, name = dst.strip("/").rpartition("/")
        self.name83(name)
        parent = self.ensure_dir(d)
        if any(x[0] == name.upper() for x in self.listdir(parent)):
            raise SystemExit(f"{dst} exists")
        size = os.path.getsize(src)
        n = -(-size // self.csize)
        clusters = self.alloc(n) if n else []
        with open(src, "rb") as s:
            # write runs of consecutive clusters in one go
            i = 0
            while i < n:
                j = i
                while j + 1 < n and clusters[j + 1] == clusters[j] + 1:
                    j += 1
                want = (j - i + 1) * self.csize
                buf = s.read(want)
                self.wr(self.cpos(clusters[i]), buf + bytes(want - len(buf)))
                i = j + 1
        self.wr(self.free_slot(parent), self.make_entry(name, 0x20, clusters[0] if n else 0, size, os.path.getmtime(src)))
        self.flush_fat()

    def get(self, path, dst):
        ent = self.lookup(path)
        if not ent or ent[1] & 0x10:
            raise SystemExit(f"no file {path}")
        left = ent[3]
        with open(dst, "wb") as o:
            for c in self.chain(ent[2]):
                k = min(left, self.csize)
                o.write(self.rd(self.cpos(c), k))
                left -= k
        if left:
            raise SystemExit(f"{path}: chain shorter than size")

    def walk(self, first=0, prefix=""):
        for name, attr, c, size, _ in self.listdir(first):
            yield prefix + "/" + name, attr, c, size
            if attr & 0x10:
                yield from self.walk(c, prefix + "/" + name)

    def check(self):
        owner = {}
        problems = 0
        def claim(c, who):
            nonlocal problems
            if c in owner:
                print(f"cross link: cluster {c} in {owner[c]} and {who}")
                problems += 1
            owner[c] = who
        for path, attr, c, size in self.walk():
            ch = self.chain(c) if c else []
            for x in ch:
                claim(x, path)
            if not attr & 0x10 and len(ch) != -(-size // self.csize):
                print(f"size mismatch: {path} {size} bytes, {len(ch)} clusters")
                problems += 1
        used = sum(1 for c in range(2, self.nclusters + 2) if self.fat[c] != 0)
        if used != len(owner):
            print(f"lost clusters: {used - len(owner)}")
            problems += 1
        f2 = self.rd(self.fat_start + self.fatsz * self.bps, self.fatsz * self.bps) if self.nfats > 1 else None
        if f2 is not None and f2 != self.rd(self.fat_start, self.fatsz * self.bps):
            print("FAT copies differ")
            problems += 1
        print(f"{problems} problems; {len(owner)} clusters used, {self.nclusters - used} free "
              f"({(self.nclusters - used) * self.csize / 1048576:.1f} MiB)")
        return problems


def main():
    img, cmd, *rest = sys.argv[1:]
    v = Vol(img, write=cmd == "put")
    if cmd == "ls":
        print(f"bps={v.bps} spc={v.spc} clusters={v.nclusters} sectors={v.total} root={v.nroot}")
        start = v.lookup(rest[0])[2] if rest else 0
        for path, attr, c, size in v.walk(start, rest[0].rstrip("/") if rest else ""):
            print(f"{'<DIR>' if attr & 0x10 else size:>12}  {path}")
    elif cmd == "check":
        sys.exit(1 if v.check() else 0)
    elif cmd == "put":
        v.put(rest[0], rest[1])
    elif cmd == "get":
        v.get(rest[0], rest[1])


if __name__ == "__main__":
    main()
