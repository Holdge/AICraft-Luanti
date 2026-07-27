-- AICraft-Luanti
-- SPDX-License-Identifier: LGPL-2.1-or-later
-- Added for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.
--
-- This mod is loaded only by util/aicraft/mineclonia-agent-smoke.sh. It gives
-- the real Mineclonia server a small deterministic interaction surface while
-- leaving all movement, inventory, placement, digging, and item-use behavior
-- on the normal Luanti protocol path.

local fixture_player = "agent_gate2"
local fixture_ready = {}

core.register_craftitem("aicraft_gate2_smoke:use_token", {
	description = "AICraft Gate 2 Use Token",
	inventory_image = "default_apple.png",
	stack_max = 16,
	on_use = function(itemstack, user)
		itemstack:take_item(1)
		core.chat_send_player(user:get_player_name(), "AICraft use token consumed.")
		return itemstack
	end,
})

local function prepare_fixture(player)
	if not player or not player:is_player() or
			player:get_player_name() ~= fixture_player then
		return
	end

	for x = -3, 3 do
		for z = -3, 3 do
			core.set_node({x = x, y = 9, z = z}, {name = "mcl_core:stone"})
			for y = 10, 12 do
				core.set_node({x = x, y = y, z = z}, {name = "air"})
			end
		end
	end

	local inventory = player:get_inventory()
	inventory:set_list("main", {})
	inventory:set_stack("main", 1, "mcl_core:cobble 8")
	inventory:set_stack("main", 3, "aicraft_gate2_smoke:use_token 2")
	inventory:set_stack("main", 4, "mcl_tools:pick_iron")
	player:set_physics_override({gravity = 0})
	player:set_pos({x = 0, y = 10, z = 0})
	player:set_look_horizontal(0)
	player:set_look_vertical(0)
	fixture_ready[player:get_player_name()] = true
	core.log("action", "[aicraft_gate2_smoke] fixture.ready player=" ..
		player:get_player_name())
end

core.register_on_joinplayer(function(player)
	if player:get_player_name() ~= fixture_player then
		return
	end
	core.emerge_area(
		{x = -4, y = 8, z = -4},
		{x = 4, y = 13, z = 4},
		function(_, _, calls_remaining)
			if calls_remaining ~= 0 or fixture_ready[fixture_player] then
				return
			end
			core.after(0, function()
				prepare_fixture(core.get_player_by_name(fixture_player))
			end)
		end
	)
end)

core.register_on_leaveplayer(function(player)
	fixture_ready[player:get_player_name()] = nil
end)
