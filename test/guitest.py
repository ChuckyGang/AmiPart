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
    env = dict(os.environ, AMIPART_GUI_HEADLESS="1", AMIPART_GUI_SCRIPT=script)
    if extra_env:
        env.update(extra_env)
    p = subprocess.run([GUI], cwd=workdir, env=env, stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, errors="replace", timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def open_image(name):
    """Script lines from program start to the main window on image <name>."""
    return ["expect WARNING", "gadid 1",          # startup warning -> Continue
            "expect Select Device", "gadid 6",    # Use Image
            "set 3 " + name, "gadid 4",           # file requester: name + Open
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
