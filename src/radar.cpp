// Hexed Radar - AML mod (libHexedRadar.so)   32-bit ARM (armeabi-v7a) first
// Clean-room radar zoom + tilt for GTA:SA mobile based clients (Nezuko, Zan, ...).
// Hooks only stock libGTASA CRadar functions. Settings come from the Lua adjuster through the exported C API.
// STATUS: not compiled on a device / not tested in game. Tilt math is a first approximation.

#include <jni.h>
#include <android/log.h>
#include "mod/amlmod.h"
#include <atomic>
#include <math.h>

#define TAG "HexedRadar"
#include <stdio.h>
#include <stdarg.h>
#ifndef HX_LOG_PATH
#define HX_LOG_PATH "/storage/emulated/0/HexedRadar_log.txt"   // same idea as HexedVoiceMod_log.txt
#endif
static void hxlog(int prio, const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    __android_log_print(prio, TAG, "%s", buf);
    if (FILE* f = fopen(HX_LOG_PATH, "a")) { fprintf(f, "[HexedRadar] %s\n", buf); fclose(f); }
}
#define LOGI(...) hxlog(ANDROID_LOG_INFO,  __VA_ARGS__)
#define LOGE(...) hxlog(ANDROID_LOG_ERROR, __VA_ARGS__)

MYMOD(net.hexedstudio.radar, Hexed Radar, 1.0.1, HEXED Studio)

namespace {

struct CVector2D { float x, y; };

std::atomic<int> g_zoom{100};   // 50..250, 100 = stock
std::atomic<int> g_tilt{0};     // 0..60 degrees
std::atomic<int> g_on{1};
std::atomic<int> g_hooks{0};    // bit0 = zoom hook installed, bit1 = tilt hook installed

typedef void (*Xform_t)(CVector2D&, const CVector2D&);
Xform_t RealToRadar_orig  = nullptr;   // CRadar::TransformRealWorldPointToRadarSpace
Xform_t RadarToScreen_orig = nullptr;  // CRadar::TransformRadarPointToScreenSpace

// Zoom: scale the radar-space result. Map sections and blips both pass through here.
void RealToRadar_hook(CVector2D& out, const CVector2D& in) {
    RealToRadar_orig(out, in);
    if (!g_on.load()) return;
    const int z = g_zoom.load();
    if (z == 100) return;
    const float k = 100.0f / (float)z;      // zoom 200 -> map looks 2x closer
    out.x *= k; out.y *= k;
}

// Tilt: tilt the radar plane around the horizontal axis (simple perspective). Input is radar space -1..1.
void RadarToScreen_hook(CVector2D& out, const CVector2D& in) {
    const int t = g_tilt.load();
    if (!g_on.load() || t == 0) { RadarToScreen_orig(out, in); return; }
    const float a = (float)t * 0.01745329252f;
    const float c = cosf(a), s = sinf(a);
    float depth = 1.0f + 0.35f * in.y * s;  // far side (y > 0) shrinks
    if (depth < 0.4f) depth = 0.4f;
    CVector2D p = { in.x / depth, (in.y * c) / depth };
    RadarToScreen_orig(out, p);
}

void* findGTASA() {
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!h) h = aml->GetLibHandle("libGTASA");
    return h;
}

} // namespace

// ---- exported C API (Lua ffi) ----
extern "C" {
#define HX_EXPORT __attribute__((visibility("default")))
HX_EXPORT void HexedRadar_SetZoom(int v)    { g_zoom.store(v < 50 ? 50 : (v > 250 ? 250 : v)); }
HX_EXPORT void HexedRadar_SetTilt(int v)    { g_tilt.store(v < 0 ? 0 : (v > 60 ? 60 : v)); }
HX_EXPORT int  HexedRadar_GetZoom()         { return g_zoom.load(); }
HX_EXPORT int  HexedRadar_GetTilt()         { return g_tilt.load(); }
HX_EXPORT void HexedRadar_SetEnabled(int o) { g_on.store(o ? 1 : 0); }
HX_EXPORT int  HexedRadar_Status()          { return g_hooks.load(); }   // 3 = both hooks active
}

static void installHooks() {
    void* gta = findGTASA();
    if (!gta) { LOGE("libGTASA.so handle not found"); return; }
    if (!(g_hooks.load() & 1)) {
        uintptr_t a = aml->GetSym(gta, "_ZN6CRadar35TransformRealWorldPointToRadarSpaceER9CVector2DRKS0_");
        LOGI("symbol RealToRadar = %p", (void*)a);
        if (a && aml->Hook((void*)a, (void*)RealToRadar_hook, (void**)&RealToRadar_orig)) g_hooks.fetch_or(1);
        else LOGE("zoom hook not installed (symbol missing or hook failed)");
    }
    if (!(g_hooks.load() & 2)) {
        uintptr_t b = aml->GetSym(gta, "_ZN6CRadar32TransformRadarPointToScreenSpaceER9CVector2DRKS0_");
        LOGI("symbol RadarToScreen = %p", (void*)b);
        if (b && aml->Hook((void*)b, (void*)RadarToScreen_hook, (void**)&RadarToScreen_orig)) g_hooks.fetch_or(2);
        else LOGE("tilt hook not installed (symbol missing or hook failed)");
    }
    LOGI("hooks=%d", g_hooks.load());
}

ON_MOD_PRELOAD() { remove(HX_LOG_PATH); LOGI("OnModPreLoad HexedRadar 1.0.1"); }
ON_MOD_LOAD()    { LOGI("OnModLoad"); installHooks(); }
ON_ALL_MODS_LOAD() { if (g_hooks.load() != 3) { LOGI("retry after all mods loaded"); installHooks(); } }
