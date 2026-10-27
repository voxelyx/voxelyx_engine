// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include <string>
#include <set>
#include <vector>
#include "irrlichttypes.h"


// Get the list of paths to mods in the environment variable VOXELYX_MOD_PATH
std::vector<std::string> getEnvModPaths();

bool getWorldExists(const std::string &world_path);
//! Try to get the displayed name of a world
std::string getWorldName(const std::string &world_path, const std::string &default_name);
//! Name of the file in a world directory that describes the world
constexpr const char *WORLD_CONF_FILENAME = "world.conf";
//! Folder of the worlds directory that holds the worlds removed from it
constexpr const char *REMOVED_WORLDS_DIRNAME = ".removed";
//! The first world of a server (the one created first) has this ID
constexpr u32 DEFAULT_WORLD_ID = 1;

/**
 * Reads the ID of a world from its world.conf. Every world has a unique,
 * positive ID, assigned when the world is created.
 * @return false if the world has no valid ID
 */
bool readWorldId(const std::string &world_path, u32 *id);
/**
 * ID for a world that is about to be created at `world_path`: one more than
 * the highest ID of any world in the worlds directories (also those that were
 * removed), so 1 for the first world.
 */
u32 allocateWorldId(const std::string &world_path);

struct WorldSpec
{
	std::string path;
	std::string name;

	WorldSpec(const std::string &path = "", const std::string &name = "") :
			path(path),
			name(name)
	{
	}

	bool isValid() const
	{
		return (!name.empty() && !path.empty());
	}
};

std::vector<WorldSpec> getAvailableWorlds();

// Creates the world directory, world.conf and map_meta.txt if they don't exist.
// If create_world is set, the path is made unique first.
void initWorld(const std::string &path, const std::string &name, bool create_world);
