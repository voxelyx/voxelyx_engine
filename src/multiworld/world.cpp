// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "multiworld/world.h"
#include "content/worlds.h" // readWorldId, WORLD_CONF_FILENAME
#include "exceptions.h"
#include "filesys.h"
#include "log.h"
#include "server.h"
#include "multiworld/world_connection.h"
#include "util/string.h" // is_yes

World::World(const std::string &path) :
	m_path(path)
{
	std::string trimmed = path;
	while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\'))
		trimmed.pop_back();
	m_name = fs::GetFilenameFromPath(trimmed.c_str());
}

World::~World() = default;

std::string World::getConfPath() const
{
	return m_path + DIR_DELIM + WORLD_CONF_FILENAME;
}

void World::load()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_conf.readConfigFile(getConfPath().c_str()))
		throw BaseException("Cannot read " + getConfPath());
	if (!readWorldId(m_path, &m_id))
		throw BaseException("world_id is missing or invalid in " + getConfPath());
}

std::optional<v3f> World::getSpawn() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::optional<v3f> spawn;
	if (m_conf.getV3FNoEx("static_spawnpoint", spawn))
		return spawn;
	return std::nullopt;
}

bool World::setSpawn(v3f pos)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_conf.setV3F("static_spawnpoint", pos);
	if (!m_conf.updateConfigFile(getConfPath().c_str())) {
		errorstream << "World \"" << m_name << "\": could not write the spawn point to "
			<< getConfPath() << std::endl;
		return false;
	}
	return true;
}

bool World::getSetting(const std::string &name, std::string &value) const
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_conf.getNoEx(name, value))
			return true;
	}
	return g_settings->getNoEx(name, value);
}

bool World::getBool(const std::string &name) const
{
	std::string value;
	return getSetting(name, value) && is_yes(value);
}
