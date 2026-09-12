AmiPart grow/shrink fuzz loop (Linux host, not Amiga)
======================================================

testloop.py exercises GROW + SHRINK end-to-end against the real AmiPart
binary running under vamos (amitools).  Each iteration:

  random FS (FFS real via xdftool / PFS3 + SFS synthesized) with random
  FS blocksize, random disk + partition geometry, random content
  -> GROW by a random amount (sometimes END)
  -> verify content survived (FFS: file md5s; synth: data-block hashes)
  -> SHRINKINFO -> SHRINK in a random form (MIN / target size / -amount)
  -> verify content + RDB + structures again
  -> sometimes a second grow/shrink round (catches interaction bugs)
  -> discard; failed iterations keep their image as fail_itN_seedM.hdf

Between FFS operations the loop rebuilds the bitmap in python (both grow
and shrink leave bm_flag=0 for the OS validator, which doesn't exist
under vamos).

Requirements: pipx amitools (vamos/rdbtool/xdftool) with machine68k==0.3.0,
plus "vamos -O locale.library=mode:off" support (fake GetCatalogStr would
otherwise return empty strings).

Usage:  python3 testloop.py [iterations] [base_seed]
Every iteration logs its full parameter set to testloop.log - any failure
is reproducible from the seed.

v2 additions: random geometry (1-16 heads x 32/63 sectors - varies
blocks/cylinder), random partition start, optional fingerprinted neighbor
partition (grow-clamp + no-touch check), DOS0-DOS3 variants for real FFS,
synthetic FFS at both 512- and 1024-byte blocks (spb=2) for geometries and
blocksizes xdftool cannot produce (incl. its 63-sector root-position bug).

Bugs this loop has caught on day one (2026-07-20):
  - FFS GROW silently overwrote user data when the relocated root's new
    centre position landed on an allocated block (small grows of full
    partitions) - now refused like the shrink does.
  - PFS3 shrink's bitmap "seal" made a later grow strand the re-added
    space (real pfs3 never seals; seal removed, grow gained a repair
    unseal pass).
  - PFS3 grow left rootblock disksize stale once MODE_SIZEFIELD had been
    cleared by an earlier grow/shrink, skewing later size math.
  - SFS/PFS3 shrink could leave the filesystem LARGER than the shrunk
    envelope with fractional blocks-per-cylinder geometries (bpc floors,
    so shrink under-removed; SFS then wrote its end root beyond the
    partition).  Both engines now hard-clamp to the RDB envelope.
  - SFS SHRINKINFO reported a floor below what the no-relocation v1
    shrink can deliver on strategy-B-grown volumes (bitmap sits high);
    the floor now respects bitmapbase+num_bmb+1.

Internal formatter test (fmttest.py)
------------------------------------
python3 fmttest.py formats a partition with the internal formatter
(src/nativefmt.c; ADDPART ... VOLNAME= without SAFE) and compares the
result byte-for-byte with a reference made by an independent, real
implementation of the same geometry (only timestamps, the checksums over
them and PFS3's AmiPart-set fnsize are masked):
  FFS  every DOS\0..DOS\7 variant + a bitmap-extension case vs xdftool;
       xdftool then opens and lists the volume.
  PFS3 several sizes/geometries + a sparse 6 GB supermode partition vs the
       real pfs3aio 19.2 handler formatting under AmiFUSE
       (pipx install amifuse; handler auto-found in ~/UAE/*/L/pfs3aio or
       AMIPART_PFS3_HANDLER=...).
  SFS  SFS\0 at 512 and 1024-byte blocks + SFS\2 vs the real
       SmartFilesystem 1.279 handler (AMIPART_SFS_HANDLER=...).
For PFS3/SFS the real handler afterwards writes a file into the
AmiPart-formatted volume and reads it back.  PFS3/SFS cases are skipped
when amifuse or the handler binaries are missing (CI only runs the FFS
part).  With vamos + out/AmiPart present the m68k binary must produce the
identical image.
