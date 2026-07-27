// AICraft-Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#ifdef AICRAFT_AGENT_CLIENT

#include "irrlichttypes.h"
#include "keycode.h"
#include <array>
#include <chrono>
#include <string>

class AgentControlBridge
{
public:
	explicit AgentControlBridge(const std::string &socket_path);
	~AgentControlBridge();

	AgentControlBridge(const AgentControlBridge &) = delete;
	AgentControlBridge &operator=(const AgentControlBridge &) = delete;

	void step();
	bool isKeyDown(GameKeyType key) const;
	bool wasKeyPressed(GameKeyType key);
	bool wasKeyReleased(GameKeyType key);
	void clearTransitions();

	float movementSpeed() const;
	float movementDirection() const;
	float yaw() const { return m_yaw; }
	float pitch() const { return m_pitch; }

private:
	void readAvailable();
	void applyLine(const std::string &line);
	void applyKey(GameKeyType key, bool down);
	void stopMovement();
	void sendReply(const std::string &action_id, const char *status, const char *code = nullptr);

	int m_socket = -1;
	std::string m_buffer;
	std::array<bool, GameKeyType::INTERNAL_ENUM_COUNT> m_down{};
	std::array<bool, GameKeyType::INTERNAL_ENUM_COUNT> m_pressed{};
	std::array<bool, GameKeyType::INTERNAL_ENUM_COUNT> m_released{};
	float m_yaw = 0.0f;
	float m_pitch = 0.0f;
	std::chrono::steady_clock::time_point m_control_deadline{};
};

#endif

