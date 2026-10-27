// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2022 Voxelyx core developers & community

#pragma once

#include "server.h"
#include "server/mods.h"
#include "scripting_server.h"

// The world has to exist before the server that runs it
struct MockWorldHolder
{
	MockWorldHolder(const std::string &path) : mock_world(path) {}
	World mock_world;
};

class MockServer : private MockWorldHolder, public Server
{
public:
	/* Set `path_world` to a real existing folder if you plan to initialize scripting! */
	MockServer(const std::string &path_world = "fakepath") :
		MockWorldHolder(path_world),
		Server(mock_world, true, UDPSocket::CreateEphemeral(false), true, nullptr)
	{}

	/*
	 * Use this in unit tests to create scripting.
	 * Note that you still need to call script->loadBuiltin() and don't forget
	 * a try-catch for `ModError`.
	 */
	void createScripting() {
		m_script = std::make_unique<ServerScripting>(this);
		m_modmgr = std::make_unique<ServerModManager>(nullptr);
	}

	void start() = delete;
	void stop() = delete;

private:
	void SendChatMessage(session_t peer_id, const ChatMessage &message) override {}
};
