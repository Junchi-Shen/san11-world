// d3dtrace: optional Direct3D 9 call tracer used to find which game code builds and draws the terrain.
// Hooks IDirect3D9::CreateDevice and a few IDirect3DDevice9 methods by patching their vtables,
// aggregates calls by (method, game call sites) and periodically writes worldmod_trace_<section>.log.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <stdio.h>
#include <stdint.h>
#include <map>
#include <tuple>
#include <vector>
#include <algorithm>
#include "d3dtrace.h"
#include "automation.h"

namespace {

const uint32_t CODE_LO = 0x401000, CODE_HI = 0x74f000;   // san11pk.exe .text

struct Key {
    int method; uint32_t c[3];
    bool operator<(const Key& o) const { return std::tie(method, c[0], c[1], c[2]) < std::tie(o.method, o.c[0], o.c[1], o.c[2]); }
};
struct Stat { uint64_t calls = 0, prims = 0; uint32_t a = 0, b = 0, c = 0, d = 0; };

CRITICAL_SECTION g_cs;
std::map<Key, Stat> g_stats;
wchar_t g_path[MAX_PATH];
uint64_t g_frames = 0;
TraceLogFn g_tlog = nullptr;
bool g_recording = false;      // false: hooks only serve automation screenshots
uint32_t g_curTexWh = 0;       // 当前 stage 0 纹理 (w<<16|h)，由 hSetTexture 维护

const char* kNames[] = { "CreateTexture", "CreateVertexBuffer", "CreateIndexBuffer", "SetTransform",
                         "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "SetTexture" };
enum { M_TEX, M_VB, M_IB, M_XFORM, M_DP, M_DIP, M_DPUP, M_DIPUP, M_SETTEX };

bool looksLikeReturn(uint32_t v) {
    if (v < CODE_LO + 8 || v >= CODE_HI) return false;
    const uint8_t* p = (const uint8_t*)(uintptr_t)v;
    return p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) ||
           (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10);
}

// Collects up to three plausible game return addresses above `frame`.
void callers(void* frame, uint32_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    uint32_t* sp = (uint32_t*)frame;
    for (int i = 0, n = 0; i < 96 && n < 3; i++) {
        uint32_t v = sp[i];
        if (looksLikeReturn(v)) out[n++] = v;
    }
}

void record(int method, void* frame, uint32_t prims, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    if (!g_recording) return;
    Key k{ method, {} };
    callers(frame, k.c);
    if (method == M_SETTEX) k.c[2] = d;            // one entry per call site and texture
    EnterCriticalSection(&g_cs);
    Stat& s = g_stats[k];
    s.calls++; s.prims += prims; s.a = a; s.b = b; s.c = c; s.d = d;
    LeaveCriticalSection(&g_cs);
}

void dump() {
    EnterCriticalSection(&g_cs);
    std::vector<std::pair<Key, Stat>> v(g_stats.begin(), g_stats.end());
    LeaveCriticalSection(&g_cs);
    std::sort(v.begin(), v.end(), [](auto& x, auto& y) {
        return x.first.method != y.first.method ? x.first.method < y.first.method : x.second.calls > y.second.calls; });
    FILE* f = _wfsopen(g_path, L"w", 0x40 /*_SH_DENYNO*/);
    if (!f) { if (g_tlog) g_tlog("trace: cannot write trace file"); return; }
    fprintf(f, "frames=%llu  (a,b,c,d = last call parameters, see d3dtrace.cpp)\n", g_frames);
    for (auto& [k, s] : v)
        fprintf(f, "%-24s calls=%-9llu prims=%-10llu callers=%08x %08x %08x  a=%u b=%u c=%u d=%u\n",
                kNames[k.method], s.calls, s.prims, k.c[0], k.c[1], k.c[2], s.a, s.b, s.c, s.d);
    fclose(f);
}

// ---- hooked methods (vtable order from d3d9.h)
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
typedef HRESULT(STDMETHODCALLTYPE* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateVB_t)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateIB_t)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* SetTransform_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
typedef HRESULT(STDMETHODCALLTYPE* DP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DIP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DPUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DIPUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(STDMETHODCALLTYPE* SetPSCF_t)(IDirect3DDevice9*, UINT, const float*, UINT);

CreateDevice_t oCreateDevice; EndScene_t oEndScene; CreateTexture_t oCreateTexture; CreateVB_t oCreateVB; CreateIB_t oCreateIB;
SetTransform_t oSetTransform; DP_t oDP; DIP_t oDIP; DPUP_t oDPUP; DIPUP_t oDIPUP; SetPSCF_t oSetPSCF;
typedef HRESULT(STDMETHODCALLTYPE* SetRS_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
SetRS_t oSetRS;
HRESULT STDMETHODCALLTYPE hSetRS(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v) {
    if (g_tlog && (st == 139 || st == 34 || st == 36 || st == 37)) {
        static int n = 0;
        if (n++ < 60) g_tlog("SetRS state=%d value=%08x", (int)st, v);
    }
    return oSetRS(d, st, v);
}
typedef HRESULT(STDMETHODCALLTYPE* CreatePS_t)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
CreatePS_t oCreatePS;
HRESULT STDMETHODCALLTYPE hCreatePS(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** ps) {
    if (g_tlog && fn) {
        static int n = 0;
        if (n++ < 6) {
            g_tlog("CreatePS @%p:", fn);
            for (int k = 0; k < 32; k++) g_tlog("  [%02d] %08x", k, fn[k]);
        }
    }
    return oCreatePS(d, fn, ps);
}
typedef HRESULT(STDMETHODCALLTYPE* Clear_t)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
Clear_t oClear;
HRESULT STDMETHODCALLTYPE hClear(IDirect3DDevice9* d, DWORD cnt, const D3DRECT* r, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    if (g_tlog) {
        static int n = 0; static D3DCOLOR last = 0xffffffff;
        if (n++ < 30 || color != last) { g_tlog("Clear color=%08x flags=%x", color, flags); last = color; }
    }
    return oClear(d, cnt, r, flags, color, z, stencil);
}
typedef HRESULT(STDMETHODCALLTYPE* SetVSCF_t)(IDirect3DDevice9*, UINT, const float*, UINT);
SetVSCF_t oSetVSCF;
HRESULT STDMETHODCALLTYPE hSetVSCF(IDirect3DDevice9* d, UINT reg, const float* data, UINT count) {
    if (g_tlog && data && (reg == 24 || reg == 26 || reg == 42) && count >= 1) {
        static int n = 0;
        if (n++ < 60) g_tlog("SetVSCF c%u = (%g, %g, %g, %g)", reg, data[0], data[1], data[2], data[3]);
    }
    return oSetVSCF(d, reg, data, count);
}

#define FRAME() _AddressOfReturnAddress()

// The game presents through a swap chain, so frames are counted at EndScene and a thread writes the file.
HRESULT STDMETHODCALLTYPE hEndScene(IDirect3DDevice9* d) {
    g_frames++;
    HRESULT hr = oEndScene(d);
    automationOnEndScene(d);
    return hr;
}

DWORD WINAPI dumpThread(void*) {
    if (g_tlog) g_tlog("trace: dump thread running");
    Sleep(3000);
    for (;;) { dump(); if (g_tlog) g_tlog("trace: dumped (%llu frames, %zu call sites)", g_frames, g_stats.size()); Sleep(10000); }
}
SetTexture_t oSetTexture;
HRESULT STDMETHODCALLTYPE hSetPixelShaderConstantF(IDirect3DDevice9* d, UINT reg, const float* data, UINT count) {
    // 打印常量，找雾色 0.69（灰白 176/255）—— 只打包含 0.4~1.0 分量的寄存器
    if (g_tlog && data && count <= 8) {
        for (UINT i = 0; i < count; i++) {
            float a = data[i * 4], b = data[i * 4 + 1], c = data[i * 4 + 2], e = data[i * 4 + 3];
            if ((a > 0.4f && a < 1.0f) || (b > 0.4f && b < 1.0f) || (c > 0.4f && c < 1.0f) || (e > 0.4f && e < 1.0f)) {
                if (g_tlog) g_tlog("psc[%u] = (%.4f, %.4f, %.4f, %.4f)", reg + i, a, b, c, e);
            }
        }
    }
    return oSetPSCF(d, reg, data, count);
}
HRESULT STDMETHODCALLTYPE hCreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* s) {
    HRESULT hr = oCreateTexture(d, w, h, l, u, f, p, t, s);
    // d = the new texture (pointer), to match it with SetTexture records
    record(M_TEX, FRAME(), 0, w, h, ((UINT)f << 8) | (UINT)p, SUCCEEDED(hr) && t ? (UINT)(uintptr_t)*t : 0);
    return hr;
}
static int g_grayLog = 0;
HRESULT STDMETHODCALLTYPE hSetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* tex) {
    UINT wh = 0, fmt = 0;
    if (tex && tex->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(((IDirect3DTexture9*)tex)->GetLevelDesc(0, &desc))) { wh = (desc.Width << 16) | desc.Height; fmt = desc.Format; }
        // 检测灰白云海纹理（雾候选）：中心/四角像素接近中性灰白
        if (stage == 0 && g_grayLog < 40 && desc.Width >= 128 && desc.Height >= 64 &&
            (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8)) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(((IDirect3DTexture9*)tex)->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
                int W = (int)desc.Width, H = (int)desc.Height, pit = (int)lr.Pitch;
                uint8_t* base = (uint8_t*)lr.pBits;
                int xs[5] = { W/2, 0, W-1, W/2, W/2 }, ys[5] = { H/2, H/2, H/2, 0, H-1 };
                bool gray = true; int r = 0, g = 0, b = 0;
                for (int k = 0; k < 5; k++) {
                    uint8_t* p = base + ys[k] * pit + xs[k] * 4;
                    b = p[0]; g = p[1]; r = p[2];           // A8R8G8B8: BGRA
                    int d1 = r > g ? r - g : g - r, d2 = g > b ? g - b : b - g;
                    if (d1 > 24 || d2 > 24 || r < 110 || r > 230) { gray = false; break; }
                }
                if (gray) {
                    uint32_t c[3]; callers(FRAME(), c);
                    g_grayLog++;
                    g_tlog("GRAY TEX %ux%u px=(%d,%d,%d) caller=%08x %08x %08x", W, H, r, g, b, c[0], c[1], c[2]);
                }
                ((IDirect3DTexture9*)tex)->UnlockRect(0);
            }
        }
    }
    if (stage == 0) g_curTexWh = wh;
    record(M_SETTEX, FRAME(), 0, stage, wh, fmt, (UINT)(uintptr_t)tex);
    return oSetTexture(d, stage, tex);
}
HRESULT STDMETHODCALLTYPE hCreateVB(IDirect3DDevice9* d, UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** vb, HANDLE* s) {
    record(M_VB, FRAME(), 0, len, u, fvf, (UINT)p); return oCreateVB(d, len, u, fvf, p, vb, s);
}
HRESULT STDMETHODCALLTYPE hCreateIB(IDirect3DDevice9* d, UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** ib, HANDLE* s) {
    record(M_IB, FRAME(), 0, len, u, (UINT)f, (UINT)p); return oCreateIB(d, len, u, f, p, ib, s);
}
HRESULT STDMETHODCALLTYPE hSetTransform(IDirect3DDevice9* d, D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m) {
    // 投影矩阵：打印关键元素（正交投影 _11=2/w, _22=2/h, _33=1/far, _41/_42=中心偏移）
    if (g_tlog && st == D3DTS_PROJECTION && m) {
        static int n = 0;
        if (n++ < 12) g_tlog("PROJ _11=%g _22=%g _33=%g _41=%g _42=%g _43=%g",
                             m->_11, m->_22, m->_33, m->_41, m->_42, m->_43);
    }
    // c,d = world-space translation (x,z) for world matrices, truncated to integers
    record(M_XFORM, FRAME(), 0, (UINT)st, 0, m ? (UINT)(INT)m->_41 : 0, m ? (UINT)(INT)m->_43 : 0);
    return oSetTransform(d, st, m);
}
HRESULT STDMETHODCALLTYPE hDP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) {
    record(M_DP, FRAME(), pc, (UINT)t, sv, pc); return oDP(d, t, sv, pc);
}
HRESULT STDMETHODCALLTYPE hDIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc) {
    if (g_tlog && nv > 5000) {
        uint32_t c[3]; callers(FRAME(), c);
        g_tlog("BIG DIP: nv=%u prim=%u type=%u texWh=%08x (w=%u h=%u) caller=%08x %08x %08x",
               nv, pc, (UINT)t, g_curTexWh, g_curTexWh >> 16, g_curTexWh & 0xffff, c[0], c[1], c[2]);
    }
    record(M_DIP, FRAME(), pc, (UINT)t, nv, si, (UINT)bv); return oDIP(d, t, bv, mi, nv, si, pc);
}
HRESULT STDMETHODCALLTYPE hDPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT pc, const void* v, UINT stride) {
    record(M_DPUP, FRAME(), pc, (UINT)t, pc, stride); return oDPUP(d, t, pc, v, stride);
}
HRESULT STDMETHODCALLTYPE hDIPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void* i, D3DFORMAT f, const void* v, UINT stride) {
    record(M_DIPUP, FRAME(), pc, (UINT)t, nv, pc, stride); return oDIPUP(d, t, mi, nv, pc, i, f, v, stride);
}

template <class T> void patchSlot(void** vtbl, int index, T hook, T* orig) {
    if (vtbl[index] == (void*)hook) return;
    *orig = (T)vtbl[index];
    DWORD old; VirtualProtect(&vtbl[index], 4, PAGE_READWRITE, &old);
    vtbl[index] = (void*)hook;
    VirtualProtect(&vtbl[index], 4, old, &old);
}

HRESULT STDMETHODCALLTYPE hCreateDevice(IDirect3D9* self, UINT ad, D3DDEVTYPE ty, HWND w, DWORD fl, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr = oCreateDevice(self, ad, ty, w, fl, pp, out);
    if (g_tlog) g_tlog("trace: CreateDevice type=%d flags=%#lx hr=%#lx device=%p", (int)ty, fl, hr, out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) {
        void** v = *(void***)*out;
        if (g_tlog) g_tlog("vtable: [57]=%p [65]=%p [74]=%p [75]=%p [88]=%p [89]=%p [94]=%p",
                           v[57], v[65], v[74], v[75], v[88], v[89], v[94]);
        patchSlot(v, 42, (EndScene_t)hEndScene, &oEndScene);
        patchSlot(v, 43, (Clear_t)hClear, &oClear);
        patchSlot(v, 23, (CreateTexture_t)hCreateTexture, &oCreateTexture);
        patchSlot(v, 26, (CreateVB_t)hCreateVB, &oCreateVB);
        patchSlot(v, 27, (CreateIB_t)hCreateIB, &oCreateIB);
        patchSlot(v, 44, (SetTransform_t)hSetTransform, &oSetTransform);
        patchSlot(v, 81, (DP_t)hDP, &oDP);
        patchSlot(v, 82, (DIP_t)hDIP, &oDIP);
        patchSlot(v, 83, (DPUP_t)hDPUP, &oDPUP);
        patchSlot(v, 84, (DIPUP_t)hDIPUP, &oDIPUP);
        patchSlot(v, 65, (SetTexture_t)hSetTexture, &oSetTexture);
        patchSlot(v, 57, (SetRS_t)hSetRS, &oSetRS);
        patchSlot(v, 94, (SetVSCF_t)hSetVSCF, &oSetVSCF);
        patchSlot(v, 76, (CreatePS_t)hCreatePS, &oCreatePS);
        if (g_tlog) g_tlog("trace: device vtable %p hooked", v);
        static bool started = false;
        if (g_recording && !started) { started = true; HANDLE t = CreateThread(nullptr, 256 << 10, dumpThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr); if (g_tlog) g_tlog("trace: dump thread %p", t); }
    }
    return hr;
}

}  // namespace

void traceInit(const wchar_t* dir, const wchar_t* section, TraceLogFn log, bool recordCalls) {
    g_tlog = log;
    g_recording = recordCalls;
    InitializeCriticalSection(&g_cs);
    swprintf_s(g_path, L"%s\\worldmod_trace_%s.log", dir, section);
}

void* traceWrapDirect3D9(void* d3d) {
    if (d3d) patchSlot(*(void***)d3d, 16, (CreateDevice_t)hCreateDevice, &oCreateDevice);
    if (g_tlog) g_tlog("trace: Direct3DCreate9 -> %p, CreateDevice hooked", d3d);
    return d3d;
}
