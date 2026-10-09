#!/usr/bin/env python3
"""Gives a 2013-pack car a clean UV layout for liveries, and the sheets to paint on.

The pack's own livery atlas is a patchwork: sponsor logos are baked in, a third of the texture is
shared by several surfaces, and 1900-odd islands are scattered over it. Here the paint surfaces (the
body's livery material and the DRS flap) are mapped again as a blueprint: six orthographic views of
the car (left, right, top, bottom, front, rear) laid out on one sheet at the same scale, each
triangle going to the view it faces most. Where a view sees two surfaces on top of one another the
nearer one is the one on the car's outside; the hidden one just shares its paint.

For assets/cars/<id> (after tools/rig_drs.py) it writes:
    body.glb, drs_flap.glb  new UVs for the livery material, which now uses livery_default.png
    livery_default.png      the car's original colours on the new layout: a starting point
    livery_template.png     transparent: panel outlines, names, a 0.5 m grid, wheel circles and
                            the centre line, to put on top of the painting as a layer
    livery_guide.png        the same on solid colours, one hue per view. Save a copy as livery.png
                            to see where each view lands on the car (and how it stretches)
    livery_mask.png         red: a team colour replaces the paint; green: the part is plain black
                            (cockpit housing, shoulder covers, floor ...); the game paints with it
    livery_views.json       the layout: each view's panel on the sheet

To use a livery in the game, paint on livery_default.png (keep it opaque) and save it as
assets/cars/<id>/livery.png: viewer v2 loads it in place of the embedded paint.

Needs numpy and Pillow. From the repo root:

    python3 tools/unwrap_livery.py assets/cars/f1_2013_02 [--size 2048]
"""
import argparse
import colorsys
import io
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig_drs as R  # noqa: E402

# The views: the outward direction, and how a point (x, y, z) of the car (x to its left, y up, z
# forward) becomes (a, b), a to the right and b up as a person looking at that side sees it.
VIEWS = {
    "left":   ((1, 0, 0),  lambda p: (-p[..., 2], p[..., 1]), "LEFT SIDE"),
    "right":  ((-1, 0, 0), lambda p: (p[..., 2], p[..., 1]), "RIGHT SIDE"),
    "top":    ((0, 1, 0),  lambda p: (-p[..., 0], p[..., 2]), "TOP"),
    "bottom": ((0, -1, 0), lambda p: (p[..., 0], p[..., 2]), "BOTTOM"),
    "front":  ((0, 0, 1),  lambda p: (p[..., 0], p[..., 1]), "FRONT"),
    "rear":   ((0, 0, -1), lambda p: (-p[..., 0], p[..., 1]), "REAR"),
    "dark":   ((0, 0, 0), None, "BLACK PARTS"),
}
NAMES = [n for n in VIEWS if n != "dark"]
GUTTER = 0.08      # m between panels
PAD = 0.03         # m around a panel's triangles


def livery_prim(j, b, material_name):
    ps = [p for p in j["meshes"][0]["primitives"] if j["materials"][p["material"]]["name"] == material_name]
    assert len(ps) == 1, f"expected one primitive of {material_name}, found {len(ps)}"
    p = ps[0]
    at = p["attributes"]
    return dict(mat=p["material"],
                pos=R.accessor(j, b, at["POSITION"]).astype(np.float64),
                nrm=R.accessor(j, b, at["NORMAL"]).astype(np.float64),
                uv=R.accessor(j, b, at["TEXCOORD_0"]).astype(np.float64),
                idx=R.accessor(j, b, p["indices"]).astype(np.int64).reshape(-1, 3))


def sample(tex, uv):
    """Bilinear sample of an RGBA float image (H, W, 4) at uv (n, 2), wrapping, v down."""
    h, w = tex.shape[:2]
    x = (uv[:, 0] % 1.0) * w - 0.5
    y = (uv[:, 1] % 1.0) * h - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    g = lambda yy, xx: tex[yy % h, xx % w]
    return (g(y0, x0) * (1 - fx) * (1 - fy) + g(y0, x0 + 1) * fx * (1 - fy) +
            g(y0 + 1, x0) * (1 - fx) * fy + g(y0 + 1, x0 + 1) * fx * fy)


def bake(tris_new, tris_old, tex, size):
    """Repaints the old texture onto the new layout, the first triangle to reach a texel keeping it.
    tris_*: (T, 3, 2) uv, v down. Returns the image, which texels were painted and which triangle painted each."""
    out = np.zeros((size, size, 4), np.float64)
    hit = np.zeros((size, size), bool)
    owner = np.full((size, size), -1, np.int64)
    for ti, (tn, to) in enumerate(zip(tris_new, tris_old)):
        p = tn * size
        x0, x1 = int(max(0, math.floor(p[:, 0].min() - 1))), int(min(size - 1, math.ceil(p[:, 0].max() + 1)))
        y0, y1 = int(max(0, math.floor(p[:, 1].min() - 1))), int(min(size - 1, math.ceil(p[:, 1].max() + 1)))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        a, bb, c = p
        den = (bb[1] - c[1]) * (a[0] - c[0]) + (c[0] - bb[0]) * (a[1] - c[1])
        if abs(den) < 1e-9:
            continue
        l0 = ((bb[1] - c[1]) * (gx - c[0]) + (c[0] - bb[0]) * (gy - c[1])) / den
        l1 = ((c[1] - a[1]) * (gx - c[0]) + (a[0] - c[0]) * (gy - c[1])) / den
        l2 = 1 - l0 - l1
        eps = 0.02
        m = (l0 >= -eps) & (l1 >= -eps) & (l2 >= -eps)
        if not m.any():
            continue
        uv = l0[m][:, None] * to[0] + l1[m][:, None] * to[1] + l2[m][:, None] * to[2]
        ys, xs = gy[m].astype(int), gx[m].astype(int)
        fresh = ~hit[ys, xs]
        out[ys[fresh], xs[fresh]] = sample(tex, uv[fresh])
        hit[ys[fresh], xs[fresh]] = True
        owner[ys[fresh], xs[fresh]] = ti
    return out, hit, owner


def pad(img, hit, passes):
    """Spreads the painted colours outward `passes` texels (bleed), 8 neighbours at a time."""
    img = img.copy()
    hit = hit.copy()
    for _ in range(passes):
        acc = np.zeros_like(img)
        cnt = np.zeros(hit.shape, np.float64)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx or dy:
                    acc += np.roll(np.roll(img * hit[..., None], dy, 0), dx, 1)
                    cnt += np.roll(np.roll(hit, dy, 0), dx, 1)
        new = (~hit) & (cnt > 0)
        img[new] = acc[new] / cnt[new][:, None]
        hit = hit | new
    return img, hit


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("car_dirs", nargs="+", metavar="car_dir")
    ap.add_argument("--size", type=int, default=2048, help="the livery sheet's side in texels")
    ap.add_argument("--mask-threshold", type=float, default=0.2,
                    help="a surface whose original paint is darker than this (0..1 brightness, the middle value) is plain black")
    a = ap.parse_args()
    for car_dir in a.car_dirs:
        unwrap(car_dir, a.size, a.mask_threshold)


def team_colour(d):
    """The livery's main colour, for the HUD: the commonest saturated colour of the paint (the
    team colour rather than the silver or white around it), else the commonest bright one."""
    paint = np.array(Image.open(os.path.join(d, "livery_default.png")).convert("RGB"), np.int32)
    m3 = np.array(Image.open(os.path.join(d, "livery_mask.png")).convert("RGB"))
    mask = (m3[..., 0] > 127) | (m3[..., 1] > 127)     # all the paint, plain-black parts included
    px = paint[mask]
    mx, mn = px.max(1), px.min(1)
    sat = (mx - mn) / np.maximum(mx, 1)
    use = (sat > 0.4) & (mx > 50)
    if use.sum() < 0.02 * len(px):
        use = mx > 60
    px = px[use]
    key = (px[:, 0] // 24) * 100 + (px[:, 1] // 24) * 10 + (px[:, 2] // 24)
    best = np.bincount(key).argmax()
    col = px[key == best].mean(0)
    meta_path = os.path.join(d, "car.json")
    meta = json.load(open(meta_path, encoding="utf-8-sig"))
    meta["team_colour"] = [int(round(float(c))) for c in col]
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=4)
        f.write("\n")
    return meta["team_colour"]


def unwrap(d, size, a_thr):
    meta = json.load(open(os.path.join(d, "car.json"), encoding="utf-8-sig"))
    assert "drs" in meta, "run tools/rig_drs.py first (the flap shares the livery sheet)"
    mat_name = meta["source_material"]
    pivot = np.array(meta["drs"]["pivot"])

    jb, bb = R.read_glb(os.path.join(d, "body.glb"))
    jf, bf = R.read_glb(os.path.join(d, meta["drs"]["file"]))
    body, flap = livery_prim(jb, bb, mat_name), livery_prim(jf, bf, mat_name)
    mi = body["mat"]
    img = jb["images"][jb["textures"][jb["materials"][mi]["pbrMetallicRoughness"]["baseColorTexture"]["index"]]["source"]]
    if "uri" in img:       # unwrapped before: the paint is livery_default.png, on the layout the uvs describe
        old_tex = np.array(Image.open(os.path.join(d, img["uri"])).convert("RGBA"), np.float64)
    else:
        bv = jb["bufferViews"][img["bufferView"]]
        old_tex = np.array(Image.open(io.BytesIO(bb[bv.get("byteOffset", 0): bv.get("byteOffset", 0) + bv["byteLength"]])).convert("RGBA"), np.float64)

    # every paint triangle in the car's frame (the flap's vertices are relative to its hinge), body first
    cpos, cnrm, cold = [], [], []
    for p, off in ((body, np.zeros(3)), (flap, pivot)):
        cpos.append(p["pos"][p["idx"]] + off)
        cnrm.append(p["nrm"][p["idx"]])
        cold.append(p["uv"][p["idx"]])
    cpos, cnrm, cold = map(np.concatenate, (cpos, cnrm, cold))      # (T,3,3) (T,3,3) (T,3,2)
    T = len(cpos)
    n_body = len(body["idx"])
    geo = np.cross(cpos[:, 1] - cpos[:, 0], cpos[:, 2] - cpos[:, 0])
    vn = cnrm.mean(1)
    geo *= np.where(np.einsum("ij,ij->i", geo, vn) < 0, -1.0, 1.0)[:, None]      # outward, like the shading
    nlen = np.linalg.norm(geo, axis=1)
    facing = np.where(nlen[:, None] > 1e-12, geo / np.maximum(nlen, 1e-12)[:, None], vn / np.maximum(np.linalg.norm(vn, axis=1), 1e-12)[:, None])
    dirs = np.array([VIEWS[n][0] for n in NAMES], float)
    view = np.argmax(facing @ dirs.T, axis=1)                         # each triangle's view

    # Which surfaces are plain black (cockpit inner housing, shoulder covers, floor, diffuser ...): a surface
    # is dark when the middle of its original colours is, sampled inside the triangle (so a logo does not
    # make it dark or light); then dark and light are voted over each surface's neighbours (shared edges,
    # vertices welded by position, weighted by area) so there are no patches.
    bary = np.array([(1 / 3, 1 / 3, 1 / 3), (.6, .2, .2), (.2, .6, .2), (.2, .2, .6), (.8, .1, .1), (.1, .8, .1),
                     (.1, .1, .8), (.45, .45, .1), (.1, .45, .45), (.45, .1, .45)])
    lums = np.zeros((T, len(bary)))
    for k, w in enumerate(bary):
        c = sample(old_tex, (cold * w[None, :, None]).sum(1))
        lums[:, k] = (0.299 * c[:, 0] + 0.587 * c[:, 1] + 0.114 * c[:, 2]) / 255.0
    dark0 = (np.median(lums, axis=1) < a_thr).astype(float)
    _, wid = np.unique(np.round(cpos.reshape(-1, 3) / 1e-4).astype(np.int64), axis=0, return_inverse=True)
    wf = wid.ravel().reshape(-1, 3)
    edges = {}
    for t in range(T):
        for c in range(3):
            u, v = int(wf[t, c]), int(wf[t, (c + 1) % 3])
            if u != v:
                edges.setdefault((min(u, v), max(u, v)), []).append(t)
    nb = [[] for _ in range(T)]
    for lst in edges.values():
        for x in lst:
            nb[x].extend(y for y in lst if y != x)
    weight = np.maximum(0.5 * nlen, 1e-9)
    state = dark0.copy()
    for _ in range(6):
        nxt = state.copy()
        for t in range(T):
            if not nb[t]:
                continue
            tot = weight[t] * 1.5 + sum(weight[y] for y in nb[t])
            blk = weight[t] * 1.5 * state[t] + sum(weight[y] * state[y] for y in nb[t])
            nxt[t] = 1.0 if blk > 0.5 * tot else 0.0
        state = nxt
    isdark = state > 0.5

    # view coordinates (metres) of every corner, in the triangle's own view
    ab = np.zeros((T, 3, 2))
    for vi, n in enumerate(NAMES):
        m = view == vi
        pa, pb = VIEWS[n][1](cpos[m])
        ab[m, :, 0], ab[m, :, 1] = pa, pb
    # the plain-black surfaces leave their views (so they cannot cover paint that lies under them, like the
    # T-cam over the airbox) and share one small panel of their own, at a fifth of the scale
    ab[isdark] *= 0.2
    panel = {}
    for vi, n in enumerate(NAMES):
        m = (view == vi) & ~isdark
        if not m.any():
            continue
        lo, hi = ab[m].reshape(-1, 2).min(0) - PAD, ab[m].reshape(-1, 2).max(0) + PAD
        panel[n] = dict(lo=lo, size=hi - lo, tris=int(m.sum()))
    if isdark.any():
        lo, hi = ab[isdark].reshape(-1, 2).min(0) - PAD, ab[isdark].reshape(-1, 2).max(0) + PAD
        panel["dark"] = dict(lo=lo, size=hi - lo, tris=int(isdark.sum()))

    def members(n):
        return np.where(isdark)[0] if n == "dark" else np.where((view == NAMES.index(n)) & ~isdark)[0]

    # layout: the long views stand on end (sides and top and bottom, side by side), front and rear lie below them
    row1 = [n for n in ("left", "right", "top", "bottom") if n in panel]
    row2 = [n for n in ("front", "rear", "dark") if n in panel]
    for n in list(NAMES) + ["dark"]:
        if n in panel:
            panel[n]["turned"] = n in ("left", "right")      # turned: the car's up points right, a points down

    def extent(n):
        w, h = panel[n]["size"]
        return (h, w) if panel[n]["turned"] else (w, h)

    x = 0.0
    for n in row1:
        panel[n]["origin"] = (x, 0.0)
        x += extent(n)[0] + GUTTER
    h1 = max(extent(n)[1] for n in row1)
    x2 = 0.0
    for n in row2:
        panel[n]["origin"] = (x2, h1 + GUTTER)
        x2 += extent(n)[0] + GUTTER
    W = max(x, x2) - GUTTER
    H = h1 + (GUTTER + max(extent(n)[1] for n in row2) if row2 else 0)
    edge = 6
    s = (size - 2 * edge) / max(W, H)                                 # texels per metre

    def to_px(n, a_, b_):
        """View coordinates (m) -> texel position on the sheet."""
        p = panel[n]
        ua, ub = a_ - p["lo"][0], b_ - p["lo"][1]
        if p["turned"]:        # up points right, right points down
            x, y = p["origin"][0] + ub, p["origin"][1] + ua
        else:
            x, y = p["origin"][0] + ua, p["origin"][1] + (p["size"][1] - ub)
        return edge + x * s, edge + y * s

    new = np.zeros((T, 3, 2))
    for n in panel:
        m = members(n)
        if len(m):
            x, y = to_px(n, ab[m, :, 0], ab[m, :, 1])
            new[m, :, 0], new[m, :, 1] = x / size, y / size
    print(f"{T} paint triangles, {s / 1000:.3f} texels per mm ({1000 / s:.1f} mm a texel); "
          + ", ".join(f"{n} {panel[n]['tris']}" for n in panel))

    # the default paint: the old texture rebaked, nearest surfaces first
    towards = np.einsum("ij,ij->i", cpos.mean(1), dirs[view])
    order = np.argsort(-towards, kind="stable")
    baked, hit, owner = bake(new[order], cold[order], old_tex, size)
    padded, got = pad(baked, hit, 24)
    padded[~got] = baked[hit].mean(0)
    padded[..., 3] = 255
    default_png = Image.fromarray(np.clip(padded + 0.5, 0, 255).astype(np.uint8), "RGBA")
    default_png.save(os.path.join(d, "livery_default.png"))
    print(f"{100 * hit.mean():.1f}% of the sheet is paint surface")

    # the paint mask, two channels: red = a flat team colour replaces the paint, green = the part is plain
    # black (the surfaces classified above); neither = as it was (only texels no surface reaches)
    ids = owner[hit]
    tri_of = order[ids]                                            # triangle that painted each texel
    mask = np.zeros((size, size, 4))
    mask[hit, 0] = 1.0 - state[tri_of]
    mask[hit, 1] = state[tri_of]
    mask_p, got_m = pad(mask, hit, 24)
    rgb = np.zeros((size, size, 3), np.uint8)
    rgb[..., 0] = (mask_p[..., 0] > 0.5) & got_m
    rgb[..., 1] = (mask_p[..., 1] > 0.5) & got_m
    rgb *= 255
    Image.fromarray(rgb, "RGB").save(os.path.join(d, "livery_mask.png"))
    pt = state[tri_of]
    print(f"{100 * float(1 - pt.mean()):.1f}% of the paint surface takes a team colour, {100 * float(pt.mean()):.1f}% is plain black")

    # the template and the guide
    sc = 2
    font = ImageFont.truetype("arialbd.ttf", 34) if os.path.exists("C:/Windows/Fonts/arialbd.ttf") else ImageFont.load_default()
    small = ImageFont.truetype("arial.ttf", 18) if os.path.exists("C:/Windows/Fonts/arial.ttf") else ImageFont.load_default()
    hue = {n: tuple(int(255 * c) for c in colorsys.hsv_to_rgb(i / 6, 0.45, 0.97)) for i, n in enumerate(NAMES)}
    hue["dark"] = (150, 150, 150)
    tpl = Image.new("RGBA", (size * sc, size * sc), (0, 0, 0, 0))
    gde = Image.new("RGBA", (size * sc, size * sc), (28, 28, 32, 255))
    dt, dg = ImageDraw.Draw(tpl), ImageDraw.Draw(gde)

    def P(n, a_, b_):
        x, y = to_px(n, np.asarray(a_), np.asarray(b_))
        return float(x) * sc, float(y) * sc

    wheels = [(w["hub"], w["radius"]) for w in meta["wheels"].values()]
    for n in panel:
        p = panel[n]
        lo, hi = p["lo"], p["lo"] + p["size"]
        corners = [P(n, lo[0], lo[1]), P(n, hi[0], lo[1]), P(n, hi[0], hi[1]), P(n, lo[0], hi[1])]
        dg.polygon(corners, fill=hue[n] + (255,))
        # the surfaces
        for t in members(n):
            poly = [(float(x) * size * sc, float(y) * size * sc) for x, y in new[t]]
            dt.polygon(poly, fill=(255, 255, 255, 38))
            dg.polygon(poly, fill=tuple(int(c * 0.82) for c in hue[n]) + (255,))
        # grid every 0.5 m of the car's own coordinates
        for ax in (0, 1):
            k0, k1 = math.ceil(lo[ax] / 0.5), math.floor(hi[ax] / 0.5)
            for k in range(k0, k1 + 1):
                v = k * 0.5
                p0 = P(n, v, lo[1]) if ax == 0 else P(n, lo[0], v)
                p1 = P(n, v, hi[1]) if ax == 0 else P(n, hi[0], v)
                for dr, col in ((dt, (255, 255, 255, 90)), (dg, (0, 0, 0, 70))):
                    dr.line([p0, p1], fill=col, width=1 if k % 2 else 2)
        # wheels, the centre line and the ground, where the view shows them
        if n in ("left", "right"):
            for hub, r in wheels:
                a_ = -hub[2] if n == "left" else hub[2]
                cx, cy = P(n, a_, hub[1])
                rr = r * s * sc
                for dr in (dt, dg):
                    dr.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=(255, 255, 255, 255) if dr is dt else (0, 0, 0, 255), width=3)
            g0, g1 = P(n, lo[0], 0.0), P(n, hi[0], 0.0)
            for dr in (dt, dg):
                dr.line([g0, g1], fill=(255, 255, 255, 255) if dr is dt else (0, 0, 0, 255), width=3)
        if n in ("top", "bottom", "front", "rear"):
            c0, c1 = P(n, 0.0, lo[1]), P(n, 0.0, hi[1])
            for dr in (dt, dg):
                dr.line([c0, c1], fill=(255, 255, 255, 255) if dr is dt else (0, 0, 0, 255), width=3)
        # frame and name
        for dr, col in ((dt, (255, 255, 255, 255)), (dg, (0, 0, 0, 255))):
            dr.line(corners + [corners[0]], fill=col, width=4)
        note = {"left": ("nose at the top,", "car's top to the right"), "right": ("nose at the top,", "car's top to the right"),
                "top": ("nose up,", "car's right on the right"), "bottom": ("nose up,", "car's left on the right"),
                "front": ("car's left on the right,", ""), "rear": ("car's right on the right,", ""),
                "dark": ("plain black in game,", "a fifth of the scale")}[n]
        tx, ty = min(c[0] for c in corners) + 14, min(c[1] for c in corners) + 12
        for dr, fc, sk in ((dt, (255, 255, 255, 255), (0, 0, 0, 255)), (dg, (0, 0, 0, 255), (255, 255, 255, 255))):
            dr.text((tx, ty), VIEWS[n][2], font=font, fill=fc, stroke_width=3, stroke_fill=sk)
            dr.text((tx, ty + 44), note[0] + " " + note[1], font=small, fill=fc, stroke_width=2, stroke_fill=sk)
            dr.text((tx, ty + 68), "grid 0.5 m", font=small, fill=fc, stroke_width=2, stroke_fill=sk)
    tpl.resize((size, size), Image.LANCZOS).save(os.path.join(d, "livery_template.png"))
    gde.resize((size, size), Image.LANCZOS).convert("RGB").save(os.path.join(d, "livery_guide.png"))

    json.dump({"size": size, "texels_per_metre": round(s, 2),
               "views": {n: {"outward": list(VIEWS[n][0]), "origin_texels": [round(edge + panel[n]["origin"][0] * s, 1), round(edge + panel[n]["origin"][1] * s, 1)],
                             "size_m": [round(float(x), 3) for x in panel[n]["size"]], "turned": panel[n]["turned"], "triangles": panel[n]["tris"]}
                         for n in panel}}, open(os.path.join(d, "livery_views.json"), "w"), indent=1)

    # new GLBs: one vertex per corner, so each view keeps its own uv
    tex = {mat_name: "livery_default.png"}      # the glbs point at the file, so a repaint can replace it

    def corners_of(sl, off):
        n = sl.stop - sl.start
        return dict(pos=(cpos[sl] - off).reshape(-1, 3), nrm=cnrm[sl].reshape(-1, 3),
                    uv=new[sl].reshape(-1, 2).astype(np.float32), idx=np.arange(n * 3).reshape(-1, 3))

    others = []
    for p in jb["meshes"][0]["primitives"]:
        if jb["materials"][p["material"]]["name"] == mat_name:
            continue
        at = p["attributes"]
        others.append(dict(mat=p["material"], pos=R.accessor(jb, bb, at["POSITION"]).astype(np.float64),
                           nrm=R.accessor(jb, bb, at["NORMAL"]).astype(np.float64),
                           uv=R.accessor(jb, bb, at["TEXCOORD_0"]).astype(np.float32),
                           idx=R.accessor(jb, bb, p["indices"]).astype(np.int64).reshape(-1, 3)))
    bp = dict(corners_of(slice(0, n_body), np.zeros(3)), mat=mi)
    fp = dict(corners_of(slice(n_body, T), pivot), mat=flap["mat"])
    mats_b = R.write_glb(os.path.join(d, "body.glb"), jb, bb, others + [bp], "body", textures=tex)
    assert mats_b == meta["materials"], f"body materials changed: {mats_b}"
    R.write_glb(os.path.join(d, meta["drs"]["file"]), jf, bf, [fp], "drs_flap", translation=pivot,
                animation=(meta["drs"]["axis"], math.radians(meta["drs"]["max_angle_deg"])), textures=tex)
    print("wrote body.glb, drs_flap.glb, livery_default.png, livery_template.png, livery_guide.png, livery_views.json,"
          " team colour", team_colour(d))


if __name__ == "__main__":
    main()
