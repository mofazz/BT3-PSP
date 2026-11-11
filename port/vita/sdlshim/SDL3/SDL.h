/* PS Vita: the dozen SDL3 audio functions the PC port's sound code uses (port/src/gs/snd_adx.c, snd_se.c), on the
   Vita's own audio output (port/vita/vita_audio.c). The two sound files build unchanged against this. */
#ifndef BT3_VITA_SDL_SHIM_H
#define BT3_VITA_SDL_SHIM_H
#include <stdbool.h>
#include <stdint.h>
#define SDLCALL
#define SDL_INIT_AUDIO 0x10u
#define SDL_AUDIO_S16 0x8010
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0xFFFFFFFFu
typedef uint32_t SDL_AudioDeviceID;
typedef struct SDL_AudioSpec { int format, channels, freq; } SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef void (SDLCALL *SDL_AudioStreamCallback)(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);
bool SDL_InitSubSystem(uint32_t flags);
SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID id, const SDL_AudioSpec *spec);
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID dev);
const char *SDL_GetCurrentAudioDriver(void);
const char *SDL_GetError(void);
SDL_AudioStream *SDL_CreateAudioStream(const SDL_AudioSpec *src, const SDL_AudioSpec *dst);
bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *stream, SDL_AudioStreamCallback cb, void *userdata);
bool SDL_BindAudioStream(SDL_AudioDeviceID dev, SDL_AudioStream *stream);
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len);
bool SDL_SetAudioStreamGain(SDL_AudioStream *stream, float gain);
bool SDL_LockAudioStream(SDL_AudioStream *stream);
bool SDL_UnlockAudioStream(SDL_AudioStream *stream);
bool SDL_ClearAudioStream(SDL_AudioStream *stream);
void SDL_DestroyAudioStream(SDL_AudioStream *stream);
#endif
