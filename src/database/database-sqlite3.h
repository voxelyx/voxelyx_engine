// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include <cstring>
#include <string>
#include "database.h"
#include "exceptions.h"

extern "C" {
#include "sqlite3.h"
}

// Template class for SQLite3 based data storage
class Database_SQLite3 : public Database
{
public:
	virtual ~Database_SQLite3();

	void beginSave() override;
	void endSave() override;

	bool initialized() const override { return m_initialized; }

	/// @note not thread-safe
	void verifyDatabase() override;

protected:
	Database_SQLite3(const std::string &savedir, const std::string &dbname);

	// Check if a specific table exists
	bool checkTable(const char *table);

	// Check if a table has a specific column
	bool checkColumn(const char *table, const char *column);

	/* Value conversion helpers */

	// SQLite is asked to make its own copy (SQLITE_TRANSIENT). Binding with a
	// NULL destructor (SQLITE_STATIC) only stores the pointer, which then has to
	// stay valid until the statement has been stepped. Callers don't (and can't
	// be expected to) guarantee that: passing a temporary, e.g. `oss.str()`,
	// used to make saved data read from freed memory, which corrupted the
	// item names of saved player inventories.
	inline void str_to_sqlite(sqlite3_stmt *s, int iCol, std::string_view str) const
	{
		sqlite3_vrfy(sqlite3_bind_text(s, iCol, str.data(), str.size(), SQLITE_TRANSIENT));
	}

	inline void blob_to_sqlite(sqlite3_stmt *s, int iCol, std::string_view str) const
	{
		sqlite3_vrfy(sqlite3_bind_blob(s, iCol, str.data(), str.size(), SQLITE_TRANSIENT));
	}

	inline void int_to_sqlite(sqlite3_stmt *s, int iCol, int val) const
	{
		sqlite3_vrfy(sqlite3_bind_int(s, iCol, val));
	}

	inline void int64_to_sqlite(sqlite3_stmt *s, int iCol, s64 val) const
	{
		sqlite3_vrfy(sqlite3_bind_int64(s, iCol, (sqlite3_int64) val));
	}

	inline void double_to_sqlite(sqlite3_stmt *s, int iCol, double val) const
	{
		sqlite3_vrfy(sqlite3_bind_double(s, iCol, val));
	}

	// Note that the return value is only valid until the statement is stepped or reset.
	inline std::string_view sqlite_to_string_view(sqlite3_stmt *s, int iCol)
	{
		const char* text = reinterpret_cast<const char*>(sqlite3_column_text(s, iCol));
		return text ? std::string_view(text) : std::string_view();
	}

	// Avoid using this in favor of `sqlite_to_string_view`.
	inline std::string sqlite_to_string(sqlite3_stmt *s, int iCol)
	{
		return std::string(sqlite_to_string_view(s, iCol));
	}

	// Converts a BLOB-type column into a string_view (null byte safe).
	// Note that the return value is only valid until the statement is stepped or reset.
	inline std::string_view sqlite_to_blob(sqlite3_stmt *s, int iCol)
	{
		const char *data = reinterpret_cast<const char*>(sqlite3_column_blob(s, iCol));
		if (!data)
			return std::string_view();
		size_t len = sqlite3_column_bytes(s, iCol);
		return std::string_view(data, len);
	}

	inline s32 sqlite_to_int(sqlite3_stmt *s, int iCol)
	{
		return sqlite3_column_int(s, iCol);
	}

	inline u32 sqlite_to_uint(sqlite3_stmt *s, int iCol)
	{
		return (u32) sqlite3_column_int(s, iCol);
	}

	inline s64 sqlite_to_int64(sqlite3_stmt *s, int iCol)
	{
		return (s64) sqlite3_column_int64(s, iCol);
	}

	inline u64 sqlite_to_uint64(sqlite3_stmt *s, int iCol)
	{
		return (u64) sqlite3_column_int64(s, iCol);
	}

	inline float sqlite_to_float(sqlite3_stmt *s, int iCol)
	{
		return (float) sqlite3_column_double(s, iCol);
	}

	inline const v3f sqlite_to_v3f(sqlite3_stmt *s, int iCol)
	{
		return v3f(sqlite_to_float(s, iCol), sqlite_to_float(s, iCol + 1),
				sqlite_to_float(s, iCol + 2));
	}

	// Helper for verifying result of sqlite3 calls
	inline void sqlite3_vrfy(int s, std::string_view m = "", int r = SQLITE_OK) const
	{
		if (s != r) {
			std::string msg(m);
			if (!msg.empty())
				msg.append(": ");
			msg.append(sqlite3_errmsg(m_database));
			throw DatabaseException(msg);
		}
	}

	inline void sqlite3_vrfy(const int s, const int r, std::string_view m = "") const
	{
		sqlite3_vrfy(s, m, r);
	}

	/// Helper for verifying result of sqlite3_step()
	/// @param s status from call
	/// @param m text for error message
	/// @return true if there's a row to read
	inline bool sqlite3_vrfy_step(int s, std::string_view m = "") const
	{
		if (s == SQLITE_ROW)
			return true;
		sqlite3_vrfy(s, m, SQLITE_DONE);
		return false;
	}

	// Called after opening a fresh database file. Should create tables and indices.
	virtual void createDatabase() = 0;

	// Called when opening an existing database file. If it returns true,
	// createDatabase() is called as for a fresh file. For databases that more
	// than one connection may open at the same time, whether the file exists is
	// not a reliable indicator that it has a schema yet.
	virtual bool schemaMissing() { return false; }

	// Abandons the transaction started by beginSave(), after a failure.
	// Does not throw.
	void rollbackSave() noexcept;

	// The statement that beginSave() runs. A database that is written by more
	// than one connection starts its transactions with "BEGIN IMMEDIATE;", so
	// that a connection that has read something can't be left unable to write.
	const char *m_begin_sql = "BEGIN;";

	// Should prepare the necessary statements.
	virtual void initStatements() = 0;

	sqlite3 *m_database = nullptr;

private:
	// Open the database
	void openDatabase();

	bool m_initialized = false;

	const std::string m_savedir;
	const std::string m_dbname;

	sqlite3_stmt *m_stmt_begin = nullptr;
	sqlite3_stmt *m_stmt_end = nullptr;

	u64 m_busy_handler_data[2];

	static int busyHandler(void *data, int count);
};

// Not sure why why we have to do this. can't C++ figure it out on its own?
#define PARENT_CLASS_FUNCS \
	void beginSave() { Database_SQLite3::beginSave(); } \
	void endSave() { Database_SQLite3::endSave(); } \
	void verifyDatabase() { Database_SQLite3::verifyDatabase(); }

class MapDatabaseSQLite3 : private Database_SQLite3, public MapDatabase
{
public:
	MapDatabaseSQLite3(const std::string &savedir);
	virtual ~MapDatabaseSQLite3();

	bool saveBlock(const v3s16 &pos, std::string_view data);
	void loadBlock(const v3s16 &pos, std::string *block);
	bool deleteBlock(const v3s16 &pos);
	void listAllLoadableBlocks(std::vector<v3s16> &dst);

	PARENT_CLASS_FUNCS

protected:
	virtual void createDatabase();
	virtual void initStatements();

private:
	/// @brief Bind block position into statement at column index
	/// @return index of next column after position
	int bindPos(sqlite3_stmt *stmt, v3s16 pos, int index = 1);

	bool m_new_format = false;

	sqlite3_stmt *m_stmt_read = nullptr;
	sqlite3_stmt *m_stmt_write = nullptr;
	sqlite3_stmt *m_stmt_list = nullptr;
	sqlite3_stmt *m_stmt_delete = nullptr;
};

/*
	Base of the classes that use the accounts database (accounts.sqlite): the
	accounts and everything about them, which includes the state of their players
	in each world. There can be several connections to the file, e.g. one for
	the accounts of the whole server and one for the players of each world.

	Schema (version 1)

	  worlds                      the worlds of the server, by world_id
	  accounts                    one row per account; current_world_id is the
	                              world the player is in, or last was in
	  privileges                  (account, privilege) pairs
	  world_players               the state of an account in a world: position
	                              and health; one row per (account, world)
	  world_player_metadata       key/value pairs of a world_players row
	  world_player_inventories    the inventory lists of a world_players row
	  world_player_inventory_items  the occupied slots of those lists

	Deleting an account deletes its privileges and its state in every world,
	deleting a world deletes the state of all players in it, and an account whose
	world is deleted is not in any world any more. All of that is done by the
	database itself (ON DELETE CASCADE / SET NULL), so there are no leftovers.
*/
class AccountsDatabase_SQLite3 : public Database_SQLite3
{
public:
	virtual ~AccountsDatabase_SQLite3();

	// Bumped with every change of the schema
	static constexpr int SCHEMA_VERSION = 1;

protected:
	AccountsDatabase_SQLite3(const std::string &savedir);

	bool schemaMissing() override;
	void createDatabase() override;

	// To be called by initStatements() of the derived classes
	void initAccountStatements();

	// The id of an account, 0 if there isn't one
	u64 getAccountId(const std::string &name);

	// Runs f() in a transaction. If it throws, the transaction is abandoned.
	template <typename F>
	void transaction(F &&f)
	{
		beginSave();
		try {
			f();
		} catch (...) {
			rollbackSave();
			throw;
		}
		endSave();
	}

private:
	int getSchemaVersion();

	sqlite3_stmt *m_stmt_account_id = nullptr;
};

class PlayerDatabaseSQLite3 : private AccountsDatabase_SQLite3, public PlayerDatabase
{
public:
	/// @param savedir directory of accounts.sqlite
	/// @param world_id the world whose players this is about, it must have been
	///        registered with AuthDatabase::registerWorld()
	PlayerDatabaseSQLite3(const std::string &savedir, u32 world_id);
	virtual ~PlayerDatabaseSQLite3();

	void savePlayer(RemotePlayer *player);
	bool loadPlayer(RemotePlayer *player, PlayerSAO *sao);
	bool removePlayer(const std::string &name);
	void listPlayers(std::vector<std::string> &res);

	PARENT_CLASS_FUNCS

protected:
	virtual void initStatements();

private:
	// Adds the account of a player that authenticates without one
	void provisionAccount(const std::string &name);

	const u32 m_world_id;

	sqlite3_stmt *m_stmt_account_add_locked = nullptr;
	sqlite3_stmt *m_stmt_player_load = nullptr;
	sqlite3_stmt *m_stmt_player_upsert = nullptr;
	sqlite3_stmt *m_stmt_player_id = nullptr;
	sqlite3_stmt *m_stmt_player_remove = nullptr;
	sqlite3_stmt *m_stmt_player_list = nullptr;
	sqlite3_stmt *m_stmt_inventory_load = nullptr;
	sqlite3_stmt *m_stmt_inventory_items_load = nullptr;
	sqlite3_stmt *m_stmt_inventory_add = nullptr;
	sqlite3_stmt *m_stmt_inventory_item_add = nullptr;
	sqlite3_stmt *m_stmt_inventory_remove = nullptr;
	sqlite3_stmt *m_stmt_metadata_load = nullptr;
	sqlite3_stmt *m_stmt_metadata_add = nullptr;
	sqlite3_stmt *m_stmt_metadata_remove = nullptr;
};

class AuthDatabaseSQLite3 : private AccountsDatabase_SQLite3, public AuthDatabase
{
public:
	/// @param savedir directory of accounts.sqlite
	AuthDatabaseSQLite3(const std::string &savedir);
	virtual ~AuthDatabaseSQLite3();

	virtual bool getAuth(const std::string &name, AuthEntry &res);
	virtual bool saveAuth(const AuthEntry &authEntry);
	virtual bool createAuth(AuthEntry &authEntry);
	virtual bool deleteAuth(const std::string &name);
	virtual void listNames(std::vector<std::string> &res);
	virtual void reload();

	virtual void registerWorld(u32 world_id, const std::string &name);
	virtual void deleteWorld(u32 world_id);
	virtual bool getCurrentWorld(const std::string &name, u32 *world_id);
	virtual void setCurrentWorld(const std::string &name, u32 world_id);

	PARENT_CLASS_FUNCS

protected:
	virtual void initStatements();

private:
	virtual void writePrivileges(const AuthEntry &authEntry);

	sqlite3_stmt *m_stmt_read = nullptr;
	sqlite3_stmt *m_stmt_write = nullptr;
	sqlite3_stmt *m_stmt_create = nullptr;
	sqlite3_stmt *m_stmt_delete = nullptr;
	sqlite3_stmt *m_stmt_list_names = nullptr;
	sqlite3_stmt *m_stmt_read_privs = nullptr;
	sqlite3_stmt *m_stmt_write_privs = nullptr;
	sqlite3_stmt *m_stmt_delete_privs = nullptr;
	sqlite3_stmt *m_stmt_world_register = nullptr;
	sqlite3_stmt *m_stmt_world_delete = nullptr;
	sqlite3_stmt *m_stmt_current_world_get = nullptr;
	sqlite3_stmt *m_stmt_current_world_set = nullptr;
};

class ModStorageDatabaseSQLite3 : private Database_SQLite3, public ModStorageDatabase
{
public:
	ModStorageDatabaseSQLite3(const std::string &savedir);
	virtual ~ModStorageDatabaseSQLite3();

	virtual void getModEntries(const std::string &modname, StringMap *storage);
	virtual void getModKeys(const std::string &modname, std::vector<std::string> *storage);
	virtual bool getModEntry(const std::string &modname,
		const std::string &key, std::string *value);
	virtual bool hasModEntry(const std::string &modname, const std::string &key);
	virtual bool setModEntry(const std::string &modname,
		const std::string &key,std::string_view value);
	virtual bool removeModEntry(const std::string &modname, const std::string &key);
	virtual bool removeModEntries(const std::string &modname);
	virtual void listMods(std::vector<std::string> *res);

	PARENT_CLASS_FUNCS

protected:
	virtual void createDatabase();
	virtual void initStatements();

private:
	sqlite3_stmt *m_stmt_get_mods = nullptr;
	sqlite3_stmt *m_stmt_get_all = nullptr;
	sqlite3_stmt *m_stmt_get_keys = nullptr;
	sqlite3_stmt *m_stmt_get = nullptr;
	sqlite3_stmt *m_stmt_has = nullptr;
	sqlite3_stmt *m_stmt_set = nullptr;
	sqlite3_stmt *m_stmt_remove = nullptr;
	sqlite3_stmt *m_stmt_remove_all = nullptr;
};

#undef PARENT_CLASS_FUNCS
