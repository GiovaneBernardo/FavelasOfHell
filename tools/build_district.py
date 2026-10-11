#!/usr/bin/env python3
"""Builds the Leblon / Ipanema district files from the OpenStreetMap dump (tools/fetch_osm_district.py) and the Rio heightmap.

    python tools/build_district.py [--preview-dir DIR]

Writes
  world/district_leblon.fohcity   the street graph, building footprints (with height and ground level), areas and points
  world/leblon_ground.fohp        the ground of the district: a height patch (2 m) over the heightmap with the real beach,
                                  the flat city, the canal and a platform under every hillside building, plus a surface mask
                                  (paving / sand / lawn) that the terrain material and the vegetation read

Why offline: the shape of the ground and the footprints are data, not code. The engine generates the geometry from them
(engine/world/src/city_*.cpp), so a better source (a survey, a cadastre) replaces the files without engine changes.

FOHC (little endian): "FOHC", u32 version
  f64 lat0, lon0
  u32 nodes:     f32 x, z
  u32 edges:     u32 a, b, u8 class, u8 lanes, u8 flags, u8 reserved, f32 width, f32 sidewalkLeft, f32 sidewalkRight, u16 name,
                 u16 n, n x (f32 x, z); the left side is (x, z) -> (z, -x) of the direction... see RoadSide in city.h
  u32 names:     u16 length, utf-8
  u32 buildings: u8 kind, u8 levels, u8 flags, u8 rings, f32 height, f32 baseZ, u32 osmId, rings: u16 n, n x (f32 x, z)
  u32 areas:     u8 kind, u8 rings, rings
  u32 points:    u8 kind, f32 x, z, f32 value, u32 extra
FOHP: "FOHP", u32 version, u32 w, h, f32 cell, originX, originZ, unit, u32 sw, sh, f32 scell, i16 heights[w*h],
      u8 weights[w*h], u8 surface[sw*sh*4] (R paving, G sand, B lawn, A unused)
"""
import argparse
import math
import os
import random
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

import district_osm as osm

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))

# ---- ground grid ----
GX0, GX1 = 100.0, 5300.0
GZ0, GZ1 = -3300.0, 800.0
CELL = 2.0

# ---- road classes (the engine switches on these numbers) ----
CLS = {"trunk": 0, "trunk_link": 0, "primary": 1, "primary_link": 1, "secondary": 2, "secondary_link": 2, "tertiary": 3, "tertiary_link": 3,
       "residential": 4, "unclassified": 4, "living_street": 5, "service": 6, "pedestrian": 7, "cycleway": 8, "footway": 9, "path": 9, "steps": 10}
# (asphalt width, sidewalk each side) when the tags do not say more
DEFAULT_WIDTH = {0: 3.3, 1: 3.3, 2: 3.2, 3: 3.0, 4: 3.0, 5: 3.0, 6: 3.2}
SIDEWALK = {0: 2.0, 1: 2.5, 2: 2.8, 3: 2.8, 4: 2.6, 5: 2.0, 6: 0.0}
PATH_WIDTH = {7: 5.0, 8: 2.6, 9: 1.8, 10: 2.0}

KIND_BUILDING = {"house": 1, "detached": 1, "semidetached_house": 1, "terrace": 1, "bungalow": 1, "residential": 0, "apartments": 0, "commercial": 2, "retail": 2,
                 "office": 2, "supermarket": 2, "hotel": 3, "church": 4, "chapel": 4, "school": 4, "hospital": 4, "university": 4, "civic": 4, "public": 4,
                 "grandstand": 4, "roof": 5, "carport": 5, "garage": 6, "garages": 6, "shed": 6, "service": 6, "industrial": 6, "warehouse": 6, "construction": 7}

AREA_KINDS = {"park": 0, "garden": 1, "pitch": 2, "pool": 3, "beach": 4, "water": 5, "wood": 6, "scrub": 7, "rock": 8, "grass": 9, "playground": 2}
POINT_KINDS = {"tree": 0, "lamp": 1, "bus_stop": 2, "bench": 3, "waste": 4, "toilets": 5, "signals": 6, "crossing": 7, "lifeguard": 8, "water": 9}


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# ---------------------------------------------------------------------------------------------------------------------
# Street graph
# ---------------------------------------------------------------------------------------------------------------------

def key_of(p):
    return (round(p["lat"] * 1e7), round(p["lon"] * 1e7))


def build_graph(highways):
    ways = []
    for e in highways:
        t = e["tags"]
        hw = t.get("highway")
        if hw not in CLS:
            continue
        if t.get("tunnel") in ("yes", "building_passage") or t.get("area") == "yes":
            continue
        if hw == "footway" and t.get("footway") in ("sidewalk", "crossing", "traffic_island"):
            continue
        if hw == "service" and t.get("service") in ("parking_aisle", "driveway"):
            continue
        if t.get("access") in ("private", "no") and hw in ("service", "footway", "path"):
            continue
        geo = e.get("geometry", [])
        if len(geo) < 2:
            continue
        ways.append((e, [key_of(p) for p in geo], osm.geom_xz(geo)))
    uses = {}
    for _, keys, _ in ways:
        for i, k in enumerate(keys):
            uses.setdefault(k, set()).add(id(keys))
            if i == 0 or i == len(keys) - 1:
                uses[k].add(("end", id(keys), i))
    node_ids, nodes = {}, []

    def node(k, xz):
        if k not in node_ids:
            node_ids[k] = len(nodes)
            nodes.append(xz)
        return node_ids[k]

    edges = []
    for e, keys, xz in ways:
        t = e["tags"]
        cut = [0]
        for i in range(1, len(keys) - 1):
            if len(uses[keys[i]]) >= 2:
                cut.append(i)
        cut.append(len(keys) - 1)
        for a, b in zip(cut[:-1], cut[1:]):
            pts = xz[a:b + 1]
            if keys[a] == keys[b] and len(pts) < 4:
                continue
            na, nb = node(keys[a], xz[a]), node(keys[b], xz[b])
            if na == nb and len(pts) < 5:
                continue
            edges.append({"a": na, "b": nb, "pts": dedupe(pts), "tags": t})
    return nodes, edges


def dedupe(pts, eps=0.4):
    out = [pts[0]]
    for p in pts[1:-1]:
        if math.hypot(p[0] - out[-1][0], p[1] - out[-1][1]) >= eps:
            out.append(p)
    out.append(pts[-1])
    return out


def classify_edges(edges, names, sea_side):
    """Fills class, lanes, width, sidewalks and flags from the tags."""
    for ed in edges:
        t = ed["tags"]
        cls = CLS[t["highway"]]
        ed["cls"] = cls
        oneway = t.get("oneway") in ("yes", "-1", "1") or t.get("junction") == "roundabout"
        lanes = 0
        try:
            lanes = int(str(t.get("lanes", "0")).split(";")[0])
        except ValueError:
            pass
        if cls <= 6:
            if lanes <= 0:
                lanes = 1 if oneway else 2
                if cls <= 2 and oneway:
                    lanes = 2
            per = DEFAULT_WIDTH[cls]
            width = lanes * per
            if oneway and cls >= 3:
                width += 2.2  # a parked row beside the lane
            if not oneway and cls >= 4:
                width += 3.0  # parked cars on both sides of a two way street: the real width of Ipanema's streets
            if cls == 6:
                width = max(width, 3.6)
            sw = SIDEWALK[cls]
            if t.get("sidewalk") == "no" or t.get("sidewalk:both") == "no":
                sw = 0.0 if cls >= 5 else 1.2
            ed["sw"] = (sw, sw)
        else:
            width = PATH_WIDTH[cls]
            try:
                width = float(t["width"])
            except (KeyError, ValueError):
                pass
            ed["sw"] = (0.0, 0.0)
        ed["width"] = float(width)
        ed["lanes"] = lanes
        ed["flags"] = (1 if oneway else 0) | (2 if t.get("lit") == "yes" else 0) | (4 if t.get("bridge") in ("yes", "viaduct") else 0)
        if t.get("oneway") == "-1":
            ed["pts"] = list(reversed(ed["pts"]))
            ed["a"], ed["b"] = ed["b"], ed["a"]
        nm = t.get("name", "")
        if nm not in names:
            names[nm] = len(names)
        ed["name"] = names[nm]
        # The beach front: the promenade (calcadao) is the sidewalk on the sea side.
        if cls <= 3 and any(k in nm for k in ("Vieira Souto", "Delfim Moreira", "Atlântica")):
            side = sea_side(ed["pts"])
            if side != 0:
                s = list(ed["sw"])
                s[0 if side < 0 else 1] = 7.0
                ed["sw"] = tuple(s)
                ed["flags"] |= 8


def drop_parallel_footways(edges):
    """Footways that run alongside a street are its sidewalk (mapped separately): the engine builds those itself."""
    cell = 20.0
    grid = {}
    roads = [e for e in edges if e["cls"] <= 6]
    for ri, e in enumerate(roads):
        for p in resample(e["pts"], 6.0):
            grid.setdefault((int(p[0] // cell), int(p[1] // cell)), []).append((p, e["width"] * 0.5 + max(e["sw"]) + 3.0))
    keep = []
    for e in edges:
        if e["cls"] not in (9,):
            keep.append(e)
            continue
        samples = resample(e["pts"], 8.0)
        near = 0
        for p in samples:
            cx, cz = int(p[0] // cell), int(p[1] // cell)
            hit = False
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for q, r in grid.get((cx + dx, cz + dz), ()):
                        if math.hypot(p[0] - q[0], p[1] - q[1]) < r:
                            hit = True
                            break
                    if hit:
                        break
                if hit:
                    break
            near += hit
        if near / max(1, len(samples)) < 0.6:
            keep.append(e)
    return keep


def resample(pts, step):
    out = [pts[0]]
    carry = 0.0
    for a, b in zip(pts[:-1], pts[1:]):
        seg = math.hypot(b[0] - a[0], b[1] - a[1])
        d = step - carry
        while d <= seg and seg > 1e-6:
            f = d / seg
            out.append((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f))
            d += step
        carry = seg - (d - step)
    if math.hypot(out[-1][0] - pts[-1][0], out[-1][1] - pts[-1][1]) > step * 0.3:
        out.append(pts[-1])
    return out


# ---------------------------------------------------------------------------------------------------------------------
# Buildings
# ---------------------------------------------------------------------------------------------------------------------

def simplify_ring(ring, min_edge=0.5, collinear_deg=2.5):
    pts = list(ring)
    changed = True
    while changed and len(pts) > 3:
        changed = False
        out = []
        n = len(pts)
        for i in range(n):
            p, a, b = pts[i], out[-1] if out else pts[i - 1], pts[(i + 1) % n]
            if math.hypot(p[0] - a[0], p[1] - a[1]) < min_edge:
                changed = True
                continue
            v1 = (p[0] - a[0], p[1] - a[1])
            v2 = (b[0] - p[0], b[1] - p[1])
            l1, l2 = math.hypot(*v1), math.hypot(*v2)
            if l1 > 1e-6 and l2 > 1e-6:
                cosang = (v1[0] * v2[0] + v1[1] * v2[1]) / (l1 * l2)
                if cosang > math.cos(math.radians(collinear_deg)) and (l1 < 25 or l2 < 25):
                    changed = True
                    continue
            out.append(p)
        pts = out
    return pts


def process_buildings(elements, rng):
    out = []
    for e in elements:
        t = e["tags"]
        for outer, holes in osm.polygons_of(e):
            outer = simplify_ring(outer)
            if len(outer) < 3:
                continue
            if osm.signed_area(outer) < 0:
                outer = outer[::-1]
            area = abs(osm.signed_area(outer))
            if area < 6.0:
                continue
            rings = [outer]
            for h in holes:
                h = simplify_ring(h)
                if len(h) < 3 or abs(osm.signed_area(h)) < 4.0:
                    continue
                if osm.signed_area(h) > 0:
                    h = h[::-1]
                rings.append(h)
            kind = KIND_BUILDING.get(t.get("building", "yes"), 0)
            height = osm.parse_height(t)
            flags = 0 if height is None else 1  # 1: the height is mapped
            levels = 0
            try:
                levels = int(float(t.get("building:levels", 0)))
            except ValueError:
                pass
            out.append({"id": e["id"], "kind": kind, "height": height, "levels": levels, "flags": flags, "rings": rings, "area": area, "tags": t})
    return out


def assign_default_heights(buildings, rng):
    """Buildings with no mapped height get a plausible one: small lots are houses, big ones towers, by the usual mix of the district."""
    for b in buildings:
        if b["height"] is not None:
            continue
        a = b["area"]
        r = rng.random()
        if b["kind"] == 1:
            h = 3.2 * (1 + (r > 0.55) + (r > 0.9)) + 0.6
        elif b["kind"] in (5,):
            h = 3.4
        elif b["kind"] in (6, 7):
            h = 3.2 + 3.0 * (r > 0.7)
        elif a < 90:
            h = 3.0 * (1 + int(r * 3)) + 0.6
        elif a < 260:
            h = 3.0 * (3 + int(r * 5)) + 0.8
        elif a < 700:
            h = 3.0 * (6 + int(r * 8)) + 1.0
        else:
            h = 3.0 * (4 + int(r * 10)) + 1.0
        b["height"] = h
    for b in buildings:
        if b["levels"] == 0:
            b["levels"] = max(1, int(round((b["height"] - 0.8) / 3.0)))


# ---------------------------------------------------------------------------------------------------------------------
# Grid helpers
# ---------------------------------------------------------------------------------------------------------------------

class Grid:
    def __init__(self, x0, z0, x1, z1, cell):
        self.x0, self.z0, self.cell = x0, z0, cell
        self.nx = int(math.ceil((x1 - x0) / cell))
        self.nz = int(math.ceil((z1 - z0) / cell))

    def px(self, p):
        return ((p[0] - self.x0) / self.cell, (p[1] - self.z0) / self.cell)

    def xs(self):
        return self.x0 + (np.arange(self.nx) + 0.5) * self.cell

    def zs(self):
        return self.z0 + (np.arange(self.nz) + 0.5) * self.cell

    def canvas(self):
        return Image.new("L", (self.nx, self.nz), 0)

    def fill(self, draw, polys, holes=(), value=255):
        for ring in polys:
            draw.polygon([self.px(p) for p in ring], fill=value)
        for ring in holes:
            draw.polygon([self.px(p) for p in ring], fill=0)

    def line(self, draw, pts, width_m, value=255):
        w = max(1, int(round(width_m / self.cell)))
        draw.line([self.px(p) for p in pts], fill=value, width=w, joint="curve")

    def arr(self, img):
        return np.asarray(img, dtype=np.uint8) > 0


def load_dem(path):
    with open(path, "rb") as f:
        magic, ver, w, h, cell, ox, oz, unit = struct.unpack("<4sIIIffff", f.read(32))
        data = np.frombuffer(f.read(w * h * 2), dtype=np.int16).reshape(h, w).astype(np.float32) * unit
    return data, cell, ox, oz


def sample_dem(dem, cell, ox, oz, xs, zs):
    u = (xs - ox) / cell
    v = (zs - oz) / cell
    gu, gv = np.meshgrid(u, v)
    return ndimage.map_coordinates(dem, [gv, gu], order=3, mode="nearest").astype(np.float32)


# ---------------------------------------------------------------------------------------------------------------------
# Ground
# ---------------------------------------------------------------------------------------------------------------------

def build_ground(grid, dem_h, areas, edges, buildings, nodes, preview_dir):
    nx, nz = grid.nx, grid.nz
    xs, zs = grid.xs(), grid.zs()

    # ---- water: the sea from the coastline (flood fill from open water), the lagoon and canals from polygons / lines ----
    cv = grid.canvas()
    d = ImageDraw.Draw(cv)
    for a in areas:
        if a["kind"] == "coast":
            grid.line(d, a["pts"], 3.5)
    coast_line = grid.arr(cv)
    free = ~coast_line
    labels, _ = ndimage.label(free)
    seed = (int((2800.0 - grid.x0) / grid.cell), int((600.0 - grid.z0) / grid.cell))
    sea = labels == labels[seed[1], seed[0]]
    if labels[seed[1], seed[0]] == 0:
        raise SystemExit("sea seed lies on the coastline")
    sea |= ndimage.binary_dilation(coast_line, iterations=1) & ndimage.binary_dilation(sea, iterations=2)

    cv = grid.canvas()
    d = ImageDraw.Draw(cv)
    for a in areas:
        if a["kind"] == AREA_KINDS["water"] and a["area"] > 2500:
            grid.fill(d, [a["outer"]], a["holes"])
    lagoon = grid.arr(cv) & ~sea

    cv = grid.canvas()
    d = ImageDraw.Draw(cv)
    for a in areas:
        if a["kind"] == "canal":
            grid.line(d, a["pts"], a["width"])
    canal = grid.arr(cv) & ~sea & ~lagoon

    cv = grid.canvas()
    d = ImageDraw.Draw(cv)
    for a in areas:
        if a["kind"] == AREA_KINDS["beach"]:
            grid.fill(d, [a["outer"]], a["holes"])
    beach_poly = grid.arr(cv) & ~sea

    land = ~sea & ~lagoon & ~canal
    # ---- distance to the shoreline, signed (+ inland) ----
    dist_coast = ndimage.distance_transform_edt(~coast_line) * grid.cell
    s = np.where(sea, -dist_coast, dist_coast).astype(np.float32)
    dist_lagoon_land = ndimage.distance_transform_edt(~lagoon) * grid.cell       # on land: how far from the lagoon
    dist_lagoon_in = ndimage.distance_transform_edt(lagoon) * grid.cell          # in the lagoon: how far from the shore

    # ---- the land the city stands on ----
    # Copernicus is a surface model: the plain of Leblon / Ipanema reads 10-15 m because of the buildings (real street level is
    # 2-5 m). A large erosion (62 m window) finds the ground between the blocks; the remaining bias is removed on the plain and
    # fades out on the hills, where the heightmap is kept as it is.
    ero = ndimage.minimum_filter(np.where(land & (dem_h > 0.5), dem_h, 99.0).astype(np.float32), size=41)
    e_s = ndimage.gaussian_filter(ero, 12.0)
    no_dem = e_s > 90.0  # land by the coastline but sea in the heightmap (the Arpoador rock): raised from the shore
    # Plain or hill? The slope of the heavily smoothed surface: a plain is flat whatever stands on it, a hill is not.
    land_f = land.astype(np.float32)
    big = ndimage.gaussian_filter(np.where(land, dem_h, 0.0).astype(np.float32), 30.0) / np.maximum(ndimage.gaussian_filter(land_f, 30.0), 1e-3)
    gz_, gx_ = np.gradient(big, grid.cell)
    slope_s = np.hypot(gx_, gz_)
    plain = np.where(land, 1.0 - smoothstep(0.035, 0.11, slope_s), 1.0)
    plain = np.where(no_dem, 1.0, plain)
    # The ground on the plain from the erosion (smooth); on the hills the surface model smoothed over ~16 m with the building
    # bias fading out as the ground rises. The two agree where one hands over to the other, so there is no cliff at the foot.
    dsm = ndimage.gaussian_filter(np.where(land, dem_h, 0.0).astype(np.float32), 8.0) / np.maximum(ndimage.gaussian_filter(land_f, 8.0), 1e-3)
    ground_plain = e_s - 9.5 * (1.0 - smoothstep(14.0, 50.0, e_s))
    ground_hill = dsm - 9.5 * (1.0 - smoothstep(10.0, 50.0, dsm))
    ground = plain * ground_plain + (1.0 - plain) * ground_hill
    city_h = np.where(no_dem, np.clip(3.0 + 0.12 * np.maximum(s, 0.0), 3.0, 18.0), np.maximum(ground, 2.6))
    city_h = ndimage.gaussian_filter(city_h.astype(np.float32), 4.0)
    flat = plain.copy()
    # The shore strip belongs to the patch whatever the heightmap says (it has no land there, so the erosion above is blind to it):
    # the beach, the promenade and the first blocks inland, fading back to the heightmap over 100 m. Where a hill meets the sea
    # (Arpoador, the end of Leblon) the hill's own heights stay.
    flat = np.where(land, np.maximum(flat, (1.0 - smoothstep(60.0, 160.0, s)) * plain), flat)
    flat = ndimage.gaussian_filter(flat.astype(np.float32), 3.0)
    plain_s = ndimage.gaussian_filter(plain.astype(np.float32), 3.0)

    # ---- the beach: a berm that rises from the water line, flattening into the promenade ----
    berm = 3.15 * (1.0 - np.exp(-np.maximum(s, 0.0) / 17.0))
    land_h = city_h + (np.minimum(city_h, berm) - city_h) * plain_s
    # Sea: gentle bed with real Ipanema's slope (about -5 m 120 m out), merging into the heightmap's bed further out.
    bed = -16.0 * (1.0 - np.exp(-np.maximum(-s, 0.0) / 190.0))
    sea_blend = smoothstep(300.0, 700.0, -s)
    sea_h = bed * (1.0 - sea_blend) + dem_h * sea_blend

    H = np.where(sea, sea_h, land_h)
    # Lagoon: a shallow bowl; its shore is a quay a little above the water.
    H = np.where(lagoon, -3.5 * (1.0 - np.exp(-dist_lagoon_in / 14.0)) - 0.15, H)
    quay = 1.5 * smoothstep(0.0, 5.0, dist_lagoon_land)  # the land climbs from the water line to its level in 5 m
    H = np.where(land & (dist_lagoon_land < 5.0), np.minimum(H, 0.35 + quay), H)
    # Canals: a trench with steep banks.
    dist_canal = ndimage.distance_transform_edt(~canal) * grid.cell
    H = np.where(canal, -2.6, np.where(dist_canal < 2.0, np.minimum(H, 0.2 + 1.8 * smoothstep(0.0, 2.0, dist_canal)), H))

    # ---- weight: where the patch replaces the heightmap ----
    W = flat.astype(np.float32)
    W = np.where(sea, np.maximum(W, 1.0 - smoothstep(560.0, 900.0, -s)), W)
    W = np.maximum(W, np.where(lagoon | canal, 1.0, 0.0))
    W = np.maximum(W, np.where(land & (dist_lagoon_land < 60.0), 1.0 - smoothstep(20.0, 60.0, dist_lagoon_land), 0.0))
    W = np.clip(W, 0.0, 1.0)

    return {"H": H.astype(np.float32), "W": W.astype(np.float32), "sea": sea, "lagoon": lagoon, "canal": canal, "land": land, "beach_poly": beach_poly,
            "s": s, "flat": flat, "dem": dem_h, "city_h": city_h, "coast": coast_line}


def pads_and_roads(grid, G, buildings, edges, nodes, preview_dir):
    """Levels the ground under hillside buildings and along hillside roads (writes into G['H'], G['W'])."""
    H, W, dem = G["H"], G["W"], G["dem"]
    nx, nz = grid.nx, grid.nz
    num = np.zeros_like(H)
    den = np.zeros_like(H)
    padw = np.zeros_like(H)

    def sample_at(arr, x, z):
        i = int(np.clip((x - grid.x0) / grid.cell, 0, nx - 1))
        j = int(np.clip((z - grid.z0) / grid.cell, 0, nz - 1))
        return float(arr[j, i])

    # ---- roads on hills: a grade-limited profile, level across ----
    stamped = 0
    best_d = np.full(H.shape, 1e9, np.float32)
    road_h = np.zeros_like(H)
    for e in edges:
        if e["cls"] > 6 and e["cls"] != 7:
            continue
        samples = resample(e["pts"], 2.0)
        if len(samples) < 2:
            continue
        hs = np.array([sample_at(H, p[0], p[1]) for p in samples], np.float32)
        hs = ndimage.uniform_filter1d(hs, 13, mode="nearest")
        hs = ndimage.uniform_filter1d(hs, 13, mode="nearest")
        grade = 0.16 * 2.0
        for i in range(1, len(hs)):
            hs[i] = np.clip(hs[i], hs[i - 1] - grade, hs[i - 1] + grade)
        for i in range(len(hs) - 2, -1, -1):
            hs[i] = np.clip(hs[i], hs[i + 1] - grade, hs[i + 1] + grade)
        core = e["width"] * 0.5 + max(e["sw"]) + 0.8
        reach = core + 5.0
        r_cells = int(math.ceil(reach / grid.cell)) + 1
        for p, h in zip(samples, hs):
            ci = int((p[0] - grid.x0) / grid.cell)
            cj = int((p[1] - grid.z0) / grid.cell)
            i0, i1 = max(0, ci - r_cells), min(nx, ci + r_cells + 1)
            j0, j1 = max(0, cj - r_cells), min(nz, cj + r_cells + 1)
            if i0 >= i1 or j0 >= j1:
                continue
            xx = grid.x0 + (np.arange(i0, i1) + 0.5) * grid.cell - p[0]
            zz = grid.z0 + (np.arange(j0, j1) + 0.5) * grid.cell - p[1]
            dd = np.sqrt(zz[:, None] ** 2 + xx[None, :] ** 2)
            sub = best_d[j0:j1, i0:i1]
            m = (dd < sub) & (dd < reach)
            sub[m] = dd[m]
            road_h[j0:j1, i0:i1][m] = h
            stamped += 1
    if stamped:
        # full weight over the core, then a fade over 5 m (the distance array keeps no per road core: 4.8 m covers asphalt and sidewalks)
        core_d = 4.8
        wr = np.where(best_d < 1e8, 1.0 - smoothstep(core_d, core_d + 5.0, best_d), 0.0)
        num += road_h * wr
        den += wr
        padw = np.maximum(padw, wr)

    # ---- buildings ----
    pads = 0
    W0 = W.copy()
    land_f = G["land"].astype(np.float32)
    dem_s = ndimage.gaussian_filter(np.where(G["land"], dem, 0.0), 4.0) / np.maximum(ndimage.gaussian_filter(land_f, 4.0), 1e-3)
    for b in buildings:
        outer = b["rings"][0]
        xs_ = [p[0] for p in outer]
        zs_ = [p[1] for p in outer]
        pad = 2.0
        i0 = max(0, int((min(xs_) - pad - 6.0 - grid.x0) / grid.cell))
        i1 = min(nx, int((max(xs_) + pad + 6.0 - grid.x0) / grid.cell) + 2)
        j0 = max(0, int((min(zs_) - pad - 6.0 - grid.z0) / grid.cell))
        j1 = min(nz, int((max(zs_) + pad + 6.0 - grid.z0) / grid.cell) + 2)
        if i0 >= i1 or j0 >= j1:
            b["baseZ"] = 0.0
            continue
        sub = Image.new("L", (i1 - i0, j1 - j0), 0)
        d = ImageDraw.Draw(sub)
        d.polygon([((p[0] - grid.x0) / grid.cell - i0, (p[1] - grid.z0) / grid.cell - j0) for p in outer], fill=255)
        mask = np.asarray(sub) > 0
        if not mask.any():
            c = (sum(xs_) / len(xs_), sum(zs_) / len(zs_))
            b["baseZ"] = sample_at(H, *c) + 0.25
            continue
        Hs = H[j0:j1, i0:i1]
        gh = Hs[mask]
        # Where the patch does not own the ground (W < 0.9) the terrain is the heightmap with its detail, which the model H
        # does not describe: read the smoothed heightmap there, and always level the ground under the building.
        on_hill = float(W0[j0:j1, i0:i1][mask].mean()) < 0.9
        if on_hill:
            gh = dem_s[j0:j1, i0:i1][mask]
        elif float(gh.max() - gh.min()) < 1.5:
            b["baseZ"] = round(float(np.percentile(gh, 35.0)) + 0.3, 2)  # a ground floor 30 cm above the street
            continue
        # A hillside building: ground floor at a low percentile of the ground under it, ground levelled to it.
        base = float(np.percentile(gh, 35.0))
        b["baseZ"] = round(base, 2)
        dist = ndimage.distance_transform_edt(~ndimage.binary_dilation(mask, iterations=int(pad / grid.cell))) * grid.cell
        wp = 1.0 - smoothstep(0.0, 5.0, dist)
        num[j0:j1, i0:i1] += wp * base
        den[j0:j1, i0:i1] += wp
        padw[j0:j1, i0:i1] = np.maximum(padw[j0:j1, i0:i1], wp)
        pads += 1
    if pads or stamped:
        lvl = np.where(den > 1e-4, num / np.maximum(den, 1e-4), H)
        H = np.where(padw > 0, lvl, H)
        G["H"] = H
        G["W"] = np.maximum(W, padw)
    print("levelled %d hillside buildings and %d road samples" % (pads, stamped))
    return padw


def build_surface(grid, G, areas, buildings, edges, padw):
    """RGBA surface mask on the 2 m grid (later halved): R paving, G sand, B lawn."""
    nx, nz = grid.nx, grid.nz
    cv_pave = grid.canvas()
    dp = ImageDraw.Draw(cv_pave)
    for b in buildings:
        grid.fill(dp, [b["rings"][0]], b["rings"][1:])
    for e in edges:
        if e["cls"] <= 7:
            grid.line(dp, e["pts"], e["width"] + 2.0 * max(e["sw"]) + 1.0)
        elif e["cls"] in (9, 10):
            grid.line(dp, e["pts"], e["width"] + 0.6)
    pave = grid.arr(cv_pave)

    cv_lawn = grid.canvas()
    dl = ImageDraw.Draw(cv_lawn)
    cv_pitch = grid.canvas()
    dpitch = ImageDraw.Draw(cv_pitch)
    for a in areas:
        if a.get("outer") is None:
            continue
        k = a["kind"]
        if k in (AREA_KINDS["park"], AREA_KINDS["garden"], AREA_KINDS["grass"], AREA_KINDS["wood"], AREA_KINDS["scrub"]):
            grid.fill(dl, [a["outer"]], a["holes"])
        elif k == AREA_KINDS["pitch"]:
            grid.fill(dpitch, [a["outer"]], a["holes"])
    lawn = grid.arr(cv_lawn)
    pitch = grid.arr(cv_pitch)
    flat = G["flat"] > 0.6
    land = G["land"]
    s = G["s"]
    # Urban ground: within 30 m of a building or a street. Elsewhere the natural rules (or lawn on flat land) apply.
    near_urban = ndimage.binary_dilation(pave, iterations=15)
    # Sand: what is open between the water line and the first street or building, found by flooding from the shore.
    open_ground = land & ~pave & (s < 260.0) & ~lawn & flat
    labels, count = ndimage.label(open_ground)
    shore = np.unique(labels[open_ground & (s < 3.0)])
    shore = shore[shore > 0]
    sand = np.isin(labels, shore) | (G["beach_poly"] & ~pave)
    urban_default = flat & land & near_urban & ~sand & ~lawn
    wild_lawn = flat & land & ~near_urban & ~sand & ~lawn  # flat open land far from any street: grass, not paving
    paved = (pave | urban_default | (padw > 0.5) | pitch) & ~sand & ~(lawn & ~pave)
    lawn_f = (lawn | wild_lawn) & ~paved & ~sand
    out = np.zeros((nz, nx, 4), np.float32)
    out[..., 0] = ndimage.gaussian_filter(paved.astype(np.float32), 0.8)
    out[..., 1] = ndimage.gaussian_filter(sand.astype(np.float32), 0.8)
    out[..., 2] = ndimage.gaussian_filter(lawn_f.astype(np.float32), 0.8)
    out[..., 3] = 1.0
    # 4 m texels: average 2 x 2
    nz2, nx2 = nz // 2 * 2, nx // 2 * 2
    sm = out[:nz2, :nx2].reshape(nz2 // 2, 2, nx2 // 2, 2, 4).mean(axis=(1, 3))
    return (np.clip(sm, 0, 1) * 255.0 + 0.5).astype(np.uint8)


# ---------------------------------------------------------------------------------------------------------------------
# Areas and points
# ---------------------------------------------------------------------------------------------------------------------

def process_areas(elements):
    out = []
    for e in elements:
        t = e["tags"]
        kind = None
        if t.get("natural") == "coastline" and e["type"] == "way":
            out.append({"kind": "coast", "pts": osm.geom_xz(e["geometry"])})
            continue
        if t.get("waterway") in ("canal", "river") and e["type"] == "way":
            wid = 14.0 if t.get("waterway") == "canal" else 8.0
            try:
                wid = float(t["width"])
            except (KeyError, ValueError):
                pass
            out.append({"kind": "canal", "pts": osm.geom_xz(e["geometry"]), "width": wid})
            continue
        n = t.get("natural")
        lz = t.get("leisure")
        lu = t.get("landuse")
        if n == "beach":
            kind = "beach"
        elif n == "water":
            kind = "water"
        elif n in ("wood",) or lu == "forest":
            kind = "wood"
        elif n in ("scrub", "wetland"):
            kind = "scrub"
        elif n in ("bare_rock", "cliff"):
            kind = "rock"
        elif lz == "park":
            kind = "park"
        elif lz == "garden":
            kind = "garden"
        elif lz in ("pitch", "playground", "track"):
            kind = "pitch"
        elif lz == "swimming_pool":
            kind = "pool"
        elif lu in ("grass", "recreation_ground"):
            kind = "grass"
        if kind is None:
            continue
        for outer, holes in osm.polygons_of(e):
            if osm.signed_area(outer) < 0:
                outer = outer[::-1]
            holes = [h[::-1] if osm.signed_area(h) > 0 else h for h in holes]
            out.append({"kind": AREA_KINDS[kind], "outer": outer, "holes": holes, "area": abs(osm.signed_area(outer))})
    return out


def process_points(elements):
    out = []
    for e in elements:
        if e["type"] != "node":
            continue
        t = e["tags"]
        x, z = osm.proj(e["lat"], e["lon"])
        k = None
        if t.get("natural") == "tree":
            k = "tree"
        elif t.get("highway") == "street_lamp" or t.get("man_made") == "street_lamp":
            k = "lamp"
        elif t.get("highway") == "bus_stop":
            k = "bus_stop"
        elif t.get("amenity") == "bench":
            k = "bench"
        elif t.get("amenity") == "waste_basket":
            k = "waste"
        elif t.get("amenity") == "toilets":
            k = "toilets"
        elif t.get("highway") == "traffic_signals":
            k = "signals"
        elif t.get("highway") == "crossing":
            k = "crossing"
        elif t.get("emergency") == "lifeguard":
            k = "lifeguard"
        elif t.get("amenity") == "drinking_water":
            k = "water"
        if k:
            out.append((POINT_KINDS[k], x, z, 0.0, 0))
    return out


# ---------------------------------------------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------------------------------------------

def write_city(path, nodes, edges, names, buildings, areas, points):
    with open(path, "wb") as f:
        f.write(struct.pack("<4sI", b"FOHC", 1))
        f.write(struct.pack("<dd", osm.LAT0, osm.LON0))
        f.write(struct.pack("<I", len(nodes)))
        for n in nodes:
            f.write(struct.pack("<ff", *n))
        f.write(struct.pack("<I", len(edges)))
        for e in edges:
            f.write(struct.pack("<IIBBBBfffHH", e["a"], e["b"], e["cls"], min(e["lanes"], 255), e["flags"], 0, e["width"], e["sw"][0], e["sw"][1], e["name"], len(e["pts"])))
            for p in e["pts"]:
                f.write(struct.pack("<ff", *p))
        order = sorted(names.items(), key=lambda kv: kv[1])
        f.write(struct.pack("<I", len(order)))
        for nm, _ in order:
            raw = nm.encode("utf-8")[:200]
            f.write(struct.pack("<H", len(raw)) + raw)
        f.write(struct.pack("<I", len(buildings)))
        for b in buildings:
            f.write(struct.pack("<BBBBffI", b["kind"], min(b["levels"], 255), b["flags"], len(b["rings"]), b["height"], b.get("baseZ", 0.0), b["id"] & 0xFFFFFFFF))
            for r in b["rings"]:
                f.write(struct.pack("<H", len(r)))
                for p in r:
                    f.write(struct.pack("<ff", *p))
        polys = [a for a in areas if a.get("outer") is not None]
        f.write(struct.pack("<I", len(polys)))
        for a in polys:
            rings = [a["outer"]] + a["holes"]
            f.write(struct.pack("<BB", a["kind"], len(rings)))
            for r in rings:
                f.write(struct.pack("<H", len(r)))
                for p in r:
                    f.write(struct.pack("<ff", *p))
        f.write(struct.pack("<I", len(points)))
        for k, x, z, v, ex in points:
            f.write(struct.pack("<BfffI", k, x, z, v, ex))
    print("wrote %s (%.1f MB): %d nodes, %d edges, %d buildings, %d areas, %d points" % (path, os.path.getsize(path) / 1e6, len(nodes), len(edges), len(buildings), len(polys), len(points)))


def write_patch(path, grid, G, surface):
    unit = 1.0 / 16.0
    q = np.clip(np.round(G["H"] / unit), -32768, 32767).astype(np.int16)
    wq = (np.clip(G["W"], 0, 1) * 255.0 + 0.5).astype(np.uint8)
    sh, sw = surface.shape[:2]
    with open(path, "wb") as f:
        f.write(struct.pack("<4sIIIfffffII", b"FOHP", 1, grid.nx, grid.nz, grid.cell, grid.x0, grid.z0, unit, grid.cell * 2.0, sw, sh))
        f.write(q.tobytes())
        f.write(wq.tobytes())
        f.write(np.ascontiguousarray(surface).tobytes())
    print("wrote %s (%.1f MB): %d x %d at %.0f m, surface %d x %d" % (path, os.path.getsize(path) / 1e6, grid.nx, grid.nz, grid.cell, sw, sh))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dem", default=os.path.join(ROOT, "world", "rio_dem.fohdem"))
    ap.add_argument("--out-city", default=os.path.join(ROOT, "world", "district_leblon.fohcity"))
    ap.add_argument("--out-ground", default=os.path.join(ROOT, "world", "leblon_ground.fohp"))
    ap.add_argument("--preview-dir", default="")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    grid = Grid(GX0, GZ0, GX1, GZ1, CELL)
    dem, dcell, dox, doz = load_dem(args.dem)
    dem_h = sample_dem(dem, dcell, dox, doz, grid.xs(), grid.zs())

    areas = process_areas(osm.load("areas") + osm.load("lagoa"))
    G = None

    # Street graph first (the ground needs the roads)
    nodes, edges = build_graph(osm.load("highways"))
    names = {"": 0}

    # Sea side of a polyline, for the promenade: -1 left / +1 right / 0 unknown, from the coast distance field
    G = build_ground(grid, dem_h, areas, [], [], nodes, args.preview_dir)

    def sea_side(pts):
        votes = 0
        for a, b in zip(pts[:-1:3], pts[1::3]):
            dx, dz = b[0] - a[0], b[1] - a[1]
            ln = math.hypot(dx, dz)
            if ln < 1e-3:
                continue
            nxr, nzr = -dz / ln, dx / ln  # right
            m = ((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5)
            def at(px, pz):
                i = int(np.clip((px - grid.x0) / grid.cell, 0, grid.nx - 1))
                j = int(np.clip((pz - grid.z0) / grid.cell, 0, grid.nz - 1))
                return G["s"][j, i]
            sr = at(m[0] + nxr * 40.0, m[1] + nzr * 40.0)
            sl = at(m[0] - nxr * 40.0, m[1] - nzr * 40.0)
            if sr < 0 and sl > 0:
                votes += 1
            elif sl < 0 and sr > 0:
                votes -= 1
        if votes == 0:
            return 0
        return 1 if votes > 0 else -1

    classify_edges(edges, names, sea_side)
    edges = drop_parallel_footways(edges)

    buildings = process_buildings(osm.load("buildings"), rng)
    assign_default_heights(buildings, rng)
    print("%d buildings, %d edges" % (len(buildings), len(edges)))

    padw = pads_and_roads(grid, G, buildings, edges, nodes, args.preview_dir)
    surface = build_surface(grid, G, areas, buildings, edges, padw)

    points = process_points(osm.load("points"))
    write_city(args.out_city, nodes, edges, names, buildings, areas, points)
    write_patch(args.out_ground, grid, G, surface)

    if args.preview_dir:
        os.makedirs(args.preview_dir, exist_ok=True)
        h = G["H"]
        img = np.zeros((grid.nz, grid.nx, 3), np.float32)
        gy, gx = np.gradient(h, grid.cell)
        shade = np.clip(0.55 + 2.2 * (gx * 0.6 + gy * 0.6), 0.2, 1.2)
        img[..., 0] = np.where(h < 0, 0.05, 0.35 + 0.05 * np.clip(h, 0, 20)) * shade
        img[..., 1] = np.where(h < 0, 0.2 + 0.2 * np.clip(h / -15.0, 0, 1), 0.45) * shade
        img[..., 2] = np.where(h < 0, 0.45 + 0.3 * np.clip(h / -15.0, 0, 1), 0.2) * shade
        sm = np.kron(surface[..., :3].astype(np.float32) / 255.0, np.ones((2, 2, 1)))
        sm = np.pad(sm, ((0, max(0, grid.nz - sm.shape[0])), (0, max(0, grid.nx - sm.shape[1])), (0, 0)))[:grid.nz, :grid.nx]
        mask = np.zeros_like(img)
        mask[..., 0] = np.clip(sm[..., 0] * 0.5 + sm[..., 1] * 0.9, 0, 1)
        mask[..., 1] = np.clip(sm[..., 0] * 0.5 + sm[..., 1] * 0.8 + sm[..., 2] * 0.8, 0, 1)
        mask[..., 2] = np.clip(sm[..., 0] * 0.5 + sm[..., 1] * 0.5, 0, 1)
        Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(os.path.join(args.preview_dir, "ground_height.png"))
        Image.fromarray((np.clip(mask, 0, 1) * 255).astype(np.uint8)).save(os.path.join(args.preview_dir, "ground_surface.png"))
        Image.fromarray((np.clip(G["W"], 0, 1) * 255).astype(np.uint8)).save(os.path.join(args.preview_dir, "ground_weight.png"))
        print("previews in", args.preview_dir)


if __name__ == "__main__":
    sys.exit(main())
