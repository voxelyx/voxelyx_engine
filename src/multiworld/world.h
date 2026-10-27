// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "irrlichttypes.h"
#include "irr_v3d.h"
#include "settings.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>

class Server;
class WorldConnection;

/*
	A world: one map with its own players, mod storage and settings.

	This is what lives in a world directory and is described by its world.conf:

	- world_id             unique number of the world, 1 for the first one
	- world_name           display name
	- backend, player_backend, mod_storage_backend   how its data is stored
	- static_spawnpoint    where players start (x, y, z in nodes). Chosen when
	                       the world is first started.
	- creative_mode, enable_damage    optional, they override the values of
	                       main.conf for this world only

	Everything that is the same for all worlds of a server (the game, the mods,
	the accounts, the announcement in the server list ...) is in main.conf.
	A World is also what the cluster runs: it owns the Server that simulates it
	(its map, Lua environment and players) and its connection to the clients.
	The Server is the engine of a world, it doesn't define what a world is.
*/
class World
{
public:
	/// Does not read anything yet, see load()
	explicit World(const std::string &path);
	~World();

	/// Reads world.conf. Throws BaseException if it is missing, or if the
	/// world has no valid world_id.
	void load();

	/// Name of the world directory
	const std::string &getName() const { return m_name; }
	const std::string &getPath() const { return m_path; }
	std::string getConfPath() const;
	/// Unique ID of the world (valid after load())
	u32 getId() const { return m_id; }

	/// Where players start, in nodes. Not set until the world was started once.
	std::optional<v3f> getSpawn() const;
	/// Remembers the spawn point in world.conf
	bool setSpawn(v3f pos_in_nodes);

	/// Value of a setting for this world: world.conf if it has it there,
	/// else main.conf. Returns false if neither does.
	bool getSetting(const std::string &name, std::string &value) const;
	bool getBool(const std::string &name) const;
	bool isCreative() const { return getBool("creative_mode"); }
	bool isDamageEnabled() const { return getBool("enable_damage"); }

	/*
		Run state, used by the WorldCluster. A world that is simulated by a
		Server of its own has these, a world that is only a description (like the
		one of a singleplayer game, which is run by the game) doesn't.
	*/
	/// The connection through which the clients of this world talk to it
	std::shared_ptr<WorldConnection> con;
	/// Guards `server` against being destroyed while the connection's threads
	/// still call into it
	std::shared_mutex lifetime;
	/// Simulates the world
	std::unique_ptr<Server> server;
	/// Set while the world is being removed: no one can enter it any more
	std::atomic<bool> removing{false};

private:
	std::string m_name;
	std::string m_path;
	u32 m_id = 0;

	mutable std::mutex m_mutex;
	Settings m_conf;
};
