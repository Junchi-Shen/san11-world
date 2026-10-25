"""convert_scen.py - convert Koei's scenario files to the expanded city layout (M7_LAYOUT.md §8).

usage:
  python convert_scen.py --cities-json bases_cities.json --out DIR [--game <game dir>] [--no-ts]
         [--troops 0 --gold 0 --food 0]
  python convert_scen.py --self-test [--game ...]      (C=42 identity check of every stock file, writes nothing)

Reads the stock files under <game>\\Media and writes converted copies (same file names):
  <out>\\scenario\\Scen*.s11       the 14 scenarios (kind 0x16)
  <out>\\scenario\\Scenario.s11    master data (kind 0x18)
  <out>\\script\\data\\TS*.s11      stage/tutorial files (save-format world, kind 0x0c/0x1b), unless --no-ts

Id layout (from gen_bases.py / bases_cities.json): cities 0..C-1 (Koei 0..41 unchanged, new 42..C-1),
gates C..C+9, ports C+10..N-1, facilities N.., unit locations N+unit, N = C+45.
Old -> new: id < 42 unchanged; 42..86 (gates/ports) + (C-42); >= 87 (facilities, N+unit locations) + (N-87).

What changes (all streams are plain: the XOR flag of every stock file is 0, which is checked):
  Scen*.s11   building records 87 -> N (C-42 new city records inserted at index 42; type 0, no owner,
              position = global lo/hi from the json), person +0x98/+0x9c renumbered, C-42 city records (81 B)
              appended. Scenario header (owner/preview arrays at 0x1DB/0x205) stays 42 entries (KEEP42).
  Scenario.s11  C-42 city records (25 B) appended after Koei's 42; Koei neighbour lists from koei_neighbours.
  TS*.s11     building table (16384 records) re-indexed, person +0x98/+0x9c, unit +0x30 (target type 0),
              C-42 city records (107 B) appended, AI block: C-42 base records inserted at index 42 and
              unit-record byte [2] renumbered. The 200x200 per-hex block is left as it is.
Every converted file is re-parsed, checked and converted back (inverse) to the original bytes; with
C=42 every conversion is the identity (checked for every stock file on every run).

Wide ids (M7_LAYOUT.md §9.6, phase C; always written): the seven i8 city / target id fields of the
streams are i16 (the DLL serializes them with 0x48a9e0), so ids can exceed 254:
  Scen*.s11   item 57 -> 58 B, force 72 -> 73 B, corps 8 -> 10 B, country name (type 14) 87 -> 89 B
  Scenario.s11  city 25 -> 31 B (6 x i16 neighbours)
  TS / saves  city 107 -> 108 B, force +1, corps +2, item 58, type 14 89; AI unit record [2..3] = u16 target
The narrow conversion above is done first (and checked against its inverse), then widened (and checked
against narrowing back).
Only reads the game files; writes only below --out.
"""
import argparse
import json
import os
import struct
import sys
from pathlib import Path

HDR = 90
C0, N0, G, P = 42, 87, 10, 35
KIND_SCEN = {2, 3, 0x16, 0x17}
KIND_MASTER = {4, 0x18}
KIND_TS = {0x0C, 0x1B}

BLD = 18            # u8 type | i8 force | u16 durability | u32 +0x14 | u32 +0x18 | u8 +0x1c | i16 lo | i16 hi | i8 +4
NBLD_SAVE = 0x4000
PERSON_SCEN, PERSON_SAVE = 152, 203
P_AFF, P_LOC = 96, 98   # i16 person+0x98 (affiliation building id), person+0x9c (location: base id or N+unit)
CITY_SCEN, CITY_SAVE = 81, 107
MASTER_CITY = 25        # name[5] reading[13] i8 province i8 neighbours[6]
MASTER_CITY_WIDE = 31   # the same with i16 neighbours[6] (phase C)
UNIT = 98
U_TTYPE, U_TID = 21, 22  # i8 unit+0x34 target type (0 = building), u16 unit+0x30 target id
AI0 = 20020             # AI sub-object block with 87 base records
AI_BASE = 0x2ce8        # base records (52 B each) inside the AI block, followed by 1000 x 4 unit records
AI_REC = 0x34
TEMPLATE_CITY = 18      # 上庸: a modest Koei city whose static attributes the new cities copy


class ConvError(Exception):
    pass


def remap(v, C, N):
    """old building/base/location id -> new id (negative = none, unchanged)"""
    if v < C0:
        return v
    if v < N0:
        return v + (C - C0)
    return v + (N - N0)


def unmap(v, C, N):
    if v < C0:
        return v
    if v < C:
        raise ConvError(f"id {v} is a new city, it has no old id")
    if v < N:
        return v - (C - C0)
    return v - (N - N0)


# ---------------------------------------------------------------- file header

def header(D):
    kind = struct.unpack_from('<I', D, 4)[0]
    v1, v2, h24, xor = struct.unpack_from('<4I', D, 0x18)
    if not D[8:0x18].startswith(b'KOEI'):
        raise ConvError('not a .s11 stream')
    return kind, v1, v2, xor


# ---------------------------------------------------------------- layouts

# positions of the i8 id fields in the narrow records (the wide record has an i16 at each, in order)
WIDE_SCEN = {'item': [55], 'force': [55], 'army': [5, 7], 'type14': [81, 82]}
WIDE_SAVE = dict(WIDE_SCEN, city=[89])


def scen_layout(C, N, wide=False, R=42):
    """R: regular forces (M8_FORCES.md); F = R+5 force and corps records, each force record with F relations"""
    w = 1 if wide else 0
    F = R + 5
    W = (F + 31) // 32          # dwords of the force bit set (+0x50), 2 for up to 64 forces
    secs = [('filehdr', 1, HDR), ('scen_info', 1, 595), ('force_desc', 42, 369), ('world_head', 1, 11),
            ('building', N, BLD), ('person', 850, PERSON_SCEN), ('item', 100, 57 + w), ('force', F, 72 + w + F - 47 + 4 * (W - 2)),
            ('army', F, 8 + 2 * w), ('city', C, CITY_SCEN), ('gate', G, 64), ('port', P, 64), ('type14', 84, 87 + 2 * w)]
    return _place(secs, 0)


# ---------------------------------------------------------------- M8: more forces and corps (M8_FORCES.md)
# Koei: forces 0..41 regular, 42..45 barbarians, 46 bandits (47); corps 0..41 regular, 42..46 belong to forces
# 42..46. With R regular forces the barbarian / bandit forces and their corps move to R..R+4 (F = R+5 of each).
# The stream's header mark becomes "WF" + u16 R (worldmod checks it against its own R).
P_CORPS = 95          # person +0x94 corps (i8)


def fmap(v, R):
    """old force / corps id -> new (0xff = none)"""
    if v == 0xff or v < 42:
        return v
    if v <= 46:
        return R + v - 42
    raise ConvError(f'force/corps id {v} out of range')


def force_mark(R):
    return b'WF' + struct.pack('<H', R) if R != 42 else WIDE_MARK


def forces_expand(W, C, N, R):
    """wide scenario stream with 47 forces/corps -> R regular forces (F = R+5); R = 42 gives W back"""
    if R == 42:
        return W
    if not 42 < R <= min(249, C):
        raise ConvError(f'regular force count {R}: 42..{min(249, C)} supported (no more than the cities, F = R+5 <= 254)')
    F = R + 5
    NW = (F + 31) // 32                                     # dwords of the force bit set
    L, end = scen_layout(C, N, wide=True)
    if len(W) != end or W[0x2c:0x30] != WIDE_MARK:
        raise ConvError('not a wide scenario stream of this layout')
    D = bytearray(W)
    # (the scenario info's 42 bytes at 0x1DB are the owners' colours for the preview map, not force ids)
    o, n, sz = L['building']                                # building +1 owner force
    for i in range(n):
        D[o + i * sz + 1] = fmap(D[o + i * sz + 1], R)
    o, n, sz = L['person']                                  # person corps
    for i in range(n):
        D[o + i * sz + P_CORPS] = fmap(D[o + i * sz + P_CORPS], R)
    for name in ('city', 'gate', 'port'):                   # +0 corps
        o, n, sz = L[name]
        for i in range(n):
            D[o + i * sz] = fmap(D[o + i * sz], R)
    newid = {old: fmap(old, R) for old in range(47)}        # old record -> new record index

    # forces: ruler i16, i16, relations u8[47], 4 bytes, target type, target id i16, force bits u64, u64
    fo, fn, fsz = L['force']
    old = [bytes(D[fo + k * fsz: fo + (k + 1) * fsz]) for k in range(fn)]
    tmpl = old[41]                                          # an unused regular force in every Koei scenario?
    forces = [None] * F
    for k, r in enumerate(old):
        forces[newid[k]] = r
    out_f = []
    for k in range(F):
        r = forces[k]
        if r is None:                                       # new regular force slot: like Koei's unused ones
            r = b'\xff\xff\xff\xff' + bytes([50] * 47) + tmpl[51:]
            r = r[:4] + bytes([50] * 47) + r[51:]
            row_old = None
        rel_old = r[4:51]
        rel = bytearray([50] * F)
        for j in range(47):
            rel[newid[j]] = rel_old[j]
        if forces[k] is None:
            rel[k] = 100
            for j in range(42, 47):
                rel[newid[j]] = tmpl[4 + j]
        tail = bytearray(r[51:])                            # +0x3c +0x40 +0x44 +0x48 type, +0x4c id (i16), masks
        if tail[3] == 0 and tail[4:6] != b'\xff\xff':       # target type 0 = force
            tail[4:6] = struct.pack('<h', fmap(struct.unpack('<h', tail[4:6])[0] & 0xff, R))
        if forces[k] is None:
            tail[1] = 0xff                                  # no 国号
            tail[3:6] = b'\xff\xff\xff'                     # no target
            tail[6:14] = bytes(8)
        bits = struct.unpack('<Q', bytes(tail[6:14]))[0]
        nb = 0
        for j in range(47):
            if bits >> j & 1:
                nb |= 1 << newid[j]
        tail[6:14] = nb.to_bytes(4 * NW, 'little')           # F bits (NW dwords)
        out_f.append(bytes(r[:4]) + bytes(rel) + bytes(tail))

    # corps: force, number, leader i16, (type, id i16) x 2
    ao, an, asz = L['army']
    olda = [bytes(D[ao + k * asz: ao + (k + 1) * asz]) for k in range(an)]
    empty = next(r for r in olda if r[0] == 0xff)
    corps = [None] * F
    for k, r in enumerate(olda):
        corps[newid[k]] = r
    out_a = []
    for k in range(F):
        r = bytearray(corps[k] if corps[k] is not None else empty)
        r[0] = fmap(r[0], R)
        for t in (4, 7):
            if r[t] == 0 and r[t + 1:t + 3] != b'\xff\xff':
                r[t + 1:t + 3] = struct.pack('<h', fmap(struct.unpack('<h', bytes(r[t + 1:t + 3]))[0] & 0xff, R))
        out_a.append(bytes(r))
    E = bytes(D[:fo]) + b''.join(out_f) + b''.join(out_a) + bytes(D[ao + an * asz:])
    E = E[:0x2c] + force_mark(R) + E[0x30:]
    L2, end2 = scen_layout(C, N, wide=True, R=R)
    if len(E) != end2:
        raise ConvError(f'forces_expand: {len(E)} bytes, layout says {end2}')
    return E


def save_layout(v1, v2, base, C, N, wide=False):
    """save-format world (TS files / saves) starting at file offset base (COORDS.md §5.1)"""
    w = 1 if wide else 0
    F = 72 - (0 if v1 >= 1 else 4) + 59 + ((58 + (24 if v2 > 0 else 0)) if v1 >= 1 else 0) + w
    A = 8 + (2 if v1 >= 1 else 4) + 4 + 1 + 2 * w
    T18 = 8 + 256 + 4096 + 47 * 4608 + 4 + 1023 + (2050 if (v1 >= 1 and v2 > 1) else 0) + 4 + 256
    secs = [('world_head', 1, 2212), ('building', NBLD_SAVE, BLD), ('person', 1100, PERSON_SAVE),
            ('item', 100, 57 + w), ('force', 47, F), ('army', 47, A), ('city', C, CITY_SAVE + w), ('gate', G, 70),
            ('port', P, 70), ('type14', 84, 87 + 2 * w), ('unit', 1000, UNIT), ('new_person', 150, 0x247),
            ('obj_1ba5ea', 50, 0x109), ('type18', 1, T18), ('ai', 1, AI0 + AI_REC * (N - N0)),
            ('hexmap', 40000, 4)]
    return _place(secs, base)


def _place(secs, o):
    out = {}
    for name, cnt, size in secs:
        out[name] = (o, cnt, size)
        o += cnt * size
    return out, o


WIDE_MARK = b'WIDE'      # header dword at 0x2c (unused by Koei, always 0): the stream has i16 id fields


def mark_wide(D):
    if D[0x2c:0x30] != bytes(4):
        raise ConvError('header dword 0x2c is not free')
    return D[:0x2c] + WIDE_MARK + D[0x30:]


def widen(D, Ln, Lw, poss):
    """narrow stream -> wide: every listed i8 field becomes the sign-extended i16 (sections in file order)"""
    out = bytearray()
    names = sorted(Ln, key=lambda k: Ln[k][0])
    start = Ln[names[0]][0]
    out += D[:start]
    for name in names:
        o, c, s = Ln[name]
        if name in poss:
            for i in range(c):
                r = D[o + s * i:o + s * (i + 1)]
                last = 0
                for q in poss[name]:
                    out += r[last:q] + struct.pack('<h', r[q] - 256 if r[q] >= 128 else r[q])
                    last = q + 1
                out += r[last:]
        else:
            out += D[o:o + s * c]
    end = max(o + s * c for o, c, s in Ln.values())
    out += D[end:]
    if len(out) != max(o + s * c for o, c, s in Lw.values()) + (len(D) - end):
        raise ConvError('widened size mismatch')
    return mark_wide(bytes(out))


def narrow(E, Lw, Ln, poss):
    """wide stream -> narrow (the inverse of widen, for checking); ids must fit a signed byte"""
    out = bytearray()
    names = sorted(Lw, key=lambda k: Lw[k][0])
    out += E[:Lw[names[0]][0]]
    for name in names:
        o, c, s = Lw[name]
        if name in poss:
            for i in range(c):
                r = E[o + s * i:o + s * (i + 1)]
                last = 0
                for k, q in enumerate(poss[name]):
                    q += k                                   # position in the wide record
                    v = struct.unpack_from('<h', r, q)[0]
                    if not -128 <= v <= 255:
                        raise ConvError(f'{name} id {v} does not fit the narrow format')
                    out += r[last:q] + bytes([v & 0xff])
                    last = q + 2
                out += r[last:]
        else:
            out += E[o:o + s * c]
    end = max(o + s * c for o, c, s in Lw.values())
    out += E[end:]
    if out[0x2c:0x30] == WIDE_MARK:
        out[0x2c:0x30] = bytes(4)
    return bytes(out)


def sect(D, L, name):
    o, c, s = L[name]
    return [bytearray(D[o + s * i:o + s * (i + 1)]) for i in range(c)]


# ---------------------------------------------------------------- new records

def i16(v):
    return struct.pack('<h', v)


def check_new(cfg):
    C, N = cfg['C'], cfg['N']
    if N != C + G + P:
        raise ConvError(f'N={N} must be C+45={C + G + P}')
    if not (C0 <= C <= 1000):
        raise ConvError(f'C={C} out of range (42..1000)')
    new = sorted(cfg.get('new', []), key=lambda x: x['id'])
    if [x['id'] for x in new] != list(range(C0, C)):
        raise ConvError('"new" must list exactly the city ids 42..C-1')
    for x in new:
        lo, hi = int(x['lo']), int(x['hi'])
        if 0 <= lo < 200 and 0 <= hi < 200:
            raise ConvError(f"city {x['id']} position ({lo},{hi}) lies inside 0..199 in both coordinates: "
                            "worldmod's load hook would translate it as a China-local position")
        if not (-32768 <= lo < 32768 and -32768 <= hi < 32768):
            raise ConvError(f"city {x['id']} position out of i16 range")
        if not 0 <= int(x['province']) <= 11:
            raise ConvError(f"city {x['id']} province {x['province']} not in 0..11")
        nb = [int(v) for v in x['neighbours']]
        if len(nb) > 6 or any(not (0 <= v < C) or v == x['id'] for v in nb):
            raise ConvError(f"city {x['id']} neighbours {nb}: at most 6 city ids < C, not itself")
        name_bytes(x)
    kn = {int(k): [int(v) for v in vs] for k, vs in cfg.get('koei_neighbours', {}).items()}
    for k, vs in kn.items():
        if not 0 <= k < C0 or len(vs) > 6 or any(not (0 <= v < C) or v == k for v in vs):
            raise ConvError(f'koei_neighbours[{k}] = {vs} invalid')
    return new, kn


def name_bytes(x):
    for enc in ('big5', 'cp950'):
        try:
            b = x['name'].encode(enc)
            break
        except UnicodeEncodeError:
            b = None
    if b is None:
        raise ConvError(f"city {x['id']} name {x['name']!r} is not Big5-encodable")
    if len(b) > 4:
        raise ConvError(f"city {x['id']} name {x['name']!r} is {len(b)} bytes, name[5] holds 4 + NUL")
    return b + b'\0' * (5 - len(b))


def new_building(tmpl, x):
    r = bytearray(tmpl)
    r[0] = 0                                  # type city
    r[1] = 0xff                               # force: none
    struct.pack_into('<hh', r, 13, int(x['lo']), int(x['hi']))
    r[17] = 4                                 # +4, 4 for every Koei base
    return r


def new_city_body(tmpl81, opts):
    """81-byte city record (scenario stream; also bytes 2..82 of the save-format record)"""
    r = bytearray(tmpl81)
    r[0] = 0xff                               # +0x38 army (corps): none -> no owner
    # +0x3c u32 (troop cap, static) kept; +0x40 troops, +0x44 gold, +0x48 food
    struct.pack_into('<III', r, 5, opts['troops'], opts['gold'], opts['food'])
    r[17:65] = bytes(48)                      # +0x4c u32[12] weapons/horses/ships: none
    r[73] = 0                                 # +0x84 (0 in every unowned Koei city)
    r[74] = 0x32                              # +0x85 (0x32 in every unowned Koei city)
    r[75:81] = bytes(6)                       # +0x86 u8[6] city specialities: none
    return r


def master_city(x, wide=True):
    r = bytearray(name_bytes(x))              # name[5]
    r += bytes(13)                            # reading[13]: empty
    r += struct.pack('<B', int(x['province']))
    nb = [int(v) for v in x['neighbours']]
    if wide:
        r += b''.join(struct.pack('<h', v) for v in nb) + b'\xff\xff' * (6 - len(nb))
        assert len(r) == MASTER_CITY_WIDE
    else:
        r += bytes(v & 0xff for v in nb) + b'\xff' * (6 - len(nb))
        assert len(r) == MASTER_CITY
    return r


def master_nb(vs, wide):
    if wide:
        return b''.join(struct.pack('<h', v) for v in vs) + b'\xff\xff' * (6 - len(vs))
    return bytes(v & 0xff for v in vs) + b'\xff' * (6 - len(vs))


# ---------------------------------------------------------------- converters

def conv_scen(D, cfg, new, opts):
    kind, v1, v2, xor = header(D)
    if kind not in KIND_SCEN or xor:
        raise ConvError(f'not a plain scenario stream (kind {kind:#x}, xor {xor})')
    L0, end0 = scen_layout(C0, N0)
    if len(D) != end0:
        raise ConvError(f'size {len(D)} != {end0}')
    C, N = cfg['C'], cfg['N']
    bld = sect(D, L0, 'building')
    tb = bld[TEMPLATE_CITY]
    nbld = bld[:C0] + [new_building(tb, x) for x in new] + bld[C0:]
    persons = sect(D, L0, 'person')
    for r in persons:
        for off in (P_AFF, P_LOC):
            v = struct.unpack_from('<h', r, off)[0]
            struct.pack_into('<h', r, off, remap(v, C, N))
    cities = sect(D, L0, 'city')
    tc = new_city_body(cities[TEMPLATE_CITY], opts)
    ncities = cities + [bytearray(tc) for _ in new]
    o_b = L0['building'][0]
    o_p = L0['person'][0]
    o_pe = o_p + 850 * PERSON_SCEN
    o_c, _, _ = L0['city']
    o_ce = o_c + C0 * CITY_SCEN
    out = (D[:o_b] + b''.join(nbld) + b''.join(persons) + D[o_pe:o_c] + b''.join(ncities) + D[o_ce:])
    return bytes(out)


def unconv_scen(E, cfg):
    C, N = cfg['C'], cfg['N']
    L, end = scen_layout(C, N)
    if len(E) != end:
        raise ConvError(f'converted size {len(E)} != {end}')
    bld = sect(E, L, 'building')
    persons = sect(E, L, 'person')
    for r in persons:
        for off in (P_AFF, P_LOC):
            v = struct.unpack_from('<h', r, off)[0]
            struct.pack_into('<h', r, off, unmap(v, C, N))
    cities = sect(E, L, 'city')
    o_b = L['building'][0]
    o_p = L['person'][0]
    o_pe = o_p + 850 * PERSON_SCEN
    o_c = L['city'][0]
    o_ce = o_c + C * CITY_SCEN
    return bytes(E[:o_b] + b''.join(bld[:C0] + bld[C:]) + b''.join(persons) + E[o_pe:o_c]
                 + b''.join(cities[:C0]) + E[o_ce:])


def check_scen(E, cfg, new):
    C, N = cfg['C'], cfg['N']
    L, end = scen_layout(C, N)
    if len(E) != end:
        raise ConvError('size')
    bld = sect(E, L, 'building')
    types = [r[0] for r in bld]
    want = [0] * C + [1] * G + [2] * P
    if types != want:
        raise ConvError('building types are not cities/gates/ports in the new id order')
    for x in new:
        r = bld[x['id']]
        lo, hi = struct.unpack_from('<hh', r, 13)
        if (lo, hi) != (x['lo'], x['hi']) or r[1] != 0xff:
            raise ConvError(f"new city building {x['id']} wrong")
    for r in sect(E, L, 'person'):
        for off in (P_AFF, P_LOC):
            v = struct.unpack_from('<h', r, off)[0]
            if v >= N and off == P_AFF:
                raise ConvError(f'person affiliation {v} >= N in a scenario')
            if C0 <= v < C:
                raise ConvError(f'person points at a new city {v}')
    cities = sect(E, L, 'city')
    for x in new:
        if cities[x['id']][0] != 0xff:
            raise ConvError('new city has an owner')


def conv_master(D, cfg, new, kn, wide=True):
    kind, v1, v2, xor = header(D)
    if kind not in KIND_MASTER or xor:
        raise ConvError(f'not a plain master stream (kind {kind:#x})')
    if len(D) != 47928:
        raise ConvError(f'master size {len(D)} != 47928')
    o = HDR
    cities = [bytearray(D[o + MASTER_CITY * i:o + MASTER_CITY * (i + 1)]) for i in range(C0)]
    for i, r in enumerate(cities):
        vs = [v for v in r[19:25] if v != 0xff]
        r[19:] = master_nb(kn.get(i, vs), wide)
    cities += [master_city(x, wide) for x in new]
    out = bytes(D[:o] + b''.join(cities) + D[o + MASTER_CITY * C0:])
    return mark_wide(out) if wide else out


def check_master(E, D, cfg, new, kn):
    C, R = cfg['C'], MASTER_CITY_WIDE
    if len(E) != 47928 + R * C - MASTER_CITY * C0:
        raise ConvError('master size')
    o = HDR
    if E[0x2c:0x30] != WIDE_MARK or E[:0x2c] != D[:0x2c] or E[0x30:o] != D[0x30:o]:
        raise ConvError('master header wrong')
    if E[o + R * C:] != D[o + MASTER_CITY * C0:]:
        raise ConvError('master data after the city records changed')

    def nbs(r):
        return [v for v in struct.unpack_from('<6h', r, 19) if v != -1]
    for i in range(C0):
        a = E[o + R * i:o + R * (i + 1)]
        b = D[o + MASTER_CITY * i:o + MASTER_CITY * (i + 1)]
        if a[:19] != b[:19]:
            raise ConvError(f'Koei city {i} name/reading/province changed')
        if nbs(a) != (kn[i] if i in kn else [v for v in b[19:25] if v != 0xff]):
            raise ConvError(f'Koei city {i} neighbours wrong')
    for x in new:
        r = E[o + R * x['id']:o + R * (x['id'] + 1)]
        if nbs(r) != [int(v) for v in x['neighbours']] or r[18] != int(x['province']):
            raise ConvError(f"master city {x['id']} wrong")


def ts_base(D):
    return HDR + 256                          # TS header (0x480b50); saves would be HDR + 345


def conv_ts(D, cfg, new, opts, wide=False):
    kind, v1, v2, xor = header(D)
    if kind not in KIND_TS or xor:
        raise ConvError(f'not a plain TS stream (kind {kind:#x})')
    base = ts_base(D)
    L0, end0 = save_layout(v1, v2, base, C0, N0)
    if len(D) != end0:
        raise ConvError(f'TS size {len(D)} != {end0}')
    C, N = cfg['C'], cfg['N']
    dC, dN = C - C0, N - N0
    # buildings: keep 16384 records
    bld = sect(D, L0, 'building')
    tail = bld[NBLD_SAVE - dN:] if dN else []
    for k, r in enumerate(tail):
        if r[0] != 0xff:
            raise ConvError(f'building slot {NBLD_SAVE - dN + k} is used; cannot shift facilities by {dN}')
    tb = bld[TEMPLATE_CITY]
    nbld = bld[:C0] + [new_building(tb, x) for x in new] + bld[C0:N0] + bld[N0:NBLD_SAVE - dN]
    assert len(nbld) == NBLD_SAVE
    persons = sect(D, L0, 'person')
    for r in persons:
        for off in (P_AFF, P_LOC):
            v = struct.unpack_from('<h', r, off)[0]
            struct.pack_into('<h', r, off, remap(v, C, N))
    cities = sect(D, L0, 'city')
    t = cities[TEMPLATE_CITY]
    tcity = bytearray(i16(-1)) + new_city_body(t[2:83], opts) + t[83:]   # governor none + body + save tail
    ncities = cities + [bytearray(tcity) for _ in new]
    units = sect(D, L0, 'unit')
    for r in units:
        if r[U_TTYPE] == 0:
            v = struct.unpack_from('<H', r, U_TID)[0]
            if v < NBLD_SAVE:
                nv = remap(v, C, N)
                if nv >= NBLD_SAVE:
                    raise ConvError('unit target facility shifted out of the building table')
                struct.pack_into('<H', r, U_TID, nv)
    ao, _, asz = L0['ai']
    ai = bytearray(D[ao:ao + asz])
    recs = [ai[AI_BASE + AI_REC * i:AI_BASE + AI_REC * (i + 1)] for i in range(N0)]
    empty = [bytes(r) for r in recs if r[0] == 0xff]
    dflt = max(set(empty), key=empty.count) if empty else b'\xff\x00\xff\xff' + bytes(AI_REC - 4)
    nrecs = recs[:C0] + [bytearray(dflt) for _ in new] + recs[C0:]
    uo = AI_BASE + AI_REC * N0
    uni = bytearray(ai[uo:uo + 4000])
    for k in range(1000):
        b = uni[4 * k + 2]
        if b != 0xff:
            nb = remap(b, C, N)
            if wide:
                struct.pack_into('<H', uni, 4 * k + 2, nb)     # [2..3] u16 (stock [3] is always 0xff with [2] = 0xff)
            elif nb > 254:
                raise ConvError('AI unit target id does not fit a byte')
            else:
                uni[4 * k + 2] = nb
    nai = ai[:AI_BASE] + b''.join(nrecs) + uni + ai[uo + 4000:]
    ob = L0['building'][0]
    op = L0['person'][0]
    oc = L0['city'][0]
    oce = oc + C0 * CITY_SAVE
    ou = L0['unit'][0]
    oue = ou + 1000 * UNIT
    out = (D[:ob] + b''.join(nbld) + b''.join(persons) + D[op + 1100 * PERSON_SAVE:oc]
           + b''.join(ncities) + D[oce:ou] + b''.join(units) + D[oue:ao] + nai + D[ao + asz:])
    return bytes(out)


def unconv_ts(E, cfg, wide=False):
    kind, v1, v2, xor = header(E)
    C, N = cfg['C'], cfg['N']
    dC, dN = C - C0, N - N0
    base = ts_base(E)
    L, end = save_layout(v1, v2, base, C, N)
    if len(E) != end:
        raise ConvError(f'converted TS size {len(E)} != {end}')
    bld = sect(E, L, 'building')
    unused = bytes(bld[-1])
    obld = bld[:C0] + bld[C:N] + bld[N:] + [bytearray(unused) for _ in range(dN)]
    persons = sect(E, L, 'person')
    for r in persons:
        for off in (P_AFF, P_LOC):
            v = struct.unpack_from('<h', r, off)[0]
            struct.pack_into('<h', r, off, unmap(v, C, N))
    cities = sect(E, L, 'city')
    units = sect(E, L, 'unit')
    for r in units:
        if r[U_TTYPE] == 0:
            v = struct.unpack_from('<H', r, U_TID)[0]
            if v < NBLD_SAVE:
                struct.pack_into('<H', r, U_TID, unmap(v, C, N))
    ao, _, asz = L['ai']
    ai = bytearray(E[ao:ao + asz])
    recs = [ai[AI_BASE + AI_REC * i:AI_BASE + AI_REC * (i + 1)] for i in range(N)]
    uo = AI_BASE + AI_REC * N
    uni = bytearray(ai[uo:uo + 4000])
    for k in range(1000):
        if wide:
            v = struct.unpack_from('<H', uni, 4 * k + 2)[0]
            if v != 0xffff:
                uni[4 * k + 2] = unmap(v, C, N); uni[4 * k + 3] = 0xff
            continue
        b = uni[4 * k + 2]
        if b != 0xff:
            uni[4 * k + 2] = unmap(b, C, N)
    oai = ai[:AI_BASE] + b''.join(recs[:C0] + recs[C:]) + uni + ai[uo + 4000:]
    ob = L['building'][0]
    op = L['person'][0]
    oc = L['city'][0]
    oce = oc + C * CITY_SAVE
    ou = L['unit'][0]
    oue = ou + 1000 * UNIT
    return bytes(E[:ob] + b''.join(obld) + b''.join(persons) + E[op + 1100 * PERSON_SAVE:oc]
                 + b''.join(cities[:C0]) + E[oce:ou] + b''.join(units) + E[oue:ao] + oai + E[ao + asz:])


def check_ts(E, cfg, new):
    kind, v1, v2, xor = header(E)
    C, N = cfg['C'], cfg['N']
    L, end = save_layout(v1, v2, ts_base(E), C, N)
    if len(E) != end:
        raise ConvError('TS size')
    bld = sect(E, L, 'building')
    types = [r[0] for r in bld[:N]]
    if types != [0] * C + [1] * G + [2] * P:
        raise ConvError('TS base building types out of order')
    if any(r[0] in (0, 1, 2) for r in bld[N:]):
        raise ConvError('a base record lies in the facility range')
    cities = sect(E, L, 'city')
    for x in new:
        if cities[x['id']][2] != 0xff or struct.unpack_from('<h', cities[x['id']], 0)[0] != -1:
            raise ConvError('new TS city has an owner or governor')


# ---------------------------------------------------------------- driver

def find_files(game):
    sc = Path(game) / 'Media' / 'scenario'
    td = Path(game) / 'Media' / 'script' / 'data'
    if not sc.is_dir():
        raise ConvError(f'{sc} not found')
    scen = sorted(p for p in sc.iterdir() if p.is_file() and p.name.lower().startswith('scen')
                  and p.name.lower() != 'scenario.s11' and p.suffix.lower() == '.s11')
    master = [p for p in sc.iterdir() if p.name.lower() == 'scenario.s11']
    ts = sorted(p for p in td.iterdir() if p.is_file() and p.name.lower().startswith('ts')
                and p.suffix.lower() == '.s11') if td.is_dir() else []
    if len(scen) not in (14, 15) or len(master) != 1:
        raise ConvError(f'expected 14 or 15 Scen*.s11 and Scenario.s11 in {sc}, found {len(scen)} / {len(master)}')
    return scen, master[0], ts


IDENT = {'C': C0, 'N': N0, 'new': [], 'koei_neighbours': {}}


def self_test(scen, master, ts, opts):
    n = 0
    for p in scen:
        D = p.read_bytes()
        if conv_scen(D, IDENT, [], opts) != D or unconv_scen(D, IDENT) != D:
            raise ConvError(f'C=42 identity failed for {p.name}')
        n += 1
    D = master.read_bytes()
    if conv_master(D, IDENT, [], {}, wide=False) != D:
        raise ConvError('C=42 identity failed for Scenario.s11')
    n += 1
    for p in ts:
        D = p.read_bytes()
        if conv_ts(D, IDENT, [], opts) != D or unconv_ts(D, IDENT) != D:
            raise ConvError(f'C=42 identity failed for {p.name}')
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cities-json')
    ap.add_argument('--out')
    ap.add_argument('--game', default=str(Path(__file__).resolve().parent.parent.parent), help='game directory (Media/scenario)')
    ap.add_argument('--no-ts', action='store_true', help='do not convert media/script/data/TS*.s11')
    ap.add_argument('--self-test', action='store_true', help='only run the C=42 identity check')
    ap.add_argument('--troops', type=int, default=0, help='troops of the new (unowned) cities (Koei: 0)')
    ap.add_argument('--gold', type=int, default=0, help='gold of the new cities (Koei unowned cities: 0)')
    ap.add_argument('--food', type=int, default=0, help='food of the new cities (Koei unowned cities: 0)')
    ap.add_argument('--forces', type=int, default=42, help='regular forces R (M8_FORCES.md); 42 = Koei')
    a = ap.parse_args()
    opts = {'troops': a.troops, 'gold': a.gold, 'food': a.food}
    try:
        scen, master, ts = find_files(a.game)
        if a.no_ts:
            ts = []
        n = self_test(scen, master, ts, opts)
        print(f'C=42 identity: {n} stock files reproduced byte for byte')
        if a.self_test:
            return 0
        if not a.cities_json or not a.out:
            ap.error('--cities-json and --out are required')
        cfg = json.loads(Path(a.cities_json).read_text(encoding='utf-8'))
        new, kn = check_new(cfg)
        out = Path(a.out)
        (out / 'scenario').mkdir(parents=True, exist_ok=True)
        C, N = cfg['C'], cfg['N']
        Ln, _ = scen_layout(C, N)
        Lw, _ = scen_layout(C, N, wide=True)
        for p in scen:
            D = p.read_bytes()
            E = conv_scen(D, cfg, new, opts)
            check_scen(E, cfg, new)
            if unconv_scen(E, cfg) != D:
                raise ConvError(f'{p.name}: inverse conversion does not give the stock file back')
            W = widen(E, Ln, Lw, WIDE_SCEN)
            if narrow(W, Lw, Ln, WIDE_SCEN) != E:
                raise ConvError(f'{p.name}: widened ids do not narrow back')
            W = forces_expand(W, C, N, a.forces)
            (out / 'scenario' / p.name).write_bytes(W)
            print(f'{p.name}: {len(D)} -> {len(W)} bytes')
        D = master.read_bytes()
        E = conv_master(D, cfg, new, kn)
        check_master(E, D, cfg, new, kn)
        (out / 'scenario' / master.name).write_bytes(E)
        print(f'{master.name}: {len(D)} -> {len(E)} bytes')
        if ts:
            (out / 'script' / 'data').mkdir(parents=True, exist_ok=True)
        for p in ts:
            D = p.read_bytes()
            E = conv_ts(D, cfg, new, opts, wide=True)
            check_ts(E, cfg, new)
            if unconv_ts(E, cfg, wide=True) != D:
                raise ConvError(f'{p.name}: inverse conversion does not give the stock file back')
            kind, v1, v2, xor = header(E)
            Ln_, _ = save_layout(v1, v2, ts_base(E), C, N)
            Lw_, _ = save_layout(v1, v2, ts_base(E), C, N, wide=True)
            W = widen(E, Ln_, Lw_, WIDE_SAVE)
            if narrow(W, Lw_, Ln_, WIDE_SAVE) != E:
                raise ConvError(f'{p.name}: widened ids do not narrow back')
            (out / 'script' / 'data' / p.name).write_bytes(W)
            print(f'{p.name}: {len(D)} -> {len(W)} bytes')
        print(f'done: C={cfg["C"]} N={cfg["N"]}, {len(new)} new cities, '
              f'{len(kn)} Koei neighbour lists changed, written to {out}')
        return 0
    except ConvError as e:
        print('error:', e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
