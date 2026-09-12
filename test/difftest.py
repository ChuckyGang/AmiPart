#!/usr/bin/env python3
"""Differential test: the m68k binary (under vamos) and the native Linux
binary must produce BYTE-IDENTICAL images for the same operation.

Every engine writes big-endian on-disk structures; a native-endian load or
store anywhere shows up here as a diff even when the per-filesystem checks
in testloop.py cannot see it (that is how the SFS bitmap bug was found).

Usage:  python3 difftest.py [seed]           (needs vamos, rdbtool, both binaries)
"""
import os, sys, random, shutil, subprocess, filecmp
import testloop as T

HERE = T.HERE

def run(binary_host, args):
    if binary_host:
        r = subprocess.run([T.HOSTBIN] + args, capture_output=True, text=True,
                           timeout=300, cwd=HERE)
    else:
        r = subprocess.run(["vamos", "-q", "-O", "locale.library=mode:off", T.DP] + args,
                           capture_output=True, text=True, timeout=300, cwd=HERE)
    return r.returncode, r.stdout + r.stderr

def make_image(path, fs, rng, heads, sectors, disk_cyls, lo, hi):
    blks_cyl = heads * sectors
    dostype = {"ffssyn": "DOS3", "pfs": "PDS3", "sfs": "SFS0"}[fs]
    if os.path.exists(path): os.remove(path)
    T.sh(f"rdbtool {path} create chs={disk_cyls},{heads},{sectors} + init "
         f"+ add name=DH0 start={lo} end={hi} dostype={dostype} >/dev/null")
    img = T.Img(path, lo * blks_cyl)
    part_blocks = (hi - lo + 1) * blks_cyl
    if fs == "ffssyn":  T.build_ffs_syn(img, rng, part_blocks, 1)
    elif fs == "pfs":   T.build_pfs(img, rng, part_blocks, 512)
    else:               T.build_sfs(img, rng, part_blocks, rng.choice([512, 1024]))

def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 20260912
    fails = 0
    for i, fs in enumerate(["sfs", "pfs", "ffssyn", "sfs"]):
        rng = random.Random(seed + i)
        heads, sectors = rng.choice([1, 2, 4]), rng.choice([32, 63])
        disk_cyls = rng.randint(1200, 2500)
        lo = rng.randint(2, 40); hi = lo + rng.randint(300, 600)
        base = os.path.join(HERE, "diff_base.hdf")
        make_image(base, fs, rng, heads, sectors, disk_cyls, lo, hi)
        grow = str(rng.randint(50, 300) * heads * sectors * 512)
        imgs = {}
        for host in (False, True):
            p = os.path.join(HERE, "diff_host.hdf" if host else "diff_m68k.hdf")
            shutil.copyfile(base, p); imgs[host] = p
            for op in (["GROW", "DH0", grow, "FORCE"], ["SHRINK=DH0", "SIZE=MIN", "FORCE"]):
                rc, out = run(host, [f"IMAGE={os.path.basename(p)}"] + op)
                if rc != 0 and "grown" not in out and "shrunk" not in out.lower():
                    print(f"[{fs}] {'host' if host else 'm68k'} {op[0]} rc={rc}: {out.strip()[-160:]}")
                if fs == "ffssyn" and op[0] == "GROW":
                    # FFS leaves bm_flag=0 for the OS validator; stand in for
                    # it (deterministic, so both copies stay comparable)
                    new_hi = hi + int(grow) // (heads * sectors * 512)
                    T.ffs_revalidate(T.Img(p, lo * heads * sectors),
                                     (new_hi - lo + 1) * heads * sectors, 1)
        same = filecmp.cmp(imgs[False], imgs[True], shallow=False)
        tag = f"{fs} geo={heads}x{sectors} part={lo}-{hi} grow={grow}"
        if same:
            print(f"IDENTICAL  {tag}")
            for p in imgs.values(): os.remove(p)
            os.remove(base)
        else:
            fails += 1
            # locate the first differing 512-byte block for the log
            with open(imgs[False], "rb") as a, open(imgs[True], "rb") as b:
                n = 0
                while True:
                    x, y = a.read(512), b.read(512)
                    if not x and not y: break
                    if x != y:
                        print(f"DIFFERENT  {tag}: first differing block {n} (kept diff_*.hdf)")
                        break
                    n += 1
    print("=== difftest:", "PASS" if fails == 0 else f"{fails} FAILED")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
