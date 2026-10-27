// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include "network/connection.h"
#include "network/networkpacket.h"

/*
	A world's view on the cluster's single UDP connection.

	Every world's Server instance expects to own an IConnection that it
	receives from and sends through. In a multiworld cluster the real
	connection is shared; the WorldCluster demultiplexes incoming packets by
	peer and pushes them into the inbox of the world that currently owns the
	peer. Outgoing traffic goes straight to the real connection (which is
	thread-safe), so worlds never wait for each other.
*/
class WorldConnection final : public con::IConnection
{
public:
	/// Packets that are queued but not yet received are dropped beyond this
	/// (protects against a stalled world being flooded)
	static constexpr size_t MAX_INBOX = 100000;

	/// Must be called before any peer exists; `backend` must outlive this
	void setBackend(con::IConnection *backend) { m_backend = backend; }

	/// Thread-safe, called by the cluster's receive thread
	void push(NetworkPacket &&pkt);
	size_t inboxSize();
	/// Drops queued packets of one peer (used when a peer leaves the world)
	void dropPacketsOf(session_t peer);

	// IConnection
	void Connect(Address) override;
	bool Connected() override;
	// A single world must not tear down the shared connection
	void Disconnect() override {}
	void DisconnectPeer(session_t peer_id) override;
	bool ReceiveTimeoutMs(NetworkPacket *pkt, u32 timeout_ms) override;
	void Send(session_t peer_id, u8 channelnum, NetworkPacket *pkt, bool reliable) override;
	session_t GetPeerID() const override;
	Address GetPeerAddress(session_t peer_id) override;
	float getPeerStat(session_t peer_id, con::rtt_stat_type type) override;
	float getLocalStat(con::rate_stat_type type) override;

private:
	con::IConnection &backend() const;

	std::atomic<con::IConnection *> m_backend{nullptr};
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::deque<NetworkPacket> m_inbox;
};
