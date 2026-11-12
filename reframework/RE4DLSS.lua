-- RE4DLSS companion script (REFramework autorun).
-- Sets the render size of Resident Evil 4's FSR 2 to the render resolution chosen in the RE4DLSS
-- ReShade overlay, including Native (100%), which the game's menu doesn't offer: with it, DLSS
-- runs as DLAA and FSR 3/4 as native anti-aliasing. The engine keeps FSR 2's quality in
-- via.render.UpscalingInterface.UpscalingQuality: 0 = native, 1 = Quality, 2 = Balanced,
-- 3 = Performance, 4 = Ultra Performance. The add-on writes the wanted value (-1 = leave the game's
-- setting alone) to reframework/data/RE4DLSS_mode.txt; this script applies it, keeps it applied if
-- the game sets its own again, and puts the game's value back when the add-on asks for -1. It writes
-- the current value to reframework/data/RE4DLSS_status.txt for the overlay.

local ui_type = sdk.find_type_definition("via.render.UpscalingInterface")
local original = nil  -- the game's value before the first override
local frame = 0

local function get_quality(ui)
    return sdk.call_native_func(ui, ui_type, "get_UpscalingQuality")
end

local function set_quality(ui, v)
    sdk.call_native_func(ui, ui_type, "set_UpscalingQuality", v)
end

re.on_frame(function()
    frame = frame + 1
    if frame % 30 ~= 0 or not ui_type then return end
    local ok, err = pcall(function()
        local ui = sdk.get_native_singleton("via.render.UpscalingInterface")
        if not ui then return end
        local want = tonumber(fs.read("RE4DLSS_mode.txt") or "") or -1
        local cur = get_quality(ui)
        if want >= 0 and want <= 4 then
            if original == nil then original = cur end
            if cur ~= want then
                set_quality(ui, want)
                cur = get_quality(ui)
            end
        elseif original ~= nil then
            set_quality(ui, original)
            original = nil
            cur = get_quality(ui)
        end
        fs.write("RE4DLSS_status.txt", string.format("%d %d", cur or -1, frame))
    end)
    if not ok then fs.write("RE4DLSS_status.txt", "error " .. tostring(err)) end
end)

-- Full-resolution ray traced reflections (the add-on's "Full-resolution reflections" switch).
-- The add-on writes 0 (full resolution) or -1 (leave the game's setting) to
-- reframework/data/RE4DLSS_rt.txt; this sets via.render.ExperimentalRayTrace.SpecularResolution on
-- the main camera (0 = Origin_Res, 1 = Half_Res), keeps it there, and puts the game's value back
-- on -1. The current value goes to reframework/data/RE4DLSS_rt_status.txt.
local rt_typeof = nil
local rt_original = nil
local rt_frame = 0

local function rt_component()
    local cam = sdk.get_primary_camera()
    if not cam then return nil end
    local go = cam:call("get_GameObject")
    if not go then return nil end
    if not rt_typeof then rt_typeof = sdk.typeof("via.render.ExperimentalRayTrace") end
    if not rt_typeof then return nil end
    return go:call("getComponent(System.Type)", rt_typeof)
end

re.on_frame(function()
    rt_frame = rt_frame + 1
    if rt_frame % 30 ~= 15 then return end
    local ok, err = pcall(function()
        local want = tonumber(fs.read("RE4DLSS_rt.txt") or "") or -1
        if want < 0 and rt_original == nil then return end  -- nothing asked, nothing to undo
        local c = rt_component()
        if not c then return end
        local cur = c:call("get_SpecularResolution")
        if want >= 0 then
            if rt_original == nil then rt_original = cur end
            if cur ~= want then c:call("set_SpecularResolution", want) end
        else
            c:call("set_SpecularResolution", rt_original)
            rt_original = nil
        end
        fs.write("RE4DLSS_rt_status.txt", tostring(c:call("get_SpecularResolution")))
    end)
    if not ok then fs.write("RE4DLSS_rt_status.txt", "error " .. tostring(err)) end
end)
