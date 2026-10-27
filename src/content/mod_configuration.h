// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013-22 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "mods.h"


/**
 * ModConfiguration is a subset of installed mods. This class
 * is used to resolve dependencies and return a sorted list of mods.
 *
 * This class should not be extended from, but instead used as a
 * component in other classes.
 */
//! Virtual name and path of the directories with mods, in order of preference
using ModPaths = std::vector<std::pair<std::string, std::string>>;

class ModConfiguration
{
public:
	/**
	 * @returns true if all dependencies are fulfilled.
	 */
	inline bool isConsistent() const { return m_unsatisfied_mods.empty(); }

	inline const std::vector<ModSpec> &getUnsatisfiedMods() const
	{
		return m_unsatisfied_mods;
	}

	/**
	 * List of mods sorted such that they can be loaded in the
	 * given order with all dependencies being fulfilled.
	 *
	 * I.e: every mod in this list has only dependencies on mods which
	 * appear earlier in the vector.
	 */
	const std::vector<ModSpec> &getMods() const { return m_sorted_mods; }

	std::string getUnsatisfiedModsError() const;

	/**
	 * Adds all mods in the given path. used for games and modpacks
	 *
	 * @param path To search, should be absolute
	 * @param virtual_path Virtual path for this directory, see comment in ModSpec
	 */
	void addModsInPath(const std::string &path, const std::string &virtual_path);

	/**
	 * Adds all mods in `new_mods`
	 */
	void addMods(const std::vector<ModSpec> &new_mods);

	/**
	 * Adds the mods that `settings_path` enables with `load_mod_<name> = true`
	 * (or the virtual path of the mod). Nothing is loaded unless it is enabled.
	 * This is how client mods are loaded; server mods use addServerMods().
	 */
	void addModsFromConfig(const std::string &settings_path,
			const std::unordered_map<std::string, std::string> &mod_paths);

	/**
	 * Adds the mods of a server: every mod found in `mod_paths` is loaded, except
	 * the ones that `settings_path` (main.conf) turns off with
	 * `load_mod_<name> = false`. If a mod name is found at several places, the
	 * first one is used, or the one that `load_mod_<name> = <virtual path>` names.
	 */
	void addServerMods(const std::string &settings_path, const ModPaths &mod_paths);

	/**
	 * Call this function once all mods have been added
	 */
	void checkConflictsAndDeps();

private:

	std::vector<ModSpec> m_sorted_mods;

	/**
	 * move mods from m_unsatisfied_mods to m_sorted_mods
	 * in an order that satisfies dependencies
	 */
	void resolveDependencies();

	// mods with unmet dependencies. Before dependencies are resolved,
	// this is where all mods are stored. Afterwards this contains
	// only the ones with really unsatisfied dependencies.
	std::vector<ModSpec> m_unsatisfied_mods;

	// set of mod names for which an unresolved name conflict
	// exists. A name conflict happens when two or more mods
	// at the same level have the same name but different paths.
	// Levels (mods in higher levels override mods in lower levels):
	// 1. game mod in modpack; 2. game mod;
	// 3. world mod in modpack; 4. world mod;
	// 5. addon mod in modpack; 6. addon mod.
	std::unordered_set<std::string> m_name_conflicts;
};
