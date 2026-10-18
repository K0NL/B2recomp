/* [Recomps#1] Linux only: GetProcessMemoryInfo for the runtime's memory lines (hitch.c, nvn_vk_m4.c), from /proc/self/status. */
#ifndef RECOMPS_LINUX_PSAPI_H
#define RECOMPS_LINUX_PSAPI_H
#include <windows.h>
typedef struct {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
           QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX;
typedef PROCESS_MEMORY_COUNTERS_EX PROCESS_MEMORY_COUNTERS;
BOOL GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS *pmc, DWORD cb);
#define K32GetProcessMemoryInfo GetProcessMemoryInfo   /* RSS = working set, data = private */
#endif
