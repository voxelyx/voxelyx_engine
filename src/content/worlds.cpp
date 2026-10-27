// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#include <common/c_internal.h>
#include "content/worlds.h"
#include "constants.h"
#include "porting.h"
#include "filesys.h"
#include "settings.h"
#include "log.h"
#include "util/strfnd.h"
#include "map_settings_manager.h"
#include "util/string.h"
#include "exceptions.h"
#include "multiworld/server_config.h"

// The maximum number of identical world names allowed
#define MAX_WORLD_NAMES 100

namespace
{

std::string getWorldPathEnv()
{
	if (const char *path = getenv("VOXELYX_WORLD_PATH"))
		return std::string(path);
	return "";
}

}

bool getWorldExists(const std::string &world_path)
{
	if (world_path.empty())
		return false;
	return fs::IsFile(world_path + DIR_DELIM + WORLD_CONF_FILENAME);
}

//! Try to get the displayed name of a world
std::string getWorldName(const std::string &world_path, const std::string &default_name)
{
	std::string conf_path = world_path + DIR_DELIM + "world.conf";
	Settings conf;
	bool succeeded = conf.readConfigFile(conf_path.c_str());
	if (!succeeded) {
		return default_name;
	}

	if (!conf.exists("world_name"))
		return default_name;
	return conf.get("world_name");
}

bool readWorldId(const std::string &world_path, u32 *id)
{
	Settings conf;
	if (!conf.readConfigFile((world_path + DIR_DELIM + WORLD_CONF_FILENAME).c_str()))
		return false;
	u32 value = 0;
	// 0 is not an ID, and neither is anything that is not a number
	if (!conf.getU32NoEx("world_id", value) || value == 0)
		return false;
	*id = value;
	return true;
}

namespace {

// IDs of all worlds in `dir`, including the ones that were removed from it
void collectWorldIds(const std::string &dir, std::set<u32> &ids)
{
	for (const fs::DirListNode &node : fs::GetDirListing(dir)) {
		if (!node.dir)
			continue;
		const std::string path = dir + DIR_DELIM + node.name;
		if (node.name == REMOVED_WORLDS_DIRNAME) {
			collectWorldIds(path, ids);
			continue;
		}
		u32 id;
		if (readWorldId(path, &id))
			ids.insert(id);
	}
}

}

u32 allocateWorldId(const std::string &world_path)
{
	// Worlds that were removed keep their ID, so that it is never given out
	// again for another world.
	std::set<u32> used;
	collectWorldIds(fs::RemoveLastPathComponent(world_path), used);
	collectWorldIds(getWorldsDir(), used);
	return used.empty() ? DEFAULT_WORLD_ID : *used.rbegin() + 1;
}

std::vector<WorldSpec> getAvailableWorlds()
{
	std::vector<WorldSpec> worlds;
	std::set<std::string> worldspaths;

	Strfnd search_paths(getWorldPathEnv());

	while (!search_paths.at_end())
		worldspaths.insert(search_paths.next(PATH_DELIM));

	worldspaths.insert(porting::path_user + DIR_DELIM + "worlds");
	infostream << "Searching worlds..." << std::endl;
	for (const std::string &worldspath : worldspaths) {
		infostream << "  In " << worldspath << ": ";
		std::vector<fs::DirListNode> dirvector = fs::GetDirListing(worldspath);
		for (const fs::DirListNode &dln : dirvector) {
			if (!dln.dir)
				continue;
			std::string fullpath = worldspath + DIR_DELIM + dln.name;
			std::string name = getWorldName(fullpath, dln.name);
			WorldSpec spec(fullpath, name);
			if (!spec.isValid()) {
				infostream << "(invalid: " << name << ") ";
			} else {
				infostream << name << " ";
				worlds.push_back(spec);
			}
		}
		infostream << std::endl;
	}
	infostream << worlds.size() << " found." << std::endl;
	return worlds;
}

void initWorld(const std::string &path, const std::string &name, bool create_world)
{
	std::string final_path = path;

	// If we're creating a new world, ensure that the path isn't already taken
	if (create_world) {
		int counter = 1;
		while (fs::PathExists(final_path) && counter < MAX_WORLD_NAMES) {
			final_path = path + "_" + std::to_string(counter);
			counter++;
		}

		if (fs::PathExists(final_path)) {
			throw BaseException("Too many similar filenames");
		}
	}

	infostream << "Initializing world at " << final_path << std::endl;

	fs::CreateAllDirs(final_path);

	// Create world.conf if does not already exist
	std::string world_conf_path = final_path + DIR_DELIM "world.conf";
	if (!fs::PathExists(world_conf_path)) {
		Settings conf; // for world.conf

		conf.set("world_name", name);
		conf.set("world_id", std::to_string(allocateWorldId(final_path)));

		conf.set("backend", "sqlite3");

		conf.set("player_backend", "sqlite3");
		conf.set("mod_storage_backend", "sqlite3");
		if (MAP_BLOCKSIZE != 16)
			conf.set("blocksize", std::to_string(MAP_BLOCKSIZE));

		if (!conf.updateConfigFile(world_conf_path.c_str())) {
			throw BaseException("Failed to update world.conf");
		}
	}

	// Create map_meta.txt if does not already exist
	std::string map_meta_path = final_path + DIR_DELIM + "map_meta.txt";
	if (!fs::PathExists(map_meta_path)) {
		MapSettingsManager mgr(map_meta_path);

		mgr.setMapSetting("seed", g_settings->get("fixed_map_seed"));

		mgr.makeMapgenParams();
		mgr.saveMapMeta();
	}
}

std::vector<std::string> getEnvModPaths()
{
	std::vector<std::string> paths;
	const char *c_mod_path = getenv("VOXELYX_MOD_PATH");

	if (c_mod_path) {
		Strfnd search_paths(c_mod_path);
		while (!search_paths.at_end())
			paths.push_back(search_paths.next(PATH_DELIM));
	}
	return paths;
}
