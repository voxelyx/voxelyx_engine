// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2018 nerzhul, Loic Blot <loic.blot@unix-experience.fr>

#include "test.h"
#include <algorithm>
#include "content/worlds.h"
#include "server/mods.h"
#include "settings.h"

class TestServerModManager : public TestBase
{
public:
	TestServerModManager() { TestManager::registerTestModule(this); }
	const char *getName() { return "TestServerModManager"; }

	void runTests(IGameDef *gamedef);

	// main.conf of the test server, and the directory with the mods
	std::string m_conf_path;
	ModPaths m_mod_paths;

	ServerModManager makeManager(const std::string &conf_path) {
		return ServerModManager(conf_path, m_mod_paths);
	}

	void testCreation();
	void testIsConsistent();
	void testNoModPaths();
	void testGetMods();
	void testDisabledMod();
	void testGetModspec();
	void testGetModNames();
	void testGetModMediaPaths();
};

static TestServerModManager g_test_instance;

static void makeMod(const std::string &dir, const std::string &name,
		const std::string &depends = "", bool textures = false)
{
	const auto p = dir + DIR_DELIM + name + DIR_DELIM;
	fs::CreateAllDirs(p);
	std::ofstream conf(p + "mod.conf", std::ios::out | std::ios::binary);
	conf << "name = " << name << "\n";
	if (!depends.empty())
		conf << "depends = " << depends << "\n";
	std::ofstream init(p + "init.lua", std::ios::out | std::ios::binary);
	init << "-- intentionally empty\n";
	if (textures)
		fs::CreateAllDirs(p + "textures");
}

void TestServerModManager::runTests(IGameDef *gamedef)
{
	// There are no games, only the mods of the server: base mods, the ones
	// that depend on them, and one that is turned off in main.conf
	const auto test_mods = getTestTempDirectory().append(DIR_DELIM "test_mods");
	makeMod(test_mods, "basenodes", "", true);
	makeMod(test_mods, "unittests", "basenodes", true);
	makeMod(test_mods, "test_mod");
	makeMod(test_mods, "disabled_mod");
	m_mod_paths = {{"mods", test_mods}};

	m_conf_path = getTestTempDirectory().append(DIR_DELIM "main.conf");

	TEST(testCreation);
	TEST(testIsConsistent);
	TEST(testNoModPaths);
	TEST(testGetMods);
	TEST(testDisabledMod);
	TEST(testGetModspec);
	TEST(testGetModNames);
	TEST(testGetModMediaPaths);
}

void TestServerModManager::testCreation()
{
	Settings server_config;
	server_config.set("load_mod_disabled_mod", "false");
	UASSERTEQ(bool, server_config.updateConfigFile(m_conf_path.c_str()), true);

	auto sm = makeManager(m_conf_path);
}

void TestServerModManager::testIsConsistent()
{
	auto sm = makeManager(m_conf_path);
	UASSERTEQ(bool, sm.isConsistent(), true);
	UASSERTEQ(bool, sm.getUnsatisfiedMods().empty(), true);
}

void TestServerModManager::testNoModPaths()
{
	// Without mod directories there are no mods, a missing main.conf is fine
	ServerModManager sm(m_conf_path + "_missing", {});
	UASSERTEQ(bool, sm.getMods().empty(), true);
	std::vector<std::string> names;
	sm.getModNames(names);
	UASSERTEQ(bool, names.empty(), true);
	std::vector<std::string> media;
	sm.getModsMediaPaths(media);
	UASSERTEQ(bool, media.empty(), true);
}

void TestServerModManager::testGetMods()
{
	auto sm = makeManager(m_conf_path);
	const auto &mods = sm.getMods();
	// All mods of the mod path, except the one that is turned off
	UASSERTEQ(std::size_t, mods.size(), 3);

	for (const auto &m : mods)
		UASSERTEQ(bool, m.path.empty(), false);

	// A mod comes after the mods it depends on
	auto pos = [&] (const std::string &name) {
		return std::find_if(mods.begin(), mods.end(),
			[&] (const ModSpec &m) { return m.name == name; }) - mods.begin();
	};
	UASSERT(pos("basenodes") < pos("unittests"));
}

void TestServerModManager::testDisabledMod()
{
	auto sm = makeManager(m_conf_path);
	UASSERTEQ(const ModSpec *, sm.getModSpec("disabled_mod"), NULL);

	// With no setting, it is loaded like all others
	auto all = makeManager(m_conf_path + "_missing");
	UASSERT(all.getModSpec("disabled_mod") != NULL);
	UASSERTEQ(std::size_t, all.getMods().size(), 4);
}

void TestServerModManager::testGetModspec()
{
	auto sm = makeManager(m_conf_path);
	UASSERTEQ(const ModSpec *, sm.getModSpec("wrongmod"), NULL);
	UASSERT(sm.getModSpec("basenodes") != NULL);
}

void TestServerModManager::testGetModNames()
{
	auto sm = makeManager(m_conf_path);
	std::vector<std::string> result;
	sm.getModNames(result);
	UASSERTEQ(bool, result.empty(), false);
	UASSERT(std::find(result.begin(), result.end(), "basenodes") != result.end());
}

void TestServerModManager::testGetModMediaPaths()
{
	auto sm = makeManager(m_conf_path);
	std::vector<std::string> result;
	sm.getModsMediaPaths(result);
	UASSERTEQ(bool, result.empty(), false);

	// Test media overriding:
	// unittests depends on basenodes, thus the unittests texture path must come
	// first in the returned media paths to take priority
	auto it = std::find(result.begin(), result.end(),
		sm.getModSpec("unittests")->path + DIR_DELIM + "textures");
	UASSERT(it != result.end());
	UASSERT(std::find(++it, result.end(),
		sm.getModSpec("basenodes")->path + DIR_DELIM + "textures") != result.end());
}
