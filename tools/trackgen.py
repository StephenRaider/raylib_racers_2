#!/usr/bin/env python3
"""Builds tracks/*.trk from traced circuit maps or corner lists.

A traced layout ("ref") reads tools/track_refs/<ref>.json, made by tools/tracetrack.py
from a circuit map image. A corner-list layout ("verts") is a closed polygon of corners
in race order (clockwise, y up):
    (x, y, radius, width)   # comment naming the real corner it is inspired by
Every corner is rounded with an arc of that radius, joined by straights, then the
whole lap is scaled to the target length. `start` is a point on the main straight.
Run it from the repo root:

    python3 tools/trackgen.py              # writes tracks/<key>.trk
    python3 tools/trackgen.py --plot DIR   # also draws a 2D map of each track
"""
import math
import sys

# Inspired by the real circuits (their signature corners and straights), not copies.
TRACKS = {
    # Traced from F1 circuit maps (tools/tracetrack.py -> tools/track_refs/<key>.json),
    # then stretched and resized so they share the shape but are not copies.
    # labels: (x, y) pixel in the source map, corner name, track width there (m).
    "monza": dict(
        name="Autodromo Monzetta", scenery="parkland", length=5700, stretch=(1.0, 1.12), width=14, runoff=9, pitspeed=22,
        about="Temple of speed: long straights broken by chicanes, Curva Grande, the Lesmos, Ascari and the Parabolica.",
        ref="monza", start_width=15, bend=0.1, edits=[
            ("flatten", "Bosco", 70, 0.9),            # Roggia becomes a fast kink
            ("bulge", "Curvone", 160, 50),        # a rounder, longer Curva Grande
            ("chicane", "Ellisse", 90, 20, -420),   # a chicane on the back straight
            ("bulge", "Ellisse", 120, 35),          # a bigger Parabolica loop
        ], labels=[
            (767, 882, "Prima Variante", 11), (424, 843, "Curvone", 15), (358, 371, "Bosco", 12),
            (160, 116, "Lesmini 1", 14), (424, 32, "Lesmini 2", 12), (876, 654, "Serpente", 11),
            (1758, 664, "Ellisse", 17)]),
    "spa": dict(
        name="Ardennes Ring", scenery="forest", length=6900, stretch=(0.95, 1.08), width=13, runoff=9, pitspeed=22,
        about="Long and fast: La Source hairpin, the Eau Rouge-Raidillon kink, Kemmel straight, Pouhon and Blanchimont.",
        ref="spa", start_width=14, start_shift=70, bend=-0.1, edits=[
            ("flatten", "Le Kink", 90, 1.0),          # a fast kink instead of the Bus Stop
            ("flatten", "Blanchepierre", 160, 0.5),    # an easier flat-out left
            ("chicane", "Les Crêtes", 90, 25, -380),   # a chicane halfway along the long straight
            ("bulge", "Grand Virage", 180, -40),             # a longer, wider double left
            ("bulge", "Stavel", 90, 40),
        ], labels=[
            (279, 1026, "La Fontaine", 17), (595, 607, "Ruisseau", 12), (552, 488, "La Montee", 11),
            (1420, 35, "Les Crêtes", 14), (1588, 48, "Malvaux", 11), (1770, 356, "Rivière", 15),
            (1290, 405, "Grand Virage", 13), (1539, 675, "Bruyères", 10), (1680, 1053, "Stavel", 14),
            (1055, 685, "Blanchepierre", 14), (649, 820, "Le Kink", 15)]),
    "silverstone": dict(
        name="Silverfield", scenery="airfield", length=5800, stretch=(1.06, 0.95), width=15, runoff=10, pitspeed=22,
        about="Fast and flowing: Abbey, the Loop, Luffield, Copse and the Maggotts-Becketts-Chapel esses into Hangar straight.",
        ref="silverstone", start_width=16, start_shift=40, bend=0.1, edits=[
            ("bulge", "Lufton", 90, 50),             # a bigger loop at the far end
            ("chicane", "Stow", 100, 25, -450),       # a chicane on the long back straight
            ("bulge", "Stow", 90, -40),
            ("flatten", "Becks", 70, 0.6),          # gentler esses
        ], labels=[
            (969, 321, "Priory", 16), (1011, 665, "Hamlet", 13), (909, 740, "The Hook", 17),
            (1431, 325, "Brookfield", 14), (1308, 199, "Lufton", 18), (1681, 742, "Spinney", 15),
            (1202, 812, "Magpies", 13), (1140, 933, "Becks", 12), (903, 950, "Chantry", 14),
            (180, 460, "Stow", 17), (444, 188, "Dale", 12), (416, 129, "Clubhouse", 15)]),
    "hungaroring": dict(
        name="Magyar Park", scenery="hills", length=4380, stretch=(0.94, 1.06), width=12, runoff=8, pitspeed=22,
        about="Tight and twisty, hard to pass on: a long run to the turn 1 hairpin, then corner after corner.",
        ref="hungaroring", start_width=14, bend=-0.1, edits=[
            ("bulge", "T4-T5", 110, 60),               # a longer loop out to the far corner
            ("flatten", "T8", 60, 0.8),
            ("chicane", "T3", 90, 18, 260),            # a flick on the run to the far corner
            ("bulge", "T12", 70, 40),
        ], labels=[
            (613, 26, "T1", 15), (782, 463, "T2", 11), (850, 343, "T3", 12), (1515, 88, "T4-T5", 13),
            (1562, 445, "T6", 10), (1344, 563, "T8", 11), (1479, 691, "T9", 12), (1236, 782, "T10", 11),
            (1212, 998, "T11", 13), (836, 911, "T12", 10), (771, 663, "T13", 12), (668, 970, "T14", 14)]),
    "zandvoort": dict(
        name="Dunes of Zandhoek", scenery="dunes", length=4260, stretch=(1.08, 1.0), width=11, runoff=7, pitspeed=20,
        about="Narrow and compact among the dunes: the Tarzan hairpin, Hugenholtz, Scheivlak and the long banked final right.",
        ref="zandvoort", start_width=12, bend=0.1, edits=[
            ("flatten", "Haaks", 60, 0.9),        # no chicane: a quick right
            ("bulge", "Scheepsvlak", 110, 50),
            ("bulge", "Meesterbocht", 80, -40),
            ("chicane", "Bocht 10", 90, 18, 260),      # a flick on the long run back
        ], labels=[
            (1106, 1053, "Tarzanbocht", 14), (1137, 651, "Gerritsbocht", 10), (1253, 578, "Komkom", 13),
            (826, 597, "Duinrug", 10), (655, 678, "Slotbocht", 11), (307, 553, "Scheepsvlak", 12),
            (487, 203, "Meesterbocht", 10), (460, 480, "Bocht 10", 11), (1138, 373, "Haaks", 10),
            (1173, 47, "Kuipbocht", 14), (1515, 154, "Lange Bocht", 14)]),
    "sepang": dict(
        name="Kuala Speedway", scenery="tropical", length=5500, stretch=(1.0, 1.1), width=17, runoff=10, pitspeed=22,
        about="Wide and fast: the turn 1-2 hairpin complex, sweeping esses, and two long straights joined by a hairpin.",
        ref="sepang", start_width=18, bend=-0.08, edits=[
            ("bulge", "T9", 80, 50),                   # a looser hairpin
            ("bulge", "T1", 100, -50),                  # a more open turn 1
            ("flatten", "T5-T6", 90, 0.7),             # gentler esses
            ("chicane", "T15", 100, 25, -500),         # a chicane on the back straight
            ("bulge", "T3", 120, 50),
        ], labels=[
            (1777, 2200, "T1", 19), (2060, 1939, "T2", 15), (2030, 1210, "T3", 17), (3602, 748, "T4", 14),
            (4158, 1482, "T5-T6", 16), (5776, 2158, "T7", 15), (5609, 2601, "T8", 17),
            (3829, 2740, "T9", 14), (4489, 3050, "T10-T11", 16), (3217, 3168, "T12", 15),
            (2105, 2861, "T14", 18), (5141, 2174, "T15", 19)]),
    "brands": dict(
    name="Brands Lane", scenery="woodland", length=3900, width=11, pitspeed=20,
    about="Old-school and narrow: Paddock Hill Bend, the Druids hairpin, a blast through the woods to Hawthorn and back via Clearways.",
    start=(130,0), runoff=7, verts=[
        (420,0,100,12),                            # Paddock Hill
        (570,-70,22,11),(540,-140,22,11),          # Druids
        (450,-140,50,11),                          # Graham Hill
        (80,-180,70,11),                           # Surtees
        (150,-500,300,11),                         # Pilgrim's Drop
        (260,-900,80,11),                          # Hawthorn
        (100,-1050,70,10),                         # Westfield
        (-20,-960,100,10),(-60,-900,60,10),        # Dingle Dell, Sheene
        (-80,-560,40,10),                          # Stirlings
        (-200,-450,50,11),                         # Clearways
        (-140,0,120,12),                           # Clark
    ], names=['Paddock Hill', 'Druids', '', 'Graham Hill', 'Surtees', "Pilgrim's Drop", 'Hawthorn', 'Westfield', 'Dingle Dell', 'Sheene', 'Stirlings', 'Clearways', 'Clark']),
    # RR2's main test track: an original hill circuit with real elevation and banking.
    # Corners: (x, y, radius, width, height at the apex, bank in degrees, + for a right-hander).
    "highmoor": dict(
    name="Highmoor Ridge", scenery="hills", length=4600, width=13, pitspeed=20,
    about="Hill circuit: a plunge into turn 1, a climb to a banked summit hairpin, the corkscrew drop and a flat-out rise back to the line.",
    start=(300,0), runoff=9, verts=[
        (800,0,30,14, 8,3),                         # The Plunge, at the foot of the start straight
        (820,-180,45,13, 12,-3),                    # Mill, left
        (1000,-300,40,13, 18,3),                    # Mill, right
        (1020,-480,60,13, 26,-3),                   # Ridge, climbing
        (1300,-560,100,13, 36,4),                   # Long Ridge, uphill and blind
        (1330,-900,20,12, 44,8),                    # Summit hairpin, banked
        (1080,-760,35,12, 42,-4),                   # Corkscrew in, over the brow
        (960,-880,30,12, 22,5),                     # Corkscrew out, at the bottom of the drop
        (600,-900,25,12, 8,2),                      # Valley hairpin
        (640,-560,50,13, 12,-3),                    # Brook
        (380,-400,45,13, 12,4),                     # Chapel
        (100,-460,40,13, 10,3),                     # Hollow
        (-60,0,60,14, 22,3),                        # Highmoor, climbing onto the start straight
    ], rises=[(11, 0.5, 13)],
    names=['The Plunge', 'Mill', '', 'Ridge', 'Long Ridge', 'Summit', 'Corkscrew', '', 'Valley', 'Brook', 'Chapel', 'Hollow', 'Highmoor']),
}


def path(verts, start, step=2.0):
    """Points (x, y, width, in_corner) round the lap, beginning nearest `start`."""
    verts = [v[:4] for v in verts]
    n = len(verts)
    info = []
    for i, (x, y, r, w) in enumerate(verts):
        px, py = verts[i - 1][:2]
        nx, ny = verts[(i + 1) % n][:2]
        a1 = math.atan2(y - py, x - px)
        a2 = math.atan2(ny - y, nx - x)
        d = (a2 - a1 + math.pi) % (2 * math.pi) - math.pi
        info.append((a1, d, r * math.tan(abs(d) / 2)))
    pts = []
    for i, (x, y, r, w) in enumerate(verts):
        a1, d, t = info[i]
        px, py, _, pw = verts[i - 1]
        tp = info[i - 1][2]
        L = math.hypot(x - px, y - py)
        if tp + t > L + 1e-6:
            raise ValueError(f"corners {i - 1} and {i} are too close for their radii ({tp:.0f} + {t:.0f} > {L:.0f} m)")
        sx, sy = px + math.cos(a1) * tp, py + math.sin(a1) * tp
        ex, ey = x - math.cos(a1) * t, y - math.sin(a1) * t
        m = max(1, int((L - tp - t) / step))
        for k in range(m):
            f = k / m
            pts.append((sx + (ex - sx) * f, sy + (ey - sy) * f, pw + (w - pw) * f, False))
        sgn = 1 if d > 0 else -1
        cx = ex + math.cos(a1 + sgn * math.pi / 2) * r
        cy = ey + math.sin(a1 + sgn * math.pi / 2) * r
        a0 = a1 - sgn * math.pi / 2
        m = max(2, int(r * abs(d) / step))
        for k in range(m):
            a = a0 + d * k / m
            pts.append((cx + math.cos(a) * r, cy + math.sin(a) * r, w, True))
    turn = sum(i[1] for i in info)
    assert abs(abs(math.degrees(turn)) - 360) < 1, "the corners do not add up to one lap"
    j = min(range(len(pts)), key=lambda k: (pts[k][0] - start[0]) ** 2 + (pts[k][1] - start[1]) ** 2)
    return pts[j:] + pts[:j]


def arc_s(pts):
    s = [0.0]
    for a, b in zip(pts, pts[1:]):
        s.append(s[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    total = s[-1] + math.hypot(pts[0][0] - pts[-1][0], pts[0][1] - pts[-1][1])
    return s, total


def overlaps(pts, margin):
    """(s1, s2, gap) where two far-apart parts of the lap come closer than their widths plus margin."""
    s, total = arc_s(pts)
    P, S, bad = pts[::3], s[::3], []
    for i in range(len(P)):
        for j in range(i + 1, len(P)):
            ds = min(S[j] - S[i], total - (S[j] - S[i]))
            lim = (P[i][2] + P[j][2]) / 2 + margin
            if ds > lim * 3 and math.hypot(P[i][0] - P[j][0], P[i][1] - P[j][1]) < lim:
                bad.append((round(S[i]), round(S[j])))
    return bad


def traced(key, t, step=5.0):
    """Points (x, y, width, in_corner) from a traced map, plus (index, name) corner labels."""
    import json
    ref = json.load(open(f"tools/track_refs/{t['ref']}.json"))
    sx, sy = t.get("stretch", (1.0, 1.0))
    raw = [(x * sx, y * sy) for x, y in ref["points"]]
    pix = ref["pixels"]
    # resample evenly and scale to the target length
    seg = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(raw, raw[1:] + raw[:1])]
    k = t["length"] / sum(seg)
    raw = [(x * k, y * k) for x, y in raw]
    cum = [0.0]
    for d in seg[:-1]:
        cum.append(cum[-1] + d * k)
    n = int(t["length"] / step)
    out, src = [], []
    j = 0
    for i in range(n):
        sv = i * t["length"] / n
        while j + 1 < len(cum) and cum[j + 1] <= sv:
            j += 1
        a, b = raw[j], raw[(j + 1) % len(raw)]
        f = (sv - cum[j]) / (seg[j] * k)
        out.append((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f))
        src.append(j)
    # corner labels at the trace point nearest each map label; widths blend between them
    labels = []
    for lx, ly, name, w in t["labels"]:
        jj = min(range(len(pix)), key=lambda q: (pix[q][0] - lx) ** 2 + (pix[q][1] - ly) ** 2)
        idx = min(range(n), key=lambda q: abs(src[q] - jj))
        labels.append((idx, name, w))
    # smooth out tracing noise (a few metres of wobble reads as curvature to the drivers)
    for _ in range(t.get("smooth", 40)):
        out = [(0.25 * out[i - 1][0] + 0.5 * out[i][0] + 0.25 * out[(i + 1) % n][0],
                0.25 * out[i - 1][1] + 0.5 * out[i][1] + 0.25 * out[(i + 1) % n][1]) for i in range(n)]
    # make it our own: bend the whole lap a little, then reshape single corners
    bend = t.get("bend", 0.0)
    if bend:
        ys = [p[1] for p in out]
        mid, span = (max(ys) + min(ys)) / 2, (max(ys) - min(ys)) or 1
        out = [(x + bend * span * ((y - mid) / span) ** 2 * 4, y) for x, y in out]
    where = {name: i for i, name, _ in labels}
    for kind, name, half, *rest in t.get("edits", []):
        amount = rest[0] if rest else 0.0
        shift = rest[1] if len(rest) > 1 else 0.0
        c = where[name] + int(round(shift / step))
        h = max(2, int(round(half / step)))
        base = list(out)
        a, b = base[(c - h) % n], base[(c + h) % n]
        for q in range(-h, h + 1):
            i = (c + q) % n
            u = q / h
            bump = math.cos(u * math.pi / 2) ** 2
            p0, p1 = base[i - 1], base[(i + 1) % n]
            tx, ty = p1[0] - p0[0], p1[1] - p0[1]
            tl = math.hypot(tx, ty) or 1
            nx, ny = -ty / tl, tx / tl  # left of the direction of travel
            x, y = base[i]
            if kind == "bulge":  # push the corner sideways (metres, + = left)
                d = amount * bump
                out[i] = (x + nx * d, y + ny * d)
            elif kind == "chicane":  # add a left-right flick
                d = amount * math.sin(u * math.pi) * bump
                out[i] = (x + nx * d, y + ny * d)
            elif kind == "flatten":  # blend the corner towards the straight line across it
                f = (q + h) / (2 * h)
                cx, cy = a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f
                w = bump * (amount or 1.0)
                out[i] = (x + (cx - x) * w, y + (cy - y) * w)
    # open up corners tighter than min_radius (tracing sharpens hairpins) with local smoothing
    rmin = t.get("min_radius", 16.0)
    for _ in range(400):
        tight = set()
        for i in range(n):
            a, b, c = out[i - 2], out[i], out[(i + 2) % n]
            ab, bc, ca = math.dist(a, b), math.dist(b, c), math.dist(c, a)
            area = abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) / 2
            if area > 1e-9 and ab * bc * ca / (4 * area) < rmin:
                tight.update((i - 2) % n + q for q in range(5))
        if not tight:
            break
        for i in tight:
            i %= n
            a, b, c = out[i - 1], out[i], out[(i + 1) % n]
            out[i] = (b[0] * 0.5 + (a[0] + c[0]) * 0.25, b[1] * 0.5 + (a[1] + c[1]) * 0.25)
    k = t["length"] / sum(math.dist(a, b) for a, b in zip(out, out[1:] + out[:1]))
    out = [(x * k, y * k) for x, y in out]
    knots = sorted([(0, t["start_width"])] + [(i, w) for i, _, w in labels]) + [(n, t["start_width"])]
    def width(i):
        for (i0, w0), (i1, w1) in zip(knots, knots[1:]):
            if i0 <= i <= i1:
                f = (i - i0) / max(1, i1 - i0)
                return round(w0 + (w1 - w0) * (3 * f * f - 2 * f ** 3), 1)
        return t["width"]
    pts = []
    for i, (x, y) in enumerate(out):
        # in a corner when the point sits off the chord of the 80 m around it
        p0, p2 = out[i - 8], out[(i + 8) % n]
        cx, cy = p2[0] - p0[0], p2[1] - p0[1]
        off = abs(cx * (y - p0[1]) - cy * (x - p0[0])) / (math.hypot(cx, cy) or 1)
        pts.append((x, y, width(i), off > 4.0))
    # move the start line along the lap (metres) when the map's line leaves too little grid room
    sh = int(round(t.get("start_shift", 0) / step))
    pts = pts[sh:] + pts[:sh]
    x0, y0 = pts[0][0], pts[0][1]
    pts = [(x - x0, y - y0, w, c) for x, y, w, c in pts]
    return pts, [((i - sh) % n, name) for i, name, _ in labels]


def build(key, t):
    if "ref" in t:
        pts, labels = traced(key, t)
    else:
        pts = path(t["verts"], t["start"])
        k = t["length"] / arc_s(pts)[1]
        verts = [(v[0] * k, v[1] * k, v[2] * k) + tuple(v[3:]) for v in t["verts"]]
        pts = path(verts, (t["start"][0] * k, t["start"][1] * k))
        apex = [min(range(len(pts)), key=lambda q: (pts[q][0] - v[0]) ** 2 + (pts[q][1] - v[1]) ** 2) for v in verts]
        labels = [(a, name) for a, name in zip(apex, t["names"]) if name]
        if len(t["verts"][0]) > 4:
            pts = relief(t, verts, pts, apex)
    bad = overlaps(pts, 2 * t["runoff"] + 4)
    if bad:
        print(f"  warning: {key} comes close to itself at s = {bad[:3]}")
    return pts, labels


def relief(t, verts, pts, apex):
    """Adds height and bank to each point: (x, y, width, in_corner, h, bank).

    Each corner vertex carries (..., h, bank): the road height at its apex and its bank
    angle (degrees, + raises the left edge, so + for a right-hander). `rises` adds height
    knots on the straights: (after corner i, fraction of the way to the next, h).
    Heights follow a Hermite curve through the knots; banks fade in and out over
    `bank_fade` metres either side of each corner."""
    s, total = arc_s(pts)
    n = len(pts)
    knots = [(s[a], v[4]) for a, v in zip(apex, verts)]
    for i, f, h in t.get("rises", []):
        a, b = s[apex[i]], s[apex[(i + 1) % len(verts)]]
        knots.append(((a + ((b - a) % total) * f) % total, h))
    knots.sort()
    ext = [(k - total, h) for k, h in knots] + knots + [(k + total, h) for k, h in knots]
    def height(sv):
        j = max(q for q in range(len(ext)) if ext[q][0] <= sv)
        (sp, hp), (s0, h0), (s1, h1), (sn, hn) = ext[j - 1], ext[j], ext[j + 1], ext[j + 2]
        L = s1 - s0
        m0 = (h1 - hp) / (s1 - sp) * L
        m1 = (hn - h0) / (sn - s0) * L
        u = (sv - s0) / L
        u2, u3 = u * u, u * u * u
        return (2 * u3 - 3 * u2 + 1) * h0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * h1 + (u3 - u2) * m1
    # corner runs (contiguous in_corner points), each owned by the vertex whose apex it holds
    runs, i0 = [], next(i for i in range(n) if not pts[i][3])
    q = 0
    while q < n:
        i = (i0 + q) % n
        if pts[i][3]:
            e = q
            while e < n and pts[(i0 + e) % n][3]:
                e += 1
            idx = [(i0 + z) % n for z in range(q, e)]
            owner = next((vi for vi, a in enumerate(apex) if a in idx), None)
            if owner is None:
                owner = min(range(len(apex)), key=lambda vi: min(abs(apex[vi] - z) for z in idx))
            runs.append((s[idx[0]], s[idx[-1]], verts[owner][5]))
            q = e
        else:
            q += 1
    fade = t.get("bank_fade", 50.0)
    def bank(sv):
        best, val = 0.0, 0.0
        for a, b, deg in runs:
            if (sv - a) % total <= (b - a) % total:
                d = 0.0
            else:
                d = min((a - sv) % total, (sv - b) % total)
            w = max(0.0, 1 - d / fade)
            w = w * w * (3 - 2 * w)
            if w > best:
                best, val = w, deg * w
        return val
    return [p[:4] + (height(sv), bank(sv)) for p, sv in zip(pts, s)]


def write(key, t, pts):
    s, total = arc_s(pts)
    # the straight the start line sits on: from the last corner exit to the first corner entry
    ahead = next(s[i] for i in range(len(pts)) if pts[i][3])
    behind = total - next(s[i] for i in range(len(pts) - 1, 0, -1) if pts[i][3])
    # pit lane on the right (the inside: the tracks run clockwise), around the start line
    entry = -min(behind - 20, 300)
    lane_start = entry + 60
    lane_end = min(ahead - 80, 300)
    exit_s = lane_end + 60
    lines = [
        f"# Raylib Racers: {t['name']}, {total / 1000:.2f} km. {t['about']}",
        "# Generated by tools/trackgen.py (edit the layout there, not here).",
        f"name {t['name']}",
        f"scenery {t['scenery']}",
        f"width {t['width']}",
        f"runoff {t['runoff']}",
        f"pit right {entry % total:.0f} {lane_start % total:.0f} {lane_end:.0f} {exit_s:.0f}",
        f"pitspeed {t['pitspeed']}",
    ]
    acc = 24.0
    for a, b in zip([pts[-1]] + pts, pts):
        acc += math.hypot(b[0] - a[0], b[1] - a[1])
        if acc >= (9.5 if b[3] else 24):  # denser control points in corners
            relief = f" h={b[4]:.2f} bank={b[5]:.1f}" if len(b) > 4 else ""
            lines.append(f"p {b[0]:.1f} {b[1]:.1f} {b[2]:g}{relief}")
            acc = 0.0
    with open(f"tracks/{key}.trk", "w") as f:
        f.write("\n".join(lines) + "\n")
    return total, (entry % total, lane_start % total, lane_end, exit_s)


def plot(key, t, pts, labels, pit, outdir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.collections import LineCollection
    s, total = arc_s(pts)
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    fig, ax = plt.subplots(figsize=(7, 6), dpi=110)
    fig.patch.set_facecolor("#14161c")
    ax.set_facecolor("#14161c")
    loop = pts + pts[:1]
    segs = [[a[:2], b[:2]] for a, b in zip(loop, loop[1:])]
    # line width proportional to the real track width so narrow and wide parts show
    ax.add_collection(LineCollection(segs, colors="#c8ccd6", linewidths=[(a[2] - 6) * 0.7 for a in loop[1:]],
                                     capstyle="round"))
    def at(v):
        v %= total
        return pts[min(range(len(s)), key=lambda i: abs(s[i] - v))]
    ent, ls, le, ex = pit
    lane = [p for p, sv in zip(pts, s) if sv <= le or sv >= ls]
    lane = sorted(lane, key=lambda p: (s[pts.index(p)] - ls) % total)
    side = [(p[0] + 0, p[1] + 0) for p in lane]
    ax.plot([p[0] for p in side], [p[1] for p in side], color="#ff8030", linewidth=1.0, alpha=0.9, label="pit lane")
    ax.plot(xs[0], ys[0], "s", color="#ff4040", markersize=7)
    ax.annotate("", xy=at(120)[:2], xytext=pts[0][:2], arrowprops=dict(arrowstyle="->", color="#ff4040", lw=1.6))
    # corner names next to their apex, pushed away from the lap centre
    cx, cy = sum(xs) / len(xs), sum(ys) / len(ys)
    span = max(max(xs) - min(xs), max(ys) - min(ys))
    for i, name in labels:
        x, y = pts[i][0], pts[i][1]
        dx, dy = x - cx, y - cy
        d = math.hypot(dx, dy) or 1
        ax.text(x + dx / d * span * 0.05, y + dy / d * span * 0.05, name, color="#8fc8ff", fontsize=7,
                ha="center", va="center")
    ax.set_aspect("equal")
    ax.margins(0.08)
    ax.axis("off")
    ax.set_title(f"{t['name']}   {total / 1000:.2f} km, {min(p[2] for p in pts):g}-{max(p[2] for p in pts):g} m wide",
                 color="white", fontsize=11, pad=16)
    x0, y0 = min(xs), min(ys) - span * 0.06
    ax.plot([x0, x0 + 500], [y0, y0], color="white", linewidth=2)
    ax.text(x0 + 250, y0 - span * 0.035, "500 m", color="white", ha="center", fontsize=7)
    fig.savefig(f"{outdir}/{key}.png", bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)
    if len(pts[0]) > 4:  # elevation and bank along the lap
        fig, ax = plt.subplots(figsize=(9, 3), dpi=110)
        fig.patch.set_facecolor("#14161c")
        ax.set_facecolor("#14161c")
        ax.fill_between(s, [p[4] for p in pts], min(p[4] for p in pts) - 5, color="#3a4a3a")
        ax.plot(s, [p[4] for p in pts], color="#c8ccd6", lw=1.5)
        ax2 = ax.twinx()
        ax2.plot(s, [p[5] for p in pts], color="#ff8030", lw=1, alpha=0.8)
        ax2.set_ylim(-30, 30)
        for i, name in labels:
            ax.text(s[i], pts[i][4] + 2, name, color="#8fc8ff", fontsize=7, ha="center", rotation=60)
        for a, c in ((ax, "white"), (ax2, "#ff8030")):
            a.tick_params(colors=c, labelsize=7)
        ax.set_xlabel("distance (m)", color="white", fontsize=8)
        ax.set_ylabel("height (m)", color="white", fontsize=8)
        ax2.set_ylabel("bank (deg)", color="#ff8030", fontsize=8)
        grade = [(pts[(i + 1) % len(pts)][4] - pts[i - 1][4]) / max(1e-3, s[(i + 1) % len(pts)] - s[i - 1])
                 for i in range(1, len(pts) - 1)]
        ax.set_title(f"{t['name']} profile: {max(p[4] for p in pts) - min(p[4] for p in pts):.0f} m relief, "
                     f"grades {min(grade) * 100:+.0f}% to {max(grade) * 100:+.0f}%", color="white", fontsize=10)
        fig.savefig(f"{outdir}/{key}_profile.png", bbox_inches="tight", facecolor=fig.get_facecolor())
        plt.close(fig)


def main():
    outdir = sys.argv[sys.argv.index("--plot") + 1] if "--plot" in sys.argv else None
    keys = [a for a in sys.argv[1:] if a in TRACKS] or list(TRACKS)
    for key in keys:
        t = TRACKS[key]
        pts, labels = build(key, t)
        total, pit = write(key, t, pts)
        print(f"{key:12s} {total:6.0f} m  pit {pit[0]:.0f}-{pit[3]:.0f}, lane {(pit[2] - pit[1]) % total:.0f} m,"
              f" grid room {total - pit[0] + 20:.0f} m")
        if outdir:
            plot(key, t, pts, labels, pit, outdir)


if __name__ == "__main__":
    main()

