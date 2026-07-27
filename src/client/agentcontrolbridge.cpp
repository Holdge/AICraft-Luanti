// AICraft-Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

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
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr size_t MAX_CONTROL_BUFFER = 64 * 1024;

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

} // namespace

AgentControlBridge::AgentControlBridge(const std::string &socket_path)
{
	if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path))
		throw BaseException("Invalid AICraft Agent control socket path");

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

	const std::string hello = "{\"request_id\":\"engine-hello\",\"type\":\"agent.hello\","
			"\"protocol\":\"aicraft-control-v1\"}\n";
	if (send(m_socket, hello.data(), hello.size(), 0) < 0)
		throw BaseException("Failed to register AICraft Agent control socket");
}

AgentControlBridge::~AgentControlBridge()
{
	if (m_socket >= 0)
		close(m_socket);
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
	reply["status"] = status;
	if (code)
		reply["code"] = code;
	const std::string encoded = fastWriteJson(reply) + "\n";
	if (send(m_socket, encoded.data(), encoded.size(), MSG_NOSIGNAL) < 0 &&
			errno != EAGAIN && errno != EWOULDBLOCK) {
		warningstream << "AICraft Agent bridge reply failed: " << std::strerror(errno) << std::endl;
	}
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
	const std::string action_id = root.get("action_id", "").asString();
	if (action_id.empty()) {
		warningstream << "Rejected AICraft Agent control without action_id" << std::endl;
		return;
	}

	if (type == "wait") {
		stopMovement();
	} else if (type == "look") {
		if (!root["yaw"].isNumeric() || !root["pitch"].isNumeric()) {
			sendReply(action_id, "rejected", "INVALID_LOOK");
			return;
		}
		m_yaw = std::clamp(root["yaw"].asFloat(), -360.0f, 360.0f);
		m_pitch = std::clamp(root["pitch"].asFloat(), -90.0f, 90.0f);
	} else if (type == "move") {
		stopMovement();
		applyKey(KeyType::FORWARD, jsonBool(root, "forward"));
		applyKey(KeyType::BACKWARD, jsonBool(root, "backward"));
		applyKey(KeyType::LEFT, jsonBool(root, "left"));
		applyKey(KeyType::RIGHT, jsonBool(root, "right"));
		applyKey(KeyType::JUMP, jsonBool(root, "jump"));
		applyKey(KeyType::SNEAK, jsonBool(root, "sneak"));
	} else if (type == "dig" || type == "attack") {
		applyKey(KeyType::DIG, true);
	} else if (type == "place" || type == "use") {
		applyKey(KeyType::PLACE, true);
	} else {
		sendReply(action_id, "rejected", "UNSUPPORTED_ACTION");
		return;
	}

	const int duration_ms = std::clamp(root.get("duration_ms", 100).asInt(), 50, 5000);
	m_control_deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(duration_ms);
	sendReply(action_id, "executing");
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
	readAvailable();
	if (m_control_deadline != std::chrono::steady_clock::time_point{} &&
			std::chrono::steady_clock::now() >= m_control_deadline) {
		stopMovement();
		m_control_deadline = {};
	}
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
	return 0.0f;
}

float AgentControlBridge::movementDirection() const
{
	return 0.0f;
}

#endif

