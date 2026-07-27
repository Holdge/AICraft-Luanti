// AICraft-Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Modified for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.

#include "agentcontrolbridge.h"

#ifdef AICRAFT_AGENT_CLIENT

#include "convert_json.h"
#include "exceptions.h"
#include "log.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <json/json.h>
#include <limits>
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr size_t MAX_CONTROL_BUFFER = 64 * 1024;
constexpr size_t MAX_OUTPUT_BUFFER = 64 * 1024;

const char *stateName(AgentClientState state)
{
	switch (state) {
	case AgentClientState::BOOTING:
		return "BOOTING";
	case AgentClientState::AUTHENTICATING:
		return "AUTHENTICATING";
	case AgentClientState::MEDIA_SYNC:
		return "MEDIA_SYNC";
	case AgentClientState::PLAYER_READY:
		return "PLAYER_READY";
	case AgentClientState::CONTROL_READY:
		return "CONTROL_READY";
	}
	return "UNKNOWN";
}

bool readJson(const std::string &line, Json::Value *value, std::string *error)
{
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	return reader->parse(line.data(), line.data() + line.size(), value, error);
}

bool jsonBool(const Json::Value &root, const char *name)
{
	return root.isMember(name) && root[name].isBool() && root[name].asBool();
}

bool boundedString(const Json::Value &value, size_t minimum, size_t maximum)
{
	return value.isString() && value.asString().size() >= minimum &&
			value.asString().size() <= maximum;
}

bool boundedInteger(const Json::Value &value, s64 minimum, s64 maximum, s64 *result)
{
	if (!value.isIntegral())
		return false;
	const s64 parsed = value.asInt64();
	if (parsed < minimum || parsed > maximum)
		return false;
	*result = parsed;
	return true;
}

bool finiteNumber(const Json::Value &value, double minimum, double maximum, f32 *result)
{
	if (!value.isNumeric())
		return false;
	const double parsed = value.asDouble();
	if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum)
		return false;
	*result = static_cast<f32>(parsed);
	return true;
}

bool nodeVector(const Json::Value &value, v3s16 *result)
{
	if (!value.isObject())
		return false;

	s64 coordinates[3];
	const char *names[] = {"x", "y", "z"};
	for (size_t i = 0; i < 3; ++i) {
		if (!boundedInteger(value[names[i]], std::numeric_limits<s16>::min(),
					std::numeric_limits<s16>::max(), &coordinates[i]))
			return false;
	}
	*result = v3s16(static_cast<s16>(coordinates[0]), static_cast<s16>(coordinates[1]),
			static_cast<s16>(coordinates[2]));
	return true;
}

bool inventoryEndpoint(const Json::Value &value, AgentInventoryEndpoint *result)
{
	if (!value.isObject() || !boundedString(value["inventory"], 1, 128) ||
			!boundedString(value["list"], 1, 128))
		return false;
	s64 slot;
	if (!boundedInteger(value["slot"], 0, std::numeric_limits<s16>::max(), &slot))
		return false;
	result->inventory = value["inventory"].asString();
	result->list = value["list"].asString();
	result->slot = static_cast<u16>(slot);
	return true;
}

const char *statusName(AgentActionStatus status)
{
	switch (status) {
	case AgentActionStatus::EXECUTING:
		return "executing";
	case AgentActionStatus::SUCCEEDED:
		return "succeeded";
	case AgentActionStatus::REJECTED:
		return "rejected";
	case AgentActionStatus::EXPIRED:
		return "expired";
	}
	return "rejected";
}

} // namespace

AgentControlBridge::AgentControlBridge(const std::string &socket_path,
		const std::string &session_id, const std::string &session_secret) :
		m_session_id(session_id),
		m_session_secret(session_secret)
{
	if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path))
		throw BaseException("Invalid AICraft Agent control socket path");
	if (session_id.empty() || session_id.size() > 128)
		throw BaseException("Invalid AICraft Agent session identifier");
	if (session_secret.size() < 32)
		throw BaseException("AICraft Agent session secret must contain at least 32 characters");
	if (session_secret.size() > 256)
		throw BaseException("AICraft Agent session secret must not exceed 256 characters");

	m_socket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (m_socket < 0)
		throw BaseException("Failed to create AICraft Agent control socket");

	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
	if (connect(m_socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
		const std::string message = "Failed to connect AICraft Agent control socket: " +
				std::string(std::strerror(errno));
		close(m_socket);
		m_socket = -1;
		throw BaseException(message);
	}

	const int flags = fcntl(m_socket, F_GETFL, 0);
	if (flags < 0 || fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) != 0) {
		close(m_socket);
		m_socket = -1;
		throw BaseException("Failed to configure AICraft Agent control socket");
	}

	infostream << "AICraft Agent state: " << stateName(m_state)
			<< " session=" << m_session_id << std::endl;
}

AgentControlBridge::~AgentControlBridge()
{
	std::fill(m_session_secret.begin(), m_session_secret.end(), '\0');
	std::fill(m_output_buffer.begin(), m_output_buffer.end(), '\0');
	if (m_socket >= 0)
		close(m_socket);
}

void AgentControlBridge::sendMessage(const std::string &message)
{
	if (m_output_buffer.size() + message.size() + 1 > MAX_OUTPUT_BUFFER)
		throw BaseException("AICraft Agent control output buffer exceeded 64 KiB");
	m_output_buffer.append(message);
	m_output_buffer.push_back('\n');
	flushOutput();
}

void AgentControlBridge::flushOutput()
{
	while (!m_output_buffer.empty()) {
		const ssize_t length = send(m_socket, m_output_buffer.data(),
				m_output_buffer.size(), MSG_NOSIGNAL);
		if (length > 0) {
			std::fill(m_output_buffer.begin(),
					m_output_buffer.begin() + length, '\0');
			m_output_buffer.erase(0, static_cast<size_t>(length));
			continue;
		}
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		throw BaseException("AICraft Agent control socket write failed");
	}
}

void AgentControlBridge::sendHello()
{
	if (m_hello_sent)
		return;

	Json::Value hello(Json::objectValue);
	hello["request_id"] = "engine-hello";
	hello["type"] = "agent.hello";
	hello["protocol"] = "aicraft-control-v1";
	hello["session_id"] = m_session_id;
	hello["state"] = stateName(m_state);
	hello["ready"] = true;
	hello["session_secret"] = m_session_secret;
	hello["capabilities"] = Json::arrayValue;
	hello["capabilities"].append("action.lifecycle.v1");
	hello["capabilities"].append("observation.client.v1");
	sendMessage(fastWriteJson(hello));
	m_hello_sent = true;
}

void AgentControlBridge::setClientState(AgentClientState state)
{
	if (state == m_state)
		return;
	if (static_cast<int>(state) != static_cast<int>(m_state) + 1) {
		throw BaseException(std::string("Invalid AICraft Agent state transition from ") +
				stateName(m_state) + " to " + stateName(state));
	}

	m_state = state;
	infostream << "AICraft Agent state: " << stateName(m_state)
			<< " session=" << m_session_id << std::endl;
	if (m_state == AgentClientState::CONTROL_READY)
		sendHello();
}

void AgentControlBridge::applyKey(GameKeyType key, bool down)
{
	const size_t index = static_cast<size_t>(key);
	if (m_down[index] == down)
		return;
	m_down[index] = down;
	if (down)
		m_pressed[index] = true;
	else
		m_released[index] = true;
}

void AgentControlBridge::stopMovement()
{
	applyKey(KeyType::FORWARD, false);
	applyKey(KeyType::BACKWARD, false);
	applyKey(KeyType::LEFT, false);
	applyKey(KeyType::RIGHT, false);
	applyKey(KeyType::JUMP, false);
	applyKey(KeyType::SNEAK, false);
	applyKey(KeyType::DIG, false);
	applyKey(KeyType::PLACE, false);
}

void AgentControlBridge::sendReply(const std::string &action_id, const char *status, const char *code)
{
	Json::Value reply(Json::objectValue);
	reply["type"] = "action.status";
	reply["action_id"] = action_id;
	reply["session_id"] = m_session_id;
	reply["status"] = status;
	if (code)
		reply["code"] = code;
	sendMessage(fastWriteJson(reply));
}

bool AgentControlBridge::parseAction(
		const Json::Value &root, AgentAction *action, const char **code) const
{
	const std::string type = root.get("type", "").asString();
	action->action_id = root["action_id"].asString();

	if (type == "move") {
		action->type = AgentActionType::MOVE;
		if (root["direction"].isObject()) {
			if (!finiteNumber(root["direction"]["forward"], -1.0, 1.0,
						&action->forward) ||
					!finiteNumber(root["direction"]["right"], -1.0, 1.0,
						&action->right)) {
				*code = "INVALID_MOVE";
				return false;
			}
		} else {
			action->forward = jsonBool(root, "forward") ? 1.0f : 0.0f;
			if (jsonBool(root, "backward"))
				action->forward -= 1.0f;
			action->right = jsonBool(root, "right") ? 1.0f : 0.0f;
			if (jsonBool(root, "left"))
				action->right -= 1.0f;
		}
		action->jump = jsonBool(root, "jump");
		action->sneak = jsonBool(root, "sneak");
		s64 duration;
		if (!boundedInteger(root["duration_ms"], 50, 2000, &duration)) {
			*code = "INVALID_DURATION";
			return false;
		}
		action->duration_ms = static_cast<s32>(duration);
		return true;
	}

	if (type == "look") {
		action->type = AgentActionType::LOOK;
		if (!finiteNumber(root["yaw"], -360.0, 360.0, &action->yaw) ||
				!finiteNumber(root["pitch"], -90.0, 90.0, &action->pitch)) {
			*code = "INVALID_LOOK";
			return false;
		}
		action->duration_ms = 1000;
		return true;
	}

	if (type == "dig" || type == "place" || type == "use" || type == "attack") {
		if (type == "dig")
			action->type = AgentActionType::DIG;
		else if (type == "place")
			action->type = AgentActionType::PLACE;
		else if (type == "use")
			action->type = AgentActionType::USE;
		else
			action->type = AgentActionType::ATTACK;

		const Json::Value &target = root["target"];
		if (!target.isObject()) {
			*code = "INVALID_TARGET";
			return false;
		}
		const std::string kind = target.get("kind", "").asString();
		if (kind == "entity") {
			if (!boundedString(target["id"], 1, 128)) {
				*code = "INVALID_TARGET_ID";
				return false;
			}
			action->target_type = AgentTargetType::ENTITY;
			action->entity_id = target["id"].asString();
		} else {
			const Json::Value &position = kind == "node" ? target["position"] : target;
			if (!nodeVector(position, &action->target_position)) {
				*code = "INVALID_TARGET_POSITION";
				return false;
			}
			action->target_type = AgentTargetType::NODE;
			if (kind == "node" && target.isMember("face")) {
				if (!nodeVector(target["face"], &action->target_face) ||
						std::abs(action->target_face.X) +
								std::abs(action->target_face.Y) +
								std::abs(action->target_face.Z) != 1) {
					*code = "INVALID_TARGET_FACE";
					return false;
				}
				action->has_target_face = true;
			}
		}
		if (root.isMember("item")) {
			if (!boundedString(root["item"], 1, 128)) {
				*code = "INVALID_ITEM";
				return false;
			}
			action->item = root["item"].asString();
		}
		action->duration_ms = 5000;
		return true;
	}

	if (type == "craft") {
		action->type = AgentActionType::CRAFT;
		if (!boundedString(root["recipe"], 1, 128)) {
			*code = "INVALID_RECIPE";
			return false;
		}
		s64 count;
		if (!boundedInteger(root["count"], 1, 99, &count)) {
			*code = "INVALID_COUNT";
			return false;
		}
		action->recipe = root["recipe"].asString();
		action->count = static_cast<u16>(count);
		action->duration_ms = 5000;
		return true;
	}

	if (type == "equip") {
		action->type = AgentActionType::EQUIP;
		s64 slot;
		if (!boundedInteger(root["slot"], 0, 255, &slot)) {
			*code = "INVALID_SLOT";
			return false;
		}
		action->slot = static_cast<u16>(slot);
		action->duration_ms = 1000;
		return true;
	}

	if (type == "chat") {
		action->type = AgentActionType::CHAT;
		if (!boundedString(root["message"], 1, 240)) {
			*code = "INVALID_CHAT_MESSAGE";
			return false;
		}
		action->message = root["message"].asString();
		action->duration_ms = 5000;
		return true;
	}

	if (type == "wait") {
		action->type = AgentActionType::WAIT;
		s64 duration;
		if (!boundedInteger(root["duration_ms"], 50, 5000, &duration)) {
			*code = "INVALID_DURATION";
			return false;
		}
		action->duration_ms = static_cast<s32>(duration);
		return true;
	}

	if (type == "inventory_move" || type == "container_move") {
		action->type = type == "inventory_move" ? AgentActionType::INVENTORY_MOVE :
				AgentActionType::CONTAINER_MOVE;
		if (!inventoryEndpoint(root["from"], &action->from) ||
				!inventoryEndpoint(root["to"], &action->to)) {
			*code = "INVALID_INVENTORY_ENDPOINT";
			return false;
		}
		s64 count;
		if (!boundedInteger(root["count"], 1, std::numeric_limits<u16>::max(), &count)) {
			*code = "INVALID_COUNT";
			return false;
		}
		action->count = static_cast<u16>(count);
		action->duration_ms = 5000;
		return true;
	}

	if (type == "formspec_submit") {
		action->type = AgentActionType::FORMSPEC_SUBMIT;
		if (!boundedString(root["form_name"], 1, 128) || !root["fields"].isObject() ||
				root["fields"].size() > 256) {
			*code = "INVALID_FORMSPEC";
			return false;
		}
		action->form_name = root["form_name"].asString();
		for (const std::string &name : root["fields"].getMemberNames()) {
			if (name.empty() || name.size() > 128) {
				*code = "INVALID_FORMSPEC_FIELD";
				return false;
			}
			const Json::Value &value = root["fields"][name];
			std::string encoded;
			if (value.isString())
				encoded = value.asString();
			else if (value.isBool())
				encoded = value.asBool() ? "true" : "false";
			else if (value.isNumeric())
				encoded = value.asString();
			else {
				*code = "INVALID_FORMSPEC_FIELD";
				return false;
			}
			if (encoded.size() > 4096) {
				*code = "INVALID_FORMSPEC_FIELD";
				return false;
			}
			action->fields.emplace(name, std::move(encoded));
		}
		action->duration_ms = 5000;
		return true;
	}

	*code = "UNSUPPORTED_ACTION";
	return false;
}

void AgentControlBridge::rememberActionId(const std::string &action_id)
{
	m_recent_action_ids[m_recent_action_cursor] = action_id;
	m_recent_action_cursor = (m_recent_action_cursor + 1) % m_recent_action_ids.size();
}

void AgentControlBridge::applyLine(const std::string &line)
{
	Json::Value root;
	std::string error;
	if (!readJson(line, &root, &error) || !root.isObject()) {
		warningstream << "Rejected invalid AICraft Agent control JSON: " << error << std::endl;
		return;
	}

	const std::string type = root.get("type", "").asString();
	if (type == "agent.registered") {
		const std::string registered_session = root.get("session_id", "").asString();
		if (!m_hello_sent || m_state != AgentClientState::CONTROL_READY)
			throw BaseException("AICraft Agent registered before CONTROL_READY");
		if (!registered_session.empty() && registered_session != m_session_id)
			throw BaseException("AICraft Agent registered with a mismatched session");
		m_registered = true;
		m_observation_stream = root.get("observation_stream", false).asBool();
		infostream << "AICraft Agent registered: session=" << m_session_id << std::endl;
		return;
	}
	const std::string action_id = root.get("action_id", "").asString();
	if (action_id.empty()) {
		warningstream << "Rejected AICraft Agent control without action_id" << std::endl;
		return;
	}
	if (!m_registered) {
		sendReply(action_id, "rejected", "AGENT_NOT_REGISTERED");
		return;
	}
	for (const std::string &recent : m_recent_action_ids) {
		if (recent == action_id) {
			sendReply(action_id, "rejected", "DUPLICATE_ACTION");
			return;
		}
	}
	if (m_pending_action || !m_active_action_id.empty()) {
		sendReply(action_id, "rejected", "ACTION_BUSY");
		return;
	}

	AgentAction action;
	const char *code = nullptr;
	if (!parseAction(root, &action, &code)) {
		rememberActionId(action_id);
		sendReply(action_id, "rejected", code);
		return;
	}
	rememberActionId(action_id);
	m_pending_action = std::move(action);
	sendReply(action_id, "accepted");
}

bool AgentControlBridge::takeAction(AgentAction *action)
{
	if (!m_pending_action)
		return false;
	if (!m_active_action_id.empty())
		throw BaseException("AICraft Agent action queue invariant failed");
	*action = std::move(*m_pending_action);
	m_pending_action.reset();
	m_active_action_id = action->action_id;
	return true;
}

void AgentControlBridge::activateActionControls(const AgentAction &action)
{
	stopMovement();
	m_movement_speed = 0.0f;
	m_movement_direction = 0.0f;

	if (action.type == AgentActionType::MOVE) {
		m_movement_speed = std::min(1.0f,
				std::sqrt(action.forward * action.forward + action.right * action.right));
		if (m_movement_speed > 0.0f)
			m_movement_direction = std::atan2(action.right, action.forward);
		applyKey(KeyType::JUMP, action.jump);
		applyKey(KeyType::SNEAK, action.sneak);
	} else if (action.type == AgentActionType::LOOK) {
		m_yaw = action.yaw;
		m_pitch = action.pitch;
	}
}

void AgentControlBridge::reportActionStatus(const std::string &action_id,
		AgentActionStatus status, const char *code)
{
	if (m_active_action_id != action_id)
		throw BaseException("AICraft Agent action status does not match active action");
	sendReply(action_id, statusName(status), code);
	if (status != AgentActionStatus::EXECUTING) {
		stopMovement();
		m_movement_speed = 0.0f;
		m_movement_direction = 0.0f;
		m_active_action_id.clear();
	}
}

void AgentControlBridge::publishObservation(const Json::Value &observation)
{
	if (!m_registered || !m_observation_stream ||
			m_state != AgentClientState::CONTROL_READY)
		return;

	Json::Value envelope(Json::objectValue);
	envelope["type"] = "agent.observation";
	envelope["protocol"] = "aicraft-control-v1";
	envelope["session_id"] = m_session_id;
	envelope["sequence"] = Json::UInt64(++m_observation_sequence);
	envelope["observation"] = observation;
	sendMessage(fastWriteJson(envelope));
}

void AgentControlBridge::readAvailable()
{
	char chunk[4096];
	while (true) {
		const ssize_t length = recv(m_socket, chunk, sizeof(chunk), 0);
		if (length > 0) {
			m_buffer.append(chunk, static_cast<size_t>(length));
			if (m_buffer.size() > MAX_CONTROL_BUFFER)
				throw BaseException("AICraft Agent control buffer exceeded 64 KiB");
			continue;
		}
		if (length == 0)
			throw BaseException("AICraft Agent control socket disconnected");
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		if (errno == EINTR)
			continue;
		throw BaseException("AICraft Agent control socket read failed");
	}

	size_t newline;
	while ((newline = m_buffer.find('\n')) != std::string::npos) {
		const std::string line = m_buffer.substr(0, newline);
		m_buffer.erase(0, newline + 1);
		if (!line.empty())
			applyLine(line);
	}
}

void AgentControlBridge::step()
{
	flushOutput();
	readAvailable();
	flushOutput();
}

bool AgentControlBridge::isKeyDown(GameKeyType key) const
{
	return m_down[static_cast<size_t>(key)];
}

bool AgentControlBridge::wasKeyPressed(GameKeyType key)
{
	const size_t index = static_cast<size_t>(key);
	const bool result = m_pressed[index];
	m_pressed[index] = false;
	return result;
}

bool AgentControlBridge::wasKeyReleased(GameKeyType key)
{
	const size_t index = static_cast<size_t>(key);
	const bool result = m_released[index];
	m_released[index] = false;
	return result;
}

void AgentControlBridge::clearTransitions()
{
	m_pressed.fill(false);
	m_released.fill(false);
}

float AgentControlBridge::movementSpeed() const
{
	return m_movement_speed;
}

float AgentControlBridge::movementDirection() const
{
	return m_movement_direction;
}

#endif
