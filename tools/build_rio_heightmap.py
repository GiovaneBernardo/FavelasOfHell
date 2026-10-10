#!/usr/bin/env python3
"""Converts GeoTIFF DEM tiles into the engine's heightmap (world/rio_dem.fohdem) around Vidigal, the world origin.

    python tools/build_rio_heightmap.py [dem.tif] [--cell 15] [--preview preview.png]

World coordinates: x east, z south, metres from (lat0, lon0) with a local equirectangular projection (good to a few
metres over 20 km at this latitude). Sea and lagoon cells of a surface model are 0 / 1 m everywhere; this tool finds
them and gives them a sea bed (the DEM has no bathymetry): the ocean falls from the shore toward the floor like the
procedural generator's sea bed, the lagoon is a few metres deep. Land is resampled with a cubic filter onto a uniform
grid and stored as int16 in 1/16 m.

File: "FOHD", version 1, width, height (u32), cell, originX, originZ, unit (f32 metres per int16 step), samples.
"""
import argparse
import math
import os
import struct

import numpy as np
import tifffile
from scipy import ndimage

LAT0, LON0 = -22.9935, -43.2330      # Vidigal: the world origin
X_RANGE = (-6000.0, 11500.0)          # metres east
Z_RANGE = (-8000.0, 4500.0)           # metres south
FLOOR = -200.0
SEABED_SCALE = 420.0                  # m: distance over which the ocean falls toward the floor
LAGOON_DEPTH = 4.0
M_PER_DEG_LAT = 110574.0
M_PER_DEG_LON = 111320.0 * math.cos(math.radians(LAT0))
TILE_LON0, TILE_LAT0, STEP = -44.0, -22.0, 1.0 / 3600.0   # the S23 W044 tile: top-left corner and pixel size


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("dem", nargs="*", default=[os.path.join(here, "..", "data", "dem", "Copernicus_DSM_S23_W044.tif"), os.path.join(here, "..", "data", "dem", "Copernicus_DSM_S24_W044.tif")],
                    help="one GeoTIFF per 1 degree tile, northernmost first (they are stacked); the first one is S23 W044")
    ap.add_argument("--cell", type=float, default=15.0)
    ap.add_argument("--out", default=os.path.join(here, "..", "world", "rio_dem.fohdem"))
    ap.add_argument("--preview", default="")
    args = ap.parse_args()

    dem = np.vstack([tifffile.imread(path).astype(np.float32) for path in args.dem])

    # ---- water: cells at (or below) 1 m that belong to a big flat region ----
    low = dem <= 1.0
    labels, count = ndimage.label(low)
    sizes = ndimage.sum(low, labels, index=np.arange(1, count + 1))
    ocean_label = int(np.argmax(sizes)) + 1
    water = np.zeros_like(low)
    lagoon = np.zeros_like(low)
    for i, size in enumerate(sizes, start=1):
        if size < 300:
            continue
        water |= labels == i
        if i != ocean_label:
            lagoon |= labels == i
    ocean = water & ~lagoon

    # ---- buildings: GLO-30 is a surface model, so the flat city is full of towers. A grey opening (erode, then dilate) with a
    # ~120 m window removes whatever is narrower than that (blocks, towers) and leaves slopes alone (it is the identity on a
    # plane). Only the low city (opened surface below 90 m) is cleaned: beach sand, the sea, hills and mountains are not touched. ----
    ground = dem.copy()
    ground[water] = 1.0e4  # water must not erode the land next to it
    window = 4
    opened = ndimage.maximum_filter(ndimage.minimum_filter(ground, size=window), size=window)
    urban = ~water & (dem > 5.0) & (opened < 90.0)  # only the low city: higher up the opening would shave real peaks
    cleaned = np.where(urban, np.minimum(dem, opened), dem)
    cleaned = np.where(urban, ndimage.gaussian_filter(cleaned, 0.8), cleaned)  # a little smoothing of what is left
    print('buildings removed: %d cells lowered by more than 3 m (max %.0f m)' % (int(((dem - cleaned) > 3).sum()), float((dem - cleaned).max())))
    dem = cleaned.astype(np.float32)

    # ---- sea bed from the distance to the shore ----
    dist_m = ndimage.distance_transform_edt(water) * (STEP * M_PER_DEG_LAT)
    out = dem.copy()
    sea = FLOOR * (1.0 - np.exp(-dist_m / SEABED_SCALE))
    sea = ndimage.gaussian_filter(sea, 6.0)  # the distance field has creases: smooth the bed
    out[ocean] = np.minimum(sea[ocean], -0.5 * np.minimum(dist_m[ocean], 2.0))  # keep the shore itself
    out[lagoon] = -LAGOON_DEPTH * (1.0 - np.exp(-dist_m[lagoon] / 90.0))

    # ---- resample onto the uniform world grid with a cubic filter ----
    cell = args.cell
    w = int(round((X_RANGE[1] - X_RANGE[0]) / cell)) + 1
    h = int(round((Z_RANGE[1] - Z_RANGE[0]) / cell)) + 1
    xs = X_RANGE[0] + np.arange(w) * cell
    zs = Z_RANGE[0] + np.arange(h) * cell
    lon = LON0 + xs / M_PER_DEG_LON
    lat = LAT0 - zs / M_PER_DEG_LAT
    px = (lon - TILE_LON0) / STEP - 0.5
    py = (TILE_LAT0 - lat) / STEP - 0.5
    gx, gy = np.meshgrid(px, py)
    grid = ndimage.map_coordinates(out, [gy, gx], order=3, mode="nearest").astype(np.float32)
    grid = np.maximum(grid, FLOOR)

    unit = 1.0 / 16.0
    q = np.clip(np.round(grid / unit), -32768, 32767).astype(np.int16)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(struct.pack("<4sIIIffff", b"FOHD", 1, w, h, cell, X_RANGE[0], Z_RANGE[0], unit))
        f.write(q.tobytes())
    print("wrote %s: %d x %d at %.0f m (%.1f x %.1f km), %.1f MB, min %.1f max %.1f m" %
          (args.out, w, h, cell, w * cell / 1000, h * cell / 1000, os.path.getsize(args.out) / 1e6, grid.min(), grid.max()))

    if args.preview:
        from PIL import Image
        gy_, gx_ = np.gradient(grid, cell)
        shade = np.clip(0.5 + 0.9 * (-gx_ * 0.6 + -gy_ * 0.6), 0, 1)
        img = np.zeros((h, w, 3), np.float32)
        land = grid > 0.3
        height_color = np.clip(grid / 500.0, 0, 1)
        img[..., 0] = np.where(land, 0.15 + 0.5 * height_color, 0.05) * shade
        img[..., 1] = np.where(land, 0.35 + 0.3 * (1 - height_color), 0.2 + 0.2 * (grid / FLOOR).clip(0, 1) * -1 + 0.2) * shade
        img[..., 2] = np.where(land, 0.12, 0.45 + 0.3 * (grid / FLOOR).clip(0, 1)) * (0.6 + 0.4 * shade)
        Image.fromarray((img.clip(0, 1) * 255).astype(np.uint8)).save(args.preview)
        print("preview", args.preview)


if __name__ == "__main__":
    main()
