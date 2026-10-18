"""
Generator fontova za MD_MAX72XX/MD_Parola.

Ulaz:  tools/fonts/*.txt  (glifovi nacrtani kao tekst, '#' = upaljena LED, '.' = ugašena)
Izlaz: src/generated/fonts.h

Format .txt fajla:
    # komentar
    [A]            <- znak (ASCII ili Unicode); slede tačno 8 redova glifa
    .###.
    ...
    [colon2]       <- imenovani glif (mala slova/cifre/_): dobija kod i #define GLYPH_COLON2;
                      za imena "colon*" automatski nastaje i prazan "colon*_blank" iste širine
                      (trepćuća dvotačka bez pomeranja cifara)
    alias АВЕ = ABE    <- Unicode znakovi levo koriste glif znaka desno (isti kod)

Kodovi:  ASCII 0x20..0x7E imaju svoj kod; ostali Unicode znakovi i imenovani glifovi dobijaju
kodove od 0x80 redom pojavljivanja (zajedničko za sve fontove). Znak koji font ne sadrži
dobija zamenski glif '[?box]'.

Pokreće se automatski kao PlatformIO pre-script, ili ručno: python tools/gen_fonts.py
"""
import os
import re
import sys

try:
    Import("env")  # noqa: F821  (PlatformIO/SCons)
    PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FONT_DIR = os.path.join(PROJECT_DIR, "tools", "fonts")
OUT_FILE = os.path.join(PROJECT_DIR, "src", "generated", "fonts.h")

# redosled je bitan: indeks = ID fonta u firmware-u
FONTS = [
    ("text.txt", "FONT_TEXT", "Standardni"),
    ("time_std.txt", "FONT_TIME_STD", "Sat standardni"),
    ("time_bold.txt", "FONT_TIME_BOLD", "Sat masni"),
    ("time_narrow.txt", "FONT_TIME_NARROW", "Sat uski"),
]

REPLACEMENT_KEY = "?box"
FIRST_EXT_CODE = 0x80


def fail(path, lineno, msg):
    raise SystemExit("gen_fonts: %s:%d: %s" % (path, lineno, msg))


def parse_font(path):
    glyphs = {}   # key (znak ili '?box') -> lista kolona
    aliases = []  # (unicode znak, ciljni znak)
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        raw = lines[i]
        line = raw.strip()
        if not line or line.startswith("#") and not set(line) <= set("#."):
            i += 1
            continue
        if line.startswith("alias "):
            left, _, right = line[6:].partition("=")
            left, right = left.strip(), right.strip()
            if len(left) != len(right):
                fail(path, i + 1, "alias: različit broj znakova levo i desno")
            aliases.extend(zip(left, right))
            i += 1
            continue
        if line.startswith("[") and line.endswith("]"):
            key = line[1:-1]
            if key == "space":
                key = " "
            if len(key) != 1 and key != REPLACEMENT_KEY and not re.fullmatch(r"[a-z][a-z0-9_]*", key):
                fail(path, i + 1, "neispravna oznaka glifa %r" % key)
            rows = [r.strip() for r in lines[i + 1:i + 9]]
            if len(rows) != 8:
                fail(path, i + 1, "glif %r nema 8 redova" % key)
            width = len(rows[0]) if rows[0] != "-" else 0
            cols = [0] * width
            for r, row in enumerate(rows):
                if row == "-":
                    row = ""
                if len(row) != width or not set(row) <= set("#."):
                    fail(path, i + 2 + r, "glif %r: red %d neispravan (%r)" % (key, r, row))
                for c, ch in enumerate(row):
                    if ch == "#":
                        cols[c] |= 1 << r
            if key in glyphs:
                fail(path, i + 1, "glif %r je definisan dva puta" % key)
            glyphs[key] = cols
            i += 9
            continue
        fail(path, i + 1, "nepoznat red %r" % raw)
    # prazne verzije dvotački (iste širine) za treptanje bez pomeranja cifara
    for k in list(glyphs):
        if len(k) > 1 and k.startswith("colon") and k + "_blank" not in glyphs:
            glyphs[k + "_blank"] = [0] * len(glyphs[k])
    return glyphs, aliases


def generate():
    fonts = []
    ext_codes = {}  # unicode znak -> kod
    alias_map = {}  # unicode znak -> ciljni znak
    for fname, cname, title in FONTS:
        glyphs, aliases = parse_font(os.path.join(FONT_DIR, fname))
        for key in glyphs:
            if (len(key) > 1 or not (0x20 <= ord(key) <= 0x7E)) and key != REPLACEMENT_KEY and key not in ext_codes:
                ext_codes[key] = FIRST_EXT_CODE + len(ext_codes)
        for src, dst in aliases:
            prev = alias_map.get(src)
            if prev is not None and prev != dst:
                raise SystemExit("gen_fonts: konfliktni alias za %r" % src)
            alias_map[src] = dst
        fonts.append((cname, title, glyphs))

    ext_codes[REPLACEMENT_KEY] = FIRST_EXT_CODE + len(ext_codes)
    last_code = max(ext_codes.values())
    if last_code > 0xFF:
        raise SystemExit("gen_fonts: previše znakova (%d)" % last_code)

    def code_of(key):
        if len(key) == 1 and 0x20 <= ord(key) <= 0x7E:
            return ord(key)
        return ext_codes[key]

    out = []
    out.append("// GENERISANO: tools/gen_fonts.py iz tools/fonts/*.txt — ne menjati ručno\n")
    out.append("#pragma once\n#include <Arduino.h>\n\n")
    out.append("#define FONT_COUNT %d\n" % len(fonts))
    out.append("#define GLYPH_REPLACEMENT 0x%02X\n" % ext_codes[REPLACEMENT_KEY])
    if "°" in ext_codes:
        out.append("#define GLYPH_DEGREE 0x%02X\n" % ext_codes["°"])
    for k, code in ext_codes.items():
        if len(k) > 1 and k != REPLACEMENT_KEY:
            out.append("#define GLYPH_%s 0x%02X\n" % (k.upper(), code))
    out.append("\n")

    sizes = []
    for cname, title, glyphs in fonts:
        repl = glyphs.get(REPLACEMENT_KEY, [])
        data = [ord("F"), 1, 0x20, last_code, 8]
        comments = []
        for code in range(0x20, last_code + 1):
            key = chr(code) if code < 0x80 else next((k for k, v in ext_codes.items() if v == code), None)
            if key in glyphs:
                cols, label = glyphs[key], key
            else:
                cols, label = repl, "(zamena za %s)" % key
            data.append(len(cols))
            data.extend(cols)
            comments.append((code, label, cols))
        sizes.append((cname, len(data)))
        out.append("// %s\n" % title)
        out.append("const uint8_t %s[] PROGMEM = {\n  'F', 1, 0x20, 0x%02X, 8,\n" % (cname, last_code))
        for code, label, cols in comments:
            vals = ", ".join(["%d" % len(cols)] + ["0x%02X" % c for c in cols])
            out.append("  %s,  // 0x%02X %s\n" % (vals, code, label.replace("\\", "backslash")))
        out.append("};\n\n")

    out.append("const uint8_t* const FONT_TABLE[FONT_COUNT] = { %s };\n" % ", ".join(c for c, _, _ in fonts))
    out.append("const char* const FONT_NAMES[FONT_COUNT] = { %s };\n\n" % ", ".join('"%s"' % t for _, t, _ in fonts))

    # Unicode -> kod (sortirano po kodnoj tački, za binarnu pretragu)
    umap = {}
    for ch, code in ext_codes.items():
        if len(ch) == 1:  # imenovani glifovi nemaju Unicode znak
            umap[ord(ch)] = code
    for src, dst in alias_map.items():
        umap[ord(src)] = code_of(dst)
    out.append("struct UniGlyph { uint16_t cp; uint8_t code; };\n")
    out.append("const UniGlyph UNI_MAP[] PROGMEM = {\n")
    for cp in sorted(umap):
        out.append("  {0x%04X, 0x%02X},  // %s\n" % (cp, umap[cp], chr(cp)))
    out.append("};\nconst uint16_t UNI_MAP_LEN = %d;\n" % len(umap))

    text = "".join(out)
    os.makedirs(os.path.dirname(OUT_FILE), exist_ok=True)
    old = None
    if os.path.exists(OUT_FILE):
        with open(OUT_FILE, encoding="utf-8") as f:
            old = f.read()
    if old != text:
        with open(OUT_FILE, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("gen_fonts: %s (%s)" % (OUT_FILE, ", ".join("%s=%dB" % s for s in sizes)))


generate()
