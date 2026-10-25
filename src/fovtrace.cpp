// Per-frame trace of the camera FOV pipeline, for diagnosing jumps during zoom transitions.
// Request: create rowe_fovtrace.req; the next ~8 seconds are written to rowe_fovtrace.csv.
//
// Rows come from two threads: "cam" rows from the game thread (camera FOV hook, camera mode
// offset hook) and "warp" rows from the render thread (parameters the lens warp used).
#include <windows.h>
#include <stdio.h>
#include "rowe.h"

struct TraceRow {
    double t;          // ms since trace start
    char kind;         // 'c' camera FOV, 'm' camera mode offset, 'w' warp, 'p' present
    float a, b, c, d;  // kind-specific values
    void* obj;
};

static const int kMaxRows = 20000;
static TraceRow g_rows[kMaxRows];
static volatile LONG g_count;
static volatile bool g_active;
static LARGE_INTEGER g_start, g_freq;
static const double kDurationMs = 8000;

// Appends a row while a trace is recording (any thread).
void FovTraceAdd(char kind, float a, float b, float c, float d, void* obj)
{
    if (!g_active) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = (now.QuadPart - g_start.QuadPart) * 1000.0 / g_freq.QuadPart;
    LONG i = InterlockedIncrement(&g_count) - 1;
    if (i >= kMaxRows) return;
    g_rows[i] = {t, kind, a, b, c, d, obj};
}

// Writes the recorded rows to rowe_fovtrace.csv.
static void Dump()
{
    char path[MAX_PATH];
    wsprintfA(path, "%srowe_fovtrace.csv", ModDir());
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "t_ms,kind,a,b,c,d,obj\n");
    LONG n = g_count < kMaxRows ? g_count : kMaxRows;
    for (LONG i = 0; i < n; ++i) {
        const TraceRow& r = g_rows[i];
        fprintf(f, "%.2f,%c,%.4f,%.4f,%.4f,%.4f,%p\n", r.t, r.kind, r.a, r.b, r.c, r.d, r.obj);
    }
    fclose(f);
    Log("fovtrace: wrote %ld rows", n);
}

// Called once per Present (render thread).
void FovTracePresent()
{
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    static DWORD counter;
    if (!g_active && ++counter % 15 == 0) {
        char req[MAX_PATH];
        wsprintfA(req, "%srowe_fovtrace.req", ModDir());
        if (GetFileAttributesA(req) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(req);
            QueryPerformanceCounter(&g_start);
            g_count = 0;
            g_active = true;
            Log("fovtrace: started");
        }
        return;
    }
    if (!g_active) return;
    FovTraceAdd('p', 0, 0, 0, 0, nullptr);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if ((now.QuadPart - g_start.QuadPart) * 1000.0 / g_freq.QuadPart > kDurationMs) {
        g_active = false;
        Dump();
    }
}
