#!/usr/bin/env python3
"""
gencat.py - AmiPart catalog toolchain (CatComp/FlexCat-compatible .cd workflow).

The .cd "catalog description" file is the single source of truth for every
translatable string.  This script replaces the parts of the FlexCat/CatComp
toolchain we need so the build is reproducible without those tools installed:

  header   <cd> <out.h>            Generate the C string-id header + the
                                   built-in (English) default string table.
  ct       <cd> <out.ct> [lang]    Emit a translation template for a translator.
  update   <cd> <old.ct> <new.ct>  Bring an existing translation up to date
                                   with a newer .cd: translated strings are
                                   kept, new ones are added in English and
                                   marked "NEW", removed ones are dropped.
  check    <cd> <in.ct>            Report what is untranslated and verify the
                                   %-placeholders and "|" button separators.
  catalog  <cd> <in.ct> <out.catalog>
                                   Compile a translator's .ct into a binary
                                   locale.library .catalog (IFF FORM CTLG).
                                   Runs the same checks as "check" and stops
                                   on a placeholder mismatch.

The .cd / .ct format is the usual CatComp one:

    #language english          ; control lines start with '#'
    #version 1
    ;
    ; a comment                ; comment lines start with ';'
    MSG_OK (//)                ; identifier, optional (ID/MinLen/MaxLen)
    OK                         ; the default string (one logical line)

IDs are assigned sequentially from 0 unless an explicit ID is given in the
(ID/MinLen/MaxLen) field, exactly like CatComp.  Escapes \\n \\t \\r \\e \\\\
\\" and \\xNN are understood.  A trailing backslash continues a long string
onto the next physical line.
"""

import re
import sys
import struct
import time


# --------------------------------------------------------------------------
# .cd / .ct parsing
# --------------------------------------------------------------------------

class Msg:
    __slots__ = ("ident", "num", "minlen", "maxlen", "text", "comment")

    def __init__(self, ident, num):
        self.ident = ident
        self.num = num
        self.minlen = 0
        self.maxlen = 0
        self.text = None        # decoded bytes (the actual string value)
        self.comment = []       # preceding ';' comment lines (for templates)


def _unescape(s):
    """Decode CatComp escapes in a source line into raw bytes."""
    out = bytearray()
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c != "\\":
            out.extend(c.encode("latin-1"))
            i += 1
            continue
        i += 1
        if i >= n:                      # trailing backslash handled by caller
            out.append(ord("\\"))
            break
        e = s[i]
        i += 1
        if e == "n":
            out.append(0x0A)
        elif e == "t":
            out.append(0x09)
        elif e == "r":
            out.append(0x0D)
        elif e == "e":
            out.append(0x1B)
        elif e == "0":
            out.append(0x00)
        elif e == "\\":
            out.append(0x5C)
        elif e == '"':
            out.append(0x22)
        elif e == "x":
            hexs = s[i:i + 2]
            out.append(int(hexs, 16) & 0xFF)
            i += 2
        else:
            out.append(ord(e))          # unknown escape -> literal char
    return bytes(out)


def parse_cd(path, want_text=True):
    """Parse a .cd (or .ct) file.  Returns (header_dict, [Msg, ...])."""
    header = {"language": "english", "version": "0", "name": "AmiPart"}
    msgs = []
    next_id = 0
    pending_comments = []
    cur = None
    expect_string = False

    with open(path, "r", encoding="latin-1") as fh:
        raw_lines = fh.read().split("\n")

    # The .cd is ISO-8859-1 (what the Amiga renders).  An editor that saved
    # it as UTF-8 turns every non-ASCII character into a 2-byte sequence that
    # shows up on the Amiga as "·", "é" ... - catch that here.
    for n, l in enumerate(raw_lines, 1):
        if "\u00c2" in l or "\u00c3" in l:
            print(f"{path}:{n}: WARNING: looks like UTF-8 encoded text "
                  f"(save the file as ISO-8859-1): {l.strip()[:60]}", file=sys.stderr)

    i = 0
    while i < len(raw_lines):
        line = raw_lines[i]
        i += 1
        stripped = line.strip()

        if expect_string:
            # The line(s) immediately following an identifier are its string.
            text = line
            # backslash line continuation
            while text.endswith("\\") and not text.endswith("\\\\") and i < len(raw_lines):
                text = text[:-1] + raw_lines[i]
                i += 1
            cur.text = _unescape(text)
            expect_string = False
            cur = None
            continue

        if stripped == "":
            continue
        if stripped.startswith(";"):
            pending_comments.append(stripped[1:].strip())
            continue
        if stripped.startswith("#"):
            # .cd control lines use one '#', .ct ones two ("## language x")
            parts = stripped.lstrip("#").split(None, 1)
            if parts:
                key = parts[0].lower()
                val = parts[1].strip() if len(parts) > 1 else ""
                if key in ("language", "version", "name", "basename"):
                    header["name" if key == "basename" else key] = val
            pending_comments = []
            continue

        # identifier line: IDENT optionally followed by (ID/MinLen/MaxLen)
        ident = stripped
        num = None
        minlen = maxlen = 0
        if "(" in stripped:
            ident = stripped[:stripped.index("(")].strip()
            spec = stripped[stripped.index("(") + 1:]
            if ")" in spec:
                spec = spec[:spec.index(")")]
            fields = spec.split("/")
            if fields and fields[0].strip():
                num = int(fields[0].strip())
            if len(fields) > 1 and fields[1].strip():
                minlen = int(fields[1].strip())
            if len(fields) > 2 and fields[2].strip():
                maxlen = int(fields[2].strip())

        if num is None:
            num = next_id
        next_id = num + 1

        # Two messages with one name would silently give the name the id of
        # the LAST one (the C header just redefines the macro), so every use
        # of the first would show the wrong text.
        if any(x.ident == ident for x in msgs):
            sys.exit("%s:%d: duplicate message identifier %s" % (path, i, ident))

        m = Msg(ident, num)
        m.minlen, m.maxlen = minlen, maxlen
        m.comment = pending_comments
        pending_comments = []
        msgs.append(m)
        cur = m
        expect_string = True

    if want_text:
        for m in msgs:
            if m.text is None:
                m.text = b""
    return header, msgs


# --------------------------------------------------------------------------
# C header / default-table generation
# --------------------------------------------------------------------------

def ct_escape(b):
    """Escape a string for a .cd/.ct line, in the form _unescape reads back:
    only control characters and the backslash are escaped; everything else
    (including ISO-8859-1 letters and quotes) is written as it is."""
    out = []
    for ch in b:
        if ch == 0x5C:
            out.append("\\\\")
        elif ch == 0x0A:
            out.append("\\n")
        elif ch == 0x09:
            out.append("\\t")
        elif ch == 0x0D:
            out.append("\\r")
        elif ch == 0x1B:
            out.append("\\e")
        elif ch < 0x20:
            out.append("\\x%02X" % ch)
        else:
            out.append(bytes([ch]).decode("latin-1"))
    return "".join(out)


def c_escape(b):
    out = []
    for ch in b:
        if ch == 0x22:
            out.append('\\"')
        elif ch == 0x5C:
            out.append("\\\\")
        elif ch == 0x0A:
            out.append("\\n")
        elif ch == 0x09:
            out.append("\\t")
        elif ch == 0x0D:
            out.append("\\r")
        elif ch == 0x1B:
            out.append("\\033")
        elif 0x20 <= ch < 0x7F:
            out.append(chr(ch))
        else:
            out.append("\\%03o" % ch)        # octal keeps it byte-exact
    return "".join(out)


def gen_header(cd, out):
    header, msgs = parse_cd(cd)
    count = len(msgs)
    lines = []
    lines.append("/* Auto-generated from %s by support/gencat.py. DO NOT EDIT. */" % cd)
    lines.append("#ifndef AMIPART_STRINGS_H")
    lines.append("#define AMIPART_STRINGS_H")
    lines.append("")
    lines.append("/* Message ids - also the locale.library catalog string numbers. */")
    for m in msgs:
        lines.append("#define %-28s %d" % (m.ident, m.num))
    lines.append("")
    lines.append("#define MSG_COUNT %d" % count)
    lines.append("")
    lines.append("#endif /* AMIPART_STRINGS_H */")
    lines.append("")
    lines.append("/* Built-in (English) defaults - used when no catalog is loaded, e.g.")
    lines.append(" * on Kickstart 2.04 where locale.library is absent.  The table is")
    lines.append(" * indexed directly by message id (ids are contiguous from 0).")
    lines.append(" * Defined out here (past the include guard) so locale_support.c can")
    lines.append(" * emit the one instance with DPSTRINGS_DEFINE_TABLE on a 2nd include. */")
    lines.append("#ifdef DPSTRINGS_DEFINE_TABLE")
    lines.append("#undef DPSTRINGS_DEFINE_TABLE")
    lines.append("const char *const DPStringDefaults[MSG_COUNT] = {")
    # emit indexed so explicit/sparse ids still line up
    by_id = {m.num: m for m in msgs}
    maxid = max(by_id) if by_id else -1
    for idx in range(maxid + 1):
        m = by_id.get(idx)
        if m is None:
            lines.append('    "",')
        else:
            lines.append('    /* %3d %s */ "%s",' % (m.num, m.ident, c_escape(m.text)))
    lines.append("};")
    lines.append("#endif /* DPSTRINGS_DEFINE_TABLE */")
    with open(out, "w", encoding="latin-1") as fh:
        fh.write("\n".join(lines) + "\n")
    sys.stderr.write("gencat: wrote %s (%d strings)\n" % (out, count))


# --------------------------------------------------------------------------
# .ct translation template
# --------------------------------------------------------------------------

NEW_MARK = "NEW - not translated yet"


def _catver(v):
    """Catalog version as "version.revision" (the .cd carries only "1")."""
    v = v.strip() or "0"
    return v if "." in v else v + ".0"


def _write_ct(cd, out, lang, old=None, old_header=None):
    """Write a .ct for every message of the .cd.  old: {ident: text bytes} of
    an existing translation to carry over (None = fresh template)."""
    header, msgs = parse_cd(cd)
    ver = "$VER: %s.catalog %s (%s)" % (header["name"], _catver(header["version"]), "DD.MM.YYYY")
    if old_header and old_header.get("version", "").startswith("$VER"):
        ver = old_header["version"]
    lines = []
    lines.append("## version %s" % ver)
    lines.append("## language %s" % lang)
    lines.append("## codeset 0")
    lines.append(";")
    lines.append("; AmiPart translation.  Source of the English text: %s" % cd.split("/")[-1])
    lines.append("; Translate the line under each identifier.")
    lines.append(";   * keep every %-placeholder (%s %lu %ld ...): same count, same order")
    lines.append(";   * keep the \"|\" separators of button rows (\"OK|Cancel\")")
    lines.append(";   * keep \\n line breaks; save this file as ISO-8859-1 (Latin-1)")
    lines.append(";   * a string left in English is fine - it simply stays English")
    lines.append(";")
    new = kept = 0
    for m in msgs:
        for c in m.comment:
            if c != NEW_MARK:
                lines.append("; %s" % c if c else ";")
        if old is not None and m.ident in old:
            text = old[m.ident]
            kept += 1
        else:
            text = m.text
            if old is not None:
                lines.append("; %s" % NEW_MARK)
                new += 1
        lines.append("%s (%d//)" % (m.ident, m.num))
        lines.append(ct_escape(text))
        lines.append(";")
    with open(out, "w", encoding="latin-1") as fh:
        fh.write("\n".join(lines) + "\n")
    return len(msgs), kept, new


def gen_ct(cd, out, lang):
    n, _, _ = _write_ct(cd, out, lang)
    sys.stderr.write("gencat: wrote %s (%d strings)\n" % (out, n))


def gen_update(cd, old_ct, out):
    old_header, old_msgs = parse_cd(old_ct)
    old = {m.ident: m.text for m in old_msgs if m.text is not None}
    cd_idents = {m.ident for m in parse_cd(cd)[1]}
    gone = sorted(i for i in old if i not in cd_idents)
    n, kept, new = _write_ct(cd, out, old_header.get("language", "unknown"),
                             old=old, old_header=old_header)
    sys.stderr.write("gencat: wrote %s (%d strings: %d kept, %d new, %d dropped)\n"
                     % (out, n, kept, new, len(gone)))
    for i in gone:
        sys.stderr.write("  dropped (no longer in the program): %s\n" % i)


# --------------------------------------------------------------------------
# translation checks
# --------------------------------------------------------------------------

_PLACEHOLDER = re.compile(rb"%(?:%|[-+ 0#]*[0-9]*(?:\.[0-9]+)?l?[sdiuxXc])")


def _placeholders(text):
    return [p for p in _PLACEHOLDER.findall(text) if p != b"%%"]


def check_ct(cd, ct, quiet=False):
    """Match a translation against the .cd by IDENTIFIER.  Returns
    (strings to compile as [(id, bytes)], number of errors)."""
    _, cd_msgs = parse_cd(cd)
    _, ct_msgs = parse_cd(ct)
    by_ident = {m.ident: m for m in cd_msgs}
    out, errors, same, seen = [], 0, 0, set()
    for m in ct_msgs:
        if m.text is None:
            continue
        ref = by_ident.get(m.ident)
        if ref is None:
            sys.stderr.write("%s: note: %s is not in the program any more - skipped\n"
                             % (ct, m.ident))
            continue
        seen.add(m.ident)
        if _placeholders(m.text) != _placeholders(ref.text):
            sys.stderr.write("%s: ERROR: %s: placeholders differ from the English text\n"
                             "    english    : %s\n    translation: %s\n"
                             % (ct, m.ident,
                                b" ".join(_placeholders(ref.text)).decode("latin-1") or "(none)",
                                b" ".join(_placeholders(m.text)).decode("latin-1") or "(none)"))
            errors += 1
            continue
        if m.text.count(b"|") != ref.text.count(b"|"):
            sys.stderr.write("%s: ERROR: %s: number of \"|\" button separators differs "
                             "(english %d, translation %d)\n"
                             % (ct, m.ident, ref.text.count(b"|"), m.text.count(b"|")))
            errors += 1
            continue
        if m.text == ref.text:
            same += 1
        out.append((ref.num, m.text))
    missing = [m.ident for m in cd_msgs if m.ident not in seen]
    if not quiet:
        sys.stderr.write("gencat: %s: %d strings in the program, %d translated, "
                         "%d still English, %d missing, %d error(s)\n"
                         % (ct, len(cd_msgs), len(out) - same, same, len(missing), errors))
        if missing:
            sys.stderr.write("  missing (shown in English; run \"update\" to add them): %s%s\n"
                             % (", ".join(missing[:8]), " ..." if len(missing) > 8 else ""))
    return out, errors


# --------------------------------------------------------------------------
# .catalog (IFF FORM CTLG) compilation
# --------------------------------------------------------------------------

def _chunk(cid, data):
    out = cid + struct.pack(">I", len(data)) + data
    if len(data) & 1:
        out += b"\x00"                            # IFF chunks are word-aligned
    return out


def gen_catalog(cd, ct, out):
    cd_header, _ = parse_cd(cd)
    ct_header, _ = parse_cd(ct)
    lang = ct_header.get("language", "unknown")
    name = cd_header.get("name", "AmiPart")
    ver = cd_header.get("version", "0")

    # Ids come from the .cd by identifier, never from the numbers in the .ct:
    # a translation made for an older .cd then still lands on the right
    # strings.  A wrong %-placeholder would crash the program, so it is fatal.
    strings, errors = check_ct(cd, ct)
    if errors:
        sys.exit("gencat: %d error(s) - no catalog written" % errors)

    fver = ct_header.get("version", "")
    if not fver.startswith("$VER") or "DD.MM.YYYY" in fver:
        fver = "$VER: %s.catalog %s (%s)" % (name, _catver(ver), time.strftime("%d.%m.%Y"))
    fver = fver.encode("latin-1") + b"\x00"
    langb = lang.encode("latin-1") + b"\x00"
    # struct CodeSet: cs_CodeSet + 7 reserved longs (32 bytes, as CatComp and
    # FlexCat write it).  Codeset 0 = ISO-8859-1.
    cset = struct.pack(">8I", 0, 0, 0, 0, 0, 0, 0, 0)

    strs = bytearray()
    for num, text in sorted(strings):
        s = text + b"\x00"
        if len(s) & 1:
            s += b"\x00"
        strs += struct.pack(">II", num, len(s))
        strs += s

    body = b"CTLG"
    body += _chunk(b"FVER", fver)
    body += _chunk(b"LANG", langb)
    body += _chunk(b"CSET", cset)
    body += _chunk(b"STRS", bytes(strs))

    form = _chunk(b"FORM", body)
    with open(out, "wb") as fh:
        fh.write(form)
    sys.stderr.write("gencat: wrote %s (%s, %d strings)\n" % (out, lang, len(strings)))


# --------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "header" and len(sys.argv) == 4:
        gen_header(sys.argv[2], sys.argv[3])
    elif cmd == "ct" and len(sys.argv) in (4, 5):
        gen_ct(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) == 5 else "deutsch")
    elif cmd == "update" and len(sys.argv) == 5:
        gen_update(sys.argv[2], sys.argv[3], sys.argv[4])
    elif cmd == "check" and len(sys.argv) == 4:
        return 1 if check_ct(sys.argv[2], sys.argv[3])[1] else 0
    elif cmd == "catalog" and len(sys.argv) == 5:
        gen_catalog(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.stderr.write(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
