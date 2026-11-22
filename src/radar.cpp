// Hexed Radar - AML mod (libHexedRadar.so)   32-bit ARM (armeabi-v7a)
// Clean-room radar zoom + tilt for GTA:SA mobile based clients (Zan, Nezuko, ...).
// Written from reading libGTASA.so of the Zan build; only stock CRadar / CSprite2d functions are hooked.
//
// HOW THE STOCK RADAR WORKS (verified by disassembly of the Zan APK's libGTASA.so, armeabi-v7a):
//   CHud::DrawRadar -> CRadar::DrawMap   (range = 180 - CTheScripts::RadarZoomValue, or 180 / speed based;
//                                         stores CRadar::m_radarRange + vec2DRadarOrigin, then calls DrawRadarMap)
//                        -> CRadar::DrawRadarMap -> DrawRadarMask (CSprite2d::SetMaskVertices) + 9x DrawRadarSection (SetVertices)
//                   -> ring / north quad: CSprite2d::Draw(8 floats, CRGBA)  (inside CHud::DrawRadar)
//                   -> CRadar::DrawBlips  (ALSO called by the pause map, Menu_MapRender!)
//   SCREEN GEOMETRY of the radar = the radar touch widget: CTouchInterface::m_pWidgets[161] (offset 0x284),
//   rect floats at +0x20 (x0) +0x24 (y0) +0x28 (x1) +0x2c (y1). Zan's PcHud rewrites those fields.
//   CRadar::m_radarRect is a WORLD-space rect (fed to StreamRadarSections) - never use it as a screen rect.
//   ZOOM = scale m_radarRange around DrawRadarMap and DrawBlips (only while inside CHud::DrawRadar)
//   TILT = project the screen positions those draw calls submit around the widget centre
//
// STATUS: not run on a device yet. Check HexedRadar_log.txt on the phone.

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

MYMOD(net.hexedstudio.radar, Hexed Radar, 1.2.1, HEXED Studio)

namespace {

struct CRGBA     { unsigned char r, g, b, a; };
struct CVector2D { float x, y; };
struct CRect     { float left, bottom, right, top; };   // GTA CRect order

std::atomic<int> g_zoom{100};    // 50..250, 100 = stock
std::atomic<int> g_tilt{0};      // 0..60 degrees
std::atomic<int> g_on{1};
std::atomic<int> g_hooks{0};     // bit0 zoom | bit1 tilt map+mask | bit2 tilt blips | bit3 CHud::DrawRadar scope + ring
std::atomic<int> g_inRadar{0};   // >0 while inside DrawRadarMap / DrawBlips (called from CHud::DrawRadar)
std::atomic<int> g_inHud{0};     // >0 while inside CHud::DrawRadar
std::atomic<int> g_geomSrc{0};   // 0 none, 1 radar widget, 2 mask polygon
std::atomic<unsigned> g_cntMap{0}, g_cntBlips{0}, g_cntProj{0};

float* g_range = nullptr;        // CRadar::m_radarRange
void** g_widgets = nullptr;      // CTouchInterface::m_pWidgets (array of CWidget*)
const int RADAR_WIDGET = 161;    // 0x284 / 4, read from CHud::DrawRadar / DrawRadarMask
const int W_X0 = 0x20, W_Y0 = 0x24, W_X1 = 0x28, W_Y1 = 0x2c;
float g_wCx = 0, g_wCy = 0, g_wH = 0;   // geometry refreshed at the start of every CHud::DrawRadar

// fallback centre taken from the radar mask polygon if m_radarRect looks wrong
float g_mCx = 0, g_mCy = 0, g_mH = 0;

// ---------------------------------------------------------------- projection
struct Proj { float cx, cy, h, s, c; };

// radar widget rect (screen space) -> centre + half height. Returns false if it looks invalid.
bool readWidget(float& cx, float& cy, float& h) {
    if (!g_widgets) return false;
    char* w = (char*)g_widgets[RADAR_WIDGET];
    if (!w) return false;
    const float x0 = *(float*)(w + W_X0), y0 = *(float*)(w + W_Y0), x1 = *(float*)(w + W_X1), y1 = *(float*)(w + W_Y1);
    const float ww = fabsf(x1 - x0), hh = fabsf(y1 - y0);
    if (!(ww > 16.f && hh > 16.f && ww < 4000.f && hh < 4000.f)) return false;   // also rejects NaN
    cx = (x0 + x1) * 0.5f; cy = (y0 + y1) * 0.5f; h = hh * 0.5f;
    return true;
}

// scope: 1 = inside DrawRadarMap/DrawBlips, 2 = also CHud::DrawRadar itself (ring quad)
bool getProj(Proj& p, int scope = 1) {
    const int t = g_tilt.load();
    if (!g_on.load() || t == 0) return false;
    if (g_inRadar.load() <= 0 && !(scope == 2 && g_inHud.load() > 0)) return false;
    float cx = g_wCx, cy = g_wCy, h = g_wH;
    if (!(h > 8.f)) {
        if (g_mH > 8.f) { cx = g_mCx; cy = g_mCy; h = g_mH; } else return false;
    }
    const float a = (float)t * 0.01745329252f;
    p.cx = cx; p.cy = cy; p.h = h; p.s = sinf(a); p.c = cosf(a);
    return true;
}

// tilt the radar plane about its horizontal centre line: far (top) side shrinks, near (bottom) side grows.
// The map sections are HUGE quads (several radar radii wide), so their corners sit far outside |v| = 1.
// A plain perspective divide has a singularity/fold there and makes the whole map texture vanish, so the
// depth term is clamped: it behaves like a real perspective inside the disc and stays linear (never
// negative, never folded) for far-away corners.
inline void proj(const Proj& p, float& x, float& y) {
    const float v = (y - p.cy) / p.h;                // -1 top .. +1 bottom
    float vd = v;
    if (vd < -2.0f) vd = -2.0f;
    if (vd >  2.0f) vd =  2.0f;
    const float depth = 1.0f - 0.25f * vd * p.s;     // 0.5 .. 1.5 for tilt 60: always > 0
    x = p.cx + (x - p.cx) / depth;
    y = p.cy + p.h * (v * p.c) / depth;
    g_cntProj.fetch_add(1);
}

// ---------------------------------------------------------------- zoom hooks
typedef void (*Void_t)();
typedef void (*Float_t)(float);
Void_t  DrawRadarMap_orig = nullptr;
Void_t  DrawRadar_orig    = nullptr;
Float_t DrawBlips_orig    = nullptr;

inline float scaleRange() {
    if (!g_range || !g_on.load()) return 0.f;
    const int z = g_zoom.load();
    if (z == 100) return 0.f;
    const float old = *g_range;
    *g_range = old * (100.0f / (float)z);            // zoom 200 -> range halves -> map looks 2x closer
    return old;
}

// CHud::DrawRadar: refresh the geometry from the radar widget, mark the scope
void DrawRadar_hook() {
    float cx, cy, h;
    if (readWidget(cx, cy, h)) { g_wCx = cx; g_wCy = cy; g_wH = h; g_geomSrc.store(1); }
    else { g_wH = 0; g_geomSrc.store(g_mH > 8.f ? 2 : 0); }
    g_inHud.fetch_add(1);
    DrawRadar_orig();
    g_inHud.fetch_sub(1);
}

void DrawRadarMap_hook() {
    if (g_inHud.load() <= 0) { DrawRadarMap_orig(); return; }
    g_cntMap.fetch_add(1);
    const float old = scaleRange();
    g_inRadar.fetch_add(1);
    DrawRadarMap_orig();
    g_inRadar.fetch_sub(1);
    if (old != 0.f && g_range) *g_range = old;
}

void DrawBlips_hook(float a) {
    // DrawBlips is also used by the pause map (Menu_MapRender): only touch the HUD radar
    if (g_inHud.load() <= 0) { DrawBlips_orig(a); return; }
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

// ring / north quad: CSprite2d::Draw(x1,y1,x2,y2,x3,y3,x4,y4, CRGBA const&) member, used by CHud::DrawRadar
typedef void (*SpriteDrawQuad_t)(void*, float, float, float, float, float, float, float, float, const CRGBA&);
SpriteDrawQuad_t SpriteDrawQuad_orig = nullptr;
void SpriteDrawQuad_hook(void* self, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, const CRGBA& c) {
    Proj p;
    if (g_inRadar.load() <= 0 && getProj(p, 2)) {
        proj(p, x1, y1); proj(p, x2, y2); proj(p, x3, y3); proj(p, x4, y4);
    }
    SpriteDrawQuad_orig(self, x1, y1, x2, y2, x3, y3, x4, y4, c);
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
    if (!g_widgets) g_widgets = (void**)aml->GetSym(gta, "_ZN15CTouchInterface10m_pWidgetsE");
    LOGI("m_radarRange=%p m_pWidgets=%p", (void*)g_range, (void*)g_widgets);

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
    if (!(have & 8)) {
        bool a = hookSym(gta, "_ZN4CHud9DrawRadarEv", (void*)DrawRadar_hook, (void**)&DrawRadar_orig);
        bool b = hookSym(gta, "_ZN9CSprite2d4DrawEffffffffRK5CRGBA", (void*)SpriteDrawQuad_hook, (void**)&SpriteDrawQuad_orig);
        if (a && b) g_hooks.fetch_or(8);
    }
    LOGI("hooks=%d (1=zoom 2=tilt map 4=tilt blips 8=DrawRadar scope+ring)", g_hooks.load());
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
HX_EXPORT int  HexedRadar_Status()          { return g_hooks.load(); }          // 15 = everything hooked
HX_EXPORT int  HexedRadar_GeomSource()      { return g_geomSrc.load(); }        // 0 none, 1 radar widget, 2 mask polygon
// 0 = DrawRadarMap calls, 1 = DrawBlips calls, 2 = projected points: proves the hooks run every frame
HX_EXPORT unsigned HexedRadar_Counter(int w) { return w == 0 ? g_cntMap.load() : (w == 1 ? g_cntBlips.load() : g_cntProj.load()); }
}

ON_MOD_PRELOAD()   { remove(HX_LOG_PATH); LOGI("OnModPreLoad HexedRadar 1.2.1"); }
ON_MOD_LOAD()      { LOGI("OnModLoad"); installHooks(); }
ON_ALL_MODS_LOAD() { if (g_hooks.load() != 15) { LOGI("retry after all mods loaded"); installHooks(); } }
