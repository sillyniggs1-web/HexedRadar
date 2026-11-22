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
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

MYMOD(net.hexedstudio.radar, Hexed Radar, 1.0.0, HEXED Studio)

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

ON_MOD_LOAD() {
    void* gta = findGTASA();
    if (!gta) { LOGE("libGTASA.so handle not found"); return; }

    uintptr_t a = aml->GetSym(gta, "_ZN6CRadar35TransformRealWorldPointToRadarSpaceER9CVector2DRKS0_");
    uintptr_t b = aml->GetSym(gta, "_ZN6CRadar32TransformRadarPointToScreenSpaceER9CVector2DRKS0_");
    LOGI("symbols: RealToRadar=%p RadarToScreen=%p", (void*)a, (void*)b);

    if (a && aml->Hook((void*)a, (void*)RealToRadar_hook, (void**)&RealToRadar_orig))
        g_hooks.fetch_or(1);
    else LOGE("zoom hook not installed (symbol missing or hook failed)");

    if (b && aml->Hook((void*)b, (void*)RadarToScreen_hook, (void**)&RadarToScreen_orig))
        g_hooks.fetch_or(2);
    else LOGE("tilt hook not installed (symbol missing or hook failed)");

    LOGI("loaded, hooks=%d", g_hooks.load());
}
