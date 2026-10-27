// Voxelyx
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <memory>
#include <mutex>
#include "database/database.h"

/*
	The account database of a server whose worlds run on separate threads.

	Accounts (password verifiers, privileges, last login) belong to the server,
	not to a world. Every world's environment uses the same instance of this
	class. The worlds have different environment locks, so access to the one
	underlying database is serialized here. No call ever leaves the database
	while the mutex is held, hence it can't take part in a deadlock.
*/
class SharedAuthDatabase final : public AuthDatabase
{
public:
	explicit SharedAuthDatabase(std::unique_ptr<AuthDatabase> inner) :
		m_inner(std::move(inner)) {}

	bool getAuth(const std::string &name, AuthEntry &res) override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_inner->getAuth(name, res);
	}
	bool saveAuth(const AuthEntry &authEntry) override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_inner->saveAuth(authEntry);
	}
	bool createAuth(AuthEntry &authEntry) override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_inner->createAuth(authEntry);
	}
	bool deleteAuth(const std::string &name) override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_inner->deleteAuth(name);
	}
	void listNames(std::vector<std::string> &res) override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_inner->listNames(res);
	}
	void reload() override
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_inner->reload();
	}

private:
	std::mutex m_mutex;
	std::unique_ptr<AuthDatabase> m_inner;
};
