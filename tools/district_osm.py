"""Shared helpers of the district tools: projection, reading the Overpass dumps, ring assembly.

World coordinates match tools/build_rio_heightmap.py: metres east (x) and south (z) from Vidigal (lat0, lon0) with a local
equirectangular projection.
"""
import json
import math
import os

LAT0, LON0 = -22.9935, -43.2330
M_PER_DEG_LAT = 110574.0
M_PER_DEG_LON = 111320.0 * math.cos(math.radians(LAT0))

HERE = os.path.dirname(os.path.abspath(__file__))
OSM_DIR = os.path.normpath(os.path.join(HERE, "..", "data", "osm"))


def proj(lat, lon):
    return ((lon - LON0) * M_PER_DEG_LON, (LAT0 - lat) * M_PER_DEG_LAT)


def load(name):
    with open(os.path.join(OSM_DIR, name + ".json"), encoding="utf-8") as f:
        return json.load(f)["elements"]


def geom_xz(geometry):
    return [proj(p["lat"], p["lon"]) for p in geometry]


def signed_area(ring):
    a = 0.0
    for i in range(len(ring)):
        x0, z0 = ring[i]
        x1, z1 = ring[(i + 1) % len(ring)]
        a += x0 * z1 - x1 * z0
    return 0.5 * a


def close_enough(a, b, eps=0.05):
    return abs(a[0] - b[0]) < eps and abs(a[1] - b[1]) < eps


def join_ways(pieces):
    """Joins open polylines end to end into closed rings (multipolygon members). Returns a list of closed rings."""
    pieces = [list(p) for p in pieces if len(p) >= 2]
    rings = []
    while pieces:
        ring = pieces.pop()
        guard = 0
        while not close_enough(ring[0], ring[-1]) and guard < 10000:
            guard += 1
            hit = None
            for i, p in enumerate(pieces):
                if close_enough(ring[-1], p[0]):
                    ring.extend(p[1:])
                    hit = i
                    break
                if close_enough(ring[-1], p[-1]):
                    ring.extend(reversed(p[:-1]))
                    hit = i
                    break
            if hit is None:
                break
            pieces.pop(hit)
        if close_enough(ring[0], ring[-1]) and len(ring) >= 4:
            rings.append(ring[:-1])
    return rings


def polygons_of(element):
    """(outer ring, [hole rings]) tuples of a way or a multipolygon relation, in world metres."""
    if element["type"] == "way":
        g = geom_xz(element.get("geometry", []))
        if len(g) >= 4 and close_enough(g[0], g[-1]):
            return [(g[:-1], [])]
        return []
    outer, inner = [], []
    for m in element.get("members", []):
        if m.get("type") != "way" or "geometry" not in m:
            continue
        (inner if m.get("role") == "inner" else outer).append(geom_xz(m["geometry"]))
    outers = join_ways(outer)
    inners = join_ways(inner)
    out = []
    for o in outers:
        holes = [h for h in inners if point_in_ring(h[0], o)]
        out.append((o, holes))
    return out


def point_in_ring(p, ring):
    x, z = p
    inside = False
    n = len(ring)
    j = n - 1
    for i in range(n):
        xi, zi = ring[i]
        xj, zj = ring[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi + 1e-12) + xi:
            inside = not inside
        j = i
    return inside


def parse_height(tags):
    """Total height in metres from OSM tags, or None."""
    h = tags.get("height") or tags.get("building:height")
    if h:
        try:
            return float(str(h).lower().replace("m", "").replace(",", ".").strip())
        except ValueError:
            pass
    lv = tags.get("building:levels")
    if lv:
        try:
            return float(lv) * 3.1 + 1.0
        except ValueError:
            pass
    return None


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

