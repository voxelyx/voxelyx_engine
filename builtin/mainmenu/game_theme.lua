-- Voxelyx
-- Copyright (C) 2013 sapier
-- SPDX-License-Identifier: LGPL-2.1-or-later

mm_game_theme = {}

local COLORS = {
	dark = { clouds = "#1c2a47", sky = "#090b1a" },
	light = { clouds = "#f0f0ff", sky = "#8cbafa" },
	halloween = { clouds = "#d2691e", sky = "#1a0b2e" },
}

--------------------------------------------------------------------------------
function mm_game_theme.init()
	mm_game_theme.texturepack = pkgmgr.get_enabled_texture_packs()[1]

	mm_game_theme.music_handle = nil
end

--------------------------------------------------------------------------------
function mm_game_theme.set_engine(hide_decorations)
	mm_game_theme.stop_music()

	core.set_topleft_text("")

	local have_bg = false
	local have_overlay = mm_game_theme.set_engine_single("overlay")

	if not have_overlay then
		have_bg = mm_game_theme.set_engine_single("background")
	end

	mm_game_theme.clear_single("header")
	mm_game_theme.clear_single("footer")
	core.set_clouds(false)

	if not hide_decorations then
		mm_game_theme.set_engine_single("header")
		mm_game_theme.set_engine_single("footer")
	end

	local c = COLORS[core.settings:get("menu_theme")]
	if not core.settings:has("menu_theme") and os.date("%m-%d") == "10-31" then
		c = COLORS.halloween
	end
	if not c then
		core.log("warning", "Invalid menu theme: " .. core.settings:get("menu_theme"))
	else
		core.set_clouds_color(c.clouds)
		core.set_sky_color(c.sky)
	end

	if not have_bg then
		core.set_clouds(core.settings:get_bool("menu_clouds"))
	end
end

--------------------------------------------------------------------------------
function mm_game_theme.clear_single(identifier)
	core.set_background(identifier, "")
end

--------------------------------------------------------------------------------
local valid_image_extensions = {
	".png",
	".jpg",
	".jpeg",
}

function mm_game_theme.set_engine_single(identifier)
	--try texture pack first
	if mm_game_theme.texturepack ~= nil then
		for _, extension in pairs(valid_image_extensions) do
			local path = mm_game_theme.texturepack .. DIR_DELIM .. "menu_" .. identifier .. extension
			if core.set_background(identifier, path) then
				return true
			end
		end
	end

	local path = defaulttexturedir .. DIR_DELIM .. "menu_" .. identifier .. ".png"
	if core.set_background(identifier, path) then
		return true
	end

	return false
end

--------------------------------------------------------------------------------
function mm_game_theme.stop_music()
	if mm_game_theme.music_handle ~= nil then
		core.sound_stop(mm_game_theme.music_handle)
	end
end
