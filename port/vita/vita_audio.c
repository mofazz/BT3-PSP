/*
 * PS Vita: sound output. The PC port's sound code (snd_adx.c: music and voices from ADX streams; snd_se.c: the
 * game's sound effects) asks SDL3 for "audio streams" that SDL pulls data from through a callback and mixes.
 * This file is that part of SDL on the Vita's audio port: one thread that, every 1024 samples at 48 kHz, asks
 * each stream for what it needs, brings it to 48 kHz (straight-line interpolation), applies the stream's gain,
 * adds the streams up and hands the block to sceAudioOut. Interface: sdlshim/SDL3/SDL.h.
 * One lock for everything (the streams are few and a block takes well under a millisecond to mix); it can be
 * taken again by the thread that holds it, as the callers do.
 */
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#define OUT_RATE 48000
#define GRAIN 1024
#define STREAMS 24
#define FIFO 8192 /* frames (stereo) a stream can hold */

struct SDL_AudioStream {
    int used, bound, freq, channels;
    float gain;
    SDL_AudioStreamCallback cb;
    void *user;
    int16_t fifo[FIFO * 2];
    int head, count; /* frames */
    uint32_t frac;   /* position between fifo[head] and the next frame, 16.16 */
};

static struct SDL_AudioStream sStreams[STREAMS];
static SceUID sMutex = -1, sThread = -1;
static volatile int sOwner = -1, sDepth;
static int sPort = -1;
unsigned gVitaAudioBlocks, gVitaAudioStarved;
extern void Vita_Log(const char *fmt, ...);

static void lock(void) {
    int me = sceKernelGetThreadId();
    if (sOwner == me) {
        sDepth++;
        return;
    }
    sceKernelLockMutex(sMutex, 1, NULL);
    sOwner = me;
    sDepth = 1;
}

static void unlock(void) {
    if (--sDepth == 0) {
        sOwner = -1;
        sceKernelUnlockMutex(sMutex, 1);
    }
}

static int audio_thread(SceSize args, void *argp) {
    static int16_t out[GRAIN * 2];
    static int32_t mix[GRAIN * 2];
    int i, k;

    (void)args; (void)argp;
    for (;;) {
        memset(mix, 0, sizeof(mix));
        lock();
        for (k = 0; k < STREAMS; k++) {
            struct SDL_AudioStream *s = &sStreams[k];
            uint32_t step;
            int need, g;

            if (!s->used || !s->bound || s->freq <= 0) {
                continue;
            }
            step = (uint32_t)(((uint64_t)s->freq << 16) / OUT_RATE);
            need = (int)(((uint64_t)step * GRAIN + s->frac) >> 16) + 2;
            if (s->count < need && s->cb != NULL) {
                s->cb(s->user, s, (need - s->count) * 2 * s->channels, need * 2 * s->channels);
            }
            if (s->count < 2) {
                continue;
            }
            g = (int)(s->gain * 256.0f);
            for (i = 0; i < GRAIN; i++) {
                const int16_t *a, *b;
                int f = (int)(s->frac >> 8) & 0xFF;
                if (s->count < 2) {
                    gVitaAudioStarved++;
                    break;
                }
                a = &s->fifo[(s->head & (FIFO - 1)) * 2];
                b = &s->fifo[((s->head + 1) & (FIFO - 1)) * 2];
                mix[i * 2] += ((a[0] * (256 - f) + b[0] * f) >> 8) * g >> 8;
                mix[i * 2 + 1] += ((a[1] * (256 - f) + b[1] * f) >> 8) * g >> 8;
                s->frac += step;
                s->head += (int)(s->frac >> 16);
                s->count -= (int)(s->frac >> 16);
                s->frac &= 0xFFFF;
            }
            if (s->count < 0) {
                s->count = 0;
            }
        }
        unlock();
        for (i = 0; i < GRAIN * 2; i++) {
            int32_t v = mix[i];
            out[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        gVitaAudioBlocks++;
        sceAudioOutOutput(sPort, out); /* returns when the block has been taken: this is the thread's clock */
    }
    return 0;
}

bool SDL_InitSubSystem(uint32_t flags) { (void)flags; return true; }
const char *SDL_GetCurrentAudioDriver(void) { return "PS Vita audio port, 48 kHz"; }
const char *SDL_GetError(void) { return "PS Vita audio"; }
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID dev) { (void)dev; return true; }

SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID id, const SDL_AudioSpec *spec) {
    (void)id; (void)spec;
    if (sThread >= 0) {
        return 1;
    }
    sMutex = sceKernelCreateMutex("bt3_audio", 0, 0, NULL);
    sPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (sMutex < 0 || sPort < 0) {
        Vita_Log("vita: sound: no audio port (mutex %08x, port %08x)\n", (unsigned)sMutex, (unsigned)sPort);
        return 0;
    }
    {   /* on the fourth core when the system lets it (vita_main.c), else the first */
        extern SceUID Vita_ThreadOnCore3(const char *name, SceKernelThreadEntry entry, int prio, int stack, int fallbackMask);
        (void)Vita_ThreadOnCore3; /* (back on the first core; on the fourth it stopped with the system's shell) */
        sThread = sceKernelCreateThread("bt3_audio", audio_thread, 100, 0x40000, 0, 0x00010000, NULL);
    }
    /* above the game's thread (160): a late block is heard.
        On the first core, the game's: the drawing thread's core is the one that is short of time now (test
        135: the game's thread takes 14-24 ms of its 33, the sound about 1 ms a picture) */
    { extern void Vita_WatchThread(int slot, int id, const char *name); Vita_WatchThread(3, sThread, "sound"); }
    if (sThread < 0 || sceKernelStartThread(sThread, 0, NULL) < 0) {
        Vita_Log("vita: sound: no audio thread (%08x)\n", (unsigned)sThread);
        sThread = -1;
        return 0;
    }
    Vita_Log("vita: sound: audio port open, 48 kHz stereo\n");
    return 1;
}

SDL_AudioStream *SDL_CreateAudioStream(const SDL_AudioSpec *src, const SDL_AudioSpec *dst) {
    int k;

    (void)dst;
    if (sMutex < 0 || src == NULL || src->channels < 1 || src->channels > 2) {
        return NULL;
    }
    lock();
    for (k = 0; k < STREAMS && sStreams[k].used; k++) {
    }
    if (k < STREAMS) {
        memset(&sStreams[k], 0, sizeof(sStreams[k]));
        sStreams[k].freq = src->freq;
        sStreams[k].channels = src->channels;
        sStreams[k].gain = 1.0f;
        sStreams[k].used = 1;
    }
    unlock();
    return k < STREAMS ? &sStreams[k] : NULL;
}

bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *s, SDL_AudioStreamCallback cb, void *userdata) {
    lock();
    s->user = userdata;
    s->cb = cb;
    unlock();
    return true;
}

bool SDL_BindAudioStream(SDL_AudioDeviceID dev, SDL_AudioStream *s) {
    (void)dev;
    s->bound = 1;
    return true;
}

/* (from a stream's callback, or under SDL_LockAudioStream) */
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *buf, int len) {
    const int16_t *p = buf;
    int frames = len / (2 * s->channels), i;

    for (i = 0; i < frames && s->count < FIFO; i++) {
        int16_t *o = &s->fifo[((s->head + s->count) & (FIFO - 1)) * 2];
        if (s->channels == 2) {
            o[0] = p[i * 2];
            o[1] = p[i * 2 + 1];
        } else {
            o[0] = o[1] = p[i];
        }
        s->count++;
    }
    return true;
}

bool SDL_SetAudioStreamGain(SDL_AudioStream *s, float gain) { s->gain = gain; return true; }
bool SDL_LockAudioStream(SDL_AudioStream *s) { (void)s; lock(); return true; }
bool SDL_UnlockAudioStream(SDL_AudioStream *s) { (void)s; unlock(); return true; }

bool SDL_ClearAudioStream(SDL_AudioStream *s) {
    lock();
    s->count = 0;
    s->frac = 0;
    unlock();
    return true;
}

void SDL_DestroyAudioStream(SDL_AudioStream *s) {
    lock();
    s->used = 0;
    s->bound = 0;
    s->cb = NULL;
    unlock();
}
