/**
 *  Animation.cpp
 *  ONScripter-RU
 *
 *  Methods to manipulate AnimationInfo.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Core/ONScripter.hpp"
#include "Engine/Components/Async.hpp"
#include "Engine/Components/Window.hpp"
#include "Engine/Graphics/GPU.hpp"
#include "Support/FileIO.hpp"

#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace {
class OptionalGPUTelemetryScope {
public:
	OptionalGPUTelemetryScope(bool active, const char *source)
	    : active(active) {
		if (active)
			GPU_PushTelemetryScope(source);
	}
	~OptionalGPUTelemetryScope() {
		if (active)
			GPU_PopTelemetryScope();
	}

	OptionalGPUTelemetryScope(const OptionalGPUTelemetryScope &) = delete;
	OptionalGPUTelemetryScope &operator=(const OptionalGPUTelemetryScope &) = delete;

private:
	bool active{false};
};

const char *animationDrawTelemetrySource(const AnimationInfo *info) {
	if (!info)
		return "draw_animation";
	if (info->type & SPRITE_BG)
		return "draw_background";
	if (info->type & (SPRITE_LSP | SPRITE_LSP2))
		return "draw_sprite";
	if (info->type & SPRITE_SENTENCE_FONT)
		return "draw_sentence_font";
	if (info->type & SPRITE_CURSOR)
		return "draw_cursor";
	if (info->type & SPRITE_TACHI)
		return "draw_tachi";
	if (info->type & (SPRITE_BAR | SPRITE_PRNUM))
		return "draw_parameter";
	if (info->type & SPRITE_BUTTONS)
		return "draw_button";
	return "draw_animation";
}

bool perfTelemetryEnabled() {
	const char *value = onsSDLGetEnv("ONS_SDL3_GPU_TELEMETRY");
	return value && *value && std::strcmp(value, "0") != 0;
}

size_t surfaceBytes(const SDL_Surface *surface) {
	return surface ? static_cast<size_t>(surface->pitch) * surface->h : 0;
}

struct AnimationMemoryStats {
	size_t animationCount{0};
	size_t surfaceCount{0};
	size_t surfaceBytes{0};
	size_t gpuImageCount{0};
	size_t gpuPixelBytes{0};
	size_t bigImageCount{0};
};

struct BigImageCellCacheTelemetry {
	uint64_t drawCalls{0};
	uint64_t cacheDraws{0};
	uint64_t cacheHits{0};
	uint64_t cacheBuilds{0};
	uint64_t fallbackChunkDraws{0};
	uint64_t fallbackScaledDraws{0};
	uint64_t fallbackOversizeDraws{0};
	uint64_t fallbackMissingDraws{0};
	uint64_t sourceChunksDrawn{0};
	uint64_t sourceChunksCached{0};
};

BigImageCellCacheTelemetry bigImageCellCacheTelemetry;

void addAnimationMemory(AnimationInfo *ai, AnimationMemoryStats &stats, std::unordered_set<AnimationInfo *> &seen) {
	if (!ai || !seen.insert(ai).second)
		return;

	++stats.animationCount;
	if (ai->image_surface) {
		++stats.surfaceCount;
		stats.surfaceBytes += surfaceBytes(ai->image_surface);
	}
	if (ai->gpu_image) {
		++stats.gpuImageCount;
		stats.gpuPixelBytes += ai->gpu_image->pixels.size();
	}
	if (ai->big_image_cell_cache) {
		++stats.gpuImageCount;
		stats.gpuPixelBytes += ai->big_image_cell_cache->pixels.size();
	}
	if (ai->big_image)
		++stats.bigImageCount;

	addAnimationMemory(ai->old_ai, stats, seen);
}

bool isSmoothTextCursorSprite(const AnimationInfo *ai) {
	if (!ai || ai->type != SPRITE_LSP || ai->num_of_cells <= 1 || !ai->duration_list || !ai->file_name)
		return false;

	return std::strstr(ai->file_name, "graphics\\system\\wnd\\cursor") ||
	       std::strstr(ai->file_name, "graphics/system/wnd/cursor");
}

int nextAnimationCell(const AnimationInfo *ai) {
	int next = ai->current_cell + ai->direction;
	if (next < 0)
		return 1;
	if (next < ai->num_of_cells)
		return next;
	if (ai->loop_mode == 0)
		return 0;
	if (ai->loop_mode == 1)
		return ai->num_of_cells - 1;
	return ai->num_of_cells - 2;
}

int smoothTextCursorAlpha(AnimationInfo *ai) {
	uint64_t durationNanos = ai->getDurationNanos(ai->current_cell);
	if (durationNanos == 0)
		return 255;

	uint64_t remainingNanos = ai->clock.remainingNanos();
	if (remainingNanos > durationNanos)
		remainingNanos = durationNanos;

	double t = static_cast<double>(durationNanos - remainingNanos) / static_cast<double>(durationNanos);
	int nextCell = nextAnimationCell(ai);
	double phase = static_cast<double>(ai->current_cell) + (static_cast<double>(nextCell - ai->current_cell) * t);
	double maxPhase = static_cast<double>(ai->num_of_cells - 1);
	if (phase < 0.0)
		phase = 0.0;
	else if (phase > maxPhase)
		phase = maxPhase;

	return static_cast<int>((1.0 - (phase / maxPhase)) * 255.0);
}

RenderRect cellClipRect(const AnimationInfo *ai, int cell) {
	RenderRect clip_rect{0, 0, ai->pos.w, ai->pos.h};
	if (ai->num_of_cells > 1 && cell != 0) {
		if (!ai->vertical_cells)
			clip_rect.x += ai->pos.w * cell;
		else
			clip_rect.y += ai->pos.h * cell;
	}
	return clip_rect;
}

RenderImage *ensureBigImageCellCache(AnimationInfo *info, int base_cell_off_x, int base_cell_off_y) {
	const bool telemetry = perfTelemetryEnabled();
	if (!info || !info->big_image) {
		if (telemetry)
			++bigImageCellCacheTelemetry.fallbackMissingDraws;
		return nullptr;
	}

	const int cacheW = static_cast<int>(info->pos.w);
	const int cacheH = static_cast<int>(info->pos.h);
	const int maxTexture = gpu.max_texture;
	const int maxImageDim = static_cast<int>(std::numeric_limits<uint16_t>::max());
	if (cacheW <= 0 || cacheH <= 0 || cacheW > maxImageDim || cacheH > maxImageDim ||
	    (maxTexture > 0 && (cacheW > maxTexture || cacheH > maxTexture))) {
		if (telemetry)
			++bigImageCellCacheTelemetry.fallbackOversizeDraws;
		return nullptr;
	}

	if (info->big_image_cell_cache &&
	    info->big_image_cell_cache_cell == info->current_cell &&
	    info->big_image_cell_cache_w == cacheW &&
	    info->big_image_cell_cache_h == cacheH) {
		if (telemetry)
			++bigImageCellCacheTelemetry.cacheHits;
		return info->big_image_cell_cache;
	}

	RenderRect fullCell{static_cast<float>(base_cell_off_x), static_cast<float>(base_cell_off_y),
	                    static_cast<float>(cacheW), static_cast<float>(cacheH)};
	auto chunks = info->big_image->getImagesForArea(fullCell);
	if (chunks.empty()) {
		if (telemetry)
			++bigImageCellCacheTelemetry.fallbackMissingDraws;
		return nullptr;
	}

	info->clearBigImageCellCache();
	RenderImage *cache = gpu.createImage(static_cast<uint16_t>(cacheW), static_cast<uint16_t>(cacheH), 4);
	GPU_GetTarget(cache);
	GPU_SetImageFilter(cache, GPU_FILTER_LINEAR);
	GPU_SetBlending(cache, true);
	GPU_SetRGBA(cache, 255, 255, 255, 255);
	gpu.clearWholeTarget(cache->target, 0, 0, 0, 0);

	RenderRect cacheClip{0, 0, static_cast<float>(cacheW), static_cast<float>(cacheH)};
	for (auto &chunk : chunks) {
		RenderImage *chunkImage = chunk.first;
		const GPU_bool previousBlending = chunkImage->use_blending;
		const SDL_Color previousColor   = chunkImage->color;
		GPU_SetBlending(chunkImage, false);
		if (previousColor.r != 255 || previousColor.g != 255 || previousColor.b != 255 || previousColor.a != 255)
			GPU_SetRGBA(chunkImage, 255, 255, 255, 255);

		gpu.copyGPUImage(chunkImage, nullptr, &cacheClip, cache->target,
		                 chunk.second.x + chunkImage->w / 2.0 - base_cell_off_x,
		                 chunk.second.y + chunkImage->h / 2.0 - base_cell_off_y,
		                 1, 1, 0, true);

		if (previousColor.r != 255 || previousColor.g != 255 || previousColor.b != 255 || previousColor.a != 255)
			GPU_SetRGBA(chunkImage, previousColor.r, previousColor.g, previousColor.b, previousColor.a);
		GPU_SetBlending(chunkImage, previousBlending);
	}

	info->big_image_cell_cache      = cache;
	info->big_image_cell_cache_cell = info->current_cell;
	info->big_image_cell_cache_w    = cacheW;
	info->big_image_cell_cache_h    = cacheH;
	if (telemetry) {
		++bigImageCellCacheTelemetry.cacheBuilds;
		bigImageCellCacheTelemetry.sourceChunksCached += chunks.size();
	}
	return cache;
}

constexpr int ScrollableTextCacheMinimumPadding = 12;

void appendScrollableTextCacheInt(std::string &key, int value) {
	key.append(std::to_string(value));
	key.push_back('\x1f');
}

void appendScrollableTextCacheColor(std::string &key, const uchar3 &color) {
	appendScrollableTextCacheInt(key, color.x);
	appendScrollableTextCacheInt(key, color.y);
	appendScrollableTextCacheInt(key, color.z);
}

std::string makeScrollableTextCacheKey(const std::string &text, Fontinfo fi, int wrapLimit, int tightlyFit) {
	while (fi.styleStack.size() > 1) fi.styleStack.pop();
	const auto &style = fi.style();

	std::string key;
	key.reserve(text.size() + 192);
	key.append(text);
	key.push_back('\x1e');
	appendScrollableTextCacheInt(key, wrapLimit);
	appendScrollableTextCacheInt(key, tightlyFit);
	appendScrollableTextCacheInt(key, fi.borderPadding);
	appendScrollableTextCacheColor(key, fi.buttonMultiplyColor);
	appendScrollableTextCacheInt(key, static_cast<int>(style.font_number));
	appendScrollableTextCacheInt(key, style.preset_id);
	appendScrollableTextCacheColor(key, style.color);
	appendScrollableTextCacheInt(key, style.is_gradient);
	appendScrollableTextCacheInt(key, style.is_centered);
	appendScrollableTextCacheInt(key, style.is_fitted);
	appendScrollableTextCacheInt(key, style.is_bold);
	appendScrollableTextCacheInt(key, style.is_italic);
	appendScrollableTextCacheInt(key, style.is_border);
	appendScrollableTextCacheInt(key, style.border_width);
	appendScrollableTextCacheColor(key, style.border_color);
	appendScrollableTextCacheInt(key, style.is_shadow);
	appendScrollableTextCacheInt(key, style.shadow_distance[0]);
	appendScrollableTextCacheInt(key, style.shadow_distance[1]);
	appendScrollableTextCacheColor(key, style.shadow_color);
	appendScrollableTextCacheInt(key, style.font_size);
	appendScrollableTextCacheInt(key, style.character_spacing);
	appendScrollableTextCacheInt(key, style.line_height);
	appendScrollableTextCacheInt(key, style.wrap_limit);
	return key;
}

int scrollableTextCachePadding(TextRenderingState &state) {
	int padding = ScrollableTextCacheMinimumPadding;
	auto inspectPiece = [&](DialoguePiece &piece) {
		padding = std::max(padding, piece.borderPadding);
		for (const auto &fi : piece.fontInfos) {
			const auto &style = fi.style();
			padding           = std::max(padding, fi.borderPadding);
			if (style.is_border)
				padding = std::max(padding, style.border_width);
			if (style.is_shadow) {
				padding = std::max(padding, std::abs(style.shadow_distance[0]));
				padding = std::max(padding, std::abs(style.shadow_distance[1]));
			}
		}
	};
	for (auto &seg : state.segments) {
		for (auto &run : seg.runs) {
			for (auto &piece : run.pieces) inspectPiece(piece);
			for (auto &piece : run.rubyPieces) inspectPiece(piece);
		}
	}
	return padding + 2;
}

void freeScrollableCachedText(AnimationInfo::ScrollableInfo::CachedText &cached) {
	if (cached.image)
		gpu.freeImage(cached.image);
	cached.image = nullptr;
	cached.key.clear();
	cached.padding = 0;
}

void setSpriteRGBA(RenderImage *src, const AnimationInfo *ai, int alpha) {
	GPU_SetRGBA(src,
	            ai->trans * ai->darkenHue.r * alpha / (255 * 255),
	            ai->trans * ai->darkenHue.g * alpha / (255 * 255),
	            ai->trans * ai->darkenHue.b * alpha / (255 * 255),
	            ai->trans * alpha / 255);
}
} // namespace

int ONScripter::proceedAnimation() {
	int minimum_duration = -1;

	auto hasAnimationState = [](const AnimationInfo &anim) {
		return anim.exists || (anim.old_ai && anim.old_ai->exists);
	};
	auto proceed = [&](AnimationInfo *anim) {
		if (!hasAnimationState(*anim))
			return;

		if (anim->visible && anim->is_animatable) {
			minimum_duration = estimateNextDuration(anim, anim->pos, minimum_duration);
		}
		if (anim->old_ai && anim->old_ai->visible && anim->old_ai->is_animatable) {
			minimum_duration = estimateNextDuration(anim->old_ai, anim->old_ai->pos, minimum_duration, true);
		}
	};

	if (animationProceedCandidatesPrepared) {
		for (AnimationInfo *anim : animationProceedCandidates)
			proceed(anim);
		animationProceedCandidatesPrepared = false;
	} else {
		animationProceedCandidatesPrepared = false;
		for (int i = 0; i < 3; ++i)
			proceed(&tachi_info[i]);
		for (int i = 0; i < MAX_SPRITE_NUM; ++i) {
			if (hasAnimationState(sprite_info[i]))
				proceed(&sprite_info[i]);
		}
		for (int i = 0; i < MAX_SPRITE_NUM; ++i) {
			if (hasAnimationState(sprite2_info[i]))
				proceed(&sprite2_info[i]);
		}
	}

//Mion - ogapee2009
#ifdef USE_LUA
	if (lua_handler.is_animatable && !script_h.isExternalScript()) {
		if (lua_handler.remaining_time == 0) {
			lua_handler.remaining_time = lua_handler.duration_time;
			if (minimum_duration == -1 ||
			    minimum_duration > lua_handler.remaining_time)
				minimum_duration = lua_handler.remaining_time;
			int lua_event_mode           = event_mode;
			int tmp_string_buffer_offset = string_buffer_offset;
			const char *current          = script_h.getCurrent();
			lua_handler.callback(LUAHandler::LUA_ANIMATION);
			script_h.setCurrent(current);
			readToken();
			string_buffer_offset = tmp_string_buffer_offset;
			event_mode           = lua_event_mode;
		} else if ((minimum_duration == -1) ||
		           (minimum_duration > lua_handler.remaining_time)) {
			minimum_duration = lua_handler.remaining_time;
		}
	}
#endif

	return minimum_duration;
}

int ONScripter::proceedCursorAnimation() {
	if (enable_custom_cursors)
		return -1;

	int minimum_duration = -1;
	AnimationInfo *anim  = nullptr;

	if (!textgosub_label && draw_cursor_flag &&
	    (clickstr_state == CLICK_WAIT || clickstr_state == CLICK_NEWPAGE)) {

		if (clickstr_state == CLICK_WAIT)
			anim = &cursor_info[CURSOR_WAIT_NO];
		else
			anim = &cursor_info[CURSOR_NEWPAGE_NO];

		if (anim->visible && anim->is_animatable) {
			RenderRect dst_rect = anim->pos;
			if (!anim->abs_flag) {
				dst_rect.x += sentence_font.x();
				dst_rect.y += sentence_font.y();
			}

			minimum_duration = estimateNextDuration(anim, dst_rect, minimum_duration);
		}

		if (anim->old_ai && anim->old_ai->visible && anim->old_ai->is_animatable) {
			RenderRect dst_rect = anim->old_ai->pos;
			if (!anim->old_ai->abs_flag) {
				dst_rect.x += sentence_font.x();
				dst_rect.y += sentence_font.y();
			}

			minimum_duration = estimateNextDuration(anim->old_ai, dst_rect, minimum_duration, true);
		}
	}

	return minimum_duration;
}

int ONScripter::estimateNextDuration(AnimationInfo *anim, RenderRect & /*rect*/, int minimum, bool old_ai) {
	if (anim->clock.expired()) {
		if (anim->trans_mode != AnimationInfo::TRANS_LAYER) {
			const int duration = anim->getDuration(anim->current_cell);
			if ((minimum == -1) || (minimum > duration))
				minimum = duration;
			if (anim->proceedAnimation()) {
				dirtySpriteRect(anim, old_ai);
			}
		} else if (anim->layer_no >= 0) {
			auto handler = getLayer<Layer>(anim->layer_no, false);
			//sendToLog(LogLevel::Info, "About to update AI %p, anim->clock.time() %d\n", anim, anim->clock.time());
			if (handler->update(old_ai))
				dirtySpriteRect(anim, old_ai);
			anim->clock.setCountdownNanos(anim->getDurationNanos(anim->current_cell));
			const int duration = anim->getDuration(anim->current_cell);
			if ((minimum == -1) || (minimum > duration))
				minimum = duration;
		}
	} else {
		if (isSmoothTextCursorSprite(anim))
			dirtySpriteRect(anim, old_ai);
		const int remaining = static_cast<int>(anim->clock.remaining());
		if ((minimum == -1) || minimum > remaining)
			minimum = remaining;
	}

	return minimum;
}

void ONScripter::advanceAIclocks(uint64_t ns) {
	auto needsClockAdvance = [](AnimationInfo &ai) {
		return ai.exists || ai.old_ai || ai.visible || ai.camera.isMoving() ||
		       ai.spriteTransforms.warpAmplitude != 0;
	};
	auto needsProceed = [](AnimationInfo &ai) {
		return (ai.visible && ai.is_animatable) ||
		       (ai.old_ai && ai.old_ai->visible && ai.old_ai->is_animatable);
	};

	animationProceedCandidates.clear();

	for (int i = 0; i < 3; i++) {
		if (needsClockAdvance(tachi_info[i]))
			advanceAnimationInfoClocks(ns, &tachi_info[i], i, -1);
		if (needsProceed(tachi_info[i]))
			animationProceedCandidates.push_back(&tachi_info[i]);
	}
	for (int i = MAX_SPRITE_NUM - 1; i >= 0; i--) { // why the hell backwards... stand upright ons writers
		if (needsClockAdvance(sprite_info[i]))
			advanceAnimationInfoClocks(ns, &sprite_info[i], i, 0);
		if (needsProceed(sprite_info[i]))
			animationProceedCandidates.push_back(&sprite_info[i]);
		if (needsClockAdvance(sprite2_info[i]))
			advanceAnimationInfoClocks(ns, &sprite2_info[i], i, 1);
		if (needsProceed(sprite2_info[i]))
			animationProceedCandidates.push_back(&sprite2_info[i]);
	}
	animationProceedCandidatesPrepared = true;

//Mion - ogapee2009
#ifdef USE_LUA
	if (lua_handler.is_animatable && !script_h.isExternalScript())
		lua_handler.remaining_time -= (ns / 1000000);
#endif
}

uint64_t ONScripter::nextAnimationUpdateNanos() {
	WorkSchedule schedule;
	auto inspect = [&](AnimationInfo &root) {
		for (AnimationInfo *ai = &root; ai; ai = ai->old_ai) {
			if (ai->camera.isMoving() || (ai->visible && ai->spriteTransforms.warpAmplitude != 0))
				schedule.requireAfter(0);
			if (ai->visible && ai->is_animatable)
				schedule.requireAfter(isSmoothTextCursorSprite(ai) ? 0 : ai->clock.remainingNanos());
		}
	};
	for (auto &ai : tachi_info)
		inspect(ai);
	for (int i = 0; i < MAX_SPRITE_NUM; ++i) {
		inspect(sprite_info[i]);
		inspect(sprite2_info[i]);
	}
	// Cursor and Lua callbacks retain their existing frame cadence.
	if (draw_cursor_flag)
		schedule.requireAfter(0);
#ifdef USE_LUA
	if (lua_handler.is_animatable && !script_h.isExternalScript())
		schedule.requireAfter(0);
#endif
	return schedule.delay(WorkSchedule::Never);
}

void ONScripter::advanceAnimationInfoClocks(uint64_t ns, AnimationInfo *ai, int i, int type) {
	if (ai->visible && ai->is_animatable) {
		ai->clock.tickNanos(ns);
		//sendToLog(LogLevel::Info, "Advanced clock for AI %p, anim->clock.time() %i, remaining() %i, expired() %i\n", anim, anim->clock.time(), anim->clock.remaining(), anim->clock.expired());
	}

	// update sprite camera positions
	// ***************** Is this broken now that we allowed camera clock updates for old_ais?? ***********************
	if (ai->camera.isMoving() && type >= 0) {
		dirtySpriteRect(i, type == 1);
		ai->camera.update(static_cast<unsigned int>(ns / 1000000));
		dirtySpriteRect(i, type == 1);
	}

	if (ai->spriteTransforms.warpAmplitude != 0)
		ai->spriteTransforms.warpClock.tickNanos(ns);

	if (ai->old_ai)
		advanceAnimationInfoClocks(ns, ai->old_ai, i, type);
}

// Can put this in AI if you want
void ONScripter::advanceSpecificAIclocks(uint64_t ns, int i, int type, bool old_ai) {
	AnimationInfo *ai = type == 0 ? &sprite_info[i] :
	                                type > 0 ? &sprite2_info[i] :
	                                           &tachi_info[i];

	if (old_ai) {
		if (!ai->old_ai) {
			errorAndExit("Asked to advance clocks for a non-existent old_ai");
			return; //dummy
		}
		ai = ai->old_ai;
	}

	advanceAnimationInfoClocks(ns, ai, i, type);
}

void ONScripter::printImageMemoryTelemetry(const char *context) {
	if (!perfTelemetryEnabled())
		return;

	AnimationMemoryStats stats;
	std::unordered_set<AnimationInfo *> seen;
	auto add = [&](AnimationInfo &ai) {
		addAnimationMemory(&ai, stats, seen);
	};
	auto addPtr = [&](AnimationInfo *ai) {
		addAnimationMemory(ai, stats, seen);
	};
	auto addButtonChain = [&](ButtonLink &root) {
		for (ButtonLink *button = root.next; button; button = button->next)
			addPtr(button->anim);
		addPtr(root.anim);
	};

	add(bg_info);
	add(btndef_info);
	add(sentence_font_info);
	for (auto &cursor : cursor_info)
		add(cursor);
	for (auto &tachi : tachi_info)
		add(tachi);
	for (int i = 0; i < MAX_SPRITE_NUM; ++i) {
		add(sprite_info[i]);
		add(sprite2_info[i]);
	}
	for (auto *bar : bar_info)
		addPtr(bar);
	for (auto *prnum : prnum_info)
		addPtr(prnum);
	add(window_effect.anim);
	addButtonChain(root_button_link);
	addButtonChain(exbtn_d_button_link);
	addButtonChain(text_button_link);
	for (auto *queued : queueAnimationInfo)
		addPtr(queued);

	size_t cacheCount = 0;
	size_t cacheBytes = 0;
	{
		Lock lock(&imageCache);
		cacheCount = imageCache.count();
		cacheBytes = imageCache.approximateBytes();
	}

	static size_t nextLogBytes = 256ull * 1024ull * 1024ull;
	const size_t totalBytes = stats.surfaceBytes + cacheBytes + stats.gpuPixelBytes;
	if (totalBytes < nextLogBytes)
		return;
	while (totalBytes >= nextLogBytes)
		nextLogBytes += 256ull * 1024ull * 1024ull;

	sendToLog(LogLevel::Info,
	          "Image memory telemetry: context=%s animations=%llu surfaces=%llu surface_bytes=%llu "
	          "gpu_images=%llu gpu_cpu_pixel_bytes=%llu big_images=%llu cache_entries=%llu cache_bytes=%llu total_bytes=%llu\n",
	          context ? context : "unknown",
	          static_cast<unsigned long long>(stats.animationCount),
	          static_cast<unsigned long long>(stats.surfaceCount),
	          static_cast<unsigned long long>(stats.surfaceBytes),
	          static_cast<unsigned long long>(stats.gpuImageCount),
	          static_cast<unsigned long long>(stats.gpuPixelBytes),
	          static_cast<unsigned long long>(stats.bigImageCount),
	          static_cast<unsigned long long>(cacheCount),
	          static_cast<unsigned long long>(cacheBytes),
	          static_cast<unsigned long long>(totalBytes));
}

void ONScripter::printBigImageCellCacheTelemetry() const {
	if (!perfTelemetryEnabled() || bigImageCellCacheTelemetry.drawCalls == 0)
		return;

	std::printf("BigImage cell cache telemetry: draw_calls=%llu cache_draws=%llu cache_hits=%llu "
	            "cache_builds=%llu fallback_chunk_draws=%llu fallback_scaled_draws=%llu "
	            "fallback_oversize_draws=%llu fallback_missing_draws=%llu "
	            "source_chunks_drawn=%llu source_chunks_cached=%llu\n",
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.drawCalls),
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.cacheDraws),
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.cacheHits),
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.cacheBuilds),
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.fallbackChunkDraws),
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.fallbackScaledDraws),
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.fallbackOversizeDraws),
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.fallbackMissingDraws),
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.sourceChunksDrawn),
	            static_cast<unsigned long long>(bigImageCellCacheTelemetry.sourceChunksCached));
}

void ONScripter::setupAnimationInfo(AnimationInfo *anim, Fontinfo *info) {
	if (anim->gpu_image && !anim->stale_image) {
		anim->exists = true;
		return;
	}

	if (deferredLoadingEnabled && !info && (skip_mode & SKIP_SUPERSKIP) && anim->type == SPRITE_LSP2) {
		// Superskip check should be unnecessary actually...
		anim->exists          = true;
		anim->deferredLoading = true;
		return;
	}

	auto st = SDL_GetTicks();
	anim->deleteImage();
	anim->abs_flag = true;
	const auto assetKey = imageAssets.enabled() ? ImageAssets::key(*anim) : std::string{};
	RenderImage *cachedImage = assetKey.empty() ? nullptr : imageAssets.get(assetKey);
	bool logPreparedAsset = cachedImage != nullptr;

	if (cachedImage) {
		anim->setImage(cachedImage);
	} else if (anim->trans_mode == AnimationInfo::TRANS_STRING) {
		Fontinfo f_info = info ? *info : sentence_font;

		while (f_info.styleStack.size() > 1) f_info.styleStack.pop();

		f_info.clear();

		if (anim->font_size_xy[0] >= 0) {
			f_info.top_xy[0]               = anim->orig_pos.x;
			f_info.top_xy[1]               = anim->orig_pos.y;
			f_info.changeStyle().font_size = anim->font_size_xy[0] > anim->font_size_xy[1] ? anim->font_size_xy[0] : anim->font_size_xy[1];
		}

		anim->pos.x = f_info.x();
		anim->pos.y = f_info.y();

		f_info.changeStyle().is_gradient = use_text_gradients_for_sprites;
		f_info.top_xy[0]                 = 0;
		f_info.top_xy[1]                 = 0;

		copyarr(f_info.buttonMultiplyColor, anim->color_list[0]);

		TextRenderingState state;
		uint16_t w, h;
		RenderRect clip{0, 0, 0, 0};
		state.dstClip = &clip;

		//TODO: make this configurable? vertical cells for buttons are less likely to exceed the texture limit
		anim->vertical_cells = true;

		for (int i = 0; i < anim->num_of_cells; i++) {
			//TODO: anim->skip_whitespace?

			if (i == 0) {
				dlgCtrl.prepareForRendering(anim->file_name, f_info, state, w, h);
				if (w == 0 || h == 0)
					break;

				if (!anim->vertical_cells)
					w *= anim->num_of_cells;
				else
					h *= anim->num_of_cells;

				if (anim->is_big_image) {
					anim->setBigImage(new GPUBigImage(w, h, 4));
					state.dst.bigImage = anim->big_image.get();
				} else {
					anim->setImage(gpu.createImage(w, h, 4));
					GPU_GetTarget(anim->gpu_image);
					state.dst.target = anim->gpu_image->target;
					gpu.clearWholeTarget(state.dst.target, 0, 0, 0, 0);
				}

				clip.w = anim->pos.w;
				clip.h = anim->pos.h;
			} else {
				// Shift letters
				if (!anim->vertical_cells) {
					state.offset.x += anim->pos.w;
					clip.x = anim->pos.w * i;
				} else {
					state.offset.y += anim->pos.h;
					clip.y = anim->pos.h * i;
				}
				// Update multiply colour
				auto updatePieceColor = [&](DialoguePiece &piece) {
					for (auto &fi : piece.fontInfos)
						copyarr(fi.buttonMultiplyColor, anim->color_list[i]);
					piece.renderCommandsPrepared = false;
				};
				for (auto &seg : state.segments) {
					for (auto &run : seg.runs) {
						for (auto &piece : run.pieces) updatePieceColor(piece);
						for (auto &piece : run.rubyPieces) updatePieceColor(piece);
					}
				}
			}

			dlgCtrl.render(state);
		}
	} else if (anim->trans_mode == AnimationInfo::TRANS_LAYER) {
		//pos w&h already screen-size
		anim->calculateImage(anim->pos.w, anim->pos.h);
		//anim->fill(0, 0, 0, 0);
	} else {
		int old_event_mode = event_mode;
		event_mode         = IDLE_EVENT_MODE;
		preventExit(true);
		auto job = assetKey.empty() ? nullptr : requestImageAsset(*anim, false);
		if (job) {
			++imageAssets.misses;
			prefetchImageAssets();
			waitForAsync([&] { return job->ready(); });
			// Transfer CPU pixels only. The job never owns a GPU object, even
			// when a trim cancels it while its worker is publishing completion.
			anim->setSurface(job->image.image_surface);
			logPreparedAsset = job->speculative && anim->image_surface;
			job->image.image_surface = nullptr;
			imageAssets.pending.erase(assetKey);
		}
		if (!anim->image_surface) {
			// Unsupported/oversized speculative assets and failures retain the
			// regular demand loader, including its missing-file diagnostics.
			async.loadImage(anim);
			waitForAsync([&] { return onsTryWaitSemaphore(async.loadImageQueue.resultsWaiting); });
		}
		preventExit(false);
		event_mode = old_event_mode;
		buildGPUImage(*anim);
		imageAssets.put(assetKey, anim->gpu_image);
		freeRedundantSurfaces(*anim);
	}
	if (logPreparedAsset && anim->gpu_image && filelog_flag) {
		Lock lock(&script_h.log_info[ScriptHandler::FILE_LOG]);
		script_h.findAndAddLog(script_h.log_info[ScriptHandler::FILE_LOG], anim->file_name, true);
		if (anim->mask_file_name)
			script_h.findAndAddLog(script_h.log_info[ScriptHandler::FILE_LOG], anim->mask_file_name, true);
	}
	anim->stale_image     = false;
	anim->exists          = true;
	anim->deferredLoading = false;

	internal_slowdown_counter += SDL_GetTicks() - st;
	printImageMemoryTelemetry("setupAnimationInfo");
}

void ONScripter::postSetupAnimationInfo(AnimationInfo *anim) {
	if (anim->type == SPRITE_LSP2) {
		anim->calcAffineMatrix(window.script_width, window.script_height);
		if (anim->visible)
			dirtySpriteRect(anim);
		if (anim->layer_no < 0)
			anim->is_animatable = false; //extended sprites don't animate unless they display layers
	}
}

void ONScripter::buildAIImage(AnimationInfo *anim, size_t maxDecodedBytes, MemoryBudget::Lease *staging) {
	bool has_alpha{false};
	bool allow_24_bpp{anim->trans_mode == AnimationInfo::TRANS_COPY};

	SDL_Surface *surface = loadImage(anim->file_name, &has_alpha, allow_24_bpp, maxDecodedBytes, staging);
	if (!surface)
		return;

	bool using_24_bpp = onsSurfaceBitsPerPixel(surface) == 24;

	SDL_Surface *surface_m = nullptr;
	MemoryBudget::Lease maskStaging;
	if (anim->trans_mode == AnimationInfo::TRANS_MASK) {
		surface_m = loadImage(anim->mask_file_name, nullptr, false, maxDecodedBytes, staging ? &maskStaging : nullptr);
		if (maxDecodedBytes && !surface_m) {
			// A bounded preload must defer the whole asset if its mask cannot fit.
			// Publishing the unmasked source would cache different visible pixels.
			SDL_FreeSurface(surface);
			return;
		}
	}

	if (!using_24_bpp) {
		surface = anim->setupImageAlpha(surface, surface_m, has_alpha);
	}

	anim->setSurface(surface);

	if (surface_m)
		SDL_FreeSurface(surface_m);
}

bool ONScripter::treatAsSameImage(const AnimationInfo &anim1, const AnimationInfo &anim2) {
	// returns true if the AnimationInfo tags would create identical images
	if (&anim1 == &anim2)
		return true;

	if ((anim1.trans_mode != anim2.trans_mode) ||
	    //assume layers aren't identical
	    (anim1.trans_mode == AnimationInfo::TRANS_LAYER) ||
	    (anim1.is_animatable != anim2.is_animatable) ||
	    (anim1.num_of_cells != anim2.num_of_cells) ||
	    (anim1.vertical_cells != anim2.vertical_cells))
		return false;

	if (!equalstr(anim1.file_name, anim2.file_name) ||
	    !equalstr(anim1.mask_file_name, anim2.mask_file_name))
		return false;

	if (anim1.color != anim2.color ||
	    (anim1.trans_mode == AnimationInfo::TRANS_DIRECT &&
	     anim1.direct_color != anim2.direct_color))
		return false;

	if (anim1.trans_mode == AnimationInfo::TRANS_STRING) {
		for (int i = 0; i < anim1.num_of_cells; i++) {
			if (anim1.color_list[i] != anim2.color_list[i])
				return false;
		}
	}
	//by this point, they most likely create the same images
	return true;
}

void ONScripter::parseTaggedString(AnimationInfo *anim, bool is_mask) {
	if (anim->image_name == nullptr)
		return;

	AnimationInfo acopy;
	if (!anim->stale_image && anim->gpu_image)
		acopy.deepcopyNonImageFields(*anim); // a copy of the tag, for later comparison

	bool has = anim->has_z_order_override;
	anim->removeTag();
	if (preserve) {
		anim->has_z_order_override = has;
	}

	int i;
	const char *buffer = anim->image_name;

	anim->num_of_cells = 1;
	anim->current_cell = 0;
	anim->trans_mode   = trans_mode;
	//use COPY as default trans_mode for masks
	if (is_mask)
		anim->trans_mode = AnimationInfo::TRANS_COPY;

	if (buffer[0] == '*') {
		//Mion: it's a layer!

		anim->trans_mode = AnimationInfo::TRANS_LAYER;
		buffer++;
		anim->layer_no = getNumberFromBuffer(&buffer);

		auto tmp = getLayerInfo(anim->layer_no, false);

		if (tmp) {
			anim->pos.x = anim->pos.y = 0;
			anim->pos.w               = window.script_width;
			anim->pos.h               = window.script_height;
			tmp->handler->setSpriteInfo(sprite_info, anim);
			anim->duration_list    = new int[1];
			anim->duration_list[0] = tmp->interval;
			anim->is_animatable    = true;
			sendToLog(LogLevel::Info, "setup a sprite for layer %d\n", anim->layer_no);
		} else
			anim->layer_no = -1;
		return;
	}
	if (buffer[0] == ':') {
		while (*++buffer == ' ')
			;

		if (buffer[0] == 'a') {
			anim->trans_mode = AnimationInfo::TRANS_ALPHA;
			buffer++;
		} else if (buffer[0] == 'l') {
			anim->trans_mode = AnimationInfo::TRANS_TOPLEFT;
			buffer++;
		} else if (buffer[0] == 'r') {
			anim->trans_mode = AnimationInfo::TRANS_TOPRIGHT;
			buffer++;
		} else if (buffer[0] == 'c') {
			anim->trans_mode = AnimationInfo::TRANS_COPY;
			buffer++;
		} else if (buffer[0] == 'd') {
			anim->trans_mode    = AnimationInfo::TRANS_COPY;
			anim->blending_mode = BlendModeId::ADD;
			buffer++;
		} else if (buffer[0] == 'b') {
			anim->trans_mode    = AnimationInfo::TRANS_COPY;
			anim->blending_mode = BlendModeId::SUB;
			buffer++;
		} else if (buffer[0] == 'u') {
			anim->trans_mode    = AnimationInfo::TRANS_COPY;
			anim->blending_mode = BlendModeId::MUL;
			buffer++;
		} else if (buffer[0] == 's') {
			anim->trans_mode = AnimationInfo::TRANS_STRING;
			buffer++;
			anim->num_of_cells = 0;
			if (*buffer == '/') {
				buffer++;
				script_h.getNext();

				script_h.pushCurrent(buffer);
				anim->font_size_xy[0] = script_h.readInt();
				anim->font_size_xy[1] = script_h.readInt();
				//IMPORTED: from ONS (pitch param is optional)
				if (script_h.hasMoreArgs()) {
					script_h.readInt(); // dummy read for pitch
					if (script_h.hasMoreArgs()) {
						script_h.readInt(); // 0 ... normal, 1 ... no anti-aliasing, 2 ... Fukuro
					}
				}
				buffer = script_h.getNext();
				script_h.popCurrent();
			} else {
				anim->font_size_xy[0] = anim->font_size_xy[1] = sentence_font.style().font_size;
			}
			while (buffer[0] != '#' && buffer[0] != '\0') buffer++;
			i = 0;
			while (buffer[i] == '#') {
				anim->num_of_cells++;
				i += 7;
			}
			anim->color_list = new uchar3[anim->num_of_cells];
			for (i = 0; i < anim->num_of_cells; i++) {
				readColor(&anim->color_list[i], buffer);
				buffer += 7;
			}
		} else if (buffer[0] == 'm') {
			anim->trans_mode  = AnimationInfo::TRANS_MASK;
			const char *start = ++buffer;
			while (buffer[0] != ';' && buffer[0] != 0x0a && buffer[0] != '\0') buffer++;
			if (buffer[0] == ';')
				script_h.setStr(&anim->mask_file_name, start, buffer - start);
		} else if (buffer[0] == '#') {
			anim->trans_mode = AnimationInfo::TRANS_DIRECT;
			readColor(&anim->direct_color, buffer);
			buffer += 7;
		} else if (buffer[0] == '!') {
			anim->trans_mode = AnimationInfo::TRANS_PALETTE;
			buffer++;
			getNumberFromBuffer(&buffer); //palette number, which is now dead
		}

		if (anim->trans_mode != AnimationInfo::TRANS_STRING)
			while (buffer[0] != '/' && buffer[0] != ';' && buffer[0] != '\0') buffer++;
	}

	//IMPORTED: from ONS
	if (buffer[0] == '/' && anim->trans_mode != AnimationInfo::TRANS_STRING) {
		buffer++;
		anim->num_of_cells = getNumberFromBuffer(&buffer);
		if (anim->num_of_cells == 0) {
			sendToLog(LogLevel::Error, "ONScripter::parseTaggedString  The number of cells is 0\n");
			return;
		}

		anim->duration_list = new int[anim->num_of_cells];

		if (*buffer == ',') {
			buffer++;

			if (*buffer == '<') {
				buffer++;
				for (i = 0; i < anim->num_of_cells; i++) {
					anim->duration_list[i] = getNumberFromBuffer(&buffer);
					buffer++;
				}
			} else {
				anim->duration_list[0] = getNumberFromBuffer(&buffer);
				for (i = 1; i < anim->num_of_cells; i++)
					anim->duration_list[i] = anim->duration_list[0];
			}
			//anim->remaining_time = anim->duration_list[0];

			buffer++;
			anim->loop_mode = *buffer++ - '0'; // 3...no animation

			if (*buffer == ',') {
				buffer++;
				anim->vertical_cells = getNumberFromBuffer(&buffer);
			}
		} else {
			for (i = 0; i < anim->num_of_cells; i++)
				anim->duration_list[0] = 0;
			anim->loop_mode = 3; // 3...no animation
		}
		if (anim->loop_mode != 3)
			anim->is_animatable = true;

		while (buffer[0] != ';' && buffer[0] != '\0') buffer++;
	}

	if (buffer[0] == ';' && anim->trans_mode != AnimationInfo::TRANS_STRING)
		buffer++;

	if (anim->trans_mode == AnimationInfo::TRANS_STRING && buffer[0] == '$') {
		script_h.pushCurrent(buffer);
		script_h.setStr(&anim->file_name, script_h.readStr());
		script_h.popCurrent();
	} else {
		script_h.setStr(&anim->file_name, buffer);
		translatePathSlashes(anim->file_name);
	}

	anim->stale_image = (anim->stale_image || (anim->gpu_image == nullptr) ||
	                     !treatAsSameImage(*anim, acopy));
}

void ONScripter::drawSpritesetToGPUTarget(RenderTarget *target, SpritesetInfo *spriteset, RenderRect *clip, int rm) {
	GPU_TelemetryScope telemetryScope("draw_spriteset");
	RenderRect myClip{0, 0, static_cast<float>(target->w), static_cast<float>(target->h)};
	if (clip) {
		myClip = *clip;
		myClip.x += camera.center_pos.x;
		myClip.y += camera.center_pos.y;
	}

	//This is a D.S.T. clip
	/*myClip.x += spriteset->pos.x;
	myClip.y += spriteset->pos.y;*/

	bool blur     = spriteset->blur > 0;
	bool mask     = spriteset->maskSpriteNumber != -1;
	bool breakup  = spriteset->breakupFactor > 0;
	bool pixelate = spriteset->pixelateFactor > 0;
	bool warp     = spriteset->warpAmplitude != 0;
	bool any      = blur || mask || breakup || pixelate || warp;

	auto &ssim     = rm & REFRESH_BEFORESCENE_MODE ? spriteset->im : spriteset->imAfterscene;
	RenderImage *src = ssim.image;
	PooledGPUImage toDraw;
	if (any) {
		if (blur)
			toDraw = gpu.getBlurredImage(ssim, spriteset->blur);
		if (mask) {
			GPUTransformableCanvasImage tmp(toDraw.image);
			toDraw = gpu.getMaskedImage(toDraw.image ? tmp : ssim, sprite_info[spriteset->maskSpriteNumber].oldNew(rm)->gpu_image);
		}
		if (breakup) {
			GPUTransformableCanvasImage tmp(toDraw.image);
			toDraw = gpu.getBrokenUpImage(toDraw.image ? tmp : ssim, {{BreakupType::SPRITESET, static_cast<int16_t>(spriteset->id)}}, spriteset->breakupFactor, spriteset->breakupDirectionFlagset, nullptr);
		}
		if (pixelate) {
			GPUTransformableCanvasImage tmp(toDraw.image);
			toDraw = gpu.getPixelatedImage((toDraw.image ? tmp : ssim), spriteset->pixelateFactor);
		}
		if (warp) {
			GPUTransformableCanvasImage tmp(toDraw.image);
			float secs = spriteset->warpClock.time() / 1000.0;
			toDraw     = gpu.getWarpedImage(toDraw.image ? tmp : ssim, secs, spriteset->warpAmplitude, spriteset->warpWaveLength, spriteset->warpSpeed);
		}
		src = toDraw.image;
	}

	if (spriteset->trans < 255) {
		GPU_SetRGBA(src, spriteset->trans, spriteset->trans, spriteset->trans, spriteset->trans);
	}
	float xpos = spriteset->pos.x + (spriteset->has_scale_center ? spriteset->scale_center_x : src->w / 2.0);
	float ypos = spriteset->pos.y + (spriteset->has_scale_center ? spriteset->scale_center_y : src->h / 2.0);

	gpu.copyGPUImage(src, nullptr, &myClip, target, xpos, ypos, spriteset->scale_x / 100.0, spriteset->scale_y / 100.0, -spriteset->rot, true);
	if (spriteset->trans < 255) {
		GPU_SetRGBA(src, 255, 255, 255, 255);
	}
}

void ONScripter::drawSpecialScrollable(RenderTarget *target, AnimationInfo *info, int refresh_mode, RenderRect *clip) {
	if (!info->scrollableInfo.isSpecialScrollable)
		return;

	// We might have scrolled since last draw, so we've got to update the mouse cursor position...
	// This breaks mouseover for some buttons sometimes somehow and is not needed anymore (because we check in dynprop.apply()
	//refreshButtonHoverState();

	RenderRect canvasPos = info->pos;
	canvasPos.x += camera.center_pos.x;
	canvasPos.y += camera.center_pos.y;
	RenderRect localClip = clip ? *clip : canvasPos;
	doClipping(&localClip, &canvasPos);
	GPU_SetClipRect(target, localClip);

	AnimationInfo::ScrollableInfo &si = info->scrollableInfo;
	StringTree &tree                  = dataTrees[si.elementTreeIndex];
	int scroll_y                      = info->scrollable.y;

	ensureScrollableGeometryCache(&si, tree);
	const auto &gc = si.geometryCache;

	//sendToLog(LogLevel::Info, "Hovered: %d\n", si.hoveredElement);

	// Compute the visible element range once, replicating the original loop's
	// break conditions (first unread log entry, first element past the bottom of
	// the visible area) so the two draw passes below stay in lockstep.
	const long firstIdx = static_cast<long>(getScrollableElementsVisibleAt(&si, tree, scroll_y) - tree.insertionOrder.begin());
	long lastIdx = firstIdx;
	for (long j = firstIdx; j < static_cast<long>(gc.size()); ++j) {
		const auto &c = gc[j];
		if (c.hasLog && !script_h.logState.logEntryIndexToIsRead(c.logIndex))
			break;
		if (c.y - scroll_y > info->pos.h)
			break;
		lastIdx = j + 1;
	}

	// Pass 1: dividers + element backgrounds. Element plates and dividers never
	// overlap each other or the element text, so drawing every background before
	// any text lets the SDL3_GPU native blit batch keep one source texture per
	// pass instead of flushing a command buffer on every bg/text texture switch.
	for (long j = firstIdx; j < lastIdx; ++j) {
		const auto &c = gc[j];
		const float h     = static_cast<float>(c.height);
		const float yTop  = c.y - scroll_y + camera.center_pos.y;
		const float yBot  = yTop + h;
		const float xLeft = c.x + camera.center_pos.x;

		if (j == firstIdx && si.divider)
			gpu.copyGPUImage(si.divider->oldNew(refresh_mode)->gpu_image, nullptr, &localClip, target, info->pos.x + camera.center_pos.x, info->pos.y + yTop - si.divider->pos.h);

		auto spriteBg = c.bgSpriteIndex >= 0 ? &sprite2_info[c.bgSpriteIndex] : si.elementBackground;
		if (spriteBg)
			spriteBg = spriteBg->oldNew(refresh_mode);
		if (spriteBg) {
			RenderRect bg_rect{0, 0, spriteBg->pos.w, spriteBg->pos.h};
			if (spriteBg->num_of_cells > 1 && si.hoveredElement == j) {
				// May need to be expanded to allow for elements you can set into a state (e.g. "playing") and then move away from
				// e.g. selectedElement field (seems confuseable with hoveredElement lol)
				bg_rect.x += spriteBg->pos.w;
			}
			// Currently prints at 0,0 of element with no attempt to do proper padding
			gpu.copyGPUImage(spriteBg->gpu_image, &bg_rect, &localClip,
			                 target, info->pos.x + xLeft, info->pos.y + yTop,
			                 1, 1, 0, false);
		}

		if (si.divider)
			gpu.copyGPUImage(si.divider->oldNew(refresh_mode)->gpu_image, nullptr, &localClip, target, info->pos.x + camera.center_pos.x, info->pos.y + yBot);
	}

	// Pass 2: element text. Each visible element's text is rendered once to a
	// small GPU texture (the same render-to-texture pattern used for lsp string
	// sprites) and blitted on subsequent frames, so the per-frame cost is one
	// blit per element instead of decodeUTF8 + layoutSegment + layoutLines +
	// per-glyph blits.
	const size_t treeSize = tree.insertionOrder.size();
	if (si.textCacheTreeVersion != tree.modificationVersion || si.textCacheTreeSize != treeSize) {
		for (auto &ct : si.textCache)
			freeScrollableCachedText(ct);
		si.textCache.clear();
		si.textCache.assign(treeSize, AnimationInfo::ScrollableInfo::CachedText{});
		si.textCacheTreeVersion    = tree.modificationVersion;
		si.textCacheTreeSize       = treeSize;
		si.textCacheHoveredElement = -1;
	}
	// Hover changes the multiply colour / gradient for the hovered element, so
	// re-render the previously hovered and newly hovered entries.
	if (si.textCacheHoveredElement != si.hoveredElement) {
		auto freeEntry = [&](long idx) {
			if (idx >= 0 && static_cast<size_t>(idx) < si.textCache.size())
				freeScrollableCachedText(si.textCache[idx]);
		};
		freeEntry(si.textCacheHoveredElement);
		freeEntry(si.hoveredElement);
		si.textCacheHoveredElement = si.hoveredElement;
	}

	for (long j = firstIdx; j < lastIdx; ++j) {
		const auto &c = gc[j];
		if (!(c.hasText || c.hasLog))
			continue;
		const float w     = c.width ? static_cast<float>(c.width) : static_cast<float>(info->pos.w);
		const float yTop  = c.y - scroll_y + camera.center_pos.y;
		const float xLeft = c.x + camera.center_pos.x;
		const float textX = info->pos.x + xLeft + c.textMarginLeft;
		const float textY = info->pos.y + yTop + c.textMarginTop;
		const int wrapLimit = static_cast<int>(w - (c.textMarginLeft + c.textMarginRight));

		Fontinfo fi = sentence_font;
		fi.clear();
		while (fi.styleStack.size() > 1) fi.styleStack.pop();
		fi.top_xy[0]                 = 0;
		fi.top_xy[1]                 = 0;
		fi.changeStyle().wrap_limit  = wrapLimit;
		fi.changeStyle().can_loghint = true;
		auto &buttonMultiplyColor    = j == si.hoveredElement ? si.hoverMultiplier : si.normalMultipler;
		auto &gradient               = j == si.hoveredElement ? si.hoverGradients : si.normalGradients;
		fi.buttonMultiplyColor       = buttonMultiplyColor;
		if (fi.style().is_gradient != gradient)
			fi.changeStyle().is_gradient = gradient;
		const std::string &text = c.hasLog ? script_h.logState.logEntryIndexToDialogueData(c.logIndex).text : *c.textValue;
		const std::string cacheKey = makeScrollableTextCacheKey(text, fi, wrapLimit, si.tightlyFit);

		auto &cached = si.textCache[j];
		if (cached.image && cached.key == cacheKey) {
			gpu.copyGPUImage(cached.image, nullptr, &localClip, target, textX - cached.padding, textY - cached.padding);
			continue;
		}
		if (cached.image)
			freeScrollableCachedText(cached);

		TextRenderingState state;
		state.tightlyFit                     = si.tightlyFit;
		state.shiftSpriteDrawByBorderPadding = false;
		uint16_t tw = 0, th = 0;
		dlgCtrl.prepareForRendering(text.c_str(), fi, state, tw, th);
		if (tw > 0 && th > 0) {
			const int padding = scrollableTextCachePadding(state);
			const int imageW  = std::min<int>(0xffff, static_cast<int>(tw) + padding * 2);
			const int imageH  = std::min<int>(0xffff, static_cast<int>(th) + padding * 2);
			RenderImage *textImage = gpu.createImage(imageW, imageH, 4);
			GPU_GetTarget(textImage);
			gpu.clear(textImage->target, 0, 0, 0, 0);
			state.offset.x = padding;
			state.offset.y = padding;
			state.dst.target = textImage->target;
			dlgCtrl.render(state);
			cached.image   = textImage;
			cached.key     = cacheKey;
			cached.padding = padding;
			gpu.copyGPUImage(textImage, nullptr, &localClip, target, textX - padding, textY - padding);
		}
		state.clear();
	}

	//sendToLog(LogLevel::Info, "scroll_y: %u\n", scroll_y);
	// all elements drawn
	// return
	GPU_UnsetClip(target);
}

// Probably belongs somewhere other than animation.cpp ... i think we will want a dedicated scrollable.cpp personally
// This method takes a specially scrollable AI and goes through its element tree,
// computing the y-position of the elements according to the value of their "height" key,
// and storing it in a newly computed "y" key which can be used by the draw function
void ONScripter::layoutSpecialScrollable(AnimationInfo *info) {
	if (!info->scrollableInfo.isSpecialScrollable)
		return;
	AnimationInfo::ScrollableInfo &si = info->scrollableInfo;
	StringTree &tree                  = dataTrees[si.elementTreeIndex];
	int dividerHeight                 = 0;
	int currentColumn                 = 0;
	if (si.divider)
		dividerHeight = si.divider->pos.h;
	int currentY = si.firstMargin + dividerHeight; // the top gets a divider too
	int currentX = 0;
	int height   = 0;

	assert(tree.insertionOrder.begin() + si.layoutedElements <= tree.insertionOrder.end());

	for (auto it = tree.insertionOrder.begin() + si.layoutedElements; it < tree.insertionOrder.end(); ++it, si.layoutedElements++) {
		auto &s = *it;

		StringTree &t = tree[s];
		if (t.has("log")) {
			bool read = script_h.logState.logEntryIndexToIsRead(std::stoi(t["log"].value));
			if (!read) {
				height = 0;
				break;
			}
		}

		height    = t.has("height") ? std::stoi(t["height"].value) : si.elementHeight;
		int width = t.has("width") ? std::stoi(t["width"].value) :
		                             si.elementWidth ? si.elementWidth :
		                                               info->pos.w;
		if (currentColumn > 0)
			t["x"].value = std::to_string(currentX);
		t["y"].value = std::to_string(currentY);
		if (si.columns > 1)
			t["col"].value = std::to_string(currentColumn);

		if (height == 0) {
			// autocalculate height
			int marginLeft = t.has("textmarginwidth") ? std::stoi(t["textmarginwidth"].value) :
			                                            t.has("textmarginleft") ? std::stoi(t["textmarginleft"].value) : si.textMarginLeft;
			int marginRight = t.has("textmarginwidth") ? std::stoi(t["textmarginwidth"].value) :
			                                             t.has("textmarginright") ? std::stoi(t["textmarginright"].value) : si.textMarginRight;
			calculateDynamicElementHeight(t, width - (marginLeft + marginRight), si.tightlyFit);
			height = std::stoi(t["height"].value);
		}

		// move on to next element position (if columns are not in use then currentColumn==0 always)
		if (it + 1 != tree.insertionOrder.end()) {
			currentColumn = (currentColumn + 1) % si.columns;
			if (currentColumn == 0) {
				currentY += height + dividerHeight;
				currentX = 0;
			} else {
				currentX += width + si.columnGap;
			}
		}
	}
	si.totalHeight = currentY + si.lastMargin + height;
	//tree.accept(StringTreePrinter());
}

void ONScripter::calculateDynamicElementHeight(StringTree &element, int width, int tightlyFit) {
	if (!element.has("text") && !element.has("log")) {
		element["height"].value = "0";
		return;
	}
	Fontinfo fi = sentence_font;
	fi.clear();
	fi.top_xy[0]                 = 0;
	fi.top_xy[1]                 = 0;
	fi.changeStyle().can_loghint = true;
	fi.changeStyle().wrap_limit  = width;
	std::string &text            = element.has("log") ? script_h.logState.logEntryIndexToDialogueData(std::stoi(element["log"].value)).text : element["text"].value;
	char *txt                    = const_cast<char *>(text.data());
	RenderRect bounds;
	dlgCtrl.renderToTarget(nullptr, &bounds, txt, &fi, false, tightlyFit); // dummy draw; get size
	element["height"].value = std::to_string(bounds.y + bounds.h);
}

void ONScripter::changeScrollableHoveredElement(AnimationInfo *info, Direction d) {
	AnimationInfo::ScrollableInfo &si       = info->scrollableInfo;
	StringTree &tree                        = dataTrees[si.elementTreeIndex];
	bool currentHoveredElemPartiallyVisible = false;
	long firstVisibleElemId = -1, lastVisibleElemId = -1;
	for (auto it = getScrollableElementsVisibleAt(&si, tree, info->scrollable.y + si.firstMargin); it != tree.insertionOrder.end(); ++it) {
		auto elemId = it - tree.insertionOrder.begin();
		if (firstVisibleElemId == -1)
			firstVisibleElemId = elemId;
		if (si.hoveredElement == elemId) {
			currentHoveredElemPartiallyVisible = true;
		}
		float w = si.elementWidth ? si.elementWidth : info->pos.w;
		float h = si.elementHeight;
		RenderRect elemRect{0, 0, w, h};
		setRectForScrollableElement(&tree[*it], elemRect);
		if (elemRect.y - info->scrollable.y >= info->pos.h - si.lastMargin) {
			// we're off the bottom of the visible area, break
			lastVisibleElemId = elemId - 1;
			break;
		}
	}

	// This fixes a bug in lookback when one hovers the last element and presses down key
	// Same thing done below
	long maxId = si.layoutedElements - 1; //tree.insertionOrder.size() - 1;
	if (lastVisibleElemId == -1)
		lastVisibleElemId = maxId;

	if (currentHoveredElemPartiallyVisible || (si.snapType != AnimationInfo::ScrollSnap::NONE &&
	                                           tree.getById(si.hoveredElement).has("y") && tree.getById(si.snappedElement).has("y") &&
	                                           std::stoi(tree.getById(si.hoveredElement)["y"].value) == std::stoi(tree.getById(si.snappedElement)["y"].value))) {

		switch (d) {
			case Direction::LEFT:
				if (si.hoveredElement - 1 >= 0)
					si.hoveredElement--;
				break;
			case Direction::RIGHT:
				if (si.hoveredElement + 1 <= maxId)
					si.hoveredElement++;
				break;
			case Direction::UP:
				if (si.hoveredElement - si.columns >= 0)
					si.hoveredElement -= si.columns;
				break;
			case Direction::DOWN:
				if (si.hoveredElement + si.columns <= maxId)
					si.hoveredElement += si.columns;
				break;
		}

		float w = si.elementWidth ? si.elementWidth : info->pos.w;
		float h = si.elementHeight;
		RenderRect elemRect{0, 0, w, h};
		int dividerH = si.divider ? si.divider->orig_pos.h : 0;
		setRectForScrollableElement(&tree.getById(si.hoveredElement), elemRect);

		if (elemRect.y + dividerH < info->scrollable.y + si.firstMargin) {
			snapScrollableToElement(info, si.hoveredElement, AnimationInfo::ScrollSnap::TOP);
			//sendToLog(LogLevel::Info, "Snapping up to %d\n", si.hoveredElement);
		}
		if (elemRect.y + elemRect.h + dividerH > info->scrollable.y + info->pos.h - si.lastMargin) {
			snapScrollableToElement(info, si.hoveredElement, AnimationInfo::ScrollSnap::BOTTOM);
			//sendToLog(LogLevel::Info, "Snapping down to %d\n", si.hoveredElement);
		} //FIXME: the use of info->scrollable.y here will probably create issues since it's being animated

	} else {
		//CHECKME: do these snaps need a condition?
		if (d == Direction::UP || d == Direction::LEFT) {
			si.hoveredElement = lastVisibleElemId;
			snapScrollableToElement(info, si.hoveredElement, AnimationInfo::ScrollSnap::BOTTOM);
		} else {
			si.hoveredElement = firstVisibleElemId;
			snapScrollableToElement(info, si.hoveredElement, AnimationInfo::ScrollSnap::TOP);
		}
	}
	fillCanvas(true, true); // temp
	flush(refreshMode());
}

void ONScripter::snapScrollableByOffset(AnimationInfo *info, int rowsDownwards) {
	if (!rowsDownwards)
		sendToLog(LogLevel::Error, "!? Asked to scroll a scrollable by 0 elements\n");
	AnimationInfo::ScrollableInfo &si = info->scrollableInfo;
	StringTree &tree                  = dataTrees[si.elementTreeIndex];
	long maxId                        = si.layoutedElements - 1; //tree.insertionOrder.size() - 1;
	bool alreadySnappedCorrectlyDown  = (rowsDownwards > 0 && si.snapType == AnimationInfo::ScrollSnap::BOTTOM);
	bool alreadySnappedCorrectlyUp    = (rowsDownwards < 0 && si.snapType == AnimationInfo::ScrollSnap::TOP);
	if (!alreadySnappedCorrectlyDown && !alreadySnappedCorrectlyUp) {
		// we need to find an appropriate element to begin the snap with
		long firstVisibleElemId = -1, lastVisibleElemId = -1;
		for (auto it = getScrollableElementsVisibleAt(&si, tree, info->scrollable.y + si.firstMargin); it != tree.insertionOrder.end(); ++it) {
			long elemId = it - tree.insertionOrder.begin();
			if (firstVisibleElemId == -1)
				firstVisibleElemId = elemId;
			float w = si.elementWidth ? si.elementWidth : info->pos.w;
			float h = si.elementHeight;
			RenderRect elemRect{0, 0, w, h};
			setRectForScrollableElement(&tree[*it], elemRect);
			if (elemRect.y - info->scrollable.y >= info->pos.h - si.lastMargin) {
				// we're off the bottom of the visible area, break
				lastVisibleElemId = elemId - 1;
				break;
			}
		}
		if (rowsDownwards < 0) {
			si.snappedElement = firstVisibleElemId;
			si.snapType       = AnimationInfo::ScrollSnap::TOP;
		} else {
			si.snappedElement = lastVisibleElemId;
			si.snapType       = AnimationInfo::ScrollSnap::BOTTOM;
		}
	}
	// ok, snap is set properly, now let's process this offset
	si.snappedElement += si.columns * rowsDownwards;
	if (si.snappedElement < 0)
		si.snappedElement = 0;
	if (si.snappedElement > maxId)
		si.snappedElement = maxId;
	snapScrollableToElement(info, si.snappedElement, si.snapType);
}

void ONScripter::snapScrollableToElement(AnimationInfo *info, long elementId, AnimationInfo::ScrollSnap snapType, bool instant) {
	if (snapType == AnimationInfo::ScrollSnap::NONE)
		sendToLog(LogLevel::Error, "!? Snap but don't snap? Think before you speak\n");
	if (!info->scrollableInfo.isSpecialScrollable)
		sendToLog(LogLevel::Error, "!? That isn't even a scrollable\n");
	AnimationInfo::ScrollableInfo &si = info->scrollableInfo;
	StringTree &tree                  = dataTrees[si.elementTreeIndex];
	si.snappedElement                 = elementId;
	si.snapType                       = snapType;
	float w                           = si.elementWidth ? si.elementWidth : info->pos.w;
	float h                           = si.elementHeight;
	RenderRect elemRect{0, 0, w, h};
	setRectForScrollableElement(&tree.getById(elementId), elemRect);
	int dividerH  = si.divider ? si.divider->orig_pos.h : 0;
	float dstYTop = snapType == AnimationInfo::ScrollSnap::TOP ? elemRect.y - dividerH - si.firstMargin : elemRect.y + elemRect.h - info->pos.h + si.lastMargin + dividerH;
	bool lsp2;
	int num = getAIno(info, lsp2);
	if (!instant)
		dynamicProperties.addSpriteProperty(info, num, lsp2, true, SPRITE_PROPERTY_SCROLLABLE_Y, dstYTop, 100, 1, true);
	else
		dynamicProperties.addSpriteProperty(info, num, lsp2, true, SPRITE_PROPERTY_SCROLLABLE_Y, dstYTop);
	//sendToLog(LogLevel::Info, "Setting dynamic property with destination %f (current pos %f)\n", dstYTop, info->scrollable.y);
}

// returns iterator to si->insertionOrder
void ONScripter::ensureScrollableGeometryCache(AnimationInfo::ScrollableInfo *si, StringTree &tree) {
	const size_t treeSize = std::min(tree.insertionOrder.size(), static_cast<size_t>(std::max<long>(0, si->layoutedElements)));
	if (si->geometryCacheTreeVersion == tree.modificationVersion &&
	    si->geometryCacheLayoutedElements == si->layoutedElements &&
	    si->geometryCacheTreeSize == treeSize && !si->geometryCache.empty())
		return;

	si->geometryCache.clear();
	si->yEndCache.clear();
	si->geometryCache.reserve(treeSize);
	si->yEndCache.reserve(treeSize);
	for (size_t i = 0; i < treeSize; ++i) {
		auto found = tree.branches.find(tree.insertionOrder[i]);
		if (found == tree.branches.end()) {
			si->geometryCache.push_back(AnimationInfo::ScrollableInfo::CachedGeometry{});
			si->yEndCache.push_back(0x7fffffff);
			continue;
		}
		StringTree &t = found->second;
		AnimationInfo::ScrollableInfo::CachedGeometry c{};
		const bool hasY = t.has("y");
		c.x               = t.has("x") ? std::stoi(t["x"].value) : 0;
		c.y               = hasY ? std::stoi(t["y"].value) : 0;
		c.width           = t.has("width") ? std::stoi(t["width"].value) : si->elementWidth;
		c.height          = t.has("height") ? std::stoi(t["height"].value) : si->elementHeight;
		c.textMarginLeft  = t.has("textmarginwidth") ? std::stoi(t["textmarginwidth"].value) : (t.has("textmarginleft") ? std::stoi(t["textmarginleft"].value) : si->textMarginLeft);
		c.textMarginRight = t.has("textmarginwidth") ? std::stoi(t["textmarginwidth"].value) : (t.has("textmarginright") ? std::stoi(t["textmarginright"].value) : si->textMarginRight);
		c.textMarginTop   = t.has("textmargintop") ? std::stoi(t["textmargintop"].value) : si->textMarginTop;
		c.bgSpriteIndex   = t.has("bg") ? std::stoi(t["bg"].value) : -1;
		c.logIndex        = t.has("log") ? std::stoi(t["log"].value) : -1;
		c.hasText         = t.has("text");
		c.hasLog          = t.has("log");
		c.textValue       = c.hasText ? &t["text"].value : nullptr;
		si->geometryCache.push_back(c);
		si->yEndCache.push_back(hasY ? (c.y + c.height) : 0x7fffffff);
	}
	si->geometryCacheTreeVersion      = tree.modificationVersion;
	si->geometryCacheLayoutedElements = si->layoutedElements;
	si->geometryCacheTreeSize         = treeSize;
}

std::vector<std::string>::iterator ONScripter::getScrollableElementsVisibleAt(AnimationInfo::ScrollableInfo *si, StringTree &tree, int y) {
	ensureScrollableGeometryCache(si, tree);
	if (!si->yEndCache.empty() && si->yEndCache.size() == si->geometryCache.size()) {
		auto it = std::lower_bound(si->yEndCache.begin(), si->yEndCache.end(), y);
		size_t idx = static_cast<size_t>(it - si->yEndCache.begin());
		if (idx > si->geometryCache.size())
			idx = si->geometryCache.size();
		return tree.insertionOrder.begin() + idx;
	}
	return tree.insertionOrder.end();
}

void ONScripter::setRectForScrollableElement(StringTree *elem, RenderRect &rect) {
	if (elem->has("x"))
		rect.x = std::stoi((*elem)["x"].value);
	if (elem->has("y"))
		rect.y = std::stoi((*elem)["y"].value);
	if (elem->has("width"))
		rect.w = std::stoi((*elem)["width"].value);
	if (elem->has("height"))
		rect.h = std::stoi((*elem)["height"].value);
}

// Script coords with scroll already taken into account, so y can be 0 to infinity.
void ONScripter::mouseOverSpecialScrollable(int aiSpriteNo, int x, int y) {
	//sendToLog(LogLevel::Info, "moused over with coords %d, %d\n", x, y);
	AnimationInfo *ai                 = &sprite_info[aiSpriteNo];
	AnimationInfo::ScrollableInfo *si = &ai->scrollableInfo;
	StringTree &tree                  = dataTrees[si->elementTreeIndex];
	for (auto it = getScrollableElementsVisibleAt(si, tree, y); it != tree.insertionOrder.end(); ++it) {
		StringTree &elem = tree[*it];
		float w          = si->elementWidth;
		float h          = si->elementHeight;
		RenderRect elemRect{0, 0, w, h};
		setRectForScrollableElement(&elem, elemRect);
		if (elemRect.y > y) {
			// Went too far down, found nothing
			si->mouseCursorIsOverHoveredElement = false;
			return;
		}
		if (x >= elemRect.x && x < elemRect.x + elemRect.w &&
		    y >= elemRect.y && y < elemRect.y + elemRect.h) {
			si->mouseCursorIsOverHoveredElement = true;
			// int newHover = std::stoi(*it);
			// stoi causes a crash with string keys. Using indices might be wrong but I cannot think of a particular issue atm.
			long newHover = it - tree.insertionOrder.begin();
			if (newHover != si->hoveredElement) {
				si->hoveredElement = newHover;
				dirtySpriteRect(aiSpriteNo, false);
				flush(refreshMode());
			}
			return;
		}
	}
	// Deliberately no code here for setting hoveredElement to -1 or something if we are over blank space.
	// This covers the case when we mouseover something, then nothing, and then try to use the gamepad.
	// We have to remember where we were somehow or gamepad can't continue.
	// So instead we do this:
	si->mouseCursorIsOverHoveredElement = false;
}

void ONScripter::drawBigImage(RenderTarget *target, AnimationInfo *info, int /*refresh_mode*/, RenderRect *clip, bool centre_coordinates) {
	RenderRect targetClip = clip ? *clip : RenderRect{-camera.center_pos.x, -camera.center_pos.y, static_cast<float>(window.canvas_width), static_cast<float>(window.canvas_height)};

	float scale_x     = info->scale_x / 100.0;
	float scale_y     = info->scale_y / 100.0;
	float bound_off_x = 0;
	float bound_off_y = 0;

	int base_cell_off_x = info->vertical_cells ? 0 : info->pos.w * info->current_cell;
	int base_cell_off_y = info->vertical_cells ? info->pos.h * info->current_cell : 0;
	int cell_off_x      = base_cell_off_x;
	int cell_off_y      = base_cell_off_y;

	RenderRect bounding_rect = info->bounding_rect;
	if (info->scrollable.h > 0) {
		cell_off_y += info->scrollable.y;
		bounding_rect.h = info->scrollable.h;
	}
	if (info->scrollable.w > 0) {
		cell_off_x += info->scrollable.x;
		bounding_rect.w = info->scrollable.w;
	}

	RenderImage *sprite_transformation_image{nullptr};
	bool needs_sprite_transformation_image{false};
	RenderRect sourceClip = info->pos;

	if (scale_x == 1 && scale_y == 1) {
		// sourceClip has script coordinates
		sourceClip.x = bounding_rect.x; // remove lsp2 specfics
		sourceClip.y = bounding_rect.y; // remove lsp2 specfics
		if (doClipping(&sourceClip, &targetClip))
			return;
		sourceClip.x -= bounding_rect.x; // switch to image coordinates
		sourceClip.y -= bounding_rect.y; // switch to image coordinates
		// Change the cell
		sourceClip.x += cell_off_x;
		sourceClip.y += cell_off_y;
	} else if (scale_x >= 1 && scale_y >= 1) {
		needs_sprite_transformation_image = true;

		// we have script coordinates in bounding_rect, containing the area a scaled image covers
		// we have a relatively small temp image (smaller than BigImage) we need to fit our unscaled area in

		// Forget about clips for now
		sourceClip.x = cell_off_x;
		sourceClip.y = cell_off_y;

		RenderRect tmp = bounding_rect;

		// We are working with canvas then let's use canvas coordinates for lower calculations
		tmp.x += camera.center_pos.x;
		tmp.y += camera.center_pos.y;

		// Calculate visible offsets...

		// Firstly the negative area (top-left part)
		if (tmp.x < 0) {
			sourceClip.x = -tmp.x / scale_x;
			// Shift the width
			sourceClip.w -= sourceClip.x;
			tmp.w += tmp.x; // 0 -> offscreen
		} else {
			// If the image top-left edge is visible on the opposite
			bound_off_x = tmp.x / scale_x;
		}
		if (tmp.y < 0) {
			sourceClip.y = -tmp.y / scale_y;
			// Shift the neight
			sourceClip.h -= sourceClip.y;
			tmp.h += tmp.y; // 0 -> offscreen
		} else {
			// If the image top-left edge is visible on the opposite
			bound_off_y = tmp.y / scale_y;
		}

		// Secondly the positive area (bottom-right part)
		if (tmp.w > window.canvas_width)
			sourceClip.w -= (tmp.w - window.canvas_width) / scale_x;
		if (tmp.h > window.canvas_height)
			sourceClip.h -= (tmp.h - window.canvas_height) / scale_y;

		// Fix possible out of scope
		if (sourceClip.w < 0)
			sourceClip.w = 0;
		if (sourceClip.h < 0)
			sourceClip.h = 0;

		// At this point we know the area of the image we need to display

		// Now we have a max possible area we can display on the canvas image starting from the top-left of a sprite_transformation_image
	} else {
		ons.errorAndExit("Big images cannot be zoomed out!");
		return; //dummy
	}

	if (doClipping(&targetClip, &bounding_rect)) {
		if (sprite_transformation_image)
			gpu.giveCanvasImage(sprite_transformation_image);
		return;
	}

	if (needs_sprite_transformation_image) {
		sprite_transformation_image = gpu.getCanvasImage();
		GPU_SetImageFilter(sprite_transformation_image, GPU_FILTER_LINEAR);
	}

	// Switch to canvas (dst) coords
	targetClip.x += camera.center_pos.x;
	targetClip.y += camera.center_pos.y;

	gpu.pushBlendMode(info->blending_mode);

	bool allowDirectCopy{false};

	if (info->trans_mode == AnimationInfo::TRANS_COPY && // the sprite has no alpha
	    gpu.blend_mode.top() == BlendModeId::NORMAL &&   // no weird blending modes
	    info->trans >= 255)                              // must not be transparent at all
		allowDirectCopy = true;

	if (perfTelemetryEnabled())
		++bigImageCellCacheTelemetry.drawCalls;

	if (!sprite_transformation_image && scale_x == 1 && scale_y == 1) {
		if (RenderImage *cellCache = ensureBigImageCellCache(info, base_cell_off_x, base_cell_off_y)) {
			RenderRect cacheSource{sourceClip.x - base_cell_off_x,
			                       sourceClip.y - base_cell_off_y,
			                       sourceClip.w,
			                       sourceClip.h};
			const GPU_bool previousBlending = cellCache->use_blending;
			const SDL_Color previousColor   = cellCache->color;

			if (allowDirectCopy)
				GPU_SetBlending(cellCache, false);
			if (info->trans < 255)
				GPU_SetRGBA(cellCache, info->trans, info->trans, info->trans, info->trans);

			gpu.copyGPUImage(cellCache, &cacheSource, &targetClip, target,
			                 targetClip.x + targetClip.w / 2.0,
			                 targetClip.y + targetClip.h / 2.0,
			                 1, 1, 0, true);

			if (info->trans < 255)
				GPU_SetRGBA(cellCache, previousColor.r, previousColor.g, previousColor.b, previousColor.a);
			if (allowDirectCopy)
				GPU_SetBlending(cellCache, previousBlending);
			gpu.popBlendMode();
			if (perfTelemetryEnabled())
				++bigImageCellCacheTelemetry.cacheDraws;
			return;
		}
	} else if (perfTelemetryEnabled()) {
		++bigImageCellCacheTelemetry.fallbackScaledDraws;
	}

	auto chunks = info->big_image->getImagesForArea(sourceClip);
	if (perfTelemetryEnabled()) {
		++bigImageCellCacheTelemetry.fallbackChunkDraws;
		bigImageCellCacheTelemetry.sourceChunksDrawn += chunks.size();
	}
	for (auto &chunk : chunks) {
		float x = chunk.second.x + chunk.first->w / 2.0;
		float y = chunk.second.y + chunk.first->h / 2.0;

		if (sprite_transformation_image) {
			GPU_SetBlending(chunk.first, false);

			gpu.copyGPUImage(chunk.first, nullptr, nullptr, sprite_transformation_image->target,
			                 x - sourceClip.x + bound_off_x,
			                 y - sourceClip.y + bound_off_y,
			                 1, 1, 0, centre_coordinates);

			GPU_SetBlending(chunk.first, true);
		} else {
			if (allowDirectCopy)
				GPU_SetBlending(chunk.first, false);
			if (info->trans < 255)
				GPU_SetRGBA(chunk.first, info->trans, info->trans, info->trans, info->trans);
			gpu.copyGPUImage(chunk.first, nullptr, &targetClip, target,
			                 x + camera.center_pos.x + bounding_rect.x - cell_off_x,
			                 y + camera.center_pos.y + bounding_rect.y - cell_off_y,
			                 1, 1, 0, centre_coordinates);
			if (allowDirectCopy)
				GPU_SetBlending(chunk.first, true);
			if (info->trans < 255)
				GPU_SetRGBA(chunk.first, 255, 255, 255, 255);
		}
	}

	if (sprite_transformation_image) {
		if (info->trans < 255)
			GPU_SetRGBA(sprite_transformation_image, info->trans, info->trans, info->trans, info->trans);
		gpu.copyGPUImage(sprite_transformation_image, nullptr, &targetClip, target,
		                 sprite_transformation_image->w / 2.0 * scale_x, sprite_transformation_image->h / 2.0 * scale_y,
		                 scale_x, scale_y, 0, centre_coordinates);
		if (info->trans < 255)
			GPU_SetRGBA(sprite_transformation_image, 255, 255, 255, 255);
	}

	gpu.popBlendMode();

	if (sprite_transformation_image)
		gpu.giveCanvasImage(sprite_transformation_image);
}

void ONScripter::offsetSpriteRangeDrawPosition(const AnimationInfo *info, float &x, float &y) const {
	if (!spriteRangeMotion.active || !info)
		return;
	if (info->id < spriteRangeMotion.start || info->id > spriteRangeMotion.end)
		return;
	if (info->type == SPRITE_LSP && !spriteRangeMotion.includeLsp)
		return;
	if (info->type == SPRITE_LSP2 && !spriteRangeMotion.includeLsp2)
		return;
	if (info->type != SPRITE_LSP && info->type != SPRITE_LSP2)
		return;

	x += spriteRangeMotion.xOffset;
	y += spriteRangeMotion.yOffset;
}

void ONScripter::drawToGPUTarget(RenderTarget *target, AnimationInfo *info, int refresh_mode, RenderRect *clip, bool centre_coordinates) {
	if (!target) {
		sendToLog(LogLevel::Error, "drawToGPUTarget has no proper target\n");
		return;
	}

	// Don't draw sprites that have a parent independently (they are drawn as part of the drawToGPUTarget(tgt, parent, ...))
	if (info->parentImage.no != -1) {
		return;
	}

	const bool telemetryScopeActive = !(info->layer_no >= 0 && info->trans_mode == AnimationInfo::TRANS_LAYER);
	OptionalGPUTelemetryScope telemetryScope(telemetryScopeActive, animationDrawTelemetrySource(info));

	RenderRect real_clip{0, 0, static_cast<float>(target->w), static_cast<float>(target->h)};
	if (clip) {
		real_clip = *clip;
		real_clip.x += camera.center_pos.x;
		real_clip.y += camera.center_pos.y;
	}

	RenderImage *sprite_transformation_image = nullptr;
	RenderImage *subimage_compositing_image  = nullptr;
	RenderImage *src                         = nullptr;

	bool opacityTransform = info->darkenHue.r < 255 || info->darkenHue.g < 255 ||
	                        info->darkenHue.b < 255 || info->trans < 255;

	float coord_x = info->rot == 0 && !info->has_hotspot ? info->pos.x : info->rendering_center.x;
	float coord_y = info->rot == 0 && !info->has_hotspot ? info->pos.y : info->rendering_center.y;

	// Adjust by sprite-specific camera
	coord_x += info->camera.pos.x;
	coord_y += info->camera.pos.y;
	offsetSpriteRangeDrawPosition(info, coord_x, coord_y);

	/* A paint at 0,0 paints to
	 ----------------------
	 |  here              |
	 |  *--------------   |
	 |  |  script_w/h |   |
	 |  ---------------   |
	 |        canvas_w/h  |
	 ----------------------
		instead of to the top left corner of the canvas.
		(The alternative is to switch everything to using centered coordinates or change all the positions in our script...) */
	coord_x += camera.center_pos.x;
	coord_y += camera.center_pos.y;

	bool needTransformationImage{true};
	BreakupID breakupID{{BreakupType::NONE, 0}};
	if (info->spriteTransforms.hasNoneExceptMaybeBreakup()) {
		// We might be able to get away without a transformation image, but let's consider breakup carefully first.
		if (info->spriteTransforms.breakupFactor == 0) {
			// No breakup either! Great! No transformation image required.
			needTransformationImage = false;
		} else {
			// OK, we have breakup... we might need a secondary image still.
			breakupID.id = info->id;
			if (!ons.new_breakup_implementation) {
				// It's old breakup. There's nothing we can do to optimize that.
				breakupID.type = BreakupType::SPRITE_CANVAS;
			} else {
				// New breakup. We only need a transformation image if it violates one of our conditions.
				bool breakupTightfits = info->rot == 0 && info->scale_x == 100 && info->scale_y == 100 && info->flip == 0 && info->layer_no == -1;
				if (!breakupTightfits) {
					// Support needs to be added into breakUpImage for us to deal with this kind of complication.
					// For now, this has to break up as BreakupType::SPRITE_CANVAS. :(
					breakupID.type = BreakupType::SPRITE_CANVAS;
				} else if (opacityTransform) {
					// It does have an opacity transform, but we can still kind of do something optimized with this.
					// We can use BreakupType::SPRITE_TIGHTFIT at least, though we still need to use a secondary texture.
					breakupID.type = BreakupType::SPRITE_TIGHTFIT;
				} else {
					// It's just a regular, very optimized new breakup. We can TIGHTFIT and we don't need anything special :)
					breakupID.type          = BreakupType::SPRITE_TIGHTFIT;
					needTransformationImage = false;
				}
			}
		}
	}

	// going to support sprite_transformation_image on special scrollables i think, should be no extra effort
	if (info->scrollableInfo.isSpecialScrollable) {
		if (needTransformationImage)
			errorAndExit("Cannot transform a SpecialScrollable");
		drawSpecialScrollable(target, info, refresh_mode, &real_clip);
		return;
	}
	if (info->is_big_image && info->big_image.get()) {
		if (needTransformationImage)
			errorAndExit("Cannot transform a BigImage");
		drawBigImage(target, info, refresh_mode, clip, centre_coordinates);
		return;
	}
	if (info->layer_no >= 0 && info->trans_mode == AnimationInfo::TRANS_LAYER) {
		if (needTransformationImage)
			sprite_transformation_image = gpu.getCanvasImage();

		auto handler      = getLayer<Layer>(info->layer_no, false);
		auto layer_target = target;
		auto mode         = handler->blendingMode(refresh_mode);
		if (sprite_transformation_image)
			layer_target = sprite_transformation_image->target;
		// TODO: Layers need some kind of support for flip at least. They may need to use a sprite_transformation_image if they have one of those properties set.
		if (mode != BlendModeId::NORMAL)
			gpu.pushBlendMode(mode);
		handler->refresh(layer_target, real_clip, coord_x, coord_y, centre_coordinates, refresh_mode,
		                 (info->flip & FLIP_HORIZONTALLY ? -1 : 1) * (info->scale_x ? info->scale_x / 100.0 : 1),
		                 (info->flip & FLIP_VERTICALLY ? -1 : 1) * (info->scale_y ? info->scale_y / 100.0 : 1));
		if (mode != BlendModeId::NORMAL)
			gpu.popBlendMode();
		if (!sprite_transformation_image)
			return;
	} else {
		if (!info->gpu_image)
			return;

		if (needTransformationImage)
			sprite_transformation_image = gpu.getCanvasImage();

		src = info->gpu_image;

		RenderRect clip_rect = cellClipRect(info, info->current_cell);
		if (info->scrollable.h > 0) {
			clip_rect.h = clip_rect.h > info->scrollable.h ? info->scrollable.h : clip_rect.h;
			clip_rect.y = info->scrollable.y;
		}
		if (info->scrollable.w > 0) {
			clip_rect.w = clip_rect.w > info->scrollable.w ? info->scrollable.w : clip_rect.w;
			clip_rect.x = info->scrollable.x;
		}

		if (!info->childImages.empty()) {
			bool foundNonNullChild{false};
			// Copy all the child images onto it in order
			for (auto &zLevel : info->childImages) {
				int sprite_no        = zLevel.second.no;
				bool lsp2            = zLevel.second.lsp2;
				AnimationInfo *child = (lsp2 ? sprite2_info[sprite_no] : sprite_info[sprite_no]).oldNew(refresh_mode);
				if (!child->gpu_image)
					continue;
				if (!foundNonNullChild) {
					// First copy the parent image
					foundNonNullChild = true;
					// Limitation: The parent image cannot be larger than the canvas
					subimage_compositing_image = gpu.getCanvasImage();
					GPU_SetBlending(src, false);
					gpu.copyGPUImage(src, &clip_rect, nullptr, subimage_compositing_image->target, 0, 0, 1, 1, 0, false);
					GPU_SetBlending(src, true);
				}
				clip_rect = {0, 0, child->pos.w, child->pos.h};
				// Respect child image cells if they have them
				if (child->num_of_cells > 1 && child->current_cell != 0) {
					if (!child->vertical_cells)
						clip_rect.x += child->pos.w * child->current_cell;
					else
						clip_rect.y += child->pos.h * child->current_cell;
				}

				gpu.pushBlendMode(BlendModeId::NORMAL);
				gpu.copyGPUImage(child->gpu_image, &clip_rect, nullptr, subimage_compositing_image->target, child->pos.x, child->pos.y, 1, 1, 0, false);
				gpu.popBlendMode();
			}
			if (foundNonNullChild) {
				src       = subimage_compositing_image;
				clip_rect = {0, 0, info->pos.w, info->pos.h};
			}
		}

		if (!sprite_transformation_image) {
			gpu.pushBlendMode(info->blending_mode);

			// Blend with transparency, but only if we do not have further blending to do.
			if (opacityTransform) {
				GPU_SetRGBA(src, info->trans * info->darkenHue.r / 255, info->trans * info->darkenHue.g / 255,
				            info->trans * info->darkenHue.b / 255, info->trans);
			}
		}

		RenderTarget *dst    = sprite_transformation_image ? sprite_transformation_image->target : target;
		RenderRect *dst_clip = sprite_transformation_image ? nullptr : &real_clip;
		float scale_x      = (info->flip & FLIP_HORIZONTALLY ? -1 : 1) * (info->scale_x ? info->scale_x / 100.0 : 1);
		float scale_y      = (info->flip & FLIP_VERTICALLY ? -1 : 1) * (info->scale_y ? info->scale_y / 100.0 : 1);

		if (breakupID.type == BreakupType::SPRITE_TIGHTFIT) {
			//TODO:
			// * remove - half w/h, breakUpImage should work with centred coordinates
			// * add scale factor with flip support
			// * add clips
			gpu.breakUpImage(breakupID, src, &clip_rect, dst, info->spriteTransforms.breakupFactor,
			                 info->spriteTransforms.breakupDirectionFlagset, nullptr, coord_x - info->pos.w / 2,
			                 coord_y - info->pos.h / 2);
		} else {
			bool allowDirectCopy{false};

			if (sprite_transformation_image || (                                                      // We're blitting onto a completely transparent surface, so there should be no need to blend.
			                                       (info->trans_mode == AnimationInfo::TRANS_COPY) && // the sprite has no alpha
			                                       (std::remainder(info->rot, 90) == 0) &&            // rectangular form (angles that do not line up with pixel boundaries probably require alpha blending, right?)
			                                       (gpu.blend_mode.top() == BlendModeId::NORMAL) &&   // no weird blending modes
			                                       (!info->scale_x && !info->scale_y) &&              // scaling could result in missing pixel boundaries again
			                                       (info->trans >= 255)                               // must not be transparent at all
			                                       )) {
				allowDirectCopy = true;
			};

			if (allowDirectCopy)
				GPU_SetBlending(src, false);
			bool interpolatedCursor = false;
			if (!sprite_transformation_image && isSmoothTextCursorSprite(info) && !info->scrollable.h && !info->scrollable.w) {
				RenderRect cursor_clip_rect = cellClipRect(info, 0);
				setSpriteRGBA(src, info, smoothTextCursorAlpha(info));
				gpu.copyGPUImage(src, &cursor_clip_rect, dst_clip, dst, coord_x, coord_y, scale_x, scale_y,
				                 -info->rot, centre_coordinates);
				interpolatedCursor = true;
			}
			if (!interpolatedCursor)
				gpu.copyGPUImage(src, &clip_rect, dst_clip, dst, coord_x, coord_y, scale_x, scale_y,
				                 //ONScripter uses right-to-left angling system and sdl-gpu prefers left-to-right. I prefer sdl-gpu, but we are to follow the standards.
				                 -info->rot, centre_coordinates);
			else if (!opacityTransform)
				GPU_SetRGBA(src, 255, 255, 255, 255);
			if (allowDirectCopy)
				GPU_SetBlending(src, true);
		}

		if (subimage_compositing_image)
			gpu.giveCanvasImage(subimage_compositing_image);
	}

	if (sprite_transformation_image) {
		src = sprite_transformation_image;
		PooledGPUImage toDraw;

		if (info->spriteTransforms.blurFactor > 0) {
			GPUTransformableCanvasImage tmp(src);
			toDraw = gpu.getBlurredImage(tmp, info->spriteTransforms.blurFactor);
			src    = toDraw.image;
			// We (have) set a larger clip for this in dirtySpriteRect to ensure we are called with a large enough clip
		}
		if (info->spriteTransforms.negative1) {
			GPUTransformableCanvasImage tmp(src);
			toDraw = gpu.getNegativeImage(tmp);
			src    = toDraw.image;
		}
		if (info->spriteTransforms.sepia) {
			GPUTransformableCanvasImage tmp(src);
			toDraw = gpu.getSepiaImage(tmp);
			src    = toDraw.image;
		}
		if (info->spriteTransforms.greyscale) {
			GPUTransformableCanvasImage tmp(src);
			toDraw = gpu.getGreyscaleImage(tmp, info->darkenHue);
			src    = toDraw.image;
		}
		if (info->spriteTransforms.negative2) {
			GPUTransformableCanvasImage tmp(src);
			toDraw = gpu.getNegativeImage(tmp);
			src    = toDraw.image;
		}
		if (breakupID.type == BreakupType::SPRITE_CANVAS) {
			GPUTransformableCanvasImage tmp(src);
			toDraw = gpu.getBrokenUpImage(tmp, breakupID, info->spriteTransforms.breakupFactor,
			                              info->spriteTransforms.breakupDirectionFlagset, nullptr);
			src    = toDraw.image;
			// We (have) set a larger clip for this in dirtySpriteRect to ensure we are called with a large enough clip
		}
		if (info->spriteTransforms.warpAmplitude != 0) {
			GPUTransformableCanvasImage tmp(src);
			float secs = info->spriteTransforms.warpClock.time() / 1000.0;
			toDraw     = gpu.getWarpedImage(tmp, secs, info->spriteTransforms.warpAmplitude, info->spriteTransforms.warpWaveLength,
                                        info->spriteTransforms.warpSpeed);
			src        = toDraw.image;
			// We (have) set a larger clip for this in dirtySpriteRect to ensure we are called with a large enough clip
		}

		if (opacityTransform)
			GPU_SetRGBA(src, info->trans * info->darkenHue.r / 255,
			            info->trans * info->darkenHue.g / 255,
			            info->trans * info->darkenHue.b / 255, info->trans);

		if (info->layer_no >= 0 && info->trans_mode == AnimationInfo::TRANS_LAYER)
			gpu.pushBlendMode(getLayer<Layer>(info->layer_no, false)->blendingMode(refresh_mode));
		else
			gpu.pushBlendMode(info->blending_mode);

		gpu.copyGPUImage(src, nullptr, &real_clip, target, 0, 0, 1, 1, 0, false);
		gpu.giveCanvasImage(sprite_transformation_image);
	}

	gpu.popBlendMode();

	if (opacityTransform)
		GPU_SetRGBA(src, 255, 255, 255, 255);
}

void ONScripter::commitVisualState() {
	//sendToLog(LogLevel::Info, "commitVisualState\n");
	for (auto i : queueAnimationInfo) {
		//So that we don't break the new AIs by continuing to run wrong timed property changes
		if (i->old_ai && i->distinguish_from_old_ai) {
			//sendToLog(LogLevel::Info, "Terminating sprite properties\n");
			dynamicProperties.terminateSpriteProperties(i);
		}
		i->commitState();
		deinitBreakup({{BreakupType::SPRITE_TIGHTFIT, static_cast<int16_t>(i->id)}});
		deinitBreakup({{BreakupType::SPRITE_CANVAS, static_cast<int16_t>(i->id)}});
	}

	for (auto &i : spritesets) {
		commitSpriteset(&i.second);
	}

	if (layer_info)
		layer_info->commit();

	queueAnimationInfo.clear();

	monocro_flag[BeforeScene]  = monocro_flag[AfterScene];
	monocro_color[BeforeScene] = monocro_color[AfterScene];
	nega_mode[BeforeScene]     = nega_mode[AfterScene];
	blur_mode[BeforeScene]     = blur_mode[AfterScene];

	// We committed, so any changes made to the after scene must now be applied to the normal (before) scene
	before_dirty_rect_hud.add(dirty_rect_hud.bounding_box_script);
	before_dirty_rect_scene.add(dirty_rect_scene.bounding_box_script);
	dirty_rect_hud.clear();
	dirty_rect_scene.clear();
}

void ONScripter::backupState(AnimationInfo *info) {
	// Do not back up sprites with transitions disabled.
	// This enables HUD elements etc to move independently on the scene using properties without caring about what is happening ingame.
	if (nontransitioningSprites.contains(info)) {
		return;
	}

	if (!info->old_ai) {
		info->backupState();
		queueAnimationInfo.push_back(info);
	}
}

void ONScripter::commitSpriteset(SpritesetInfo *si) {
	dynamicProperties.terminateSpritesetProperties(si);
	cleanSpritesetCache(si, true);
	cleanSpritesetCache(si, false);
	si->commit();
}

void ONScripter::stopCursorAnimation(int click) {
	if (enable_custom_cursors)
		return;

	int no;

	if (textgosub_label || !draw_cursor_flag)
		return;

	if (click == CLICK_WAIT)
		no = CURSOR_WAIT_NO;
	else if (click == CLICK_NEWPAGE)
		no = CURSOR_NEWPAGE_NO;
	else
		return;

	if (cursor_info[no].gpu_image == nullptr)
		return;

	RenderRect dst_rect = cursor_info[no].pos;

	if (!cursor_info[no].abs_flag) {
		dst_rect.x += sentence_font.x();
		dst_rect.y += sentence_font.y();
	}

	RenderRect empty_rect{0, 0, 0, 0};
	flushDirect(empty_rect, dst_rect, refreshMode());
}

void ONScripter::buildGPUImage(AnimationInfo &ai) {
	if (!ai.image_surface) {
		//sendToLog(LogLevel::Error, "No image_surface in buildGPUImage\n");
		return;
	}

	ai.big_image.reset();
	if (ai.gpu_image) {
		gpu.freeImage(ai.gpu_image);
		ai.gpu_image = nullptr;
	}

	if (ai.image_surface->w == 0 || ai.image_surface->h == 0) {
		SDL_FreeSurface(ai.image_surface);
		ai.image_surface = nullptr;
		return;
	}

	//sendToLog(LogLevel::Info, "Creating gpu image in buildGPUImage (%d,%d)@%d\n",ai.image_surface->w, ai.image_surface->h, ai.image_surface->format->BytesPerPixel);

	if (ai.is_big_image) {
		ai.big_image = std::make_shared<GPUBigImage>(ai.image_surface);
	} else {
#if defined(ONS_USE_SDL3)
		// The transfer path converts alpha while staging bounded uploads. There
		// is no CPU image mirror, temporary texture or frame wait per chunk.
		ai.gpu_image = gpu.copyImageFromSurface(ai.image_surface, true);
#else
#if !defined(IOS) && !defined(DROID) // There is some issue with loadGPUImageByChunks on iOS
		if (!(skip_mode & SKIP_SUPERSKIP))
			ai.gpu_image = gpu.loadGPUImageByChunks(ai.image_surface);
		else
#endif
			ai.gpu_image = gpu.copyImageFromSurface(ai.image_surface);

		gpu.multiplyAlpha(ai.gpu_image);
#endif
		GPU_GetTarget(ai.gpu_image);
		GPU_DiscardImagePixels(ai.gpu_image);
	}
}

void ONScripter::freeRedundantSurfaces(AnimationInfo &ai) {
	if (!ai.image_surface) {
		// Can't free if it doesn't exist.
		return;
	}

	if (!ai.gpu_image && !ai.big_image) {
		// CPU-only images still need their surface.
		return;
	}

	if (ai.trans_mode == AnimationInfo::TRANS_STRING ||
	    ai.type == SPRITE_SENTENCE_FONT ||
	    ai.type == SPRITE_BUTTONS) {
		// Text surfaces and button templates are still direct CPU consumers.
		return;
	}

	SDL_FreeSurface(ai.image_surface);
	ai.image_surface = nullptr;
}
