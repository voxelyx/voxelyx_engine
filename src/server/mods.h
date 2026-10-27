// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2018 nerzhul, Loic Blot <loic.blot@unix-experience.fr>

#pragma once

#include "content/mod_configuration.h"

class ServerScripting;

/**
 * Manages server mods
 *
 * All new calls to this class must be tested in test_servermodmanager.cpp
 */
class ServerModManager
{
	ModConfiguration configuration;

public:

	/**
	 * The directories in which a server looks for mods: <user dir>/mods,
	 * <share dir>/mods and the ones of VOXELYX_MOD_PATH.
	 */
	static ModPaths getModPaths();

	/**
	 * Creates the ServerModManager of a server. All worlds use the same mods:
	 * every mod in the mod paths, except the ones that are turned off with
	 * `load_mod_<name> = false` in main.conf.
	 * @param server_conf_path path to main.conf
	 */
	ServerModManager(const std::string &server_conf_path,
			const ModPaths &mod_paths = getModPaths());

	/**
	 * Creates an empty ServerModManager. For testing purposes.
	 * Note: definition looks like this so it isn't accidentally used.
	*/
	explicit ServerModManager(std::nullptr_t) {};

	void loadMods(ServerScripting &script);
	const ModSpec *getModSpec(const std::string &modname) const;
	void getModNames(std::vector<std::string> &modlist) const;

	inline const std::vector<ModSpec> &getMods() const {
		return configuration.getMods();
	}

	inline const std::vector<ModSpec> &getUnsatisfiedMods() const {
		return configuration.getUnsatisfiedMods();
	}

	inline bool isConsistent() const {
		return configuration.isConsistent();
	}

	inline std::string getUnsatisfiedModsError() const {
		return configuration.getUnsatisfiedModsError();
	}

	/**
	 * Recursively gets all paths of mod folders that can contain media files.
	 *
	 * Result is ordered in descending priority, ie. files from an earlier path
	 * should not be replaced by files from a latter one.
	 *
	 * @param paths result vector
	 */
	void getModsMediaPaths(std::vector<std::string> &paths) const;
};
