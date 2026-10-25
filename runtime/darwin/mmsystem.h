/* [Recomps#1] Darwin only: the waveOut subset nnaudio.c uses, on an SDL3 audio stream (waveout_sdl.c; notes/macos-port-plan.md
 * step 5). Same contract as Windows: headers are queued in order, each is marked WHDR_DONE when the output has taken all of
 * it and the CALLBACK_EVENT event is set; the position counts the sample frames taken since the last reset. */
#ifndef RECOMPS_DARWIN_MMSYSTEM_H
#define RECOMPS_DARWIN_MMSYSTEM_H
#include <windows.h>
typedef struct WAVEHDR {
    LPSTR lpData;
    DWORD dwBufferLength, dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD dwFlags, dwLoops;
    struct WAVEHDR *lpNext;
    DWORD_PTR reserved;
} WAVEHDR, *LPWAVEHDR;
#define WHDR_DONE 0x1u
#define WHDR_PREPARED 0x2u
#define WHDR_INQUEUE 0x10u
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
#define WAVE_FORMAT_PCM 1
#define WAVE_MAPPER ((UINT)-1)
#define CALLBACK_EVENT 0x50000u
typedef struct { UINT wType; union { DWORD ms, sample, cb, ticks; } u; } MMTIME;
#define TIME_MS 0x1u
#define TIME_SAMPLES 0x2u
#define TIME_BYTES 0x4u
#define MMSYSERR_NOERROR 0u
#define MMSYSERR_ERROR 1u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_NODRIVER 6u
#define WAVERR_STILLPLAYING 33u
#define WAVERR_UNPREPARED 34u
typedef struct WaveOut *HWAVEOUT;
MMRESULT waveOutOpen(HWAVEOUT *out, UINT dev, const WAVEFORMATEX *f, DWORD_PTR callback, DWORD_PTR inst, DWORD flags);
MMRESULT waveOutClose(HWAVEOUT w);
MMRESULT waveOutPrepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size);
MMRESULT waveOutUnprepareHeader(HWAVEOUT w, WAVEHDR *h, UINT size);
MMRESULT waveOutWrite(HWAVEOUT w, WAVEHDR *h, UINT size);
MMRESULT waveOutPause(HWAVEOUT w);
MMRESULT waveOutRestart(HWAVEOUT w);
MMRESULT waveOutReset(HWAVEOUT w);
MMRESULT waveOutGetPosition(HWAVEOUT w, MMTIME *t, UINT size);
MMRESULT waveOutSetVolume(HWAVEOUT w, DWORD volume);
#endif
