"""stage_meshopt.py - reduces a stage's LOD levels with meshoptimizer (zeux/meshoptimizer, MIT: its simplifier, built
as a shared library: libmeshopt.so) instead of Blender's decimate. Input: a stage_lod.py output made with nodec=1
(LOD1 / LOD2 welded, not reduced; the buildings' boxes apart, "<block>_LOD2_cajas", left as they are).

    python stage_meshopt.py <in lod.gltf> <out.gltf> [lib=~/mo_src/libmeshopt.so] [r1=0.3 e1=0.02 r2=0.04 e2=0.08]

Per LOD node: its primitives (one per material) are reduced together - meshopt_simplifyWithAttributes with UV and
colour as attributes (the seams between materials and UV islands kept: vertices at one place with other attributes)
and with the vertices where the node meets another node of its level (a cell or block cut) locked, so neighbours
meet without cracks. r = target share of the triangles, e = the error allowed (relative to the node's size): the
simplifier stops at whichever comes first. LOD2 also prunes small loose bits (meshopt_SimplifyPrune).
Runs where the library was built (WSL: numpy + pillow in ~/mo_venv). Made from the player's own disc on their own
machine: never distributed."""
import ctypes
import re
import json
import os
import struct
import sys
from collections import Counter

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stage_pack as sp                                 # (load, accessor, node_mats)

a = sys.argv[1:]
src, dst = a[0], a[1]
opt = dict(x.split("=", 1) for x in a[2:])
lib = ctypes.CDLL(os.path.expanduser(opt.get("lib", "~/mo_src/libmeshopt.so")))
f = lib.meshopt_simplifyWithAttributes
f.restype = ctypes.c_size_t
f.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t,
              ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
              ctypes.c_float, ctypes.c_uint, ctypes.c_void_p]
PRUNE = 1 << 3
R = {1: float(opt.get("r1", 0.3)), 2: float(opt.get("r2", 0.04))}
# per kind of thing (level, kind): (share of triangles, error, prune, each material apart). The ground's layers
# (roads, markings over grass) are reduced material by material: merged, their places coincide and the texture
# transfer would take the wrong layer
KIND = {(1, "suelo"): (0.3, 0.03, 0, 1), (2, "suelo"): (0.02, 0.3, 0, 1),
        (1, "edif"): (0.3, 0.05, 0, 0), (2, "edif"): (0.1, 0.1, 1, 0),
        (1, "arbol"): (0.4, 0.04, 0, 1), (2, "arbol"): (0.05, 0.2, 1, 1),
        (1, "otros"): (0.3, 0.08, 0, 0), (2, "otros"): (0.005, 1.0, 1, 0)}
if opt.get("rules") and opt.get("stage"):             # the stage's own kinds (stage_rules.json)
    st_ = json.load(open(opt["rules"])).get(str(int(opt["stage"])), [])
    for rl in (st_.get("reglas", []) if isinstance(st_, dict) else st_):
        r1_, e1_, r2_, e2_ = rl["lod"]
        KIND[(1, rl["nombre"])] = (r1_, e1_, 0, 0)
        KIND[(2, rl["nombre"])] = (r2_, e2_, 0, 0)
for k_ in list(KIND):                                  # kind_r1=, kind_e2=... on the command line
    lv_, kd_ = k_
    r_, e_, p_, m_ = KIND[k_]
    KIND[k_] = (float(opt.get("%s_r%d" % (kd_, lv_), r_)), float(opt.get("%s_e%d" % (kd_, lv_), e_)), p_, m_)
NOLOCK = int(opt.get("nolock", 0))
LOCK2 = int(opt.get("lock2", 0))                       # LOD2 (far): cuts not locked unless lock2=1 (more reduction)
E = {1: float(opt.get("e1", 0.02)), 2: float(opt.get("e2", 0.08))}

g, bufs, base = sp.load(src)
MATKIND = {i: {"op": 0, "mk": 1, "bl": 2}.get(m.get("name", "").split(".")[0][-2:], 0)
           for i, m in enumerate(g.get("materials", []))}
W = sp.node_mats(g)


def level_of(name):
    """(level, kind) of a LOD object: <tag>_LOD1 / _LOD2, optionally _<kind> (stage_lod.py classes=1); boxes: 0"""
    m = re.match(r".*_LOD([12])(?:_([a-z]+))?$", name)
    if not m or m.group(2) == "cajas":
        return 0, None
    return int(m.group(1)), m.group(2) or "todo"


def node_data(ni):
    """the node's primitives merged: pos (local), attrs (u v r g b a), idx, prim id per vertex, materials"""
    P, A, I, PID, mats = [], [], [], [], []
    off = 0
    for k, p in enumerate(g["meshes"][g["nodes"][ni]["mesh"]]["primitives"]):
        at = p["attributes"]
        pos = sp.accessor(g, bufs, at["POSITION"]).astype(np.float32)
        n = len(pos)
        uv = sp.accessor(g, bufs, at["TEXCOORD_0"]).astype(np.float32) if "TEXCOORD_0" in at else np.zeros((n, 2), np.float32)
        col = sp.accessor(g, bufs, at["COLOR_0"]).astype(np.float32) if "COLOR_0" in at else np.ones((n, 4), np.float32)
        if col.shape[1] == 3:
            col = np.c_[col, np.ones(n, np.float32)]
        idx = sp.accessor(g, bufs, p["indices"]).reshape(-1).astype(np.uint32) if "indices" in p else np.arange(n, dtype=np.uint32)
        P.append(pos); A.append(np.c_[uv, col]); I.append(idx + off); PID.append(np.full(n, k, np.int32))
        mats.append(p.get("material", 0))
        off += n
    return (np.concatenate(P), np.ascontiguousarray(np.concatenate(A), np.float32), np.concatenate(I).astype(np.uint32),
            np.concatenate(PID), mats)


def wpos(ni, P):
    M = W[ni]
    return (M @ np.c_[P.astype(np.float64), np.ones(len(P))].T).T[:, :3]


# 1. the cuts: world positions found in more than one node of a level
lv_nodes = {}
for ni, nd in enumerate(g["nodes"]):
    lv, kd = level_of(nd.get("name", ""))
    if lv and "mesh" in nd:
        lv_nodes.setdefault((lv, kd), []).append(ni)
data, keys = {}, {}
for lv, nodes in lv_nodes.items():
    cnt = Counter()
    for ni in nodes:
        data[ni] = node_data(ni)
        k = [tuple(r) for r in np.round(wpos(ni, data[ni][0]), 1)]
        keys[ni] = k
        cnt.update(set(k))
    for ni in nodes:
        data[ni] = data[ni] + (np.array([1 if cnt[t] > 1 and not NOLOCK else 0 for t in keys[ni]], np.uint8),)

# 2. reduce, split back per material, compact
blob = bytearray()
newbuf = len(g["buffers"])


def add_view(arr, target=None):
    while len(blob) % 4:
        blob.append(0)
    o = len(blob)
    blob.extend(arr.tobytes())
    v = {"buffer": newbuf, "byteOffset": o, "byteLength": arr.nbytes}
    if target:
        v["target"] = target
    g["bufferViews"].append(v)
    return len(g["bufferViews"]) - 1


def add_acc(arr, typ, ct, target=None, minmax=False):
    acc = {"bufferView": add_view(arr, target), "componentType": ct, "count": len(arr), "type": typ}
    if minmax:
        acc["min"] = [float(x) for x in arr.min(0)]
        acc["max"] = [float(x) for x in arr.max(0)]
    g["accessors"].append(acc)
    return len(g["accessors"]) - 1


def transfer(Pw, Iw, P, A, I, PID, out_tri):
    """per new triangle (welded positions Pw): the nearest source triangle facing the same way (P, A, I: the
    original) gives its material and its UV / colour mapping, applied to the new corners (a triangle soup)"""
    S = I.reshape(-1, 3)
    sp0, sp1, sp2 = P[S[:, 0]].astype(np.float64), P[S[:, 1]].astype(np.float64), P[S[:, 2]].astype(np.float64)
    sc = (sp0 + sp1 + sp2) / 3
    sn = np.cross(sp1 - sp0, sp2 - sp0)
    sn /= np.linalg.norm(sn, axis=1, keepdims=True) + 1e-12
    e1, e2 = sp1 - sp0, sp2 - sp0                         # barycentric solve: [e1 e2] b = q - p0 (least squares)
    G = np.stack([np.stack([(e1 * e1).sum(1), (e1 * e2).sum(1)], 1), np.stack([(e1 * e2).sum(1), (e2 * e2).sum(1)], 1)], 1)
    Gi = np.linalg.pinv(G)
    T = out_tri
    q = Pw[T].astype(np.float64)                          # (n, 3 corners, 3)
    oc = q.mean(1)
    on = np.cross(q[:, 1] - q[:, 0], q[:, 2] - q[:, 0])
    on /= np.linalg.norm(on, axis=1, keepdims=True) + 1e-12
    best = np.zeros(len(T), np.int64)
    for a in range(0, len(T), 256):                       # (in slices: memory)
        d = ((oc[a:a + 256, None, :] - sc[None, :, :]) ** 2).sum(2)
        facing = (on[a:a + 256] @ sn.T)
        d = d + (facing < 0.5) * 1e12
        best[a:a + 256] = d.argmin(1)
    r = q - sp0[best][:, None, :]
    rhs = np.stack([(r * e1[best][:, None, :]).sum(2), (r * e2[best][:, None, :]).sum(2)], 2)   # (n, 3, 2)
    bb = np.einsum("nij,nkj->nki", Gi[best], rhs)          # (n, 3, 2): b1 b2 per corner
    a0, a1, a2 = A[S[best, 0]], A[S[best, 1]], A[S[best, 2]]
    attr = a0[:, None, :] + bb[..., :1] * (a1 - a0)[:, None, :] + bb[..., 1:] * (a2 - a0)[:, None, :]
    attr[..., 2:] = np.clip(attr[..., 2:], 0, 1)
    return PID[S[best, 0]], attr.astype(np.float32)


def reduce(P, A, I, PID, lock, r, e, prune):
    """geometry welded by place, simplified (locks kept), the texture mapping transferred: (material ids, corners
    (n, 3, 3), attributes (n, 3, 6))"""
    Pw, inv = np.unique(np.round(P, 2), axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    Pw = np.ascontiguousarray(Pw, np.float32)
    Iw = inv[I].astype(np.uint32).reshape(-1, 3)
    Iw = Iw[(Iw[:, 0] != Iw[:, 1]) & (Iw[:, 1] != Iw[:, 2]) & (Iw[:, 0] != Iw[:, 2])].reshape(-1)
    Iw = np.ascontiguousarray(Iw, np.uint32)
    if not len(Iw):
        return None
    lw = np.zeros(len(Pw), np.uint8)
    np.maximum.at(lw, inv, lock)
    out = np.zeros(len(Iw), np.uint32)
    err = ctypes.c_float(0)
    target = max(3, int(len(Iw) * r) // 3 * 3)
    n = fs(out.ctypes.data, Iw.ctypes.data, len(Iw), Pw.ctypes.data, len(Pw), 12, None, 0, None, 0,
           lw.ctypes.data, target, e, PRUNE if prune else 0, ctypes.byref(err))
    tri = out[:n].reshape(-1, 3)
    if not len(tri):
        return None
    pid, attr = transfer(Pw, Iw, P, A, I, PID, tri)
    return pid, Pw[tri], attr


tot = {}
fs = lib.meshopt_simplifyWithAttributes
for (lv, kd), nodes in lv_nodes.items():
    r, e, prune, permat = KIND.get((lv, kd), (R[lv], E[lv], lv == 2, 0))
    for ni in nodes:
        P, A, I, PID, mats, lock = data[ni]
        if lv == 2 and not LOCK2 and kd != "suelo":   # (far: cuts free, except the ground's - gaps show there)
            lock = np.zeros_like(lock)
        P = np.ascontiguousarray(P, np.float32)
        parts = []
        if kd == "suelo":                              # the ground: its base reduced as one surface (no gaps
            T = I.reshape(-1, 3)                       # between materials); the layers drawn over it (the same
            pk = [tuple(sorted(tuple(np.round(P[v], 1)) for v in t)) for t in T]
            op = [MATKIND.get(mats[PID[t[0]]], 0) == 0 for t in T]
            first = {}
            for t_i, k_ in enumerate(pk):              # the base: an opaque one where there is one
                if k_ not in first or (op[t_i] and not op[first[k_]]):
                    first[k_] = t_i
            isb = np.zeros(len(T), bool)
            isb[list(first.values())] = True
            res = reduce(P, A, np.ascontiguousarray(T[isb].reshape(-1), np.uint32), PID, lock, r, e, prune)
            if res:
                parts.append(res)
            over = T[~isb]
            for k in range(len(mats)):
                t = over[PID[over[:, 0]] == k].reshape(-1)
                if len(t):
                    res = reduce(P, A, np.ascontiguousarray(t, np.uint32), PID, np.zeros_like(lock), r, e, prune)
                    if res:
                        parts.append(res)
        elif permat:                                   # each material (layer) on its own
            T = I.reshape(-1, 3)
            for k in range(len(mats)):
                t = T[PID[T[:, 0]] == k].reshape(-1)
                if len(t):
                    res = reduce(P, A, np.ascontiguousarray(t, np.uint32), PID, lock, r, e, prune)
                    if res:
                        parts.append(res)
        else:
            res = reduce(P, A, I, PID, lock, r, e, prune)
            if res:
                parts.append(res)
        t0 = tot.setdefault((lv, kd), [0, 0])
        t0[0] += len(I) // 3
        t0[1] += sum(len(x[0]) for x in parts)
        prims = []
        if parts:
            pid = np.concatenate([x[0] for x in parts])
            cor = np.concatenate([x[1] for x in parts])
            attr = np.concatenate([x[2] for x in parts])
            for k, mat in enumerate(mats):
                sel = pid == k
                if not sel.any():
                    continue
                pos = np.ascontiguousarray(cor[sel].reshape(-1, 3), np.float32)
                at = attr[sel].reshape(-1, 6)
                attrs = {"POSITION": add_acc(pos, "VEC3", 5126, 34962, True),
                         "TEXCOORD_0": add_acc(np.ascontiguousarray(at[:, :2]), "VEC2", 5126, 34962),
                         "COLOR_0": add_acc(np.ascontiguousarray(at[:, 2:6]), "VEC4", 5126, 34962)}
                prims.append({"attributes": attrs, "indices": add_acc(np.arange(len(pos), dtype=np.uint32), "SCALAR",
                                                                        5125, 34963), "material": mat})
        g["meshes"][g["nodes"][ni]["mesh"]]["primitives"] = prims
        if not prims:
            del g["nodes"][ni]["mesh"]

# 3. the sky ("fondo"): one low-poly half sphere with the sky's main texture (sky_seg=16 around, sky_rings=5 up;
#    sky=0 keeps the original). Each corner takes the texture place of the original sky nearest in direction; a
#    triangle crossing the texture's wrap (u jumping by more than a half) is shifted by a whole turn (GU_REPEAT)
SKY = int(opt.get("sky", 1))
for ni, nd in enumerate(g["nodes"]):
    if not SKY or not nd.get("name", "").startswith("fondo") or "mesh" not in nd:
        continue
    P, A, I, PID, mats, _ = node_data(ni) + (None,)
    T = I.reshape(-1, 3)
    area = {}
    for t in T:
        k = PID[t[0]]
        area[k] = area.get(k, 0) + np.linalg.norm(np.cross(P[t[1]] - P[t[0]], P[t[2]] - P[t[0]])) / 2
    main = max(area, key=lambda k: (MATKIND.get(mats[k], 0) == 0, area[k]))
    used = np.unique(T[PID[T[:, 0]] == main].reshape(-1))
    Q = P[used].astype(np.float64)
    lo, hi = Q.min(0), Q.max(0)
    c = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])      # (glTF: +y up)
    rx, ry, rz = (hi[0] - lo[0]) / 2, hi[1] - lo[1], (hi[2] - lo[2]) / 2
    D = (Q - c) / np.array([rx, ry, rz])
    D /= np.linalg.norm(D, axis=1, keepdims=True) + 1e-12
    NS, NR = int(opt.get("sky_seg", 16)), int(opt.get("sky_rings", 5))
    ring = [(np.cos(np.pi / 2 * r / NR), np.sin(np.pi / 2 * r / NR)) for r in range(NR + 1)]   # (cos, sin) elevation

    def vert(si, ri):
        ce, se = ring[ri]
        a = 2 * np.pi * si / NS
        return np.array([np.cos(a) * ce, se, np.sin(a) * ce])
    tris = []
    for ri in range(NR):
        for si in range(NS):
            a, b_ = vert(si, ri), vert(si + 1, ri)
            c_, d_ = vert(si + 1, ri + 1), vert(si, ri + 1)
            tris.append((a, b_, c_))
            if ri + 1 < NR:
                tris.append((a, c_, d_))
    tris = np.array(tris)                                  # (n, 3, 3) unit directions
    near = np.array([[np.argmax(D @ v) for v in t] for t in tris])
    attr = A[used][near]                                   # (n, 3, 6)
    for t in attr:                                         # the wrap: corners brought within half a turn
        for ax in (0, 1):
            ref = t[0, ax]
            for k in (1, 2):
                while t[k, ax] - ref > 0.5:
                    t[k, ax] -= 1.0
                while ref - t[k, ax] > 0.5:
                    t[k, ax] += 1.0
    pos = (tris * np.array([rx, ry, rz]) + c).astype(np.float32).reshape(-1, 3)
    at = attr.reshape(-1, 6).astype(np.float32)
    attrs = {"POSITION": add_acc(np.ascontiguousarray(pos), "VEC3", 5126, 34962, True),
             "TEXCOORD_0": add_acc(np.ascontiguousarray(at[:, :2]), "VEC2", 5126, 34962),
             "COLOR_0": add_acc(np.ascontiguousarray(at[:, 2:6]), "VEC4", 5126, 34962)}
    keep = [pr for k, pr in enumerate(g["meshes"][nd["mesh"]]["primitives"]) if k != main]   # (sea, horizon, clouds:
    g["meshes"][nd["mesh"]]["primitives"] = [{"attributes": attrs, "material": mats[main],   #  as they are)
                                              "indices": add_acc(np.arange(len(pos), dtype=np.uint32), "SCALAR", 5125, 34963)}] + keep
    print("STAGE_MESHOPT: sky dome %d -> %d triangles (half sphere, %d x %d); %d other sky layers kept" % (
        int((PID[T[:, 0]] == main).sum()), len(tris), NS, NR, len(keep)))

name = os.path.splitext(os.path.basename(dst))[0] + "_mo.bin"
g["buffers"].append({"uri": name, "byteLength": len(blob)})
for b in g["buffers"][:-1]:                             # (the original buffers: by path from the new file's place)
    b["uri"] = os.path.relpath(os.path.join(base, b["uri"]), os.path.dirname(os.path.abspath(dst))).replace("\\", "/")
for im in g.get("images", []):
    if "uri" in im:
        im["uri"] = os.path.relpath(os.path.join(base, im["uri"]), os.path.dirname(os.path.abspath(dst))).replace("\\", "/")
os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
open(os.path.join(os.path.dirname(os.path.abspath(dst)), name), "wb").write(blob)
json.dump(g, open(dst, "w"))
print("STAGE_MESHOPT: " + "; ".join("LOD%d %s %d -> %d" % (k[0], k[1], v[0], v[1]) for k, v in sorted(tot.items()))
      + "; %d KB of new data" % (len(blob) >> 10))
