script_name("Hexed Radar Adjuster")
script_author("HEXED Studio")
script_version("2.0")
if script_properties then pcall(script_properties, "work-in-pause") end
require "lib.moonloader"
local imgui  = require("mimgui")
local inicfg = require("inicfg")
local ffi    = require("ffi")
local bit    = require("bit")
local DPI    = MONET_DPI_SCALE or 1

-- ===================== native link (same method as HexedVoiceMod_Menu) =====================
pcall(ffi.cdef, [[
    void* dlopen(const char* name, int flags);
    void* dlsym(void* handle, const char* symbol);
]])

local R = { kind = "none", f = nil, log = {}, tried = false }
local function note(s) R.log[#R.log + 1] = s if #R.log > 60 then table.remove(R.log, 1) end end

-- key = { exported symbol, function pointer type, optional }
local SYM_HEXED = {
    setZoom = { "HexedRadar_SetZoom",  "void(*)(int)" },
    setTilt = { "HexedRadar_SetTilt",  "void(*)(int)" },
    getZoom = { "HexedRadar_GetZoom",  "int(*)(void)" },
    getTilt = { "HexedRadar_GetTilt",  "int(*)(void)" },
    enabled = { "HexedRadar_SetEnabled", "void(*)(int)", true },
    status  = { "HexedRadar_Status",   "int(*)(void)", true },
    geom    = { "HexedRadar_GeomSource", "int(*)(void)", true },
    widx    = { "HexedRadar_WidgetIndex", "int(*)(void)", true },
    counter = { "HexedRadar_Counter", "unsigned(*)(int)", true },
}
local SYM_MULTI = {
    setZoom = { "_ZN7Radar3D7SetZoomEi", "void(*)(int)" },
    setTilt = { "_ZN7Radar3D7SetTiltEi", "void(*)(int)" },
    getZoom = { "_ZN7Radar3D7GetZoomEv", "int(*)(void)" },
    getTilt = { "_ZN7Radar3D7GetTiltEv", "int(*)(void)" },
    preview = { "_ZN7Radar3D10SetPreviewEb", "void(*)(bool)", true },
}

local function bind(handle, map)
    local t = {}
    for k, v in pairs(map) do
        local s = ffi.C.dlsym(handle, v[1])
        if s == nil then
            if not v[3] then return nil, v[1] end
        else
            t[k] = ffi.cast(v[2], s)
        end
    end
    return t
end

local function tryHandle(h, map, what)
    if h == nil then note(what .. ": no handle") return nil end
    local t, miss = bind(h, map)
    if t then note(what .. ": OK") return t end
    note(what .. ": handle ok, symbol missing: " .. tostring(miss))
end

local SKIP = { "/system/", "/apex/", "/vendor/", "/product/", "/odm/", "/system_ext/" }
local function skipPath(p)
    for _, s in ipairs(SKIP) do if p:sub(1, #s) == s then return true end end
end

local function mapsList(word)
    local named, others, seen = {}, {}, {}
    local f = io.open("/proc/self/maps", "r")
    if not f then note("cannot open /proc/self/maps") return named, others end
    for line in f:lines() do
        local p = line:match("(/[^%s]+%.so)")
        if p and not seen[p] then
            seen[p] = true
            if p:lower():find(word, 1, true) then named[#named + 1] = p
            elseif not skipPath(p) then others[#others + 1] = p end
        end
    end
    f:close()
    return named, others
end

local function link(libName, word, map)
    -- 1) RTLD_NOW|RTLD_NOLOAD (6): succeeds only if AML already loaded the mod
    local ok, h = pcall(function() return ffi.C.dlopen(libName, 6) end)
    local t = ok and tryHandle(h, map, "dlopen(" .. libName .. ",NOLOAD)")
    if t then return t end
    -- 2) real path from the process maps (named matches first, then any non-system .so: AML may use a renamed copy)
    local named, others = mapsList(word)
    note(("maps: %d named '%s', %d other .so"):format(#named, word, #others))
    for _, list in ipairs({ named, others }) do
        for _, p in ipairs(list) do
            local ok2, h2 = pcall(function() return ffi.C.dlopen(p, 6) end)
            t = ok2 and tryHandle(h2, map, p:match("([^/]+)$") or p)
            if t then return t end
        end
    end
    -- 3) plain dlopen by name (loads it if it was not loaded)
    local ok3, h3 = pcall(function() return ffi.C.dlopen(libName, 2) end)
    return ok3 and tryHandle(h3, map, "dlopen(" .. libName .. ")")
end

local function initBackend()
    R.log = {}
    local t = link("libHexedRadar.so", "hexedradar", SYM_HEXED)
    if t then R.f, R.kind = t, "hexed" return end
    note("libHexedRadar.so not found, trying MultiSA client")
    t = link("libMultiSA.so", "multisa", SYM_MULTI)
    if t then R.f, R.kind = t, "multisa" return end
    R.kind = "none"
end

local function nSet(zoom, tilt)
    if R.f then pcall(function() R.f.setZoom(zoom); R.f.setTilt(tilt) end) end
end
local function nEnabled(on)
    if R.f and R.f.enabled then pcall(function() R.f.enabled(on and 1 or 0) end) end
end
local function nPreview(on)
    if R.f and R.f.preview then pcall(function() R.f.preview(on) end) end
end
local function nGet()
    if not R.f then return nil end
    local ok, z, t = pcall(function() return R.f.getZoom(), R.f.getTilt() end)
    if ok then return z, t end
end
local function nStatus()
    if R.f and R.f.status then
        local ok, v = pcall(function() return R.f.status() end)
        if ok then return v end
    end
    return 0
end

-- ===================== config =====================
local ZMIN, ZMAX, TMIN, TMAX = 50, 250, 0, 60
local cfg = inicfg.load({
    radar = { enabled = true, zoom = 100, tilt = 0 },
    meta  = { ver = 1 },
}, "Hexed Radar.ini")

local S = {
    menu    = imgui.new.bool(false),
    enabled = imgui.new.bool(cfg.radar.enabled),
    zoom    = imgui.new.int(math.max(ZMIN, math.min(ZMAX, cfg.radar.zoom))),
    tilt    = imgui.new.int(math.max(TMIN, math.min(TMAX, cfg.radar.tilt))),
    preview = imgui.new.bool(false),
    details = imgui.new.bool(false),
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

-- ===================== UI =====================
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
    imgui.SetNextWindowSize(imgui.ImVec2(520 * DPI, 420 * DPI), imgui.Cond.Always)
    imgui.Begin("##hexedradar", S.menu, imgui.WindowFlags.NoTitleBar + imgui.WindowFlags.NoResize)

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

    if R.kind == "multisa" and R.f and R.f.preview then
        if imgui.Checkbox("Live Preview", S.preview) then nPreview(S.preview[0]) end
    end

    if imgui.Button("Reset", imgui.ImVec2(130 * DPI, 35 * DPI)) then
        S.zoom[0], S.tilt[0] = 100, 0; apply()
    end
    imgui.SameLine()
    if imgui.Button("Close", imgui.ImVec2(130 * DPI, 35 * DPI)) then S.menu[0] = false; nPreview(false) end
    imgui.SameLine()
    if imgui.Button("Retry link", imgui.ImVec2(130 * DPI, 35 * DPI)) then initBackend(); apply() end

    imgui.Separator()
    imgui.Text("Backend: " .. R.kind)
    local z, t = nGet()
    if z then imgui.Text(("Native values: zoom %d  tilt %d"):format(z, t)) end
    if R.kind == "hexed" then
        local hk = nStatus()
        local function on(b) return bit.band(hk, b) ~= 0 and "ON" or "OFF" end
        imgui.Text(("Hooks: scope %s  zoom %s  tilt %s"):format(on(1), on(2), on(4)))
        local g, wi = 0, -1
        if R.f and R.f.geom then local ok, v = pcall(function() return R.f.geom() end); if ok then g = v end end
        if R.f and R.f.widx then local ok, v = pcall(function() return R.f.widx() end); if ok then wi = v end end
        imgui.Text(("Radar widget: %s (index %d)"):format(g == 1 and "found by vtable" or (g == 2 and "by index only" or "not found yet"), wi))
        if R.f and R.f.counter then
            local ok, a, b, c, d = pcall(function() local f = R.f.counter; return f(0), f(1), f(3), f(4) end)
            if ok then imgui.Text(("Frames %d  map %d  tilted draws %d  vertices %d"):format(tonumber(a), tonumber(b), tonumber(c), tonumber(d))) end
        end
        if hk ~= 7 then imgui.TextWrapped("A hook is OFF: see HexedRadar_log.txt (it names the symbol / PLT slot that failed).") end
    elseif R.kind == "none" then
        imgui.TextWrapped("libHexedRadar.so is not loaded by AML. Check: file is in the AML mods folder of THIS client, Modloader ON, game restarted after copying. Tap Retry link after fixing.")
    end
    imgui.Checkbox("Show link details", S.details)
    if S.details[0] then
        for i = math.max(1, #R.log - 14), #R.log do imgui.TextWrapped(R.log[i]) end
    end
    imgui.End()
end)

-- ===================== main =====================
function main()
    while not isSampAvailable or not isSampAvailable() do wait(100) end
    initBackend()
    apply()
    sampRegisterChatCommand("hradar", function()
        S.menu[0] = not S.menu[0]
        if S.menu[0] and R.kind == "none" then initBackend(); apply() end
    end)
    imgui.Process = true
    local t = os.clock()
    while true do
        wait(0); imgui.Process = true
        if os.clock() - t > 3 then t = os.clock(); if R.kind ~= "none" then nEnabled(S.enabled[0]); if S.enabled[0] then nSet(S.zoom[0], S.tilt[0]) else nSet(100, 0) end end end   -- re-send values (survives a mod reload)
    end
end
