// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "lua_api/l_base.h"

/*
	Multiworld API: see doc/multiworld.md and "Multiworld" in doc/lua_api.md.

	Every world of a multiworld server runs in its own Lua state on its own
	thread. These functions are the only way for mods in different worlds to
	interact. On a regular single-world server they are available as well:
	there is exactly one world, no transfers are possible.
*/
class ModApiMultiworld : public ModApiBase
{
private:
	// core.get_world_id() -> number
	static int l_get_world_id(lua_State *L);
	// core.get_world_setting(name) -> string or nil
	static int l_get_world_setting(lua_State *L);
	// core.get_world_name() -> string
	static int l_get_world_name(lua_State *L);
	// core.get_worlds() -> {{id=, name=, is_self=, compatible=, player_count=}, ...}
	static int l_get_worlds(lua_State *L);
	// core.transfer_player_to_world(player|name, world (name or ID), [pos]) -> true | false, errmsg
	static int l_transfer_player_to_world(lua_State *L);
	// core.send_to_world(world|"*", channel, message) -> true | false, errmsg
	static int l_send_to_world(lua_State *L);
	static int l_create_world(lua_State *L);
	static int l_remove_world(lua_State *L);

public:
	static void Initialize(lua_State *L, int top);
};
