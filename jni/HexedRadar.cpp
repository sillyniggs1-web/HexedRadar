// libHexedRadar.so  -  HEXED Studio  -  clean-room radar zoom/tilt for GTA:SA mobile (32-bit ARM)
// Written from scratch. Hooks only stock libGTASA CRadar functions. NOT compiled or tested yet.
// Build with Android NDK (armeabi-v7a) against the AML headers (amlmod.h).
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <math.h>
#include <atomic>

MYMOD(net.hexedstudio.radar, HexedRadar, 1.0, HEXED Studio)

struct CVector2D { float x, y; };

static void* hGTASA = nullptr;
static std::atomic<int>  g_zoom{100};   // 50..250, 100 = stock
static std::atomic<int>  g_tilt{0};     // 0..60 degrees
static std::atomic<int>  g_on{1};

// ---- exported C API (what the Lua script calls through ffi) ----
extern "C" {
__attribute__((visibility("default"))) void HexedRadar_SetZoom(int v){ g_zoom = v<50?50:(v>250?250:v); }
__attribute__((visibility("default"))) void HexedRadar_SetTilt(int v){ g_tilt = v<0?0:(v>60?60:v); }
__attribute__((visibility("default"))) int  HexedRadar_GetZoom(void){ return g_zoom; }
__attribute__((visibility("default"))) int  HexedRadar_GetTilt(void){ return g_tilt; }
__attribute__((visibility("default"))) void HexedRadar_SetEnabled(int on){ g_on = on; }
}

// ---- hooks ----
// 1) Zoom: scale the radar-space result of world->radar. Map sections and blips both go through it.
static void (*TransformRealWorldPointToRadarSpace_orig)(CVector2D&, const CVector2D&);
static void TransformRealWorldPointToRadarSpace_hook(CVector2D& out, const CVector2D& in)
{
    TransformRealWorldPointToRadarSpace_orig(out, in);
    if (!g_on) return;
    float k = 100.0f / (float)g_zoom;       // zoom 200 => map looks 2x closer (range halves)
    out.x *= k; out.y *= k;
}

// 2) Tilt: tilt the radar plane around the horizontal axis in screen space (simple perspective).
//    Input is the radar point in -1..1 space, output is screen space.
static void (*TransformRadarPointToScreenSpace_orig)(CVector2D&, const CVector2D&);
static void TransformRadarPointToScreenSpace_hook(CVector2D& out, const CVector2D& in)
{
    if (!g_on || g_tilt == 0) { TransformRadarPointToScreenSpace_orig(out, in); return; }
    float a = (float)g_tilt * 0.01745329252f;
    float c = cosf(a), s = sinf(a);
    float depth = 1.0f + 0.35f * in.y * s;  // far side (y>0) shrinks
    if (depth < 0.4f) depth = 0.4f;
    CVector2D p = { in.x / depth, in.y * c / depth };
    TransformRadarPointToScreenSpace_orig(out, p);
}

ONLOAD() {
    logger->SetTag("HexedRadar");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    if (!hGTASA) { logger->Error("libGTASA.so not found"); return; }
    void* a = aml->GetSym(hGTASA, "_ZN6CRadar35TransformRealWorldPointToRadarSpaceER9CVector2DRKS0_");
    void* b = aml->GetSym(hGTASA, "_ZN6CRadar32TransformRadarPointToScreenSpaceER9CVector2DRKS0_");
    if (!a || !b) { logger->Error("CRadar symbols missing in this GTASA build"); return; }
    aml->Hook(a, (void*)TransformRealWorldPointToRadarSpace_hook, (void**)&TransformRealWorldPointToRadarSpace_orig);
    aml->Hook(b, (void*)TransformRadarPointToScreenSpace_hook,    (void**)&TransformRadarPointToScreenSpace_orig);
    logger->Info("HexedRadar loaded");
}
