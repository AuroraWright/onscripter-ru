/**
 * ImageAssets.hpp
 * Bounded preparation and reuse of immutable sprite images.
 * Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include "Engine/Entities/Animation.hpp"
#include "Support/MemoryBudget.hpp"

#include <atomic>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>

// Workers own only CPU data. Textures and cache bookkeeping stay on the render
// thread; publishing ready is the sole handoff from a worker to that thread.
struct ImageAssetJob {
	MemoryBudget::Lease staging;
	AnimationInfo image;
	std::atomic_bool cancelled{false};
	bool speculative{false};
	bool claimed{false}; // render thread only

	bool ready() const { return state.load(std::memory_order_acquire) == State::Ready; }
	bool start() {
		State expected = State::Queued;
		return state.compare_exchange_strong(expected, State::Running);
	}
	void finish() { state.store(State::Ready, std::memory_order_release); }
	void cancel() {
		cancelled.store(true, std::memory_order_relaxed);
		// A queued job will never run after its instruction is removed. Wake a
		// demand waiter now; a running job must finish publishing its pixels first.
		State expected = State::Queued;
		state.compare_exchange_strong(expected, State::Ready);
	}

private:
	enum class State { Queued, Running, Ready };
	std::atomic<State> state{State::Queued};
};

class ImageAssets {
public:
	static constexpr size_t MaxPending = 4;
	static constexpr size_t MaxDecodedBytes = 16 * 1024 * 1024;

	static std::string key(const AnimationInfo &image);
	RenderImage *get(const std::string &key);
	void put(const std::string &key, RenderImage *image);
	void clear();
	void cancelPrefetch();
	bool evictOne(bool unusedOnly = false);
	void remove(const std::string &filename);
	void reject(const std::string &key);
	bool enabled() const;
	bool contains(const std::string &key) const;

	std::unordered_map<std::string, std::shared_ptr<ImageAssetJob>> pending;
	bool servicing{false};
	uint64_t lastService{0};
	uint64_t hits{0}, misses{0}, prefetched{0}, evictions{0};

private:
	struct Entry {
		MemoryBudget::Lease retention;
		RenderImage *image;
		size_t bytes;
		std::list<std::string>::iterator age;
	};
	std::unordered_map<std::string, Entry> images;
	std::list<std::string> recent;
	std::list<std::string> rejected;
	size_t bytes{0};
	size_t budget() const;
};
