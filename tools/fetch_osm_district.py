#!/usr/bin/env python3
"""Downloads the OpenStreetMap data of the Leblon / Ipanema district (roads, buildings, beach, parks, water, coast).

    python tools/fetch_osm_district.py [--bbox S W N E]     # writes data/osm/<layer>.json (git-ignored)

Then run tools/build_district.py to turn it into the engine's files (world/district_leblon.fohcity and
world/leblon_ground.fohp).

License: (c) OpenStreetMap contributors, Open Database License (ODbL) 1.0. Attribution is recorded in
world/ATTRIBUTION.md. Only geometry and tags (heights, levels, road classes) are used; street names are real places and
may stay (they are geography, not brands), shops and people are invented by the game.
"""
import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "data", "osm"))
ENDPOINTS = ["https://overpass-api.de/api/interpreter", "https://maps.mail.ru/osm/tools/overpass/api/interpreter"]
UA = "foh-game-dev/0.1 (open-world game prototype; https://github.com/GiovaneBernardo)"

# Leblon (west) to Arpoador (east), the beach to the Lagoa's south shore. Origin of the world is Vidigal.
DEFAULT_BBOX = (-22.9945, -43.2310, -22.9640, -43.1830)


def query(q, name):
    path = os.path.join(OUT, name + ".json")
    if os.path.exists(path):
        print("already there:", path)
        return
    os.makedirs(OUT, exist_ok=True)
    data = urllib.parse.urlencode({"data": q}).encode()
    for attempt in range(6):
        url = ENDPOINTS[attempt % len(ENDPOINTS)]
        try:
            req = urllib.request.Request(url, data=data, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=300) as r:
                body = r.read()
            doc = json.loads(body)
            if not doc.get("elements"):
                raise RuntimeError("empty answer")
            with open(path, "wb") as f:
                f.write(body)
            print("%s: %d elements (%.1f MB)" % (name, len(doc["elements"]), len(body) / 1e6))
            return
        except Exception as e:  # noqa: BLE001 - overpass fails in many ways; just retry elsewhere
            print("  %s attempt %d failed: %s" % (name, attempt + 1, e), file=sys.stderr)
            time.sleep(8 * (attempt + 1))
    raise SystemExit("could not download " + name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bbox", nargs=4, type=float, default=DEFAULT_BBOX, metavar=("S", "W", "N", "E"))
    args = ap.parse_args()
    s, w, n, e = args.bbox
    box = "(%f,%f,%f,%f)" % (s, w, n, e)
    head = "[out:json][timeout:180];"
    query(head + 'way["highway"]' + box + ";out tags geom;", "highways")
    query(head + '(way["building"]' + box + ';relation["building"]' + box + ";);out tags geom;", "buildings")
    query(head + '(way["natural"~"beach|water|coastline|wetland|scrub|wood|bare_rock|cliff"]' + box + ';way["leisure"~"park|garden|pitch|playground|track|swimming_pool"]' + box +
          ';way["landuse"~"grass|forest|recreation_ground|cemetery|railway"]' + box + ';way["waterway"]' + box + ';relation["natural"="water"]' + box + ';relation["leisure"="park"]' + box + ";);out tags geom;", "areas")
    query(head + "rel(2138398);out geom;", "lagoa")  # the Lagoa Rodrigo de Freitas: the bbox query returns it without members
    query(head + '(node["highway"="bus_stop"]' + box + ';node["highway"="traffic_signals"]' + box + ';node["highway"="crossing"]' + box + ';node["natural"="tree"]' + box +
          ';node["amenity"~"bench|waste_basket|toilets|drinking_water"]' + box + ';node["emergency"="lifeguard"]' + box + ';node["leisure"~"pitch|beach_resort"]' + box + ';node["man_made"~"street_lamp|surveillance"]' + box +
          ';node["highway"="street_lamp"]' + box + ";);out;", "points")


if __name__ == "__main__":
    main()
