#!/usr/bin/env python3
"""Compile a gettext .po file into a GNU .mo file without requiring msgfmt.

Used by local Windows builds to place compiled translations next to the
built executable (build/locale) even when gettext tools are not installed.
The output is a standard revision-0 .mo file with the original-string table
sorted by byte order, exactly like GNU msgfmt produces.
"""

import struct
import sys
from pathlib import Path


_ESCAPES = {
    "a": "\a", "b": "\b", "f": "\f", "n": "\n",
    "r": "\r", "t": "\t", "v": "\v", "\\": "\\", '"': '"',
}


def unescape(s: str) -> str:
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        i += 1
        if i >= n:
            break
        e = s[i]
        if e in _ESCAPES:
            out.append(_ESCAPES[e])
            i += 1
        elif e == "0":
            out.append("\0")
            i += 1
        elif e == "x":
            hexs = s[i + 1:i + 3]
            if len(hexs) == 2:
                out.append(chr(int(hexs, 16)))
                i += 3
            else:
                out.append("x")
                i += 1
        elif e.isdigit():
            j = i
            while j < min(n, i + 3) and s[j].isdigit():
                j += 1
            out.append(chr(int(s[i:j], 8)))
            i = j
        else:
            out.append(e)
            i += 1
    return "".join(out)


def unquote(s: str) -> str:
    s = s.strip()
    if s.startswith('"'):
        s = s[1:]
    if s.endswith('"') and len(s) >= 1:
        s = s[:-1]
    return unescape(s)


def parse_po(text: str):
    entries = []
    cur = None
    field = None
    obsolete = False

    def new_entry():
        nonlocal cur, field, obsolete
        cur = {"ctxt": "", "id": "", "id_plural": None, "strs": []}
        field = None
        obsolete = False

    def flush():
        nonlocal cur
        if cur is not None and not obsolete:
            entries.append(cur)
        cur = None

    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            flush()
            field = None
            continue
        if line.startswith("#~"):
            obsolete = True
            if cur is None:
                new_entry()
            continue
        if line.startswith("#"):
            continue
        if obsolete:
            continue
        if line.startswith("msgctxt"):
            flush()
            new_entry()
            cur["ctxt"] = unquote(line[7:])
            field = "ctxt"
        elif line.startswith("msgid_plural"):
            cur["id_plural"] = unquote(line[len("msgid_plural"):])
            field = "id_plural"
        elif line.startswith("msgid"):
            flush()
            new_entry()
            cur["id"] = unquote(line[len("msgid"):])
            field = "id"
        elif line.startswith("msgstr["):
            idx = int(line[line.index("[") + 1:line.index("]")])
            while len(cur["strs"]) <= idx:
                cur["strs"].append("")
            cur["strs"][idx] = unquote(line[line.index("]") + 1:])
            field = ("str", idx)
        elif line.startswith("msgstr"):
            while len(cur["strs"]) < 1:
                cur["strs"].append("")
            cur["strs"][0] = unquote(line[len("msgstr"):])
            field = ("str", 0)
        elif line.startswith('"') and cur is not None:
            s = unquote(line)
            if field == "ctxt":
                cur["ctxt"] += s
            elif field == "id":
                cur["id"] += s
            elif field == "id_plural":
                cur["id_plural"] += s
            elif isinstance(field, tuple) and field[0] == "str":
                idx = field[1]
                cur["strs"][idx] += s
    flush()
    return entries


def build_catalog(entries):
    catalog = {}
    for e in entries:
        key = (e["ctxt"] + "\x04" + e["id"]) if e["ctxt"] else e["id"]
        if e["id_plural"] is not None:
            key += "\x00" + e["id_plural"]
        value = "\x00".join(e["strs"]) if e["strs"] else ""
        catalog[key] = value
    return catalog


def write_mo(catalog, out: Path):
    keys = sorted(catalog, key=lambda k: k.encode("utf-8"))
    n = len(keys)
    orig = [k.encode("utf-8") for k in keys]
    trans = [catalog[k].encode("utf-8") for k in keys]

    table_off = 20
    orig_off = table_off + 8 * n
    trans_off = orig_off + 8 * n
    data_off = trans_off + 8 * n

    buf = bytearray(struct.pack("<IiiII", 0x950412DE, 0, n, orig_off, trans_off))
    # Tables live at their declared offsets, after the 20-byte header.
    buf += b"\0" * (orig_off - len(buf))

    pos = data_off
    orig_pairs = []
    for b in orig:
        orig_pairs.append((len(b), pos))
        pos += len(b) + 1
    trans_pairs = []
    for b in trans:
        trans_pairs.append((len(b), pos))
        pos += len(b) + 1

    for length, offset in orig_pairs:
        buf += struct.pack("<II", length, offset)
    for length, offset in trans_pairs:
        buf += struct.pack("<II", length, offset)
    for b in orig:
        buf += b + b"\0"
    for b in trans:
        buf += b + b"\0"

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(buf)


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: genmo.py INPUT.po OUTPUT.mo\n")
        return 2
    po_path = Path(sys.argv[1])
    mo_path = Path(sys.argv[2])
    text = po_path.read_text(encoding="utf-8-sig")
    entries = parse_po(text)
    catalog = build_catalog(entries)
    write_mo(catalog, mo_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
