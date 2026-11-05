// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#include "database-sqlite3.h"

#include "log.h"
#include "filesys.h"
#include "exceptions.h"
#include "settings.h"
#include "porting.h"
#include "util/string.h"
#include "remoteplayer.h"
#include "irrlicht_changes/printing.h"
#include "server/player_sao.h"

#include <cassert>

// When to print messages when the database is being held locked by another process
// Note: occasional delays of over 250ms are seen while a map tool reads the database.
enum {
	BUSY_INFO_TRESHOLD    = 100,   // Print first informational message.
	BUSY_WARNING_TRESHOLD = 250,   // Print warning message. Significant lag.
	BUSY_FATAL_TRESHOLD   = 3000,  // Allow SQLITE_BUSY to be returned back to the caller.
	BUSY_ERROR_INTERVAL   = 10000, // Safety net: report again every 10 seconds
};

#define SQLRES(s, r, m) sqlite3_vrfy(s, m, r);
#define SQLOK(s, m) SQLRES(s, SQLITE_OK, m)
#define SQLSTEP(stmt, errmsg) sqlite3_vrfy_step(sqlite3_step(stmt), errmsg)

#define PREPARE_STATEMENT(name, query) \
	SQLOK(sqlite3_prepare_v2(m_database, query, -1, &m_stmt_##name, NULL), \
		std::string("Failed to prepare query \"").append(query).append("\""))

#define SQLOK_ERRSTREAM(s, m)                           \
	if ((s) != SQLITE_OK) {                             \
		errorstream << (m) << ": "                      \
			<< sqlite3_errmsg(m_database) << std::endl; \
	}

#define FINALIZE_STATEMENT(name) \
	sqlite3_finalize(m_stmt_##name); /* if this fails who cares */ \
	m_stmt_##name = nullptr;

int Database_SQLite3::busyHandler(void *data, int count)
{
	u64 &first_time = reinterpret_cast<u64*>(data)[0];
	u64 &prev_time = reinterpret_cast<u64*>(data)[1];
	u64 cur_time = porting::getTimeMs();

	if (count == 0) {
		first_time = cur_time;
		prev_time = first_time;
	}

	const auto total_diff = cur_time - first_time; // time since first call
	const auto this_diff = prev_time - first_time; // time since last call

	if (total_diff < BUSY_INFO_TRESHOLD) {
		// do nothing
	} else if (total_diff >= BUSY_INFO_TRESHOLD &&
			this_diff < BUSY_INFO_TRESHOLD) {
		infostream << "SQLite3 database has been locked for "
			<< total_diff << " ms." << std::endl;
	} else if (total_diff >= BUSY_WARNING_TRESHOLD &&
			this_diff < BUSY_WARNING_TRESHOLD) {
		warningstream << "SQLite3 database has been locked for "
			<< total_diff << " ms; this causes lag." << std::endl;
	} else if (total_diff >= BUSY_FATAL_TRESHOLD &&
			this_diff < BUSY_FATAL_TRESHOLD) {
		errorstream << "SQLite3 database has been locked for "
			<< total_diff << " ms - giving up!" << std::endl;
	} else if (total_diff / BUSY_ERROR_INTERVAL !=
			this_diff / BUSY_ERROR_INTERVAL) {
		// Safety net: keep reporting every BUSY_ERROR_INTERVAL
		errorstream << "SQLite3 database has been locked for "
			<< total_diff / 1000 << " seconds!" << std::endl;
	}

	prev_time = cur_time;

	if (total_diff >= BUSY_FATAL_TRESHOLD)
		return 0; // Make sqlite transaction fail

	// Retrying right away would only keep the CPU from whoever has the lock.
	// Back off, like SQLite's own busy timeout does: 1 ms at first, up to 20 ms.
	sqlite3_sleep(count < 10 ? 1 : (count < 50 ? 5 : 20));
	return 1;
}


Database_SQLite3::Database_SQLite3(const std::string &savedir, const std::string &dbname) :
	m_savedir(savedir),
	m_dbname(dbname)
{
}

void Database_SQLite3::beginSave()
{
	verifyDatabase();
	SQLRES(sqlite3_step(m_stmt_begin), SQLITE_DONE,
		"Failed to start SQLite3 transaction");
	sqlite3_reset(m_stmt_begin);
}

void Database_SQLite3::endSave()
{
	verifyDatabase();
	SQLRES(sqlite3_step(m_stmt_end), SQLITE_DONE,
		"Failed to commit SQLite3 transaction");
	sqlite3_reset(m_stmt_end);
}

void Database_SQLite3::rollbackSave() noexcept
{
	if (!m_database)
		return;
	// Fails if there is no transaction, which is fine. Statements that are
	// still running are aborted by this.
	sqlite3_exec(m_database, "ROLLBACK;", NULL, NULL, NULL);
}

void Database_SQLite3::openDatabase()
{
	if (m_database) return;

	std::string dbp = m_savedir + DIR_DELIM + m_dbname + ".sqlite";

	// Open the database connection

	if (!fs::CreateAllDirs(m_savedir)) {
		errorstream << "Database_SQLite3: Failed to create directory \""
			<< m_savedir << "\"" << std::endl;
		throw FileNotGoodException("Failed to create database "
				"save directory");
	}

	bool needs_create = !fs::PathExists(dbp);

	auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
#ifdef SQLITE_OPEN_EXRESCODE
	flags |= SQLITE_OPEN_EXRESCODE;
#endif
	SQLOK(sqlite3_open_v2(dbp.c_str(), &m_database, flags, NULL),
		std::string("Failed to open SQLite3 database file ") + dbp);

	SQLOK(sqlite3_busy_handler(m_database, Database_SQLite3::busyHandler,
		m_busy_handler_data), "Failed to set SQLite3 busy handler");

	if (needs_create || schemaMissing()) {
		createDatabase();
	}

	std::string query_str = std::string("PRAGMA synchronous = ")
			 + itos(g_settings->getU16("sqlite_synchronous"));
	SQLOK(sqlite3_exec(m_database, query_str.c_str(), NULL, NULL, NULL),
		"Failed to set SQLite3 synchronous mode");
	SQLOK(sqlite3_exec(m_database, "PRAGMA foreign_keys = ON", NULL, NULL, NULL),
		"Failed to enable SQLite3 foreign key support");
}

void Database_SQLite3::verifyDatabase()
{
	if (m_initialized) return;

	openDatabase();

	PREPARE_STATEMENT(begin, m_begin_sql);
	PREPARE_STATEMENT(end, "COMMIT;");

	initStatements();

	m_initialized = true;
}

bool Database_SQLite3::checkTable(const char *table)
{
	assert(m_database);

	// PRAGMA table_list would be cleaner here but it was only introduced in
	// sqlite 3.37.0 (2021-11-27).
	// So let's do this: https://stackoverflow.com/a/83195

	sqlite3_stmt *m_stmt_tmp = nullptr;
	PREPARE_STATEMENT(tmp, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;");
	str_to_sqlite(m_stmt_tmp, 1, table);

	bool ret = sqlite3_vrfy_step(sqlite3_step(m_stmt_tmp));

	FINALIZE_STATEMENT(tmp)
	return ret;
}

bool Database_SQLite3::checkColumn(const char *table, const char *column)
{
	assert(m_database);

	sqlite3_stmt *m_stmt_tmp = nullptr;
	auto query_str = std::string("PRAGMA table_info(").append(table).append(");");
	PREPARE_STATEMENT(tmp, query_str.c_str());

	bool ret = false;
	while (sqlite3_vrfy_step(sqlite3_step(m_stmt_tmp))) {
		ret |= sqlite_to_string_view(m_stmt_tmp, 1) == column;
		if (ret)
			break;
	}

	FINALIZE_STATEMENT(tmp)
	return ret;
}

Database_SQLite3::~Database_SQLite3()
{
	FINALIZE_STATEMENT(begin)
	FINALIZE_STATEMENT(end)

	SQLOK_ERRSTREAM(sqlite3_close(m_database), "Failed to close database");
}

/*
 * Map database
 */

MapDatabaseSQLite3::MapDatabaseSQLite3(const std::string &savedir):
	Database_SQLite3(savedir, "map"),
	MapDatabase()
{
}

MapDatabaseSQLite3::~MapDatabaseSQLite3()
{
	FINALIZE_STATEMENT(read)
	FINALIZE_STATEMENT(write)
	FINALIZE_STATEMENT(list)
	FINALIZE_STATEMENT(delete)
}


void MapDatabaseSQLite3::createDatabase()
{
	assert(m_database);

	// Note: before 5.12.0 the format was blocks(pos INT, data BLOB).
	// This function only runs for newly created databases.

	const char *schema =
		"CREATE TABLE IF NOT EXISTS `blocks` (\n"
			"`x` INTEGER,"
			"`y` INTEGER,"
			"`z` INTEGER,"
			"`data` BLOB NOT NULL,"
			// Declaring a primary key automatically creates an index and the
			// order largely dictates which range operations can be sped up.
			// see also: <https://www.sqlite.org/optoverview.html#skipscan>
			// Putting XZ before Y matches our MapSector abstraction.
			"PRIMARY KEY (`x`, `z`, `y`)"
		");\n"
	;
	SQLOK(sqlite3_exec(m_database, schema, NULL, NULL, NULL),
		"Failed to create database table");
}

void MapDatabaseSQLite3::initStatements()
{
	assert(checkTable("blocks"));
	m_new_format = checkColumn("blocks", "z");
	infostream << "MapDatabaseSQLite3: split column format = "
		<< (m_new_format ? "yes" : "no") << std::endl;

	if (m_new_format) {
		PREPARE_STATEMENT(read, "SELECT `data` FROM `blocks` WHERE `x` = ? AND `y` = ? AND `z` = ? LIMIT 1");
		PREPARE_STATEMENT(write, "REPLACE INTO `blocks` (`x`, `y`, `z`, `data`) VALUES (?, ?, ?, ?)");
		PREPARE_STATEMENT(delete, "DELETE FROM `blocks` WHERE `x` = ? AND `y` = ? AND `z` = ?");
		PREPARE_STATEMENT(list, "SELECT `x`, `y`, `z` FROM `blocks`");
	} else {
		PREPARE_STATEMENT(read, "SELECT `data` FROM `blocks` WHERE `pos` = ? LIMIT 1");
		PREPARE_STATEMENT(write, "REPLACE INTO `blocks` (`pos`, `data`) VALUES (?, ?)");
		PREPARE_STATEMENT(delete, "DELETE FROM `blocks` WHERE `pos` = ?");
		PREPARE_STATEMENT(list, "SELECT `pos` FROM `blocks`");
	}
}

inline int MapDatabaseSQLite3::bindPos(sqlite3_stmt *stmt, v3s16 pos, int index)
{
	if (m_new_format) {
		int_to_sqlite(stmt, index, pos.X);
		int_to_sqlite(stmt, index + 1, pos.Y);
		int_to_sqlite(stmt, index + 2, pos.Z);
		return index + 3;
	} else {
		int64_to_sqlite(stmt, index, getBlockAsInteger(pos));
		return index + 1;
	}
}

bool MapDatabaseSQLite3::deleteBlock(const v3s16 &pos)
{
	verifyDatabase();

	bindPos(m_stmt_delete, pos);

	bool good = sqlite3_step(m_stmt_delete) == SQLITE_DONE;
	sqlite3_reset(m_stmt_delete);

	if (!good) {
		warningstream << "deleteBlock: Failed to delete block "
			<< pos << ": " << sqlite3_errmsg(m_database) << std::endl;
	}
	return good;
}

bool MapDatabaseSQLite3::saveBlock(const v3s16 &pos, std::string_view data)
{
	verifyDatabase();

	int col = bindPos(m_stmt_write, pos);
	blob_to_sqlite(m_stmt_write, col, data);

	SQLRES(sqlite3_step(m_stmt_write), SQLITE_DONE, "Failed to save block")
	sqlite3_reset(m_stmt_write);

	return true;
}

void MapDatabaseSQLite3::loadBlock(const v3s16 &pos, std::string *block)
{
	verifyDatabase();

	bindPos(m_stmt_read, pos);
	if (!SQLSTEP(m_stmt_read, "Failed to load block")) {
		block->clear();
		sqlite3_reset(m_stmt_read);
		return;
	}

	auto data = sqlite_to_blob(m_stmt_read, 0);
	block->assign(data);

	// We should never get more than 1 row, so ok to reset
	sqlite3_reset(m_stmt_read);
}

void MapDatabaseSQLite3::listAllLoadableBlocks(std::vector<v3s16> &dst)
{
	verifyDatabase();

	v3s16 p;
	while (SQLSTEP(m_stmt_list, "Failed to list blocks")) {
		if (m_new_format) {
			p.X = sqlite_to_int(m_stmt_list, 0);
			p.Y = sqlite_to_int(m_stmt_list, 1);
			p.Z = sqlite_to_int(m_stmt_list, 2);
		} else {
			p = getIntegerAsBlock(sqlite_to_int64(m_stmt_list, 0));
		}
		dst.push_back(p);
	}

	sqlite3_reset(m_stmt_list);
}

/*
 * Accounts database (accounts.sqlite)
 */

namespace {

// Everything below is executed in one transaction when the database is created.
// SQLite only has five storage classes and ignores sizes like VARCHAR(32), so
// the types are the ones SQLite really stores. Times are seconds since the Unix
// epoch (UTC), in INTEGER columns.
const char *const ACCOUNTS_SCHEMA = R"SQL(
CREATE TABLE `worlds` (
	-- The world_id of the world (world.conf). The name is only to help whoever
	-- reads the database: the name of a world can change, and a name can be used
	-- again by another world later, the id identifies.
	`id`   INTEGER PRIMARY KEY,
	`name` TEXT NOT NULL
);

CREATE TABLE `accounts` (
	-- Not reused after an account is deleted
	`id`               INTEGER PRIMARY KEY AUTOINCREMENT,
	`name`             TEXT    NOT NULL UNIQUE CHECK (`name` <> ''),
	-- Verifier, as made by the authentication handler. Opaque to the database:
	-- it can be empty (no password), or ACCOUNT_LOCKED_PASSWORD.
	`password`         TEXT    NOT NULL,
	`created_at`       INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
	-- NULL: has never logged in
	`last_login`       INTEGER CHECK (`last_login` IS NULL OR `last_login` > 0),
	-- The world the player is in, or last was in. NULL: none yet (or the world
	-- is gone), the player starts in the default world.
	`current_world_id` INTEGER REFERENCES `worlds` (`id`) ON DELETE SET NULL
);
CREATE INDEX `accounts_current_world_id` ON `accounts` (`current_world_id`);

CREATE TABLE `privileges` (
	`account_id` INTEGER NOT NULL REFERENCES `accounts` (`id`) ON DELETE CASCADE,
	-- Privileges are defined by mods at run time, so they are not a table of
	-- their own: one that is not defined (any more) is still kept.
	`privilege`  TEXT    NOT NULL CHECK (`privilege` <> ''),
	PRIMARY KEY (`account_id`, `privilege`)
) WITHOUT ROWID;

CREATE TABLE `world_players` (
	-- Surrogate key, so that the tables below don't have to repeat
	-- (account, world), let alone a name, in every row.
	`id`         INTEGER PRIMARY KEY,
	`account_id` INTEGER NOT NULL REFERENCES `accounts` (`id`) ON DELETE CASCADE,
	`world_id`   INTEGER NOT NULL REFERENCES `worlds` (`id`)   ON DELETE CASCADE,
	`pitch`      REAL    NOT NULL,
	`yaw`        REAL    NOT NULL,
	`pos_x`      REAL    NOT NULL,
	`pos_y`      REAL    NOT NULL,
	`pos_z`      REAL    NOT NULL,
	`hp`         INTEGER NOT NULL CHECK (`hp`     BETWEEN 0 AND 65535),
	`breath`     INTEGER NOT NULL CHECK (`breath` BETWEEN 0 AND 65535),
	`created_at` INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
	`updated_at` INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
	UNIQUE (`account_id`, `world_id`)
);
CREATE INDEX `world_players_world_id` ON `world_players` (`world_id`);

-- Keys and values are arbitrary byte strings (Lua strings), hence BLOB: as TEXT
-- a value with a NUL byte in it can't be read back.
CREATE TABLE `world_player_metadata` (
	`world_player_id` INTEGER NOT NULL
		REFERENCES `world_players` (`id`) ON DELETE CASCADE,
	`key`             BLOB    NOT NULL,
	`value`           BLOB    NOT NULL,
	PRIMARY KEY (`world_player_id`, `key`)
) WITHOUT ROWID;

CREATE TABLE `world_player_inventories` (
	`world_player_id` INTEGER NOT NULL
		REFERENCES `world_players` (`id`) ON DELETE CASCADE,
	-- Position of the list in the inventory
	`list_index`      INTEGER NOT NULL CHECK (`list_index` >= 0),
	`name`            TEXT    NOT NULL,
	`width`           INTEGER NOT NULL CHECK (`width` >= 0),
	`size`            INTEGER NOT NULL CHECK (`size`  >= 0),
	PRIMARY KEY (`world_player_id`, `list_index`),
	UNIQUE (`world_player_id`, `name`)
) WITHOUT ROWID;

-- Only the slots that hold something. A slot that has no row is empty.
CREATE TABLE `world_player_inventory_items` (
	`world_player_id` INTEGER NOT NULL,
	`list_index`      INTEGER NOT NULL,
	`slot`            INTEGER NOT NULL CHECK (`slot` >= 0),
	-- A serialized ItemStack
	`item`            TEXT    NOT NULL CHECK (`item` <> ''),
	PRIMARY KEY (`world_player_id`, `list_index`, `slot`),
	FOREIGN KEY (`world_player_id`, `list_index`)
		REFERENCES `world_player_inventories` (`world_player_id`, `list_index`)
		ON DELETE CASCADE
) WITHOUT ROWID;
)SQL";

} // namespace

AccountsDatabase_SQLite3::AccountsDatabase_SQLite3(const std::string &savedir) :
	Database_SQLite3(savedir, "accounts")
{
	m_begin_sql = "BEGIN IMMEDIATE;";
}

AccountsDatabase_SQLite3::~AccountsDatabase_SQLite3()
{
	FINALIZE_STATEMENT(account_id)
}

int AccountsDatabase_SQLite3::getSchemaVersion()
{
	sqlite3_stmt *m_stmt_tmp = nullptr;
	PREPARE_STATEMENT(tmp, "PRAGMA user_version;");
	int version = 0;
	try {
		if (sqlite3_vrfy_step(sqlite3_step(m_stmt_tmp), "Failed to read the schema version"))
			version = sqlite_to_int(m_stmt_tmp, 0);
	} catch (...) {
		FINALIZE_STATEMENT(tmp)
		throw;
	}
	FINALIZE_STATEMENT(tmp)
	return version;
}

bool AccountsDatabase_SQLite3::schemaMissing()
{
	const int version = getSchemaVersion();
	if (version > SCHEMA_VERSION) {
		throw DatabaseException("The accounts database has schema version " +
			itos(version) + ", this version only knows up to " +
			itos(SCHEMA_VERSION) + ". It was made by a newer version.");
	}
	return version == 0;
}

void AccountsDatabase_SQLite3::createDatabase()
{
	assert(m_database);

	// IMMEDIATE: if another connection is doing the same, this waits for it.
	SQLOK(sqlite3_exec(m_database, "BEGIN IMMEDIATE;", NULL, NULL, NULL),
		"Failed to start creating the accounts database");
	try {
		// Whoever had the lock before may have created it
		if (getSchemaVersion() == 0) {
			SQLOK(sqlite3_exec(m_database, ACCOUNTS_SCHEMA, NULL, NULL, NULL),
				"Failed to create the accounts database tables");
			const std::string pragma = "PRAGMA user_version = " +
				itos(SCHEMA_VERSION) + ";";
			SQLOK(sqlite3_exec(m_database, pragma.c_str(), NULL, NULL, NULL),
				"Failed to set the schema version");
		}
		SQLOK(sqlite3_exec(m_database, "COMMIT;", NULL, NULL, NULL),
			"Failed to commit the accounts database tables");
	} catch (...) {
		rollbackSave();
		throw;
	}
}

void AccountsDatabase_SQLite3::initAccountStatements()
{
	PREPARE_STATEMENT(account_id, "SELECT `id` FROM `accounts` WHERE `name` = ?");
}

u64 AccountsDatabase_SQLite3::getAccountId(const std::string &name)
{
	str_to_sqlite(m_stmt_account_id, 1, name);
	u64 id = 0;
	if (SQLSTEP(m_stmt_account_id, "Failed to look up account"))
		id = sqlite_to_uint64(m_stmt_account_id, 0);
	sqlite3_reset(m_stmt_account_id);
	return id;
}

/*
 * Player Database
 */

PlayerDatabaseSQLite3::PlayerDatabaseSQLite3(const std::string &savedir, u32 world_id):
	AccountsDatabase_SQLite3(savedir),
	PlayerDatabase(),
	m_world_id(world_id)
{
}

PlayerDatabaseSQLite3::~PlayerDatabaseSQLite3()
{
	FINALIZE_STATEMENT(account_add_locked)
	FINALIZE_STATEMENT(player_load)
	FINALIZE_STATEMENT(player_upsert)
	FINALIZE_STATEMENT(player_id)
	FINALIZE_STATEMENT(player_remove)
	FINALIZE_STATEMENT(player_list)
	FINALIZE_STATEMENT(inventory_load)
	FINALIZE_STATEMENT(inventory_items_load)
	FINALIZE_STATEMENT(inventory_add)
	FINALIZE_STATEMENT(inventory_item_add)
	FINALIZE_STATEMENT(inventory_remove)
	FINALIZE_STATEMENT(metadata_load)
	FINALIZE_STATEMENT(metadata_add)
	FINALIZE_STATEMENT(metadata_remove)
}

void PlayerDatabaseSQLite3::initStatements()
{
	initAccountStatements();

	PREPARE_STATEMENT(account_add_locked,
		"INSERT OR IGNORE INTO `accounts` (`name`, `password`) VALUES (?, ?)");

	PREPARE_STATEMENT(player_load,
		"SELECT wp.`id`, wp.`pitch`, wp.`yaw`, wp.`pos_x`, wp.`pos_y`, wp.`pos_z`, "
		"wp.`hp`, wp.`breath` FROM `world_players` wp "
		"JOIN `accounts` a ON a.`id` = wp.`account_id` "
		"WHERE a.`name` = ? AND wp.`world_id` = ?");
	PREPARE_STATEMENT(player_upsert,
		"INSERT INTO `world_players` (`account_id`, `world_id`, `pitch`, `yaw`, "
		"`pos_x`, `pos_y`, `pos_z`, `hp`, `breath`) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
		"ON CONFLICT (`account_id`, `world_id`) DO UPDATE SET "
		"`pitch` = excluded.`pitch`, `yaw` = excluded.`yaw`, "
		"`pos_x` = excluded.`pos_x`, `pos_y` = excluded.`pos_y`, "
		"`pos_z` = excluded.`pos_z`, `hp` = excluded.`hp`, "
		"`breath` = excluded.`breath`, `updated_at` = strftime('%s', 'now')");
	PREPARE_STATEMENT(player_id,
		"SELECT `id` FROM `world_players` WHERE `account_id` = ? AND `world_id` = ?");
	PREPARE_STATEMENT(player_remove,
		"DELETE FROM `world_players` WHERE `world_id` = ? AND `account_id` = "
		"(SELECT `id` FROM `accounts` WHERE `name` = ?)");
	PREPARE_STATEMENT(player_list,
		"SELECT a.`name` FROM `world_players` wp "
		"JOIN `accounts` a ON a.`id` = wp.`account_id` "
		"WHERE wp.`world_id` = ? ORDER BY a.`name`");

	PREPARE_STATEMENT(inventory_load,
		"SELECT `list_index`, `width`, `name`, `size` FROM `world_player_inventories` "
		"WHERE `world_player_id` = ? ORDER BY `list_index`");
	PREPARE_STATEMENT(inventory_items_load,
		"SELECT `slot`, `item` FROM `world_player_inventory_items` "
		"WHERE `world_player_id` = ? AND `list_index` = ?");
	PREPARE_STATEMENT(inventory_add,
		"INSERT INTO `world_player_inventories` "
		"(`world_player_id`, `list_index`, `width`, `name`, `size`) VALUES (?, ?, ?, ?, ?)");
	PREPARE_STATEMENT(inventory_item_add,
		"INSERT INTO `world_player_inventory_items` "
		"(`world_player_id`, `list_index`, `slot`, `item`) VALUES (?, ?, ?, ?)");
	// The items go with it (ON DELETE CASCADE)
	PREPARE_STATEMENT(inventory_remove,
		"DELETE FROM `world_player_inventories` WHERE `world_player_id` = ?");

	PREPARE_STATEMENT(metadata_load,
		"SELECT `key`, `value` FROM `world_player_metadata` WHERE `world_player_id` = ?");
	PREPARE_STATEMENT(metadata_add,
		"INSERT INTO `world_player_metadata` (`world_player_id`, `key`, `value`) "
		"VALUES (?, ?, ?)");
	PREPARE_STATEMENT(metadata_remove,
		"DELETE FROM `world_player_metadata` WHERE `world_player_id` = ?");
}

void PlayerDatabaseSQLite3::provisionAccount(const std::string &name)
{
	if (getAccountId(name) != 0)
		return;
	// Written in a transaction of its own, the caller is only reading
	transaction([&] {
		str_to_sqlite(m_stmt_account_add_locked, 1, name);
		str_to_sqlite(m_stmt_account_add_locked, 2, ACCOUNT_LOCKED_PASSWORD);
		sqlite3_vrfy(sqlite3_step(m_stmt_account_add_locked), SQLITE_DONE);
		sqlite3_reset(m_stmt_account_add_locked);
	});
	infostream << "PlayerDatabaseSQLite3: added a locked account for \"" << name
		<< "\", which has none" << std::endl;
}

void PlayerDatabaseSQLite3::savePlayer(RemotePlayer *player)
{
	PlayerSAO *sao = player->getPlayerSAO();
	sanity_check(sao);

	verifyDatabase();

	const std::string &name = player->getName();
	const v3f &pos = sao->getBasePosition();

	bool saved = true;
	transaction([&] {
		const u64 account_id = getAccountId(name);
		if (account_id == 0) {
			// The account was deleted while the player was online. Don't bring it back.
			warningstream << "PlayerDatabaseSQLite3: \"" << name << "\" has no account, "
				"not saving their state" << std::endl;
			saved = false;
			return;
		}

		int64_to_sqlite(m_stmt_player_upsert, 1, account_id);
		int64_to_sqlite(m_stmt_player_upsert, 2, m_world_id);
		double_to_sqlite(m_stmt_player_upsert, 3, sao->getLookPitch());
		double_to_sqlite(m_stmt_player_upsert, 4, sao->getRotation().Y);
		double_to_sqlite(m_stmt_player_upsert, 5, pos.X);
		double_to_sqlite(m_stmt_player_upsert, 6, pos.Y);
		double_to_sqlite(m_stmt_player_upsert, 7, pos.Z);
		int64_to_sqlite(m_stmt_player_upsert, 8, sao->getHP());
		int64_to_sqlite(m_stmt_player_upsert, 9, sao->getBreath());
		sqlite3_vrfy(sqlite3_step(m_stmt_player_upsert), SQLITE_DONE);
		sqlite3_reset(m_stmt_player_upsert);

		int64_to_sqlite(m_stmt_player_id, 1, account_id);
		int64_to_sqlite(m_stmt_player_id, 2, m_world_id);
		if (!SQLSTEP(m_stmt_player_id, "Failed to look up player"))
			throw DatabaseException("Player disappeared while saving");
		const s64 pid = sqlite_to_int64(m_stmt_player_id, 0);
		sqlite3_reset(m_stmt_player_id);

		// Inventory and metadata are written anew
		int64_to_sqlite(m_stmt_inventory_remove, 1, pid);
		sqlite3_vrfy(sqlite3_step(m_stmt_inventory_remove), SQLITE_DONE);
		sqlite3_reset(m_stmt_inventory_remove);

		int64_to_sqlite(m_stmt_metadata_remove, 1, pid);
		sqlite3_vrfy(sqlite3_step(m_stmt_metadata_remove), SQLITE_DONE);
		sqlite3_reset(m_stmt_metadata_remove);

		const auto &inventory_lists = player->inventory.getLists();
		std::ostringstream oss;
		for (u16 i = 0; i < inventory_lists.size(); i++) {
			const InventoryList *list = inventory_lists[i];

			int64_to_sqlite(m_stmt_inventory_add, 1, pid);
			int_to_sqlite(m_stmt_inventory_add, 2, i);
			int_to_sqlite(m_stmt_inventory_add, 3, list->getWidth());
			str_to_sqlite(m_stmt_inventory_add, 4, list->getName());
			int_to_sqlite(m_stmt_inventory_add, 5, list->getSize());
			sqlite3_vrfy(sqlite3_step(m_stmt_inventory_add), SQLITE_DONE);
			sqlite3_reset(m_stmt_inventory_add);

			for (u32 j = 0; j < list->getSize(); j++) {
				const ItemStack &stack = list->getItem(j);
				if (stack.empty())
					continue;
				oss.str("");
				oss.clear();
				stack.serialize(oss);

				int64_to_sqlite(m_stmt_inventory_item_add, 1, pid);
				int_to_sqlite(m_stmt_inventory_item_add, 2, i);
				int_to_sqlite(m_stmt_inventory_item_add, 3, j);
				str_to_sqlite(m_stmt_inventory_item_add, 4, oss.str());
				sqlite3_vrfy(sqlite3_step(m_stmt_inventory_item_add), SQLITE_DONE);
				sqlite3_reset(m_stmt_inventory_item_add);
			}
		}

		const StringMap &attrs = sao->getMeta().getStrings();
		for (const auto &attr : attrs) {
			int64_to_sqlite(m_stmt_metadata_add, 1, pid);
			blob_to_sqlite(m_stmt_metadata_add, 2, attr.first);
			blob_to_sqlite(m_stmt_metadata_add, 3, attr.second);
			sqlite3_vrfy(sqlite3_step(m_stmt_metadata_add), SQLITE_DONE);
			sqlite3_reset(m_stmt_metadata_add);
		}
	});

	// Also when it was not saved: there is nothing to keep trying
	(void)saved;
	player->onSuccessfulSave();
}

bool PlayerDatabaseSQLite3::loadPlayer(RemotePlayer *player, PlayerSAO *sao)
{
	verifyDatabase();

	const std::string &name = player->getName();
	provisionAccount(name);

	str_to_sqlite(m_stmt_player_load, 1, name);
	int64_to_sqlite(m_stmt_player_load, 2, m_world_id);
	if (!SQLSTEP(m_stmt_player_load, "Failed to load player")) {
		sqlite3_reset(m_stmt_player_load);
		return false;
	}

	const s64 pid = sqlite_to_int64(m_stmt_player_load, 0);
	sao->setLookPitch(sqlite_to_float(m_stmt_player_load, 1));
	sao->setPlayerYaw(sqlite_to_float(m_stmt_player_load, 2));
	sao->setBasePosition(sqlite_to_v3f(m_stmt_player_load, 3));
	sao->setHPRaw((u16) MYMIN(sqlite_to_int(m_stmt_player_load, 6), U16_MAX));
	sao->setBreath((u16) MYMIN(sqlite_to_int(m_stmt_player_load, 7), U16_MAX), false);
	sqlite3_reset(m_stmt_player_load);

	// Load inventory
	int64_to_sqlite(m_stmt_inventory_load, 1, pid);
	while (SQLSTEP(m_stmt_inventory_load, "Failed to load inventory")) {
		const u32 list_index = sqlite_to_uint(m_stmt_inventory_load, 0);
		InventoryList *invList = player->inventory.addList(
			sqlite_to_string(m_stmt_inventory_load, 2),
			sqlite_to_uint(m_stmt_inventory_load, 3));
		invList->setWidth(sqlite_to_uint(m_stmt_inventory_load, 1));

		int64_to_sqlite(m_stmt_inventory_items_load, 1, pid);
		int_to_sqlite(m_stmt_inventory_items_load, 2, list_index);
		while (SQLSTEP(m_stmt_inventory_items_load, "Failed to load inventory items")) {
			ItemStack stack;
			stack.deSerialize(sqlite_to_string(m_stmt_inventory_items_load, 1));
			invList->changeItem(sqlite_to_uint(m_stmt_inventory_items_load, 0), stack);
		}
		sqlite3_reset(m_stmt_inventory_items_load);
	}
	sqlite3_reset(m_stmt_inventory_load);

	int64_to_sqlite(m_stmt_metadata_load, 1, pid);
	while (SQLSTEP(m_stmt_metadata_load, "Failed to load player metadata")) {
		const std::string attr(sqlite_to_blob(m_stmt_metadata_load, 0));
		sao->getMeta().setString(attr, sqlite_to_blob(m_stmt_metadata_load, 1));
	}
	sao->getMeta().setModified(false);
	sqlite3_reset(m_stmt_metadata_load);
	return true;
}

bool PlayerDatabaseSQLite3::removePlayer(const std::string &name)
{
	verifyDatabase();

	bool removed = false;
	transaction([&] {
		int64_to_sqlite(m_stmt_player_remove, 1, m_world_id);
		str_to_sqlite(m_stmt_player_remove, 2, name);
		sqlite3_vrfy(sqlite3_step(m_stmt_player_remove), SQLITE_DONE);
		removed = sqlite3_changes(m_database) > 0;
		sqlite3_reset(m_stmt_player_remove);
	});
	return removed;
}

void PlayerDatabaseSQLite3::listPlayers(std::vector<std::string> &res)
{
	verifyDatabase();

	int64_to_sqlite(m_stmt_player_list, 1, m_world_id);
	while (SQLSTEP(m_stmt_player_list, "Failed to list players"))
		res.emplace_back(sqlite_to_string_view(m_stmt_player_list, 0));

	sqlite3_reset(m_stmt_player_list);
}

/*
 * Auth database
 */

AuthDatabaseSQLite3::AuthDatabaseSQLite3(const std::string &savedir) :
		AccountsDatabase_SQLite3(savedir), AuthDatabase()
{
}

AuthDatabaseSQLite3::~AuthDatabaseSQLite3()
{
	FINALIZE_STATEMENT(read)
	FINALIZE_STATEMENT(write)
	FINALIZE_STATEMENT(create)
	FINALIZE_STATEMENT(delete)
	FINALIZE_STATEMENT(list_names)
	FINALIZE_STATEMENT(read_privs)
	FINALIZE_STATEMENT(write_privs)
	FINALIZE_STATEMENT(delete_privs)
	FINALIZE_STATEMENT(world_register)
	FINALIZE_STATEMENT(world_delete)
	FINALIZE_STATEMENT(current_world_get)
	FINALIZE_STATEMENT(current_world_set)
}

void AuthDatabaseSQLite3::initStatements()
{
	initAccountStatements();

	// last_login: 0 and below (the Lua side uses -1 for "not yet") is "never"
	PREPARE_STATEMENT(read, "SELECT `id`, `name`, `password`, `last_login` "
		"FROM `accounts` WHERE `name` = ?");
	PREPARE_STATEMENT(write, "UPDATE `accounts` SET `name` = ?, `password` = ?, "
		"`last_login` = NULLIF(MAX(?, 0), 0) WHERE `id` = ?");
	PREPARE_STATEMENT(create, "INSERT INTO `accounts` (`name`, `password`, `last_login`) "
		"VALUES (?, ?, NULLIF(MAX(?, 0), 0))");
	// Privileges and the state in the worlds go with it (ON DELETE CASCADE)
	PREPARE_STATEMENT(delete, "DELETE FROM `accounts` WHERE `name` = ?");

	PREPARE_STATEMENT(list_names, "SELECT `name` FROM `accounts` ORDER BY `name` DESC");

	PREPARE_STATEMENT(read_privs, "SELECT `privilege` FROM `privileges` WHERE `account_id` = ?");
	PREPARE_STATEMENT(write_privs, "INSERT OR IGNORE INTO `privileges` "
		"(`account_id`, `privilege`) VALUES (?, ?)");
	PREPARE_STATEMENT(delete_privs, "DELETE FROM `privileges` WHERE `account_id` = ?");

	PREPARE_STATEMENT(world_register, "INSERT INTO `worlds` (`id`, `name`) VALUES (?, ?) "
		"ON CONFLICT (`id`) DO UPDATE SET `name` = excluded.`name`");
	PREPARE_STATEMENT(world_delete, "DELETE FROM `worlds` WHERE `id` = ?");
	PREPARE_STATEMENT(current_world_get,
		"SELECT `current_world_id` FROM `accounts` WHERE `name` = ?");
	// IS NOT: also true when it is NULL. No write if nothing changes.
	PREPARE_STATEMENT(current_world_set, "UPDATE `accounts` SET `current_world_id` = ? "
		"WHERE `name` = ? AND `current_world_id` IS NOT ?");
}

bool AuthDatabaseSQLite3::getAuth(const std::string &name, AuthEntry &res)
{
	verifyDatabase();
	str_to_sqlite(m_stmt_read, 1, name);
	if (!SQLSTEP(m_stmt_read, "Failed to get auth")) {
		sqlite3_reset(m_stmt_read);
		return false;
	}

	res.id = sqlite_to_uint64(m_stmt_read, 0);
	res.name = sqlite_to_string_view(m_stmt_read, 1);
	res.password = sqlite_to_string_view(m_stmt_read, 2);
	res.last_login = sqlite_to_int64(m_stmt_read, 3); // NULL is 0
	sqlite3_reset(m_stmt_read);

	res.privileges.clear();
	int64_to_sqlite(m_stmt_read_privs, 1, res.id);
	while (SQLSTEP(m_stmt_read_privs, "")) {
		res.privileges.emplace_back(sqlite_to_string_view(m_stmt_read_privs, 0));
	}
	sqlite3_reset(m_stmt_read_privs);

	return true;
}

bool AuthDatabaseSQLite3::saveAuth(const AuthEntry &authEntry)
{
	transaction([&] {
		str_to_sqlite(m_stmt_write, 1, authEntry.name);
		str_to_sqlite(m_stmt_write, 2, authEntry.password);
		int64_to_sqlite(m_stmt_write, 3, authEntry.last_login);
		int64_to_sqlite(m_stmt_write, 4, authEntry.id);
		sqlite3_vrfy(sqlite3_step(m_stmt_write), SQLITE_DONE);
		sqlite3_reset(m_stmt_write);

		writePrivileges(authEntry);
	});
	return true;
}

bool AuthDatabaseSQLite3::createAuth(AuthEntry &authEntry)
{
	transaction([&] {
		// id autoincrements
		str_to_sqlite(m_stmt_create, 1, authEntry.name);
		str_to_sqlite(m_stmt_create, 2, authEntry.password);
		int64_to_sqlite(m_stmt_create, 3, authEntry.last_login);
		sqlite3_vrfy(sqlite3_step(m_stmt_create), SQLITE_DONE);
		sqlite3_reset(m_stmt_create);

		// obtain id and write back to original authEntry
		authEntry.id = (u64) sqlite3_last_insert_rowid(m_database);

		writePrivileges(authEntry);
	});
	return true;
}

bool AuthDatabaseSQLite3::deleteAuth(const std::string &name)
{
	verifyDatabase();

	int changes = 0;
	transaction([&] {
		str_to_sqlite(m_stmt_delete, 1, name);
		sqlite3_vrfy(sqlite3_step(m_stmt_delete), SQLITE_DONE);
		changes = sqlite3_changes(m_database);
		sqlite3_reset(m_stmt_delete);
	});

	return changes > 0;
}

void AuthDatabaseSQLite3::listNames(std::vector<std::string> &res)
{
	verifyDatabase();

	while (SQLSTEP(m_stmt_list_names, "Failed to list names")) {
		res.emplace_back(sqlite_to_string_view(m_stmt_list_names, 0));
	}
	sqlite3_reset(m_stmt_list_names);
}

void AuthDatabaseSQLite3::reload()
{
	// noop for SQLite
}

void AuthDatabaseSQLite3::writePrivileges(const AuthEntry &authEntry)
{
	int64_to_sqlite(m_stmt_delete_privs, 1, authEntry.id);
	sqlite3_vrfy(sqlite3_step(m_stmt_delete_privs), SQLITE_DONE);
	sqlite3_reset(m_stmt_delete_privs);
	for (const std::string &privilege : authEntry.privileges) {
		if (privilege.empty())
			continue; // not a privilege
		int64_to_sqlite(m_stmt_write_privs, 1, authEntry.id);
		str_to_sqlite(m_stmt_write_privs, 2, privilege);
		sqlite3_vrfy(sqlite3_step(m_stmt_write_privs), SQLITE_DONE);
		sqlite3_reset(m_stmt_write_privs);
	}
}

void AuthDatabaseSQLite3::registerWorld(u32 world_id, const std::string &name)
{
	transaction([&] {
		int64_to_sqlite(m_stmt_world_register, 1, world_id);
		str_to_sqlite(m_stmt_world_register, 2, name);
		sqlite3_vrfy(sqlite3_step(m_stmt_world_register), SQLITE_DONE);
		sqlite3_reset(m_stmt_world_register);
	});
}

void AuthDatabaseSQLite3::deleteWorld(u32 world_id)
{
	transaction([&] {
		int64_to_sqlite(m_stmt_world_delete, 1, world_id);
		sqlite3_vrfy(sqlite3_step(m_stmt_world_delete), SQLITE_DONE);
		sqlite3_reset(m_stmt_world_delete);
	});
}

bool AuthDatabaseSQLite3::getCurrentWorld(const std::string &name, u32 *world_id)
{
	verifyDatabase();

	str_to_sqlite(m_stmt_current_world_get, 1, name);
	bool found = false;
	if (SQLSTEP(m_stmt_current_world_get, "Failed to get the world of a player") &&
			sqlite3_column_type(m_stmt_current_world_get, 0) != SQLITE_NULL) {
		*world_id = sqlite_to_uint(m_stmt_current_world_get, 0);
		found = true;
	}
	sqlite3_reset(m_stmt_current_world_get);
	return found;
}

void AuthDatabaseSQLite3::setCurrentWorld(const std::string &name, u32 world_id)
{
	transaction([&] {
		int64_to_sqlite(m_stmt_current_world_set, 1, world_id);
		str_to_sqlite(m_stmt_current_world_set, 2, name);
		int64_to_sqlite(m_stmt_current_world_set, 3, world_id);
		sqlite3_vrfy(sqlite3_step(m_stmt_current_world_set), SQLITE_DONE);
		sqlite3_reset(m_stmt_current_world_set);
	});
}

ModStorageDatabaseSQLite3::ModStorageDatabaseSQLite3(const std::string &savedir):
	Database_SQLite3(savedir, "mod_storage"), ModStorageDatabase()
{
}

ModStorageDatabaseSQLite3::~ModStorageDatabaseSQLite3()
{
	FINALIZE_STATEMENT(remove_all)
	FINALIZE_STATEMENT(remove)
	FINALIZE_STATEMENT(set)
	FINALIZE_STATEMENT(has)
	FINALIZE_STATEMENT(get)
	FINALIZE_STATEMENT(get_keys)
	FINALIZE_STATEMENT(get_all)
	FINALIZE_STATEMENT(get_mods)
}

void ModStorageDatabaseSQLite3::createDatabase()
{
	assert(m_database);

	SQLOK(sqlite3_exec(m_database,
		"CREATE TABLE IF NOT EXISTS `entries` (\n"
			"	`modname` TEXT NOT NULL,\n"
			"	`key` BLOB NOT NULL,\n"
			"	`value` BLOB NOT NULL,\n"
			"	PRIMARY KEY (`modname`, `key`)\n"
			");\n",
		NULL, NULL, NULL),
		"Failed to create database table");
}

void ModStorageDatabaseSQLite3::initStatements()
{
	PREPARE_STATEMENT(get_mods, "SELECT DISTINCT `modname` FROM `entries`");
	PREPARE_STATEMENT(get_all, "SELECT `key`, `value` FROM `entries` WHERE `modname` = ?");
	PREPARE_STATEMENT(get_keys, "SELECT `key` FROM `entries` WHERE `modname` = ?");
	PREPARE_STATEMENT(get,
		"SELECT `value` FROM `entries` WHERE `modname` = ? AND `key` = ? LIMIT 1");
	PREPARE_STATEMENT(has,
		"SELECT 1 FROM `entries` WHERE `modname` = ? AND `key` = ? LIMIT 1");
	PREPARE_STATEMENT(set,
		"REPLACE INTO `entries` (`modname`, `key`, `value`) VALUES (?, ?, ?)");
	PREPARE_STATEMENT(remove, "DELETE FROM `entries` WHERE `modname` = ? AND `key` = ?");
	PREPARE_STATEMENT(remove_all, "DELETE FROM `entries` WHERE `modname` = ?");
}

void ModStorageDatabaseSQLite3::getModEntries(const std::string &modname, StringMap *storage)
{
	verifyDatabase();

	str_to_sqlite(m_stmt_get_all, 1, modname);
	while (SQLSTEP(m_stmt_get_all, "Failed to get mod entries")) {
		auto key = sqlite_to_blob(m_stmt_get_all, 0);
		auto value = sqlite_to_blob(m_stmt_get_all, 1);
		(*storage)[std::string(key)].assign(value);
	}
	sqlite3_vrfy(sqlite3_errcode(m_database), SQLITE_DONE);

	sqlite3_reset(m_stmt_get_all);
}

void ModStorageDatabaseSQLite3::getModKeys(const std::string &modname,
		std::vector<std::string> *storage)
{
	verifyDatabase();

	str_to_sqlite(m_stmt_get_keys, 1, modname);
	while (SQLSTEP(m_stmt_get_keys, "Failed to get mod keys")) {
		auto key = sqlite_to_blob(m_stmt_get_keys, 0);
		storage->emplace_back(key);
	}
	sqlite3_vrfy(sqlite3_errcode(m_database), SQLITE_DONE);

	sqlite3_reset(m_stmt_get_keys);
}

bool ModStorageDatabaseSQLite3::getModEntry(const std::string &modname,
	const std::string &key, std::string *value)
{
	verifyDatabase();

	str_to_sqlite(m_stmt_get, 1, modname);
	blob_to_sqlite(m_stmt_get, 2, key);
	bool found = SQLSTEP(m_stmt_get, "Failed to get entry");
	if (found) {
		auto sv = sqlite_to_blob(m_stmt_get, 0);
		value->assign(sv);
		sqlite3_step(m_stmt_get);
	}

	sqlite3_reset(m_stmt_get);

	return found;
}

bool ModStorageDatabaseSQLite3::hasModEntry(const std::string &modname,
		const std::string &key)
{
	verifyDatabase();

	str_to_sqlite(m_stmt_has, 1, modname);
	blob_to_sqlite(m_stmt_has, 2, key);
	bool found = SQLSTEP(m_stmt_has, "");
	sqlite3_reset(m_stmt_has);

	return found;
}

bool ModStorageDatabaseSQLite3::setModEntry(const std::string &modname,
	const std::string &key, std::string_view value)
{
	verifyDatabase();

	str_to_sqlite(m_stmt_set, 1, modname);
	blob_to_sqlite(m_stmt_set, 2, key);
	blob_to_sqlite(m_stmt_set, 3, value);
	SQLRES(sqlite3_step(m_stmt_set), SQLITE_DONE, "Failed to set mod entry")

	sqlite3_reset(m_stmt_set);

	return true;
}

bool ModStorageDatabaseSQLite3::removeModEntry(const std::string &modname,
		const std::string &key)
{
	verifyDatabase();

	str_to_sqlite(m_stmt_remove, 1, modname);
	blob_to_sqlite(m_stmt_remove, 2, key);
	sqlite3_vrfy(sqlite3_step(m_stmt_remove), SQLITE_DONE);
	int changes = sqlite3_changes(m_database);

	sqlite3_reset(m_stmt_remove);

	return changes > 0;
}

bool ModStorageDatabaseSQLite3::removeModEntries(const std::string &modname)
{
	verifyDatabase();

	str_to_sqlite(m_stmt_remove_all, 1, modname);
	sqlite3_vrfy(sqlite3_step(m_stmt_remove_all), SQLITE_DONE);
	int changes = sqlite3_changes(m_database);

	sqlite3_reset(m_stmt_remove_all);

	return changes > 0;
}

void ModStorageDatabaseSQLite3::listMods(std::vector<std::string> *res)
{
	verifyDatabase();

	while (SQLSTEP(m_stmt_get_mods, "Failed to list mods")) {
		auto name = sqlite_to_string_view(m_stmt_get_mods, 0);
		res->emplace_back(name);
	}
	sqlite3_vrfy(sqlite3_errcode(m_database), SQLITE_DONE);

	sqlite3_reset(m_stmt_get_mods);
}
