"""
Upisuje vreme builda u src/generated/build_info.h (FW_BUILD), da /api/status uvek pokazuje
stvarni build — i kad se ne kompajlira ponovo fajl koji ga prikazuje. Menja se samo taj
header, pa se ponovo kompajlira samo src/build_info.cpp.
"""
import os
import time

try:
    Import("env")  # noqa: F821
    PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

out = os.path.join(PROJECT_DIR, "src", "generated", "build_info.h")
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("// GENERISANO: tools/build_info.py — ne menjati ručno\n#pragma once\n")
    f.write('#define FW_BUILD_TIME "%s"\n' % time.strftime("%Y-%m-%d %H:%M:%S"))
