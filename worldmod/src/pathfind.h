#pragma once
// Hex-grid A* (0x567520): faster replacement, verification and statistics (see pathfind.cpp).
#include <stdint.h>
#include "d3dtrace.h"

struct PathfindConfig {
    int mode;           // 0 = original, 1 = binary heap (same results), 2 = + 2x heuristic, 3 = + search window
    int margin;         // mode 3: hexes searched around the bounding box of the sources and the goal
    int benchSlow;      // 1 = replay the first 10 slow calls (>= 5 ms) with every variant and log the times
    uint32_t benchGoal; // packed (lo | hi << 16) + 1, or 0: at calls 1, 20, 100, 300 also time every variant to this goal
    int stats;          // 1 = log calls slower than 5 ms and a summary every 10 s
    int enterLog;       // 1 = log every call as it starts (the last one before a freeze is the stuck call)
    int verify;         // 1 = run the original too and compare HEX28 (mode 1 semantics)
    int benchable;      // 1 = hook even when nothing else is on, so "astar bench" works
    int chinaLo, chinaHi;   // where Koei's China sits: (0,0) "no position" defaults are moved there
    uint8_t* hex28;     // HEX28 allocation (MEM_WRITE_WATCH)
    size_t hex28Size;
    void (*clear28)();  // what the original does at 0x567541 (clear HEX28)
};
bool pathfindInstall(const PathfindConfig& cfg, TraceLogFn log);
void pathfindBench(int lo, int hi);   // time original vs replacement at the next A* call
