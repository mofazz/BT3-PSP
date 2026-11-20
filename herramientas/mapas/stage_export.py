"""stage_export.py - turns a stage dump of the PS2RC BT3 build (ps2rc.cfg stage_dump=V, gs_ge_vu1.h) into glTF 2.0:
a .gltf + .bin + one PNG per texture, for an editor (Whisk3D, Blender...). The dump is made on the player's own
machine from their own disc; so is this export. Nothing of it may be distributed.

    python stage_export.py <stage_dump.bin> <out dir> [--name NAME] [--raw]

Geometry: every program-4 batch is a triangle strip (PRIM 4) of positions in the batch's model space; the batch's
clip matrix (VU 4..7, columns) is C = P V M. The most common C is taken as the camera's P V (M = identity, the stage's
own space); a batch drawn with another C gets M = (P V)^-1 C, so it lands where the game draws it. Colours: the PS2's
128 = 1.0 (texture modulation), alpha 128 = 1.0. Materials: one per (texture, alpha class: m_<texture>_op opaque / _mk cut-out / _bl blended); double sided (the
PS2 does not cull). Axes: the game's Y points down; the export turns the map 180 degrees about X, (x, -y, -z), so
that glTF's +Y is up (a rotation, not a mirror); --raw keeps the game's own axes."""
import json
import os
import struct
import sys
from collections import Counter, defaultdict

import numpy as np
from PIL import Image


def read_dump(path):
    d = open(path, "rb").read()
    if d[:4] != b"BT3S":
        sys.exit("not a stage dump: " + path)
    o, tex, bats = 8, {}, []
    while o + 4 <= len(d):
        tag = d[o:o + 4]
        if tag == b"TEX ":
            h, w, hh = struct.unpack_from("<3I", d, o + 4)
            px = np.frombuffer(d, dtype="<u4", count=w * hh, offset=o + 16)
            tex[h] = (w, hh, px.copy())
            o += 16 + 4 * w * hh
        elif tag == b"BAT ":
            tex0, tex1, clamp, alpha, test, prim = struct.unpack_from("<6Q", d, o + 4)
            th, = struct.unpack_from("<I", d, o + 52)
            vu = np.frombuffer(d, dtype="<f4", count=32, offset=o + 56).reshape(8, 4)
            n, = struct.unpack_from("<I", d, o + 184)
            v = np.frombuffer(d, dtype="<f4", count=12 * n, offset=o + 188).reshape(n, 12)
            bats.append(dict(tex0=tex0, alpha=alpha, test=test, prim=prim, th=th, clip=vu[4:8].copy(), v=v.copy()))
            o += 188 + 48 * n
        else:
            sys.exit("bad record at %d: %r" % (o, tag))
    return tex, bats


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    src, out = a[0], a[1]
    name = a[a.index("--name") + 1] if "--name" in a else "stage"
    raw = "--raw" in a
    tex, bats = read_dump(src)
    os.makedirs(out, exist_ok=True)
    # the camera: the most common clip matrix
    key = lambda c: c.round(4).tobytes()
    cnt = Counter(key(b["clip"]) for b in bats)
    ref = next(b["clip"] for b in bats if key(b["clip"]) == cnt.most_common(1)[0][0])
    pv = ref.T                                        # columns in the dump -> row-major matrix
    pv_inv = np.linalg.inv(pv)
    # textures; their alpha class (the stage draws everything with blending and alpha test on, so what a material
    # really is comes from its pixels): "op" every texel opaque, "mk" only (near) 0 or opaque - a cut-out, "bl" else
    tclass = {}
    for h, (w, hh, px) in tex.items():
        rgba = px.view(np.uint8).reshape(hh, w, 4).astype(np.uint16)
        a = rgba[..., 3]
        tclass[h] = "op" if a.min() >= 0x7C else "mk" if ((a <= 8) | (a >= 0x78)).all() else "bl"
        rgba[..., 3] = np.minimum(rgba[..., 3] * 2, 255)
        Image.fromarray(rgba.astype(np.uint8), "RGBA").save(os.path.join(out, "tex_%08x.png" % h))
    # geometry, grouped by material
    groups = defaultdict(lambda: {"pos": [], "col": [], "uv": [], "idx": []})
    moved = 0
    for b in bats:
        v = b["v"]
        pos = v[:, 0:4].astype(np.float64)
        if key(b["clip"]) != key(ref):
            m = pv_inv @ b["clip"].T
            pos = (m @ pos.T).T
            moved += 1
        pos = pos[:, :3] / np.where(pos[:, 3:4] != 0, pos[:, 3:4], 1)
        q = np.where(v[:, 10] != 0, v[:, 10], 1)
        uv = np.stack([v[:, 8] / q, v[:, 9] / q], 1)
        col = np.clip(v[:, 4:8] / 128.0, 0, 1)
        abe = (b["prim"] >> 6) & 1
        th = b["th"] if (b["prim"] >> 4) & 1 else 0
        kind = tclass.get(th, "op") if th else "op"
        if abe and v[:, 7].min() < 0x78:              # the vertices' own alpha fades it: blended
            kind = "bl"
        mkey = (th, kind)
        g = groups[mkey]
        base = sum(len(p) for p in g["pos"])
        g["pos"].append(pos)
        g["col"].append(col)
        g["uv"].append(uv)
        if (b["prim"] & 7) == 4:                     # triangle strip
            for i in range(len(v) - 2):
                g["idx"].append((base + i, base + i + 1, base + i + 2) if i % 2 == 0 else (base + i + 1, base + i, base + i + 2))
        elif (b["prim"] & 7) == 3:                   # triangle list
            for i in range(0, len(v) - 2, 3):
                g["idx"].append((base + i, base + i + 1, base + i + 2))
        elif (b["prim"] & 7) == 5:                   # fan
            for i in range(1, len(v) - 1):
                g["idx"].append((base, base + i, base + i + 1))
    # glTF
    blob = bytearray()
    gl = {"asset": {"version": "2.0", "generator": "PS2RC BT3 stage_export.py"}, "scene": 0,
          "scenes": [{"nodes": [0]}], "nodes": [{"name": name, "mesh": 0}], "meshes": [{"name": name, "primitives": []}],
          "buffers": [], "bufferViews": [], "accessors": [], "materials": [], "textures": [], "images": [],
          "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}]}

    def add(arr, comp, typ, target, minmax=False):
        arr = np.ascontiguousarray(arr)
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(arr.tobytes())
        gl["bufferViews"].append({"buffer": 0, "byteOffset": off, "byteLength": arr.nbytes, "target": target})
        acc = {"bufferView": len(gl["bufferViews"]) - 1, "componentType": comp, "count": int(arr.shape[0]), "type": typ}
        if minmax:
            acc["min"] = [float(x) for x in arr.min(0)]
            acc["max"] = [float(x) for x in arr.max(0)]
        gl["accessors"].append(acc)
        return len(gl["accessors"]) - 1

    img_of = {}
    ntri = 0
    for (th, kind), g in groups.items():
        if not g["idx"]:
            continue
        pos = np.concatenate(g["pos"]).astype(np.float32)
        if not raw:
            pos = pos * np.array([1, -1, -1], np.float32)
        col = np.concatenate(g["col"]).astype(np.float32)
        uv = np.concatenate(g["uv"]).astype(np.float32)
        idx = np.array(g["idx"], np.uint32).reshape(-1)
        ntri += len(g["idx"])
        mat = {"name": "m_%08x_%s" % (th, kind), "doubleSided": True,
               "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
        if th and th in tex:
            if th not in img_of:
                gl["images"].append({"uri": "tex_%08x.png" % th})
                gl["textures"].append({"source": len(gl["images"]) - 1, "sampler": 0})
                img_of[th] = len(gl["textures"]) - 1
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": img_of[th]}
        if kind == "bl":
            mat["alphaMode"] = "BLEND"
        elif kind == "mk":
            mat["alphaMode"] = "MASK"
            mat["alphaCutoff"] = 0.5
        gl["materials"].append(mat)
        prim = {"attributes": {"POSITION": add(pos, 5126, "VEC3", 34962, True),
                               "COLOR_0": add(col, 5126, "VEC4", 34962),
                               "TEXCOORD_0": add(uv, 5126, "VEC2", 34962)},
                "indices": add(idx, 5125, "SCALAR", 34963), "material": len(gl["materials"]) - 1, "mode": 4}
        gl["meshes"][0]["primitives"].append(prim)
    open(os.path.join(out, name + ".bin"), "wb").write(blob)
    gl["buffers"].append({"uri": name + ".bin", "byteLength": len(blob)})
    json.dump(gl, open(os.path.join(out, name + ".gltf"), "w"), indent=1)
    allpos = np.concatenate([p for g in groups.values() for p in g["pos"]])
    print("%s: %d batches (%d with another matrix), %d triangles, %d materials, %d textures; bounds %s .. %s"
          % (name, len(bats), moved, ntri, len(gl["materials"]), len(tex),
             np.round(allpos.min(0), 1), np.round(allpos.max(0), 1)))
    print("clip matrices: %d distinct" % len(cnt))


if __name__ == "__main__":
    main()
