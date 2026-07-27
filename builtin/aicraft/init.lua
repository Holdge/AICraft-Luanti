-- AICraft
-- SPDX-License-Identifier: LGPL-2.1-or-later
--
-- Branded launcher. Public discovery, ContentDB, client mod management, and
-- single-player administration are intentionally absent.

local escape = core.formspec_escape
local state = {
	mode = core.settings:get("aicraft_mode") or "match",
	error = "",
}

local modes = {
	match = {
		title = "Join match",
		description = "Enter a ranked or invited AICraft match as a human player.",
		button = "JOIN MATCH",
	},
	explore = {
		title = "Free explore",
		description = "Connect to the shared exploration world.",
		button = "EXPLORE",
	},
	observe = {
		title = "Observe match",
		description = "Use an observer ticket for a read-only player-follow or free camera.",
		button = "OBSERVE",
	},
	create = {
		title = "Create world",
		description = "Create a local aicraft_game world. Publish and fork are handled by AICraft.",
		button = "CREATE WORLD",
	},
}

if not modes[state.mode] then
	state.mode = "match"
end

local function setting(name, fallback)
	local value = core.settings:get(name)
	return value and value ~= "" and value or fallback
end

local function render()
	local selected = modes[state.mode]
	local accent = "#7CF7C4"
	local muted = "#A8B4C3"
	local error_text = state.error ~= "" and
		("textarea[1.1,7.55;13.2,0.55;;;" .. escape(state.error) .. "]") or ""

	local common = table.concat({
		"formspec_version[6]",
		"size[16,9]",
		"bgcolor[#081018;true]",
		"style_type[label;font=bold;textcolor=#F4F8FB]",
		"style_type[field;border=false;bgcolor=#142330;textcolor=#F4F8FB]",
		"style_type[pwdfield;border=false;bgcolor=#142330;textcolor=#F4F8FB]",
		"style_type[button;border=false;bgcolor=#172936;textcolor=#D7E2EA]",
		"style[connect;bgcolor=" .. accent .. ";textcolor=#07120E;font=bold]",
		"label[1.1,0.75;A I C R A F T]",
		"style[tag;font=normal;textcolor=" .. muted .. "]",
		"label[1.1,1.2;Humans and agents. One world. Equal rules.]",
		"button[1.1,2.1;3.15,0.75;mode_match;JOIN MATCH]",
		"button[4.45,2.1;3.15,0.75;mode_explore;FREE EXPLORE]",
		"button[7.8,2.1;3.15,0.75;mode_create;CREATE WORLD]",
		"button[11.15,2.1;3.15,0.75;mode_observe;OBSERVE]",
		"box[1.1,3.15;13.2,0.02;" .. accent .. "]",
		"label[1.1,3.7;" .. escape(selected.title) .. "]",
		"textarea[1.1,4.15;13.2,0.65;description;;" .. escape(selected.description) .. "]",
		error_text,
		"button[12.25,8.25;2.05,0.55;quit;QUIT]",
	})

	if state.mode == "create" then
		return common .. table.concat({
			"label[1.1,5.05;World name]",
			"field[1.1,5.4;8.8,0.75;world_name;;AICraft World]",
			"button[10.2,5.4;4.1,0.75;connect;" .. selected.button .. "]",
		})
	end

	return common .. table.concat({
		"label[1.1,5.05;Player name]",
		"label[6.0,5.05;Ticket / password]",
		"field[1.1,5.4;4.55,0.75;player_name;;" .. escape(setting("name", "Player")) .. "]",
		"pwdfield[6.0,5.4;4.55,0.75;password;]",
		"label[1.1,6.35;Server]",
		"label[8.1,6.35;Port]",
		"field[1.1,6.7;6.65,0.75;address;;" .. escape(setting("address", "127.0.0.1")) .. "]",
		"field[8.1,6.7;2.45,0.75;port;;" .. escape(setting("remote_port", "31000")) .. "]",
		"button[10.9,6.7;3.4,0.75;connect;" .. selected.button .. "]",
	})
end

local function select_mode(mode)
	state.mode = mode
	state.error = ""
	core.settings:set("aicraft_mode", mode)
	core.update_formspec(render())
end

core.button_handler = function(fields)
	for _, mode in ipairs({"match", "explore", "create", "observe"}) do
		if fields["mode_" .. mode] then
			select_mode(mode)
			return true
		end
	end

	if fields.quit then
		core.close()
		return true
	end

	if not fields.connect then
		return false
	end

	if state.mode == "create" then
		local name = (fields.world_name or ""):match("^%s*(.-)%s*$")
		if name == "" then
			state.error = "Enter a world name."
			core.update_formspec(render())
			return true
		end
		local message = core.create_world(name, "aicraft_game", {
			creative_mode = "true",
			enable_damage = "false",
			aicraft_mode = "creation",
		})
		if message then
			state.error = message
			core.update_formspec(render())
			return true
		end
		for index, world in ipairs(core.get_worlds()) do
			if world.name == name then
				gamedata.selected_world = index
				gamedata.singleplayer = true
				core.start()
				return true
			end
		end
		state.error = "World was created but could not be selected."
		core.update_formspec(render())
		return true
	end

	local address = (fields.address or ""):match("^%s*(.-)%s*$")
	local player_name = (fields.player_name or ""):match("^%s*(.-)%s*$")
	local port = tonumber(fields.port)
	if address == "" or player_name == "" or not port or port < 1 or port > 65535 then
		state.error = "Enter a player name, server, and valid port."
		core.update_formspec(render())
		return true
	end

	core.settings:set("name", player_name)
	core.settings:set("address", address)
	core.settings:set("remote_port", tostring(port))
	core.settings:set("aicraft_mode", state.mode)
	gamedata.playername = player_name
	gamedata.password = fields.password or ""
	gamedata.address = address
	gamedata.port = port
	gamedata.allow_login_or_register = "any"
	gamedata.selected_world = 0
	core.start()
	return true
end

core.event_handler = function(event)
	if event == "MenuQuit" then
		core.close()
		return true
	end
	return false
end

core.update_formspec(render())
