#!/usr/bin/env python3
"""
Create a minimal LHA archive using -lh0- (stored, no compression).
Level-0 headers — compatible with all Amiga LhA/LhArc versions.

Usage:
  python3 support/make_lha.py output.lha arcdir/file1 arcdir/file2 ...
  The archive path inside the LHA is taken from the argument as given,
  so pass  AmiPart/AmiPart.exe  to store under that path.
"""

import struct, sys, os

def _crc16(data: bytes) -> int:
    """CRC-16/ARC (polynomial 0xA001) — used by LHA."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

def _lha0_entry(arcpath: str, filedata: bytes) -> bytes:
    """Build one level-0 LHA header + stored file data."""
    # LHA uses backslash as path separator inside headers
    name = arcpath.replace('/', '\\').encode('ascii')
    name_len = len(name)

    # Fields that form the header body (everything after the 2-byte preamble)
    body = (
        b'-lh0-'                                    # compression method (5)
        + struct.pack('<II', len(filedata), len(filedata))  # comp / orig size (8)
        + struct.pack('<I', 0)                      # DOS timestamp (4) — zero = unknown
        + struct.pack('BBB', 0x20, 0, name_len)     # attribute, level, name length (3)
        + name                                      # filename (N)
        + struct.pack('<H', _crc16(filedata))        # CRC-16 of original data (2)
    )

    header_size = len(body)
    checksum    = sum(body) & 0xFF
    return bytes([header_size, checksum]) + body + filedata


def create(output_path: str, entries: list) -> None:
    """
    entries: list of (arcpath, real_filepath) tuples.
    arcpath is the path stored inside the archive (e.g. 'AmiPart/AmiPart.exe').
    """
    with open(output_path, 'wb') as f:
        for arcpath, real_path in entries:
            with open(real_path, 'rb') as src:
                data = src.read()
            f.write(_lha0_entry(arcpath, data))
        f.write(b'\x00')   # null header = EOF


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} output.lha [arcpath:realpath ...]")
        print(f"  e.g. {sys.argv[0]} out.lha AmiPart/AmiPart.exe:out/AmiPart.exe")
        sys.exit(1)

    output = sys.argv[1]
    entries = []
    for arg in sys.argv[2:]:
        if ':' in arg:
            arc, real = arg.split(':', 1)
        else:
            arc = real = arg
        entries.append((arc, real))

    create(output, entries)
    size = os.path.getsize(output)
    print(f"Created {output} ({size:,} bytes, {len(entries)} file(s))")
