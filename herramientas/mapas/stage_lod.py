"""stage_lod.py - LODs and occluders for a stage exported by stage_export.py, made with Blender in background mode:

    blender -b --factory-startup --python stage_lod.py -- <in.gltf> <out.gltf> [max_tris=1500] [min_cell=500] [min_tris=40]
                                                                               [lod1=0.4] [lod2=0.12] [occ=0.06] [far=20000]
                                                                               [d1f=1.5 d1min=1500 d2f=4 d2min=5000: LOD distances]

1. the far backdrop (sky dome, horizon: loose parts reaching past FAR units from the centre) is kept apart as
   "fondo" - no LOD, no occlusion;
2. the rest is cut into adaptive cells (a quadtree, by face centre: split while a cell holds more than max_tris
   triangles and is larger than min_cell); cells under min_tris go together into "dispersos" (no LOD);
3. per cell: LOD0 = the original faces, LOD1 / LOD2 = welded (vertices split by the PS2 strips joined) and decimated
   to lod1 / lod2 of the triangles; the UVs and materials are kept (face-corner data);
4. per cell an occluder: only the opaque faces (no blending, no alpha test), welded, decimated to occ, without
   materials - for occlusion tests only, never drawn;
5. glTF out: empties "LOD_<cell>" (extras w3d_lod_distancias) holding "<cell>_LOD0..2", the occluders under
   "Oclusores" (extras w3d_solo_oclusion), "fondo" alone.
The input and output are made from the player's own disc on their own machine: never distributed."""
import math
import sys

import bmesh
import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
src, dst = argv[0], argv[1]
opt = dict(a.split("=") for a in argv[2:])
CELL = float(opt.get("cell", 3000))
R1, R2, RO = float(opt.get("lod1", 0.4)), float(opt.get("lod2", 0.12)), float(opt.get("occ", 0.06))
FAR = float(opt.get("far", 20000))
WELD = float(opt.get("weld", 0.5))
TOL1, TOL2 = float(opt.get("tol1", 0.02)), float(opt.get("tol2", 0.05))   # surface a level may lose
PIN = int(opt.get("pin", 1))                          # open borders pinned
NODEC = int(opt.get("nodec", 0))                      # 1: LOD1 / LOD2 welded, not reduced (stage_meshopt.py)
PIN2 = opt.get("pin2", "cut")                         # LOD2: "cut" = only the cell cuts pinned, "all"

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
for o in bpy.context.scene.objects:
    o.select_set(False)
for o in meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
if len(meshes) > 1:
    bpy.ops.object.join()
src_obj = bpy.context.view_layer.objects.active
src_obj.name = "mapa"
bpy.ops.object.select_all(action="DESELECT")
# glTF import parents the mesh under a node with the file's transform: apply it
src_obj.select_set(True)
bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def split_faces(obj, keep, name):
    """a copy of obj with only the faces keep(face) says yes to (None when empty)"""
    me = obj.data.copy()
    bm = bmesh.new()
    bm.from_mesh(me)
    dead = [f for f in bm.faces if not keep(f)]
    bmesh.ops.delete(bm, geom=dead, context="FACES")
    if not bm.faces:
        bm.free()
        bpy.data.meshes.remove(me)
        return None
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(o)
    return o


def _area(me):
    return sum(p.area for p in me.polygons)


OPEN = None                                            # the welded map's own open borders (rounded positions)


def open_borders(obj):
    global OPEN
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=WELD)
    OPEN = {tuple(round(c, 1) for c in v.co) for e in bm.edges if e.is_boundary for v in e.verts}
    bm.free()


def weld_decimate(o, ratio, drop_materials=False, tol=None, pin="all"):
    """welds, then collapses to `ratio` of the triangles - with the open borders (the cell's edges, where a wall meets
    nothing) pinned and UV seams kept, so neighbouring cells meet without cracks; when the result lost more than `tol`
    of the surface (thin parts collapsed: holes that show the sky), tried again with more triangles"""
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=WELD)
    bm.to_mesh(o.data)
    bm.free()
    if NODEC:                                          # (stage_meshopt.py reduces it: welded only)
        if drop_materials:
            o.data.materials.clear()
        return len(o.data.polygons)
    a0 = _area(o.data)
    vg = o.vertex_groups.new(name="libres")            # 1 = may move, 0 = pinned (border vertices)
    border = set()
    bm = bmesh.new()
    bm.from_mesh(o.data)
    for e in bm.edges:
        if e.is_boundary:
            for v in e.verts:                          # pin="cut": only where the cell was cut from its neighbours
                if pin == "all" or tuple(round(c, 1) for c in v.co) not in OPEN:
                    border.add(v.index)
    bm.free()
    free = [v.index for v in o.data.vertices if v.index not in border]
    if free:
        vg.add(free, 1.0, "REPLACE")
    keep = o.data.copy()
    tol = TOL1 if tol is None else tol
    r = ratio
    while True:
        m = o.modifiers.new("dec", "DECIMATE")
        m.decimate_type = "COLLAPSE"
        m.ratio = r
        m.use_collapse_triangulate = True
        m.delimit = {"UV"}
        m.vertex_group = "libres"
        m.vertex_group_factor = 1000.0 if PIN else 0.0
        bpy.context.view_layer.objects.active = o
        o.select_set(True)
        bpy.ops.object.modifier_apply(modifier="dec")
        o.select_set(False)
        loss = 1.0 - _area(o.data) / a0 if a0 > 0 else 0.0
        if loss <= tol or r >= 0.95:
            break
        o.data = keep.copy()                           # back to the welded mesh, more triangles
        r = min(0.95, r * 1.5)
    o["w3d_lod_ratio"] = r
    if drop_materials:
        o.data.materials.clear()
    return len(o.data.polygons)


BOX_H = float(opt.get("box_h", 0))                    # >0: LOD2 buildings (loose parts taller than this, footprint
BOX_W = float(opt.get("box_w", 3000))                 # under box_w) become boxes: 4 sides + roof, 10 triangles


def _hull_rect(pts):
    """the minimum-area rectangle around 2D points: (centre, axis u, axis v, half u, half v)"""
    import mathutils
    from mathutils import Vector
    vs = [Vector(p) for p in pts]
    hull = [vs[i] for i in mathutils.geometry.convex_hull_2d(vs)]
    best = None
    for i in range(len(hull)):
        e = hull[(i + 1) % len(hull)] - hull[i]
        if e.length < 1e-6:
            continue
        u = e.normalized()
        v = Vector((-u.y, u.x))
        pu = [p.dot(u) for p in hull]
        pv = [p.dot(v) for p in hull]
        a = (max(pu) - min(pu)) * (max(pv) - min(pv))
        if best is None or a < best[0]:
            best = (a, u, v, min(pu), max(pu), min(pv), max(pv))
    if best is None:
        return None
    _, u, v, u0, u1, v0, v1 = best
    c = u * ((u0 + u1) / 2) + v * ((v0 + v1) / 2)
    return c, u, v, (u1 - u0) / 2, (v1 - v0) / 2


def boxify(o):
    """takes the buildings out of o (welded): returns their boxes [(corners 8, material, uv density, colour)]"""
    if BOX_H <= 0:
        return []
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=WELD)
    bm.faces.ensure_lookup_table()
    uvl = bm.loops.layers.uv.active
    coll = bm.loops.layers.float_color.active or bm.loops.layers.color.active
    seen, isl = set(), []
    for f in bm.faces:                                 # loose parts
        if f.index in seen:
            continue
        stack, part = [f], []
        seen.add(f.index)
        while stack:
            g = stack.pop()
            part.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h.index not in seen:
                        seen.add(h.index)
                        stack.append(h)
        isl.append(part)
    info = []                                          # (faces, x0, y0, x1, y1, z0, z1)
    for part in isl:
        vs = {v for f in part for v in f.verts}
        xs = [v.co.x for v in vs]; ys = [v.co.y for v in vs]; zs = [v.co.z for v in vs]
        info.append([part, min(xs), min(ys), max(xs), max(ys), min(zs), max(zs)])
    # parts of one building: tall parts whose footprints overlap join (union-find)
    tall = [i for i, t in enumerate(info) if t[6] - t[5] >= BOX_H and max(t[3] - t[1], t[4] - t[2]) <= BOX_W]
    par = {i: i for i in tall}

    def root(i):
        while par[i] != i:
            par[i] = par[par[i]]
            i = par[i]
        return i
    for a in range(len(tall)):
        for b in range(a + 1, len(tall)):
            A, B = info[tall[a]], info[tall[b]]
            ix = min(A[3], B[3]) - max(A[1], B[1]); iy = min(A[4], B[4]) - max(A[2], B[2])
            if ix > 0 and iy > 0:
                par[root(tall[a])] = root(tall[b])
    bld = {}
    for i in tall:
        bld.setdefault(root(i), []).extend(info[i][0])
    boxes, dead = [], []
    for faces in bld.values():
        vs = list({v for f in faces for v in f.verts})
        r = _hull_rect([(v.co.x, v.co.y) for v in vs])
        if r is None:
            continue
        c, u, v, hu, hv = r
        z0 = min(x.co.z for x in vs); z1 = max(x.co.z for x in vs)
        area = {}
        dens, col, ncol = [], [0.0, 0.0, 0.0, 0.0], 0
        for f in faces:
            area[f.material_index] = area.get(f.material_index, 0) + f.calc_area()
            if uvl:
                ls = f.loops
                for k in range(len(ls)):
                    a, b = ls[k], ls[(k + 1) % len(ls)]
                    dp = (a.vert.co - b.vert.co).length
                    if dp > 1e-3:
                        dens.append((a[uvl].uv - b[uvl].uv).length / dp)
            if coll:
                for l in f.loops:
                    cc = l[coll]
                    for k in range(4):
                        col[k] += cc[k]
                    ncol += 1
        dens.sort()
        d = dens[len(dens) // 2] if dens else 0.01
        mat = max(area, key=area.get)
        colour = [x / ncol for x in col] if ncol else [1, 1, 1, 1]
        from mathutils import Vector
        P = []
        for z in (z0, z1):
            for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                q = c + u * (su * hu) + v * (sv * hv)
                P.append(Vector((q.x, q.y, z)))
        boxes.append((P, mat, d, colour))
        dead.extend(faces)
    bmesh.ops.delete(bm, geom=list(set(dead)), context="FACES")
    bm.to_mesh(o.data)
    bm.free()
    return boxes


def add_boxes(o, boxes):
    """the boxes into o: 4 sides and the roof (quads: 10 triangles), the building's material and colour, its
    texture repeated at its own density"""
    if not boxes:
        return
    bm = bmesh.new()
    bm.from_mesh(o.data)
    uvl = bm.loops.layers.uv.active or bm.loops.layers.uv.new("UVMap")
    coll = bm.loops.layers.float_color.active or bm.loops.layers.color.active
    for P, mat, d, colour in boxes:
        V = [bm.verts.new(p) for p in P]
        quads = [(0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7), (4, 5, 6, 7)]
        for q in quads:
            try:
                f = bm.faces.new([V[i] for i in q])
            except ValueError:
                continue
            f.material_index = mat
            a, b = P[q[0]], P[q[1]]
            if q == (4, 5, 6, 7):                      # roof: planar
                for l in f.loops:
                    l[uvl].uv = (l.vert.co.x * d, l.vert.co.y * d)
            else:
                w = (b - a).length
                for l in f.loops:
                    t = (l.vert.co - a)
                    along = t.xy.dot((b - a).xy.normalized()) if w > 1e-6 else 0
                    l[uvl].uv = (along * d, (l.vert.co.z - P[0].z) * d)
            if coll:
                for l in f.loops:
                    l[coll] = colour
    bm.to_mesh(o.data)
    bm.free()



import json as _json
RULES = []
if opt.get("rules") and opt.get("stage"):
    RULES = _json.load(open(opt["rules"])).get(str(int(opt["stage"])), [])
    RULES = RULES.get("reglas", []) if isinstance(RULES, dict) else RULES
CLASSES = ("suelo", "edif", "arbol", "otros") + tuple(r["nombre"] for r in RULES)


def classify(obj, skip):
    """each face's kind, by its loose part (faces joined by a shared vertex position): "arbol" (mostly cut-out
    materials: foliage), "edif" (taller than box_h, footprint under box_w), "suelo" (flat: wider than 3 x its height,
    or mostly facing up), "otros" (cars, props...)"""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.faces.ensure_lookup_table()
    par = list(range(len(bm.faces)))

    def root(i):
        while par[i] != i:
            par[i] = par[par[i]]
            i = par[i]
        return i
    first = {}
    for f in bm.faces:
        if f.index in skip:
            continue
        for v in f.verts:
            k = (round(v.co.x, 1), round(v.co.y, 1), round(v.co.z, 1))
            j = first.setdefault(k, f.index)
            if j != f.index:
                a, b = root(j), root(f.index)
                if a != b:
                    par[a] = b
    parts = {}
    for f in bm.faces:
        if f.index not in skip:
            parts.setdefault(root(f.index), []).append(f)
    mats_ = obj.data.materials
    out = {}
    H = BOX_H if BOX_H > 0 else 150.0
    for fs in parts.values():
        xs = [v.co.x for f in fs for v in f.verts]; ys = [v.co.y for f in fs for v in f.verts]
        zs = [v.co.z for f in fs for v in f.verts]
        dz, fw = max(zs) - min(zs), max(max(xs) - min(xs), max(ys) - min(ys))
        area = sum(f.calc_area() for f in fs) or 1.0
        mk = sum(f.calc_area() for f in fs if f.material_index < len(mats_) and
                 mats_[f.material_index].name.split(".")[0].endswith("_mk"))
        up = sum(f.calc_area() for f in fs if abs(f.normal.z) > 0.7)
        k = None
        for rl in RULES:                               # a rule of this stage (stage_rules.json) first
            x0, x1, y0, y1 = rl["caja"]
            if (min(xs) >= x0 - 5 and max(xs) <= x1 + 5 and min(ys) >= y0 - 5 and max(ys) <= y1 + 5
                    and len(fs) >= rl.get("min_tris", 0) and dz <= rl.get("max_dz", 1e9)):
                k = rl["nombre"]
                break
        if k:
            pass
        elif mk > 0.5 * area:
            k = "arbol"
        elif dz >= H and fw <= BOX_W:
            k = "edif"
        elif dz * 3 <= fw or up > 0.6 * area:
            k = "suelo"
        else:
            k = "otros"
        for f in fs:
            out[f.index] = k
    bm.free()
    return out

# 1. the backdrop: faces of loose parts that reach past FAR
bm = bmesh.new()
bm.from_mesh(src_obj.data)
bm.faces.ensure_lookup_table()
far_faces = set()
seen = set()
for f in bm.faces:
    if f.index in seen:
        continue
    stack, part = [f], []
    seen.add(f.index)
    while stack:
        g = stack.pop()
        part.append(g.index)
        for e in g.edges:
            for h in e.link_faces:
                if h.index not in seen:
                    seen.add(h.index)
                    stack.append(h)
    reach = max(max(abs(v.co.x), abs(v.co.y)) for i in part for v in bm.faces[i].verts)   # (Blender: Z up)
    if reach > FAR:
        far_faces.update(part)
bm.free()
fondo = split_faces(src_obj, lambda f: f.index in far_faces, "fondo")

# 2. cells
open_borders(src_obj)
CLS = classify(src_obj, far_faces) if int(opt.get("classes", 0)) else None
mats = src_obj.data.materials


MAXT = int(opt.get("max_tris", 1500))                 # a cell is split while it holds more triangles than this
MINC = float(opt.get("min_cell", 500))                # ... and is larger than this
MINT = int(opt.get("min_tris", 40))                   # cells with fewer triangles go to "dispersos" (no LOD)

bm = bmesh.new()
bm.from_mesh(src_obj.data)
ctr = {}
for f in bm.faces:
    if f.index not in far_faces:
        c = f.calc_center_median()
        ctr[f.index] = (c.x, c.y)                    # (Blender: the ground is X / Y)
bm.free()


def quad(ids, x0, y0, size, out, maxt=None, minc=None):
    """adaptive cells: a quadtree cell is split while it is crowded"""
    maxt = MAXT if maxt is None else maxt
    minc = MINC if minc is None else minc
    if len(ids) <= maxt or size <= minc:
        out.append((x0, y0, size, ids))
        return
    h = size / 2
    parts = [[], [], [], []]
    for i in ids:
        x, y = ctr[i]
        parts[(x >= x0 + h) + 2 * (y >= y0 + h)].append(i)
    for k, p in enumerate(parts):
        if p:
            quad(p, x0 + h * (k & 1), y0 + h * (k >> 1), h, out, maxt, minc)


xs = [c[0] for c in ctr.values()]
ys = [c[1] for c in ctr.values()]
x0, y0 = min(xs), min(ys)
size = max(max(xs) - x0, max(ys) - y0) + 1
GRP = int(opt.get("grp_tris", 0))                     # >0: hierarchical LOD - blocks of up to this many triangles
                                                      # hold one LOD2 (far); their small cells hold LOD1 (near)
small = set()
cells = []                                            # (x0, y0, size, ids, group)
groups = []                                           # (x0, y0, size, ids)
if GRP:
    quad(list(ctr), x0, y0, size, groups, GRP, MINC)
else:
    groups = [(x0, y0, size, list(ctr))]
for gi, g in enumerate(groups):
    cl = []
    quad(g[3], g[0], g[1], g[2], cl)
    for c in cl:
        if len(c[3]) < MINT:
            small.update(c[3])
        else:
            cells.append(c + (gi if GRP else -1,))

lod_root = bpy.data.objects.new("LODs", None)
occ_root = bpy.data.objects.new("Oclusores", None)
occ_root["w3d_solo_oclusion"] = True
for e in (lod_root, occ_root):
    bpy.context.scene.collection.objects.link(e)
stats = []
disp = split_faces(src_obj, lambda f: f.index in small, "dispersos") if small else None
if disp:
    disp["w3d_sin_lod"] = True
GL2, NBOX = [], []
if GRP:                                               # the blocks' LOD2: one decimation per block (few cut borders)
    for gi, (gx, gy, gsz, gids) in enumerate(groups):
        gfaces = {i for c in cells if c[4] == gi for i in c[3]}
        if not gfaces:
            continue
        gtag = "grupo_%03d" % gi
        gh = bpy.data.objects.new("GRP_" + gtag, None)
        gh["w3d_lod_distancias"] = [max(gsz * float(opt.get("g1f", 0.25)), float(opt.get("g1min", 300))), 0.0]
        gh["w3d_celda"] = [gx, gy, gsz]
        gh.parent = lod_root
        bpy.context.scene.collection.objects.link(gh)
        kinds = CLASSES if CLS else (None,)
        bx, nl2 = [], 0
        for kd in kinds:                               # one object per kind: never reduced together
            fs_ = gfaces if kd is None else {i for i in gfaces if CLS.get(i) == kd}
            if not fs_:
                continue
            g2 = split_faces(src_obj, lambda f, s=fs_: f.index in s, gtag + "_LOD2" + ("_" + kd if kd else ""))
            if g2 is None:
                continue
            if kd in (None, "edif"):
                bx += boxify(g2)
            n = weld_decimate(g2, R2, tol=TOL2, pin=PIN2) if len(g2.data.polygons) else 0
            nl2 += sum(len(p.vertices) - 2 for p in g2.data.polygons)
            g2.parent = gh
            if not len(g2.data.polygons):
                bpy.data.objects.remove(g2)
        if bx:                                         # the boxes: an object of their own ("<block>_LOD2_cajas")
            gb = bpy.data.objects.new(gtag + "_LOD2_cajas", bpy.data.meshes.new(gtag + "_cajas"))
            for m_ in src_obj.data.materials:
                gb.data.materials.append(m_)
            bm_ = bmesh.new()
            bm_.from_mesh(src_obj.data)
            bmesh.ops.delete(bm_, geom=list(bm_.verts), context="VERTS")
            bm_.to_mesh(gb.data)
            bm_.free()
            bpy.context.scene.collection.objects.link(gb)
            add_boxes(gb, bx)
            gb.parent = gh
        NBOX.append(len(bx))
        GL2.append(nl2 + 10 * len(bx))
for n, (cx, cy, csz, ids, grp) in enumerate(sorted(cells, key=lambda c: (c[4], c[1], c[0]))):
    faces = set(ids)
    tag = "celda_%03d" % n
    d1 = max(csz * float(opt.get("d1f", 1.5)), float(opt.get("d1min", 1500)))   # LOD0 up to d1 from the cell,
    d2 = max(csz * float(opt.get("d2f", 4.0)), float(opt.get("d2min", 5000)))   # LOD1 up to d2, then LOD2
    holder = bpy.data.objects.new("LOD_" + tag, None)
    holder["w3d_lod_distancias"] = [d1, d2]
    holder["w3d_celda"] = [cx, cy, csz]
    holder.parent = lod_root
    bpy.context.scene.collection.objects.link(holder)
    l0 = split_faces(src_obj, lambda f, s=faces: f.index in s, tag + "_LOD0")
    n0 = len(l0.data.polygons)
    l1s = []
    for kd in (CLASSES if CLS else (None,)):
        fs_ = faces if kd is None else {i for i in faces if CLS.get(i) == kd}
        o_ = split_faces(src_obj, lambda f, s=fs_: f.index in s, tag + "_LOD1" + ("_" + kd if kd else "")) if fs_ else None
        if o_:
            weld_decimate(o_, R1)
            l1s.append(o_)
    n1 = sum(len(o_.data.polygons) for o_ in l1s)
    l1 = None
    for o_ in l1s:
        o_.parent = holder
    if grp >= 0:                                      # (its LOD2 is its block's)
        holder["w3d_grupo"] = "grupo_%03d" % grp
        l2, n2 = None, 0
    else:
        l2 = split_faces(src_obj, lambda f, s=faces: f.index in s, tag + "_LOD2")
        n2 = weld_decimate(l2, R2, tol=TOL2, pin=PIN2)
    for o in (l0, l1, l2):
        if o:
            o.parent = holder
    def opaque_face(f, s=faces):
        if f.index not in s:
            return False
        m = mats[f.material_index] if f.material_index < len(mats) else None
        return m is None or m.name.split(".")[0].endswith("_op")    # stage_export.py: m_<texture>_op = opaque
    oc = split_faces(src_obj, opaque_face, tag + "_oclusor")
    no = 0
    if oc:
        no = weld_decimate(oc, RO, drop_materials=True, tol=1.0)
        oc.parent = occ_root
        oc["w3d_solo_oclusion"] = True
    stats.append((tag, n0, n1, n2, no))

bpy.data.objects.remove(src_obj)
if fondo:
    fondo["w3d_sin_lod"] = True
bpy.ops.export_scene.gltf(filepath=dst, export_format="GLTF_SEPARATE", export_extras=True, export_apply=True,
                          export_yup=True, export_texture_dir="")
t = [sum(s[i] for s in stats) for i in range(1, 5)]
t[2] += sum(GL2)
print("STAGE_LOD: %d blocks (LOD2 %d triangles, %d buildings as boxes)" % (len(GL2), sum(GL2), sum(NBOX)))
print("STAGE_LOD: %d cells (adaptive, <= %d triangles or %g units); triangles LOD0 %d, LOD1 %d, LOD2 %d, occluders %d; "
      "backdrop %d, scattered %d" % (len(stats), MAXT, MINC, t[0], t[1], t[2], t[3], len(fondo.data.polygons) if fondo else 0,
                                     len(disp.data.polygons) if disp else 0))
for s in stats:
    print("STAGE_LOD:   %s: %d / %d / %d, occluder %d" % s)
