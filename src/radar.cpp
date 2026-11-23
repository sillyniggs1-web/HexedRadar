// Hexed Radar - AML mod (libHexedRadar.so) v2.0   32-bit ARM (armeabi-v7a)
// Radar zoom + tilt for the Zan / Nezuko SA-MP Android clients, including the PC-style HUD.
//
// ============================================================================================
// HOW THE RADAR WORKS (verified by disassembling Zan's libGTASA.so and libSAMP.so)
// ============================================================================================
//  * The PC HUD does NOT draw its own radar. libSAMP's "PcHud" inline-hooks CWidgetRadar::Update:
//        it calls the original, then (PC HUD flag set) overwrites the radar widget rectangle
//        CTouchInterface::m_pWidgets[161]  +0x20 x0, +0x24 y0, +0x28 x1, +0x2c y1  (screen px)
//        with the PC layout (40/640..134/640 of the width, 344/448..420/448 of the height) and
//        mirrors it to +0x34..+0x40 (touch rect).
//  * The stock pipeline then draws from that rectangle. Exactly these functions read the widget
//    inline (no helper call): CHud::DrawRadar, CRadar::DrawRadarMap / DrawRadarMask /
//    DrawRadarSection / DrawBlips / DrawRadarSprite / DrawRotatingRadarSprite / DrawCoordBlip /
//    DrawEntityBlip / ShowRadarTraceWithHeight / DrawAreaOnRadar / DrawRadarGangOverlay /
//    TransformRadarPointToScreenSpace.
//  * Range: CRadar::DrawMap stores CRadar::m_radarRange every frame (180 on foot, up to 350 in a
//    fast vehicle, or 180 - CTheScripts::RadarZoomValue), then calls DrawRadarMap. DrawBlips
//    (called later by CHud::DrawRadar, also by the pause map) reads the same variable.
//  * Every radar primitive ends in one of three submit calls:
//        RwIm2DRenderIndexedPrimitive_BUGFIX  (mask, map sections, gang overlay)
//        RwIm2DRenderPrimitive_BUGFIX         (CSprite2d::Draw / DrawRect / Draw2DPolygon: blips, arrows, ring)
//        RwIm2DRenderTriangle                 (DrawCircleAtNearClip: blip circles)
//    RwOpenGLVertex is 28 bytes: x@0 y@4 z@8 rhw@0xC rgba@0x10 u@0x14 v@0x18 (screen space).
//
// WHAT CHANGED FROM v1 (why v1 was unreliable)
//  * v1 inline-hooked CSprite2d::SetVertices / SetMaskVertices (prologue = IT block), CHud::DrawRadar
//    and CSprite2d::Draw (prologue = PC-relative ldr) and tilted only some CSprite2d wrappers
//    (DrawRect, Draw2DPolygon and DrawAreaOnRadar were never tilted). Inline hooks on such prologues
//    are exactly what libSAMP's own hooker refuses ("entry would not relocate").
//  * v2 hooks by GOT slot: every call between these functions goes through a PLT stub, so replacing
//    the slot pointer needs no instruction relocation and is a single atomic store. The slot is
//    found at runtime from the ELF dynamic section (R_ARM_JUMP_SLOT by symbol name), so no
//    build-specific offsets are hardcoded and it also works on other libGTASA builds.
//  * Tilt now happens at the three RwIm2D submit calls, so EVERY radar primitive is covered.
//  * The radar widget is found by vtable (CWidgetRadar) instead of trusting index 161 blindly.
//
// STATUS: static analysis only, not run on a device. Check HexedRadar_log.txt on the phone.

#include <jni.h>
#include <android/log.h>
#include "mod/amlmod.h"
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif
#ifndef ELF32_R_TYPE
#define ELF32_R_TYPE(i) ((i) & 0xff)
#define ELF32_R_SYM(i)  ((i) >> 8)
#endif

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

MYMOD(net.hexedstudio.radar, Hexed Radar, 2.0.0, HEXED Studio)

namespace {

// RwOpenGLVertex (verified from CSprite2d::SetVertices / SetMaskVertices / Draw)
struct RwV { float x, y, z, rhw; unsigned char r, g, b, a; float u, v; };
static_assert(sizeof(RwV) == 28, "RwOpenGLVertex must be 28 bytes");

std::atomic<int> g_zoom{100};     // 50..250, 100 = stock
std::atomic<int> g_tilt{0};       // 0..60 degrees
std::atomic<int> g_on{1};
std::atomic<int> g_hooks{0};      // bit0 CHud::DrawRadar scope | bit1 zoom (DrawRadarMap+DrawBlips) | bit2 tilt (3x RwIm2D submit)
std::atomic<int> g_inHud{0};      // >0 while inside CHud::DrawRadar
std::atomic<int> g_geomSrc{0};    // 0 none, 1 radar widget (vtable verified), 2 radar widget (index fallback, vtable unknown)
std::atomic<int> g_widgetIdx{-1};
std::atomic<unsigned> g_cntHud{0}, g_cntMap{0}, g_cntBlips{0}, g_cntProj{0}, g_cntSub{0};

const int HOOK_SCOPE = 1, HOOK_ZOOM = 2, HOOK_TILT = 4, HOOK_ALL = 7;

float*  g_range    = nullptr;     // CRadar::m_radarRange
void**  g_widgets  = nullptr;     // CTouchInterface::m_pWidgets  (array of 190 CWidget*)
void*   g_radarVt  = nullptr;     // &_ZTV12CWidgetRadar + 8  (what a CWidgetRadar object stores at offset 0)
const int WIDGETS_N = 190;        // sizeof(m_pWidgets) 760 / 4
const int RADAR_WIDGET_FALLBACK = 161;   // 0x284 / 4: the offset CHud::DrawRadar / DrawRadarMask use
const int W_X0 = 0x20, W_Y0 = 0x24, W_X1 = 0x28, W_Y1 = 0x2c;

float g_wCx = 0, g_wCy = 0, g_wH = 0;    // radar rect centre + half height, refreshed at every CHud::DrawRadar

// ------------------------------------------------------------------ radar widget
char* radarWidget() {
    if (!g_widgets) return nullptr;
    int idx = g_widgetIdx.load();
    if (idx >= 0 && idx < WIDGETS_N) {
        char* w = (char*)g_widgets[idx];
        if (w && (!g_radarVt || *(void**)w == g_radarVt)) return w;
    }
    if (g_radarVt) {                                  // (re)discover by vtable
        for (int i = 0; i < WIDGETS_N; i++) {
            char* w = (char*)g_widgets[i];
            if (w && *(void**)w == g_radarVt) {
                if (g_widgetIdx.load() != i) { LOGI("radar widget found by vtable: index %d", i); g_widgetIdx.store(i); }
                g_geomSrc.store(1);
                return w;
            }
        }
        return nullptr;                               // vtable known but no such widget (yet)
    }
    char* w = (char*)g_widgets[RADAR_WIDGET_FALLBACK];  // no vtable symbol: trust the verified index
    if (w) { g_widgetIdx.store(RADAR_WIDGET_FALLBACK); g_geomSrc.store(2); }
    return w;
}

bool readWidget(float& cx, float& cy, float& h) {
    char* w = radarWidget();
    if (!w) return false;
    const float x0 = *(float*)(w + W_X0), y0 = *(float*)(w + W_Y0), x1 = *(float*)(w + W_X1), y1 = *(float*)(w + W_Y1);
    const float ww = fabsf(x1 - x0), hh = fabsf(y1 - y0);
    if (!(ww > 16.f && hh > 16.f && ww < 4000.f && hh < 4000.f)) return false;   // also rejects NaN
    cx = (x0 + x1) * 0.5f; cy = (y0 + y1) * 0.5f; h = hh * 0.5f;
    return true;
}

// ------------------------------------------------------------------ projection (tilt)
struct Proj { float cx, cy, h, s, c; };

bool getProj(Proj& p) {
    const int t = g_tilt.load();
    if (!g_on.load() || t == 0 || g_inHud.load() <= 0 || !(g_wH > 8.f)) return false;
    const float a = (float)t * 0.01745329252f;
    p.cx = g_wCx; p.cy = g_wCy; p.h = g_wH; p.s = sinf(a); p.c = cosf(a);
    return true;
}

// Tilt the radar plane about its horizontal centre line: the far (top) side shrinks, the near (bottom) side grows.
// Map sections are huge quads whose corners lie far outside |v| = 1, so the depth term is clamped: it behaves like
// a perspective divide inside the radar and stays linear (never negative, never folded) far away.
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

// ------------------------------------------------------------------ hooks: scope + zoom
typedef void (*Void_t)();
typedef void (*Float_t)(float);
Void_t  DrawRadar_orig    = nullptr;
Void_t  DrawRadarMap_orig = nullptr;
Float_t DrawBlips_orig    = nullptr;

struct Scope { Scope() { g_inHud.fetch_add(1); } ~Scope() { g_inHud.fetch_sub(1); } };

inline float scaleRange() {
    if (!g_range || !g_on.load()) return 0.f;
    const int z = g_zoom.load();
    if (z == 100) return 0.f;
    const float old = *g_range;
    if (!(old > 1.f && old < 5000.f)) return 0.f;     // garbage guard
    *g_range = old * (100.0f / (float)z);            // zoom 200 -> range halves -> map looks 2x closer
    return old;
}

// CHud::DrawRadar: refresh the geometry from the radar widget (already rewritten by PcHud's CWidgetRadar::Update), open the scope
void DrawRadar_hook() {
    g_cntHud.fetch_add(1);
    float cx, cy, h;
    if (readWidget(cx, cy, h)) { g_wCx = cx; g_wCy = cy; g_wH = h; }
    else { g_wH = 0; g_geomSrc.store(0); }
    Scope s;
    DrawRadar_orig();
}

void DrawRadarMap_hook() {
    if (g_inHud.load() <= 0) { DrawRadarMap_orig(); return; }   // only the HUD radar
    g_cntMap.fetch_add(1);
    const float old = scaleRange();
    DrawRadarMap_orig();
    if (old != 0.f && g_range) *g_range = old;
}

void DrawBlips_hook(float a) {
    if (g_inHud.load() <= 0) { DrawBlips_orig(a); return; }     // DrawBlips is also used by the pause map (Menu_MapRender)
    g_cntBlips.fetch_add(1);
    const float old = scaleRange();
    DrawBlips_orig(a);
    if (old != 0.f && g_range) *g_range = old;
}

// ------------------------------------------------------------------ hooks: tilt at the RwIm2D submit level
const int MAXV = 128;
RwV g_vb[MAXV];

typedef int (*Prim_t)(int, RwV*, int);
typedef int (*IdxPrim_t)(int, RwV*, int, unsigned short*, int);
typedef int (*Tri_t)(RwV*, int, int, int, int);
Prim_t    Prim_orig = nullptr;
IdxPrim_t Idx_orig  = nullptr;
Tri_t     Tri_orig  = nullptr;

// copies the vertices to a scratch buffer and projects them; returns the buffer or the original pointer
RwV* tilted(RwV* v, int n) {
    Proj p;
    if (!v || n <= 0) return v;
    if (!getProj(p)) return v;
    g_cntSub.fetch_add(1);
    if (n > MAXV) return v;
    memcpy(g_vb, v, (size_t)n * sizeof(RwV));
    for (int i = 0; i < n; i++) proj(p, g_vb[i].x, g_vb[i].y);
    return g_vb;
}

int Prim_hook(int type, RwV* v, int n)                                      { return Prim_orig(type, tilted(v, n), n); }
int Idx_hook(int type, RwV* v, int n, unsigned short* idx, int ni)           { return Idx_orig(type, tilted(v, n), n, idx, ni); }
int Tri_hook(RwV* v, int n, int a, int b, int c)                            { return Tri_orig(tilted(v, n), n, a, b, c); }

// ------------------------------------------------------------------ GOT-slot hooking
// Finds the R_ARM_JUMP_SLOT of `sym` in the loaded ELF image at `base` (via PT_DYNAMIC: DT_JMPREL/DT_SYMTAB/DT_STRTAB).
uintptr_t findPltSlot(uintptr_t base, const char* sym) {
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)base;
    if (!eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return 0;
    const Elf32_Phdr* ph = (const Elf32_Phdr*)(base + eh->e_phoff);
    const Elf32_Dyn* dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_DYNAMIC) { dyn = (const Elf32_Dyn*)(base + ph[i].p_vaddr); break; }
    if (!dyn) return 0;
    uintptr_t jmprel = 0, symtab = 0, strtab = 0; size_t pltrelsz = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
            case DT_JMPREL:   jmprel   = dyn->d_un.d_ptr; break;
            case DT_PLTRELSZ: pltrelsz = dyn->d_un.d_val; break;
            case DT_SYMTAB:   symtab   = dyn->d_un.d_ptr; break;
            case DT_STRTAB:   strtab   = dyn->d_un.d_ptr; break;
        }
    }
    if (!jmprel || !symtab || !strtab || !pltrelsz) return 0;
    // bionic leaves d_ptr link-relative; some loaders rewrite it to absolute: accept both
    if (jmprel < base) jmprel += base;
    if (symtab < base) symtab += base;
    if (strtab < base) strtab += base;
    const Elf32_Rel* r = (const Elf32_Rel*)jmprel;
    const size_t n = pltrelsz / sizeof(Elf32_Rel);
    for (size_t i = 0; i < n; i++) {
        if (ELF32_R_TYPE(r[i].r_info) != R_ARM_JUMP_SLOT) continue;
        const Elf32_Sym* s = (const Elf32_Sym*)symtab + ELF32_R_SYM(r[i].r_info);
        if (strcmp((const char*)strtab + s->st_name, sym) == 0) return base + r[i].r_offset;
    }
    return 0;
}

// is the page of `addr` already writable? (the GOT sits in the RELRO region, which is mapped read-only)
bool pageWritable(uintptr_t addr) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512]; bool w = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long s, e; char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3 && addr >= s && addr < e) { w = perms[1] == 'w'; break; }
    }
    fclose(f);
    return w;
}

bool gotHook(uintptr_t slot, void* hook, void** orig) {
    void** p = (void**)slot;
    if (*p == hook) return true;                       // already ours
    const uintptr_t pg = slot & ~(uintptr_t)(getpagesize() - 1);
    const bool wasW = pageWritable(slot);
    if (!wasW && mprotect((void*)pg, getpagesize(), PROT_READ | PROT_WRITE) != 0) return false;
    if (orig) *orig = *p;
    __atomic_store_n(p, hook, __ATOMIC_SEQ_CST);
    if (!wasW) mprotect((void*)pg, getpagesize(), PROT_READ);
    return true;
}

uintptr_t g_base = 0;
void*     g_lib  = nullptr;

// GOT slot first (preferred); optional inline fallback through AML for builds that link these symbols differently
bool hookSym(const char* sym, void* hook, void** orig, bool inlineFallback) {
    if (g_base) {
        const uintptr_t slot = findPltSlot(g_base, sym);
        if (slot) {
            if (gotHook(slot, hook, orig)) { LOGI("GOT-hooked %s (slot +0x%x, orig %p)", sym, (unsigned)(slot - g_base), orig ? *orig : nullptr); return true; }
            LOGE("GOT write failed for %s", sym);
        } else LOGE("no PLT slot for %s", sym);
    }
    if (inlineFallback && g_lib) {
        const uintptr_t a = aml->GetSym(g_lib, sym);
        if (a && aml->Hook((void*)a, hook, orig)) { LOGI("inline-hooked %s @ %p (fallback)", sym, (void*)a); return true; }
        LOGE("inline fallback failed for %s", sym);
    }
    return false;
}

void installHooks() {
    if (!g_lib) { g_lib = aml->GetLibHandle("libGTASA.so"); if (!g_lib) g_lib = aml->GetLibHandle("libGTASA"); }
    if (!g_base) g_base = aml->GetLib("libGTASA.so");
    if (!g_lib || !g_base) { LOGE("libGTASA.so not found (handle %p base %p)", g_lib, (void*)g_base); return; }

    if (!g_range)   g_range   = (float*)aml->GetSym(g_lib, "_ZN6CRadar12m_radarRangeE");
    if (!g_widgets) g_widgets = (void**)aml->GetSym(g_lib, "_ZN15CTouchInterface10m_pWidgetsE");
    if (!g_radarVt) { const uintptr_t vt = aml->GetSym(g_lib, "_ZTV12CWidgetRadar"); if (vt) g_radarVt = (void*)(vt + 8); }
    LOGI("base=%p m_radarRange=%p m_pWidgets=%p CWidgetRadar vtable obj=%p", (void*)g_base, (void*)g_range, (void*)g_widgets, g_radarVt);

    const int have = g_hooks.load();
    if (!(have & HOOK_SCOPE) && g_widgets) {
        if (hookSym("_ZN4CHud9DrawRadarEv", (void*)DrawRadar_hook, (void**)&DrawRadar_orig, true)) g_hooks.fetch_or(HOOK_SCOPE);
    }
    if (!(have & HOOK_ZOOM) && g_range) {
        bool a = hookSym("_ZN6CRadar12DrawRadarMapEv", (void*)DrawRadarMap_hook, (void**)&DrawRadarMap_orig, true);
        bool b = hookSym("_ZN6CRadar9DrawBlipsEf",     (void*)DrawBlips_hook,    (void**)&DrawBlips_orig,    true);
        if (a && b) g_hooks.fetch_or(HOOK_ZOOM);
    }
    if (!(have & HOOK_TILT)) {   // GOT only: the RwIm2D *_BUGFIX exports are 4-byte b.w thunks, never inline-hook them
        bool a = hookSym("_Z28RwIm2DRenderPrimitive_BUGFIX15RwPrimitiveTypeP14RwOpenGLVertexi", (void*)Prim_hook, (void**)&Prim_orig, false);
        bool b = hookSym("_Z35RwIm2DRenderIndexedPrimitive_BUGFIX15RwPrimitiveTypeP14RwOpenGLVertexiPti", (void*)Idx_hook, (void**)&Idx_orig, false);
        bool c = hookSym("_Z20RwIm2DRenderTriangleP14RwOpenGLVertexiiii", (void*)Tri_hook, (void**)&Tri_orig, false);
        if (a && b && c) g_hooks.fetch_or(HOOK_TILT);
    }
    LOGI("hooks=%d (1=scope 2=zoom 4=tilt, 7=all)", g_hooks.load());
}

} // namespace

// ------------------------------------------------------------------ exported C API (Lua ffi)
extern "C" {
#define HX_EXPORT __attribute__((visibility("default")))
HX_EXPORT void HexedRadar_SetZoom(int v)    { g_zoom.store(v < 50 ? 50 : (v > 250 ? 250 : v)); }
HX_EXPORT void HexedRadar_SetTilt(int v)    { g_tilt.store(v < 0 ? 0 : (v > 60 ? 60 : v)); }
HX_EXPORT int  HexedRadar_GetZoom()         { return g_zoom.load(); }
HX_EXPORT int  HexedRadar_GetTilt()         { return g_tilt.load(); }
HX_EXPORT void HexedRadar_SetEnabled(int o) { g_on.store(o ? 1 : 0); }
HX_EXPORT int  HexedRadar_Status()          { return g_hooks.load(); }          // 7 = everything hooked
HX_EXPORT int  HexedRadar_GeomSource()      { return g_geomSrc.load(); }        // 0 none, 1 radar widget (vtable verified), 2 widget by index only
HX_EXPORT int  HexedRadar_WidgetIndex()     { return g_widgetIdx.load(); }      // expected 161 on the Zan build
// 0 = CHud::DrawRadar frames, 1 = DrawRadarMap calls, 2 = DrawBlips calls, 3 = tilted RwIm2D submits, 4 = projected vertices
HX_EXPORT unsigned HexedRadar_Counter(int w) {
    switch (w) { case 0: return g_cntHud.load(); case 1: return g_cntMap.load(); case 2: return g_cntBlips.load(); case 3: return g_cntSub.load(); default: return g_cntProj.load(); }
}
}

ON_MOD_PRELOAD()   { remove(HX_LOG_PATH); LOGI("OnModPreLoad HexedRadar 2.0.0"); }
ON_MOD_LOAD()      { LOGI("OnModLoad"); installHooks(); }
ON_ALL_MODS_LOAD() { if (g_hooks.load() != HOOK_ALL) { LOGI("retry after all mods loaded"); installHooks(); } }
