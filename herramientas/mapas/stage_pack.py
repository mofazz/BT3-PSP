"""stage_pack.py - a stage's LOD glTF (stage_lod.py) -> the PSP's native map file (.bt3m) of the PS2RC BT3 build
(gs_ge_map.h): the cells with their bounds, LOD distances and three levels, as GE-ready vertices (u v, colour, x y z:
24 bytes), 16-bit indices and 8-bit textures with their palette. Coordinates in the game's own space (the export's
(x, -y, -z) undone). Made on the player's machine from their disc, never distributed.

    python stage_pack.py <lod.gltf> <out.bt3m> --stage N

Layout (little endian, every block 16-byte aligned, offsets from the file start):
  header  "BT3M" u32 version 1, u32 stage, u32 ncell, u32 nmat, u32 ntex, u32 fondo_batches_off(0), 4 reserved -> 32 B
  tex[ntex]   u32 w, h, pal_off, px_off                (pal: 256 u32 RGBA; px: w*h u8)
  mat[nmat]   i32 tex (-1 none), u32 kind (0 opaque, 1 cut-out, 2 blended), 2 reserved
  cell[ncell] f32 min[3], max[3], d1, d2, u32 nbatch[3], u32 batch_off[3]      (64 B)
  batch       u32 mat, nvert, nidx, vert_off, idx_off, 3 reserved               (32 B)
"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

KIND = {"op": 0, "mk": 1, "bl": 2}


def load(path):
    g = json.load(open(path))
    base = os.path.dirname(path)
    bufs = [open(os.path.join(base, b["uri"]), "rb").read() for b in g["buffers"]]
    return g, bufs, base


def accessor(g, bufs, i):
    a = g["accessors"][i]
    bv = g["bufferViews"][a["bufferView"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    dt = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[a["componentType"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride", 0)
    item = np.dtype(dt).itemsize * n
    raw = bufs[bv["buffer"]]
    if stride and stride != item:
        arr = np.stack([np.frombuffer(raw, dt, n, off + k * stride) for k in range(a["count"])])
    else:
        arr = np.frombuffer(raw, dt, a["count"] * n, off).reshape(a["count"], n)
    if a.get("normalized") and dt != np.float32:
        arr = arr.astype(np.float32) / float(np.iinfo(dt).max)
    return arr


def node_mats(g):
    """world matrix of every node"""
    import math
    W = {}

    def local(nd):
        if "matrix" in nd:
            return np.array(nd["matrix"], np.float64).reshape(4, 4).T
        t = np.eye(4)
        if "translation" in nd:
            t[:3, 3] = nd["translation"]
        r = np.eye(4)
        if "rotation" in nd:
            x, y, z, w = nd["rotation"]
            r[:3, :3] = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
        s = np.eye(4)
        if "scale" in nd:
            s[0, 0], s[1, 1], s[2, 2] = nd["scale"]
        return t @ r @ s

    def walk(i, parent):
        W[i] = parent @ local(g["nodes"][i])
        for c in g["nodes"][i].get("children", []):
            walk(c, W[i])
    for root in g["scenes"][g.get("scene", 0)]["nodes"]:
        walk(root, np.eye(4))
    return W


def main():
    a = sys.argv[1:]
    src, dst = a[0], a[1]
    stage = int(a[a.index("--stage") + 1]) if "--stage" in a else 0
    keep0 = "--lod0" in a                              # the original level too (the game draws LOD1 near, LOD2 far)
    g, bufs, base = load(src)
    W = node_mats(g)
    # materials and textures
    mats, texs, tex_of = [], [], {}
    for m in g.get("materials", []):
        name = m.get("name", "m_0_op").split(".")[0]
        kind = KIND.get(name[-2:], 0)
        t = -1
        bt = m.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if bt is not None:
            uri = g["images"][g["textures"][bt["index"]]["source"]]["uri"]
            if uri not in tex_of:
                tex_of[uri] = len(texs)
                texs.append(uri)
            t = tex_of[uri]
        mats.append((t, kind))
    # cells: nodes LOD_<cell> with children <cell>_LOD0..2
    cells = []
    for i, nd in enumerate(g["nodes"]):
        if not nd.get("name", "").startswith("LOD_"):
            continue
        tag = nd["name"][4:]
        d1, d2 = (nd.get("extras") or {}).get("w3d_lod_distancias", [1e9, 1e9])
        lods = [None, None, None]
        for c in nd.get("children", []):
            cn = g["nodes"][c]["name"]
            for k in range(3):
                if cn.startswith(tag + "_LOD%d" % k):     # (one object per kind of thing: a list)
                    lods[k] = (lods[k] or []) + [c]
        cells.append((tag, d1, d2, lods))
    # hierarchical LOD (stage_lod.py grp_tris): a block "GRP_<g>" (its LOD2, d1 = where its cells take over) followed
    # by its cells (their LOD1); written as: block d2 = -1, nb[0] = its cells' count, bo[1] = the first one's index;
    # cell d2 = -2 (drawn only through its block)
    grp_of = {}
    for i, nd in enumerate(g["nodes"]):
        if nd.get("name", "").startswith("LOD_") and (nd.get("extras") or {}).get("w3d_grupo"):
            grp_of[nd["name"][4:]] = nd["extras"]["w3d_grupo"]
    if grp_of:
        plain = [c for c in cells if c[0] not in grp_of]
        hier = []
        for i, nd in enumerate(g["nodes"]):
            if not nd.get("name", "").startswith("GRP_"):
                continue
            gt = nd["name"][4:]
            l2 = [c for c in nd.get("children", []) if g["nodes"][c]["name"].startswith(gt + "_LOD2")] or None
            kids = [c for c in cells if grp_of.get(c[0]) == gt]
            hier.append(("@" + gt, (nd.get("extras") or {}).get("w3d_lod_distancias", [300, 0])[0], -1.0,
                         [None, None, l2], len(kids)))
            for c in kids:
                hier.append((c[0], c[1], -2.0, [None, c[3][1], None], 0))
        cells = [h[:4] + (h[4] if len(h) > 4 else 0,) for h in hier] + [c + (0,) for c in plain]
    else:
        cells = [c + (0,) for c in cells]
    for i, nd in enumerate(g["nodes"]):                # the backdrop and the scattered pieces: one level, always
        if nd.get("name", "").split(".")[0] in ("fondo", "dispersos") and "mesh" in nd:
            cells.append((nd["name"], 1e30, 1e30, [i, None, None], 0))
    flip = np.array([1, -1, -1], np.float32)           # glTF (x, -y, -z) -> the game's (x, y, z)

    def mesh_batches(ni):
        """[(mat, verts (n, 6) as u v c x y z, idx)] of a node's mesh, in game coordinates"""
        nd = g["nodes"][ni]
        out = []
        if "mesh" not in nd:
            return out
        M = W[ni]
        for p in g["meshes"][nd["mesh"]]["primitives"]:
            at = p["attributes"]
            pos = accessor(g, bufs, at["POSITION"]).astype(np.float64)
            pos = (M @ np.c_[pos, np.ones(len(pos))].T).T[:, :3].astype(np.float32) * flip
            uv = accessor(g, bufs, at["TEXCOORD_0"]) if "TEXCOORD_0" in at else np.zeros((len(pos), 2), np.float32)
            col = accessor(g, bufs, at["COLOR_0"]) if "COLOR_0" in at else np.ones((len(pos), 4), np.float32)
            if col.shape[1] == 3:
                col = np.c_[col, np.ones(len(col))]
            rgba = (np.clip(col, 0, 1) * 255 + 0.5).astype(np.uint32)
            c32 = rgba[:, 0] | (rgba[:, 1] << 8) | (rgba[:, 2] << 16) | (rgba[:, 3] << 24)
            idx = accessor(g, bufs, p["indices"]).reshape(-1).astype(np.uint32) if "indices" in p else np.arange(len(pos), dtype=np.uint32)
            mat = p.get("material", 0)
            # 16-bit indices: split a primitive whose vertices exceed 65535 (never at these sizes)
            if len(pos) > 65535:
                raise SystemExit("a primitive with %d vertices: split it" % len(pos))
            v = np.zeros(len(pos), dtype=[("u", "<f4"), ("v", "<f4"), ("c", "<u4"), ("x", "<f4"), ("y", "<f4"), ("z", "<f4")])
            v["u"], v["v"], v["c"] = uv[:, 0], uv[:, 1], c32
            v["x"], v["y"], v["z"] = pos[:, 0], pos[:, 1], pos[:, 2]
            out.append((mat, v, idx.astype(np.uint16), pos))
        return out

    blob = bytearray(b"\0" * 32)

    def put(data, align=16):
        while len(blob) % align:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        return off

    # textures: 8-bit with a 256-colour palette (RGBA)
    tex_recs = []
    for uri in texs:
        im = Image.open(os.path.join(base, uri)).convert("RGBA")
        q = im.quantize(colors=256, method=Image.Quantize.FASTOCTREE)
        pal = q.getpalette("RGBA") if hasattr(q, "getpalette") else None
        pal = np.array(pal[:1024] if pal else [0] * 1024, np.uint8)
        pal = np.pad(pal, (0, 1024 - len(pal)))
        pal_off = put(pal.tobytes())
        px_off = put(np.array(q, np.uint8).tobytes())
        tex_recs.append((im.width, im.height, pal_off, px_off))
    tex_off = put(b"".join(struct.pack("<4I", *t) for t in tex_recs))
    mat_off = put(b"".join(struct.pack("<iI2I", t, k, 0, 0) for t, k in mats))
    cell_recs, stats = [], [0, 0, 0]
    idx_of_next = 0
    for ci, (tag, d1, d2, lods, nkids) in enumerate(cells):
        allpos, nb, boff = [], [], []
        for k in range(3):
            recs = []
            if lods[k] is not None and not (k == 0 and lods[1] is not None and not keep0):   # (LOD0 not used in game)
                for mat, v, idx, pos in (b for n_ in (lods[k] if isinstance(lods[k], list) else [lods[k]])
                                         for b in mesh_batches(n_)):
                    vo = put(v.tobytes())
                    io = put(idx.tobytes())
                    recs.append(struct.pack("<5I3I", mat, len(v), len(idx), vo, io, 0, 0, 0))
                    allpos.append(pos)
                    stats[k] += len(idx) // 3
            nb.append(len(recs))
            boff.append(put(b"".join(recs)) if recs else 0)
        if d2 == -1.0:                                 # a block: its box covers its cells; nb[0] / bo[1] = its cells
            for j in range(ci + 1, ci + 1 + nkids):
                for k in range(3):
                    if cells[j][3][k] is not None:
                        for n_ in (cells[j][3][k] if isinstance(cells[j][3][k], list) else [cells[j][3][k]]):
                            allpos.extend(p for _, _, _, p in mesh_batches(n_))
            nb[0], boff[1] = nkids, ci + 1
        P = np.concatenate(allpos) if allpos else np.zeros((1, 3), np.float32)
        cell_recs.append(struct.pack("<8f3I3I", *P.min(0), *P.max(0), d1, d2, *nb, *boff))
    cell_off = put(b"".join(cell_recs))
    struct.pack_into("<4sIIIIIII", blob, 0, b"BT3M", 1, stage, len(cells), len(mats), len(texs), 0, 0)
    # (the offsets of the three tables follow the header at fixed places: reserved words reused)
    blob[24:32] = struct.pack("<II", tex_off, mat_off)
    blob.extend(struct.pack("<I", cell_off))          # the last word of the file: the cell table's offset
    open(dst, "wb").write(blob)
    print("%s: stage %d, %d cells, %d materials, %d textures; triangles LOD0 %d, LOD1 %d, LOD2 %d; %d KB" % (
        dst, stage, len(cells), len(mats), len(texs), stats[0], stats[1], stats[2], len(blob) >> 10))


if __name__ == "__main__":
    main()
