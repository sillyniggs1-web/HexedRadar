# Hexed Radar (AML mod) v2.0 - radar zoom + tilt

`libHexedRadar.so` (armeabi-v7a, AML) + MoonLoader menu `Hexed_Radar_Adjuster.lua` (chat command `/hradar`).

## How the PC HUD radar works (verified in Zan libGTASA.so / libSAMP.so)
- The PC HUD has no radar renderer of its own. libSAMP inline-hooks `CWidgetRadar::Update`: it calls the original, then (PC HUD flag set)
  rewrites the radar widget rect `CTouchInterface::m_pWidgets[161]` (+0x20 x0, +0x24 y0, +0x28 x1, +0x2c y1) to the PC layout
  (40..134 / 640 of the width, 344..420 / 448 of the height) and mirrors it to +0x34..+0x40.
- Stock `CHud::DrawRadar`, `DrawRadarMap/Mask/Section`, `DrawBlips`, `DrawRadarSprite`, `DrawRotatingRadarSprite`, `DrawCoordBlip`,
  `DrawEntityBlip`, `ShowRadarTraceWithHeight`, `DrawAreaOnRadar`, `DrawRadarGangOverlay`, `TransformRadarPointToScreenSpace` all read that rect inline.
- Range: `CRadar::DrawMap` writes `CRadar::m_radarRange` each frame; `DrawRadarMap` and `DrawBlips` read it.
- Everything ends in `RwIm2DRenderIndexedPrimitive_BUGFIX`, `RwIm2DRenderPrimitive_BUGFIX` or `RwIm2DRenderTriangle` (RwOpenGLVertex = 28 bytes, x@0 y@4).

## What v2 does differently
- Hooks by GOT slot (found at runtime from the ELF dynamic section by symbol name), not inline: v1 inline-hooked prologues that start with an IT block
  or a PC-relative ldr and the RwIm2D `_BUGFIX` exports are 4-byte thunks. Inline fallback is used only for DrawRadar/DrawRadarMap/DrawBlips.
- Zoom scales `m_radarRange` around DrawRadarMap and DrawBlips, only inside `CHud::DrawRadar` (pause map untouched).
- Tilt matches the reference client: the radar frame, mask, ring and N marker stay fixed; only the map plane (DrawRadarSection, DrawAreaOnRadar) is projected, and blips are moved to their tilted position without being distorted.
- The radar widget is located by its `CWidgetRadar` vtable (index 161 on this build) before its rect is trusted.
- Menu shows: hooks (scope / zoom / tilt, 7 = all), widget index, frame/draw/vertex counters. Log: `/storage/emulated/0/HexedRadar_log.txt`.

## Build / install
Push, run Actions -> "Build HexedRadar AML mod", take the artifact. `libHexedRadar.so` -> AML mods folder; Lua -> MonetLoader scripts.

## Status
Static analysis + compile check only; NOT run on a device. Map tilt is a per-triangle projection (textures are not perspective-correct on big map quads).
