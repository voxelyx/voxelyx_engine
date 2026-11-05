// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2018 bendeutsch, Ben Deutsch <ben@bendeutsch.de>

#include "test.h"

#include <algorithm>
#include "database/database-dummy.h"
#include "database/database-sqlite3.h"
#include "config.h"
#if USE_POSTGRESQL
#include "database/database-postgresql.h"
#endif
#include "util/string.h"
#include "filesys.h"

namespace
{
// Anonymous namespace to create classes that are only
// visible to this file
//
// These are helpers that return a *AuthDatabase and
// allow us to run the same tests on different databases and
// database acquisition strategies.

class AuthDatabaseProvider
{
public:
	virtual ~AuthDatabaseProvider() = default;
	virtual AuthDatabase *getAuthDatabase() = 0;
};

class FixedProvider : public AuthDatabaseProvider
{
public:
	FixedProvider(AuthDatabase *auth_db) : auth_db(auth_db){};
	virtual ~FixedProvider(){};
	virtual AuthDatabase *getAuthDatabase() { return auth_db; };

private:
	AuthDatabase *auth_db;
};

class SQLite3Provider : public AuthDatabaseProvider
{
public:
	SQLite3Provider(const std::string &dir) : dir(dir){};
	virtual ~SQLite3Provider() { delete auth_db; };
	virtual AuthDatabase *getAuthDatabase()
	{
		delete auth_db;
		auth_db = new AuthDatabaseSQLite3(dir);
		return auth_db;
	};

private:
	std::string dir;
	AuthDatabase *auth_db = nullptr;
};

#if USE_POSTGRESQL
// Only if VOXELYX_TEST_PGSQL is the connection string of a database that may be
// emptied, e.g. "host=127.0.0.1 dbname=voxelyx_test user=..."
class PostgreSQLProvider : public AuthDatabaseProvider
{
public:
	PostgreSQLProvider(const std::string &connect) : connect(connect)
	{
		PGconn *c = PQconnectdb(connect.c_str());
		if (PQstatus(c) != CONNECTION_OK)
			throw DatabaseException(PQerrorMessage(c));
		PQclear(PQexec(c, "DROP SCHEMA public CASCADE; CREATE SCHEMA public;"));
		PQfinish(c);
	}
	virtual ~PostgreSQLProvider() { delete auth_db; };
	virtual AuthDatabase *getAuthDatabase()
	{
		delete auth_db;
		auth_db = new AuthDatabasePostgreSQL(connect);
		return auth_db;
	};

private:
	std::string connect;
	AuthDatabase *auth_db = nullptr;
};
#endif

class MemoryProvider : public AuthDatabaseProvider
{
public:
	MemoryProvider() = default;
	virtual ~MemoryProvider() = default;
	virtual AuthDatabase *getAuthDatabase() { return &auth_db; };

private:
	AuthDatabaseDummy auth_db;
};
}

class TestAuthDatabase : public TestBase
{
public:
	TestAuthDatabase() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestAuthDatabase"; }

	void runTests(IGameDef *gamedef);
	void runTestsForCurrentDB();

	void testRecallFail();
	void testCreate();
	void testRecall();
	void testChange();
	void testRecallChanged();
	void testChangePrivileges();
	void testRecallChangedPrivileges();
	void testListNames();
	void testDelete();
	void testWorlds();
	void testLastLogin();

private:
	AuthDatabaseProvider *auth_provider;
};

static TestAuthDatabase g_test_instance;

void TestAuthDatabase::runTests(IGameDef *gamedef)
{
	// fixed directory, for persistence
	const std::string test_dir = getTestTempDirectory();

	// Each set of tests is run twice for each database type:
	// one where we reuse the same AuthDatabase object (to test local caching),
	// and one where we create a new AuthDatabase object for each call
	// (to test actual persistence).

	rawstream << "-------- SQLite3 database (same object)" << std::endl;

	AuthDatabase *auth_db = new AuthDatabaseSQLite3(test_dir);
	auth_provider = new FixedProvider(auth_db);

	runTestsForCurrentDB();

	delete auth_db;
	delete auth_provider;

	// reset database
	fs::DeleteSingleFileOrEmptyDirectory(test_dir + DIR_DELIM + "accounts.sqlite");

	rawstream << "-------- SQLite3 database (new objects)" << std::endl;

	auth_provider = new SQLite3Provider(test_dir);

	runTestsForCurrentDB();

	delete auth_provider;

#if USE_POSTGRESQL
	if (const char *pg = getenv("VOXELYX_TEST_PGSQL")) {
		rawstream << "-------- PostgreSQL database (new objects)" << std::endl;
		auth_provider = new PostgreSQLProvider(pg);
		runTestsForCurrentDB();
		delete auth_provider;
	}
#endif

	// The memory backend is volatile, so we only exercise it with a
	// "same object" provider: recreating the database between calls would
	// simply discard all stored data.
	rawstream << "-------- Memory database (same object)" << std::endl;

	auth_provider = new MemoryProvider();

	runTestsForCurrentDB();

	delete auth_provider;
}

////////////////////////////////////////////////////////////////////////////////

void TestAuthDatabase::runTestsForCurrentDB()
{
	TEST(testRecallFail);
	TEST(testCreate);
	TEST(testRecall);
	TEST(testChange);
	TEST(testRecallChanged);
	TEST(testChangePrivileges);
	TEST(testRecallChangedPrivileges);
	TEST(testListNames);
	TEST(testDelete);
	TEST(testRecallFail);
	TEST(testLastLogin);
	TEST(testWorlds);
}

void TestAuthDatabase::testRecallFail()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	AuthEntry authEntry;

	// no such user yet
	UASSERT(!auth_db->getAuth("TestName", authEntry));
}

void TestAuthDatabase::testCreate()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	AuthEntry authEntry;

	authEntry.name = "TestName";
	authEntry.password = "TestPassword";
	authEntry.privileges.emplace_back("shout");
	authEntry.privileges.emplace_back("interact");
	authEntry.last_login = 1000;
	UASSERT(auth_db->createAuth(authEntry));
}

void TestAuthDatabase::testRecall()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	AuthEntry authEntry;

	UASSERT(auth_db->getAuth("TestName", authEntry));
	UASSERTEQ(std::string, authEntry.name, "TestName");
	UASSERTEQ(std::string, authEntry.password, "TestPassword");
	// the order of privileges is unimportant
	std::sort(authEntry.privileges.begin(), authEntry.privileges.end());
	UASSERTEQ(std::string, str_join(authEntry.privileges, ","), "interact,shout");
}

void TestAuthDatabase::testChange()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	AuthEntry authEntry;

	UASSERT(auth_db->getAuth("TestName", authEntry));
	authEntry.password = "NewPassword";
	authEntry.last_login = 1002;
	UASSERT(auth_db->saveAuth(authEntry));
}

void TestAuthDatabase::testRecallChanged()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	AuthEntry authEntry;

	UASSERT(auth_db->getAuth("TestName", authEntry));
	UASSERTEQ(std::string, authEntry.password, "NewPassword");
	// the order of privileges is unimportant
	std::sort(authEntry.privileges.begin(), authEntry.privileges.end());
	UASSERTEQ(std::string, str_join(authEntry.privileges, ","), "interact,shout");
	UASSERTEQ(u64, authEntry.last_login, 1002);
}

void TestAuthDatabase::testChangePrivileges()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	AuthEntry authEntry;

	UASSERT(auth_db->getAuth("TestName", authEntry));
	authEntry.privileges.clear();
	authEntry.privileges.emplace_back("interact");
	authEntry.privileges.emplace_back("fly");
	authEntry.privileges.emplace_back("dig");
	UASSERT(auth_db->saveAuth(authEntry));
}

void TestAuthDatabase::testRecallChangedPrivileges()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	AuthEntry authEntry;

	UASSERT(auth_db->getAuth("TestName", authEntry));
	// the order of privileges is unimportant
	std::sort(authEntry.privileges.begin(), authEntry.privileges.end());
	UASSERTEQ(std::string, str_join(authEntry.privileges, ","), "dig,fly,interact");
}

void TestAuthDatabase::testListNames()
{

	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	std::vector<std::string> list;

	AuthEntry authEntry;

	authEntry.name = "SecondName";
	authEntry.password = "SecondPassword";
	authEntry.privileges.emplace_back("shout");
	authEntry.privileges.emplace_back("interact");
	authEntry.last_login = 1003;
	auth_db->createAuth(authEntry);

	auth_db->listNames(list);
	// not necessarily sorted, so sort before comparing
	std::sort(list.begin(), list.end());
	UASSERTEQ(std::string, str_join(list, ","), "SecondName,TestName");
}

void TestAuthDatabase::testDelete()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();

	UASSERT(!auth_db->deleteAuth("NoSuchName"));
	UASSERT(auth_db->deleteAuth("TestName"));
	// second try, expect failure
	UASSERT(!auth_db->deleteAuth("TestName"));
}

void TestAuthDatabase::testLastLogin()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	AuthEntry authEntry;

	// The Lua side creates accounts with -1: has not logged in yet
	authEntry.name = "NeverLoggedIn";
	authEntry.password = "pw";
	authEntry.last_login = -1;
	UASSERT(auth_db->createAuth(authEntry));
	UASSERT(authEntry.id != 0);

	AuthEntry read;
	UASSERT(auth_db->getAuth("NeverLoggedIn", read));
	UASSERTEQ(s64, read.last_login, 0);
	UASSERTEQ(u64, read.id, authEntry.id);

	read.last_login = 12345;
	UASSERT(auth_db->saveAuth(read));
	AuthEntry read2;
	UASSERT(auth_db->getAuth("NeverLoggedIn", read2));
	UASSERTEQ(s64, read2.last_login, 12345);

	UASSERT(auth_db->deleteAuth("NeverLoggedIn"));
}

void TestAuthDatabase::testWorlds()
{
	AuthDatabase *auth_db = auth_provider->getAuthDatabase();
	u32 world = 0;

	AuthEntry a;
	a.name = "Alice";
	a.password = "pw";
	a.last_login = 1;
	UASSERT(auth_db->createAuth(a));

	// Not in any world yet
	UASSERT(!auth_db->getCurrentWorld("Alice", &world));
	// No such account: nothing happens
	auth_db->registerWorld(1, "main");
	auth_db->setCurrentWorld("Nobody", 1);
	UASSERT(!auth_db->getCurrentWorld("Nobody", &world));

	auth_db->registerWorld(2, "second");
	auth_db->setCurrentWorld("Alice", 2);
	UASSERT(auth_db->getCurrentWorld("Alice", &world));
	UASSERTEQ(u32, world, 2);
	// Same again, and a change
	auth_db->setCurrentWorld("Alice", 2);
	auth_db->setCurrentWorld("Alice", 1);
	UASSERT(auth_db->getCurrentWorld("Alice", &world));
	UASSERTEQ(u32, world, 1);

	// Registering again only renames
	auth_db->registerWorld(1, "renamed");
	UASSERT(auth_db->getCurrentWorld("Alice", &world));
	UASSERTEQ(u32, world, 1);

	// A world that is deleted is not where anybody is any more
	auth_db->deleteWorld(1);
	UASSERT(!auth_db->getCurrentWorld("Alice", &world));

	// Setting the world doesn't touch the rest of the account
	auth_db->setCurrentWorld("Alice", 2);
	AuthEntry read;
	UASSERT(auth_db->getAuth("Alice", read));
	UASSERTEQ(std::string, read.password, "pw");
	UASSERTEQ(s64, read.last_login, 1);

	// The world goes with the account
	UASSERT(auth_db->deleteAuth("Alice"));
	UASSERT(!auth_db->getCurrentWorld("Alice", &world));
	auth_db->deleteWorld(2);
}
