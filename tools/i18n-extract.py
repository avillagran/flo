#!/usr/bin/env python3
"""Lists Flo's translatable strings into i18n/strings.json.

Scans src/ for fo::tr("...") and fo::trn("...") literals (adjacent string
literals are joined, C escapes decoded) and adds the strings translated at
run time from data (sidebar places, sort names, the folder errors the core
reports). Run after changing strings; tools/i18n-check.py then reports
what each catalog lacks.

    tools/i18n-extract.py
"""
import json
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
# Strings that reach fo::tr() through a variable.
DYNAMIC = [
    # Sidebar places (home folders named by their XDG role, and views).
    "Home", "Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos", "Templates", "Public",
    "Recents", "Trash",
    # The sort menu.
    "Name", "Kind", "Newest", "Oldest", "Size",
    # Folder errors from flo-core's lister, shown as they come.
    "This folder no longer exists.", "You don't have permission to open this folder.", "This is not a folder.",
    # Saved folder views (core/folder_views.cpp), shown as they come.
    "Up to 256 folders keep a view; forget one first.", "Only local folders keep a view.",
    "The folder views file can't be changed.",
    # The favorites hub's buttons and the core's favorites sentences.
    "Enter", "Save", "Current", "Label", "Remove",
    "This folder is not a favorite.", "Type a single line without control characters.",
    "Only folders can be pinned.", "Up to 10000 starred items.",
    "The starred list can't be read; it is kept unchanged.",
    # Properties (core/properties.cpp reasons, the permission row labels).
    "Owner", "Group", "Others", "Only local paths have properties.",
    "Only your own files and folders without special bits can change here.",
    "The object changed since it was read; reload before applying.",
]

call = re.compile(r'\b(?:fo::)?(trn?)\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)')
literal = re.compile(r'"((?:[^"\\]|\\.)*)"')


def decode(s):
    # C escapes as Flo uses them: \xNN (UTF-8 bytes), \n, \", \\.
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                j = i + 2
                while j < len(s) and j < i + 4 and s[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(int(s[i + 2:j], 16))
                i = j
                continue
            out += {"n": b"\n", "t": b"\t", '"': b'"', "\\": b"\\", "'": b"'"}.get(n, n.encode())
            i += 2
            continue
        out += c.encode()
        i += 1
    return out.decode("utf-8")


strings = {}
for path in sorted((root / "src").rglob("*.cpp")):
    text = path.read_text(encoding="utf-8")
    for m in call.finditer(text):
        value = "".join(decode(x) for x in literal.findall(m.group(2)))
        plural = m.group(1) == "trn"
        strings[value] = strings.get(value, False) or plural
for s in DYNAMIC:
    strings.setdefault(s, False)

out = [{"source": k, "plural": v} for k, v in sorted(strings.items(), key=lambda kv: kv[0].lower())]
(root / "i18n" / "strings.json").write_text(json.dumps(out, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
print(f"{len(out)} strings ({sum(1 for x in out if x['plural'])} plural) -> i18n/strings.json")
