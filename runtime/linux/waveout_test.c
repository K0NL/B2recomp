/* [Recomps#1] Linux only: self-test of runtime/linux/waveout_sdl.c (the waveOut emulation nnaudio.c plays through).
 * Build: clang -O1 -fdeclspec -I runtime/linux runtime/linux/waveout_test.c runtime/linux/waveout_sdl.c
 *        runtime/linux/win32_compat.c -lSDL3 -lpthread -ldl -lm -o waveout_test
 * Run:   SDL_AUDIO_DRIVER=dummy ./waveout_test   (timing only)   or   ./waveout_test   (a quiet 440 Hz tone for one second)
 * Checks: 10 buffers of 100 ms are all marked done, in order, the event is set, the position reaches the 48000 frames
 * handed over, it takes about a second of real time, and a reset releases what is still queued. */
#include "mmsystem.h"
#include <math.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    enum { RATE = 48000, N = 10, FRAMES = RATE / 10 };
    static int16_t pcm[N][FRAMES * 2];
    static WAVEHDR h[N];
    for (int b = 0; b < N; b++)
        for (int i = 0; i < FRAMES; i++) {
            double t = (double)(b * FRAMES + i) / RATE;
            pcm[b][2 * i] = pcm[b][2 * i + 1] = (int16_t)(3000 * sin(2 * 3.14159265 * 440 * t));
        }
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0};
    HWAVEOUT w;
    MMRESULT r = waveOutOpen(&w, WAVE_MAPPER, &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    CHECK(r == MMSYSERR_NOERROR, "waveOutOpen: %u", r);
    if (r) { printf("waveout_test: %d passed, %d failed\n", g_pass, g_fail); return 1; }
    waveOutPause(w);
    for (int b = 0; b < N; b++) {
        h[b].lpData = (LPSTR)pcm[b];
        h[b].dwBufferLength = FRAMES * 4;
        CHECK(waveOutPrepareHeader(w, &h[b], sizeof h[b]) == MMSYSERR_NOERROR && waveOutWrite(w, &h[b], sizeof h[b]) == MMSYSERR_NOERROR, "queue %d", b);
    }
    CHECK(waveOutUnprepareHeader(w, &h[0], sizeof h[0]) == WAVERR_STILLPLAYING, "a queued header cannot be unprepared");
    Sleep(200);
    CHECK(!(h[0].dwFlags & WHDR_DONE), "paused: nothing taken");
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    waveOutRestart(w);
    int done = 0, in_order = 1, events = 0;
    while (done < N) {
        if (WaitForSingleObject(ev, 3000) != WAIT_OBJECT_0) break;
        events++;
        while (done < N && (h[done].dwFlags & WHDR_DONE)) done++;
        for (int b = done; b < N; b++) if (h[b].dwFlags & WHDR_DONE) in_order = 0;
    }
    QueryPerformanceCounter(&t1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
    CHECK(done == N && in_order, "all %d buffers done in order (got %d)", N, done);
    CHECK(events >= 2, "the event was set (%d times)", events);
    CHECK(secs > 0.7 && secs < 1.6, "about one second of real time (%.3f s)", secs);
    Sleep(300);                                        /* let the device play out what it was handed */
    MMTIME mt = {TIME_SAMPLES};
    CHECK(waveOutGetPosition(w, &mt, sizeof mt) == MMSYSERR_NOERROR && mt.u.sample > RATE * 9 / 10 && mt.u.sample <= RATE, "position %u of %d frames", mt.u.sample, RATE);
    for (int b = 0; b < N; b++) waveOutUnprepareHeader(w, &h[b], sizeof h[b]);
    waveOutPause(w);
    for (int b = 0; b < 3; b++) { waveOutPrepareHeader(w, &h[b], sizeof h[b]); waveOutWrite(w, &h[b], sizeof h[b]); }
    waveOutReset(w);
    CHECK((h[0].dwFlags & WHDR_DONE) && (h[2].dwFlags & WHDR_DONE) && !(h[2].dwFlags & WHDR_INQUEUE), "reset releases the queue");
    mt.wType = TIME_SAMPLES;
    waveOutGetPosition(w, &mt, sizeof mt);
    CHECK(mt.u.sample == 0, "reset: position 0 (%u)", mt.u.sample);
    CHECK(waveOutClose(w) == MMSYSERR_NOERROR, "close");
    printf("waveout_test: %d passed, %d failed (%.3f s for one second of sound)\n", g_pass, g_fail, secs);
    return g_fail != 0;
}
