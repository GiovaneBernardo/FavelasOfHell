#!/usr/bin/env python3
"""Downloads ambientCG (CC0) texture sets into assets/workshop/ambientCG, where the editor's terrain finds them.

    python tools/fetch_ambientcg.py                 # the four ground layers and the tree bark the terrain and vegetation use
    python tools/fetch_ambientcg.py Rock030 Grass005  # any other ambientCG ids

assets/workshop is git-ignored (third-party content, large), so a fresh checkout runs this once. Without the files the
terrain still renders: a layer with no textures shows a plain colour. Each set comes with its ATTRIBUTION.txt.
"""
import io
import os
import sys
import urllib.request
import zipfile

DEFAULT_SETS = ["Grass004", "Ground048", "Rock051", "Ground054", "Bark006", "Road008A", "Concrete047A"]  # grass, soil, rock, sand, tree bark, road asphalt, pavement
RESOLUTION = "2K-JPG"

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "workshop", "ambientCG"))


def folder_for(asset_id):
    # The editor's workshop names Ground054 "Ground_054_2K-JPG"; every other set keeps the plain id.
    if asset_id == "Ground054":
        return os.path.join(root, "Ground_054_2K-JPG")
    return os.path.join(root, "%s_%s" % (asset_id, RESOLUTION))


def fetch(asset_id):
    folder = folder_for(asset_id)
    if os.path.exists(os.path.join(folder, "%s_%s_Color.jpg" % (asset_id, RESOLUTION))):
        print(asset_id, "already there")
        return
    url = "https://ambientcg.com/get?file=%s_%s.zip" % (asset_id, RESOLUTION)
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (foh asset fetch)"})
    with urllib.request.urlopen(request, timeout=600) as response:
        data = response.read()
    os.makedirs(folder, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        archive.extractall(folder)
        print(asset_id, "->", folder, "(%d files, %d KB)" % (len(archive.namelist()), len(data) // 1024))


if __name__ == "__main__":
    for asset in sys.argv[1:] or DEFAULT_SETS:
        fetch(asset)
