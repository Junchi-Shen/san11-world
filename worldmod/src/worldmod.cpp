// worldmod: d3d9.dll proxy that enlarges the San11PK hex map.
// Build: build.bat (MSVC x86). Install: copy d3d9.dll + worldmod.ini + worldmod_patches.txt next to san11pk.exe.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <share.h>
#include <shlobj.h>
#include <vector>
#include <intrin.h>
#include "d3dtrace.h"
#include "automation.h"
#include "profiler.h"
#include "pathfind.h"
#include "radar.h"
#include "terrainworld.h"
#include "bases.h"
#include "midmap.h"
#include "forces.h"
#include "units.h"

// ---------------------------------------------------------------- logging
static FILE* g_log = nullptr;
static wchar_t g_dir[MAX_PATH];

static void logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- config
struct Config {
    int W = 200, H = 200;        // world size in hexes
    int C = 42;                  // number of cities (Koei: 42); gates G=10 and ports P=35 follow, N = C+G+P bases
    int bases = 0;               // 1 = base-count patch set (worldmod_bases*.txt, bases.cpp): cities = C
    int R = 42;                  // regular forces = regular corps (forces.cpp, M8_FORCES.md); forces = corps = R + 5
    int U = 1000;                // units (部队) on the map (units.cpp, M9_UNITS.md); locations N..N+U-1 are units
    int X0 = 0, Y0 = 0;          // where the original 200x200 China map sits in the world (Y0 must be even)
    int enable = 1;              // 0 = proxy only, no patches
    int verifyOnly = 0;          // 1 = check patch table against exe bytes, write nothing
    int trace = 0;               // 1 = log Direct3D calls by game call site (worldmod_trace_<section>.log)
    int automation = 0;          // 1 = test-automation channel (automation.cpp)
    int autoSkip = 0;            // 1 = click through the opening events and dialogs when a map loads (automation.cpp)
    int worldTerrain = 0;        // 1 = whole-world 3D terrain (terrainworld.cpp, worldmod_terrain*.txt)
    wchar_t dataDir[MAX_PATH] = L"";   // world_shex.bin / world_height.npy / world_mat.npy / world_palette.bin
    char saveDir[MAX_PATH] = "";       // save_dir=: the game's Documents folder (saves, settings) becomes <game>\<save_dir>
    int fastClear = 1;           // 1 = clear only dirty HEX28 pages in the movement-range search
    uint32_t watch = 0;          // debug: hardware write watchpoint on this address (main thread), hits are logged
    int watchObjList = 0;        // debug: watch the object manager's list heads and validate its node lists
    int objListLock = 1;         // serialize the object manager's list operations (loader thread vs main thread)
    int modelGuard = 1;          // skip the unit model rebuild callback for units without a model (0x5a03e0)
    int getterCheck = 0;         // debug: log gate / port getter calls with an out-of-range index and their call sites
    int unitFirst = 0;           // debug (M9 tests): new units take the first free id from this one on
    int astarStats = 0;          // debug: time every A* call (0x567520) and log the slow ones
    int astarEnter = 0;          // debug: log every A* call as it starts (the last one before a freeze is stuck)
    int astarMode = 3;           // A*: 0 original, 1 binary heap (same results), 2 + 2x heuristic, 3 + search window
    int astarMargin = 100;       // A* mode 3: search window margin in hexes
    int astarBenchSlow = 0;      // debug: replay slow A* calls with every variant
    uint32_t astarBenchGoal = 0; // debug: "lo,hi" -> time every A* variant to this hex at calls 1, 20, 100, 300
    int astarVerify = 0;         // debug: run the original A* too and compare the results
    int heapCheck = 0;           // debug: validate all heaps every 20 ms; log where the main thread is when one breaks
    int watchRead = 0;           // debug: 1 = also break on reads; distinct code addresses are logged once each
    int watchLo = -1, watchHi = -1, watchMirror = 1;   // debug: watch HEX20 dword+4 of this hex; 0 = no area mirror
} g_cfg;

static wchar_t g_section[64] = L"map";   // [<exe name without extension>], so each launcher exe has its own map settings

static void loadConfig() {
    wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%s\\worldmod.ini", g_dir);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\'); name = name ? name + 1 : exe;
    wcsncpy_s(g_section, name, _TRUNCATE);
    if (wchar_t* dot = wcsrchr(g_section, L'.')) *dot = 0;
    g_cfg.W = GetPrivateProfileIntW(g_section, L"width", 200, ini);
    g_cfg.H = GetPrivateProfileIntW(g_section, L"height", 200, ini);
    g_cfg.C = GetPrivateProfileIntW(g_section, L"cities", 42, ini);
    g_cfg.bases = GetPrivateProfileIntW(g_section, L"bases", 0, ini);
    g_cfg.R = GetPrivateProfileIntW(g_section, L"forces", 42, ini);
    g_cfg.U = GetPrivateProfileIntW(g_section, L"units", 1000, ini);
    g_cfg.X0 = GetPrivateProfileIntW(g_section, L"china_x", 0, ini);
    g_cfg.Y0 = GetPrivateProfileIntW(g_section, L"china_y", 0, ini);
    g_cfg.enable = GetPrivateProfileIntW(L"mod", L"enable", 1, ini);
    g_cfg.verifyOnly = GetPrivateProfileIntW(L"mod", L"verify_only", 0, ini);
    g_cfg.trace = GetPrivateProfileIntW(g_section, L"trace", 0, ini);
    g_cfg.fastClear = GetPrivateProfileIntW(g_section, L"fast_clear", 1, ini);
    wchar_t w[32] = L"";
    GetPrivateProfileStringW(g_section, L"watch", L"", w, 32, ini);
    g_cfg.watch = (uint32_t)wcstoul(w, nullptr, 16);
    g_cfg.watchRead = GetPrivateProfileIntW(g_section, L"watch_read", 0, ini);
    g_cfg.heapCheck = GetPrivateProfileIntW(g_section, L"heap_check", 0, ini);
    g_cfg.astarStats = GetPrivateProfileIntW(g_section, L"astar_stats", 0, ini);
    g_cfg.astarEnter = GetPrivateProfileIntW(g_section, L"astar_enter", 0, ini);
    g_cfg.astarMode = GetPrivateProfileIntW(g_section, L"astar", 3, ini);
    g_cfg.astarMargin = GetPrivateProfileIntW(g_section, L"astar_margin", 100, ini);
    g_cfg.astarBenchSlow = GetPrivateProfileIntW(g_section, L"astar_bench_slow", 0, ini);
    {
        wchar_t bg[32] = L""; int blo = -1, bhi = -1;
        GetPrivateProfileStringW(g_section, L"astar_bench_goal", L"", bg, 32, ini);
        if (swscanf_s(bg, L"%d,%d", &blo, &bhi) == 2 && blo >= 0 && bhi >= 0)
            g_cfg.astarBenchGoal = ((uint32_t)(uint16_t)blo | ((uint32_t)(uint16_t)bhi << 16)) + 1;
    }
    g_cfg.astarVerify = GetPrivateProfileIntW(g_section, L"astar_verify", 0, ini);
    g_cfg.watchObjList = GetPrivateProfileIntW(g_section, L"watch_objlist", 0, ini);
    g_cfg.objListLock = GetPrivateProfileIntW(g_section, L"objlist_lock", 1, ini);
    g_cfg.modelGuard = GetPrivateProfileIntW(g_section, L"model_guard", 1, ini);
    g_cfg.getterCheck = GetPrivateProfileIntW(g_section, L"getter_check", 0, ini);
    g_cfg.unitFirst = GetPrivateProfileIntW(g_section, L"unit_first", 0, ini);
    g_cfg.watchLo = GetPrivateProfileIntW(g_section, L"watch_lo", -1, ini);
    g_cfg.watchHi = GetPrivateProfileIntW(g_section, L"watch_hi", -1, ini);
    g_cfg.watchMirror = GetPrivateProfileIntW(g_section, L"area_mirror", 1, ini);
    g_cfg.automation = GetPrivateProfileIntW(g_section, L"automation", 0, ini);
    {
        wchar_t sd[MAX_PATH] = L"";
        GetPrivateProfileStringW(g_section, L"save_dir", g_cfg.automation ? L"UserData_test" : L"", sd, MAX_PATH, ini);
        WideCharToMultiByte(CP_ACP, 0, sd, -1, g_cfg.saveDir, MAX_PATH, nullptr, nullptr);
    }
    g_cfg.autoSkip = GetPrivateProfileIntW(g_section, L"auto_skip", 0, ini);
    g_cfg.worldTerrain = GetPrivateProfileIntW(g_section, L"world_terrain", 0, ini);
    wchar_t sub[MAX_PATH] = L"";
    GetPrivateProfileStringW(g_section, L"data_dir", L"", sub, MAX_PATH, ini);
    if (sub[0]) swprintf_s(g_cfg.dataDir, L"%s\\%s", g_dir, sub); else wcscpy_s(g_cfg.dataDir, g_dir);
}

static bool configValid() {
    const Config& c = g_cfg;
    // unrolled loops in the exe step 5 rows/columns at a time and clear W*H/4 dwords
    if (c.W < 200 || c.H < 200 || c.W % 200 || c.H % 200) return false;
    if (c.W > 8000 || c.H > 8000) return false;
    if (c.X0 < 0 || c.Y0 < 0 || c.X0 + 200 > c.W || c.Y0 + 200 > c.H || (c.Y0 & 1)) return false;
    return true;
}

// ---------------------------------------------------------------- world memory
struct World {
    uint8_t* hex20 = nullptr;  // W*H*20 (replaces 0x6fb0e68)
    uint8_t* hex28 = nullptr;  // W*H*28 (replaces 0x9438128)
} g_world;

// Whole-world terrain arrays (M4B.md); their bases and dimensions are patch-expression symbols.
static TerrainLayout g_terrain;
// Camera clamp limits (world units), read by the patched 0x5714a0 through FA_* symbols. The original
// shares one float for x and z; the world is not square.
static float g_faXmax, g_faZmax, g_faXmax16, g_faZmax16;

static const uint32_t ORIG_HEX20 = 0x6fb0e68, ORIG_HEX20_END = 0x7074368;
static const uint32_t ORIG_HEX28 = 0x9438128, ORIG_HEX28_END = 0x9549828;

// ---------------------------------------------------------------- expression evaluator
// Grammar: expr := term (('+'|'-') term)* ; term := factor (('*'|'/') factor)* ;
// factor := number | name | '(' expr ')' | '-' factor
struct Eval {
    const char* p; bool ok = true;
    int64_t var(const char* name, size_t n) {
        auto is = [&](const char* s) { return strlen(s) == n && !strncmp(s, name, n); };
        int64_t W = g_cfg.W, H = g_cfg.H;
        if (is("W")) return W;
        if (is("H")) return H;
        if (is("M")) return W > H ? W : H;
        if (is("C")) return g_cfg.C;
        if (is("G")) return 10;
        if (is("P")) return 35;
        if (is("N")) return g_cfg.C + 45;
        if (is("R")) return g_cfg.R;
        if (is("U")) return g_cfg.U;
        if (is("B20")) return (int64_t)(uintptr_t)g_world.hex20;
        if (is("E20")) return (int64_t)(uintptr_t)g_world.hex20 + W * H * 20;
        if (is("B28")) return (int64_t)(uintptr_t)g_world.hex28;
        if (is("E28")) return (int64_t)(uintptr_t)g_world.hex28 + W * H * 28;
        for (int i = 0; i < s_nsyms; i++) if (is(s_syms[i].name)) return (int64_t)s_syms[i].value;
        ok = false; return 0;
    }
    // worldmod symbols (addresses of DLL variables / helper functions) usable in patch and cave expressions
    struct Sym { const char* name; uintptr_t value; };
    static Sym s_syms[256];
    static int s_nsyms;
    static void add(const char* name, const void* addr) { s_syms[s_nsyms++] = { name, (uintptr_t)addr }; }
    void ws() { while (*p == ' ') p++; }
    int64_t factor() {
        ws();
        if (*p == '(') { p++; int64_t v = expr(); ws(); if (*p == ')') p++; else ok = false; return v; }
        if (*p == '-') { p++; return -factor(); }
        if (*p >= '0' && *p <= '9') { char* e; int64_t v = _strtoi64(p, &e, 0); p = e; return v; }
        const char* s = p; while ((*p >= 'A' && *p <= 'Z') || *p == '_' || (*p >= '0' && *p <= '9' && p != s)) p++;
        if (p == s) { ok = false; return 0; }
        size_t n = p - s;
        if (*p == '(') {                       // helper functions: A4/A8/A16 (align up), R3 (round up to 3), MAX, MIN
            p++;
            int64_t a[4] = {}; int k = 0;
            for (;;) { if (k < 4) a[k++] = expr(); ws(); if (*p == ',') { p++; continue; } if (*p == ')') { p++; break; } ok = false; return 0; }
            auto fn = [&](const char* f) { return strlen(f) == n && !strncmp(f, s, n); };
            if (fn("A4")) return (a[0] + 3) & ~(int64_t)3;
            if (fn("A8")) return (a[0] + 7) & ~(int64_t)7;
            if (fn("A16")) return (a[0] + 15) & ~(int64_t)15;
            if (fn("R3")) return (a[0] + 2) / 3 * 3;
            if (fn("MAX")) { int64_t m = a[0]; for (int i = 1; i < k; i++) if (a[i] > m) m = a[i]; return m; }
            if (fn("MIN")) { int64_t m = a[0]; for (int i = 1; i < k; i++) if (a[i] < m) m = a[i]; return m; }
            ok = false; return 0;
        }
        return var(s, n);
    }
    int64_t term() {
        int64_t v = factor();
        for (;;) {
            ws();
            if (*p == '*') { p++; v *= factor(); }
            else if (*p == '/') { p += p[1] == '/' ? 2 : 1; int64_t d = factor(); if (d) v /= d; else ok = false; }   // '/' and '//'

            else return v;
        }
    }
    int64_t sum() { int64_t v = term(); for (;;) { ws(); if (*p == '+') { p++; v += term(); } else if (*p == '-') { p++; v -= term(); } else return v; } }
    int64_t expr() {                           // lowest precedence: << and >>
        int64_t v = sum();
        for (;;) {
            ws();
            if (p[0] == '<' && p[1] == '<') { p += 2; v <<= sum(); }
            else if (p[0] == '>' && p[1] == '>') { p += 2; v >>= sum(); }
            else return v;
        }
    }
};
Eval::Sym Eval::s_syms[256];
int Eval::s_nsyms = 0;

static bool bytesMatch(const uint8_t* at, const char* hex) {
    for (size_t i = 0, len = strlen(hex) / 2; i < len; i++) {
        unsigned b; sscanf_s(hex + 2 * i, "%2x", &b);
        if (at[i] != (uint8_t)b) return false;
    }
    return true;
}

// ---------------------------------------------------------------- patch table
// Text lines: VA  OFF  SIZE  OLDBYTES(hex, whole instruction)  EXPR  [; comment]
//   VA    instruction address (hex)
//   OFF   byte offset of the immediate/displacement inside the instruction
//   SIZE  1, 2 or 4
//   EXPR  new value of that field
struct Patch { uint8_t* at; unsigned size; int64_t value; };
static Patch g_patches[16384];
static int g_npatches = 0;

// Parses and verifies the patch table against the untouched exe; nothing is written here.
static int loadPatches(const wchar_t* file) {
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\%s", g_dir, file);
    FILE* f = nullptr; _wfopen_s(&f, path, L"r");
    if (!f) { logf("ERROR: cannot open %ls", file); return 1; }
    Patch* patches = g_patches;
    char line[1024]; int n = g_npatches, n0 = g_npatches, bad = 0, lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char* c = strchr(line, ';'); if (c) *c = 0;
        char va_s[32], hex[128], ex[256]; unsigned off, size;
        if (sscanf_s(line, "%31s %u %u %127s %255[^\r\n]", va_s, (unsigned)sizeof va_s, &off, &size, hex, (unsigned)sizeof hex, ex, (unsigned)sizeof ex) != 5) continue;
        uint8_t* va = (uint8_t*)(uintptr_t)strtoul(va_s, nullptr, 16);
        if (!bytesMatch(va, hex)) { logf("MISMATCH %ls line %d at %p: exe bytes differ from table", file, lineno, va); bad++; continue; }
        // "f:EXPR" stores the (integer) expression as a 32-bit float, e.g. camera bounds in world units
        bool asFloat = !strncmp(ex, "f:", 2);
        Eval e{ asFloat ? ex + 2 : ex }; int64_t v = e.expr();
        if (asFloat) { float fv = (float)v; uint32_t bits; memcpy(&bits, &fv, 4); v = bits; }
        if (!e.ok) { logf("BAD EXPR %ls line %d: %s", file, lineno, ex); bad++; continue; }
        if ((size == 1 && (v < -128 || v > 255)) || (size == 2 && (v < -32768 || v > 65535))) {
            logf("OVERFLOW %ls line %d at %p: %s = %lld does not fit in %u bytes", file, lineno, va, ex, v, size); bad++; continue;
        }
        if (n == (int)(sizeof g_patches / sizeof g_patches[0])) { logf("ERROR: patch table too long"); bad++; break; }
        patches[n++] = { va + off, size, v };
    }
    fclose(f);
    g_npatches = n;
    logf("%ls: %d valid, %d rejected", file, n - n0, bad);
    return bad;
}

static void commitPatches() {
    for (int i = 0; i < g_npatches; i++) {
        DWORD old; VirtualProtect(g_patches[i].at, g_patches[i].size, PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_patches[i].at, &g_patches[i].value, g_patches[i].size);
        VirtualProtect(g_patches[i].at, g_patches[i].size, old, &old);
    }
    logf("patches: %d applied", g_npatches);
}

// ---------------------------------------------------------------- code caves (worldmod_caves.txt, see tools/detour.py)
// CAVE <site> <steal_len> <old_bytes> <cave_bytes> [@<off>=rel:<hexaddr> | @<off>=abs|abs16|abs8:<expr>]... ; comment
// The stolen span at <site> becomes `jmp cave` + nops. rel fixups are rel32 to an absolute exe address,
// abs fixups are 32-bit values of an expression (worldmod symbols, W, H, B20...).
struct Cave { uint8_t* site; int steal; uint8_t* code; bool inPlace; };
static Cave g_caves[4096];
static int g_ncaves = 0;
static uint8_t* g_cavePool = nullptr;
static size_t g_cavePoolUsed = 0, g_cavePoolSize = 4 << 20;

static int loadCaves(const wchar_t* file, bool required) {
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\%s", g_dir, file);
    FILE* f = nullptr; _wfopen_s(&f, path, L"r");
    if (!f) { logf("no %ls", file); return required ? 1 : 0; }
    if (!g_cavePool) g_cavePool = (uint8_t*)VirtualAlloc(nullptr, g_cavePoolSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    static char line[16384];
    int bad = 0, lineno = 0, n0 = g_ncaves;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (char* c = strchr(line, ';')) *c = 0;
        char* ctx = nullptr;
        char* tok = strtok_s(line, " \t\r\n", &ctx);
        if (!tok || (strcmp(tok, "CAVE") && strcmp(tok, "REWRITE"))) continue;
        bool inPlace = !strcmp(tok, "REWRITE");     // REWRITE: the new bytes replace the span itself
        char* site_s = strtok_s(nullptr, " \t\r\n", &ctx);
        char* steal_s = strtok_s(nullptr, " \t\r\n", &ctx);
        char* old_s = strtok_s(nullptr, " \t\r\n", &ctx);
        char* code_s = strtok_s(nullptr, " \t\r\n", &ctx);
        if (!site_s || !steal_s || !old_s || !code_s) { logf("CAVE line %d: malformed", lineno); bad++; continue; }
        uint8_t* site = (uint8_t*)(uintptr_t)strtoul(site_s, nullptr, 16);
        int steal = atoi(steal_s);
        if ((!inPlace && steal < 5) || (int)strlen(old_s) != steal * 2 || !bytesMatch(site, old_s) ||
            (inPlace && (int)strlen(code_s) != steal * 2)) {
            logf("CAVE line %d at %p: exe bytes differ from table", lineno, site); bad++; continue;
        }
        size_t len = strlen(code_s) / 2;
        if (g_cavePoolUsed + len + 16 > g_cavePoolSize || g_ncaves == (int)(sizeof g_caves / sizeof g_caves[0])) {
            logf("ERROR: cave pool full"); bad++; break;
        }
        uint8_t* code = g_cavePool + g_cavePoolUsed;
        for (size_t i = 0; i < len; i++) { unsigned b; sscanf_s(code_s + 2 * i, "%2x", &b); code[i] = (uint8_t)b; }
        while (char* fx = strtok_s(nullptr, " \t\r\n", &ctx)) {
            unsigned off = 0; char kind[8] = {}; char arg[256] = {};
            int width = 4;                            // abs / rel: 4 bytes, abs16: 2, abs8: 1
            if (sscanf_s(fx, "@%u=%7[a-z0-9]:%255s", &off, kind, (unsigned)sizeof kind, arg, (unsigned)sizeof arg) != 3 ||
                off + (width = !strcmp(kind, "abs16") ? 2 : !strcmp(kind, "abs8") ? 1 : 4) > len) {
                logf("CAVE line %d: bad fixup %s", lineno, fx); bad++; continue;
            }
            int32_t v;
            uint8_t* at = inPlace ? site : code;      // rel32 is relative to where the bytes will run
            if (!strcmp(kind, "rel")) v = (int32_t)(strtoul(arg, nullptr, 16) - ((uintptr_t)at + off + 4));
            else { Eval e{ arg }; v = (int32_t)e.expr(); if (!e.ok) { logf("CAVE line %d: bad expr %s", lineno, arg); bad++; continue; } }
            memcpy(code + off, &v, width);
        }
        g_cavePoolUsed += (len + 15) & ~(size_t)15;
        g_caves[g_ncaves++] = { site, steal, code, inPlace };
    }
    fclose(f);
    logf("%ls: %d caves valid, %d rejected", file, g_ncaves - n0, bad);
    return bad;
}

static void commitCaves() {
    for (int i = 0; i < g_ncaves; i++) {
        uint8_t* p = g_caves[i].site;
        DWORD old; VirtualProtect(p, g_caves[i].steal, PAGE_EXECUTE_READWRITE, &old);
        if (g_caves[i].inPlace) {
            memcpy(p, g_caves[i].code, g_caves[i].steal);
        } else {
            p[0] = 0xE9; *(int32_t*)(p + 1) = (int32_t)((uintptr_t)g_caves[i].code - ((uintptr_t)p + 5));
            memset(p + 5, 0x90, g_caves[i].steal - 5);
        }
        VirtualProtect(p, g_caves[i].steal, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    logf("caves: %d installed", g_ncaves);
}

// ---------------------------------------------------------------- crash logger
static const DWORD WATCH_ARM = 0xE0574D01;   // raised once to load the debug registers from the handler
static int g_watchHits = 0;
static volatile uint32_t g_watchTest = 0;     // DR1 self-test target

// object manager M = S+0x1327ad0 (TERRAIN_WINDOW.md): 0xffff list nodes {key, prev, next} at M+0x20,
// used-list head M+0xc0014, free-list head M+0xc0018, object instances (0x80 B) from M+0xc0020
static DWORD g_mainThreadId = 0;
static const uint32_t OBJ_POOL = 0x4588350, OBJ_POOL_END = OBJ_POOL + 0xffff * 12;
static const uint32_t OBJ_USED = OBJ_POOL_END, OBJ_FREE = OBJ_POOL_END + 4;
static bool objLinkOk(uint32_t v) { return !v || (v >= OBJ_POOL && v < OBJ_POOL_END && (v - OBJ_POOL) % 12 == 0); }
static volatile LONG g_objHeadHits = 0;
// watch_objlist=2: the page holding the heads is read-only; writes from anywhere but the list functions
// (0x41cde0 unlink, 0x41ce60 link) that touch the heads are logged, then the write is single-stepped
static const uint32_t OBJ_GUARD_PAGE = OBJ_USED & ~0xfffu;
static volatile DWORD g_guardThread = 0;
static volatile LONG g_guardHits = 0, g_guardBad = 0;

static void logStackShort(CONTEXT* cx, char* buf, size_t size) {
    uint32_t* sp = (uint32_t*)(uintptr_t)cx->Esp; int k = 0;
    for (int i = 0; i < 400 && k < (int)size - 12; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        if (sp[i] >= 0x401000 && sp[i] < 0x74f000) k += sprintf_s(buf + k, size - k, " %08x", sp[i]);
    }
    buf[k] = 0;
}

static LONG CALLBACK onException(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* cx = ep->ContextRecord;
    if (code == WATCH_ARM) {
        cx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        DWORD dr7 = 0;
        if (g_cfg.watch) {
            cx->Dr0 = g_cfg.watch;
            cx->Dr1 = (DWORD)(uintptr_t)&g_watchTest;
            // L0, L1; DR0 and DR1: break on write, 4 bytes
            dr7 |= 1 | 4 | ((g_cfg.watchRead ? 3u : 1u) << 16) | (3u << 18) | (1u << 20) | (3u << 22);
        }
        if (g_cfg.watchObjList) {        // L2, L3: write, 4 bytes on the two list heads
            cx->Dr2 = OBJ_USED; cx->Dr3 = OBJ_FREE;
            dr7 |= 0x10 | 0x40 | (1u << 24) | (3u << 26) | (1u << 28) | (3u << 30);
        }
        cx->Dr7 = dr7;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_guardThread == GetCurrentThreadId()) {    // re-protect after the write
        DWORD old; VirtualProtect((void*)(uintptr_t)OBJ_GUARD_PAGE, 4096, PAGE_READONLY, &old);
        g_guardThread = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && g_cfg.watchObjList == 2 && ep->ExceptionRecord->NumberParameters > 1 &&
        ep->ExceptionRecord->ExceptionInformation[0] == 1 &&
        (ep->ExceptionRecord->ExceptionInformation[1] & ~0xfffu) == OBJ_GUARD_PAGE) {
        uint32_t a = (uint32_t)ep->ExceptionRecord->ExceptionInformation[1];
        InterlockedIncrement(&g_guardHits);
        bool listFn = (cx->Eip >= 0x41cde0 && cx->Eip < 0x41cf00) || (cx->Eip >= 0x41d180 && cx->Eip < 0x41d1c4) ||
                      (cx->Eip >= 0x41c140 && cx->Eip < 0x41c1e0);
        static volatile LONG otherThread = 0;
        if (listFn && a + 4 > OBJ_USED && a < OBJ_FREE + 4 && GetCurrentThreadId() != g_mainThreadId &&
            InterlockedIncrement(&otherThread) <= 40) {
            char buf[600]; logStackShort(cx, buf, sizeof buf);
            logf("OBJLIST head written by thread %lu (not the main thread) at EIP=%08lx | stack:%s", GetCurrentThreadId(), cx->Eip, buf);
        }
        if (a + 4 > OBJ_USED && a < OBJ_FREE + 4 && !listFn && InterlockedIncrement(&g_guardBad) <= 30) {
            char buf[1600]; logStackShort(cx, buf, sizeof buf);
            logf("OBJLIST GUARD: write to %08x (heads %08x/%08x) by EIP=%08lx thread %lu EAX=%08lx ECX=%08lx EDX=%08lx EBX=%08lx ESI=%08lx EDI=%08lx EBP=%08lx | stack:%s",
                 a, *(uint32_t*)(uintptr_t)OBJ_USED, *(uint32_t*)(uintptr_t)OBJ_FREE, cx->Eip, GetCurrentThreadId(),
                 cx->Eax, cx->Ecx, cx->Edx, cx->Ebx, cx->Esi, cx->Edi, cx->Ebp, buf);
        }
        while (InterlockedCompareExchange((volatile LONG*)&g_guardThread, (LONG)GetCurrentThreadId(), 0) != 0) Sleep(0);
        DWORD old; VirtualProtect((void*)(uintptr_t)OBJ_GUARD_PAGE, 4096, PAGE_READWRITE, &old);
        cx->EFlags |= 0x100;                                 // trap after the write
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && (cx->Dr6 & 0xC)) {
        cx->Dr6 = 0;
        InterlockedIncrement(&g_objHeadHits);
        uint32_t u = *(uint32_t*)(uintptr_t)OBJ_USED, f = *(uint32_t*)(uintptr_t)OBJ_FREE;
        static int bad = 0;
        if ((!objLinkOk(u) || !objLinkOk(f)) && bad++ < 20) {
            char buf[1600]; logStackShort(cx, buf, sizeof buf);
            logf("OBJLIST head written with a bad value: used=%08x free=%08x before EIP=%08lx EAX=%08lx ECX=%08lx EDX=%08lx EBX=%08lx ESI=%08lx EDI=%08lx EBP=%08lx | stack:%s",
                 u, f, cx->Eip, cx->Eax, cx->Ecx, cx->Edx, cx->Ebx, cx->Esi, cx->Edi, cx->Ebp, buf);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && (cx->Dr6 & 2)) {
        cx->Dr6 = 0;
        logf("watch self-test ok (thread %lu)", GetCurrentThreadId());
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && (cx->Dr6 & 1)) {
        cx->Dr6 = 0;
        static uint32_t seen[256]; static int nseen = 0;
        bool fresh = true;
        for (int i = 0; i < nseen; i++) if (seen[i] == cx->Eip) { fresh = false; break; }
        if (fresh && nseen < 256) seen[nseen++] = cx->Eip;
        if (fresh && g_watchHits++ < 200) {
            uint32_t* sp = (uint32_t*)(uintptr_t)cx->Esp;
            char buf[512]; int k = 0;
            for (int i = 0; i < 40 && k < 400; i++) {
                if (IsBadReadPtr(sp + i, 4)) break;
                if (sp[i] >= 0x401000 && sp[i] < 0x74f000) k += sprintf_s(buf + k, sizeof buf - k, " %08x", sp[i]);
            }
            buf[k] = 0;
            logf("WATCH %08x = %08x  written before EIP=%08lx  EAX=%08lx ECX=%08lx EDX=%08lx EBX=%08lx ESI=%08lx EDI=%08lx EBP=%08lx | stack:%s",
                 g_cfg.watch, *(uint32_t*)(uintptr_t)g_cfg.watch, cx->Eip, cx->Eax, cx->Ecx, cx->Edx, cx->Ebx, cx->Esi, cx->Edi, cx->Ebp, buf);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    logf("EXCEPTION %08lx at EIP=%08lx  addr=%p", code, c->Eip,
         ep->ExceptionRecord->NumberParameters > 1 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr);
    logf("  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx",
         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    uint32_t* sp = (uint32_t*)(uintptr_t)c->Esp;
    char buf[2048]; int k = 0;
    for (int i = 0; i < 1024 && k < 1900; i++) {
        uint32_t v = 0; if (IsBadReadPtr(sp + i, 4)) break; v = sp[i];
        if (v >= 0x401000 && v < 0x74f000) k += sprintf_s(buf + k, sizeof buf - k, " %08x", v);   // likely return addresses
    }
    buf[k] = 0; logf("  stack code ptrs:%s", buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- debug: heap validation thread
// g_mainThreadId: declared with the object manager constants above
static volatile DWORD g_loadThreadId = 0;          // thread that last read a world stream

static void logThreadStack(HANDLE th, const char* what) {
    CONTEXT c = {}; c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &c)) return;
    char buf[2048]; int k = 0;
    uint32_t* sp = (uint32_t*)(uintptr_t)c.Esp;
    for (int i = 0; i < 1024 && k < 1900; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        if (sp[i] >= 0x401000 && sp[i] < 0x74f000) k += sprintf_s(buf + k, sizeof buf - k, " %08x", sp[i]);
    }
    buf[k] = 0;
    logf("%s: main thread EIP=%08lx ESP=%08lx | stack:%s", what, c.Eip, c.Esp, buf);
}

// ---------------------------------------------------------------- object manager lock
// The object manager M (S+0x1327ad0) keeps its objects in two linked lists without any locking. The
// stage loader thread links objects (0x419280 -> 0x417590 -> 0x41d180 / 0x41d1f0 -> 0x41ce60) and resets
// the table (0x41c720) while the main thread unlinks unit models (0x417640 -> 0x41d1d0 -> 0x41cde0);
// interleaved updates leave a torn head pointer and the next list walk crashes (0x41cdf0).
// All four list operations are wrapped in one recursive lock.
static CRITICAL_SECTION g_objCs;
static volatile LONG g_objWaits = 0;
static void* g_trUnlink = nullptr; static void* g_trLinkIdx = nullptr;
static void* g_trLink = nullptr; static void* g_trReset = nullptr;

extern "C" static void __cdecl objLock() {
    if (TryEnterCriticalSection(&g_objCs)) return;
    DWORD owner = (DWORD)(uintptr_t)g_objCs.OwningThread;
    LONG n = InterlockedIncrement(&g_objWaits);
    EnterCriticalSection(&g_objCs);
    if (n <= 30) logf("object list: thread %lu waited for thread %lu (main %lu), wait #%ld", GetCurrentThreadId(), owner, g_mainThreadId, n);
}
extern "C" static void __cdecl objUnlock() { LeaveCriticalSection(&g_objCs); }

__declspec(naked) static void objUnlinkHook() {          // thiscall, 1 argument
    __asm {
        push ecx
        call objLock
        pop ecx
        push dword ptr [esp + 4]
        call g_trUnlink
        push eax
        call objUnlock
        pop eax
        ret 4
    }
}
__declspec(naked) static void objLinkIdxHook() {         // thiscall, 1 argument
    __asm {
        push ecx
        call objLock
        pop ecx
        push dword ptr [esp + 4]
        call g_trLinkIdx
        push eax
        call objUnlock
        pop eax
        ret 4
    }
}
__declspec(naked) static void objLinkHook() {            // thiscall, no arguments
    __asm {
        push ecx
        call objLock
        pop ecx
        call g_trLink
        push eax
        call objUnlock
        pop eax
        ret
    }
}
__declspec(naked) static void objResetHook() {           // thiscall, no arguments
    __asm {
        push ecx
        call objLock
        pop ecx
        call g_trReset
        push eax
        call objUnlock
        pop eax
        ret
    }
}

// ---------------------------------------------------------------- object creation failures (diagnostics)
// 0x417590(record) creates a 3D object; it returns -1 when 0x4169a0 rejects the type, the position, or a
// footprint cell of the object grid is already taken. Some callers (0x5a0476: a unit changing its model)
// do not check and then write into objects[-1], which overlaps the object manager's list heads.
static void* g_trCreate = nullptr;
static volatile LONG g_createFails = 0;
typedef int (__cdecl* FootprintFn)(int16_t** out, int type);
extern "C" static void __cdecl logCreateFail(const uint16_t* rec, uint32_t ret) {
    LONG n = InterlockedIncrement(&g_createFails);
    if (n > 40) return;
    int type = rec[1], our = rec[2], ouc = rec[3];
    int lo = (our - 57) / 2, hi = (ouc - 57 - (lo & 1)) / 2;
    int typeOk = ((int(__cdecl*)(int))0x41baa0)(type);
    int vr = *(uint16_t*)(0x4169c2 + 2), vc = *(uint16_t*)(0x4169fd + 3);
    int r2 = 2 * our - 0x70, c2 = 2 * ouc - 0x70 - (((r2 >> 2) & 1) ? 2 : 0);
    bool posOk = r2 >= 0 && r2 < vr && (r2 & 3) == 2 && c2 >= 0 && c2 < vc && (c2 & 3) == 2;
    char occ[200] = "";
    if (typeOk && posOk) {
        int16_t* fp = nullptr;
        int k = ((FootprintFn)0x41bce0)(&fp, type);
        uint8_t* S = (uint8_t*)0x3260860;
        uint32_t gdisp = *(uint32_t*)(0x416a54 + 4); int gsh = *(uint8_t*)(0x416a4b + 2);
        int m = 0;
        for (int i = 0; i < k && fp && m < 180; i++) {
            uint32_t idx = ((uint32_t)(fp[2 * i] + our) << gsh) + fp[2 * i + 1] + ouc;
            uint16_t o = *(uint16_t*)(S + gdisp + idx * 2);
            if (o != 0xffff) {
                uint8_t* obj = (uint8_t*)0x4648350 + (size_t)o * 0x80;
                m += sprintf_s(occ + m, sizeof occ - m, " cell(%d,%d)=obj %u type %u at ou(%u,%u);",
                               fp[2 * i], fp[2 * i + 1], o, obj[0x72], *(uint16_t*)(obj + 0x6e), *(uint16_t*)(obj + 0x70));
            }
        }
    }
    logf("OBJECT CREATE FAILED #%ld from %08x: type %d at ou(%d,%d) = hex (%d,%d) [China (%d,%d)]: type %s, position %s%s",
         n, ret, type, our, ouc, lo, hi, lo - g_cfg.Y0, hi - g_cfg.X0, typeOk ? "ok" : "REJECTED",
         posOk ? "ok" : "OUT OF RANGE / misaligned", occ[0] ? occ : (typeOk && posOk ? " (grid free?)" : ""));
}

__declspec(naked) static void objCreateHook() {          // thiscall (S), 1 argument
    __asm {
        push dword ptr [esp + 4]
        call g_trCreate
        cmp eax, -1
        jne done
        pushad
        push dword ptr [esp + 32]         // our return address
        push dword ptr [esp + 40]         // the record
        call logCreateFail
        add esp, 8
        popad
    done:
        ret 4
    }
}

// ---------------------------------------------------------------- unit model update guard
// 0x5a03e0 is a queued callback (vtable 0x84b2ac, [this+4] = unit) that rebuilds a unit's 3D model when
// its kind changed. Unit removal (0x5a02e0) queues it and also sets the unit's model slot (+0x22) to
// 0xffff. When the callback then runs for a unit without a model, objects[0xffff] is memory past the
// object table (the texture manager): the "old model" it reads is garbage, the new model is refused
// (0x417590 -> -1) and the callback writes into objects[-1], which holds the object list heads.
// The list walk at 0x41cdf0 crashes a few seconds later. A unit without a model has nothing to rebuild.
static void* g_trModelUpd = nullptr;
static volatile LONG g_modelSkips = 0;
extern "C" static void __cdecl noteModelSkip(void* unit) {
    LONG n = InterlockedIncrement(&g_modelSkips);
    if (n <= 5 || (n & (n - 1)) == 0) logf("unit model update skipped for unit %p: it has no model (slot 0xffff), #%ld", unit, n);
}
__declspec(naked) static void modelUpdateHook() {        // thiscall, no arguments, returns 0
    __asm {
        mov eax, [ecx + 4]
        test eax, eax
        jz go
        cmp word ptr [eax + 0x22], 0xffff
        jne go
        push ecx
        push eax
        call noteModelSkip
        add esp, 4
        pop ecx
        xor eax, eax
        ret
    go:
        jmp g_trModelUpd
    }
}

static void* makeTrampoline(uint32_t from, size_t len, const uint8_t* expect);
static void writeJmp(uint32_t from, const void* to);
static bool installModelGuard() {
    static const uint8_t head[] = { 0x83, 0xec, 0x10, 0x56, 0x57, 0x8b, 0x79, 0x04 };   // sub esp,10h; push esi; push edi; mov edi,[ecx+4]
    g_trModelUpd = makeTrampoline(0x5a03e0, sizeof head, head);
    if (!g_trModelUpd) return false;
    writeJmp(0x5a03e0, (void*)modelUpdateHook);
    return true;
}

// debug (getter_check=1): the gate and port getters take an index (building id - first gate / port id).
// A caller that still computes it from Koei's id layout passes an index out of range and gets NULL back;
// these replacements behave the same and log each such call site once (-1 = "not a gate/port" is normal).
static void noteBadIndex(const char* what, int idx, uint32_t ret) {
    static uint32_t seen[128];
    static volatile LONG n;
    for (LONG i = 0; i < n; i++) if (seen[i] == ret) return;
    LONG k = InterlockedIncrement(&n) - 1;
    if (k < 128) seen[k] = ret;
    logf("getter_check: %s index %d, call at %08x", what, idx, ret - 5);
}
static void* __fastcall gateGetter(uint8_t* world, int, int idx) {
    if (idx >= 0 && idx <= 9) return world + 0x61a8 + idx * 0x90;
    if (idx != -1) noteBadIndex("gate", idx, (uint32_t)(uintptr_t)_ReturnAddress());
    return nullptr;
}
static void* __fastcall portGetter(uint8_t* world, int, int idx) {
    if (idx >= 0 && idx <= 34) return world + 0x6748 + idx * 0x90;
    if (idx != -1) noteBadIndex("port", idx, (uint32_t)(uintptr_t)_ReturnAddress());
    return nullptr;
}
static Units* g_units = nullptr;
// debug (unit_first=N, M9 tests): the free unit search 0x491040 starts at id N (then wraps to 0), so new units use
// ids beyond Koei's 1000 without a thousand units on the map
typedef int(__thiscall* UnitBusyFn)(void*);
static void* __fastcall findFreeUnitFrom(uint8_t*, int) {
    int U = g_units->U;
    for (int k = 0; k < U; k++) {
        uint8_t* u = g_units->UNITARR + ((g_cfg.unitFirst + k) % U) * 0xf4;
        if (!(*(UnitBusyFn*)(*(uint8_t**)u + 8))(u)) return u;
    }
    return nullptr;
}
static bool installUnitFirst() {
    static const uint8_t head[] = { 0x56, 0x57, 0x33, 0xf6 };      // push esi; push edi; xor esi, esi
    if (!g_units || memcmp((void*)0x491040, head, sizeof head)) return false;
    writeJmp(0x491040, (void*)findFreeUnitFrom);
    return true;
}
static bool installGetterCheck() {
    static const uint8_t gate[] = { 0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0, 0x7c, 0x15, 0x83, 0xf8, 0x09 };
    static const uint8_t port[] = { 0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0, 0x7c, 0x15, 0x83, 0xf8, 0x22 };
    if (memcmp((void*)0x490a40, gate, sizeof gate) || memcmp((void*)0x490a70, port, sizeof port)) return false;
    writeJmp(0x490a40, (void*)gateGetter);
    writeJmp(0x490a70, (void*)portGetter);
    return true;
}

static bool installObjListLock() {
    static const uint8_t unlinkHead[] = { 0x8b, 0x81, 0xf4, 0xff, 0x0b, 0x00 };   // mov eax,[ecx+0bfff4h]
    static const uint8_t linkIdxHead[] = { 0x8b, 0x81, 0xf8, 0xff, 0x0b, 0x00 };  // mov eax,[ecx+0bfff8h]
    static const uint8_t linkHead[] = { 0x8b, 0x81, 0x18, 0x00, 0x0c, 0x00 };     // mov eax,[ecx+0c0018h]
    static const uint8_t resetHead[] = { 0x53, 0x56, 0x8b, 0xd9, 0x57 };          // push ebx; push esi; mov ebx,ecx; push edi
    InitializeCriticalSectionAndSpinCount(&g_objCs, 4000);
    g_trUnlink = makeTrampoline(0x41cde0, sizeof unlinkHead, unlinkHead);
    g_trLinkIdx = makeTrampoline(0x41ce60, sizeof linkIdxHead, linkIdxHead);
    g_trLink = makeTrampoline(0x41d180, sizeof linkHead, linkHead);
    g_trReset = makeTrampoline(0x41c720, sizeof resetHead, resetHead);
    if (!g_trUnlink || !g_trLinkIdx || !g_trLink || !g_trReset) return false;
    static const uint8_t createHead[] = { 0x53, 0x57, 0x8b, 0x7c, 0x24, 0x0c };     // push ebx; push edi; mov edi,[esp+0ch]
    g_trCreate = makeTrampoline(0x417590, sizeof createHead, createHead);
    if (g_trCreate) writeJmp(0x417590, (void*)objCreateHook);
    writeJmp(0x41cde0, (void*)objUnlinkHook);
    writeJmp(0x41ce60, (void*)objLinkIdxHook);
    writeJmp(0x41d180, (void*)objLinkHook);
    writeJmp(0x41c720, (void*)objResetHook);
    return true;
}

// debug: walk the object manager's lists every 20 ms (main thread suspended) and report the first bad link
static DWORD WINAPI objListThread(void*) {
    HANDLE main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mainThreadId);
    if (!main) return 0;
    int reports = 0;
    for (;;) {
        Sleep(50);
        // with the lock installed every list edit holds g_objCs, so holding it gives a consistent view;
        // without it, suspending the main thread is the best we can do (the loader thread keeps running)
        if (g_trUnlink) EnterCriticalSection(&g_objCs); else SuspendThread(main);
        const char* what = nullptr; uint32_t at = 0, val = 0;
        for (int list = 0; list < 2 && !what; list++) {
            uint32_t n = *(uint32_t*)(uintptr_t)(list ? OBJ_FREE : OBJ_USED), prev = 0;
            for (int steps = 0; n && steps < 0x10000; steps++) {
                if (!objLinkOk(n)) { what = list ? "free list" : "used list"; at = prev; val = n; break; }
                uint32_t* node = (uint32_t*)(uintptr_t)n;
                if (node[1] != prev && list == 0) { what = "used list back link"; at = n; val = node[1]; break; }
                prev = n; n = node[2];
            }
        }
        if (g_trUnlink) LeaveCriticalSection(&g_objCs); else ResumeThread(main);   // log only afterwards (CRT lock)
        if (what && reports++ < 5)
            logf("OBJLIST broken: %s, link after node %08x (node #%d) = %08x (head writes seen by DR2/DR3: %ld, guard page hits %ld)",
                 what, at, at ? (int)((at - OBJ_POOL) / 12) : -1, val, g_objHeadHits, g_guardHits);
        static DWORD lastReport = GetTickCount();
        if (GetTickCount() - lastReport > 60000) {
            lastReport = GetTickCount();
            logf("object list watch: %ld head writes seen by DR2/DR3, %ld guard page hits, %ld suspicious", g_objHeadHits, g_guardHits, g_guardBad);
        }
        if (reports >= 5) return 0;
    }
}

static DWORD WINAPI heapCheckThread(void*) {
    HANDLE main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mainThreadId);
    bool broken[64] = {};
    for (;;) {
        Sleep(20);
        HANDLE heaps[64]; DWORD n = GetProcessHeaps(64, heaps);
        for (DWORD i = 0; i < n && i < 64; i++) {
            if (broken[i]) continue;
            if (!HeapValidate(heaps[i], 0, nullptr)) {
                broken[i] = true;
                if (main) { SuspendThread(main); logThreadStack(main, "HEAP CORRUPT (main)"); ResumeThread(main); }
                if (g_loadThreadId && g_loadThreadId != g_mainThreadId) {
                    HANDLE lt = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_loadThreadId);
                    if (lt) { SuspendThread(lt); logThreadStack(lt, "HEAP CORRUPT (loader)"); ResumeThread(lt); CloseHandle(lt); }
                }
                logf("HEAP CORRUPT: heap %p (#%lu)", heaps[i], i);
            }
        }
    }
}

// ---------------------------------------------------------------- code hooks
static void writeJmp(uint32_t from, const void* to) {
    DWORD old; VirtualProtect((void*)(uintptr_t)from, 5, PAGE_EXECUTE_READWRITE, &old);
    uint8_t* p = (uint8_t*)(uintptr_t)from;
    p[0] = 0xE9; *(int32_t*)(p + 1) = (int32_t)((uintptr_t)to - (from + 5));
    VirtualProtect((void*)(uintptr_t)from, 5, old, &old);
}

// Copies `len` bytes of whole, position-independent instructions from `from` into an executable
// trampoline followed by a jmp back, and returns the trampoline.
static void* makeTrampoline(uint32_t from, size_t len, const uint8_t* expect) {
    if (memcmp((void*)(uintptr_t)from, expect, len)) { logf("hook @%08x: unexpected bytes, not hooked", from); return nullptr; }
    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    memcpy(t, (void*)(uintptr_t)from, len);
    t[len] = 0xE9; *(int32_t*)(t + len + 1) = (int32_t)((from + len) - ((uintptr_t)t + len + 5));
    return t;
}

// 0x483b70  HEX20 constructor (thiscall, no args). The original is unrolled for exactly 200x200.
static void __fastcall hexCtor(uint8_t* base) {
    size_t cells = (size_t)g_cfg.W * g_cfg.H;
    memset(base, 0, cells * 20);
    for (size_t i = 0; i < cells; i++) {
        uint8_t* r = base + i * 20;
        *(uint32_t*)(r + 0) |= 0x300000;
        *(uint32_t*)(r + 4) = (*(uint32_t*)(r + 4) & 0xffffbfe0) | 0x18000;
        *(uint16_t*)(r + 8) = 0xffff;
    }
}

// 0x484090  SHEX block parser (thiscall, args: data, size; ret 8).
// The stage resource and old saves carry the 200x200 China map; widen it to the world here.
typedef int(__fastcall* ShexParse)(void* self, void* edx, const uint8_t* data, int size);
static ShexParse g_shexOrig = nullptr;
static uint8_t* g_worldShex = nullptr;     // 'SHEX0008' + W*H*11, base terrain of the whole world
static uint8_t* g_area20 = nullptr;        // u16 area per hex at the byte offset of its HEX20 record + 4 (bases=1)

// With the base-count patches the SHEX loader stores the area only in AREA20; mirror its low 7 bits
// into the old HEX20 field (dword+4 bits 5..11) so that any reader still using it sees what it used to.
static void mirrorAreas() {
    if (!g_area20 || !g_cfg.watchMirror) return;
    size_t cells = (size_t)g_cfg.W * g_cfg.H;
    for (size_t i = 0; i < cells; i++) {
        uint32_t a = *(uint16_t*)(g_area20 + i * 20 + 4);
        uint32_t* d = (uint32_t*)(g_world.hex20 + i * 20 + 4);
        *d = (*d & ~(0x7fu << 5)) | ((a & 0x7f) << 5);
    }
}
static const int SHEX_REC = 11, CHINA = 200;
#pragma pack(push, 1)
struct ChinaOverride { uint16_t lo, hi; uint8_t rec[11]; };
#pragma pack(pop)
static std::vector<ChinaOverride> g_overrides;
static bool g_overridesFinal = false;      // "WCO2": area ids already in the final numbering (gen_bases.py)

static void loadWorldShex() {
    size_t cells = (size_t)g_cfg.W * g_cfg.H, size = 8 + cells * SHEX_REC;
    g_worldShex = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memcpy(g_worldShex, "SHEX0008", 8);
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\world_shex.bin", g_cfg.dataDir);
    FILE* f = nullptr; _wfopen_s(&f, path, L"rb");
    uint32_t hdr[3] = {};
    if (f && fread(hdr, 4, 3, f) == 3 && hdr[0] == 'XHSW' && hdr[1] == (uint32_t)g_cfg.W && hdr[2] == (uint32_t)g_cfg.H &&
        fread(g_worldShex + 8, SHEX_REC, cells, f) == cells) {
        logf("world_shex.bin loaded (%dx%d)", g_cfg.W, g_cfg.H);
    } else {
        // no world data: everything outside China is impassable mountain (terrain type 15)
        for (size_t i = 0; i < cells; i++) { uint8_t* r = g_worldShex + 8 + i * SHEX_REC; memset(r, 0, SHEX_REC); r[0] = 15; }
        logf("world_shex.bin missing or wrong size; using an all-mountain world");
    }
    if (f) fclose(f);

    // china_overrides.bin: "WCOV" (Koei's area numbering) or "WCO2" (final numbering, applied after the
    // renumbering), u32 n, n x {u16 lo, u16 hi, 11-byte record}. Deliberate edits to the stock China block
    // (Koei's open-sea strip on the east edge becomes sea; the edge band new cities take, gen_bases.py).
    swprintf_s(path, L"%s\\china_overrides.bin", g_cfg.dataDir);
    f = nullptr; _wfopen_s(&f, path, L"rb");
    uint32_t ov[2] = {};
    if (f && fread(ov, 4, 2, f) == 2 && (ov[0] == 'VOCW' || ov[0] == '2OCW') && ov[1] <= CHINA * CHINA) {
        g_overridesFinal = ov[0] == '2OCW';
        g_overrides.resize(ov[1]);
        if (fread(g_overrides.data(), sizeof(ChinaOverride), ov[1], f) != ov[1]) g_overrides.clear();
        logf("china_overrides.bin: %u records (%s numbering)", (unsigned)g_overrides.size(), g_overridesFinal ? "final" : "Koei's");
    }
    if (f) fclose(f);
}

static int __fastcall shexHook(void* self, void* edx, const uint8_t* data, int size) {
    const int chinaSize = 8 + CHINA * CHINA * SHEX_REC;
    if (size == chinaSize && !memcmp(data, "SHEX0008", 8) && (g_cfg.W != CHINA || g_cfg.H != CHINA)) {
        uint8_t* w = g_worldShex;
        for (int y = 0; y < CHINA; y++)
            memcpy(w + 8 + ((size_t)(g_cfg.Y0 + y) * g_cfg.W + g_cfg.X0) * SHEX_REC, data + 8 + (size_t)y * CHINA * SHEX_REC, CHINA * SHEX_REC);
        auto applyOverrides = [&]() {
            for (const ChinaOverride& o : g_overrides)
                if (o.lo < CHINA && o.hi < CHINA)
                    memcpy(w + 8 + ((size_t)(g_cfg.Y0 + o.lo) * g_cfg.W + g_cfg.X0 + o.hi) * SHEX_REC, o.rec, SHEX_REC);
        };
        if (!g_overridesFinal) applyOverrides();
        if (g_cfg.bases && g_cfg.C > 42)                 // ids >= 42 (gates, ports, special areas) move up by C-42
            for (int y = 0; y < CHINA; y++)
                for (int x = 0; x < CHINA; x++) {
                    uint8_t* r = w + 8 + ((size_t)(g_cfg.Y0 + y) * g_cfg.W + g_cfg.X0 + x) * SHEX_REC;
                    uint16_t a = (uint16_t)(r[1] | (r[2] << 8));
                    if (a >= 42) { a = (uint16_t)(a + g_cfg.C - 42); r[1] = (uint8_t)a; r[2] = (uint8_t)(a >> 8); }
                }
        if (g_overridesFinal) applyOverrides();
        int r = g_shexOrig(self, edx, w, 8 + g_cfg.W * g_cfg.H * SHEX_REC);
        mirrorAreas();
        logf("SHEX: widened 200x200 block into %dx%d world at (%d,%d) -> %d", g_cfg.W, g_cfg.H, g_cfg.X0, g_cfg.Y0, r);
        return r;
    }
    int r = g_shexOrig(self, edx, data, size);
    mirrorAreas();
    return r;
}

// 0x567541  `mov ecx,W*H*7; mov edi,B28; rep stosd` in the movement-range search 0x567520.
// It zeroes the whole HEX28 array on every query (140 MB for an Earth-sized world). HEX28 is
// allocated with MEM_WRITE_WATCH, so only pages written since the previous query are cleared.
static uint8_t* g_hex28Alloc = nullptr;
static size_t g_hex28AllocSize = 0;
static uint32_t g_hex28End = 0;
static void** g_dirtyPages = nullptr;

static void clearHex28Dirty() {
    ULONG_PTR count = g_hex28AllocSize / 4096; DWORD gran = 4096;
    if (GetWriteWatch(0, g_hex28Alloc, g_hex28AllocSize, g_dirtyPages, &count, &gran) == 0) {
        for (ULONG_PTR i = 0; i < count; i++) memset(g_dirtyPages[i], 0, gran);
        ResetWriteWatch(g_hex28Alloc, g_hex28AllocSize);
    } else {
        memset(g_hex28Alloc, 0, g_hex28AllocSize);
    }
}

static void clearHex28All() { memset(g_hex28Alloc, 0, g_hex28AllocSize); }

__declspec(naked) static void clearHex28Thunk() {
    __asm {
        pushad
        pushfd
        cld
        call clearHex28Dirty
        popfd
        popad
        xor ecx, ecx                 // registers as `rep stosd` would leave them
        mov edi, g_hex28End
        ret
    }
}

static bool hookHex28Clear() {
    uint8_t* p = (uint8_t*)0x567541;
    // b9 imm32 / bf imm32 / f3 ab  (the immediates were already rewritten by the patch table)
    if (p[0] != 0xB9 || p[5] != 0xBF || p[10] != 0xF3 || p[11] != 0xAB) return false;
    g_dirtyPages = (void**)VirtualAlloc(nullptr, g_hex28AllocSize / 4096 * sizeof(void*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old; VirtualProtect(p, 12, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE8; *(int32_t*)(p + 1) = (int32_t)((uintptr_t)clearHex28Thunk - ((uintptr_t)p + 5));
    memset(p + 5, 0x90, 7);
    VirtualProtect(p, 12, old, &old);
    return true;
}

// IAT slot of SHGetPathFromIDList(A). The game asks for Documents (CSIDL_PERSONAL) to create
// "Koei\San11 Tc\..." for saves and custom officers; on a non-Chinese ANSI code page a Chinese
// Documents path turns into '?' and directory creation fails. Redirect such paths to <game>\UserData.
static const uint32_t IAT_SHGetPathFromIDListA = 0x74e3ac;

// ---------------------------------------------------------------- started from a non-ASCII path
// The game opens its data files with ANSI paths. When the system's non-Unicode language cannot represent
// the game directory (e.g. Chinese folder names under code page 1252) every file read fails and the game
// shows "固定劇本資料讀取失敗". Restart through an ASCII path to the same directory: [mod] ascii_dir in
// worldmod.ini, or a directory link (junction) in the root of any drive that points here.
static bool g_pathBad = false;

static bool sameDirectory(const wchar_t* a, const wchar_t* b) {
    HANDLE ha = CreateFileW(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = CreateFileW(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    bool same = false;
    if (hb != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION ia, ib;
        if (GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib))
            same = ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber && ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
        CloseHandle(hb);
    }
    CloseHandle(ha);
    return same;
}

static bool asciiOnly(const wchar_t* t) { for (; *t; t++) if (*t > 126) return false; return true; }

static bool findAsciiPath(wchar_t* out) {
    wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%s\\worldmod.ini", g_dir);
    GetPrivateProfileStringW(L"mod", L"ascii_dir", L"", out, MAX_PATH, ini);
    if (*out && asciiOnly(out) && sameDirectory(out, g_dir)) return true;
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; d++) {
        if (!(drives & (1u << d))) continue;
        wchar_t root[8]; swprintf_s(root, L"%c:\\", L'A' + d);
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        wchar_t pat[8]; swprintf_s(pat, L"%c:\\*", L'A' + d);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && asciiOnly(fd.cFileName)) {
                swprintf_s(out, MAX_PATH, L"%c:\\%s", L'A' + d, fd.cFileName);
                if (sameDirectory(out, g_dir)) { FindClose(h); return true; }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    *out = 0;
    return false;
}

// Called from the first hooked call (outside the loader lock); does nothing when the path is fine.
static void relaunchFromAsciiPath() {
    static volatile LONG once = 0;
    if (!g_pathBad || InterlockedExchange(&once, 1)) return;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe;
    wchar_t dir[MAX_PATH];
    if (findAsciiPath(dir)) {
        wchar_t target[MAX_PATH]; swprintf_s(target, L"%s\\%s", dir, name);
        const wchar_t* args = GetCommandLineW();               // keep the arguments, replace the program
        if (*args == L'"') { args = wcschr(args + 1, L'"'); args = args ? args + 1 : L""; }
        else { while (*args && *args != L' ') args++; }
        static wchar_t cmd[2048]; swprintf_s(cmd, L"\"%s\"%s", target, args);
        STARTUPINFOW si = {}; GetStartupInfoW(&si); si.cb = sizeof si;   // same window state as this launch
        PROCESS_INFORMATION pi = {};
        logf("started from a path the game cannot read; restarting as %ls", target);
        if (CreateProcessW(target, cmd, nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            if (g_log) fflush(g_log);
            ExitProcess(0);
        }
        logf("restart failed (error %lu)", GetLastError());
    }
    wchar_t msg[1024];
    swprintf_s(msg, L"游戏所在的路径含有中文：\n%s\n\n系统的「非 Unicode 程序语言」不是中文，游戏无法用这个路径读取数据。\n\n"
                    L"请为游戏目录建一个英文路径的目录链接，并从那里启动，例如：\n"
                    L"mklink /J G:\\San11PK \"%s\"\n然后运行 G:\\San11PK\\%s\n\n"
                    L"建好链接后直接从这里启动也可以，worldmod 会自动改从英文路径重新启动。",
               g_dir, g_dir, name);
    MessageBoxW(nullptr, msg, L"三国志11 worldmod", MB_OK | MB_ICONWARNING);
    ExitProcess(1);
}

static BOOL WINAPI pathFromIDListHook(PCIDLIST_ABSOLUTE pidl, LPSTR out) {
    relaunchFromAsciiPath();
    wchar_t w[MAX_PATH];
    if (!SHGetPathFromIDListW(pidl, w)) return FALSE;
    if (g_cfg.saveDir[0]) {                 // save_dir= (the test copy: UserData_test): saves apart per launcher
        char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH);
        if (char* s = strrchr(dir, '\\')) *s = 0;
        sprintf_s(out, MAX_PATH, "%s\\%s", dir, g_cfg.saveDir);
        CreateDirectoryA(out, nullptr);
        return TRUE;
    }
    BOOL lossy = FALSE;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, MAX_PATH, nullptr, &lossy) > 0 && !lossy)
        return TRUE;
    char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char* s = strrchr(dir, '\\')) *s = 0;
    sprintf_s(out, MAX_PATH, "%s\\UserData", dir);
    CreateDirectoryA(out, nullptr);
    logf("Documents path is not representable in the ANSI code page; using %s", out);
    return TRUE;
}

static void hookIat(uint32_t slot, const void* fn);
typedef HANDLE(WINAPI* CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileAFn g_createFileA = nullptr;
static HANDLE WINAPI createFileAHook(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    relaunchFromAsciiPath();
    // Scen%03d.s11 and Scenario.s11 under media\scenario hold 42 cities; tools/convert_scen.py writes
    // copies for the configured city count into <data_dir>\scenario
    const char* base = name ? strrchr(name, '\\') : nullptr;
    if (base && (access & GENERIC_READ) && !(access & GENERIC_WRITE)) {
        base++;
        bool scen = (!_strnicmp(base, "Scen", 4) && strlen(base) == 11 && !_stricmp(base + 7, ".s11")) || !_stricmp(base, "Scenario.s11");
        if (scen && (strstr(name, "scenario\\") || strstr(name, "SCENARIO\\") || strstr(name, "Scenario\\"))) {
            char alt[MAX_PATH];
            sprintf_s(alt, "%ls\\scenario\\%s", g_cfg.dataDir, base);
            HANDLE h = g_createFileA(alt, access, share, sa, disp, flags, tmpl);
            if (h != INVALID_HANDLE_VALUE) return h;
            logf("WARNING: converted %s not found in the data directory; the game reads the 42-city original", base);
        }
    }
    return g_createFileA(name, access, share, sa, disp, flags, tmpl);
}

// automation=1 (the test copy): give its named mutexes their own names, so the single-instance check
// (0x472cd0: OpenMutexA, then FindWindowA and exit) does not see a normal game that is running
typedef HANDLE(WINAPI* OpenMutexAFn)(DWORD, BOOL, LPCSTR);
typedef HANDLE(WINAPI* CreateMutexAFn)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static OpenMutexAFn g_openMutexA = nullptr;
static CreateMutexAFn g_createMutexA = nullptr;
static const char* testMutexName(LPCSTR name, char* buf, size_t n) {
    if (!name) return nullptr;
    sprintf_s(buf, n, "%s_worldmod_test", name);
    return buf;
}
static HANDLE WINAPI openMutexAHook(DWORD access, BOOL inherit, LPCSTR name) {
    char b[300]; return g_openMutexA(access, inherit, testMutexName(name, b, sizeof b));
}
static HANDLE WINAPI createMutexAHook(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name) {
    char b[300]; return g_createMutexA(sa, owner, testMutexName(name, b, sizeof b));
}

static void hookIat(uint32_t slot, const void* fn) {
    DWORD old; VirtualProtect((void*)(uintptr_t)slot, 4, PAGE_READWRITE, &old);
    *(const void**)(uintptr_t)slot = fn;
    VirtualProtect((void*)(uintptr_t)slot, 4, old, &old);
}

// 0x418fb0 K3ST (stage vertices + quads) and 0x4191f0 GCOL (seasonal vertex colours) parsers,
// thiscall on the stage object, args (data, size), ret 8, return 1 on success. Both are replaced:
// terrainworld.cpp parses Koei's China stage and fills the whole world.
static volatile LONG g_radarRepaint = 0;      // set when a stage loads: the UI may have reloaded Koei's picture
static volatile LONG g_stageFresh = 0;        // a stage was loaded and the map camera has not run since
static int __fastcall k3stHook(void* self, void* edx, const uint8_t* d, int s) {
    InterlockedExchange(&g_radarRepaint, 1);                     // a new stage: repaint the radar picture once it is shown
    InterlockedExchange(&g_stageFresh, 1);                       // auto_skip starts when the map camera runs (clampHook)
    if (g_cfg.watch) { RaiseException(WATCH_ARM, 0, 0, nullptr); logf("watch armed at K3ST load (thread %lu), [watch]=%08x:", GetCurrentThreadId(), *(uint32_t*)(uintptr_t)g_cfg.watch); g_watchTest++; }
    int r = terrainLoadK3st(d, s);
    // 0x415f20 rebuilds the per-cell records (water level, texture sets) from the quads, as the original did
    typedef void(__fastcall* CellRebuild)(void* self, void* edx);
    if (r) ((CellRebuild)0x415f20)(self, nullptr);
    return r;
}
static int __fastcall gcolHook(void* self, void* edx, const uint8_t* d, int s) {
    return terrainLoadGcol(d, s);
}

// 0x419280 OBJS parser (thiscall on the stage, args (data, size), ret 8): "OBJS" "0004" then 0xffff
// records of 14 bytes {u8 used, u16 type, u16 row, u16 col (half-vertex units), u8, f32, u8, u8}.
// Koei's objects (bases, walls, landmarks) are in China stage coordinates; move them to China's place.
typedef int(__fastcall* ObjsParse)(void* self, void* edx, const uint8_t* data, int size);
static ObjsParse g_objsOrig = nullptr;
static int __fastcall objsHook(void* self, void* edx, const uint8_t* d, int s) {
    const int REC = 14, COUNT = 0xffff;
    if (g_cfg.watch) { logf("watch self-test at OBJS load (thread %lu), [watch]=%08x:", GetCurrentThreadId(), *(uint32_t*)(uintptr_t)g_cfg.watch); g_watchTest++; }
    if ((g_cfg.X0 || g_cfg.Y0) && s >= 8 + REC * COUNT && !memcmp(d, "OBJS", 4)) {
        std::vector<uint8_t> copy(d, d + s);
        int moved = 0;
        for (int i = 0; i < COUNT; i++) {
            uint8_t* r = copy.data() + 8 + (size_t)i * REC;
            if (!r[0]) continue;
            *(uint16_t*)(r + 3) += (uint16_t)(2 * g_cfg.Y0);
            *(uint16_t*)(r + 5) += (uint16_t)(2 * g_cfg.X0);
            moved++;
        }
        int added = 0;
        if (g_cfg.bases) {                               // bases_objs.bin: "WBOB", u32 n, n x {u16 slot, 14-byte record}
            wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\bases_objs.bin", g_cfg.dataDir);
            FILE* f = nullptr; _wfopen_s(&f, path, L"rb");
            uint32_t h[2] = {};
            if (f && fread(h, 4, 2, f) == 2 && h[0] == 'BOBW')
                for (uint32_t i = 0; i < h[1]; i++) {
                    uint16_t slot; uint8_t r[REC];
                    if (fread(&slot, 2, 1, f) != 1 || fread(r, REC, 1, f) != 1 || slot >= COUNT) break;
                    memcpy(copy.data() + 8 + (size_t)slot * REC, r, REC);
                    added++;
                }
            if (f) fclose(f);
        }
        int ret = g_objsOrig(self, edx, copy.data(), s);
        logf("OBJS: %d objects moved by (%d,%d) half-vertices, %d city models added -> %d", moved, 2 * g_cfg.Y0, 2 * g_cfg.X0, added, ret);
        return ret;
    }
    return g_objsOrig(self, edx, d, s);
}

// 0x5714a0 camera clamp (cdecl, arg: camera; +0x00 eye x,y,z  +0x10 look-at x,y,z). Called whenever the
// camera moves, so it is where the terrain window follows the camera.
typedef int(__cdecl* CameraClamp)(float* cam);
static CameraClamp g_clampOrig = nullptr;
static float g_camX = 0, g_camZ = 0;   // last look-at point (world units)
static float* g_camPtr = nullptr;      // the camera clamped last (look-at x/z at [4]/[6]); the game also moves it
                                       // without the clamp (0x5723c0 at the start of a game, jumps from lists)

static volatile LONG g_gotoPending = 0;
static float g_gotoX = 0, g_gotoZ = 0;
// ---------------------------------------------------------------- minimap window
// The radar (minimap object 0x4ecd7d0) shows a 256x256-cell window of the terrain: its picture is drawn
// by 0x410eb0 from the vertex colours, markers are placed by 0x40fba0, clicks go through 0x5735d0 /
// 0x63b9a0. Originally the window is Koei's stage; in the world it starts on China and follows the camera:
// when the look-at point gets more than MM_SLACK cells from the window centre, the window scrolls after it
// (MM_SPEED cells/s; at once when the camera is off the radar) and the picture is redrawn. MMLO/MMHI (first cell) and MMVR/MMVC/MMVOFF (its vertex offsets) are read by
// the patched minimap code (worldmod_terrain_caves.txt). One cell = 4 vertices = 20 world units.
static int32_t g_mmLo = 0, g_mmHi = 0, g_mmVr = 0, g_mmVc = 0, g_mmVoff = 0;
static int g_mmMaxLo = 0, g_mmMaxHi = 0, g_mmVC = 0;
static const int MM_CELLS = 256, MM_SLACK = 72;
static const float MM_SPEED = 48.0f;                   // cells per second (at the radar's rim) while the camera is on the radar
static float g_mmFLo = 0, g_mmFHi = 0;                 // window position with fractions (scrolling)
static ULONGLONG g_mmTick = 0;
static uint8_t* const MINIMAP = (uint8_t*)0x4ecd7d0;
typedef void (__thiscall* MinimapDrawFn)(void* self, const int16_t* rect, uint32_t arg);   // 0x410eb0
typedef void (WINAPI* Vec3TransformFn)(void* out, const float* v, const void* m);          // 0x6ad2cb

static void minimapSetWindow(int lo, int hi) {
    lo = max(0, min(g_mmMaxLo, lo)) & ~1;              // even: odd rows sit half a hex lower
    hi = max(0, min(g_mmMaxHi, hi));
    g_mmLo = lo; g_mmHi = hi; g_mmVr = 4 * lo; g_mmVc = 4 * hi;
    g_mmVoff = (g_mmVr * g_mmVC + g_mmVc) * 10;
}

static bool g_radarLearned = false;

static void minimapFollow(float x, float z) {
    if (!g_mmVC || !*(void**)(MINIMAP + 0x90)) return;            // no world terrain / radar not set up yet
    RadarWorld rw = { g_world.hex20, g_cfg.W, g_cfg.H, g_cfg.Y0, g_cfg.X0 };
    if (!g_radarLearned) {                                         // Koei's picture, before anything is painted over it
        if (!radarLearn(rw, logf)) return;
        g_radarLearned = true;
        if (g_mmLo != g_cfg.Y0 || g_mmHi != g_cfg.X0) InterlockedExchange(&g_radarRepaint, 1);
    }
    int r = (int)(x * 0.05f), c = (int)(z * 0.05f);
    bool moved = false;
    // While the camera is still on the radar (e.g. the player drags the view frame towards the radar's edge),
    // the window scrolls at MM_SPEED cells per second, like edge scrolling; re-centring at once would make the
    // held cursor point another ~100 cells further every frame. A camera off the radar (a jump from a list,
    // a city) re-centres immediately.
    ULONGLONG now = GetTickCount64();
    if (!g_mmTick) { g_mmFLo = (float)g_mmLo; g_mmFHi = (float)g_mmHi; }
    float dt = g_mmTick ? min(0.1f, (now - g_mmTick) * 0.001f) : 0.0f;
    g_mmTick = now;
    float dr = r - (g_mmFLo + MM_CELLS / 2), dc = c - (g_mmFHi + MM_CELLS / 2);
    if (fabsf(dr) > MM_CELLS / 2 || fabsf(dc) > MM_CELLS / 2) {
        g_mmFLo = (float)(r - MM_CELLS / 2); g_mmFHi = (float)(c - MM_CELLS / 2);
    } else {
        auto step = [&](float d) { return MM_SPEED * dt * min(1.0f, (fabsf(d) - MM_SLACK) / (120.0f - MM_SLACK)); };
        if (fabsf(dr) > MM_SLACK) g_mmFLo += (dr > 0 ? 1 : -1) * min(step(dr), fabsf(dr) - MM_SLACK);
        if (fabsf(dc) > MM_SLACK) g_mmFHi += (dc > 0 ? 1 : -1) * min(step(dc), fabsf(dc) - MM_SLACK);
    }
    g_mmFLo = max(0.0f, min((float)g_mmMaxLo, g_mmFLo)); g_mmFHi = max(0.0f, min((float)g_mmMaxHi, g_mmFHi));
    int lo0 = g_mmLo, hi0 = g_mmHi;
    minimapSetWindow((int)lroundf(g_mmFLo), (int)lroundf(g_mmFHi));
    moved = g_mmLo != lo0 || g_mmHi != hi0;                         // not when pinned at the world's edge
    if (InterlockedExchange(&g_radarRepaint, 0) || moved) {
        bool ok = radarPaint(rw, g_mmLo, g_mmHi);
        static int n = 0;
        if (++n <= 20) logf("radar window at cells (%d,%d) (camera at cell %d,%d); picture %s", g_mmLo, g_mmHi, r, c, ok ? "repainted" : "NOT repainted");
    }
}

// 0x40fba0 (thiscall, ret 8): world position -> radar coordinates. Same arithmetic as the game (cell =
// world * 0.05, minus the window, minus the centre 128, then the radar matrix at this+0x50); markers
// outside the radar circle are moved far off screen instead of being drawn around the frame.
static void __fastcall minimapWorldToMap(uint8_t* self, void*, const float* in, void* out) {
    float v[4] = { (float)((int)(in[0] * 0.05f) - g_mmLo) - 128.0f, (float)((int)(in[2] * 0.05f) - g_mmHi) - 128.0f, 0.0f, 1.0f };
    if (v[0] * v[0] + v[1] * v[1] > 128.0f * 128.0f) v[0] = v[1] = 100000.0f;
    ((Vec3TransformFn)0x6ad2cb)(out, v, self + 0x50);
}

static int __cdecl clampHook(float* cam) {
    if (InterlockedExchange(&g_gotoPending, 0)) {     // automation "goto": shift eye and look-at together
        float dx = g_gotoX - cam[4], dz = g_gotoZ - cam[6];
        cam[0] += dx; cam[2] += dz; cam[4] += dx; cam[6] += dz;
    }
    int r = g_clampOrig(cam);
    g_camPtr = cam;
    // the stage is also loaded behind the scenario / force selection screens; the camera runs only on the map
    if (g_stageFresh && InterlockedExchange(&g_stageFresh, 0)) autoSkipOnStageLoad();   // auto_skip=1
    g_camX = cam[4];
    g_camZ = cam[6];
    minimapFollow(cam[4], cam[6]);
    return r;
}
void automationCamera(float* x, float* z) {     // the scene camera's look-at (object 0x32602b0, vt[0])
    typedef float* (__thiscall* SceneCameraFn)(void* scene);
    float* cam = *(void**)(uintptr_t)0x32602b0 ? ((SceneCameraFn)(*(void***)(uintptr_t)0x32602b0)[0])((void*)(uintptr_t)0x32602b0) : nullptr;
    *x = cam ? cam[4] : g_camX; *z = cam ? cam[6] : g_camZ;
}
void automationGoto(float x, float z) { g_gotoX = x; g_gotoZ = z; InterlockedExchange(&g_gotoPending, 1); }

static void installViewTweaks();
static void installEnvFxGuard();

static void installTerrainHooks() {
    static const uint8_t clampHead[] = { 0x83, 0xEC, 0x1C, 0xA1, 0xA0, 0x9F, 0x1B, 0x09 };
    g_clampOrig = (CameraClamp)makeTrampoline(0x5714a0, sizeof clampHead, clampHead);
    if (g_clampOrig) writeJmp(0x5714a0, (void*)clampHook);
    terrainOpen(g_cfg.dataDir, g_cfg.Y0, g_cfg.X0);
    writeJmp(0x418fb0, (void*)k3stHook);
    writeJmp(0x4191f0, (void*)gcolHook);
    static const uint8_t objsHead[] = { 0x83, 0xEC, 0x30, 0x57, 0x89, 0x4C, 0x24, 0x20 };
    g_objsOrig = (ObjsParse)makeTrampoline(0x419280, sizeof objsHead, objsHead);
    if (g_objsOrig) writeJmp(0x419280, (void*)objsHook);
    static const uint8_t mmHead[] = { 0x83, 0xEC, 0x10, 0x56, 0x57 };          // sub esp,10h; push esi; push edi
    bool mm = !memcmp((void*)0x40fba0, mmHead, sizeof mmHead);
    if (mm) writeJmp(0x40fba0, (void*)minimapWorldToMap);
    logf("terrain hooks: k3st, gcol replaced; objs=%s; camera=%s; radar follows the camera=%s",
         g_objsOrig ? "yes" : "no", g_clampOrig ? "yes" : "no", mm ? "yes" : "no");
    installViewTweaks();
    installEnvFxGuard();
}

// 0x4d2d90「在可见格包围盒内随机选格放环境特效」里 div ecx 会因行跨度截断成 0 而除零崩溃。
// 原版地图可见范围小不会触发；worldmod 世界 + 远缩放会让包围盒跨度超 16 位。加保护：跨度=0 就跳过。
static void installEnvFxGuard() {
    static const uint8_t head[] = { 0x0F, 0xBF, 0xCE, 0x33, 0xD2 };   // movsx ecx, si; xor edx, edx
    // 行跨度 div @0x4d2de2（前面无 FPU 压栈）
    uint8_t* at1 = (uint8_t*)0x4d2ddd;
    if (!memcmp(at1, head, sizeof head)) {
        uint8_t* c = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        uint8_t* p = c;
        *p++ = 0x66; *p++ = 0x85; *p++ = 0xF6;                           // test si, si
        *p++ = 0x0F; *p++ = 0x84; *(int32_t*)p = (int32_t)(0x4d2ea3 - ((uintptr_t)p + 4)); p += 4;  // jz 0x4d2ea3
        *p++ = 0x0F; *p++ = 0xBF; *p++ = 0xCE;                           // movsx ecx, si
        *p++ = 0x33; *p++ = 0xD2;                                        // xor edx, edx
        *p++ = 0xE9; *(int32_t*)p = (int32_t)(0x4d2de2 - ((uintptr_t)p + 4)); p += 4;  // jmp 0x4d2de2
        writeJmp(0x4d2ddd, c);
    } else logf("env-fx guard @4d2ddd: unexpected bytes, not hooked");
    // 列跨度 div @0x4d2e0d（前面 0x4d2e04 fild 压了 FPU 栈，跳转前要弹掉）
    uint8_t* at2 = (uint8_t*)0x4d2e08;
    if (!memcmp(at2, head, sizeof head)) {
        uint8_t* c = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        uint8_t* p = c;
        *p++ = 0x66; *p++ = 0x85; *p++ = 0xF6;                           // test si, si
        uint8_t* jz = p; *p++ = 0x0F; *p++ = 0x84; p += 4;               // jz cleanup（占位，下面回填）
        *p++ = 0x0F; *p++ = 0xBF; *p++ = 0xCE;                           // movsx ecx, si
        *p++ = 0x33; *p++ = 0xD2;                                        // xor edx, edx
        *p++ = 0xE9; *(int32_t*)p = (int32_t)(0x4d2e0d - ((uintptr_t)p + 4)); p += 4;  // jmp 0x4d2e0d
        *(int32_t*)(jz + 2) = (int32_t)((uintptr_t)p - (uintptr_t)(jz + 6));           // jz -> cleanup
        *p++ = 0xDD; *p++ = 0xD8;                                        // fstp st(0)（清理 fild）
        *p++ = 0xE9; *(int32_t*)p = (int32_t)(0x4d2ea3 - ((uintptr_t)p + 4)); p += 4;  // jmp 0x4d2ea3
        writeJmp(0x4d2e08, c);
    } else logf("env-fx guard @4d2e08: unexpected bytes, not hooked");
    logf("env-fx guard installed (div-by-zero protection x2)");
}

// 视图增强：缩放上限放大。
// 让相机能缩到整个世界。
static void installViewTweaks() {
    float* zt = (float*)0x84b36c;
    DWORD oldz; VirtualProtect(zt, 12, PAGE_READWRITE, &oldz);
    zt[0] = 20000.0f; zt[1] = 40000.0f; zt[2] = 80000.0f;
    VirtualProtect(zt, 12, oldz, &oldz);
    *(float*)0x8b95ec = 80000.0f;                  // 立即生效（当前档）
    logf("view tweaks: zoom table %g/%g/%g", zt[0], zt[1], zt[2]);
    // 第二表 0x84b378（整数，0x5a0b90 读入 0x8a5a40，疑似视野格数上限）150/300/500 -> 1500/3000/6000
    DWORD oldg; VirtualProtect((void*)0x84b378, 12, PAGE_READWRITE, &oldg);
    *(int*)0x84b378 = 2000;
    *(int*)(0x84b378 + 4) = 4000;
    *(int*)(0x84b378 + 8) = 8000;
    VirtualProtect((void*)0x84b378, 12, oldg, &oldg);
    logf("view tweaks: 第二表 %d/%d/%d", *(int*)0x84b378, *(int*)(0x84b378 + 4), *(int*)(0x84b378 + 8));
}

// ---------------------------------------------------------------- 中地圖 (midmap.cpp, research/midmap_re.md)
// The map panel MapUI (vt 0x859670) composes its 628x628 picture into the shared texture [0x89e01c] in
// 0x640260 and places the base / unit markers with 0x63ee40 (hex -> panel pixel, 3 pixels per hex, no
// clipping); a left click jumps the camera to 0x63d320(panel pixel). worldmod paints the inner 600x600
// map area itself after Koei's compose (Koei's frame stays), replaces both transforms with its view
// (markers outside the map area are moved far off screen) and adds zoom (mouse wheel) and panning (left
// drag; a click without dragging still moves the camera there).
static Bases* g_bases = nullptr;
static Forces* g_forces = nullptr;
static const uint32_t MAPUI_VT = 0x859670;
typedef int(__thiscall* MapUiComposeFn)(void* ui);                                   // 0x640260
typedef void(__thiscall* MapUiLayoutFn)(void* ui);                                   // 0x63fd00
typedef void(__thiscall* MapUiColourFn)(void* ui, uint32_t* out, int idx);           // 0x63eda0
typedef int(__thiscall* MapUiMouseFn)(void* ui, void* target, uint32_t wp, POINT* pt);
typedef int(__thiscall* MapUiWheelFn)(void* ui, void* target, uint32_t keys, int delta, POINT* pt);
typedef void*(__thiscall* MapUiDeleteFn)(void* ui, int flags);
static MapUiComposeFn g_mmComposeOrig = nullptr;
static MapUiMouseFn g_mmMoveOrig = nullptr, g_mmDownOrig = nullptr, g_mmUpOrig = nullptr;
static MapUiWheelFn g_mmWheelOrig = nullptr;
static MapUiDeleteFn g_mmDeleteOrig = nullptr;
static void* g_mmUi = nullptr;                 // the open MapUI (set at its first compose, cleared when deleted)
static float g_mmScale = 3.0f;                 // zoom kept between openings (Koei's: 3 pixels per hex)
static struct { bool down, dragging; POINT at, last; uint32_t wp; void* target; } g_mmDrag = {};
static const int MM_BORDER = 14;               // the panel's frame; the map area is 600x600 inside it

static int __fastcall mmCompose(uint8_t* ui, void*) {
    int r = g_mmComposeOrig(ui);
    if (ui != g_mmUi) {                         // opened: centre on the main camera
        g_mmUi = ui;
        // the scene's camera (what 0x63d790 moves on a click): object 0x32602b0, vt[0] -> camera, look-at x/z at +0x10/+0x18
        typedef float* (__thiscall* SceneCameraFn)(void* scene);
        float* cam = ((SceneCameraFn)(*(void***)(uintptr_t)0x32602b0)[0])((void*)(uintptr_t)0x32602b0);
        float cx = cam ? cam[4] : g_camX, cz = cam ? cam[6] : g_camZ;
        float lo = cx / 20.0f - 28.5f, hi = cz / 20.0f - 28.5f;
        if (cx <= 0 && cz <= 0) { lo = g_cfg.Y0 + 100.0f; hi = g_cfg.X0 + 100.0f; }
        midmapSetView(lo + 0.5f, hi + 0.5f, g_mmScale);
    }
    void* tex = *(void**)(uintptr_t)0x89e01c;          // IDirect3DTexture9 (LockRect = vtable +0x4c, UnlockRect +0x50)
    if (!tex || !g_bases || !midmapReady()) return r;
    const int N = g_bases->N;
    static std::vector<uint32_t> col;
    col.assign(N, 0);
    int mode = *(int*)(ui + 0x78);
    if (mode >= 0 && mode <= 4)
        for (int b = 0; b < N; b++) {
            uint32_t c = 0;
            if (b == 255) {                         // Koei's colour functions treat index 255 as the border colour
                if (mode == 0) {                    // force colour (table 0x6fae424, 55 entries) at 80 %, as 0x63eae0
                    int f = *(int*)(uintptr_t)(0x728b088 + 255 * 0x38 + 0xc);
                    int ci = (f >= 0 && f < 47) ? *(int*)(uintptr_t)(0x7201958 + 0x7af8 + f * 0x12c + 0x44) : -1;
                    if (ci >= 0 && ci < 55) { c = *(uint32_t*)(uintptr_t)(0x6fae424 + 4 * ci); c = (c & 0xffffff) | (((c >> 24) * 80 / 100) << 24); }
                }
            } else ((MapUiColourFn)0x63eda0)(ui, &c, b);
            col[b] = c;
        }
    struct { int Pitch; void* pBits; } lr = {};
    RECT rc = { MM_BORDER, MM_BORDER, MM_BORDER + 600, MM_BORDER + 600 };
    typedef long(__stdcall* LockFn)(void* t, unsigned level, void* lr, const RECT* r, unsigned long flags);
    typedef long(__stdcall* UnlockFn)(void* t, unsigned level);
    void** vt = *(void***)tex;
    if (((LockFn)vt[0x4c / 4])(tex, 0, &lr, &rc, 0) >= 0) {
        midmapRender((uint32_t*)lr.pBits, lr.Pitch / 4, col.data(), mode >= 0 && mode <= 4, (ui[0x80] & 0x20) != 0);
        midmapRenderNames((uint32_t*)lr.pBits, lr.Pitch / 4);
        ((UnlockFn)vt[0x50 / 4])(tex, 0);
    }
    return r;
}
static void mmRefresh(void* ui) {               // view changed: picture and marker positions
    g_mmScale = midmapScale();
    mmCompose((uint8_t*)ui, nullptr);
    ((MapUiLayoutFn)0x63fd00)(ui);
}

// 0x63ee40 (cdecl): packed position (lo | hi << 16) -> marker centre in panel pixels
static POINT* __cdecl mmHexToPanel(POINT* out, const uint32_t* pos) {
    int lo = (int16_t)(*pos & 0xffff), hi = (int16_t)(*pos >> 16);
    bool show = true;
    float s = midmapScale();
    uintptr_t a = (uintptr_t)pos;
    if (g_bases && a >= 0x728b088 && a < 0x728b088 + 0x4000 * 0x38) {
        int id = (int)((a - 0x728b088) / 0x38), type = *(int*)(uintptr_t)(0x728b088 + id * 0x38 + 8);
        if (id >= g_bases->N) show = s >= 2.0f;                 // facilities (根據地)
        else if (type != 0) show = s >= 1.4f;                   // gates and ports: only when there is room
    }
    float px, py;
    if (show && midmapHexToPixel(lo, hi, 7.0f, &px, &py)) { out->x = MM_BORDER + (int)px; out->y = MM_BORDER + (int)py; }
    else { out->x = -30000; out->y = -30000; }                  // drawn far off screen, never hit
    return out;
}
// 0x63d320 (cdecl): panel pixel -> packed hex, used by the click that moves the camera
static uint32_t __cdecl mmPanelToHex(const POINT* pt) {
    int lo, hi;
    if (!midmapPixelToHex((float)(pt->x - MM_BORDER), (float)(pt->y - MM_BORDER), &lo, &hi)) {
        float l, h, s; midmapView(&l, &h, &s); lo = (int)l; hi = (int)h;
    }
    return (uint32_t)(lo & 0xffff) | ((uint32_t)hi << 16);
}

static int __fastcall mmWheel(uint8_t* ui, void*, void* target, uint32_t keys, int delta, POINT* pt) {
    if (!pt || !midmapReady()) return g_mmWheelOrig(ui, target, keys, delta, pt);
    midmapZoom(pt->x - MM_BORDER, pt->y - MM_BORDER, (int16_t)delta);
    mmRefresh(ui);
    return 1;
}
static int __fastcall mmDown(uint8_t* ui, void*, void* target, uint32_t wp, POINT* pt) {
    if (!pt || !midmapReady()) return g_mmDownOrig(ui, target, wp, pt);
    g_mmDrag.down = true; g_mmDrag.dragging = false; g_mmDrag.at = *pt; g_mmDrag.last = *pt; g_mmDrag.wp = wp; g_mmDrag.target = target;
    return 1;                                   // decided at button up: a drag pans, a click is Koei's click
}
static int __fastcall mmMove(uint8_t* ui, void*, void* target, uint32_t wp, POINT* pt) {
    if (g_mmDrag.down && pt) {
        if (!(wp & MK_LBUTTON)) g_mmDrag.down = false;          // released outside the panel
        else {
            if (!g_mmDrag.dragging && (abs(pt->x - g_mmDrag.at.x) > 4 || abs(pt->y - g_mmDrag.at.y) > 4)) g_mmDrag.dragging = true;
            if (g_mmDrag.dragging && (pt->x != g_mmDrag.last.x || pt->y != g_mmDrag.last.y)) {
                midmapPan(pt->x - g_mmDrag.last.x, pt->y - g_mmDrag.last.y);
                g_mmDrag.last = *pt;
                mmRefresh(ui);
            }
        }
    }
    return g_mmMoveOrig(ui, target, wp, pt);
}
static int __fastcall mmUp(uint8_t* ui, void*, void* target, uint32_t wp, POINT* pt) {
    if (!g_mmDrag.down) return g_mmUpOrig(ui, target, wp, pt);
    bool click = !g_mmDrag.dragging;
    g_mmDrag.down = g_mmDrag.dragging = false;
    if (!click) return 1;
    POINT at = g_mmDrag.at;
    g_mmDownOrig(ui, g_mmDrag.target, g_mmDrag.wp, &at);     // Koei's click: camera jump (mode 4)
    return g_mmUpOrig(ui, target, wp, pt);                    //               and selection (pick modes)
}
static void* __fastcall mmDelete(uint8_t* ui, void*, int flags) {   // vt[0], the deleting destructor
    if (ui == g_mmUi) g_mmUi = nullptr;
    g_mmDrag.down = false;
    return g_mmDeleteOrig(ui, flags);
}

static void* patchSlot(uint32_t slot, const void* fn) {
    void* old = *(void**)(uintptr_t)slot;
    DWORD o; VirtualProtect((void*)(uintptr_t)slot, 4, PAGE_READWRITE, &o);
    *(const void**)(uintptr_t)slot = fn;
    VirtualProtect((void*)(uintptr_t)slot, 4, o, &o);
    return old;
}

static void installMidmapHooks() {
    if (!g_bases) return;
    MidmapWorld mw = { g_world.hex20, g_area20, g_cfg.W, g_cfg.H, g_cfg.Y0, g_cfg.X0, g_bases->C, g_bases->N, (const int32_t*)g_bases->AREA2BASE };
    midmapSetup(mw, g_dir, logf);
    midmapSetCityArray(g_bases->CITYARR);
    static const uint8_t composeHead[] = { 0x83, 0xEC, 0x08, 0x56, 0x57 };       // sub esp,8; push esi; push edi
    static const uint8_t toPanelHead[] = { 0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x10 };
    static const uint8_t toHexHead[] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };
    if (memcmp((void*)0x63ee40, toPanelHead, sizeof toPanelHead) || memcmp((void*)0x63d320, toHexHead, sizeof toHexHead) ||
        *(uint32_t*)(uintptr_t)(MAPUI_VT + 0xa8) != 0x4754d0 || *(uint32_t*)(uintptr_t)(MAPUI_VT + 0x88) != 0x63f450 ||
        *(uint32_t*)(uintptr_t)(MAPUI_VT + 0x90) != 0x63d790 || *(uint32_t*)(uintptr_t)(MAPUI_VT + 0x94) != 0x63d800 ||
        *(uint32_t*)(uintptr_t)(MAPUI_VT + 0x00) != 0x63fce0) { logf("midmap: unexpected code, not installed"); return; }
    g_mmComposeOrig = (MapUiComposeFn)makeTrampoline(0x640260, sizeof composeHead, composeHead);
    if (!g_mmComposeOrig) { logf("midmap: compose hook failed"); return; }
    writeJmp(0x640260, (void*)mmCompose);
    writeJmp(0x63ee40, (void*)mmHexToPanel);
    writeJmp(0x63d320, (void*)mmPanelToHex);
    g_mmWheelOrig = (MapUiWheelFn)patchSlot(MAPUI_VT + 0xa8, (void*)mmWheel);
    g_mmMoveOrig = (MapUiMouseFn)patchSlot(MAPUI_VT + 0x88, (void*)mmMove);
    g_mmDownOrig = (MapUiMouseFn)patchSlot(MAPUI_VT + 0x90, (void*)mmDown);
    g_mmUpOrig = (MapUiMouseFn)patchSlot(MAPUI_VT + 0x94, (void*)mmUp);
    g_mmDeleteOrig = (MapUiDeleteFn)patchSlot(MAPUI_VT + 0x00, (void*)mmDelete);
    logf("midmap: world 中地圖 installed (compose, transforms, wheel zoom, drag)");
}

// 0x493BCE (in the world serializer 0x4937B0, after every object of a loaded stream has been read and
// before the post-load fixups 0x493400, which look positions up in the map): scenarios and TS stages
// store China-local hex positions; move them to China's place in the world (COORDS.md H1).
// Saves written in world mode already hold world positions.
static void __stdcall translateLoaded(uint8_t* stream) {
    g_loadThreadId = GetCurrentThreadId();
    int rdKind = *(int*)(stream + 0x54);           // M9: unit records the stream did not hold (not master data 4 / 0x18)
    if (*(int*)(stream + 8) == 1 && g_units && rdKind != 4 && rdKind != 0x18) unitsResetTail(*g_units);
    if (*(int*)(stream + 8) != 1 || (!g_cfg.X0 && !g_cfg.Y0)) return;      // reading only
    int kind = *(int*)(stream + 0x54);
    bool chinaLocal = kind == 2 || kind == 3 || kind == 0x16 || kind == 0x17 || kind == 0x0C || kind == 0x1B;
    if (!chinaLocal) return;
    auto move = [](int16_t* p) {
        if (p[0] >= 0 && p[0] < CHINA && p[1] >= 0 && p[1] < CHINA) { p[0] += (int16_t)g_cfg.Y0; p[1] += (int16_t)g_cfg.X0; return 1; }
        return 0;
    };
    int nb = 0, nu = 0;
    for (int i = 0; i < 16384; i++) nb += move((int16_t*)(uintptr_t)(0x728B088 + 0x38 * i + 0x1E));
    for (int i = 0; i < 1000; i++) {
        uint8_t* u = (uint8_t*)(uintptr_t)(0x736B088 + 0xF4 * i);
        nu += move((int16_t*)(u + 0x3C));
        move((int16_t*)(u + 0x38));
    }
    logf("loaded stream kind %#x: %d building and %d unit positions moved into the world", kind, nb, nu);
}

__declspec(naked) static void loadedThunk() {
    __asm {
        pushad
        push ebp
        call translateLoaded
        popad
        cmp dword ptr [ebp + 8], 1            // the 10 replaced bytes: cmp [ebp+8],1 / jne 0x493C73
        je cont
        push 0x493C73
        ret
    cont:
        push 0x493BD8
        ret
    }
}

// Stream headers (0x43b330 reads one, 0x43b490 writes one; thiscall(header, stream), ret 4): the unused
// dword at file offset 0x2c (header +0x30) marks streams with 16-bit id fields (phase C, bases.cpp serId).
static const uint32_t WIDE_MARK = 'EDIW';                 // bytes "WIDE"
typedef int(__thiscall* HeaderIoFn)(void* hdr, void* stream);
static HeaderIoFn g_hdrReadOrig = nullptr, g_hdrWriteOrig = nullptr;
// M8: streams for more than 42 regular forces carry "WF" + u16 R instead (also wide).
// M9: streams for more than 1000 units carry 'U' + u8 R + u16 U (also wide; R <= 249 fits the byte).
static uint32_t streamMark() {
    int R = g_forces ? g_forces->R : 42, U = g_units ? g_units->U : 1000;
    if (U != 1000) return 'U' | (uint32_t)R << 8 | (uint32_t)U << 16;
    return R != 42 ? ('F' << 8 | 'W' | (uint32_t)R << 16) : WIDE_MARK;
}
static int __fastcall hdrRead(uint8_t* hdr, void*, uint8_t* stream) {
    int r = g_hdrReadOrig(hdr, stream);
    uint32_t m = *(uint32_t*)(hdr + 0x30);
    bool wf = (m & 0xffff) == ('F' << 8 | 'W'), wu = (m & 0xff) == 'U';
    if (g_bases && g_bases->IDWIDE) InterlockedExchange((LONG*)g_bases->IDWIDE, m == WIDE_MARK || wf || wu);
    int streamR = wf ? (int)(m >> 16) : wu ? (int)(m >> 8 & 0xff) : 42;
    int streamU = wu ? (int)(m >> 16) : 1000;
    uint32_t kind = *(uint32_t*)(hdr + 8);         // 4 / 0x18: master data (no forces, no units)
    bool master = kind == 4 || kind == 0x18;
    if (g_forces && streamR != g_forces->R && m != 0 && !master)
        logf("ERROR: this file was made for %d regular forces, the game runs %d (worldmod.ini forces=): it will not load correctly", streamR, g_forces->R);
    if (g_units) {
        int U = g_units->U;
        if (streamU > U && !master)
            logf("ERROR: this file holds %d units, the game runs %d (worldmod.ini units=): it will not load correctly", streamU, U);
        *g_units->USTREAM = streamU < U ? streamU : U;    // older saves: 1000 records, the rest stay empty
    }
    return r;
}
static int __fastcall hdrWrite(uint8_t* hdr, void*, uint8_t* stream) {
    *(uint32_t*)(hdr + 0x30) = streamMark();
    if (g_bases && g_bases->IDWIDE) InterlockedExchange((LONG*)g_bases->IDWIDE, 1);
    if (g_units) *g_units->USTREAM = g_units->U;
    return g_hdrWriteOrig(hdr, stream);
}

// M8 (forces.cpp): with more than 42 regular forces the two u8[47] force x force members live in side tables
typedef void(__thiscall* ForceResetFn)(void*, int);
static ForceResetFn g_forceResetOrig = nullptr;
static void __fastcall forceResetHook(uint8_t* force, void*, int arg) {
    g_forceResetOrig(force, arg);
    forceClearRows(force);
}
static bool installForceHooks() {
    static const uint8_t getRel[] = { 0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0, 0x7c, 0x0c, 0x83, 0xf8, 0x2e };
    static const uint8_t setRel[] = { 0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0, 0x7c, 0x12, 0x83, 0xf8, 0x2e };
    static const uint8_t getA64[] = { 0x56, 0x8b, 0xf1, 0x8b, 0x06, 0xff, 0x50, 0x08 };
    static const uint8_t setA64[] = { 0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0, 0x7c, 0x0d, 0x83, 0xf8, 0x2e };
    static const uint8_t reset[] = { 0x8b, 0x44, 0x24, 0x04, 0x53, 0x56, 0x57 };
    static const uint8_t serRel[] = { 0xe8, 0x60, 0xfd, 0xff, 0xff };     // 0x481d7b call 0x481ae0
    static const uint8_t serA64[] = { 0xe8, 0xdb, 0xfc, 0xff, 0xff };     // 0x481e00 call 0x481ae0
    if (memcmp((void*)0x4814e0, getRel, sizeof getRel) || memcmp((void*)0x481ac0, setRel, sizeof setRel) ||
        memcmp((void*)0x4812c0, getA64, sizeof getA64) || memcmp((void*)0x4814c0, setA64, sizeof setA64) ||
        memcmp((void*)0x481d7b, serRel, sizeof serRel) || memcmp((void*)0x481e00, serA64, sizeof serA64)) return false;
    g_forceResetOrig = (ForceResetFn)makeTrampoline(0x481020, sizeof reset, reset);
    if (!g_forceResetOrig) return false;
    writeJmp(0x481020, (void*)forceResetHook);
    writeJmp(0x4814e0, g_forces->GET_REL);
    writeJmp(0x481ac0, g_forces->SET_REL);
    writeJmp(0x4812c0, g_forces->GET_A64);
    writeJmp(0x4814c0, g_forces->SET_A64);
    auto setCall = [](uint32_t at, const void* to) {
        DWORD old; VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
        *(int32_t*)(at + 1) = (int32_t)((uintptr_t)to - (at + 5));
        VirtualProtect((void*)at, 5, old, &old);
    };
    setCall(0x481d7b, g_forces->SER_FREL);
    setCall(0x481e00, g_forces->SER_FA64);
    // the force bit sets (F bits per row)
    static const uint8_t testMask[] = { 0x8b, 0x44, 0x24, 0x04, 0x85, 0xc0, 0x7c, 0x15 };   // 0x4811b0
    static const uint8_t pairSet[] = { 0x55, 0x8b, 0x6c, 0x24, 0x08, 0x56, 0x57, 0x55 };    // 0x4b4f40
    static const uint8_t serMask[] = { 0xe8, 0x8e, 0xfd, 0xff, 0xff };                      // 0x481dbd call 0x481b50
    static const uint8_t aiHead[] = { 0x8b, 0x44, 0x24, 0x04 };
    static const uint32_t ai[] = { 0x5f69a0, 0x5f69f0, 0x5f6a20, 0x5f6a50, 0x5f6a80, 0x5f6ab0, 0x5f6ae0 };
    static void* const aiFn[] = { (void*)aiReset, (void*)aiClear0, (void*)aiSet0, (void*)aiTest0, (void*)aiClear1,
                                  (void*)aiSet1, (void*)aiTest1 };
    if (memcmp((void*)0x4811b0, testMask, sizeof testMask) || memcmp((void*)0x4b4f40, pairSet, sizeof pairSet) ||
        memcmp((void*)0x481dbd, serMask, sizeof serMask)) return false;
    for (uint32_t a : ai) if (memcmp((void*)(uintptr_t)a, aiHead, sizeof aiHead)) return false;
    writeJmp(0x4811b0, (void*)forceTestMask);
    writeJmp(0x4b4f40, (void*)forceSetMaskPair);
    setCall(0x481dbd, (void*)forceSerMask);
    for (int i = 0; i < 7; i++) writeJmp(ai[i], aiFn[i]);
    return true;
}

static void installHooks() {
    writeJmp(0x483b70, (void*)hexCtor);
    if (g_forces) logf("force tables and bit sets (R=%d): %s", g_forces->R, installForceHooks() ? "installed" : "NOT installed (unexpected code)");
    if (g_forces) logf("stack-array store traps: %s", forcesInstallTraps() ? "installed" : "NOT installed (unexpected code)");
    if (g_bases) {
        static const uint8_t rdHead[] = { 0x51, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x14 };   // push ecx/ebx/ebp/esi; mov esi,[esp+14h]
        static const uint8_t wrHead[] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };                     // push esi; mov esi,[esp+8]
        g_hdrReadOrig = (HeaderIoFn)makeTrampoline(0x43b330, sizeof rdHead, rdHead);
        g_hdrWriteOrig = (HeaderIoFn)makeTrampoline(0x43b490, sizeof wrHead, wrHead);
        if (g_hdrReadOrig && g_hdrWriteOrig) { writeJmp(0x43b330, (void*)hdrRead); writeJmp(0x43b490, (void*)hdrWrite); }
        else logf("ERROR: stream header hooks not installed: wide-id streams will be misread");
    }
    if (g_bases && g_bases->WPICK) writeJmp(0x5f7530, g_bases->WPICK);     // weighted pick with int count / result (phase C)
    {
        static const uint8_t head[] = { 0x83, 0x7D, 0x08, 0x01, 0x0F, 0x85, 0x9B, 0x00, 0x00, 0x00 };
        if (!memcmp((void*)0x493BCE, head, sizeof head)) {
            writeJmp(0x493BCE, (void*)loadedThunk);
            DWORD old; VirtualProtect((void*)0x493BD3, 5, PAGE_EXECUTE_READWRITE, &old);
            memset((void*)0x493BD3, 0x90, 5);
            VirtualProtect((void*)0x493BD3, 5, old, &old);
        } else logf("hook @493bce: unexpected bytes, not hooked");
    }
    static const uint8_t shexHead[] = { 0x83, 0xEC, 0x10, 0x8B, 0x44, 0x24, 0x18 };
    g_shexOrig = (ShexParse)makeTrampoline(0x484090, sizeof shexHead, shexHead);
    if (g_shexOrig) writeJmp(0x484090, (void*)shexHook);
    bool clear = g_cfg.fastClear && hookHex28Clear();
    if (g_cfg.objListLock) logf("object list lock: %s", installObjListLock() ? "installed" : "NOT installed (unexpected code)");
    if (g_cfg.modelGuard) logf("unit model update guard: %s", installModelGuard() ? "installed" : "NOT installed (unexpected code)");
    if (g_cfg.getterCheck) logf("gate/port getter check: %s", installGetterCheck() ? "installed" : "NOT installed (unexpected code)");
    if (g_cfg.unitFirst > 0) logf("unit_first=%d: %s", g_cfg.unitFirst, installUnitFirst() ? "installed" : "NOT installed (unexpected code)");
    logf("hooks installed (hexCtor, shex=%s, hex28 dirty-page clear=%s)", g_shexOrig ? "yes" : "no", clear ? "yes" : "no");
    PathfindConfig pf = {};
    pf.mode = g_cfg.astarMode; pf.margin = g_cfg.astarMargin; pf.benchSlow = g_cfg.astarBenchSlow; pf.benchGoal = g_cfg.astarBenchGoal; pf.stats = g_cfg.astarStats; pf.enterLog = g_cfg.astarEnter; pf.verify = g_cfg.astarVerify; pf.benchable = g_cfg.automation;
    pf.chinaLo = g_cfg.Y0; pf.chinaHi = g_cfg.X0; pf.hex28 = g_hex28Alloc; pf.hex28Size = g_hex28AllocSize;
    pf.clear28 = clear ? clearHex28Dirty : clearHex28All;
    pathfindInstall(pf, logf);
    if (g_cfg.worldTerrain) installTerrainHooks();
    if (g_cfg.worldTerrain && g_cfg.bases) installMidmapHooks();
}

// ---------------------------------------------------------------- d3d9 forwarding
static HMODULE g_real = nullptr;
static FARPROC realProc(const char* name) {
    if (!g_real) {
        char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH); strcat_s(sys, "\\d3d9.dll");
        g_real = LoadLibraryA(sys);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}
extern "C" void* WINAPI Direct3DCreate9(UINT sdk) {
    relaunchFromAsciiPath();
    typedef void* (WINAPI* F)(UINT); static F f = (F)realProc("Direct3DCreate9");
    void* d3d = f ? f(sdk) : nullptr;
    return (g_cfg.trace || g_cfg.automation || g_cfg.autoSkip) ? traceWrapDirect3D9(d3d) : d3d;
}
extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, void** out) {
    typedef HRESULT(WINAPI* F)(UINT, void**); static F f = (F)realProc("Direct3DCreate9Ex");
    return f ? f(sdk, out) : E_FAIL;
}
extern "C" int WINAPI D3DPERF_BeginEvent(DWORD c, LPCWSTR n) { typedef int(WINAPI* F)(DWORD, LPCWSTR); static F f = (F)realProc("D3DPERF_BeginEvent"); return f ? f(c, n) : 0; }
extern "C" int WINAPI D3DPERF_EndEvent() { typedef int(WINAPI* F)(); static F f = (F)realProc("D3DPERF_EndEvent"); return f ? f() : 0; }
extern "C" void WINAPI D3DPERF_SetMarker(DWORD c, LPCWSTR n) { typedef void(WINAPI* F)(DWORD, LPCWSTR); static F f = (F)realProc("D3DPERF_SetMarker"); if (f) f(c, n); }
extern "C" void WINAPI D3DPERF_SetRegion(DWORD c, LPCWSTR n) { typedef void(WINAPI* F)(DWORD, LPCWSTR); static F f = (F)realProc("D3DPERF_SetRegion"); if (f) f(c, n); }
extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame() { typedef BOOL(WINAPI* F)(); static F f = (F)realProc("D3DPERF_QueryRepeatFrame"); return f ? f() : FALSE; }
extern "C" void WINAPI D3DPERF_SetOptions(DWORD o) { typedef void(WINAPI* F)(DWORD); static F f = (F)realProc("D3DPERF_SetOptions"); if (f) f(o); }
extern "C" DWORD WINAPI D3DPERF_GetStatus() { typedef DWORD(WINAPI* F)(); static F f = (F)realProc("D3DPERF_GetStatus"); return f ? f() : 0; }

// ---------------------------------------------------------------- entry
static bool isTargetExe() {
    // only patch the exact dumped san11pk.exe we analysed (PE timestamp 1168334429, SizeOfImage 0x9883000)
    auto base = (uint8_t*)GetModuleHandleA(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    return nt->FileHeader.TimeDateStamp == 1168334429 && nt->OptionalHeader.SizeOfImage == 0x9883000;
}

// With hundreds of cities some functions of the game keep tens of kilobytes of per-city arrays on the
// stack (frames sized by C / N in the patch tables) and touch them without probing page by page, which
// can jump over the stack's guard page. Commit the top of the main thread's stack (up to 8 MB, never the last 64 KB of its
// reservation) up front; DllMain runs on that thread.
static void commitMainStack() {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(tib->StackLimit, &mi, sizeof mi)) return;
    uint8_t* base = (uint8_t*)mi.AllocationBase, *limit = (uint8_t*)tib->StackLimit;
    uint8_t* from = base + 64 * 1024;
    if (limit - from > 8 * 1024 * 1024) from = limit - 8 * 1024 * 1024;   // san11pk_world.exe reserves 48 MB: 8 MB is plenty
    if (from >= limit) return;
    void* r = VirtualAlloc(from, limit - from, MEM_COMMIT, PAGE_READWRITE);
    logf("main thread stack: committed %u KB below the stack limit (%s)", (unsigned)((limit - from) >> 10), r ? "ok" : "FAILED");
}

static void init() {
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(g_dir, L'\\')) *s = 0;
    loadConfig();
    wchar_t lp[MAX_PATH]; swprintf_s(lp, L"%s\\worldmod_%s.log", g_dir, g_section);
    g_log = _wfsopen(lp, L"w", _SH_DENYNO);   // shared so the log can be read while the game runs
    char ap[MAX_PATH]; GetModuleFileNameA(nullptr, ap, MAX_PATH);
    logf("worldmod loaded; [%ls] W=%d H=%d china=(%d,%d) enable=%d verify_only=%d", g_section, g_cfg.W, g_cfg.H, g_cfg.X0, g_cfg.Y0, g_cfg.enable, g_cfg.verifyOnly);
    logf("exe path as the game sees it (ANSI): %s", ap);
    if (strchr(ap, '?')) {
        g_pathBad = true;
        logf("WARNING: game path is not representable in the ANSI code page; the game would fail to load its data. "
             "worldmod restarts it through an ASCII path to the same directory ([mod] ascii_dir or a junction) at the first file access.");
    }
    AddVectoredExceptionHandler(1, onException);
    g_mainThreadId = GetCurrentThreadId();
    commitMainStack();
    if (g_cfg.automation) profilerInit(logf);
    if (g_cfg.heapCheck) { CreateThread(nullptr, 1 << 18, heapCheckThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr); logf("heap check thread started"); }
    if (g_cfg.watch) { RaiseException(WATCH_ARM, 0, 0, nullptr); logf("watchpoint armed on %08x (main thread)", g_cfg.watch); g_watchTest = 1; }
    if (g_cfg.watchObjList) {
        if (!g_cfg.watch) RaiseException(WATCH_ARM, 0, 0, nullptr);
        CreateThread(nullptr, 1 << 16, objListThread, nullptr, 0, nullptr);
        if (g_cfg.watchObjList == 2) { DWORD old; VirtualProtect((void*)(uintptr_t)OBJ_GUARD_PAGE, 4096, PAGE_READONLY, &old); }
        logf("object list watch: heads %08x/%08x, validation thread started", OBJ_USED, OBJ_FREE);
    }
    if (g_cfg.trace || g_cfg.automation || g_cfg.autoSkip) traceInit(g_dir, g_section, logf, g_cfg.trace != 0);
    if (g_cfg.trace) logf("d3d trace enabled");
    if (!isTargetExe()) { logf("not the expected san11pk.exe build; patches disabled"); return; }
    hookIat(IAT_SHGetPathFromIDListA, (void*)pathFromIDListHook);
    if (g_cfg.automation) {                              // 0x74e264 OpenMutexA, 0x74e2f0 CreateMutexA
        g_openMutexA = *(OpenMutexAFn*)(uintptr_t)0x74e264; hookIat(0x74e264, (void*)openMutexAHook);
        g_createMutexA = *(CreateMutexAFn*)(uintptr_t)0x74e2f0; hookIat(0x74e2f0, (void*)createMutexAHook);
    }
    if (g_cfg.bases && g_cfg.C > 42) {                   // 0x74e2a8: IAT slot of CreateFileA
        g_createFileA = *(CreateFileAFn*)(uintptr_t)0x74e2a8;
        hookIat(0x74e2a8, (void*)createFileAHook);
    }
    if (g_cfg.automation) automationInit(g_dir, logf);
    if (g_cfg.autoSkip) autoSkipInit(g_dir, logf);
    if (!g_cfg.enable) return;
    if (!configValid()) {
        logf("ERROR: invalid [map] settings (width/height must be multiples of 200, <= 8000; china_x/china_y inside, china_y even). Patches disabled.");
        return;
    }
    size_t cells = (size_t)g_cfg.W * g_cfg.H;
    // one spare row on each side absorbs the row-granular overshoot of some full-map scan loops
    size_t pad20 = (size_t)g_cfg.W * 20 * 2, pad28 = (size_t)g_cfg.W * 28 * 2;
    uint8_t* a20 = (uint8_t*)VirtualAlloc(nullptr, cells * 20 + 2 * pad20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_hex28AllocSize = cells * 28 + 2 * pad28;
    uint8_t* a28 = (uint8_t*)VirtualAlloc(nullptr, g_hex28AllocSize, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!a20 || !a28) { logf("ERROR: allocation failed for %zu cells", cells); return; }
    g_world.hex20 = a20 + pad20; g_world.hex28 = a28 + pad28;
    g_hex28Alloc = a28; g_hex28End = (uint32_t)(uintptr_t)(g_world.hex28 + cells * 28);
    if (g_cfg.watchLo >= 0) {           // debug: watch one hex's HEX20 dword+4 (on the main thread)
        g_cfg.watch = (uint32_t)(uintptr_t)(g_world.hex20 + ((size_t)g_cfg.watchLo * g_cfg.W + g_cfg.watchHi) * 20 + 4);
        RaiseException(WATCH_ARM, 0, 0, nullptr);
        logf("watchpoint (%s) on HEX20+4 of hex (%d,%d) = %08x", g_cfg.watchRead ? "read/write" : "write", g_cfg.watchLo, g_cfg.watchHi, g_cfg.watch);
    }
    logf("hex20 @ %p (%zu bytes), hex28 @ %p (%zu bytes)", g_world.hex20, cells * 20, g_world.hex28, cells * 28);
    if (g_cfg.W != CHINA || g_cfg.H != CHINA) {
        // the big stack frames in 0x597f20 / 0x5a8800 grow with W*H; the stock exe reserves only 1 MB of stack
        auto base = (uint8_t*)GetModuleHandleA(nullptr);
        auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        size_t need = cells * 5 + (8u << 20);
        if (nt->OptionalHeader.SizeOfStackReserve < need) {
            logf("ERROR: stack reserve %#lx < needed %#zx; launch san11pk_world.exe (built by make_world_exe.py). Patches disabled.",
                 nt->OptionalHeader.SizeOfStackReserve, need);
            return;
        }
        loadWorldShex();
    }
    // verify everything against the untouched exe first, then write: patches, caves (which may
    // overwrite patched instructions they carry over), hooks
    auto num = [](const char* name, intptr_t v) { Eval::add(name, (const void*)v); };
    num("CHLO", g_cfg.Y0);                 // China block: first hex row (lo)
    num("CHHI", g_cfg.X0);                 //              first hex column (hi)
    num("CVR", 4 * g_cfg.Y0);              // global vertex of China stage vertex (0,0)
    num("CVC", 4 * g_cfg.X0);
    int bad = loadPatches(L"worldmod_patches.txt");
    bad += loadCaves(L"worldmod_caves.txt", false);
    if (g_cfg.bases) {
        static Units un;
        if (!unitsSetup(g_cfg.U, un, logf)) { logf("ERROR: unit arrays could not be set up. Patches disabled."); return; }
        g_units = &un;
        g_cfg.U = un.U;                    // the patch expressions use the count unitsSetup accepted
        struct { const char* n; const void* v; } usyms[] = {
            { "UNITARR", un.UNITARR }, { "AIUREC", un.AIUREC }, { "USTREAM", un.USTREAM }, { "UWRAP", un.UWRAP },
            { "UPOOL", un.UPOOL }, { "SNAPSHADOW", (void*)snapShadow } };
        for (auto& e : usyms) Eval::add(e.n, e.v);
        static Bases b;
        if (!basesSetup(g_cfg.C, g_cfg.U, g_cfg.dataDir, b, logf)) { logf("ERROR: base arrays could not be set up. Patches disabled."); return; }
        g_bases = &b;
        struct { const char* n; const void* v; } syms[] = {
            { "CITYARR", b.CITYARR }, { "AIBASE", b.AIBASE }, { "AICITY", b.AICITY }, { "AIUNIT", b.AIUNIT },
            { "AIBLD16", b.AIBLD16 }, { "AREAHEX", b.AREAHEX }, { "CITYBITS", b.CITYBITS }, { "AIRT", b.AIRT },
            { "AREACOL", b.AREACOL }, { "UISCR_A", b.UISCR_A }, { "UISCR_B", b.UISCR_B }, { "UISCR_C", b.UISCR_C },
            { "UISCR_D", b.UISCR_D }, { "UISCR_E", b.UISCR_E }, { "UISCR_F", b.UISCR_F }, { "AREAADJ", b.AREAADJ },
            { "MAT87A", b.MAT87A }, { "MAT87B", b.MAT87B }, { "MATC", b.MATC }, { "AREA2CITY", b.AREA2CITY },
            { "AREA2BASE", b.AREA2BASE }, { "CITYTAB844", b.CITYTAB844 }, { "DBGBASE", b.DBGBASE },
            { "BLD_U8", b.BLD_U8 }, { "SER_U8ID", b.SER_U8ID }, { "SER_ID", b.SER_ID }, { "IDWIDE", b.IDWIDE } };
        for (auto& e : syms) Eval::add(e.n, e.v);
        static Forces fo;
        if (g_cfg.R > g_cfg.C) g_cfg.R = g_cfg.C;
        if (!forcesSetup(g_cfg.R, fo, logf)) { logf("ERROR: force arrays could not be set up. Patches disabled."); return; }
        g_forces = &fo;
        g_cfg.R = fo.R;                    // the patch expressions use the count forcesSetup accepted
        struct { const char* n; const void* v; } fsyms[] = {
            { "FORCEARR", fo.FORCEARR }, { "CORPSARR", fo.CORPSARR }, { "FREL", fo.FREL }, { "FA64", fo.FA64 },
            { "GET_REL", fo.GET_REL }, { "SET_REL", fo.SET_REL }, { "GET_A64", fo.GET_A64 }, { "SET_A64", fo.SET_A64 },
            { "SER_FREL", fo.SER_FREL }, { "SER_FA64", fo.SER_FA64 }, { "TURNORD", fo.TURNORD },
            { "AIFREG", fo.AIFREG }, { "AICORPS", fo.AICORPS }, { "FLAG104", fo.FLAG104 }, { "FLAG84A", fo.FLAG84A },
            { "FLAG84B", fo.FLAG84B }, { "FLAG1004", fo.FLAG1004 }, { "SFRC1", fo.SFRC1 }, { "SFRC2", fo.SFRC2 },
            { "SFRC3", fo.SFRC3 }, { "SFRC4", fo.SFRC4 }, { "STKARR", fo.STKARR }, { "DLGREC", fo.DLGREC },
            { "AIM0", fo.AIM0 }, { "FBITS", fo.FBITS }, { "FROWPTR", (void*)forceMaskRow }, { "SER_AIM", (void*)forceSerAIMasks } };
        for (auto& e : fsyms) Eval::add(e.n, e.v);
        // u16 area of every hex, at the same byte offset as its HEX20 record (+4): the 7-bit field
        // in HEX20 dword+4 bits 5..11 cannot hold more than 128 areas
        uint8_t* a = (uint8_t*)VirtualAlloc(nullptr, cells * 20 + 2 * pad20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!a) { logf("ERROR: AREA20 allocation failed"); return; }
        Eval::add("AREA20", a + pad20);
        g_area20 = a + pad20;
        bad += loadPatches(L"worldmod_bases.txt");
        bad += loadCaves(L"worldmod_bases_caves.txt", true);
    }
    if (g_cfg.worldTerrain) {
        if (!terrainAllocate(g_cfg.H, g_cfg.W, g_terrain, logf)) { logf("ERROR: terrain allocation failed. Patches disabled."); return; }
        const TerrainLayout& t = g_terrain;
        num("VR", t.VR); num("VC", t.VC); num("QR", t.QR); num("QC", t.QC);
        num("NCR", t.NCR); num("NCC", t.NCC); num("CSH", t.CSH);
        num("NGR", t.NGR); num("NGC", t.NGC); num("GSH", t.GSH);
        num("QB", (intptr_t)t.QB); num("VB", (intptr_t)t.VB); num("GB", (intptr_t)t.GB); num("CB", (intptr_t)t.CB);
        num("CST", (intptr_t)1 << t.CSH); num("GST", (intptr_t)1 << t.GSH);
        num("S", 0x3260860); num("TG", 0x3260868);
        g_faXmax = 5.0f * t.VR + 2560; g_faZmax = 5.0f * t.VC + 2560;        // normal mode: eye x/z upper bound
        g_faXmax16 = 5.0f * t.VR - 645; g_faZmax16 = 5.0f * t.VC - 545;     // mode 0x16: look-at x/z upper bound
        Eval::add("FA_XMAX", &g_faXmax); Eval::add("FA_ZMAX", &g_faZmax);
        Eval::add("FA_XMAX16", &g_faXmax16); Eval::add("FA_ZMAX16", &g_faZmax16);
        g_mmVC = t.VC; g_mmMaxLo = t.NCR - MM_CELLS; g_mmMaxHi = t.NCC - MM_CELLS;
        minimapSetWindow(g_cfg.Y0, g_cfg.X0);                      // the radar starts on China, as in Koei's game
        Eval::add("MMLO", &g_mmLo); Eval::add("MMHI", &g_mmHi);
        Eval::add("MMVR", &g_mmVr); Eval::add("MMVC", &g_mmVc); Eval::add("MMVOFF", &g_mmVoff);
        if ((uintptr_t)t.CB < 0x80000000u && (uintptr_t)t.CB + ((size_t)t.NCR << t.CSH) * 10 > 0x80000000u)
            logf("WARNING: cell array straddles 0x80000000 (0x57ef36 compares pointers signed)");
        bad += loadPatches(L"worldmod_terrain.txt");
        bad += loadCaves(L"worldmod_terrain_caves.txt", true);
    }
    if (bad) { logf("ERROR: rejected entries; nothing written"); return; }
    if (g_cfg.verifyOnly) { logf("verify_only: nothing written"); return; }
    commitPatches();
    commitCaves();
    installHooks();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) init();
    // debug registers are per thread: arm the watchpoint on every thread the game starts
    if (reason == DLL_THREAD_ATTACH && (g_cfg.watch || g_cfg.watchObjList)) RaiseException(WATCH_ARM, 0, 0, nullptr);
    return TRUE;
}
