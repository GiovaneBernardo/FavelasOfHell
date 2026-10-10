# Attribution for the data in this folder

- `rio_dem.fohdem` is derived from the **Copernicus DEM GLO-30** (30 m global digital surface model, ESA / Copernicus):
  "Contains modified Copernicus DEM data (2021) produced using Copernicus data and information funded by the European
  Union - EU-DEM layers." Free for any use, attribution required. Modifications: stacked from two 1-degree tiles,
  resampled to 15 m cells with a cubic filter, buildings removed in the low city by a grey opening, a sea bed added
  (the model has none). Built by `tools/build_rio_heightmap.py` from `tools/fetch_rio_dem.py`. This is a placeholder until a
  higher resolution terrain model replaces it.
- Ground, bark, road and pavement textures (not stored here: `python tools/fetch_ambientcg.py`) are CC0 from ambientCG.com.
