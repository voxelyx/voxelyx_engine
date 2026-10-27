// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "multiworld/server_config.h"
#include "filesys.h"
#include "porting.h"
#include "settings.h"

const char *const SERVER_CONF_FILENAME = "main.conf";

std::string getServerConfPath(const std::string &server_dir)
{
	return server_dir + DIR_DELIM + SERVER_CONF_FILENAME;
}

bool createServerConfIfMissing(const std::string &path,
		const std::string &default_world)
{
	if (fs::PathExists(path))
		return true;

	return fs::CreateAllDirs(fs::RemoveLastPathComponent(path)) &&
		fs::safeWriteToFile(path, std::string(
		"# Configuration of this server, the same for all of its worlds.\n"
		"# See doc/multiworld.md.\n"
		"#\n"
		"# The world in which new players start\n"
		"default_world = " + default_world + "\n"
		"#\n"
		"# All mods in the mods directories are loaded, by all worlds. To turn one\n"
		"# off:\n"
		"#load_mod_mymod = false\n"
		"#\n"
		"# Where the accounts of all players are stored: sqlite3, files, ...\n"
		"auth_backend = sqlite3\n"
		"#\n"
		"# creative_mode and enable_damage can be set here for all worlds, and in\n"
		"# the world.conf of a world to override them for that world.\n"));
}

std::string getWorldsDir()
{
	std::string dir = g_settings->get("multiworld_dir");
	if (dir.empty())
		dir = porting::path_user + DIR_DELIM + "worlds";
	return dir;
}

bool addMissingServerConfKeys(const std::string &path,
		const std::string &default_world)
{
	Settings conf;
	if (!conf.readConfigFile(path.c_str()))
		return false;
	bool changed = false;
	if (!conf.exists("default_world") && !default_world.empty()) {
		conf.set("default_world", default_world);
		changed = true;
	}
	if (!changed)
		return true;
	infostream << "Adding default_world to " << path << std::endl;
	// Keeps the other entries and the comments of the file
	return conf.updateConfigFile(path.c_str());
}
