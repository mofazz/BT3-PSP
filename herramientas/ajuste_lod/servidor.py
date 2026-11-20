"""servidor.py - the LOD tuner of the native maps: a local web page (http://127.0.0.1:8765) to tune each stage's
map by hand and see the result before choosing it.

    python herramientas/ajuste_lod/servidor.py        (Windows Python; the same environment as herramientas/config.sh)

Environment: BT3_WORK (stage dumps and exports: volcar_escenarios.sh / generar_mapas.sh), BT3_GAME (the game folder:
maps/ is written there), BLENDER, MO_VENV, MO_LIB, BT3_PPSSPP - as in herramientas/config.sh, but Windows paths.

What it does, per stage (all on this PC; the maps come from your disc and are never shared):
  prepare   the dump's glTF export (stage_export.py) and the LOD structure in Blender (stage_lod.py nodec=1: cells,
            blocks, kinds, the stage's rules, buildings as boxes) - needed again after a rule changes
  variant   stage_meshopt.py (WSL) with the per-kind settings of the page: share of triangles r and error e, near
            (LOD1) and far (LOD2); "semillas" = four variants around the current settings to compare
  apply     the chosen variant packed (stage_pack.py) into BT3_GAME/maps/stage_NN.bt3m; its settings saved in
            stage_rules.json (meshopt_args), so generar_mapas.sh makes the same map again
  rules     a piece picked in the viewer becomes a kind of its own (stage_rules.json "reglas": its box, its r/e)
Nothing is sent anywhere: the server only listens on 127.0.0.1."""
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
MAPAS = os.path.normpath(os.path.join(HERE, "..", "mapas"))
RULES = os.path.join(MAPAS, "stage_rules.json")
WORK = os.path.abspath(os.environ.get("BT3_WORK", os.path.join(HERE, "..", "..", "work_mapas")))
GAME = os.environ.get("BT3_GAME", "")
if GAME.startswith("/") and len(GAME) > 2 and GAME[2] == "/":          # (a Git Bash path: /c/x -> C:/x)
    GAME = GAME[1].upper() + ":" + GAME[2:]
BLENDER = os.environ.get("BLENDER", r"C:\Program Files\Blender Foundation\5.0\blender.exe")
PPSSPP = os.environ.get("BT3_PPSSPP", r"C:\PPSSPP\PPSSPPWindows64.exe")
MO_VENV = os.environ.get("MO_VENV", "~/mo_venv")
MO_LIB = os.environ.get("MO_LIB", "~/mo_src/libmeshopt.so")
PORT = int(os.environ.get("BT3_TUNER_PORT", "8765"))
BASE_KINDS = ["suelo", "edif", "arbol", "otros"]
# (r1, e1, r2, e2): the defaults of stage_meshopt.py
DEFAULTS = {"suelo": [0.3, 0.03, 0.02, 0.3], "edif": [0.3, 0.05, 0.1, 0.1], "arbol": [0.4, 0.04, 0.05, 0.2],
            "otros": [0.3, 0.08, 0.005, 1.0]}
LOD_ARGS = "max_tris=400 min_cell=250 grp_tris=1500 g1f=0 g1min=60 box_h=150 nodec=1 classes=1"
SEEDS = [0.5, 0.75, 1.0, 1.5]                         # "semillas": the current settings times these

JOBS, JLOCK = {}, threading.Lock()


def wsl_path(p):
    return subprocess.run(["wsl", "wslpath", "-a", p.replace("\\", "/")], capture_output=True, text=True).stdout.strip()


def sdir(n):
    return os.path.join(WORK, "ajuste", "s%02d" % n)


def rules_all():
    return json.load(open(RULES, encoding="utf-8")) if os.path.exists(RULES) else {}


def rules_stage(n):
    d = rules_all().get(str(n), {})
    return d if isinstance(d, dict) else {"reglas": d}


def save_rules_stage(n, d):
    a = rules_all()
    a[str(n)] = d
    txt = json.dumps(a, indent=1, ensure_ascii=False)
    txt = re.sub(r"\[\s*([^\[\]{}]*?)\s*\]", lambda m: "[" + re.sub(r"\s*\n\s*", " ", m.group(1)) + "]", txt)   # lists inline
    txt = re.sub(r"\{\s*(\"nombre\"[^{}]*?)\s*\}", lambda m: "{" + re.sub(r"\s*\n\s*", " ", m.group(1)) + "}", txt)  # rules
    open(RULES, "w", encoding="utf-8", newline="\n").write(txt + "\n")


def parse_args(s):
    return dict(x.split("=", 1) for x in (s or "").split() if "=" in x)


def current_params(n):
    """{kind: [r1, e1, r2, e2]} - the defaults, the stage's rules' own values, then its saved meshopt_args"""
    d = rules_stage(n)
    p = {k: list(v) for k, v in DEFAULTS.items()}
    for rl in d.get("reglas", []):
        p[rl["nombre"]] = list(rl["lod"])
    for k, v in parse_args(d.get("meshopt_args")).items():
        m = re.match(r"([a-z]+)_([re])([12])$", k)
        if m and m.group(1) in p:
            p[m.group(1)][{"r1": 0, "e1": 1, "r2": 2, "e2": 3}[m.group(2) + m.group(3)]] = float(v)
    return p


def params_args(p):
    return " ".join("%s_r1=%g %s_e1=%g %s_r2=%g %s_e2=%g" % (k, v[0], k, v[1], k, v[2], k, v[3]) for k, v in p.items())


def job(fn, *a):
    jid = "%d" % (time.time() * 1000)
    with JLOCK:
        JOBS[jid] = {"state": "run", "log": "", "result": None}

    def run():
        try:
            JOBS[jid]["result"] = fn(JOBS[jid], *a)
            JOBS[jid]["state"] = "ok"
        except Exception as ex:                      # (shown on the page)
            JOBS[jid]["log"] += "\nERROR: %s" % ex
            JOBS[jid]["state"] = "error"
    threading.Thread(target=run, daemon=True).start()
    return jid


def sh(j, cmd, **kw):
    j["log"] += "\n$ " + (cmd if isinstance(cmd, str) else " ".join(cmd))[:300]
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    tail = (r.stdout + r.stderr).strip().splitlines()[-6:]
    j["log"] += "\n" + "\n".join(tail)
    if r.returncode:
        raise RuntimeError("exit code %d" % r.returncode)
    return r.stdout


# ---------------------------------------------------------------- the steps
def do_prepare(j, n):
    nn = "%02d" % n
    exp = os.path.join(WORK, "export", "s" + nn)
    if not os.path.exists(os.path.join(exp, "s%s.gltf" % nn)):
        dump = os.path.join(WORK, "stage_dumps", "stage_%s.bin" % nn)
        if not os.path.exists(dump):
            raise RuntimeError("no dump for stage %s: volcar_escenarios.sh %d first" % (nn, n))
        sh(j, [sys.executable, os.path.join(MAPAS, "stage_export.py"), dump, exp, "--name", "s" + nn])
    lod = os.path.join(sdir(n), "estructura")
    d = rules_stage(n)
    sh(j, [BLENDER, "-b", "--factory-startup", "--python", os.path.join(MAPAS, "stage_lod.py"), "--",
           os.path.join(exp, "s%s.gltf" % nn), os.path.join(lod, "estructura.gltf")] + LOD_ARGS.split() +
       ["rules=" + RULES, "stage=%d" % n] + (d.get("lod_args") or "").split())
    for v in os.listdir(sdir(n)):                      # (variants of an older structure: no longer valid)
        if v.startswith("v") and os.path.exists(os.path.join(sdir(n), v, "info.json")):
            info = json.load(open(os.path.join(sdir(n), v, "info.json")))
            info["vieja"] = True
            json.dump(info, open(os.path.join(sdir(n), v, "info.json"), "w"))
    return {"ok": True}


def do_variant(j, n, p, label):
    if not os.path.exists(os.path.join(sdir(n), "estructura", "estructura.gltf")):
        do_prepare(j, n)
    vs = [v for v in os.listdir(sdir(n)) if re.match(r"v\d+$", v)]
    vid = "v%03d" % (max([int(v[1:]) for v in vs] or [0]) + 1)
    vd = os.path.join(sdir(n), vid)
    os.makedirs(vd, exist_ok=True)
    W = wsl_path(sdir(n))
    out = sh(j, ["wsl", "bash", "-lc", "cd '%s' && %s/bin/python '%s' estructura/estructura.gltf %s/mapa.gltf lib=%s "
                 "rules='%s' stage=%d %s" % (W, MO_VENV, wsl_path(os.path.join(MAPAS, "stage_meshopt.py")), vid,
                                             MO_LIB, wsl_path(RULES), n, params_args(p))])
    tris = {}
    for m in re.finditer(r"LOD([12]) ([a-z]+) (\d+) -> (\d+)", out):
        tris.setdefault(m.group(2), [0, 0])[int(m.group(1)) - 1] = int(m.group(4))
    info = {"id": vid, "label": label, "params": p, "tris": tris, "t": time.time(),
            "lod1": sum(v[0] for v in tris.values()), "lod2": sum(v[1] for v in tris.values())}
    json.dump(info, open(os.path.join(vd, "info.json"), "w"), indent=1)
    return info


def do_seeds(j, n, p):
    out = []
    for k, f in enumerate(SEEDS):
        q = {kd: [min(1.0, v[0] * f), min(1.0, v[1] * f), min(1.0, v[2] * f), min(1.0, v[3] * f)] for kd, v in p.items()}
        out.append(do_variant(j, n, q, "semilla %d (x%g)" % (k + 1, f)))
    return out


def do_apply(j, n, vid):
    vd = os.path.join(sdir(n), vid)
    info = json.load(open(os.path.join(vd, "info.json")))
    pack = os.path.join(sdir(n), "stage_%02d.bt3m" % n)
    sh(j, [sys.executable, os.path.join(MAPAS, "stage_pack.py"), os.path.join(vd, "mapa.gltf"), pack, "--stage", str(n)])
    if GAME:
        os.makedirs(os.path.join(GAME, "maps"), exist_ok=True)
        with open(pack, "rb") as a, open(os.path.join(GAME, "maps", "stage_%02d.bt3m" % n), "wb") as b:
            b.write(a.read())
    d = rules_stage(n)
    base = {k: list(v) for k, v in DEFAULTS.items()}
    for rl in d.get("reglas", []):
        base[rl["nombre"]] = list(rl["lod"])
    names = ("r1", "e1", "r2", "e2")                   # (only the values that differ from the defaults / the rules)
    d["meshopt_args"] = " ".join("%s_%s=%g" % (k, names[i], x) for k, v in info["params"].items() for i, x in enumerate(v)
                                 if k not in base or abs(base[k][i] - x) > 1e-9)
    d["elegida"] = vid
    save_rules_stage(n, d)
    j["log"] += "\nmapa: %s%s" % (pack, (" -> " + GAME + "/maps") if GAME else " (BT3_GAME no definido: no copiado)")
    return {"ok": True}


def do_ppsspp(j, n):
    if not GAME:
        raise RuntimeError("BT3_GAME no definido")
    subprocess.run(["taskkill", "/IM", "PPSSPPWindows64.exe", "/F"], capture_output=True)
    time.sleep(1)
    open(os.path.join(GAME, "ps2rc.cfg"), "w").write("mem_volatile=1\ndemo_direct=1\ndemo_stage=%d\nmap_native=1\n" % n)
    subprocess.Popen([PPSSPP, os.path.join(GAME, "EBOOT.PBP")])
    return {"ok": True}


def stages():
    out = []
    names = set()
    for f in os.listdir(os.path.join(WORK, "stage_dumps")) if os.path.isdir(os.path.join(WORK, "stage_dumps")) else []:
        m = re.match(r"stage_(\d+)\.bin$", f)
        if m:
            names.add(int(m.group(1)))
    for f in os.listdir(os.path.join(WORK, "export")) if os.path.isdir(os.path.join(WORK, "export")) else []:
        m = re.match(r"s(\d+)$", f)
        if m:
            names.add(int(m.group(1)))
    for n in sorted(names):
        vs = []
        if os.path.isdir(sdir(n)):
            for v in sorted(os.listdir(sdir(n))):
                f = os.path.join(sdir(n), v, "info.json")
                if os.path.exists(f):
                    vs.append(json.load(open(f)))
        d = rules_stage(n)
        out.append({"stage": n, "estructura": os.path.exists(os.path.join(sdir(n), "estructura", "estructura.gltf")),
                    "variantes": vs, "elegida": d.get("elegida"), "params": current_params(n),
                    "reglas": d.get("reglas", []), "lod_args": d.get("lod_args", "")})
    return out


# ---------------------------------------------------------------- HTTP
class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        b = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path in ("/", "/index.html"):
            return self.send(200, open(os.path.join(HERE, "index.html"), "rb").read(), "text/html; charset=utf-8")
        if u.path == "/api/stages":
            return self.send(200, stages())
        if u.path.startswith("/api/job/"):
            return self.send(200, JOBS.get(u.path[9:], {"state": "none"}))
        if u.path.startswith("/files/"):                # files under BT3_WORK only
            p = os.path.normpath(os.path.join(WORK, urllib.parse.unquote(u.path[7:])))
            if not p.startswith(os.path.normpath(WORK)) or not os.path.isfile(p):
                return self.send(404, {"error": "no"})
            ext = os.path.splitext(p)[1].lower()
            ct = {".gltf": "model/gltf+json", ".bin": "application/octet-stream", ".png": "image/png"}.get(ext, "application/octet-stream")
            return self.send(200, open(p, "rb").read(), ct)
        self.send(404, {"error": "no"})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0)) or 0) or b"{}")
        n = int(q.get("stage", 0))
        if u.path == "/api/prepare":
            return self.send(200, {"job": job(do_prepare, n)})
        if u.path == "/api/variant":
            return self.send(200, {"job": job(do_variant, n, q["params"], q.get("label") or "manual")})
        if u.path == "/api/seeds":
            return self.send(200, {"job": job(do_seeds, n, q["params"])})
        if u.path == "/api/apply":
            return self.send(200, {"job": job(do_apply, n, q["variant"])})
        if u.path == "/api/ppsspp":
            return self.send(200, {"job": job(do_ppsspp, n)})
        if u.path == "/api/rule":                     # add / replace (same name) / delete a rule
            d = rules_stage(n)
            rs = [r for r in d.get("reglas", []) if r["nombre"] != q["rule"]["nombre"]]
            if not q.get("delete"):
                rs.insert(0, q["rule"])
            d["reglas"] = rs
            save_rules_stage(n, d)
            return self.send(200, {"ok": True})
        self.send(404, {"error": "no"})


if __name__ == "__main__":
    os.makedirs(os.path.join(WORK, "ajuste"), exist_ok=True)
    print("LOD tuner: http://127.0.0.1:%d  (work %s, game %s)" % (PORT, WORK, GAME or "-"))
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
