# HexedRadar

Radar zoom and tilt adjuster for GTA:SA mobile (SA-MP clients), 32-bit ARM, built as an AML mod plus a MoonLoader script.
Status: early, untested on a device.

## Layout
- `jni/` native mod (`libHexedRadar.so`)
- `lua/` MoonLoader menu (`/hradar`)
- `.github/workflows/build.yml` CI build

## Build
Push to `main`, open a PR, or run the workflow by hand: the zip appears under Actions > Artifacts.
Push a tag like `v1.0` to publish it as a GitHub Release.

## Install
Put `libHexedRadar.so` in the AML mods folder of the client and the `.lua` in its MoonLoader scripts folder.

## API (ffi)
`HexedRadar_SetZoom(int 50..250)`, `HexedRadar_SetTilt(int 0..60)`, `HexedRadar_GetZoom()`, `HexedRadar_GetTilt()`, `HexedRadar_SetEnabled(int)`.
