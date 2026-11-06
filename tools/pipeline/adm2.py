"""Clusters of counties (ADM2) inside the world's states (ADM1): the simulation's regions.

States are the ADM1 units of build_world.build_regions (legal units: governors, district
seats, redrawable by their nation's government). Each state is split into clusters of its
ADM2 units (counties, districts, prefectures) sized by population: a state gets
round(population / PEOPLE_PER_CLUSTER) clusters, at least one and at most its number of
ADM2 units, so the desert north of Niger is one cluster and its populous south many, Texas
about sixty, China thousands. The totals are capped: MAX_CLUSTERS regions, and POP_BUDGET pops (each region holds
4 pops per identity group of its nation; the pop container holds 500000 objects);
the threshold rises until both fit.

Inputs:
  data/raw/geoboundaries_adm2/*.geojson   geoBoundaries gbOpen ADM2 (simplified)
  data/raw/ghs_pop/*.zip                  GHS-POP R2023A 2020, 30 arc-seconds (JRC), tiles
Population per ADM2 unit and per state comes from GHS-POP aggregated to a 2.5-arc-minute
grid (each 30" cell into a 5 x 5 block), summed over the cells each unit covers when
rasterised on that grid (units smaller than a cell take the cell at their centroid, scaled
by area). Urban population counts the 30" cells with at least 1500 people per km2 (the
density threshold of the Degree of Urbanisation's urban centres; Dijkstra et al. 2021);
the loader-facing urban_share is that share rescaled to the national urbanisation rate.

An ADM2 unit belongs to the state it overlaps most (on the grid). Units a sovereign's or
claimant's file shares with a dependent / held territory (Taiwan's counties in China's
file) are left to that territory. States without ADM2 data are one unit (their own
polygon). Clustering: recursive bisection of the state's units by population along the
wider axis of their centroids, then a contiguity repair (a detached piece of a cluster
joins the neighbouring cluster it borders most; islands without a neighbour stay).
"""

import array
import io
import json
import math
import pathlib
import zipfile
from collections import Counter, defaultdict

import geo

GRID_W, GRID_H = 8640, 4320          # 2.5 arc-minutes
SUB = 5                              # 30" cells per grid cell, per axis
PEOPLE_PER_CLUSTER = 0.5e6
MAX_CLUSTERS = 28000
POP_BUDGET = 450000                  # simulation pops (4 per identity group per region); the
                                     # container holds 500000 and long campaigns add some
URBAN_DENSITY = 1500.0               # people per km2 (DEGURBA urban centre cells)
CELL30_KM2 = (6371.0088 * math.pi / 180.0 / 120.0) ** 2   # a 30" cell at the equator


def population_grid(raw):
    """-> (pop, urban): array('f') of GRID_W x GRID_H, people per 2.5' cell (2020)."""
    from PIL import Image, ImageMath, TiffImagePlugin
    Image.MAX_IMAGE_PIXELS = None
    # GHS-POP tiles are BigTIFF float64 (LZW, floating-point predictor): Pillow decodes them
    # through libtiff once told the pixel layout.
    TiffImagePlugin.OPEN_INFO[(b"II", 1, (3,), 1, (64,), ())] = ("F", "F;64F")
    pop = array.array("f", bytes(4 * GRID_W * GRID_H))
    urb = array.array("f", bytes(4 * GRID_W * GRID_H))
    tiles = sorted((raw / "ghs_pop").glob("*.zip"))
    if not tiles:
        raise SystemExit("no GHS-POP tiles in data/raw/ghs_pop (python3 tools/pipeline/fetch.py --adm2-only)")
    for t in tiles:
        with zipfile.ZipFile(t) as z:
            name = next(n for n in z.namelist() if n.lower().endswith(".tif"))
            img = Image.open(io.BytesIO(z.read(name)))
            img.load()
        tags = img.tag_v2
        sx, sy = tags[33550][0], tags[33550][1]          # pixel scale (degrees)
        tie = tags[33922]                                  # (i, j, k, x, y, z)
        lon0, lat0 = tie[3] - tie[0] * sx, tie[4] + tie[1] * sy
        w, h = img.size
        if img.mode != "F":
            img = img.convert("F")
        img = ImageMath.unsafe_eval("max(a, 0.0)", a=img)  # no-data (-200) -> 0
        lat_mid = lat0 - h * sy / 2.0
        thr = URBAN_DENSITY * CELL30_KM2 * max(0.05, math.cos(math.radians(lat_mid)))
        urban = ImageMath.unsafe_eval("a * float(a >= t)", a=img, t=thr)
        # 30" -> 2.5': sum 5 x 5 blocks (reduce() averages).
        ox = round((lon0 + 180.0) * 24.0)
        oy = round((90.0 - lat0) * 24.0)
        for src, dst in ((img, pop), (urban, urb)):
            red = src.reduce(SUB)
            rw, rh = red.size
            vals = array.array("f", red.tobytes())
            for y in range(rh):
                gy = oy + y
                if gy < 0 or gy >= GRID_H:
                    continue
                row = vals[y * rw:(y + 1) * rw]
                base = gy * GRID_W
                for x, v in enumerate(row):
                    if v > 0.0:
                        gx = ox + x
                        if 0 <= gx < GRID_W:
                            dst[base + gx] += v * SUB * SUB
    return pop, urb


def load_adm2(raw, iso):
    path = raw / "geoboundaries_adm2" / f"{iso}.geojson"
    if not path.exists():
        return []
    try:
        feats = json.loads(path.read_text()).get("features", [])
    except json.JSONDecodeError:
        return []
    out = []
    for k, f in enumerate(feats):
        rp = geo.rings_of(f.get("geometry") or {"type": "None"})
        if not rp:
            continue
        p = f.get("properties") or {}
        name = " ".join(str(p.get("shapeName") or f"{iso} {k}").split())  # some names hold newlines
        out.append({"key": p.get("shapeID") or f"{iso}-ADM2-{k}", "name": name, "polys": rp})
    return out


def _bisect(items, k):
    """items: list of (x, y, w, payload) -> k lists, population-balanced."""
    if k <= 1 or len(items) <= 1:
        return [items]
    k1 = k // 2
    total = sum(it[2] for it in items) or 1.0
    xs = [it[0] for it in items]
    ys = [it[1] for it in items]
    axis = 0 if (max(xs) - min(xs)) >= (max(ys) - min(ys)) else 1
    items = sorted(items, key=lambda it: (it[axis], it[1 - axis]))
    target = total * k1 / k
    acc, cut = 0.0, len(items) - 1
    for i, it in enumerate(items):
        if acc + it[2] / 2.0 > target:
            cut = i
            break
        acc += it[2]
    # Each side keeps at least as many units as clusters it must make (k <= len(items)).
    lo, hi = min(k1, len(items) - 1), max(1, len(items) - (k - k1))
    cut = max(lo, min(cut, hi)) if lo <= hi else hi
    return _bisect(items[:cut], k1) + _bisect(items[cut:], k - k1)


def build(raw, regions, allowed, nation_urban, pops_per_region=None, log=print):
    """regions: build_world's ADM1 regions (the states), each with 'polys' and 'iso3'.
    -> (clusters, adm2_rows, adjacency, states)"""
    from build_map import Raster  # lazy: build_map imports build_world
    log("population grid (GHS-POP 2020) ...")
    pop, urb = population_grid(raw)
    log(f"  world population on the grid: {sum(pop) / 1e9:.2f} bn")

    # --- rasters: states and ADM2 units ----------------------------------------------------
    st_r = Raster(GRID_W, GRID_H)
    for i, s in enumerate(regions):
        if s.get("polys"):
            st_r.paint(s["polys"], i + 1)
    units = []   # {key, name, iso, polys, state?}
    by_iso = defaultdict(list)
    for s in regions:
        by_iso[s["iso3"]].append(s)
    for iso in sorted(by_iso):
        for u in load_adm2(raw, iso):
            u["iso"] = iso
            units.append(u)
    log(f"  {len(units)} ADM2 units")
    ad_r = Raster(GRID_W, GRID_H)
    for i, u in enumerate(units):
        ad_r.paint(u["polys"], i + 1)
    S = array.array("i", st_r.img.tobytes())
    A = array.array("i", ad_r.img.tobytes())
    del st_r, ad_r

    # --- one pass over the grid: unit populations, unit x state overlaps -------------------
    log("  overlaying units, states and population ...")
    upop = defaultdict(float)
    uurb = defaultdict(float)
    over = defaultdict(Counter)       # unit -> state -> cells (weighted by 1 + pop)
    spop = defaultdict(float)
    for idx in range(GRID_W * GRID_H):
        a = A[idx]
        s = S[idx]
        if not a and not s:
            continue
        p = pop[idx]
        if a:
            upop[a - 1] += p
            uurb[a - 1] += urb[idx]
            if s:
                over[a - 1][s - 1] += 1.0 + p
        if s:
            spop[s - 1] += p

    def cell_of(lon, lat):
        x = min(GRID_W - 1, max(0, int((lon + 180.0) * GRID_W / 360.0)))
        y = min(GRID_H - 1, max(0, int((90.0 - lat) * GRID_H / 180.0)))
        return y * GRID_W + x

    # --- units -> states -------------------------------------------------------------------
    state_units = defaultdict(list)
    left_to_territory = 0
    for i, u in enumerate(units):
        area, (lon, lat) = geo.area_and_centroid(u["polys"])
        u["area"], u["lon"], u["lat"] = area, lon, lat
        cands = over.get(i)
        if not cands:
            c = cell_of(lon, lat)
            s = S[c] - 1
            cands = Counter({s: 1.0}) if s >= 0 else Counter()
            if i not in upop:
                cell_km2 = (6371.0 * math.pi / 180.0 / 24.0) ** 2 * max(0.05, math.cos(math.radians(lat)))
                share = min(1.0, area / cell_km2) if cell_km2 > 0 else 1.0
                upop[i] = pop[c] * share
                uurb[i] = urb[c] * share
        best = cands.most_common(1)[0][0] if cands else -1
        if best >= 0 and regions[best]["iso3"] != u["iso"]:
            if regions[best]["iso3"] in allowed.get(u["iso"], set()):
                left_to_territory += 1
                continue  # the dependent / held territory's own data covers it
            same = [(n, s) for s, n in cands.items() if regions[s]["iso3"] == u["iso"]]
            best = max(same)[1] if same else -1
        if best < 0:
            mine = [j for j, s in enumerate(regions) if s["iso3"] == u["iso"]]
            if not mine:
                continue
            best = min(mine, key=lambda j: (regions[j]["lon"] - lon) ** 2 + (regions[j]["lat"] - lat) ** 2)
        u["state"] = best
        state_units[best].append(i)
    log(f"  {left_to_territory} units left to dependent / held territories")

    # States without ADM2 data: one unit, the state itself.
    for j, s in enumerate(regions):
        if state_units.get(j):
            continue
        units.append({"key": s["id"], "name": s["name"], "iso": s["iso3"], "polys": s.get("polys") or [], "state": j,
                      "area": s["area_km2"], "lon": s["lon"], "lat": s["lat"], "whole_state": True})
        i = len(units) - 1
        upop[i] = spop.get(j, 0.0)
        uurb[i] = 0.0
        if upop[i] <= 0.0:
            c = cell_of(s["lon"], s["lat"])
            upop[i], uurb[i] = pop[c], urb[c]
        state_units[j].append(i)

    # --- adjacency of units (grid neighbours) ----------------------------------------------
    log("  unit adjacency ...")
    touch = Counter()
    for y in range(GRID_H):
        base = y * GRID_W
        row = A[base:base + GRID_W]
        below = A[base + GRID_W:base + 2 * GRID_W] if y + 1 < GRID_H else None
        prev = 0
        for x, a in enumerate(row):
            if a:
                if prev and prev != a:
                    touch[(min(a, prev) - 1, max(a, prev) - 1)] += 1
                if below is not None:
                    b = below[x]
                    if b and b != a:
                        touch[(min(a, b) - 1, max(a, b) - 1)] += 1
            prev = a
        # antimeridian wrap
        if row[0] and row[-1] and row[0] != row[-1]:
            touch[(min(row[0], row[-1]) - 1, max(row[0], row[-1]) - 1)] += 1
    nbr = defaultdict(dict)
    for (a, b), n in touch.items():
        nbr[a][b] = n
        nbr[b][a] = n

    # --- clusters per state ----------------------------------------------------------------
    def count_for(j, people):
        n = len(state_units[j])
        return max(1, min(n, int(round(spop_u[j] / people))))

    spop_u = {j: sum(upop[i] for i in us) for j, us in state_units.items()}
    ppr = pops_per_region or {}
    people = PEOPLE_PER_CLUSTER

    def over(people):
        n = pops = 0
        for j in state_units:
            k = count_for(j, people)
            n += k
            pops += k * ppr.get(regions[j]["iso3"], 4)
        return n > MAX_CLUSTERS or pops > POP_BUDGET
    while over(people):
        people *= 1.05
    log(f"  people per cluster: {people / 1e6:.2f} M -> {sum(count_for(j, people) for j in state_units)} clusters")

    cluster_of = {}
    clusters_by_state = defaultdict(list)
    for j in sorted(state_units):
        us = state_units[j]
        k = count_for(j, people)
        lat0 = math.radians(sum(units[i]["lat"] for i in us) / len(us))
        items = [(units[i]["lon"] * math.cos(lat0), units[i]["lat"], upop[i] + 1.0, i) for i in us]
        parts = _bisect(items, k)
        groups = [[it[3] for it in p] for p in parts if p]
        # Contiguity repair: detached pieces join the cluster they border most.
        for _ in range(4):
            cid = {}
            for g, members in enumerate(groups):
                for i in members:
                    cid[i] = g
            moved = False
            for g, members in enumerate(groups):
                if len(members) < 2:
                    continue
                comps, seen = [], set()
                mset = set(members)
                for i in members:
                    if i in seen:
                        continue
                    comp, stack = [], [i]
                    seen.add(i)
                    while stack:
                        x = stack.pop()
                        comp.append(x)
                        for y in nbr.get(x, {}):
                            if y in mset and y not in seen:
                                seen.add(y)
                                stack.append(y)
                    comps.append(comp)
                if len(comps) < 2:
                    continue
                comps.sort(key=lambda c: -sum(upop[i] for i in c))
                for comp in comps[1:]:
                    border = Counter()
                    for x in comp:
                        for y, n in nbr.get(x, {}).items():
                            if y in cid and cid[y] != g and units[y].get("state") == j:
                                border[cid[y]] += n
                    if border:
                        tgt = border.most_common(1)[0][0]
                        for x in comp:
                            members.remove(x)
                            groups[tgt].append(x)
                            cid[x] = tgt
                        moved = True
            groups = [g for g in groups if g]
            if not moved:
                break
        for members in groups:
            c = len(clusters_by_state[j])
            clusters_by_state[j].append(members)
            for i in members:
                cluster_of[i] = (j, c)

    # --- outputs ---------------------------------------------------------------------------
    states = []
    clusters = []
    cluster_index = {}
    nation_pop = defaultdict(float)
    for j in clusters_by_state:
        nation_pop[regions[j]["iso3"]] += spop_u[j]
    for j, s in enumerate(regions):
        states.append({"state": s["id"], "iso3": s["iso3"], "name": s["name"], "code": s["code"], "lat": s["lat"], "lon": s["lon"]})
        for c, members in enumerate(clusters_by_state.get(j, [])):
            p = sum(upop[i] for i in members)
            a = sum(units[i]["area"] for i in members) or 1.0
            lon = sum(units[i]["lon"] * units[i]["area"] for i in members) / a
            lat = sum(units[i]["lat"] * units[i]["area"] for i in members) / a
            top = max(members, key=lambda i: upop[i])
            name = units[top]["name"] if len(members) == 1 or units[top].get("whole_state") else f"{units[top]['name']} area"
            if units[top].get("whole_state"):
                name = s["name"]
            rid = f"{s['id']}-{c:02d}"
            cluster_index[(j, c)] = len(clusters)
            clusters.append({"id": rid, "iso3": s["iso3"], "state": s["id"], "name": name, "code": s["code"],
                             "lat": lat, "lon": lon, "area_km2": a, "pop": p, "urban_raw": sum(uurb[i] for i in members) / p if p > 0 else 0.0})
    # Population shares within each nation (a sliver for empty clusters) and urban shares
    # rescaled to the national rate.
    by_n = defaultdict(list)
    for c in clusters:
        by_n[c["iso3"]].append(c)
    for iso, cs in by_n.items():
        shares = [max(c["pop"], 1e-6 * max(1.0, nation_pop[iso])) for c in cs]
        tot = sum(shares) or 1.0
        for c, s in zip(cs, shares):
            c["pop_share"] = s / tot
        un = nation_urban.get(iso, 0.5)
        mean = sum(c["urban_raw"] * c["pop_share"] for c in cs)
        for c in cs:
            c["urban_share"] = min(1.0, c["urban_raw"] * un / mean) if mean > 0 else un
    adm2_rows = []
    for i, u in enumerate(units):
        if i not in cluster_of:
            continue
        adm2_rows.append({"key": u["key"], "name": u["name"], "region": clusters[cluster_index[cluster_of[i]]]["id"],
                          "pop": round(upop[i]), "unit": i})
    # Grouped by cluster (and so by state): the map draws a state's units as one range.
    adm2_rows.sort(key=lambda a: (a["region"], -a["pop"]))
    # Cluster adjacency (within and across states of a nation, as before for regions).
    cadj = Counter()
    for (a, b), n in touch.items():
        if a in cluster_of and b in cluster_of:
            ca, cb = cluster_index[cluster_of[a]], cluster_index[cluster_of[b]]
            if ca != cb and clusters[ca]["iso3"] == clusters[cb]["iso3"]:
                cadj[(min(ca, cb), max(ca, cb))] += n
    linked = set(x for pair in cadj for x in pair)
    idx_of = defaultdict(list)
    for k, c in enumerate(clusters):
        idx_of[c["iso3"]].append(k)
    for iso, idx in idx_of.items():  # islands: the nearest cluster of the nation
        if len(idx) < 2:
            continue
        for k in idx:
            if k in linked:
                continue
            m = min((q for q in idx if q != k), key=lambda q: geo.haversine_km((clusters[k]["lon"], clusters[k]["lat"]), (clusters[q]["lon"], clusters[q]["lat"])))
            cadj[(min(k, m), max(k, m))] += 1
    adjacency = [(clusters[a]["id"], clusters[b]["id"], n) for (a, b), n in sorted(cadj.items())]
    return clusters, adm2_rows, adjacency, states, units
