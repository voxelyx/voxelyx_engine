// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "config.h"

#if USE_LEVELDB

#include <memory>
#include <string>
#include "database.h"
#include "leveldb/db.h"

class Database_LevelDB : public MapDatabase
{
public:
	Database_LevelDB(const std::string &savedir);
	~Database_LevelDB() = default;

	bool saveBlock(const v3s16 &pos, std::string_view data);
	void loadBlock(const v3s16 &pos, std::string *block);
	bool deleteBlock(const v3s16 &pos);
	void listAllLoadableBlocks(std::vector<v3s16> &dst);

	void beginSave() {}
	void endSave() {}

private:
	std::unique_ptr<leveldb::DB> m_database;
};

#endif // USE_LEVELDB
