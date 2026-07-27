// AICraft-Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Modified for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.

#pragma once

#ifdef AICRAFT_AGENT_CLIENT

#include "irrlichttypes.h"
#include "irr_v3d.h"
#include "keycode.h"
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace Json {
class Value;
}

enum class AgentClientState {
	BOOTING,
	AUTHENTICATING,
	MEDIA_SYNC,
	PLAYER_READY,
	CONTROL_READY,
};

enum class AgentActionType {
	MOVE,
	LOOK,
	DIG,
	PLACE,
	USE,
	ATTACK,
	CRAFT,
	EQUIP,
	CHAT,
	WAIT,
	INVENTORY_MOVE,
	CONTAINER_MOVE,
	FORMSPEC_SUBMIT,
};

enum class AgentTargetType {
	NONE,
	NODE,
	ENTITY,
};

enum class AgentActionStatus {
	EXECUTING,
	SUCCEEDED,
	REJECTED,
	EXPIRED,
};

struct AgentInventoryEndpoint
{
	std::string inventory;
	std::string list;
	u16 slot = 0;
};

struct AgentAction
{
	std::string action_id;
	AgentActionType type = AgentActionType::WAIT;
	s32 duration_ms = 0;

	f32 forward = 0.0f;
	f32 right = 0.0f;
	bool jump = false;
	bool sneak = false;
	f32 yaw = 0.0f;
	f32 pitch = 0.0f;

	AgentTargetType target_type = AgentTargetType::NONE;
	v3s16 target_position;
	v3s16 target_face;
	bool has_target_face = false;
	std::string entity_id;
	std::string item;

	std::string recipe;
	u16 count = 0;
	u16 slot = 0;
	std::string message;

	AgentInventoryEndpoint from;
	AgentInventoryEndpoint to;

	std::string form_name;
	std::map<std::string, std::string> fields;
};

class AgentControlBridge
{
public:
	AgentControlBridge(const std::string &socket_path, const std::string &session_id,
			const std::string &session_secret);
	~AgentControlBridge();

	AgentControlBridge(const AgentControlBridge &) = delete;
	AgentControlBridge &operator=(const AgentControlBridge &) = delete;

	void step();
	void setClientState(AgentClientState state);
	AgentClientState getClientState() const { return m_state; }
	bool isRegistered() const { return m_registered; }
	bool takeAction(AgentAction *action);
	void activateActionControls(const AgentAction &action);
	void reportActionStatus(const std::string &action_id, AgentActionStatus status,
			const char *code = nullptr);
	void publishObservation(const Json::Value &observation);
	bool isKeyDown(GameKeyType key) const;
	bool wasKeyPressed(GameKeyType key);
	bool wasKeyReleased(GameKeyType key);
	void clearTransitions();

	float movementSpeed() const;
	float movementDirection() const;
	float yaw() const { return m_yaw; }
	float pitch() const { return m_pitch; }
	bool observationStreamEnabled() const { return m_observation_stream; }

private:
	friend class TestAgentControlBridge;

	void readAvailable();
	void flushOutput();
	void applyLine(const std::string &line);
	void applyKey(GameKeyType key, bool down);
	void stopMovement();
	void sendHello();
	void sendMessage(const std::string &message);
	void sendReply(const std::string &action_id, const char *status, const char *code = nullptr);
	bool parseAction(const Json::Value &root, AgentAction *action, const char **code) const;
	void rememberActionId(const std::string &action_id);

	int m_socket = -1;
	std::string m_session_id;
	std::string m_session_secret;
	std::string m_buffer;
	std::string m_output_buffer;
	std::array<bool, GameKeyType::INTERNAL_ENUM_COUNT> m_down{};
	std::array<bool, GameKeyType::INTERNAL_ENUM_COUNT> m_pressed{};
	std::array<bool, GameKeyType::INTERNAL_ENUM_COUNT> m_released{};
	float m_yaw = 0.0f;
	float m_pitch = 0.0f;
	float m_movement_speed = 0.0f;
	float m_movement_direction = 0.0f;
	std::chrono::steady_clock::time_point m_control_deadline{};
	std::optional<AgentAction> m_pending_action;
	std::string m_active_action_id;
	std::array<std::string, 256> m_recent_action_ids;
	size_t m_recent_action_cursor = 0;
	u64 m_observation_sequence = 0;
	AgentClientState m_state = AgentClientState::BOOTING;
	bool m_hello_sent = false;
	bool m_registered = false;
	bool m_observation_stream = false;
};

#endif
