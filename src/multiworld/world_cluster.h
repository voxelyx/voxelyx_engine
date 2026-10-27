// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <atomic>
#include <csignal>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "multiworld/world.h"
#include "network/peerhandler.h"
#include "network/socket.h"
#include "server.h" // ModIPCStore, Server
#include "server/ban.h"
#include "threading/thread.h"
#include "util/numeric.h" // IntervalLimiter
#include "multiworld/content_ids.h"
#include "multiworld/world_link.h"
#include "multiworld/world_connection.h"
#include "multiworld/shared_auth.h"

struct ChatInterface;
class Settings;

/*
	WorldCluster: runs all worlds of a dedicated server in the same process,
	concurrently. This is how every dedicated server runs, a server with a
	single world is a cluster of one.

	Architecture
	------------
	- Each world is a complete `Server` instance: own thread, own Lua state,
	  own environment, map, emerge threads, databases and definition managers.
	  Worlds are therefore stepped truly in parallel and never share mutable
	  game state.
	- The cluster owns the one UDP socket / MTP connection and a receive
	  thread that routes every incoming packet to the world its sender is
	  currently in. Outgoing packets bypass the cluster (the connection is
	  thread-safe).
	- Cross-world operations (teleporting a player, sending messages, kicking
	  a player who is in another world) are tasks that are posted to the
	  target world and executed on its own thread, so no world ever blocks on
	  another one.
	- What belongs to the server and not to a world: the network connection,
	  the accounts (passwords and privileges, one SharedAuthDatabase), the ban
	  list, the list of mods (main.conf, there are no per-world mods), the
	  core.ipc_* store and the global settings.
	- All worlds belong to the same game and run the same mods. Seamless
	  transfers further require identical node/item definitions and media
	  ("content group"), which is the normal case; otherwise the client is
	  asked to reconnect, into the new world.
*/
class WorldCluster final : public con::PeerHandler, public IWorldLink
{
public:
	struct Config {
		// Directory for the files that belong to the server as a whole:
		// main.conf and the accounts
		std::string server_dir;
		// The server's main.conf. Empty = <server_dir>/main.conf
		std::string server_conf_path;
		// Directory that contains the worlds (one sub directory each)
		std::string worlds_dir;
		// Which worlds to load. Empty = all valid worlds in worlds_dir
		std::vector<std::string> worlds;
		// Name of the world that new players start in. Empty = the world
		// at default_world_path, else the first one alphabetically.
		// If it doesn't exist yet it is created in worlds_dir.
		std::string default_world;
		// Path of the default world, if it isn't a world of worlds_dir. May not
		// exist yet, then it is created (like a single world server does).
		std::string default_world_path;
		// Terminal chat interface (--terminal), attached to the default world
		ChatInterface *chat_iface = nullptr;
		// Remembers the last world of every player
		std::string state_path;
		// Ban list shared by all worlds
		std::string ban_path;
	};

	WorldCluster(const Config &cfg, UDPSocket &&socket);
	~WorldCluster();
	DISABLE_CLASS_COPY(WorldCluster)

	/// Initializes all worlds (this runs their mods), then starts their threads
	/// and finally the network. Throws ServerError / ModError.
	void start();
	/// Throws if a world hit a fatal error. Call regularly from one thread.
	void step();
	bool isShutdownRequested();
	void setStepSettings(const Server::StepSettings &settings);

	size_t worldCount() const
	{
		std::shared_lock<std::shared_mutex> lock(m_worlds_mutex);
		return m_worlds.size();
	}
	/// Direct access, mainly for tests. Valid between start() and destruction.
	Server *getWorldServer(const std::string &name);
	std::vector<std::string> getWorldNames() const;
	const Address &getBindAddress() const { return m_bind_addr; }

	// IWorldLink
	std::vector<WorldInfo> listWorlds(const std::string &from) override;
	bool transferPlayer(const std::string &from, const WorldTransferRequest &req,
			std::string *err) override;
	bool handoffClient(const std::string &to, ClientHandoff &&handoff) override;
	bool isPeerRoutedTo(session_t peer, const std::string &world) override;
	bool createWorld(const std::string &from, const std::string &name,
			std::string *err) override;
	bool removeWorld(const std::string &from, const std::string &name,
			bool delete_files, std::string *err) override;
	void setLoginWorld(const std::string &player, const std::string &world) override;
	void carryPlayerState(const std::string &world, const std::string &player,
			CarriedPlayerState state) override;
	bool sendWorldMessage(const std::string &from, const std::string &to,
			const std::string &channel, const std::string &message,
			std::string *err) override;
	void onPlayerJoined(const std::string &world, const std::string &player) override;
	void onPlayerLeft(const std::string &world, const std::string &player) override;
	std::vector<std::string> getOnlinePlayers() override;
	u32 getPlayerCountElsewhere(const std::string &from) override;
	void notifyAuthModified(const std::string &from, const std::string &name) override;
	bool disconnectPlayer(const std::string &from, const std::string &name,
			const std::string &message, bool reconnect) override;
	bool banPlayer(const std::string &from, const std::string &name) override;
	AuthDatabase *getAuthDatabase() override { return m_auth.get(); }
	const std::string &getServerConfPath() const override { return m_server_conf_path; }
	ModIPCStore *getIPCStore() override { return &m_ipc; }
	BanManager *getBanManager() override { return &m_ban; }
	ContentIdRegistry *getContentIds() override { return &m_content_ids; }

	// con::PeerHandler (called from the connection's threads)
	void peerAdded(con::IPeer *peer) override;
	void deletingPeer(con::IPeer *peer, bool timeout) override;

	/// Longest message/channel accepted by sendWorldMessage()
	static constexpr size_t MAX_MESSAGE_SIZE = 1 << 20;
	static constexpr size_t MAX_CHANNEL_SIZE = 256;

private:

	class ReceiveThread;
	friend class ReceiveThread;
	class AdminThread;

	struct WorldEntry {
		std::string name;
		std::string path;
	};
	// All worlds to run, and the name of the default one
	std::vector<WorldEntry> discoverWorlds(std::string *default_name) const;
	// Where the world of a given player is, "" if they are not online
	World *findPlayerWorld(const std::string &player);
	void openAuthDatabase();
	// Loads (or creates) and starts a world. Throws if that doesn't work.
	std::unique_ptr<World> loadWorld(const WorldEntry &entry, ChatInterface *iface);
	void doCreateWorld(const std::string &name, const std::string &from);
	void doRemoveWorld(const std::string &name, bool delete_files,
			const std::string &from);
	// Runs core.register_on_worlds_changed callbacks in all worlds
	void notifyWorldsChanged(const std::string &event, const std::string &world,
			bool ok, const std::string &err);
	void finishAdminJob(const std::string &name);
	World *findWorld(const std::string &name) const;
	World *findWorldById(u32 id) const;
	World *chooseLoginWorld(const std::string &player);
	void routePacket(NetworkPacket &pkt);
	void loadState();
	void saveStateLocked();

	Config m_cfg;
	Address m_bind_addr;
	UDPSocket m_socket;

	// Worlds come and go while the server runs. The list is only changed with the
	// mutex held for writing, and every use of a World * outside of the routing
	// code is under it held for reading.
	mutable std::shared_mutex m_worlds_mutex;
	std::vector<std::unique_ptr<World>> m_worlds;
	// Removed worlds. Their servers are gone, but objects that the routing
	// code may still point to stay until the cluster ends.
	std::vector<std::unique_ptr<World>> m_retired;
	World *m_default_world = nullptr;
	// Creates and removes worlds, one at a time, away from all world threads
	std::unique_ptr<AdminThread> m_admin;
	// Worlds that are being created or removed
	std::set<std::string> m_busy;

	std::string m_server_conf_path;
	std::unique_ptr<SharedAuthDatabase> m_auth;

	ModIPCStore m_ipc;
	ContentIdRegistry m_content_ids;
	BanManager m_ban;
	IntervalLimiter m_ban_save_interval;

	std::unique_ptr<con::IConnection> m_con;
	std::unique_ptr<ReceiveThread> m_recv_thread;
	std::atomic<bool> m_shutting_down{false};

	// Routing of peers. Never held while calling into a Server.
	std::mutex m_route_mutex;
	std::unordered_map<session_t, World *> m_routes;
	std::unordered_set<session_t> m_unassigned;

	std::mutex m_state_mutex;
	std::map<std::string, std::string> m_login_worlds;
	// player name -> name of the world they are in (only players who are online)
	std::map<std::string, std::string> m_online;
};

/// Counterpart of dedicated_server_loop() for clusters
void cluster_server_loop(WorldCluster &cluster, volatile std::sig_atomic_t &kill);
