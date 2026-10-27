// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include <string>
#include <algorithm>
#include <vector>
#include <unordered_map>

typedef std::unordered_map<std::string, int> ItemGroupList;

/**
 * Entries of a map-like container, sorted by key.
 * Used for serialization so identical definitions always produce identical bytes
 * (iteration order of unordered_map depends on the instance; the multiworld
 * server compares the serialized definitions of different worlds).
 */
template<typename Map>
inline std::vector<const typename Map::value_type *> sortedEntries(const Map &map)
{
	std::vector<const typename Map::value_type *> out;
	out.reserve(map.size());
	for (const auto &entry : map)
		out.push_back(&entry);
	std::sort(out.begin(), out.end(), [] (const auto *a, const auto *b) {
		return a->first < b->first;
	});
	return out;
}

static inline int itemgroup_get(const ItemGroupList &groups, const std::string &name)
{
	ItemGroupList::const_iterator i = groups.find(name);
	if (i == groups.end())
		return 0;
	return i->second;
}
