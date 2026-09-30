/**
 *  Cache.hpp
 *  ONScripter-RU
 *
 *  Object caching interface with prebuilt implementations for some classes.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include "External/Compatibility.hpp"
#include "External/LRUCache.hpp"
#include "Support/MemoryBudget.hpp"

#include "Support/SDLCompat.hpp"
#include "Support/SDLMixerCompat.hpp"

#include "Engine/Graphics/RendererBackend.hpp"

#include <unordered_map>
#include <string>
#include <memory>
#include <cassert>
#include <cstddef>

struct Wrapped_SDL_Surface {
	MemoryBudget::Lease retention;
	SDL_Surface *surface = nullptr;
	bool has_alpha       = false;
	Wrapped_SDL_Surface(SDL_Surface *_surface, bool _has_alpha)
	    : surface(_surface), has_alpha(_has_alpha) {
		//if (surface)
		//	sendToLog(LogLevel::Info, "CREATE %p - %p with ref %d\n",this,surface,surface->refcount);
	}
	size_t memoryBytes() const {
		return surface ? static_cast<size_t>(surface->pitch) * surface->h : 0;
	}
	bool retain() {
		if (!retention)
			retention = memoryBudget().reserve(MemoryBudget::Kind::Images, memoryBytes());
		return !!retention;
	}
	~Wrapped_SDL_Surface() {
		if (surface) {
			//sendToLog(LogLevel::Info, "DELETE %p - %p with ref %d\n",this,surface,surface->refcount);
			SDL_FreeSurface(surface);
		}
	}
	Wrapped_SDL_Surface(const Wrapped_SDL_Surface &other) {
		retention = other.retention;
		surface = other.surface;
		if (surface)
			surface->refcount++;
		has_alpha = other.has_alpha;
	}
	Wrapped_SDL_Surface &operator=(const Wrapped_SDL_Surface &other) {
		retention = other.retention;
		if (other.surface)
			other.surface->refcount++;
		if (surface)
			SDL_FreeSurface(surface);
		surface   = other.surface;
		has_alpha = other.has_alpha;
		return *this;
	}
};

struct Wrapped_GPU_Image {
	MemoryBudget::Lease retention;
	RenderImage *img{nullptr};
	size_t memoryBytes() const {
		return img ? static_cast<size_t>(img->w) * img->h * 4 : 0;
	}
	bool retain() {
		if (!retention)
			retention = memoryBudget().reserve(MemoryBudget::Kind::Pools, memoryBytes());
		return !!retention;
	}
	Wrapped_GPU_Image(RenderImage *image) {
		//assert(image != nullptr && image->refcount == 1);
		img = image;
	}
	Wrapped_GPU_Image(Wrapped_GPU_Image &&image) noexcept {
		retention = std::move(image.retention);
		img       = image.img;
		image.img = nullptr;
	}
	Wrapped_GPU_Image(const Wrapped_GPU_Image &image) {
		if (image.img)
			img = GPU_CopyImage(image.img);
	}
	Wrapped_GPU_Image &operator=(const Wrapped_GPU_Image &) = delete;
	Wrapped_GPU_Image &operator=(Wrapped_GPU_Image &&other) noexcept {
		if (this != &other) {
			if (img)
				GPU_FreeImage(img);
			retention = std::move(other.retention);
			img = other.img;
			other.img = nullptr;
		}
		return *this;
	}
	~Wrapped_GPU_Image() {
		if (img)
			GPU_FreeImage(img);
	}
};

class Wrapped_Mix_Chunk {
public:
	MemoryBudget::Lease retention;
	Mix_Chunk *chunk{nullptr};
	size_t memoryBytes() const {
#if defined(ONS_USE_SDL3)
		return chunk ? std::max<size_t>(chunk->alen, chunk->capacityBytes) : 0;
#else
		return chunk ? chunk->alen : 0;
#endif
	}
	bool retain() {
		if (!retention)
			retention = memoryBudget().reserve(MemoryBudget::Kind::Sounds, memoryBytes());
		return !!retention;
	}
	Wrapped_Mix_Chunk(Mix_Chunk *_chunk)
	    : chunk(_chunk) {}
	~Wrapped_Mix_Chunk() {
		if (chunk)
			Mix_FreeChunk(chunk);
	}
	Wrapped_Mix_Chunk(const Wrapped_Mix_Chunk &other) = delete;
	Wrapped_Mix_Chunk &operator=(const Wrapped_Mix_Chunk &other) = delete;
};

template <typename SETELEM>
inline size_t cachedElementApproxBytes(const std::shared_ptr<SETELEM> &) {
	return 0;
}

inline size_t cachedElementApproxBytes(const std::shared_ptr<Wrapped_SDL_Surface> &elem) {
	return elem ? elem->memoryBytes() : 0;
}

inline size_t cachedElementApproxBytes(const std::shared_ptr<Wrapped_GPU_Image> &elem) {
	return elem ? elem->memoryBytes() : 0;
}

inline size_t cachedElementApproxBytes(const std::shared_ptr<Wrapped_Mix_Chunk> &elem) {
	return elem ? elem->memoryBytes() : 0;
}

template <typename SETELEM, typename KEY = std::string>
class CachedSet {
public:
	virtual void add(const KEY &keyname, const std::shared_ptr<SETELEM> &elem) = 0;
	virtual void clear()                                         = 0;
	virtual void remove(const KEY &keyname)                      = 0;
	virtual std::shared_ptr<SETELEM> get(const KEY &keyname)     = 0;
	virtual size_t count() const                                 = 0;
	virtual size_t approxBytes() const                           = 0;
	virtual bool evictOne()                                      = 0;
	virtual ~CachedSet()                                         = default;
};

template <typename SETELEM, typename KEY = std::string>
class LRUCachedSet : public CachedSet<SETELEM, KEY> {
protected:
	int capacity = 0;
	LRUCache<KEY, std::shared_ptr<SETELEM>, std::unordered_map> elemCache;
	size_t cachedBytes{0};

public:
	void add(const KEY &keyname, const std::shared_ptr<SETELEM> &elem) {
		if (!elem->retain())
			return;
		if (elemCache.size() == 0) {
			elemCache.set(keyname, elem);
			return;
		}

		if (const auto *existing = elemCache.peek(keyname)) {
			cachedBytes -= cachedElementApproxBytes(*existing);
		} else if (elemCache.count() == elemCache.size()) {
			const auto *oldest = elemCache.oldest();
			assert(oldest);
			cachedBytes -= cachedElementApproxBytes(*oldest);
		}
		elemCache.set(keyname, elem);
		cachedBytes += cachedElementApproxBytes(elem);
	}
	std::shared_ptr<SETELEM> get(const KEY &keyname) {
		try {
			auto wrapped = elemCache.get(keyname);
			//sendToLog(LogLevel::Info, "(LRU) Found image cache entry %s\n", filename.c_str());
			return wrapped;
		} catch (int) {
			//sendToLog(LogLevel::Info, "(LRU) Failed to find cache entry %s\n", filename.c_str());
			return nullptr;
		}
	}
	void remove(const KEY &keyname) {
		if (const auto *existing = elemCache.peek(keyname))
			cachedBytes -= cachedElementApproxBytes(*existing);
		elemCache.remove(keyname);
	}
	void clear() {
		elemCache.resize(0);        // evict all elements
		elemCache.resize(capacity); // make it capable of holding the original capacity again
		cachedBytes = 0;
		                            // (lru cache has no clear() method)
	}
	size_t count() const {
		return elemCache.count();
	}
	size_t approxBytes() const {
		return cachedBytes;
	}
	bool evictOne() {
		const auto *oldest = elemCache.oldest();
		if (!oldest)
			return false;
		cachedBytes -= cachedElementApproxBytes(*oldest);
		return elemCache.evict_one();
	}
	LRUCachedSet(int capacity)
	    : capacity(capacity), elemCache(capacity) {}
};

template <typename SETELEM, typename KEY = std::string>
class UnlimitedCachedSet : public CachedSet<SETELEM, KEY> {
protected:
	std::unordered_map<KEY, std::shared_ptr<SETELEM>> elemCache;
	size_t cachedBytes{0};

public:
	void add(const KEY &keyname, const std::shared_ptr<SETELEM> &elem) {
		if (!elem->retain())
			return;
		const auto result = elemCache.emplace(keyname, elem);
		if (result.second)
			cachedBytes += cachedElementApproxBytes(elem);
	}
	std::shared_ptr<SETELEM> get(const KEY &keyname) {
		auto iterator = elemCache.find(keyname);
		if (iterator != elemCache.end()) {
			//sendToLog(LogLevel::Info, (Unlimited) Found image cache entry %s\n", filename.c_str());
			return iterator->second;
		}
		//sendToLog(LogLevel::Info, (Unlimited) Failed to find cache entry %s\n", filename.c_str());
		return nullptr;
	}
	void remove(const KEY &keyname) {
		auto iterator = elemCache.find(keyname);
		if (iterator != elemCache.end()) {
			cachedBytes -= cachedElementApproxBytes(iterator->second);
			elemCache.erase(iterator);
		}
	}
	void clear() {
		elemCache.clear();
		cachedBytes = 0;
	}
	size_t count() const {
		return elemCache.size();
	}
	size_t approxBytes() const {
		return cachedBytes;
	}
	bool evictOne() {
		if (elemCache.empty())
			return false;
		auto entry = elemCache.begin();
		cachedBytes -= cachedElementApproxBytes(entry->second);
		elemCache.erase(entry);
		return true;
	}
	UnlimitedCachedSet() = default;
};

template <typename SETELEM>
class CacheController {
	friend class CachedImageSet;

protected:
	void deleteExistingSet(int cacheSetNumber) {
		auto entry = cacheSets.find(cacheSetNumber);
		if (entry == cacheSets.end())
			return;
		CachedSet<SETELEM> *set = entry->second;
		set->clear();
		delete set;
		cacheSets.erase(entry);
	}
	std::unordered_map<int, CachedSet<SETELEM> *> cacheSets;

public:
	void clearAll() {
		for (auto &number_set_pair : cacheSets) number_set_pair.second->clear();
	}
	void clear(int cacheSetNumber) {
		try {
			CachedSet<SETELEM> *set = cacheSets.at(cacheSetNumber);
			set->clear();
		} catch (std::out_of_range &) {
			return;
		}
	}
	void makeLRU(int cacheSetNumber, int capacity) {
		if (cacheSets.find(cacheSetNumber) != cacheSets.end())
			deleteExistingSet(cacheSetNumber);
		auto set = new LRUCachedSet<SETELEM>(capacity);
		cacheSets.emplace(cacheSetNumber, set);
	}
	void makeUnlimited(int cacheSetNumber) {
		if (cacheSets.find(cacheSetNumber) != cacheSets.end())
			deleteExistingSet(cacheSetNumber);
		auto set = new UnlimitedCachedSet<SETELEM>();
		cacheSets.emplace(cacheSetNumber, set);
	}
	void add(int cacheSetNumber, const std::string &filename, std::shared_ptr<SETELEM> elem) {
		assert(elem);
		if (!elem->retention && elem->memoryBytes() > memoryBudget().snapshot().limit)
			return;
		while (!elem->retain())
			if (!evictOne())
				return;
		CachedSet<SETELEM> *set = nullptr;
		auto entry = cacheSets.find(cacheSetNumber);
		if (entry == cacheSets.end()) {
			// that set didn't exist, add it as default (unlimited)
			set = new UnlimitedCachedSet<SETELEM>();
			cacheSets.emplace(cacheSetNumber, set);
		} else {
			set = entry->second;
		}

		set->add(filename, elem);
	}
	void remove(int cacheSetNumber, const std::string &filename) {
		auto entry = cacheSets.find(cacheSetNumber);
		if (entry == cacheSets.end()) {
			// That set doesn't exist; cannot remove
			return;
		}
		entry->second->remove(filename);
	}
	void removeAll(std::string filename) {
		for (auto &number_set_pair : cacheSets) {
			number_set_pair.second->remove(filename);
		}
	}
	virtual std::shared_ptr<SETELEM> get(const std::string &filename) {
		for (auto &number_set_pair : cacheSets) {
			std::shared_ptr<SETELEM> elem = number_set_pair.second->get(filename);
			if (elem) {
				return elem;
			}
		}
		return nullptr;
	}
	size_t count() const {
		size_t total = 0;
		for (const auto &number_set_pair : cacheSets)
			total += number_set_pair.second->count();
		return total;
	}
	size_t approximateBytes() const {
		size_t total = 0;
		for (const auto &number_set_pair : cacheSets)
			total += number_set_pair.second->approxBytes();
		return total;
	}
	bool evictOne() {
		CachedSet<SETELEM> *largest = nullptr;
		size_t largestBytes = 0;
		for (auto &number_set_pair : cacheSets) {
			const size_t bytes = number_set_pair.second->approxBytes();
			if (bytes > largestBytes) {
				largestBytes = bytes;
				largest      = number_set_pair.second;
			}
		}
		return largest && largest->evictOne();
	}
};

class ImageCacheController : public CacheController<Wrapped_SDL_Surface> {
public:
	void add(int cacheSetNumber, const std::string &filename, const std::shared_ptr<Wrapped_SDL_Surface> &surface);
	std::shared_ptr<Wrapped_SDL_Surface> get(const std::string &filename) override;
	size_t availablePrefetchBytes(int cacheSetNumber);

private:
	size_t decodedSurfaceBudgetBytes{0};
	bool decodedSurfaceBudgetInitialized{false};
	size_t cacheBudgetBytes();
	void enforceBudget();
};

class SoundCacheController : public CacheController<Wrapped_Mix_Chunk> {
public:
	void add(int cacheSetNumber, const std::string &filename, const std::shared_ptr<Wrapped_Mix_Chunk> &chunk);
	std::shared_ptr<Wrapped_Mix_Chunk> get(const std::string &filename) override;
};
