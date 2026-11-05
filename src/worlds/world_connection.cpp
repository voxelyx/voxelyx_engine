// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "world_connection.h"
#include "exceptions.h"
#include "network/networkexceptions.h"
#include <algorithm>
#include <chrono>

con::IConnection &WorldConnection::backend() const
{
	auto *b = m_backend.load();
	if (!b)
		throw con::ConnectionException("WorldConnection has no backend yet");
	return *b;
}

void WorldConnection::push(NetworkPacket &&pkt)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_inbox.size() >= MAX_INBOX)
			return;
		m_inbox.push_back(std::move(pkt));
	}
	m_cv.notify_one();
}

size_t WorldConnection::inboxSize()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_inbox.size();
}

void WorldConnection::dropPacketsOf(session_t peer)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_inbox.erase(std::remove_if(m_inbox.begin(), m_inbox.end(),
			[peer] (const NetworkPacket &p) { return p.getPeerId() == peer; }),
			m_inbox.end());
}

void WorldConnection::Connect(Address)
{
	throw con::ConnectionException("WorldConnection is server-side only");
}

bool WorldConnection::Connected()
{
	auto *b = m_backend.load();
	return b && b->Connected();
}

void WorldConnection::DisconnectPeer(session_t peer_id)
{
	backend().DisconnectPeer(peer_id);
}

bool WorldConnection::ReceiveTimeoutMs(NetworkPacket *pkt, u32 timeout_ms)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	if (m_inbox.empty()) {
		if (timeout_ms == 0)
			return false;
		m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
				[this] { return !m_inbox.empty(); });
		if (m_inbox.empty())
			return false;
	}
	*pkt = std::move(m_inbox.front());
	m_inbox.pop_front();
	return true;
}

void WorldConnection::Send(session_t peer_id, u8 channelnum, NetworkPacket *pkt,
		bool reliable)
{
	backend().Send(peer_id, channelnum, pkt, reliable);
}

session_t WorldConnection::GetPeerID() const
{
	return backend().GetPeerID();
}

Address WorldConnection::GetPeerAddress(session_t peer_id)
{
	return backend().GetPeerAddress(peer_id);
}

float WorldConnection::getPeerStat(session_t peer_id, con::rtt_stat_type type)
{
	return backend().getPeerStat(peer_id, type);
}

float WorldConnection::getLocalStat(con::rate_stat_type type)
{
	return backend().getLocalStat(type);
}
