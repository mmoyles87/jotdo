#!/usr/bin/env python3
"""Sign Debian source uploads with gpg, the way debsign does: clearsign each
.dsc and .buildinfo a .changes lists, put their new sizes and checksums in the
.changes, then clearsign the .changes.

    packaging/sign-changes.py KEYID build/ppa/*_source.changes
"""

import hashlib
import os
import re
import subprocess
import sys

SIGNED = "-----BEGIN PGP SIGNED MESSAGE-----"


def unsign(text):
    """The body of a clearsigned file, or the text as it is."""
    if not text.startswith(SIGNED):
        return text
    body = text.split("\n\n", 1)[1]
    body = body.split("\n-----BEGIN PGP SIGNATURE-----", 1)[0]
    return re.sub(r"^- ", "", body, flags=re.M) + "\n"


def clearsign(path, key):
    with open(path) as f:
        text = unsign(f.read())
    signed = subprocess.run(["gpg", "--batch", "--yes", "--armor", "--clearsign", "-u", key],
                            input=text.encode(), stdout=subprocess.PIPE, check=True).stdout
    with open(path, "wb") as f:
        f.write(signed)


def sums(path):
    data = open(path, "rb").read()
    return {"Files": hashlib.md5(data).hexdigest(), "Checksums-Sha1": hashlib.sha1(data).hexdigest(),
            "Checksums-Sha256": hashlib.sha256(data).hexdigest(), "size": str(len(data))}


def sign_changes(changes, key):
    folder = os.path.dirname(changes) or "."
    with open(changes) as f:
        text = unsign(f.read())
    names = re.findall(r"^ \S+ \d+ \S+ \S+ (\S+\.(?:dsc|buildinfo))$", text, flags=re.M)
    for name in names:
        path = os.path.join(folder, name)
        clearsign(path, key)
        new = sums(path)
        # each checksum field lists the file as " <sum> <size> [section priority] <name>"
        lines, field = [], None
        for line in text.split("\n"):
            m = re.match(r"^(\S[^:]*):", line)
            if m:
                field = m.group(1)
            parts = line.split()
            if line.startswith(" ") and parts and parts[-1] == name and field in new:
                parts[0], parts[1] = new[field], new["size"]
                line = " " + " ".join(parts)
            lines.append(line)
        text = "\n".join(lines)
    with open(changes, "w") as f:
        f.write(text)
    clearsign(changes, key)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    for c in sys.argv[2:]:
        sign_changes(c, sys.argv[1])
        print("signed", os.path.basename(c))
