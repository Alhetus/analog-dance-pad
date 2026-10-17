#include <catch2/catch_test_macros.hpp>

#include <Model/Device.h>
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
