// Hex-grid A* at 0x567520 (cdecl): node* A*(unit*, u32 goal, const u32* sources, int count)
// Coordinates are packed (lo word = row, hi word = column). Nodes live in HEX28 (28 bytes per hex):
//   +0 / +4  open-list tree links      +8  parent node       +0xc packed coordinate
//   +0x10    flags: 1 open, 2 closed, 4 on the returned path
//   +0x14    g (cost so far)           +0x18 f = g + h, h = |d row| + |d column| to the goal
// Returns the goal node (after marking the path with flag 4), or 0 when the goal cannot be reached.
//
// The original keeps the open list in an unbalanced binary tree ordered by f (ties go right, the
// leftmost node is expanded next). Expansion order is therefore (f, insertion order). On a big map the
// tree degenerates and every insert walks a long chain. The replacement below uses a binary heap keyed
// by (f, insertion counter): the same expansion order, so every node ends with the same parent, g, f
// and flags (mode 1). Mode 2 also doubles h: still admissible and consistent (a step costs at least 4,
// a step changes |d row| + |d column| by at most 2), so the path cost stays optimal, but far fewer
// hexes are expanded; among equally short paths it may pick a different one. Mode 3 adds a search
// window: only hexes within max(margin, half the box's longer side) of the bounding box of the sources
// and the goal are searched (margin default 100, half the original map); when the goal is not found
// inside the window the search is repeated on the whole map, so "reachable" means the same as before.
// Step costs include large penalties (0x40..0x100 next to units and buildings), so a search whose goal
// is hard to reach expands every hex with f below the goal's cost: at most 40 000 hexes on the original
// 200x200 map, but over a million on the Eurasia map (~0.8 s).
//
// Measurement and checks (worldmod.ini): astar_stats=1 logs slow calls; astar_verify=1 runs the original
// first on every call and compares HEX28 byte for byte; automation "astar bench <lo> <hi>" times both
// versions from the next real call's unit and sources to that goal.
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <vector>
#include "pathfind.h"

static TraceLogFn g_log = nullptr;
static PathfindConfig g_cfg = {};
static uint8_t* g_tramp = nullptr;          // original A*: copied prologue + jmp back
static uint32_t g_savedRet = 0, g_args = 0;
static LARGE_INTEGER g_freq, g_lastSummary;
static uint64_t g_calls = 0, g_ticks = 0, g_maxTicks = 0, g_fails = 0, g_slow = 0, g_verified = 0, g_mismatch = 0;
static void** g_pages = nullptr;
static volatile LONG g_benchPending = 0;
static uint32_t g_benchGoal = 0, g_lastGoal = 0;
static int g_benchSlowLeft = 10;

// ------------------------------------------------------------------ game functions and data
typedef void* (__thiscall* UnitTFn)(void* unit);                                  // 0x56dba0
typedef void (__cdecl* MoveFlagsFn)(uint32_t* flags, void* t);                    // 0x5a3680
typedef void (__thiscall* NeighborFn)(void* map, uint32_t* out, uint32_t c, int dir);   // 0x483a50
typedef uint8_t (__thiscall* StepCostFn)(void* unit, const uint8_t* rec);        // 0x56dd00
typedef int (__cdecl* ZocFn)(void* t, uint32_t c);                                // 0x567440
typedef void* (__thiscall* RecUnitFn)(const uint8_t* rec);                        // 0x483b20
typedef int (__thiscall* RelFn)(void* t, void* other);                            // 0x496c50
typedef int (__thiscall* UnitTestFn)(void* u);                                    // 0x487e10, 0x487af0
typedef int (__cdecl* UnitPairFn)(void* t, void* u);                              // 0x5673e0
typedef void* (__thiscall* BuildingFn)(void* mgr, uint32_t id);                   // 0x490e70

static const UnitTFn unitT = (UnitTFn)0x56dba0;
static const MoveFlagsFn moveFlags = (MoveFlagsFn)0x5a3680;
static const NeighborFn neighbor = (NeighborFn)0x483a50;
static const StepCostFn stepCost = (StepCostFn)0x56dd00;
static const ZocFn zoc = (ZocFn)0x567440;
static const RecUnitFn recUnit = (RecUnitFn)0x483b20;
static const RelFn related = (RelFn)0x496c50;
static const UnitTestFn unitTestA = (UnitTestFn)0x487e10, unitTestB = (UnitTestFn)0x487af0;
static const UnitPairFn unitPair = (UnitPairFn)0x5673e0;
static const BuildingFn building = (BuildingFn)0x490e70;
static const int32_t* PASSABLE = (const int32_t*)0x8a5df8;      // per terrain type

// addresses and sizes as the patch tables left them in the original function
static uint8_t* HEX20 = nullptr;   // 0x5676c3 mov ecx, imm32
static uint8_t* HEX28 = nullptr;   // 0x567591 add eax, imm32
static int W = 200, H = 200;       // 0x56757f imul eax,eax,W ; 0x5676df cmp eax,H ; 0x5676f2 cmp ecx,W
static void* BMGR = nullptr;       // 0x567809 mov ecx, imm32

#pragma pack(push, 1)
struct Node {
    uint32_t left, right;   // original: tree links; here +0 holds heap index + 1 while open
    Node* parent;
    uint32_t coord;
    uint8_t flags, pad[3];
    int32_t g, f;
};
#pragma pack(pop)
static_assert(sizeof(Node) == 28, "HEX28 node");

static inline Node* nodeAt(int lo, int hi) { return (Node*)(HEX28 + ((size_t)lo * W + hi) * 28); }
static inline int manhattan(int lo, int hi, int glo, int ghi) { return abs(glo - lo) + abs(ghi - hi); }

// ------------------------------------------------------------------ open list: binary heap on (f, seq)
struct HeapItem { int32_t f; uint32_t seq; Node* n; };
static std::vector<HeapItem> g_heap;
static uint32_t g_seq = 0;

static inline bool heapLess(const HeapItem& a, const HeapItem& b) { return a.f < b.f || (a.f == b.f && a.seq < b.seq); }
static inline void place(size_t i, const HeapItem& it) { g_heap[i] = it; it.n->left = (uint32_t)i + 1; }

static void siftUp(size_t i) {
    HeapItem it = g_heap[i];
    while (i) {
        size_t p = (i - 1) / 2;
        if (!heapLess(it, g_heap[p])) break;
        place(i, g_heap[p]); i = p;
    }
    place(i, it);
}
static void siftDown(size_t i) {
    HeapItem it = g_heap[i];
    size_t n = g_heap.size();
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && heapLess(g_heap[c + 1], g_heap[c])) c++;
        if (!heapLess(g_heap[c], it)) break;
        place(i, g_heap[c]); i = c;
    }
    place(i, it);
}
static void push(Node* n) { g_heap.push_back({ n->f, g_seq++, n }); siftUp(g_heap.size() - 1); }
static void removeAt(size_t i) {
    g_heap[i].n->left = 0;
    HeapItem last = g_heap.back(); g_heap.pop_back();
    if (i == g_heap.size()) return;
    place(i, last);
    if (i && heapLess(last, g_heap[(i - 1) / 2])) siftUp(i); else siftDown(i);
}
static Node* popMin() { Node* n = g_heap[0].n; removeAt(0); return n; }

// ------------------------------------------------------------------ replacement
static Node* astarHeap(void* unit, uint32_t goal, const uint32_t* src, int count, int hscale, int margin) {
    void* t = unitT(unit);
    uint32_t mf[4];
    moveFlags(mf, t);
    g_cfg.clear28();
    g_heap.clear(); g_seq = 0;
    const int glo = (short)goal, ghi = (short)(goal >> 16);
    int lo0 = 0, lo1 = H - 1, hi0 = 0, hi1 = W - 1;          // search window
    bool windowed = false;
    if (margin > 0) {
        lo0 = lo1 = glo; hi0 = hi1 = ghi;
        for (int i = 0; i < count; i++) {
            int lo = (short)src[i], hi = (short)(src[i] >> 16);
            lo0 = min(lo0, lo); lo1 = max(lo1, lo); hi0 = min(hi0, hi); hi1 = max(hi1, hi);
        }
        // a long, flat box must not cut off a detour around a sea or a mountain range
        int m = max(margin, max(lo1 - lo0, hi1 - hi0) / 2);
        lo0 = max(0, lo0 - m); lo1 = min(H - 1, lo1 + m);
        hi0 = max(0, hi0 - m); hi1 = min(W - 1, hi1 + m);
        windowed = lo0 > 0 || lo1 < H - 1 || hi0 > 0 || hi1 < W - 1;
    }
    for (int i = 0; i < count; i++) {
        uint32_t c = src[i];
        int lo = (short)c, hi = (short)(c >> 16);
        Node* n = nodeAt(lo, hi);
        if (n->flags & 1) continue;                     // a repeated source (the original would corrupt its tree)
        n->coord = c;
        n->f = manhattan(lo, hi, glo, ghi) * hscale;
        if (*(const uint32_t*)(HEX20 + ((size_t)lo * W + hi) * 20) & 3) n->f += 0x40;
        n->flags = 1;
        push(n);
    }
    while (!g_heap.empty()) {
        Node* cur = popMin();
        if ((short)cur->coord == (short)goal && (short)(cur->coord >> 16) == (short)(goal >> 16)) {
            for (Node* p = cur; p; p = p->parent) p->flags |= 4;
            return cur;
        }
        for (int dir = 0; dir < 6; dir++) {
            uint32_t nc = 0;
            neighbor(HEX20, &nc, cur->coord, dir);
            int lo = (short)nc, hi = (short)(nc >> 16);
            if (lo < lo0 || lo > lo1 || hi < hi0 || hi > hi1) continue;
            const uint8_t* rec = HEX20 + ((size_t)lo * W + hi) * 20;
            uint32_t terr = *(const uint32_t*)(rec + 4) & 0x1f;
            if (!PASSABLE[terr]) continue;
            int cost = stepCost(unit, rec);
            switch (*(const uint32_t*)rec & 3) {
            case 1: {                                   // building
                void* b = building(BMGR, *(const uint16_t*)(rec + 8));
                if (related(t, b)) cost += 0x20;
                break;
            }
            case 2: {                                   // unit
                void* u = recUnit(rec);
                if (related(t, u)) cost += unitTestA(u) ? 0x100 : 0x80;
                else if (unitTestB(u)) cost += unitPair(t, u) ? 0x100 : 0x40;
                else cost += 0x10;
                break;
            }
            default:
                if (terr == 4) { if (!mf[2]) cost += 0x40; }
                else if (terr == 0xb) { if (!mf[3]) cost += 0x40; }
                if (terr == 7 || terr == 8) { if (mf[1]) cost += zoc(unitT(unit), nc); }
                else if (mf[0]) cost += zoc(unitT(unit), nc);
            }
            Node* nb = nodeAt(lo, hi);
            int32_t ng = cur->g + cost;
            uint8_t fl = nb->flags;
            if ((fl & 3) && nb->g <= ng) continue;
            fl &= ~2;
            nb->coord = nc; nb->parent = cur; nb->flags = fl;
            int32_t h = manhattan(lo, hi, glo, ghi) * hscale;
            if (fl & 1) removeAt(nb->left - 1);
            nb->g = ng; nb->f = ng + h;
            push(nb);
            nb->flags |= 1;
        }
        cur->flags = (cur->flags & ~1) | 2;
    }
    if (windowed) return astarHeap(unit, goal, src, count, hscale, 0);   // not inside the window: whole map
    return nullptr;
}

// ------------------------------------------------------------------ dispatch, verification, statistics
typedef Node* (__cdecl* AstarFn)(void*, uint32_t, const uint32_t*, int);

static size_t dirtyPages(bool reset) {
    ULONG_PTR n = g_cfg.hex28Size / 4096; DWORD gran = 4096;
    if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, g_cfg.hex28, g_cfg.hex28Size, g_pages, &n, &gran) != 0) return 0;
    return n;
}

static double msSince(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    return 1000.0 * (t1.QuadPart - t0.QuadPart) / g_freq.QuadPart;
}

// Runs the original, keeps a copy of every page it wrote, runs the replacement and compares all node
// fields except the open-list links (+0, +4).
static Node* verifyCall(void* unit, uint32_t goal, const uint32_t* src, int count) {
    static std::vector<uint8_t> snap; static std::vector<uint8_t*> pages;
    Node* a = ((AstarFn)(void*)g_tramp)(unit, goal, src, count);
    size_t np = dirtyPages(false);
    pages.assign((uint8_t**)g_pages, (uint8_t**)g_pages + np);
    snap.resize(np * 4096);
    for (size_t i = 0; i < np; i++) memcpy(&snap[i * 4096], pages[i], 4096);
    Node* b = astarHeap(unit, goal, src, count, 1, 0);
    size_t nb = dirtyPages(false);
    int diffs = 0; uint32_t firstAt = 0;
    std::vector<uint8_t*> all(pages); all.insert(all.end(), (uint8_t**)g_pages, (uint8_t**)g_pages + nb);
    for (size_t k = 0; k < all.size(); k++) {
        uint8_t* p = all[k];
        const uint8_t* old = nullptr;
        for (size_t i = 0; i < pages.size(); i++) if (pages[i] == p) { old = &snap[i * 4096]; break; }
        for (int i = 0; i < 4096; i++) {
            if (p + i < HEX28 || (size_t)(p + i - HEX28) % 28 < 8) continue;
            uint8_t o = old ? old[i] : 0;
            if (o != p[i]) { if (!diffs++) firstAt = (uint32_t)(uintptr_t)(p + i); }
        }
    }
    bool sameResult = (a == b);
    g_verified++;
    if (diffs || !sameResult) {
        g_mismatch++;
        if (g_mismatch <= 20)
            g_log("A* verify MISMATCH: goal (%d,%d) %d sources: original %p, heap %p; %d differing bytes, first at %08x",
                  (short)goal, (short)(goal >> 16), count, a, b, diffs, firstAt);
    }
    return b;
}

static void bench(void* unit, const uint32_t* src, int count, void* caller) {
    uint32_t goal = g_benchGoal ? g_benchGoal : g_lastGoal;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    Node* a = ((AstarFn)(void*)g_tramp)(unit, goal, src, count);
    double ta = msSince(t0); size_t pa = dirtyPages(false);
    int32_t ga = a ? a->g : -1;
    QueryPerformanceCounter(&t0);
    Node* b = astarHeap(unit, goal, src, count, 1, 0);
    double tb = msSince(t0); size_t pb = dirtyPages(false);
    int32_t gb = b ? b->g : -1;
    QueryPerformanceCounter(&t0);
    Node* c = astarHeap(unit, goal, src, count, 2, 0);
    double tc = msSince(t0); size_t pc = dirtyPages(false);
    int32_t gc = c ? c->g : -1;
    QueryPerformanceCounter(&t0);
    Node* e = astarHeap(unit, goal, src, count, 2, g_cfg.margin);
    double te = msSince(t0); size_t pe = dirtyPages(false);
    int32_t ge = e ? e->g : -1;
    uint32_t s = count > 0 ? src[0] : 0;
    g_log("A* bench (%d,%d) -> (%d,%d), %d sources, caller %p: original %.1f ms (cost %d, %zu pages) | heap %.1f ms (cost %d, %zu pages) | "
          "heap+2h %.1f ms (cost %d, %zu pages) | +window %.1f ms (cost %d, %zu pages)",
          (short)s, (short)(s >> 16), (short)goal, (short)(goal >> 16), count, caller, ta, ga, pa, tb, gb, pb, tc, gc, pc, te, ge, pe);
}

// Some callers search from the hexes around an object whose position was never set, i.e. (0,0). On
// Koei's map that is China's own corner and the search stays small; in the world it is the far
// north-west corner, and the search floods the whole map (~0.85 s). Such corner positions are moved to
// China's corner, which is what the original game searched from (the world corner is open sea).
static inline bool atCorner(uint32_t c) { return (short)c >= 0 && (short)c <= 1 && (short)(c >> 16) >= 0 && (short)(c >> 16) <= 1; }
static inline uint32_t toChina(uint32_t c) {
    return (uint32_t)(uint16_t)((short)c + g_cfg.chinaLo) | ((uint32_t)(uint16_t)((short)(c >> 16) + g_cfg.chinaHi) << 16);
}
static volatile LONG g_cornerFixes = 0;
static void noteCorner(void* caller, const void* frame) {
    LONG n = InterlockedIncrement(&g_cornerFixes);
    if (n > 5 && (n & (n - 1))) return;
    char buf[400]; int k = 0;
    const uint32_t* sp = (const uint32_t*)frame;
    for (int i = 0; i < 160 && k < 360; i++)
        if (sp[i] >= 0x401000 && sp[i] < 0x74f000) k += sprintf_s(buf + k, sizeof buf - k, " %08x", sp[i]);
    buf[k] = 0;
    g_log("A*: start/goal at the world corner (0,0) moved to China's corner, #%ld, caller %p | stack:%s", n, caller, buf);
}

extern "C" static Node* __cdecl astarDispatch(void* unit, uint32_t goal, const uint32_t* src, int count) {
    void* caller = _ReturnAddress();
    uint32_t fixed[64];
    if ((g_cfg.chinaLo || g_cfg.chinaHi) && count > 0 && count <= 64) {
        bool all = true;
        for (int i = 0; i < count && all; i++) all = atCorner(src[i]);
        if (all || atCorner(goal)) {
            noteCorner(caller, _AddressOfReturnAddress());
            for (int i = 0; i < count; i++) fixed[i] = all ? toChina(src[i]) : src[i];
            if (atCorner(goal)) goal = toChina(goal);
            src = fixed;
        }
    }
    // bench: an explicit goal at the next call, or (g_benchSlow) the next call that is slow in the original
    if (g_cfg.enterLog) {
        uint32_t s = count > 0 ? src[0] : 0;
        g_log("A* enter (%d,%d) -> (%d,%d) %d src caller %p",
              (short)s, (short)(s >> 16), (short)goal, (short)(goal >> 16), count, caller);
    }
    if (InterlockedExchange(&g_benchPending, 0)) bench(unit, src, count, caller);
    if (g_cfg.benchGoal && (g_calls == 0 || g_calls == 19 || g_calls == 99 || g_calls == 299)) {
        g_benchGoal = g_cfg.benchGoal - 1;
        bench(unit, src, count, caller);
        g_benchGoal = 0;
    }
    g_lastGoal = goal;
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    Node* r;
    if (g_cfg.verify) r = verifyCall(unit, goal, src, count);
    else if (g_cfg.mode == 0) r = ((AstarFn)(void*)g_tramp)(unit, goal, src, count);
    else r = astarHeap(unit, goal, src, count, g_cfg.mode >= 2 ? 2 : 1, g_cfg.mode >= 3 ? g_cfg.margin : 0);
    if (g_cfg.stats) {
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        uint64_t d = (uint64_t)(t1.QuadPart - t0.QuadPart);
        g_calls++; g_ticks += d; if (d > g_maxTicks) g_maxTicks = d;
        if (!r) g_fails++;
        double ms = 1000.0 * d / g_freq.QuadPart;
        if (ms >= 5.0) {
            g_slow++;
            uint32_t s = count > 0 ? src[0] : 0;
            size_t pages = dirtyPages(false);
            g_log("A*: %.1f ms %s  (%d,%d) -> (%d,%d)  sources %d  %zu HEX28 pages touched, caller %p",
                  ms, r ? "found" : "NOT FOUND", (short)s, (short)(s >> 16), (short)goal, (short)(goal >> 16), count, pages, caller);
            if (g_cfg.benchSlow && g_benchSlowLeft > 0) {     // replay this call with every variant
                g_benchSlowLeft--;
                g_benchGoal = goal;
                bench(unit, src, count, caller);
                g_benchGoal = 0;
                r = g_cfg.mode == 0 ? ((AstarFn)(void*)g_tramp)(unit, goal, src, count)
                                    : astarHeap(unit, goal, src, count, g_cfg.mode >= 2 ? 2 : 1, g_cfg.mode >= 3 ? g_cfg.margin : 0);
            }
        }
        if ((t1.QuadPart - g_lastSummary.QuadPart) > 10 * g_freq.QuadPart) {
            g_lastSummary = t1;
            g_log("A* summary (mode %d): %llu calls, %.0f ms total, max %.1f ms, %llu not found, %llu slower than 5 ms; verified %llu, mismatches %llu",
                  g_cfg.mode, g_calls, 1000.0 * g_ticks / g_freq.QuadPart, 1000.0 * g_maxTicks / g_freq.QuadPart, g_fails, g_slow, g_verified, g_mismatch);
        }
    }
    return r;
}

void pathfindBench(int lo, int hi) {
    g_benchGoal = (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16);
    InterlockedExchange(&g_benchPending, 1);
}

bool pathfindInstall(const PathfindConfig& cfg, TraceLogFn log) {
    g_cfg = cfg; g_log = log;
    if (!cfg.mode && !cfg.stats && !cfg.enterLog && !cfg.verify && !cfg.benchable) return false;
    uint8_t* site = (uint8_t*)0x567520;
    static const uint8_t expect[7] = { 0x83, 0xec, 0x24, 0x8b, 0x4c, 0x24, 0x28 };   // sub esp,24h; mov ecx,[esp+28h]
    if (memcmp(site, expect, 7) || site[0x5676c3 - 0x567520] != 0xB9 || site[0x567591 - 0x567520] != 0x05 ||
        site[0x567809 - 0x567520] != 0xB9 || site[0x5676df - 0x567520] != 0x3D) {
        log("A*: unexpected code at 0x567520, not hooked"); return false;
    }
    HEX20 = *(uint8_t**)(0x5676c3 + 1);
    HEX28 = *(uint8_t**)(0x567591 + 1);
    W = *(int*)(0x56757f + 2);
    H = *(int*)(0x5676df + 1);
    int W2 = *(int*)(0x5676f2 + 2);
    BMGR = *(void**)(0x567809 + 1);
    if (W2 != W) { log("A*: row/column bounds disagree (%d vs %d), not hooked", W, W2); return false; }
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_lastSummary);
    g_pages = (void**)VirtualAlloc(nullptr, cfg.hex28Size / 4096 * sizeof(void*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_heap.reserve(1 << 16);
    g_tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    memcpy(g_tramp, expect, 7);
    g_tramp[7] = 0xE9; *(int32_t*)(g_tramp + 8) = (int32_t)(0x567527 - (uintptr_t)(g_tramp + 12));
    DWORD old; VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xE9; *(int32_t*)(site + 1) = (int32_t)((uintptr_t)astarDispatch - (uintptr_t)(site + 5));
    site[5] = 0x90; site[6] = 0x90;
    VirtualProtect(site, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 7);
    static const char* names[] = { "original", "binary heap, same expansion order", "binary heap, 2x heuristic",
                                   "binary heap, 2x heuristic, search window" };
    log("A*: hooked (mode %d: %s, margin %d; stats %d, verify %d), HEX20 %p HEX28 %p, %dx%d", cfg.mode,
        names[cfg.mode < 0 || cfg.mode > 3 ? 0 : cfg.mode], cfg.margin, cfg.stats, cfg.verify, HEX20, HEX28, W, H);
    return true;
}
