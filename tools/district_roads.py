"""The streets of the district as a designed surface (used by tools/build_district.py).

The raw heightmap is a surface model with the city's bias in it, and the old pipeline draped every street on whatever the terrain
was. That is what made roads dive into canals, ride bumps, and run into buildings. Here the streets are designed first and
everything else adapts to them:

  fit_cross_sections   each street gets the width the real buildings leave it (asphalt + sidewalks), not a table value
  fix_buildings        footprints that still overlap a street are pushed out of it or dropped
  split_bridges        where a street crosses water the edge is cut and the middle piece becomes a bridge (flag 4)
  design_roads         a smooth height profile per street (grade limited, shared at junctions), level across
  stamp_roads          that profile is baked into the ground patch, with a slope limited blend into the natural ground
  place_buildings      the ground floor of each building follows the street in front of it; the ground adapts to it
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from scipy.spatial import cKDTree

import district_osm as osm

resample = osm.resample
VEHICLE = 6  # classes 0..6 are streets for vehicles; 7 pedestrian, 8 cycleway, 9 footway, 10 steps


def smooth01(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def smooth_path(pts, iterations=2):
    """Chaikin corner cutting: paths mapped as a few straight pieces become the curves they are (the ends stay put)."""
    if len(pts) < 3:
        return pts
    for _ in range(iterations):
        out = [pts[0]]
        for a, b in zip(pts[:-1], pts[1:]):
            out.append((a[0] * 0.75 + b[0] * 0.25, a[1] * 0.75 + b[1] * 0.25))
            out.append((a[0] * 0.25 + b[0] * 0.75, a[1] * 0.25 + b[1] * 0.75))
        out.append(pts[-1])
        pts = out
    return pts


# ---------------------------------------------------------------------------------------------------------------------
# Cross sections that fit the buildings
# ---------------------------------------------------------------------------------------------------------------------

def building_raster(grid1, buildings):
    cv = grid1.canvas()
    d = ImageDraw.Draw(cv)
    for b in buildings:
        d.polygon([grid1.px(p) for p in b["rings"][0]], fill=255)
        for h in b["rings"][1:]:
            d.polygon([grid1.px(p) for p in h], fill=0)
    return np.asarray(cv) > 0


def _tangents(P):
    T = np.gradient(P, axis=0) if len(P) > 1 else np.array([[1.0, 0.0]])
    n = np.linalg.norm(T, axis=1, keepdims=True)
    return T / np.maximum(n, 1e-6)


def clearances(raster, grid1, pts, step=3.0, maxd=26.0):
    """Distance from the street's centre line to the first building on the left and on the right, per sample (inf = none)."""
    s = resample(pts, step)
    if len(s) < 4:
        return None, None
    P = np.array(s, np.float64)
    T = _tangents(P)
    R = np.stack([-T[:, 1], T[:, 0]], axis=1)
    lo, hi = max(1, int(len(P) * 0.15)), max(2, int(len(P) * 0.85))
    P, R = P[lo:hi], R[lo:hi]
    ts = np.arange(1.5, maxd, 0.5)
    out = []
    for side in (-1.0, 1.0):  # left, right
        pos = P[:, None, :] + side * ts[None, :, None] * R[:, None, :]
        ii = np.floor((pos[..., 0] - grid1.x0) / grid1.cell).astype(int)
        jj = np.floor((pos[..., 1] - grid1.z0) / grid1.cell).astype(int)
        inb = (ii >= 0) & (ii < raster.shape[1]) & (jj >= 0) & (jj < raster.shape[0])
        hit = np.zeros(ii.shape, bool)
        hit[inb] = raster[jj[inb], ii[inb]]
        first = hit.argmax(axis=1)
        dist = np.where(hit.any(axis=1), ts[first], np.inf)
        out.append(dist)
    return out[0], out[1]


def _side(c):
    if c is None or len(c) == 0:
        return math.inf
    fin = c[np.isfinite(c)]
    if len(fin) < 0.35 * len(c):
        return math.inf  # open ground on that side
    return float(np.percentile(fin, 20))


def fit_cross_sections(grid1, edges, buildings):
    """Sets width and sidewalks of every street from the room the buildings leave it."""
    raster = building_raster(grid1, buildings)
    fitted = shrunk = 0
    for e in edges:
        if e["cls"] > 6 or e["flags"] & 4:
            continue
        cl, cr = clearances(raster, grid1, e["pts"])
        if cl is None:
            continue
        avail = [_side(cl), _side(cr)]
        if math.isinf(avail[0]) and math.isinf(avail[1]):
            continue
        hw = e["width"] * 0.5
        sw = list(e["sw"])
        oneway = bool(e["flags"] & 1)
        min_hw = (3.0 if oneway else 5.4) * 0.5
        if e["cls"] >= 5:
            min_hw = 1.6
        hw_fit = hw
        for k in range(2):
            if not math.isinf(avail[k]):
                hw_fit = min(hw_fit, avail[k] - min(sw[k], 1.3) - 0.1)
        hw_fit = max(hw_fit, min_hw)
        for k in range(2):
            if math.isinf(avail[k]):
                continue
            w = float(np.clip(avail[k] - hw_fit - 0.1, 0.0, sw[k]))
            sw[k] = w if w >= 0.7 else 0.0
        if hw_fit < hw - 0.05 or tuple(sw) != tuple(e["sw"]):
            shrunk += 1
        e["width"] = float(hw_fit * 2.0)
        e["sw"] = (sw[0], sw[1])
        fitted += 1
    print("fitted %d street cross sections to the buildings (%d narrowed)" % (fitted, shrunk))
    return raster


# ---------------------------------------------------------------------------------------------------------------------
# Buildings out of the streets
# ---------------------------------------------------------------------------------------------------------------------

def fix_buildings(grid1, edges, buildings):
    """Pushes footprints out of the asphalt, drops the ones a street runs through."""
    pts, hws = [], []
    cv = grid1.canvas()
    d = ImageDraw.Draw(cv)
    for e in edges:
        if e["cls"] > 7 or e["flags"] & 4:
            continue
        hw = e["width"] * 0.5
        grid1.line(d, e["pts"], e["width"])
        for p in resample(e["pts"], 1.5):
            pts.append(p)
            hws.append(hw)
    asph = np.asarray(cv) > 0
    tree = cKDTree(np.array(pts))
    hws = np.array(hws)
    keep, dropped, pushed = [], 0, 0
    for b in buildings:
        if b["kind"] == 5:  # canopies may stand over a street
            keep.append(b)
            continue
        outer = b["rings"][0]
        xs = [p[0] for p in outer]
        zs = [p[1] for p in outer]
        i0, i1 = int(min(xs) - grid1.x0) - 1, int(max(xs) - grid1.x0) + 2
        j0, j1 = int(min(zs) - grid1.z0) - 1, int(max(zs) - grid1.z0) + 2
        if i0 < 0 or j0 < 0 or i1 > asph.shape[1] or j1 > asph.shape[0]:
            keep.append(b)
            continue
        sub = Image.new("L", (i1 - i0, j1 - j0), 0)
        ImageDraw.Draw(sub).polygon([((p[0] - grid1.x0) - i0, (p[1] - grid1.z0) - j0) for p in outer], fill=255)
        m = np.asarray(sub) > 0
        area_px = int(m.sum())
        if area_px == 0:
            keep.append(b)
            continue
        ov = int((asph[j0:j1, i0:i1] & m).sum())
        frac = ov / area_px
        if ov > 10 and (frac > 0.30 or (ov > 250 and frac > 0.15)):
            dropped += 1
            continue
        if ov > 2:
            ring = []
            n = len(outer)
            for k in range(n):
                a, c = outer[k], outer[(k + 1) % n]
                L = math.hypot(c[0] - a[0], c[1] - a[1])
                steps = max(1, int(math.ceil(L / 4.0)))
                for q in range(steps):
                    f = q / steps
                    ring.append((a[0] + (c[0] - a[0]) * f, a[1] + (c[1] - a[1]) * f))
            ring = np.array(ring)
            dist, idx = tree.query(ring)
            inside = dist < hws[idx] + 0.25
            if inside.any():
                near = np.array(pts)[idx]
                away = ring - near
                ln = np.linalg.norm(away, axis=1, keepdims=True)
                away = np.where(ln > 1e-4, away / np.maximum(ln, 1e-4), np.array([[1.0, 0.0]]))
                target = near + away * (hws[idx][:, None] + 0.35)
                ring = np.where(inside[:, None], target, ring)
                new = [tuple(p) for p in ring]
                if abs(osm.signed_area(new)) >= 0.55 * b["area"]:
                    b["rings"][0] = new
                    b["area"] = abs(osm.signed_area(new))
                    pushed += 1
                else:
                    dropped += 1
                    continue
        keep.append(b)
    print("buildings: %d dropped (a street runs through them), %d pushed out of a street" % (dropped, pushed))
    return keep


# ---------------------------------------------------------------------------------------------------------------------
# Bridges
# ---------------------------------------------------------------------------------------------------------------------

def _cut(pts, s0, s1):
    """Splits a polyline at arc lengths s0 < s1: returns (before, middle, after) point lists (each may be empty)."""
    cum = [0.0]
    for a, b in zip(pts[:-1], pts[1:]):
        cum.append(cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))

    def at(s):
        s = min(max(s, 0.0), cum[-1])
        for i in range(len(pts) - 1):
            if cum[i + 1] >= s:
                seg = max(cum[i + 1] - cum[i], 1e-9)
                f = (s - cum[i]) / seg
                return (pts[i][0] + (pts[i + 1][0] - pts[i][0]) * f, pts[i][1] + (pts[i + 1][1] - pts[i][1]) * f)
        return pts[-1]

    pa, pb = at(s0), at(s1)
    inner = [p for p, c in zip(pts, cum) if s0 + 0.01 < c < s1 - 0.01]
    before = [p for p, c in zip(pts, cum) if c < s0 - 0.01] + [pa]
    after = [pb] + [p for p, c in zip(pts, cum) if c > s1 + 0.01]
    return before, [pa] + inner + [pb], after


def split_bridges(grid, G, nodes, edges):
    """Cuts the streets where they cross water: the middle piece gets the bridge flag."""
    wet = ndimage.binary_dilation(G["sea"] | G["lagoon"] | G["canal"], iterations=1)
    out = []
    bridges = 0
    for e in edges:
        if e["cls"] > 9 or e["flags"] & 4:
            out.append(e)
            continue
        s = resample(e["pts"], 1.0)
        if len(s) < 8:
            out.append(e)
            continue
        ii = np.clip(((np.array([p[0] for p in s]) - grid.x0) / grid.cell).astype(int), 0, grid.nx - 1)
        jj = np.clip(((np.array([p[1] for p in s]) - grid.z0) / grid.cell).astype(int), 0, grid.nz - 1)
        w = wet[jj, ii]
        if w.sum() < 4:
            out.append(e)
            continue
        runs = []
        k = 0
        while k < len(w):
            if w[k]:
                q = k
                while q + 1 < len(w) and w[q + 1]:
                    q += 1
                if q - k + 1 >= 4:
                    runs.append((k, q))
                k = q + 1
            else:
                k += 1
        if not runs:
            out.append(e)
            continue
        total = len(s) - 1  # about the length in metres
        pieces = []
        rest = e["pts"]
        consumed = 0.0
        for (r0, r1) in runs:
            s0 = max(0.0, r0 - 3.0) - consumed
            s1 = min(float(total), r1 + 3.0) - consumed
            if s1 - s0 < 3.0:
                continue
            before, mid, after = _cut(rest, s0, s1)
            if len(before) >= 2 and s0 > 0.8:
                pieces.append((before, False))
            elif len(before) >= 2:
                mid = before[:-1] + mid
            pieces.append((mid, True))
            rest = after
            consumed += s1
            if len(rest) < 2:
                break
        if len(rest) >= 2 and math.hypot(rest[-1][0] - rest[0][0], rest[-1][1] - rest[0][1]) > 0.8:
            pieces.append((rest, False))
        elif pieces and len(rest) >= 1:
            pieces[-1] = (pieces[-1][0] + rest[1:], pieces[-1][1])
        # nodes between the pieces
        a = e["a"]
        for pi, (pp, br) in enumerate(pieces):
            last = pi == len(pieces) - 1
            if last:
                b = e["b"]
            else:
                nodes.append(pp[-1])
                b = len(nodes) - 1
            ne = dict(e)
            ne["a"], ne["b"] = a, b
            ne["pts"] = pp
            ne["flags"] = e["flags"] | (4 if br else 0)
            bridges += 1 if br else 0
            out.append(ne)
            a = b
    print("%d bridges where streets cross water" % bridges)
    return out


# ---------------------------------------------------------------------------------------------------------------------
# The designed surface
# ---------------------------------------------------------------------------------------------------------------------

def _sample(arr, grid, pts):
    u = (np.array([p[0] for p in pts]) - grid.x0) / grid.cell - 0.5
    v = (np.array([p[1] for p in pts]) - grid.z0) / grid.cell - 0.5
    return ndimage.map_coordinates(arr, [v, u], order=1, mode="nearest")


def design_roads(grid, G, nodes, edges):
    """Per street: sample points and a height for each. Streets share the height of the junction they meet at."""
    H = G["H"]
    wet = ndimage.binary_dilation(G["sea"] | G["lagoon"] | G["canal"], iterations=1)
    designed = []
    for e in edges:
        if e["cls"] > 8:
            continue
        pts = resample(e["pts"], 2.0)
        if math.hypot(pts[-1][0] - e["pts"][-1][0], pts[-1][1] - e["pts"][-1][1]) > 0.05:
            pts.append(e["pts"][-1])
        if len(pts) < 2:
            continue
        base = _sample(H, grid, pts).astype(np.float64)
        bridge = bool(e["flags"] & 4)
        if bridge:
            valid = np.zeros(len(pts), bool)
            valid[0] = valid[-1] = True
        else:
            valid = ~(_sample(wet.astype(np.float32), grid, pts) > 0.5)
            if not valid.any():
                valid[:] = True
        idx = np.arange(len(pts))
        if valid.sum() >= 2:
            base = np.interp(idx, idx[valid], base[valid])
        sm = base
        if not bridge and len(pts) > 6:
            win = int(min(13, max(3, len(pts) // 3 | 1)))
            sm = ndimage.uniform_filter1d(ndimage.uniform_filter1d(base, win, mode="nearest"), win, mode="nearest")
        e["_pts"] = pts
        e["_sm"] = sm
        e["_len"] = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts[:-1], pts[1:]))
        designed.append(e)
    # Junction heights: the mean of what the streets that meet there would be.
    acc = {}
    for e in designed:
        acc.setdefault(e["a"], []).append(float(e["_sm"][0]))
        acc.setdefault(e["b"], []).append(float(e["_sm"][-1]))
    node_h = {n: float(np.mean(v)) for n, v in acc.items()}
    for e in designed:
        L = len(e["_sm"])
        t = np.linspace(0.0, 1.0, L)
        sm = e["_sm"]
        corr = (node_h[e["a"]] - sm[0]) * (1.0 - t) + (node_h[e["b"]] - sm[-1]) * t
        # A street never strays far from the ground it was designed over: where the hillside is steeper than a road can be, the street
        # follows it (and cuts across the slope) instead of being dragged away from it.
        e["_h"] = sm + np.clip(corr, -4.0, 4.0) if not (e["flags"] & 4) else sm + corr
    dbg = os.environ.get("FOH_DEBUG_AT")  # "x,z,r": prints the streets near a point (QA of the baked ground)
    if dbg:
        qx, qz, qr = [float(v) for v in dbg.split(",")]
        for e in designed:
            near = [(p, h) for p, h in zip(e["_pts"], e["_h"]) if math.hypot(p[0] - qx, p[1] - qz) < qr]
            if near:
                raw = _sample(H, grid, [p for p, _ in near])
                print("   near: cls %d flags %d nodes %d-%d width %.1f  heights %s  ground %s" % (e["cls"], e["flags"], e["a"], e["b"], e["width"], np.round([h for _, h in near], 1)[:6], np.round(raw, 1)[:6]))
    off = []
    for e in designed:
        raw = _sample(H, grid, e["_pts"])
        d = float(np.max(np.abs(e["_h"] - raw)))
        if d > 5.0 and not (e["flags"] & 4):
            off.append((d, e["_pts"][len(e["_pts"]) // 2], e["cls"], e["a"], e["b"], float(raw.min()), float(raw.max()), float(e["_sm"].min()), float(e["_sm"].max()), float(e["_h"].min()), float(e["_h"].max()), len(e["_pts"])))
    off.sort(reverse=True)
    print("designed %d street profiles; %d stray more than 5 m from the ground" % (len(designed), len(off)))
    for d, p, cls, a, b, *r in off[:6]:
        print("   %.1f m at (%.0f, %.0f) class %d nodes %d-%d raw %.0f..%.0f sm %.0f..%.0f h %.0f..%.0f n=%d" % ((d, p[0], p[1], cls, a, b) + tuple(r)))
    return designed


def stamp_roads(grid, G, designed):
    """Bakes the profiles into the ground (G['H'], G['W']); returns (core mask, height of the street under each cell)."""
    H0 = G["H"].copy()
    nx, nz = grid.nx, grid.nz
    wet = ndimage.binary_dilation(G["sea"] | G["lagoon"] | G["canal"], iterations=1)
    key = np.full(H0.shape, -1.0, np.float32)
    hbest = H0.copy()
    wbest = np.zeros(H0.shape, np.float32)
    core_mask = np.zeros(H0.shape, bool)
    flat = G["flat"]
    order = sorted(designed, key=lambda e: e["cls"])  # big streets first: they win ties through the key
    for e in order:
        if e["flags"] & 4 and e["cls"] <= 8:
            continue  # a bridge leaves the ground under it alone
        hw = e["width"] * 0.5
        sw = max(e["sw"]) if e["cls"] <= 6 else 0.0
        core = hw + sw + (0.7 if e["cls"] <= 6 else 0.8)
        fl = float(np.mean(_sample(flat, grid, e["_pts"])))
        reach = core + 6.0 + 26.0 * (1.0 - fl)
        r_cells = int(math.ceil(reach / grid.cell)) + 1
        for p, h in zip(e["_pts"], e["_h"]):
            ci = int((p[0] - grid.x0) / grid.cell)
            cj = int((p[1] - grid.z0) / grid.cell)
            i0, i1 = max(0, ci - r_cells), min(nx, ci + r_cells + 1)
            j0, j1 = max(0, cj - r_cells), min(nz, cj + r_cells + 1)
            if i0 >= i1 or j0 >= j1:
                continue
            xx = grid.x0 + (np.arange(i0, i1) + 0.5) * grid.cell - p[0]
            zz = grid.z0 + (np.arange(j0, j1) + 0.5) * grid.cell - p[1]
            dd = np.sqrt(zz[:, None] ** 2 + xx[None, :] ** 2)
            base = H0[j0:j1, i0:i1]
            fade = np.clip(2.2 * np.abs(h - base), 5.0, max(reach - core, 5.0))
            w = 1.0 - smooth01((dd - core) / fade)
            k = (w - dd * 1e-3).astype(np.float32)
            m = (w > 0.002) & (k > key[j0:j1, i0:i1]) & ~wet[j0:j1, i0:i1]
            if not m.any():
                continue
            key[j0:j1, i0:i1][m] = k[m]
            hbest[j0:j1, i0:i1][m] = h
            wbest[j0:j1, i0:i1][m] = w[m]
            core_mask[j0:j1, i0:i1] = np.where(m, dd <= core, core_mask[j0:j1, i0:i1])
    H = np.where(wbest > 0, H0 + (hbest - H0) * wbest, H0)
    G["H"] = H.astype(np.float32)
    G["W"] = np.maximum(G["W"], wbest).astype(np.float32)
    print("stamped %d street profiles into the ground" % len(order))
    return core_mask, hbest, wbest


# ---------------------------------------------------------------------------------------------------------------------
# Buildings follow the street
# ---------------------------------------------------------------------------------------------------------------------

def place_buildings(grid, G, buildings, core_mask, road_h, dem_s):
    """baseZ of every building from the street in front of it, then the ground adapts around it (slope limited)."""
    H = G["H"]
    W0 = G["W"].copy()
    nx, nz = grid.nx, grid.nz
    num = np.zeros_like(H)
    den = np.zeros_like(H)
    inner = np.zeros(H.shape, bool)
    by_street = by_ground = 0
    pad = 2.0
    for b in buildings:
        outer = b["rings"][0]
        xs = [p[0] for p in outer]
        zs = [p[1] for p in outer]
        margin = 14.0 + pad
        i0 = max(0, int((min(xs) - margin - grid.x0) / grid.cell))
        i1 = min(nx, int((max(xs) + margin - grid.x0) / grid.cell) + 2)
        j0 = max(0, int((min(zs) - margin - grid.z0) / grid.cell))
        j1 = min(nz, int((max(zs) + margin - grid.z0) / grid.cell) + 2)
        if i0 >= i1 or j0 >= j1:
            b["baseZ"] = 0.0
            continue
        sub = Image.new("L", (i1 - i0, j1 - j0), 0)
        ImageDraw.Draw(sub).polygon([((p[0] - grid.x0) / grid.cell - i0, (p[1] - grid.z0) / grid.cell - j0) for p in outer], fill=255)
        mask = np.asarray(sub) > 0
        if not mask.any():
            c = (sum(xs) / len(xs), sum(zs) / len(zs))
            ci = int(np.clip((c[0] - grid.x0) / grid.cell, 0, nx - 1))
            cj = int(np.clip((c[1] - grid.z0) / grid.cell, 0, nz - 1))
            b["baseZ"] = round(float(H[cj, ci]) + 0.3, 2)
            continue
        near = ndimage.binary_dilation(mask, iterations=int(14.0 / grid.cell))
        cm = core_mask[j0:j1, i0:i1] & near & ~mask
        on_hill = float(W0[j0:j1, i0:i1][mask].mean()) < 0.9
        gh = dem_s[j0:j1, i0:i1][mask] if on_hill else H[j0:j1, i0:i1][mask]
        ground_ref = float(np.percentile(gh, 35.0))
        floor = None
        if int(cm.sum()) >= 3:
            street = float(np.percentile(road_h[j0:j1, i0:i1][cm], 35.0)) + 0.35
            # The street in front is the reference unless the building is not on its level (a house up the slope, a block above a cut).
            if abs(street - ground_ref) <= 4.0:
                floor = street
                by_street += 1
        if floor is None:
            floor = ground_ref + (0.0 if on_hill else 0.3)
            by_ground += 1
        b["baseZ"] = round(floor, 2)
        target = floor - 0.3
        grown = ndimage.binary_dilation(mask, iterations=int(pad / grid.cell))
        dist = ndimage.distance_transform_edt(~grown) * grid.cell
        Hs = H[j0:j1, i0:i1]
        fade = np.clip(2.0 * np.abs(target - Hs), 3.0, 16.0)
        wp = 1.0 - smooth01(dist / fade)
        # A pad never moves the ground by more than a few metres: beyond that the building stands against the slope (its wall
        # disappears into the hill, its foundation reaches the low side) instead of a crater being dug for it.
        wp = wp * (1.0 - smooth01((np.abs(target - Hs) - 3.5) / 3.0))
        wp = np.where(core_mask[j0:j1, i0:i1], 0.0, wp)  # the street was designed first and keeps its surface
        num[j0:j1, i0:i1] += wp * target
        den[j0:j1, i0:i1] += wp
        inner[j0:j1, i0:i1] |= grown & ~core_mask[j0:j1, i0:i1]
    wp_all = np.clip(den, 0.0, 1.0)
    lvl = np.where(den > 1e-4, num / np.maximum(den, 1e-4), H)
    G["H"] = (H + (lvl - H) * wp_all).astype(np.float32)
    G["W"] = np.maximum(G["W"], wp_all).astype(np.float32)
    print("buildings: %d on the street level in front of them, %d on the ground" % (by_street, by_ground))
    return inner
