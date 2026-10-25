"""Generate the data for more cities than Koei's 42 (M7_LAYOUT.md): territories in the world map,
the area/base tables the game kept as static data, and the new cities' records.

Usage: python gen_bases.py [world_dir] --cities C
  world_dir: data/world (world_types.npy, world_shex.bin, world_info.json, cities_world.json from
             tools/select_cities.py). C = total number of cities, 42..1000 (new cities = the first C-42 of
             cities_world.json, which is in importance order).
Writes into --out (default world_dir; the world_dir inputs from make_world.py are read as they are):
  world_shex.bin     area ids of the territories (u16) and the 7-hex city footprints (terrain 16)
  china_overrides.bin  edits of Koei's China block in final numbering ("WCO2"): make_world's, plus the
                     outer band hexes new cities take (the seam with Koei's areas, see territories())
  bases_tables.bin   "WBTB", u32 C, then {u32 size, bytes} for AREAADJ, MAT87A, MAT87B, MATC, AREA2CITY,
                     AREA2BASE, CITYTAB844, DBGBASE (the order worldmod's bases.cpp reads)
  bases_cities.json  the new cities (id, name, lo, hi, province, neighbours) for the scenario converter
  bases_objs.bin     their 3D city models (OBJS records added by worldmod)
  world_height.npy   the vertex heights with the land around each new city levelled (Koei's cities stand
                     on plateaus at their model's height); only when the world directory has vertex data
  china_patch.bin    copied from the world directory (make_world.py): where worldmod draws China from it
Id layout: cities 0..C-1 (Koei's 0..41 unchanged), gates C..C+9, ports C+10..N-1, special areas N..N+5,
N = C+45. With --cities 42 the tables equal the originals byte for byte (--verify checks that).
"""
import argparse
import json
import shutil
import struct
from collections import defaultdict, deque
from pathlib import Path

import numpy as np
from scipy import ndimage

HERE = Path(__file__).resolve().parent
DATA = HERE.parent / "data"
GAME = HERE.parent.parent                 # the game directory (--game); the default is this project's own layout
EXE = None                                # <game>/san11pk.exe: Koei's tables are read from it (main() sets these)
SCENARIO = None                           # <game>/Media/scenario/Scenario.s11: Koei's city neighbour lists
WATER = (6, 7, 8)
CITY_T = 16


def exe_bytes(va, n):
    with open(EXE, "rb") as f:
        f.seek(va - 0x400000)
        return f.read(n)


def hex_nbrs(y, x):
    d = [(0, -1), (0, 1), (-1, -1), (-1, 0), (1, -1), (1, 0)] if y % 2 == 0 else [(0, -1), (0, 1), (-1, 0), (-1, 1), (1, 0), (1, 1)]
    return [(y + a, x + b) for a, b in d]


def wall_ou_cells(pack, clo, chi):
    """城墙（OBJS type 46/47/48）的 footprint 占用的世界 ou 格子。pack 是 Media/san11pkres.bin 的路径，
    clo/chi 是 China 块的世界 hex 偏移（ou 偏移 = 2*clo, 2*chi）。内政用地（type 45）的 footprint 和这些
    格子重叠时，游戏会拒绝创建它的 3D 物体，所以铺地时要用它避开城墙。"""
    if not Path(pack).exists():
        return set()
    with open(pack, "rb") as f:
        head = f.read(16)
        if head[:4] != b"LINK":
            return set()
        count = struct.unpack_from("<I", head, 4)[0]
        if 0x12C5 >= count:
            return set()
        f.seek(16 + 8 * 0x12C5)
        off, size = struct.unpack("<II", f.read(8))
        f.seek(off)
        objs = f.read(size)
    dy, dx = 2 * clo, 2 * chi
    fp = ((0, 0), (-1, 0), (0, -1), (-1, -1))
    out = set()
    for i in range(65535):
        rec = objs[8 + i * 14:8 + i * 14 + 14]
        if not rec[0] or struct.unpack_from("<H", rec, 1)[0] not in (46, 47, 48):
            continue
        our = struct.unpack_from("<H", rec, 3)[0] + dy
        ouc = struct.unpack_from("<H", rec, 5)[0] + dx
        for a, b in fp:
            out.add((our + a, ouc + b))
    return out


# Facility plots (内政用地): SHEX byte 5 = HEX20 bit18 (0x484090); on entering the map 0x57E910 lists each
# city's bit18 hexes (by area) in its +0xF0 table of 30, without a bound check. Koei gives every city 10, 12,
# 15, 18, 20 or 22 plots of soil (type 1), in 1-4 compact patches of 4-10, 3.5-14 hexes from the city, in its
# own area, at most 26% of the flat land there (every Koei city has 76 or more such hexes). New cities get the
# tier of their town's population about AD 200 (data/city_population.json, by name, "pop"); without one, of
# their rank; places that did not exist then get PLOT_GONE. They go 3-14 hexes out on flat land of the city's
# own territory, at most PLOT_LAND_SHARE of it, so cities with little land get fewer.
PLOT_TIERS = ((250000, 22), (100000, 20), (40000, 18), (15000, 15), (5000, 12), (0, 10))
RANK_PLOTS = {1: 15, 2: 12, 3: 10}
PLOT_GONE = 6
PLOT_LAND_SHARE = 0.25
PLOT_MIN = 2                                    # ... but this many wherever the land allows
PLOT_TERRAIN = (0, 1, 2, 3, 5, 9, 14)           # flat cover and banks (no roads, water, mountains)


def plot_count(c, pop):
    p = pop.get(c["name"], {})
    if p.get("existed_200") is False:
        return PLOT_GONE
    if p.get("pop"):
        return next(n for lim, n in PLOT_TIERS if p["pop"] >= lim)
    return RANK_PLOTS.get(c.get("rank", 3), PLOT_TIERS[-1][1])


def place_plots(y0, x0, n, ok):
    """up to n plot hexes (and no more than PLOT_LAND_SHARE of the land that may hold one) for the city at
    (y0, x0), in patches of about 7 grown around seeds spread round the city 4-9 hexes out; ok(y, x): the hex
    may be a plot of this city"""
    def dist(y, x):
        return np.hypot((y - y0) * 0.866, x + 0.5 * (y & 1) - x0 - 0.5 * (y0 & 1))
    cand = {(y, x) for y in range(y0 - 17, y0 + 18) for x in range(x0 - 15, x0 + 16)
            if 3 <= dist(y, x) <= 14 and ok(y, x)}
    n = min(n, max(PLOT_MIN, int(PLOT_LAND_SHARE * len(cand))), len(cand))
    if n <= 0:
        return []
    k = max(1, round(n / 7))
    seeds = []
    for _ in range(k):
        pool = [h for h in sorted(cand) if 4 <= dist(*h) <= 9] or sorted(cand)
        if not pool:
            break
        if seeds:
            h = max(pool, key=lambda h: min(np.hypot((h[0] - s[0]) * 0.866, h[1] - s[1]) for s in seeds))
        else:
            h = min(pool, key=lambda h: abs(dist(*h) - 6))
        seeds.append(h)
    def grow(s, size):
        patch = [s]
        cand.discard(s)
        while len(patch) < size:
            nxt = {h for p in patch for h in hex_nbrs(*p) if h in cand}
            if not nxt:
                break
            h = min(sorted(nxt), key=lambda h: np.hypot((h[0] - s[0]) * 0.866, h[1] - s[1]))
            patch.append(h); cand.discard(h)
        return patch
    out = []
    for i, s in enumerate(seeds):
        size = (n - len(out)) // (len(seeds) - i)      # what is left, spread over the patches still to grow
        if size > 0 and s in cand:
            out += grow(s, size)
    while len(out) < n and cand:                       # patches that could not grow: start more
        out += grow(min(sorted(cand), key=lambda h: abs(dist(*h) - 6)), n - len(out))
    return out


def flatten_city_sites(w, out, new, clo, chi, patch, r_flat=8, r_blend=12):
    """Level the land around each new city to one height (Koei's cities stand on plateaus) and return
    those heights. Reads w/world_height.npy and w/world_mat.npy (material 0 = water, left alone) and
    writes out/world_height.npy; without vertex data (a --hex-only world) every city gets Koei's usual 20.
    A city inside Koei's block stands on his stage (the game draws his own there, which is not levelled):
    its height is his terrain height at the centre, so pick a flat site for it. The patch (make_world's
    china_patch(), 200x200 mask) is drawn from world_height.npy like the world outside."""
    src = w / "world_height.npy"
    if not src.exists() or not new:
        return [20] * len(new)
    dst = np.array(np.load(src))                     # in memory (~30 MB), so --out may equal the world directory
    mat = np.load(w / "world_mat.npy", mmap_mode="r")
    koei_v = np.load(DATA / "china_k3st_v.npy", mmap_mode="r")
    yy, xx = np.mgrid[-r_blend:r_blend + 1, -r_blend:r_blend + 1]
    d = np.hypot(yy, xx)
    wgt = np.clip((r_blend - d) / (r_blend - r_flat), 0, 1)          # 1 inside r_flat, 0 beyond r_blend
    heights = []
    for c in new:
        vr = c["lo"] * 4 + 114
        vc = c["hi"] * 4 + 114 + 2 * (c["lo"] & 1)
        if clo <= c["lo"] < clo + 200 and chi <= c["hi"] < chi + 200 and not patch[c["lo"] - clo, c["hi"] - chi]:
            heights.append(int(koei_v[vr - 4 * clo, vc - 4 * chi, 0]))
            continue
        sl = (slice(vr - r_blend, vr + r_blend + 1), slice(vc - r_blend, vc + r_blend + 1))
        hw = dst[sl].astype(np.float32)
        land = mat[sl] != 0
        near = land & (d <= 6)
        h = int(np.median(hw[near])) if near.any() else 20
        h = max(8, min(250, h))
        m = land & (wgt > 0)
        hw[m] = wgt[m] * h + (1 - wgt[m]) * hw[m]
        dst[sl] = np.clip(np.round(hw), 0, 255).astype(np.uint8)
        heights.append(h)
    np.save(out / "world_height.npy", dst)
    return heights


# Territories tile the land like the pieces of a jigsaw puzzle, as Koei's areas tile China: every land
# hex belongs to the city that is cheapest to walk to (one Dijkstra from all cities at once over the hex
# grid). Mountains and above all rivers are expensive to cross, so borders follow ridges and rivers.
# Coastal waters (up to 6 hexes from land) belong to the city of the nearest coast; open sea, and land
# farther than max_cost from every city (Siberia, the Sahara), stay with the special area of the margins.
TERR_COST = {0: 1.0, 1: 1.0, 2: 1.2, 3: 1.6, 4: 1.6, 5: 1.4, 6: 3.0, 7: 7.0, 8: 4.0, 9: 1.2, 10: 1.0, 11: 2.0,
             12: 2.0, 13: 1.5, 14: 1.0, 15: 3.0, 16: 1.0, 17: 1.0, 18: 1.0, 19: 1.0}


def hex_graph(cost):
    """hex grid graph (lo = row, odd rows half a hex further along hi); edge weight = cost of entering"""
    from scipy.sparse import csr_matrix
    R, C = cost.shape
    idx = np.arange(R * C).reshape(R, C)
    a_list = [idx[:, :-1].ravel(), idx[:-1].ravel()]
    b_list = [idx[:, 1:].ravel(), idx[1:].ravel()]
    rows = np.arange(R - 1)
    ev, od = rows[rows % 2 == 0], rows[rows % 2 == 1]
    a_list += [idx[ev, 1:].ravel(), idx[od, :-1].ravel()]
    b_list += [idx[ev + 1, :-1].ravel(), idx[od + 1, 1:].ravel()]
    a = np.concatenate(a_list); b = np.concatenate(b_list)
    w = cost.ravel()
    src = np.concatenate([a, b]); dst = np.concatenate([b, a])
    ok = np.isfinite(w[dst]) & np.isfinite(w[src])
    return csr_matrix((w[dst][ok].astype(np.float32), (src[ok], dst[ok])), shape=(R * C, R * C))


# The seam with Koei's China: Koei's areas fill his 200x200 block right up to its edge (the outer ones are
# decorative mountain margins), so territories that simply stopped at the block would meet his along a
# straight line. Instead the outer BAND hexes of the block are contested too: the seeds are the new cities
# and every block hex deeper than BAND (with its Koei area), and the band and the world outside go to
# the cheapest seed. A Koei seed starts with the walking cost from the nearest Koei city (so his areas
# grow outward around his cities, not as a strip parallel to the block edge); the decorative margins
# (special areas) start at 0. A band hex changes only when a new city wins it (Koei's own areas never move
# inside the block, and nothing changes next to a Koei base or on his facility plots); outside the block
# Koei's areas continue.
BAND = 12


def territories(t, new, clo, chi, china_area, koei_pos, coast=6, max_cost=60.0):
    """area of every hex outside the core of the block (new cities by their id, Koei areas in Koei numbering
    as negative -1 - a, -2**30 = none) and the band hexes a new city wins (mask)"""
    from scipy.sparse.csgraph import dijkstra
    R, C = t.shape
    cost = np.full(t.shape, np.inf, np.float32)
    for k, c in TERR_COST.items():
        cost[t == k] = c
    g = hex_graph(cost)
    ii, jj = np.mgrid[0:200, 0:200]
    depth = np.minimum(np.minimum(ii, 199 - ii), np.minimum(jj, 199 - jj))
    core = depth >= BAND
    core_lo, core_hi = np.nonzero(core)
    seeds = np.concatenate([np.array([c["lo"] * C + c["hi"] for c in new], np.int64),
                            ((core_lo + clo) * C + core_hi + chi).astype(np.int64)])
    label = np.concatenate([np.array([c["id"] for c in new], np.int64), -1 - china_area[core_lo, core_hi]])
    # start cost of the Koei seeds: walking cost from the nearest Koei city (0 for the special areas)
    kcity = np.array([lo_ * C + hi_ for lo_, hi_ in koei_pos], np.int64)
    dk = dijkstra(g, indices=kcity, min_only=True)
    special = china_area[core_lo, core_hi] >= 87
    start = np.concatenate([np.zeros(len(new)), np.where(special, 0.0, dk[seeds[len(new):]])])
    start = np.where(np.isfinite(start), start, 0.0)
    # one Dijkstra from a virtual node V joined to every seed by an edge of its start cost
    from scipy.sparse import csr_matrix, vstack, hstack
    V = R * C
    row = csr_matrix((start + 1e-3, (np.zeros(len(seeds), np.int64), seeds)), shape=(1, V + 1))
    G = vstack([hstack([g, csr_matrix((V, 1))]), row]).tocsr()
    dist, pred = dijkstra(G, indices=V, return_predecessors=True)
    dist = dist[:V].reshape(R, C)
    pred = pred[:V].astype(np.int64)
    # the seed every hex was reached from: pointer jumping up the shortest-path tree
    seed_of = np.where(pred == V, np.arange(V), pred)
    seed_of[pred < 0] = -1
    for _ in range(40):
        ok = seed_of >= 0
        nxt = seed_of.copy()
        nxt[ok] = seed_of[seed_of[ok]]
        if np.array_equal(nxt, seed_of):
            break
        seed_of = nxt
    lut = np.full(V, 0, np.int64)
    lut[seeds] = np.arange(len(seeds))
    lab = np.full(V, -2 ** 30, np.int64)
    ok = seed_of >= 0
    lab[ok] = label[lut[seed_of[ok]]]
    lab = lab.reshape(R, C)
    lab[dist > max_cost] = -2 ** 30
    sea = t == 8
    lab[sea & (ndimage.distance_transform_edt(sea) > coast)] = -2 ** 30   # open sea
    # band: only where a new city wins, not on or next to Koei's bases (terrain 16 city, 17 port, 18 gate)
    blk_t = t[clo:clo + 200, chi:chi + 200]
    near_base = ndimage.binary_dilation(np.isin(blk_t, (16, 17, 18)), iterations=2)
    blab = lab[clo:clo + 200, chi:chi + 200]
    won = (~core) & (blab >= 42) & ~near_base
    return lab, won


# New cities inside Koei's block take their territory out of his areas (his gates' and ports' too, which
# reach far inland): a block hex goes to the cheapest of them when walking there from it is cheaper than
# from every other base. The hexes on or next to Koei's bases, his facility plots and open sea never change
# hands; everything else in the block stays as Koei drew it.
def block_territories(t, cities, others, clo, chi, plots, max_cost=60.0, m=80):
    """block-local labels (city id, -1 = Koei's area stays) for the new cities inside the block
    (cities: [{id, lo, hi}]); others: [(lo, hi)] of every other base (Koei's base hexes, the new cities outside)"""
    from scipy.sparse.csgraph import dijkstra
    R, C = t.shape
    y0, y1 = max(0, clo - m), min(R, clo + 200 + m)
    y0 -= y0 % 2                                          # keep row parity
    x0, x1 = max(0, chi - m), min(C, chi + 200 + m)
    tw = t[y0:y1, x0:x1]
    cost = np.full(tw.shape, np.inf, np.float32)
    for k, c in TERR_COST.items():
        cost[tw == k] = c
    g = hex_graph(cost)
    W = x1 - x0
    src = [(c["lo"] - y0) * W + c["hi"] - x0 for c in cities]
    d_new, _, src_of = dijkstra(g, indices=src, min_only=True, return_predecessors=True)
    d_other = dijkstra(g, indices=[(y - y0) * W + x - x0 for y, x in others if y0 <= y < y1 and x0 <= x < x1],
                       min_only=True)
    sl = (slice(clo - y0, clo - y0 + 200), slice(chi - x0, chi - x0 + 200))
    d_new, d_other, src_of = (a_.reshape(y1 - y0, W)[sl] for a_ in (d_new, d_other, src_of))
    tb = t[clo:clo + 200, chi:chi + 200]
    near_base = ndimage.binary_dilation(np.isin(tb, (16, 17, 18)), iterations=2)
    take = (d_new < d_other) & (d_new <= max_cost) & ~near_base & (plots == 0) & (tb != 8)
    cid = {s: c["id"] for s, c in zip(src, cities)}
    out = np.full((200, 200), -1, np.int64)
    out[take] = [cid[s] for s in src_of[take]]
    return out


def bfs_matrix(edges, n, sources, nodes_ok):
    """hop counts between sources; paths may only pass through nodes for which nodes_ok(v) is true"""
    M = np.full((len(sources), len(sources)), 255, np.uint8)
    idx = {s: i for i, s in enumerate(sources)}
    for s in sources:
        dist = {s: 0}
        q = deque([s])
        while q:
            u = q.popleft()
            if u != s and not nodes_ok(u):
                continue
            for v in edges[u]:
                if v not in dist:
                    dist[v] = dist[u] + 1
                    q.append(v)
        for v, d in dist.items():
            if v in idx:
                M[idx[s], idx[v]] = min(d, 254)
    return M


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("world", nargs="?", default=str(DATA / "world"))
    ap.add_argument("--cities", type=int, required=True)
    ap.add_argument("--verify", action="store_true", help="with --cities 42: compare with the exe's tables")
    ap.add_argument("--out", help="output directory (default: the world directory); the inputs are never modified")
    ap.add_argument("--game", default=str(GAME), help="game directory (san11pk.exe and Media/scenario/Scenario.s11 are read)")
    a = ap.parse_args()
    global EXE, SCENARIO
    EXE = Path(a.game) / "san11pk.exe"
    SCENARIO = Path(a.game) / "Media" / "scenario" / "Scenario.s11"
    for f in (EXE, SCENARIO):
        if not f.exists():
            raise SystemExit(f"{f} not found: --game must be the game directory")
    C = a.cities
    if not 42 <= C <= 1000:
        raise SystemExit("--cities must be 42..1000")
    N = C + 45
    dC = C - 42
    w = Path(a.world)
    info = json.loads((w / "world_info.json").read_text())
    R, Cc = info["rows"], info["cols"]
    clo, chi = info["china_lo"], info["china_hi"]
    t = np.load(w / "world_types.npy")
    shex = np.fromfile(w / "world_shex.bin", np.uint8)
    hdr, rec = shex[:12], shex[12:].reshape(R, Cc, 11).copy()

    # ---- original tables, renumbered: gates/ports 42..86 -> C..N-1, special 87..92 -> N..N+5
    def renum(a_):
        a_ = int(a_)
        return a_ if a_ < 42 else a_ + dC
    adj0 = np.frombuffer(exe_bytes(0x796768, 93 * 56), np.int16).reshape(93, 7, 4)
    a2c0 = np.frombuffer(exe_bytes(0x79c2b0, 93), np.uint8)
    tab0 = np.frombuffer(exe_bytes(0x844130, 42 * 4), np.int32)
    dbg0 = np.frombuffer(exe_bytes(0x7e7bd8, 87 * 16), np.uint8).reshape(87, 16)
    master = SCENARIO.read_bytes()
    koei_nb = []
    koei_prov = []
    for i in range(42):
        r = master[0x5a + 25 * i:0x5a + 25 * i + 25]
        koei_prov.append(r[18])
        koei_nb.append([x for x in r[19:25] if x != 255])

    adj = {}                       # area -> list of (neighbour, route type)
    for u in range(93):
        adj[renum(u)] = [(renum(v), int(rt), int(p1), int(p2)) for v, rt, p1, p2 in adj0[u] if v >= 0]   # p1/p2: a hex for types 1/4
    city_nb = {i: list(koei_nb[i]) for i in range(42)}

    # ---- new cities and their territories
    new = json.loads((w / "cities_world.json").read_text(encoding="utf-8"))[:dC]
    koei = json.loads((DATA / "cities_georef.json").read_text(encoding="utf-8"))
    area = np.full((R, Cc), N + 3, np.int32)              # unclaimed world: the special area Koei used for its margins (90)
    china_area = rec[clo:clo + 200, chi:chi + 200, 1].astype(np.int32) | (rec[clo:clo + 200, chi:chi + 200, 2].astype(np.int32) << 8)
    seeds = [{**c, "id": 42 + k} for k, c in enumerate(new)]
    inside = []                                           # new cities inside Koei's block (on his own terrain)
    for c in new:
        cells = [(c["lo"], c["hi"])] + hex_nbrs(c["lo"], c["hi"])
        ins = [clo <= y < clo + 200 and chi <= x < chi + 200 for y, x in cells]
        if any(ins) and not all(ins):
            raise SystemExit(f"{c['name']}: its 7 hexes cross the edge of Koei's China block; move it fully inside or outside")
        inside.append(ins[0])
    lab, won = territories(t, [s for s, i in zip(seeds, inside) if not i], clo, chi, china_area,
                           [(k["lo"] + clo, k["hi"] + chi) for k in koei])
    unclaimed = lab == -2 ** 30
    koei_lab = (lab < 0) & ~unclaimed
    area[lab >= 42] = lab[lab >= 42]
    area[koei_lab] = np.vectorize(renum)(-1 - lab[koei_lab])              # Koei's areas continued outward
    area[clo:clo + 200, chi:chi + 200] = -1               # China keeps Koei's own areas (remapped by worldmod)
    band_new = np.where(won & (rec[clo:clo + 200, chi:chi + 200, 5] == 0),     # band hexes taken by new cities
                        lab[clo:clo + 200, chi:chi + 200], -1)
    if any(inside):
        bases = [(y + clo, x + chi) for y, x in zip(*np.nonzero(np.isin(t[clo:clo + 200, chi:chi + 200], (16, 17, 18))))]
        blk_lab = block_territories(t, [s for s, i in zip(seeds, inside) if i],
                                    bases + [(s["lo"], s["hi"]) for s, i in zip(seeds, inside) if not i],
                                    clo, chi, rec[clo:clo + 200, chi:chi + 200, 5])
        band_new = np.where(blk_lab >= 0, blk_lab, band_new)
    foot_blk = np.zeros((200, 200), bool)                 # city hexes of the new cities inside the block
    for k, c in enumerate(new):
        cid = 42 + k
        for y, x in [(c["lo"], c["hi"])] + hex_nbrs(c["lo"], c["hi"]):
            t[y, x] = CITY_T
            area[y, x] = cid
            rec[y, x, 0] = CITY_T
            rec[y, x, 4] = 6
            if inside[k]:
                band_new[y - clo, x - chi] = cid
                foot_blk[y - clo, x - chi] = True
    # facility plots: soil hexes flagged in SHEX byte 5, in the city's own territory
    pf = DATA / "city_population.json"
    pop = json.loads(pf.read_text(encoding="utf-8")) if pf.exists() else {}
    plot_blk = np.zeros((200, 200), bool)
    plots_made, plots_short = 0, []
    wall_ou = wall_ou_cells(Path(a.game) / "Media" / "san11pkres.bin", clo, chi)

    def plot_free(y, x):
        our, ouc = 2 * y + 57, 2 * x + 57 + (y & 1)
        return all((our + a, ouc + b) not in wall_ou for a, b in ((0, 0), (-1, 0), (0, -1), (-1, -1)))

    def owner(y, x):
        if clo <= y < clo + 200 and chi <= x < chi + 200:
            return band_new[y - clo, x - chi]
        return area[y, x]
    for k, c in enumerate(new):
        cid, want = 42 + k, plot_count(c, pop)
        got = place_plots(c["lo"], c["hi"], want, lambda y, x: 0 <= y < R and 0 <= x < Cc and owner(y, x) == cid
                          and t[y, x] in PLOT_TERRAIN and not rec[y, x, 5] and plot_free(y, x))
        for y, x in got:
            t[y, x] = 1
            if clo <= y < clo + 200 and chi <= x < chi + 200:
                plot_blk[y - clo, x - chi] = True
            else:
                rec[y, x, 0], rec[y, x, 4], rec[y, x, 5] = 1, 6, 1
        plots_made += len(got)
        if len(got) < want:
            plots_short.append(f"{c['name']} {len(got)}/{want}")

    # ---- adjacency between areas (whole world, China included with Koei's ids renumbered)
    full = area.copy()
    full[clo:clo + 200, chi:chi + 200] = np.where(band_new >= 0, band_new, np.vectorize(renum)(china_area))
    passable = ~np.isin(t, WATER) & (t != 15)
    border = defaultdict(lambda: [0, 0])                  # (a, b) -> [land contacts, water contacts]
    for dy, dx_even, dx_odd in ((0, 1, 1), (1, -1, 0), (1, 0, 1)):
        for parity in (0, 1):
            dx = dx_even if parity == 0 else dx_odd
            ys = np.arange(parity, R - dy, 2)
            if dx >= 0:
                A_ = full[ys][:, :Cc - dx]; B_ = full[ys + dy][:, dx:]
                pa = passable[ys][:, :Cc - dx]; pb = passable[ys + dy][:, dx:]
            else:
                A_ = full[ys][:, -dx:]; B_ = full[ys + dy][:, :Cc + dx]
                pa = passable[ys][:, -dx:]; pb = passable[ys + dy][:, :Cc + dx]
            m = A_ != B_
            for u, v, land in zip(A_[m].ravel(), B_[m].ravel(), (pa & pb)[m].ravel()):
                key = (int(min(u, v)), int(max(u, v)))
                border[key][0 if land else 1] += 1
    newareas = set(range(42, C))
    cand = defaultdict(list)
    for (u, v), (land, wat) in border.items():
        if u >= N or v >= N:                               # special areas get no new links
            continue
        if u not in newareas and v not in newareas:        # Koei's own links stay as they are
            continue
        rt = 0 if land >= 2 else 3
        score = land * 10 + wat
        cand[u].append((score, v, rt))
        cand[v].append((score, u, rt))
    for u in sorted(cand):
        cand[u].sort(reverse=True)
    for u in range(42, C):
        adj[u] = []
    added = 0
    for u in sorted(cand):
        for score, v, rt in cand[u]:
            if len(adj.setdefault(u, [])) >= 7 or len(adj.setdefault(v, [])) >= 7:
                continue
            if any(e[0] == v for e in adj[u]):
                continue
            adj[u].append((v, rt, -1, -1))
            adj[v].append((u, rt, -1, -1))
            added += 1
    # city adjacency (<= 6): direct city-to-city area links
    for u in range(C):
        city_nb.setdefault(u, [])
    for u in range(42, C):
        for v, rt, _, _ in sorted(adj[u], key=lambda e: -border[(min(u, e[0]), max(u, e[0]))][0]):
            if v < C and v not in city_nb[u] and len(city_nb[u]) < 6 and len(city_nb[v]) < 6:
                city_nb[u].append(v)
                city_nb[v].append(u)

    # make the city graph connected: nearest city pairs first, across components first (Kruskal), then
    # give every new city at least 2 neighbours; links keep the 6-city / 7-area limits
    pos = {i: (k["lo"] + clo, k["hi"] + chi + 0.5 * ((k["lo"] + clo) & 1)) for i, k in enumerate(koei)}
    for c in new:
        pos[c["id"]] = (c["lo"], c["hi"] + 0.5 * (c["lo"] & 1))
    ids = sorted(pos)
    P = np.array([pos[i] for i in ids])
    D = np.hypot(P[:, None, 0] - P[None, :, 0], P[:, None, 1] - P[None, :, 1])
    pairs = sorted((D[i, j], ids[i], ids[j]) for i in range(len(ids)) for j in range(i + 1, len(ids))
                   if D[i, j] <= 160 and (ids[i] >= 42 or ids[j] >= 42))
    parent = list(range(C))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for u in range(C):
        for v in city_nb[u]:
            parent[find(u)] = find(v)

    def link(u, v, d):
        if len(city_nb[u]) >= 6 or len(city_nb[v]) >= 6 or len(adj[u]) >= 7 or len(adj[v]) >= 7:
            return False
        if v in city_nb[u]:
            return False
        # route type: land if the straight line is mostly land, else water
        ys = np.linspace(pos[u][0], pos[v][0], 40).round().astype(int)
        xs = np.linspace(pos[u][1], pos[v][1], 40).round().astype(int).clip(0, Cc - 1)
        rt = 0 if (~np.isin(t[ys.clip(0, R - 1), xs], WATER)).mean() > 0.6 else 3
        city_nb[u].append(v); city_nb[v].append(u)
        if not any(e[0] == v for e in adj[u]):
            adj[u].append((v, rt, -1, -1)); adj[v].append((u, rt, -1, -1))
        return True
    extra = 0
    for d, u, v in pairs:
        if find(u) != find(v) and link(u, v, d):
            parent[find(u)] = find(v); extra += 1
    for d, u, v in pairs:
        if d <= 45 and (len(city_nb[u]) < 2 and u >= 42 or len(city_nb[v]) < 2 and v >= 42) and link(u, v, d):
            extra += 1

    # ---- tables
    AREAADJ = np.full((N + 6, 7, 4), -1, np.int16)
    for u, lst in adj.items():
        for k, e in enumerate(lst[:7]):
            AREAADJ[u, k] = e
    edges_a = {u: [e[0] for e in adj.get(u, []) if e[1] in (0, 3, 5)] for u in range(N + 6)}
    edges_b = {u: [e[0] for e in adj.get(u, [])] for u in range(N + 6)}
    MAT87A = bfs_matrix(edges_a, N + 6, list(range(N)), lambda v: v < N)
    MAT87B = bfs_matrix(edges_b, N + 6, list(range(N)), lambda v: v < N)
    MATC = bfs_matrix({u: city_nb[u] for u in range(C)}, C, list(range(C)), lambda v: True)
    AREA2CITY = np.zeros(N + 6, np.uint16)          # u16 since phase C (the game's table is u8)
    AREA2CITY[:C] = np.arange(C)
    AREA2CITY[C:N] = a2c0[42:87]
    AREA2CITY[N:N + 6] = a2c0[87:93]
    AREA2BASE = np.concatenate([np.arange(N), [-1, -8, -21, -24, -39, -41]]).astype(np.int32)
    CITYTAB844 = np.concatenate([tab0, np.full(dC, 12)]).astype(np.int32)
    DBGBASE = np.zeros((N, 16), np.uint8)
    DBGBASE[:42] = dbg0[:42]
    DBGBASE[C:N] = dbg0[42:87]

    if a.verify:
        ok = {
            "AREAADJ": AREAADJ.tobytes() == exe_bytes(0x796768, 93 * 56),
            "MAT87A": MAT87A.tobytes() == exe_bytes(0x797bc0, 87 * 87),
            "MAT87B": MAT87B.tobytes() == exe_bytes(0x799958, 87 * 87),
            "MATC": MATC.tobytes() == exe_bytes(0x79b830, 42 * 42),
            "AREA2CITY": AREA2CITY.astype(np.uint8).tobytes() == exe_bytes(0x79c2b0, 93),
            "AREA2BASE": AREA2BASE.tobytes() == exe_bytes(0x79c358, 93 * 4),
            "CITYTAB844": CITYTAB844.tobytes() == exe_bytes(0x844130, 42 * 4),
            "DBGBASE": DBGBASE.tobytes() == exe_bytes(0x7e7bd8, 87 * 16),
        }
        print("verify against the exe:", ok)

    out = Path(a.out) if a.out else w
    out.mkdir(parents=True, exist_ok=True)
    with open(out / "bases_tables.bin", "wb") as f:
        f.write(b"WBTB" + struct.pack("<I", C))
        for arr in (AREAADJ, MAT87A, MAT87B, MATC, AREA2CITY, AREA2BASE, CITYTAB844, DBGBASE):
            b = arr.tobytes()
            f.write(struct.pack("<I", len(b)) + b)
    # world SHEX with the territories (China's block is replaced by worldmod with Koei's, remapped)
    outside = area >= 0
    rec[..., 1][outside] = (area[outside] & 0xff).astype(np.uint8)
    rec[..., 2][outside] = (area[outside] >> 8).astype(np.uint8)
    (out / "world_shex.bin").write_bytes(hdr.tobytes() + rec.tobytes())
    # china_overrides.bin, final numbering ("WCO2": worldmod applies it after renumbering Koei's ids): the
    # make_world overrides (Koei numbering) renumbered, plus the band hexes new cities took
    ov = {}
    src_ov = w / "china_overrides.bin"
    if src_ov.exists():
        b = src_ov.read_bytes()
        if b[:4] == b"WCOV":
            n = struct.unpack_from("<I", b, 4)[0]
            for i in range(n):
                lo_, hi_ = struct.unpack_from("<HH", b, 8 + 15 * i)
                r = bytearray(b[12 + 15 * i:8 + 15 * (i + 1)])
                a_ = renum(r[1] | (r[2] << 8))
                r[1], r[2] = a_ & 0xff, a_ >> 8
                ov[(lo_, hi_)] = r
    for lo_, hi_ in zip(*np.nonzero(band_new >= 0)):
        r = ov.get((int(lo_), int(hi_)), bytearray(rec[clo + lo_, chi + hi_]))
        a_ = int(band_new[lo_, hi_])
        r[1], r[2] = a_ & 0xff, a_ >> 8
        if foot_blk[lo_, hi_]:                            # city hex: as Koei's city hexes (flags clear)
            r[0], r[4] = CITY_T, 6
            r[3] = r[5] = r[6] = r[7] = r[8] = r[10] = 0
        if plot_blk[lo_, hi_]:                            # facility plot: soil, byte 5 set
            r[0], r[4], r[5] = 1, 6, 1
        ov[(int(lo_), int(hi_))] = r
    with open(out / "china_overrides.bin", "wb") as f:
        f.write(b"WCO2" + struct.pack("<I", len(ov)))
        for (lo_, hi_), r in sorted(ov.items()):
            f.write(struct.pack("<HH", lo_, hi_) + bytes(r))
    np.save(out / "world_types.npy", t)
    # 3D city models for the new cities: OBJS records (type 0 = city model) in free object slots; worldmod
    # adds them when it loads Koei's OBJS. bases_objs.bin: "WBOB", u32 n, n x {u16 slot, 14-byte record}.
    # The record's height byte is the ground height the model stands on: in Koei's OBJS it always equals the
    # terrain height at the city centre, and the terrain around the city is a plateau at that height. So each
    # new city gets the median land height near its centre, and the land within 8 vertices is levelled to it
    # (blended out to 12), in a copy of world_height.npy written to the output directory.
    patch = np.load(w / "china_patch.npy") if (w / "china_patch.npy").exists() else np.zeros((200, 200), bool)
    heights = flatten_city_sites(w, out, new, clo, chi, patch)
    if out.resolve() != w.resolve():                      # worldmod draws the patch from world_height.npy
        if (w / "china_patch.bin").exists():
            shutil.copy2(w / "china_patch.bin", out / "china_patch.bin")
        else:
            (out / "china_patch.bin").unlink(missing_ok=True)
    with open(out / "bases_objs.bin", "wb") as f:
        f.write(b"WBOB" + struct.pack("<I", len(new)))
        for k, c in enumerate(new):
            our = 2 * c["lo"] + 57
            ouc = 2 * c["hi"] + 57 + (c["lo"] & 1)
            f.write(struct.pack("<H", 60000 + k) + struct.pack("<BHHHBfBB", 1, 0, our, ouc, heights[k], 0.0, 0, 0))
    # new city records for the scenario converter
    kpos = np.array([[k["lo"] + clo, k["hi"] + chi] for k in koei], float)
    cities = []
    for k, c in enumerate(new):
        j = int(np.argmin(np.hypot(kpos[:, 0] - c["lo"], kpos[:, 1] - c["hi"])))
        cities.append({"id": 42 + k, "name": c["name"], "full": c.get("full", c["name"]), "lo": c["lo"], "hi": c["hi"],
                       "province": int(koei_prov[koei[j]["id"]]), "neighbours": city_nb[42 + k]})
    koei_changed = {i: city_nb[i] for i in range(42) if city_nb[i] != koei_nb[i]}
    (out / "bases_cities.json").write_text(json.dumps({"C": C, "N": N, "new": cities, "koei_neighbours": koei_changed},
                                                      ensure_ascii=False, indent=1), encoding="utf-8")
    area[clo:clo + 200, chi:chi + 200] = -1               # the block's hexes are counted from band_new
    owned = np.concatenate([area[(area >= 42) & (area < C)].ravel(), band_new[band_new >= 0].ravel()])
    sizes = np.bincount(owned - 42, minlength=dC) if dC else []
    print(f"C={C} N={N}: {dC} new cities, {added} border links + {extra} distance links, territory sizes "
          f"{(int(np.min(sizes)), int(np.median(sizes)), int(np.max(sizes))) if dC else '-'}; "
          f"unreachable city pairs {int((MATC == 255).sum())}, base pairs {int((MAT87B == 255).sum())}")
    print(f"facility plots: {plots_made} for {dC} new cities"
          + (f"; fewer than planned (little land in the territory) for {len(plots_short)}: {', '.join(plots_short)}" if plots_short else ""))


if __name__ == "__main__":
    main()
