// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "multiworld/world_cluster.h"
#include "multiworld/server_config.h"
#include "serverenvironment.h"
#include "content/worlds.h"
#include "database/database.h"
#include "debug.h"
#include "exceptions.h"
#include "filesys.h"
#include "log.h"
#include "network/networkexceptions.h"
#include "network/networkpacket.h"
#include "network/networkprotocol.h"
#include "porting.h"
#include "profiler.h"
#include "script/common/c_packer.h" // PackedValue, for ModIPCStore
#include "settings.h"
#include "util/string.h"
#include "threading/thread.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <functional>
#include <thread>
#include <sstream>

/*
	Receive thread: the only reader of the real connection
*/
class WorldCluster::ReceiveThread : public Thread
{
public:
	ReceiveThread(WorldCluster *cluster) :
		Thread("ClusterReceive"), m_cluster(cluster) {}

	void *run() override
	{
		BEGIN_DEBUG_EXCEPTION_HANDLER
		while (!stopRequested()) {
			NetworkPacket pkt;
			try {
				if (!m_cluster->m_con->ReceiveTimeoutMs(&pkt, 100))
					continue;
				m_cluster->routePacket(pkt);
			} catch (const con::InvalidIncomingDataException &e) {
				infostream << "WorldCluster: InvalidIncomingDataException: "
					<< e.what() << std::endl;
			} catch (const con::PeerNotFoundException &e) {
				infostream << "WorldCluster: PeerNotFoundException" << std::endl;
			} catch (const SerializationError &e) {
				infostream << "WorldCluster: SerializationError: " << e.what()
					<< std::endl;
			} catch (const PacketError &e) {
				infostream << "WorldCluster: PacketError: " << e.what() << std::endl;
			}
		}
		END_DEBUG_EXCEPTION_HANDLER
		return nullptr;
	}

private:
	WorldCluster *m_cluster;
};

// Runs the jobs for creating and removing worlds, one after the other
class WorldCluster::AdminThread : public Thread
{
public:
	AdminThread() : Thread("ClusterAdmin") {}

	void post(std::function<void()> job)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_jobs.push_back(std::move(job));
		}
		m_cv.notify_one();
	}

	void *run() override
	{
		BEGIN_DEBUG_EXCEPTION_HANDLER
		while (!stopRequested()) {
			std::function<void()> job;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_cv.wait_for(lock, std::chrono::milliseconds(100),
					[this] { return !m_jobs.empty(); });
				if (m_jobs.empty())
					continue;
				job = std::move(m_jobs.front());
				m_jobs.pop_front();
			}
			job();
		}
		END_DEBUG_EXCEPTION_HANDLER
		return nullptr;
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::deque<std::function<void()>> m_jobs;
};

/*
	Runs a function on its own thread (used to shut down worlds in parallel)
*/
namespace {
class TaskThread : public Thread
{
public:
	TaskThread(const std::string &name, std::function<void()> f) :
		Thread(name), m_f(std::move(f)) {}
	void *run() override
	{
		try {
			m_f();
		} catch (const std::exception &e) {
			errorstream << "WorldCluster: exception while shutting down: "
				<< e.what() << std::endl;
		}
		return nullptr;
	}
private:
	std::function<void()> m_f;
};
}

WorldCluster::WorldCluster(const Config &cfg, UDPSocket &&socket) :
	m_cfg(cfg),
	m_bind_addr(socket.GetLocalAddress()),
	m_socket(std::move(socket)),
	m_ban(cfg.ban_path)
{
}

WorldCluster::~WorldCluster()
{
	m_shutting_down = true;

	// No more creating and removing of worlds
	if (m_admin) {
		m_admin->stop();
		m_admin->wait();
	}

	// No new packets for the worlds
	if (m_recv_thread) {
		m_recv_thread->stop();
		m_recv_thread->wait();
	}

	// Shut all worlds down in parallel: saving maps and players of big worlds
	// takes a while. Each world kicks its own players.
	{
		std::vector<std::unique_ptr<TaskThread>> threads;
		for (auto &w : m_worlds) {
			World *world = w.get();
			threads.push_back(std::make_unique<TaskThread>(
				"Shutdown:" + world->getName(), [world] {
					std::unique_lock<std::shared_mutex> lock(world->lifetime);
					world->server.reset();
				}));
			threads.back()->start();
		}
		for (auto &t : threads)
			t->wait();
	}

	if (m_ban.isModified())
		m_ban.save();

	// The connection must outlive the worlds
	m_con.reset();
	m_worlds.clear();
	m_retired.clear();
}

static std::string stripTrailingDelim(std::string path)
{
	while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
		path.pop_back();
	return path;
}

std::vector<WorldCluster::WorldEntry> WorldCluster::discoverWorlds(
		std::string *default_name) const
{
	std::vector<std::string> names;
	if (!m_cfg.worlds.empty()) {
		names = m_cfg.worlds;
	} else {
		for (const auto &node : fs::GetDirListing(m_cfg.worlds_dir)) {
			if (!node.dir)
				continue;
			if (!fs::PathExists(m_cfg.worlds_dir + DIR_DELIM + node.name +
					DIR_DELIM "world.conf"))
				continue;
			names.push_back(node.name);
		}
	}

	std::vector<WorldEntry> entries;
	std::map<u32, std::string> ids; // world ID -> name
	std::sort(names.begin(), names.end());
	for (const std::string &name : names) {
		const std::string path = m_cfg.worlds_dir + DIR_DELIM + name;
		Settings conf;
		if (!conf.readConfigFile((path + DIR_DELIM "world.conf").c_str())) {
			warningstream << "WorldCluster: skipping world \"" << name
				<< "\": it has no world.conf" << std::endl;
			continue;
		}
		u32 id;
		if (!readWorldId(path, &id)) {
			warningstream << "WorldCluster: skipping world \"" << name
				<< "\": world.conf has no valid world_id" << std::endl;
			continue;
		}
		const auto dup = ids.emplace(id, name);
		if (!dup.second)
			throw ServerError("WorldCluster: the worlds \"" + dup.first->second +
				"\" and \"" + name + "\" have the same world_id " +
				std::to_string(id) + ". Every world needs its own ID.");
		entries.push_back({name, path});
	}

	// The default world is part of the server even if it is somewhere else or
	// doesn't exist yet. It is created then, like on a single world server.
	std::string def = m_cfg.default_world;
	std::string def_path = stripTrailingDelim(m_cfg.default_world_path);
	if (!def_path.empty()) {
		const std::string def_dir_name = fs::GetFilenameFromPath(def_path.c_str());
		if (def.empty())
			def = def_dir_name;
		else if (def != def_dir_name)
			throw ServerError("WorldCluster: the default world \"" + def +
				"\" is not the world at " + def_path);
	}

	const auto by_name = [] (const WorldEntry &a, const WorldEntry &b) {
		return a.name < b.name;
	};
	std::sort(entries.begin(), entries.end(), by_name);
	if (def.empty())
		def = entries.empty() ? "main" : entries.front().name;

	auto it = std::find_if(entries.begin(), entries.end(),
		[&] (const WorldEntry &e) { return e.name == def; });
	if (it == entries.end()) {
		entries.push_back({def, def_path.empty() ?
			m_cfg.worlds_dir + DIR_DELIM + def : def_path});
		std::sort(entries.begin(), entries.end(), by_name);
	} else if (!def_path.empty() &&
			fs::AbsolutePath(it->path) != fs::AbsolutePath(def_path)) {
		throw ServerError("WorldCluster: there are two worlds called \"" + def +
			"\": " + it->path + " and " + def_path);
	}

	entries.erase(std::unique(entries.begin(), entries.end(),
		[] (const WorldEntry &a, const WorldEntry &b) { return a.name == b.name; }),
		entries.end());
	*default_name = def;
	return entries;
}

void WorldCluster::openAuthDatabase()
{
	std::string default_name = m_cfg.default_world;
	if (default_name.empty() && !m_cfg.default_world_path.empty())
		default_name = fs::GetFilenameFromPath(
			stripTrailingDelim(m_cfg.default_world_path).c_str());
	if (default_name.empty())
		default_name = "main";
	if (!createServerConfIfMissing(m_server_conf_path, default_name) ||
			!addMissingServerConfKeys(m_server_conf_path, default_name))
		warningstream << "WorldCluster: couldn't create "
			<< m_server_conf_path << std::endl;

	Settings conf;
	conf.readConfigFile(m_server_conf_path.c_str());
	std::string backend = "sqlite3";
	conf.getNoEx("auth_backend", backend);
	if (backend == "files") {
		warningstream << "/!\\ You are using old auth file backend. This backend is "
			"deprecated and will be removed in a future release /!\\" << std::endl
			<< "Set auth_backend = sqlite3 in main.conf." << std::endl;
	}
	m_auth = std::make_unique<SharedAuthDatabase>(std::unique_ptr<AuthDatabase>(
		ServerEnvironment::openAuthDatabase(backend, m_cfg.server_dir, conf)));
}

void WorldCluster::loadState()
{
	std::string content;
	if (!fs::ReadFile(m_cfg.state_path, content))
		return;
	std::istringstream is(content);
	std::string line;
	while (std::getline(is, line)) {
		const size_t sep = line.find('\t');
		if (sep == std::string::npos)
			continue;
		m_login_worlds[line.substr(0, sep)] = trim(line.substr(sep + 1));
	}
}

void WorldCluster::saveStateLocked()
{
	if (m_cfg.state_path.empty())
		return;
	std::ostringstream os;
	for (const auto &it : m_login_worlds)
		os << it.first << '\t' << it.second << '\n';
	if (!fs::safeWriteToFile(m_cfg.state_path, os.str()))
		warningstream << "WorldCluster: couldn't write " << m_cfg.state_path << std::endl;
}

void WorldCluster::start()
{
	m_server_conf_path = m_cfg.server_conf_path.empty() ?
		::getServerConfPath(m_cfg.server_dir) : m_cfg.server_conf_path;

	std::string default_name;
	const std::vector<WorldEntry> entries = discoverWorlds(&default_name);

	loadState();

	// Accounts are the server's, not the worlds'
	openAuthDatabase();

	if (entries.size() > 1 && g_settings->getBool("random_mod_load_order")) {
		warningstream << "WorldCluster: random_mod_load_order is enabled. Each world "
			"then loads its mods in a different order, which usually makes their "
			"definitions differ, so players could only enter other worlds by "
			"reconnecting. Disable it for seamless world changes." << std::endl;
	}

	infostream << "WorldCluster: loading " << entries.size() << " world(s)" << std::endl;

	for (const WorldEntry &entry : entries) {
		const std::string &name = entry.name;
		// The default world is created if it doesn't exist yet
		const bool existed = fs::PathExists(entry.path + DIR_DELIM "world.conf");
		std::unique_ptr<World> world;
		try {
			world = loadWorld(entry, name == default_name ? m_cfg.chat_iface : nullptr);
		} catch (const BaseException &e) {
			if (name == default_name)
				throw;
			errorstream << "WorldCluster: world \"" << name << "\" failed to load, "
				"skipping it: " << e.what() << std::endl;
			continue;
		}
		// realpath() so that the log doesn't show things like "bin/../worlds"
		std::string abs_path = fs::AbsolutePath(entry.path);
		if (abs_path.empty())
			abs_path = entry.path;
		actionstream << (existed ? "Found" : "Created") << " world \"" << name
			<< "\" at [" << abs_path << "]" << std::endl;
		if (name == default_name)
			m_default_world = world.get();
		m_worlds.push_back(std::move(world));
	}
	FATAL_ERROR_IF(!m_default_world, "default world missing after loading");

	// Which worlds can players move between without reconnecting?
	const std::string &ref = m_default_world->server->getContentFingerprint();
	for (auto &w : m_worlds) {
		if (w->server->getContentFingerprint() != ref) {
			warningstream << "WorldCluster: world \"" << w->getName() << "\" has different "
				"mods/definitions than \"" << m_default_world->getName() << "\" (different "
				"mods enabled or random mod load order). "
				"Players can enter it only by reconnecting." << std::endl;
		}
	}

	// Now open the network
	m_con.reset(con::createMTP(true, std::move(m_socket), this));
	for (auto &w : m_worlds)
		w->con->setBackend(m_con.get());
	m_recv_thread = std::make_unique<ReceiveThread>(this);
	m_recv_thread->start();
	m_admin = std::make_unique<AdminThread>();
	m_admin->start();

	actionstream << m_worlds.size() << " world(s) running, default is \""
		<< m_default_world->getName() << "\", listening on ";
	m_bind_addr.print(actionstream);
	actionstream << "." << std::endl;
}

std::unique_ptr<World> WorldCluster::loadWorld(const WorldEntry &entry,
		ChatInterface *iface)
{
	auto world = std::make_unique<World>(entry.path);
	world->con = std::make_shared<WorldConnection>();
	if (m_con)
		world->con->setBackend(m_con.get());
	world->server = std::make_unique<Server>(*world,
		world->con, m_bind_addr, this, nullptr, iface);
	// Initializes the world (runs its mods) and starts its thread
	world->server->start();
	return world;
}

/*
	Creating and removing worlds
*/

static bool isValidWorldName(const std::string &name)
{
	if (name.empty() || name.size() > 64 || name[0] == '.')
		return false;
	// Names made of digits only would be mistaken for world IDs
	if (name.find_first_not_of("0123456789") == std::string::npos)
		return false;
	for (unsigned char c : name) {
		if (!(c < 128 && (std::isalnum(c) || c == '_' || c == '-' || c == '.')))
			return false;
	}
	return true;
}

bool WorldCluster::createWorld(const std::string &from, const std::string &name,
		std::string *err)
{
	if (!isValidWorldName(name)) {
		*err = "invalid world name, use letters, digits, _ - . (at most 64, "
			"not starting with a dot, not only digits)";
		return false;
	}
	if (!m_cfg.worlds.empty()) {
		*err = "the world list is fixed by the multiworld_worlds setting";
		return false;
	}
	if (m_shutting_down || !m_admin) {
		*err = "the server is not ready or shutting down";
		return false;
	}
	{
		std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
		if (findWorld(name)) {
			*err = "there is already a world called \"" + name + "\"";
			return false;
		}
	}
	if (fs::PathExists(m_cfg.worlds_dir + DIR_DELIM + name)) {
		*err = "the worlds directory already has a folder called \"" + name + "\"";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		if (!m_busy.insert(name).second) {
			*err = "world \"" + name + "\" is being created or removed";
			return false;
		}
	}
	actionstream << "WorldCluster: world \"" << from << "\" requests creating world \""
		<< name << "\"" << std::endl;
	m_admin->post([this, name, from] { doCreateWorld(name, from); });
	return true;
}

void WorldCluster::finishAdminJob(const std::string &name)
{
	std::lock_guard<std::mutex> lock(m_state_mutex);
	m_busy.erase(name);
}

void WorldCluster::doCreateWorld(const std::string &name, const std::string &)
{
	const WorldEntry entry{name, m_cfg.worlds_dir + DIR_DELIM + name};
	std::string err;
	try {
		std::unique_ptr<World> world = loadWorld(entry, nullptr);
		if (world->server->getContentFingerprint() !=
				m_default_world->server->getContentFingerprint()) {
			warningstream << "WorldCluster: world \"" << name << "\" has different "
				"definitions than \"" << m_default_world->getName() << "\". Players can "
				"enter it only by reconnecting." << std::endl;
		}
		std::unique_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
		m_worlds.push_back(std::move(world));
	} catch (const BaseException &e) {
		err = e.what();
		if (err.empty())
			err = "unknown error";
	}

	if (!err.empty()) {
		errorstream << "WorldCluster: could not create world \"" << name << "\": "
			<< err << std::endl;
		// Don't leave a half made world behind
		fs::RecursiveDelete(entry.path);
	} else {
		actionstream << "WorldCluster: world \"" << name << "\" created" << std::endl;
	}
	finishAdminJob(name);
	notifyWorldsChanged("created", name, err.empty(), err);
}

bool WorldCluster::removeWorld(const std::string &from, const std::string &name,
		bool delete_files, std::string *err)
{
	if (m_shutting_down || !m_admin) {
		*err = "the server is not ready or shutting down";
		return false;
	}
	{
		std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
		World *w = findWorld(name);
		if (!w) {
			*err = "unknown world \"" + name + "\"";
			return false;
		}
		if (w == m_default_world) {
			*err = "the default world can't be removed";
			return false;
		}
		if (!m_cfg.worlds.empty()) {
			*err = "the world list is fixed by the multiworld_worlds setting";
			return false;
		}
		std::lock_guard<std::mutex> lock(m_state_mutex);
		if (!m_busy.insert(name).second) {
			*err = "world \"" + name + "\" is being created or removed";
			return false;
		}
		// Nobody can enter it from now on
		w->removing = true;
	}
	actionstream << "WorldCluster: world \"" << from << "\" requests removing world \""
		<< name << "\"" << (delete_files ? " and deleting its files" : "") << std::endl;
	m_admin->post([this, name, delete_files, from] {
		doRemoveWorld(name, delete_files, from);
	});
	return true;
}

void WorldCluster::doRemoveWorld(const std::string &name, bool delete_files,
		const std::string &)
{
	using namespace std::chrono;
	std::string err;

	// 1. Move the players out. Those who can't be moved seamlessly are asked to
	//    reconnect, they arrive in the default world. Those who don't leave in
	//    time are kicked.
	const auto start = steady_clock::now();
	bool kicked = false;
	for (;;) {
		std::vector<std::string> players;
		{
			std::lock_guard<std::mutex> lock(m_state_mutex);
			for (const auto &it : m_online)
				if (it.second == name)
					players.push_back(it.first);
		}
		if (players.empty() || m_shutting_down)
			break;

		const auto waited = steady_clock::now() - start;
		if (waited > seconds(30) && !kicked) {
			kicked = true;
			std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
			if (World *w = findWorld(name)) {
				for (const std::string &p : players) {
					w->server->enqueueTask([p] (Server &s) {
						s.disconnectLocalPlayer(p, "This world was removed.", false);
					});
				}
			}
		} else if (waited > seconds(40)) {
			break;
		} else if (!kicked) {
			for (const std::string &p : players) {
				WorldTransferRequest req;
				req.player_name = p;
				req.target_world = m_default_world->getName();
				std::string ignored;
				transferPlayer(name, req, &ignored);
			}
		}
		std::this_thread::sleep_for(milliseconds(500));
	}

	// 2. Take it out of the list
	std::unique_ptr<World> world;
	{
		std::unique_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
		auto it = std::find_if(m_worlds.begin(), m_worlds.end(),
			[&] (const std::unique_ptr<World> &w) { return w->getName() == name; });
		if (it != m_worlds.end()) {
			world = std::move(*it);
			m_worlds.erase(it);
		}
	}
	if (!world) {
		finishAdminJob(name);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_route_mutex);
		for (auto it = m_routes.begin(); it != m_routes.end();) {
			if (it->second == world.get())
				it = m_routes.erase(it);
			else
				++it;
		}
	}
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		bool changed = false;
		for (auto it = m_login_worlds.begin(); it != m_login_worlds.end();) {
			if (it->second == name) {
				it = m_login_worlds.erase(it);
				changed = true;
			} else {
				++it;
			}
		}
		if (changed)
			saveStateLocked();
	}

	// 3. Stop it. This saves the map and takes a while for big worlds. The World
	//    object stays, the routing code may still point at it.
	{
		std::unique_lock<std::shared_mutex> lock(world->lifetime);
		world->server.reset();
	}
	const std::string path = world->getPath();
	m_retired.push_back(std::move(world));

	// 4. Its files
	if (delete_files) {
		if (!fs::RecursiveDelete(path))
			err = "could not delete " + path;
	} else {
		const std::string trash = m_cfg.worlds_dir + DIR_DELIM ".removed";
		const std::string target = trash + DIR_DELIM + name + "-" +
			std::to_string(time(nullptr));
		if (!fs::CreateAllDirs(trash) || !fs::Rename(path, target))
			err = "could not move " + path + " to " + target;
		else
			actionstream << "WorldCluster: files of world \"" << name << "\" moved to "
				<< target << std::endl;
	}

	if (!err.empty())
		errorstream << "WorldCluster: " << err << std::endl;
	actionstream << "WorldCluster: world \"" << name << "\" removed" << std::endl;
	finishAdminJob(name);
	// The world is gone even if its files couldn't be moved
	notifyWorldsChanged("removed", name, true, err);
}

void WorldCluster::notifyWorldsChanged(const std::string &event, const std::string &world,
		bool ok, const std::string &err)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	for (auto &w : m_worlds) {
		w->server->enqueueTask([=] (Server &s) {
			s.dispatchWorldsChanged(event, world, ok, err);
		});
	}
}

void WorldCluster::step()
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	for (auto &w : m_worlds)
		w->server->step();

	if (m_ban.isModified() && m_ban_save_interval.step(0.05f, 10.0f))
		m_ban.save();
}

bool WorldCluster::isShutdownRequested()
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	for (auto &w : m_worlds)
		if (w->server->isShutdownRequested())
			return true;
	return false;
}

void WorldCluster::setStepSettings(const Server::StepSettings &settings)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	for (auto &w : m_worlds)
		w->server->setStepSettings(settings);
}

World *WorldCluster::findWorldById(u32 id) const
{
	for (auto &w : m_worlds) {
		if (w->getId() == id)
			return w.get();
	}
	return nullptr;
}

World *WorldCluster::findWorld(const std::string &name) const
{
	for (auto &w : m_worlds)
		if (w->getName() == name)
			return w.get();
	return nullptr;
}

Server *WorldCluster::getWorldServer(const std::string &name)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	World *w = findWorld(name);
	return w ? w->server.get() : nullptr;
}

std::vector<std::string> WorldCluster::getWorldNames() const
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	std::vector<std::string> names;
	for (auto &w : m_worlds)
		names.push_back(w->getName());
	return names;
}

World *WorldCluster::chooseLoginWorld(const std::string &player)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	std::string wanted;
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		auto it = m_login_worlds.find(player);
		if (it != m_login_worlds.end())
			wanted = it->second;
	}
	if (!wanted.empty())
		if (World *w = findWorld(wanted))
			if (!w->removing)
				return w;
	return m_default_world;
}

/*
	Network routing
*/

void WorldCluster::peerAdded(con::IPeer *peer)
{
	verbosestream << "WorldCluster::peerAdded(): id=" << peer->id << std::endl;
	std::lock_guard<std::mutex> lock(m_route_mutex);
	m_unassigned.insert(peer->id);
}

void WorldCluster::deletingPeer(con::IPeer *peer, bool timeout)
{
	verbosestream << "WorldCluster::deletingPeer(): id=" << peer->id
		<< ", timeout=" << timeout << std::endl;

	World *world = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_route_mutex);
		m_unassigned.erase(peer->id);
		auto it = m_routes.find(peer->id);
		if (it != m_routes.end()) {
			world = it->second;
			m_routes.erase(it);
		}
	}
	if (!world || m_shutting_down)
		return;

	world->con->dropPacketsOf(peer->id);
	// Not under m_route_mutex: DeleteClient takes the world's env lock
	std::shared_lock<std::shared_mutex> lock(world->lifetime);
	if (world->server)
		world->server->deletingPeer(peer, timeout);
}

void WorldCluster::routePacket(NetworkPacket &pkt)
{
	const session_t peer = pkt.getPeerId();
	World *target = nullptr;
	bool first = false;
	{
		std::lock_guard<std::mutex> lock(m_route_mutex);
		auto it = m_routes.find(peer);
		if (it != m_routes.end())
			target = it->second;
		else
			first = m_unassigned.count(peer) > 0;
	}

	if (!target && first) {
		// The first packet of a connection is TOSERVER_INIT, it carries the
		// player's name. That's what decides which world the player starts in.
		if (pkt.getCommand() != TOSERVER_INIT) {
			infostream << "WorldCluster: peer " << peer << " sent command "
				<< pkt.getCommand() << " before INIT, ignoring" << std::endl;
			return;
		}
		std::string name;
		try {
			NetworkPacket copy = pkt;
			u8 max_ser_ver;
			u16 unused, min_proto, max_proto;
			copy >> max_ser_ver >> unused >> min_proto >> max_proto >> name;
		} catch (const PacketError &) {
			name.clear();
		}
		World *world = chooseLoginWorld(name);

		{
			std::lock_guard<std::mutex> lock(m_route_mutex);
			// The peer may have gone away while we decided
			if (m_unassigned.erase(peer) == 0)
				return;
			m_routes[peer] = world;
		}
		{
			std::shared_lock<std::shared_mutex> lock(world->lifetime);
			if (!world->server)
				return;
			world->server->addPeer(peer);
		}
		target = world;
	}

	if (target)
		target->con->push(std::move(pkt));
}

/*
	IWorldLink
*/

std::vector<WorldInfo> WorldCluster::listWorlds(const std::string &from)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	World *self = findWorld(from);
	std::vector<WorldInfo> list;
	for (auto &w : m_worlds) {
		WorldInfo info;
		info.name = w->getName();
		info.id = w->getId();
		info.is_self = (w.get() == self);
		info.compatible = self && w->server->getContentFingerprint() ==
			self->server->getContentFingerprint();
		info.player_count = w->server->getPlayerCountApprox();
		list.push_back(std::move(info));
	}
	return list;
}

bool WorldCluster::transferPlayer(const std::string &from,
		const WorldTransferRequest &req, std::string *err)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	// The player may be in another world than the caller (e.g. an admin moving
	// somebody else), so look where they are
	World *src = findPlayerWorld(req.player_name);
	World *dst = findWorld(req.target_world);
	if (!findWorld(from)) {
		*err = "unknown source world";
		return false;
	}
	if (!src) {
		*err = "player is not online";
		return false;
	}
	if (!dst) {
		*err = "unknown world \"" + req.target_world + "\"";
		return false;
	}
	if (dst->removing) {
		*err = "world \"" + req.target_world + "\" is being removed";
		return false;
	}
	if (src == dst) {
		*err = "player is already in that world";
		return false;
	}
	if (req.player_name.empty()) {
		*err = "invalid player";
		return false;
	}

	WorldTransferRequest r = req;
	// Worlds with different definitions can only be entered by reconnecting
	r.reconnect_only = src->server->getContentFingerprint() !=
		dst->server->getContentFingerprint();

	src->server->enqueueTask([r](Server &s) { s.transferPlayerOut(r); });
	return true;
}

bool WorldCluster::handoffClient(const std::string &to, ClientHandoff &&handoff)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	World *dst = findWorld(to);
	if (!dst || !handoff.client)
		return false;
	const session_t peer = handoff.client->peer_id;

	World *src = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_route_mutex);
		auto it = m_routes.find(peer);
		if (it == m_routes.end())
			return false; // disconnected, nothing to hand over
		src = it->second;
		// From here on the peer's packets go to the new world
		it->second = dst;
	}
	if (src)
		src->con->dropPacketsOf(peer);

	auto h = std::make_shared<ClientHandoff>(std::move(handoff));
	dst->server->enqueueTask([h](Server &s) { s.receiveHandoff(h); });
	return true;
}

bool WorldCluster::isPeerRoutedTo(session_t peer, const std::string &world)
{
	std::lock_guard<std::mutex> lock(m_route_mutex);
	auto it = m_routes.find(peer);
	return it != m_routes.end() && it->second->getName() == world;
}

void WorldCluster::setLoginWorld(const std::string &player, const std::string &world)
{
	std::lock_guard<std::mutex> lock(m_state_mutex);
	if (m_login_worlds[player] != world) {
		m_login_worlds[player] = world;
		saveStateLocked();
	}
}

bool WorldCluster::sendWorldMessage(const std::string &from, const std::string &to,
		const std::string &channel, const std::string &message, std::string *err)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	if (channel.size() > MAX_CHANNEL_SIZE || message.size() > MAX_MESSAGE_SIZE) {
		*err = "channel or message too long";
		return false;
	}

	std::vector<World *> targets;
	if (to == "*") {
		for (auto &w : m_worlds)
			if (w->getName() != from)
				targets.push_back(w.get());
	} else if (World *w = findWorld(to)) {
		targets.push_back(w);
	} else {
		*err = "unknown world \"" + to + "\"";
		return false;
	}

	for (World *w : targets) {
		w->server->enqueueTask([from, channel, message](Server &s) {
			s.dispatchWorldMessage(from, channel, message);
		});
	}
	return true;
}

void WorldCluster::onPlayerJoined(const std::string &world, const std::string &player)
{
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		m_online[player] = world;
	}
	setLoginWorld(player, world);
}

void WorldCluster::onPlayerLeft(const std::string &world, const std::string &player)
{
	// The player's login world stays the one they left from
	std::lock_guard<std::mutex> lock(m_state_mutex);
	auto it = m_online.find(player);
	// They may already be in another world
	if (it != m_online.end() && it->second == world)
		m_online.erase(it);
}

World *WorldCluster::findPlayerWorld(const std::string &player)
{
	std::string world;
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		auto it = m_online.find(player);
		if (it == m_online.end())
			return nullptr;
		world = it->second;
	}
	return findWorld(world);
}

void WorldCluster::carryPlayerState(const std::string &world, const std::string &player,
		CarriedPlayerState state)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	World *w = findWorld(world);
	if (!w)
		return;
	w->server->enqueueTask([player, st = std::move(state)] (Server &s) {
		s.stashCarriedState(player, st);
	});
}

std::vector<std::string> WorldCluster::getOnlinePlayers()
{
	std::lock_guard<std::mutex> lock(m_state_mutex);
	std::vector<std::string> names;
	for (const auto &it : m_online)
		names.push_back(it.first);
	return names;
}

u32 WorldCluster::getPlayerCountElsewhere(const std::string &from)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	u32 count = 0;
	for (auto &w : m_worlds)
		if (w->getName() != from)
			count += w->server->getPlayerCountApprox();
	return count;
}

void WorldCluster::notifyAuthModified(const std::string &from, const std::string &name)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	for (auto &w : m_worlds) {
		if (w->getName() == from)
			continue;
		w->server->enqueueTask([name] (Server &s) {
			s.reportPrivsModifiedLocal(name);
		});
	}
}

bool WorldCluster::disconnectPlayer(const std::string &from, const std::string &name,
		const std::string &message, bool reconnect)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	World *w = findPlayerWorld(name);
	if (!w || w->getName() == from)
		return false;
	w->server->enqueueTask([name, message, reconnect] (Server &s) {
		s.disconnectLocalPlayer(name, message, reconnect);
	});
	return true;
}

bool WorldCluster::banPlayer(const std::string &from, const std::string &name)
{
	std::shared_lock<std::shared_mutex> worlds_lock(m_worlds_mutex);
	World *w = findPlayerWorld(name);
	if (!w || w->getName() == from)
		return false;
	w->server->enqueueTask([name] (Server &s) { s.banLocalPlayer(name); });
	return true;
}

/*
	Main loop
*/

void cluster_server_loop(WorldCluster &cluster, volatile std::sig_atomic_t &kill)
{
	verbosestream << "cluster_server_loop()" << std::endl;

	IntervalLimiter profiler_interval;
	constexpr float steplen = 0.05f;
	const float profiler_print_interval = g_settings->getFloat("profiler_print_interval");

	cluster.setStepSettings(Server::StepSettings{
		g_settings->getFloat("dedicated_server_step"), false});

	for (;;) {
		sleep_ms((int)(steplen * 1000.0f));
		cluster.step();

		if (cluster.isShutdownRequested() || kill)
			break;

		if (profiler_print_interval > 0) {
			if (profiler_interval.step(steplen, profiler_print_interval)) {
				infostream << "Profiler:" << std::endl;
				g_profiler->print(infostream);
				g_profiler->clear();
			}
		}
	}

	infostream << "Cluster server quitting" << std::endl;
}
