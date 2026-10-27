// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
	Integration tests for multiworld servers (WorldCluster).

	These start a real cluster of two worlds, connect to it
	over UDP with a minimal protocol-level client, and check that
	- worlds run concurrently (own thread and Lua state each),
	- a player can join, be moved to another world and back,
	- the world change is seamless (TOCLIENT_WORLD_CHANGE),
	- worlds are isolated from each other,
	- accounts, mods and the ban list belong to the server and not to a world,
	- a server without any world creates the default one.
*/

#include "test.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include "filesys.h"
#include "log.h"
#include "network/connection.h"
#include "network/mtp/internal.h"
#include "serialization.h"
#include "network/networkexceptions.h"
#include "network/networkpacket.h"
#include "network/networkprotocol.h"
#include "network/peerhandler.h"
#include "network/socket.h"
#include "multiworld/world_cluster.h"
#include "multiworld/shared_auth.h"
#include "multiworld/server_config.h"
#include "database/database.h"
#include "database/database-dummy.h"
#include "serverenvironment.h"
#include "inventory.h"
#include "itemdef.h"
#include "player.h"
#include "porting.h"
#include "remoteplayer.h"
#include "script/scripting_server.h"
#include "server.h"
#include "server/player_sao.h"
#include "serverenvironment.h"
#include "settings.h"
#include "util/auth.h"
#include "util/numeric.h"

class TestMultiworld : public TestBase
{
public:
	TestMultiworld() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestMultiworld"; }

	void runTests(IGameDef *gamedef);

	void testWorldConnection();
	void testSharedAuthDatabase();
	void testDefaultWorldIsCreated();
	void testCluster();
	void testInventoryFollowsPlayer();
	void testCreateAndRemoveWorlds();
	void testWorldIds();
};

static TestMultiworld g_test_instance;

void TestMultiworld::runTests(IGameDef *gamedef)
{
	TEST(testWorldConnection);
	TEST(testSharedAuthDatabase);
	TEST(testDefaultWorldIsCreated);
	TEST(testCluster);
	TEST(testInventoryFollowsPlayer);
	TEST(testCreateAndRemoveWorlds);
	TEST(testWorldIds);
}

////////////////////////////////////////////////////////////////////////////////

void TestMultiworld::testWorldConnection()
{
	WorldConnection con;
	NetworkPacket out;

	// Nothing there, must time out
	UASSERT(!con.ReceiveTimeoutMs(&out, 10));

	NetworkPacket a(TOSERVER_PLAYERPOS, 0, 7);
	NetworkPacket b(TOSERVER_CHAT_MESSAGE, 0, 8);
	NetworkPacket c(TOSERVER_PLAYERPOS, 0, 7);
	con.push(std::move(a));
	con.push(std::move(b));
	con.push(std::move(c));
	UASSERTEQ(size_t, con.inboxSize(), 3);

	// Packets of one peer can be dropped (peer left the world)
	con.dropPacketsOf(7);
	UASSERTEQ(size_t, con.inboxSize(), 1);
	UASSERT(con.ReceiveTimeoutMs(&out, 10));
	UASSERTEQ(session_t, out.getPeerId(), 8);
	UASSERT(!con.ReceiveTimeoutMs(&out, 0));

	// A receiver blocked in ReceiveTimeoutMs() is woken up by push()
	std::thread t([&] {
		sleep_ms(30);
		con.push(NetworkPacket(TOSERVER_PLAYERPOS, 0, 9));
	});
	const u64 t0 = porting::getTimeMs();
	UASSERT(con.ReceiveTimeoutMs(&out, 5000));
	UASSERT(porting::getTimeMs() - t0 < 2000);
	UASSERTEQ(session_t, out.getPeerId(), 9);
	t.join();

	// Without a backend, sending is an error rather than a crash
	bool threw = false;
	try {
		NetworkPacket p(TOCLIENT_HP, 0, 1);
		con.Send(1, 0, &p, true);
	} catch (const con::ConnectionException &) {
		threw = true;
	}
	UASSERT(threw);
}

////////////////////////////////////////////////////////////////////////////////

void TestMultiworld::testSharedAuthDatabase()
{
	// The worlds of a server use one account database from different threads
	SharedAuthDatabase db(std::make_unique<AuthDatabaseDummy>());

	constexpr int per_thread = 200;
	std::atomic<int> failures{0};
	auto worker = [&] (const std::string &prefix) {
		for (int i = 0; i < per_thread; i++) {
			AuthEntry e;
			e.id = 0;
			e.name = prefix + std::to_string(i);
			e.password = "pw";
			e.privileges = {"interact", "shout"};
			e.last_login = 1;
			AuthEntry got;
			if (!db.createAuth(e) || !db.getAuth(e.name, got) ||
					got.privileges.size() != 2)
				failures++;
			got.privileges.push_back("fly");
			if (!db.saveAuth(got))
				failures++;
			std::vector<std::string> names;
			db.listNames(names);
		}
	};
	std::thread a(worker, "a"), b(worker, "b");
	a.join();
	b.join();
	UASSERTEQ(int, failures.load(), 0);

	std::vector<std::string> names;
	db.listNames(names);
	UASSERTEQ(size_t, names.size(), 2 * per_thread);

	AuthEntry e;
	UASSERT(db.getAuth("a7", e));
	UASSERTEQ(size_t, e.privileges.size(), 3);
	UASSERT(db.deleteAuth("a7"));
	UASSERT(!db.getAuth("a7", e));
}

////////////////////////////////////////////////////////////////////////////////

namespace {

// Gives the cluster tests their own settings state
struct SettingsGuard {
	bool old_empty_pw = g_settings->getBool("disallow_empty_password");
	bool old_random_order = g_settings->getBool("random_mod_load_order");
	std::string old_worlds_dir = g_settings->get("multiworld_dir");
	// The mods of the test servers: just enough for a map generator to work
	std::string mods_dir = fs::TempPath() + DIR_DELIM + "voxelyx_mw_mods_" +
		std::to_string(porting::getTimeMs());
	SettingsGuard()
	{
		const std::string mod = mods_dir + DIR_DELIM + "mwbase";
		fs::CreateAllDirs(mod);
		fs::safeWriteToFile(mod + DIR_DELIM + "mod.conf", "name = mwbase\n");
		fs::safeWriteToFile(mod + DIR_DELIM + "init.lua",
			"core.register_node('mwbase:stone', {description = 'Stone'})\n"
			"local water = {drawtype = 'liquid', walkable = false, pointable = false,\n"
			"	buildable_to = true, liquid_viscosity = 1, liquid_range = 8,\n"
			"	liquid_alternative_flowing = 'mwbase:water_flowing',\n"
			"	liquid_alternative_source = 'mwbase:water'}\n"
			"water.description = 'Water'; water.liquidtype = 'source'\n"
			"core.register_node('mwbase:water', water)\n"
			"local flowing = table.copy(water)\n"
			"flowing.liquidtype = 'flowing'\n"
			"core.register_node('mwbase:water_flowing', flowing)\n"
			"core.register_alias('mapgen_stone', 'mwbase:stone')\n"
			"core.register_alias('mapgen_water_source', 'mwbase:water')\n"
			"core.register_alias('mapgen_river_water_source', 'mwbase:water')\n");
		// All mods in the mod paths are loaded by every server
#ifdef _WIN32
		_putenv_s("VOXELYX_MOD_PATH", mods_dir.c_str());
#else
		setenv("VOXELYX_MOD_PATH", mods_dir.c_str(), 1);
#endif
		g_settings->setBool("disallow_empty_password", false);
		// Worlds are only interchangeable if their mods load in the same order
		g_settings->setBool("random_mod_load_order", false);
	}
	// World IDs are given out per worlds directory
	void worldsDir(const std::string &dir) { g_settings->set("multiworld_dir", dir); }
	~SettingsGuard()
	{
		g_settings->setBool("disallow_empty_password", old_empty_pw);
		g_settings->setBool("random_mod_load_order", old_random_order);
		g_settings->set("multiworld_dir", old_worlds_dir);
#ifdef _WIN32
		_putenv_s("VOXELYX_MOD_PATH", "");
#else
		unsetenv("VOXELYX_MOD_PATH");
#endif
		fs::RecursiveDelete(mods_dir);
	}
};

}

void TestMultiworld::testDefaultWorldIsCreated()
{
	SettingsGuard guard;

	const std::string root = fs::TempPath() + DIR_DELIM + "voxelyx_multiworld_new_" +
		std::to_string(porting::getTimeMs());
	fs::RecursiveDelete(root);
	UASSERT(fs::CreateAllDirs(root));

	WorldCluster::Config cfg;
	cfg.server_dir = root;
	cfg.worlds_dir = root + DIR_DELIM + "worlds";
	guard.worldsDir(cfg.worlds_dir);
	cfg.state_path = root + DIR_DELIM + "players.txt";
	cfg.ban_path = root + DIR_DELIM + "ipban.txt";
	// No worlds exist and none is asked for: like a single world server does,
	// the cluster creates the first world.
	{
		const u16 port = 43000 + (porting::getTimeMs() % 2000);
		WorldCluster cluster(cfg, UDPSocket::Create(Address(127, 0, 0, 1, port)));
		cluster.start();
		UASSERTEQ(size_t, cluster.worldCount(), 1);
		UASSERTEQ(std::string, cluster.getWorldNames()[0], "main");
		UASSERT(fs::PathExists(cfg.worlds_dir + DIR_DELIM "main" DIR_DELIM "world.conf"));
		// The first world has the ID 1
		u32 id = 0;
		UASSERT(readWorldId(cfg.worlds_dir + DIR_DELIM "main", &id));
		UASSERTEQ(u32, id, 1);
		// The server-wide files exist, the world has no accounts of its own
		UASSERT(fs::PathExists(getServerConfPath(root)));
		{
			// The server's settings, not the world's: game and default world
			Settings server_conf;
			UASSERT(server_conf.readConfigFile(getServerConfPath(root).c_str()));
			UASSERT(!server_conf.exists("gameid"));
			UASSERTEQ(std::string, server_conf.get("default_world"), "main");
			Settings world_conf;
			UASSERT(world_conf.readConfigFile((cfg.worlds_dir + DIR_DELIM "main"
				DIR_DELIM "world.conf").c_str()));
			UASSERT(!world_conf.exists("gameid"));
			UASSERT(!world_conf.exists("auth_backend"));
		}
		// A new world chooses its spawn point and keeps it in its world.conf
		{
			World *world = cluster.getWorldServer("main")->getWorld();
			const u64 end = porting::getTimeMs() + 60000;
			while (!world->getSpawn().has_value() && porting::getTimeMs() < end)
				sleep_ms(20);
			UASSERT(world->getSpawn().has_value());
			Settings world_conf;
			UASSERT(world_conf.readConfigFile((cfg.worlds_dir + DIR_DELIM "main"
				DIR_DELIM "world.conf").c_str()));
			UASSERT(world_conf.exists("static_spawnpoint"));
		}
		{
			std::vector<std::string> names;
			cluster.getAuthDatabase()->listNames(names); // opens the file
		}
		UASSERT(fs::PathExists(root + DIR_DELIM "auth.sqlite"));
		UASSERT(!fs::PathExists(cfg.worlds_dir + DIR_DELIM "main" DIR_DELIM "auth.sqlite"));
	}

	// A named default world that doesn't exist is created next to the others
	cfg.default_world = "second";
	{
		const u16 port = 43000 + ((porting::getTimeMs() + 7) % 2000);
		WorldCluster cluster(cfg, UDPSocket::Create(Address(127, 0, 0, 1, port)));
		cluster.start();
		UASSERTEQ(size_t, cluster.worldCount(), 2);
		UASSERT(fs::PathExists(cfg.worlds_dir + DIR_DELIM "second" DIR_DELIM "world.conf"));
		u32 id = 0;
		UASSERT(readWorldId(cfg.worlds_dir + DIR_DELIM "second", &id));
		UASSERTEQ(u32, id, 2);
	}

	// The default world may be anywhere, as with --world
	cfg.default_world.clear();
	cfg.default_world_path = root + DIR_DELIM + "elsewhere" + DIR_DELIM + "mymap";
	{
		const u16 port = 43000 + ((porting::getTimeMs() + 13) % 2000);
		WorldCluster cluster(cfg, UDPSocket::Create(Address(127, 0, 0, 1, port)));
		cluster.start();
		UASSERTEQ(size_t, cluster.worldCount(), 3);
		UASSERT(fs::PathExists(cfg.default_world_path + DIR_DELIM "world.conf"));
		// Also a world somewhere else gets an ID that no other world has
		u32 id = 0;
		UASSERT(readWorldId(cfg.default_world_path, &id));
		UASSERTEQ(u32, id, 3);
	}
	fs::RecursiveDelete(root);
}

////////////////////////////////////////////////////////////////////////////////

namespace {

struct NullHandler : public con::PeerHandler {
	void peerAdded(con::IPeer *) override {}
	void deletingPeer(con::IPeer *, bool) override {}
};

/// Just enough of a client to join a server, and to follow world changes
class TestClient
{
public:
	TestClient(const Address &server_addr, const std::string &name) :
		m_name(name),
		m_con(512, 5.0f, UDPSocket::CreateEphemeral(false), false, &m_handler)
	{
		m_con.Connect(server_addr);
	}

	void send(NetworkPacket &pkt, bool reliable = true)
	{
		m_con.Send(PEER_ID_SERVER, 1, &pkt, reliable);
	}

	/// Waits for a packet of the given type. Others are skipped.
	bool expect(u16 command, NetworkPacket *out, u32 timeout_ms = 15000)
	{
		const u64 end = porting::getTimeMs() + timeout_ms;
		while (porting::getTimeMs() < end) {
			NetworkPacket pkt;
			try {
				if (!m_con.ReceiveTimeoutMs(&pkt, 100))
					continue;
			} catch (const con::InvalidIncomingDataException &) {
				continue;
			}
			if (pkt.getCommand() == command) {
				*out = pkt;
				return true;
			}
		}
		return false;
	}

	/// Whether the server offered to create a new account (it does so for names
	/// it doesn't know)
	bool offeredRegistration() const { return m_offered_registration; }

	bool join(u8 caps = 1)
	{
		NetworkPacket init(TOSERVER_INIT, 0);
		init << (u8) SER_FMT_VER_HIGHEST_READ << (u16) 0
			<< (u16) CLIENT_PROTOCOL_VERSION_MIN << (u16) LATEST_PROTOCOL_VERSION
			<< m_name;
		send(init, false);

		NetworkPacket hello;
		if (!expect(TOCLIENT_HELLO, &hello))
			return false;
		u8 ser; u16 comp, proto; u32 mechs;
		hello >> ser >> comp >> proto >> mechs;
		m_offered_registration = (mechs & AUTH_MECHANISM_FIRST_SRP) != 0;
		if (!m_offered_registration)
			return false; // we only know how to register a new account

		std::string verifier, salt;
		generate_srp_verifier_and_salt(m_name, "secret", &verifier, &salt);
		NetworkPacket first_srp(TOSERVER_FIRST_SRP, 0);
		first_srp << salt << verifier << (u8) 0;
		send(first_srp);

		NetworkPacket accept;
		if (!expect(TOCLIENT_AUTH_ACCEPT, &accept))
			return false;

		NetworkPacket init2(TOSERVER_INIT2, 0);
		init2 << std::string("en");
		send(init2);

		// Definitions are sent right after INIT2. We already have everything
		// we need (this client has no media to fetch), so continue.
		NetworkPacket nodedef;
		if (!expect(TOCLIENT_NODEDEF, &nodedef))
			return false;
		return sendReady(caps);
	}

	/// Like the real client does after joining or after a world change.
	/// `caps` = capability flags (1 = understands TOCLIENT_WORLD_CHANGE)
	bool sendReady(u8 caps = 1)
	{
		const std::string hash = "test";
		NetworkPacket ready(TOSERVER_CLIENT_READY, 0);
		ready << (u8) 5 << (u8) 18 << (u8) 0 << (u8) 0 << (u16) hash.size();
		ready.putRawString(hash.data(), hash.size());
		ready << (u16) FORMSPEC_API_VERSION << caps;
		send(ready);
		return true;
	}

	/// Waits until the server tells us where we are
	bool expectMovePlayer(v3f *pos)
	{
		NetworkPacket pkt;
		if (!expect(TOCLIENT_MOVE_PLAYER, &pkt))
			return false;
		pkt >> *pos;
		return true;
	}

	/// Mods may move a new player around before the requested destination
	/// is applied, so wait for the packet that places us where we want to be
	bool expectMovePlayerNear(const v3f &target, v3f *last_pos)
	{
		for (int i = 0; i < 20; i++) {
			if (!expectMovePlayer(last_pos))
				return false;
			if ((*last_pos - target).getLength() < 0.01f)
				return true;
		}
		return false;
	}

	const std::string &name() const { return m_name; }

private:
	std::string m_name;
	bool m_offered_registration = false;
	NullHandler m_handler;
	con::Connection m_con;
};

struct PlayerInfo {
	bool present = false;
	bool has_peer = false;
	v3f pos;
};

/// Asks a world about a player, on that world's own thread
PlayerInfo queryPlayer(Server *server, const std::string &name)
{
	auto promise = std::make_shared<std::promise<PlayerInfo>>();
	auto future = promise->get_future();
	server->enqueueTask([promise, name] (Server &s) {
		PlayerInfo info;
		RemotePlayer *p = s.getEnv().getPlayer(name.c_str());
		if (p && p->getPlayerSAO()) {
			info.present = true;
			info.has_peer = p->getPeerId() != PEER_ID_INEXISTENT;
			info.pos = p->getPlayerSAO()->getBasePosition();
		}
		promise->set_value(info);
	});
	if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
		UASSERT(!"world did not answer");
	return future.get();
}

/// Polls until `pred` holds
template<typename F>
bool waitFor(F pred, u32 timeout_ms = 10000)
{
	const u64 end = porting::getTimeMs() + timeout_ms;
	while (porting::getTimeMs() < end) {
		if (pred())
			return true;
		sleep_ms(20);
	}
	return pred();
}

/// Runs `fn` on the world's own thread and waits for it
template<typename F>
void runOnWorld(Server *server, F fn)
{
	auto promise = std::make_shared<std::promise<void>>();
	auto future = promise->get_future();
	server->enqueueTask([promise, fn] (Server &s) {
		fn(s);
		promise->set_value();
	});
	if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
		UASSERT(!"world did not answer");
}

/// Serialized inventory of a player of the given world ("" if not there)
std::string playerInventory(Server *server, const std::string &name)
{
	std::string result;
	runOnWorld(server, [&] (Server &s) {
		RemotePlayer *p = s.getEnv().getPlayer(name.c_str());
		if (!p)
			return;
		std::ostringstream os(std::ios::binary);
		p->inventory.serialize(os);
		result = os.str();
	});
	return result;
}

void setHealth(Server *server, const std::string &name, u16 hp, u16 breath)
{
	runOnWorld(server, [&] (Server &s) {
		RemotePlayer *p = s.getEnv().getPlayer(name.c_str());
		UASSERT(p && p->getPlayerSAO());
		p->getPlayerSAO()->setHPRaw(hp);
		p->getPlayerSAO()->setBreath(breath, false);
	});
}

/// {hp, breath} of a player of the given world
std::pair<u16, u16> getHealth(Server *server, const std::string &name)
{
	std::pair<u16, u16> result{0, 0};
	runOnWorld(server, [&] (Server &s) {
		RemotePlayer *p = s.getEnv().getPlayer(name.c_str());
		if (p && p->getPlayerSAO())
			result = {p->getPlayerSAO()->getHP(), p->getPlayerSAO()->getBreath()};
	});
	return result;
}

void giveItems(Server *server, const std::string &name, const std::string &item, u16 count)
{
	runOnWorld(server, [&] (Server &s) {
		RemotePlayer *p = s.getEnv().getPlayer(name.c_str());
		UASSERT(p);
		InventoryList *list = p->inventory.getList("main");
		if (!list)
			list = p->inventory.addList("main", 32);
		list->addItem(ItemStack(item, count, 0, s.getItemDefManager()));
		p->setModified(true);
	});
}

// (the game is not a property of a world, but of the server: gameid in main.conf)
void makeWorld(const std::string &dir, const std::string &name, u32 id)
{
	fs::CreateAllDirs(dir);
	std::string conf = "backend = sqlite3\nplayer_backend = sqlite3\n"
		"mod_storage_backend = sqlite3\nworld_name = " +
		name + "\nworld_id = " + std::to_string(id) + "\n";
	UASSERT(fs::safeWriteToFile(dir + DIR_DELIM + "world.conf", conf));
}

}

void TestMultiworld::testCluster()
{

	const std::string root = fs::TempPath() + DIR_DELIM + "voxelyx_multiworld_test_" +
		std::to_string(porting::getTimeMs());
	fs::RecursiveDelete(root);
	makeWorld(root + DIR_DELIM + "worlds" + DIR_DELIM + "alpha", "alpha", 1);
	makeWorld(root + DIR_DELIM + "worlds" + DIR_DELIM + "beta", "beta", 2);

	// A world mod. There are no per-world mods, so it must not run.
	{
		const std::string mod = root + DIR_DELIM "worlds" DIR_DELIM "beta" DIR_DELIM
			"worldmods" DIR_DELIM "extra";
		UASSERT(fs::CreateAllDirs(mod));
		UASSERT(fs::safeWriteToFile(mod + DIR_DELIM "mod.conf", "name = extra\n"));
		UASSERT(fs::safeWriteToFile(mod + DIR_DELIM "init.lua",
			"core.safe_file_write(core.get_worldpath() .. '/worldmod_ran.txt', 'x')\n"));
		// Neither does enabling mods in world.conf
		const std::string wmt = root + DIR_DELIM "worlds" DIR_DELIM "beta" DIR_DELIM "world.conf";
		std::string text;
		UASSERT(fs::ReadFile(wmt, text));
		UASSERT(fs::safeWriteToFile(wmt, text + "load_mod_nonexistent = true\n"));
	}

	SettingsGuard guard;

	WorldCluster::Config cfg;
	cfg.server_dir = root;
	cfg.worlds_dir = root + DIR_DELIM + "worlds";
	guard.worldsDir(cfg.worlds_dir);
	cfg.default_world = "alpha";
	cfg.state_path = root + DIR_DELIM + "players.txt";
	cfg.ban_path = root + DIR_DELIM + "ipban.txt";

	const u16 port = 41000 + (porting::getTimeMs() % 2000);
	Address bind(127, 0, 0, 1, port);

	{
		WorldCluster cluster(cfg, UDPSocket::Create(bind));
		cluster.start();

		UASSERTEQ(size_t, cluster.worldCount(), 2);
		Server *alpha = cluster.getWorldServer("alpha");
		Server *beta = cluster.getWorldServer("beta");
		UASSERT(alpha && beta && alpha != beta);
		UASSERTEQ(std::string, alpha->getWorld()->getName(), "alpha");

		// Same game and mods => same content, seamless transfers possible
		UASSERTEQ(std::string, alpha->getContentFingerprint(),
			beta->getContentFingerprint());
		for (const WorldInfo &w : cluster.listWorlds("alpha"))
			UASSERT(w.compatible);

		/*
			Accounts belong to the server: both worlds use the same database, which
			lives in the server directory and not in the worlds.
		*/
		AuthDatabase *auth = alpha->getEnv().getAuthDatabase();
		UASSERT(auth != nullptr);
		UASSERT(auth == beta->getEnv().getAuthDatabase());
		{
			std::vector<std::string> names;
			auth->listNames(names); // opens the file
		}
		UASSERT(fs::PathExists(root + DIR_DELIM "auth.sqlite"));

		/*
			There are no mods of single worlds: worldmods are ignored, everything
			comes from the game and the server-wide configuration.
		*/
		UASSERT(!fs::PathExists(root + DIR_DELIM "worlds" DIR_DELIM "beta" DIR_DELIM
			"worldmod_ran.txt"));
		UASSERT(fs::PathExists(getServerConfPath(root)));

		/*
			Parallelism: run a busy Lua script in each world at the same time. Each
			world has its own thread and its own Lua state, so the busy periods must
			overlap in wall-clock time. If worlds shared a thread or a Lua lock,
			they would run one after the other.
		*/
		{
			const std::string script = root + DIR_DELIM + "busy.lua";
			UASSERT(fs::safeWriteToFile(script,
				"local t0 = core.get_us_time()\n"
				"while core.get_us_time() - t0 < 600000 do end\n"
				"core.safe_file_write(core.get_worldpath() .. '/busy.txt',\n"
				"	t0 .. ' ' .. core.get_us_time())\n"));

			auto done = std::make_shared<std::atomic<int>>(0);
			auto threads = std::make_shared<std::vector<std::thread::id>>(2);
			auto mtx = std::make_shared<std::mutex>();
			int idx = 0;
			for (Server *s : {alpha, beta}) {
				const int my = idx++;
				s->enqueueTask([=] (Server &srv) {
					{
						std::lock_guard<std::mutex> l(*mtx);
						(*threads)[my] = std::this_thread::get_id();
					}
					srv.getScriptIface()->loadScript(script);
					(*done)++;
				});
			}
			UASSERT(waitFor([&] { return done->load() == 2; }, 20000));
			UASSERT((*threads)[0] != (*threads)[1]);
			UASSERT((*threads)[0] != std::this_thread::get_id());

			s64 start[2], end[2];
			int i = 0;
			for (const char *w : {"alpha", "beta"}) {
				std::string text;
				UASSERT(fs::ReadFile(cfg.worlds_dir + DIR_DELIM + w + DIR_DELIM +
					"busy.txt", text));
				UASSERTEQ(int, sscanf(text.c_str(), "%lld %lld",
					(long long *) &start[i], (long long *) &end[i]), 2);
				i++;
			}
			// Both 600 ms busy loops overlapped for most of their duration
			const s64 overlap = std::min(end[0], end[1]) - std::max(start[0], start[1]);
			infostream << "multiworld: busy loops overlapped for " << overlap / 1000
				<< " ms" << std::endl;
			UASSERT(overlap > 300000);
		}

		/*
			Messages between worlds arrive in the Lua callbacks of the receiving
			world, on its own thread.
		*/
		{
			const std::string script = root + DIR_DELIM + "listen.lua";
			UASSERT(fs::safeWriteToFile(script,
				"core.register_on_world_message(function(from, channel, msg)\n"
				"	core.safe_file_write(core.get_worldpath() .. '/msg_' .. channel .. '.txt',\n"
				"		core.get_world_name() .. '<-' .. from .. ':' .. msg)\n"
				"end)\n"));
			auto loaded = std::make_shared<std::atomic<int>>(0);
			for (Server *s : {alpha, beta}) {
				s->enqueueTask([=] (Server &srv) {
					srv.getScriptIface()->loadScript(script);
					(*loaded)++;
				});
			}
			UASSERT(waitFor([&] { return loaded->load() == 2; }));

			std::string err;
			UASSERT(cluster.sendWorldMessage("alpha", "beta", "direct", "hello", &err));
			UASSERT(cluster.sendWorldMessage("alpha", "*", "all", "everyone", &err));
			UASSERT(!cluster.sendWorldMessage("alpha", "nowhere", "x", "y", &err));

			const std::string alpha_dir = cfg.worlds_dir + DIR_DELIM + "alpha";
			const std::string beta_dir = cfg.worlds_dir + DIR_DELIM + "beta";
			std::string text;
			UASSERT(waitFor([&] {
				return fs::ReadFile(beta_dir + DIR_DELIM + "msg_direct.txt", text);
			}));
			UASSERTEQ(std::string, text, "beta<-alpha:hello");
			UASSERT(waitFor([&] {
				return fs::ReadFile(beta_dir + DIR_DELIM + "msg_all.txt", text);
			}));
			UASSERTEQ(std::string, text, "beta<-alpha:everyone");
			// A broadcast doesn't come back to the sender
			sleep_ms(300);
			UASSERT(!fs::PathExists(alpha_dir + DIR_DELIM + "msg_all.txt"));
			UASSERT(!fs::PathExists(alpha_dir + DIR_DELIM + "msg_direct.txt"));
		}

		/*
			A player joins. New players start in the default world.
		*/
		TestClient client(Address(127, 0, 0, 1, port), "mwtester");
		UASSERT(client.join());
		v3f pos;
		UASSERT(client.expectMovePlayer(&pos));
		UASSERT(waitFor([&] { return queryPlayer(alpha, "mwtester").has_peer; }));
		UASSERT(!queryPlayer(beta, "mwtester").present);

		/*
			Move to beta at a given position. The client is told to reset its
			world, answers with CLIENT_READY and is then placed in beta.
		*/
		WorldTransferRequest req;
		req.player_name = "mwtester";
		req.target_world = "beta";
		req.has_pos = true;
		req.pos = v3f(10, 20, 30);
		std::string err;
		UASSERT(cluster.transferPlayer("alpha", req, &err));

		NetworkPacket change;
		UASSERT(client.expect(TOCLIENT_WORLD_CHANGE, &change));
		std::string world;
		u32 world_id;
		u8 flags;
		change >> world_id >> world >> flags;
		UASSERTEQ(u32, world_id, 2);
		UASSERTEQ(std::string, world, "beta");

		// Not in either world until the client confirms
		UASSERT(!queryPlayer(beta, "mwtester").has_peer);
		UASSERT(waitFor([&] { return !queryPlayer(alpha, "mwtester").has_peer; }));

		client.sendReady();
		// The requested destination wins over spawn logic of the new world
		UASSERT(client.expectMovePlayerNear(v3f(10, 20, 30) * BS, &pos));

		// The client may still show the old world's sky and HUD settings, so the
		// new world's state is sent explicitly
		NetworkPacket sky, hudflags;
		UASSERT(client.expect(TOCLIENT_SET_SKY, &sky));
		UASSERT(client.expect(TOCLIENT_HUD_SET_FLAGS, &hudflags));

		PlayerInfo in_beta = queryPlayer(beta, "mwtester");
		UASSERT(in_beta.present && in_beta.has_peer);
		UASSERT(fabs(in_beta.pos.Y - 20 * BS) < 0.01f);

		// Worlds are isolated: alpha has no active session for the player
		UASSERT(!queryPlayer(alpha, "mwtester").has_peer);

		// The world they are in is remembered for the next login
		std::string state;
		UASSERT(fs::ReadFile(cfg.state_path, state));
		UASSERT(state.find("mwtester\tbeta") != std::string::npos);

		/*
			And straight back, immediately. alpha may still be cleaning up the
			old session of this player, the handoff has to wait for that.
		*/
		WorldTransferRequest back;
		back.player_name = "mwtester";
		back.target_world = "alpha";
		UASSERT(cluster.transferPlayer("beta", back, &err));
		UASSERT(client.expect(TOCLIENT_WORLD_CHANGE, &change));
		change >> world_id >> world;
		UASSERTEQ(u32, world_id, 1);
		client.sendReady();
		UASSERT(client.expectMovePlayer(&pos));
		UASSERT(waitFor([&] { return queryPlayer(alpha, "mwtester").has_peer; }));
		UASSERT(waitFor([&] { return !queryPlayer(beta, "mwtester").has_peer; }));

		/*
			A client that can't switch worlds on an open connection is asked to
			reconnect. Accounts are server-wide, so nobody can register its name
			in the other world.
		*/
		{
			TestClient old_client(Address(127, 0, 0, 1, port), "oldclient");
			UASSERT(old_client.join(/*caps=*/0));
			v3f p;
			UASSERT(old_client.expectMovePlayer(&p));
			UASSERT(waitFor([&] { return queryPlayer(alpha, "oldclient").has_peer; }));

			giveItems(alpha, "oldclient", "testnodes:normal", 4);
			WorldTransferRequest r;
			r.player_name = "oldclient";
			r.target_world = "beta";
			UASSERT(cluster.transferPlayer("alpha", r, &err));

			NetworkPacket denied;
			UASSERT(old_client.expect(TOCLIENT_ACCESS_DENIED, &denied));
			UASSERT(waitFor([&] { return !queryPlayer(alpha, "oldclient").has_peer; }));
			// The inventory is waiting for the player in the world they're sent to
			{
				bool carried = false;
				UASSERT(waitFor([&] {
					runOnWorld(beta, [&] (Server &s) {
						carried = s.hasCarriedState("oldclient");
					});
					return carried;
				}));
			}

			std::string st;
			UASSERT(waitFor([&] {
				return fs::ReadFile(cfg.state_path, st) &&
					st.find("oldclient\tbeta") != std::string::npos;
			}));

			// Coming back, the server routes the player to beta. The account is the
			// same there, registering it again is not offered.
			TestClient again(Address(127, 0, 0, 1, port), "oldclient");
			UASSERT(!again.join());
			UASSERT(!again.offeredRegistration());
		}

		/*
			Commands work on players of other worlds, e.g. /kick and /ban.
		*/
		{
			TestClient victim(Address(127, 0, 0, 1, port), "kickme");
			UASSERT(victim.join());
			v3f p;
			UASSERT(victim.expectMovePlayer(&p));
			UASSERT(waitFor([&] { return queryPlayer(alpha, "kickme").has_peer; }));

			WorldTransferRequest r;
			r.player_name = "kickme";
			r.target_world = "beta";
			UASSERT(cluster.transferPlayer("alpha", r, &err));
			NetworkPacket wc;
			UASSERT(victim.expect(TOCLIENT_WORLD_CHANGE, &wc));
			victim.sendReady();
			UASSERT(waitFor([&] { return queryPlayer(beta, "kickme").has_peer; }));

			// Everybody who is online, in whichever world, is known
			const std::vector<std::string> online = cluster.getOnlinePlayers();
			UASSERT(std::find(online.begin(), online.end(), "kickme") != online.end());
			UASSERT(std::find(online.begin(), online.end(), "mwtester") != online.end());

			// Not "elsewhere" for the world the player is in
			UASSERT(!cluster.disconnectPlayer("beta", "kickme", "no", false));
			UASSERT(!cluster.disconnectPlayer("alpha", "nobody", "no", false));
			UASSERT(cluster.disconnectPlayer("alpha", "kickme", "Out you go", false));
			NetworkPacket denied;
			UASSERT(victim.expect(TOCLIENT_ACCESS_DENIED, &denied));
			UASSERT(waitFor([&] { return !queryPlayer(beta, "kickme").present; }));
			UASSERT(waitFor([&] {
				const auto names = cluster.getOnlinePlayers();
				return std::find(names.begin(), names.end(), "kickme") == names.end();
			}));
		}

		// Invalid requests are refused up front
		WorldTransferRequest bad = back;
		bad.target_world = "nowhere";
		UASSERT(!cluster.transferPlayer("alpha", bad, &err));
		bad.target_world = "alpha";
		UASSERT(!cluster.transferPlayer("alpha", bad, &err)); // already there
	}

	fs::RecursiveDelete(root);
}

void TestMultiworld::testInventoryFollowsPlayer()
{
	SettingsGuard guard;

	const std::string root = fs::TempPath() + DIR_DELIM + "voxelyx_multiworld_inv_" +
		std::to_string(porting::getTimeMs());
	fs::RecursiveDelete(root);
	makeWorld(root + DIR_DELIM + "worlds" + DIR_DELIM + "alpha", "alpha", 1);
	makeWorld(root + DIR_DELIM + "worlds" + DIR_DELIM + "beta", "beta", 2);

	WorldCluster::Config cfg;
	cfg.server_dir = root;
	cfg.worlds_dir = root + DIR_DELIM + "worlds";
	guard.worldsDir(cfg.worlds_dir);
	cfg.default_world = "alpha";
	cfg.state_path = root + DIR_DELIM + "players.txt";
	cfg.ban_path = root + DIR_DELIM + "ipban.txt";

	const u16 port = 44000 + (porting::getTimeMs() % 1000);
	{
		WorldCluster cluster(cfg, UDPSocket::Create(Address(127, 0, 0, 1, port)));
		cluster.start();
		Server *alpha = cluster.getWorldServer("alpha");
		Server *beta = cluster.getWorldServer("beta");

		TestClient client(Address(127, 0, 0, 1, port), "invtester");
		UASSERT(client.join());
		v3f pos;
		UASSERT(client.expectMovePlayer(&pos));
		UASSERT(waitFor([&] { return queryPlayer(alpha, "invtester").has_peer; }));

		giveItems(alpha, "invtester", "testnodes:normal", 7);
		setHealth(alpha, "invtester", 7, 5);
		UASSERT(playerInventory(alpha, "invtester").find("testnodes:normal 7") !=
			std::string::npos);

		auto move = [&] (const char *from, const char *to, Server *target) {
			WorldTransferRequest req;
			req.player_name = "invtester";
			req.target_world = to;
			std::string err;
			UASSERT(cluster.transferPlayer(from, req, &err));
			NetworkPacket change;
			UASSERT(client.expect(TOCLIENT_WORLD_CHANGE, &change));
			client.sendReady();
			UASSERT(waitFor([&] { return queryPlayer(target, "invtester").has_peer; }));
		};

		// The inventory comes along, and the client is told about it
		move("alpha", "beta", beta);
		UASSERT(playerInventory(beta, "invtester").find("testnodes:normal 7") !=
			std::string::npos);
		{
			NetworkPacket inv;
			UASSERT(client.expect(TOCLIENT_INVENTORY, &inv));
			std::string content = inv.readLongString();
			UASSERT(content.find("testnodes:normal 7") != std::string::npos);
		}

		// So does the health
		UASSERT(getHealth(beta, "invtester") == (std::pair<u16, u16>(7, 5)));

		// Changes made in beta come back
		giveItems(beta, "invtester", "testnodes:normal", 3);
		setHealth(beta, "invtester", 4, 9);
		move("beta", "alpha", alpha);
		UASSERT(playerInventory(alpha, "invtester").find("testnodes:normal 10") !=
			std::string::npos);
		UASSERT(getHealth(alpha, "invtester") == (std::pair<u16, u16>(4, 9)));

	}
	fs::RecursiveDelete(root);
}

void TestMultiworld::testCreateAndRemoveWorlds()
{
	SettingsGuard guard;

	const std::string root = fs::TempPath() + DIR_DELIM + "voxelyx_multiworld_cr_" +
		std::to_string(porting::getTimeMs());
	fs::RecursiveDelete(root);
	makeWorld(root + DIR_DELIM + "worlds" + DIR_DELIM + "alpha", "alpha", 1);

	WorldCluster::Config cfg;
	cfg.server_dir = root;
	cfg.worlds_dir = root + DIR_DELIM + "worlds";
	guard.worldsDir(cfg.worlds_dir);
	cfg.default_world = "alpha";
	cfg.state_path = root + DIR_DELIM + "players.txt";
	cfg.ban_path = root + DIR_DELIM + "ipban.txt";

	{
		const u16 port = 45000 + (porting::getTimeMs() % 1000);
		WorldCluster cluster(cfg, UDPSocket::Create(Address(127, 0, 0, 1, port)));
		cluster.start();
		Server *alpha = cluster.getWorldServer("alpha");
		UASSERTEQ(size_t, cluster.worldCount(), 1);

		std::string err;
		// Bad requests are refused right away
		UASSERT(!cluster.createWorld("alpha", "", &err));
		UASSERT(!cluster.createWorld("alpha", "../escape", &err));
		UASSERT(!cluster.createWorld("alpha", ".removed", &err));
		UASSERT(!cluster.createWorld("alpha", "with space", &err));
		UASSERT(!cluster.createWorld("alpha", "alpha", &err));
		UASSERT(!cluster.removeWorld("alpha", "nonexistent", false, &err));
		UASSERT(!cluster.removeWorld("alpha", "alpha", false, &err)); // the default world

		// Creating
		UASSERT(cluster.createWorld("alpha", "gamma", &err));
		UASSERT(!cluster.createWorld("alpha", "gamma", &err)); // already on its way
		UASSERT(waitFor([&] { return cluster.worldCount() == 2; }, 60000));
		UASSERT(fs::PathExists(cfg.worlds_dir + DIR_DELIM "gamma" DIR_DELIM "world.conf"));
		Server *gamma = cluster.getWorldServer("gamma");
		UASSERT(gamma != nullptr);
		// The world after alpha (1) is 2
		UASSERTEQ(u32, gamma->getWorld()->getId(), 2);
		// Names made of digits are IDs, not names
		UASSERT(!cluster.createWorld("alpha", "42", &err));
		// Same mods, so players can go there without reconnecting
		for (const WorldInfo &w : cluster.listWorlds("alpha"))
			UASSERT(w.compatible);

		// A player can be moved into it ...
		TestClient client(Address(127, 0, 0, 1, port), "crtester");
		UASSERT(client.join());
		v3f pos;
		UASSERT(client.expectMovePlayer(&pos));
		UASSERT(waitFor([&] { return queryPlayer(alpha, "crtester").has_peer; }));
		WorldTransferRequest req;
		req.player_name = "crtester";
		req.target_world = "gamma";
		UASSERT(cluster.transferPlayer("alpha", req, &err));
		NetworkPacket change;
		UASSERT(client.expect(TOCLIENT_WORLD_CHANGE, &change));
		client.sendReady();
		UASSERT(waitFor([&] { return queryPlayer(gamma, "crtester").has_peer; }));

		// ... and is moved out again when it is removed. The files are kept.
		UASSERT(cluster.removeWorld("alpha", "gamma", false, &err));
		// Nobody can go in any more
		UASSERT(!cluster.transferPlayer("alpha", req, &err));
		UASSERT(client.expect(TOCLIENT_WORLD_CHANGE, &change));
		client.sendReady();
		UASSERT(waitFor([&] { return queryPlayer(alpha, "crtester").has_peer; }));
		UASSERT(waitFor([&] { return cluster.worldCount() == 1; }, 60000));
		UASSERT(waitFor([&] {
			return !fs::PathExists(cfg.worlds_dir + DIR_DELIM "gamma");
		}, 20000));
		{
			bool kept = false;
			for (const auto &node : fs::GetDirListing(cfg.worlds_dir + DIR_DELIM ".removed"))
				if (node.dir && str_starts_with(node.name, "gamma-"))
					kept = true;
			UASSERT(kept);
		}
		// The player is fine
		UASSERT(!playerInventory(alpha, "crtester").empty());

		// A new world of the same name is possible, and can be removed for good
		UASSERT(cluster.createWorld("alpha", "gamma", &err));
		UASSERT(waitFor([&] { return cluster.worldCount() == 2; }, 60000));
		// The ID of the removed world is not given out again
		UASSERTEQ(u32, cluster.getWorldServer("gamma")->getWorld()->getId(), 3);
		UASSERT(cluster.removeWorld("alpha", "gamma", true, &err));
		UASSERT(waitFor([&] { return cluster.worldCount() == 1; }, 60000));
		UASSERT(waitFor([&] {
			return !fs::PathExists(cfg.worlds_dir + DIR_DELIM "gamma");
		}, 20000));
	}
	fs::RecursiveDelete(root);
}

void TestMultiworld::testWorldIds()
{
	SettingsGuard guard;

	const std::string root = fs::TempPath() + DIR_DELIM + "voxelyx_multiworld_ids_" +
		std::to_string(porting::getTimeMs());
	fs::RecursiveDelete(root);
	const std::string worlds = root + DIR_DELIM + "worlds";
	guard.worldsDir(worlds);

	// IDs: one more than the highest, also counting removed worlds
	UASSERTEQ(u32, allocateWorldId(worlds + DIR_DELIM + "x"), 1);
	makeWorld(worlds + DIR_DELIM + "a", "a", 1);
	makeWorld(worlds + DIR_DELIM + "b", "b", 5);
	makeWorld(worlds + DIR_DELIM + REMOVED_WORLDS_DIRNAME + DIR_DELIM + "c-1",
		"c", 9);
	UASSERTEQ(u32, allocateWorldId(worlds + DIR_DELIM + "x"), 10);
	u32 id = 0;
	UASSERT(readWorldId(worlds + DIR_DELIM + "b", &id));
	UASSERTEQ(u32, id, 5);
	// No ID, no world: nothing is guessed
	UASSERT(fs::safeWriteToFile(worlds + DIR_DELIM + "b" + DIR_DELIM + "world.conf",
		"world_id = 0\n"));
	UASSERT(!readWorldId(worlds + DIR_DELIM + "b", &id));
	UASSERT(fs::safeWriteToFile(worlds + DIR_DELIM + "b" + DIR_DELIM + "world.conf",
		"world_name = b\n"));
	UASSERT(!readWorldId(worlds + DIR_DELIM + "b", &id));

	// Two worlds with the same ID: the server refuses to start
	makeWorld(worlds + DIR_DELIM + "b", "b", 1);
	WorldCluster::Config cfg;
	cfg.server_dir = root;
	cfg.worlds_dir = worlds;
	cfg.default_world = "a";
	cfg.state_path = root + DIR_DELIM + "players.txt";
	cfg.ban_path = root + DIR_DELIM + "ipban.txt";
	const u16 port = 45000 + (porting::getTimeMs() % 2000);
	bool threw = false;
	try {
		WorldCluster cluster(cfg, UDPSocket::Create(Address(127, 0, 0, 1, port)));
		cluster.start();
	} catch (const ServerError &) {
		threw = true;
	}
	UASSERT(threw);
	fs::RecursiveDelete(root);
}
