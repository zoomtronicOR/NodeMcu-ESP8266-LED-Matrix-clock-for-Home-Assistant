"""
Pravi slike web UI-ja za README (docs/screenshots/*.png) pomoću headless Edge/Chrome-a i
demo servera (tools/mock_server.py — izmišljeni podaci, bez stvarnih mreža i naloga).

    python tools/screenshots.py

Potrebni su Microsoft Edge ili Google Chrome i Pillow (pip install pillow).
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "screenshots")
PORT = 8765

sys.path.insert(0, os.path.join(ROOT, "tools"))
import mock_server  # noqa: E402

BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    "msedge", "google-chrome", "chromium", "chrome",
]

# (fajl, kartica, širina, tamna tema)
SHOTS = [
    ("pregled.png", "pregled", 1100, False),
    ("prikaz.png", "prikaz", 1100, False),
    ("poruke.png", "poruke", 1100, False),
    ("vreme.png", "vreme", 1100, False),
    ("mreza.png", "mreza", 1100, False),
    ("senzori.png", "senzori", 1100, False),
    ("mqtt.png", "mqtt", 1100, False),
    ("sistem.png", "sistem", 1100, False),
    ("pregled-tamno.png", "pregled", 1100, True),
    ("telefon.png", "pregled", 390, False),
]


def find_browser():
    for b in BROWSERS:
        p = b if os.path.isabs(b) else shutil.which(b)
        if p and os.path.exists(p):
            return p
    raise SystemExit("Nije pronađen Edge ni Chrome")


def crop_to_content(path, width, margin=24):
    # odseca prazan donji deo (boja pozadine stranice), ostavlja malu marginu
    im = Image.open(path).convert("RGB")
    im = im.crop((0, 0, min(width, im.size[0]), im.size[1]))
    w, h = im.size
    bg = im.getpixel((2, h - 2))
    px = im.load()
    bottom = h - 1
    while bottom > 0 and all(px[x, bottom] == bg for x in range(0, w, 4)):
        bottom -= 1
    im.crop((0, 0, w, min(h, bottom + margin))).save(path, optimize=True)


def main():
    browser = find_browser()
    os.makedirs(OUT, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", PORT), mock_server.Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="mc-shots-")
    try:
        for name, tab, width, dark in SHOTS:
            path = os.path.join(OUT, name)
            # uži od ~500 px headless prozor ne može: telefon se snima kroz iframe tačne širine
            narrow = width < 600
            url = ("http://127.0.0.1:%d/frame?w=%d&tab=%s" % (PORT, width, tab) if narrow
                   else "http://127.0.0.1:%d/#%s" % (PORT, tab))
            args = [browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                    "--user-data-dir=" + profile, "--force-device-scale-factor=1",
                    "--window-size=%d,2600" % (800 if narrow else width), "--virtual-time-budget=6000",
                    "--screenshot=" + path, url]
            # tema eksplicitno (inače headless prati temu sistema): 0 = tamna, 1 = svetla
            args.insert(1, "--blink-settings=preferredColorScheme=%d" % (0 if dark else 1))
            subprocess.run(args, check=True, timeout=60, capture_output=True)
            crop_to_content(path, width)
            print("%-20s %s" % (name, Image.open(path).size))
    finally:
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    main()
