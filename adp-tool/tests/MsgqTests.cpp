#include <catch2/catch_test_macros.hpp>

#include <string>

#include "MSGQ.hpp"

TEST_CASE("tryPop returns nullopt when empty", "[msgq]")
{
	MSGQ<int> q;
	CHECK_FALSE(q.tryPop().has_value());
}

TEST_CASE("FIFO ordering is preserved", "[msgq]")
{
	MSGQ<int> q;
	q.push(1);
	q.push(2);
	q.push(3);
	CHECK(q.tryPop() == 1);
	CHECK(q.tryPop() == 2);
	CHECK(q.tryPop() == 3);
	CHECK_FALSE(q.tryPop().has_value());
}

TEST_CASE("carries QueueMessage by value with its sender", "[msgq]")
{
	MSGQ<QueueMessage> q;
	q.push(QueueMessage("hello", "conn-1"));
	auto msg = q.tryPop();
	REQUIRE(msg.has_value());
	CHECK(msg->data == "hello");
	CHECK(msg->sender == "conn-1");
}
