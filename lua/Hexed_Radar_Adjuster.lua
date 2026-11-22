script_name("Hexed Radar Adjuster")
script_author("HEXED Studio")
script_version("1.1")
if script_properties then pcall(script_properties, "work-in-pause") end
require "lib.moonloader"
local imgui  = require("mimgui")
local inicfg = require("inicfg")
local ffi    = require("ffi")
local DPI    = MONET_DPI_SCALE or 1

-- ===== native backend: libHexedRadar.so (ours) or libMultiSA.so (MultiSA client) =====
local R = { lib = nil, kind = "none", err = "" }

-- AML loads mods with its own dlopen, so find the already-loaded .so path in /proc/self/maps (same as Discord_RPC helper)
local function findLoaded(word)
    local f = io.open("/proc/self/maps", "r")
    local path
    if f then
        for line in f:lines() do
            local p = line:match("(/%S*%.so)%s*$")
            if p and p:match("([^/]+)$"):lower():find(word, 1, true) then path = p break end
        end
        f:close()
    end
    return path
end

local function tryHexed()
    pcall(ffi.cdef, [[ void HexedRadar_SetZoom(int); void HexedRadar_SetTilt(int);
               int  HexedRadar_GetZoom(void); int HexedRadar_GetTilt(void);
               void HexedRadar_SetEnabled(int); int HexedRadar_Status(void); ]])
    for _, what in ipairs({ findLoaded("hexedradar"), "libHexedRadar.so", "HexedRadar" }) do
        if what then
            local ok, lib = pcall(ffi.load, what)
            if ok and lib then
                local ok2, fn = pcall(function() return lib.HexedRadar_GetZoom end)
                if ok2 and fn then return lib end
            end
        end
    end
    error("libHexedRadar.so not loaded (is it in the AML mods folder?)")
end
local function tryMulti()
    pcall(ffi.cdef, [[ void _ZN7Radar3D7SetZoomEi(int); void _ZN7Radar3D7SetTiltEi(int);
               int  _ZN7Radar3D7GetZoomEv(void); int _ZN7Radar3D7GetTiltEv(void);
               void _ZN7Radar3D10SetPreviewEb(bool); ]])
    local lib = ffi.load("MultiSA")
    local _ = lib._ZN7Radar3D7GetZoomEv
    return lib
end

local function initBackend()
    local ok, lib = pcall(tryHexed)
    if ok then R.lib, R.kind = lib, "hexed" return end
    R.err = tostring(lib)
    ok, lib = pcall(tryMulti)
    if ok then R.lib, R.kind = lib, "multisa" return end
    R.err = R.err .. " | " .. tostring(lib)
end

local function nSet(zoom, tilt)
    if not R.lib then return end
    pcall(function()
        if R.kind == "hexed" then
            R.lib.HexedRadar_SetZoom(zoom); R.lib.HexedRadar_SetTilt(tilt)
        else
            R.lib._ZN7Radar3D7SetZoomEi(zoom); R.lib._ZN7Radar3D7SetTiltEi(tilt)
        end
    end)
end
local function nEnabled(on)
    if R.kind == "hexed" then pcall(function() R.lib.HexedRadar_SetEnabled(on and 1 or 0) end) end
end
local function nPreview(on)
    if R.kind == "multisa" then pcall(function() R.lib._ZN7Radar3D10SetPreviewEb(on) end) end
end
local function nGet()
    if not R.lib then return nil, nil end
    local ok, z, t = pcall(function()
        if R.kind == "hexed" then return R.lib.HexedRadar_GetZoom(), R.lib.HexedRadar_GetTilt() end
        return R.lib._ZN7Radar3D7GetZoomEv(), R.lib._ZN7Radar3D7GetTiltEv()
    end)
    if ok then return z, t end
end

-- ===== config =====
local ZMIN, ZMAX, TMIN, TMAX = 50, 250, 0, 60
local cfg = inicfg.load({
    radar = { enabled = true, zoom = 100, tilt = 0, preview = false },
    meta  = { ver = 1 },
}, "Hexed Radar.ini")

local S = {
    menu    = imgui.new.bool(false),
    enabled = imgui.new.bool(cfg.radar.enabled),
    zoom    = imgui.new.int(math.max(ZMIN, math.min(ZMAX, cfg.radar.zoom))),
    tilt    = imgui.new.int(math.max(TMIN, math.min(TMAX, cfg.radar.tilt))),
    preview = imgui.new.bool(false),
    status  = "",
}

local function saveRadarCfg()
    cfg.radar.enabled = S.enabled[0]
    cfg.radar.zoom    = S.zoom[0]
    cfg.radar.tilt    = S.tilt[0]
    inicfg.save(cfg, "Hexed Radar.ini")
end

local function apply()
    S.zoom[0] = math.max(ZMIN, math.min(ZMAX, S.zoom[0]))
    S.tilt[0] = math.max(TMIN, math.min(TMAX, S.tilt[0]))
    nEnabled(S.enabled[0])
    if S.enabled[0] then nSet(S.zoom[0], S.tilt[0]) else nSet(100, 0) end
    saveRadarCfg()
end

-- ===== UI =====
imgui.OnInitialize(function()
    local st, c = imgui.GetStyle(), imgui.GetStyle().Colors
    st.WindowRounding, st.FrameRounding, st.GrabRounding = 10 * DPI, 6 * DPI, 6 * DPI
    st.WindowBorderSize = 1
    c[imgui.Col.WindowBg]      = imgui.ImVec4(0.02, 0.03, 0.07, 0.96)
    c[imgui.Col.Border]        = imgui.ImVec4(0.10, 0.35, 0.85, 1)
    c[imgui.Col.FrameBg]       = imgui.ImVec4(0.06, 0.09, 0.18, 1)
    c[imgui.Col.SliderGrab]    = imgui.ImVec4(0.10, 0.35, 0.85, 1)
    c[imgui.Col.CheckMark]     = imgui.ImVec4(0.20, 0.55, 1.00, 1)
    c[imgui.Col.Button]        = imgui.ImVec4(0.08, 0.20, 0.55, 1)
    c[imgui.Col.ButtonHovered] = imgui.ImVec4(0.12, 0.30, 0.75, 1)
    c[imgui.Col.ButtonActive]  = imgui.ImVec4(0.10, 0.35, 0.85, 1)
end)

local function widthLong() return imgui.GetContentRegionAvail().x - 150 * DPI end

imgui.OnFrame(function() return S.menu[0] end, function()
    local io = imgui.GetIO()
    imgui.SetNextWindowPos(imgui.ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), imgui.Cond.Appearing, imgui.ImVec2(0.5, 0.5))
    imgui.SetNextWindowSize(imgui.ImVec2(520 * DPI, 400 * DPI), imgui.Cond.Always)
    imgui.Begin("##hexedradar", S.menu, imgui.WindowFlags.NoTitleBar + imgui.WindowFlags.NoResize + imgui.WindowFlags.NoScrollbar)

    local title = "RADAR ADJUSTER"
    imgui.SetCursorPosX((imgui.GetWindowWidth() - imgui.CalcTextSize(title).x) / 2)
    imgui.TextColored(imgui.ImVec4(0.20, 0.55, 1, 1), title)
    imgui.Separator()

    if imgui.Checkbox("Enable Radar Adjuster", S.enabled) then apply() end

    imgui.PushItemWidth(widthLong())
    if imgui.SliderInt("Main Radar Zoom", S.zoom, ZMIN, ZMAX) then apply() end
    if imgui.InputInt("##zoomv", S.zoom, 5, 25) then apply() end
    if imgui.SliderInt("Radar Tilt", S.tilt, TMIN, TMAX) then apply() end
    if imgui.InputInt("##tiltv", S.tilt, 1, 5) then apply() end
    imgui.PopItemWidth()

    if R.kind == "multisa" then
        if imgui.Checkbox("Live Preview", S.preview) then nPreview(S.preview[0]) end
    end

    if imgui.Button("Reset", imgui.ImVec2(130 * DPI, 35 * DPI)) then
        S.zoom[0], S.tilt[0] = 100, 0; apply()
    end
    imgui.SameLine()
    if imgui.Button("Close", imgui.ImVec2(130 * DPI, 35 * DPI)) then S.menu[0] = false; nPreview(false) end

    imgui.Separator()
    local z, t = nGet()
    imgui.Text(("Backend: %s"):format(R.kind))
    if z then imgui.Text(("Native values: zoom %d  tilt %d"):format(z, t)) end
    if R.kind == "hexed" then
        local ok, hk = pcall(function() return R.lib.HexedRadar_Status() end)
        hk = ok and hk or 0
        imgui.Text(("Hooks: zoom %s  tilt %s"):format(bit.band(hk,1)~=0 and "ON" or "OFF", bit.band(hk,2)~=0 and "ON" or "OFF"))
        if hk ~= 3 then imgui.TextWrapped("A hook is OFF: this client's libGTASA has no matching CRadar symbol. See logcat tag HexedRadar.") end
    end
    if R.kind == "none" then
        imgui.TextWrapped("No radar library found. Put libHexedRadar.so in the AML mods folder.")
    end
    imgui.End()
end)

-- ===== main =====
function main()
    while not isSampAvailable or not isSampAvailable() do wait(100) end
    initBackend()
    apply()
    sampRegisterChatCommand("hradar", function() S.menu[0] = not S.menu[0] end)
    imgui.Process = true
    while true do wait(0) imgui.Process = true end
end
