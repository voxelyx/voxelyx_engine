// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "cpp_api/s_base.h"
#include <set>

class ScriptApiServer
		: virtual public ScriptApiBase
{
public:
	// Calls on_chat_message handlers
	// Returns true if script handled message
	bool on_chat_message(const std::string &name, const std::string &message);

	// Calls when mods are loaded
	void on_mods_loaded();

	// Calls on_shutdown handlers
	void on_shutdown();

	// Calls core.register_on_worlds_changed handlers (multiworld)
	void on_worlds_changed(const std::string &event, const std::string &world,
			bool ok, const std::string &err);
	// Calls core.register_on_world_message handlers (multiworld)
	void on_world_message(const std::string &from_world,
		const std::string &channel, const std::string &message);

	// Calls core.format_chat_message
	std::string formatChatMessage(const std::string &name,
		const std::string &message);

	/* auth */
	bool getAuth(const std::string &playername,
		std::string *dst_password,
		std::set<std::string> *dst_privs,
		s64 *dst_last_login = nullptr);
	void createAuth(const std::string &playername,
		const std::string &password);
	bool setPassword(const std::string &playername,
		const std::string &password);

	/* dynamic media handling */
	static u32 allocateDynamicMediaCallback(lua_State *L, int f_idx);
	void freeDynamicMediaCallback(u32 token);
	void on_dynamic_media_added(u32 token, const std::string &playername);

private:
	void getAuthHandler();
	void readPrivileges(int index, std::set<std::string> &result);
};
