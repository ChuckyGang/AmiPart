#!/usr/bin/env python3
"""RESCUE test - destroy the partition table, scan, compare.

Every case builds a partitioned image with the host binary, records the
real layout from INFO, damages the image, runs RESCUE and requires the
proposed partition table to name exactly the original block ranges.

  wiped      RDB area zeroed: empty FFS (512 + 1024-byte blocks), PFS3, SFS
  rdsk       only the RDSK block zeroed: PART blocks must carry the names
  files      FFS with files and two ADF images stored as files (must be
             ignored, normal and DEEP scan)
  noboot     ...and the FFS boot block wiped as well
  grown      FFS grown with GROW (relocated root block), then wiped
  stale      disk repartitioned smaller; the old volume's root block and
             files still lie in the new partitions (DEEP must not fall
             for it)
  m68k       the m68k binary under vamos prints the identical report

Every damaged image is then repaired with RESCUE WRITE and must come back
with the original layout, a clean CHECK, and not one changed byte in the
partition area.  Further write cases:

  intact     nothing to add, image stays byte-identical
  deleted    a partition deleted from a valid RDB is put back
  chain      a PART block with a broken checksum cuts the chain: the
             partitions behind it come back with their names
  driver     RDSK zeroed on a disk with a filesystem driver in the RDB: the
             rebuilt RDB is byte-identical to the original

Needs: host/amipart, amitools (xdftool).  The m68k case is skipped when
vamos or out/AmiPart is missing.

Usage:  python3 rescuetest.py
"""
import os, re, subprocess, sys, time

HERE    = os.path.dirname(os.path.abspath(__file__))
HOSTBIN = os.path.join(HERE, "..", "host", "amipart")
M68K    = os.path.join(HERE, "..", "out", "AmiPart")
TMP     = [f"rs_{n}.hdf" for n in ("a", "b", "w", "m", "vol", "old")] + ["rs_game.adf"]

def sh(cmd, check=True):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=HERE)
    if check and r.returncode != 0:
        raise RuntimeError(f"cmd failed: {cmd}\n{r.stdout}{r.stderr}")
    return r.stdout + r.stderr

def find_handler():
    import glob
    p = os.environ.get("AMIPART_PFS3_HANDLER")
    if p and os.path.exists(p): return p
    for c in glob.glob(os.path.expanduser("~/UAE/*/L/pfs3aio")):
        return c
    return None

def have(cmd):
    return subprocess.run(f"command -v {cmd}", shell=True, capture_output=True).returncode == 0

def amipart(img, args):
    return sh(f"{HOSTBIN} IMAGE={img} {args}")

def zero(img, first, count):
    with open(os.path.join(HERE, img), "r+b") as f:
        f.seek(first * 512)
        f.write(bytes(count * 512))

def put(img, src, block):
    with open(os.path.join(HERE, src), "rb") as s, \
         open(os.path.join(HERE, img), "r+b") as f:
        f.seek(block * 512)
        f.write(s.read())

def layout(img):
    """[(name, first_block, last_block, dostype)] from INFO."""
    out = amipart(img, "INFO")
    g = re.search(r"(\d+) cyls x (\d+) heads x (\d+) sectors", out)
    cyl = int(g.group(2)) * int(g.group(3))
    res = []
    for m in re.finditer(r"^\s+\d+: (\S+)\s+cyls\s+(\d+)-\s*(\d+)\s+(\S+)", out, re.M):
        res.append((m.group(1), int(m.group(2)) * cyl,
                    (int(m.group(3)) + 1) * cyl - 1, m.group(4)))
    return res

def proposal(out):
    res = []
    for m in re.finditer(r"^\s+(\S+)\s+cylinders \d+-\d+, blocks (\d+)-(\d+), ([^,]+),",
                         out, re.M):
        res.append((m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)))
    return res

def new_image(img, mb, parts):
    sh(f"rm -f {img}")
    amipart(img, f"CREATE SIZE={mb}M")
    amipart(img, "INIT NEW FORCE")
    for p in parts:
        amipart(img, f"ADDPART {p} FORCE")

def make_volume(vol, cyls, name):
    """A real FFS volume with a directory, a file and two ADF images."""
    sh(f"rm -f {vol} rs_game.adf")
    sh("xdftool rs_game.adf format Game ofs")
    sh(f"xdftool rs_game.adf write {HOSTBIN} tool")
    sh(f"xdftool {vol} create chs={cyls},16,63 + format {name} DOS3")
    sh(f"xdftool {vol} makedir C + write rs_game.adf + write {HOSTBIN} C/tool "
       f"+ write rs_game.adf C/copy.adf")

def read_from(img, block):
    with open(os.path.join(HERE, img), "rb") as f:
        f.seek(block * 512)
        return f.read()

def bad(msg):
    global fails
    fails += 1
    print(f"FAIL  {msg}")

def repair(tag, img, want, names, deep=False):
    """RESCUE WRITE on a copy of img: layout back, data untouched."""
    sh(f"cp {img} rs_w.hdf")
    first = min(a for (_, a, _, _) in want)
    before = read_from("rs_w.hdf", first)
    out = amipart("rs_w.hdf", "RESCUE WRITE FORCE" + (" DEEP" if deep else ""))
    got = layout("rs_w.hdf")
    w = [(a, b) for (_, a, b, _) in want]
    g = [(a, b) for (_, a, b, _) in got]
    ok = (w == g)
    if names:
        ok = ok and [n for (n, _, _, _) in want] == [n for (n, _, _, _) in got]
    ok = ok and all(gt == wt or wt.startswith("PFS") or wt.startswith("PDS")
                    for (_, _, _, gt), (_, _, _, wt) in zip(got, want))
    if not ok:
        return bad(f"{tag}: layout after WRITE\n  want {want}\n  got  {got}\n{out}")
    if read_from("rs_w.hdf", first) != before:
        return bad(f"{tag}: WRITE changed bytes inside the partition area")
    chk = amipart("rs_w.hdf", "CHECK")
    if "PASS" not in chk:
        return bad(f"{tag}: CHECK after WRITE\n{chk}")
    print(f"PASS  {tag} -> WRITE")

fails = 0
def check(tag, want, out, names):
    global fails
    got = proposal(out)
    w = [(n if names else "-", a, b) for (n, a, b, _) in want]
    g = [(n, a, b) for (n, a, b, _) in got]
    # PFS partition types are not stored on disk: PFS\3 is assumed.
    types_ok = all(gt == wt for (_, _, _, gt), (_, _, _, wt) in zip(got, want)
                   if gt != "?")
    if w == g and types_ok:
        print(f"PASS  {tag}")
    else:
        fails += 1
        print(f"FAIL  {tag}\n  want {want}\n  got  {got}\n{out}")

def main():
    if not os.path.exists(HOSTBIN):
        sys.exit("host/amipart missing - run `make linux` first")
    if not have("xdftool"):
        sys.exit("xdftool (amitools) missing")

    four = ["NAME=DH0 LOW=2 HIGH=200 TYPE=DOS3 VOLNAME=System BOOTABLE",
            "NAME=DH1 LOW=201 HIGH=600 TYPE=PFS3 VOLNAME=Work",
            "NAME=DH2 LOW=601 HIGH=900 TYPE=SFS0 VOLNAME=Smart",
            "NAME=DH3 LOW=901 HIGH=1218 TYPE=DOS7 BLOCKSIZE=1024 VOLNAME=Big"]

    # --- wiped / rdsk -------------------------------------------------
    new_image("rs_a.hdf", 600, four)
    want = layout("rs_a.hdf")
    assert len(want) == 4, want
    intact_report = amipart("rs_a.hdf", "RESCUE")
    check("intact disk", want, intact_report, True)

    sh("cp rs_a.hdf rs_b.hdf"); zero("rs_b.hdf", 0, 1)
    check("rdsk: RDSK block zeroed", want, amipart("rs_b.hdf", "RESCUE"), True)
    repair("rdsk", "rs_b.hdf", want, True)

    # intact: nothing to add, nothing written
    sh("cp rs_a.hdf rs_w.hdf")
    out = amipart("rs_w.hdf", "RESCUE WRITE FORCE")
    if "Nothing to add" in out and read_from("rs_w.hdf", 0) == read_from("rs_a.hdf", 0):
        print("PASS  intact -> WRITE leaves the disk alone")
    else:
        bad(f"intact: WRITE touched an intact disk\n{out}")

    # deleted: one partition removed from a valid RDB
    sh("cp rs_a.hdf rs_b.hdf")
    amipart("rs_b.hdf", "DELPART NAME=DH2 FORCE")
    assert len(layout("rs_b.hdf")) == 3
    repair("deleted: partition removed from a valid RDB", "rs_b.hdf", want, False)

    # chain: PART block of DH1 (block 2) gets a broken checksum
    sh("cp rs_a.hdf rs_b.hdf")
    with open(os.path.join(HERE, "rs_b.hdf"), "r+b") as f:
        f.seek(2 * 512 + 8); c = f.read(4)
        f.seek(2 * 512 + 8); f.write(bytes([c[0] ^ 0x55]) + c[1:])
    assert len(layout("rs_b.hdf")) == 1
    sh("cp rs_b.hdf rs_w.hdf")
    amipart("rs_w.hdf", "RESCUE WRITE FORCE")
    got = layout("rs_w.hdf")
    if [(a, b) for (_, a, b, _) in got] == [(a, b) for (_, a, b, _) in want] and \
       [n for (n, _, _, _) in got if n in ("DH0", "DH2", "DH3")] == ["DH0", "DH2", "DH3"]:
        print("PASS  chain: partitions behind a broken PART block -> WRITE")
    else:
        bad(f"chain: want {want}\n  got {got}")

    sh("cp rs_a.hdf rs_b.hdf"); zero("rs_b.hdf", 0, 2016)
    check("wiped: RDB area zeroed", want, amipart("rs_b.hdf", "RESCUE"), False)
    check("wiped: DEEP", want, amipart("rs_b.hdf", "RESCUE DEEP"), False)
    repair("wiped", "rs_b.hdf", want, False)

    # --- m68k parity --------------------------------------------------
    if have("vamos") and os.path.exists(M68K):
        r = subprocess.run(["vamos", "-q", "-O", "locale.library=mode:off", M68K,
                            "IMAGE=rs_b.hdf", "RESCUE"],
                           capture_output=True, text=True, cwd=HERE)
        host = amipart("rs_b.hdf", "RESCUE")
        cut = lambda t: t[t.index("=== RDB rescue scan"):] if "=== RDB rescue scan" in t else t
        if cut(r.stdout) == cut(host):
            print("PASS  m68k: identical report under vamos")
        else:
            globals()["fails"] += 1
            print(f"FAIL  m68k report differs\n--- m68k\n{r.stdout}{r.stderr}\n--- host\n{host}")
        sh("cp rs_b.hdf rs_m.hdf"); sh("cp rs_b.hdf rs_w.hdf")
        subprocess.run(["vamos", "-q", "-O", "locale.library=mode:off", M68K,
                        "IMAGE=rs_m.hdf", "RESCUE", "WRITE", "FORCE"],
                       capture_output=True, text=True, cwd=HERE)
        amipart("rs_w.hdf", "RESCUE WRITE FORCE")
        if read_from("rs_m.hdf", 0) == read_from("rs_w.hdf", 0) and len(layout("rs_m.hdf")) == 4:
            print("PASS  m68k: WRITE produces the identical image")
        else:
            bad("m68k: WRITE image differs from the host build's")
    else:
        print("SKIP  m68k (vamos or out/AmiPart missing)")

    # --- files / noboot -----------------------------------------------
    make_volume("rs_vol.hdf", 199, "System")
    put("rs_a.hdf", "rs_vol.hdf", want[0][1])
    sh("cp rs_a.hdf rs_b.hdf"); zero("rs_b.hdf", 0, 2016)
    out = amipart("rs_b.hdf", "RESCUE")
    check("files: FFS with files + ADF images", want, out, False)
    out = amipart("rs_b.hdf", "RESCUE DEEP")
    check("files: DEEP", want, out, False)
    if "signatures inside these partitions ignored" not in out:
        globals()["fails"] += 1
        print("FAIL  files: DEEP did not report the nested ADF images")
    repair("files", "rs_b.hdf", want, False, deep=True)
    zero("rs_b.hdf", want[0][1], 2)
    check("noboot: FFS boot block wiped too", want, amipart("rs_b.hdf", "RESCUE"), False)
    repair("noboot", "rs_b.hdf", want, False)

    # --- grown --------------------------------------------------------
    new_image("rs_a.hdf", 400, ["NAME=DH0 LOW=2 HIGH=200 TYPE=DOS3"])
    put("rs_a.hdf", "rs_vol.hdf", 2016)
    amipart("rs_a.hdf", "ADDPART NAME=DH1 LOW=500 HIGH=811 TYPE=PFS3 VOLNAME=Tail FORCE")
    amipart("rs_a.hdf", "GROW DH0 100M FORCE")
    want = layout("rs_a.hdf")
    assert want[0][2] > 202607, want
    zero("rs_a.hdf", 0, 2016)
    check("grown: FFS after GROW (relocated root)", want, amipart("rs_a.hdf", "RESCUE"), False)
    check("grown: DEEP", want, amipart("rs_a.hdf", "RESCUE DEEP"), False)
    repair("grown", "rs_a.hdf", want, False)

    # --- stale --------------------------------------------------------
    make_volume("rs_old.hdf", 599, "Old")
    time.sleep(2)                       # the new volume must be newer
    make_volume("rs_vol.hdf", 199, "System")
    new_image("rs_a.hdf", 420, ["NAME=DH0 LOW=2 HIGH=600 TYPE=DOS3"])
    put("rs_a.hdf", "rs_old.hdf", 2016)
    amipart("rs_a.hdf", "INIT NEW FORCE")
    for p in ["NAME=DH0 LOW=2 HIGH=200 TYPE=DOS3",
              "NAME=DH1 LOW=201 HIGH=400 TYPE=PFS3 VOLNAME=Work",
              "NAME=DH2 LOW=401 HIGH=820 TYPE=SFS0 VOLNAME=Smart"]:
        amipart("rs_a.hdf", f"ADDPART {p} FORCE")
    put("rs_a.hdf", "rs_vol.hdf", 2016)
    want = layout("rs_a.hdf")
    assert len(want) == 3, want
    zero("rs_a.hdf", 0, 2016)
    check("stale: repartitioned disk", want, amipart("rs_a.hdf", "RESCUE"), False)
    out = amipart("rs_a.hdf", "RESCUE DEEP")
    check("stale: DEEP", want, out, False)
    if '"Old"' not in out:
        globals()["fails"] += 1
        print("FAIL  stale: DEEP did not even see the old volume (test is not testing)")
    repair("stale", "rs_a.hdf", want, False, deep=True)

    # --- driver: a filesystem driver in the RDB survives a lost RDSK ----
    handler = find_handler()
    if handler:
        new_image("rs_a.hdf", 300, ["NAME=DH0 LOW=2 HIGH=200 TYPE=DOS3 VOLNAME=Sys",
                                    "NAME=DH1 LOW=201 HIGH=500 TYPE=PFS3 VOLNAME=Work"])
        amipart("rs_a.hdf", f"ADDFS TYPE=PFS3 FILE={handler} FORCE")
        if "Filesystems: 1" not in amipart("rs_a.hdf", "INFO"):
            bad("driver: ADDFS did not add the test driver")
        sh("cp rs_a.hdf rs_w.hdf"); zero("rs_w.hdf", 0, 1)
        out = amipart("rs_w.hdf", "RESCUE WRITE FORCE")
        a, b = read_from("rs_a.hdf", 0), read_from("rs_w.hdf", 0)
        diff = sorted({i // 512 for i in range(0, 2016 * 512) if a[i] != b[i]})
        if diff in ([], [0]) and a[2016 * 512:] == b[2016 * 512:] and \
           "Filesystems: 1" in amipart("rs_w.hdf", "INFO"):
            print(f"PASS  driver: RDB rebuilt with its filesystem driver (blocks differing: {diff})")
        else:
            bad(f"driver: blocks differing {diff[:20]}\n{out}")
    else:
        print("SKIP  driver (no pfs3aio handler found; set AMIPART_PFS3_HANDLER)")

    sh("rm -f " + " ".join(TMP))
    print("ALL PASS" if fails == 0 else f"{fails} FAILED")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
