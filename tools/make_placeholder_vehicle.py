"""Writes a low-poly placeholder sedan (glTF with an embedded buffer) so there is something to drive before real models are imported.

    python tools/make_placeholder_vehicle.py [output.gltf]

The pieces are separate meshes like a typical Sketchfab export would have them (body, cabin, a tire and a rim per wheel, lights),
with the vertices baked into model space (the origin of a wheel mesh is NOT its centre), so the Asset Forge has to find the wheels
and their pivots the way it will for a real model. Fictional design, original geometry.

Conventions: +Z forward, +Y up, +X to the left of the car, meters.
"""
import base64
import json
import math
import struct
import sys

HALF_WIDTH = 0.86
WHEEL_RADIUS = 0.33
WHEEL_WIDTH = 0.22
WHEEL_X = 0.80
WHEEL_Z = (1.38, -1.32)  # front, rear axle


class Mesh:
    def __init__(self, name, material):
        self.name = name
        self.material = material
        self.positions = []
        self.normals = []
        self.indices = []
        self.center = (0.0, 0.0, 0.0)  # inside the shape being emitted: faces are made to point away from it

    def _face(self, points):
        a, b, c = points[0], points[1], points[2]
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        n = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
        length = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) or 1.0
        n = tuple(x / length for x in n)
        mid = [sum(p[k] for p in points) / len(points) for k in range(3)]
        if sum(n[k] * (mid[k] - self.center[k]) for k in range(3)) < 0.0:
            points = [points[0]] + list(reversed(points[1:]))
            n = tuple(-x for x in n)
        return points, n

    def quad(self, a, b, c, d):
        points, n = self._face([a, b, c, d])
        base = len(self.positions)
        for p in points:
            self.positions.append(p)
            self.normals.append(n)
        self.indices += [base, base + 1, base + 2, base, base + 2, base + 3]

    def tri(self, a, b, c):
        points, n = self._face([a, b, c])
        base = len(self.positions)
        for p in points:
            self.positions.append(p)
            self.normals.append(n)
        self.indices += [base, base + 1, base + 2]


def prism(mesh, profile, x0, x1):
    """Extrudes a convex side profile [(z, y), ...] from x0 to x1."""
    n = len(profile)
    mesh.center = ((x0 + x1) / 2, sum(p[1] for p in profile) / n, sum(p[0] for p in profile) / n)
    for i in range(n):
        z0, y0 = profile[i]
        z1, y1 = profile[(i + 1) % n]
        mesh.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z1), (x1, y1, z1))
    for i in range(1, n - 1):
        a, b, c = profile[0], profile[i], profile[i + 1]
        mesh.tri((x1, a[1], a[0]), (x1, b[1], b[0]), (x1, c[1], c[0]))
        mesh.tri((x0, a[1], a[0]), (x0, c[1], c[0]), (x0, b[1], b[0]))


def box(mesh, cx, cy, cz, hx, hy, hz):
    mesh.center = (cx, cy, cz)
    p = lambda sx, sy, sz: (cx + sx * hx, cy + sy * hy, cz + sz * hz)
    mesh.quad(p(1, -1, -1), p(1, 1, -1), p(1, 1, 1), p(1, -1, 1))     # +X
    mesh.quad(p(-1, -1, 1), p(-1, 1, 1), p(-1, 1, -1), p(-1, -1, -1))  # -X
    mesh.quad(p(-1, 1, -1), p(-1, 1, 1), p(1, 1, 1), p(1, 1, -1))     # +Y
    mesh.quad(p(-1, -1, 1), p(-1, -1, -1), p(1, -1, -1), p(1, -1, 1))  # -Y
    mesh.quad(p(1, -1, 1), p(1, 1, 1), p(-1, 1, 1), p(-1, -1, 1))     # +Z
    mesh.quad(p(-1, -1, -1), p(-1, 1, -1), p(1, 1, -1), p(1, -1, -1))  # -Z


def cylinder_x(mesh, cx, cy, cz, radius, width, segments=22):
    """A cylinder whose axis is X (a wheel)."""
    mesh.center = (cx, cy, cz)
    ring = [(math.cos(2 * math.pi * i / segments), math.sin(2 * math.pi * i / segments)) for i in range(segments)]
    x0, x1 = cx - width / 2, cx + width / 2
    for i in range(segments):
        (c0, s0), (c1, s1) = ring[i], ring[(i + 1) % segments]
        a = (cy + c0 * radius, cz + s0 * radius)
        b = (cy + c1 * radius, cz + s1 * radius)
        mesh.quad((x0, a[0], a[1]), (x0, b[0], b[1]), (x1, b[0], b[1]), (x1, a[0], a[1]))
        mesh.tri((x1, cy, cz), (x1, a[0], a[1]), (x1, b[0], b[1]))
        mesh.tri((x0, cy, cz), (x0, b[0], b[1]), (x0, a[0], a[1]))


def build():
    meshes = []
    # side profile (z, y), counter-clockwise seen from +X: rear bottom -> front bottom -> up the nose -> hood -> cabin base -> trunk
    body_profile = [(-2.12, 0.30), (2.10, 0.30), (2.14, 0.58), (1.98, 0.80), (1.05, 0.90), (-1.25, 0.92), (-1.98, 0.88), (-2.14, 0.72)]
    body = Mesh("Body", 0)
    prism(body, body_profile, -HALF_WIDTH, HALF_WIDTH)
    meshes.append(body)

    cabin_profile = [(-1.18, 0.91), (1.02, 0.89), (0.48, 1.44), (-0.78, 1.44)]
    cabin = Mesh("Cabin", 1)
    prism(cabin, cabin_profile, -0.77, 0.77)
    meshes.append(cabin)

    bumper = Mesh("Bumpers", 4)
    box(bumper, 0.0, 0.40, 2.13, 0.84, 0.10, 0.06)
    box(bumper, 0.0, 0.40, -2.13, 0.84, 0.10, 0.06)
    meshes.append(bumper)

    lights = Mesh("Headlights", 3)
    for sx in (-1, 1):
        box(lights, sx * 0.60, 0.68, 2.12, 0.20, 0.07, 0.03)
    meshes.append(lights)
    tails = Mesh("Taillights", 5)
    for sx in (-1, 1):
        box(tails, sx * 0.62, 0.74, -2.12, 0.19, 0.06, 0.03)
    meshes.append(tails)

    names = [("FL", 1, WHEEL_Z[0]), ("FR", -1, WHEEL_Z[0]), ("RL", 1, WHEEL_Z[1]), ("RR", -1, WHEEL_Z[1])]
    for tag, side, z in names:
        tire = Mesh("Tire_" + tag, 2)
        cylinder_x(tire, side * WHEEL_X, WHEEL_RADIUS, z, WHEEL_RADIUS, WHEEL_WIDTH)
        meshes.append(tire)
        rim = Mesh("Rim_" + tag, 6)
        cylinder_x(rim, side * (WHEEL_X + 0.005), WHEEL_RADIUS, z, WHEEL_RADIUS * 0.62, WHEEL_WIDTH + 0.012, 12)
        meshes.append(rim)
    return meshes


MATERIALS = [
    ("Paint", [0.72, 0.13, 0.10, 1.0], 0.35, 0.38, None),
    ("Glass", [0.04, 0.07, 0.10, 1.0], 0.0, 0.08, None),
    ("Tire", [0.025, 0.025, 0.028, 1.0], 0.0, 0.85, None),
    ("Headlight", [0.95, 0.93, 0.8, 1.0], 0.0, 0.2, [1.5, 1.4, 1.1]),
    ("Plastic", [0.05, 0.05, 0.055, 1.0], 0.0, 0.6, None),
    ("Taillight", [0.6, 0.02, 0.02, 1.0], 0.0, 0.3, [0.8, 0.02, 0.02]),
    ("Rim", [0.62, 0.64, 0.68, 1.0], 0.9, 0.3, None),
]


def write(path):
    meshes = build()
    blob = bytearray()
    views, accessors, gltf_meshes, nodes = [], [], [], []

    def add(data, target, comp, count, kind, minmax=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
        blob.extend(data)
        acc = {"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": kind}
        if minmax:
            acc["min"], acc["max"] = minmax
        accessors.append(acc)
        return len(accessors) - 1

    for i, m in enumerate(meshes):
        pos = b"".join(struct.pack("<3f", *p) for p in m.positions)
        nor = b"".join(struct.pack("<3f", *n) for n in m.normals)
        idx = b"".join(struct.pack("<I", v) for v in m.indices)
        lo = [min(p[k] for p in m.positions) for k in range(3)]
        hi = [max(p[k] for p in m.positions) for k in range(3)]
        a_pos = add(pos, 34962, 5126, len(m.positions), "VEC3", (lo, hi))
        a_nor = add(nor, 34962, 5126, len(m.normals), "VEC3")
        a_idx = add(idx, 34963, 5125, len(m.indices), "SCALAR")
        gltf_meshes.append({"name": m.name, "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nor}, "indices": a_idx, "material": m.material}]})
        nodes.append({"name": m.name, "mesh": i})

    materials = []
    for name, color, metallic, roughness, emissive in MATERIALS:
        mat = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": color, "metallicFactor": metallic, "roughnessFactor": roughness}}
        if emissive:
            mat["emissiveFactor"] = emissive
        materials.append(mat)

    doc = {
        "asset": {"version": "2.0", "generator": "tools/make_placeholder_vehicle.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": gltf_meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode()}],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    print(f"wrote {path}: {len(meshes)} meshes, {sum(len(m.indices) // 3 for m in meshes)} triangles")


if __name__ == "__main__":
    write(sys.argv[1] if len(sys.argv) > 1 else "assets/vehicles/placeholder_sedan/placeholder_sedan.gltf")
