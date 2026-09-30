/**
 *  Cache.cpp
 *  ONScripter-RU
 *
 *  Object caching interface with prebuilt implementations for some classes.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Support/Cache.hpp"

#include <cassert>
#include <cstdlib>
#include <limits>

namespace {
constexpr size_t DefaultDecodedImageCacheBudgetBytes = 64ULL * 1024ULL * 1024ULL;

size_t readDecodedImageCacheBudgetBytes() {
	const char *value = onsSDLGetEnv("ONS_IMAGE_CACHE_MB");
	if (!value || !*value)
		return DefaultDecodedImageCacheBudgetBytes;

	char *end = nullptr;
	const unsigned long long mb = std::strtoull(value, &end, 10);
	if (end == value)
		return DefaultDecodedImageCacheBudgetBytes;
	return static_cast<size_t>(mb) * 1024ULL * 1024ULL;
}
} // namespace

// --------------------- ImageCacheController methods ---------------------

void ImageCacheController::add(int cacheSetNumber, const std::string &filename, const std::shared_ptr<Wrapped_SDL_Surface> &surface) {
	assert(surface);

	if (!surface->surface)
		return; //Don't add nullptrs to cache

	CacheController<Wrapped_SDL_Surface>::add(cacheSetNumber, filename, surface);
	enforceBudget();
}

std::shared_ptr<Wrapped_SDL_Surface> ImageCacheController::get(const std::string &filename) {
	std::shared_ptr<Wrapped_SDL_Surface> res = CacheController<Wrapped_SDL_Surface>::get(filename);
	if (!res)
		return nullptr;
	assert(res->surface);
	//get should not be responsible for refcounts!
	return res;
}

size_t ImageCacheController::cacheBudgetBytes() {
	if (!decodedSurfaceBudgetInitialized) {
		decodedSurfaceBudgetBytes = readDecodedImageCacheBudgetBytes();
		decodedSurfaceBudgetInitialized = true;
	}
	return decodedSurfaceBudgetBytes;
}

size_t ImageCacheController::availablePrefetchBytes(int cacheSetNumber) {
	const size_t budget = cacheBudgetBytes();
	if (!budget)
		return std::numeric_limits<size_t>::max();
	const auto set = cacheSets.find(cacheSetNumber);
	if (set != cacheSets.end() && dynamic_cast<LRUCachedSet<Wrapped_SDL_Surface> *>(set->second))
		return budget; // Explicit rolling caches replace old scene assets as usual.
	// Bulk warmups should stop at the budget instead of decoding the entire
	// catalog and evicting earlier assets before any of them can be displayed.
	const size_t used = approximateBytes();
	return used < budget ? budget - used : 0;
}

void ImageCacheController::enforceBudget() {
	const size_t budget = cacheBudgetBytes();
	if (budget == 0)
		return;

	size_t bytes = approximateBytes();
	while (bytes > budget && evictOne()) {
		bytes = approximateBytes();
	}
}

// --------------------- SoundCacheController methods ---------------------

void SoundCacheController::add(int cacheSetNumber, const std::string &filename, const std::shared_ptr<Wrapped_Mix_Chunk> &chunk) {
	assert(chunk);
	if (!chunk->chunk)
		return; //Don't add nullptrs to cache

	CacheController<Wrapped_Mix_Chunk>::add(cacheSetNumber, filename, chunk);
}

std::shared_ptr<Wrapped_Mix_Chunk> SoundCacheController::get(const std::string &filename) {
	std::shared_ptr<Wrapped_Mix_Chunk> res = CacheController<Wrapped_Mix_Chunk>::get(filename);
	if (!res)
		return nullptr;
	assert(res->chunk); //we didn't add nullptrs
	return res;
}
