#!/usr/bin/env python3
"""Splits the DRS flap (the rear wing's upper element) off a 2013-pack car and rigs it.

For each car folder (assets/cars/<id> in the 2013 pack layout: body.glb, car.json, ...):
  - finds the flap: of the wide islands behind the rear axle, the one that reaches highest,
    plus the small end caps that sit inside its side profile;
  - writes it to drs_flap.glb as one node "drs_flap" whose origin is the hinge, so the flap
    opens by rotating the node about the hinge axis, and removes it from body.glb;
  - picks the opening angle that takes the slot gap (the narrowest distance between the main
    plane and the flap, in side view) to 50 mm, the 2013 limit (Technical Regulations 3.18.3),
    then turns it EXTRA_OPEN_DEG further, which reads better on screen with this pack's short flaps;
  - adds a "drs" block to car.json: file, node, pivot, axis, max_angle_deg, gaps.

drs_flap.glb also carries a glTF animation "drs_open" whose time is the open amount: t = 0 s
closed, t = 1 s fully open, so tools that play glTF animations (Blender, three.js) can drive it.

Needs numpy and scipy. Run from the repo root:

    python3 tools/rig_drs.py assets/cars/f1_2013_02 [more car folders...]

A car whose car.json already has a "drs" block is left alone.
"""
import json
import math
import os
import struct
import sys

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial import cKDTree

GAP_OPEN = 0.050          # m: the 2013 limit on the slot gap with the flap open
EXTRA_OPEN_DEG = 10.0     # degrees past that, by eye (2026-10-09)
CAP_MARGIN = 0.035        # m: how far an end cap may stick out of the flap's side profile
CT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


# --- GLB in and out -------------------------------------------------------------------------

def read_glb(path):
    f = open(path, "rb").read()
    cl = struct.unpack_from("<I", f, 12)[0]
    j = json.loads(f[20:20 + cl])
    off = 20 + cl
    bl = struct.unpack_from("<I", f, off)[0]
    return j, f[off + 8: off + 8 + bl]


def accessor(j, b, i):
    a = j["accessors"][i]
    bv = j["bufferViews"][a["bufferView"]]
    dt = np.dtype(CT[a["componentType"]])
    n = NC[a["type"]]
    start = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride", 0)
    if stride and stride != dt.itemsize * n:
        raw = np.frombuffer(b, dtype=np.uint8, count=stride * a["count"], offset=start).reshape(-1, stride)
        arr = raw[:, :dt.itemsize * n].copy().view(dt)
    else:
        arr = np.frombuffer(b, dtype=dt, count=a["count"] * n, offset=start).copy()
    return arr.reshape(a["count"], n) if n > 1 else arr


def load_prims(j, b):
    """The body's primitives (one node at the origin, as the pack splitter writes them)."""
    node = j["nodes"][0]
    assert not any(k in node for k in ("rotation", "scale", "translation", "matrix")), "body node must be at the origin"
    prims = []
    for p in j["meshes"][node["mesh"]]["primitives"]:
        at = p["attributes"]
        prims.append(dict(mat=p["material"],
                          pos=accessor(j, b, at["POSITION"]).astype(np.float64),
                          nrm=accessor(j, b, at["NORMAL"]).astype(np.float64),
                          uv=accessor(j, b, at["TEXCOORD_0"]).astype(np.float32),
                          idx=accessor(j, b, p["indices"]).astype(np.int64).reshape(-1, 3)))
    return prims


def islands(prim, weld=1e-4):
    """A geometric island id per triangle (vertices welded by position)."""
    key = np.round(prim["pos"] / weld).astype(np.int64)
    _, wid = np.unique(key, axis=0, return_inverse=True)
    t = wid.ravel()[prim["idx"]]
    n = t.max() + 1
    r = np.concatenate([t[:, 0], t[:, 1], t[:, 2]])
    c = np.concatenate([t[:, 1], t[:, 2], t[:, 0]])
    _, lab = connected_components(coo_matrix((np.ones(len(r), np.int8), (r, c)), shape=(n, n)), directed=False)
    return lab[t[:, 0]]


def subset(prim, mask, offset=None):
    idx = prim["idx"][mask]
    used, inv = np.unique(idx.ravel(), return_inverse=True)
    pos = prim["pos"][used]
    if offset is not None:
        pos = pos - offset
    return dict(mat=prim["mat"], pos=pos, nrm=prim["nrm"][used], uv=prim["uv"][used], idx=inv.reshape(-1, 3))


def write_glb(path, src, src_bin, parts, node_name, translation=None, animation=None, textures=None):
    """One node (at `translation`) with one mesh of `parts`; materials and their textures are
    copied from `src`, in the source's order. animation: (axis, max angle in rad) -> "drs_open".
    textures: {source material name: PNG bytes, or a file name to reference} replaces that material's base colour image."""
    used = sorted({p["mat"] for p in parts})
    out = {"asset": {"version": "2.0", "generator": "RR2 rig_drs"},
           "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"name": node_name, "mesh": 0}],
           "meshes": [{"name": node_name, "primitives": []}],
           "accessors": [], "bufferViews": [], "buffers": [],
           "materials": [], "textures": [], "images": [], "samplers": src.get("samplers", [])}
    if translation is not None:
        out["nodes"][0]["translation"] = [float(x) for x in translation]
    if src.get("extensionsUsed"):
        out["extensionsUsed"] = src["extensionsUsed"]
    blob = bytearray()

    def add_view(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        bv = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
        if target:
            bv["target"] = target
        blob.extend(data)
        out["bufferViews"].append(bv)
        return len(out["bufferViews"]) - 1

    def add_acc(arr, ctype, typ, target=None, minmax=False):
        a = {"bufferView": add_view(arr.tobytes(), target), "componentType": ctype, "count": int(arr.shape[0]), "type": typ}
        if minmax:
            a["min"] = np.atleast_1d(arr.min(0)).tolist()
            a["max"] = np.atleast_1d(arr.max(0)).tolist()
        out["accessors"].append(a)
        return len(out["accessors"]) - 1

    img_map, tex_map, mat_map = {}, {}, {}
    for m in used:
        md = json.loads(json.dumps(src["materials"][m]))
        pbr = md.get("pbrMetallicRoughness", {})
        for holder, key in [(pbr, "baseColorTexture"), (pbr, "metallicRoughnessTexture"),
                            (md, "normalTexture"), (md, "occlusionTexture"), (md, "emissiveTexture")]:
            if key not in holder:
                continue
            ti = holder[key]["index"]
            if ti not in tex_map:
                t = dict(src["textures"][ti])
                si = t["source"]
                if si not in img_map:
                    im = src["images"][si]
                    repl = textures.get(md["name"]) if textures and key == "baseColorTexture" else None
                    if isinstance(repl, str):      # a file next to the glb, not embedded
                        out["images"].append({"uri": repl, "name": im.get("name", "")})
                    else:
                        if repl is None:
                            bv = src["bufferViews"][im["bufferView"]]
                            repl = src_bin[bv.get("byteOffset", 0): bv.get("byteOffset", 0) + bv["byteLength"]]
                        out["images"].append({"bufferView": add_view(bytes(repl)), "mimeType": im.get("mimeType", "image/png"), "name": im.get("name", "")})
                    img_map[si] = len(out["images"]) - 1
                t["source"] = img_map[si]
                out["textures"].append(t)
                tex_map[ti] = len(out["textures"]) - 1
            holder[key]["index"] = tex_map[ti]
        out["materials"].append(md)
        mat_map[m] = len(out["materials"]) - 1

    for p in parts:
        if len(p["idx"]) == 0:
            continue
        out["meshes"][0]["primitives"].append({"attributes": {
            "POSITION": add_acc(p["pos"].astype(np.float32), 5126, "VEC3", 34962, True),
            "NORMAL": add_acc(p["nrm"].astype(np.float32), 5126, "VEC3", 34962),
            "TEXCOORD_0": add_acc(p["uv"].astype(np.float32), 5126, "VEC2", 34962)},
            "indices": add_acc(p["idx"].astype(np.uint32).ravel(), 5125, "SCALAR", 34963), "material": mat_map[p["mat"]]})

    if animation is not None:
        axis, angle = animation
        times = np.linspace(0, 1, 5, dtype=np.float32)
        quats = np.array([[*(np.asarray(axis) * math.sin(angle * t / 2)), math.cos(angle * t / 2)] for t in times], np.float32)
        out["animations"] = [{"name": "drs_open",
                              "samplers": [{"input": add_acc(times, 5126, "SCALAR", minmax=True),
                                            "output": add_acc(quats, 5126, "VEC4"), "interpolation": "LINEAR"}],
                              "channels": [{"sampler": 0, "target": {"node": 0, "path": "rotation"}}]}]
    while len(blob) % 4:
        blob.append(0)
    out["buffers"] = [{"byteLength": len(blob)}]
    js = json.dumps(out, separators=(",", ":")).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack("<II", len(blob), 0x004E4942))
        f.write(blob)
    return [src["materials"][m]["name"] for m in used]


# --- finding and rigging the flap -----------------------------------------------------------

def profile_edges(parts, n=12):
    """Points along every triangle edge, in side view (y, z)."""
    pts = []
    for p in parts:
        v = p["pos"][p["idx"]][:, :, 1:]                       # (T, 3, 2)
        for a, b in ((0, 1), (1, 2), (2, 0)):
            for s in np.linspace(0, 1, n):
                pts.append(v[:, a] * (1 - s) + v[:, b] * s)
    return np.concatenate(pts)


def rot_yz(pts, pivot, angle):
    """Side-view points (y, z) turned about pivot by angle about +X (right-handed: y towards z)."""
    c, s = math.cos(angle), math.sin(angle)
    d = pts - pivot
    return np.stack([d[:, 0] * c - d[:, 1] * s, d[:, 0] * s + d[:, 1] * c], 1) + pivot


def rig(car_dir):
    meta_path = os.path.join(car_dir, "car.json")
    meta = json.load(open(meta_path, encoding="utf-8-sig"))
    if "drs" in meta:
        print(f"{car_dir}: already rigged")
        return
    j, b = read_glb(os.path.join(car_dir, "body.glb"))
    prims = load_prims(j, b)
    rear_z = min(w["hub"][2] for w in meta["wheels"].values())

    labs = [islands(p) for p in prims]
    isl = []   # (prim, label, min, max)
    for pi, (p, lab) in enumerate(zip(prims, labs)):
        for k in range(lab.max() + 1):
            v = p["pos"][np.unique(p["idx"][lab == k])]
            isl.append((pi, k, v.min(0), v.max(0)))
    # the wing elements: wide, behind the rear axle, up high; the flap reaches highest
    wide = [i for i in isl if i[3][0] - i[2][0] > 0.4 and i[2][2] < rear_z - 0.15 and i[3][2] < rear_z + 0.1 and i[3][1] > 0.7]
    wide.sort(key=lambda i: i[3][1])
    assert len(wide) >= 2, f"{car_dir}: no rear-wing elements found"
    flap, main = wide[-1], wide[-2]
    fmn, fmx = flap[2], flap[3]
    # end caps and tabs: narrow islands inside the flap's span and side profile
    sel = [(flap[0], flap[1])]
    for pi, k, mn, mx in isl:
        if (pi, k) == (flap[0], flap[1]) or mx[0] - mn[0] > 0.4:
            continue
        if (mn[0] >= fmn[0] - 0.005 and mx[0] <= fmx[0] + 0.005 and
                mn[1] >= fmn[1] - CAP_MARGIN and mx[1] <= fmx[1] + CAP_MARGIN and
                mn[2] >= fmn[2] - CAP_MARGIN and mx[2] <= fmx[2] + CAP_MARGIN):
            sel.append((pi, k))
    masks = [np.zeros(len(p["idx"]), bool) for p in prims]
    for pi, k in sel:
        masks[pi] |= labs[pi] == k

    flap_parts = [subset(p, m) for p, m in zip(prims, masks) if m.any()]
    main_parts = [subset(p, labs[main[0]] == main[1]) for p in [prims[main[0]]]]
    surf = subset(prims[flap[0]], labs[flap[0]] == flap[1])
    sv = surf["pos"][:, 1:]                                    # (y, z)
    # hinge: the trailing edge, the flap's highest and rearmost point (furthest along up-and-back)
    te = sv[np.argmax(sv[:, 0] - sv[:, 1])]
    near = sv[np.linalg.norm(sv - te, axis=1) < 0.012]
    pivot_yz = near.mean(0)
    le = sv[np.argmax(np.linalg.norm(sv - pivot_yz, axis=1))]  # leading edge: furthest from the hinge
    chord = float(np.linalg.norm(le - pivot_yz))
    # opening lifts the leading edge: about +X if that raises it, else about -X
    sign = 1.0 if rot_yz(le[None], pivot_yz, 0.01)[0, 0] > le[0] else -1.0

    fl_pts = profile_edges([subset(prims[flap[0]], labs[flap[0]] == flap[1])])
    tree = cKDTree(profile_edges(main_parts))

    def gap(angle):
        return float(tree.query(rot_yz(fl_pts, pivot_yz, sign * angle))[0].min())

    closed = gap(0.0)
    lo, hi = 0.0, math.radians(45)
    assert gap(hi) > GAP_OPEN, f"{car_dir}: 45 degrees does not open a {GAP_OPEN * 1000:.0f} mm gap"
    for _ in range(40):
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if gap(mid) < GAP_OPEN else (lo, mid)
    angle_rule = (lo + hi) / 2
    angle = angle_rule + math.radians(EXTRA_OPEN_DEG)

    pivot = np.array([(fmn[0] + fmx[0]) / 2, pivot_yz[0], pivot_yz[1]])
    axis = [sign, 0.0, 0.0]
    # write the flap (about its hinge) and the body without it
    flap_local = [subset(p, m, pivot) for p, m in zip(prims, masks) if m.any()]
    flap_mats = write_glb(os.path.join(car_dir, "drs_flap.glb"), j, b, flap_local, "drs_flap",
                          translation=pivot, animation=(axis, angle))
    body_parts = [subset(p, ~m) for p, m in zip(prims, masks) if (~m).any()]
    body_mats = write_glb(os.path.join(car_dir, "body.glb"), j, b, body_parts, "body")
    assert body_mats == meta["materials"], f"{car_dir}: body materials changed {body_mats}"

    meta["drs"] = {
        "file": "drs_flap.glb",
        "node": "drs_flap",
        "pivot": [round(float(x), 4) for x in pivot],
        "axis": axis,
        "max_angle_deg": round(math.degrees(angle), 2),
        "chord": round(chord, 4),
        "rule_angle_deg": round(math.degrees(angle_rule), 2),
        "slot_gap_closed_mm": round(closed * 1000, 1),
        "slot_gap_open_mm": round(gap(angle) * 1000, 1),
        "materials": flap_mats,
    }
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=4)
        f.write("\n")
    tris = sum(len(p["idx"]) for p in flap_local)
    print(f"{meta['id']}: flap {tris} tris in {len(sel)} islands, chord {chord * 1000:.0f} mm, "
          f"hinge y {pivot[1]:.3f} z {pivot[2]:.3f}, gap {closed * 1000:.1f} -> {gap(angle) * 1000:.1f} mm "
          f"at {math.degrees(angle):.1f} deg")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for d in sys.argv[1:]:
        rig(d)
