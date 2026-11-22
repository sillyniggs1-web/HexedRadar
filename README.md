# Hexed Radar (AML mod) - radar zoom + tilt

AML mod `libHexedRadar.so` (armeabi-v7a, for Nezuko / Zan and other 32-bit SA-MP Android clients) plus a MoonLoader menu.
Built on the same layout as Discord_RPC_AML: `src/`, `third_party/aml/`, CMake, GitHub Actions.
Clean-room code: it hooks the stock `CRadar` functions of `libGTASA.so`. It contains no code from any other client.

## What it does
- `CRadar::TransformRealWorldPointToRadarSpace` hook -> Main Radar Zoom (50..250, 100 = stock)
- `CRadar::TransformRadarPointToScreenSpace` hook -> Radar Tilt (0..60 degrees)
- Exported C API for Lua: `HexedRadar_SetZoom/SetTilt/GetZoom/GetTilt/SetEnabled/Status`

## Install
```
<AML mods folder>/libHexedRadar.so
<MonetLoader scripts folder>/Hexed_Radar_Adjuster.lua      (chat command: /hradar)
```
Settings are saved by the Lua script in `Hexed Radar.ini` and reapplied each launch.

## Build
Push the folder, run **Actions -> Build HexedRadar AML mod**, take the `HexedRadar_AML` artifact.

## Status
NOT built or run yet. Unknowns: (1) whether Nezuko's and Zan's `libGTASA.so` export the two `CRadar` symbols
(if not, the menu shows "Hooks ... OFF" and logcat tag `HexedRadar` says which); (2) the tilt formula is a first guess and
may need extra hooks for the radar mask, blips and north marker; (3) whether the Zan loader uses the same AML API.


## v1.2.0 changes (analysis of the Zan APK's libGTASA.so)
- Tilt centre now comes from the radar widget `CTouchInterface::m_pWidgets[161]` rect (+0x20/+0x24/+0x28/+0x2c),
  the same data CHud::DrawRadar / DrawRadarMask use. `CRadar::m_radarRect` is a WORLD rect and is no longer used
  (it made blips fly off screen when tilt > 0). Mask polygon bounds remain as fallback.
- New hook on `CHud::DrawRadar`: zoom/tilt only apply inside it, so the pause map (`Menu_MapRender` -> `DrawBlips`) is untouched.
- Ring / north quad (`CSprite2d::Draw` 8-float variant) is tilted together with the map.
- Menu: full status is 15 (zoom, tilt map, tilt blips, HUD scope); the false "hook OFF" warning (status 7) is gone;
  shows which geometry source is in use.
- Not run on a device yet: send HexedRadar_log.txt and a short video if tilt still looks off.
