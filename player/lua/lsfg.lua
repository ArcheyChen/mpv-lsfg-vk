local mp = require "mp"
local msg = require "mp.msg"

local function get_opt(name, fallback)
    local v = mp.get_property_native("options/" .. name)
    if v == nil then
        return fallback
    end
    return v
end

local function get_prop(name, fallback)
    local v = mp.get_property_native(name)
    if v == nil then
        return fallback
    end
    return v
end

local function set_opt(name, value)
    local ok, err = pcall(mp.set_property_native, "options/" .. name, value)
    if not ok then
        msg.error(string.format("failed to set %s: %s", name, tostring(err)))
        return false
    end
    return true
end

local function clamp(v, lo, hi)
    if v < lo then
        return lo
    end
    if v > hi then
        return hi
    end
    return v
end

local get_runtime_stats
local rt_history = {}

local function read_runtime_stats_cumulative()
    local s = get_prop("lsfg-stats", nil)
    if type(s) ~= "table" then
        return {total = 0, interpolated = 0, real = 0, fallback = 0, interp_ratio = 0}
    end
    return {
        total = tonumber(s.total) or 0,
        interpolated = tonumber(s.interpolated) or 0,
        real = tonumber(s.real) or 0,
        fallback = tonumber(s.fallback) or 0,
        interp_ratio = tonumber(s.interp_ratio) or 0,
    }
end

local function clamp_non_negative(v)
    if v < 0 then
        return 0
    end
    return v
end

get_runtime_stats = function(window_sec)
    local now = mp.get_time() or 0
    local win = tonumber(window_sec) or 0
    local cur = read_runtime_stats_cumulative()

    if #rt_history > 0 then
        local last = rt_history[#rt_history]
        if cur.total < last.total or cur.interpolated < last.interpolated or
           cur.real < last.real or cur.fallback < last.fallback
        then
            rt_history = {}
        end
    end

    rt_history[#rt_history + 1] = {
        t = now,
        total = cur.total,
        interpolated = cur.interpolated,
        real = cur.real,
        fallback = cur.fallback,
    }

    local keep_from = now - math.max(2.5, win * 2.0)
    while #rt_history > 2 and rt_history[1].t < keep_from do
        table.remove(rt_history, 1)
    end

    if win <= 0 then
        return cur
    end

    if #rt_history < 2 then
        return {total = 0, interpolated = 0, real = 0, fallback = 0, interp_ratio = 0}
    end

    local threshold = now - win
    local ref = rt_history[1]
    for i = 2, #rt_history do
        local sample = rt_history[i]
        if sample.t <= threshold then
            ref = sample
        else
            break
        end
    end

    local d_total = clamp_non_negative(cur.total - ref.total)
    local d_interp = clamp_non_negative(cur.interpolated - ref.interpolated)
    local d_real = clamp_non_negative(cur.real - ref.real)
    local d_fallback = clamp_non_negative(cur.fallback - ref.fallback)
    local ratio = d_total > 0 and (d_interp / d_total) or 0

    return {
        total = d_total,
        interpolated = d_interp,
        real = d_real,
        fallback = d_fallback,
        interp_ratio = ratio,
    }
end

local function show_status()
    local enabled = get_opt("lsfg-enable", false)
    local multiplier = get_opt("lsfg-multiplier", 2)
    local flow = get_opt("lsfg-flow-scale", 1.0)
    local mode = get_opt("lsfg-performance-mode", "quality")
    local vsync_mode = tostring(get_opt("video-sync", ""))
    local ready = vsync_mode == "display-resample"

    local src_fps = tonumber(get_prop("estimated-vf-fps", 0)) or 0
    if src_fps <= 0 then
        src_fps = tonumber(get_prop("container-fps", 0)) or 0
    end
    local display_fps = tonumber(get_prop("display-fps", 0)) or 0
    local target_fps = src_fps * (tonumber(multiplier) or 2)
    local out_fps = target_fps
    if display_fps > 0 then
        out_fps = math.min(out_fps, display_fps)
    end
    local paused = get_prop("pause", false)
    local active = enabled and ready and (not paused) and out_fps > (src_fps + 0.5)
    local rt = get_runtime_stats(1.0)

    local text = string.format("LSFG %s (%s) | src %.1f -> out %.1f | x%d flow %.2f %s | rt1s %.1f%% (%d/%d) | sync=%s%s",
                               enabled and "on" or "off",
                               active and "ACTIVE" or (paused and "PAUSED" or "IDLE"),
                               src_fps,
                               out_fps,
                               tonumber(multiplier) or 2,
                               tonumber(flow) or 1.0,
                               tostring(mode),
                               rt.interp_ratio * 100.0,
                               rt.interpolated,
                               rt.total,
                               vsync_mode,
                               ready and "" or " (set display-resample)")
    mp.osd_message(text, 1.4)
end

local function enforce_fixed_settings()
    set_opt("lsfg-strict", true)
    set_opt("lsfg-processing-res", "video")
end

local function ensure_prerequisites()
    enforce_fixed_settings()
    if tostring(get_opt("video-sync", "")) ~= "display-resample" then
        set_opt("video-sync", "display-resample")
    end
end

local menu = {
    active = false,
    index = 1,
}
local menu_refresh_timer = nil

local function menu_items()
    return {
        {
            label = "Enable",
            value = function()
                return get_opt("lsfg-enable", false) and "on" or "off"
            end,
            apply = function(dir)
                local enabled = get_opt("lsfg-enable", false)
                local next_enabled
                if dir == 0 then
                    next_enabled = not enabled
                else
                    next_enabled = dir > 0
                end
                if next_enabled then
                    ensure_prerequisites()
                end
                set_opt("lsfg-enable", next_enabled)
            end,
        },
        {
            label = "Multiplier",
            value = function()
                return "x" .. tostring(tonumber(get_opt("lsfg-multiplier", 2)) or 2)
            end,
            apply = function(dir)
                local order = {2, 3, 4}
                local cur = tonumber(get_opt("lsfg-multiplier", 2)) or 2
                local idx = 1
                for i, v in ipairs(order) do
                    if v == cur then
                        idx = i
                        break
                    end
                end
                if dir == 0 then
                    idx = (idx % #order) + 1
                elseif dir > 0 then
                    idx = math.min(#order, idx + 1)
                else
                    idx = math.max(1, idx - 1)
                end
                set_opt("lsfg-multiplier", order[idx])
            end,
        },
        {
            label = "Mode",
            value = function()
                return tostring(get_opt("lsfg-performance-mode", "quality"))
            end,
            apply = function(_)
                local mode = tostring(get_opt("lsfg-performance-mode", "quality"))
                local next_mode = mode == "quality" and "performance" or "quality"
                set_opt("lsfg-performance-mode", next_mode)
            end,
        },
        {
            label = "Flow scale",
            value = function()
                return string.format("%.2f", tonumber(get_opt("lsfg-flow-scale", 1.0)) or 1.0)
            end,
            apply = function(dir)
                local flow = tonumber(get_opt("lsfg-flow-scale", 1.0)) or 1.0
                local step = 0.05
                if dir == 0 then
                    dir = 1
                end
                flow = clamp(flow + step * dir, 0.25, 1.0)
                set_opt("lsfg-flow-scale", flow)
            end,
        },
    }
end

local function render_menu()
    if not menu.active then
        return
    end

    local items = menu_items()
    menu.index = clamp(menu.index, 1, #items)

    local src_fps = tonumber(get_prop("estimated-vf-fps", 0)) or 0
    if src_fps <= 0 then
        src_fps = tonumber(get_prop("container-fps", 0)) or 0
    end
    local multiplier = tonumber(get_opt("lsfg-multiplier", 2)) or 2
    local display_fps = tonumber(get_prop("display-fps", 0)) or 0
    local target_fps = src_fps * multiplier
    local out_fps = target_fps
    if display_fps > 0 then
        out_fps = math.min(target_fps, display_fps)
    end
    local ready = tostring(get_opt("video-sync", "")) == "display-resample"
    local paused = get_prop("pause", false)
    local enabled = get_opt("lsfg-enable", false)
    local active = enabled and ready and (not paused) and out_fps > (src_fps + 0.5)
    local state_text = active and "ACTIVE" or (paused and "PAUSED" or "IDLE")
    local rt = get_runtime_stats(1.0)

    local lines = {"LSFG Menu", "Author: Ausar", string.format("State: %s", state_text),
                   string.format("FPS: %.1f -> %.1f (display %.1f)", src_fps, out_fps, display_fps),
                   string.format("RT (1s): %.1f%% interpolated (%d/%d), real=%d, fallback=%d",
                                 rt.interp_ratio * 100.0, rt.interpolated, rt.total,
                                 rt.real, rt.fallback),
                   ""}
    for i, item in ipairs(items) do
        local prefix = (i == menu.index) and "> " or "  "
        lines[#lines + 1] = string.format("%s%s: %s", prefix, item.label, item.value())
    end
    lines[#lines + 1] = ""
    lines[#lines + 1] = "Up/Down: select  Left/Right/Enter: change  Esc: close"
    mp.osd_message(table.concat(lines, "\n"), 60)
end

local function menu_apply(dir)
    local items = menu_items()
    local item = items[menu.index]
    if item and item.apply then
        item.apply(dir)
    end
    render_menu()
end

local function menu_move(delta)
    local items = menu_items()
    menu.index = clamp(menu.index + delta, 1, #items)
    render_menu()
end

local function menu_close()
    if not menu.active then
        return
    end
    menu.active = false
    mp.remove_key_binding("lsfg-menu-up")
    mp.remove_key_binding("lsfg-menu-down")
    mp.remove_key_binding("lsfg-menu-left")
    mp.remove_key_binding("lsfg-menu-right")
    mp.remove_key_binding("lsfg-menu-enter")
    mp.remove_key_binding("lsfg-menu-esc")
    mp.remove_key_binding("lsfg-menu-wheel-up")
    mp.remove_key_binding("lsfg-menu-wheel-down")
    if menu_refresh_timer then
        menu_refresh_timer:kill()
        menu_refresh_timer = nil
    end
    mp.osd_message("", 0)
    show_status()
end

local function menu_open()
    if menu.active then
        render_menu()
        return
    end
    menu.active = true
    mp.add_forced_key_binding("UP", "lsfg-menu-up", function() menu_move(-1) end, {repeatable = true})
    mp.add_forced_key_binding("DOWN", "lsfg-menu-down", function() menu_move(1) end, {repeatable = true})
    mp.add_forced_key_binding("LEFT", "lsfg-menu-left", function() menu_apply(-1) end, {repeatable = true})
    mp.add_forced_key_binding("RIGHT", "lsfg-menu-right", function() menu_apply(1) end, {repeatable = true})
    mp.add_forced_key_binding("ENTER", "lsfg-menu-enter", function() menu_apply(0) end)
    mp.add_forced_key_binding("ESC", "lsfg-menu-esc", menu_close)
    mp.add_forced_key_binding("WHEEL_UP", "lsfg-menu-wheel-up", function() menu_apply(1) end, {repeatable = true})
    mp.add_forced_key_binding("WHEEL_DOWN", "lsfg-menu-wheel-down", function() menu_apply(-1) end, {repeatable = true})
    menu_refresh_timer = mp.add_periodic_timer(0.5, function()
        if menu.active then
            render_menu()
        end
    end)
    render_menu()
end

local function menu_toggle()
    if menu.active then
        menu_close()
    else
        menu_open()
    end
end

local function toggle_enable()
    local enabled = get_opt("lsfg-enable", false)
    local next_enabled = not enabled
    if next_enabled then
        ensure_prerequisites()
    end
    if set_opt("lsfg-enable", next_enabled) then
        if menu.active then
            render_menu()
        else
            show_status()
        end
    end
end

local function cycle_multiplier()
    local m = tonumber(get_opt("lsfg-multiplier", 2)) or 2
    if m <= 2 then
        m = 3
    elseif m == 3 then
        m = 4
    else
        m = 2
    end
    if set_opt("lsfg-multiplier", m) then
        if menu.active then
            render_menu()
        else
            show_status()
        end
    end
end

local function cycle_mode()
    local mode = tostring(get_opt("lsfg-performance-mode", "quality"))
    local next_mode = mode == "quality" and "performance" or "quality"
    if set_opt("lsfg-performance-mode", next_mode) then
        if menu.active then
            render_menu()
        else
            show_status()
        end
    end
end

local function adjust_flow(delta)
    local flow = tonumber(get_opt("lsfg-flow-scale", 1.0)) or 1.0
    flow = clamp(flow + delta, 0.25, 1.0)
    if set_opt("lsfg-flow-scale", flow) then
        if menu.active then
            render_menu()
        else
            show_status()
        end
    end
end

local function flow_up()
    adjust_flow(0.1)
end

local function flow_down()
    adjust_flow(-0.1)
end

mp.add_key_binding(nil, "lsfg-toggle", toggle_enable)
mp.add_key_binding(nil, "lsfg-multiplier", cycle_multiplier)
mp.add_key_binding(nil, "lsfg-mode", cycle_mode)
mp.add_key_binding(nil, "lsfg-flow-up", flow_up, {repeatable = true})
mp.add_key_binding(nil, "lsfg-flow-down", flow_down, {repeatable = true})
mp.add_key_binding(nil, "lsfg-status", show_status)

mp.register_script_message("toggle", toggle_enable)
mp.register_script_message("cycle-multiplier", cycle_multiplier)
mp.register_script_message("cycle-mode", cycle_mode)
mp.register_script_message("flow-up", flow_up)
mp.register_script_message("flow-down", flow_down)
mp.register_script_message("status", show_status)
mp.register_script_message("menu-open", menu_open)
mp.register_script_message("menu-close", menu_close)
mp.register_script_message("menu-toggle", menu_toggle)

enforce_fixed_settings()
