"""Topology-preserving polygon simplification for the vector map (stdlib only).

Rings that share vertices (neighbouring ADM1 regions of one geoBoundaries file)
are cut into arcs at the vertices where the set of rings using them changes
(junctions), every arc is simplified once with Douglas-Peucker in a canonical
direction, and the rings are reassembled from the simplified arcs, so shared
borders stay identical between neighbours: no cracks or overlaps are added
(TopoJSON-style arcs, Bostock 2012; Douglas & Peucker 1973).
"""

import math


def _dp(points, tol):
    """Douglas-Peucker on an open polyline (endpoints kept). points: list of (x, y)."""
    n = len(points)
    if n <= 2:
        return list(points)
    keep = [False] * n
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    tol2 = tol * tol
    while stack:
        a, b = stack.pop()
        ax, ay = points[a]
        bx, by = points[b]
        dx, dy = bx - ax, by - ay
        den = dx * dx + dy * dy
        best, best_d = -1, tol2
        for i in range(a + 1, b):
            px, py = points[i]
            if den > 0.0:
                t = ((px - ax) * dx + (py - ay) * dy) / den
                t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
                qx, qy = ax + t * dx - px, ay + t * dy - py
            else:
                qx, qy = ax - px, ay - py
            d = qx * qx + qy * qy
            if d > best_d:
                best, best_d = i, d
        if best >= 0:
            keep[best] = True
            stack.append((a, best))
            stack.append((best, b))
    return [p for p, k in zip(points, keep) if k]


def _key(p):
    return (round(p[0], 6), round(p[1], 6))


def simplify_group(rings, tol):
    """rings: list of rings (lists of (lon, lat), closed or not) -> simplified rings, same order.

    Rings that collapse below three vertices come back empty."""
    clean = []
    for ring in rings:
        pts = [_key(p) for p in ring]
        if len(pts) > 1 and pts[0] == pts[-1]:
            pts.pop()
        # drop consecutive duplicates
        out = [p for i, p in enumerate(pts) if i == 0 or p != pts[i - 1]]
        clean.append(out)
    users = {}
    for ri, pts in enumerate(clean):
        for p in pts:
            u = users.get(p)
            if u is None:
                users[p] = ri
            elif isinstance(u, int):
                if u != ri:
                    users[p] = frozenset((u, ri))
            elif ri not in u:
                users[p] = u | {ri}
    cache = {}

    def arc(seq):
        """Simplify an open arc identically whichever ring walks it, in either direction."""
        a, b = seq[0], seq[-1]
        mid = seq[len(seq) // 2]
        forward = (a, b, mid) <= (b, a, mid)
        k = (a, b, mid, len(seq)) if forward else (b, a, mid, len(seq))
        hit = cache.get(k)
        if hit is None:
            canon = seq if forward else seq[::-1]
            hit = _dp(canon, tol)
            cache[k] = hit
        return hit if forward else hit[::-1]

    result = []
    for pts in clean:
        n = len(pts)
        if n < 3:
            result.append([])
            continue
        members = [users[p] if not isinstance(users[p], int) else users[p] for p in pts]
        fixed = [i for i in range(n) if members[i] != members[i - 1] or members[i] != members[(i + 1) % n]]
        if not fixed:
            # A whole ring shared by the same rings everywhere (an enclave and its hole) or
            # an island: start at its smallest vertex, walk in a canonical direction, and
            # keep the start and the farthest vertex fixed.
            s = min(range(n), key=lambda i: pts[i])
            rot = pts[s:] + pts[:s]
            if rot[1] > rot[-1]:
                rot = [rot[0]] + rot[1:][::-1]
                flipped = True
            else:
                flipped = False
            far = max(range(n), key=lambda i: (rot[i][0] - rot[0][0]) ** 2 + (rot[i][1] - rot[0][1]) ** 2)
            simp = _dp(rot[:far + 1], tol)[:-1] + _dp(rot[far:] + [rot[0]], tol)[:-1]
            if flipped:
                simp = [simp[0]] + simp[1:][::-1]
            result.append(simp if len(simp) >= 3 else [])
            continue
        out = []
        for j, start in enumerate(fixed):
            end = fixed[(j + 1) % len(fixed)]
            seq = pts[start:end + 1] if end > start else pts[start:] + pts[:end + 1]
            out.extend(arc(seq)[:-1])
        result.append(out if len(out) >= 3 else [])
    return result


def ring_area(ring):
    a = 0.0
    for i in range(len(ring)):
        x0, y0 = ring[i - 1]
        x1, y1 = ring[i]
        a += x0 * y1 - x1 * y0
    return a * 0.5


# --- terrain detail atlas (procedural) -----------------------------------------------------------

def _hash2(ix, iy, seed):
    h = (ix * 374761393 + iy * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def _vnoise(x, y, period, seed):
    """Value noise, periodic in x and y with `period` lattice cells (tileable)."""
    ix, iy = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - ix, y - iy
    ux, uy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    x0, x1 = ix % period, (ix + 1) % period
    y0, y1 = iy % period, (iy + 1) % period
    a, b = _hash2(x0, y0, seed), _hash2(x1, y0, seed)
    c, d = _hash2(x0, y1, seed), _hash2(x1, y1, seed)
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy


def _fbm(u, v, base, octaves, seed, ridged=False):
    s, amp, tot = 0.0, 0.5, 0.0
    f = base
    for o in range(octaves):
        n = _vnoise(u * f, v * f, f, seed + o)
        if ridged:
            n = 1.0 - abs(2.0 * n - 1.0)
        s += amp * n
        tot += amp
        amp *= 0.5
        f *= 2
    return s / tot


ATLAS_LAYERS = ["sand", "grass", "forest", "tundra", "rock", "snow", "wetland", "water"]


def terrain_atlas(size=256):
    """Tileable grayscale detail textures (mean ~0.5), one per ATLAS_LAYERS entry, for the
    map shader to modulate the flat terrain colour when zoomed in. All procedural."""
    layers = []
    for li, name in enumerate(ATLAS_LAYERS):
        out = bytearray(size * size)
        for y in range(size):
            v = y / size
            for x in range(size):
                u = x / size
                if name == "sand":  # dune ripples, warped, plus grain
                    w = _fbm(u, v, 4, 3, 11)
                    r = 0.5 + 0.5 * math.sin(2 * math.pi * (12 * u + 3 * v + 2.5 * w))
                    val = 0.55 * r + 0.3 * _fbm(u, v, 32, 2, 13) + 0.15 * w
                elif name == "grass":  # patchy fields
                    val = 0.6 * _fbm(u, v, 8, 4, 21) + 0.4 * _fbm(u, v, 32, 2, 23)
                elif name == "forest":  # canopy clumps
                    c = _fbm(u, v, 16, 3, 31)
                    val = 0.25 + 0.75 * (c * c * (3 - 2 * c)) * (0.7 + 0.3 * _fbm(u, v, 64, 1, 33))
                elif name == "tundra":  # mottled lichen
                    m = _fbm(u, v, 16, 4, 41)
                    val = 0.4 + 0.5 * m + (0.15 if _fbm(u, v, 64, 1, 43) > 0.72 else 0.0)
                elif name == "rock":  # ridges and scree
                    val = 0.75 * _fbm(u, v, 4, 5, 51, ridged=True) + 0.25 * _fbm(u, v, 64, 1, 53)
                elif name == "snow":  # soft drifts
                    val = 0.45 + 0.25 * _fbm(u, v, 4, 3, 61) + 0.1 * _fbm(u, v, 16, 2, 63, ridged=True)
                elif name == "wetland":  # pools in grass
                    m = _fbm(u, v, 8, 4, 71)
                    val = 0.55 + 0.3 * _fbm(u, v, 32, 2, 73) - (0.35 if m < 0.38 else 0.0)
                else:  # water: soft caustic ripples
                    a = math.sin(2 * math.pi * (6 * u + 2 * _fbm(u, v, 4, 2, 81)))
                    b = math.sin(2 * math.pi * (5 * v + 2 * _fbm(u, v, 4, 2, 83)))
                    val = 0.5 + 0.2 * a * b + 0.1 * (_fbm(u, v, 16, 2, 85) - 0.5)
                out[y * size + x] = max(0, min(255, int(round(val * 255))))
        # centre each layer on 0.5 (the shader modulates around the base colour)
        mean = sum(out) / len(out)
        shift = 128 - mean
        layers.append(bytes(max(0, min(255, int(round(p + shift)))) for p in out))
    return layers
