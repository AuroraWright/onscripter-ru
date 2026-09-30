/**
 * MemoryBudget.cpp
 * Consult LICENSE file for licensing terms and copyright holders.
 */
#include "Support/MemoryBudget.hpp"
#include "Support/WorkScheduler.hpp"
#include "Support/SDLCompat.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif

struct MemoryBudget::State {
	std::mutex mutex;
	Snapshot stats;
	size_t configured;
	Trim trim{Trim::None};
	size_t needed{0};
	uint64_t restrictedUntil{0}, lastPoll{0};
	explicit State(size_t bytes) : configured(bytes) { stats.limit = bytes; }
};

struct MemoryBudget::Charge {
	std::shared_ptr<State> state;
	Kind kind{Kind::Images};
	size_t bytes{0};
	~Charge() {
		if (state) {
			std::lock_guard<std::mutex> lock(state->mutex);
			state->stats.used -= bytes;
			state->stats.bytes[static_cast<size_t>(kind)] -= bytes;
		}
	}
};

MemoryBudget::MemoryBudget(size_t bytes) : state(std::make_shared<State>(bytes)) {}

MemoryBudget::Lease MemoryBudget::reserve(Kind kind, size_t bytes) {
	auto charge = std::make_shared<Charge>();
	std::lock_guard<std::mutex> lock(state->mutex);
	if (bytes > state->stats.limit || state->stats.used > state->stats.limit - bytes ||
	    (kind == Kind::Prefetch && state->restrictedUntil)) {
		++state->stats.denied;
		if (bytes <= state->stats.limit && !state->restrictedUntil) {
			if (state->trim == Trim::None || state->trim == Trim::Idle)
				state->trim = Trim::Budget;
			state->needed = std::max(state->needed, bytes);
		}
		return {};
	}
	state->stats.used += bytes;
	state->stats.bytes[static_cast<size_t>(kind)] += bytes;
	state->stats.peak = std::max(state->stats.peak, state->stats.used);
	charge->state = state;
	charge->kind = kind;
	charge->bytes = bytes;
	Lease lease;
	lease.charge = std::move(charge);
	return lease;
}

void MemoryBudget::Lease::shrinkTo(size_t bytes) {
	if (!charge)
		return;
	std::lock_guard<std::mutex> lock(charge->state->mutex);
	if (bytes < charge->bytes) {
		const size_t released = charge->bytes - bytes;
		charge->state->stats.used -= released;
		charge->state->stats.bytes[static_cast<size_t>(charge->kind)] -= released;
		charge->bytes = bytes;
	}
}

void MemoryBudget::Lease::reclassify(Kind kind) {
	if (!charge)
		return;
	std::lock_guard<std::mutex> lock(charge->state->mutex);
	charge->state->stats.bytes[static_cast<size_t>(charge->kind)] -= charge->bytes;
	charge->state->stats.bytes[static_cast<size_t>(kind)] += charge->bytes;
	charge->kind = kind;
}

MemoryBudget::Snapshot MemoryBudget::snapshot() const {
	std::lock_guard<std::mutex> lock(state->mutex);
	return state->stats;
}

size_t MemoryBudget::available() const {
	const auto stats = snapshot();
	return stats.used < stats.limit ? stats.limit - stats.used : 0;
}

size_t MemoryBudget::prefetchAllowance(size_t maximum, size_t copies) const {
	std::lock_guard<std::mutex> lock(state->mutex);
	if (!copies || state->restrictedUntil || state->stats.used >= state->stats.limit)
		return 0;
	return std::min(maximum, (state->stats.limit - state->stats.used) / copies);
}

void MemoryBudget::requestTrim(bool pressure, size_t needed) {
	std::lock_guard<std::mutex> lock(state->mutex);
	if (pressure) {
		state->stats.limit = state->configured / 4;
		state->restrictedUntil = WorkSchedule::now() + 30000000000ULL;
		state->trim = Trim::Pressure;
	} else if (state->trim == Trim::None || state->trim == Trim::Idle) {
		state->trim = Trim::Budget;
	}
	state->needed = std::max(state->needed, needed);
}

MemoryBudget::Trim MemoryBudget::takeTrimRequest(size_t *needed) {
	std::lock_guard<std::mutex> lock(state->mutex);
	const auto result = state->trim;
	if (needed)
		*needed = state->needed;
	state->needed = 0;
	state->trim = Trim::None;
	return result;
}

void MemoryBudget::pollPressure() {
	const uint64_t now = WorkSchedule::now();
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (now - state->lastPoll < 1000000000ULL)
			return;
		state->lastPoll = now;
		if (state->trim == Trim::None)
			state->trim = Trim::Idle;
		if (state->restrictedUntil && now >= state->restrictedUntil) {
			state->restrictedUntil = 0;
			state->stats.limit = state->configured;
		}
	}
	uint64_t available = 0, total = 0;
	bool measured = false;
#ifdef _WIN32
	MEMORYSTATUSEX status{};
	status.dwLength = sizeof(status);
	if (GlobalMemoryStatusEx(&status)) {
		measured = true;
		available = status.ullAvailPhys;
		total = status.ullTotalPhys;
	}
#elif defined(DROID) || defined(LINUX)
	if (FILE *file = std::fopen("/proc/meminfo", "r")) {
		char line[128];
		unsigned long long kb;
		while (std::fgets(line, sizeof(line), file)) {
			if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
				available = kb * 1024;
				measured = true;
			} else if (std::sscanf(line, "MemTotal: %llu kB", &kb) == 1)
				total = kb * 1024;
		}
		std::fclose(file);
	}
#endif
	if (measured && total && available < std::max<uint64_t>(128 * MiB, total / 10))
		requestTrim(true);
}

size_t MemoryBudget::bufferedItems(size_t itemBytes, size_t minimum, size_t maximum) const {
	if (!itemBytes)
		return minimum;
	return std::clamp(snapshot().limit / 4 / itemBytes, minimum, maximum);
}

MemoryBudget &memoryBudget() {
	static MemoryBudget budget([] {
		const size_t ramMB = static_cast<size_t>(std::max(SDL_GetSystemRAM(), 0));
		size_t mb = ramMB ? std::clamp(ramMB / 32, size_t{32}, size_t{128}) : 64;
		if (const char *value = onsSDLGetEnv("ONS_MEMORY_BUDGET_MB")) {
			char *end = nullptr;
			const long parsed = std::strtol(value, &end, 10);
			if (end != value && *end == '\0' && parsed >= 0 && parsed <= 1024)
				mb = static_cast<size_t>(parsed);
		}
		return mb * MemoryBudget::MiB;
	}());
	return budget;
}
