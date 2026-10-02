"""PlatformIO pre-build hook for WiFiProvision.

Gzips the portal UI into the consumer project's ``data/`` folder so the normal
LittleFS image build (``pio run -t buildfs``) picks it up. The device then serves
the page straight from flash with ``Content-Encoding: gzip``.

The HTML source lives inside this library (``../web/portal.html``). A project may
override or add assets by creating its own ``web/`` folder — those shadow the
library's files of the same name.

Register it in the consumer ``platformio.ini``:

    extra_scripts = pre:${platformio.libdeps_dir}/${this.__env__}/WiFiProvision/scripts/gzip_web.py

Then flash the filesystem:  pio run -t buildfs && pio run -t uploadfs
"""
Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

import gzip
import inspect
import os
import re

proj = env["PROJECT_DIR"]  # noqa: F821
data_dir = os.path.join(proj, "data")

# web/ shipped with the library (this file is <lib>/scripts/gzip_web.py).
# SCons exec()s extra scripts without defining __file__, so take the path from
# the frame instead — SCons compiles the script under its real filename.
script_dir = os.path.dirname(os.path.abspath(inspect.getfile(inspect.currentframe())))
lib_web = os.path.join(os.path.dirname(script_dir), "web")
# optional per-project override
proj_web = os.path.join(proj, "web")

os.makedirs(data_dir, exist_ok=True)


# The portal AP has no internet. A remote @import (the design system pulls its
# webfonts from Google Fonts) would resolve to our own captive DNS, hit the
# portal's 302 and fail — a wasted round-trip on a single-threaded server for
# every page load. The font stacks all end in a system fallback, so strip it.
REMOTE_IMPORT = re.compile(rb"@import\s+url\(\s*['\"]?https?:[^)]*\)\s*;", re.I)


def strip_remote_imports(raw, rel):
    out, n = REMOTE_IMPORT.subn(b"", raw)
    if n:
        print("[WiFiProvision] %s: dropped %d remote @import (no internet on the AP)" % (rel, n))
    return out


def pack(src_dir):
    if not os.path.isdir(src_dir):
        return 0
    count = 0
    for root, _, files in os.walk(src_dir):
        for fn in files:
            if fn.endswith(".gz"):
                continue
            src = os.path.join(root, fn)
            rel = os.path.relpath(src, src_dir)
            dst = os.path.join(data_dir, rel + ".gz")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(src, "rb") as fi:
                raw = fi.read()
            if fn.endswith(".css"):
                raw = strip_remote_imports(raw, rel)
            # mtime=0 keeps the output byte-identical across rebuilds.
            with gzip.GzipFile(filename=dst, mode="wb", compresslevel=9, mtime=0) as fo:
                fo.write(raw)
            print("[WiFiProvision] %s -> data/%s.gz (%d -> %d B)"
                  % (rel, rel, len(raw), os.path.getsize(dst)))
            count += 1
    return count


total = pack(lib_web)
total += pack(proj_web)  # project files override library ones of the same name
print("[WiFiProvision] packed %d asset(s) into data/" % total)
