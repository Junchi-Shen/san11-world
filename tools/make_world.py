"""Generate the world map data for worldmod from public elevation and hydrography data.

The terrain is drawn in Koei's style, calibrated on the stock China map (data/china_shex.npy and
data/china_k3st_v.npy) so the world outside China has the same granularity:
  * impassable mountains (type 15) cover about half of rugged land and leave passable corridors
    along rivers, like Koei's map; flat lowlands keep only scattered hills
  * major rivers are 4-6 hexes wide (type 7), tributaries about 2 (type 6), narrowing upstream
  * passable land is a hex-scale mix of grass/soil/sand/marsh/forest/wasteland whose proportions
    follow a crude climate model and blend into China's own mix near the China block
  * 3D heights follow Koei: flat valleys, ridges rising 40-130 above them, water at 0;
    vertex materials are sampled from China's per-terrain material statistics
China's own hexes and stage vertices are copied in, and the edge of the China block is continued
outwards (mountains, land cover, rivers) so there is no visible seam. Koei's south-east mountain margin
(Fujian, south Zhejiang) is the exception: its land is classified and drawn like the generated world
(china_patch()), so cities fit there.

Outputs (in <out_dir>):
  world_shex.bin   b"WSHX", u32 cols(W), u32 rows(H), then rows*cols SHEX0008 records (11 bytes,
                   row-major: index = lo*cols + hi). worldmod also overlays the China block at load time.
  world_types.npy  terrain type per hex (rows, cols), including China
  world_height.npy global vertex heights (Koei height bytes), shape (rows*4+225, cols*4+225)
  world_mat.npy    global vertex materials (0..35), same shape
  world_info.json  the WorldSpec and statistics
  cities_world.json new city sites
  china_overrides.bin  edits of Koei's China block (the sea strip and the patch), see write_overrides()
  china_patch.npy  200x200 mask of the patched China hexes
  china_patch_types.npy  their terrain before roads (255 elsewhere), where connect_cities.py starts from
  china_patch.bin  the stage vertices worldmod draws from the world grid: the patch, and the roads
                   connect_cities.py laid on Koei's block (his heights, painted with road material)

Global vertex of hex (lo, hi): z = lo*4 + 112, x = hi*4 + 112 + 2*(lo & 1) (hex centre +2, +2).

Inputs: data/etopo60s_i16.npy (ETOPO 2022 60", int16 metres), data/raw/ne_rivers/*.zip,
data/raw/ne_lakes/*.zip (Natural Earth 1:10m), data/cities_georef.json, data/china_shex.npy,
data/china_k3st_v.npy, data/new_cities.json.

Usage: python make_world.py [out_dir] [--rows 1800 --cols 1000]  (defaults: Eurasia, China at row 1234, col 436)
"""
import argparse
import io
import json
import struct
import time
import zipfile
from pathlib import Path

import numpy as np
import shapefile  # pyshp
from scipy import ndimage
from scipy.sparse import csr_matrix
from scipy.sparse.csgraph import dijkstra, minimum_spanning_tree

from worldgeo import WorldGeo, WorldSpec

HERE = Path(__file__).resolve().parent
DATA = HERE.parent / "data"

# Koei terrain types (SHEX byte 0)
GRASS, SOIL, SAND, MARSH, POISON, FOREST, STREAM, RIVER, SEA, WASTE, ROAD = range(11)
PLANK, BRIDGE, FORD, SHORE, MOUNTAIN, CITY, GATE, PORT, PATH = 11, 12, 13, 14, 15, 16, 17, 18, 19
WATER = (STREAM, RIVER, SEA)
COVER = (GRASS, SOIL, SAND, MARSH, FOREST, WASTE)
UNOWNED_AREA = 90

# land-cover proportions per climate zone (order of COVER). "china" is measured on Koei's map.
MIX = {
    "china":    (.28, .19, .10, .14, .18, .11),
    "desert":   (.03, .10, .62, .02, .01, .22),
    "steppe":   (.33, .33, .08, .05, .03, .18),
    "alpine":   (.15, .20, .08, .10, .02, .45),
    "taiga":    (.12, .04, .01, .17, .59, .07),
    "tundra":   (.08, .05, .01, .30, .04, .52),
    "tropical": (.22, .10, .03, .20, .42, .03),
}


def smoothstep(x, a, b):
    t = np.clip((np.asarray(x, dtype=np.float32) - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def noise(shape, rng, sigma):
    """Spatially coherent Gaussian noise, mean 0, std 1."""
    n = ndimage.gaussian_filter(rng.standard_normal(shape, dtype=np.float32), sigma, mode="wrap")
    return (n - n.mean()) / (n.std() + 1e-6)


def uniform_noise(shape, rng, sigma):
    """Coherent noise with a uniform [0, 1) marginal (rank transform), for thresholding by probability."""
    n = noise(shape, rng, sigma).ravel()
    u = np.empty(n.size, np.float32)
    u[np.argsort(n, kind="stable")] = np.arange(n.size, dtype=np.float32) / n.size
    return u.reshape(shape)


class Dem:
    def __init__(self, path):
        self.a = np.load(path, mmap_mode="r")

    def sample(self, lon, lat):
        """Bilinear sample in metres."""
        x = (np.asarray(lon) + 180.0) * 60.0 - 0.5
        y = (90.0 - np.asarray(lat)) * 60.0 - 0.5
        x = np.mod(x, 21600.0)
        y = np.clip(y, 0, 10798.999)
        x0 = np.floor(x).astype(np.int64)
        y0 = np.floor(y).astype(np.int64)
        fx = x - x0
        fy = y - y0
        x1 = (x0 + 1) % 21600
        a = self.a
        v00 = a[y0, x0].astype(np.float32)
        v01 = a[y0, x1].astype(np.float32)
        v10 = a[y0 + 1, x0].astype(np.float32)
        v11 = a[y0 + 1, x1].astype(np.float32)
        return (v00 * (1 - fx) * (1 - fy) + v01 * fx * (1 - fy) + v10 * (1 - fx) * fy + v11 * fx * fy)


def read_shapes(zip_path):
    z = zipfile.ZipFile(zip_path)
    names = {Path(n).suffix.lower(): n for n in z.namelist()}
    r = shapefile.Reader(shp=io.BytesIO(z.read(names[".shp"])), dbf=io.BytesIO(z.read(names[".dbf"])),
                         shx=io.BytesIO(z.read(names[".shx"])))
    return r


def lonlat_to_hex(geo: WorldGeo, lon, lat, iters=4):
    """Inverse of WorldGeo.hex_to_lonlat by fixed-point iteration on the displacement field.
    Returns continuous (lo, x) where x = hi + 0.5*(lo & 1) is the geometric column."""
    s = geo.spec
    lo0, hi0 = s.lonlat_to_linear(lon, lat)
    lo, hi = lo0.copy(), hi0.copy()
    for _ in range(iters):
        d = geo.rbf(np.stack([lo, hi], 1)) * geo._fade(lo, hi)[:, None]
        lo, hi = lo0 + d[:, 0], hi0 + d[:, 1]
    return lo, hi


def to_hex(lo, x):
    """Continuous geometric (lo, x) -> integer hex (lo, hi)."""
    lo = np.round(lo).astype(int)
    return lo, np.round(x - 0.5 * (lo & 1)).astype(int)


def hex_neighbors(mask):
    """Boolean dilation by the six hex neighbours (odd rows shifted towards +hi)."""
    out = mask.copy()
    out[:, 1:] |= mask[:, :-1]
    out[:, :-1] |= mask[:, 1:]
    even = np.zeros(mask.shape[0], bool)
    even[0::2] = True
    for d in (-1, 1):
        src = np.roll(mask, d, axis=0)          # row lo receives row lo-d
        if d == 1:
            src[0] = False
        else:
            src[-1] = False
        out |= src
        # the second neighbour in the adjacent row: hi-1 for even rows, hi+1 for odd rows
        sh = np.zeros_like(mask)
        sh[even, 1:] = src[even, :-1]
        sh[~even, :-1] = src[~even, 1:]
        out |= sh
    return out


# ---------------------------------------------------------------- sampling

def sample_hexes(geo, dem, R, C):
    e0 = np.zeros((R, C), np.float32)
    relief = np.zeros((R, C), np.float32)
    lat_c = np.zeros((R, C), np.float32)
    offs = [(0, 0), (.35, 0), (-.35, 0), (0, .4), (0, -.4), (.25, .3), (-.25, -.3)]
    for r0 in range(0, R, 200):
        lo, hi = np.mgrid[r0:r0 + 200, 0:C].astype(float)
        vals = []
        for dlo, dhi in offs:
            lon, lat = geo.hex_to_lonlat(lo.ravel() + dlo, hi.ravel() + dhi)
            vals.append(dem.sample(lon, lat).reshape(lo.shape))
            if (dlo, dhi) == (0, 0):
                lat_c[r0:r0 + 200] = lat.reshape(lo.shape)
        v = np.stack(vals)
        e0[r0:r0 + 200] = v[0]
        relief[r0:r0 + 200] = v.max(0) - v.min(0)
    return e0, relief, lat_c


# ---------------------------------------------------------------- hydrography

def river_class(rec):
    """(terrain type, full width in hexes at sea level) or None. Koei: rivers 4-6 wide, streams ~2."""
    sr = int(rec["scalerank"])
    if sr <= 3:
        return RIVER, 5.0
    if sr <= 5:
        return STREAM, 2.6
    if sr <= 8:
        return STREAM, 2.0
    return None


def rasterize_rivers(geo, shapes, R, C, e0, rng):
    """River hexes with Koei-like widths: each polyline is swept by a brush perpendicular to it.
    Width narrows with elevation (upstream) and wobbles slowly along the river."""
    out = np.zeros((R, C), np.uint8)
    s = geo.spec
    lon_lo, lat_hi = s.linear_to_lonlat(0, 0)
    lon_hi, lat_lo = s.linear_to_lonlat(R, C)
    for sr in shapes.iterShapeRecords():
        cls = river_class(sr.record)
        if not cls:
            continue
        bx = sr.shape.bbox                       # skip rivers far outside the world
        if bx[2] < lon_lo - 5 or bx[0] > lon_hi + 5 or bx[3] < lat_lo - 5 or bx[1] > lat_hi + 5:
            continue
        val, width = cls
        allpts = np.asarray(sr.shape.points, dtype=float)
        parts = list(sr.shape.parts) + [len(allpts)]
        for a, b in zip(parts[:-1], parts[1:]):          # multi-part lines: never join separate parts
            pts = allpts[a:b]
            if len(pts) < 2:
                continue
            seg = np.hypot(np.diff(pts[:, 0]), np.diff(pts[:, 1]))
            n = np.maximum(1, np.ceil(seg / 0.02).astype(int))
            dense = np.concatenate([np.linspace(pts[i], pts[i + 1], n[i], endpoint=False) for i in range(len(seg))] + [pts[-1:]])
            lo, x = lonlat_to_hex(geo, dense[:, 0], dense[:, 1])
            if lo.max() < -3 or lo.min() > R + 3 or x.max() < -3 or x.min() > C + 3:
                continue
            tl, tx = np.gradient(lo), np.gradient(x)
            nrm = np.hypot(tl, tx) + 1e-9
            nl, nx = -tx / nrm, tl / nrm
            hl, hh = to_hex(lo, x)
            elev = e0[np.clip(hl, 0, R - 1), np.clip(hh, 0, C - 1)]
            taper = np.clip(1.15 - elev / 2500.0, 0.45, 1.0)
            wob = 1 + 0.18 * ndimage.gaussian_filter1d(rng.standard_normal(len(lo)), 25) * 6
            w = np.maximum(1.0, width * taper * np.clip(wob, 0.7, 1.3))
            k = int(np.ceil(w.max() / 0.35)) + 1
            for s in np.linspace(-0.5, 0.5, k):
                yl, yh = to_hex(lo + s * w * nl, x + s * w * nx)
                ok = (yl >= 0) & (yl < R) & (yh >= 0) & (yh < C)
                np.maximum.at(out, (yl[ok], yh[ok]), val)      # STREAM < RIVER: rivers win at confluences
    return out


def points_in_ring(px, py, ring):
    """Even-odd ray casting of points (px, py) against a closed ring of (x, y)."""
    inside = np.zeros(px.shape, bool)
    x0, y0 = ring[:, 0], ring[:, 1]
    x1, y1 = np.roll(x0, -1), np.roll(y0, -1)
    for a, b, c, d in zip(x0, y0, x1, y1):
        if b == d:
            continue
        cross = (b > py) != (d > py)
        xi = a + (py - b) * (c - a) / (d - b)
        inside ^= cross & (px < xi)
    return inside


def rasterize_polygons(geo, shapes, rows, cols):
    """Lake mask on the hex grid: hex centres tested against each polygon ring (rings toggle, so holes work)."""
    out = np.zeros((rows, cols), bool)
    for sr in shapes.iterShapeRecords():
        shp = sr.shape
        parts = list(shp.parts) + [len(shp.points)]
        for a, b in zip(parts[:-1], parts[1:]):
            ring = np.asarray(shp.points[a:b], dtype=float)
            if len(ring) < 3:
                continue
            lo, hi = lonlat_to_hex(geo, ring[:, 0], ring[:, 1])
            l0, l1 = int(max(0, lo.min())), int(min(rows - 1, lo.max() + 1))
            h0, h1 = int(max(0, hi.min())), int(min(cols - 1, hi.max() + 1))
            if l1 < l0 or h1 < h0 or (l1 - l0) * (h1 - h0) == 0:
                continue
            L, Hh = np.mgrid[l0:l1 + 1, h0:h1 + 1]
            Lr, Hr = L.ravel(), Hh.ravel()
            inside = points_in_ring(Lr.astype(float), Hr + 0.5 * (Lr & 1), np.stack([lo, hi], 1))
            out[Lr[inside], Hr[inside]] ^= True     # rings toggle (holes/islands)
    return out


# ---------------------------------------------------------------- climate and land cover

def climate_weights(e0, lat, coast_km, dchina, rng):
    """Soft membership (sums to 1) of each hex in the MIX zones."""
    alat = np.abs(lat)
    interior = np.clip((coast_km - 300.0) / 1500.0, 0, 1)
    subtropic = np.exp(-((alat - 24.0) / 9.0) ** 2)                       # Hadley-cell dry belt
    temperate_dry = np.exp(-((alat - 42.0) / 8.0) ** 2) * interior        # Central Asian rain shadow
    arid = (0.9 * subtropic * np.clip(coast_km / 400.0, 0.2, 1) + 0.8 * temperate_dry
            + 0.12 * noise(e0.shape, rng, 12))
    w = {}
    w["desert"] = smoothstep(arid, 0.55, 0.72)
    w["steppe"] = smoothstep(arid, 0.36, 0.5) * (1 - w["desert"])
    w["tundra"] = smoothstep(alat + 2.5 * noise(e0.shape, rng, 15), 60, 67)
    w["taiga"] = smoothstep(alat + 2.0 * noise(e0.shape, rng, 12), 49, 56) * (1 - w["tundra"]) * (1 - w["steppe"] - w["desert"])
    w["tropical"] = (1 - smoothstep(alat, 13, 23)) * (1 - w["desert"] - w["steppe"])
    w["alpine"] = smoothstep(e0, 2600, 3800)
    for k in ("desert", "steppe", "tundra", "taiga", "tropical"):
        w[k] = np.clip(w[k] * (1 - w["alpine"]), 0, 1)
    rest = sum(w.values())
    w["china"] = np.clip(1 - rest, 0, 1)
    tot = sum(w.values())
    w = {k: v / tot for k, v in w.items()}
    # blend into Koei's own mix next to the China block
    fade = 1 - smoothstep(dchina, 8, 60)
    w = {k: v * (1 - fade) for k, v in w.items()}
    w["china"] = w["china"] + fade
    return w


def land_cover(shape, weights, dw, dmt, e0, rng, land, iters=3):
    """Hex-scale mix of COVER types: argmax of log-weight + coherent noise (patches of ~2-6 hexes)."""
    K = len(COVER)
    wt = np.zeros((K,) + shape, np.float32)
    for zone, wz in weights.items():
        for i, p in enumerate(MIX[zone]):
            wt[i] += wz * p
    near_water = np.exp(-(np.minimum(dw, 20) - 1) / 1.5)
    near_mtn = np.exp(-(np.minimum(dmt, 20) - 1) / 2.0)
    wt[COVER.index(MARSH)] *= 0.35 + 2.2 * near_water * (e0 < 900)
    wt[COVER.index(WASTE)] *= 0.6 + 1.0 * near_mtn + 0.8 * smoothstep(e0, 1200, 3000)
    wt[COVER.index(FOREST)] *= 0.7 + 0.7 * near_mtn
    wt /= wt.sum(0, keepdims=True)
    logw = np.log(wt + 1e-4)
    nz = np.stack([noise(shape, rng, 1.7) for _ in range(K)]) * 1.15
    target = np.array([wt[i][land].mean() for i in range(K)])
    bias = np.zeros(K, np.float32)
    for _ in range(iters):
        pick = np.argmax(logw + nz + bias[:, None, None], 0)
        got = np.array([(pick[land] == i).mean() for i in range(K)]) + 1e-6
        bias += np.log(target / got).astype(np.float32) * 0.8
    pick = np.argmax(logw + nz + bias[:, None, None], 0)
    return np.asarray(COVER, np.uint8)[pick]


def mountain_mask(e0, relief, dw, land, rng):
    """Koei-style impassable mountains. The target share rises with regional ruggedness (few hills on
    plains, about half of hilly land, most of high ranges). Inside that share mountains sit on local
    ridges and away from rivers, so valleys along rivers stay open like Koei's corridors."""
    rr = ndimage.gaussian_filter(relief, 3)
    p = 0.04 + 0.88 * smoothstep(rr, 30, 330) + 0.15 * smoothstep(e0, 2500, 4500)
    p = np.clip(p, 0, 0.93)
    tpi = (e0 - ndimage.gaussian_filter(e0, 3)) / (ndimage.gaussian_filter(relief, 4) + 40)
    dterm = np.minimum(dw, 12) / 12.0

    def z(a):
        return (a - a[land].mean()) / (a[land].std() + 1e-6)
    s = 1.0 * z(tpi) + 0.8 * z(dterm) + 1.0 * noise(e0.shape, rng, 2.2)
    flat = s[land]
    u = np.zeros_like(s)
    u[land] = (np.argsort(np.argsort(flat, kind="stable"), kind="stable") / flat.size).astype(np.float32)
    m = land & (u > 1 - p)
    # Koei's ranges are contiguous (median cluster 22 hexes): drop small specks and fill small holes
    m = drop_small(m, 6)
    gaps = land & ~m                       # fill small passable pockets enclosed by mountains only
    lab, n = ndimage.label(gaps)
    sizes = np.bincount(lab.ravel(), minlength=n + 1)
    wet = np.zeros(n + 1, bool)
    wet[np.unique(lab[hex_neighbors(~land) & gaps])] = True
    fill = (sizes < 4) & ~wet
    fill[0] = False
    return m | fill[lab], p


def drop_small(mask, min_size):
    lab, n = ndimage.label(mask)
    sizes = np.bincount(lab.ravel(), minlength=n + 1)
    keep = sizes >= min_size
    keep[0] = False
    return keep[lab]


# ---------------------------------------------------------------- China block

def china_box(spec):
    return slice(spec.china_lo, spec.china_lo + 200), slice(spec.china_hi, spec.china_hi + 200)


def dist_to_china(spec, R, C):
    lo, hi = np.mgrid[0:R, 0:C]
    dlo = np.maximum(0, np.maximum(spec.china_lo - lo, lo - (spec.china_lo + 199)))
    dhi = np.maximum(0, np.maximum(spec.china_hi - hi, hi - (spec.china_hi + 199)))
    return np.hypot(dlo, dhi).astype(np.float32)


def continue_china_edge(t, spec, rng, reach=9.0):
    """Continue the China block's edge outward: hexes near the block copy the type of the nearest
    China hex with a probability that decays with distance (noisy, so no straight line remains)."""
    R, C = t.shape
    inside = np.zeros(t.shape, bool)
    inside[china_box(spec)] = True
    d, (il, ih) = ndimage.distance_transform_edt(~inside, return_indices=True)
    src = t[il, ih]
    u = uniform_noise(t.shape, rng, 1.0)
    p = 0.9 * np.exp(-(d - 1) / (reach / 3))
    band = ~inside & (d <= reach) & (u < p)
    copy = band & np.isin(src, COVER + (MOUNTAIN, STREAM, RIVER, SEA))
    copy &= ~np.isin(t, WATER) | np.isin(src, WATER)        # never cut generated rivers with land
    t[copy] = src[copy]
    return t


def china_sea_strip(china_shex):
    """Koei fills the open sea along the China block's east edge with a strip of type 6 (川) water.
    In the world it would be a straight band in the ocean, so the open-sea part (more than 2 hexes from
    any Koei land) becomes sea (8). Returns override records {(lo, hi): record}; coasts are untouched."""
    t = china_shex[..., 0]
    lab, n = ndimage.label(t == STREAM)
    edge = np.unique(lab[199][lab[199] > 0])
    strip = np.isin(lab, edge)
    far = ndimage.distance_transform_edt(np.isin(t, WATER)) > 2.0
    out = {}
    for lo, hi in zip(*np.nonzero(strip & far)):
        r = china_shex[lo, hi].copy()
        r[0], r[4] = SEA, 4
        out[(int(lo), int(hi))] = r
    return out


PATCH_AREA = 90         # Koei's decorative mountain margin; its land in the south-east (Fujian, south Zhejiang)


def china_patch(china_shex, e0, relief, seed):
    """Koei draws Fujian and the south Zhejiang coast as one block of mountains (special area 90), so no
    city fits there. Its land hexes are classified again from the DEM like the generated world (mountains
    on rugged ground, Koei's China mix of cover, banks on the coast); Koei's coastline, rivers and sea stay.
    e0, relief: the block's 200x200 hex elevation fields. Returns (override records {(lo, hi): record},
    200x200 mask of the patched hexes); the 3D terrain there is generated too (china_patch.bin)."""
    rng = np.random.default_rng(seed)
    t = china_shex[..., 0]
    area = china_shex[..., 1].astype(int) | (china_shex[..., 2].astype(int) << 8)
    patch = (area == PATCH_AREA) & np.isin(t, (SHORE, MOUNTAIN))
    # Koei's streams around it join the patch unchanged, so connect_cities.py may ferry across them (the
    # patch is otherwise walled in by his streams and mountains)
    streams = (t == STREAM) & (ndimage.distance_transform_edt(~patch) <= 2)
    water = np.isin(t, WATER)
    dw = ndimage.distance_transform_edt(~water).astype(np.float32)
    mtn, _ = mountain_mask(e0, relief, dw, patch, rng)
    dmt = ndimage.distance_transform_edt(~mtn).astype(np.float32)
    cover = land_cover(t.shape, {"china": np.ones(t.shape, np.float32)}, dw, dmt, e0, rng, patch & ~mtn)
    new = np.where(mtn, MOUNTAIN, cover).astype(np.uint8)
    shore = ~mtn & hex_neighbors(water) & (uniform_noise(t.shape, rng, 1.3) < np.where(hex_neighbors(t == SEA), 0.75, 0.42))
    new[shore] = SHORE
    # the other bytes as on Koei's hexes of the same type (most common value), flags clear
    land = ~np.isin(t, WATER)
    b4 = {k: np.bincount(china_shex[..., 4][land & (t == k)]).argmax() for k in np.unique(new[patch])}
    out = {}
    for lo, hi in zip(*np.nonzero(patch)):
        r = china_shex[lo, hi].copy()
        r[0] = new[lo, hi]
        r[4] = b4[r[0]]
        r[3] = r[5] = r[6] = r[7] = r[8] = r[10] = 0
        out[(int(lo), int(hi))] = r
    for lo, hi in zip(*np.nonzero(streams)):
        out[(int(lo), int(hi))] = china_shex[lo, hi].copy()
    return out, patch | streams


def write_overrides(path, overrides):
    """china_overrides.bin: b"WCOV", u32 count, then count x (u16 lo, u16 hi, 11-byte SHEX record).
    worldmod applies these on top of the stock China block."""
    with open(path, "wb") as f:
        f.write(b"WCOV" + struct.pack("<I", len(overrides)))
        for (lo, hi), r in sorted(overrides.items()):
            f.write(struct.pack("<HH", lo, hi) + bytes(r))


def connect_china_rivers(t, spec, max_len=45):
    """Koei's rivers that leave the China block are joined to the nearest generated water outside."""
    s_lo, s_hi = china_box(spec)
    ring = []
    for i in range(200):
        ring += [(0, i), (199, i), (i, 0), (i, 199)]
    water_out = np.isin(t, WATER)
    water_out[s_lo, s_hi] = False
    dchina = dist_to_china(spec, *t.shape)
    china = t[s_lo, s_hi]
    lab, n = ndimage.label(np.isin(china, (STREAM, RIVER)))
    done = 0
    for comp in range(1, n + 1):
        edge = [(a, b) for a, b in ring if lab[a, b] == comp]
        if not edge or len(edge) > 30:          # long runs are coast strips, handled by continue_china_edge
            continue
        a, b = edge[len(edge) // 2]
        width = max(1.5, min(5.0, len(edge) * 0.8))
        y0, x0 = spec.china_lo + a, spec.china_hi + b
        # step one hex outside the block
        y0 += -1 if a == 0 else (1 if a == 199 else 0)
        x0 += -1 if b == 0 else (1 if b == 199 else 0)
        # nearest outside water that lies outward (away from the block), never along its edge
        wl = slice(max(0, y0 - max_len), y0 + max_len + 1)
        wh = slice(max(0, x0 - max_len), x0 + max_len + 1)
        cy, cx = np.nonzero(water_out[wl, wh])
        cy, cx = cy + wl.start, cx + wh.start
        L = np.hypot(cy - y0, cx - x0)
        ok = (L >= 1) & (L <= max_len) & (dchina[cy, cx] >= 0.6 * L)
        if not ok.any():
            continue
        k = np.argmin(np.where(ok, L, np.inf))
        y1, x1, L = cy[k], cx[k], L[k]
        val = RIVER if china[a, b] == RIVER else STREAM
        steps = int(L * 3) + 2
        for s in np.linspace(0, 1, steps):
            py, px = y0 + (y1 - y0) * s, x0 + (x1 - x0) * s
            for o in np.linspace(-width / 2, width / 2, int(width / 0.4) + 1):
                ny, nx = (x1 - x0) / L, -(y1 - y0) / L
                yy, xx = int(round(py + o * ny)), int(round(px + o * nx))
                if 0 <= yy < t.shape[0] and 0 <= xx < t.shape[1] and not (
                        spec.china_lo <= yy < spec.china_lo + 200 and spec.china_hi <= xx < spec.china_hi + 200):
                    if t[yy, xx] != SEA:
                        t[yy, xx] = max(t[yy, xx], val) if t[yy, xx] in WATER else val
        done += 1
    return done


# ---------------------------------------------------------------- roads

PASSABLE = COVER + (ROAD, PLANK, BRIDGE, FORD, SHORE, CITY, GATE, PORT, PATH)
ROAD_COST = np.full(256, np.inf, np.float32)
for _k, _c in {GRASS: 1.0, SOIL: 1.0, SAND: 1.4, MARSH: 2.2, FOREST: 1.6, WASTE: 1.3, SHORE: 1.2,
               MOUNTAIN: 7.0, STREAM: 6.0, RIVER: 30.0, ROAD: 0.35, PLANK: 0.5, BRIDGE: 0.5, CITY: 1.0}.items():
    ROAD_COST[_k] = _c


def hex_graph(cost):
    """Directed hex-grid graph, edge weight = cost of entering the target hex (inf hexes excluded)."""
    R, C = cost.shape
    idx = np.arange(R * C).reshape(R, C)
    lo = np.repeat(np.arange(R), C).reshape(R, C)
    odd = (lo & 1).astype(bool)
    src, dst = [], []

    def add(a, b):
        src.append(a.ravel())
        dst.append(b.ravel())
    add(idx[:, :-1], idx[:, 1:])
    add(idx[:, 1:], idx[:, :-1])
    for d in (-1, 1):
        a = idx[max(0, -d):R - max(0, d)]
        b = idx[max(0, d):R - max(0, -d)]
        o = odd[max(0, -d):R - max(0, d)]
        # same column
        add(a, b)
        # second neighbour: hi-1 for even rows, hi+1 for odd rows
        add(a[~o[:, 0], 1:], b[~o[:, 0], :-1])
        add(a[o[:, 0], :-1], b[o[:, 0], 1:])
    src = np.concatenate(src)
    dst = np.concatenate(dst)
    w = cost.ravel()[dst]
    ok = np.isfinite(w) & np.isfinite(cost.ravel()[src])
    return csr_matrix((w[ok], (src[ok], dst[ok])), shape=(R * C, R * C))


def china_gateways(t, spec, china_cities, reach=45):
    """For Koei cities near the block edge: the hex just outside the block that is reached from the city
    by the shortest passable path inside China. Roads from the world end there, so they lead into China."""
    s_lo, s_hi = china_box(spec)
    ct = t[s_lo, s_hi]
    cost = np.where(np.isin(ct, PASSABLE), 1.0, np.inf).astype(np.float32)
    g = hex_graph(cost)
    ring = np.zeros((200, 200), bool)
    ring[0, :] = ring[-1, :] = ring[:, 0] = ring[:, -1] = True
    out = []
    for c in china_cities:
        lo, hi = c["lo"], c["hi"]
        if min(lo, hi, 199 - lo, 199 - hi) > reach or not np.isfinite(cost[lo, hi]):
            continue
        dist = dijkstra(g, indices=lo * 200 + hi, limit=reach * 1.5).reshape(200, 200)
        cand = ring & np.isfinite(dist)
        if not cand.any():
            continue
        a, b = np.unravel_index(np.argmin(np.where(cand, dist, np.inf)), dist.shape)
        y, x = spec.china_lo + a, spec.china_hi + b
        y += -1 if a == 0 else (1 if a == 199 else 0)
        x += -1 if b == 0 else (1 if b == 199 else 0)
        out.append((c["name"], int(y), int(x)))
    return out


def build_roads(t, spec, nodes, footprint, n_gates, max_cost=900.0):
    """Koei-style road network: a spanning tree of cheapest land routes between the nodes plus each
    node's nearest neighbour, built one route at a time so later routes reuse earlier roads.
    Mountains crossed become plank roads (11), rivers bridges (12). China itself is never modified.
    The last n_gates nodes are China gateways: already linked through China, so no roads between them."""
    R, C = t.shape
    s_lo, s_hi = china_box(spec)
    n = len(nodes)
    ids = np.array([y * C + x for _, y, x in nodes])
    gate = np.zeros(n, bool)
    gate[n - n_gates:] = True
    border = dist_to_china(spec, R, C) <= 3          # keep roads off the block edge

    def costmap():
        c = ROAD_COST[t].copy()
        c[border] *= 4
        c[s_lo, s_hi] = np.inf
        c[footprint] = 1.0
        return c
    g = hex_graph(costmap())
    D = np.full((n, n), np.inf)
    for i in range(n):
        d = dijkstra(g, indices=ids[i], limit=max_cost)
        D[i] = d[ids]
    D = np.minimum(D, D.T)
    D[np.ix_(gate, gate)] = 1e-3                     # one super-node: China
    np.fill_diagonal(D, np.inf)
    W = np.where(np.isfinite(D), D, 0)
    mst = minimum_spanning_tree(csr_matrix(W)).toarray()
    edges = {(min(i, j), max(i, j)) for i, j in zip(*np.nonzero(mst)) if not (gate[i] and gate[j])}
    for i in np.flatnonzero(~gate):
        d = D[i].copy()
        j = int(np.argmin(d))
        if np.isfinite(d[j]):
            edges.add((min(i, j), max(i, j)))
    built = 0
    for i, j in sorted(edges, key=lambda e: D[e]):
        g = hex_graph(costmap())
        dist, pred = dijkstra(g, indices=ids[i], limit=max_cost * 1.2, return_predecessors=True)
        v = ids[j]
        if not np.isfinite(dist[v]):
            continue
        while v != ids[i] and v >= 0:
            y, x = divmod(int(v), C)
            if not footprint[y, x]:
                k = t[y, x]
                if k == MOUNTAIN:
                    t[y, x] = PLANK
                elif k in (STREAM, RIVER):
                    t[y, x] = BRIDGE
                elif k in COVER or k == SHORE:
                    t[y, x] = ROAD
            v = pred[v]
        built += 1
    return built, len(edges)


# ---------------------------------------------------------------- cities

FIRST_NEW_CITY_ID = 42      # new cities take base ids 42.. ; gates/ports are renumbered after them


def hex_ring(y, x):
    d = [(0, -1), (0, 1), (-1, -1), (-1, 0), (1, -1), (1, 0)] if y % 2 == 0 else [(0, -1), (0, 1), (-1, 0), (-1, 1), (1, 0), (1, 1)]
    return [(y + a, x + b) for a, b in d]


def place_cities(geo, t, cities, china):
    """Pick a centre near each city's real location whose 7-hex footprint is mostly passable land,
    then stamp the footprint as city terrain. Returns [(name, lo, hi)]."""
    R, C = t.shape
    passable = np.isin(t, COVER + (SHORE, ROAD))
    lon = np.array([c["lon"] for c in cities])
    lat = np.array([c["lat"] for c in cities])
    lo, x = lonlat_to_hex(geo, lon, lat)
    placed = []
    taken = np.zeros_like(passable)
    for c, l, h in zip(cities, lo, x):
        y0 = int(round(l))
        x0 = int(round(h - 0.5 * (y0 & 1)))
        if not (6 <= y0 < R - 6 and 6 <= x0 < C - 6):
            continue                                  # outside this world
        best = None
        for dy in range(-6, 7):
            for dx in range(-6, 7):
                y, xx = y0 + dy, x0 + dx
                cells = [(y, xx)] + hex_ring(y, xx)
                if any(not (0 <= a < R and 0 <= b < C) for a, b in cells):
                    continue
                if any(china[0] <= a < china[0] + 200 and china[1] <= b < china[1] + 200 for a, b in cells):
                    continue
                if any(taken[a, b] for a, b in cells):
                    continue
                score = sum(passable[a, b] for a, b in cells) * 10 - (dy * dy + dx * dx) ** 0.5
                if passable[y, xx]:
                    score += 20
                if best is None or score > best[0]:
                    best = (score, y, xx)
        if best is None:
            continue
        _, y, xx = best
        for a, b in [(y, xx)] + hex_ring(y, xx):
            t[a, b] = CITY
            taken[a, b] = True
        placed.append((c["name"], y, xx))
    return placed


# ---------------------------------------------------------------- 3D (vertex) level

def learn_materials(shex, k3st, hbins):
    """P(material | terrain type, height bin) on the China stage; returns (mats, cdf) tables
    of shape (256, nbins, 8)."""
    t = shex[..., 0]
    Z, X = np.mgrid[112:912, 112:912]
    lo = np.clip(np.round((Z - 114) / 4).astype(int), 0, 199)
    hi = np.clip(np.round((X - 114 - 2 * (lo & 1)) / 4).astype(int), 0, 199)
    tt = t[lo, hi]
    h = k3st[112:912, 112:912, 0]
    m = k3st[112:912, 112:912, 7]
    hb = np.digitize(h, hbins)
    nb = len(hbins) + 1
    mats = np.zeros((256, nb, 8), np.uint8)
    cdf = np.ones((256, nb, 8), np.float32)
    for k in np.unique(tt):
        sk = tt == k
        overall = np.bincount(m[sk], minlength=64)
        for b in range(nb):
            cnt = np.bincount(m[sk & (hb == b)], minlength=64).astype(float)
            if cnt.sum() < 50:
                cnt = overall.astype(float)
            top = np.argsort(-cnt)[:8]
            p = cnt[top] / cnt[top].sum()
            mats[k, b] = top
            cdf[k, b] = np.cumsum(p)
    return mats, cdf


def hex_heights(t, e0, mtn, rng):
    """Koei-style height byte per hex centre."""
    base = 20 + 48 * smoothstep(e0, 500, 3000) + 3 * noise(t.shape, rng, 1.5)
    depth = ndimage.distance_transform_edt(mtn)
    bump = (42 + 90 * smoothstep(depth, 1, 3.2)) * (0.8 + 0.45 * smoothstep(e0, 800, 4500))
    h = np.where(mtn, base + bump + 18 * noise(t.shape, rng, 1.0), base)
    h = np.where(t == MARSH, 20, h)
    h = np.where(t == SHORE, 16, h)
    h = np.where(t == STREAM, 2, h)
    h = np.where(np.isin(t, (RIVER, SEA)), 0, h)
    return np.clip(h, 0, 250).astype(np.float32)


def hex_to_vertex(field, VZ, VX, z0, z1, order=1):
    """Sample a per-hex field at global vertices rows z0..z1 (bilinear between hex centres, odd rows
    shifted by half a hex)."""
    R, C = field.shape
    Z, X = np.meshgrid(np.arange(z0, z1, dtype=np.float32), np.arange(VX, dtype=np.float32), indexing="ij")
    lf = (Z - 114) / 4.0
    l0 = np.floor(lf)
    f = lf - l0
    out = np.zeros(Z.shape, np.float32)
    for dl, wgt in ((0, 1 - f), (1, f)):
        row = np.clip(l0 + dl, 0, R - 1)
        col = (X - 114 - 2 * (row.astype(int) & 1)) / 4.0
        v = ndimage.map_coordinates(field, [row.ravel(), np.clip(col, 0, C - 1).ravel()], order=order, mode="nearest")
        out += wgt * v.reshape(Z.shape)
    return out


def upsample2(a, z0, z1, VX, order):
    """Rows z0..z1 and columns 0..VX of a field stored at half resolution (exact, so strips join)."""
    Z, X = np.meshgrid(np.arange(z0, z1, dtype=np.float32) / 2, np.arange(VX, dtype=np.float32) / 2, indexing="ij")
    return ndimage.map_coordinates(a, [Z.ravel(), X.ravel()], order=order, mode="nearest").reshape(Z.shape)


def nearest_hex(VZ, VX, z0, z1, R, C):
    Z, X = np.meshgrid(np.arange(z0, z1), np.arange(VX), indexing="ij")
    lo = np.clip(np.round((Z - 114) / 4.0).astype(int), 0, R - 1)
    hi = np.clip(np.round((X - 114 - 2 * (lo & 1)) / 4.0).astype(int), 0, C - 1)
    return lo, hi


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default=str(DATA / "world"))
    ap.add_argument("--rows", type=int, default=1800)
    ap.add_argument("--cols", type=int, default=1000)
    ap.add_argument("--lon0", type=float, default=-15.07)
    ap.add_argument("--lat0", type=float, default=77.0)
    ap.add_argument("--china-lo", type=int, default=1234)
    ap.add_argument("--china-hi", type=int, default=436)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--cache", help="npz cache for the sampled hex elevations (speeds up re-runs)")
    ap.add_argument("--hex-only", action="store_true", help="skip the vertex level")
    ap.add_argument("--no-roads", action="store_true")
    ap.add_argument("--keep-china-strip", action="store_true", help="keep Koei's east-edge type-6 sea strip")
    ap.add_argument("--stamp-cities", action="store_true", help="write new city footprints into the hex map (needs M7)")
    ap.add_argument("--types-from", help="take the hex terrain from this world_types.npy (e.g. edited in tools/editor) "
                                         "instead of classifying the DEM; cities_world.json is left as it is")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    spec = WorldSpec(rows=a.rows, cols=a.cols, lon0=a.lon0, lat0=a.lat0, china_lo=a.china_lo, china_hi=a.china_hi)
    geo = WorldGeo(spec, DATA / "cities_georef.json")
    dem = Dem(DATA / "etopo60s_i16.npy")
    rng = np.random.default_rng(a.seed)
    R, C = spec.rows, spec.cols
    CL, CH = china_box(spec)
    china_shex = np.load(DATA / "china_shex.npy")
    overrides = {} if a.keep_china_strip else china_sea_strip(china_shex)
    for (lo, hi), r in overrides.items():
        china_shex[lo, hi] = r
    t0 = time.time()

    # ---- hex level: elevation
    key = f"{R},{C},{spec.lon0},{spec.lat0},{spec.china_lo},{spec.china_hi}"
    if a.cache and Path(a.cache).exists() and str(np.load(a.cache)["key"]) == key:
        z = np.load(a.cache)
        e0, relief, lat_c = z["e0"], z["relief"], z["lat"]
    else:
        e0, relief, lat_c = sample_hexes(geo, dem, R, C)
        if a.cache:
            np.savez(a.cache, e0=e0, relief=relief, lat=lat_c, key=key)
    print(f"hex elevation sampled ({time.time() - t0:.0f}s)")
    china_learn = china_shex.copy()             # Koei's own types: the 3D materials are learned from these
    patch_ov, patch_hex = china_patch(china_shex, e0[CL, CH], relief[CL, CH], a.seed + 101)
    patch_types = np.full((200, 200), 255, np.uint8)  # the patch before roads (connect_cities.py starts from it)
    for (lo, hi), r in patch_ov.items():
        patch_types[lo, hi] = r[0]
    road_hex = np.zeros((200, 200), bool)       # roads connect_cities.py laid on Koei's own part of the block
    if a.types_from:                            # keep the roads connect_cities.py laid in the block
        prev = np.load(a.types_from)[CL, CH]
        laid = np.isin(prev, (ROAD, PLANK, BRIDGE))
        for (lo, hi), r in patch_ov.items():
            if laid[lo, hi]:
                r[0] = prev[lo, hi]
        kt = china_learn[..., 0]
        b4 = {k: int(np.bincount(china_learn[..., 4][kt == k]).argmax()) if (kt == k).any() else 6
              for k in (ROAD, PLANK, BRIDGE)}
        for lo, hi in zip(*np.nonzero(laid & ~patch_hex & (china_shex[..., 0] != prev))):
            r = china_shex[lo, hi].copy()
            r[0] = prev[lo, hi]
            r[4] = b4[r[0]]
            patch_ov[(int(lo), int(hi))] = r
            road_hex[lo, hi] = True
    for (lo, hi), r in patch_ov.items():
        china_shex[lo, hi] = r
    overrides.update(patch_ov)
    write_overrides(out / "china_overrides.bin", overrides)
    np.save(out / "china_patch.npy", patch_hex)
    np.save(out / "china_patch_types.npy", patch_types)
    china_t = china_shex[..., 0]
    print(f"China patch: {int(patch_hex.sum())} hexes of Koei's south-east mountains classified again")

    if a.types_from:
        t = np.load(a.types_from)
        if t.shape != (R, C):
            raise SystemExit(f"{a.types_from}: shape {t.shape}, expected {(R, C)}")
        t = t.astype(np.uint8)
        t[CL, CH] = china_t
        print(f"terrain types read from {a.types_from}")
    else:
        # ---- water
        lakes = rasterize_polygons(geo, read_shapes(next((DATA / "raw/ne_lakes").glob("*.zip"))), R, C)
        rivers = rasterize_rivers(geo, read_shapes(next((DATA / "raw/ne_rivers").glob("*.zip"))), R, C, e0, rng)
        sea = (e0 < 0) & ~lakes
        lab, n = ndimage.label(sea)          # ocean only where connected to the open sea (no Turpan basin)
        sizes = ndimage.sum(sea, lab, range(1, n + 1))
        sea = np.isin(lab, 1 + np.flatnonzero(sizes > 2000))
        lab, n = ndimage.label(lakes)        # Koei draws lakes as rivers (type 7); only inland seas are sea
        sizes = ndimage.sum(lakes, lab, range(1, n + 1))
        inland_sea = np.isin(lab, 1 + np.flatnonzero(sizes > 3000))
        t = np.full((R, C), GRASS, np.uint8)
        t[rivers > 0] = rivers[rivers > 0]
        t[lakes] = RIVER
        t[sea | inland_sea] = SEA
        t[CL, CH] = china_t
        print(f"hydrography ({time.time() - t0:.0f}s): {lakes.sum()} lake hexes, {(rivers > 0).sum()} river hexes")

        # ---- mountains, land cover
        water = np.isin(t, WATER)
        dw = ndimage.distance_transform_edt(~water).astype(np.float32)
        coast_km = ndimage.distance_transform_edt(~(t == SEA)) * 9.0
        land = ~water
        land[CL, CH] = False
        mtn, p_mtn = mountain_mask(e0, relief, dw, land, rng)
        cland = ~np.isin(china_t, WATER + (ROAD, 11, 12, 13, CITY, 17, 18, 19))
        print(f"mountain target share on China's own DEM: {p_mtn[CL, CH][cland].mean():.3f} (Koei: {(china_t[cland] == MOUNTAIN).mean():.3f})")
        t[mtn] = MOUNTAIN
        dmt = ndimage.distance_transform_edt(~(t == MOUNTAIN)).astype(np.float32)
        dchina = dist_to_china(spec, R, C)
        weights = climate_weights(e0, lat_c, coast_km, dchina, rng)
        cover_land = land & ~mtn
        cover = land_cover((R, C), weights, dw, dmt, e0, rng, cover_land)
        t[cover_land] = cover[cover_land]
        joined = connect_china_rivers(t, spec)
        t = continue_china_edge(t, spec, rng)
        t[CL, CH] = china_t
        # shores: Koei puts a 1-hex bank (type 14) on about 45% of the non-mountain land touching water,
        # most of it along the sea
        water = np.isin(t, WATER)
        touch_sea = hex_neighbors(t == SEA)
        touch_water = hex_neighbors(water)
        u = uniform_noise((R, C), rng, 1.3)
        cand = np.isin(t, COVER) & touch_water & (relief < 250)
        shore = cand & (u < np.where(touch_sea, 0.75, 0.42))
        shore[CL, CH] = False
        t[shore] = SHORE
        t[CL, CH] = china_t
        print(f"terrain classified ({time.time() - t0:.0f}s), {joined} China rivers joined")

        # new cities: choose sites now; stamping them as city terrain needs the base-count expansion (M7)
        cities_json = DATA / "new_cities.json"
        if cities_json.exists():
            t_sites = t.copy()
            placed = place_cities(geo, t_sites, json.loads(cities_json.read_text(encoding="utf-8")),
                                  (spec.china_lo, spec.china_hi))
            (out / "cities_world.json").write_text(json.dumps(
                [{"id": FIRST_NEW_CITY_ID + i, "name": nm, "lo": int(y), "hi": int(x)} for i, (nm, y, x) in enumerate(placed)],
                ensure_ascii=False, indent=1), encoding="utf-8")
            if a.stamp_cities:
                t = t_sites
            print(f"{len(placed)} city sites chosen{' and stamped' if a.stamp_cities else ''}")
            if not a.no_roads:
                footprint = np.zeros((R, C), bool)
                for _, y, x in placed:
                    for yy, xx in [(y, x)] + hex_ring(y, x):
                        footprint[yy, xx] = True
                gates = china_gateways(t, spec, json.loads((DATA / "cities_georef.json").read_text(encoding="utf-8")))
                for _, y, x in gates:
                    footprint[y, x] = True
                nodes = [(nm, y, x) for nm, y, x in placed] + gates
                built, ne = build_roads(t, spec, nodes, footprint, len(gates))
                print(f"roads: {built}/{ne} routes between {len(placed)} sites and {len(gates)} China gateways "
                      f"({', '.join(g[0] for g in gates)}) ({time.time() - t0:.0f}s)")

    outside = np.ones((R, C), bool)
    outside[CL, CH] = False
    stats = {int(k): int(v) for k, v in zip(*np.unique(t[outside], return_counts=True))}
    lnd = outside & ~np.isin(t, WATER)
    print("terrain types outside China:", stats)
    print(f"mountain share of land outside China: {(t[lnd] == MOUNTAIN).mean():.3f} (Koei China: 0.48)")
    np.save(out / "world_types.npy", t)

    rec = np.zeros((R, C, 11), np.uint8)
    rec[..., 0] = t
    rec[..., 1] = UNOWNED_AREA & 0xFF
    rec[..., 2] = UNOWNED_AREA >> 8
    rec[..., 4] = np.where(t == SEA, 4, np.where(t == RIVER, 2, 6))
    rec[..., 9] = 15
    rec[CL, CH] = china_shex
    with open(out / "world_shex.bin", "wb") as f:
        f.write(b"WSHX" + struct.pack("<II", C, R))
        f.write(rec.tobytes())
    print(f"world_shex.bin written ({time.time() - t0:.0f}s)")
    info = {"rows": R, "cols": C, "lon0": spec.lon0, "lat0": spec.lat0, "china_lo": spec.china_lo,
            "china_hi": spec.china_hi, "terrain_counts_outside_china": stats,
            "mountain_share_of_land": float((t[lnd] == MOUNTAIN).mean()), "georef_rms_hex": geo.residual_rms}
    if a.hex_only:
        (out / "world_info.json").write_text(json.dumps(info, indent=1))
        return

    # ---- vertex level (global vertex grid)
    k3st = np.load(DATA / "china_k3st_v.npy")
    hbins = np.array([40, 80, 120, 160, 200])
    mats, cdf = learn_materials(china_learn, k3st, hbins)
    VZ, VX = R * 4 + 225, C * 4 + 225
    vrng = np.random.default_rng(a.seed + 7)
    hh = hex_heights(t, e0, t == MOUNTAIN, vrng)
    mtnf = (t == MOUNTAIN).astype(np.float32)
    hgt = np.lib.format.open_memmap(out / "world_height.npy", mode="w+", dtype=np.uint8, shape=(VZ, VX))
    mat = np.lib.format.open_memmap(out / "world_mat.npy", mode="w+", dtype=np.uint8, shape=(VZ, VX))
    # one noise lattice for the whole grid (coarse, interpolated) so strips join seamlessly
    ridge = noise((VZ // 2 + 2, VX // 2 + 2), vrng, 1.8)
    patch = uniform_noise((VZ // 2 + 2, VX // 2 + 2), vrng, 1.5)
    S = 512
    for z0 in range(0, VZ, S):
        z1 = min(VZ, z0 + S)
        h = hex_to_vertex(hh, VZ, VX, z0, z1)
        m = hex_to_vertex(mtnf, VZ, VX, z0, z1)
        rz = upsample2(ridge, z0, z1, VX, 1)
        pz = upsample2(patch, z0, z1, VX, 0)
        h = h + m * 18 * rz
        lo, hi = nearest_hex(VZ, VX, z0, z1, R, C)
        tt = t[lo, hi]
        wv = np.isin(tt, WATER)
        h = np.where(wv, np.minimum(h, 3), np.maximum(h, 8))
        hb = np.digitize(h, hbins)
        c = cdf[tt, hb]                                   # (rows, VX, 8)
        idx = (c < pz[..., None]).sum(-1).clip(0, 7)
        mm = np.take_along_axis(mats[tt, hb], idx[..., None], -1)[..., 0]
        hgt[z0:z1] = np.clip(np.round(h), 0, 255).astype(np.uint8)
        mat[z0:z1] = mm
        if z0 % 2048 == 0:
            print(f"  vertex rows {z0}/{VZ} ({time.time() - t0:.0f}s)")
    # China's own stage, with the generated surroundings (and the patch) eased into its edge heights
    gz, gx = spec.china_lo * 4, spec.china_hi * 4
    kz, kx = np.mgrid[112:913, 112:913]
    klo = np.clip(np.round((kz - 114) / 4).astype(int), 0, 199)
    khi = np.clip(np.round((kx - 114 - 2 * (klo & 1)) / 4).astype(int), 0, 199)
    pv = patch_hex[klo, khi]                          # stage vertices of the patch: generated, not Koei's
    pad = 24
    a0, a1 = gz + 112 - pad, gz + 913 + pad
    b0, b1 = gx + 112 - pad, gx + 913 + pad
    win = hgt[a0:a1, b0:b1].astype(np.float32)
    inside = np.zeros(win.shape, bool)
    inside[pad:-pad, pad:-pad] = ~pv
    win[inside] = k3st[112:913, 112:913, 0][~pv]
    d, (iz, ix) = ndimage.distance_transform_edt(~inside, return_indices=True)
    w = 1 - smoothstep(d, 0, pad)
    win = np.where(inside, win, w * win[iz, ix] + (1 - w) * win)
    hgt[a0:a1, b0:b1] = np.clip(np.round(win), 0, 255).astype(np.uint8)
    blk = (slice(gz + 112, gz + 913), slice(gx + 112, gx + 913))
    # roads laid on Koei's own block keep his heights and get the material Koei's roads of that type have
    rv = road_hex[klo, khi] & ~pv
    road_mat = mats[china_t[klo, khi], np.digitize(k3st[112:913, 112:913, 0], hbins), 0]
    mat[blk] = np.where(pv, mat[blk], np.where(rv, road_mat, k3st[112:913, 112:913, 7]))
    # china_patch.bin: b"WCPM", u32 1025, then 1025x1025 bytes over Koei's stage vertices, 1 = worldmod
    # draws the generated vertex (world_height/world_mat) instead of Koei's there
    pvs = np.zeros((1025, 1025), np.uint8)
    pvs[112:913, 112:913] = pv | rv
    (out / "china_patch.bin").write_bytes(b"WCPM" + struct.pack("<I", 1025) + pvs.tobytes())
    hgt.flush()
    mat.flush()
    info["vertex_shape"] = [VZ, VX]
    (out / "world_info.json").write_text(json.dumps(info, indent=1))
    print(f"done in {time.time() - t0:.0f}s -> {out}")


if __name__ == "__main__":
    main()
