"""
Demo server za web UI bez uređaja: služi web/index.html i odgovara na /api/* izmišljenim
podacima (bez stvarnih mreža, adresa i naloga). Pregled fonta se crta istim fontovima kao
na uređaju (tools/fonts/*.txt).

    python tools/mock_server.py [port]      ->  http://127.0.0.1:8080/  (#prikaz, #mqtt, ...)

Koristi se za razvoj UI-ja i za slike u docs/screenshots/. Izmene se ne čuvaju.
"""
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
START = time.time()

# ---------------------------------------------------------------- fontovi (deli parser sa gen_fonts.py)
_ns = {"__file__": os.path.join(ROOT, "tools", "gen_fonts.py"), "__name__": "gen_fonts_lib"}
_src = open(_ns["__file__"], encoding="utf-8").read().replace("\ngenerate()\n", "\n")
exec(compile(_src, _ns["__file__"], "exec"), _ns)
FONTS = []
for fname, _, title in _ns["FONTS"]:
    glyphs, aliases = _ns["parse_font"](os.path.join(ROOT, "tools", "fonts", fname))
    FONTS.append((title, glyphs, dict(aliases)))


def render(font, text):
    _, glyphs, aliases = FONTS[font]
    cols = []
    for ch in text:
        g = glyphs.get(ch) or glyphs.get(aliases.get(ch, "")) or glyphs.get("?box", [])
        if not g:
            continue
        if cols:
            cols.append(0)
        cols.extend(g)
    return cols[:400]


# ---------------------------------------------------------------- demo podaci
CONFIG = {
    "net": {"hostname": "matrixclock-a1b2", "ssid": "MojaMreza", "portalFails": 5, "passSet": True,
            "adminPassSet": True},
    "time": {"tzName": "Europe/Belgrade", "tz": "CET-1CEST,M3.5.0,M10.5.0/3",
             "ntp": ["pool.ntp.org", "time.google.com", "time.cloudflare.com"],
             "h24": True, "seconds": False, "anim": 1, "animH": True, "animM": True, "animS": True, "blink": True, "lead0": True, "dateFmt": 0, "dot": 0, "dateNarrow": False, "cyr": False},
    "disp": {"hw": 1, "modules": 4, "timeFont": 1, "colon": 1, "bDay": 6, "bNight": 1, "bMax": 10, "night": True,
             "nStart": 1320, "nEnd": 420, "nMode": 0, "speed": 40, "pause": 1000, "dir": 0, "tSec": 15,
             "iSec": 4, "order": [0, 1, 2, 4, 5, 3], "pages": [True, True, True, True, True, True]},
    "msgs": [{"on": True, "t": "Srećan rođendan, Ana!"},
             {"on": True, "t": "Ђурђевдан — 6. мај"},
             {"on": False, "t": "Ne zaboravi ključeve"},
             {"on": False, "t": ""}, {"on": False, "t": ""}, {"on": False, "t": ""}],
    "mqtt": {"on": True, "host": "192.168.1.10", "port": 1883, "user": "ha_user", "passSet": True,
             "base": "matrix_clock_a1b2", "disc": True, "discPrefix": "homeassistant", "tele": 120},
    "owm": {"on": True, "keySet": True, "city": "Beograd,RS", "interval": 15, "label": "Napolju",
            "showT": True, "showF": False, "showH": True, "showP": False, "showW": False, "showD": True},
    "sens": {"type": 2, "dhtPin": 16, "interval": 30, "tOff": -0.5, "hOff": 0, "showT": True, "showH": True,
             "showP": True},
}

META = {"fonts": [f[0] for f in FONTS], "msgMax": 150, "msgCount": 6, "modMin": 4, "modMax": 16}

NETS = [{"ssid": "MojaMreza", "rssi": -52, "open": False}, {"ssid": "Komšija_5G", "rssi": -78, "open": False},
        {"ssid": "Kafić gost", "rssi": -84, "open": True}]


def status():
    lt = time.localtime()
    up = int(time.time() - START) + 3 * 86400 + 5 * 3600 + 12 * 60
    return {
        "fw": "3.1.0", "build": "Oct 17 2026 16:30:23",
        "time": time.strftime("%H:%M:%S", lt), "date": time.strftime("%d.%m.%Y.", lt),
        "ntp": "sinhronizovano", "ntpOk": True, "ntpAge": 840, "ntpCount": 76,
        "wifi": {"state": "povezan", "ok": True, "ssid": "MojaMreza", "ip": "192.168.1.50", "rssi": -52, "ch": 6,
                 "host": "matrixclock-a1b2", "fails": 0, "disc": 1, "mac": "AA:BB:CC:A1:B2:C3"},
        "portal": {"on": False, "ssid": "MatrixClock-A1B2"},
        "mqtt": {"on": True, "state": "povezan", "ok": True, "err": "", "connects": 2, "fails": 0, "age": 41000,
                 "base": "matrix_clock_a1b2"},
        "sens": {"type": 2, "name": "BME280/BMP280", "model": "BME280", "state": "radi", "age": 12, "reads": 9312,
                 "errors": 0, "t": 22.4, "h": 46.3, "p": 1013.8},
        "owm": {"on": True, "state": "radi", "err": "", "place": "Belgrade", "age": 420, "ok": 31, "errors": 0, "req": 31,
                "t": 14.6, "feels": 13.9, "hum": 72, "press": 1016,
                "wind": 3.1, "desc": "slaba kiša"},
        "disp": {"item": "vreme", "mode": "normal", "bright": 6, "on": True, "hw": 1, "modules": 4},
        "sys": {"heap": 33480, "heapMin": 29120, "heapBoot": 39376, "maxBlock": 31200, "frag": 6, "uptime": up,
                "loopMax": 48, "reset": "Power On", "cfgErr": "", "dirty": False, "restart": False,
                "chip": "a1b2c3", "flash": 4194304, "sketch": 512128, "sketchFree": 2633728,
                "otaMax": 2629632, "otaNet": True, "otaRssi": -80, "fsUsed": 24576, "fsTotal": 1024000},
    }


class Handler(BaseHTTPRequestHandler):
    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == "/":
            body = open(os.path.join(ROOT, "web", "index.html"), "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif u.path == "/frame":
            # stranica u okviru zadate širine (npr. telefon 390 px); headless prozor ne može biti uži od ~500 px
            w = int(q.get("w", ["390"])[0])
            tab = q.get("tab", ["pregled"])[0]
            html = ('<!doctype html><meta charset="utf-8"><body style="margin:0;background:#f4f5f7">'
                    '<iframe src="/#%s" style="border:0;width:%dpx;height:2400px;display:block"></iframe>' % (tab, w))
            body = html.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif u.path == "/api/status":
            self._json(status())
        elif u.path in ("/api/config", "/api/defaults"):
            self._json(CONFIG)
        elif u.path == "/api/meta":
            self._json(META)
        elif u.path == "/api/scan":
            self._json({"scanning": False, "nets": NETS})
        elif u.path == "/api/render":
            font = int(q.get("font", ["0"])[0])
            self._json({"cols": render(font, q.get("text", [""])[0])})
        else:
            self.send_error(404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(n)
        if self.path.startswith("/api/config"):
            self._json({"ok": True, "saved": "save=1" in self.path, "restart": False, "restarting": False})
        else:
            self._json({"ok": True})

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print("Demo web UI: http://127.0.0.1:%d/" % port)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
