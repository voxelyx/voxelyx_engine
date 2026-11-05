// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test.h"

#include "mock_server.h"
#include "database/database-sqlite3.h"
#include "config.h"
#if USE_POSTGRESQL
#include "database/database-postgresql.h"
#endif
#include "emerge.h"
#include "filesys.h"
#include "inventory.h"
#include "remoteplayer.h"
#include "server/player_sao.h"
#include "serverenvironment.h"
#include "servermap.h"

/*
	The accounts database (accounts.sqlite): the schema, and the state of the
	players in the worlds. The accounts themselves are in test_authdatabase.cpp.
*/
class TestAccountsDatabase : public TestBase
{
public:
	TestAccountsDatabase() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestAccountsDatabase"; }

	void runTests(IGameDef *gamedef);

	void testSchema();
	void testRoundTrip();
	void testWorldsAreSeparate();
	void testRemovePlayer();
	void testCascades();
	void testLockedAccount();
	void testDeletedAccountStaysDeleted();
	void testTwoConnections();
	void testPostgreSQL();

private:
	// All that is needed to have a player that can be saved and loaded
	struct TestPlayer {
		TestPlayer(ServerEnvironment *env, IItemDefManager *idef, const std::string &name) :
			player(name, idef), sao(std::make_unique<PlayerSAO>(env, &player, 1, false))
		{
			player.setPlayerSAO(sao.get());
		}
		~TestPlayer() { player.setPlayerSAO(nullptr); }
		RemotePlayer player;
		std::unique_ptr<PlayerSAO> sao;
	};

	// Number of rows in a table, looked at without the database classes
	s64 count(const std::string &table, const std::string &where = "1");

	std::string m_dir;
	ServerEnvironment *m_env = nullptr;
	IItemDefManager *m_idef = nullptr;
};

static TestAccountsDatabase g_test_instance;

namespace {

AuthEntry makeAccount(AuthDatabase &db, const std::string &name)
{
	AuthEntry e;
	e.name = name;
	e.password = "pw";
	e.last_login = 1;
	UASSERT(db.createAuth(e));
	return e;
}

}

s64 TestAccountsDatabase::count(const std::string &table, const std::string &where)
{
	sqlite3 *db = nullptr;
	UASSERTEQ(int, sqlite3_open_v2((m_dir + DIR_DELIM "accounts.sqlite").c_str(), &db,
		SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
	sqlite3_stmt *stmt = nullptr;
	const std::string sql = "SELECT COUNT(*) FROM " + table + " WHERE " + where;
	UASSERTEQ(int, sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK);
	UASSERTEQ(int, sqlite3_step(stmt), SQLITE_ROW);
	s64 n = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return n;
}

void TestAccountsDatabase::runTests(IGameDef *gamedef)
{
	MockServer server(getTestTempDirectory());
	{
		std::ofstream ofs(server.getWorldPath() + DIR_DELIM "world.conf",
			std::ios::out | std::ios::binary);
		ofs << "backend = dummy\nmod_storage_backend = dummy\nworld_id = 1\n";
	}
	MetricsBackend mb;
	EmergeManager emerge(&server, &mb);
	auto map = std::make_unique<ServerMap>(server.getWorldPath(), gamedef, &emerge, &mb);
	ServerEnvironment env(std::move(map), &server, &mb);
	m_env = &env;
	m_idef = gamedef->getItemDefManager();

	const std::string base = getTestTempDirectory() + DIR_DELIM "accounts_test";
	int n = 0;
	auto fresh = [&] {
		// A new directory, so that every test starts with a new database
		m_dir = base + std::to_string(n++);
		fs::RecursiveDelete(m_dir);
		fs::CreateAllDirs(m_dir);
	};

	fresh(); TEST(testSchema);
	fresh(); TEST(testRoundTrip);
	fresh(); TEST(testWorldsAreSeparate);
	fresh(); TEST(testRemovePlayer);
	fresh(); TEST(testCascades);
	fresh(); TEST(testLockedAccount);
	fresh(); TEST(testDeletedAccountStaysDeleted);
	fresh(); TEST(testTwoConnections);
	TEST(testPostgreSQL);

	m_env = nullptr;
}

////////////////////////////////////////////////////////////////////////////////

void TestAccountsDatabase::testSchema()
{
	UASSERT(!fs::PathExists(m_dir + DIR_DELIM "accounts.sqlite"));
	{
		AuthDatabaseSQLite3 auth(m_dir);
		auth.registerWorld(1, "one");
	}
	UASSERT(fs::PathExists(m_dir + DIR_DELIM "accounts.sqlite"));
	// Nothing of the old formats
	UASSERT(!fs::PathExists(m_dir + DIR_DELIM "auth.sqlite"));
	UASSERT(!fs::PathExists(m_dir + DIR_DELIM "players.sqlite"));

	for (const char *table : {"worlds", "accounts", "privileges", "world_players",
			"world_player_metadata", "world_player_inventories",
			"world_player_inventory_items"})
		UASSERT(count("sqlite_master", std::string("type = 'table' AND name = '") +
			table + "'") == 1);
	UASSERT(count("sqlite_master", "name IN ('auth', 'user_privileges', 'player')") == 0);

	// Opening it again, from both kinds of connection, keeps it
	{
		AuthDatabaseSQLite3 auth(m_dir);
		PlayerDatabaseSQLite3 players(m_dir, 1);
		std::vector<std::string> names;
		auth.listNames(names);
		players.listPlayers(names);
	}
	UASSERTEQ(s64, count("worlds"), 1);

	// A database of a newer version is not touched
	{
		sqlite3 *db = nullptr;
		UASSERTEQ(int, sqlite3_open((m_dir + DIR_DELIM "accounts.sqlite").c_str(), &db), SQLITE_OK);
		UASSERTEQ(int, sqlite3_exec(db, "PRAGMA user_version = 99;", nullptr, nullptr, nullptr),
			SQLITE_OK);
		sqlite3_close(db);
	}
	AuthDatabaseSQLite3 auth(m_dir);
	bool threw = false;
	try {
		auth.registerWorld(1, "one");
	} catch (const DatabaseException &) {
		threw = true;
	}
	UASSERT(threw);
}

void TestAccountsDatabase::testRoundTrip()
{
	AuthDatabaseSQLite3 auth(m_dir);
	auth.registerWorld(1, "one");
	makeAccount(auth, "alice");
	PlayerDatabaseSQLite3 db(m_dir, 1);

	// Lists in the order they have in the inventory: what the player starts with, then ours
	std::vector<std::string> list_names;
	std::vector<u32> list_sizes;
	{
		TestPlayer p(m_env, m_idef, "alice");
		p.sao->setBasePosition(v3f(1.5f, -20.25f, 300.f));
		p.sao->setLookPitch(12.5f);
		p.sao->setPlayerYaw(-90.f);
		p.sao->setHPRaw(7);
		p.sao->setBreath(3, false);
		InventoryList *main = p.player.inventory.addList("main", 8);
		main->setWidth(4);
		ItemStack stack;
		stack.deSerialize("test:stone 5");
		main->changeItem(2, stack);
		stack.deSerialize("test:pick 1 300");
		main->changeItem(7, stack);
		p.player.inventory.addList("craft", 9);
		p.player.inventory.addList("empty", 0);
		// Metadata is arbitrary bytes
		p.sao->getMeta().setString("plain", "value");
		p.sao->getMeta().setString(std::string("nul\0key", 7), std::string("a\0b\0", 4));
		for (const InventoryList *l : p.player.inventory.getLists()) {
			list_names.push_back(l->getName());
			list_sizes.push_back(l->getSize());
		}
		db.savePlayer(&p.player);
	}

	// Only the slots that hold something are stored
	UASSERTEQ(s64, count("world_players"), 1);
	UASSERTEQ(s64, count("world_player_inventories"), (s64) list_names.size());
	UASSERTEQ(s64, count("world_player_inventory_items"), 2);
	UASSERTEQ(s64, count("world_player_metadata"), 2);

	TestPlayer q(m_env, m_idef, "alice");
	UASSERT(db.loadPlayer(&q.player, q.sao.get()));
	UASSERT((q.sao->getBasePosition() - v3f(1.5f, -20.25f, 300.f)).getLength() < 0.001f);
	UASSERT(fabsf(q.sao->getLookPitch() - 12.5f) < 0.001f);
	UASSERTEQ(u16, q.sao->getHP(), 7);
	UASSERTEQ(u16, q.sao->getBreath(), 3);

	const auto &lists = q.player.inventory.getLists();
	UASSERTEQ(size_t, lists.size(), list_names.size());
	for (size_t i = 0; i < lists.size(); i++) { // same lists, same order
		UASSERTEQ(std::string, lists[i]->getName(), list_names[i]);
		UASSERTEQ(u32, lists[i]->getSize(), list_sizes[i]);
	}
	const InventoryList *main = q.player.inventory.getList("main");
	UASSERT(main);
	UASSERTEQ(u32, main->getSize(), 8);
	UASSERTEQ(u32, main->getWidth(), 4);
	UASSERTEQ(std::string, main->getItem(2).getItemString(), "test:stone 5");
	UASSERTEQ(std::string, main->getItem(7).getItemString(), "test:pick 1 300");
	UASSERT(main->getItem(0).empty());
	UASSERTEQ(u32, q.player.inventory.getList("craft")->getSize(), 9);
	UASSERT(q.player.inventory.getList("empty"));

	const StringMap &meta = q.sao->getMeta().getStrings();
	UASSERTEQ(size_t, meta.size(), 2);
	UASSERTEQ(std::string, meta.at("plain"), "value");
	UASSERTEQ(std::string, meta.at(std::string("nul\0key", 7)), std::string("a\0b\0", 4));

	// Saving again replaces what was there
	{
		TestPlayer p(m_env, m_idef, "alice");
		p.player.inventory.addList("main", 2);
		p.sao->setBasePosition(v3f(0, 0, 0));
		p.sao->setHPRaw(20);
		p.sao->setBreath(10, false);
		db.savePlayer(&p.player);
	}
	UASSERTEQ(s64, count("world_players"), 1);
	UASSERTEQ(s64, count("world_player_inventory_items"), 0);
	UASSERTEQ(s64, count("world_player_metadata"), 0);
}

void TestAccountsDatabase::testWorldsAreSeparate()
{
	AuthDatabaseSQLite3 auth(m_dir);
	auth.registerWorld(1, "one");
	auth.registerWorld(2, "two");
	makeAccount(auth, "alice");
	PlayerDatabaseSQLite3 one(m_dir, 1), two(m_dir, 2);

	TestPlayer p(m_env, m_idef, "alice");
	p.sao->setBasePosition(v3f(10, 20, 30));
	p.sao->setHPRaw(5);
	one.savePlayer(&p.player);

	std::vector<std::string> names;
	one.listPlayers(names);
	UASSERTEQ(size_t, names.size(), 1);
	UASSERTEQ(std::string, names[0], "alice");
	names.clear();
	two.listPlayers(names);
	UASSERT(names.empty());

	// A new player there
	TestPlayer q(m_env, m_idef, "alice");
	UASSERT(!two.loadPlayer(&q.player, q.sao.get()));

	p.sao->setBasePosition(v3f(-1, -2, -3));
	p.sao->setHPRaw(9);
	two.savePlayer(&p.player);
	UASSERTEQ(s64, count("world_players"), 2);

	TestPlayer r1(m_env, m_idef, "alice"), r2(m_env, m_idef, "alice");
	UASSERT(one.loadPlayer(&r1.player, r1.sao.get()));
	UASSERT(two.loadPlayer(&r2.player, r2.sao.get()));
	UASSERT((r1.sao->getBasePosition() - v3f(10, 20, 30)).getLength() < 0.001f);
	UASSERT((r2.sao->getBasePosition() - v3f(-1, -2, -3)).getLength() < 0.001f);
	UASSERTEQ(u16, r1.sao->getHP(), 5);
	UASSERTEQ(u16, r2.sao->getHP(), 9);
}

void TestAccountsDatabase::testRemovePlayer()
{
	AuthDatabaseSQLite3 auth(m_dir);
	auth.registerWorld(1, "one");
	auth.registerWorld(2, "two");
	makeAccount(auth, "alice");
	PlayerDatabaseSQLite3 one(m_dir, 1), two(m_dir, 2);

	TestPlayer p(m_env, m_idef, "alice");
	p.player.inventory.addList("main", 4)->changeItem(0, ItemStack());
	p.sao->getMeta().setString("k", "v");
	one.savePlayer(&p.player);
	two.savePlayer(&p.player);

	UASSERT(!one.removePlayer("nobody"));
	UASSERT(one.removePlayer("alice"));
	UASSERT(!one.removePlayer("alice"));

	// Only in that world, and the account is not touched
	TestPlayer q(m_env, m_idef, "alice");
	UASSERT(!one.loadPlayer(&q.player, q.sao.get()));
	UASSERT(two.loadPlayer(&q.player, q.sao.get()));
	AuthEntry e;
	UASSERT(auth.getAuth("alice", e));
	UASSERTEQ(s64, count("world_players"), 1);
	UASSERTEQ(s64, count("world_player_inventories", "world_player_id IN "
		"(SELECT id FROM world_players)"), count("world_player_inventories"));
	UASSERT(count("world_player_inventories") > 0); // of the other world's
	UASSERTEQ(s64, count("world_player_metadata"), 1);
}

void TestAccountsDatabase::testCascades()
{
	AuthDatabaseSQLite3 auth(m_dir);
	auth.registerWorld(1, "one");
	auth.registerWorld(2, "two");
	PlayerDatabaseSQLite3 one(m_dir, 1), two(m_dir, 2);

	AuthEntry alice = makeAccount(auth, "alice");
	alice.privileges = {"shout", "fly"};
	UASSERT(auth.saveAuth(alice));
	makeAccount(auth, "bob");
	for (const char *name : {"alice", "bob"}) {
		TestPlayer p(m_env, m_idef, name);
		InventoryList *l = p.player.inventory.addList("main", 4);
		ItemStack s;
		s.deSerialize("test:stone 1");
		l->changeItem(1, s);
		p.sao->getMeta().setString("k", "v");
		one.savePlayer(&p.player);
		two.savePlayer(&p.player);
	}
	auth.setCurrentWorld("alice", 2);
	UASSERTEQ(s64, count("world_players"), 4);
	UASSERTEQ(s64, count("privileges"), 2);

	// A world takes the players in it with it. Those who were there are nowhere.
	auth.deleteWorld(2);
	UASSERTEQ(s64, count("world_players"), 2);
	UASSERTEQ(s64, count("world_players", "world_id = 2"), 0);
	UASSERTEQ(s64, count("world_player_inventory_items"), 2);
	UASSERTEQ(s64, count("world_player_metadata"), 2);
	u32 w;
	UASSERT(!auth.getCurrentWorld("alice", &w));

	// An account takes its privileges and everything in the worlds with it
	UASSERT(auth.deleteAuth("alice"));
	UASSERTEQ(s64, count("privileges"), 0);
	UASSERTEQ(s64, count("world_players"), 1);
	UASSERTEQ(s64, count("world_player_inventory_items"), 1);
	UASSERTEQ(s64, count("world_player_metadata"), 1);
	UASSERTEQ(s64, count("world_player_inventories", "world_player_id NOT IN "
		"(SELECT id FROM world_players)"), 0);

	// No row points to nothing
	sqlite3 *db = nullptr;
	UASSERTEQ(int, sqlite3_open((m_dir + DIR_DELIM "accounts.sqlite").c_str(), &db), SQLITE_OK);
	sqlite3_stmt *stmt = nullptr;
	UASSERTEQ(int, sqlite3_prepare_v2(db, "PRAGMA foreign_key_check;", -1, &stmt, nullptr),
		SQLITE_OK);
	UASSERTEQ(int, sqlite3_step(stmt), SQLITE_DONE);
	sqlite3_finalize(stmt);
	sqlite3_close(db);
}

void TestAccountsDatabase::testLockedAccount()
{
	AuthDatabaseSQLite3 auth(m_dir);
	auth.registerWorld(1, "one");
	PlayerDatabaseSQLite3 db(m_dir, 1);

	// carol authenticates some other way: she has no account until she is loaded
	AuthEntry e;
	UASSERT(!auth.getAuth("carol", e));
	TestPlayer p(m_env, m_idef, "carol");
	UASSERT(!db.loadPlayer(&p.player, p.sao.get())); // a new player

	UASSERT(auth.getAuth("carol", e));
	UASSERTEQ(std::string, e.password, ACCOUNT_LOCKED_PASSWORD);
	UASSERT(e.privileges.empty());
	UASSERTEQ(s64, e.last_login, 0);

	// Now she can be saved, and loaded in the next session
	p.sao->setHPRaw(11);
	db.savePlayer(&p.player);
	TestPlayer q(m_env, m_idef, "carol");
	UASSERT(db.loadPlayer(&q.player, q.sao.get()));
	UASSERTEQ(u16, q.sao->getHP(), 11);
	// Loading doesn't add another one
	UASSERTEQ(s64, count("accounts"), 1);
}

void TestAccountsDatabase::testDeletedAccountStaysDeleted()
{
	AuthDatabaseSQLite3 auth(m_dir);
	auth.registerWorld(1, "one");
	makeAccount(auth, "dave");
	PlayerDatabaseSQLite3 db(m_dir, 1);

	TestPlayer p(m_env, m_idef, "dave");
	UASSERT(!db.loadPlayer(&p.player, p.sao.get()));
	// The account is deleted while he is online, then he leaves
	UASSERT(auth.deleteAuth("dave"));
	db.savePlayer(&p.player);

	AuthEntry e;
	UASSERT(!auth.getAuth("dave", e));
	UASSERTEQ(s64, count("accounts"), 0);
	UASSERTEQ(s64, count("world_players"), 0);
}

void TestAccountsDatabase::testTwoConnections()
{
	// What a server with several worlds does: several worlds write to the same file
	// while the accounts are written through another connection
	AuthDatabaseSQLite3 auth(m_dir);
	auth.registerWorld(1, "one");
	auth.registerWorld(2, "two");
	for (int i = 0; i < 4; i++)
		makeAccount(auth, "player" + std::to_string(i));

	auto work = [&](u32 world) {
		PlayerDatabaseSQLite3 db(m_dir, world);
		for (int round = 0; round < 25; round++) {
			for (int i = 0; i < 4; i++) {
				TestPlayer p(m_env, m_idef, "player" + std::to_string(i));
				p.player.inventory.addList("main", 4)->changeItem(0, [&] {
					ItemStack s;
					s.deSerialize("test:stone 1");
					return s;
				}());
				p.sao->setHPRaw(round);
				db.savePlayer(&p.player);
			}
		}
	};
	std::exception_ptr err1, err2;
	std::thread t1([&] { try { work(1); } catch (...) { err1 = std::current_exception(); } });
	std::thread t2([&] { try { work(2); } catch (...) { err2 = std::current_exception(); } });
	// Meanwhile on the accounts connection
	for (int i = 0; i < 100; i++) {
		AuthEntry e;
		UASSERT(auth.getAuth("player0", e));
		e.last_login = 100 + i;
		UASSERT(auth.saveAuth(e));
		auth.setCurrentWorld("player1", 1 + i % 2);
	}
	t1.join();
	t2.join();
	UASSERT(!err1);
	UASSERT(!err2);
	UASSERTEQ(s64, count("world_players"), 8);
	UASSERTEQ(s64, count("world_player_inventory_items"), 8);
}

// The same things as above that don't need to look into the file, on PostgreSQL.
// Only if VOXELYX_TEST_PGSQL is set, see test_authdatabase.cpp.
void TestAccountsDatabase::testPostgreSQL()
{
#if USE_POSTGRESQL
	const char *connect = getenv("VOXELYX_TEST_PGSQL");
	if (!connect)
		return;
	{
		PGconn *c = PQconnectdb(connect);
		UASSERT(PQstatus(c) == CONNECTION_OK);
		PQclear(PQexec(c, "DROP SCHEMA public CASCADE; CREATE SCHEMA public;"));
		PQfinish(c);
	}

	AuthDatabasePostgreSQL auth(connect);
	auth.registerWorld(1, "one");
	auth.registerWorld(2, "two");
	makeAccount(auth, "alice");
	PlayerDatabasePostgreSQL one(connect, 1), two(connect, 2);

	std::vector<std::string> list_names;
	{
		TestPlayer p(m_env, m_idef, "alice");
		p.sao->setBasePosition(v3f(1.5f, -20.25f, 300.f));
		p.sao->setLookPitch(12.5f);
		p.sao->setHPRaw(7);
		p.sao->setBreath(3, false);
		InventoryList *main = p.player.inventory.addList("main", 8);
		main->setWidth(4);
		ItemStack stack;
		stack.deSerialize("test:stone 5");
		main->changeItem(2, stack);
		p.sao->getMeta().setString("plain", "value");
		p.sao->getMeta().setString(std::string("nul\0key", 7), std::string("a\0b\0", 4));
		for (const InventoryList *l : p.player.inventory.getLists())
			list_names.push_back(l->getName());
		one.savePlayer(&p.player);
	}
	TestPlayer q(m_env, m_idef, "alice");
	UASSERT(one.loadPlayer(&q.player, q.sao.get()));
	UASSERT((q.sao->getBasePosition() - v3f(1.5f, -20.25f, 300.f)).getLength() < 0.001f);
	UASSERTEQ(u16, q.sao->getHP(), 7);
	UASSERTEQ(u16, q.sao->getBreath(), 3);
	UASSERTEQ(size_t, q.player.inventory.getLists().size(), list_names.size());
	const InventoryList *main = q.player.inventory.getList("main");
	UASSERT(main);
	UASSERTEQ(u32, main->getWidth(), 4);
	UASSERTEQ(std::string, main->getItem(2).getItemString(), "test:stone 5");
	UASSERT(main->getItem(0).empty());
	const StringMap &meta = q.sao->getMeta().getStrings();
	UASSERTEQ(size_t, meta.size(), 2);
	UASSERTEQ(std::string, meta.at(std::string("nul\0key", 7)), std::string("a\0b\0", 4));

	// Another world, another state
	TestPlayer r(m_env, m_idef, "alice");
	UASSERT(!two.loadPlayer(&r.player, r.sao.get()));
	std::vector<std::string> names;
	one.listPlayers(names);
	UASSERTEQ(size_t, names.size(), 1);
	names.clear();
	two.listPlayers(names);
	UASSERT(names.empty());

	// A player without an account gets a locked one; a deleted account stays deleted
	TestPlayer c(m_env, m_idef, "carol");
	UASSERT(!one.loadPlayer(&c.player, c.sao.get()));
	AuthEntry e;
	UASSERT(auth.getAuth("carol", e));
	UASSERTEQ(std::string, e.password, ACCOUNT_LOCKED_PASSWORD);
	UASSERT(auth.deleteAuth("carol"));
	one.savePlayer(&c.player);
	UASSERT(!auth.getAuth("carol", e));

	// Worlds and accounts take their players with them
	auth.setCurrentWorld("alice", 1);
	u32 w = 0;
	UASSERT(auth.getCurrentWorld("alice", &w));
	UASSERTEQ(u32, w, 1);
	two.savePlayer(&q.player);
	auth.deleteWorld(1);
	UASSERT(!auth.getCurrentWorld("alice", &w));
	names.clear();
	one.listPlayers(names);
	UASSERT(names.empty());
	names.clear();
	two.listPlayers(names);
	UASSERTEQ(size_t, names.size(), 1);
	UASSERT(auth.deleteAuth("alice"));
	names.clear();
	two.listPlayers(names);
	UASSERT(names.empty());
#endif
}
