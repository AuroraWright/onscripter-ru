/**
 *  Image.cpp
 *  ONScripter-RU
 *
 *  Code for image loading and processing.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#include "Engine/Core/ONScripter.hpp"
#include "Resources/Support/Resources.hpp"
#include "Engine/Components/Async.hpp"
#include "Engine/Components/Window.hpp"
#include "Engine/Readers/Base.hpp"
#include "Engine/Graphics/PNG.hpp"
#include "Engine/Graphics/Pool.hpp"

#include "Support/SDLCompat.hpp"

#include <new>
#include <string>
#include <map>
#include <cstdio>
#include <cstring>

void ONScripter::loadImageIntoCache(int id, const std::string &filename_str, bool allow_rgb, bool speculative) {
	size_t maxDecodedBytes = 0;
	MemoryBudget::Lease staging;
	{
		Lock lock(&imageCache);
		if (auto existing = imageCache.get(filename_str)) {
			imageCache.add(id, filename_str, existing);
			return;
		}
		if (speculative) {
			maxDecodedBytes = memoryBudget().prefetchAllowance(
			    std::min(imageCache.availablePrefetchBytes(id), ImageAssets::MaxDecodedBytes), 4);
			if (!maxDecodedBytes)
				return;
		}
	}
	bool has_alpha{false};
	SDL_Surface *surface = loadImage(filename_str.c_str(), &has_alpha, allow_rgb, maxDecodedBytes, speculative ? &staging : nullptr);
	{
		Lock lock(&imageCache);
		auto ptr = imageCache.get(filename_str);
		if (ptr) {
			imageCache.add(id, filename_str, ptr);
			SDL_FreeSurface(surface);
			return;
		}
		auto wrapped = std::make_shared<Wrapped_SDL_Surface>(surface, has_alpha);
		if (staging) {
			staging.shrinkTo(wrapped->memoryBytes());
			staging.reclassify(MemoryBudget::Kind::Images);
			wrapped->retention = std::move(staging);
		}
		imageCache.add(id, filename_str, wrapped);
	}
}

void ONScripter::dropCache(int *id, const std::string &filename_str) {
	imageAssets.remove(filename_str);
	// Pass nullptr to drop string from all caches
	{
		Lock lock(&imageCache);
		if (!id) {
			imageCache.removeAll(filename_str);
		} else {
			imageCache.remove(*id, filename_str);
		}
	}
}

RenderImage *ONScripter::loadGpuImage(const char *file_name, bool allow_rgb) {
	//This function is unable to handle archives

	if (!file_name) {
		sendToLog(LogLevel::Error, "loadGpuImage: Incorrect file_name was passed!\n");
		return nullptr;
	}

	SDL_Surface *input_surface = loadImage(file_name, nullptr, allow_rgb);

	if (!input_surface) {
		sendToLog(LogLevel::Error, "loadGpuImage: File %s cannot be opened!\n", file_name);
		return nullptr;
	}

	RenderImage *img = gpu.copyImageFromSurface(input_surface, true);

	if (!img) {
		sendToLog(LogLevel::Error, "loadGpuImage: File %s cannot be opened!\n", file_name);
		SDL_FreeSurface(input_surface);
		return nullptr;
	}

	GPU_DiscardImagePixels(img);
	SDL_FreeSurface(input_surface);

	return img;
}

SDL_Surface *ONScripter::loadImage(const char *filename, bool *has_alpha, bool allow_rgb, size_t maxDecodedBytes, MemoryBudget::Lease *staging) {

	//sendToLog(LogLevel::Info, "loadImage (%s)\n", filename);

	if (!filename)
		return nullptr;

	{
		Lock lock(&imageCache);
		std::shared_ptr<Wrapped_SDL_Surface> r = imageCache.get(filename);
		if (r && r->surface) {
			if (maxDecodedBytes && static_cast<uint64_t>(r->surface->w) * r->surface->h > maxDecodedBytes / 4)
				return nullptr;
			if (staging) {
				*staging = memoryBudget().reserve(MemoryBudget::Kind::Prefetch,
				    static_cast<size_t>(r->surface->w) * r->surface->h * 4 * 3);
				if (!*staging)
					return nullptr;
			}
			if (filelog_flag && !maxDecodedBytes) {
				Lock logLock(&script_h.log_info[ScriptHandler::FILE_LOG]);
				script_h.findAndAddLog(script_h.log_info[ScriptHandler::FILE_LOG], filename, true);
			}
			if (has_alpha)
				*has_alpha = r->has_alpha;
			if (!allow_rgb && onsSurfaceBitsPerPixel(r->surface) == 24) {
				SDL_Surface *ret = onsConvertSurfaceFormat(r->surface, pixel_format_enum_32bpp, SDL_SWSURFACE);
				// Allow the 24-bit r->surface to be freed by the wrapped surface destruction
				return ret;
			}
			// Alpha/mask preparation writes pixels. Each consumer needs its own
			// surface so different tags and concurrent jobs cannot corrupt cache data.
			return onsConvertSurfaceFormat(r->surface, onsSurfacePixelFormatEnum(r->surface), SDL_SWSURFACE);
		}
	}

	SDL_Surface *tmp = nullptr;

	if (filename[0] == '>' && maxDecodedBytes)
		return nullptr; // Generated images are cheap to create on demand.
	if (filename[0] == '>')
		tmp = createRectangleSurface(filename);
	else if (filename[0] != '*') // layers begin with *
		tmp = createSurfaceFromFile(filename, maxDecodedBytes, staging);
	if (tmp == nullptr) {
		//sendToLog(LogLevel::Info, "returning from loadImage [2]\n");
		return nullptr;
	}

	bool has_colorkey = false;
	uint32_t colorkey = 0;

	if (has_alpha) {
		*has_alpha = (onsSurfaceAmask(tmp) != 0);
		if (!(*has_alpha) && onsGetColorKey(tmp, &colorkey)) {
			has_colorkey = true;

			if (onsSurfacePalette(tmp)) {
				//palette will be converted to RGBA, so don't do colorkey check
				has_colorkey = false;
			}
			*has_alpha = true;
		}
	}

	uint32_t format  = onsSurfacePixelFormatEnum(tmp);
	SDL_Surface *ret = tmp;

	bool conversionRequired = !(
	    // no conversion to 32-bit is required if:
	    (format == pixel_format_enum_32bpp) ||             // the image is already in 32-bit format;
	    (allow_rgb && (format == pixel_format_enum_24bpp)) // or the image is already in 24-bit format and we're allowing images without alpha
	);

	if (conversionRequired) {
		ret = onsConvertSurfaceFormat(tmp, pixel_format_enum_32bpp, SDL_SWSURFACE);
		SDL_FreeSurface(tmp);
	}
	if (!ret)
		return nullptr;

	//  A PNG image may contain an alpha channel, which complicates
	// handling loaded images when the ":a" alphablend tag is used,
	// since the standard method was to assume the right half of the image
	// contains an alpha data mask for the left half.
	//  The current default behavior is to use the PNG image's alpha
	// channel if available, and only process for an old-style mask
	// when no alpha channel was provided.
	// However, this could cause problems running older NScr games
	// which have PNG images containing old-style masks but also an
	// opaque alpha channel.
	//  Therefore, we provide a hack, set with the --detect-png-nscmask
	// command-line option, to auto-detect if a PNG image is likely to
	// have an old-style mask.  We assume that an old-style mask is intended
	// if the image either has no alpha channel, or the alpha channel it has
	// is completely opaque.  (Note that this used to be the default
	// behavior for onscripter-en.)
	//  Note that using the --force-png-nscmask option will always assume
	// old-style masks, while --force-png-alpha will produce the current
	// default behavior.
	if ((png_mask_type != PNG_MASK_USE_ALPHA) &&
	    has_alpha && *has_alpha) {
		if (png_mask_type == PNG_MASK_USE_NSCRIPTER)
			*has_alpha = false;
		else if (png_mask_type == PNG_MASK_AUTODETECT) {
			const uint32_t amask = onsSurfaceAmask(ret);
			const uint32_t aval  = *static_cast<uint32_t *>(ret->pixels) & amask;
			if (aval == amask) {
				*has_alpha = false;
				for (int y = 0; y < ret->h; ++y) {
					uint32_t *pixbuf = reinterpret_cast<uint32_t *>(static_cast<char *>(ret->pixels) + y * ret->pitch);
					for (int x = ret->w; x > 0; --x, ++pixbuf) {
						// Resolving ambiguity per Tatu's patch, 20081118.
						// I note that this technically changes the meaning of the
						// code, since != is higher-precedence than &, but this
						// version is obviously what I intended when I wrote this.
						// Has this been broken all along?  :/  -- Haeleth
						if ((*pixbuf & amask) != aval) {
							*has_alpha = true;
							return ret;
						}
					}
				}
			}

			if (!*has_alpha && has_colorkey) {
				// has a colorkey, so run a match against rgb values
				const uint32_t amask = onsSurfaceAmask(ret);
				const uint32_t aval  = colorkey & ~amask;
				if (aval == (*static_cast<uint32_t *>(ret->pixels) & ~amask))
					return ret;
				*has_alpha = false;
				for (int y = 0; y < ret->h; ++y) {
					uint32_t *pixbuf = reinterpret_cast<uint32_t *>(static_cast<char *>(ret->pixels) + y * ret->pitch);
					for (int x = ret->w; x > 0; --x, ++pixbuf) {
						if ((*pixbuf & ~amask) == aval) {
							*has_alpha = true;
							return ret;
						}
					}
				}
			}
		}
	}

	//sendToLog(LogLevel::Info, "returning from loadImage [3]\n");

	//printClock("loadImage ends");
	return ret;
}

SDL_Surface *ONScripter::createRectangleSurface(const char *filename) {
	int c = 1, w = 0, h = 0;
	while (filename[c] != 0x0a && filename[c] != 0x00) {
		if (filename[c] >= '0' && filename[c] <= '9')
			w = w * 10 + filename[c] - '0';
		if (filename[c] == ',') {
			c++;
			break;
		}
		c++;
	}

	while (filename[c] != 0x0a && filename[c] != 0x00) {
		if (filename[c] >= '0' && filename[c] <= '9')
			h = h * 10 + filename[c] - '0';
		if (filename[c] == ',') {
			c++;
			break;
		}
		c++;
	}

	while (filename[c] == ' ' || filename[c] == '\t') c++;
	int n = 0, c2 = c;
	while (filename[c] == '#') {
		uchar3 col;
		readColor(&col, filename + c);
		n++;
		c += 7;
		while (filename[c] == ' ' || filename[c] == '\t') c++;
	}

	SDL_Surface *tmp = onsCreateRGBSurface(SDL_SWSURFACE, w, h,
	                                        32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);

	c = c2;
	for (int i = 0; i < n; i++) {
		uchar3 col;
		readColor(&col, filename + c);
		c += 7;
		while (filename[c] == ' ' || filename[c] == '\t') c++;

		SDL_Rect rect;
		rect.x = w * i / n;
		rect.y = 0;
		rect.w = w * (i + 1) / n - rect.x;
		if (i == n - 1)
			rect.w = w - rect.x;
		rect.h = h;
		SDL_FillRect(tmp, &rect, onsMapRGBA(tmp, col.x, col.y, col.z, 0xff));
	}

	return tmp;
}

SDL_Surface *ONScripter::createSurfaceFromFile(const char *filename, size_t maxDecodedBytes, MemoryBudget::Lease *staging) {

	static int surfaceCreationLockVar = 0;

	size_t length{0};
	uint8_t *buffer{nullptr};
	MemoryBudget::Lease compressed;

	if (filename[0]) {
		Lock lock(&surfaceCreationLockVar);
		if (maxDecodedBytes) {
			script_h.reader->getFile(filename, length, nullptr);
			if (length > maxDecodedBytes)
				return nullptr;
			if (staging) {
				compressed = memoryBudget().reserve(MemoryBudget::Kind::Prefetch, length + 1);
				if (!compressed)
					return nullptr;
			}
		}
		script_h.reader->getFile(filename, length, &buffer);
	}

	if (length == 0) {
		if (maxDecodedBytes)
			return nullptr;
		//don't complain about missing cursors
		if (!equalstr(filename, "uoncur.bmp") &&
		    !equalstr(filename, "uoffcur.bmp") &&
		    !equalstr(filename, "doncur.bmp") &&
		    !equalstr(filename, "doffcur.bmp") &&
		    !equalstr(filename, "cursor0.bmp") &&
		    !equalstr(filename, "cursor1.bmp")) {
			std::snprintf(script_h.errbuf, MAX_ERRBUF_LEN,
			              "can't find file [%s]", filename);
			errorAndCont(script_h.errbuf, nullptr, "I/O Issue");
		}
		return nullptr;
	}

	// Speculation is deliberately limited to PNGs with a bounded decoded size.
	// Check before calling a decoder, not after it has allocated a huge surface.
	if (maxDecodedBytes) {
		constexpr uint8_t signature[]{137, 80, 78, 71, 13, 10, 26, 10};
		auto read32 = [&](size_t offset) {
			return (uint32_t(buffer[offset]) << 24) | (uint32_t(buffer[offset + 1]) << 16) |
			       (uint32_t(buffer[offset + 2]) << 8) | buffer[offset + 3];
		};
		if (length < 24 || std::memcmp(buffer, signature, sizeof(signature)) != 0 ||
		    std::memcmp(buffer + 12, "IHDR", 4) != 0 ||
		    uint64_t(read32(16)) * read32(20) > maxDecodedBytes / 4) {
			freearr(&buffer);
			return nullptr;
		}
		if (staging) {
			// Charge the header's actual dimensions before decoding, including
			// room for format/alpha conversion. Reserving the whole allowance for
			// each small icon would evict the cache while the worker was busy.
			*staging = memoryBudget().reserve(MemoryBudget::Kind::Prefetch,
			    static_cast<size_t>(read32(16)) * read32(20) * 4 * 3);
			if (!*staging) {
				freearr(&buffer);
				return nullptr;
			}
		}
	}

	if (filelog_flag && !maxDecodedBytes) {
		Lock lock(&script_h.log_info[ScriptHandler::FILE_LOG]);
		script_h.findAndAddLog(script_h.log_info[ScriptHandler::FILE_LOG], filename, true);
	}

	const char *ext  = std::strrchr(filename, '.');
	SDL_RWops *src   = SDL_RWFromMem(buffer, static_cast<int>(length));
	SDL_Surface *tmp = nullptr;

	if (ext && (equalstr(ext + 1, "PNG") || equalstr(ext + 1, "png"))) {
		PNGLoader *loader = pngImageLoaderPool.getLoader();
		tmp               = loader->loadPng(src);
		if (!tmp)
			sendToLog(LogLevel::Error, "Failed to use internal PNGLoader on %s\n", filename);
		pngImageLoaderPool.giveLoader(loader);
	}

	if (!tmp) {
		Lock lock(&surfaceCreationLockVar);
		tmp = IMG_Load_RW(src, 0);
		if (!tmp && ext && (equalstr(ext + 1, "JPG") || equalstr(ext + 1, "jpg"))) {
			sendToLog(LogLevel::Warn, " *** force-loading a JPG image [%s]\n", filename);
			tmp = IMG_LoadJPG_RW(src);
		}
		if (!tmp)
			sendToLog(LogLevel::Error, " *** can't load file [%s] with purported length %llu bytes: %s ***\n",
			          filename, static_cast<unsigned long long>(length), IMG_GetError());
	}

	SDL_RWclose(src);

	freearr(&buffer);

	return tmp;
}

void ONScripter::effectBlendToCombinedImage(RenderImage *mask_gpu, int trans_mode, uint32_t mask_value, RenderImage *image) {
	if (image == pre_screen_gpu)
		pre_screen_render = true;

	int refresh_mode_src = effect_refresh_mode_src;
	int refresh_mode_dst = effect_refresh_mode_dst;
	if (refresh_mode_src == -1)
		refresh_mode_src = refreshMode() | REFRESH_BEFORESCENE_MODE;
	if (refresh_mode_dst == -1)
		refresh_mode_dst = refreshMode();

	bool srcb4         = refresh_mode_src & REFRESH_BEFORESCENE_MODE;
	bool dstb4         = refresh_mode_dst & REFRESH_BEFORESCENE_MODE;
	auto &src_dr_scene = (srcb4 ? before_dirty_rect_scene : dirty_rect_scene);
	auto &src_dr_hud   = (srcb4 ? before_dirty_rect_hud : dirty_rect_hud);
	auto &dst_dr_scene = (dstb4 ? before_dirty_rect_scene : dirty_rect_scene);
	auto &dst_dr_hud   = (dstb4 ? before_dirty_rect_hud : dirty_rect_hud);

	if (camera.has_moved || !src_dr_scene.isEmpty() || !src_dr_hud.isEmpty())
		mergeForEffect(combined_effect_src_gpu, &src_dr_scene.bounding_box_script, &src_dr_hud.bounding_box_script, refresh_mode_src | CONSTANT_REFRESH_MODE);
	if (camera.has_moved || !dst_dr_scene.isEmpty() || !dst_dr_hud.isEmpty())
		mergeForEffect(combined_effect_dst_gpu, &dst_dr_scene.bounding_box_script, &dst_dr_hud.bounding_box_script, refresh_mode_dst | CONSTANT_REFRESH_MODE);
	// v   note: we pass nullptr to effectblendgpu -- the whole src and dst are blitted onto prescreen

	/*sendToLog(LogLevel::Info, "src dr scene %f x %f ; hud %f x %f ; dst dr scene %f x %f ; hud %f x %f ; rfmodesrc %d ; dst %d\n",
		src_dr_scene.bounding_box_script.w,
		src_dr_scene.bounding_box_script.h,
		src_dr_hud.bounding_box_script.w,
		src_dr_hud.bounding_box_script.h,
		dst_dr_scene.bounding_box_script.w,
		dst_dr_scene.bounding_box_script.h,
		dst_dr_hud.bounding_box_script.w,
		dst_dr_hud.bounding_box_script.h,
		refresh_mode_src,
		refresh_mode_dst
	);*/

	effectBlendGPU(mask_gpu, trans_mode, mask_value, nullptr, combined_effect_src_gpu, combined_effect_dst_gpu, image);
}

void ONScripter::effectBlendGPU(RenderImage *mask_gpu, int trans_mode,
                                uint32_t mask_value, RenderRect *clip,
                                RenderImage *src1, RenderImage *src2, RenderImage *dst) {
	if (!src1 || !src2 || !dst) {
		sendToLog(LogLevel::Error, "Invalid effectBlendGPU arguments\n");
		return;
	}

	RenderRect fullclip{0, 0, static_cast<float>(window.script_width), static_cast<float>(window.script_height)};
	if (!clip)
		clip = &fullclip;

	//sendToLog(LogLevel::Info, "effectBlendGPU clip %d %d %d %d\n", clip->x, clip->y, clip->w, clip->h);

	if (trans_mode == ALPHA_BLEND_CONST) {
		gpu.setShaderProgram("blendByMask.frag");
		gpu.bindImageToSlot(src1, 0);
		gpu.bindImageToSlot(src2, 1);
		gpu.bindImageToSlot(src2, 2); // hack for now, to avoid declared but unused param. need to make shader work without supplying it
		gpu.setShaderVar("mask_value", static_cast<int>(mask_value) * 2);
		gpu.setShaderVar("constant_mask", 1);
		gpu.setShaderVar("crossfade", 1);
		gpu.copyGPUImage(src1, clip, clip, dst->target, clip->x, clip->y);
		gpu.unsetShaderProgram();
	} else if ((trans_mode == ALPHA_BLEND_FADE_MASK ||
	            trans_mode == ALPHA_BLEND_CROSSFADE_MASK) &&
	           mask_gpu) {
		gpu.setShaderProgram("blendByMask.frag");
		gpu.bindImageToSlot(src2, 1);
		gpu.bindImageToSlot(mask_gpu, 2);
		gpu.setShaderVar("constant_mask", 0);
		gpu.setShaderVar("mask_value", static_cast<int>(mask_value));
		gpu.setShaderVar("crossfade", trans_mode == ALPHA_BLEND_CROSSFADE_MASK);
		gpu.copyGPUImage(src1, clip, clip, dst->target, clip->x, clip->y);
		gpu.unsetShaderProgram();
	} else {
		gpu.clearWholeTarget(dst->target, 255, 0, 0, 255);
	}
}

bool ONScripter::colorGlyph(const GlyphParams *key, GlyphValues *glyph, SDL_Color *color, bool border, GlyphAtlasController *atlas) {
	// 1. atlas -> atlas (src_atlas && atlas) -> via temp image
	// 2. image -> image (!src_atlas && !atlas) -> overwrite self
	// 3. image -> atlas (!src_atlas && atlas) -> coords & colour
	// 4. atlas -> image (src_atlas && !atlas) -> overwrite image

	// is atlas a place we are blitting from
	bool src_atlas     = ((!border && glyph->glyph_pos.has()) || (border && glyph->border_pos.has()));
	RenderImage *src_img = src_atlas ? glyphAtlas.atlas : (border ? glyph->border_gpu : glyph->glyph_gpu);
	RenderRect *src_rect = !src_atlas ? nullptr : (border ? &glyph->border_pos.get() : &glyph->glyph_pos.get());
	std::unique_ptr<RenderRect> dst_rect;

	if (src_img == nullptr || color == nullptr) {
		return true;
	}

	GPU_GetTarget(src_img);
	RenderTarget *target = src_img->target; // case 2

	float x = src_img->w / 2.0, y = src_img->h / 2.0; // cases 2 & 4
	RenderImage *tmp{nullptr};
	bool atlas_ok{true};

	if (src_atlas && atlas) { // case 1
		tmp = gpu.createImage(src_rect->w, src_rect->h, 4);
		GPU_GetTarget(tmp);
		target   = tmp->target;
		dst_rect = std::make_unique<RenderRect>();
		if (atlas->add(tmp->w + 2, tmp->h + 2, *dst_rect)) {
			x = dst_rect->x + tmp->w / 2.0;
			y = dst_rect->y + tmp->h / 2.0;
		} else {
			sendToLog(LogLevel::Error, "ONScripter@colorGlyph: Texture atlas addition failed (case #1)!\n");
			atlas_ok = false;
		}
	} else if (!src_rect && atlas) { // case 3
		dst_rect = std::make_unique<RenderRect>();
		if (atlas->add(src_img->w + 2, src_img->h + 2, *dst_rect)) {
			x = dst_rect->x + dst_rect->w / 2.0;
			y = dst_rect->y + dst_rect->h / 2.0;
		} else {
			sendToLog(LogLevel::Error, "ONScripter@colorGlyph: Texture atlas addition failed (case #3)!\n");
			atlas_ok = false;
		}
	} else if (src_rect && !atlas) { // case 4
		RenderImage *image = (border ? glyph->border_gpu : glyph->glyph_gpu);
		GPU_GetTarget(image);
		target = image->target;
	}

	if (!atlas_ok) {
		if (tmp)
			gpu.freeImage(tmp);
		return false;
	}

	bool src_needs_copy   = src_img->target == target && !gpu.render_to_self;
	RenderImage *actual_src = src_img;

	if (key->is_gradient && !border) {
		if (src_needs_copy) {
			actual_src = gpu.copyImage(actual_src);
			GPU_GetTarget(actual_src);
		}

		// Add a gradient instead!
		gpu.setShaderProgram("glyphGradient.frag");
		gpu.bindImageToSlot(actual_src, 0);
		gpu.setShaderVar("color", *color);
		// Make sure we call the right overloaded version of setShaderVar(!!)
		gpu.setShaderVar("faceAscender", static_cast<int>(glyph->faceAscender));
		gpu.setShaderVar("maxy", static_cast<int>(glyph->maxy + (src_rect ? (src_rect->y) : 0)));
		gpu.setShaderVar("height", static_cast<int>(/*src_rect ? src_rect->h :*/ src_img->h));
		GPU_SetBlending(actual_src, false);
		if (tmp)
			gpu.copyGPUImage(actual_src, src_rect, nullptr, target);
		else
			gpu.copyGPUImage(actual_src, src_rect, dst_rect.get(), target, x, y, 1, 1, 0, true);

		gpu.unsetShaderProgram();
		GPU_SetBlending(actual_src, true);
		if (atlas && target == atlas->atlas->target)
			gpu.simulateRead(atlas->atlas);
	} else {
		//Don't take alpha into account
		if (color->r == 0 && color->b == 0 && color->g == 0) {
			if (!tmp) {
				gpu.multiplyAlpha(src_img, dst_rect.get());
				return true;
			}
			throw std::runtime_error("Temporary glyph texture should not have been allocated for a black glyph");
		}

		if (src_needs_copy) {
			actual_src = gpu.copyImage(actual_src);
			GPU_GetTarget(actual_src);
		}

		SDL_Color srcColor{0, 0, 0, 0};

		gpu.setShaderProgram("colorModification.frag");

		gpu.bindImageToSlot(actual_src, 0);
		GPU_SetBlending(actual_src, false);
		gpu.setShaderVar("modificationType", 7);
		gpu.setShaderVar("replaceSrcColor", srcColor);
		gpu.setShaderVar("replaceDstColor", *color);
		gpu.setShaderVar("multiplyAlpha", 1);
		if (tmp)
			gpu.copyGPUImage(actual_src, src_rect, nullptr, target);
		else
			gpu.copyGPUImage(actual_src, src_rect, dst_rect.get(), target, x, y, 1, 1, 0, true);

		gpu.unsetShaderProgram();
		GPU_SetBlending(actual_src, true);
		if (atlas && target == atlas->atlas->target)
			gpu.simulateRead(atlas->atlas);
	}

	if (tmp) {
		gpu.copyGPUImage(tmp, nullptr, dst_rect.get(), atlas->atlas->target, x, y, 1, 1, 0, true);
		gpu.freeImage(tmp);
		gpu.simulateRead(atlas->atlas);
	}

	if (dst_rect.get())
		(border ? glyph->border_pos : glyph->glyph_pos).set(*dst_rect);

	if (src_needs_copy)
		gpu.freeImage(actual_src);

	return true;
}

void ONScripter::makeNegaTarget(RenderTarget *target, RenderRect clip) {
	if (target == nullptr || target->image == nullptr) {
		sendToLog(LogLevel::Error, "makeNegaTarget@Target has no image\n");
		return;
	}

	gpu.setShaderProgram("colorModification.frag");
	gpu.bindImageToSlot(target->image, 0);

	gpu.setShaderVar("modificationType", 5);

	//Switch to canvas coordinate system
	clip.x += camera.center_pos.x;
	clip.y += camera.center_pos.y;

	gpu.copyGPUImage(target->image, nullptr, &clip, target);
	gpu.unsetShaderProgram();
}

void ONScripter::makeMonochromeTarget(RenderTarget *target, RenderRect clip, bool before_scene) {
	if (target == nullptr || target->image == nullptr) {
		sendToLog(LogLevel::Error, "makeMonochromeTarget@Target has no image\n");
		return;
	}

	gpu.setShaderProgram("colorModification.frag");
	gpu.bindImageToSlot(target->image, 0);

	gpu.setShaderVar("modificationType", 4);
	gpu.setShaderVar("greyscaleHue", monocro_color[before_scene]);

	//Switch to canvas coordinate system
	clip.x += camera.center_pos.x;
	clip.y += camera.center_pos.y;

	gpu.copyGPUImage(target->image, nullptr, &clip, target);
	gpu.unsetShaderProgram();
}

void ONScripter::makeBlurTarget(RenderTarget *target, RenderRect clip, bool before_scene) {
	if (target == nullptr || target->image == nullptr) {
		sendToLog(LogLevel::Error, "makeBlurTarget@Target has no image\n");
		return;
	}

	//Switch to canvas coordinate system
	clip.x += camera.center_pos.x;
	clip.y += camera.center_pos.y;

	GPUTransformableCanvasImage tmp(target->image);
	PooledGPUImage toDraw = gpu.getBlurredImage(tmp, blur_mode[before_scene]);
	gpu.copyGPUImage(toDraw.image, nullptr, &clip, target);
}

void ONScripter::makeWarpedTarget(RenderTarget *target, RenderRect clip, bool /*before_scene*/) {
	if (target == nullptr || target->image == nullptr) {
		sendToLog(LogLevel::Error, "makeWarpedTarget@Target has no image\n");
		return;
	}

	//Switch to canvas coordinate system
	clip.x += camera.center_pos.x;
	clip.y += camera.center_pos.y;

	GPUTransformableCanvasImage tmp(target->image);
	float secs            = warpClock.time() / 1000.0;
	PooledGPUImage toDraw = gpu.getWarpedImage(tmp, secs, warpAmplitude, warpWaveLength, warpSpeed);
	gpu.copyGPUImage(toDraw.image, nullptr, &clip, target);
}

//Adds to correct dirty rect by z level
void ONScripter::dirtyRectForZLevel(int num, RenderRect &rect) {
	AnimationInfo *sprite = &sprite_info[num];
	const int zOrder = sprite->has_z_order_override ? sprite->z_order_override : num;
	if (zOrder > z_order_hud && !isRetainedSceneLayer(sprite))
		markRetainedRainSceneStaticDirty("zlevel");
	DirtyRect *dirty = (num <= z_order_hud) ? &before_dirty_rect_hud : &before_dirty_rect_scene;
	dirty->add(rect);
	dirty = (num <= z_order_hud) ? &dirty_rect_hud : &dirty_rect_scene;
	dirty->add(rect);
}

void ONScripter::dirtySpriteRect(AnimationInfo *ai, bool before) {
	dirtySpriteRect(ai->id, ai->type == SPRITE_LSP2, before);
}

void ONScripter::dirtySpriteRect(int num, bool lsp2, bool before) {
	if (num == -1)
		return;

	DirtyRect *dirty   = (num <= z_order_hud) ? (before ? &before_dirty_rect_hud : &dirty_rect_hud) : (before ? &before_dirty_rect_scene : &dirty_rect_scene);
	AnimationInfo *spr = lsp2 ? &sprite2_info[num] : &sprite_info[num];
	spr                = (before && spr->old_ai) ? spr->old_ai : spr;
	const int zOrder = spr->has_z_order_override ? spr->z_order_override : num;
	if (zOrder > z_order_hud && !isRetainedSceneLayer(spr))
		markRetainedRainSceneStaticDirty("sprite");
	RenderRect toAdd{0, 0, 0, 0};

	if (spr->parentImage.no != -1) {
		bool parentIsLsp2        = spr->parentImage.lsp2;
		AnimationInfo *parentSpr = parentIsLsp2 ? (before ? (sprite2_info[spr->parentImage.no].old_ai ? sprite2_info[spr->parentImage.no].old_ai : &sprite2_info[spr->parentImage.no]) : &sprite2_info[spr->parentImage.no]) :
		                                          (before ? (sprite_info[spr->parentImage.no].old_ai ? sprite_info[spr->parentImage.no].old_ai : &sprite_info[spr->parentImage.no]) : &sprite_info[spr->parentImage.no]);
		toAdd = parentIsLsp2 ? parentSpr->bounding_rect : parentSpr->pos;
		toAdd.x += parentSpr->camera.pos.x;
		toAdd.y += parentSpr->camera.pos.y;
		if (parentSpr->scrollable.h > 0) {
			toAdd.h = parentSpr->scrollable.h;
		}
		if (parentSpr->scrollable.w > 0) {
			toAdd.w = parentSpr->scrollable.w;
		}
	} else {
		toAdd = lsp2 ? spr->bounding_rect : spr->pos;
		toAdd.x += spr->camera.pos.x;
		toAdd.y += spr->camera.pos.y;
		if (spr->scrollable.h > 0) {
			toAdd.h = spr->scrollable.h;
		}
		if (spr->scrollable.w > 0) {
			toAdd.w = spr->scrollable.w;
		}
	}

	if (spr->spriteTransforms.breakupFactor > 0 || spr->spriteTransforms.blurFactor > 0 || std::fabs(spr->spriteTransforms.warpAmplitude) > 0) {
		dirty->fill(window.canvas_width, window.canvas_height);
	}

	dirty->add(toAdd);

	if (num > z_order_hud) {
		SpritesetInfo *cleanSet{nullptr};
		// Sets 1+
		auto firstSpriteset = z_order_spritesets.find(1);
		if (firstSpriteset != z_order_spritesets.end() && num <= firstSpriteset->second) {
			//Belongs to a spriteset, we need to tell that spriteset about this
			int spriteset = 1;
			auto nextSpriteset = z_order_spritesets.find(spriteset + 1);
			while (nextSpriteset != z_order_spritesets.end() && num <= nextSpriteset->second) {
				spriteset++;
				nextSpriteset = z_order_spritesets.find(spriteset + 1);
			}
			cleanSet = &spritesets[spriteset];
		}
		// Set 0
		else if (num < z_order_ld) {
			cleanSet = &spritesets[0];
		}

		if (cleanSet) {
			//TODO: If the spriteset isNullTransform, or has pos and nothing else that might change the dirty rect like blur,
			// then simply adjust the dirty rect we add above instead of updating the whole screen
			if (before)
				before_dirty_rect_scene.fill(window.canvas_width, window.canvas_height);
			else
				dirty_rect_scene.fill(window.canvas_width, window.canvas_height);
			cleanSpritesetCache(cleanSet, before);
		}
	}

	if (!before && !spr->old_ai) {
		dirtySpriteRect(num, lsp2, true); // This sprite is on both the beforescene and afterscene -- call ourselves again to update the beforescene rects
	}
}

int ONScripter::getAIno(AnimationInfo *info, bool &lsp2) {
	if (info == nullptr)
		return -1;

	lsp2 = false;
	if (info->type == SPRITE_LSP2)
		lsp2 = true;
	else if (info->type == SPRITE_LSP)
		lsp2 = false;
	else
		return -1;

	return info->id;
}

void ONScripter::fillCanvas(bool after, bool before) {
	if (after || before)
		markRetainedRainSceneStaticDirty("canvas");
	if (after) {
		dirty_rect_scene.fill(window.canvas_width, window.canvas_height);
		dirty_rect_hud.fill(window.canvas_width, window.canvas_height);
	}
	if (before) {
		before_dirty_rect_scene.fill(window.canvas_width, window.canvas_height);
		before_dirty_rect_hud.fill(window.canvas_width, window.canvas_height);
	}
}

void ONScripter::resetSpritesets() {
	for (auto &id_set : spritesets) {
		cleanSpritesetCache(&id_set.second, true);
		cleanSpritesetCache(&id_set.second, false);
	}
	spritesets.clear();
}

void ONScripter::cleanSpritesetCache(SpritesetInfo *spriteset, bool before) {
	if (!spriteset)
		return;
	auto &ssim = before ? spriteset->im : spriteset->imAfterscene;
	if (!ssim.image)
		return;
	gpu.giveCanvasImage(ssim.image);
	ssim.clearImage();
}

void ONScripter::setupZLevels(int refresh_mode) {
	for (int zOrder : spriteZLevelIndices)
		spriteZLevels[zOrder].clear();
	spriteZLevelIndices.clear();

	auto insertSprite = [&](AnimationInfo &ai) {
		auto spr = ai.oldNew(refresh_mode);
		if (!spr->exists)
			return;
		if (!spr->visible)
			return;
		if (spr->parentImage.no != -1)
			return;
		if (spr->type == SPRITE_LSP && all_sprite_hide_flag)
			return;
		if (spr->type == SPRITE_LSP2 && all_sprite2_hide_flag)
			return;

		const int zOrder = spr->has_z_order_override ? spr->z_order_override : spr->id;
		if (zOrder < 0 || zOrder >= MAX_SPRITE_NUM)
			return;

		auto &level = spriteZLevels[zOrder];
		if (level.empty())
			spriteZLevelIndices.push_back(zOrder);
		level.push_back(spr);
	};
	for (int i = 0; i < MAX_SPRITE_NUM; i++) insertSprite(sprite_info[i]);
	for (int i = 0; i < MAX_SPRITE_NUM; i++) insertSprite(sprite2_info[i]);

	for (int zOrder : spriteZLevelIndices) {
		auto &level = spriteZLevels[zOrder];
		if (level.size() > 1)
			std::sort(level.begin(), level.end(), cmpById{});
	}
	std::sort(spriteZLevelIndices.begin(), spriteZLevelIndices.end(), [](int a, int b) {
		return a > b;
	});
}

// Helper function for refreshSceneTo & refreshHudTo.
void ONScripter::drawSpritesBetween(int upper_inclusive, int lower_exclusive, RenderTarget *target, RenderRect *clip_dst, int refresh_mode) {
	auto zIt = std::lower_bound(spriteZLevelIndices.begin(), spriteZLevelIndices.end(), upper_inclusive,
	                            [](int zOrder, int upper) { return zOrder > upper; });
	for (; zIt != spriteZLevelIndices.end(); ++zIt) {
		int zOrder = *zIt;
		if (zOrder <= lower_exclusive)
			break;
		if (refresh_mode & REFRESH_SAYA_MODE && zOrder <= 9)
			return;

		auto &level = spriteZLevels[zOrder];

		for (AnimationInfo *spr : level) {
			// Will iterate through sprites in correct z order using cmpById
			// Draw it!
			drawToGPUTarget(target, spr, refresh_mode, clip_dst, spr->type == SPRITE_LSP2);
		}
	}
}

void ONScripter::drawSceneForeground(RenderTarget *target, RenderRect &scriptClip, int refreshMode) {
	for (int i = 0; i < 3; i++) {
		AnimationInfo *tc = tachi_info[human_order[2 - i]].oldNew(refreshMode);
		if (tc->exists)
			drawToGPUTarget(target, tc, refreshMode, &scriptClip);
	}
	drawSceneSpritesets(target, scriptClip, refreshMode, 0);
}

void ONScripter::drawSceneSpritesets(RenderTarget *target, RenderRect &scriptClip, int refreshMode, int firstSpritesetNo) {
	if (firstSpritesetNo > 0 && !z_order_spritesets.contains(firstSpritesetNo))
		return;

	for (int spritesetNo = firstSpritesetNo;; spritesetNo++) {
		int startZ = spritesetNo == 0 ? z_order_ld : z_order_spritesets[spritesetNo];
		auto nextSpriteset = z_order_spritesets.find(spritesetNo + 1);
		int endZ = nextSpriteset != z_order_spritesets.end() ? nextSpriteset->second : z_order_hud;
		if (spritesetNo == 0 || spritesets[spritesetNo].isEnabled(refreshMode & REFRESH_BEFORESCENE_MODE)) {
			if (spritesets[spritesetNo].isNullTransform()) {
				if (spritesetNo != 0)
					gpu.clearWholeTarget(target, 0, 0, 0, 255);
				drawSpritesBetween(startZ, endZ, target, &scriptClip, refreshMode);
			} else {
				auto &ssim = refreshMode & REFRESH_BEFORESCENE_MODE ? spritesets[spritesetNo].im : spritesets[spritesetNo].imAfterscene;
				if (!ssim.image) {
					RenderImage *spritesetImage = gpu.getCanvasImage();
					if (spritesetNo != 0)
						gpu.clearWholeTarget(spritesetImage->target, 0, 0, 0, 255);
					RenderRect fullRect = full_script_clip;
					drawSpritesBetween(startZ, endZ, spritesetImage->target, &fullRect, refreshMode);
					ssim = GPUTransformableCanvasImage(spritesetImage);
				}
				drawSpritesetToGPUTarget(target, &spritesets[spritesetNo], &scriptClip, refreshMode);
			}
		}
		if (endZ == z_order_hud)
			break;
	}
}

bool ONScripter::retainedRainCompositingEnabled() const {
	const char *value = onsSDLGetEnv("ONS_RETAINED_RAIN_COMPOSITING");
	return !value || !*value || std::strcmp(value, "0") != 0;
}

bool ONScripter::isRetainedSceneLayer(const AnimationInfo *sprite) {
	if (!sprite || sprite->trans_mode != AnimationInfo::TRANS_LAYER || sprite->layer_no < 0)
		return false;
	auto *layer = getLayer<Layer>(sprite->layer_no, false);
	return layer && layer->supportsRetainedSceneCompositing();
}

void ONScripter::markRetainedRainSceneStaticDirty(const char *source) {
	auto &cache = retainedRainSceneCache;
	if (!cache.staticSceneDirty) {
		++cache.invalidations;
		if (std::strcmp(source, "sprite") == 0)
			++cache.spriteInvalidations;
		else if (std::strcmp(source, "zlevel") == 0)
			++cache.zLevelInvalidations;
		else if (std::strcmp(source, "canvas") == 0)
			++cache.canvasInvalidations;
		else
			++cache.explicitInvalidations;
	}
	cache.staticSceneDirty = true;
	cache.valid            = false;
}

void ONScripter::clearRetainedRainSceneCache() {
	auto &cache = retainedRainSceneCache;
	for (RenderImage *band : cache.bands) {
		if (band)
			gpu.freeImage(band);
	}
	cache.bands.clear();
	cache.layers.clear();
	cache.width            = 0;
	cache.height           = 0;
	cache.centerX          = 0;
	cache.centerY          = 0;
	cache.valid            = false;
	cache.staticSceneDirty = true;
}

bool ONScripter::tryRetainedRainScene(RenderTarget *target, RenderRect &scriptClip, int refreshMode) {
	auto &cache = retainedRainSceneCache;
	++cache.attempts;

	auto ineligible = [&](uint64_t &reason) {
		++cache.ineligibleFallbacks;
		++reason;
		cache.valid            = false;
		cache.staticSceneDirty = true;
		return false;
	};

	if (!retainedRainCompositingEnabled()) {
		if (!cache.bands.empty())
			clearRetainedRainSceneCache();
		return ineligible(cache.disabledFallbacks);
	}

	if (!target || !target->image || target->w != window.canvas_width || target->h != window.canvas_height ||
	    effect_current || pre_screen_render || spriteRangeMotion.active || !queueAnimationInfo.empty() ||
	    (refreshMode & REFRESH_SAYA_MODE)) {
		return ineligible(cache.targetStateFallbacks);
	}

	AnimationInfo *background = bg_info.oldNew(refreshMode);
	if (!background || !background->exists || bg_info.old_ai || background->trans < 255 ||
	    background->blending_mode != BlendModeId::NORMAL ||
	    background->pos.x > 0 || background->pos.y > 0 ||
	    background->pos.x + background->pos.w < window.script_width ||
	    background->pos.y + background->pos.h < window.script_height) {
		return ineligible(cache.backgroundFallbacks);
	}

	for (AnimationInfo &tachi : tachi_info) {
		if (tachi.old_ai || (tachi.exists && tachi.visible &&
		                     (tachi.is_animatable || tachi.camera.isMoving() || tachi.spriteTransforms.warpAmplitude != 0)))
			return ineligible(cache.tachiFallbacks);
	}
	for (auto &spriteset : spritesets) {
		if (spriteset.second.isUncommitted())
			return ineligible(cache.spritesetFallbacks);
	}

	std::array<RetainedRainLayer, 4> layers{};
	size_t layerCount = 0;
	for (int zOrder : spriteZLevelIndices) {
		if (zOrder <= z_order_hud)
			break;
		auto &level = spriteZLevels[zOrder];
		AnimationInfo *retainedLayer = nullptr;
		for (AnimationInfo *sprite : level) {
			if (!isRetainedSceneLayer(sprite))
				continue;
			if (retainedLayer) {
				if (cache.layerLayoutFallbacks == 0)
					sendToLog(LogLevel::Info, "Retained rain layout rejection: multiple retained layers at z=%d\n", zOrder);
				return ineligible(cache.layerLayoutFallbacks);
			}
			retainedLayer = sprite;
		}
		if (retainedLayer) {
			if (layerCount == layers.size()) {
				if (cache.layerLayoutFallbacks == 0)
					sendToLog(LogLevel::Info, "Retained rain layout rejection: more than four layers\n");
				return ineligible(cache.layerLayoutFallbacks);
			}
			layers[layerCount++] = {retainedLayer, zOrder, retainedLayer->type == SPRITE_LSP2};
		}
	}

	if (layerCount == 0) {
		if (!cache.bands.empty())
			clearRetainedRainSceneCache();
		return ineligible(cache.noRainFallbacks);
	}

	const int firstRainZ = layers[0].zOrder;
	const bool rainBeforeTachi = firstRainZ > z_order_ld;
	const auto spritesetOne = z_order_spritesets.find(1);
	const int spritesetZeroEnd = spritesetOne != z_order_spritesets.end() ? spritesetOne->second : z_order_hud;
	for (size_t i = 0; i < layerCount; ++i) {
		const RetainedRainLayer &layer = layers[i];
		const bool inSameRegion = rainBeforeTachi ? layer.zOrder > z_order_ld :
		                                               layer.zOrder <= z_order_ld && layer.zOrder > spritesetZeroEnd;
		if (!inSameRegion) {
			if (cache.layerLayoutFallbacks == 0)
				sendToLog(LogLevel::Info,
				          "Retained rain layout rejection: split region first=%d rejected=%d humanz=%d spriteset0_end=%d\n",
				          firstRainZ, layer.zOrder, z_order_ld, spritesetZeroEnd);
			return ineligible(cache.layerLayoutFallbacks);
		}
	}
	if (rainBeforeTachi) {
		for (AnimationInfo &tachi : tachi_info) {
			if (tachi.exists && tachi.visible && tachi.blending_mode != BlendModeId::NORMAL)
				return ineligible(cache.unsafeBlendFallbacks);
		}
	}

	for (int zOrder : spriteZLevelIndices) {
		if (zOrder <= z_order_hud)
			break;
		bool afterFirstRain = zOrder < firstRainZ;
		for (AnimationInfo *sprite : spriteZLevels[zOrder]) {
			if (isRetainedSceneLayer(sprite)) {
				const bool captured = std::any_of(layers.begin(), layers.begin() + layerCount,
				                                  [sprite](const RetainedRainLayer &layer) { return layer.sprite == sprite; });
				if (!captured) {
					if (cache.layerLayoutFallbacks == 0)
						sendToLog(LogLevel::Info, "Retained rain layout rejection: uncaptured retained layer z=%d id=%d\n", zOrder, sprite->id);
					return ineligible(cache.layerLayoutFallbacks);
				}
				if (sprite == layers[0].sprite)
					afterFirstRain = true;
				continue;
			}
			if (sprite->trans_mode == AnimationInfo::TRANS_LAYER)
				return ineligible(cache.unsupportedLayerFallbacks);
			if (afterFirstRain && sprite->blending_mode != BlendModeId::NORMAL)
				return ineligible(cache.unsafeBlendFallbacks);
		}
	}

	const bool sameLayers = cache.layers.size() == layerCount &&
	                        std::equal(layers.begin(), layers.begin() + layerCount, cache.layers.begin());
	const bool layoutChanged = cache.width != target->w || cache.height != target->h ||
	                           cache.centerX != camera.center_pos.x || cache.centerY != camera.center_pos.y || !sameLayers;
	if (layoutChanged) {
		if (cache.valid)
			++cache.invalidations;
		cache.valid   = false;
		cache.layers.assign(layers.begin(), layers.begin() + layerCount);
		cache.centerX = camera.center_pos.x;
		cache.centerY = camera.center_pos.y;
	}

	if (cache.staticSceneDirty) {
		cache.valid            = false;
		cache.staticSceneDirty = false;
		++cache.staticFallbacks;
		return false;
	}

	const size_t requiredBands = layerCount + 1;
	constexpr size_t maxRetainedBytes = 96ULL * 1024 * 1024;
	const size_t retainedBytes = requiredBands * static_cast<size_t>(target->w) * static_cast<size_t>(target->h) * 4;
	if (retainedBytes > maxRetainedBytes)
		return ineligible(cache.memoryFallbacks);

	if (cache.bands.size() != requiredBands || cache.width != target->w || cache.height != target->h) {
		for (RenderImage *band : cache.bands) {
			if (band)
				gpu.freeImage(band);
		}
		cache.bands.clear();
		cache.bands.reserve(requiredBands);
		for (size_t i = 0; i < requiredBands; ++i) {
			RenderImage *band = gpu.createImage(static_cast<uint16_t>(target->w), static_cast<uint16_t>(target->h), 4);
			GPU_GetTarget(band);
			GPU_SetBlending(band, true);
			GPU_SetRGBA(band, 255, 255, 255, 255);
			cache.bands.push_back(band);
		}
		cache.width  = target->w;
		cache.height = target->h;
		cache.centerX = camera.center_pos.x;
		cache.centerY = camera.center_pos.y;
		cache.valid  = false;
	}

	if (!cache.valid) {
		RenderRect fullClip = full_script_clip;
		auto drawLevelSiblings = [&](RenderTarget *bandTarget, const RetainedRainLayer &layer, bool beforeRetained) {
			bool retainedSeen = false;
			for (AnimationInfo *sprite : spriteZLevels[layer.zOrder]) {
				if (sprite == layer.sprite) {
					retainedSeen = true;
					continue;
				}
				if (retainedSeen != beforeRetained)
					drawToGPUTarget(bandTarget, sprite, refreshMode, &fullClip, sprite->type == SPRITE_LSP2);
			}
		};

		gpu.clearWholeTarget(cache.bands[0]->target, 0, 0, 0, 255);
		drawToGPUTarget(cache.bands[0]->target, background, refreshMode, &fullClip);
		if (rainBeforeTachi) {
			drawSpritesBetween(MAX_SPRITE_NUM - 1, layers[0].zOrder, cache.bands[0]->target, &fullClip, refreshMode);
		} else {
			drawSpritesBetween(MAX_SPRITE_NUM - 1, z_order_ld, cache.bands[0]->target, &fullClip, refreshMode);
			for (int i = 0; i < 3; i++) {
				AnimationInfo *tc = tachi_info[human_order[2 - i]].oldNew(refreshMode);
				if (tc->exists)
					drawToGPUTarget(cache.bands[0]->target, tc, refreshMode, &fullClip);
			}
			drawSpritesBetween(z_order_ld, layers[0].zOrder, cache.bands[0]->target, &fullClip, refreshMode);
		}
		drawLevelSiblings(cache.bands[0]->target, layers[0], true);

		for (size_t i = 0; i < layerCount; ++i) {
			RenderImage *band = cache.bands[i + 1];
			gpu.clearWholeTarget(band->target, 0, 0, 0, 0);
			drawLevelSiblings(band->target, layers[i], false);
			const int lower = i + 1 < layerCount ? layers[i + 1].zOrder :
			                                                   rainBeforeTachi ? z_order_ld : spritesetZeroEnd;
			drawSpritesBetween(layers[i].zOrder - 1, lower, band->target, &fullClip, refreshMode);
			if (i + 1 < layerCount)
				drawLevelSiblings(band->target, layers[i + 1], true);
			if (i + 1 == layerCount) {
				if (rainBeforeTachi)
					drawSceneForeground(band->target, fullClip, refreshMode);
				else
					drawSceneSpritesets(band->target, fullClip, refreshMode, 1);
			}
		}
		GPU_FlushBlitBuffer();
		cache.valid = true;
		++cache.cacheBuilds;
	} else {
		++cache.cacheHits;
	}

	RenderRect canvasClip = scriptClip;
	canvasClip.x += camera.center_pos.x;
	canvasClip.y += camera.center_pos.y;

	GPU_TelemetryScope cacheScope("retained_rain_composite");
	const GPU_bool baseBlending = cache.bands[0]->use_blending;
	GPU_SetBlending(cache.bands[0], false);
	gpu.copyGPUImage(cache.bands[0], nullptr, &canvasClip, target);
	GPU_SetBlending(cache.bands[0], baseBlending);
	++cache.bandBlits;

	for (size_t i = 0; i < layerCount; ++i) {
		drawToGPUTarget(target, layers[i].sprite, refreshMode, &scriptClip, layers[i].lsp2);
		gpu.copyGPUImage(cache.bands[i + 1], nullptr, &canvasClip, target);
		++cache.rainDraws;
		++cache.bandBlits;
	}

	++cache.composedFrames;
	return true;
}

void ONScripter::printRetainedRainSceneTelemetry() const {
	const char *value = onsSDLGetEnv("ONS_SDL3_GPU_TELEMETRY");
	if ((!value || !*value || std::strcmp(value, "0") == 0) || retainedRainSceneCache.telemetryPrinted || retainedRainSceneCache.attempts == 0)
		return;

	const auto &cache = retainedRainSceneCache;
	cache.telemetryPrinted = true;
	sendToLog(LogLevel::Info,
	          "Retained rain telemetry: attempts=%llu composed_frames=%llu cache_hits=%llu cache_builds=%llu "
	          "static_fallbacks=%llu ineligible_fallbacks=%llu invalidations=%llu band_blits=%llu rain_draws=%llu "
	          "bands=%llu layers=%llu valid=%d disabled=%llu target_state=%llu background=%llu tachi=%llu "
	          "spriteset=%llu layer_layout=%llu no_rain=%llu unsupported_layer=%llu unsafe_blend=%llu memory=%llu "
	          "invalidation_sprite=%llu invalidation_zlevel=%llu invalidation_canvas=%llu invalidation_explicit=%llu\n",
	          static_cast<unsigned long long>(cache.attempts),
	          static_cast<unsigned long long>(cache.composedFrames),
	          static_cast<unsigned long long>(cache.cacheHits),
	          static_cast<unsigned long long>(cache.cacheBuilds),
	          static_cast<unsigned long long>(cache.staticFallbacks),
	          static_cast<unsigned long long>(cache.ineligibleFallbacks),
	          static_cast<unsigned long long>(cache.invalidations),
	          static_cast<unsigned long long>(cache.bandBlits),
	          static_cast<unsigned long long>(cache.rainDraws),
	          static_cast<unsigned long long>(cache.bands.size()),
	          static_cast<unsigned long long>(cache.layers.size()), cache.valid ? 1 : 0,
	          static_cast<unsigned long long>(cache.disabledFallbacks),
	          static_cast<unsigned long long>(cache.targetStateFallbacks),
	          static_cast<unsigned long long>(cache.backgroundFallbacks),
	          static_cast<unsigned long long>(cache.tachiFallbacks),
	          static_cast<unsigned long long>(cache.spritesetFallbacks),
	          static_cast<unsigned long long>(cache.layerLayoutFallbacks),
	          static_cast<unsigned long long>(cache.noRainFallbacks),
	          static_cast<unsigned long long>(cache.unsupportedLayerFallbacks),
	          static_cast<unsigned long long>(cache.unsafeBlendFallbacks),
	          static_cast<unsigned long long>(cache.memoryFallbacks),
	          static_cast<unsigned long long>(cache.spriteInvalidations),
	          static_cast<unsigned long long>(cache.zLevelInvalidations),
	          static_cast<unsigned long long>(cache.canvasInvalidations),
	          static_cast<unsigned long long>(cache.explicitInvalidations));
}

// This function rebuilds the game screen and blits it to the target.
void ONScripter::refreshSceneTo(RenderTarget *target, RenderRect *passed_script_clip_dst, int refresh_mode) {

	int &rm = refresh_mode; // We'll be passing this around a lot, let's make it short

	if (!(rm & CONSTANT_REFRESH_MODE)) {
		rm &= ~REFRESH_BEFORESCENE_MODE;
		constant_refresh_mode |= rm;
		return;
	}

	if (target == nullptr) {
		sendToLog(LogLevel::Error, "refreshSceneTo: Null target was passed\n");
		return;
	}

	if (!(rm & REFRESH_SOMETHING))
		return;

	/* Some basic variable sanity checks, which should probably be done somewhere other than here, like after define. */
	if (!(MAX_SPRITE_NUM > z_order_ld &&
	      z_order_ld > z_order_hud &&
	      z_order_hud > z_order_window &&
	      z_order_window > z_order_text &&
	      z_order_window > 0)) {
		errorAndExit("z_orders are somehow wrong. Make sure max > humanz > spriteset(1) > spriteset(2) > ... > hudz > windowz > 0.");
	}

	setupZLevels(rm);

	RenderRect script_clip_dst = full_script_clip;
	if (passed_script_clip_dst)
		if (doClipping(&script_clip_dst, passed_script_clip_dst))
			return;

	if (!tryRetainedRainScene(target, script_clip_dst, rm)) {
		AnimationInfo *bg = bg_info.oldNew(rm);
		if (bg->exists)
			drawToGPUTarget(target, bg, rm, &script_clip_dst);

		drawSpritesBetween(MAX_SPRITE_NUM - 1, z_order_ld, target, &script_clip_dst, rm);
		drawSceneForeground(target, script_clip_dst, rm);
	}

	//Apply nega in the end of normal rebuild
	bool before = rm & REFRESH_BEFORESCENE_MODE;

	if (nega_mode[before] == 1)
		makeNegaTarget(target, script_clip_dst);
	if (monocro_flag[before])
		makeMonochromeTarget(target, script_clip_dst, before);
	if (nega_mode[before] == 2)
		makeNegaTarget(target, script_clip_dst);
	if (blur_mode[before] > 0)
		makeBlurTarget(target, script_clip_dst, before);
	if (std::fabs(warpAmplitude) > 0)
		makeWarpedTarget(target, script_clip_dst, before);

	//sendToLog(LogLevel::Info, "enddraw\n");
}

void ONScripter::refreshHudTo(RenderTarget *target, RenderRect *passed_script_clip_dst, int refresh_mode) {

	/* a) make sure textwindow renders properly and according to its position. 
Including leaveTextMode and enterTextMode (all the sprites that should be 
above textwindow are indeed above textwindow while it transitions or text renders)

b) text is always rendered on the top of any sprites excluding buttons, 
this can lead to a possible glitch, but dammit, who is the mad man to 
use buttons & text this way */

	int &rm = refresh_mode; // We'll be passing this around a lot, let's make it short

	if (!(rm & CONSTANT_REFRESH_MODE)) {
		rm &= ~REFRESH_BEFORESCENE_MODE;
		constant_refresh_mode |= rm;
		return;
	}

	if (target == nullptr) {
		sendToLog(LogLevel::Error, "refreshHudTo: Null target was passed\n");
		return;
	}

	if (target->w != window.canvas_width || target->h != window.canvas_height) {
		sendToLog(LogLevel::Error, "refreshHudTo: not canvas dst\n");
		return;
	}

	if (!(rm & REFRESH_SOMETHING))
		return;

	if (display_draw) {
		gpu.copyGPUImage(draw_screen_gpu, nullptr, nullptr, target, (target->w - draw_screen_gpu->w) / 2.0, (target->h - draw_screen_gpu->h) / 2.0);
		return;
	}

	RenderRect script_clip_dst = full_script_clip;
	if (passed_script_clip_dst)
		if (doClipping(&script_clip_dst, passed_script_clip_dst))
			return;

	RenderRect canvas_clip_dst = script_clip_dst;
	canvas_clip_dst.x += camera.center_pos.x;
	canvas_clip_dst.y += camera.center_pos.y;

	//hud has no background, so we have to set a clip rect and clear it before we can draw onto it.
	GPU_SetClipRect(target, canvas_clip_dst);
	gpu.clear(target);
	GPU_UnsetClip(target);

	//canvas_clip_dst is used for text only which doesn't occupy the whole canvas
	RenderRect middle_of_canvas{camera.center_pos.x, camera.center_pos.y, static_cast<float>(window.script_width), static_cast<float>(window.script_height)};
	doClipping(&canvas_clip_dst, &middle_of_canvas);

	drawSpritesBetween(z_order_hud, z_order_window, target, &script_clip_dst, rm);

	if (refresh_mode & REFRESH_WINDOW_MODE) {
		if (wndCtrl.usingDynamicTextWindow) {
			if (!dlgCtrl.dialogueProcessingState.active) {
				gpu.copyGPUImage(window_gpu, nullptr, &canvas_clip_dst, target, camera.center_pos.x, camera.center_pos.y);
			} else {
				wndCtrl.updateTextboxExtension(true);
				renderDynamicTextWindow(target, &canvas_clip_dst, rm);
			}
		} else {
			AnimationInfo *si = sentence_font_info.oldNew(rm);
			if (si->exists)
				drawToGPUTarget(target, si, rm, &script_clip_dst);
		}
	}

	drawSpritesBetween(z_order_window, z_order_text, target, &script_clip_dst, rm);

	AnimationInfo *spr;
	if (!(refresh_mode & REFRESH_SAYA_MODE)) {
		for (auto &i : bar_info)
			if (i && (spr = i->oldNew(rm)))
				drawToGPUTarget(target, spr, rm, &script_clip_dst);
		for (auto &i : prnum_info)
			if (i && (spr = i->oldNew(rm)))
				drawToGPUTarget(target, spr, rm, &script_clip_dst);
	}

	if (refresh_mode & REFRESH_TEXT_MODE)
		dlgCtrl.renderDialogueToTarget(target, &canvas_clip_dst, rm);

	if (refresh_mode & REFRESH_CURSOR_MODE && !textgosub_label && !enable_custom_cursors) {
		if (clickstr_state == CLICK_WAIT)
			drawToGPUTarget(target, cursor_info[CURSOR_WAIT_NO].oldNew(rm), rm, &script_clip_dst);
		else if (clickstr_state == CLICK_NEWPAGE)
			drawToGPUTarget(target, cursor_info[CURSOR_NEWPAGE_NO].oldNew(rm), rm, &script_clip_dst);
	}

	drawSpritesBetween(z_order_text, -1, target, &script_clip_dst, rm);

	ButtonLink *p_button_link = root_button_link.next;
	while (p_button_link) {
		ButtonLink *cur_button_link = p_button_link;
		while (cur_button_link) {
			if (cur_button_link->show_flag) {
				drawToGPUTarget(target, cur_button_link->anim->oldNew(rm), rm, &script_clip_dst);
			}
			cur_button_link = cur_button_link->same;
		}
		p_button_link = p_button_link->next;
	}
}

void ONScripter::refreshSprite(int sprite_no, bool active_flag,
                               int cell_no, RenderRect *check_src_rect,
                               RenderRect *check_dst_rect) {
	if ((sprite_info[sprite_no].image_name ||
	     ((sprite_info[sprite_no].trans_mode == AnimationInfo::TRANS_STRING) &&
	      sprite_info[sprite_no].file_name)) &&
	    ((sprite_info[sprite_no].visible != active_flag) ||
	     ((cell_no >= 0) && (sprite_info[sprite_no].current_cell != cell_no)) ||
	     (doClipping(check_src_rect, &sprite_info[sprite_no].pos) == 0) ||
	     (doClipping(check_dst_rect, &sprite_info[sprite_no].pos) == 0))) {
		if (cell_no >= 0)
			sprite_info[sprite_no].setCell(cell_no);

		sprite_info[sprite_no].visible = active_flag;

		dirtySpriteRect(sprite_no, false);
	}
}

void ONScripter::createBackground() {
	bg_info.type = SPRITE_BG;
	// Default bg should have 1 cell, black colour, and COPY
	// bg_info.trans_mode = AnimationInfo::TRANS_COPY; // set by constructor / previous bg
	bg_info.num_of_cells = 1;
	bg_info.color        = {};
	bg_info.deleteImage();

	if (equalstr(bg_info.file_name, "white")) {
		bg_info.color = {0xff, 0xff, 0xff};
	} else if (bg_info.file_name[0] == '#') {
		readColor(&bg_info.color, bg_info.file_name);
	} else if (!equalstr(bg_info.file_name, "black") != 0) {
		script_h.setStr(&bg_info.image_name, bg_info.file_name);
		parseTaggedString(&bg_info);
		// Enforce cell number and trans_mode after parsing
		bg_info.trans_mode   = AnimationInfo::TRANS_COPY;
		bg_info.num_of_cells = 1;
		setupAnimationInfo(&bg_info);

		if (bg_info.image_surface) {
			SDL_FreeSurface(bg_info.image_surface);
			bg_info.image_surface = nullptr;
		}

		if (bg_info.gpu_image) {
			bg_info.pos.x = static_cast<float>((window.script_width - bg_info.gpu_image->w) / 2);
			bg_info.pos.y = static_cast<float>((window.script_height - bg_info.gpu_image->h) / 2);
			bg_info.pos.w = bg_info.gpu_image->w;
			bg_info.pos.h = bg_info.gpu_image->h;
		}
	}

	if (!bg_info.gpu_image) {
		// Wrapping will stretch it automatically
		bg_info.gpu_image = gpu.createImage(1, 1, 3);
		GPU_GetTarget(bg_info.gpu_image);
		gpu.clearWholeTarget(bg_info.gpu_image->target, bg_info.color.x, bg_info.color.y, bg_info.color.z, 0xff);
		bg_info.pos = full_script_clip;
	}

	bg_info.exists = true;
}

void ONScripter::loadBreakupCellforms() {
	const InternalResource *breakup_cellforms_res = getResource("breakup-cellforms.png");
	if (breakup_cellforms_res) {
		SDL_RWops *rwcells               = SDL_RWFromConstMem(breakup_cellforms_res->buffer,
                                                static_cast<int>(breakup_cellforms_res->size));
		SDL_Surface *breakup_cellforms_s = IMG_Load_RW(rwcells, 0);
		breakup_cellforms_gpu            = gpu.copyImageFromSurface(breakup_cellforms_s);
		SDL_FreeSurface(breakup_cellforms_s);
	} else {
		sendToLog(LogLevel::Error, "breakup-cellforms.png not loaded resource compilation broken\n");
	}
}

void ONScripter::loadDrawImages() {
	if (!draw_gpu) {
		draw_gpu = gpu.createImage(window.script_width, window.script_height, 4);
		GPU_GetTarget(draw_gpu);
	}
	if (!draw_screen_gpu) {
		draw_screen_gpu = gpu.createImage(window.script_width, window.script_height, 4);
		GPU_GetTarget(draw_screen_gpu);
	}
}

void ONScripter::unloadDrawImages() {
	display_draw = false;

	if (draw_gpu) {
		gpu.freeImage(draw_gpu);
		draw_gpu = nullptr;
	}

	if (draw_screen_gpu) {
		gpu.freeImage(draw_screen_gpu);
		draw_screen_gpu = nullptr;
	}
}

void ONScripter::clearDrawImages(int r, int g, int b, bool clear_screen) {
	loadDrawImages();
	gpu.clearWholeTarget(draw_gpu->target, r, g, b, 0xff);
	if (clear_screen) {
		gpu.clearWholeTarget(draw_screen_gpu->target, r, g, b, 0xff);
		display_draw = true;
	}
}
