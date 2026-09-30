/**
 * ImageAssets.cpp
 * Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Components/ImageAssets.hpp"
#include "Engine/Components/Async.hpp"
#include "Engine/Core/ONScripter.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>

size_t ImageAssets::budget() const {
	// This is additional retained GPU memory, not the live scene or decode queue.
	static const size_t limit = [] {
		constexpr size_t defaultMB = 64;
		const char *value = onsSDLGetEnv("ONS_ASSET_CACHE_MB");
		if (!value || !*value)
			return defaultMB * 1024 * 1024;
		char *end = nullptr;
		const long mb = std::strtol(value, &end, 10);
		return (end != value && *end == '\0' && mb >= 0 && mb <= 256 ? static_cast<size_t>(mb) : defaultMB) * 1024 * 1024;
	}();
	return limit;
}

bool ImageAssets::enabled() const {
	return budget() != 0;
}

bool ImageAssets::contains(const std::string &key) const {
	return images.contains(key) || std::find(rejected.begin(), rejected.end(), key) != rejected.end();
}

void ImageAssets::reject(const std::string &key) {
	if (rejected.size() == 64)
		rejected.pop_front();
	rejected.push_back(key);
}

std::string ImageAssets::key(const AnimationInfo &image) {
	// Text depends on font/layout state; layers, generated images and CPU button
	// templates have different ownership rules and continue through their loaders.
	if (!image.file_name || !*image.file_name || image.file_name[0] == '>' ||
	    image.trans_mode == AnimationInfo::TRANS_STRING || image.trans_mode == AnimationInfo::TRANS_LAYER ||
	    image.is_big_image || image.type == SPRITE_BUTTONS || image.type == SPRITE_SENTENCE_FONT || image.num_of_cells <= 0)
		return {};
	std::string result(image.file_name);
	result.push_back('\0');
	if (image.mask_file_name)
		result += image.mask_file_name;
	result.push_back('\0');
	std::replace(result.begin(), result.end(), '\\', '/');
	result += std::to_string(image.trans_mode) + ',' + std::to_string(image.num_of_cells) + ',' +
	          std::to_string(image.vertical_cells) + ',' + std::to_string(image.direct_color.x) + ',' +
	          std::to_string(image.direct_color.y) + ',' + std::to_string(image.direct_color.z);
	return result;
}

RenderImage *ImageAssets::get(const std::string &key) {
	const auto found = images.find(key);
	if (found == images.end())
		return nullptr;
	recent.splice(recent.begin(), recent, found->second.age);
	++hits;
	// Handles have independent drawing state; the renderer shares their pixels
	// until a sprite is modified, keeping the cached contents immutable.
	RenderImage *image = gpu.copyImage(found->second.image);
	if (image)
		GPU_GetTarget(image);
	return image;
}

void ImageAssets::put(const std::string &key, RenderImage *image) {
	if (key.empty() || !image || images.contains(key))
		return;
	const size_t cost = static_cast<size_t>(image->w) * image->h * 4;
	if (cost > budget() || cost > memoryBudget().snapshot().limit || !enabled())
		return;
	// Keep assets already used by this scene. Cycling through a menu larger
	// than the cache must not evict every image before its next opening.
	auto needsRoom = [&] {
		return bytes > budget() - cost || images.size() >= 1024 || cost > memoryBudget().available();
	};
	while (needsRoom() && evictOne(true)) {}
	if (needsRoom())
		return;
	auto retention = memoryBudget().reserve(MemoryBudget::Kind::Textures, cost);
	if (!retention)
		return;
	RenderImage *copy = gpu.copyImage(image);
	if (!copy)
		return;
	GPU_DiscardImagePixels(copy);
	recent.push_front(key);
	images.emplace(key, Entry{std::move(retention), copy, cost, recent.begin()});
	bytes += cost;
}

bool ImageAssets::evictOne(bool unusedOnly) {
	auto candidate = recent.end();
	while (candidate != recent.begin()) {
		auto oldest = images.find(*--candidate);
#if defined(ONS_USE_SDL3)
		if (unusedOnly && oldest->second.image->storage.use_count() > 1)
			continue;
#else
		static_cast<void>(unusedOnly);
#endif
		bytes -= oldest->second.bytes;
		GPU_FreeImage(oldest->second.image);
		images.erase(oldest);
		recent.erase(candidate);
		++evictions;
		return true;
	}
	return false;
}

void ImageAssets::cancelPrefetch() {
	for (auto it = pending.begin(); it != pending.end();) {
		if (it->second->claimed) {
			++it;
		} else {
			it->second->cancel();
			it = pending.erase(it);
		}
	}
	SDL_AtomicLock(&async.imageAssetQueue.lock);
	auto &queue = async.imageAssetQueue.q;
	std::erase_if(queue, [](const auto &instruction) {
		return static_cast<PrepareImageInstruction *>(instruction.get())->job->cancelled.load(std::memory_order_relaxed);
	});
	SDL_AtomicUnlock(&async.imageAssetQueue.lock);
}

void ImageAssets::clear() {
	for (auto &entry : pending)
		entry.second->cancel();
	pending.clear();
	for (auto &entry : images)
		GPU_FreeImage(entry.second.image);
	images.clear();
	recent.clear();
	rejected.clear();
	bytes = 0;
}

void ImageAssets::remove(const std::string &filename) {
	std::string path(filename);
	std::replace(path.begin(), path.end(), '\\', '/');
	auto usesFile = [&](const std::string &key) {
		const auto separator = key.find('\0');
		return key.substr(0, separator) == path ||
		       key.substr(separator + 1, key.find('\0', separator + 1) - separator - 1) == path;
	};
	for (auto it = images.begin(); it != images.end();) {
		if (!usesFile(it->first)) {
			++it;
			continue;
		}
		bytes -= it->second.bytes;
		GPU_FreeImage(it->second.image);
		recent.erase(it->second.age);
		it = images.erase(it);
	}
	for (auto it = pending.begin(); it != pending.end();) {
		if (usesFile(it->first)) {
			it->second->cancel();
			it = pending.erase(it);
		} else {
			++it;
		}
	}
	rejected.remove_if(usesFile);
}

void ONScripter::clearImageAssets() {
	if (onsSDLGetEnv("ONS_ASSET_TELEMETRY") && (imageAssets.hits || imageAssets.misses || imageAssets.prefetched)) {
		sendToLog(LogLevel::Info, "Image assets: hits=%llu demand_jobs=%llu prefetched=%llu evictions=%llu\n",
		          static_cast<unsigned long long>(imageAssets.hits), static_cast<unsigned long long>(imageAssets.misses),
		          static_cast<unsigned long long>(imageAssets.prefetched), static_cast<unsigned long long>(imageAssets.evictions));
	}
	imageAssets.clear();
	// Discard queued speculative work too, so a trim cannot immediately refill
	// the cache. An already running CPU job owns its data until it finishes.
	SDL_AtomicLock(&async.imageAssetQueue.lock);
	async.imageAssetQueue.q.clear();
	SDL_AtomicUnlock(&async.imageAssetQueue.lock);
}

std::shared_ptr<ImageAssetJob> ONScripter::requestImageAsset(const AnimationInfo &image, bool speculative) {
	const auto key = ImageAssets::key(image);
	if (key.empty() || !imageAssets.enabled())
		return nullptr;
	if (auto found = imageAssets.pending.find(key); found != imageAssets.pending.end()) {
		if (!speculative) {
			found->second->claimed = true;
			SDL_AtomicLock(&async.imageAssetQueue.lock);
			auto &queue = async.imageAssetQueue.q;
			const auto queued = std::find_if(queue.begin(), queue.end(), [&](const auto &instruction) {
				return static_cast<PrepareImageInstruction *>(instruction.get())->job == found->second;
			});
			if (queued != queue.end())
				std::rotate(queue.begin(), queued, std::next(queued));
			SDL_AtomicUnlock(&async.imageAssetQueue.lock);
		}
		return found->second;
	}
	if (speculative && imageAssets.pending.size() >= ImageAssets::MaxPending)
		return nullptr;
	auto job = std::make_shared<ImageAssetJob>();
	job->image.deepcopyNonImageFields(image);
	job->speculative = speculative;
	job->claimed = !speculative;
	imageAssets.pending.emplace(key, job);
	async.prepareImage(job, !speculative);
	return job;
}

void ONScripter::prepareImageAsset(const std::shared_ptr<ImageAssetJob> &job) {
	if (!job->start())
		return;
	size_t maxDecodedBytes = 0;
	if (job->speculative) {
		// The loader reserves the actual input and image sizes before allocating
		// them. This allowance is only a ceiling, not a charge for every icon.
		maxDecodedBytes = memoryBudget().prefetchAllowance(ImageAssets::MaxDecodedBytes, 4);
		if (!maxDecodedBytes)
			job->cancel();
	}
	if (!job->cancelled.load(std::memory_order_relaxed)) {
		try {
			buildAIImage(&job->image, maxDecodedBytes, job->speculative ? &job->staging : nullptr);
		} catch (const AsyncController::ThreadTerminate &) {
			job->finish();
			throw;
		} catch (const std::exception &error) {
			// Speculation must not terminate the game or open a dialog. Demand
			// failures are retried by the ordinary loader with its diagnostics.
			sendToLog(LogLevel::Warn, "Image preparation failed: %s\n", error.what());
		}
	}
	if (job->image.image_surface)
		job->staging.shrinkTo(static_cast<size_t>(job->image.image_surface->pitch) * job->image.image_surface->h);
	else {
		job->staging.reset();
		// A smaller allowance is temporary, so do not blacklist this asset.
		if (job->speculative && maxDecodedBytes < ImageAssets::MaxDecodedBytes)
			job->cancel();
	}
	job->finish();
}

bool ONScripter::serviceImageAssets() {
	// At most one upload per frame. Never re-enter through event processing in
	// the upload path, and do no speculative GPU work during video playback.
	const uint64_t now = SDL_GetTicks();
	if (imageAssets.servicing || now - imageAssets.lastService < 16 || !imageAssets.enabled())
		return false;
	while (onsTryWaitSemaphore(async.imageAssetQueue.resultsWaiting)) {}
	for (auto it = imageAssets.pending.begin(); it != imageAssets.pending.end(); ++it) {
		auto job = it->second;
		if (job->claimed || !job->ready())
			continue;
		const auto key = it->first;
		imageAssets.pending.erase(it);
		imageAssets.lastService = now;
		if (job->cancelled.load(std::memory_order_relaxed))
			return true;
		if (!job->image.image_surface) {
			imageAssets.reject(key);
			return true;
		}
		auto upload = memoryBudget().reserve(MemoryBudget::Kind::Prefetch,
		    static_cast<size_t>(job->image.image_surface->w) * job->image.image_surface->h * 4);
		if (!upload)
			return true;
		imageAssets.servicing = true;
		// Upload directly: this is already an idle-time slice, so a nested
		// chunk-loader event loop would stretch each asset into another frame.
		RenderImage *image = gpu.copyImageFromSurface(job->image.image_surface, true);
		if (image) {
			GPU_GetTarget(image);
			GPU_DiscardImagePixels(image);
			imageAssets.put(key, image);
			GPU_FreeImage(image);
			++imageAssets.prefetched;
		}
		imageAssets.servicing = false;
		return true;
	}
	return false;
}

void ONScripter::prefetchImageAssets() {
	if (imageAssets.servicing || !imageAssets.enabled() || (skip_mode & SKIP_SUPERSKIP) ||
	    !memoryBudget().prefetchAllowance(ImageAssets::MaxDecodedBytes, 4))
		return;
	// Read only a small window of upcoming sprite commands. Do not execute
	// branches, evaluate expressions or alter the interpreter's position.
	const char *cursor = script_h.getNext();
	if (!cursor)
		return;
	size_t remaining = 8192;
	for (size_t lines = 0; lines < 64 && remaining && *cursor; ++lines) {
		const char *end = cursor;
		while (remaining && *end && *end != '\n') {
			++end;
			--remaining;
		}
		std::string_view line(cursor, end - cursor);
		cursor = *end == '\n' ? end + 1 : end;
		const auto start = line.find_first_not_of(" \t\r");
		if (start == std::string_view::npos)
			continue;
		line.remove_prefix(start);
		const auto space = line.find_first_of(" \t");
		const auto command = line.substr(0, space);
		if (command != "lsp" && command != "lsph" && command != "lsp2" && command != "lsph2")
			continue;
		const auto comma = line.find(',');
		if (comma == std::string_view::npos)
			continue;
		line.remove_prefix(comma + 1);
		const auto value = line.find_first_not_of(" \t");
		if (value == std::string_view::npos)
			continue;
		line.remove_prefix(value);
		std::string tag;
		size_t length;
		if (line.front() == '"') {
			length = line.find('"', 1);
			if (length == std::string_view::npos)
				continue;
			tag = line.substr(1, length - 1);
			++length;
		} else {
			length = line.find_first_of(", \t");
			if (length == std::string_view::npos || !script_h.findStrAlias(std::string(line.substr(0, length)).c_str(), &tag))
				continue;
		}
		const auto tail = line.find_first_not_of(" \t", length);
		// Concatenated paths and variable-dependent tags must be evaluated only
		// by the real command. Plain alpha/copy tags cover ordinary menu art.
		if (tail == std::string_view::npos || line[tail] != ',' || tag.size() < 4 || tag[0] != ':' ||
		    (tag[1] != 'a' && tag[1] != 'c') || tag.find_first_of("$%?[]") != std::string::npos ||
		    tag.find(';') == std::string::npos)
			continue;
		AnimationInfo image;
		image.setImageName(tag.c_str());
		parseTaggedString(&image);
		const auto key = ImageAssets::key(image);
		if (imageAssets.contains(key))
			continue;
		requestImageAsset(image, true);
		if (imageAssets.pending.size() >= ImageAssets::MaxPending)
			break;
	}
}
