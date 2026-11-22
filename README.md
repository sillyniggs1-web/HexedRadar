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
