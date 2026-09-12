#!/usr/bin/env python3
"""Internal (native) FFS formatter test.

For every DOS\\0..DOS\\7 variant (and a partition big enough to need a
bitmap-extension block) the host binary formats a partition with the
internal formatter (ADDPART ... VOLNAME= without SAFE) and the result is
compared BYTE-FOR-BYTE with an xdftool-formatted volume of identical
geometry.  Only the root block's timestamps, its checksum and the DOS\\6/7
fstype longword (which amitools writes on every variant) are masked.

Then the freshly formatted partition is opened and listed by xdftool (an
independent implementation) to prove the volume is readable.  If vamos and
the m68k binary are available, the same script is also run with the m68k
binary and both images must be identical apart from the masked longwords
(the native-endian regression check difftest.py does for grow/shrink).

Usage:  python3 fmttest.py           (needs amitools: xdftool + rdbtool)
"""
import os, sys, subprocess

HERE    = os.path.dirname(os.path.abspath(__file__))
HOSTBIN = os.path.join(HERE, "..", "host", "amipart")
M68K    = os.path.join(HERE, "..", "out", "AmiPart")

def sh(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=HERE)
    if r.returncode != 0:
        raise RuntimeError(f"cmd failed: {cmd}\n{r.stdout}{r.stderr}")
    return r.stdout

def have(cmd):
    return subprocess.run(f"command -v {cmd}", shell=True,
                          capture_output=True).returncode == 0

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
    if "Formatted DH0" not in out or "Internal DOS" not in out:
        raise RuntimeError(f"format did not run:\n{out}")
    return out

def masked_longs(nlongs):
    """Root-block longword indexes that legitimately differ between two formats."""
    m = {5}                                   # checksum
    m |= {nlongs - 23, nlongs - 22, nlongs - 21}      # last root change
    m |= set(range(nlongs - 10, nlongs - 4))          # disk change + creation
    m |= {nlongs - 4}                                 # fstype (DOS6/7 only)
    return m

def compare(a, b, root_blk, bsz, what):
    assert len(a) == len(b), f"{what}: size {len(a)} vs {len(b)}"
    nlongs = bsz // 4
    mask = masked_longs(nlongs)
    bad = []
    for off in range(0, len(a), 4):
        if a[off:off+4] == b[off:off+4]: continue
        blk, lg = off // bsz, (off % bsz) // 4
        if blk == root_blk and lg in mask: continue
        bad.append((blk, lg, a[off:off+4].hex(), b[off:off+4].hex()))
    if bad:
        for blk, lg, x, y in bad[:20]:
            print(f"    blk {blk} long {lg}: {x} vs {y}")
        raise RuntimeError(f"{what}: {len(bad)} unexpected differing longwords")

def checksum_ok(blk):
    s = 0
    for i in range(0, len(blk), 4):
        s = (s + int.from_bytes(blk[i:i+4], "big")) & 0xFFFFFFFF
    return s == 0

# (dosn, cyls, heads, sectors)  - all even block counts so amitools' root
# position (blocks//2) is the FFS one.  1200x8x32 = 307200 blocks = 76 bitmap
# blocks -> needs a bitmap-extension block (root holds 25 pointers).
CASES = [(0, 300, 4, 32), (1, 300, 4, 32), (2, 300, 4, 32), (3, 300, 4, 32),
         (4, 300, 4, 32), (5, 300, 4, 32), (6, 300, 4, 32), (7, 300, 4, 32),
         (3, 1200, 8, 32), (3, 640, 2, 32)]

def main():
    if not have("xdftool") or not have("rdbtool"):
        print("fmttest: amitools (xdftool/rdbtool) not found"); return 1
    do_m68k = have("vamos") and os.path.exists(M68K)
    fails = 0
    for dosn, cyls, heads, secs in CASES:
        tag = f"DOS{dosn} {cyls}x{heads}x{secs}"
        try:
            rm("fmt_ref.hdf", "fmt_img.hdf", "fmt_m68k.hdf", "fmt.script")
            cylb = heads * secs * 512
            sh(f"xdftool fmt_ref.hdf create chs={cyls},{heads},{secs} + format Test DOS{dosn} >/dev/null")
            sh(f"rdbtool fmt_img.hdf create chs={cyls + 1},{heads},{secs} + init >/dev/null")
            with open(os.path.join(HERE, "fmt.script"), "w") as f:
                f.write("OPEN FILE fmt_img.hdf\n"
                        f"ADDPART NAME=DH0 LOW=1 HIGH={cyls} TYPE=DOS{dosn} VOLNAME=Test\n"
                        "WRITE\nCLOSE\n")
            if do_m68k:
                sh("cp fmt_img.hdf fmt_m68k.hdf")
            run_amipart(True, "fmt.script")
            ref = open(os.path.join(HERE, "fmt_ref.hdf"), "rb").read()
            img = open(os.path.join(HERE, "fmt_img.hdf"), "rb").read()
            part = img[cylb:cylb + cyls * cylb]
            root = (cyls * heads * secs) // 2
            compare(ref, part, root, 512, "host vs xdftool")
            rb = part[root * 512:(root + 1) * 512]
            if not checksum_ok(rb): raise RuntimeError("root checksum invalid")
            if int.from_bytes(rb[-4:], "big") != 1: raise RuntimeError("root sec_type")
            # independent reader must see an empty, valid volume
            lst = sh("xdftool fmt_img.hdf open part=DH0 + info + list 2>&1")
            if "Test" not in lst: raise RuntimeError(f"xdftool can't read it:\n{lst}")
            if do_m68k:
                # same script, m68k binary under vamos
                s = open(os.path.join(HERE, "fmt.script")).read().replace("fmt_img.hdf", "fmt_m68k.hdf")
                open(os.path.join(HERE, "fmt.script"), "w").write(s)
                run_amipart(False, "fmt.script")
                m = open(os.path.join(HERE, "fmt_m68k.hdf"), "rb").read()
                compare(img, m, cyls * heads * secs // 2 + heads * secs, 512,
                        "host vs m68k")   # root index is disk-absolute here
            print(f"PASS {tag}" + ("  (+m68k identical)" if do_m68k else ""))
        except Exception as e:
            fails += 1
            print(f"FAIL {tag}: {e}")
    rm("fmt_ref.hdf", "fmt_img.hdf", "fmt_m68k.hdf", "fmt.script")
    print(f"fmttest: {len(CASES) - fails}/{len(CASES)} passed")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
