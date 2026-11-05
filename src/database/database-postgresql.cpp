// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2016 Loic Blot <loic.blot@unix-experience.fr>

#include "config.h"

#if USE_POSTGRESQL

#include "database-postgresql.h"

#ifdef _WIN32
	#include <windows.h>
	#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

#include "debug.h"
#include "exceptions.h"
#include "remoteplayer.h"
#include "server/player_sao.h"
#include <cstdlib>

Database_PostgreSQL::Database_PostgreSQL(const std::string &connect_string,
	const char *type) :
	m_connect_string(connect_string)
{
	if (m_connect_string.empty()) {
		// Use given type to reference the exact setting in the error message
		std::string s = type;
		std::string msg =
			"Set pgsql" + s + "_connection string in world.conf to "
			"use the postgresql backend\n"
			"Notes:\n"
			"pgsql" + s + "_connection has the following form: \n"
			"\tpgsql" + s + "_connection = host=127.0.0.1 port=5432 "
			"user=mt_user password=mt_password dbname=voxelyx" + s + "\n"
			"mt_user should have CREATE TABLE, INSERT, SELECT, UPDATE and "
			"DELETE rights on the database. "
			"Don't create mt_user as a SUPERUSER!";
		throw SettingNotFoundException(msg);
	}
}

Database_PostgreSQL::~Database_PostgreSQL()
{
	PQfinish(m_conn);
}

void Database_PostgreSQL::connectToDatabase()
{
	m_conn = PQconnectdb(m_connect_string.c_str());

	if (PQstatus(m_conn) != CONNECTION_OK) {
		throw DatabaseException(std::string(
			"PostgreSQL database error: ") +
			PQerrorMessage(m_conn));
	}

	m_pgversion = PQserverVersion(m_conn);

	/*
	* We are using UPSERT feature from PostgreSQL 9.5
	* to have the better performance where possible.
	*/
	if (m_pgversion < 90500) {
		warningstream << "Your PostgreSQL server lacks UPSERT "
			<< "support. Use version 9.5 or better if possible."
			<< std::endl;
	}

	infostream << "PostgreSQL Database: Version " << m_pgversion
			<< " Connection made." << std::endl;

	createDatabase();
	initStatements();
}

void Database_PostgreSQL::verifyDatabase()
{
	if (PQstatus(m_conn) == CONNECTION_OK)
		return;

	PQreset(m_conn);
	ping();
	initStatements();
}

void Database_PostgreSQL::ping()
{
	if (PQping(m_connect_string.c_str()) != PQPING_OK) {
		throw DatabaseException(std::string(
			"PostgreSQL database error: ") +
			PQerrorMessage(m_conn));
	}
}

bool Database_PostgreSQL::initialized() const
{
	return m_conn && PQstatus(m_conn) == CONNECTION_OK;
}

PGresult *Database_PostgreSQL::checkResults(PGresult *result, bool clear)
{
	ExecStatusType statusType = PQresultStatus(result);

	switch (statusType) {
	case PGRES_COMMAND_OK:
	case PGRES_TUPLES_OK:
		break;
	case PGRES_FATAL_ERROR:
	default:
		throw DatabaseException(
			std::string("PostgreSQL database error: ") +
			PQresultErrorMessage(result));
	}

	if (clear)
		PQclear(result);

	return result;
}

void Database_PostgreSQL::createTableIfNotExists(const std::string &table_name,
		const std::string &definition)
{
	std::string sql_check_table = "SELECT relname FROM pg_class WHERE relname='" +
		table_name + "';";
	PGresult *result = checkResults(PQexec(m_conn, sql_check_table.c_str()), false);

	// If table doesn't exist, create it
	if (!PQntuples(result)) {
		checkResults(PQexec(m_conn, definition.c_str()));
	}

	PQclear(result);
}

void Database_PostgreSQL::execSimple(const char *sql)
{
	checkResults(PQexec(m_conn, sql));
}

PGresult *Database_PostgreSQL::execQuery(const char *sql)
{
	return checkResults(PQexec(m_conn, sql), false);
}

void Database_PostgreSQL::beginSave()
{
	verifyDatabase();
	checkResults(PQexec(m_conn, "BEGIN;"));
}

void Database_PostgreSQL::endSave()
{
	checkResults(PQexec(m_conn, "COMMIT;"));
}

void Database_PostgreSQL::rollback()
{
	checkResults(PQexec(m_conn, "ROLLBACK;"));
}

MapDatabasePostgreSQL::MapDatabasePostgreSQL(const std::string &connect_string):
	Database_PostgreSQL(connect_string, ""),
	MapDatabase()
{
	connectToDatabase();
}


void MapDatabasePostgreSQL::createDatabase()
{
	createTableIfNotExists("blocks",
		"CREATE TABLE blocks ("
			"posX smallint NOT NULL,"
			"posY smallint NOT NULL,"
			"posZ smallint NOT NULL,"
			"data BYTEA,"
			"PRIMARY KEY (posX,posY,posZ)"
			");"
	);

	infostream << "PostgreSQL: Map Database was initialized." << std::endl;
}

void MapDatabasePostgreSQL::initStatements()
{
	prepareStatement("read_block",
		"SELECT data FROM blocks "
			"WHERE posX = $1::int4 AND posY = $2::int4 AND "
			"posZ = $3::int4");

	if (getPGVersion() < 90500) {
		prepareStatement("write_block_insert",
			"INSERT INTO blocks (posX, posY, posZ, data) SELECT "
				"$1::int4, $2::int4, $3::int4, $4::bytea "
				"WHERE NOT EXISTS (SELECT true FROM blocks "
				"WHERE posX = $1::int4 AND posY = $2::int4 AND "
				"posZ = $3::int4)");

		prepareStatement("write_block_update",
			"UPDATE blocks SET data = $4::bytea "
				"WHERE posX = $1::int4 AND posY = $2::int4 AND "
				"posZ = $3::int4");
	} else {
		prepareStatement("write_block",
			"INSERT INTO blocks (posX, posY, posZ, data) VALUES "
				"($1::int4, $2::int4, $3::int4, $4::bytea) "
				"ON CONFLICT ON CONSTRAINT blocks_pkey DO "
				"UPDATE SET data = $4::bytea");
	}

	prepareStatement("delete_block", "DELETE FROM blocks WHERE "
		"posX = $1::int4 AND posY = $2::int4 AND posZ = $3::int4");

	prepareStatement("list_all_loadable_blocks",
		"SELECT posX, posY, posZ FROM blocks");
}

bool MapDatabasePostgreSQL::saveBlock(const v3s16 &pos, std::string_view data)
{
	// Verify if we don't overflow the platform integer with the mapblock size
	if (data.size() > INT_MAX) {
		errorstream << "Database_PostgreSQL::saveBlock: Data truncation! "
			<< "data.size() over 0xFFFFFFFF (== " << data.size()
			<< ")" << std::endl;
		return false;
	}

	verifyDatabase();

	s32 x, y, z;
	x = htonl(pos.X);
	y = htonl(pos.Y);
	z = htonl(pos.Z);

	const void *args[] = { &x, &y, &z, data.data() };
	const int argLen[] = {
		sizeof(x), sizeof(y), sizeof(z), (int)data.size()
	};
	const int argFmt[] = { 1, 1, 1, 1 };

	if (getPGVersion() < 90500) {
		execPrepared("write_block_update", ARRLEN(args), args, argLen, argFmt);
		execPrepared("write_block_insert", ARRLEN(args), args, argLen, argFmt);
	} else {
		execPrepared("write_block", ARRLEN(args), args, argLen, argFmt);
	}
	return true;
}

void MapDatabasePostgreSQL::loadBlock(const v3s16 &pos, std::string *block)
{
	verifyDatabase();

	s32 x, y, z;
	x = htonl(pos.X);
	y = htonl(pos.Y);
	z = htonl(pos.Z);

	const void *args[] = { &x, &y, &z };
	const int argLen[] = { sizeof(x), sizeof(y), sizeof(z) };
	const int argFmt[] = { 1, 1, 1 };

	PGresult *results = execPrepared("read_block", ARRLEN(args), args,
		argLen, argFmt, false);

	if (PQntuples(results))
		*block = pg_to_string(results, 0, 0);
	else
		block->clear();

	PQclear(results);
}

bool MapDatabasePostgreSQL::deleteBlock(const v3s16 &pos)
{
	verifyDatabase();

	s32 x, y, z;
	x = htonl(pos.X);
	y = htonl(pos.Y);
	z = htonl(pos.Z);

	const void *args[] = { &x, &y, &z };
	const int argLen[] = { sizeof(x), sizeof(y), sizeof(z) };
	const int argFmt[] = { 1, 1, 1 };

	execPrepared("delete_block", ARRLEN(args), args, argLen, argFmt);

	return true;
}

void MapDatabasePostgreSQL::listAllLoadableBlocks(std::vector<v3s16> &dst)
{
	verifyDatabase();

	PGresult *results = execPrepared("list_all_loadable_blocks", 0,
		NULL, NULL, NULL, false, false);

	int numrows = PQntuples(results);

	for (int row = 0; row < numrows; ++row)
		dst.push_back(pg_to_v3s16(results, row, 0));

	PQclear(results);
}

/*
 * Accounts database
 */

namespace {

// See the SQLite version of this schema for the reasons behind it. Times are
// seconds since the Unix epoch.
const char *const ACCOUNTS_SCHEMA = R"SQL(
CREATE TABLE worlds (
	id   BIGINT PRIMARY KEY,
	name TEXT NOT NULL
);

CREATE TABLE accounts (
	id               BIGSERIAL PRIMARY KEY,
	name             TEXT   NOT NULL UNIQUE CHECK (name <> ''),
	password         TEXT   NOT NULL,
	created_at       BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM now())::BIGINT),
	last_login       BIGINT CHECK (last_login IS NULL OR last_login > 0),
	current_world_id BIGINT REFERENCES worlds (id) ON DELETE SET NULL
);
CREATE INDEX accounts_current_world_id ON accounts (current_world_id);

CREATE TABLE privileges (
	account_id BIGINT NOT NULL REFERENCES accounts (id) ON DELETE CASCADE,
	privilege  TEXT   NOT NULL CHECK (privilege <> ''),
	PRIMARY KEY (account_id, privilege)
);

CREATE TABLE world_players (
	id         BIGSERIAL PRIMARY KEY,
	account_id BIGINT  NOT NULL REFERENCES accounts (id) ON DELETE CASCADE,
	world_id   BIGINT  NOT NULL REFERENCES worlds (id)   ON DELETE CASCADE,
	pitch      DOUBLE PRECISION NOT NULL,
	yaw        DOUBLE PRECISION NOT NULL,
	pos_x      DOUBLE PRECISION NOT NULL,
	pos_y      DOUBLE PRECISION NOT NULL,
	pos_z      DOUBLE PRECISION NOT NULL,
	hp         INTEGER NOT NULL CHECK (hp     BETWEEN 0 AND 65535),
	breath     INTEGER NOT NULL CHECK (breath BETWEEN 0 AND 65535),
	created_at BIGINT  NOT NULL DEFAULT (EXTRACT(EPOCH FROM now())::BIGINT),
	updated_at BIGINT  NOT NULL DEFAULT (EXTRACT(EPOCH FROM now())::BIGINT),
	UNIQUE (account_id, world_id)
);
CREATE INDEX world_players_world_id ON world_players (world_id);

CREATE TABLE world_player_metadata (
	world_player_id BIGINT NOT NULL REFERENCES world_players (id) ON DELETE CASCADE,
	key             BYTEA  NOT NULL,
	value           BYTEA  NOT NULL,
	PRIMARY KEY (world_player_id, key)
);

CREATE TABLE world_player_inventories (
	world_player_id BIGINT  NOT NULL REFERENCES world_players (id) ON DELETE CASCADE,
	list_index      INTEGER NOT NULL CHECK (list_index >= 0),
	name            TEXT    NOT NULL,
	width           INTEGER NOT NULL CHECK (width >= 0),
	size            INTEGER NOT NULL CHECK (size  >= 0),
	PRIMARY KEY (world_player_id, list_index),
	UNIQUE (world_player_id, name)
);

CREATE TABLE world_player_inventory_items (
	world_player_id BIGINT  NOT NULL,
	list_index      INTEGER NOT NULL,
	slot            INTEGER NOT NULL CHECK (slot >= 0),
	item            TEXT    NOT NULL CHECK (item <> ''),
	PRIMARY KEY (world_player_id, list_index, slot),
	FOREIGN KEY (world_player_id, list_index)
		REFERENCES world_player_inventories (world_player_id, list_index)
		ON DELETE CASCADE
);
)SQL";

// A float so that it reads back as the same float
std::string float_to_pg(double v)
{
	char buf[40];
	snprintf(buf, sizeof(buf), "%.9g", v);
	return buf;
}

} // namespace

AccountsDatabase_PostgreSQL::AccountsDatabase_PostgreSQL(const std::string &connect_string) :
	Database_PostgreSQL(connect_string, "_auth")
{
}

void AccountsDatabase_PostgreSQL::createDatabase()
{
	// Several connections can do this at the same time: the lock is released
	// when the transaction ends, and whoever comes second finds it done.
	beginSave();
	try {
		execSimple("SELECT pg_advisory_xact_lock(7160181)");
		execSimple("CREATE TABLE IF NOT EXISTS schema_version "
			"(version INTEGER NOT NULL)");
		PGresult *res = execQuery("SELECT version FROM schema_version");
		const int rows = PQntuples(res);
		const int version = rows ? atoi(PQgetvalue(res, 0, 0)) : 0;
		PQclear(res);
		if (version > SCHEMA_VERSION) {
			throw DatabaseException("The accounts database has schema version " +
				itos(version) + ", this version only knows up to " +
				itos(SCHEMA_VERSION) + ". It was made by a newer version.");
		}
		if (rows == 0) {
			execSimple(ACCOUNTS_SCHEMA);
			execSimple(("INSERT INTO schema_version (version) VALUES (" +
				itos(SCHEMA_VERSION) + ")").c_str());
		}
	} catch (...) {
		try {
			rollback();
		} catch (const DatabaseException &) {
		}
		throw;
	}
	endSave();
}

void AccountsDatabase_PostgreSQL::initAccountStatements()
{
	prepareStatement("account_id", "SELECT id FROM accounts WHERE name = $1");
}

u64 AccountsDatabase_PostgreSQL::getAccountId(const std::string &name)
{
	const char *values[] = { name.c_str() };
	PGresult *res = execPrepared("account_id", 1, values, false, false);
	u64 id = 0;
	if (PQntuples(res))
		id = strtoull(PQgetvalue(res, 0, 0), nullptr, 10);
	PQclear(res);
	return id;
}

/*
 * Player Database
 */

PlayerDatabasePostgreSQL::PlayerDatabasePostgreSQL(const std::string &connect_string,
		u32 world_id):
	AccountsDatabase_PostgreSQL(connect_string),
	PlayerDatabase(),
	m_world_id(world_id)
{
	connectToDatabase();
}

void PlayerDatabasePostgreSQL::initStatements()
{
	initAccountStatements();

	prepareStatement("account_add_locked", "INSERT INTO accounts (name, password) "
		"VALUES ($1, $2) ON CONFLICT (name) DO NOTHING");

	prepareStatement("player_load",
		"SELECT wp.id, wp.pitch, wp.yaw, wp.pos_x, wp.pos_y, wp.pos_z, wp.hp, wp.breath "
		"FROM world_players wp JOIN accounts a ON a.id = wp.account_id "
		"WHERE a.name = $1 AND wp.world_id = $2::bigint");
	prepareStatement("player_upsert",
		"INSERT INTO world_players (account_id, world_id, pitch, yaw, pos_x, pos_y, "
		"pos_z, hp, breath) VALUES ($1::bigint, $2::bigint, $3::double precision, "
		"$4::double precision, $5::double precision, $6::double precision, "
		"$7::double precision, $8::integer, $9::integer) "
		"ON CONFLICT (account_id, world_id) DO UPDATE SET "
		"pitch = EXCLUDED.pitch, yaw = EXCLUDED.yaw, pos_x = EXCLUDED.pos_x, "
		"pos_y = EXCLUDED.pos_y, pos_z = EXCLUDED.pos_z, hp = EXCLUDED.hp, "
		"breath = EXCLUDED.breath, updated_at = EXTRACT(EPOCH FROM now())::BIGINT "
		"RETURNING id");
	prepareStatement("player_remove",
		"DELETE FROM world_players WHERE world_id = $1::bigint AND account_id = "
		"(SELECT id FROM accounts WHERE name = $2)");
	prepareStatement("player_list",
		"SELECT a.name FROM world_players wp JOIN accounts a ON a.id = wp.account_id "
		"WHERE wp.world_id = $1::bigint ORDER BY a.name");

	prepareStatement("inventory_load",
		"SELECT list_index, width, name, size FROM world_player_inventories "
		"WHERE world_player_id = $1::bigint ORDER BY list_index");
	prepareStatement("inventory_items_load",
		"SELECT slot, item FROM world_player_inventory_items "
		"WHERE world_player_id = $1::bigint AND list_index = $2::integer");
	prepareStatement("inventory_add",
		"INSERT INTO world_player_inventories (world_player_id, list_index, width, "
		"name, size) VALUES ($1::bigint, $2::integer, $3::integer, $4, $5::integer)");
	prepareStatement("inventory_item_add",
		"INSERT INTO world_player_inventory_items (world_player_id, list_index, "
		"slot, item) VALUES ($1::bigint, $2::integer, $3::integer, $4)");
	prepareStatement("inventory_remove",
		"DELETE FROM world_player_inventories WHERE world_player_id = $1::bigint");

	prepareStatement("metadata_load",
		"SELECT key, value FROM world_player_metadata WHERE world_player_id = $1::bigint");
	prepareStatement("metadata_add",
		"INSERT INTO world_player_metadata (world_player_id, key, value) "
		"VALUES ($1::bigint, $2, $3)");
	prepareStatement("metadata_remove",
		"DELETE FROM world_player_metadata WHERE world_player_id = $1::bigint");
}

void PlayerDatabasePostgreSQL::provisionAccount(const std::string &name)
{
	if (getAccountId(name) != 0)
		return;
	transaction([&] {
		const char *values[] = { name.c_str(), ACCOUNT_LOCKED_PASSWORD };
		execPrepared("account_add_locked", 2, values);
	});
	infostream << "PlayerDatabasePostgreSQL: added a locked account for \"" << name
		<< "\", which has none" << std::endl;
}

void PlayerDatabasePostgreSQL::savePlayer(RemotePlayer *player)
{
	PlayerSAO *sao = player->getPlayerSAO();
	sanity_check(sao);

	verifyDatabase();

	const std::string &name = player->getName();
	const v3f pos = sao->getBasePosition();
	const std::string world = itos(m_world_id);

	transaction([&] {
		const u64 account_id = getAccountId(name);
		if (account_id == 0) {
			// The account was deleted while the player was online. Don't bring it back.
			warningstream << "PlayerDatabasePostgreSQL: \"" << name << "\" has no "
				"account, not saving their state" << std::endl;
			return;
		}

		const std::string account = std::to_string(account_id);
		const std::string pitch = float_to_pg(sao->getLookPitch());
		const std::string yaw = float_to_pg(sao->getRotation().Y);
		const std::string px = float_to_pg(pos.X), py = float_to_pg(pos.Y),
			pz = float_to_pg(pos.Z);
		const std::string hp = itos(sao->getHP()), breath = itos(sao->getBreath());
		const char *values[] = { account.c_str(), world.c_str(), pitch.c_str(),
			yaw.c_str(), px.c_str(), py.c_str(), pz.c_str(), hp.c_str(), breath.c_str() };
		PGresult *res = execPrepared("player_upsert", 9, values, false, false);
		const std::string pid = pg_to_string(res, 0, 0);
		PQclear(res);

		// Inventory and metadata are written anew
		const char *pid_value[] = { pid.c_str() };
		execPrepared("inventory_remove", 1, pid_value);
		execPrepared("metadata_remove", 1, pid_value);

		const auto &inventory_lists = player->inventory.getLists();
		std::ostringstream oss;
		for (u16 i = 0; i < inventory_lists.size(); i++) {
			const InventoryList *list = inventory_lists[i];
			const std::string index = itos(i), width = itos(list->getWidth()),
				size = itos(list->getSize());
			const char *inv_values[] = { pid.c_str(), index.c_str(), width.c_str(),
				list->getName().c_str(), size.c_str() };
			execPrepared("inventory_add", 5, inv_values);

			for (u32 j = 0; j < list->getSize(); j++) {
				const ItemStack &stack = list->getItem(j);
				if (stack.empty())
					continue;
				oss.str("");
				oss.clear();
				stack.serialize(oss);
				const std::string slot = itos(j), item = oss.str();
				const char *item_values[] = { pid.c_str(), index.c_str(), slot.c_str(),
					item.c_str() };
				execPrepared("inventory_item_add", 4, item_values);
			}
		}

		for (const auto &attr : sao->getMeta().getStrings()) {
			// Arbitrary bytes: sent as they are
			const void *args[] = { pid.c_str(), attr.first.data(), attr.second.data() };
			const int argLen[] = { -1, (int)MYMIN(attr.first.size(), INT_MAX),
				(int)MYMIN(attr.second.size(), INT_MAX) };
			const int argFmt[] = { 0, 1, 1 };
			execPrepared("metadata_add", ARRLEN(args), args, argLen, argFmt);
		}
	});

	player->onSuccessfulSave();
}

bool PlayerDatabasePostgreSQL::loadPlayer(RemotePlayer *player, PlayerSAO *sao)
{
	sanity_check(sao);
	verifyDatabase();

	const std::string &name = player->getName();
	provisionAccount(name);

	const std::string world = itos(m_world_id);
	const char *values[] = { name.c_str(), world.c_str() };
	PGresult *results = execPrepared("player_load", 2, values, false, false);
	if (!PQntuples(results)) {
		PQclear(results);
		return false;
	}

	const std::string pid = pg_to_string(results, 0, 0);
	sao->setLookPitch(pg_to_float(results, 0, 1));
	sao->setPlayerYaw(pg_to_float(results, 0, 2));
	sao->setBasePosition(v3f(pg_to_float(results, 0, 3), pg_to_float(results, 0, 4),
		pg_to_float(results, 0, 5)));
	sao->setHPRaw((u16) MYMIN(pg_to_int(results, 0, 6), U16_MAX));
	sao->setBreath((u16) MYMIN(pg_to_int(results, 0, 7), U16_MAX), false);
	PQclear(results);

	const char *pid_value[] = { pid.c_str() };
	results = execPrepared("inventory_load", 1, pid_value, false, false);
	for (int row = 0; row < PQntuples(results); ++row) {
		const std::string index = pg_to_string(results, row, 0);
		InventoryList *invList = player->inventory.addList(
			pg_to_string(results, row, 2), pg_to_uint(results, row, 3));
		invList->setWidth(pg_to_uint(results, row, 1));

		const char *item_values[] = { pid.c_str(), index.c_str() };
		PGresult *items = execPrepared("inventory_items_load", 2, item_values, false, false);
		for (int r = 0; r < PQntuples(items); r++) {
			ItemStack stack;
			stack.deSerialize(pg_to_string(items, r, 1));
			invList->changeItem(pg_to_uint(items, r, 0), stack);
		}
		PQclear(items);
	}
	PQclear(results);

	// Binary results: the keys and values are bytes
	results = execPrepared("metadata_load", 1, pid_value, false, true);
	for (int row = 0; row < PQntuples(results); row++)
		sao->getMeta().setString(pg_to_string(results, row, 0), pg_to_string(results, row, 1));
	sao->getMeta().setModified(false);
	PQclear(results);
	return true;
}

bool PlayerDatabasePostgreSQL::removePlayer(const std::string &name)
{
	verifyDatabase();

	const std::string world = itos(m_world_id);
	const char *values[] = { world.c_str(), name.c_str() };
	PGresult *res = execPrepared("player_remove", 2, values, false, false);
	const bool removed = atoi(PQcmdTuples(res)) > 0;
	PQclear(res);
	return removed;
}

void PlayerDatabasePostgreSQL::listPlayers(std::vector<std::string> &res)
{
	verifyDatabase();

	const std::string world = itos(m_world_id);
	const char *values[] = { world.c_str() };
	PGresult *results = execPrepared("player_list", 1, values, false, false);
	for (int row = 0; row < PQntuples(results); row++)
		res.emplace_back(PQgetvalue(results, row, 0));
	PQclear(results);
}

/*
 * Auth database
 */

AuthDatabasePostgreSQL::AuthDatabasePostgreSQL(const std::string &connect_string) :
	AccountsDatabase_PostgreSQL(connect_string),
	AuthDatabase()
{
	connectToDatabase();
}

void AuthDatabasePostgreSQL::initStatements()
{
	initAccountStatements();

	// last_login: 0 and below (the Lua side uses -1 for "not yet") is "never"
	prepareStatement("auth_read", "SELECT id, name, password, COALESCE(last_login, 0) "
		"FROM accounts WHERE name = $1");
	prepareStatement("auth_write", "UPDATE accounts SET name = $1, password = $2, "
		"last_login = NULLIF(GREATEST($3::bigint, 0), 0) WHERE id = $4::bigint");
	prepareStatement("auth_create", "INSERT INTO accounts (name, password, last_login) "
		"VALUES ($1, $2, NULLIF(GREATEST($3::bigint, 0), 0)) RETURNING id");
	prepareStatement("auth_delete", "DELETE FROM accounts WHERE name = $1");

	prepareStatement("auth_list_names", "SELECT name FROM accounts ORDER BY name DESC");

	prepareStatement("auth_read_privs", "SELECT privilege FROM privileges "
		"WHERE account_id = $1::bigint");
	prepareStatement("auth_write_privs", "INSERT INTO privileges (account_id, privilege) "
		"VALUES ($1::bigint, $2) ON CONFLICT DO NOTHING");
	prepareStatement("auth_delete_privs", "DELETE FROM privileges WHERE account_id = $1::bigint");

	prepareStatement("world_register", "INSERT INTO worlds (id, name) VALUES ($1::bigint, $2) "
		"ON CONFLICT (id) DO UPDATE SET name = EXCLUDED.name");
	prepareStatement("world_delete", "DELETE FROM worlds WHERE id = $1::bigint");
	prepareStatement("current_world_get", "SELECT current_world_id FROM accounts WHERE name = $1");
	prepareStatement("current_world_set", "UPDATE accounts SET current_world_id = $1::bigint "
		"WHERE name = $2 AND current_world_id IS DISTINCT FROM $1::bigint");
}

bool AuthDatabasePostgreSQL::getAuth(const std::string &name, AuthEntry &res)
{
	verifyDatabase();

	const char *values[] = { name.c_str() };
	PGresult *result = execPrepared("auth_read", 1, values, false, false);
	if (PQntuples(result) == 0) {
		PQclear(result);
		return false;
	}

	res.id = strtoull(PQgetvalue(result, 0, 0), nullptr, 10);
	res.name = pg_to_string(result, 0, 1);
	res.password = pg_to_string(result, 0, 2);
	res.last_login = strtoll(PQgetvalue(result, 0, 3), nullptr, 10);
	PQclear(result);

	res.privileges.clear();
	const std::string id = std::to_string(res.id);
	const char *privsValues[] = { id.c_str() };
	PGresult *results = execPrepared("auth_read_privs", 1, privsValues, false, false);
	for (int row = 0; row < PQntuples(results); row++)
		res.privileges.emplace_back(PQgetvalue(results, row, 0));
	PQclear(results);

	return true;
}

bool AuthDatabasePostgreSQL::saveAuth(const AuthEntry &authEntry)
{
	transaction([&] {
		const std::string last_login = std::to_string(authEntry.last_login);
		const std::string id = std::to_string(authEntry.id);
		const char *values[] = { authEntry.name.c_str(), authEntry.password.c_str(),
			last_login.c_str(), id.c_str() };
		execPrepared("auth_write", 4, values);

		writePrivileges(authEntry);
	});
	return true;
}

bool AuthDatabasePostgreSQL::createAuth(AuthEntry &authEntry)
{
	transaction([&] {
		const std::string last_login = std::to_string(authEntry.last_login);
		const char *values[] = { authEntry.name.c_str(), authEntry.password.c_str(),
			last_login.c_str() };
		PGresult *result = execPrepared("auth_create", 3, values, false, false);
		if (PQntuples(result) == 0) {
			PQclear(result);
			throw DatabaseException("No ID returned on account creation");
		}
		authEntry.id = strtoull(PQgetvalue(result, 0, 0), nullptr, 10);
		PQclear(result);

		writePrivileges(authEntry);
	});
	return true;
}

bool AuthDatabasePostgreSQL::deleteAuth(const std::string &name)
{
	verifyDatabase();

	// Privileges and the state in the worlds go with it (ON DELETE CASCADE)
	const char *values[] = { name.c_str() };
	PGresult *res = execPrepared("auth_delete", 1, values, false, false);
	const bool deleted = atoi(PQcmdTuples(res)) > 0;
	PQclear(res);
	return deleted;
}

void AuthDatabasePostgreSQL::listNames(std::vector<std::string> &res)
{
	verifyDatabase();

	PGresult *results = execPrepared("auth_list_names", 0, NULL, NULL, NULL, false, false);
	for (int row = 0; row < PQntuples(results); ++row)
		res.emplace_back(PQgetvalue(results, row, 0));
	PQclear(results);
}

void AuthDatabasePostgreSQL::reload()
{
	// noop for PgSQL
}

void AuthDatabasePostgreSQL::writePrivileges(const AuthEntry &authEntry)
{
	const std::string authIdStr = std::to_string(authEntry.id);
	const char *values[] = { authIdStr.c_str() };
	execPrepared("auth_delete_privs", 1, values);

	for (const std::string &privilege : authEntry.privileges) {
		if (privilege.empty())
			continue; // not a privilege
		const char *values[] = { authIdStr.c_str(), privilege.c_str() };
		execPrepared("auth_write_privs", 2, values);
	}
}

void AuthDatabasePostgreSQL::registerWorld(u32 world_id, const std::string &name)
{
	transaction([&] {
		const std::string id = itos(world_id);
		const char *values[] = { id.c_str(), name.c_str() };
		execPrepared("world_register", 2, values);
	});
}

void AuthDatabasePostgreSQL::deleteWorld(u32 world_id)
{
	transaction([&] {
		const std::string id = itos(world_id);
		const char *values[] = { id.c_str() };
		execPrepared("world_delete", 1, values);
	});
}

bool AuthDatabasePostgreSQL::getCurrentWorld(const std::string &name, u32 *world_id)
{
	verifyDatabase();

	const char *values[] = { name.c_str() };
	PGresult *res = execPrepared("current_world_get", 1, values, false, false);
	bool found = false;
	if (PQntuples(res) && !PQgetisnull(res, 0, 0)) {
		*world_id = (u32) strtoul(PQgetvalue(res, 0, 0), nullptr, 10);
		found = true;
	}
	PQclear(res);
	return found;
}

void AuthDatabasePostgreSQL::setCurrentWorld(const std::string &name, u32 world_id)
{
	transaction([&] {
		const std::string id = itos(world_id);
		const char *values[] = { id.c_str(), name.c_str() };
		execPrepared("current_world_set", 2, values);
	});
}

ModStorageDatabasePostgreSQL::ModStorageDatabasePostgreSQL(const std::string &connect_string):
	Database_PostgreSQL(connect_string, "_mod_storage"),
	ModStorageDatabase()
{
	connectToDatabase();
}

void ModStorageDatabasePostgreSQL::createDatabase()
{
	createTableIfNotExists("mod_storage",
		"CREATE TABLE mod_storage ("
			"modname TEXT NOT NULL,"
			"key BYTEA NOT NULL,"
			"value BYTEA NOT NULL,"
			"PRIMARY KEY (modname, key)"
		");");

	infostream << "PostgreSQL: Mod Storage Database was initialized." << std::endl;
}

void ModStorageDatabasePostgreSQL::initStatements()
{
	prepareStatement("get_all",
		"SELECT key, value FROM mod_storage WHERE modname = $1");
	prepareStatement("get_all_keys",
		"SELECT key FROM mod_storage WHERE modname = $1");
	prepareStatement("get",
		"SELECT value FROM mod_storage WHERE modname = $1 AND key = $2::bytea");
	prepareStatement("has",
		"SELECT true FROM mod_storage WHERE modname = $1 AND key = $2::bytea");
	if (getPGVersion() < 90500) {
		prepareStatement("set_insert",
			"INSERT INTO mod_storage (modname, key, value) "
				"SELECT $1, $2::bytea, $3::bytea "
					"WHERE NOT EXISTS ("
						"SELECT true FROM mod_storage WHERE modname = $1 AND key = $2::bytea"
					")");
		prepareStatement("set_update",
			"UPDATE mod_storage SET value = $3::bytea WHERE modname = $1 AND key = $2::bytea");
	} else {
		prepareStatement("set",
			"INSERT INTO mod_storage (modname, key, value) VALUES ($1, $2::bytea, $3::bytea) "
				"ON CONFLICT ON CONSTRAINT mod_storage_pkey DO "
					"UPDATE SET value = $3::bytea");
	}
	prepareStatement("remove",
		"DELETE FROM mod_storage WHERE modname = $1 AND key = $2::bytea");
	prepareStatement("remove_all",
		"DELETE FROM mod_storage WHERE modname = $1");
	prepareStatement("list",
		"SELECT DISTINCT modname FROM mod_storage");
}

void ModStorageDatabasePostgreSQL::getModEntries(const std::string &modname, StringMap *storage)
{
	verifyDatabase();

	const void *args[] = { modname.c_str() };
	const int argLen[] = { -1 };
	const int argFmt[] = { 0 };
	PGresult *results = execPrepared("get_all", ARRLEN(args),
			args, argLen, argFmt, false);

	int numrows = PQntuples(results);

	for (int row = 0; row < numrows; ++row)
		(*storage)[pg_to_string(results, row, 0)] = pg_to_string(results, row, 1);

	PQclear(results);
}

void ModStorageDatabasePostgreSQL::getModKeys(const std::string &modname,
		std::vector<std::string> *storage)
{
	verifyDatabase();

	const void *args[] = { modname.c_str() };
	const int argLen[] = { -1 };
	const int argFmt[] = { 0 };
	PGresult *results = execPrepared("get_all_keys", ARRLEN(args),
			args, argLen, argFmt, false);

	int numrows = PQntuples(results);

	storage->reserve(storage->size() + numrows);
	for (int row = 0; row < numrows; ++row)
		storage->push_back(pg_to_string(results, row, 0));

	PQclear(results);
}

bool ModStorageDatabasePostgreSQL::getModEntry(const std::string &modname,
	const std::string &key, std::string *value)
{
	verifyDatabase();

	const void *args[] = { modname.c_str(), key.c_str() };
	const int argLen[] = { -1, (int)MYMIN(key.size(), INT_MAX) };
	const int argFmt[] = { 0, 1 };
	PGresult *results = execPrepared("get", ARRLEN(args), args, argLen, argFmt, false);

	int numrows = PQntuples(results);
	bool found = numrows > 0;

	if (found)
		*value = pg_to_string(results, 0, 0);

	PQclear(results);

	return found;
}

bool ModStorageDatabasePostgreSQL::hasModEntry(const std::string &modname,
		const std::string &key)
{
	verifyDatabase();

	const void *args[] = { modname.c_str(), key.c_str() };
	const int argLen[] = { -1, (int)MYMIN(key.size(), INT_MAX) };
	const int argFmt[] = { 0, 1 };
	PGresult *results = execPrepared("has", ARRLEN(args), args, argLen, argFmt, false);

	int numrows = PQntuples(results);
	bool found = numrows > 0;

	PQclear(results);

	return found;
}

bool ModStorageDatabasePostgreSQL::setModEntry(const std::string &modname,
	const std::string &key, std::string_view value)
{
	verifyDatabase();

	const void *args[] = { modname.c_str(), key.c_str(), value.data() };
	const int argLen[] = {
		-1,
		(int)MYMIN(key.size(), INT_MAX),
		(int)MYMIN(value.size(), INT_MAX),
	};
	const int argFmt[] = { 0, 1, 1 };
	if (getPGVersion() < 90500) {
		execPrepared("set_insert", ARRLEN(args), args, argLen, argFmt);
		execPrepared("set_update", ARRLEN(args), args, argLen, argFmt);
	} else {
		execPrepared("set", ARRLEN(args), args, argLen, argFmt);
	}

	return true;
}

bool ModStorageDatabasePostgreSQL::removeModEntry(const std::string &modname,
		const std::string &key)
{
	verifyDatabase();

	const void *args[] = { modname.c_str(), key.c_str() };
	const int argLen[] = { -1, (int)MYMIN(key.size(), INT_MAX) };
	const int argFmt[] = { 0, 1 };
	PGresult *results = execPrepared("remove", ARRLEN(args), args, argLen, argFmt, false);

	int affected = atoi(PQcmdTuples(results));

	PQclear(results);

	return affected > 0;
}

bool ModStorageDatabasePostgreSQL::removeModEntries(const std::string &modname)
{
	verifyDatabase();

	const void *args[] = { modname.c_str() };
	const int argLen[] = { -1 };
	const int argFmt[] = { 0 };
	PGresult *results = execPrepared("remove_all", ARRLEN(args), args, argLen, argFmt, false);

	int affected = atoi(PQcmdTuples(results));

	PQclear(results);

	return affected > 0;
}

void ModStorageDatabasePostgreSQL::listMods(std::vector<std::string> *res)
{
	verifyDatabase();

	PGresult *results = execPrepared("list", 0, NULL, false);

	int numrows = PQntuples(results);

	for (int row = 0; row < numrows; ++row)
		res->push_back(pg_to_string(results, row, 0));

	PQclear(results);
}


#endif // USE_POSTGRESQL
