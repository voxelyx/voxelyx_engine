// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>

/*
	Configuration of a dedicated server.

	A dedicated server runs all of its worlds with the same mods and the same
	accounts. What a world.conf would say about that for a single world is
	therefore kept in the one configuration file of the server, main.conf (in
	the user directory, or wherever --config points). Besides all the other
	settings it knows

	- load_mod_<name> = false                   turns off a mod. Every mod in the
	  mods directories is loaded, there are no games and no mods of
	  individual worlds. (= <virtual path> picks one of several mods of
	  the same name.)
	- auth_backend = sqlite3 | files | ...       where the accounts are kept
	  (<user dir>/auth.sqlite, <user dir>/auth.txt, ...). Default: sqlite3.
	- pgsql_auth_connection                     for the postgresql backend
*/

/// Default name of the configuration file inside the server directory
extern const char *const SERVER_CONF_FILENAME;

/// Default path of the configuration file in the given server directory
std::string getServerConfPath(const std::string &server_dir);

/// Writes a configuration file for the given default world at `path`
/// if there is none yet. Returns
/// false if it doesn't exist and couldn't be created.
bool createServerConfIfMissing(const std::string &path,
		const std::string &default_world);

/// Adds default_world to an existing configuration file that does
/// not have them yet (the other entries and comments stay as they are).
bool addMissingServerConfKeys(const std::string &path,
		const std::string &default_world);

/// Directory that contains the worlds of a dedicated server: the setting
/// multiworld_dir, else <user dir>/worlds
std::string getWorldsDir();
