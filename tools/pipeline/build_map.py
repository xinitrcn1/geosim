#!/usr/bin/env python3
"""Build the world map asset assets/map/world.gsmap (docs/07-map-and-filters.md).

Inputs (python3 tools/pipeline/fetch.py --map-only downloads the ones in data/raw/map/):
  data/raw/geoboundaries/*.geojson     ADM1 polygons, matched to assets/scenario/world2020/regions.csv
                                       with the same rules as build_world.py (jurisdictions, claims)
  data/raw/map/ne_10m_admin_0_countries.geojson
                                       territories without ADM1 data (Hong Kong, Puerto Rico, ...)
  data/raw/map/ne_50m_lakes.geojson    large lakes (cut out of the regions: drawn as water)
  data/raw/map/etopo1_5min.asc         ETOPO1 ice-surface relief + bathymetry, 5' nodes
  data/raw/map/koppen_geiger_tif.zip   Koppen-Geiger classes 1991-2020 at 0.1 degree (Beck et al. 2023)

Output layers, all equirectangular (lon -180..180 left to right, lat 90..-90 top to bottom,
cell-centred):
  IDS0  uint16 [ID_H][ID_W]   0 = water, 0xFFFF = land of no region (Antarctica, small islands
                              missing from the polygons), else 1 + the region's row in regions.csv
  ELEV  uint8  [T_H][T_W]     relief code: 0..127 sea (depth = 11000 m x ((127 - c) / 127)^2),
                              128..255 land (height = 8800 m x ((c - 128) / 127)^2); stored with a
                              per-row "sub" filter (each byte minus its left neighbour, mod 256)
  CLIM  uint8  [T_H][T_W]     Koppen-Geiger class 1..30 (0 = none), spread a few cells into the sea
                              so coastal land texels always have a climate
  POLY  int32 stream          the regions as simplified rings (shared borders simplified once per
                              nation, polygons.simplify_group; layout in build_vector), then lakes
  ATLS  u32 size, u32 layers, then layers x size x size bytes: procedural tileable grayscale
                              terrain detail (polygons.terrain_atlas: sand, grass, forest, tundra,
                              rock, snow, wetland, water; periodic value-noise fbm, no images)
The client colours the terrain from ELEV + CLIM + latitude at load time (map_data.cpp).

File layout (little-endian):
  char magic[8] "GSMAP01\\0"; u32 id_w, id_h, terrain_w, terrain_h, nation_count, region_count, section_count
  nations: nation_count x { char iso3[4]; u8 r, g, b, 0; f32 lat, lon }   (fill colour, label point)
  regions: region_count x { char code[12]; u16 nation; u16 flags; f32 lat, lon }   (regions.csv order)
  sections: section_count x { char tag[4]; u32 raw_bytes; u32 packed_bytes; zlib data }

Nation colours: a greedy colouring of the land-adjacency graph (Welsh-Powell order) with
PALETTE, so neighbours never share a colour; stable for a given geometry.

Needs Pillow besides the standard library. Usage:
  python3 tools/pipeline/build_map.py [--raw data/raw] [--assets assets] [--width 8192]
"""

import argparse
import array
import csv
import io
import json
import math
import pathlib
import struct
import sys
import zipfile
import zlib
from collections import Counter, defaultdict

from PIL import Image, ImageDraw, ImageFilter, ImageMath

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import build_world as bw  # noqa: E402
import geo  # noqa: E402
import polygons as pg  # noqa: E402

Image.MAX_IMAGE_PIXELS = None

T_W, T_H = 4320, 2160          # terrain grid: 5 arc-minutes
UNCLAIMED = 0xFFFF
GAP_FILL_PASSES = 5            # coastal land outside the simplified polygons joins the nearest region
CLIMATE_SPREAD_PASSES = 10     # climate classes spread into the sea (coastal texels)

# Pastel political colours (sRGB), distinct from the ocean blues.
PALETTE = [
    (0xE3, 0xC4, 0x86),  # sand
    (0xB8, 0xD4, 0x8C),  # green
    (0xEC, 0xA0, 0x94),  # salmon
    (0xB4, 0xA4, 0xD8),  # lavender
    (0xF0, 0xDC, 0x7E),  # yellow
    (0x92, 0xCC, 0xBC),  # mint
    (0xE6, 0xA9, 0xCC),  # pink
    (0xD8, 0xA0, 0x6C),  # orange
]

KOPPEN_MEMBER = "1991_2020/koppen_geiger_0p1.tif"


def read_csv(path):
    return list(csv.DictReader(line for line in path.open(encoding="utf-8") if not line.startswith("#")))


def circular_mean(points):
    """Area-weighted mean of (lon, lat, w) on the sphere -> (lon, lat)."""
    x = y = z = 0.0
    for lon, lat, w in points:
        lo, la = math.radians(lon), math.radians(lat)
        x += w * math.cos(la) * math.cos(lo)
        y += w * math.cos(la) * math.sin(lo)
        z += w * math.sin(la)
    if x == y == z == 0.0:
        return 0.0, 0.0
    return math.degrees(math.atan2(y, x)), math.degrees(math.atan2(z, math.hypot(x, y)))


class Raster:
    """The region-ID raster (PIL mode I) and polygon painting with holes and antimeridian wrap."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.img = Image.new("I", (w, h), 0)
        self.draw = ImageDraw.Draw(self.img)

    def px(self, ring, shift):
        sx, sy = self.w / 360.0, self.h / 180.0
        return [((p[0] + shift + 180.0) * sx, (90.0 - p[1]) * sy) for p in ring]

    def paint(self, polygons, value):
        for poly in polygons:
            if not poly or len(poly[0]) < 3:
                continue
            lons = [p[0] for p in poly[0]]
            shifts = [0.0]
            if max(lons) > 180.0:
                shifts.append(-360.0)
            if min(lons) < -180.0:
                shifts.append(360.0)
            for sh in shifts:
                outer = self.px(poly[0], sh)
                if len(poly) == 1:
                    self.draw.polygon(outer, fill=value)
                    continue
                # Holes: paint through a mask so enclaves painted earlier survive.
                x0 = max(0, int(math.floor(min(p[0] for p in outer))))
                y0 = max(0, int(math.floor(min(p[1] for p in outer))))
                x1 = min(self.w, int(math.ceil(max(p[0] for p in outer))) + 1)
                y1 = min(self.h, int(math.ceil(max(p[1] for p in outer))) + 1)
                if x1 <= x0 or y1 <= y0:
                    continue
                mask = Image.new("L", (x1 - x0, y1 - y0), 0)
                md = ImageDraw.Draw(mask)
                md.polygon([(x - x0, y - y0) for x, y in outer], fill=255)
                for hole in poly[1:]:
                    if len(hole) >= 3:
                        md.polygon([(x - x0, y - y0) for x, y in self.px(hole, sh)], fill=0)
                self.img.paste(value, (x0, y0, x1, y1), mask)

    def dot(self, lon, lat, value, radius=1.0):
        x, y = (lon + 180.0) * self.w / 360.0, (90.0 - lat) * self.h / 180.0
        self.draw.ellipse([x - radius, y - radius, x + radius, y + radius], fill=value)


def binary_mask(img_i):
    """Mode-I image of 0/1 -> mode-L mask of 0/255."""
    return img_i.convert("L").point(lambda v: 255 if v else 0)


def load_etopo(path):
    """ESRI ASCII grid of nodes (ERDDAP etopo180, 5') -> (PIL F image, ncols, nrows)."""
    with path.open() as f:
        header = {}
        for _ in range(6):
            k, v = f.readline().split()
            header[k.lower()] = v
        ncols, nrows = int(header["ncols"]), int(header["nrows"])
        values = array.array("f", (float(t) for t in f.read().split()))
    if len(values) != ncols * nrows:
        sys.exit(f"{path}: expected {ncols * nrows} values, got {len(values)}")
    img = Image.frombytes("F", (ncols, nrows), values.tobytes())
    # ERDDAP writes the northernmost row first for esriAscii (yllcenter is the south edge).
    return img, ncols, nrows


def elevation_codes(values):
    out = bytearray(len(values))
    for i, h in enumerate(values):
        if h < 0.0:
            c = 127 - int(round(127.0 * math.sqrt(min(1.0, -h / 11000.0))))
        else:
            c = 128 + int(round(127.0 * math.sqrt(min(1.0, h / 8800.0))))
        out[i] = c
    return out


def sub_filter(data, w, h):
    out = bytearray(len(data))
    for y in range(h):
        row = y * w
        prev = 0
        for x in range(w):
            v = data[row + x]
            out[row + x] = (v - prev) & 0xFF
            prev = v
    return out


def spread_into_zero(img, passes):
    """Grow non-zero values (max of the 3x3 neighbourhood) into zero cells."""
    for _ in range(passes):
        grown = img.filter(ImageFilter.MaxFilter(3))
        empty = binary_mask(ImageMath.unsafe_eval("int(a == 0)", a=img))
        img = Image.composite(grown, img, empty)
    return img


def colour_nations(isos, adjacency):
    """Greedy graph colouring; ties go to the least used colour, then to a hash of the code."""
    order = sorted(isos, key=lambda i: (-len(adjacency[i]), i))
    colour, used = {}, Counter()
    for iso in order:
        taken = {colour[n] for n in adjacency[iso] if n in colour}
        free = [k for k in range(len(PALETTE)) if k not in taken] or list(range(len(PALETTE)))
        h = sum(ord(c) * 31 ** i for i, c in enumerate(iso))
        k = min(free, key=lambda k: (used[k], (k + h) % len(PALETTE)))
        colour[iso] = k
        used[k] += 1
    return colour


QUANT = 1e5  # vector coordinates: 1e-5 degree units (~1 m)


def build_vector(regions, region_polys, lake_rings, tol):
    """POLY section: simplified rings per region (shared borders simplified once per
    nation), then the lakes. Layout (int32 little-endian): tolerance x 1e6, region count,
    then per region: ring count, per ring: vertex count, then (lon, lat) in 1e-5 degree
    units, each the delta from the previous vertex of the ring (the first absolute); then
    the lake ring count and the lake rings the same way."""
    by_nation = defaultdict(list)
    for row, polys in region_polys.items():
        by_nation[regions[row]["iso3"]].append(row)
    rings_of_row = defaultdict(list)
    total_in = total_out = 0
    for iso, rows in sorted(by_nation.items()):
        flat, owner = [], []
        for row in rows:
            for poly in region_polys[row]:
                for ring in poly:
                    if len(ring) >= 4:
                        flat.append(ring)
                        owner.append(row)
        total_in += sum(len(r) for r in flat)
        simplified = pg.simplify_group(flat, tol)
        for row, ring in zip(owner, simplified):
            if len(ring) >= 3:
                rings_of_row[row].append(ring)
                total_out += len(ring)
        # Regions whose every ring collapsed (micro-states, small islands: Monaco, Nauru)
        # keep their largest ring, simplified alone at a finer tolerance.
        for row in rows:
            if rings_of_row.get(row):
                continue
            cands = [ring for ring, o in zip(flat, owner) if o == row]
            if not cands:
                continue
            big = max(cands, key=lambda r: abs(pg.ring_area(r)))
            ring = pg.simplify_group([big], tol / 20.0)[0]
            if len(ring) >= 3:
                rings_of_row[row].append(ring)
                total_out += len(ring)
    lakes = [r for r in pg.simplify_group([r for r in lake_rings if len(r) >= 4], tol) if len(r) >= 3]
    out = array.array("i", [int(round(tol * 1e6)), len(regions)])

    def put_ring(ring):
        out.append(len(ring))
        px = py = 0
        for x, y in ring:
            qx, qy = int(round(x * QUANT)), int(round(y * QUANT))
            out.append(qx - px)
            out.append(qy - py)
            px, py = qx, qy

    for row in range(len(regions)):
        rings = rings_of_row.get(row, [])
        out.append(len(rings))
        for ring in rings:
            put_ring(ring)
    out.append(len(lakes))
    for ring in lakes:
        put_ring(ring)
    print(f"vector: {total_in} -> {total_out} vertices ({len(rings_of_row)} regions), {sum(len(r) for r in lakes)} lake vertices")
    return out.tobytes()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", default="data/raw")
    ap.add_argument("--assets", default="assets")
    ap.add_argument("--width", type=int, default=8192, help="region-ID raster width (height = width / 2)")
    ap.add_argument("--tolerance", type=float, default=0.01, help="polygon simplification tolerance (degrees)")
    args = ap.parse_args()
    raw, assets = pathlib.Path(args.raw), pathlib.Path(args.assets)
    scen = assets / "scenario" / "world2020"
    id_w, id_h = args.width, args.width // 2

    regions = read_csv(scen / "regions.csv")
    row_of = {r["region"]: i for i, r in enumerate(regions)}
    isos = sorted({r["iso3"] for r in regions})
    print(f"{len(regions)} regions, {len(isos)} nations")

    # --- regions -> polygons (the assignment of build_world.build_regions) ------------------
    dependencies = bw.read_pairs(scen / "jurisdictions_curated.csv", "sovereign", "iso3")
    claimed = bw.read_pairs(scen / "claims_curated.csv", "claimant", "holder")
    allowed = defaultdict(set)
    for a, b in dependencies | claimed:
        allowed[a].add(b)
    bounds, total = bw.load_boundaries(raw, isos)
    raster = Raster(id_w, id_h)
    mismatched, painted = [], set()
    region_polys = defaultdict(list)  # row -> polygons (lists of rings) for the vector map
    for iso in isos:
        k = 0
        for f, rp, box in bounds[iso]:
            if bw.owned_by_smaller(iso, rp, box, bounds, total, allowed):
                continue
            rid = f"{iso}-{k:03d}"
            k += 1
            row = row_of.get(rid)
            name = (f.get("properties") or {}).get("shapeName") or ""
            if row is None or (name and regions[row]["name"] != name):
                mismatched.append((rid, name))
                continue
            raster.paint(rp, row + 1)
            painted.add(row)
            region_polys[row].extend(rp)
    if mismatched:
        print(f"warning: {len(mismatched)} polygons do not match regions.csv, e.g. {mismatched[:5]}")

    # Territories without ADM1 polygons: Natural Earth admin-0 shapes, painted over the
    # sovereign's duplicate (Hong Kong over China's Hong Kong SAR region, ...).
    ne = json.loads((raw / "map" / "ne_10m_admin_0_countries.geojson").read_text(encoding="utf-8"))["features"]
    ne_by = defaultdict(list)
    for f in ne:
        p = f["properties"]
        for key in ("ADM0_A3", "ISO_A3", "SOV_A3"):
            if p.get(key) and p[key] != "-99":
                ne_by[p[key]].append(f)
                break
    aliases = {"CHI": ["JEY", "GGY"], "XKX": ["KOS"]}
    missing = []
    before = Counter(array.array("H", raster.img.convert("I;16").tobytes()))
    for iso in isos:
        if bounds[iso]:
            continue
        row = row_of.get(f"{iso}-000")
        feats = [f for code in aliases.get(iso, [iso]) for f in ne_by.get(code, [])]
        if row is None or not feats:
            missing.append(iso)
            continue
        for f in feats:
            raster.paint(geo.rings_of(f["geometry"]), row + 1)
            region_polys[row].extend(geo.rings_of(f["geometry"]))
        painted.add(row)
    if missing:
        print("warning: no geometry for", ", ".join(missing))
    # A sovereign's duplicate of a territory (China's Hong Kong SAR region) is overpainted:
    # it gets no polygons in the vector map either (overlapping polygons would clash).
    after = Counter(array.array("H", raster.img.convert("I;16").tobytes()))
    overpainted = [row for row in list(region_polys) if before.get(row + 1, 0) > 0 and after.get(row + 1, 0) < 0.3 * before[row + 1]]
    for row in overpainted:
        del region_polys[row]
    print(f"{len(overpainted)} duplicate regions overpainted by territories: "
          + ", ".join(regions[r]["region"] for r in overpainted[:12]))

    # Lakes are water.
    lakes = Raster(id_w, id_h)
    lake_rings = []
    for f in json.loads((raw / "map" / "ne_50m_lakes.geojson").read_text(encoding="utf-8"))["features"]:
        rp = geo.rings_of(f["geometry"])
        raster.paint(rp, 0)
        lakes.paint(rp, 1)
        lake_rings.extend(ring for poly in rp for ring in poly)

    # Regions too small for a cell (or overdrawn by a neighbour) get a dot at their centroid.
    ids = raster.img
    counts = Counter(array.array("H", ids.convert("I;16").tobytes()))
    dots = 0
    for i, r in enumerate(regions):
        if counts.get(i + 1, 0) == 0:
            raster.dot(float(r["lon"]), float(r["lat"]), i + 1)
            dots += 1
    print(f"{dots} regions drawn as dots")

    # --- relief -------------------------------------------------------------------------------
    etopo, ncols, nrows = load_etopo(raw / "map" / "etopo1_5min.asc")
    print(f"ETOPO1 nodes {ncols}x{nrows}")
    # Nodes sit on cell corners of the cell-centred grids: sample them half a node in.
    def resample(w, h):
        return etopo.resize((w, h), Image.BILINEAR, box=(0.5, 0.5, ncols - 0.5, nrows - 0.5))

    # Coastal land outside the simplified polygons joins the neighbouring region; the rest of
    # the land without a region (Antarctica, islands) is UNCLAIMED.
    elev_hi = resample(id_w, id_h)
    land_gap = ImageMath.unsafe_eval("int(e > 0) * int(l == 0)", e=elev_hi, l=lakes.img)
    del elev_hi
    for _ in range(GAP_FILL_PASSES):
        grown = ids.filter(ImageFilter.MaxFilter(3))
        fill = binary_mask(ImageMath.unsafe_eval("g * int(a == 0) * int(b > 0)", g=land_gap, a=ids, b=grown))
        ids = Image.composite(grown, ids, fill)
    rest = binary_mask(ImageMath.unsafe_eval("g * int(a == 0)", g=land_gap, a=ids))
    ids.paste(UNCLAIMED, (0, 0, id_w, id_h), rest)
    del land_gap, rest, lakes
    id_bytes = ids.convert("I;16").tobytes()

    elev = resample(T_W, T_H)
    elev_codes = elevation_codes(array.array("f", elev.tobytes()))
    del elev

    # --- climate --------------------------------------------------------------------------------
    with zipfile.ZipFile(raw / "map" / "koppen_geiger_tif.zip") as z:
        name = next((n for n in z.namelist() if n.endswith(KOPPEN_MEMBER)), None)
        if name is None:
            sys.exit(f"{KOPPEN_MEMBER} not in koppen_geiger_tif.zip: {z.namelist()[:10]}")
        kg = Image.open(io.BytesIO(z.read(name)))
        kg.load()
    print(f"Koppen-Geiger {kg.size[0]}x{kg.size[1]} mode {kg.mode}")
    kg = Image.frombytes("L", kg.size, kg.tobytes()).resize((T_W, T_H), Image.NEAREST)  # class indices, not palette colours
    kg = kg.point(lambda v: v if v <= 30 else 0)
    kg = spread_into_zero(kg, CLIMATE_SPREAD_PASSES)
    clim = kg.tobytes()

    # --- nations: adjacency, colours, label points -------------------------------------------
    small = ids.resize((id_w // 4, id_h // 4), Image.NEAREST).convert("I;16")
    sw, sh = small.size
    cells = array.array("H", small.tobytes())
    nation_of_row = [r["iso3"] for r in regions]
    nat = [nation_of_row[v - 1] if 0 < v < UNCLAIMED else None for v in cells]
    adjacency = defaultdict(set)
    for y in range(sh):
        base = y * sw
        for x in range(sw):
            a = nat[base + x]
            if a is None:
                continue
            for b in (nat[base + (x + 1) % sw], nat[base + sw + x] if y + 1 < sh else None):
                if b is not None and b != a:
                    adjacency[a].add(b)
                    adjacency[b].add(a)
    colour = colour_nations(isos, adjacency)
    nation_index = {iso: i for i, iso in enumerate(isos)}
    label = {}
    for iso in isos:
        pts = [(float(r["lon"]), float(r["lat"]), float(r["area_km2"] or 1.0)) for r in regions if r["iso3"] == iso]
        label[iso] = circular_mean(pts)

    # --- vector regions ---------------------------------------------------------------------------
    poly_bytes = build_vector(regions, region_polys, lake_rings, args.tolerance)

    # --- write ------------------------------------------------------------------------------------
    out = assets / "map" / "world.gsmap"
    out.parent.mkdir(parents=True, exist_ok=True)
    atlas = pg.terrain_atlas(256)
    atlas_bytes = struct.pack("<II", 256, len(atlas)) + b"".join(atlas)
    sections = [(b"IDS0", id_bytes), (b"ELEV", bytes(sub_filter(elev_codes, T_W, T_H))), (b"CLIM", clim), (b"POLY", poly_bytes),
                (b"ATLS", atlas_bytes)]
    with out.open("wb") as f:
        f.write(b"GSMAP01\0")
        f.write(struct.pack("<7I", id_w, id_h, T_W, T_H, len(isos), len(regions), len(sections)))
        for iso in isos:
            r, g, b = PALETTE[colour[iso]]
            lon, lat = label[iso]
            f.write(struct.pack("<4s4B2f", iso.encode(), r, g, b, 0, lat, lon))
        for i, r in enumerate(regions):
            f.write(struct.pack("<12sHH2f", r["region"].encode()[:12], nation_index[r["iso3"]], 0,
                                float(r["lat"]), float(r["lon"])))
        for tag, data in sections:
            packed = zlib.compress(data, 9)
            f.write(struct.pack("<4sII", tag, len(data), len(packed)))
            f.write(packed)
            print(f"  {tag.decode()}: {len(data) / 1e6:.1f} MB -> {len(packed) / 1e6:.2f} MB")
    print(f"wrote {out} ({out.stat().st_size / 1e6:.2f} MB); {len(painted)} regions with polygons")
    return 0


if __name__ == "__main__":
    sys.exit(main())
