"""
Ugrađuje web UI u firmware: web/index.html -> gzip -> src/generated/web_index.h (PROGMEM).
Ne treba poseban upload fajl sistema. Pokreće se kao PlatformIO pre-script ili ručno.
"""
import gzip
import os

try:
    Import("env")  # noqa: F821
    PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC = os.path.join(PROJECT_DIR, "web", "index.html")
OUT = os.path.join(PROJECT_DIR, "src", "generated", "web_index.h")


def main():
    with open(SRC, "rb") as f:
        raw = f.read()
    gz = gzip.compress(raw, compresslevel=9, mtime=0)
    lines = []
    for i in range(0, len(gz), 20):
        lines.append("  " + ", ".join("0x%02X" % b for b in gz[i:i + 20]) + ",")
    text = (
        "// GENERISANO: tools/embed_web.py iz web/index.html — ne menjati ručno\n"
        "#pragma once\n#include <Arduino.h>\n\n"
        "const uint8_t WEB_INDEX_GZ[] PROGMEM = {\n" + "\n".join(lines) + "\n};\n"
        "const size_t WEB_INDEX_GZ_LEN = %d;\n" % len(gz)
    )
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    old = None
    if os.path.exists(OUT):
        with open(OUT, encoding="utf-8") as f:
            old = f.read()
    if old != text:
        with open(OUT, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("embed_web: %s (%d B -> %d B gzip)" % (OUT, len(raw), len(gz)))


main()
