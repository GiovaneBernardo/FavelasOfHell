#!/usr/bin/env python3
"""Downloads the two Copernicus GLO-30 DEM tiles that cover Rio de Janeiro (30 m, open data on AWS, no login):
S23 W044 (lat -23..-22) and S24 W044 (-24..-23; Vidigal sits 700 m north of the tile edge).

    python tools/fetch_rio_dem.py        # writes data/dem/Copernicus_DSM_S23_W044.tif and ..._S24_... (git-ignored)

License: Copernicus DEM GLO-30, "Contains modified Copernicus DEM data (2021) produced using Copernicus data and
information funded by the European Union - EU-DEM layers". Free for any use, attribution required (see README).
GLO-30 is a surface model: buildings and tree canopy are inside the heights. Replace with a better terrain model when
one is available (tools/build_rio_heightmap.py takes any GeoTIFF).
"""
import os
import urllib.request

BASE = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "dem"))
TILES = ["S23_00_W044_00", "S24_00_W044_00"]


def fetch(code):
    name = "Copernicus_DSM_COG_10_%s_DEM" % code
    url = "https://copernicus-dem-30m.s3.amazonaws.com/%s/%s.tif" % (name, name)
    out = os.path.join(BASE, "Copernicus_DSM_%s_%s.tif" % (code[:3], code[7:11]))
    if os.path.exists(out):
        print("already there:", out)
        return
    os.makedirs(BASE, exist_ok=True)
    with urllib.request.urlopen(url, timeout=600) as r, open(out, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
    print("saved", out, os.path.getsize(out) // 1024, "KB")


if __name__ == "__main__":
    for code in TILES:
        fetch(code)
