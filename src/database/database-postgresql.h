// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include <string>
#include <libpq-fe.h>
#include "database.h"
#include "exceptions.h"

// Template class for PostgreSQL based data storage
class Database_PostgreSQL : public Database
{
public:
	Database_PostgreSQL(const std::string &connect_string, const char *type);
	~Database_PostgreSQL();

	void beginSave() override;
	void endSave() override;
	void rollback();

	bool initialized() const override;

	void verifyDatabase() override;

protected:
	// Conversion helpers
	inline int pg_to_int(PGresult *res, int row, int col)
	{
		return atoi(PQgetvalue(res, row, col));
	}

	inline u32 pg_to_uint(PGresult *res, int row, int col)
	{
		return (u32) atoi(PQgetvalue(res, row, col));
	}

	inline float pg_to_float(PGresult *res, int row, int col)
	{
		return (float) atof(PQgetvalue(res, row, col));
	}

	inline v3s16 pg_to_v3s16(PGresult *res, int row, int col)
	{
		return v3s16(
			pg_to_int(res, row, col),
			pg_to_int(res, row, col + 1),
			pg_to_int(res, row, col + 2)
		);
	}

	inline std::string pg_to_string(PGresult *res, int row, int col)
	{
		return std::string(PQgetvalue(res, row, col), PQgetlength(res, row, col));
	}

	inline PGresult *execPrepared(const char *stmtName, const int paramsNumber,
		const void **params,
		const int *paramsLengths = NULL, const int *paramsFormats = NULL,
		bool clear = true, bool nobinary = true)
	{
		return checkResults(PQexecPrepared(m_conn, stmtName, paramsNumber,
			(const char* const*) params, paramsLengths, paramsFormats,
			nobinary ? 1 : 0), clear);
	}

	inline PGresult *execPrepared(const char *stmtName, const int paramsNumber,
		const char **params, bool clear = true, bool nobinary = true)
	{
		return execPrepared(stmtName, paramsNumber,
			(const void **)params, NULL, NULL, clear, nobinary);
	}

	void createTableIfNotExists(const std::string &table_name, const std::string &definition);

	// Runs SQL without parameters (it can be several statements)
	void execSimple(const char *sql);
	// Same, for a query. The caller has to PQclear() the result.
	PGresult *execQuery(const char *sql);

	// Database initialization
	void connectToDatabase();
	virtual void createDatabase() = 0;
	virtual void initStatements() = 0;
	inline void prepareStatement(const std::string &name, const std::string &sql)
	{
		checkResults(PQprepare(m_conn, name.c_str(), sql.c_str(), 0, NULL));
	}

	int getPGVersion() const { return m_pgversion; }

private:
	// Database connectivity checks
	void ping();

	// Database usage
	PGresult *checkResults(PGresult *res, bool clear = true);

	// Attributes
	std::string m_connect_string;
	PGconn *m_conn = nullptr;
	int m_pgversion = 0;
};

// Not sure why why we have to do this. can't C++ figure it out on its own?
#define PARENT_CLASS_FUNCS \
	void beginSave() { Database_PostgreSQL::beginSave(); } \
	void endSave() { Database_PostgreSQL::endSave(); } \
	void verifyDatabase() { Database_PostgreSQL::verifyDatabase(); }

class MapDatabasePostgreSQL : private Database_PostgreSQL, public MapDatabase
{
public:
	MapDatabasePostgreSQL(const std::string &connect_string);
	virtual ~MapDatabasePostgreSQL() = default;

	bool saveBlock(const v3s16 &pos, std::string_view data);
	void loadBlock(const v3s16 &pos, std::string *block);
	bool deleteBlock(const v3s16 &pos);
	void listAllLoadableBlocks(std::vector<v3s16> &dst);

	PARENT_CLASS_FUNCS

protected:
	virtual void createDatabase();
	virtual void initStatements();
};

/*
	Base of the classes that use the accounts database. The schema is the same
	as that of accounts.sqlite, see AccountsDatabase_SQLite3 in database-sqlite3.h.
*/
class AccountsDatabase_PostgreSQL : public Database_PostgreSQL
{
public:
	// Bumped with every change of the schema
	static constexpr int SCHEMA_VERSION = 1;

protected:
	AccountsDatabase_PostgreSQL(const std::string &connect_string);

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
			try {
				rollback();
			} catch (const DatabaseException &) {
			}
			throw;
		}
		endSave();
	}
};

class PlayerDatabasePostgreSQL : private AccountsDatabase_PostgreSQL, public PlayerDatabase
{
public:
	/// @param world_id the world whose players this is about, it must have been
	///        registered with AuthDatabase::registerWorld()
	PlayerDatabasePostgreSQL(const std::string &connect_string, u32 world_id);
	virtual ~PlayerDatabasePostgreSQL() = default;

	void savePlayer(RemotePlayer *player);
	bool loadPlayer(RemotePlayer *player, PlayerSAO *sao);
	bool removePlayer(const std::string &name);
	void listPlayers(std::vector<std::string> &res);

	PARENT_CLASS_FUNCS

protected:
	virtual void initStatements();

private:
	void provisionAccount(const std::string &name);

	const u32 m_world_id;
};

class AuthDatabasePostgreSQL : private AccountsDatabase_PostgreSQL, public AuthDatabase
{
public:
	AuthDatabasePostgreSQL(const std::string &connect_string);
	virtual ~AuthDatabasePostgreSQL() = default;

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
};

class ModStorageDatabasePostgreSQL : private Database_PostgreSQL, public ModStorageDatabase
{
public:
	ModStorageDatabasePostgreSQL(const std::string &connect_string);
	~ModStorageDatabasePostgreSQL() = default;

	void getModEntries(const std::string &modname, StringMap *storage);
	void getModKeys(const std::string &modname, std::vector<std::string> *storage);
	bool getModEntry(const std::string &modname, const std::string &key, std::string *value);
	bool hasModEntry(const std::string &modname, const std::string &key);
	bool setModEntry(const std::string &modname,
			const std::string &key, std::string_view value);
	bool removeModEntry(const std::string &modname, const std::string &key);
	bool removeModEntries(const std::string &modname);
	void listMods(std::vector<std::string> *res);

	PARENT_CLASS_FUNCS

protected:
	virtual void createDatabase();
	virtual void initStatements();
};

#undef PARENT_CLASS_FUNCS
