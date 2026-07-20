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
import os

proj = env["PROJECT_DIR"]  # noqa: F821
data_dir = os.path.join(proj, "data")

# web/ shipped with the library (this file is <lib>/scripts/gzip_web.py)
script_dir = os.path.dirname(os.path.abspath(__file__))
lib_web = os.path.join(os.path.dirname(script_dir), "web")
# optional per-project override
proj_web = os.path.join(proj, "web")

os.makedirs(data_dir, exist_ok=True)


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
