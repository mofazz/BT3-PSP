"""ajustador.py - the LOD tuner of the native maps as a desktop window (PySide6 + OpenGL): tune each stage's map by
kind and by piece, compare variants in a 3D view and choose the one the game uses.

    python herramientas/ajuste_lod/ajustador.py            (pip install PySide6 PyOpenGL numpy pillow)

The steps (prepare / variant / seeds / choose / rules / PPSSPP) are those of servidor.py, imported from it, with the
same environment: BT3_WORK, BT3_GAME, BLENDER, MO_VENV, MO_LIB, BT3_PPSSPP (see herramientas/config.sh; abrir.sh
sets them). Everything stays on this PC: the maps come from your disc and are never shared.

View: left drag = orbit, right drag (or middle) = pan, wheel = zoom, F = frame the stage, click (in "Original") =
pick a piece."""
import ctypes
import json
import math
import os
import re
import sys
import threading

import numpy as np
from PIL import Image
from PySide6.QtCore import QObject, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QSurfaceFormat
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtWidgets import (QAbstractItemView, QApplication, QCheckBox, QComboBox, QDoubleSpinBox, QFormLayout,
                               QGroupBox, QHBoxLayout, QHeaderView, QLabel, QLineEdit, QListWidget, QListWidgetItem,
                               QMainWindow, QMessageBox, QPlainTextEdit, QPushButton, QScrollArea, QSplitter,
                               QTableWidget, QVBoxLayout, QWidget)
from OpenGL import GL as gl

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "mapas"))
import servidor as S                                   # noqa: E402  (the steps and the paths)
import stage_pack as SP                                # noqa: E402  (glTF reading)

KCOL = {"suelo": (90, 160, 90), "edif": (224, 138, 60), "arbol": (31, 107, 58), "otros": (154, 106, 214),
        "cajas": (217, 72, 72), "fondo": (59, 111, 182), "dispersos": (136, 136, 136), "todo": (200, 200, 200),
        "original": (200, 200, 200)}
BASE = {"suelo", "edif", "arbol", "otros", "todo", "cajas", "fondo", "dispersos", "original"}


def kcol(k):
    if k in KCOL:
        return KCOL[k]
    h = 0
    for c in k:
        h = (h * 31 + ord(c)) & 0xFFFFFFFF
    q = QColor.fromHslF((h % 360) / 360.0, 0.65, 0.55)
    return (q.red(), q.green(), q.blue())


def kind_of(name):
    """(level, kind) of a glTF node by its name: <tag>_LOD0/1/2[_kind], fondo, dispersos, cajas"""
    if name.startswith("fondo"):
        return 0, "fondo"
    if name.startswith("dispersos"):
        return 0, "dispersos"
    m = re.search(r"_LOD([012])(?:_([a-z]+))?$", name)
    if m:
        return int(m.group(1)), m.group(2) or "todo"
    return -1, "original"


# ---------------------------------------------------------------- a glTF model as draw batches
class Model:
    """every primitive of the file as a batch of world-space triangles (positions, uv, colour), its texture, the kind
    and level of its node, the block it belongs to (hierarchical LOD) - ready for client-array drawing"""

    def __init__(self, path):
        g, bufs, base = SP.load(path)
        W = SP.node_mats(g)
        self.base = base
        parent = {}
        for i, nd in enumerate(g["nodes"]):
            for c in nd.get("children", []):
                parent[c] = i

        def up(i):
            while i is not None:
                yield i
                i = parent.get(i)
        self.images = {}                               # uri -> PIL image (uploaded on first draw)
        self.batches = []
        self.blocks = {}                               # block tag -> {"d1", "mn", "mx"}
        for i, nd in enumerate(g["nodes"]):
            if nd.get("name", "").startswith("GRP_"):
                self.blocks[nd["name"][4:]] = {"d1": float((nd.get("extras") or {}).get("w3d_lod_distancias", [60])[0]),
                                               "mn": np.full(3, 1e30), "mx": np.full(3, -1e30)}
        for i, nd in enumerate(g["nodes"]):
            if "mesh" not in nd:
                continue
            lv, kd = kind_of(nd.get("name", ""))
            blk = cell_blk = None
            for a in up(i):
                n = g["nodes"][a].get("name", "")
                if n.startswith("GRP_"):
                    blk = n[4:]
                ex = g["nodes"][a].get("extras") or {}
                if ex.get("w3d_grupo"):
                    cell_blk = ex["w3d_grupo"]
            M = W[i]
            for p in g["meshes"][nd["mesh"]]["primitives"]:
                at = p["attributes"]
                pos = SP.accessor(g, bufs, at["POSITION"]).astype(np.float64)
                pos = (M @ np.c_[pos, np.ones(len(pos))].T).T[:, :3].astype(np.float32)
                n = len(pos)
                uv = SP.accessor(g, bufs, at["TEXCOORD_0"]).astype(np.float32) if "TEXCOORD_0" in at else np.zeros((n, 2), np.float32)
                col = SP.accessor(g, bufs, at["COLOR_0"]).astype(np.float32) if "COLOR_0" in at else np.ones((n, 4), np.float32)
                if col.shape[1] == 3:
                    col = np.c_[col, np.ones(n, np.float32)]
                idx = SP.accessor(g, bufs, p["indices"]).reshape(-1) if "indices" in p else np.arange(n)
                idx = idx[: len(idx) // 3 * 3]
                if not len(idx):
                    continue
                mat = g["materials"][p.get("material", 0)] if g.get("materials") else {}
                mname = mat.get("name", "m_op").split(".")[0]
                tex = None
                bt = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture")
                if bt is not None:
                    tex = g["images"][g["textures"][bt["index"]]["source"]].get("uri")
                P, UV, C = pos[idx], uv[idx], np.clip(col[idx], 0, 1)
                nrm = np.cross(P[1::3] - P[0::3], P[2::3] - P[0::3])
                nrm /= np.linalg.norm(nrm, axis=1, keepdims=True) + 1e-9
                shade = np.repeat(0.55 + 0.45 * np.abs(nrm @ np.array([0.35, 0.85, 0.4], np.float32)), 3).astype(np.float32)
                b = {"P": np.ascontiguousarray(P), "UV": np.ascontiguousarray(UV), "C": np.ascontiguousarray(C),
                     "shade": shade, "tex": tex, "alpha": mname[-2:], "lv": lv, "kind": kd, "blk": blk,
                     "cell_blk": cell_blk, "flat": None}
                self.batches.append(b)
                if blk and blk in self.blocks:
                    bb = self.blocks[blk]
                    bb["mn"] = np.minimum(bb["mn"], P.min(0))
                    bb["mx"] = np.maximum(bb["mx"], P.max(0))
                if tex and tex not in self.images:
                    try:
                        self.images[tex] = Image.open(os.path.join(base, tex)).convert("RGBA")
                    except OSError:
                        self.images[tex] = None
        allp = np.concatenate([b["P"] for b in self.batches]) if self.batches else np.zeros((1, 3), np.float32)
        self.mn, self.mx = allp.min(0), allp.max(0)
        self.gltex = {}

    def flat(self, b):
        if b["flat"] is None:
            c = np.array(kcol(b["kind"] if b["kind"] != "todo" else "original"), np.float32) / 255.0
            f = np.ones((len(b["P"]), 4), np.float32)
            f[:, :3] = c[None, :] * b["shade"][:, None]
            b["flat"] = f
        return b["flat"]


def build_islands(model):
    """loose parts of the original stage as stage_lod.py classifies them: triangles joined by a shared vertex place
    (0.1 units); returns (per-triangle part id, triangles (n,3,3) in view coordinates)"""
    tris = np.concatenate([b["P"].reshape(-1, 3, 3) for b in model.batches])
    keys = np.round(tris.reshape(-1, 3), 1)
    _, vid = np.unique(keys, axis=0, return_inverse=True)
    vid = vid.reshape(-1, 3)
    par = np.arange(len(tris))
    first = {}

    def find(i):
        while par[i] != i:
            par[i] = par[par[i]]
            i = par[i]
        return i
    for t in range(len(tris)):
        for v in vid[t]:
            j = first.setdefault(int(v), t)
            if j != t:
                a, b = find(j), find(t)
                if a != b:
                    par[a] = b
    root = np.array([find(i) for i in range(len(tris))])
    return root, tris


def ray_pick(tris, o, d):
    """index of the nearest triangle hit by the ray o + t d (Moller-Trumbore, all at once), or -1"""
    v0, v1, v2 = tris[:, 0], tris[:, 1], tris[:, 2]
    e1, e2 = v1 - v0, v2 - v0
    p = np.cross(d, e2)
    det = (e1 * p).sum(1)
    ok = np.abs(det) > 1e-9
    inv = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
    s = o - v0
    u = (s * p).sum(1) * inv
    q = np.cross(s, e1)
    v = (q * d).sum(1) * inv
    t = (q * e2).sum(1) * inv
    hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0)
    if not hit.any():
        return -1
    t = np.where(hit, t, np.inf)
    return int(t.argmin())


# ---------------------------------------------------------------- the 3D view
class View(QOpenGLWidget):
    picked = Signal(object)
    stats = Signal(str)

    def __init__(self):
        super().__init__()
        self.orig = None
        self.var = None
        self.mode = "orig"
        self.colour = False
        self.wire = False
        self.isl = None
        self.hl = None
        self.yaw, self.pitch, self.dist = 45.0, 30.0, 1500.0
        self.target = np.zeros(3)
        self.last = None
        self.down = None
        self.setMouseTracking(False)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.update)
        self.timer.start(33)

    # camera
    def eye(self):
        y, p = math.radians(self.yaw), math.radians(self.pitch)
        return self.target + self.dist * np.array([math.cos(p) * math.sin(y), math.sin(p), math.cos(p) * math.cos(y)])

    def frame(self, m):
        if m is None:
            return
        c = (m.mn + m.mx) / 2
        self.target = np.array([c[0], 0.0, c[2]]) if (m.mx - m.mn).max() > 20000 else c
        self.dist = 1500.0 if (m.mx - m.mn).max() > 20000 else float((m.mx - m.mn).max()) * 0.9 + 100

    def matrices(self):
        w, h = max(1, self.width()), max(1, self.height())
        f = 1 / math.tan(math.radians(55) / 2)
        n, fa = 1.0, 200000.0
        Pm = np.array([[f * h / w, 0, 0, 0], [0, f, 0, 0], [0, 0, (fa + n) / (n - fa), 2 * fa * n / (n - fa)], [0, 0, -1, 0]])
        e = self.eye()
        z = e - self.target
        z /= np.linalg.norm(z)
        x = np.cross([0, 1, 0], z)
        x /= np.linalg.norm(x) + 1e-12
        yv = np.cross(z, x)
        V = np.eye(4)
        V[0, :3], V[1, :3], V[2, :3] = x, yv, z
        V[:3, 3] = -V[:3, :3] @ e
        return Pm, V

    # GL
    def initializeGL(self):
        gl.glClearColor(0.063, 0.071, 0.086, 1)
        gl.glEnable(gl.GL_DEPTH_TEST)
        gl.glDisable(gl.GL_CULL_FACE)

    def resizeGL(self, w, h):
        gl.glViewport(0, 0, w, h)

    def texture(self, m, uri):
        if uri in m.gltex:
            return m.gltex[uri]
        im = m.images.get(uri)
        if im is None:
            m.gltex[uri] = 0
            return 0
        t = gl.glGenTextures(1)
        gl.glBindTexture(gl.GL_TEXTURE_2D, t)
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MIN_FILTER, gl.GL_LINEAR)
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_MAG_FILTER, gl.GL_LINEAR)
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_WRAP_S, gl.GL_REPEAT)
        gl.glTexParameteri(gl.GL_TEXTURE_2D, gl.GL_TEXTURE_WRAP_T, gl.GL_REPEAT)
        gl.glTexImage2D(gl.GL_TEXTURE_2D, 0, gl.GL_RGBA, im.width, im.height, 0, gl.GL_RGBA, gl.GL_UNSIGNED_BYTE, im.tobytes())
        m.gltex[uri] = t
        return t

    def visible(self, m, b, near):
        if m is self.orig:
            return self.mode == "orig" or self.var is None
        if self.mode == "orig":
            return False
        if b["lv"] == 0 or b["lv"] < 0:
            return b["kind"] in ("fondo", "dispersos")
        if self.mode == "lod1":
            return b["lv"] == 1
        if self.mode == "lod2":
            return b["lv"] == 2
        if b["lv"] == 1:                               # the game's rule: a block near the camera -> its cells' LOD1
            return bool(near.get(b["cell_blk"]))
        return not near.get(b["blk"])

    def paintGL(self):
        gl.glClear(gl.GL_COLOR_BUFFER_BIT | gl.GL_DEPTH_BUFFER_BIT)
        Pm, V = self.matrices()
        gl.glMatrixMode(gl.GL_PROJECTION)
        gl.glLoadMatrixf(Pm.T.astype(np.float32))
        gl.glMatrixMode(gl.GL_MODELVIEW)
        gl.glLoadMatrixf(V.T.astype(np.float32))
        gl.glPolygonMode(gl.GL_FRONT_AND_BACK, gl.GL_LINE if self.wire else gl.GL_FILL)
        e = self.eye()
        near = {}
        if self.var is not None and self.mode == "game":
            for k, bb in self.var.blocks.items():
                d = np.linalg.norm(np.maximum(0, np.maximum(bb["mn"] - e, e - bb["mx"])))
                near[k] = d < bb["d1"]
        ntri = 0
        gl.glEnableClientState(gl.GL_VERTEX_ARRAY)
        gl.glEnableClientState(gl.GL_COLOR_ARRAY)
        for m in (self.orig, self.var):
            if m is None:
                continue
            order = sorted(m.batches, key=lambda b: b["alpha"] == "bl")
            for b in order:
                if not self.visible(m, b, near):
                    continue
                tex = 0 if self.colour else (self.texture(m, b["tex"]) if b["tex"] else 0)
                if tex:
                    gl.glEnable(gl.GL_TEXTURE_2D)
                    gl.glBindTexture(gl.GL_TEXTURE_2D, tex)
                    gl.glEnableClientState(gl.GL_TEXTURE_COORD_ARRAY)
                    gl.glTexCoordPointer(2, gl.GL_FLOAT, 0, b["UV"])
                else:
                    gl.glDisable(gl.GL_TEXTURE_2D)
                    gl.glDisableClientState(gl.GL_TEXTURE_COORD_ARRAY)
                a = b["alpha"]
                if a == "bl" and not self.colour:
                    gl.glEnable(gl.GL_BLEND)
                    gl.glBlendFunc(gl.GL_SRC_ALPHA, gl.GL_ONE_MINUS_SRC_ALPHA)
                    gl.glDepthMask(gl.GL_FALSE)
                else:
                    gl.glDisable(gl.GL_BLEND)
                    gl.glDepthMask(gl.GL_TRUE)
                if a == "mk" and not self.colour:
                    gl.glEnable(gl.GL_ALPHA_TEST)
                    gl.glAlphaFunc(gl.GL_GREATER, 0.5)
                else:
                    gl.glDisable(gl.GL_ALPHA_TEST)
                gl.glColorPointer(4, gl.GL_FLOAT, 0, m.flat(b) if self.colour else b["C"])
                gl.glVertexPointer(3, gl.GL_FLOAT, 0, b["P"])
                gl.glDrawArrays(gl.GL_TRIANGLES, 0, len(b["P"]))
                ntri += len(b["P"]) // 3
        gl.glDisableClientState(gl.GL_TEXTURE_COORD_ARRAY)
        gl.glDisable(gl.GL_TEXTURE_2D)
        gl.glDisable(gl.GL_BLEND)
        gl.glDisable(gl.GL_ALPHA_TEST)
        gl.glDepthMask(gl.GL_TRUE)
        if self.hl is not None and self.mode == "orig":  # the picked piece, over everything
            gl.glDisable(gl.GL_DEPTH_TEST)
            gl.glEnable(gl.GL_BLEND)
            gl.glBlendFunc(gl.GL_SRC_ALPHA, gl.GL_ONE_MINUS_SRC_ALPHA)
            gl.glDisableClientState(gl.GL_COLOR_ARRAY)
            gl.glColor4f(1.0, 0.83, 0.0, 0.55)
            gl.glVertexPointer(3, gl.GL_FLOAT, 0, self.hl)
            gl.glDrawArrays(gl.GL_TRIANGLES, 0, len(self.hl))
            gl.glEnable(gl.GL_DEPTH_TEST)
            gl.glDisable(gl.GL_BLEND)
        gl.glDisableClientState(gl.GL_COLOR_ARRAY)
        gl.glDisableClientState(gl.GL_VERTEX_ARRAY)
        self.stats.emit("Triángulos visibles: %d   ·   cámara %.0f %.0f %.0f" % (ntri, e[0], e[1], e[2]))

    # mouse
    def mousePressEvent(self, ev):
        self.last = ev.position()
        self.down = ev.position()

    def mouseMoveEvent(self, ev):
        if self.last is None:
            return
        dx, dy = ev.position().x() - self.last.x(), ev.position().y() - self.last.y()
        self.last = ev.position()
        if ev.buttons() & Qt.LeftButton:
            self.yaw -= dx * 0.3
            self.pitch = max(-89.0, min(89.0, self.pitch + dy * 0.3))
        elif ev.buttons() & (Qt.RightButton | Qt.MiddleButton):
            Pm, V = self.matrices()
            s = self.dist * 0.0015
            self.target = self.target - V[0, :3] * dx * s + V[1, :3] * dy * s

    def mouseReleaseEvent(self, ev):
        if self.down is not None and ev.button() == Qt.LeftButton:
            p = ev.position()
            if abs(p.x() - self.down.x()) + abs(p.y() - self.down.y()) < 4:
                self.pick(p.x(), p.y())
        self.last = self.down = None

    def wheelEvent(self, ev):
        self.dist = max(5.0, self.dist * (0.88 if ev.angleDelta().y() > 0 else 1.0 / 0.88))

    def keyPressEvent(self, ev):
        if ev.key() == Qt.Key_F:
            self.frame(self.var if self.mode != "orig" and self.var else self.orig)

    def pick(self, x, y):
        if self.mode != "orig" or self.isl is None:
            return
        Pm, V = self.matrices()
        w, h = max(1, self.width()), max(1, self.height())
        ndc = np.array([2 * x / w - 1, 1 - 2 * y / h, -1, 1])
        inv = np.linalg.inv(Pm @ V)
        a = inv @ ndc
        b = inv @ np.array([ndc[0], ndc[1], 1, 1])
        a, b = a[:3] / a[3], b[:3] / b[3]
        d = (b - a) / np.linalg.norm(b - a)
        root, tris = self.isl
        t = ray_pick(tris.astype(np.float64), a, d)
        if t < 0:
            return
        sel = tris[root == root[t]]
        self.hl = np.ascontiguousarray(sel.reshape(-1, 3), np.float32)
        B = np.stack([sel[..., 0], -sel[..., 2], sel[..., 1]], -1).reshape(-1, 3)      # Blender: x, -z, y
        self.picked.emit({"n": len(sel), "mn": B.min(0).tolist(), "mx": B.max(0).tolist()})


# ---------------------------------------------------------------- jobs in the background
class Bus(QObject):
    done = Signal(str, object, str)                    # what, result (None = error), log


class Win(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Ajuste de LOD de los mapas nativos")
        self.resize(1500, 900)
        self.bus = Bus()
        self.bus.done.connect(self.job_done)
        self.busy = False
        self.cur = None
        self.params = {}
        self.sel_var = None
        self.view = View()
        self.view.picked.connect(self.on_pick)
        self.view.stats.connect(lambda s: self.statusBar().showMessage(s))
        side = QWidget()
        L = QVBoxLayout(side)

        top = QHBoxLayout()
        top.addWidget(QLabel("Escenario"))
        self.stage = QComboBox()
        self.stage.currentIndexChanged.connect(self.stage_changed)
        top.addWidget(self.stage, 1)
        L.addLayout(top)
        r = QHBoxLayout()
        self.b_prep = QPushButton("Preparar estructura")
        self.b_prep.setToolTip("Exporta el volcado y genera celdas, bloques, tipos y cajas en Blender (otra vez tras cambiar reglas)")
        self.b_prep.clicked.connect(lambda: self.run("Preparar estructura", S.do_prepare, self.cur["stage"]))
        self.b_ppsspp = QPushButton("Probar en PPSSPP")
        self.b_ppsspp.clicked.connect(lambda: self.run("PPSSPP", S.do_ppsspp, self.cur["stage"]))
        r.addWidget(self.b_prep)
        r.addWidget(self.b_ppsspp)
        L.addLayout(r)

        g = QGroupBox("Reducción por tipo   (r = parte de triángulos que se conserva, e = error permitido)")
        gl_ = QVBoxLayout(g)
        self.kt = QTableWidget(0, 5)
        self.kt.setHorizontalHeaderLabels(["Tipo", "r cerca", "e cerca", "r lejos", "e lejos"])
        self.kt.verticalHeader().setVisible(False)
        self.kt.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self.kt.setSelectionMode(QAbstractItemView.NoSelection)
        gl_.addWidget(self.kt)
        r = QHBoxLayout()
        self.b_var = QPushButton("Generar variante")
        self.b_var.clicked.connect(lambda: self.run("Variante", S.do_variant, self.cur["stage"], self.read_params(), "manual"))
        self.b_seeds = QPushButton("4 semillas")
        self.b_seeds.setToolTip("Cuatro variantes: los ajustes ×0,5 ×0,75 ×1 ×1,5")
        self.b_seeds.clicked.connect(lambda: self.run("4 semillas", S.do_seeds, self.cur["stage"], self.read_params()))
        self.b_reset = QPushButton("Restablecer")
        self.b_reset.clicked.connect(lambda: self.fill_kinds(self.cur["params"]))
        for b in (self.b_var, self.b_seeds, self.b_reset):
            r.addWidget(b)
        gl_.addLayout(r)
        L.addWidget(g)

        g = QGroupBox("Variantes")
        gv = QVBoxLayout(g)
        self.vl = QListWidget()
        self.vl.itemDoubleClicked.connect(lambda it: self.show_variant(it.data(Qt.UserRole)))
        gv.addWidget(self.vl)
        r = QHBoxLayout()
        self.b_see = QPushButton("Ver")
        self.b_see.clicked.connect(lambda: self.vl.currentItem() and self.show_variant(self.vl.currentItem().data(Qt.UserRole)))
        self.b_use = QPushButton("Usar sus ajustes")
        self.b_use.clicked.connect(self.use_params)
        self.b_choose = QPushButton("Elegir para el juego")
        self.b_choose.setStyleSheet("font-weight:bold")
        self.b_choose.clicked.connect(self.choose)
        for b in (self.b_see, self.b_use, self.b_choose):
            r.addWidget(b)
        gv.addLayout(r)
        L.addWidget(g)

        g = QGroupBox("Vista")
        gw = QHBoxLayout(g)
        self.mode = QComboBox()
        for t, v in (("Original (sin LOD)", "orig"), ("Cerca (LOD1)", "lod1"), ("Lejos (LOD2)", "lod2"),
                     ("Juego (según distancia)", "game")):
            self.mode.addItem(t, v)
        self.mode.currentIndexChanged.connect(self.view_changed)
        self.c_col = QCheckBox("colores por tipo")
        self.c_col.toggled.connect(self.view_changed)
        self.c_wire = QCheckBox("alambre")
        self.c_wire.toggled.connect(self.view_changed)
        gw.addWidget(self.mode, 1)
        gw.addWidget(self.c_col)
        gw.addWidget(self.c_wire)
        L.addWidget(g)
        self.legend = QLabel()
        self.legend.setWordWrap(True)
        L.addWidget(self.legend)

        g = QGroupBox("Pieza seleccionada   (clic en la vista «Original»)")
        gp = QFormLayout(g)
        self.pinfo = QLabel("Ninguna.")
        gp.addRow(self.pinfo)
        self.pname = QLineEdit()
        self.pname.setPlaceholderText("casa, torre, rocas...")
        gp.addRow("Nombre", self.pname)
        self.ponly = QCheckBox("solo esta pieza (no las pequeñas que haya dentro de su caja)")
        self.ponly.setChecked(True)
        gp.addRow(self.ponly)
        pr = QHBoxLayout()
        self.pspin = []
        for lab, v, st in (("r cerca", 0.8, 0.05), ("e cerca", 0.01, 0.005), ("r lejos", 0.4, 0.05), ("e lejos", 0.03, 0.005)):
            s = QDoubleSpinBox()
            s.setRange(0, 1)
            s.setDecimals(3)
            s.setSingleStep(st)
            s.setValue(v)
            s.setToolTip(lab)
            pr.addWidget(QLabel(lab))
            pr.addWidget(s)
            self.pspin.append(s)
        gp.addRow(pr)
        self.b_rule = QPushButton("Crear regla")
        self.b_rule.clicked.connect(self.make_rule)
        gp.addRow(self.b_rule)
        L.addWidget(g)

        g = QGroupBox("Reglas del escenario")
        gr = QVBoxLayout(g)
        self.rl = QListWidget()
        gr.addWidget(self.rl)
        self.b_unrule = QPushButton("Quitar regla")
        self.b_unrule.clicked.connect(self.del_rule)
        gr.addWidget(self.b_unrule)
        L.addWidget(g)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(2000)
        L.addWidget(QLabel("Registro"))
        L.addWidget(self.log, 1)

        sc = QScrollArea()
        sc.setWidget(side)
        sc.setWidgetResizable(True)
        sc.setMinimumWidth(470)
        split = QSplitter()
        split.addWidget(sc)
        split.addWidget(self.view)
        split.setStretchFactor(1, 1)
        self.setCentralWidget(split)
        self.buttons = [self.b_prep, self.b_ppsspp, self.b_var, self.b_seeds, self.b_reset, self.b_see, self.b_use,
                        self.b_choose, self.b_rule, self.b_unrule]
        self.pick = None
        self.say("Trabajo: %s\nJuego: %s" % (S.WORK, S.GAME or "(BT3_GAME sin definir: «Elegir» no copia el mapa)"))
        self.refresh()

    # ------------------------------------------------------------ helpers
    def say(self, s):
        self.log.appendPlainText(s)

    def refresh(self, keep=None):
        self.all = S.stages()
        n0 = keep if keep is not None else (self.cur["stage"] if self.cur else None)
        self.stage.blockSignals(True)
        self.stage.clear()
        for s in self.all:
            self.stage.addItem("%02d%s%s" % (s["stage"], "  ✓ elegida " + s["elegida"] if s["elegida"] else "",
                                             "" if s["estructura"] else "  (sin preparar)"), s["stage"])
        i = max(0, self.stage.findData(n0)) if n0 is not None else 0
        self.stage.setCurrentIndex(i)
        self.stage.blockSignals(False)
        if not self.all:
            self.say("No hay volcados en %s/stage_dumps: usa volcar_escenarios.sh primero." % S.WORK)
            return
        new = self.all[i]
        changed = self.cur is None or self.cur["stage"] != new["stage"]
        self.cur = new
        if changed:
            self.fill_kinds(new["params"])
            self.load_original()
            self.sel_var = new["elegida"]
            if self.sel_var:
                self.show_variant(self.sel_var)
        self.fill_vars()
        self.fill_rules()

    def stage_changed(self):
        n = self.stage.currentData()
        if n is not None and (self.cur is None or n != self.cur["stage"]):
            self.cur = None
            self.refresh(n)

    def fill_kinds(self, p):
        self.params = json.loads(json.dumps(p))
        self.kt.setRowCount(len(self.params))
        self.kspin = {}
        for r, (k, v) in enumerate(self.params.items()):
            it = QListWidgetItem()
            lab = QLabel("  ■ " + k)
            c = kcol(k)
            lab.setStyleSheet("color: rgb(%d,%d,%d); font-weight:bold" % c)
            self.kt.setCellWidget(r, 0, lab)
            self.kspin[k] = []
            for i in range(4):
                s = QDoubleSpinBox()
                s.setRange(0, 1)
                s.setDecimals(3)
                s.setSingleStep(0.005 if i % 2 else 0.05)
                s.setValue(v[i])
                self.kt.setCellWidget(r, i + 1, s)
                self.kspin[k].append(s)
        self.kt.setMinimumHeight(30 + 30 * len(self.params))
        self.legend.setText("  ".join('<span style="color: rgb(%d,%d,%d)">■</span> %s' % (kcol(k) + (k,))
                                      for k in list(self.params) + ["cajas", "fondo"]))

    def read_params(self):
        return {k: [s.value() for s in ss] for k, ss in self.kspin.items()}

    def fill_vars(self):
        self.vl.clear()
        for v in reversed(self.cur["variantes"]):
            per = "  ·  ".join("%s %d/%d" % (k, t[0], t[1]) for k, t in v["tris"].items())
            txt = "%s  %s%s%s\ncerca %d · lejos %d triángulos\n%s" % (
                v["id"], v["label"], "   ● ELEGIDA" if v["id"] == self.cur["elegida"] else "",
                "   (estructura antigua)" if v.get("vieja") else "", v["lod1"], v["lod2"], per)
            it = QListWidgetItem(txt)
            it.setData(Qt.UserRole, v["id"])
            if v["id"] == self.cur["elegida"]:
                it.setForeground(QColor(63, 185, 80))
            self.vl.addItem(it)
            if v["id"] == self.sel_var:
                self.vl.setCurrentItem(it)

    def fill_rules(self):
        self.rl.clear()
        for r in self.cur["reglas"]:
            it = QListWidgetItem("%s   caja [%s]   lod [%s]%s%s" % (
                r["nombre"], ", ".join("%.0f" % x for x in r["caja"]), ", ".join("%g" % x for x in r["lod"]),
                "   ≥%d tri" % r["min_tris"] if r.get("min_tris") else "", "   dz≤%g" % r["max_dz"] if r.get("max_dz") else ""))
            it.setData(Qt.UserRole, r)
            it.setForeground(QColor(*kcol(r["nombre"])))
            self.rl.addItem(it)

    def view_changed(self):
        self.view.mode = self.mode.currentData()
        self.view.colour = self.c_col.isChecked()
        self.view.wire = self.c_wire.isChecked()

    def load_original(self):
        nn = "%02d" % self.cur["stage"]
        f = os.path.join(S.WORK, "export", "s" + nn, "s%s.gltf" % nn)
        self.view.makeCurrent()
        self.view.orig, self.view.var, self.view.hl, self.view.isl = None, None, None, None
        if os.path.exists(f):
            m = Model(f)
            self.view.orig = m
            self.view.isl = build_islands(m)
            self.view.frame(m)
            self.say("Escenario %s: %d triángulos originales." % (nn, len(self.view.isl[0])))
        else:
            self.say("Escenario %s sin exportar: pulsa «Preparar estructura»." % nn)
        self.mode.setCurrentIndex(0)

    def show_variant(self, vid):
        f = os.path.join(S.sdir(self.cur["stage"]), vid, "mapa.gltf")
        if not os.path.exists(f):
            return
        self.view.makeCurrent()
        self.view.var = Model(f)
        self.sel_var = vid
        if self.mode.currentData() == "orig":
            self.mode.setCurrentIndex(1)
        self.say("Viendo %s." % vid)

    def use_params(self):
        it = self.vl.currentItem()
        if it:
            v = next(x for x in self.cur["variantes"] if x["id"] == it.data(Qt.UserRole))
            self.fill_kinds(v["params"])

    def choose(self):
        it = self.vl.currentItem()
        if it:
            self.run("Elegir " + it.data(Qt.UserRole), S.do_apply, self.cur["stage"], it.data(Qt.UserRole))

    def on_pick(self, p):
        self.pick = p
        mn, mx = p["mn"], p["mx"]
        self.pinfo.setText("%d triángulos · ancho %.0f × %.0f · alto %.0f" % (p["n"], mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]))

    def make_rule(self):
        if not self.pick:
            return QMessageBox.information(self, "Regla", "Primero haz clic en una pieza en la vista «Original».")
        name = re.sub(r"[^a-z]", "", self.pname.text().lower())
        if not name or name in BASE:
            return QMessageBox.information(self, "Regla", "Pon un nombre propio, solo letras (p. ej. casa, torre, rocas).")
        mn, mx, pad = self.pick["mn"], self.pick["mx"], 2
        rule = {"nombre": name, "caja": [mn[0] - pad, mx[0] + pad, mn[1] - pad, mx[1] + pad],
                "lod": [round(s.value(), 4) for s in self.pspin]}
        if self.ponly.isChecked():
            rule["min_tris"] = max(1, int(self.pick["n"] * 0.9))
        d = S.rules_stage(self.cur["stage"])
        d["reglas"] = [rule] + [r for r in d.get("reglas", []) if r["nombre"] != name]
        S.save_rules_stage(self.cur["stage"], d)
        self.say("Regla «%s» guardada: pulsa «Preparar estructura» para aplicarla." % name)
        self.view.hl, self.pick = None, None
        self.pinfo.setText("Ninguna.")
        self.refresh()

    def del_rule(self):
        it = self.rl.currentItem()
        if not it:
            return
        r = it.data(Qt.UserRole)
        d = S.rules_stage(self.cur["stage"])
        d["reglas"] = [x for x in d.get("reglas", []) if x["nombre"] != r["nombre"]]
        S.save_rules_stage(self.cur["stage"], d)
        self.say("Regla «%s» quitada: pulsa «Preparar estructura»." % r["nombre"])
        self.refresh()

    # ------------------------------------------------------------ background jobs
    def run(self, what, fn, *a):
        if self.busy or not self.cur:
            return
        self.busy = True
        for b in self.buttons:
            b.setEnabled(False)
        self.say("== %s…" % what)
        j = {"log": ""}

        def go():
            try:
                res = fn(j, *a)
            except Exception as ex:                    # (shown in the log)
                j["log"] += "\nERROR: %s" % ex
                res = None
            self.bus.done.emit(what, res, j["log"])
        threading.Thread(target=go, daemon=True).start()

    def job_done(self, what, res, log):
        self.busy = False
        for b in self.buttons:
            b.setEnabled(True)
        self.say("\n".join(log.strip().splitlines()[-8:]))
        self.say("== %s: %s" % (what, "hecho" if res is not None else "ERROR"))
        n = self.cur["stage"]
        self.cur = None
        self.refresh(n)
        if res is not None:
            if isinstance(res, dict) and res.get("id"):
                self.show_variant(res["id"])
                self.fill_vars()
            elif isinstance(res, list) and res:
                self.show_variant(res[2]["id"] if len(res) > 2 else res[-1]["id"])
                self.fill_vars()
            elif what.startswith("Preparar"):
                self.load_original()


def main():
    fmt = QSurfaceFormat()
    fmt.setDepthBufferSize(24)
    fmt.setSamples(4)
    fmt.setProfile(QSurfaceFormat.CompatibilityProfile)
    QSurfaceFormat.setDefaultFormat(fmt)
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    w = Win()
    w.show()
    shot = os.environ.get("BT3_TUNER_SHOT")             # (self-test: pictures of the view in each mode, then quit)
    if shot:
        steps = [("orig", False), ("orig", True), ("lod1", False), ("lod2", True), ("game", False)]

        def take(i=0):
            if i >= len(steps):
                return app.quit()
            mode, col = steps[i]
            if mode != "orig" and w.view.var is None and w.cur and w.cur["variantes"]:
                w.show_variant(w.cur["variantes"][-1]["id"])
            w.mode.setCurrentIndex(w.mode.findData(mode))
            w.c_col.setChecked(col)
            QTimer.singleShot(1200, lambda: (w.grab().save("%s_%d_%s%s.png" % (shot, i, mode, "_col" if col else "")),
                                             take(i + 1)))
        QTimer.singleShot(2500, take)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
