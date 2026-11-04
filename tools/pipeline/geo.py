"""Small geometry helpers for GeoJSON admin boundaries (stdlib only).

Coordinates are (lon, lat) degrees. Areas use the spherical polygon area
formula on a sphere of radius 6371 km; good to ~0.5% at admin-1 scale.
"""

import math

R_KM = 6371.0088


def rings_of(geometry):
    """-> list of polygons, each a list of rings (outer first), each a list of (lon, lat)."""
    t = geometry["type"]
    if t == "Polygon":
        return [geometry["coordinates"]]
    if t == "MultiPolygon":
        return geometry["coordinates"]
    return []


def ring_area_km2(ring):
    """Signed spherical area of a closed ring (Chamberlain & Duquette 2007)."""
    total = 0.0
    n = len(ring)
    for i in range(n - 1):
        lon1, lat1 = ring[i][0], ring[i][1]
        lon2, lat2 = ring[i + 1][0], ring[i + 1][1]
        total += math.radians(lon2 - lon1) * (2 + math.sin(math.radians(lat1)) + math.sin(math.radians(lat2)))
    return total * R_KM * R_KM / 2.0


def area_and_centroid(polygons):
    """-> (area_km2, (lon, lat)) using area-weighted ring vertex means (adequate for labels/distances)."""
    area = 0.0
    cx = cy = 0.0
    for poly in polygons:
        for k, ring in enumerate(poly):
            a = abs(ring_area_km2(ring))
            if k > 0:
                a = -a  # holes
            if len(ring) < 2:
                continue
            mx = sum(p[0] for p in ring) / len(ring)
            my = sum(p[1] for p in ring) / len(ring)
            area += a
            cx += a * mx
            cy += a * my
    if area <= 0:
        pts = [p for poly in polygons for ring in poly for p in ring]
        if not pts:
            return 0.0, (0.0, 0.0)
        return 0.0, (sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts))
    return area, (cx / area, cy / area)


def bbox(polygons):
    xs = [p[0] for poly in polygons for ring in poly for p in ring]
    ys = [p[1] for poly in polygons for ring in poly for p in ring]
    return (min(xs), min(ys), max(xs), max(ys)) if xs else (0, 0, 0, 0)


def point_in_ring(x, y, ring):
    inside = False
    n = len(ring)
    j = n - 1
    for i in range(n):
        xi, yi = ring[i][0], ring[i][1]
        xj, yj = ring[j][0], ring[j][1]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi + 1e-300) + xi:
            inside = not inside
        j = i
    return inside


def point_in_polygons(x, y, polygons):
    for poly in polygons:
        if poly and point_in_ring(x, y, poly[0]) and not any(point_in_ring(x, y, h) for h in poly[1:]):
            return True
    return False


def haversine_km(a, b):
    lon1, lat1, lon2, lat2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((lat2 - lat1) / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin((lon2 - lon1) / 2) ** 2
    return 2 * R_KM * math.asin(math.sqrt(h))


def shared_vertex_adjacency(region_polys, precision=3):
    """Regions sharing at least two (rounded) boundary vertices are neighbours.
    -> {(i, j): shared_vertex_count} for i < j."""
    owners = {}
    for idx, polys in enumerate(region_polys):
        seen = set()
        for poly in polys:
            for ring in poly:
                for p in ring:
                    key = (round(p[0], precision), round(p[1], precision))
                    if key not in seen:
                        seen.add(key)
                        owners.setdefault(key, []).append(idx)
    pairs = {}
    for regs in owners.values():
        if len(regs) < 2:
            continue
        for a in range(len(regs)):
            for b in range(a + 1, len(regs)):
                i, j = min(regs[a], regs[b]), max(regs[a], regs[b])
                pairs[(i, j)] = pairs.get((i, j), 0) + 1
    return {k: v for k, v in pairs.items() if v >= 2}
