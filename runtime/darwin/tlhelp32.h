/* [Recomps#1] Darwin only: thread snapshots for the runtime's diagnostics (hang sampler, write-watch test tool).
 * Not available on Linux yet (plan step 4): the snapshot fails, so those tools report that they cannot run. */
#ifndef RECOMPS_DARWIN_TLHELP32_H
#define RECOMPS_DARWIN_TLHELP32_H
#include <windows.h>
#define TH32CS_SNAPTHREAD 0x4u
typedef struct { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; } THREADENTRY32;
static inline HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid) { (void)flags; (void)pid; return INVALID_HANDLE_VALUE; }
static inline BOOL Thread32First(HANDLE s, THREADENTRY32 *e) { (void)s; (void)e; return FALSE; }
static inline BOOL Thread32Next(HANDLE s, THREADENTRY32 *e) { (void)s; (void)e; return FALSE; }
#define TH32CS_SNAPMODULE 0x8u
typedef struct { DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage; BYTE *modBaseAddr; DWORD modBaseSize;
                 HMODULE hModule; char szModule[256], szExePath[MAX_PATH]; } MODULEENTRY32;
static inline BOOL Module32First(HANDLE s, MODULEENTRY32 *e) { (void)s; (void)e; return FALSE; }
static inline BOOL Module32Next(HANDLE s, MODULEENTRY32 *e) { (void)s; (void)e; return FALSE; }
#endif
