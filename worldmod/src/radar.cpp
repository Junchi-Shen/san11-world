// The radar's background picture. Koei draws it from UI image 0x11: a 304x304 region of UI page 4
// (a 512x512 managed A8R8G8B8 texture shared with other UI images), hand-painted for China: one pixel
// per terrain cell (4x4 vertices), the window's centre cell at pixel (151.5, 151.5), x = row (east),
// y = column (south); a parchment map inside a circle of radius ~127, the compass frame around it.
// worldmod moves the radar window with the camera (worldmod.cpp, minimapFollow) and repaints the map
// area here: inside China it copies Koei's own pixels, elsewhere it paints Koei's parchment colour with
// fine deterministic noise, tinted with Koei's sea / river colours for water and drawn in Koei's road
// colour for roads. Everything is learnt from the original picture the first time the radar is shown.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <algorithm>
#include <vector>
#include "radar.h"

static const uint32_t UI_MGR = 0x4ed14c8;                     // 0x433a20 returns this singleton
static const int16_t* const IMAGES = (const int16_t*)0x786aa8; // 16-byte records: id, ?, ?, page, x, y, w, h
typedef IDirect3DTexture9* (__thiscall* PageTextureFn)(void* mgr, int page);   // 0x4342c0

static const float R_MAP = 126.0f;                            // map circle radius in pixels (the frame begins at ~128)

static bool g_learned = false;
static int g_w = 0, g_h = 0;
static std::vector<uint32_t> g_orig;                          // Koei's picture (for the China window)
static uint32_t g_paper = 0;                                  // Koei's plain parchment colour
static uint32_t g_sea = 0, g_river = 0, g_road = 0;
static IDirect3DTexture9* g_tex = nullptr;

static IDirect3DTexture9* radarTexture(RECT* r) {
    const int16_t* d = IMAGES + 0x11 * 8;
    IDirect3DTexture9* t = ((PageTextureFn)0x4342c0)((void*)UI_MGR, d[3]);
    r->left = d[4]; r->top = d[5]; r->right = d[4] + d[6]; r->bottom = d[5] + d[7];
    return t;
}

static int hexType(const RadarWorld& w, int lo, int hi) {
    if (lo < 0 || lo >= w.H || hi < 0 || hi >= w.W) return -1;
    return w.hex20[((size_t)lo * w.W + hi) * 20 + 4] & 0x1f;
}

// pixel (x, y) of a window whose first cell is (wlo, whi) -> hex (lo, hi) and its cell (cr, cc)
static void pixelHex(int wlo, int whi, int x, int y, int* lo, int* hi, int* cr, int* cc) {
    float fr = wlo + (x + 0.5f - g_w * 0.5f) + 128.0f, fc = whi + (y + 0.5f - g_h * 0.5f) + 128.0f;
    *cr = (int)floorf(fr); *cc = (int)floorf(fc);
    *lo = (int)floorf(fr - 28.0f);
    *hi = (int)floorf(fc - 28.0f - 0.5f * (*lo & 1));
}

static uint32_t median(std::vector<uint32_t>& v) {
    if (v.empty()) return 0;
    uint32_t out = 0xff000000;
    for (int ch = 0; ch < 3; ch++) {
        std::vector<int> c(v.size());
        for (size_t i = 0; i < v.size(); i++) c[i] = (v[i] >> (8 * ch)) & 0xff;
        std::nth_element(c.begin(), c.begin() + c.size() / 2, c.end());
        out |= (uint32_t)c[c.size() / 2] << (8 * ch);
    }
    return out;
}

static inline uint32_t blend(uint32_t a, uint32_t b, int t256) {      // t256/256 of b
    uint32_t o = 0xff000000;
    for (int ch = 0; ch < 3; ch++) {
        int x = (a >> (8 * ch)) & 0xff, y = (b >> (8 * ch)) & 0xff;
        o |= (uint32_t)((x * (256 - t256) + y * t256) >> 8) << (8 * ch);
    }
    return o;
}

static inline bool isWater(int t) { return t == 6 || t == 7 || t == 8; }
static inline bool isRoad(int t) { return t == 10 || t == 11 || t == 12; }

bool radarLearn(const RadarWorld& w, TraceLogFn log) {
    RECT r; IDirect3DTexture9* t = radarTexture(&r);
    if (!t) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, &r, D3DLOCK_READONLY))) return false;
    g_w = r.right - r.left; g_h = r.bottom - r.top;
    g_orig.resize((size_t)g_w * g_h);
    for (int y = 0; y < g_h; y++) memcpy(&g_orig[(size_t)y * g_w], (uint8_t*)lr.pBits + (size_t)y * lr.Pitch, 4 * g_w);
    t->UnlockRect(0);
    // colours of Koei's sea, rivers and roads (the picture shows the China window)
    std::vector<uint32_t> sea, river, road, paper;
    for (int y = 0; y < g_h; y++) for (int x = 0; x < g_w; x++) {
        if (hypotf(x + 0.5f - g_w * 0.5f, y + 0.5f - g_h * 0.5f) > R_MAP - 6) continue;
        int lo, hi, cr, cc; pixelHex(w.chinaLo, w.chinaHi, x, y, &lo, &hi, &cr, &cc);
        if (lo < w.chinaLo || lo >= w.chinaLo + 200 || hi < w.chinaHi || hi >= w.chinaHi + 200) continue;
        uint32_t p = g_orig[(size_t)y * g_w + x];
        int tt = hexType(w, lo, hi);
        int R = (p >> 16) & 0xff, G = (p >> 8) & 0xff, B = p & 0xff;
        if (tt == 8) sea.push_back(p);
        else if (tt == 7 && B > R + 8) river.push_back(p);
        else if (isRoad(tt) && R > B + 20) road.push_back(p);
        else if (!isWater(tt) && !isRoad(tt)) paper.push_back(p);
    }
    g_sea = median(sea); g_river = river.empty() ? g_sea : median(river); g_road = median(road);
    g_paper = paper.empty() ? 0xff9aa08a : median(paper);
    g_tex = t;
    g_learned = true;
    if (log) log("radar: learnt Koei's picture (%dx%d): sea %06x (%zu px), river %06x (%zu), road %06x (%zu), paper %06x (%zu px)",
                 g_w, g_h, g_sea & 0xffffff, sea.size(), g_river & 0xffffff, river.size(), g_road & 0xffffff, road.size(), g_paper & 0xffffff, paper.size());
    return true;
}

// deterministic value noise, anchored to world cells: no tiling period, so the parchment never repeats
static inline uint32_t hashU(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

static inline float lattice(int x, int y, uint32_t seed) {
    return (hashU((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ seed) & 0xffff) * (1.0f / 65535.0f);
}

static float valueNoise(float x, float y, uint32_t seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3.0f - 2.0f * fx); fy = fy * fy * (3.0f - 2.0f * fy);
    float a = lattice(xi, yi, seed), b = lattice(xi + 1, yi, seed);
    float c = lattice(xi, yi + 1, seed), d = lattice(xi + 1, yi + 1, seed);
    float ab = a + (b - a) * fx, cd = c + (d - c) * fx;
    return ab + (cd - ab) * fy;
}

static inline uint32_t shade(uint32_t p, int k) {
    int r = ((p >> 16) & 0xff) + k, g = ((p >> 8) & 0xff) + k, b = (p & 0xff) + k;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static inline uint32_t parchment(int cr, int cc) {
    float n = 0.6f * valueNoise(cr * 0.125f, cc * 0.125f, 0x9e3779b9u)
            + 0.4f * valueNoise(cr * 0.5f, cc * 0.5f, 0x85ebca6bu);
    return shade(g_paper, (int)lroundf((n - 0.5f) * 20.0f));
}

bool radarPaint(const RadarWorld& w, int wlo, int whi) {
    if (!g_learned) return false;
    RECT r; IDirect3DTexture9* t = radarTexture(&r);
    if (!t || r.right - r.left != g_w || r.bottom - r.top != g_h) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, &r, 0))) return false;
    const int ox = wlo - w.chinaLo, oy = whi - w.chinaHi;        // this window relative to Koei's picture
    const bool china = ox == 0 && oy == 0;
    for (int y = 0; y < g_h; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)lr.pBits + (size_t)y * lr.Pitch);
        for (int x = 0; x < g_w; x++) {
            float d = hypotf(x + 0.5f - g_w * 0.5f, y + 0.5f - g_h * 0.5f);
            if (china || d >= R_MAP) { row[x] = g_orig[(size_t)y * g_w + x]; continue; }   // frame, or Koei's window
            int lo, hi, cr, cc; pixelHex(wlo, whi, x, y, &lo, &hi, &cr, &cc);
            // Koei's own pixels for China (where his picture covers it)
            int sx = x + ox, sy = y + oy;
            if (lo >= w.chinaLo && lo < w.chinaLo + 200 && hi >= w.chinaHi && hi < w.chinaHi + 200 &&
                sx >= 0 && sx < g_w && sy >= 0 && sy < g_h && hypotf(sx + 0.5f - g_w * 0.5f, sy + 0.5f - g_h * 0.5f) < R_MAP - 1) {
                row[x] = g_orig[(size_t)sy * g_w + sx];
                continue;
            }
            uint32_t p = parchment(cr, cc);
            int tt = hexType(w, lo, hi);
            if (tt < 0) p = blend(p, 0xff606058, 96);                         // beyond the world's edge
            else if (tt == 8) p = blend(p, g_sea, 200);
            else if (tt == 6 || tt == 7) p = blend(p, g_river, 170);
            else if (isRoad(tt)) p = blend(p, g_road, 200);
            row[x] = p;
        }
    }
    t->UnlockRect(0);
    return true;
}

int radarDump(const char* path) {
    RECT r; IDirect3DTexture9* t = radarTexture(&r);
    if (!t) return -1;
    D3DSURFACE_DESC desc; t->GetLevelDesc(0, &desc);
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, &r, D3DLOCK_READONLY))) return -2;
    FILE* f = nullptr; fopen_s(&f, path, "wb");
    if (f) {
        int w = r.right - r.left, h = r.bottom - r.top;
        uint32_t hdr[6] = { 'DRAR', (uint32_t)w, (uint32_t)h, (uint32_t)desc.Format, (uint32_t)desc.Pool, (uint32_t)(uintptr_t)t };
        fwrite(hdr, 4, 6, f);
        for (int y = 0; y < h; y++) fwrite((uint8_t*)lr.pBits + (size_t)y * lr.Pitch, 4, w, f);
        fclose(f);
    }
    t->UnlockRect(0);
    return f ? 0 : -3;
}
