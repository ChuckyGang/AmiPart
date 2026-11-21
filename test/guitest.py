#!/usr/bin/env python3
"""guitest.py - headless tests of the Linux GUI (host/amipart-gui).

The GUI is driven with AMIPART_GUI_HEADLESS=1 and a script file
(AMIPART_GUI_SCRIPT, see hg_script_step in host/host_gui.c): no display
and no SDL video are needed, so this runs in CI.

What is checked:
  - the GUI starts, lists devices and opens an image file
  - Delete + Write in the GUI gives the same image, byte for byte, as
    DELPART on the command line
  - Add in a free gap + Write gives a table the CLI reads back correctly
  - Rescue (scan, "Load into AmiPart", Write) gives the same image as
    RESCUE WRITE on the command line
  - the map zoom controls change the view and leave the table alone
  - Filesystems > Add defaults to the built-in pfs3aio; a file named by
    the user is used instead, whatever its version
  - every menu entry can be opened without the program failing

Build first:  make -C host amipart gui
Run:          python3 test/guitest.py        (from the repo root)
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUI = os.path.join(ROOT, "host", "amipart-gui")
CLI = os.path.join(ROOT, "host", "amipart")

# Menu layout of the main window: number of entries per menu.  Entries
# that are separator bars are skipped by the GUI itself.
MENUS = {0: 3, 1: 31, 2: 7, 3: 11}

fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS  " if ok else "FAIL  ") + name + (("  " + detail) if detail and not ok else ""))
    if not ok:
        fails += 1


def cli(img, args):
    return subprocess.run([CLI, "IMAGE=" + img] + args.split(), stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, errors="replace", timeout=120)


def gui(workdir, lines, timeout=120, extra_env=None):
    """Run the GUI with a script; returns (returncode, stdout)."""
    script = os.path.join(workdir, "script.txt")
    with open(script, "w", encoding="latin-1") as f:
        f.write("\n".join(lines) + "\n")
    # File requesters start in the user's home directory (not the current
    # one), so the work directory poses as home; cwd is deliberately not it.
    env = dict(os.environ, AMIPART_GUI_HEADLESS="1", AMIPART_GUI_SCRIPT=script,
               HOME=workdir)
    env.pop("SUDO_USER", None)
    if extra_env:
        env.update(extra_env)
    p = subprocess.run([GUI], cwd="/", env=env, stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, errors="replace", timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def open_image(name):
    """Script lines from program start to the main window on image <name>."""
    return ["expect WARNING", "gadid 1",          # startup warning -> Continue
            "expect Select Device", "gadid 6",    # Use Image
            "set 3 " + name,                      # file requester: name + Return
            "expect " + name]


def make_image(path):
    for a in ("CREATE SIZE=200M", "INIT NEW FORCE",
              "ADDPART NAME=DH0 LOW=2 HIGH=100 TYPE=DOS3 BOOTABLE FORCE",
              "ADDPART NAME=DH1 LOW=101 HIGH=300 TYPE=PFS3 FORCE"):
        r = cli(path, a)
        if r.returncode != 0:
            print(r.stdout, r.stderr)
            sys.exit("cannot build the test image: " + a)


def same(a, b):
    with open(a, "rb") as fa, open(b, "rb") as fb:
        return fa.read() == fb.read()


def main():
    for b in (GUI, CLI):
        if not os.path.exists(b):
            sys.exit("missing %s - run: make -C host amipart gui" % b)

    d = tempfile.mkdtemp(prefix="amipart-guitest-")
    try:
        base = os.path.join(d, "base.hdf")
        make_image(base)

        # ---- start + open ---------------------------------------------
        shutil.copy(base, os.path.join(d, "t.hdf"))
        rc, out = gui(d, open_image("t.hdf") + ["dump"])
        check("start, device list, open image", rc == 0 and "DH0" in out and "DH1" in out
              and "FFS+Intl" in out, out[-600:])

        # ---- file requester: click a file in the list, then Open ----------
        shutil.copy(base, os.path.join(d, "t.hdf"))
        rc, out = gui(d, ["gadid 1", "gadid 6", "expect image file",
                          "lvrow 2 2", "expect image file", "dump",     # rows: base.hdf, script.txt, t.hdf
                          "gadid 4", "expect t.hdf", "dump"])
        check("file requester: pick a file by clicking it",
              rc == 0 and 'value="t.hdf"' in out and "DH1" in out, out[-700:])

        # ---- delete + write == CLI DELPART ------------------------------
        shutil.copy(base, os.path.join(d, "t.hdf"))
        shutil.copy(base, os.path.join(d, "c.hdf"))
        rc, out = gui(d, open_image("t.hdf") + [
            "lvrow 1 1", "gadid 5",            # select DH1, Delete
            "expect Delete", "gadid 1",        # confirm
            "gadid 7", "expect Write partition table", "gadid 1"])  # Write, confirm
        r = cli(os.path.join(d, "c.hdf"), "DELPART NAME=DH1 FORCE")
        check("delete + write: same image as CLI DELPART",
              rc == 0 and r.returncode == 0 and same(os.path.join(d, "t.hdf"), os.path.join(d, "c.hdf")),
              out[-600:])

        # ---- same through the raw-device backend ---------------------------
        # The shim driver lets a plain file under /dev/shm stand in for a
        # block device (as the CLI job in CI does), so no root is needed.
        if os.path.isdir("/dev/shm"):
            dev = "/dev/shm/amipart-guitest-%d.hdf" % os.getpid()
            shutil.copy(base, dev)
            try:
                rc, out = gui(d, ["expect WARNING", "gadid 1", "expect Select Device",
                                  "set 5 " + dev, "gadid 2",          # manual device name, Select
                                  "expect " + dev, "dump", "lvrow 1 0", "gadid 2",   # unit 0
                                  "expect unit 0", "lvrow 1 1", "gadid 5",
                                  "expect Delete", "gadid 1",
                                  "gadid 7", "expect Write partition table", "gadid 1"],
                              extra_env={"AMIPART_SHIM_DRIVER": "warpata"})
                check("device backend: unit list shows the RDB", "RDB @ block 0: DH0 DH1" in out, out[-600:])
                check("device backend: delete + write same image as CLI",
                      rc == 0 and same(dev, os.path.join(d, "c.hdf")), out[-600:])
            finally:
                os.remove(dev)

        # ---- add in the free gap + write --------------------------------
        shutil.copy(base, os.path.join(d, "t.hdf"))
        rc, out = gui(d, open_image("t.hdf") + [
            "click 600 232",                   # free area at the right of the map
            "gadid 3", "expect Add Partition", "dump",
            "set 1 WORK", "set 3 20", "gadid 15",      # name, 20 MB, do not format
            "gadid 8",                          # OK
            "gadid 7", "expect Write partition table", "gadid 1",
            "expect Partition table written", "gadid 1"])
        info = cli(os.path.join(d, "t.hdf"), "INFO").stdout
        line = [l for l in info.splitlines() if "WORK" in l]
        check("add in free gap + write: CLI reads the new partition",
              rc == 0 and "Lo: 301" in out and len(line) == 1 and "301-" in line[0].replace(" ", ""),
              out[-400:] + info[-400:])

        # ---- map zoom: buttons, keys, wheel, pan scroller -----------------
        # The zoom row sits under the map; the scroller's values in the dump
        # show the visible cylinder window.  Zooming must not change the table.
        shutil.copy(base, os.path.join(d, "t.hdf"))
        rc, out = gui(d, open_image("t.hdf") + [
            "dump",                                     # whole disk
            "lvrow 1 0", "gadid 16", "dump",            # Selected: zoom to DH0
            "key 45", "dump",                           # '-' : out one step
            "wheel 400 222 1", "dump",                  # wheel up over the map: in
            "gadid 15", "dump",                         # Fit
            "gadid 7", "expect Write partition table", "gadid 1"])
        vis = [int(l.split("visible=")[1].split()[0]) for l in out.splitlines() if "scroller" in l]
        tot = [int(l.split("total=")[1].split()[0]) for l in out.splitlines() if "scroller" in l]
        check("map zoom: Selected, '-', wheel and Fit change the view",
              rc == 0 and len(vis) == 5 and vis[0] == tot[0] and vis[1] < vis[0]
              and vis[2] > vis[1] and vis[3] < vis[2] and vis[4] == tot[4], str(vis) + out[-300:])
        check("map zoom: table unchanged", same(os.path.join(d, "t.hdf"), base))

        # ---- rescue: GUI load + write == CLI RESCUE WRITE ----------------
        for n in ("t.hdf", "c.hdf"):
            shutil.copy(base, os.path.join(d, n))
            with open(os.path.join(d, n), "r+b") as f:
                f.write(b"\0" * 512)            # destroy the RDSK block
        rc, out = gui(d, open_image("t.hdf") + [
            "menu 1 30", "expect Scan this disk", "gadid 1",       # Rescue..., Scan
            "expect Rescue Lost Partitions", "dump",
            "gad Load into AmiPart",
            "expect UNSAVED", "dump",
            "gadid 7", "expect Write partition table", "gadid 1"])
        r = cli(os.path.join(d, "c.hdf"), "RESCUE WRITE FORCE")
        check("rescue: report says what Load does",
              "2 entries can be loaded into AmiPart" in out and "nothing is" in out, out[-900:])
        check("rescue + load + write: same image as CLI RESCUE WRITE",
              rc == 0 and r.returncode == 0 and same(os.path.join(d, "t.hdf"), os.path.join(d, "c.hdf")),
              out[-600:] + r.stdout[-300:])

        # ---- Filesystems > Add: built-in pfs3aio, or a file from disk ------
        # The Add dialog starts on the pfs3aio embedded in the binary.  A file
        # the user names is used instead, even when it is an older version.
        fsadd = ["gadid 6", "expect FileSystem Drivers", "gadid 3",
                 "expect Add FileSystem Driver", "dump"]
        fsdone = ["gadid 4", "gadid 6",          # OK, Done
                  "gadid 7", "expect Write partition table", "gadid 1"]
        builtin = open(os.path.join(ROOT, "support", "pfs3aio", "pfs3aio"), "rb").read()
        shutil.copy(base, os.path.join(d, "t.hdf"))
        rc, out = gui(d, open_image("t.hdf") + fsadd + fsdone)
        info = cli(os.path.join(d, "t.hdf"), "INFO").stdout
        img = open(os.path.join(d, "t.hdf"), "rb").read(4 << 20)
        check("add filesystem: built-in pfs3aio is the default",
              rc == 0 and 'value="PFS\\3"' in out and "built-in pfs3aio" in out
              and "PFS\\3     v19.2" in info and builtin[:400] in img, out[-600:] + info[-300:])

        old = bytearray(2000)                    # stand-in handler, version 18.5
        old[0:4] = bytes.fromhex("000003f3")
        old[0x22:0x24] = b"\x4a\xfc"
        old[0x22 + 11] = 18
        old[100:129] = b"$VER: pfs3old 18.5 (1.1.2010)"
        with open(os.path.join(d, "pfs3old"), "wb") as f:
            f.write(old)
        shutil.copy(base, os.path.join(d, "t.hdf"))
        rc, out = gui(d, open_image("t.hdf") + fsadd
                      + ["set 2 " + os.path.join(d, "pfs3old")] + fsdone)
        info = cli(os.path.join(d, "t.hdf"), "INFO").stdout
        img = open(os.path.join(d, "t.hdf"), "rb").read(4 << 20)
        check("add filesystem: a file from disk replaces the built-in (older version)",
              rc == 0 and "PFS\\3     v18.5" in info and bytes(old[:400]) in img
              and builtin[:400] not in img, out[-600:] + info[-300:])

        # ---- every menu entry opens --------------------------------------
        bad = []
        for m, count in MENUS.items():
            for i in range(count):
                shutil.copy(base, os.path.join(d, "t.hdf"))
                rc, out = gui(d, open_image("t.hdf") + ["lvrow 1 0", "menu %d %d" % (m, i), "dump"])
                if rc != 0:
                    bad.append("%d/%d rc=%d %s" % (m, i, rc, out[-200:]))
        check("all %d menu entries open" % sum(MENUS.values()), not bad, "; ".join(bad))
    finally:
        shutil.rmtree(d, ignore_errors=True)

    print("ALL PASS" if not fails else "%d FAILED" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
