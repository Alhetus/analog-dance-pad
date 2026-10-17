#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <Model/Device.h>
#include "Application.h"
#include "WebsocketServer.h"

using namespace adp;

// HandleClientMessage runs on the device-I/O thread and must be fully
// defensive: no device is connected here (Device::Init is never called in
// tests), so every message should be parsed/validated and then ignored without
// throwing. The config-apply path itself is covered by ProfileTests against
// PadDevice directly.
TEST_CASE("HandleClientMessage ignores bad input without throwing", "[handlemessage]")
{
	CHECK_NOTHROW(Device::HandleClientMessage("not json at all"));
	CHECK_NOTHROW(Device::HandleClientMessage(""));
	CHECK_NOTHROW(Device::HandleClientMessage("[1,2,3]"));          // valid JSON, not an object
	CHECK_NOTHROW(Device::HandleClientMessage("42"));               // valid JSON, not an object
	CHECK_NOTHROW(Device::HandleClientMessage("{\"sensors\":[]}")); // object, but no device
	CHECK_NOTHROW(Device::HandleClientMessage("{\"releaseMode\":1}"));
	CHECK_NOTHROW(Device::HandleClientMessage("{\"releaseMode\":99}")); // out of range
	CHECK_NOTHROW(Device::HandleClientMessage("{\"unhandled\":true}"));
	CHECK_NOTHROW(Device::HandleClientMessage("{\"pad\":\"nope\",\"releaseMode\":1}")); // unknown pad
	CHECK_NOTHROW(Device::HandleClientMessage("{\"pad\":42,\"sensors\":[]}"));          // non-string pad
}

TEST_CASE("HandleClientMessage only builds paths for known players", "[handlemessage]")
{
	// No scan has run, so no player is known: path-bearing requests are dropped,
	// including traversal attempts. An op on an unknown pad is answered.
	CHECK(Device::HandleClientMessage("{\"listProfiles\":{\"player\":\"../../etc\"}}").empty());
	CHECK(Device::HandleClientMessage("{\"listProfiles\":{\"player\":42}}").empty());
	CHECK(Device::HandleClientMessage("{\"listProfiles\":\"Mike\"}").empty());
	const auto replies =
	    Device::HandleClientMessage("{\"pad\":\"x\",\"loadProfile\":{\"player\":\"Mike\",\"id\":\"a\"}}");
	REQUIRE(replies.size() == 1);
	CHECK(replies[0].message["msgType"] == 8);
	CHECK(replies[0].message["ok"] == false);
	CHECK(replies[0].message["error"] == "pad not connected");
	CHECK_NOTHROW(Device::HandleClientMessage("{\"rescanPlayers\":true}"));
}

TEST_CASE("AccessAllowed checks the Origin allowlist and the token", "[websocket]")
{
	WebsocketAccess open;
	CHECK(AccessAllowed(open, "http://evil.example", "/"));

	WebsocketAccess origins;
	origins.allowedOrigins = {"http://cab1:5173"};
	CHECK(AccessAllowed(origins, "http://cab1:5173", "/"));
	CHECK_FALSE(AccessAllowed(origins, "http://evil.example", "/"));
	CHECK_FALSE(AccessAllowed(origins, "", "/"));

	WebsocketAccess token;
	token.token = "s3cret";
	CHECK(AccessAllowed(token, "", "/?token=s3cret"));
	CHECK(AccessAllowed(token, "", "/?a=1&token=s3cret&b=2"));
	CHECK_FALSE(AccessAllowed(token, "", "/"));
	CHECK_FALSE(AccessAllowed(token, "", "/?token=s3cretX"));
	CHECK_FALSE(AccessAllowed(token, "", "/?xtoken=s3cret"));
	CHECK_FALSE(AccessAllowed(token, "", "/?token="));

	WebsocketAccess encoded;
	encoded.token = "a b&c%";
	CHECK(AccessAllowed(encoded, "", "/?token=a%20b%26c%25"));
	CHECK(AccessAllowed(encoded, "", "/?token=a+b%26c%25#frag"));
	CHECK_FALSE(AccessAllowed(encoded, "", "/?token=a b&c%"));
}

TEST_CASE("Configure edits of one pad wait their turn without being overtaken", "[application]")
{
	using A = Application;
	const auto now = A::Clock::now();
	std::deque<A::Inbound> backlog = {
	    {"c1", {{"pad", "p1"}, {"sensors", json::array()}}},
	    {"c1", {{"pad", "p1"}, {"releaseMode", 0}}},
	    {"c1", {{"pad", "p2"}, {"sensors", json::array()}}},
	};
	std::map<std::string, A::Clock::time_point> last;
	CHECK(A::NextDue(backlog, last, {}, now) == 0);
	last["p1"] = now;
	CHECK(A::NextDue(backlog, last, {}, now) == 2); // p1's edit waits, and its releaseMode behind it
	CHECK(A::NextDue(backlog, last, {}, now + A::CONFIGURE_INTERVAL) == 0);
	// A pad saving in the background gets nothing at all until it is done.
	CHECK(A::NextDue(backlog, {}, {"p1"}, now) == 2);
	CHECK(A::NextDue(backlog, {}, {"p1", "p2"}, now) == std::string::npos);
	backlog.pop_back();
	CHECK(A::NextDue(backlog, last, {}, now) == std::string::npos);
}

TEST_CASE("Edits of one pad fold into the waiting one, whoever sent them", "[application]")
{
	using A = Application;
	std::deque<A::Inbound> backlog;
	REQUIRE(A::Enqueue(backlog, {"c1", {{"pad", "p1"}, {"sensors", {{{"threshold", 0.5}}}}, {"requestId", 1}}, {}}));
	REQUIRE(A::Enqueue(backlog, {"c2", {{"pad", "p1"}, {"sensors", {{{"releaseThreshold", 0.2}}}}}, {}}));
	REQUIRE(backlog.size() == 1);
	CHECK(backlog[0].message["sensors"][0] == json{{"threshold", 0.5}, {"releaseThreshold", 0.2}});
	// Answered as the later one, with a copy to the earlier asker.
	CHECK(backlog[0].sender == "c2");
	CHECK_FALSE(backlog[0].message.contains("requestId"));
	REQUIRE(backlog[0].folded.size() == 1);
	CHECK(backlog[0].folded[0] == A::Asker{"c1", 1});

	// The same asker again isn't owed a second copy.
	REQUIRE(A::Enqueue(backlog, {"c2", {{"pad", "p1"}, {"sensors", {json::object()}}}, {}}));
	CHECK(backlog[0].folded.size() == 1);

	// Nothing folds past another op for the pad.
	REQUIRE(A::Enqueue(backlog, {"c1", {{"pad", "p1"}, {"releaseMode", 0}}, {}}));
	REQUIRE(A::Enqueue(backlog, {"c1", {{"pad", "p1"}, {"sensors", {json::object()}}}, {}}));
	CHECK(backlog.size() == 3);

	while (backlog.size() < A::MAX_BACKLOG)
		backlog.push_back({"c", {{"pad", "p9"}, {"releaseMode", 0}}, {}});
	CHECK_FALSE(A::Enqueue(backlog, {"c1", {{"pad", "p2"}, {"releaseMode", 0}}, {}}));
	// A fold still fits: it doesn't grow the backlog.
	CHECK(A::Enqueue(backlog, {"c1", {{"pad", "p1"}, {"sensors", {json::object()}}}, {}}));
}

TEST_CASE("The server admits clients, queues their messages and forgets them on close", "[websocket]")
{
	using namespace std::chrono_literals;
	const auto waitFor = [](auto done) {
		for (int i = 0; i < 500; ++i, std::this_thread::sleep_for(10ms))
			if (done())
				return true;
		return false;
	};
	constexpr int port = 18008;
	ix::initNetSystem();
	MSGQ<QueueMessage> queue;
	std::atomic<bool> running{true};
	WebsocketServer server;
	WebsocketAccess access;
	access.token = "t";
	std::thread serverThread(&WebsocketServer::Init, &server, std::ref(queue), std::string("127.0.0.1"), access,
	                         std::ref(running), port);

	// Retries until the server listens.
	std::atomic<bool> hello{false};
	ix::WebSocket good;
	good.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/?token=t");
	good.setMinWaitBetweenReconnectionRetries(10);
	good.setOnMessageCallback([&](const ix::WebSocketMessagePtr& m) {
		if (m->type == ix::WebSocketMessageType::Message && json::parse(m->str)["msgType"] == 0)
			hello = true;
	});
	good.start();
	CHECK(waitFor([&] { return hello.load() && server.HasClients(); }));
	good.send(R"({"pad":"p1","releaseMode":0})");
	std::optional<QueueMessage> got;
	CHECK(waitFor([&] { return (got = queue.tryPop()).has_value(); }));
	if (got)
		CHECK(got->data == R"({"pad":"p1","releaseMode":0})");
	good.stop();
	CHECK(waitFor([&] { return !server.HasClients(); }));

	// Refused: closed with 4003 and never admitted.
	std::atomic<int> closeCode{0};
	ix::WebSocket bad;
	bad.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/");
	bad.disableAutomaticReconnection();
	bad.setOnMessageCallback([&](const ix::WebSocketMessagePtr& m) {
		if (m->type == ix::WebSocketMessageType::Close)
			closeCode = m->closeInfo.code;
	});
	bad.start();
	CHECK(waitFor([&] { return closeCode.load() != 0; }));
	CHECK(closeCode == 4003);
	CHECK_FALSE(server.HasClients());
	bad.stop();

	server.Stop();
	serverThread.join();
	ix::uninitNetSystem();
}
