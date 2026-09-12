#!/usr/bin/env python3
"""Internal (native) formatter test - FFS, PFS3 and SFS.

Every case formats a partition with AmiPart's internal formatter (ADDPART
... VOLNAME= without SAFE) and compares the result BYTE-FOR-BYTE with a
reference volume of identical geometry made by an independent, real
implementation:

  FFS   xdftool (amitools) for every DOS\\0..DOS\\7 variant, plus a partition
        big enough to need a bitmap-extension block.
  PFS3  the real pfs3aio handler (L:pfs3aio 19.2) formatting under AmiFUSE,
        several sizes/geometries, plus a sparse 6 GB supermode partition.
  SFS   the real SmartFilesystem 1.279 handler under AmiFUSE: SFS\\0 at 512
        and 1024-byte blocks, and SFS\\2.

Only timestamps (and the checksums that cover them, and PFS3's AmiPart-set
filename-length field) are masked.  For PFS3/SFS the real handler then also
opens the AmiPart-formatted volume, writes a file into it and reads it
back, proving the handler accepts the volume and can allocate on it.

When vamos and out/AmiPart are present the m68k binary runs the same
scripts and must produce the identical image (the native-endian
regression check difftest.py does for grow/shrink).

Needs: amitools (xdftool, rdbtool).  The PFS3/SFS cases need `amifuse`
on PATH and the handler binaries; they are skipped (not failed) when
those are missing.  Override the handler paths with AMIPART_PFS3_HANDLER
and AMIPART_SFS_HANDLER.

Usage:  python3 fmttest.py
"""
import os, sys, subprocess, glob

HERE    = os.path.dirname(os.path.abspath(__file__))
HOSTBIN = os.path.join(HERE, "..", "host", "amipart")
M68K    = os.path.join(HERE, "..", "out", "AmiPart")

def find_handler(env, names):
    p = os.environ.get(env)
    if p and os.path.exists(p): return p
    for n in names:
        for c in glob.glob(os.path.expanduser(f"~/UAE/*/L/{n}")):
            return c
    return None

PFS3_HANDLER = find_handler("AMIPART_PFS3_HANDLER", ["pfs3aio"])
SFS_HANDLER  = find_handler("AMIPART_SFS_HANDLER", ["SmartFilesystem"])

def sh(cmd, timeout=600):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=HERE, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"cmd failed: {cmd}\n{r.stdout}{r.stderr}")
    return r.stdout + r.stderr

def have(cmd):
    return subprocess.run(f"command -v {cmd}", shell=True, capture_output=True).returncode == 0

def rm(*paths):
    for p in paths:
        fp = os.path.join(HERE, p)
        if os.path.exists(fp): os.remove(fp)

def run_amipart(host, script):
    if host:
        cmd = f"{HOSTBIN} SCRIPT {script} FORCE"
    else:
        cmd = f"vamos -q -O locale.library=mode:off {M68K} SCRIPT {script} FORCE"
    out = sh(cmd)
    if "Formatted DH0" not in out or "Internal " not in out:
        raise RuntimeError(f"format did not run:\n{out}")
    return out

# ---------------------------------------------------------------- masks --
def ffs_mask(blk, data, nlongs, root_blk):
    """byte offsets in this block that may legitimately differ (FFS)"""
    if blk != root_blk: return set()
    m = set(range(5*4, 6*4))                                   # checksum
    for lg in (nlongs-23, nlongs-22, nlongs-21):               # last root change
        m |= set(range(lg*4, lg*4+4))
    m |= set(range((nlongs-10)*4, (nlongs-4)*4))               # disk change + creation
    m |= set(range((nlongs-4)*4, (nlongs-3)*4))                # fstype (amitools sets it on all)
    return m

def pfs_mask(blk, data):
    if blk == 2: return set(range(12, 18))                     # creation date
    if data[:2] == b"EX": return set(range(16, 28)) | {56, 57} # root/volume date, fnsize
    if data[:2] == b"DD": return set(range(26, 32))            # creation date
    return set()

def sfs_mask(blk, data, bsz, v2):
    if data[:3] == b"SFS": return set(range(4, 8)) | set(range(16, 20))
    if data[:4] == b"OBJC":
        x = 2 if v2 else 0
        return set(range(4, 8)) | set(range(44+x, 48+x)) | set(range(bsz-24, bsz-20))
    return set()

def compare(ref, mine, bsz, maskfn, what, blocks=None, extra_blocks=()):
    """compare block by block; blocks = how many leading blocks (None = all)"""
    assert len(ref) == len(mine), f"{what}: size {len(ref)} vs {len(mine)}"
    n = len(ref) // bsz
    todo = list(range(n if blocks is None else min(n, blocks))) + list(extra_blocks)
    bad = []
    for blk in todo:
        x = ref[blk*bsz:(blk+1)*bsz]; y = mine[blk*bsz:(blk+1)*bsz]
        if x == y: continue
        mask = maskfn(blk, x)
        d = [i for i in range(bsz) if x[i] != y[i] and i not in mask]
        if d: bad.append((blk, x[:4], d[:8], x[d[0]:d[0]+8].hex(), y[d[0]:d[0]+8].hex()))
    if bad:
        for e in bad[:10]: print("   ", e)
        raise RuntimeError(f"{what}: {len(bad)} unexpectedly differing blocks")

def read_part(path, off, length):
    with open(os.path.join(HERE, path), "rb") as f:
        f.seek(off); return f.read(length)

# ---------------------------------------------------------------- cases --
# ("fs", dostype, cyls, heads, secs, blocksize, sparse_image_size_or_None)
CASES = [("ffs", f"DOS{d}", 300, 4, 32, 512, None) for d in range(8)] + [
    ("ffs", "DOS3", 1200, 8, 32, 512, None),   # 307200 blocks -> bitmap extension block
    ("ffs", "DOS3", 640, 2, 32, 512, None),
    ("pfs", "PDS3", 300, 4, 32, 512, None),
    ("pfs", "PFS3", 200, 4, 63, 512, None),
    ("pfs", "PDS3", 133, 7, 17, 512, None),
    ("pfs", "PDS3", 2000, 4, 32, 512, None),   # 256000 sectors: 2-block rootblock cluster
    ("pfs", "PDS3", 49599, 8, 32, 512, "6200M"),  # >5 GB: supermode (SB block, 104 MI slots)
    ("sfs", "SFS0", 300, 4, 32, 512, None),
    ("sfs", "SFS0", 300, 4, 32, 1024, None),
    ("sfs", "SFS2", 300, 4, 32, 512, None),
]

def main():
    if not have("xdftool") or not have("rdbtool"):
        print("fmttest: amitools (xdftool/rdbtool) not found"); return 1
    do_m68k = have("vamos") and os.path.exists(M68K)
    amifuse = have("amifuse")
    fails = skips = 0
    for fs, dt, cyls, heads, secs, bsz, sparse in CASES:
        tag = f"{dt} {cyls}x{heads}x{secs} bs={bsz}"
        drv = PFS3_HANDLER if fs == "pfs" else SFS_HANDLER if fs == "sfs" else None
        if fs != "ffs" and (not amifuse or not drv):
            print(f"SKIP {tag} (needs amifuse + handler binary)"); skips += 1; continue
        try:
            rm("fmt_ref.hdf", "fmt_img.hdf", "fmt_m68k.hdf", "fmt_rt.hdf", "fmt.script", "fmt_in.bin", "fmt_out.bin")
            cylb = heads * secs * 512
            spb  = bsz // 512
            # --- empty RDB images (partition = cylinders 1..cyls, RDB in 0) ---
            if sparse:
                for p in ("fmt_ref.hdf", "fmt_img.hdf"):
                    sh(f"truncate -s {sparse} {p} && rdbtool {p} init >/dev/null")
            else:
                sh(f"rdbtool fmt_img.hdf create chs={cyls + 1},{heads},{secs} + init >/dev/null")
                if fs != "ffs":
                    sh(f"rdbtool fmt_ref.hdf create chs={cyls + 1},{heads},{secs} + init >/dev/null")
            # --- reference volume ---
            if fs == "ffs":
                sh(f"xdftool fmt_ref.hdf create chs={cyls},{heads},{secs} + format Test {dt} >/dev/null")
            else:
                if bsz == 512:
                    sh(f"rdbtool fmt_ref.hdf add name=DH0 start=1 end={cyls} dostype={dt} >/dev/null")
                else:
                    with open(os.path.join(HERE, "fmt.script"), "w") as f:
                        f.write(f"OPEN FILE fmt_ref.hdf\nADDPART NAME=DH0 LOW=1 HIGH={cyls} TYPE={dt} BLOCKSIZE={bsz}\nWRITE\nCLOSE\n")
                    sh(f"{HOSTBIN} SCRIPT fmt.script FORCE >/dev/null")
                sh(f"amifuse format fmt_ref.hdf DH0 Test --driver {drv} 2>&1 | grep -q 'Format complete'")
            # --- AmiPart internal formatter ---
            bs_kw = f" BLOCKSIZE={bsz}" if bsz != 512 else ""
            script_text = ("OPEN FILE fmt_img.hdf\n"
                           f"ADDPART NAME=DH0 LOW=1 HIGH={cyls} TYPE={dt} VOLNAME=Test{bs_kw}\n"
                           "WRITE\nCLOSE\n")
            with open(os.path.join(HERE, "fmt.script"), "w") as f:
                f.write(script_text)
            if do_m68k and not sparse:
                sh("cp fmt_img.hdf fmt_m68k.hdf")
            run_amipart(True, "fmt.script")
            # --- compare ---
            total  = cyls * heads * secs // spb
            # PFS3 writes only its reserved area (+ boot); SFS only its admin
            # area, bitmaps and the two root blocks - compare those regions
            # in full, and everything on the small images.
            limit  = None if not sparse else 300000
            extra  = [total - 1] if fs == "sfs" else []
            plen   = (limit if limit else total) * bsz
            ref    = read_part("fmt_ref.hdf", 0 if fs == "ffs" else cylb, plen)
            mine   = read_part("fmt_img.hdf", cylb, plen)
            if fs == "sfs":
                ref  = read_part("fmt_ref.hdf", cylb, total * bsz); mine = read_part("fmt_img.hdf", cylb, total * bsz)
            if fs == "ffs":
                nl = bsz // 4; root = total // 2
                compare(ref, mine, bsz, lambda b, d: ffs_mask(b, d, nl, root), "host vs xdftool")
            elif fs == "pfs":
                compare(ref, mine, bsz, pfs_mask, "host vs pfs3aio", blocks=limit)
            else:
                compare(ref, mine, bsz, lambda b, d: sfs_mask(b, d, bsz, dt == "SFS2"),
                        "host vs SmartFilesystem", extra_blocks=extra)
            # --- independent reader / real handler round-trip on OUR volume ---
            if fs == "ffs":
                lst = sh("xdftool fmt_img.hdf open part=DH0 + info + list 2>&1")
                if "Test" not in lst: raise RuntimeError(f"xdftool can't read it:\n{lst}")
            else:
                # AmiFUSE's emulated scsi.device loses writes on PDS\x (DirectSCSI)
                # partitions - the real handler's own PDS3 format shows the same -
                # so the round-trip runs on a PFS3-typed copy of our volume (the
                # on-disk format does not depend on the RDB dostype).
                rt_dt = "PFS3" if fs == "pfs" else dt
                rm("fmt_rt.hdf")
                if sparse:
                    sh(f"truncate -s {sparse} fmt_rt.hdf && rdbtool fmt_rt.hdf init >/dev/null")
                else:
                    sh(f"rdbtool fmt_rt.hdf create chs={cyls + 1},{heads},{secs} + init >/dev/null")
                with open(os.path.join(HERE, "fmt.script"), "w") as f:
                    f.write("OPEN FILE fmt_rt.hdf\n"
                            f"ADDPART NAME=DH0 LOW=1 HIGH={cyls} TYPE={rt_dt} VOLNAME=Test{bs_kw}\n"
                            "WRITE\nCLOSE\n")
                run_amipart(True, "fmt.script")
                sh("dd if=/dev/urandom of=fmt_in.bin bs=1024 count=600 status=none")
                sh(f"amifuse write fmt_rt.hdf --file X --in fmt_in.bin --driver {drv} >/dev/null 2>&1")
                sh(f"amifuse read fmt_rt.hdf --file X --out fmt_out.bin --driver {drv} >/dev/null 2>&1")
                sh("cmp fmt_in.bin fmt_out.bin")
                lst = sh(f"amifuse ls fmt_rt.hdf --driver {drv} 2>&1")
                if "X" not in lst: raise RuntimeError(f"handler listing:\n{lst}")
                rm("fmt_rt.hdf")
            # --- m68k binary must produce the identical image ---
            if do_m68k and not sparse:
                open(os.path.join(HERE, "fmt.script"), "w").write(
                    script_text.replace("fmt_img.hdf", "fmt_m68k.hdf"))
                run_amipart(False, "fmt.script")
                a = read_part("fmt_img.hdf", cylb, total * bsz); b = read_part("fmt_m68k.hdf", cylb, total * bsz)
                if fs == "ffs":
                    compare(a, b, bsz, lambda b_, d: ffs_mask(b_, d, bsz // 4, total // 2), "host vs m68k")
                elif fs == "pfs":
                    compare(a, b, bsz, pfs_mask, "host vs m68k")
                else:
                    compare(a, b, bsz, lambda b_, d: sfs_mask(b_, d, bsz, dt == "SFS2"), "host vs m68k")
            print(f"PASS {tag}" + ("  (+m68k identical)" if do_m68k and not sparse else ""))
        except Exception as e:
            fails += 1
            print(f"FAIL {tag}: {e}")
    rm("fmt_ref.hdf", "fmt_img.hdf", "fmt_m68k.hdf", "fmt_rt.hdf", "fmt.script", "fmt_in.bin", "fmt_out.bin")
    print(f"fmttest: {len(CASES) - fails - skips}/{len(CASES)} passed, {skips} skipped")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
