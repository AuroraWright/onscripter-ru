/**
 * MemoryBudget.hpp
 * Shared admission and lifetime accounting for reusable resource memory.
 * Consult LICENSE file for licensing terms and copyright holders.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

class MemoryBudget {
	struct State;
	struct Charge;
public:
	enum class Kind { Images, Sounds, Textures, Pools, Prefetch, Count };
	enum class Trim { None, Idle, Budget, Pressure };
	static constexpr size_t MiB = 1024 * 1024;
	struct Snapshot {
		size_t limit{}, used{}, peak{};
		std::array<size_t, static_cast<size_t>(Kind::Count)> bytes{};
		uint64_t denied{};
	};
	// Copies share a single charge, just as shared owners share one allocation.
	// The charge survives cache eviction while an active consumer still owns it.
	class Lease {
		friend class MemoryBudget;
		std::shared_ptr<Charge> charge;
	public:
		explicit operator bool() const { return !!charge; }
		void reset() { charge.reset(); }
		void shrinkTo(size_t bytes);
		void reclassify(Kind kind);
	};

	explicit MemoryBudget(size_t bytes);
	Lease reserve(Kind kind, size_t bytes);
	Snapshot snapshot() const;
	size_t available() const;
	size_t prefetchAllowance(size_t maximum, size_t copies = 1) const;
	void requestTrim(bool pressure = false, size_t needed = 0);
	Trim takeTrimRequest(size_t *needed = nullptr);
	void pollPressure(); // Call on the main thread; samples at most once a second.
	// Essential decoding may always retain the minimum needed to make progress.
	size_t bufferedItems(size_t itemBytes, size_t minimum, size_t maximum) const;

private:
	std::shared_ptr<State> state;
};

MemoryBudget &memoryBudget();
