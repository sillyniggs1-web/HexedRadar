// Hexed Radar - AML mod (libHexedRadar.so)   32-bit ARM (armeabi-v7a)
// Clean-room radar zoom + tilt for GTA:SA mobile based clients (Zan, Nezuko, ...).
// Written from reading libGTASA.so of the Zan build; only stock CRadar / CSprite2d functions are hooked.
//
// WHAT THE MINI-RADAR REALLY DOES (verified in Zan's libGTASA.so, armeabi-v7a):
//   CHud::DrawRadar -> CRadar::DrawMap            (writes CRadar::m_radarRange, then calls DrawRadarMap)
//                        -> CRadar::DrawRadarMap  -> DrawRadarMask (CSprite2d::SetMaskVertices)
//                                                 -> 9x DrawRadarSection (CSprite2d::SetVertices)
//                                                 -> DrawAreaOnRadar     (CSprite2d::SetVertices)
//                   -> CRadar::DrawBlips           -> DrawRadarSprite/CSprite2d::Draw(CRect), DrawRotatingRadarSprite,
//                                                    CSprite2d::DrawCircleAtNearClip
//   CRadar::TransformRadarPointToScreenSpace / TransformRealWorldPointToRadarSpace are only used by the PAUSE MAP
//   (Menu_MapUpdate). The mini-radar reads m_radarRange and the origin directly. So:
//     ZOOM = scale m_radarRange around DrawRadarMap and DrawBlips (map and blips stay in sync)
//     TILT = project the screen positions those draw calls submit, but only while inside the radar draw
//
// STATUS: not run on a device yet. Tilt is a first projection; check HexedRadar_log.txt on the phone.

#include <jni.h>
#include <android/log.h>
#include "mod/amlmod.h"
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#define TAG "HexedRadar"
#ifndef HX_LOG_PATH
#define HX_LOG_PATH "/storage/emulated/0/HexedRadar_log.txt"
#endif
static void hxlog(int prio, const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    __android_log_print(prio, TAG, "%s", buf);
    if (FILE* f = fopen(HX_LOG_PATH, "a")) { fprintf(f, "[HexedRadar] %s\n", buf); fclose(f); }
}
#define LOGI(...) hxlog(ANDROID_LOG_INFO,  __VA_ARGS__)
#define LOGE(...) hxlog(ANDROID_LOG_ERROR, __VA_ARGS__)

MYMOD(net.hexedstudio.radar, Hexed Radar, 1.1.0, HEXED Studio)

namespace {

struct CRGBA     { unsigned char r, g, b, a; };
struct CVector2D { float x, y; };
struct CRect     { float left, bottom, right, top; };   // GTA CRect order

std::atomic<int> g_zoom{100};    // 50..250, 100 = stock
std::atomic<int> g_tilt{0};      // 0..60 degrees
std::atomic<int> g_on{1};
std::atomic<int> g_hooks{0};     // bit0 zoom (map+blips) | bit1 tilt map+mask | bit2 tilt blips
std::atomic<int> g_inRadar{0};   // >0 while inside DrawRadarMap / DrawBlips
std::atomic<unsigned> g_cntMap{0}, g_cntBlips{0}, g_cntProj{0};

float* g_range = nullptr;        // CRadar::m_radarRange
CRect* g_rect  = nullptr;        // CRadar::m_radarRect (screen rect of the mini-radar)

// fallback centre taken from the radar mask polygon if m_radarRect looks wrong
float g_mCx = 0, g_mCy = 0, g_mH = 0;

// ---------------------------------------------------------------- projection
struct Proj { float cx, cy, h, s, c; };

bool getProj(Proj& p) {
    const int t = g_tilt.load();
    if (!g_on.load() || t == 0 || g_inRadar.load() <= 0) return false;
    float cx = 0, cy = 0, h = 0;
    bool ok = false;
    if (g_rect) {
        const float w  = fabsf(g_rect->right - g_rect->left);
        const float hh = fabsf(g_rect->bottom - g_rect->top);
        if (w > 16.f && hh > 16.f && w < 4000.f && hh < 4000.f) {
            cx = (g_rect->left + g_rect->right) * 0.5f;
            cy = (g_rect->top + g_rect->bottom) * 0.5f;
            h  = hh * 0.5f; ok = true;
        }
    }
    if (!ok && g_mH > 8.f) { cx = g_mCx; cy = g_mCy; h = g_mH; ok = true; }
    if (!ok) return false;
    const float a = (float)t * 0.01745329252f;
    p.cx = cx; p.cy = cy; p.h = h; p.s = sinf(a); p.c = cosf(a);
    return true;
}

// tilt the radar plane about its horizontal centre line: far (top) side shrinks and is squashed
inline void proj(const Proj& p, float& x, float& y) {
    const float v = (y - p.cy) / p.h;                // -1 top .. +1 bottom
    float depth = 1.0f - 0.30f * v * p.s;            // v<0 (far) => depth>1
    if (depth < 0.35f) depth = 0.35f;
    x = p.cx + (x - p.cx) / depth;
    y = p.cy + p.h * (v * p.c) / depth;
    g_cntProj.fetch_add(1);
}

// ---------------------------------------------------------------- zoom hooks
typedef void (*Void_t)();
typedef void (*Float_t)(float);
Void_t  DrawRadarMap_orig = nullptr;
Float_t DrawBlips_orig    = nullptr;

inline float scaleRange() {
    if (!g_range || !g_on.load()) return 0.f;
    const int z = g_zoom.load();
    if (z == 100) return 0.f;
    const float old = *g_range;
    *g_range = old * (100.0f / (float)z);            // zoom 200 -> range halves -> map looks 2x closer
    return old;
}

void DrawRadarMap_hook() {
    g_cntMap.fetch_add(1);
    const float old = scaleRange();
    g_inRadar.fetch_add(1);
    DrawRadarMap_orig();
    g_inRadar.fetch_sub(1);
    if (old != 0.f && g_range) *g_range = old;
}

void DrawBlips_hook(float a) {
    g_cntBlips.fetch_add(1);
    const float old = scaleRange();
    g_inRadar.fetch_add(1);
    DrawBlips_orig(a);
    g_inRadar.fetch_sub(1);
    if (old != 0.f && g_range) *g_range = old;
}

// ---------------------------------------------------------------- tilt hooks (map + mask)
typedef void (*SetVertices_t)(int, float*, float*, const CRGBA&);
typedef void (*SetMaskVertices_t)(int, float*, float);
SetVertices_t     SetVertices_orig     = nullptr;
SetMaskVertices_t SetMaskVertices_orig = nullptr;
float g_buf[64];

void SetVertices_hook(int n, float* pos, float* uv, const CRGBA& c) {
    Proj p;
    if (pos && n > 0 && n <= 32 && getProj(p)) {
        for (int i = 0; i < n * 2; i++) g_buf[i] = pos[i];
        for (int i = 0; i < n; i++) proj(p, g_buf[2 * i], g_buf[2 * i + 1]);
        SetVertices_orig(n, g_buf, uv, c);
        return;
    }
    SetVertices_orig(n, pos, uv, c);
}

void SetMaskVertices_hook(int n, float* pos, float z) {
    if (pos && n > 2 && n <= 32 && g_inRadar.load() > 0) {          // learn the radar centre from the mask polygon
        float mnx = pos[0], mxx = pos[0], mny = pos[1], mxy = pos[1];
        for (int i = 1; i < n; i++) {
            if (pos[2*i] < mnx) mnx = pos[2*i];   if (pos[2*i] > mxx) mxx = pos[2*i];
            if (pos[2*i+1] < mny) mny = pos[2*i+1]; if (pos[2*i+1] > mxy) mxy = pos[2*i+1];
        }
        const float hh = (mxy - mny) * 0.5f;
        if (hh > g_mH * 0.9f || g_mH == 0.f) {                       // keep the biggest polygon (the radar disc)
            static int logged = 0;
            if (logged < 3 && (mxy - mny) > 16.f) { logged++; LOGI("mask bbox n=%d x[%.1f..%.1f] y[%.1f..%.1f]", n, mnx, mxx, mny, mxy); }
            g_mCx = (mnx + mxx) * 0.5f; g_mCy = (mny + mxy) * 0.5f; g_mH = hh;
        }
    }
    Proj p;
    if (pos && n > 0 && n <= 32 && getProj(p)) {
        for (int i = 0; i < n * 2; i++) g_buf[i] = pos[i];
        for (int i = 0; i < n; i++) proj(p, g_buf[2 * i], g_buf[2 * i + 1]);
        SetMaskVertices_orig(n, g_buf, z);
        return;
    }
    SetMaskVertices_orig(n, pos, z);
}

// ---------------------------------------------------------------- tilt hooks (blips)
typedef void (*SpriteDrawRect_t)(void*, const CRect&, const CRGBA&);
typedef void (*DrawRotRadarSprite_t)(void*, float, float, float, float, CRGBA);
typedef void (*DrawCircle_t)(const CVector2D&, float, const CRGBA&, int);
SpriteDrawRect_t     SpriteDrawRect_orig = nullptr;
DrawRotRadarSprite_t DrawRotRadarSprite_orig = nullptr;
DrawCircle_t         DrawCircle_orig = nullptr;

// CSprite2d::Draw(CRect const&, CRGBA const&) is a member function: r0 = this
void SpriteDrawRect_hook(void* self, const CRect& r, const CRGBA& c) {
    Proj p;
    if (getProj(p)) {
        const float w = r.right - r.left, hgt = r.top - r.bottom;
        float x = (r.left + r.right) * 0.5f, y = (r.top + r.bottom) * 0.5f;
        proj(p, x, y);
        CRect n = { x - w * 0.5f, y - hgt * 0.5f, x + w * 0.5f, y + hgt * 0.5f };
        SpriteDrawRect_orig(self, n, c);
        return;
    }
    SpriteDrawRect_orig(self, r, c);
}

void DrawRotRadarSprite_hook(void* spr, float x, float y, float a, float s, CRGBA c) {
    Proj p;
    if (getProj(p)) proj(p, x, y);
    DrawRotRadarSprite_orig(spr, x, y, a, s, c);
}

void DrawCircle_hook(const CVector2D& ctr, float rad, const CRGBA& c, int n) {
    Proj p;
    if (getProj(p)) {
        CVector2D q = ctr; proj(p, q.x, q.y);
        DrawCircle_orig(q, rad, c, n);
        return;
    }
    DrawCircle_orig(ctr, rad, c, n);
}

// ---------------------------------------------------------------- install
void* findGTASA() {
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!h) h = aml->GetLibHandle("libGTASA");
    return h;
}

bool hookSym(void* lib, const char* sym, void* hook, void** orig) {
    uintptr_t a = aml->GetSym(lib, sym);
    if (!a) { LOGE("symbol missing: %s", sym); return false; }
    if (!aml->Hook((void*)a, hook, orig)) { LOGE("hook failed: %s", sym); return false; }
    LOGI("hooked %s @ %p", sym, (void*)a);
    return true;
}

void installHooks() {
    void* gta = findGTASA();
    if (!gta) { LOGE("libGTASA.so handle not found"); return; }
    if (!g_range) g_range = (float*)aml->GetSym(gta, "_ZN6CRadar12m_radarRangeE");
    if (!g_rect)  g_rect  = (CRect*)aml->GetSym(gta, "_ZN6CRadar11m_radarRectE");
    LOGI("m_radarRange=%p m_radarRect=%p", (void*)g_range, (void*)g_rect);

    const int have = g_hooks.load();
    if (!(have & 1) && g_range) {
        bool a = hookSym(gta, "_ZN6CRadar12DrawRadarMapEv", (void*)DrawRadarMap_hook, (void**)&DrawRadarMap_orig);
        bool b = hookSym(gta, "_ZN6CRadar9DrawBlipsEf",     (void*)DrawBlips_hook,    (void**)&DrawBlips_orig);
        if (a && b) g_hooks.fetch_or(1);
    }
    if (!(have & 2)) {
        bool a = hookSym(gta, "_ZN9CSprite2d11SetVerticesEiPfS0_RK5CRGBA", (void*)SetVertices_hook,     (void**)&SetVertices_orig);
        bool b = hookSym(gta, "_ZN9CSprite2d15SetMaskVerticesEiPff",      (void*)SetMaskVertices_hook, (void**)&SetMaskVertices_orig);
        if (a && b) g_hooks.fetch_or(2);
    }
    if (!(have & 4)) {
        bool a = hookSym(gta, "_ZN9CSprite2d4DrawERK5CRectRK5CRGBA", (void*)SpriteDrawRect_hook, (void**)&SpriteDrawRect_orig);
        bool b = hookSym(gta, "_ZN6CRadar23DrawRotatingRadarSpriteEP9CSprite2dffff5CRGBA", (void*)DrawRotRadarSprite_hook, (void**)&DrawRotRadarSprite_orig);
        bool c = hookSym(gta, "_ZN9CSprite2d20DrawCircleAtNearClipERK9CVector2DfRK5CRGBAi", (void*)DrawCircle_hook, (void**)&DrawCircle_orig);
        if (a && b && c) g_hooks.fetch_or(4);
    }
    LOGI("hooks=%d (1=zoom 2=tilt map 4=tilt blips)", g_hooks.load());
}

} // namespace

// ---------------------------------------------------------------- exported C API (Lua ffi)
extern "C" {
#define HX_EXPORT __attribute__((visibility("default")))
HX_EXPORT void HexedRadar_SetZoom(int v)    { g_zoom.store(v < 50 ? 50 : (v > 250 ? 250 : v)); }
HX_EXPORT void HexedRadar_SetTilt(int v)    { g_tilt.store(v < 0 ? 0 : (v > 60 ? 60 : v)); }
HX_EXPORT int  HexedRadar_GetZoom()         { return g_zoom.load(); }
HX_EXPORT int  HexedRadar_GetTilt()         { return g_tilt.load(); }
HX_EXPORT void HexedRadar_SetEnabled(int o) { g_on.store(o ? 1 : 0); }
HX_EXPORT int  HexedRadar_Status()          { return g_hooks.load(); }          // 7 = everything hooked
// 0 = DrawRadarMap calls, 1 = DrawBlips calls, 2 = projected points: proves the hooks run every frame
HX_EXPORT unsigned HexedRadar_Counter(int w) { return w == 0 ? g_cntMap.load() : (w == 1 ? g_cntBlips.load() : g_cntProj.load()); }
}

ON_MOD_PRELOAD()   { remove(HX_LOG_PATH); LOGI("OnModPreLoad HexedRadar 1.1.0"); }
ON_MOD_LOAD()      { LOGI("OnModLoad"); installHooks(); }
ON_ALL_MODS_LOAD() { if (g_hooks.load() != 7) { LOGI("retry after all mods loaded"); installHooks(); } }
