/**
 *  Pool.hpp
 *  ONScripter-RU
 *
 *  Contains graphics pools for load and preserve.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include "External/Compatibility.hpp"

#include "Support/SDLCompat.hpp"
#include "Support/MemoryBudget.hpp"

#include <unordered_map>
#include <vector>

class PNGLoader;

class TempImagePool {
private:
	struct Entry {
		bool inUse{true};
		MemoryBudget::Lease retention;
		uint64_t lastUsed{0};
	};
	std::unordered_map<SDL_Surface *, Entry> pool;
	std::vector<SDL_Surface *> freeImages;
public:
	SDL_Point size;
	SDL_Surface *getImage();         // get a fresh temporary image
	void giveImage(SDL_Surface *im); // return a temporary image to the pool for reuse
	void addImages(int n);           // pre-create some blank temporary images to avoid delays later
	size_t clearUnused();
	bool evictOneUnused(uint64_t idleMilliseconds);
	~TempImagePool();
};

class TempImageLoaderPool {
private:
	std::unordered_map<PNGLoader *, bool> pool;
	std::vector<PNGLoader *> freeLoaders;

public:
	PNGLoader *getLoader();
	void giveLoader(PNGLoader *ldr);
	void addLoaders(int n);
	~TempImageLoaderPool();
};

// This is out of a general model, because it does not have a state to protect.
// It might be an ONScripter object some day.
extern TempImageLoaderPool pngImageLoaderPool;
