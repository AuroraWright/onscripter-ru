/**
 * WorkScheduler.hpp
 * Deadline-based work scheduling and interruptible, non-spinning waits.
 * Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>

class WorkSchedule {
public:
	static constexpr uint64_t Never = std::numeric_limits<uint64_t>::max();
	static constexpr uint64_t Millisecond = 1000000;

	// Producers declare when they next need service. Unknown/custom work can
	// request 0 to retain frame-rate updates; event-only work returns Never.
	void requireAfter(uint64_t nanos) { next = std::min(next, nanos); }
	uint64_t delay(uint64_t maximum) const { return std::min(next, maximum); }

	static uint64_t now() {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
		           std::chrono::steady_clock::now().time_since_epoch()).count();
	}

private:
	uint64_t next{Never};
};

class WorkSignal {
public:
	WorkSignal();
	~WorkSignal();
	WorkSignal(const WorkSignal &) = delete;
	WorkSignal &operator=(const WorkSignal &) = delete;

	uint64_t snapshot() {
		std::lock_guard<std::mutex> lock(mutex);
		return generation;
	}
	void notify();
	// Snapshot BEFORE checking the work predicate. A completion between that
	// check and this wait then cannot be lost. Notifications are coalesced.
	// One consumer waits; any number of producers may notify it.
	bool waitUntil(uint64_t observed, uint64_t deadline);

private:
	std::mutex mutex;
	std::condition_variable condition;
	uint64_t generation{0};
#ifdef _WIN32
	void *wakeEvent{nullptr};
	void *timer{nullptr};
#endif
};

extern WorkSignal mainThreadWork;
