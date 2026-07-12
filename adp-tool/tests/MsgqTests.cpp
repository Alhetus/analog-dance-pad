#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>

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

TEST_CASE("carries move-only-ish value types", "[msgq]")
{
	MSGQ<QueueMessage> q;
	q.push(QueueMessage("hello"));
	auto msg = q.waitPop();
	REQUIRE(msg.has_value());
	CHECK(msg->data == "hello");
}

TEST_CASE("waitPop blocks then wakes on push", "[msgq]")
{
	MSGQ<int> q;
	std::atomic<bool> gotValue{false};

	std::thread consumer([&] {
		auto v = q.waitPop();
		if (v == 42)
		{
			gotValue = true;
		}
	});

	q.push(42);
	consumer.join();
	CHECK(gotValue.load());
}

TEST_CASE("stop() wakes a blocked waiter with nullopt", "[msgq]")
{
	MSGQ<int> q;
	std::atomic<bool> returnedEmpty{false};

	std::thread consumer([&] {
		auto v = q.waitPop();
		if (!v.has_value())
		{
			returnedEmpty = true;
		}
	});

	q.stop();
	consumer.join();
	CHECK(returnedEmpty.load());
	CHECK(q.isStopped());
}

TEST_CASE("stop() still drains remaining items before signalling empty", "[msgq]")
{
	MSGQ<int> q;
	q.push(7);
	q.stop();
	// Queued work is not lost on stop.
	CHECK(q.waitPop() == 7);
	// Now empty + stopped -> nullopt.
	CHECK_FALSE(q.waitPop().has_value());
}
