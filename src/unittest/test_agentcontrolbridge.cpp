// AICraft-Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Added for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.

#include "test.h"

#if defined(AICRAFT_AGENT_CLIENT) && !defined(_WIN32)

#include "client/agentcontrolbridge.h"
#include "exceptions.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <json/json.h>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

class ScopedSocket
{
public:
	explicit ScopedSocket(int fd = -1) : m_fd(fd) {}
	~ScopedSocket()
	{
		if (m_fd >= 0)
			close(m_fd);
	}
	int get() const { return m_fd; }
	void reset(int fd)
	{
		if (m_fd >= 0)
			close(m_fd);
		m_fd = fd;
	}

private:
	int m_fd;
};

Json::Value parseJson(const std::string &encoded)
{
	Json::Value value;
	std::string error;
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	UASSERT(reader->parse(encoded.data(), encoded.data() + encoded.size(),
			&value, &error));
	return value;
}

void sendLine(int fd, const std::string &line)
{
	const std::string encoded = line + "\n";
	UASSERTEQ(ssize_t, send(fd, encoded.data(), encoded.size(), 0),
			static_cast<ssize_t>(encoded.size()));
}

std::string receiveLine(int fd)
{
	pollfd event {fd, POLLIN, 0};
	UASSERTEQ(int, poll(&event, 1, 1000), 1);
	UASSERT((event.revents & POLLIN) != 0);
	char buffer[4096];
	const ssize_t length = recv(fd, buffer, sizeof(buffer), 0);
	UASSERT(length > 0);
	const std::string encoded(buffer, static_cast<size_t>(length));
	const size_t newline = encoded.find('\n');
	UASSERT(newline != std::string::npos);
	return encoded.substr(0, newline);
}

class BridgeFixture
{
public:
	explicit BridgeFixture(std::string socket_path_) :
			socket_path(std::move(socket_path_)),
			listener(socket(AF_UNIX, SOCK_STREAM, 0))
	{
		UASSERT(listener.get() >= 0);
		sockaddr_un address {};
		address.sun_family = AF_UNIX;
		UASSERT(socket_path.size() < sizeof(address.sun_path));
		std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
		UASSERTEQ(int, bind(listener.get(), reinterpret_cast<sockaddr *>(&address),
				sizeof(address)), 0);
		UASSERTEQ(int, listen(listener.get(), 1), 0);

		bridge = std::make_unique<AgentControlBridge>(
				socket_path, "test-session", session_secret);
		peer.reset(::accept(listener.get(), nullptr, nullptr));
		UASSERT(peer.get() >= 0);
		const int flags = fcntl(peer.get(), F_GETFL, 0);
		UASSERT(flags >= 0);
		UASSERTEQ(int, fcntl(peer.get(), F_SETFL, flags | O_NONBLOCK), 0);
	}

	~BridgeFixture()
	{
		bridge.reset();
		unlink(socket_path.c_str());
	}

	void registerReady(bool observation_stream = false)
	{
		bridge->setClientState(AgentClientState::AUTHENTICATING);
		bridge->setClientState(AgentClientState::MEDIA_SYNC);
		bridge->setClientState(AgentClientState::PLAYER_READY);
		bridge->setClientState(AgentClientState::CONTROL_READY);
		Json::Value hello = parseJson(receiveLine(peer.get()));
		UASSERTEQ(std::string, hello["type"].asString(), "agent.hello");
		sendLine(peer.get(), std::string(
				R"({"type":"agent.registered","session_id":"test-session","observation_stream":)") +
				(observation_stream ? "true}" : "false}"));
		bridge->step();
		UASSERT(bridge->isRegistered());
	}

	AgentAction takeAccepted(const std::string &line)
	{
		sendLine(peer.get(), line);
		bridge->step();
		Json::Value accepted = parseJson(receiveLine(peer.get()));
		UASSERTEQ(std::string, accepted["status"].asString(), "accepted");
		AgentAction action;
		UASSERT(bridge->takeAction(&action));
		return action;
	}

	void finish(const AgentAction &action)
	{
		bridge->reportActionStatus(action.action_id, AgentActionStatus::EXECUTING);
		Json::Value executing = parseJson(receiveLine(peer.get()));
		UASSERTEQ(std::string, executing["status"].asString(), "executing");
		bridge->reportActionStatus(action.action_id, AgentActionStatus::SUCCEEDED);
		Json::Value succeeded = parseJson(receiveLine(peer.get()));
		UASSERTEQ(std::string, succeeded["status"].asString(), "succeeded");
	}

	std::string socket_path;
	const std::string session_secret = "0123456789abcdef0123456789abcdef";
	ScopedSocket listener;
	ScopedSocket peer;
	std::unique_ptr<AgentControlBridge> bridge;
};

} // namespace

class TestAgentControlBridge : public TestBase
{
public:
	TestAgentControlBridge() { TestManager::registerTestModule(this); }
	const char *getName() override { return "TestAgentControlBridge"; }

	void runTests(IGameDef *) override
	{
		TEST(testReadyHandshake);
		TEST(testMovementAndViewActions);
		TEST(testTargetActions);
		TEST(testInventoryCraftAndEquipActions);
		TEST(testChatAndFormspecActions);
		TEST(testRejectsInvalidSessionSecret);
	}

	void testReadyHandshake();
	void testMovementAndViewActions();
	void testTargetActions();
	void testInventoryCraftAndEquipActions();
	void testChatAndFormspecActions();
	void testRejectsInvalidSessionSecret();
};

static TestAgentControlBridge g_test_instance;

void TestAgentControlBridge::testReadyHandshake()
{
	BridgeFixture fixture(getTestTempFile());

	char unexpected;
	UASSERTEQ(ssize_t, recv(fixture.peer.get(), &unexpected, 1, 0), -1);
	UASSERT(errno == EAGAIN || errno == EWOULDBLOCK);

	fixture.bridge->setClientState(AgentClientState::AUTHENTICATING);
	fixture.bridge->setClientState(AgentClientState::MEDIA_SYNC);
	fixture.bridge->setClientState(AgentClientState::PLAYER_READY);

	sendLine(fixture.peer.get(), R"({"type":"wait","action_id":"too-early"})");
	fixture.bridge->step();
	Json::Value early = parseJson(receiveLine(fixture.peer.get()));
	UASSERTEQ(std::string, early["status"].asString(), "rejected");
	UASSERTEQ(std::string, early["code"].asString(), "AGENT_NOT_REGISTERED");

	fixture.bridge->setClientState(AgentClientState::CONTROL_READY);
	Json::Value hello = parseJson(receiveLine(fixture.peer.get()));
	UASSERTEQ(std::string, hello["type"].asString(), "agent.hello");
	UASSERTEQ(std::string, hello["protocol"].asString(), "aicraft-control-v1");
	UASSERTEQ(std::string, hello["session_id"].asString(), "test-session");
	UASSERTEQ(std::string, hello["state"].asString(), "CONTROL_READY");
	UASSERTEQ(std::string, hello["session_secret"].asString(), fixture.session_secret);
	UASSERT(hello["ready"].asBool());
	UASSERTEQ(std::string, fixture.bridge->m_session_secret, fixture.session_secret);
	UASSERT(!fixture.bridge->isRegistered());

	sendLine(fixture.peer.get(),
			R"({"type":"agent.registered","session_id":"test-session","observation_stream":true})");
	fixture.bridge->step();
	UASSERT(fixture.bridge->isRegistered());
	UASSERT(fixture.bridge->observationStreamEnabled());

	AgentAction wait = fixture.takeAccepted(
			R"({"type":"wait","action_id":"wait-ready","duration_ms":100})");
	UASSERT(wait.type == AgentActionType::WAIT);
	fixture.finish(wait);

	Json::Value observation(Json::objectValue);
	observation["source"] = "test";
	fixture.bridge->publishObservation(observation);
	Json::Value published = parseJson(receiveLine(fixture.peer.get()));
	UASSERTEQ(std::string, published["type"].asString(), "agent.observation");
	UASSERTEQ(std::string,
			published["observation"]["source"].asString(), "test");
}

void TestAgentControlBridge::testMovementAndViewActions()
{
	BridgeFixture fixture(getTestTempFile());
	fixture.registerReady();

	AgentAction move = fixture.takeAccepted(
			R"({"type":"move","action_id":"move-1","direction":{"forward":0.5,"right":-0.25},"jump":true,"sneak":false,"duration_ms":250})");
	UASSERT(move.type == AgentActionType::MOVE);
	UASSERT(std::abs(move.forward - 0.5f) < 0.001f);
	UASSERT(std::abs(move.right + 0.25f) < 0.001f);
	UASSERT(move.jump);
	fixture.bridge->activateActionControls(move);
	UASSERT(fixture.bridge->movementSpeed() > 0.5f);
	fixture.finish(move);

	AgentAction look = fixture.takeAccepted(
			R"({"type":"look","action_id":"look-1","yaw":45,"pitch":-15})");
	UASSERT(look.type == AgentActionType::LOOK);
	fixture.bridge->activateActionControls(look);
	UASSERT(std::abs(fixture.bridge->yaw() - 45.0f) < 0.001f);
	UASSERT(std::abs(fixture.bridge->pitch() + 15.0f) < 0.001f);
	fixture.finish(look);

	AgentAction wait = fixture.takeAccepted(
			R"({"type":"wait","action_id":"wait-1","duration_ms":500})");
	UASSERT(wait.duration_ms == 500);
	fixture.finish(wait);
}

void TestAgentControlBridge::testTargetActions()
{
	BridgeFixture fixture(getTestTempFile());
	fixture.registerReady();

	AgentAction dig = fixture.takeAccepted(
			R"({"type":"dig","action_id":"dig-1","target":{"x":1,"y":2,"z":3}})");
	UASSERT(dig.type == AgentActionType::DIG);
	UASSERT(dig.target_type == AgentTargetType::NODE);
	UASSERT(dig.target_position == v3s16(1, 2, 3));
	fixture.finish(dig);

	AgentAction place = fixture.takeAccepted(
			R"({"type":"place","action_id":"place-1","target":{"kind":"node","position":{"x":4,"y":5,"z":6},"face":{"x":0,"y":1,"z":0}},"item":"mcl_core:cobble"})");
	UASSERT(place.type == AgentActionType::PLACE);
	UASSERT(place.has_target_face);
	UASSERT(place.target_face == v3s16(0, 1, 0));
	UASSERTEQ(std::string, place.item, "mcl_core:cobble");
	fixture.finish(place);

	AgentAction use = fixture.takeAccepted(
			R"({"type":"use","action_id":"use-1","target":{"kind":"entity","id":"object:42"}})");
	UASSERT(use.type == AgentActionType::USE);
	UASSERT(use.target_type == AgentTargetType::ENTITY);
	UASSERTEQ(std::string, use.entity_id, "object:42");
	fixture.finish(use);

	AgentAction attack = fixture.takeAccepted(
			R"({"type":"attack","action_id":"attack-1","target":{"kind":"entity","id":"43"}})");
	UASSERT(attack.type == AgentActionType::ATTACK);
	fixture.finish(attack);

	sendLine(fixture.peer.get(),
			R"({"type":"dig","action_id":"dig-invalid","target":{"kind":"node","position":{"x":1,"y":2,"z":3},"face":{"x":1,"y":1,"z":0}}})");
	fixture.bridge->step();
	Json::Value rejected = parseJson(receiveLine(fixture.peer.get()));
	UASSERTEQ(std::string, rejected["status"].asString(), "rejected");
	UASSERTEQ(std::string, rejected["code"].asString(), "INVALID_TARGET_FACE");
}

void TestAgentControlBridge::testInventoryCraftAndEquipActions()
{
	BridgeFixture fixture(getTestTempFile());
	fixture.registerReady();

	AgentAction craft = fixture.takeAccepted(
			R"({"type":"craft","action_id":"craft-1","recipe":"mcl_core:stick","count":2})");
	UASSERT(craft.type == AgentActionType::CRAFT);
	UASSERTEQ(std::string, craft.recipe, "mcl_core:stick");
	UASSERT(craft.count == 2);
	fixture.finish(craft);

	AgentAction equip = fixture.takeAccepted(
			R"({"type":"equip","action_id":"equip-1","slot":7})");
	UASSERT(equip.type == AgentActionType::EQUIP);
	UASSERT(equip.slot == 7);
	fixture.finish(equip);

	AgentAction inventory_move = fixture.takeAccepted(
			R"({"type":"inventory_move","action_id":"inventory-1","from":{"inventory":"current_player","list":"main","slot":0},"to":{"inventory":"current_player","list":"main","slot":1},"count":3})");
	UASSERT(inventory_move.type == AgentActionType::INVENTORY_MOVE);
	UASSERT(inventory_move.from.slot == 0);
	UASSERT(inventory_move.to.slot == 1);
	UASSERT(inventory_move.count == 3);
	fixture.finish(inventory_move);

	AgentAction container_move = fixture.takeAccepted(
			R"({"type":"container_move","action_id":"container-1","from":{"inventory":"nodemeta:1,2,3","list":"main","slot":0},"to":{"inventory":"current_player","list":"main","slot":2},"count":1})");
	UASSERT(container_move.type == AgentActionType::CONTAINER_MOVE);
	UASSERTEQ(std::string, container_move.from.inventory, "nodemeta:1,2,3");
	fixture.finish(container_move);
}

void TestAgentControlBridge::testChatAndFormspecActions()
{
	BridgeFixture fixture(getTestTempFile());
	fixture.registerReady();

	AgentAction chat = fixture.takeAccepted(
			R"({"type":"chat","action_id":"chat-1","message":"hello Mineclonia"})");
	UASSERT(chat.type == AgentActionType::CHAT);
	UASSERTEQ(std::string, chat.message, "hello Mineclonia");
	fixture.finish(chat);

	AgentAction formspec = fixture.takeAccepted(
			R"({"type":"formspec_submit","action_id":"formspec-1","form_name":"chest","fields":{"take":true,"amount":2,"label":"ok"}})");
	UASSERT(formspec.type == AgentActionType::FORMSPEC_SUBMIT);
	UASSERTEQ(std::string, formspec.form_name, "chest");
	UASSERTEQ(std::string, formspec.fields["take"], "true");
	UASSERTEQ(std::string, formspec.fields["amount"], "2");
	fixture.finish(formspec);
}

void TestAgentControlBridge::testRejectsInvalidSessionSecret()
{
	const std::string unused_socket_path = getTestTempFile();
	EXCEPTION_CHECK(BaseException,
			AgentControlBridge(unused_socket_path, "test-session", "short"));
	EXCEPTION_CHECK(BaseException,
			AgentControlBridge(unused_socket_path, "test-session",
					std::string(257, 'x')));
}

#endif
