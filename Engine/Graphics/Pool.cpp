/**
 *  Pool.cpp
 *  ONScripter-RU
 *
 *  Contains graphics pools for load and preserve.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Graphics/Pool.hpp"
#include "Engine/Graphics/PNG.hpp"
#include "Engine/Components/Async.hpp"
#include "Support/FileDefs.hpp"

#include <algorithm>

SDL_Surface *TempImagePool::getImage() {
	Lock lock(this);
	SDL_Surface *r = nullptr;
	while (!freeImages.empty()) {
		SDL_Surface *candidate = freeImages.back();
		freeImages.pop_back();
		auto it = pool.find(candidate);
		if (it != pool.end() && !it->second.inUse) {
			r = candidate;
			break;
		}
	}

	if (!r) {
		r = onsCreateRGBSurface(SDL_SWSURFACE, size.x, size.y, 24,
		                        0x000000ff, 0x0000ff00, 0x00ff0000, 0);
	}
	if (r) {
		auto &entry = pool[r];
		entry.inUse = true;
		if (!entry.retention)
			entry.retention = memoryBudget().reserve(MemoryBudget::Kind::Pools, static_cast<size_t>(r->pitch) * r->h);
	}
	return r;
}

void TempImagePool::giveImage(SDL_Surface *im) {
	if (!im)
		return;
	Lock lock(this);
	auto it = pool.find(im);
	if (it == pool.end() || it->second.inUse) {
		auto &entry = pool[im];
		const auto budget = memoryBudget().snapshot();
		if (budget.used > budget.limit)
			entry.retention.reset();
		if (!entry.retention)
			entry.retention = memoryBudget().reserve(MemoryBudget::Kind::Pools, static_cast<size_t>(im->pitch) * im->h);
		if (!entry.retention) {
			pool.erase(im);
			SDL_FreeSurface(im);
			return;
		}
		entry.inUse = false;
		entry.lastUsed = SDL_GetTicks();
		freeImages.push_back(im);
	}
}

void TempImagePool::addImages(int n) {
	Lock lock(this);
	SDL_Surface *im;
	for (int i = 0; i < n; i++) {
		auto retention = memoryBudget().reserve(MemoryBudget::Kind::Prefetch, ((static_cast<size_t>(size.x) * 3 + 3) & ~size_t{3}) * size.y);
		if (!retention)
			break;
		im       = onsCreateRGBSurface(SDL_SWSURFACE, size.x, size.y, 24,
                                  0x000000ff, 0x0000ff00, 0x00ff0000, 0);
		if (!im)
			break;
		retention.reclassify(MemoryBudget::Kind::Pools);
		pool[im] = {false, std::move(retention), SDL_GetTicks()};
		freeImages.push_back(im);
	}
}

bool TempImagePool::evictOneUnused(uint64_t idleMilliseconds) {
	Lock lock(this);
	const uint64_t now = SDL_GetTicks();
	auto oldest = pool.end();
	for (auto it = pool.begin(); it != pool.end(); ++it)
		if (!it->second.inUse && now - it->second.lastUsed >= idleMilliseconds &&
		    (oldest == pool.end() || it->second.lastUsed < oldest->second.lastUsed))
			oldest = it;
	if (oldest == pool.end())
		return false;
	freeImages.erase(std::remove(freeImages.begin(), freeImages.end(), oldest->first), freeImages.end());
	SDL_FreeSurface(oldest->first);
	pool.erase(oldest);
	return true;
}

size_t TempImagePool::clearUnused() {
	Lock lock(this);
	size_t released = 0;
	for (auto it = pool.begin(); it != pool.end();) {
		if (it->second.inUse) {
			++it;
			continue;
		}
		released += static_cast<size_t>(it->first->pitch) * it->first->h;
		SDL_FreeSurface(it->first);
		it = pool.erase(it);
	}
	std::vector<SDL_Surface *>().swap(freeImages);
	return released;
}

TempImagePool::~TempImagePool() {
	for (const auto &diver : pool) {
		if (!diver.second.inUse) {
			SDL_FreeSurface(diver.first);
		} else {
			sendToLog(LogLevel::Error, "~TempImagePool@Diver cannot be eaten\n");
		}
	}
}

PNGLoader *TempImageLoaderPool::getLoader() {
	Lock lock(this);
	PNGLoader *r = nullptr;
	while (!freeLoaders.empty()) {
		PNGLoader *candidate = freeLoaders.back();
		freeLoaders.pop_back();
		auto it = pool.find(candidate);
		if (it != pool.end() && !it->second) {
			r = candidate;
			break;
		}
	}

	if (!r)
		r = new PNGLoader();

	pool[r] = true;
	return r;
}

void TempImageLoaderPool::giveLoader(PNGLoader *ldr) {
	Lock lock(this);
	auto it = pool.find(ldr);
	if (it == pool.end() || it->second) {
		pool[ldr] = false;
		freeLoaders.push_back(ldr);
	}
}

void TempImageLoaderPool::addLoaders(int n) {
	Lock lock(this);
	PNGLoader *ldr;
	for (int i = 0; i < n; i++) {
		ldr       = new PNGLoader();
		pool[ldr] = false;
		freeLoaders.push_back(ldr);
	}
}

TempImageLoaderPool::~TempImageLoaderPool() {
	for (auto diver : pool) {
		if (!diver.second) {
			delete diver.first;
		} else {
			sendToLog(LogLevel::Error, "~TempImageLoaderPool@Diver cannot be eaten\n");
		}
	}
}

TempImageLoaderPool pngImageLoaderPool;
