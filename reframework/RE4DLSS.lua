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
