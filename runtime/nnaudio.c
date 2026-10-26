/* nn::audio (AudioOut: raw PCM output) on Windows waveOut, and nn::codec::HardwareOpusDecoder.
 *
 * What the game does (read off its code, addresses are guest addresses):
 *   0x710373ec20  InitializeAudioOutParameter(&p); p.sampleRate (u32 at +0) = 48000; p.channelCount (u16 at +4) = n;
 *                 OpenDefaultAudioOut(&audioOut 0x7106dd48a8, &systemEvent 0x7106dd4860, p) -> Result (0 = ok);
 *                 then GetAudioOutChannelCount / SampleRate / SampleFormat. The format's value is ignored: the
 *                 mixer always writes 16-bit signed samples (0x710398b0fc: float * 32767 -> strh).
 *                 It allocates N buffers (N at 0x7106dd489c, clamped to 2..16) of 512 frames
 *                 (512 * channels * 2 bytes, capacity rounded up to 4 KB) and for each calls
 *                 SetAudioOutBufferInfo(buf, data, capacity, dataSize) and AppendAudioOutBuffer.
 *                 The AudioOutBuffer array has a stride of 0x28 bytes.
 *   0x7103987890  thread "nus3::audio update": StartAudioOut, then forever:
 *                 b = GetReleasedAudioOutBuffer(); if null: TimedWaitSystemEvent(0x7106dd4860, 32 ms) and retry;
 *                 else mix 512 frames into GetAudioOutBufferDataPointer(b) and AppendAudioOutBuffer(b).
 *                 The event is waited on at the SystemEvent's own address, so SystemEventType is its first member.
 *                 On shutdown: StopAudioOut, CloseAudioOut, GetAudioOutBufferDataPointer of every buffer to free it.
 *   0x710373f8b0  AcquireAudioDeviceNotificationForOutput(&event); a thread "AudioDeviceChange" waits on it and
 *                 calls GetActiveAudioDeviceName(char name[0x100]) (used with strcpy/strlen: a C string).
 *   0x71022bca24  SetAudioDeviceOutputVolume(name, volume): "AudioTvOutput" 1.0, "AudioStereoJackOutput" 1.4,
 *                 "AudioBuiltInSpeakerOutput" 2.0, "AudioUsbDeviceOutput" 1.0.
 *
 * AudioOutBuffer is only touched through these functions, so the field order inside its 0x28 bytes is ours
 * (the same as in the public SDK headers): +0 next, +8 data, +0x10 capacity, +0x18 data size, +0x20 data offset.
 *
 * Host side: each AudioOut the game opens is its own output (the game has two open during a movie: Wwise's and
 * CRI's). Every appended buffer is copied (6 channels are mixed down to stereo) and handed to that output's waveOut at once.
 * A buffer is "released" when the device has finished playing its copy, in the order it was appended; the
 * guest's event is signalled then. Without an output device the buffers are released by the clock instead.
 *
 * Environment: SSBU_WAV=<file>          write the first 30 s of what is played to a WAV file
 *              SSBU_AUDIO_CHANNELS=6    grant 6 channels when the game asks for them (default: stereo, the game
 *                                       then does its own mix-down, as on a console set to stereo output)
 *              SSBU_AUDIO_LOG=1         print the statistics every second instead of every 10 s
 *              SSBU_AUDIO_MUTE=1        no host device (clock-driven)
 *              SSBU_AUDIO_TONE=1        add a quiet 440 Hz test tone to the first second (checks the host path)
 *              SSBU_OPUS_DLL=<file>     libopus DLL for the Opus decoder (default libopus-0.dll, then MSYS2's)
 *
 * HardwareOpusDecoder: the game only imports the constructor; everything else goes through the object's
 * virtual table (0x71039937b8 and following): +0x00 destructor, +0x10 GetWorkBufferSize(sampleRate, channels),
 * +0x20 Initialize(sampleRate, channels, buffer, size) -> 0 = ok, +0x28 Finalize,
 * +0x48 DecodeInterleaved(size_t *consumed, int *sampleCount, int16_t *out, size_t outBytes, in, inBytes) -> 0 = ok.
 * Each input packet starts with an 8-byte header (big-endian u32 payload length, u32 final range) as in the
 * public descriptions of the console's Opus service. Decoding is done by libopus, loaded at run time; without
 * it the decoder produces silence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define A6 (c->x[6])
#define RET(v) (c->x[0] = (uint64_t)(v))

void os_init_event(uint64_t key, int autoclear);     /* os_threads.c */
void os_signal_event(uint64_t key);

/* ---------------------------------------------------------------- AudioOut */
#define BUF_NEXT 0x00
#define BUF_DATA 0x08
#define BUF_CAPACITY 0x10
#define BUF_SIZE 0x18
#define BUF_OFFSET 0x20
#define SAMPLE_FORMAT_PCM16 2                         /* nn::audio::SampleFormat_PcmInt16 */
#define SLOTS 64                                      /* more than the game's maximum of 16 buffers */
#define WAV_SECONDS 30
/* [Recomps#1] The game opens more than one AudioOut at a time: Wwise's (guest 710107fb74) for the whole session, and CRI's
 * (guest 7101108f8c, CRI Atom's own output) while a movie plays. Each one is its own output with its own queue,
 * released list, event and waveOut stream (Windows mixes the streams), as on the console. Until 2026-10-26 this file
 * had one global queue: the second open reset it, and the two then took each other's released buffers. CRI got back
 * Wwise's buffers, which it does not own, so its own output stalled and every movie ran at about 1/6 speed (issue #63). */
#define AO_MAX 4

typedef struct {
    WAVEHDR hdr;
    int16_t *pcm;                                     /* host copy, stereo */
    size_t capacity;                                  /* bytes allocated for pcm */
    uint64_t guest;                                   /* the guest's AudioOutBuffer */
    LONGLONG due;                                     /* clock-driven mode: when it has been "played" */
    unsigned frames;
    int prepared;
} Slot;

typedef struct {
    uint64_t handle;                                  /* the guest's AudioOut object (0 = unused entry) */
    uint64_t event;
    int rate, channels, open, started;
    HWAVEOUT wave;
    Slot slots[SLOTS];
    unsigned head, tail;                              /* playing queue: slots head..tail-1 (mod SLOTS) */
    uint64_t released[SLOTS];
    unsigned rel_head, rel_tail;
    LONGLONG clock_due;
    int tone_left, heard;
    unsigned long long tone_phase;
    /* statistics, protected by g_lock */
    unsigned long long total_appended, total_released, total_dropped, nonsilent_seconds;
    unsigned sec_appended, sec_released, sec_peak, sec_polls, sec_empty;
    unsigned long long sec_frames;
    /* Underruns (bounded round 2026-10-09): the device played every buffer it had while the output was started, i.e.
     * the game did not append the next one in time; the listener hears a gap until the next AppendAudioOutBuffer.
     * Counted from the first buffer with sound on (before it the game's mixer may legitimately idle). */
    unsigned long long underruns, underrun_ticks, underrun_longest;
    LONGLONG empty_since;
    unsigned queue_max;
    /* A second, stricter measure: at an append, the DEVICE's own play position (waveOutGetPosition, samples) had
     * already reached the end of everything handed to it: the output really ran out of samples. */
    unsigned long long frames_submitted, starved;
    /* the 10 s report window */
    unsigned acc_appended, acc_released, acc_peak, acc_polls, acc_empty;
    unsigned long long acc_frames;
    unsigned char who_seen[2];
} AoDev;

static CRITICAL_SECTION g_lock;
static uint64_t g_device_event;
static AoDev g_ao[AO_MAX];
static HANDLE g_wake;                                 /* every waveOut stream signals this one event */
static LARGE_INTEGER g_freq;
static float g_volume = 1.0f;                         /* output volume of the active device */
static const char g_device_name[] = "AudioTvOutput";
static FILE *g_wav;                                   /* SSBU_WAV: what the FIRST AudioOut opened plays */
static AoDev *g_wav_dev;
static int g_wav_rate = 48000;
static unsigned long long g_wav_bytes;

const char *guest_thread_name_of(unsigned long tid);   /* os_threads.c */
static void audio_who(AoDev *d, int slot, const char *what, Ctx *c) {   /* once per AudioOut and call */
    if (d->who_seen[slot]) return;
    d->who_seen[slot] = 1;
    const char *n = guest_thread_name_of(GetCurrentThreadId());
    printf("nn::audio: %s on AudioOut %d is called from guest %llx on thread \"%s\"\n", what, (int)(d - g_ao),
           (unsigned long long)c->import_from, n ? n : "?");
    fflush(stdout);
}

/* The AudioOut the guest object belongs to; null if it was never opened. Caller holds g_lock. */
static AoDev *ao_find(uint64_t handle) {
    for (int i = 0; i < AO_MAX; i++) if (g_ao[i].handle == handle && handle) return &g_ao[i];
    return NULL;
}

static LONGLONG now_ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static void wav_header(void) {
    unsigned char h[44];
    uint32_t data = (uint32_t)g_wav_bytes, riff = data + 36, rate = (uint32_t)g_wav_rate, bytes_per_sec = rate * 4;
    uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
    uint32_t fmt_size = 16;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmt_size, 4);
    memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &bytes_per_sec, 4);
    memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, g_wav);
    fseek(g_wav, 0, SEEK_END);
    fflush(g_wav);
}

static int16_t sat16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* Copies a guest buffer to host stereo samples; returns the number of frames and the peak amplitude. */
static unsigned convert(const AoDev *d, Slot *s, const int16_t *in, size_t bytes, unsigned *peak_out) {
    unsigned frames = (unsigned)(bytes / (2u * (unsigned)d->channels)), peak = 0;
    size_t need = (size_t)frames * 4;
    if (need > s->capacity || !s->pcm) {
        free(s->pcm);
        s->capacity = need < 4096 ? 4096 : need;
        s->pcm = malloc(s->capacity);
    }
    int16_t *out = s->pcm;
    for (unsigned i = 0; i < frames; i++) {
        float l, r;
        if (d->channels == 6) {                       /* FL FR FC LFE RL RR */
            const int16_t *p = in + (size_t)i * 6;
            l = p[0] + 0.707f * p[2] + 0.251f * p[3] + 0.707f * p[4];
            r = p[1] + 0.707f * p[2] + 0.251f * p[3] + 0.707f * p[5];
        } else if (d->channels == 1) {
            l = r = in[i];
        } else {
            const int16_t *p = in + (size_t)i * (unsigned)d->channels;
            l = p[0];
            r = p[1];
        }
        int li = (int)(l * g_volume), ri = (int)(r * g_volume);
        out[i * 2] = sat16(li);
        out[i * 2 + 1] = sat16(ri);
        unsigned a = (unsigned)abs(out[i * 2]), b = (unsigned)abs(out[i * 2 + 1]);
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    *peak_out = peak;
    return frames;
}

/* SSBU_AUDIO_TONE: a quiet 440 Hz tone over the first second, to check the host output path by ear. */
static unsigned add_tone(AoDev *d, int16_t *pcm, unsigned frames, unsigned peak) {
    for (unsigned i = 0; i < frames; i++, d->tone_phase++) {
        int v = (int)(3000.0 * sin(6.283185307179586 * 440.0 * (double)d->tone_phase / d->rate));
        for (int k = 0; k < 2; k++) {
            pcm[i * 2 + k] = sat16(pcm[i * 2 + k] + v);
            unsigned a = (unsigned)abs(pcm[i * 2 + k]);
            if (a > peak) peak = a;
        }
    }
    return peak;
}

/* Moves the buffers that have finished playing to the released list. Returns how many. Caller holds g_lock. */
static int collect_finished(AoDev *d, int everything) {
    int n = 0;
    LONGLONG now = now_ticks();
    while (d->head != d->tail) {
        Slot *s = &d->slots[d->head % SLOTS];
        if (!everything) {
            if (s->prepared) { if (!(s->hdr.dwFlags & WHDR_DONE)) break; }
            else if (!d->started || now < s->due) break;
        }
        if (s->prepared) { waveOutUnprepareHeader(d->wave, &s->hdr, sizeof s->hdr); s->prepared = 0; }
        d->released[d->rel_tail++ % SLOTS] = s->guest;
        d->head++;
        d->total_released++;
        d->sec_released++;
        n++;
    }
    if (n && !everything && d->head == d->tail && d->started && d->heard && !d->empty_since) { d->underruns++; d->empty_since = now; }
    return n;
}

static DWORD WINAPI audio_thread(LPVOID arg) {
    (void)arg;
    LONGLONG next_report = now_ticks() + g_freq.QuadPart;
    unsigned seconds = 0;
    int every_second = getenv("SSBU_AUDIO_LOG") != NULL;
    for (;;) {
        WaitForSingleObject(g_wake, 2);
        uint64_t ev[AO_MAX];
        char lines[AO_MAX][320];
        int report[AO_MAX];
        EnterCriticalSection(&g_lock);
        LONGLONG now = now_ticks();
        int tick = now >= next_report;
        if (tick) { next_report += g_freq.QuadPart; seconds++; }
        if (tick && g_wav) wav_header();
        for (int i = 0; i < AO_MAX; i++) {
            AoDev *d = &g_ao[i];
            int n = d->open ? collect_finished(d, 0) : 0;
            ev[i] = n ? d->event : 0;
            report[i] = 0;
            if (!tick || !d->handle) continue;
            if (d->sec_peak) d->nonsilent_seconds++;
            d->acc_appended += d->sec_appended; d->acc_released += d->sec_released; d->acc_polls += d->sec_polls;
            d->acc_empty += d->sec_empty; d->acc_frames += d->sec_frames;
            if (d->sec_peak > d->acc_peak) d->acc_peak = d->sec_peak;
            d->sec_appended = d->sec_released = d->sec_peak = d->sec_polls = d->sec_empty = 0;
            d->sec_frames = 0;
            if (d->open && (every_second || seconds % 10 == 0)) {
                unsigned window = every_second ? 1 : 10;
                snprintf(lines[i], sizeof lines[i],
                         "nn::audio: [%d] t=%us appended %u released %u polls %u (empty %u) queued %u peak %u, total %llu buffers, %llu s with sound\n"
                         "nn::audio: [%d]   %llu frames in that window = %.0f Hz of output (the device runs at %d Hz)\n",
                         i, seconds, d->acc_appended, d->acc_released, d->acc_polls, d->acc_empty, d->tail - d->head, d->acc_peak,
                         d->total_appended, d->nonsilent_seconds, i, d->acc_frames, (double)d->acc_frames / window, d->rate);
                report[i] = 1;
            }
            if (!d->open || every_second || seconds % 10 == 0) {
                d->acc_appended = d->acc_released = d->acc_peak = d->acc_polls = d->acc_empty = 0;
                d->acc_frames = 0;
            }
        }
        LeaveCriticalSection(&g_lock);
        for (int i = 0; i < AO_MAX; i++) if (ev[i]) os_signal_event(ev[i]);
        for (int i = 0; i < AO_MAX; i++) if (report[i]) { fputs(lines[i], stdout); fflush(stdout); }
    }
}

static void audio_InitializeAudioOutParameter(Ctx *c) {   /* (AudioOutParameter *): u32 sampleRate, u16 channelCount, u16 reserved */
    wr32(A0, 0);
    wr16(A0 + 4, 0);
    wr16(A0 + 6, 0);
}

static void audio_OpenDefaultAudioOut(Ctx *c) {       /* (AudioOut *, nn::os::SystemEvent *, const AudioOutParameter &) -> Result */
    unsigned want_rate = rd32(A2), want_channels = rd16(A2 + 4);
    EnterCriticalSection(&g_lock);
    AoDev *d = ao_find(A0);
    if (d && d->open) {                               /* the same object opened twice: start it over */
        LeaveCriticalSection(&g_lock);
        printf("nn::audio: OpenDefaultAudioOut on an AudioOut that is already open; it is reopened\n");
        EnterCriticalSection(&g_lock);
    }
    if (!d) for (int i = 0; i < AO_MAX && !d; i++) if (!g_ao[i].handle || !g_ao[i].open) d = &g_ao[i];
    if (!d) {
        LeaveCriticalSection(&g_lock);
        printf("nn::audio: OpenDefaultAudioOut: more than %d AudioOuts open at once; refused\n", AO_MAX);
        RET(0x1c99);                                  /* any failure: the game logs it and does without */
        return;
    }
    HWAVEOUT old = d->open ? d->wave : NULL;
    if (old) { waveOutReset(old); collect_finished(d, 1); }
    for (unsigned i = 0; i < SLOTS; i++) d->slots[i].prepared = 0;
    unsigned long long keep_appended = d->handle == A0 ? d->total_appended : 0;   /* statistics stay per object */
    unsigned long long keep_released = d->handle == A0 ? d->total_released : 0;
    {   /* everything but the host copies of the samples (Slot.pcm) starts over */
        Slot keep[SLOTS];
        memcpy(keep, d->slots, sizeof keep);
        memset(d, 0, sizeof *d);
        for (unsigned i = 0; i < SLOTS; i++) { d->slots[i].pcm = keep[i].pcm; d->slots[i].capacity = keep[i].capacity; }
    }
    d->total_appended = keep_appended;
    d->total_released = keep_released;
    d->handle = A0;
    d->event = A1;
    d->rate = want_rate ? (int)want_rate : 48000;
    const char *env = getenv("SSBU_AUDIO_CHANNELS");
    d->channels = want_channels == 6 && env && atoi(env) == 6 ? 6 : 2;
    os_init_event(d->event, 1);
    if (!getenv("SSBU_AUDIO_MUTE")) {
        WAVEFORMATEX f;
        memset(&f, 0, sizeof f);
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 2;
        f.nSamplesPerSec = (DWORD)d->rate;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 4;
        f.nAvgBytesPerSec = (DWORD)d->rate * 4;
        MMRESULT r = waveOutOpen(&d->wave, WAVE_MAPPER, &f, (DWORD_PTR)g_wake, 0, CALLBACK_EVENT);
        if (r != MMSYSERR_NOERROR) { d->wave = NULL; printf("nn::audio: waveOutOpen failed (%u): no sound, buffers are released by the clock\n", (unsigned)r); }
        else {
            waveOutPause(d->wave);                    /* nothing plays before StartAudioOut */
            /* Test copies are silent: only the copies a person plays (ssbu_native.exe, ssbu_user.exe) are audible,
             * like the window rule in display.c. The device stays open, so timing is the same as with sound.
             * SSBU_VOLUME=<0..100> overrides either way. */
            char exe[MAX_PATH] = "";
            GetModuleFileNameA(NULL, exe, sizeof exe);
            const char *base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe, *vol = getenv("SSBU_VOLUME");
            int percent = vol ? atoi(vol) : (!_stricmp(base, "ssbu_native.exe") || !_stricmp(base, "ssbu_user.exe") || !_stricmp(base, "game_native.exe")) ? 100 : 0;   /* [Recomps#1] our exe is audible */
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            DWORD level = (DWORD)(0xffff * percent / 100);
            waveOutSetVolume(d->wave, level | level << 16);
            printf("nn::audio: host volume %d%%%s\n", percent, percent ? "" : " (test copy: silent; SSBU_VOLUME=<0..100> to change)");
        }
    }
    const char *wav = getenv("SSBU_WAV");
    if (wav && !g_wav) {
        g_wav = fopen(wav, "wb");
        g_wav_bytes = 0;
        g_wav_dev = d;
        g_wav_rate = d->rate;
        if (g_wav) wav_header();
    }
    d->tone_left = getenv("SSBU_AUDIO_TONE") ? d->rate / 512 : 0;
    d->open = 1;
    int index = (int)(d - g_ao);
    LeaveCriticalSection(&g_lock);
    if (old) waveOutClose(old);
    const char *n = guest_thread_name_of(GetCurrentThreadId());
    printf("nn::audio: OpenDefaultAudioOut [%d] (from guest %llx, thread \"%s\"): game asks %u Hz, %u channels -> %d Hz, %d channels, 16-bit, %s\n",
           index, (unsigned long long)c->import_from, n ? n : "?", want_rate, want_channels, d->rate, d->channels,
           d->wave ? "waveOut device open" : "no host device");
    RET(0);
}

static void audio_GetAudioOutChannelCount(Ctx *c) {
    EnterCriticalSection(&g_lock); AoDev *d = ao_find(A0); int v = d ? d->channels : 2; LeaveCriticalSection(&g_lock);
    RET(v);
}
static void audio_GetAudioOutSampleRate(Ctx *c) {
    EnterCriticalSection(&g_lock); AoDev *d = ao_find(A0); int v = d ? d->rate : 48000; LeaveCriticalSection(&g_lock);
    RET(v);
}
static void audio_GetAudioOutSampleFormat(Ctx *c) { RET(SAMPLE_FORMAT_PCM16); }

static void audio_SetAudioOutBufferInfo(Ctx *c) {     /* (AudioOutBuffer *, void *data, size_t capacity, size_t dataSize) */
    wr64(A0 + BUF_NEXT, 0);
    wr64(A0 + BUF_DATA, A1);
    wr64(A0 + BUF_CAPACITY, A2);
    wr64(A0 + BUF_SIZE, A3);
    wr64(A0 + BUF_OFFSET, 0);
}

static void audio_GetAudioOutBufferDataPointer(Ctx *c) { RET(rd64(A0 + BUF_DATA)); }

static void audio_AppendAudioOutBuffer(Ctx *c) {      /* (AudioOut *, AudioOutBuffer *) -> bool */
    uint64_t buf = A1, data = rd64(buf + BUF_DATA), size = rd64(buf + BUF_SIZE);
    int ok = 0;
    EnterCriticalSection(&g_lock);
    AoDev *d = ao_find(A0);
    if (d) audio_who(d, 0, "AppendAudioOutBuffer", c);
    if (d && d->open && d->tail - d->head < SLOTS && data && size) {
        Slot *s = &d->slots[d->tail % SLOTS];
        unsigned peak = 0;
        unsigned frames = convert(d, s, (const int16_t *)(uintptr_t)data, (size_t)size, &peak);
        s->guest = buf;
        s->frames = frames;
        s->prepared = 0;
        if (d->tone_left > 0) { d->tone_left--; peak = add_tone(d, s->pcm, frames, peak); }
        if (peak && !d->heard) { d->heard = 1; printf("nn::audio: [%d] first buffer with sound (buffer %llu, peak %u)\n", (int)(d - g_ao), d->total_appended, peak); }
        {   /* TEST ONLY, SSBU_AUDIO_HASH=<file> (round "four players"): one line per buffer the game hands to the device:
             * index, frames, peak, FNV-1a 64 of the game's own samples (before our stereo copy). Compares the mixer's output
             * of two builds (tests\p15_pcm_compare.py). One getenv at the first buffer; nothing without the variable.
             * Only the first AudioOut opened (the game's own mixer) is hashed. */
            static FILE *hf; static int tried;
            if (!tried) { const char *hp = getenv("SSBU_AUDIO_HASH"); tried = 1; if (hp && *hp) hf = fopen(hp, "w"); }
            if (hf && d == &g_ao[0]) {
                uint64_t h = 0xcbf29ce484222325ULL;
                const uint8_t *pb = (const uint8_t *)(uintptr_t)data;
                for (uint64_t i = 0; i < size; i++) { h ^= pb[i]; h *= 0x100000001b3ULL; }
                fprintf(hf, "%llu %u %u %016llx\n", d->total_appended, frames, peak, (unsigned long long)h);
                if ((d->total_appended & 63) == 0) fflush(hf);
            }
        }
        if (g_wav && d == g_wav_dev && g_wav_bytes < (unsigned long long)WAV_SECONDS * (unsigned)g_wav_rate * 4) {
            fwrite(s->pcm, 4, frames, g_wav);
            g_wav_bytes += (unsigned long long)frames * 4;
        }
        if (d->wave) {
            if (d->started && d->heard && d->frames_submitted) {
                MMTIME mt;
                memset(&mt, 0, sizeof mt);
                mt.wType = TIME_SAMPLES;
                if (waveOutGetPosition(d->wave, &mt, sizeof mt) == MMSYSERR_NOERROR && mt.wType == TIME_SAMPLES && (unsigned long long)mt.u.sample >= d->frames_submitted) d->starved++;
            }
            d->frames_submitted += frames;
            memset(&s->hdr, 0, sizeof s->hdr);
            s->hdr.lpData = (LPSTR)s->pcm;
            s->hdr.dwBufferLength = frames * 4;
            if (waveOutPrepareHeader(d->wave, &s->hdr, sizeof s->hdr) == MMSYSERR_NOERROR) {
                if (waveOutWrite(d->wave, &s->hdr, sizeof s->hdr) == MMSYSERR_NOERROR) s->prepared = 1;
                else waveOutUnprepareHeader(d->wave, &s->hdr, sizeof s->hdr);
            }
        }
        if (!s->prepared) {                           /* clock-driven: one buffer after the other in real time */
            LONGLONG now = now_ticks();
            if (d->clock_due < now) d->clock_due = now;
            d->clock_due += (LONGLONG)frames * g_freq.QuadPart / d->rate;
            s->due = d->clock_due;
        }
        d->tail++;
        if (d->empty_since) {
            unsigned long long gap = (unsigned long long)(now_ticks() - d->empty_since);
            d->underrun_ticks += gap; if (gap > d->underrun_longest) d->underrun_longest = gap;
            d->empty_since = 0;
        }
        if (d->tail - d->head > d->queue_max) d->queue_max = d->tail - d->head;
        d->total_appended++;
        d->sec_appended++;
        d->sec_frames += frames;
        if (peak > d->sec_peak) d->sec_peak = peak;
        ok = 1;
    } else if (d) {
        d->total_dropped++;
    }
    LeaveCriticalSection(&g_lock);
    RET(ok);
}

/* End-of-run line (hostrt.c report_and_exit): what the output path did in this run, per AudioOut. */
void audio_report(FILE *out) {
    double f = g_freq.QuadPart ? (double)g_freq.QuadPart : 1.0;
    for (int i = 0; i < AO_MAX; i++) {
        const AoDev *d = &g_ao[i];
        if (!d->total_appended) continue;
        fprintf(out, "nn::audio: [%d] %d Hz, %d channels granted, 16-bit; %llu buffers appended, %llu released, %llu refused; deepest queue %u buffers; "
                     "underruns (device ran dry while started, after the first sound): %llu, %.1f ms in all, longest %.1f ms; %llu s with sound\n",
                i, d->rate, d->channels, d->total_appended, d->total_released, d->total_dropped, d->queue_max,
                d->underruns, (double)d->underrun_ticks * 1000.0 / f, (double)d->underrun_longest * 1000.0 / f, d->nonsilent_seconds);
        fprintf(out, "nn::audio: [%d] appends that found the device's own play position at the end of everything submitted (the output had no samples left): %llu of %llu\n",
                i, d->starved, d->total_appended);
    }
}

static void audio_StartAudioOut(Ctx *c) {             /* (AudioOut *) -> Result */
    unsigned long long queued = 0;
    int index = -1;
    EnterCriticalSection(&g_lock);
    AoDev *d = ao_find(A0);
    if (d && d->open && !d->started) {
        d->started = 1;
        d->clock_due = now_ticks();                   /* clock-driven buffers queued before the start begin now */
        for (unsigned i = d->head; i != d->tail; i++) {
            Slot *s = &d->slots[i % SLOTS];
            d->clock_due += (LONGLONG)s->frames * g_freq.QuadPart / d->rate;
            s->due = d->clock_due;
        }
        if (d->wave) waveOutRestart(d->wave);
    }
    if (d) { queued = d->tail - d->head; index = (int)(d - g_ao); }
    LeaveCriticalSection(&g_lock);
    printf("nn::audio: StartAudioOut [%d] (%llu buffers queued)\n", index, queued);
    RET(0);
}

static void audio_GetReleasedAudioOutBuffer(Ctx *c) { /* (AudioOut *) -> AudioOutBuffer * or null */
    uint64_t r = 0;
    EnterCriticalSection(&g_lock);
    AoDev *d = ao_find(A0);
    if (d) {
        audio_who(d, 1, "GetReleasedAudioOutBuffer", c);
        if (d->open) collect_finished(d, 0);
        if (d->rel_head != d->rel_tail) r = d->released[d->rel_head++ % SLOTS];
        d->sec_polls++;
        if (!r) d->sec_empty++;
    }
    LeaveCriticalSection(&g_lock);
    RET(r);
}

static void audio_StopAudioOut(Ctx *c) {              /* (AudioOut *): everything queued becomes released */
    EnterCriticalSection(&g_lock);
    AoDev *d = ao_find(A0);
    HWAVEOUT w = d ? d->wave : NULL;
    LeaveCriticalSection(&g_lock);
    if (!d) { printf("nn::audio: StopAudioOut on an AudioOut that was never opened\n"); return; }
    if (w) waveOutReset(w);                           /* marks every queued header as done */
    EnterCriticalSection(&g_lock);
    d->frames_submitted = 0;                          /* waveOutReset also sets the device position back to 0 */
    if (d->open) collect_finished(d, 1);
    d->started = 0;
    if (d->wave) waveOutPause(d->wave);
    uint64_t ev = d->event;
    int index = (int)(d - g_ao);
    LeaveCriticalSection(&g_lock);
    if (ev) os_signal_event(ev);
    printf("nn::audio: StopAudioOut [%d]\n", index);
}

static void audio_CloseAudioOut(Ctx *c) {             /* (AudioOut *) */
    EnterCriticalSection(&g_lock);
    AoDev *d = ao_find(A0);
    HWAVEOUT w = d ? d->wave : NULL;
    LeaveCriticalSection(&g_lock);
    if (!d) { printf("nn::audio: CloseAudioOut on an AudioOut that was never opened\n"); return; }
    if (w) waveOutReset(w);
    EnterCriticalSection(&g_lock);
    if (d->open) collect_finished(d, 1);
    d->open = d->started = 0;
    d->wave = NULL;
    d->event = 0;
    if (g_wav && d == g_wav_dev) wav_header();
    unsigned long long app = d->total_appended, rel = d->total_released;
    int index = (int)(d - g_ao);
    LeaveCriticalSection(&g_lock);
    if (w) waveOutClose(w);
    printf("nn::audio: CloseAudioOut [%d] (%llu buffers appended, %llu released)\n", index, app, rel);
}

/* ---------------------------------------------------------------- audio device */
/* [sdk] 0x990f0 AcquireAudioDeviceNotificationForOutput(SystemEvent *): a second call returns 0x499 (module 153,
 * description 2; flag byte 0xc5efa2); otherwise it asks the audio device service for its output event
 * (slot +0x78 = QueryAudioDeviceOutputEvent, any failure aborts) and attaches the handle with
 * nn::os::AttachReadableHandleToSystemEvent(event, handle, managed, clear mode 1 = auto clear); Result 0.
 * The service side (not in the dump), after [doc] suyu audio/audio_device.cpp: the event is signalled when the
 * service object is created, i.e. it arrives signalled, and again whenever the output device changes.
 * Here: the output device is the Windows default render endpoint; IMMNotificationClient::OnDefaultDeviceChanged
 * (render, console role) signals the event. */
typedef struct NotifyClient { const void *const *vtbl; } NotifyClient;
typedef struct { const void *const *vtbl; } ComObj;
static const GUID k_clsid_enumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID k_iid_enumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID k_iid_notify = {0x7991eec9, 0x7e89, 0x4d85, {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};
static const GUID k_iid_unknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static volatile LONG g_device_acquired, g_device_changes;
static HRESULT STDMETHODCALLTYPE nc_query(NotifyClient *self, const GUID *iid, void **out) {
    if (!memcmp(iid, &k_iid_notify, sizeof *iid) || !memcmp(iid, &k_iid_unknown, sizeof *iid)) { *out = self; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE nc_ref(NotifyClient *self) { (void)self; return 1; }      /* static object */
static HRESULT STDMETHODCALLTYPE nc_state(NotifyClient *self, const WCHAR *id, DWORD state) { (void)self; (void)id; (void)state; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_id(NotifyClient *self, const WCHAR *id) { (void)self; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_default(NotifyClient *self, int flow, int role, const WCHAR *id) {
    (void)self; (void)id;
    if (flow == 0 && role == 0) {                             /* eRender, eConsole */
        InterlockedIncrement(&g_device_changes);
        printf("nn::audio: the Windows default output device changed: device notification signalled\n");
        if (g_device_event) os_signal_event(g_device_event);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_prop(NotifyClient *self, const WCHAR *id, PROPERTYKEY key) { (void)self; (void)id; (void)key; return S_OK; }
static const void *const g_nc_vtbl[] = {nc_query, nc_ref, nc_ref, nc_state, nc_id, nc_id, nc_default, nc_prop};   /* IUnknown, OnDeviceStateChanged, OnDeviceAdded, OnDeviceRemoved, OnDefaultDeviceChanged, OnPropertyValueChanged */
static NotifyClient g_nc = {g_nc_vtbl};
static DWORD WINAPI device_watch_thread(void *arg) {
    ComObj *en = NULL;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(&k_clsid_enumerator, NULL, CLSCTX_ALL, &k_iid_enumerator, (void **)&en);
    if (SUCCEEDED(hr) && en) {
        /* IMMDeviceEnumerator: slot 6 = RegisterEndpointNotificationCallback(IMMNotificationClient *) */
        hr = ((HRESULT (STDMETHODCALLTYPE *)(ComObj *, NotifyClient *))en->vtbl[6])(en, &g_nc);
    }
    if (FAILED(hr)) printf("nn::audio: the Windows device notification is not available (0x%08lx): output device changes are not reported\n", (unsigned long)hr);
    Sleep(INFINITE);                                          /* keeps the apartment and the registration alive */
    return 0;
}
#define AUDIO_DEVICE_NOTIFICATION_ALREADY_ACQUIRED 0x499u
static void audio_AcquireAudioDeviceNotificationForOutput(Ctx *c) {   /* (nn::os::SystemEvent *) -> Result */
    if (InterlockedCompareExchange(&g_device_acquired, 1, 0) != 0) { RET(AUDIO_DEVICE_NOTIFICATION_ALREADY_ACQUIRED); return; }
    os_init_event(A0, 1);
    g_device_event = A0;
    os_signal_event(A0);                                      /* the service's event arrives signalled */
    CloseHandle(CreateThread(NULL, 0, device_watch_thread, NULL, 0, NULL));
    RET(0);
}

/* [Recomps#1] SDK 3.6.0 forms our game uses.
 * ListAudioDeviceName(AudioDeviceName *names, int count) -> int: char[0x100] names of the console's outputs. Our sdk asks the
 * audio service (IPC); the answer here is the console's three outputs [doc: switchbrew audren IAudioDevice], the active one
 * (g_device_name) first. */
static void audio_ListAudioDeviceName(Ctx *c) {
    static const char *const names[] = {g_device_name, "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput"};
    int n = (int)A1 < 3 ? (int)A1 : 3;
    for (int i = 0; i < n; i++) {
        memset((void *)(uintptr_t)(A0 + 0x100ull * i), 0, 0x100);
        strcpy((char *)(uintptr_t)(A0 + 0x100ull * i), names[i]);
    }
    RET(n < 0 ? 0 : n);
}
/* GetAudioOutName(const AudioOut *) -> const char *: [sdk 0x185920] returns AudioOut + 0x10 (the name the service gave at
 * open). This runtime's AudioOut is host state, so the default output's name [doc: libnx audout "DeviceOut"] is returned. */
static void audio_GetAudioOutName(Ctx *c) { static const char name[] = "DeviceOut"; RET((uint64_t)(uintptr_t)name); }
/* GetAudioOutBufferDataSize(const AudioOutBuffer *) -> size_t: [sdk 0x173978] ldr [x0, #0x18], the dataSize that
 * SetAudioOutBufferInfo stored (BUF_SIZE above). */
static void audio_GetAudioOutBufferDataSize(Ctx *c) { RET(rd64(A0 + BUF_SIZE)); }

static void audio_GetActiveAudioDeviceName(Ctx *c) {  /* (AudioDeviceName *): char[0x100] */
    memset((void *)(uintptr_t)A0, 0, 0x100);
    memcpy((void *)(uintptr_t)A0, g_device_name, sizeof g_device_name);
}

static void audio_SetAudioDeviceOutputVolume(Ctx *c) {   /* (const AudioDeviceName *, float) */
    const char *name = (const char *)(uintptr_t)A0;
    float v = c->v[0].f[0];
    trace_printf("nn::audio: SetAudioDeviceOutputVolume(%s, %g)\n", name, v);
    if (strcmp(name, g_device_name) == 0 && v >= 0.0f && v <= 4.0f) g_volume = v;
}

/* ---------------------------------------------------------------- nn::codec::HardwareOpusDecoder */
#define OPUS_SLOTS 10                                 /* [sdk] _ZTVN2nn5codec19HardwareOpusDecoderE has exactly 10 entries (0x00..0x48) */
#define OPUS_HEADER 8
#define OPUS_MAX_FRAMES 5760                          /* 120 ms at 48 kHz, the longest Opus packet */
#define OBJ_VTABLE 0x00
#define OBJ_STATE 0x08                                /* our decoder state (inside the game's work buffer) */
#define OBJ_RATE 0x10
#define OBJ_CHANNELS 0x14

static uint64_t g_opus_vtable[OPUS_SLOTS];
static char g_opus_names[OPUS_SLOTS][48];
static unsigned char g_opus_seen[OPUS_SLOTS];
static HMODULE g_opus_dll;
static int g_opus_tried;
static int (*p_opus_decoder_get_size)(int channels);
static int (*p_opus_decoder_init)(void *st, int rate, int channels);
static int (*p_opus_decode)(void *st, const unsigned char *data, int len, int16_t *pcm, int frame_size, int fec);
static volatile LONG64 g_opus_packets, g_opus_errors, g_opus_frames;

static void opus_load(void) {
    if (g_opus_tried) return;
    g_opus_tried = 1;
    const char *env = getenv("SSBU_OPUS_DLL");
    const char *const tries[] = { env, "libopus-0.dll", "opus.dll", "C:\\msys64\\ucrt64\\bin\\libopus-0.dll", "C:\\msys64\\mingw64\\bin\\libopus-0.dll", "C:\\msys64\\clang64\\bin\\libopus-0.dll" };
    const char *used = NULL;
    for (unsigned i = 0; i < sizeof tries / sizeof tries[0] && !g_opus_dll; i++) {
        if (!tries[i]) continue;
        g_opus_dll = LoadLibraryExA(tries[i], NULL, strchr(tries[i], '\\') ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
        used = tries[i];
    }
    if (g_opus_dll) {
        p_opus_decoder_get_size = (int (*)(int))GetProcAddress(g_opus_dll, "opus_decoder_get_size");
        p_opus_decoder_init = (int (*)(void *, int, int))GetProcAddress(g_opus_dll, "opus_decoder_init");
        p_opus_decode = (int (*)(void *, const unsigned char *, int, int16_t *, int, int))GetProcAddress(g_opus_dll, "opus_decode");
        if (!p_opus_decoder_get_size || !p_opus_decoder_init || !p_opus_decode) g_opus_dll = NULL;
    }
    printf("nn::codec: libopus %s%s\n", g_opus_dll ? "loaded from " : "NOT FOUND: Opus streams will be silent (set SSBU_OPUS_DLL)", g_opus_dll ? used : "");
}

static uint32_t be32(uint64_t a) { uint32_t v = rd32(a); return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }

static void opus_destructor(Ctx *c) { wr64(A0 + OBJ_STATE, 0); }

static void opus_GetWorkBufferSize(Ctx *c) {          /* (this, int sampleRate, int channelCount) -> size_t */
    opus_load();
    int channels = (int)A2 == 1 ? 1 : 2;
    size_t size = g_opus_dll ? (size_t)p_opus_decoder_get_size(channels) : 0;
    RET((size + 0x1fff) & ~(size_t)0xfff);
}

static void opus_Initialize(Ctx *c) {                 /* (this, int sampleRate, int channelCount, void *buffer, size_t size) -> 0 ok */
    uint64_t self = A0, buffer = A3;
    int rate = (int)A1, channels = (int)A2, ok = 0;
    opus_load();
    wr32(self + OBJ_RATE, (uint32_t)rate);
    wr32(self + OBJ_CHANNELS, (uint32_t)channels);
    wr64(self + OBJ_STATE, 0);
    if (g_opus_dll && buffer && (channels == 1 || channels == 2) && (size_t)p_opus_decoder_get_size(channels) <= A4) {
        ok = p_opus_decoder_init((void *)(uintptr_t)buffer, rate, channels) == 0;
        if (ok) wr64(self + OBJ_STATE, buffer);
    }
    printf("nn::codec: HardwareOpusDecoder::Initialize(%d Hz, %d channels, work buffer %llu bytes): %s\n", rate, channels,
           (unsigned long long)A4, ok ? "libopus decoder ready" : "no decoder, output will be silence");
    RET(0);
}

static void opus_Finalize(Ctx *c) { wr64(A0 + OBJ_STATE, 0); wr32(A0 + OBJ_RATE, 0); wr32(A0 + OBJ_CHANNELS, 0); }

static void opus_DecodeInterleaved(Ctx *c) {          /* (this, size_t *consumed, int *sampleCount, int16_t *out, size_t outBytes, const void *in, size_t inBytes) */
    uint64_t self = A0, in = A5, in_size = A6;
    int16_t *out = (int16_t *)(uintptr_t)A3;
    int channels = (int)rd32(self + OBJ_CHANNELS), rate = (int)rd32(self + OBJ_RATE);
    if (channels != 1) channels = 2;
    int room = (int)(A4 / (2u * (unsigned)channels));  /* frames that fit in the output */
    if (in_size < OPUS_HEADER) { InterlockedIncrement64(&g_opus_errors); RET(6); return; }      /* any non-zero value: the game only tests for 0 */
    uint32_t len = be32(in);
    if ((uint64_t)len + OPUS_HEADER > in_size) { InterlockedIncrement64(&g_opus_errors); RET(6); return; }
    void *st = (void *)(uintptr_t)rd64(self + OBJ_STATE);
    int frames;
    if (st) {
        { void r1_nosim(uint64_t, uint64_t); r1_nosim((uint64_t)(uintptr_t)out, A4); r1_nosim((uint64_t)(uintptr_t)st, (uint64_t)p_opus_decoder_get_size(channels)); }   /* R1 (r1_state.c): decoded sound and the decoder's state (libopus writes it in the game's work buffer, round 5) are not simulation state (nothing without SSBU_R1_MARK) */
        frames = p_opus_decode(st, (const unsigned char *)(uintptr_t)(in + OPUS_HEADER), (int)len, out, room > OPUS_MAX_FRAMES ? OPUS_MAX_FRAMES : room, 0);
        if (frames < 0) {
            if (InterlockedIncrement64(&g_opus_errors) <= 5) printf("nn::codec: opus_decode error %d (packet of %u bytes, input %llu bytes)\n", frames, len, (unsigned long long)in_size);
            RET(6);
            return;
        }
    } else {
        frames = rate / 50;                           /* a 20 ms packet of silence */
        if (frames > room) frames = room;
        memset(out, 0, (size_t)frames * 2 * (unsigned)channels);
    }
    wr64(A1, (uint64_t)len + OPUS_HEADER);
    wr32(A2, (uint32_t)frames);
    InterlockedAdd64(&g_opus_frames, frames);
    LONG64 n = InterlockedIncrement64(&g_opus_packets);
    if (n == 1 || n % 5000 == 0)
        printf("nn::codec: %lld Opus packets decoded (%lld frames, %lld errors); last: %u bytes -> %d frames\n",
               (long long)n, (long long)g_opus_frames, (long long)g_opus_errors, len, frames);
    RET(0);
}

/* The four remaining virtual functions of the real class ([sdk] vtable: +0x18 IsInitialized, +0x30 GetSampleRate,
 * +0x38 GetChannelCount, +0x40 Reset) and the deleting destructor (+0x08). */
static void opus_IsInitialized(Ctx *c) { RET(rd32(A0 + OBJ_RATE) != 0); }       /* Initialize stores the rate, Finalize clears it */
static void opus_GetSampleRate(Ctx *c) { RET(rd32(A0 + OBJ_RATE)); }
static void opus_GetChannelCount(Ctx *c) { RET(rd32(A0 + OBJ_CHANNELS)); }
static void opus_Reset(Ctx *c) {                      /* back to the state right after Initialize: the decoder's history is dropped */
    void *st = (void *)(uintptr_t)rd64(A0 + OBJ_STATE);
    int channels = (int)rd32(A0 + OBJ_CHANNELS) == 1 ? 1 : 2;
    if (st && p_opus_decoder_init) p_opus_decoder_init(st, (int)rd32(A0 + OBJ_RATE), channels);
}
static void opus_deleting_destructor(Ctx *c) {        /* ~HardwareOpusDecoder() then operator delete(this): the game's own operator delete */
    uint64_t self = A0, del = 0;
    wr64(self + OBJ_STATE, 0);
    for (int i = 0; i < dyn_sym_count && !del; i++)
        if (dyn_syms[i].defined && strcmp(dyn_syms[i].name, "_ZdlPv") == 0) del = GUEST_BASE + dyn_syms[i].value;
    if (!del) guest_trap("HardwareOpusDecoder deleting destructor: the game's operator delete was not found", c->x[30] - 4);
    c->x[0] = self;
    call_guest(c, del);
}
static void opus_unknown(Ctx *c) {                    /* a virtual function the game was not seen to call */
    const char *name = hostfn_current_name();
    for (int i = 0; i < OPUS_SLOTS; i++)
        if (name == g_opus_names[i] && !g_opus_seen[i]) {
            g_opus_seen[i] = 1;
            printf("nn::codec: %s called (x1=%llx x2=%llx x3=%llx) -> 0\n", name, (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3);
        }
    RET(0);
}

static void opus_constructor(Ctx *c) {                /* HardwareOpusDecoder(int option) */
    EnterCriticalSection(&g_lock);
    if (!g_opus_vtable[0]) {
        for (int i = 0; i < OPUS_SLOTS; i++) {
            ImportFn fn = opus_unknown;
            const char *what = NULL;
            switch (i * 8) {
            case 0x00: fn = opus_destructor; what = "destructor"; break;
            case 0x08: fn = opus_deleting_destructor; what = "deleting destructor"; break;
            case 0x18: fn = opus_IsInitialized; what = "IsInitialized"; break;
            case 0x30: fn = opus_GetSampleRate; what = "GetSampleRate"; break;
            case 0x38: fn = opus_GetChannelCount; what = "GetChannelCount"; break;
            case 0x40: fn = opus_Reset; what = "Reset"; break;
            case 0x10: fn = opus_GetWorkBufferSize; what = "GetWorkBufferSize"; break;
            case 0x20: fn = opus_Initialize; what = "Initialize"; break;
            case 0x28: fn = opus_Finalize; what = "Finalize"; break;
            case 0x48: fn = opus_DecodeInterleaved; what = "DecodeInterleaved"; break;
            }
            if (what) snprintf(g_opus_names[i], sizeof g_opus_names[i], "HardwareOpusDecoder::%s", what);
            else snprintf(g_opus_names[i], sizeof g_opus_names[i], "HardwareOpusDecoder vtable+0x%x", i * 8);
            g_opus_vtable[i] = hostfn_address(g_opus_names[i], fn);
        }
    }
    LeaveCriticalSection(&g_lock);
    wr64(A0 + OBJ_VTABLE, (uint64_t)(uintptr_t)g_opus_vtable);
    wr64(A0 + OBJ_STATE, 0);
    wr64(A0 + OBJ_RATE, 0);
}

/* ---------------------------------------------------------------- registration */
#define AU(n) "_ZN2nn5audio" n
static const ImportDef audio_imports[] = {
    {AU("27InitializeAudioOutParameterEPNS0_17AudioOutParameterE"), audio_InitializeAudioOutParameter},
    {AU("19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE"), audio_OpenDefaultAudioOut},
    {AU("23GetAudioOutChannelCountEPKNS0_8AudioOutE"), audio_GetAudioOutChannelCount},
    {AU("21GetAudioOutSampleRateEPKNS0_8AudioOutE"), audio_GetAudioOutSampleRate},
    {AU("23GetAudioOutSampleFormatEPKNS0_8AudioOutE"), audio_GetAudioOutSampleFormat},
    {AU("21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm"), audio_SetAudioOutBufferInfo},
    {AU("28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE"), audio_GetAudioOutBufferDataPointer},
    {AU("20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE"), audio_AppendAudioOutBuffer},
    {AU("13StartAudioOutEPNS0_8AudioOutE"), audio_StartAudioOut},
    {AU("25GetReleasedAudioOutBufferEPNS0_8AudioOutE"), audio_GetReleasedAudioOutBuffer},
    {AU("12StopAudioOutEPNS0_8AudioOutE"), audio_StopAudioOut},
    {AU("13CloseAudioOutEPNS0_8AudioOutE"), audio_CloseAudioOut},
    {AU("39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE"), audio_AcquireAudioDeviceNotificationForOutput},
    {AU("24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE"), audio_GetActiveAudioDeviceName},
    {AU("19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi"), audio_ListAudioDeviceName}, {AU("15GetAudioOutNameEPKNS0_8AudioOutE"), audio_GetAudioOutName},   /* [Recomps#1] */
    {AU("25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE"), audio_GetAudioOutBufferDataSize},                                                          /* [Recomps#1] */
    {AU("26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf"), audio_SetAudioDeviceOutputVolume},
    {"_ZN2nn5codec19HardwareOpusDecoderC1Ei", opus_constructor},
};

void register_audio_imports(void) {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    register_imports(audio_imports, (int)(sizeof audio_imports / sizeof audio_imports[0]));
}
