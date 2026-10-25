// terrainworld: the whole-world 3D terrain (M4B.md).
//
// The stage object 0x3260860 keeps its header and members; its four big arrays (quads, vertices,
// object grid, cells) live in DLL allocations sized for the world, and the patch table points the
// game at them. Global vertex of hex (lo, hi): row = lo*4 + 112, col = hi*4 + 112 + 2*(lo & 1).
//
// The K3ST and GCOL parsers are replaced: Koei's 1025x1025 China stage is parsed here and pasted at
// China's place (stage vertex (0,0) = global (4*china_lo, 4*china_hi)); everything else comes from
// world_height.npy / world_mat.npy (tools/make_world.py). Vertex colours, normals and quad records
// are synthesised with Koei's own rules, so the generated world is textured exactly like China:
//   quad bit0 = 1, bits 2-3 = layers-1, then per layer 6-bit material + 4-bit corner mask
//   (layers = the distinct corner materials ascending, the first covers all corners; mask bit0..3 =
//   corners (r+1,c+1), (r,c+1), (r+1,c), (r,c)), bits 44-51 water surface height (11 where a corner
//   is water), bits 54-63 the coarse-LOD layer of the 2x2 block, spread over its four quads.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "terrainworld.h"

namespace {

const int N = 1025, QN = 1024;             // China stage: vertices / quads per side
const size_t VREC = 10, QREC = 8, CREC = 10;
// China's 200x200 hexes cover stage vertices [112, 912]; without world data the whole stage is used
int g_ch0 = 112, g_ch1 = 912;
const int GUARD = 4;                       // spare rows before/after each array

TraceLogFn g_log = nullptr;
TerrainLayout L;

struct Npy { HANDLE file = nullptr, map = nullptr; const uint8_t* data = nullptr; int rows = 0, cols = 0; };
Npy g_height, g_mat;

int g_cvr = 0, g_cvc = 0;                  // global vertex of China stage vertex (0,0)
uint8_t* g_chinaV = nullptr;               // N*N*VREC, China stage vertices
uint8_t* g_chinaQ = nullptr;               // QN*QN*QREC, China stage quads
uint8_t* g_patch = nullptr;                // N*N, 1 = China stage vertex drawn from the world grid (china_patch.bin)
bool g_haveChina = false;
uint8_t g_pal[64][3];                      // material -> colour, learned from China's current colours

bool openNpy(const wchar_t* path, Npy& n) {
    n.file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (n.file == INVALID_HANDLE_VALUE) { n.file = nullptr; return false; }
    n.map = CreateFileMappingW(n.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!n.map) return false;
    const uint8_t* base = (const uint8_t*)MapViewOfFile(n.map, FILE_MAP_READ, 0, 0, 0);
    if (!base || memcmp(base, "\x93NUMPY", 6)) return false;
    uint16_t hlen = *(const uint16_t*)(base + 8);
    const char* hdr = (const char*)base + 10;
    const char* sh = strstr(hdr, "'shape': (");
    if (!sh || !strstr(hdr, "'|u1'") || sscanf_s(sh + 10, "%d, %d", &n.rows, &n.cols) != 2) return false;
    n.data = base + 10 + hlen;
    return true;
}

uint8_t* allocArray(size_t rowBytes, size_t rows, uint8_t fill) {
    size_t size = rowBytes * (rows + 2 * GUARD);
    uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!p) return nullptr;
    if (fill) memset(p, fill, size);
    return p + rowBytes * GUARD;
}

int shiftFor(int n) { int s = 0; while ((1 << s) < n) s++; return s; }

inline bool inPatch(int kr, int kc) { return g_patch && g_patch[(size_t)kr * N + kc]; }
inline bool inChinaVertex(int r, int c) {
    int kr = r - g_cvr, kc = c - g_cvc;
    return kr >= g_ch0 && kr <= g_ch1 && kc >= g_ch0 && kc <= g_ch1 && !inPatch(kr, kc);
}
inline bool inChinaQuad(int r, int c) {            // quad (r,c) spans vertices r..r+1, c..c+1
    int kr = r - g_cvr, kc = c - g_cvc;
    return kr >= g_ch0 && kr < g_ch1 && kc >= g_ch0 && kc < g_ch1 &&
           !inPatch(kr, kc) && !inPatch(kr, kc + 1) && !inPatch(kr + 1, kc) && !inPatch(kr + 1, kc + 1);
}

inline uint8_t* vtx(int r, int c) { return L.VB + ((size_t)r * L.VC + c) * VREC; }
inline uint64_t* quad(int r, int c) { return (uint64_t*)(L.QB + ((size_t)r * L.QC + c) * QREC); }

// Colour of a generated vertex: the material's colour, darker on slopes (from the stored normal).
inline void shadeColour(uint8_t* v) {
    float ny = (v[5] - 128) / 127.0f;
    int shade = (int)(ny * 255.0f) - 230;
    for (int k = 0; k < 3; k++) {
        int x = g_pal[v[7] & 63][k] + shade;
        v[1 + k] = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
    }
}

// Mean colour of each material over the China stage; colours: N*N RGB triplets, or null = vertex RGB.
void learnPalette(const uint8_t* colours) {
    double sum[64][3] = {}; double cnt[64] = {};
    for (int i = 0; i < N * N; i++) {
        const uint8_t* v = g_chinaV + (size_t)i * VREC;
        const uint8_t* c = colours ? colours + (size_t)i * 3 : v + 1;
        int m = v[7] & 63;
        for (int k = 0; k < 3; k++) sum[m][k] += c[k];
        cnt[m] += 1;
    }
    for (int m = 0; m < 64; m++)
        for (int k = 0; k < 3; k++) g_pal[m][k] = cnt[m] ? (uint8_t)(sum[m][k] / cnt[m] + 0.5) : 110;
    if (g_log) {
        for (int m = 0; m < 64; m++)
            g_log("pal[%d] = (%u,%u,%u) cnt=%.0f", m, g_pal[m][0], g_pal[m][1], g_pal[m][2], cnt[m]);
    }
}

// Koei's layer encoding for corner materials a=(r,c) b=(r,c+1) cc=(r+1,c) d=(r+1,c+1).
// Returns the layer list (ascending distinct materials) and their corner masks.
inline int layers(int a, int b, int cc, int d, int* ids, int* masks) {
    int m[4] = { a, b, cc, d };
    // sort 4 values
    for (int i = 1; i < 4; i++) { int x = m[i], j = i - 1; while (j >= 0 && m[j] > x) { m[j + 1] = m[j]; j--; } m[j + 1] = x; }
    int n = 0;
    for (int i = 0; i < 4; i++) if (i == 0 || m[i] != m[i - 1]) ids[n++] = m[i];
    for (int i = 0; i < n; i++) {
        int id = ids[i];
        masks[i] = i == 0 ? 15 : ((d == id) << 0) | ((b == id) << 1) | ((cc == id) << 2) | ((a == id) << 3);
    }
    return n;
}

void fillQuads() {
    // fine layers + water
    for (int r = 0; r < L.QR; r++) {
        for (int c = 0; c < L.QC; c++) {
            uint64_t* q = quad(r, c);
            if (g_haveChina && inChinaQuad(r, c)) {
                *q = *(const uint64_t*)(g_chinaQ + ((size_t)(r - g_cvr) * QN + (c - g_cvc)) * QREC);
                continue;
            }
            int a = vtx(r, c)[7] & 63, b = vtx(r, c + 1)[7] & 63, cc = vtx(r + 1, c)[7] & 63, d = vtx(r + 1, c + 1)[7] & 63;
            int ids[4], masks[4];
            int n = layers(a, b, cc, d, ids, masks);
            uint64_t v = 1 | ((uint64_t)(n - 1) << 2);
            for (int i = 0; i < n; i++) v |= (uint64_t)(ids[i] | (masks[i] << 6)) << (4 + 10 * i);
            if (a == 0 || b == 0 || cc == 0 || d == 0) v |= (uint64_t)11 << 44;     // water surface
            *q = v;
        }
    }
    // coarse LOD: per 2x2 block at even (r,c), corners two vertices apart; layer k goes to quad
    // (r + (k&1), c + (k>>1)) of the block
    for (int r = 0; r + 1 < L.QR; r += 2) {
        for (int c = 0; c + 1 < L.QC; c += 2) {
            if (g_haveChina && inChinaQuad(r, c) && inChinaQuad(r + 1, c + 1)) continue;   // Koei's own
            int r2 = r + 2 < L.VR ? r + 2 : L.VR - 1, c2 = c + 2 < L.VC ? c + 2 : L.VC - 1;
            int ids[4], masks[4];
            int n = layers(vtx(r, c)[7] & 63, vtx(r, c2)[7] & 63, vtx(r2, c)[7] & 63, vtx(r2, c2)[7] & 63, ids, masks);
            for (int k = 0; k < 4; k++) {
                uint64_t* q = quad(r + (k & 1), c + (k >> 1));
                uint64_t lod = k < n ? (uint64_t)(ids[k] | (masks[k] << 6)) : 0;
                *q = (*q & ~(0x3FFull << 54)) | (lod << 54);
            }
        }
    }
}

// Generated vertex from the world grid; China's stage where it belongs.
void fillVertices() {
    for (int r = 0; r < L.VR; r++) {
        const uint8_t* hr = g_height.data ? g_height.data + (size_t)r * g_height.cols : nullptr;
        const uint8_t* hu = g_height.data ? g_height.data + (size_t)(r > 0 ? r - 1 : r) * g_height.cols : nullptr;
        const uint8_t* hd = g_height.data ? g_height.data + (size_t)(r + 1 < L.VR ? r + 1 : r) * g_height.cols : nullptr;
        const uint8_t* mr = g_mat.data ? g_mat.data + (size_t)r * g_mat.cols : nullptr;
        for (int c = 0; c < L.VC; c++) {
            uint8_t* v = vtx(r, c);
            if (g_haveChina && inChinaVertex(r, c)) {
                memcpy(v, g_chinaV + ((size_t)(r - g_cvr) * N + (c - g_cvc)) * VREC, VREC);
                continue;
            }
            uint8_t h = hr ? hr[c] : 20, m = mr ? mr[c] : 18;
            // normal from neighbouring heights (fit on the stock stage: y = h*0.25, spacing 5, signs -1/-1)
            float dx = hr ? (hr[c + 1 < L.VC ? c + 1 : c] - hr[c > 0 ? c - 1 : c]) * 0.25f / 10.0f : 0;
            float dz = hr ? (hd[c] - hu[c]) * 0.25f / 10.0f : 0;
            float inv = 1.0f / sqrtf(dx * dx + 1.0f + dz * dz);
            v[0] = h;
            v[4] = (uint8_t)(128 + 127 * dx * inv);
            v[5] = (uint8_t)(128 + 127 * inv);
            v[6] = (uint8_t)(128 + 127 * dz * inv);
            v[7] = m;
            v[8] = h;
            v[9] = 0;
            shadeColour(v);
        }
    }
}

}  // namespace

bool terrainAllocate(int H, int W, TerrainLayout& out, TraceLogFn log) {
    g_log = log;
    L.VR = 4 * H + 225; L.VC = 4 * W + 225;
    L.QR = L.VR - 1;    L.QC = L.VC - 1;
    L.NCR = H + 56;     L.NCC = W + 56;      L.CSH = shiftFor(L.NCC);
    L.NGR = 2 * H + 112; L.NGC = 2 * W + 112; L.GSH = shiftFor(L.NGC);
    L.QB = allocArray((size_t)L.QC * QREC, L.QR, 0);
    L.VB = allocArray((size_t)L.VC * VREC, L.VR, 0);
    L.GB = allocArray((size_t)2 << L.GSH, L.NGR, 0xff);
    // the cell array: two sites encode "cell + 6" in 10-byte units ((CB - S + 6) / 10), so CB must be
    // congruent to S - 6 modulo 10; take a few spare bytes and pick the matching start
    {
        uint8_t* c = allocArray(CREC << L.CSH, L.NCR + 1, 0);
        if (c) {
            const uintptr_t S = 0x3260860;
            while (((uintptr_t)c - S + 6) % 10) c++;
        }
        L.CB = c;
    }
    out = L;
    if (g_log) g_log("terrain arrays: quads %dx%d @%p, vertices %dx%d @%p, grid %dx%d (stride %d) @%p, cells %dx%d (stride %d) @%p",
                     L.QR, L.QC, L.QB, L.VR, L.VC, L.VB, L.NGR, L.NGC, 1 << L.GSH, L.GB, L.NCR, L.NCC, 1 << L.CSH, L.CB);
    return L.QB && L.VB && L.GB && L.CB;
}

bool terrainOpen(const wchar_t* dir, int chinaLo, int chinaHi) {
    g_cvr = chinaLo * 4;
    g_cvc = chinaHi * 4;
    wchar_t p[MAX_PATH];
    swprintf_s(p, L"%s\\world_height.npy", dir);
    bool ok = openNpy(p, g_height);
    swprintf_s(p, L"%s\\world_mat.npy", dir);
    ok = ok && openNpy(p, g_mat);
    if (ok && (g_height.rows != L.VR || g_height.cols != L.VC || g_mat.rows != L.VR || g_mat.cols != L.VC)) {
        if (g_log) g_log("terrain: world grid %dx%d does not match the world size %dx%d; ignoring it", g_height.rows, g_height.cols, L.VR, L.VC);
        ok = false;
    }
    if (!ok) { g_height.data = nullptr; g_mat.data = nullptr; g_ch0 = 0; g_ch1 = 1024; }
    if (g_log) g_log("terrain: world vertex grid %s", ok ? "mapped" : "missing: Koei's whole stage is used, flat placeholder elsewhere");
    // china_patch.bin (tools/make_world.py): "WCPM", u32 1025, 1025x1025 bytes; only with the world grid
    if (ok) {
        swprintf_s(p, L"%s\\china_patch.bin", dir);
        FILE* f = nullptr; _wfopen_s(&f, p, L"rb");
        uint32_t h[2] = {};
        if (f && fread(h, 4, 2, f) == 2 && h[0] == 'MPCW' && h[1] == (uint32_t)N) {
            if (!g_patch) g_patch = (uint8_t*)VirtualAlloc(nullptr, (size_t)N * N, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (g_patch && fread(g_patch, 1, (size_t)N * N, f) != (size_t)N * N) { VirtualFree(g_patch, 0, MEM_RELEASE); g_patch = nullptr; }
        } else if (g_patch) { VirtualFree(g_patch, 0, MEM_RELEASE); g_patch = nullptr; }
        if (f) fclose(f);
        if (g_log) g_log("terrain: China patch %s", g_patch ? "loaded (china_patch.bin)" : "none");
    }
    return ok;
}

int terrainLoadK3st(const uint8_t* data, int size) {
    // "K3ST" "0006", 1025*1025 x {height, r, g, b, nx, ny, nz, material}, 1024*1024 quads (u64)
    const size_t need = 8 + (size_t)N * N * 8 + (size_t)QN * QN * QREC;
    if (size < (int)need || memcmp(data, "K3ST", 4) || memcmp(data + 4, "0006", 4)) {
        if (g_log) g_log("terrain: unsupported K3ST resource (%.8s, %d bytes)", (const char*)data, size);
        return 0;
    }
    if (!g_chinaV) {
        g_chinaV = (uint8_t*)VirtualAlloc(nullptr, (size_t)N * N * VREC, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_chinaQ = (uint8_t*)VirtualAlloc(nullptr, (size_t)QN * QN * QREC, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_chinaV || !g_chinaQ) return 0;
    }
    const uint8_t* s = data + 8;
    for (int i = 0; i < N * N; i++, s += 8) {
        uint8_t* v = g_chinaV + (size_t)i * VREC;
        memcpy(v, s, 8);
        v[8] = s[0];          // original height, restored when objects are removed
        v[9] = 0;
    }
    memcpy(g_chinaQ, s, (size_t)QN * QN * QREC);
    g_haveChina = true;
    DWORD t0 = GetTickCount();
    learnPalette(nullptr);
    fillVertices();
    fillQuads();
    if (g_log) g_log("terrain: world filled from K3ST (%lu ms)", GetTickCount() - t0);
    return 1;
}

int terrainLoadGcol(const uint8_t* data, int size) {
    // "GCOL" "0001", 1025*1025 RGB
    if (size < 8 + N * N * 3 || memcmp(data, "GCOL0001", 8)) return 0;
    if (!g_haveChina) return 0;
    const uint8_t* rgb = data + 8;
    learnPalette(rgb);
    for (int r = 0; r < L.VR; r++)
        for (int c = 0; c < L.VC; c++) {
            uint8_t* v = vtx(r, c);
            if (inChinaVertex(r, c)) memcpy(v + 1, rgb + ((size_t)(r - g_cvr) * N + (c - g_cvc)) * 3, 3);
            else shadeColour(v);
        }
    if (g_log) g_log("terrain: GCOL colours applied");
    return 1;
}
