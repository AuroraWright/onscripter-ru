/**
 *  SDLMixerCompat.hpp
 *  ONScripter-RU
 *
 *  SDL_mixer include boundary for the SDL2-to-SDL3 migration.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include "Support/SDLCompat.hpp"

#if defined(ONS_USE_SDL3)
	#include <SDL3_mixer/SDL_mixer.h>

	#define MIX_DEFAULT_FREQUENCY 44100
	#define MIX_DEFAULT_FORMAT AUDIO_S16SYS
	#define MIX_DEFAULT_CHANNELS 2
	#define MIX_MAX_VOLUME 128

	typedef enum Mix_MusicType {
		MUS_NONE,
		MUS_CMD,
		MUS_WAV,
		MUS_MOD,
		MUS_MID,
		MUS_OGG,
		MUS_MP3,
		MUS_FLAC,
		MUS_OPUS
	} Mix_MusicType;

	struct Mix_Chunk {
		int allocated{0};
		Uint8 *abuf{nullptr};
		Uint32 alen{0};
		size_t capacityBytes{0};
		Uint8 volume{MIX_MAX_VOLUME};
		MIX_Audio *audio{nullptr};
		bool quickRaw{false};
	};
	struct Mix_Music;

	typedef void(SDLCALL *Mix_EffectFunc_t)(int chan, void *stream, int len, void *udata);
	typedef void(SDLCALL *Mix_EffectDone_t)(int chan, void *udata);

	const char *Mix_GetError();
	int Mix_OpenAudio(int frequency, SDL_AudioFormat format, int channels, int chunksize);
	int Mix_OpenAudioDevice(int frequency, SDL_AudioFormat format, int channels, int chunksize, const char *device, int allowed_changes);
	void Mix_CloseAudio();
	int Mix_QuerySpec(int *frequency, SDL_AudioFormat *format, int *channels);
	int Mix_AllocateChannels(int numchans);
	void Mix_ChannelFinished(void(SDLCALL *channel_finished)(int channel));
	int Mix_Volume(int channel, int volume);
	int Mix_VolumeMusic(int volume);
	int Mix_VolumeFloat(int channel, float volume);
	int Mix_VolumeMusicFloat(float volume);
	Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc, size_t maxDecodedBytes = 0);
	Mix_Chunk *Mix_QuickLoad_RAW(Uint8 *mem, Uint32 len);
	void Mix_FreeChunk(Mix_Chunk *chunk);
	int Mix_RegisterEffect(int chan, Mix_EffectFunc_t f, Mix_EffectDone_t d, void *arg);
	int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
	int Mix_Playing(int channel);
	void Mix_Pause(int channel);
	int Mix_Paused(int channel);
	int Mix_HaltChannel(int channel);
	Mix_Music *Mix_LoadMUS(const char *file);
	Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc);
	void Mix_FreeMusic(Mix_Music *music);
	Mix_MusicType Mix_GetMusicType(const Mix_Music *music);
	void Mix_HookMusicFinished(void(SDLCALL *music_finished)());
	int Mix_PlayMusic(Mix_Music *music, int loops);
	int Mix_PlayingMusic();
	int Mix_HaltMusic();
	int Mix_SetMusicCMD(const char *command);
	void *Mix_GetMusicHookData();
#else
	#include <SDL2/SDL_mixer.h>

	static inline int Mix_VolumeFloat(int channel, float volume) {
		return Mix_Volume(channel, volume < 0.0f ? -1 : static_cast<int>(volume + 0.5f));
	}

	static inline int Mix_VolumeMusicFloat(float volume) {
		return Mix_VolumeMusic(volume < 0.0f ? -1 : static_cast<int>(volume + 0.5f));
	}
#endif
