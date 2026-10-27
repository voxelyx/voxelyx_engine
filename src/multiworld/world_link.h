// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>
#include "irr_v3d.h"
#include "network/networkprotocol.h" // session_t

class RemoteClient;
class AuthDatabase;
class BanManager;
class ContentIdRegistry;
struct ModIPCStore;
class Server;

/*
	Multiworld support: interface between a world's Server instance (which
	runs on its own thread with its own Lua state) and the WorldCluster that
	owns all worlds and the shared network connection.

	All methods are thread-safe, they may be called from any world thread.
	None of them blocks on another world's thread: cross-world effects are
	delivered as tasks that the target world executes on its own thread.
*/

struct WorldInfo {
	std::string name;
	// Unique ID of the world, from its world.conf (1 for the first world)
	u32 id = 0;
	bool is_self = false;
	// Worlds share a "content group" if their node/item definitions and media
	// are identical. Seamless transfers only work inside one content group.
	bool compatible = false;
	u32 player_count = 0;
};

struct WorldTransferRequest {
	std::string player_name;
	std::string target_world;
	bool has_pos = false;
	v3f pos; // in nodes (BS-scaled units are NOT used here)
	// Set by the cluster for worlds with different content: the client has to
	// reconnect, a seamless switch isn't possible.
	bool reconnect_only = false;
};

/// A connected client in flight from one world to another
/// What a player takes along when they move to another world
struct CarriedPlayerState {
	std::string inventory; // serialized
	u16 hp = 0;
	u16 breath = 0;
};

struct ClientHandoff {
	std::unique_ptr<RemoteClient> client;
	std::string player_name;
	std::string from_world;
	bool has_pos = false;
	v3f pos;
	CarriedPlayerState carried;
	bool has_carried = false;
};

class IWorldLink {
public:
	virtual ~IWorldLink() = default;

	/// All loaded worlds, as seen from `from`
	virtual std::vector<WorldInfo> listWorlds(const std::string &from) = 0;

	/**
	 * Asynchronously moves a player from world `from` to another world.
	 * Only validates the request here; the actual move happens on the source
	 * world's thread during its next step.
	 * @return false and sets *err if the request is invalid
	 */
	virtual bool transferPlayer(const std::string &from,
			const WorldTransferRequest &req, std::string *err) = 0;

	/// Hands a client over to its new world. Called on the source world's thread
	/// after the player has left it.
	virtual bool handoffClient(const std::string &to, ClientHandoff &&handoff) = 0;

	/// Whether `peer` is (still) routed to `world`. Used by the receiving world
	/// to discard handoffs of clients that disconnected meanwhile.
	virtual bool isPeerRoutedTo(session_t peer, const std::string &world) = 0;

	/// Makes a player log into `world` on their next connection
	/// (used as fallback for clients that can't do seamless transfers).
	/// Accounts are server-wide, nothing else has to be moved.
	virtual void setLoginWorld(const std::string &player, const std::string &world) = 0;
	/// Hands what a player takes along (inventory, health) to `world`, which gives
	/// it to the player when they enter it (used when they have to reconnect to
	/// get there).
	virtual void carryPlayerState(const std::string &world, const std::string &player,
			CarriedPlayerState state) = 0;

	/// Delivers a message to the Lua callbacks of another world (or all other
	/// worlds if `to` is "*")
	virtual bool sendWorldMessage(const std::string &from, const std::string &to,
			const std::string &channel, const std::string &message,
			std::string *err) = 0;

	virtual void onPlayerJoined(const std::string &world, const std::string &player) = 0;
	virtual void onPlayerLeft(const std::string &world, const std::string &player) = 0;

	/*
		Worlds can be created and removed while the server runs. Both happen in
		the background, the call only checks the request. When done (or failed),
		every world's core.register_on_worlds_changed callbacks are run.
	*/

	/// Creates and starts a new world with the game's settings (mapgen, seed...)
	virtual bool createWorld(const std::string &from, const std::string &name,
			std::string *err) = 0;
	/// Moves the players of the world to the default world, stops it and
	/// takes it out of the world directory. With `delete_files` its files are
	/// deleted, otherwise they are kept in the .removed folder of the worlds
	/// directory. The default world can't be removed.
	virtual bool removeWorld(const std::string &from, const std::string &name,
			bool delete_files, std::string *err) = 0;

	/*
		Server-wide administration. Accounts, privileges, the ban list and the
		list of mods are the same for every world; these functions make the
		players of *other* worlds reachable for commands such as /kick and /ban.
	*/

	/// Names of all players that are online in any world
	virtual std::vector<std::string> getOnlinePlayers() = 0;
	/// Number of players in the worlds other than `from` (approximate)
	virtual u32 getPlayerCountElsewhere(const std::string &from) = 0;
	/// Tells the other worlds that the account `name` ("" = all) changed, so they
	/// can update the privileges the player's client knows about
	virtual void notifyAuthModified(const std::string &from,
			const std::string &name) = 0;
	/// Disconnects a player who is online in another world.
	/// @return false if there is no such player in another world
	virtual bool disconnectPlayer(const std::string &from, const std::string &name,
			const std::string &message, bool reconnect) = 0;
	/// Bans the IP of a player who is online in another world.
	/// @return false if there is no such player in another world
	virtual bool banPlayer(const std::string &from, const std::string &name) = 0;

	/// Cluster-wide state shared by all worlds
	virtual ModIPCStore *getIPCStore() = 0;
	/// Server-wide account database (thread-safe)
	virtual AuthDatabase *getAuthDatabase() = 0;
	/// Server-wide configuration file (mods to load), see doc/multiworld.md
	virtual const std::string &getServerConfPath() const = 0;
	virtual BanManager *getBanManager() = 0;
	virtual ContentIdRegistry *getContentIds() = 0;
};
