// automation: lets a test script drive the game without touching the real desktop.
//
// Enabled with `automation=1` in the exe's worldmod.ini section. A background thread polls
// <game>\worldmod_cmd.txt; each line is one command, executed in order, then the file is deleted
// and <game>\worldmod_cmd.done is written with one result line per command.
//
//   shot <file.bmp>          capture the next rendered frame (back buffer) to a 24-bit BMP
//   move <x> <y>             move the virtual cursor (client coordinates)
//   click <x> <y>            move + left button down/up      rclick <x> <y>   same, right button
//   down / up                left button down / up at the current position
//   wheel <delta>            mouse wheel at the current position (120 per notch, negative = towards the user)
//   drag <x1> <y1> <x2> <y2> left-drag in 10 steps
//   key <vk>                 press and release a virtual key (decimal or 0x hex)
//   wait <ms>                sleep
//   mem <hexaddr> <len>      hex dump of game memory
//   pixel <x> <y>            colour of the next frame at (x, y)
//   skip [ms]                click through dialogs and event scenes until the map is idle (see skipDialogs)
//
// While enabled, the game's view of the cursor, mouse buttons, keyboard and window focus comes
// from here (IAT hooks), and ClipCursor / SetForegroundWindow are ignored, so the real mouse and
// the user's foreground window are never affected.
//
// auto_skip=1 (normal play, no automation): when a map stage loads (new game, loaded save), the same
// skipper clicks through the opening events and dialogs; the game sees the virtual input only meanwhile.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "automation.h"
#include "profiler.h"
#include "pathfind.h"
#include "radar.h"
#include "units.h"

namespace {

TraceLogFn g_log = nullptr;
wchar_t g_dir[MAX_PATH];
HWND g_hwnd = nullptr;
volatile LONG g_cx = 400, g_cy = 300;          // virtual cursor, client coordinates
volatile LONG g_lbutton = 0, g_rbutton = 0;
volatile LONG g_keys[256];
HANDLE g_shotDone = nullptr;
wchar_t g_shotPath[MAX_PATH];
volatile LONG g_shotPending = 0;
volatile LONG g_nCursor = 0, g_nAsync = 0, g_nKeyState = 0, g_nFg = 0;
volatile LONG g_fake = 1;                       // the game sees the virtual input (automation: always; auto_skip: while skipping)
typedef BOOL(WINAPI* GetCursorPosFn)(LPPOINT);
typedef SHORT(WINAPI* KeyStateFn)(int);
GetCursorPosFn g_realCursorPos = nullptr;
KeyStateFn g_realAsyncKeyState = nullptr, g_realKeyState = nullptr;

// ---- IAT hooks (addresses are san11pk.exe's import slots)
const uint32_t IAT_GetForegroundWindow = 0x74e40c, IAT_GetFocus = 0x74e468, IAT_GetActiveWindow = 0x74e4a4,
               IAT_GetKeyState = 0x74e51c, IAT_GetAsyncKeyState = 0x74e56c, IAT_ClipCursor = 0x74e570,
               IAT_GetCursorPos = 0x74e5a8, IAT_SetForegroundWindow = 0x74e600, IAT_IsIconic = 0x74e5f4;

BOOL CALLBACK findWindow(HWND h, LPARAM) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    char cls[64];
    if (pid == GetCurrentProcessId() && GetClassNameA(h, cls, sizeof cls) && !strcmp(cls, "KOEI_SAN_11_WINDOW")) { g_hwnd = h; return FALSE; }
    return TRUE;
}
HWND window() { if (!g_hwnd || !IsWindow(g_hwnd)) { g_hwnd = nullptr; EnumWindows(findWindow, 0); } return g_hwnd; }

BOOL WINAPI hGetCursorPos(LPPOINT p) {
    if (!g_fake) return g_realCursorPos(p);
    InterlockedIncrement(&g_nCursor);
    if (!p) return FALSE;
    p->x = g_cx; p->y = g_cy;
    if (HWND w = window()) ClientToScreen(w, p);
    return TRUE;
}
SHORT keyState(int vk) {
    if (vk == VK_LBUTTON) return g_lbutton ? (SHORT)0x8000 : 0;
    if (vk == VK_RBUTTON) return g_rbutton ? (SHORT)0x8000 : 0;
    return (vk >= 0 && vk < 256 && g_keys[vk]) ? (SHORT)0x8000 : 0;
}
SHORT WINAPI hGetAsyncKeyState(int vk) { if (!g_fake) return g_realAsyncKeyState(vk); InterlockedIncrement(&g_nAsync); return keyState(vk); }
SHORT WINAPI hGetKeyState(int vk) { if (!g_fake) return g_realKeyState(vk); InterlockedIncrement(&g_nKeyState); return keyState(vk); }
HWND WINAPI hGetForegroundWindow() { InterlockedIncrement(&g_nFg); return window(); }
HWND WINAPI hGetActiveWindow() { return window(); }
HWND WINAPI hGetFocus() { return window(); }
BOOL WINAPI hClipCursor(const RECT*) { return TRUE; }
BOOL WINAPI hSetForegroundWindow(HWND) { return TRUE; }
BOOL WINAPI hIsIconic(HWND) { return FALSE; }   // keep rendering while the test window is minimized

void hookIat(uint32_t slot, const void* fn) {
    DWORD old; VirtualProtect((void*)(uintptr_t)slot, 4, PAGE_READWRITE, &old);
    *(const void**)(uintptr_t)slot = fn;
    VirtualProtect((void*)(uintptr_t)slot, 4, old, &old);
}

// ---- input
LPARAM xy() { return MAKELPARAM(g_cx, g_cy); }
WPARAM buttons() { return (g_lbutton ? MK_LBUTTON : 0) | (g_rbutton ? MK_RBUTTON : 0); }
void post(UINT m, WPARAM w, LPARAM l) { if (HWND h = window()) PostMessageA(h, m, w, l); }
void moveTo(int x, int y) { g_cx = x; g_cy = y; post(WM_MOUSEMOVE, buttons(), xy()); Sleep(30); }
void left(bool down) { g_lbutton = down; post(down ? WM_LBUTTONDOWN : WM_LBUTTONUP, buttons(), xy()); Sleep(60); }
void right(bool down) { g_rbutton = down; post(down ? WM_RBUTTONDOWN : WM_RBUTTONUP, buttons(), xy()); Sleep(60); }
void key(int vk) {
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    g_keys[vk & 255] = 1; post(WM_KEYDOWN, vk, 1 | (sc << 16)); Sleep(80);
    g_keys[vk & 255] = 0; post(WM_KEYUP, vk, 1 | (sc << 16) | 0xC0000000); Sleep(40);
}

// ---- screenshot (runs on the render thread from EndScene)
bool writeBmp(const wchar_t* path, const uint8_t* bgrx, int w, int h, int pitch) {
    FILE* f = nullptr; _wfopen_s(&f, path, L"wb");
    if (!f) return false;
    int row = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + row * h), 0, 0,
                            sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
    BITMAPINFOHEADER ih = { sizeof ih, w, h, 1, 24, BI_RGB };
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
    uint8_t* line = (uint8_t*)calloc(row, 1);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t* s = bgrx + (size_t)y * pitch;
        for (int x = 0; x < w; x++) { line[x * 3] = s[x * 4]; line[x * 3 + 1] = s[x * 4 + 1]; line[x * 3 + 2] = s[x * 4 + 2]; }
        fwrite(line, row, 1, f);
    }
    free(line); fclose(f);
    return true;
}

void capture(IDirect3DDevice9* d) {
    IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* sys = nullptr;
    D3DSURFACE_DESC desc;
    bool ok = false;
    if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && SUCCEEDED(rt->GetDesc(&desc)) &&
        (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(d->GetRenderTargetData(rt, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            ok = writeBmp(g_shotPath, (const uint8_t*)lr.pBits, desc.Width, desc.Height, lr.Pitch);
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    if (rt) rt->Release();
    if (g_log) g_log("automation: screenshot %s", ok ? "written" : "FAILED");
}

// ---- dialog skipper
// The end-turn button (bottom-left, 30 px from the left and bottom edges) is drawn in colour only while
// the map is idle; under a dialog, a menu or a list it is grey, during event scenes and loading black.
// The skipper clicks near the top of the screen until the button has been in colour for a while.
HANDLE g_probeDone = nullptr;
volatile LONG g_probePending = 0, g_probeRGB = 0, g_rtW = 0, g_rtH = 0;
volatile LONG g_probeX = -1, g_probeY = -1;      // pixel to read (-1: the end-turn button)
IDirect3DSurface9* g_probeSys = nullptr;
volatile LONG g_skipping = 0;
bool g_automation = false, g_autoSkipOn = false;

IDirect3DSurface9* g_probeRt = nullptr;      // single-sampled copy of a multisampled render target

void probe(IDirect3DDevice9* d) {     // render thread
    IDirect3DSurface9* rt = nullptr;
    D3DSURFACE_DESC desc;
    LONG rgb = -1;
    bool known = false;
    if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && SUCCEEDED(rt->GetDesc(&desc))) {
        known = desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_R5G6B5 ||
                desc.Format == D3DFMT_X1R5G5B5 || desc.Format == D3DFMT_A1R5G5B5;
        g_rtW = desc.Width; g_rtH = desc.Height;
    }
    if (known && desc.Height > 40) {
        IDirect3DSurface9* src = rt;
        D3DSURFACE_DESC sd;
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {          // GetRenderTargetData needs a single-sampled source
            if (g_probeRt && (FAILED(g_probeRt->GetDesc(&sd)) || sd.Width != desc.Width || sd.Height != desc.Height || sd.Format != desc.Format)) {
                g_probeRt->Release(); g_probeRt = nullptr;
            }
            if (!g_probeRt) d->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_probeRt, nullptr);
            src = (g_probeRt && SUCCEEDED(d->StretchRect(rt, nullptr, g_probeRt, nullptr, D3DTEXF_NONE))) ? g_probeRt : nullptr;
        }
        if (g_probeSys && (FAILED(g_probeSys->GetDesc(&sd)) || sd.Width != desc.Width || sd.Height != desc.Height || sd.Format != desc.Format)) {
            g_probeSys->Release(); g_probeSys = nullptr;
        }
        if (!g_probeSys) d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &g_probeSys, nullptr);
        D3DLOCKED_RECT lr;
        if (src && g_probeSys && SUCCEEDED(d->GetRenderTargetData(src, g_probeSys))) {
            LONG x = g_probeX >= 0 ? g_probeX : 30, y = g_probeY >= 0 ? g_probeY : (LONG)desc.Height - 30;
            x = min(x, (LONG)desc.Width - 1); y = min(y, (LONG)desc.Height - 1);
            RECT r = { x, y, x + 1, y + 1 };
            if (SUCCEEDED(g_probeSys->LockRect(&lr, &r, D3DLOCK_READONLY))) {
                if (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8) rgb = *(const uint32_t*)lr.pBits & 0xffffff;
                else {
                    uint32_t v = *(const uint16_t*)lr.pBits, R, G, B;
                    if (desc.Format == D3DFMT_R5G6B5) { R = (v >> 11) & 31; G = (v >> 5) & 63; B = v & 31; G = G * 255 / 63; }
                    else { R = (v >> 10) & 31; G = (v >> 5) & 31; B = v & 31; G = G * 255 / 31; }
                    rgb = (LONG)(((R * 255 / 31) << 16) | (G << 8) | (B * 255 / 31));
                }
                g_probeSys->UnlockRect();
            }
        }
    }
    if (rt) rt->Release();
    g_probeRGB = rgb;
}

LONG readPixel(int x, int y, DWORD waitMs = 2000) {   // RGB of the next frame at (x, y) (-1, -1: the end-turn button); -1 = no frame
    g_probeX = x; g_probeY = y;
    ResetEvent(g_probeDone); InterlockedExchange(&g_probePending, 1);
    if (WaitForSingleObject(g_probeDone, waitMs) != WAIT_OBJECT_0) return -1;
    return g_probeRGB;
}

int mapIdle() {                       // 1 = the map is idle, 0 = something is shown over it, -1 = no frame
    if (readPixel(-1, -1) < 0) return -1;
    int r = (g_probeRGB >> 16) & 255, b = g_probeRGB & 255;
    return r > 40 && r - b > 15 ? 1 : 0;
}

// returns the number of clicks; *idle = whether the map ended up idle (else the time ran out)
// In normal play (auto_skip) the player takes over at once: a real click or a mouse move of more than
// 40 px stops the skipper (needed e.g. for an event that asks a question: clicks beside it do nothing).
bool userTookOver(const POINT& start) {
    if (g_automation || !g_realCursorPos) return false;          // automation: the test script is the user
    POINT p;
    if ((g_realAsyncKeyState(VK_LBUTTON) | g_realAsyncKeyState(VK_RBUTTON) | g_realAsyncKeyState(VK_ESCAPE) |
         g_realAsyncKeyState(VK_RETURN) | g_realAsyncKeyState(VK_SPACE)) & 0x8000) return true;
    return g_realCursorPos(&p) && (abs(p.x - start.x) > 40 || abs(p.y - start.y) > 40);
}

// One synthetic click. In normal play the game sees the virtual cursor and button only for the moment
// of the click (the game reads both when it handles the posted messages); the rest of the time the
// player's own mouse works as usual.
void skipClick(int x, int y) {
    if (!g_automation) InterlockedExchange(&g_fake, 1);
    moveTo(x, y); left(true); left(false);
    if (!g_automation) { Sleep(120); g_lbutton = g_rbutton = 0; InterlockedExchange(&g_fake, 0); }
}

int skipDialogs(int maxMs, bool* idle) {
    ULONGLONG t0 = GetTickCount64();
    int quiet = 0, clicks = 0;
    POINT start = {};
    if (g_realCursorPos) g_realCursorPos(&start);
    *idle = false;
    while (GetTickCount64() - t0 < (ULONGLONG)maxMs) {
        if (userTookOver(start)) break;
        int s = mapIdle();
        if (s == 1) {
            if (++quiet >= 5) { *idle = true; break; }    // idle for ~2 s: events can follow each other closely
            Sleep(400); continue;
        }
        quiet = 0;
        if (s == 0) { skipClick(g_rtW ? g_rtW / 2 : 400, 40); clicks++; }
        Sleep(90);
    }
    return clicks;
}

DWORD WINAPI autoSkipThread(void*) {
    Sleep(1500);
    bool idle; int n = skipDialogs(180000, &idle);
    g_lbutton = g_rbutton = 0;
    InterlockedExchange(&g_skipping, 0);
    if (g_log) g_log("auto_skip: %d clicks, %s", n, idle ? "map idle" : "stopped (player input or 180 s)");
    return 0;
}

// The movie scene's play function 0x4d3e00 skips playback when the game's movie switch [0x8a5924] is 0
// (`mov eax, [0x8a5924]` at 0x4d3e68). Until the title screen is reached that read is pointed at a zero
// in the DLL, so the launch movies are not played at all; afterwards it reads the game's switch again.
const uint32_t MOVIE_SWITCH_READ = 0x4d3e68, MOVIE_SWITCH = 0x8a5924;
LONG g_noMovie = 0;
void launchMovies(bool play) {
    uint8_t* p = (uint8_t*)(uintptr_t)MOVIE_SWITCH_READ;
    uint32_t want = play ? MOVIE_SWITCH : (uint32_t)(uintptr_t)&g_noMovie;
    if (p[0] != 0xa1) return;
    uint32_t cur = *(uint32_t*)(p + 1);
    if (cur != MOVIE_SWITCH && cur != (uint32_t)(uintptr_t)&g_noMovie) return;      // not the code we know
    DWORD old;
    if (VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { *(uint32_t*)(p + 1) = want; VirtualProtect(p + 1, 4, old, &old); }
}

// At launch Koei plays the KOEI logo and the opening movie outside Direct3D (no frames are rendered
// meanwhile); a click skips each of them. Click until the title screen (sky and mountains in the upper
// part of the picture, without the big gold logo of the "請點擊滑鼠" screen before it) has been rendered
// for a moment. Clicks go to the top right corner, where the
// title screen has nothing.
static inline int lum(LONG c) { return c < 0 ? -1 : (((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10; }
DWORD WINAPI startupSkipThread(void*) {
    for (int i = 0; i < 600 && !window(); i++) Sleep(100);
    if (InterlockedExchange(&g_skipping, 1)) return 0;
    ULONGLONG t0 = GetTickCount64();
    int bright = 0, clicks = 0;
    bool took = false;
    POINT start = {};
    if (g_realCursorPos) g_realCursorPos(&start);
    while (GetTickCount64() - t0 < 120000) {
        if (userTookOver(start)) { took = true; break; }
        int W = g_rtW ? g_rtW : 1920, H = g_rtH ? g_rtH : 1200;
        int a = lum(readPixel(W * 83 / 100, H / 5, 400)), b = lum(readPixel(W / 2, H / 8, 400));
        LONG g = readPixel(W * 3 / 8, H * 373 / 1000, 400);          // the gold 三國志11 logo of the "click the mouse" screen
        bool logo = g >= 0 && ((g >> 16) & 255) > 150 && ((g >> 16) & 255) - (g & 255) > 80;
        if (a > 60 && b > 60 && !logo) { if (++bright >= 4) break; Sleep(250); continue; }
        bright = 0;
        skipClick(W * 9 / 10, H / 12); clicks++;
        Sleep(350);
    }
    g_lbutton = g_rbutton = 0;
    launchMovies(true);                       // movies inside the game (events) play as usual
    InterlockedExchange(&g_skipping, 0);
    if (g_log) g_log("auto_skip: launch movies, %d clicks, %s", clicks,
                     bright >= 4 ? "title screen reached" : took ? "stopped: player input" : "gave up after 120 s");
    return 0;
}

// ---- command channel
void dumpMem(FILE* out, uint32_t addr, int len) {
    fprintf(out, "mem %08x:", addr);
    for (int i = 0; i < len; i++) {
        uint8_t* p = (uint8_t*)(uintptr_t)(addr + i);
        if (IsBadReadPtr(p, 1)) { fprintf(out, " ??"); continue; }
        fprintf(out, " %02x", *p);
    }
    fprintf(out, "\n");
}

void run(char* line, FILE* out) {
    char cmd[32] = {}; char arg[MAX_PATH] = {};
    int a = 0, b = 0, c = 0, e = 0;
    if (sscanf_s(line, "%31s", cmd, (unsigned)sizeof cmd) != 1) return;
    if (!strcmp(cmd, "shot") && sscanf_s(line, "%*s %259[^\r\n]", arg, (unsigned)sizeof arg) == 1) {
        MultiByteToWideChar(CP_ACP, 0, arg, -1, g_shotPath, MAX_PATH);
        ResetEvent(g_shotDone); InterlockedExchange(&g_shotPending, 1);
        DWORD r = WaitForSingleObject(g_shotDone, 5000);
        fprintf(out, "shot %s\n", r == WAIT_OBJECT_0 ? "ok" : "timeout (is the game rendering?)");
    } else if (!strcmp(cmd, "move") && sscanf_s(line, "%*s %d %d", &a, &b) == 2) { moveTo(a, b); fprintf(out, "ok\n"); }
    else if (!strcmp(cmd, "click") && sscanf_s(line, "%*s %d %d", &a, &b) == 2) { moveTo(a, b); left(true); left(false); fprintf(out, "ok\n"); }
    else if (!strcmp(cmd, "rclick") && sscanf_s(line, "%*s %d %d", &a, &b) == 2) { moveTo(a, b); right(true); right(false); fprintf(out, "ok\n"); }
    else if (!strcmp(cmd, "wheel") && sscanf_s(line, "%*s %d", &a) == 1) {          // wheel <delta>: 120 per notch, negative = towards the user
        POINT p = { g_cx, g_cy }; if (HWND w = window()) ClientToScreen(w, &p);
        post(WM_MOUSEWHEEL, MAKEWPARAM(buttons(), (short)a), MAKELPARAM(p.x, p.y)); Sleep(60); fprintf(out, "ok\n");
    }
    else if (!strcmp(cmd, "down")) { left(true); fprintf(out, "ok\n"); }
    else if (!strcmp(cmd, "up")) { left(false); fprintf(out, "ok\n"); }
    else if (!strcmp(cmd, "drag") && sscanf_s(line, "%*s %d %d %d %d", &a, &b, &c, &e) == 4) {
        moveTo(a, b); left(true);
        for (int i = 1; i <= 10; i++) moveTo(a + (c - a) * i / 10, b + (e - b) * i / 10);
        left(false); fprintf(out, "ok\n");
    } else if (!strcmp(cmd, "key") && sscanf_s(line, "%*s %i", &a) == 1) { key(a); fprintf(out, "ok\n"); }
    else if (!strcmp(cmd, "wait") && sscanf_s(line, "%*s %d", &a) == 1) { Sleep(a); fprintf(out, "ok\n"); }
    else if (!strcmp(cmd, "activate")) {
        post(WM_ACTIVATEAPP, TRUE, 0); post(WM_ACTIVATE, WA_ACTIVE, 0); post(WM_SETFOCUS, 0, 0); post(WM_NCACTIVATE, TRUE, 0);
        Sleep(200); fprintf(out, "ok\n");
    } else if (!strcmp(cmd, "restore")) {
        if (HWND w = window()) {
            ShowWindow(w, SW_SHOWNOACTIVATE);
            SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        fprintf(out, "ok\n");
    } else if (!strcmp(cmd, "camera")) {
        float x, z; automationCamera(&x, &z);
        fprintf(out, "camera look-at x=%.1f z=%.1f  (vertex %.1f, %.1f)\n", x, z, x / 5.0f, z / 5.0f);
    } else if (!strcmp(cmd, "goto")) {          // goto <lo> <hi>: move the camera over that hex (applied at the next camera update)
        int lo = 0, hi = 0;
        if (sscanf_s(line, "%*s %d %d", &lo, &hi) == 2) {
            float x = (lo * 4 + 114) * 5.0f, z = (hi * 4 + 114 + 2 * (lo & 1)) * 5.0f;
            automationGoto(x, z);
            fprintf(out, "goto hex (%d,%d) = world (%.0f, %.0f)\n", lo, hi, x, z);
        } else fprintf(out, "usage: goto <lo> <hi>\n");
    } else if (!strcmp(cmd, "stats")) {
        RECT rc = {}; if (HWND w = window()) GetClientRect(w, &rc);
        fprintf(out, "hwnd=%p client=%ldx%ld GetCursorPos=%ld GetAsyncKeyState=%ld GetKeyState=%ld GetForegroundWindow=%ld\n",
                window(), rc.right, rc.bottom, g_nCursor, g_nAsync, g_nKeyState, g_nFg);
    }
    else if (!strcmp(cmd, "mem") && sscanf_s(line, "%*s %x %d", (unsigned*)&a, &b) == 2) dumpMem(out, (uint32_t)a, b > 4096 ? 4096 : b);
    else if (!strcmp(cmd, "unitclone") && sscanf_s(line, "%*s %d %259[^\r\n]", &a, arg, (unsigned)sizeof arg) == 2)
        fprintf(out, "unitclone: %d\n", unitsClone(a, arg));       // debug (M9 tests): units.cpp
    else if (!strcmp(cmd, "wild") && sscanf_s(line, "%*s %d", &a) == 1) {   // wild <city>: 在野/未登场/未发现武将都去这个城市
        int moved = 0;
        for (int i = 0; i < 1100; i++) {
            uint8_t* p = (uint8_t*)(uintptr_t)(0x720DA14 + (size_t)i * 0x190);
            int status = *(int*)(p + 0xa0);
            if (status == 4 || status == 6 || status == 7) {       // 在野 / 未登场 / 未发现
                *(int*)(p + 0x98) = a;                              // 所在
                *(int*)(p + 0x9c) = a;                              // 位置
                moved++;
            }
        }
        fprintf(out, "wild: %d officers moved to city %d\n", moved, a);
    }
    else if (!strcmp(cmd, "radardump") && sscanf_s(line, "%*s %259[^\r\n]", arg, (unsigned)sizeof arg) == 1)
        fprintf(out, "radardump: %d\n", radarDump(arg));
    else if (!strcmp(cmd, "poke") && sscanf_s(line, "%*s %x %x", (unsigned*)&a, (unsigned*)&b) == 2) {   // poke <addr> <dword>, hex
        DWORD old;
        if (VirtualProtect((void*)(uintptr_t)(uint32_t)a, 4, PAGE_READWRITE, &old)) {
            *(uint32_t*)(uintptr_t)(uint32_t)a = (uint32_t)b; VirtualProtect((void*)(uintptr_t)(uint32_t)a, 4, old, &old);
            fprintf(out, "ok\n");
        } else fprintf(out, "cannot write %08x\n", (uint32_t)a);
    }
    else if (!strcmp(cmd, "pixel") && sscanf_s(line, "%*s %d %d", &a, &b) == 2) {
        LONG c = readPixel(a, b);
        if (c < 0) fprintf(out, "pixel: no frame\n"); else fprintf(out, "pixel %d %d = %d %d %d\n", a, b, (c >> 16) & 255, (c >> 8) & 255, c & 255);
    }
    else if (!strcmp(cmd, "skip")) {
        if (sscanf_s(line, "%*s %d", &a) != 1) a = 120000;
        bool idle; int n = skipDialogs(a, &idle);
        fprintf(out, "skip: %d clicks, %s\n", n, idle ? "map idle" : "timeout");
    }
    else if (!strcmp(cmd, "astar") && sscanf_s(line, "%*s bench %d %d", &a, &b) == 2) {
        pathfindBench(a, b);
        fprintf(out, "bench armed: runs at the next A* call\n");
    }
    else if (!strcmp(cmd, "prof") && sscanf_s(line, "%*s %31s", arg, (unsigned)sizeof arg) == 1 && !strcmp(arg, "start"))
        fprintf(out, profilerStart() ? "ok\n" : "profiler already running\n");
    else if (!strcmp(cmd, "prof") && sscanf_s(line, "%*s stop %259[^\r\n]", arg, (unsigned)sizeof arg) == 1)
        fprintf(out, "prof: %d samples\n", profilerStop(arg));
    else fprintf(out, "unknown: %s", line);
}

// The real window is minimized / in the background while tests run, so Windows keeps telling the game
// it lost activation and the game then ignores input. Present every activation message as "active".
WNDPROC g_origProc = nullptr;
LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ACTIVATEAPP: w = TRUE; break;
    case WM_ACTIVATE: w = WA_ACTIVE; l = 0; break;
    case WM_NCACTIVATE: w = TRUE; break;
    case WM_KILLFOCUS: return 0;
    }
    return CallWindowProcA(g_origProc, h, m, w, l);
}

DWORD WINAPI commandThread(void*) {
    if (g_automation) {                                     // keep the game window out of the user's way (tests only)
        for (int i = 0; i < 300 && !window(); i++) Sleep(100);
        if (HWND w = window()) {
            SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            g_origProc = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)wndProc);
            post(WM_ACTIVATEAPP, TRUE, 0); post(WM_ACTIVATE, WA_ACTIVE, 0); post(WM_SETFOCUS, 0, 0);
            if (g_log) g_log("automation: window %p subclassed", w);
        }
    }
    wchar_t cmdPath[MAX_PATH], donePath[MAX_PATH], tmpPath[MAX_PATH];
    swprintf_s(cmdPath, L"%s\\worldmod_cmd.txt", g_dir);
    swprintf_s(donePath, L"%s\\worldmod_cmd.done", g_dir);
    swprintf_s(tmpPath, L"%s\\worldmod_cmd.running", g_dir);
    for (;;) {
        Sleep(100);
        if (!MoveFileExW(cmdPath, tmpPath, MOVEFILE_REPLACE_EXISTING)) continue;   // atomically claim the batch
        FILE* in = nullptr; _wfopen_s(&in, tmpPath, L"r");
        FILE* out = nullptr; _wfopen_s(&out, donePath, L"w");
        char line[512];
        while (in && out && fgets(line, sizeof line, in)) { run(line, out); fflush(out); }
        if (in) fclose(in);
        if (out) { fprintf(out, "end\n"); fclose(out); }
        DeleteFileW(tmpPath);
    }
}

}  // namespace

// 命令通道（不碰窗口/输入）：automation 和 auto_skip 都用它，这样正常游戏也能收 worldmod_cmd.txt
void commandInit(const wchar_t* dir, TraceLogFn log) {
    g_log = log;
    wcscpy_s(g_dir, dir);
    CreateThread(nullptr, 256 << 10, commandThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (g_log) g_log("command channel enabled (worldmod_cmd.txt)");
}

void automationInit(const wchar_t* dir, TraceLogFn log) {
    g_automation = true;
    commandInit(dir, log);                                   // 命令通道（设置 g_dir + 启动 commandThread）
    g_shotDone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_probeDone) g_probeDone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_fake = 1;
    hookIat(IAT_GetCursorPos, (void*)hGetCursorPos);
    hookIat(IAT_GetAsyncKeyState, (void*)hGetAsyncKeyState);
    hookIat(IAT_GetKeyState, (void*)hGetKeyState);
    hookIat(IAT_GetForegroundWindow, (void*)hGetForegroundWindow);
    hookIat(IAT_GetActiveWindow, (void*)hGetActiveWindow);
    hookIat(IAT_GetFocus, (void*)hGetFocus);
    hookIat(IAT_ClipCursor, (void*)hClipCursor);
    hookIat(IAT_SetForegroundWindow, (void*)hSetForegroundWindow);
    hookIat(IAT_IsIconic, (void*)hIsIconic);
    if (g_log) g_log("automation enabled");
}

void automationOnEndScene(IDirect3DDevice9* d) {
    if (g_shotPending && InterlockedExchange(&g_shotPending, 0)) { capture(d); SetEvent(g_shotDone); }
    if (g_probePending && InterlockedExchange(&g_probePending, 0)) { probe(d); SetEvent(g_probeDone); }
}

// auto_skip in normal play: only the cursor and button queries are hooked, and they pass through to
// Windows except while the skipper runs
void autoSkipInit(const wchar_t* dir, TraceLogFn log) {
    commandInit(dir, log);                                   // 命令通道：auto_skip 下也开启（正常游戏可收命令）
    g_autoSkipOn = 1;
    if (g_automation) {                                          // the test copy: input is virtual anyway
        if (g_log) g_log("auto_skip enabled (with automation)");
        launchMovies(false);
        CreateThread(nullptr, 1 << 16, startupSkipThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        return;
    }
    g_probeDone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_fake = 0;
    g_realCursorPos = *(GetCursorPosFn*)(uintptr_t)IAT_GetCursorPos;
    g_realAsyncKeyState = *(KeyStateFn*)(uintptr_t)IAT_GetAsyncKeyState;
    g_realKeyState = *(KeyStateFn*)(uintptr_t)IAT_GetKeyState;
    hookIat(IAT_GetCursorPos, (void*)hGetCursorPos);
    hookIat(IAT_GetAsyncKeyState, (void*)hGetAsyncKeyState);
    hookIat(IAT_GetKeyState, (void*)hGetKeyState);
    if (g_log) g_log("auto_skip enabled: the launch movies, and the opening events and dialogs when a map loads, are clicked through");
    launchMovies(false);
    CreateThread(nullptr, 1 << 16, startupSkipThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}

void autoSkipOnStageLoad() {
    if (!g_autoSkipOn || !g_probeDone) return;
    if (InterlockedExchange(&g_skipping, 1)) return;
    CreateThread(nullptr, 1 << 16, autoSkipThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}
