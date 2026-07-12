#pragma once

// A fake ReporterBackend for unit tests: it records everything the Reporter
// sends and serves canned bytes for every read. Wrap it in a real Reporter
// (via the unique_ptr<ReporterBackend> ctor) to drive Reporter and PadDevice
// logic without any real HID device.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

#include <Model/Reporter.h>

namespace adp
{

class RecordingBackend : public ReporterBackend
{
  public:
	// Every outgoing feature-report / write payload, in order.
	std::vector<std::vector<uint8_t>> sent;

	// Canned responses served FIFO to get_feature_report()/read().
	struct Response
	{
		std::vector<uint8_t> bytes; // copied into the caller's buffer
		int returnValue;            // what the backend call returns (bytes read)
	};
	std::deque<Response> reads;

	// Override the return value of send_feature_report()/write() to exercise
	// the Reporter's failure paths. When unset, they return the payload length.
	std::optional<int> sendFeatureReturn;
	std::optional<int> writeReturn;

	// Queue a report struct as a well-formed read (returns its exact size).
	// NOTE: matches the non-MINGW expected-size (sizeof(T)); the MINGW build
	// path expects sizeof(T)+1, which these tests do not target.
	template <typename T> void QueueGet(const T& report) { QueueRead(&report, sizeof(T), (int)sizeof(T)); }

	// Queue raw bytes plus the value the backend call should return.
	void QueueRead(const void* data, size_t nbytes, int returnValue)
	{
		Response r;
		const uint8_t* p = static_cast<const uint8_t*>(data);
		r.bytes.assign(p, p + nbytes);
		r.returnValue = returnValue;
		reads.push_back(std::move(r));
	}

	// Queue a read that returns a byte count with no meaningful content
	// (e.g. 0 for NO_DATA, negative for a HID failure).
	void QueueReadCount(int returnValue) { reads.push_back(Response{{}, returnValue}); }

	int get_feature_report(unsigned char* data, size_t length) override { return ServeRead(data, length); }

	int read(unsigned char* data, size_t length) override { return ServeRead(data, length); }

	int send_feature_report(const unsigned char* data, size_t length) override
	{
		sent.emplace_back(data, data + length);
		return sendFeatureReturn.value_or((int)length);
	}

	int write(unsigned char* data, size_t length) override
	{
		sent.emplace_back(data, data + length);
		return writeReturn.value_or((int)length);
	}

	const wchar_t* error() override { return L"RecordingBackend"; }

  private:
	int ServeRead(unsigned char* data, size_t length)
	{
		if (reads.empty())
			return 0; // nothing queued -> looks like NO_DATA

		Response r = std::move(reads.front());
		reads.pop_front();
		size_t n = std::min(length, r.bytes.size());
		if (n > 0)
			std::memcpy(data, r.bytes.data(), n);
		return r.returnValue;
	}
};

} // namespace adp
