// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <cmath>
#include "lua_api/l_multiworld.h"
#include "lua_api/l_internal.h"
#include "common/c_converter.h"
#include "multiworld/world_link.h"
#include "server.h"

static bool read_player_name(lua_State *L, int idx, std::string &name)
{
	if (lua_isstring(L, idx)) {
		name = lua_tostring(L, idx);
		return !name.empty();
	}
	// An ObjectRef: ask it for its name
	luaL_checktype(L, idx, LUA_TUSERDATA);
	lua_getfield(L, idx, "get_player_name");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return false;
	}
	lua_pushvalue(L, idx);
	lua_call(L, 1, 1);
	if (!lua_isstring(L, -1)) {
		lua_pop(L, 1);
		return false;
	}
	name = lua_tostring(L, -1);
	lua_pop(L, 1);
	return !name.empty();
}

// A world can be given by name or by ID (a number, or a string of digits, since
// names are never only digits). Returns false if no world has that ID.
static bool read_world_ref(lua_State *L, int idx, Server *server, std::string &name)
{
	u32 id = 0;
	if (lua_type(L, idx) == LUA_TNUMBER) {
		const lua_Number n = lua_tonumber(L, idx);
		if (!(n >= 1 && n < 4294967296.0) || n != std::floor(n))
			return false;
		id = (u32) n;
	} else {
		name = luaL_checkstring(L, idx);
		if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
			return true;
		if (name.size() > 10)
			return false;
		id = (u32) std::stoull(name);
	}
	if (id == server->getWorld()->getId()) {
		name = server->getWorld()->getName();
		return true;
	}
	IWorldLink *link = server->getWorldLink();
	if (!link)
		return false;
	for (const WorldInfo &w : link->listWorlds(server->getWorld()->getName())) {
		if (w.id == id) {
			name = w.name;
			return true;
		}
	}
	return false;
}

int ModApiMultiworld::l_get_world_id(lua_State *L)
{
	lua_pushinteger(L, getServer(L)->getWorld()->getId());
	return 1;
}

int ModApiMultiworld::l_get_world_setting(lua_State *L)
{
	// A setting of main.conf, unless world.conf of this world overrides it
	std::string value;
	if (getServer(L)->getWorld()->getSetting(luaL_checkstring(L, 1), value))
		lua_pushstring(L, value.c_str());
	else
		lua_pushnil(L);
	return 1;
}

int ModApiMultiworld::l_get_world_name(lua_State *L)
{
	lua_pushstring(L, getServer(L)->getWorld()->getName().c_str());
	return 1;
}

int ModApiMultiworld::l_get_worlds(lua_State *L)
{
	Server *server = getServer(L);
	IWorldLink *link = server->getWorldLink();

	std::vector<WorldInfo> worlds;
	if (link) {
		worlds = link->listWorlds(server->getWorld()->getName());
	} else {
		// Plain single-world server
		WorldInfo self;
		self.name = server->getWorld()->getName();
		self.id = server->getWorld()->getId();
		self.is_self = true;
		self.compatible = true;
		self.player_count = server->getPlayerCountApprox();
		worlds.push_back(self);
	}

	lua_createtable(L, worlds.size(), 0);
	int i = 1;
	for (const WorldInfo &w : worlds) {
		lua_createtable(L, 0, 5);
		lua_pushinteger(L, w.id);
		lua_setfield(L, -2, "id");
		lua_pushstring(L, w.name.c_str());
		lua_setfield(L, -2, "name");
		lua_pushboolean(L, w.is_self);
		lua_setfield(L, -2, "is_self");
		lua_pushboolean(L, w.compatible);
		lua_setfield(L, -2, "compatible");
		lua_pushinteger(L, w.player_count);
		lua_setfield(L, -2, "player_count");
		lua_rawseti(L, -2, i++);
	}
	return 1;
}

int ModApiMultiworld::l_transfer_player_to_world(lua_State *L)
{
	Server *server = getServer(L);

	WorldTransferRequest req;
	if (!read_player_name(L, 1, req.player_name)) {
		lua_pushboolean(L, false);
		lua_pushstring(L, "invalid player");
		return 2;
	}
	if (!read_world_ref(L, 2, server, req.target_world)) {
		lua_pushboolean(L, false);
		lua_pushstring(L, "there is no world with that ID");
		return 2;
	}
	if (!lua_isnoneornil(L, 3)) {
		req.has_pos = true;
		req.pos = check_v3f(L, 3);
	}

	IWorldLink *link = server->getWorldLink();
	std::string err;
	if (!link) {
		err = "this server has only one world";
	} else if (link->transferPlayer(server->getWorld()->getName(), req, &err)) {
		lua_pushboolean(L, true);
		return 1;
	}
	lua_pushboolean(L, false);
	lua_pushstring(L, err.c_str());
	return 2;
}

int ModApiMultiworld::l_send_to_world(lua_State *L)
{
	Server *server = getServer(L);

	std::string to;
	if (lua_isstring(L, 1) && lua_tostring(L, 1) == std::string("*")) {
		to = "*";
	} else if (!read_world_ref(L, 1, server, to)) {
		lua_pushboolean(L, false);
		lua_pushstring(L, "there is no world with that ID");
		return 2;
	}
	const std::string channel = readParam<std::string>(L, 2);
	const std::string message = readParam<std::string>(L, 3);

	IWorldLink *link = server->getWorldLink();
	std::string err;
	if (!link) {
		err = "this server has only one world";
	} else if (link->sendWorldMessage(server->getWorld()->getName(), to, channel,
			message, &err)) {
		lua_pushboolean(L, true);
		return 1;
	}
	lua_pushboolean(L, false);
	lua_pushstring(L, err.c_str());
	return 2;
}

int ModApiMultiworld::l_create_world(lua_State *L)
{
	Server *server = getServer(L);
	const std::string name = readParam<std::string>(L, 1);

	IWorldLink *link = server->getWorldLink();
	std::string err;
	if (!link) {
		err = "this server has only one world";
	} else if (link->createWorld(server->getWorld()->getName(), name, &err)) {
		lua_pushboolean(L, true);
		return 1;
	}
	lua_pushboolean(L, false);
	lua_pushstring(L, err.c_str());
	return 2;
}

int ModApiMultiworld::l_remove_world(lua_State *L)
{
	Server *server = getServer(L);
	std::string name;
	if (!read_world_ref(L, 1, server, name)) {
		lua_pushboolean(L, false);
		lua_pushstring(L, "there is no world with that ID");
		return 2;
	}
	const bool delete_files = readParam<bool>(L, 2, false);

	IWorldLink *link = server->getWorldLink();
	std::string err;
	if (!link) {
		err = "this server has only one world";
	} else if (link->removeWorld(server->getWorld()->getName(), name, delete_files, &err)) {
		lua_pushboolean(L, true);
		return 1;
	}
	lua_pushboolean(L, false);
	lua_pushstring(L, err.c_str());
	return 2;
}

void ModApiMultiworld::Initialize(lua_State *L, int top)
{
	API_FCT(get_world_id);
	API_FCT(get_world_setting);
	API_FCT(get_world_name);
	API_FCT(get_worlds);
	API_FCT(transfer_player_to_world);
	API_FCT(send_to_world);
	API_FCT(create_world);
	API_FCT(remove_world);
}
