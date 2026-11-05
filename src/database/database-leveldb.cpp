// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#include "config.h"

#if USE_LEVELDB

#include "database-leveldb.h"

#include "debug.h"
#include "log.h"
#include "filesys.h"
#include "exceptions.h"
#include "remoteplayer.h"
#include "irrlicht_changes/printing.h"
#include "server/player_sao.h"
#include "util/serialize.h"
#include "util/string.h"

#include "leveldb/db.h"


#define ENSURE_STATUS_OK(s) \
	if (!(s).ok()) { \
		throw DatabaseException(std::string("LevelDB error: ") + \
				(s).ToString()); \
	}


Database_LevelDB::Database_LevelDB(const std::string &savedir)
{
	leveldb::Options options;
	options.create_if_missing = true;
	leveldb::DB *db;
	leveldb::Status status = leveldb::DB::Open(options,
		savedir + DIR_DELIM + "map.db", &db);
	ENSURE_STATUS_OK(status);
	m_database.reset(db);
}

bool Database_LevelDB::saveBlock(const v3s16 &pos, std::string_view data)
{
	leveldb::Slice data_s(data.data(), data.size());
	leveldb::Status status = m_database->Put(leveldb::WriteOptions(),
			i64tos(getBlockAsInteger(pos)), data_s);
	if (!status.ok()) {
		warningstream << "saveBlock: LevelDB error saving block "
			<< pos << ": " << status.ToString() << std::endl;
		return false;
	}

	return true;
}

void Database_LevelDB::loadBlock(const v3s16 &pos, std::string *block)
{
	leveldb::Status status = m_database->Get(leveldb::ReadOptions(),
		i64tos(getBlockAsInteger(pos)), block);

	if (status.IsNotFound()) {
		block->clear();
	} else {
		ENSURE_STATUS_OK(status)
	}
}

bool Database_LevelDB::deleteBlock(const v3s16 &pos)
{
	leveldb::Status status = m_database->Delete(leveldb::WriteOptions(),
			i64tos(getBlockAsInteger(pos)));
	if (!status.ok()) {
		warningstream << "deleteBlock: LevelDB error deleting block "
			<< pos << ": " << status.ToString() << std::endl;
		return false;
	}

	return true;
}

void Database_LevelDB::listAllLoadableBlocks(std::vector<v3s16> &dst)
{
	std::unique_ptr<leveldb::Iterator> it(m_database->NewIterator(leveldb::ReadOptions()));
	for (it->SeekToFirst(); it->Valid(); it->Next()) {
		dst.push_back(getIntegerAsBlock(stoi64(it->key().ToString())));
	}
	ENSURE_STATUS_OK(it->status());  // Check for any errors found during the scan
}

#endif // USE_LEVELDB
