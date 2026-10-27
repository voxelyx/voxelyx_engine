// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "nodedef.h" // content_t, CONTENT_MAX

/*
	Cluster-wide assignment of node content IDs.

	Map blocks that are sent to clients refer to nodes by content ID, and the
	client learned the ID <-> node mapping from the world it joined. For a
	client to move between worlds without re-downloading definitions, every
	world must use the same ID for the same node name.

	Ordinarily each world assigns IDs in the order its mods register nodes,
	which differs between worlds because Lua's `pairs()` order is randomized
	per Lua state. With a registry, the first world to register a name decides
	its ID and all other worlds follow. Thread-safe.
*/
class ContentIdRegistry
{
public:
	/**
	 * @param is_free whether the caller can use the given ID itself
	 * @return the ID for `name`, or CONTENT_IGNORE if none is available
	 */
	content_t acquire(const std::string &name,
			const std::function<bool(content_t)> &is_free)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_by_name.find(name);
		if (it != m_by_name.end())
			return it->second;

		for (u32 id = m_next; id <= CONTENT_MAX; id++) {
			if (m_used.count(id) || !is_free(id))
				continue;
			m_by_name[name] = id;
			m_used.insert(id);
			m_next = id + 1;
			return id;
		}
		return CONTENT_IGNORE;
	}

private:
	std::mutex m_mutex;
	std::unordered_map<std::string, content_t> m_by_name;
	std::unordered_set<content_t> m_used;
	u32 m_next = 0;
};
