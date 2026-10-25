/* [Recomps#1] Darwin only: waveOut (mmsystem.h) on an SDL3 audio stream, for nnaudio.c (notes/macos-port-plan.md step 5).
 * SDL asks for more sound when its output needs it (the stream's get-callback); the callback hands it the queued headers in
 * order and marks each one WHDR_DONE as soon as all of it was handed over, then sets the CALLBACK_EVENT event, as waveOut
 * marks a buffer done when its own pipeline has taken it. An empty queue plays silence (an underrun, as on Windows).
 * Lock order: SDL's stream lock (held around the callback) before ours; ours is never held while calling into SDL. */
#include "mmsystem.h"
#include <SDL3/SDL.h>

struct WaveOut {
    SDL_AudioStream *st;
    pthread_mutex_t m;
    WAVEHDR *head, *tail;
    size_t off;                                          /* bytes of head already handed to SDL */
    uint64_t taken;                                      /* sample frames handed over since the last reset */
    int block;                                           /* bytes per sample frame */
    HANDLE event;
};

static void SDLCALL feed(void *ud, SDL_AudioStream *st, int additional, int total) {
    (void)total;
    struct WaveOut *w = ud;
    int done = 0;
    pthread_mutex_lock(&w->m);
    while (additional > 0 && w->head) {
        WAVEHDR *h = w->head;
        size_t left = h->dwBufferLength - w->off, n = left < (size_t)additional ? left : (size_t)additional;
        n -= n % (size_t)w->block;
        if (!n && left) n = left;                        /* never split below one frame; a short tail goes whole */
        SDL_PutAudioStreamData(st, h->lpData + w->off, (int)n);
        w->off += n;
        additional -= (int)n;
        w->taken += n / (size_t)w->block;
        if (w->off >= h->dwBufferLength) {
            h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
            w->head = h->lpNext;
            if (!w->head) w->tail = NULL;
            w->off = 0;
            done = 1;
        }
    }
    pthread_mutex_unlock(&w->m);
    if (done && w->event) SetEvent(w->event);
}

MMRESULT waveOutOpen(HWAVEOUT *out, UINT dev, const WAVEFORMATEX *f, DWORD_PTR callback, DWORD_PTR inst, DWORD flags) {
    (void)dev; (void)inst;
    *out = NULL;
    if (!f || f->wFormatTag != WAVE_FORMAT_PCM || f->wBitsPerSample != 16) return MMSYSERR_ERROR;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        printf("nn::audio: SDL audio could not start: %s\n", SDL_GetError());
        return MMSYSERR_NODRIVER;
    }
    struct WaveOut *w = calloc(1, sizeof *w);
    pthread_mutex_init(&w->m, NULL);
    w->block = f->nBlockAlign ? f->nBlockAlign : f->nChannels * 2;
    w->event = (flags & CALLBACK_EVENT) ? (HANDLE)callback : NULL;
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, f->nChannels, (int)f->nSamplesPerSec};
    w->st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, w);
    if (!w->st) {
        printf("nn::audio: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        pthread_mutex_destroy(&w->m);
        free(w);
        return MMSYSERR_NODRIVER;
    }
    SDL_ResumeAudioStreamDevice(w->st);                  /* waveOutOpen leaves the device playing */
    printf("nn::audio: SDL audio driver %s, %d Hz, %d channels\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
           spec.freq, spec.channels);
    *out = w;
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutClose(HWAVEOUT w) {
    if (!w) return MMSYSERR_INVALHANDLE;
    SDL_DestroyAudioStream(w->st);                       /* no callback after this */
    pthread_mutex_destroy(&w->m);
    free(w);
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutPrepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size) { (void)w; (void)size; h->dwFlags |= WHDR_PREPARED; return MMSYSERR_NOERROR; }
MMRESULT waveOutUnprepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size) {
    (void)size;
    if (w) pthread_mutex_lock(&w->m);
    int busy = (h->dwFlags & WHDR_INQUEUE) != 0;
    if (!busy) h->dwFlags &= ~WHDR_PREPARED;
    if (w) pthread_mutex_unlock(&w->m);
    return busy ? WAVERR_STILLPLAYING : MMSYSERR_NOERROR;
}
MMRESULT waveOutWrite(HWAVEOUT w, WAVEHDR *h, UINT size) {
    (void)size;
    if (!w) return MMSYSERR_INVALHANDLE;
    if (!(h->dwFlags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
    pthread_mutex_lock(&w->m);
    h->dwFlags = (h->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
    h->lpNext = NULL;
    if (w->tail) w->tail->lpNext = h; else w->head = h;
    w->tail = h;
    pthread_mutex_unlock(&w->m);
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutPause(HWAVEOUT w) { if (!w) return MMSYSERR_INVALHANDLE; SDL_PauseAudioStreamDevice(w->st); return MMSYSERR_NOERROR; }
MMRESULT waveOutRestart(HWAVEOUT w) { if (!w) return MMSYSERR_INVALHANDLE; SDL_ResumeAudioStreamDevice(w->st); return MMSYSERR_NOERROR; }
MMRESULT waveOutReset(HWAVEOUT w) {                      /* every queued header done, position 0, what SDL holds is dropped */
    if (!w) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&w->m);
    for (WAVEHDR *h = w->head; h; h = h->lpNext) h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
    w->head = w->tail = NULL;
    w->off = 0;
    w->taken = 0;
    pthread_mutex_unlock(&w->m);
    SDL_ClearAudioStream(w->st);
    if (w->event) SetEvent(w->event);
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutGetPosition(HWAVEOUT w, MMTIME *t, UINT size) {
    (void)size;
    if (!w) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&w->m);
    uint64_t frames = w->taken;
    pthread_mutex_unlock(&w->m);
    int queued = SDL_GetAudioStreamQueued(w->st);       /* handed over but not yet played */
    if (queued > 0) frames = frames > (uint64_t)(queued / w->block) ? frames - (uint64_t)(queued / w->block) : 0;
    if (t->wType == TIME_BYTES) t->u.cb = (DWORD)(frames * (uint64_t)w->block);
    else { t->wType = TIME_SAMPLES; t->u.sample = (DWORD)frames; }
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutSetVolume(HWAVEOUT w, DWORD volume) {    /* low word left, high word right: one gain from the louder */
    if (!w) return MMSYSERR_INVALHANDLE;
    DWORD l = volume & 0xffff, r = volume >> 16;
    SDL_SetAudioStreamGain(w->st, (float)(l > r ? l : r) / 65535.0f);
    return MMSYSERR_NOERROR;
}
