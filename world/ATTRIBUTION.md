# Attribution for the data in this folder

- `rio_dem.fohdem` is derived from the **Copernicus DEM GLO-30** (30 m global digital surface model, ESA / Copernicus):
  "Contains modified Copernicus DEM data (2021) produced using Copernicus data and information funded by the European
  Union - EU-DEM layers." Free for any use, attribution required. Modifications: stacked from two 1-degree tiles,
  resampled to 15 m cells with a cubic filter, buildings removed in the low city by a grey opening, a sea bed added
  (the model has none). Built by `tools/build_rio_heightmap.py` from `tools/fetch_rio_dem.py`. This is a placeholder until a
  higher resolution terrain model replaces it.
- Ground, bark, road and pavement textures (not stored here: `python tools/fetch_ambientcg.py`) are CC0 from ambientCG.com.
- `district_leblon.fohcity` and `leblon_ground.fohp` (derived, git-ignored; `python tools/fetch_osm_district.py` then
  `python tools/build_district.py`) contain map data (c) **OpenStreetMap contributors**, available under the **Open Database
  License (ODbL) 1.0** (https://www.openstreetmap.org/copyright): street geometry and classes, building footprints, heights and
  levels, parks, the coastline and surveyed points. The derived files are a "produced work" built from that database; the
  game's credits must say "Map data (c) OpenStreetMap contributors (ODbL)". The beach and ground in `leblon_ground.fohp` also
  derive from the Copernicus DEM above.
