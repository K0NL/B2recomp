/* nn::time on the host clock.
 * PosixTime { int64 seconds }. CalendarTime { int16 year; int8 month, day, hour, minute, second; }.
 * CalendarAdditionalInfo { int32 dayOfWeek (0 = Sunday); int32 yearDay;
 *                          char zoneName[8]; bool isDst; int32 utcOffsetSeconds; }. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <shlwapi.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define RET(v) (c->x[0] = (uint64_t)(v))

static int64_t now_posix(void) { return (int64_t)_time64(NULL); }

static void write_calendar(uint64_t out, const struct tm *t) {
    wr16(out, (uint16_t)(t->tm_year + 1900));
    wr8(out + 2, (uint8_t)(t->tm_mon + 1));
    wr8(out + 3, (uint8_t)t->tm_mday);
    wr8(out + 4, (uint8_t)t->tm_hour);
    wr8(out + 5, (uint8_t)t->tm_min);
    wr8(out + 6, (uint8_t)t->tm_sec);
    wr8(out + 7, 0);
}
static void read_calendar(uint64_t in, struct tm *t) {
    memset(t, 0, sizeof *t);
    t->tm_year = (int16_t)rd16(in) - 1900;
    t->tm_mon = (int8_t)rd8(in + 2) - 1;
    t->tm_mday = (int8_t)rd8(in + 3);
    t->tm_hour = (int8_t)rd8(in + 4);
    t->tm_min = (int8_t)rd8(in + 5);
    t->tm_sec = (int8_t)rd8(in + 6);
    t->tm_isdst = -1;
}

void host_clock_caller(const char *what, uint64_t lr);       /* nnsdk.c: SSBU_TICK_CALLERS */
int host_fixed_clock(uint64_t lr);                            /* nnsdk.c: SSBU_FIXED_SEED */
static void time_GetCurrentTime(Ctx *c) { host_clock_caller("GetCurrentTime", c->x[30]); wr64(A0, host_fixed_clock(c->x[30]) ? 1700000000ULL : (uint64_t)now_posix()); RET(0); }
/* ------------------------------------------------------------------------------------------------------------------
 * Time zone rule (close4, 2026-10-09).
 * On the console: [sdk 0x391820] ToCalendarTime(out, info, posix) = ITimeZoneService slot +0x60 (ToCalendarTimeWithMyRule),
 * [0x391760] the form with a rule = slot +0x58, [0x391970] ToPosixTime = slot +0x70 with min(count, 2) results,
 * [0x391700] LoadTimeZoneRule = slot +0x40; each answers 0x74 when nn::time is not initialised. The time module (not in
 * the dump) keeps "my rule": the TZif file of the device's location, from the TimeZoneBinary system archive, parsed into
 * the 0x4000-byte rule below (suyu externals/tz/tz/tz.h Rule; glue/time/time_zone.cpp; psc/time/time_zone.cpp), and
 * converts with the public tz code (localsub). The archive is not in ..\sysdata, so the rule is BUILT here, in the same
 * format, from what Windows knows about the zone: registry "Time Zones\<key>" TZI + "Dynamic DST" year entries (the data
 * behind GetDynamicTimeZoneInformation), one transition list for 1970..2037 as a compiled zone file has. Windows has no
 * abbreviations and only a short history (measured, tz_vs_tzdb.py: W. Europe equals the tz database from 1996 on, not
 * before), so when the zone's file of the public tz database is found (%SSBU_TZDIR%, ..\sysdata\zoneinfo,
 * C:\msys64\usr\share\zoneinfo) its own transitions, types and names are the rule's past (what the console's archive is
 * compiled from) and Windows' transitions continue it after the file's last explicit one; the names of Windows' types
 * come from the file (same offset) or its footer, e.g. "CET-1CEST,M3.5.0,M10.5.0/3". Without the file: Windows' data
 * alone and the tz database's numeric name form ("+01", "+0530"). */
typedef struct { int32_t utoff; uint8_t isdst, p0[3]; int32_t desigidx; uint8_t ttisstd, ttisut, p1[2]; } TzType;
typedef struct {
    int32_t timecnt, typecnt, charcnt; uint8_t goback, goahead, pad0[2];
    int64_t ats[1000]; uint8_t types[1000]; TzType ttis[128]; char chars[512]; int32_t defaulttype; uint8_t pad1[0x12c4];
} TzRule;
_Static_assert(sizeof(TzRule) == 0x4000 && sizeof(TzType) == 0x10, "time zone rule layout");
typedef struct { int year, mon, mday, hour, min, sec, wday, yday, isdst, utoff; char zone[16]; } TzTm;
typedef struct { LONG Bias, StandardBias, DaylightBias; SYSTEMTIME StandardDate, DaylightDate; } RegTzi;
typedef struct { RegTzi base; int first, last; RegTzi dyn[160]; uint8_t have[160]; } WinZone;
#define TZ_OVERFLOW 0x19274u            /* module 116, description 201 */
#define TZ_OUT_OF_RANGE 0x70c74u        /* 902 */
#define TZ_NOT_FOUND 0x7ba74u           /* 989 */

static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}
static int tz_split(int64_t t, int32_t utoff, TzTm *o) {                   /* tz timesub: UTC seconds + offset -> fields */
    if (t > 67767976233532799LL || t < -67768040609740800LL) return -1;       /* the year would not fit 32 bits */
    int64_t local = t + utoff, days = local / 86400, rem = local % 86400, y;
    if (rem < 0) { rem += 86400; days--; }
    civil_from_days(days, &y, &o->mon, &o->mday);
    o->year = (int)y;
    o->hour = (int)(rem / 3600); o->min = (int)(rem / 60 % 60); o->sec = (int)(rem % 60);
    o->wday = (int)(((days % 7) + 11) % 7);                                   /* 1970-01-01 was a Thursday */
    o->yday = (int)(days - days_from_civil(y, 1, 1));
    o->utoff = utoff;
    return 0;
}
static int tz_local(const TzRule *r, int64_t t, TzTm *o) {                 /* tz localsub (suyu externals/tz/tz/tz.cpp line 525..) */
    int i;
    if (r->timecnt > 0 && ((r->goback && t < r->ats[0]) || (r->goahead && t > r->ats[r->timecnt - 1]))) {
        const int64_t repeat = 146097LL * 86400;
        int64_t seconds = (t < r->ats[0] ? r->ats[0] - t : t - r->ats[r->timecnt - 1]) - 1;
        int64_t years = seconds / repeat * 400, newt;
        seconds = years * (repeat / 400);
        years += 400;
        newt = t < r->ats[0] ? t + seconds + repeat : t - seconds - repeat;
        if (newt < r->ats[0] || newt > r->ats[r->timecnt - 1]) return -1;
        if (tz_local(r, newt, o)) return -1;
        int64_t newy = (int64_t)o->year + (t < r->ats[0] ? -years : years);
        if (newy < INT32_MIN || newy > INT32_MAX) return -1;
        o->year = (int)newy;
        return 0;
    }
    if (r->timecnt == 0 || t < r->ats[0]) i = r->defaulttype;
    else {
        int lo = 1, hi = r->timecnt;
        while (lo < hi) { int mid = (lo + hi) >> 1; if (t < r->ats[mid]) hi = mid; else lo = mid + 1; }
        i = r->types[lo - 1];
    }
    if (i < 0 || i >= 128) return -1;
    if (tz_split(t, r->ttis[i].utoff, o)) return -1;
    o->isdst = r->ttis[i].isdst;
    memset(o->zone, 0, sizeof o->zone);
    if (r->ttis[i].desigidx >= 0 && r->ttis[i].desigidx < (int)sizeof r->chars) strncpy(o->zone, r->chars + r->ttis[i].desigidx, sizeof o->zone - 1);
    return 0;
}
static int tz_validate(const TzRule *r) {                                   /* suyu psc/time/time_zone.cpp ValidateRule */
    if (r->typecnt > 128 || r->timecnt > 1000 || r->charcnt > 50 || r->typecnt < 0 || r->timecnt < 0) return -1;
    for (int i = 0; i < r->timecnt; i++) if (r->types[i] >= r->typecnt) return -1;
    for (int i = 0; i < r->typecnt; i++) if (r->ttis[i].desigidx >= (int)sizeof r->chars) return -1;
    return 0;
}

static int winzone_load(const wchar_t *key, WinZone *z) {
    wchar_t path[256];
    DWORD size = sizeof z->base, type = 0, v = 0;
    memset(z, 0, sizeof *z);
    _snwprintf(path, 255, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\%ls", key); path[255] = 0;
    if (SHGetValueW(HKEY_LOCAL_MACHINE, path, L"TZI", &type, &z->base, &size) != ERROR_SUCCESS || size != sizeof z->base) return -1;
    wcscat(path, L"\\Dynamic DST");
    size = sizeof v;
    if (SHGetValueW(HKEY_LOCAL_MACHINE, path, L"FirstEntry", &type, &v, &size) != ERROR_SUCCESS) return 0;
    z->first = (int)v;
    size = sizeof v;
    if (SHGetValueW(HKEY_LOCAL_MACHINE, path, L"LastEntry", &type, &v, &size) != ERROR_SUCCESS) { z->first = 0; return 0; }
    z->last = (int)v;
    if (z->last < z->first || z->last - z->first >= 160) { z->first = z->last = 0; return 0; }
    for (int y = z->first; y <= z->last; y++) {
        wchar_t name[16];
        _snwprintf(name, 15, L"%d", y);
        size = sizeof z->base;
        z->have[y - z->first] = SHGetValueW(HKEY_LOCAL_MACHINE, path, name, &type, &z->dyn[y - z->first], &size) == ERROR_SUCCESS && size == sizeof z->base;
    }
    return 0;
}
static const RegTzi *winzone_year(const WinZone *z, int year) {             /* a year outside the entries uses the nearest one */
    if (!z->first) return &z->base;
    int y = year < z->first ? z->first : year > z->last ? z->last : year;
    return z->have[y - z->first] ? &z->dyn[y - z->first] : &z->base;
}
static int winzone_yearstart(const SYSTEMTIME *st) {                       /* Windows' form of "at the start of the year": 1 January 00:00:00.000, any weekday */
    return !st->wYear && st->wMonth == 1 && st->wDay == 1 && !st->wHour && !st->wMinute && !st->wSecond && !st->wMilliseconds;
}
static int winzone_date(const SYSTEMTIME *st, int year, int64_t *local) {  /* local wall-clock second of a TZI transition date */
    static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int d;
    if (st->wMonth < 1 || st->wMonth > 12) return 0;
    if (st->wYear) { if (st->wYear != year) return 0; d = st->wDay; }      /* absolute date */
    else if (winzone_yearstart(st)) d = 1;                                  /* (measured: Central Brazilian Standard Time before 2004) */
    else {                                                                  /* wDay-th wDayOfWeek of the month, 5 = last */
        int len = mdays[st->wMonth - 1] + (st->wMonth == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0));
        int64_t first = days_from_civil(year, st->wMonth, 1);
        int wd = (int)(((first % 7) + 11) % 7);
        d = 1 + (st->wDayOfWeek - wd + 7) % 7 + 7 * (st->wDay - 1);
        while (d > len) d -= 7;
    }
    *local = days_from_civil(year, st->wMonth, d) * 86400 + st->wHour * 3600 + st->wMinute * 60 + st->wSecond + (st->wMilliseconds >= 500);
    return 1;
}
static int tzrule_type(TzRule *r, int32_t utoff, int isdst) {
    for (int i = 0; i < r->typecnt; i++) if (r->ttis[i].utoff == utoff && r->ttis[i].isdst == isdst) return i;
    if (r->typecnt >= 128) return 0;
    r->ttis[r->typecnt].utoff = utoff; r->ttis[r->typecnt].isdst = (uint8_t)isdst;
    return r->typecnt++;
}
static void tzrule_at(TzRule *r, int64_t at, int type, int *cur) {
    if (type == *cur) return;
    if (r->timecnt && at <= r->ats[r->timecnt - 1]) {                       /* two changes at one instant: the later rule wins */
        r->types[r->timecnt - 1] = (uint8_t)type; *cur = type;
        if (r->timecnt >= 2 ? r->types[r->timecnt - 2] == type : r->defaulttype == type) r->timecnt--;
        return;
    }
    if (r->timecnt >= 1000) return;
    r->ats[r->timecnt] = at; r->types[r->timecnt++] = (uint8_t)type; *cur = type;
}
/* A zone file (TZif) of the public tz database: version 2+ body into the rule, exactly as stored (64-bit transition times,
 * type indices, {offset, isdst, name index} records, names), and the footer's names: std name, offset, optional dst name
 * and offset. Returns 0 when the file was found and read. */
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static int tzdb_load(const char *iana, TzRule *r, char *std_name, int32_t *std_off, char *dst_name, int32_t *dst_off) {
    const char *dirs[3] = {getenv("SSBU_TZDIR"), "..\\sysdata\\zoneinfo", "C:\\msys64\\usr\\share\\zoneinfo"};
    std_name[0] = dst_name[0] = 0;
    if (!iana || !iana[0] || strstr(iana, "..") || strchr(iana, ':') || iana[0] == '/' || iana[0] == '\\') return -1;
    for (int k = 0; k < 3; k++) {
        char path[MAX_PATH];
        static uint8_t buf[256 * 1024];
        if (!dirs[k] || !dirs[k][0]) continue;
        snprintf(path, sizeof path, "%s\\%s", dirs[k], iana);
        for (char *p = path; *p; p++) if (*p == '/') *p = '\\';
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        if (n < 44 * 2 || memcmp(buf, "TZif", 4) || buf[4] < '2' || buf[n - 1] != '\n') continue;
        /* skip the 32-bit block: counts are isut, isstd, leap, time, type, char */
        size_t v1 = (size_t)be32(buf + 32) * 5 + (size_t)be32(buf + 36) * 6 + be32(buf + 40) + (size_t)be32(buf + 28) * 8 + be32(buf + 24) + be32(buf + 20);
        const uint8_t *h = buf + 44 + v1;
        if (44 + v1 + 44 > n || memcmp(h, "TZif", 4)) continue;
        uint32_t isut = be32(h + 20), isstd = be32(h + 24), leap = be32(h + 28), timecnt = be32(h + 32), typecnt = be32(h + 36), charcnt = be32(h + 40);
        const uint8_t *p8 = h + 44;
        size_t body = (size_t)timecnt * 9 + (size_t)typecnt * 6 + charcnt + (size_t)leap * 12 + isstd + isut;
        if (timecnt > 1000 || typecnt == 0 || typecnt > 128 || charcnt > 50 || (size_t)(p8 - buf) + body + 2 > n) continue;
        memset(r, 0, sizeof *r);
        r->timecnt = (int32_t)timecnt; r->typecnt = (int32_t)typecnt; r->charcnt = (int32_t)charcnt;
        for (uint32_t i = 0; i < timecnt; i++) r->ats[i] = (int64_t)(((uint64_t)be32(p8 + i * 8) << 32) | be32(p8 + i * 8 + 4));
        p8 += (size_t)timecnt * 8;
        int ok = 1;
        for (uint32_t i = 0; i < timecnt; i++) { r->types[i] = p8[i]; if (p8[i] >= typecnt) ok = 0; }
        p8 += timecnt;
        for (uint32_t i = 0; i < typecnt; i++, p8 += 6) {
            r->ttis[i].utoff = (int32_t)be32(p8); r->ttis[i].isdst = p8[4] != 0; r->ttis[i].desigidx = p8[5];
            if (p8[5] >= charcnt) ok = 0;
        }
        memcpy(r->chars, p8, charcnt);
        p8 += charcnt + (size_t)leap * 12;
        for (uint32_t i = 0; i < isstd && i < typecnt; i++) r->ttis[i].ttisstd = p8[i] != 0;
        p8 += isstd;
        for (uint32_t i = 0; i < isut && i < typecnt; i++) r->ttis[i].ttisut = p8[i] != 0;
        if (!ok) continue;
        {                                                                   /* tz code: the type for times before the first transition */
            int i;
            for (i = 0; i < r->timecnt; i++) if (r->types[i] == 0) break;
            i = i < r->timecnt ? -1 : 0;
            if (i < 0 && r->timecnt > 0 && r->ttis[r->types[0]].isdst) { i = r->types[0]; while (--i >= 0) if (!r->ttis[i].isdst) break; }
            if (i < 0) { i = 0; while (r->ttis[i].isdst) if (++i >= r->typecnt) { i = 0; break; } }
            r->defaulttype = i;
        }
        size_t e = n - 1, s = e;
        while (s > 0 && buf[s - 1] != '\n') s--;
        buf[e] = 0;
        const char *p = (const char *)buf + s;
        for (int which = 0; which < 2; which++) {
            char *name = which ? dst_name : std_name;
            int len = 0, sign = 1, v[3] = {0, 0, 0}, got = 0;
            if (*p == '<') { for (p++; *p && *p != '>' && len < 15; p++) name[len++] = *p; if (*p == '>') p++; }
            else while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) && len < 15) name[len++] = *p++;
            name[len] = 0;
            if (!len) break;
            if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
            for (int part = 0; part < 3 && *p >= '0' && *p <= '9'; part++) {
                while (*p >= '0' && *p <= '9') v[part] = v[part] * 10 + (*p++ - '0');
                got = 1;
                if (*p != ':') break;
                p++;
            }
            int32_t off = -sign * (v[0] * 3600 + v[1] * 60 + v[2]);        /* POSIX counts west of Greenwich as positive */
            if (which == 0) { if (!got) { std_name[0] = 0; break; } *std_off = off; }
            else *dst_off = got ? off : *std_off + 3600;
            if (*p == ',' || !*p) break;
        }
        return 0;
    }
    return -1;
}
static int tzrule_name(TzRule *r, const char *name) {                       /* index of a name in the rule's name block */
    for (int p = 0; p < r->charcnt; p += (int)strlen(r->chars + p) + 1) if (!strcmp(r->chars + p, name)) return p;
    if (r->charcnt + (int)strlen(name) + 1 > 50) return 0;
    int at = r->charcnt;
    strcpy(r->chars + at, name);
    r->charcnt += (int)strlen(name) + 1;
    return at;
}
/* w = the transitions Windows' data gives for 1970..2037 (types without names). */
static void winrule_build(TzRule *w, const WinZone *z, int fixed, LONG fixed_bias) {
    int cur = -1;
    memset(w, 0, sizeof *w);
    for (int year = 1970; year <= 2037; year++) {
        const RegTzi *t = winzone_year(z, year);
        int64_t to_dst = 0, to_std = 0;
        int32_t std_off = -(t->Bias + t->StandardBias) * 60, dst_off = -(t->Bias + t->DaylightBias) * 60;
        int both = !fixed && winzone_date(&t->DaylightDate, year, &to_dst) && winzone_date(&t->StandardDate, year, &to_std);
        if (fixed) std_off = -fixed_bias * 60;
        int std_type = tzrule_type(w, std_off, 0), dst_type = both ? tzrule_type(w, dst_off, 1) : std_type;
        to_dst -= std_off; to_std -= dst_off;                               /* the dates are in the wall clock before the change */
        int start = both && to_std < to_dst ? dst_type : std_type;          /* southern pattern: the year begins in daylight time */
        if (cur >= 0 && both && winzone_yearstart(&t->StandardDate)) start = cur;   /* not for "standard time from 1 January 00:00": the year begins as the last one ended
                                                                               and that date is a change at 00:00 local time (Windows' conversion, measured: Alberta 2026 stays in standard
                                                                               time, Central Brazilian 1971..2003 keeps daylight time until 00:00 local) */
        if (cur < 0) { w->defaulttype = cur = start; }
        else if (cur != start) tzrule_at(w, days_from_civil(year, 1, 1) * 86400, start, &cur);       /* Windows changes the year's entry at 00:00 UTC (measured) */
        if (!both) continue;
        if (to_dst < to_std) { tzrule_at(w, to_dst, dst_type, &cur); tzrule_at(w, to_std, std_type, &cur); }
        else { tzrule_at(w, to_std, std_type, &cur); tzrule_at(w, to_dst, dst_type, &cur); }
    }
}
/* The rule of a location. With the zone's tz database file: the file's own transitions (the zone's history, as the
 * console's archive has it), continued after the file's last explicit transition by the transitions Windows' data gives
 * (a compiled zone file continues with the rule in its footer; Windows' current entry is that rule, kept up to date by
 * Windows Update). Without the file (iana NULL or not found): Windows' data for all of 1970..2037, numeric names. */
static void tzrule_build(TzRule *r, const WinZone *z, const char *iana, int fixed, LONG fixed_bias) {
    static TzRule w;
    static SRWLOCK lock = SRWLOCK_INIT;                                     /* w and the file buffer are shared */
    AcquireSRWLockExclusive(&lock);
    char std_name[16], dst_name[16];
    int32_t n_std = 0, n_dst = 0;
    int map[128], cur, have_file = iana && tzdb_load(iana, r, std_name, &n_std, dst_name, &n_dst) == 0;
    int64_t last = INT64_MIN;
    winrule_build(&w, z, fixed, fixed_bias);
    if (!have_file) { memset(r, 0, sizeof *r); std_name[0] = dst_name[0] = 0; cur = -1; }
    else { cur = r->timecnt ? r->types[r->timecnt - 1] : r->defaulttype; if (r->timecnt) last = r->ats[r->timecnt - 1]; }
    for (int i = 0; i < w.typecnt; i++) {                                   /* Windows' types in the rule: the newest file type with the same offset, else a new one */
        char name[16];
        int32_t off = w.ttis[i].utoff, a = off < 0 ? -off : off;
        map[i] = -1;
        for (int k = r->typecnt - 1; k >= 0 && map[i] < 0; k--) if (r->ttis[k].utoff == off && r->ttis[k].isdst == w.ttis[i].isdst) map[i] = k;
        if (map[i] >= 0 || r->typecnt >= 128) { if (map[i] < 0) map[i] = 0; continue; }
        if (!w.ttis[i].isdst && std_name[0] && off == n_std) strcpy(name, std_name);
        else if (w.ttis[i].isdst && dst_name[0] && off == n_dst) strcpy(name, dst_name);
        else if (a % 3600 == 0) snprintf(name, sizeof name, "%c%02d", off < 0 ? '-' : '+', a / 3600);
        else snprintf(name, sizeof name, "%c%02d%02d", off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
        map[i] = r->typecnt;
        r->ttis[r->typecnt].utoff = off; r->ttis[r->typecnt].isdst = w.ttis[i].isdst; r->ttis[r->typecnt].desigidx = tzrule_name(r, name);
        r->typecnt++;
    }
    if (cur < 0) r->defaulttype = cur = map[w.defaulttype];
    for (int k = 0; k < w.timecnt; k++) {
        int type = map[w.types[k]];
        if (w.ats[k] <= last) continue;
        if (type == cur || r->timecnt >= 1000) continue;
        r->ats[r->timecnt] = w.ats[k]; r->types[r->timecnt++] = (uint8_t)type; cur = type;
    }
    ReleaseSRWLockExclusive(&lock);
}
static void location_name(char out[0x24]);
static TzRule *my_rule(void) {                                              /* the rule of the Windows time zone, built once */
    static TzRule rule;
    static volatile LONG ready;
    if (InterlockedCompareExchange(&ready, 1, 0) == 0) {
        DYNAMIC_TIME_ZONE_INFORMATION d;
        static WinZone z;
        char iana[0x24];
        memset(&d, 0, sizeof d);
        GetDynamicTimeZoneInformation(&d);
        location_name(iana);
        if (winzone_load(d.TimeZoneKeyName, &z)) {                          /* a zone without a registry key: what the API gave */
            memset(&z, 0, sizeof z);
            z.base.Bias = d.Bias; z.base.StandardBias = d.StandardBias; z.base.DaylightBias = d.DaylightBias;
            z.base.StandardDate = d.StandardDate; z.base.DaylightDate = d.DaylightDate;
        }
        tzrule_build(&rule, &z, iana, d.DynamicDaylightTimeDisabled != 0, d.Bias + d.StandardBias);   /* "adjust for daylight saving time" off */
        ready = 2;
    }
    while (ready != 2) SwitchToThread();
    return &rule;
}
static int tzrule_for_location(TzRule *out, const char *iana) {            /* LoadTimeZoneRule: any location Windows has a zone for */
    char mine[0x24];
    location_name(mine);
    if (!strcmp(mine, iana)) { memcpy(out, my_rule(), sizeof *out); return 0; }
    HMODULE icu = LoadLibraryA("icu.dll");
    typedef int (__cdecl *ToWin)(const unsigned short *, int, unsigned short *, int, int *);
    ToWin to_win = icu ? (ToWin)GetProcAddress(icu, "ucal_getWindowsTimeZoneID") : NULL;
    unsigned short id[0x24], win[128];
    int err = 0, n = 0;
    static WinZone z;                                                       /* callers are serialised by the service's lock on the console; here by this lock */
    static SRWLOCK lock = SRWLOCK_INIT;
    if (!to_win) return -1;
    for (; iana[n] && n < 0x23; n++) id[n] = (unsigned char)iana[n];
    id[n] = 0;
    n = to_win(id, n, win, 127, &err);
    if (err > 0 || n <= 0) return -1;
    win[n] = 0;
    AcquireSRWLockExclusive(&lock);
    int bad = winzone_load((const wchar_t *)win, &z);
    if (!bad) tzrule_build(out, &z, iana, 0, 0);
    ReleaseSRWLockExclusive(&lock);
    return bad;
}
static void calendar_out(Ctx *c, const TzRule *r) {
    TzTm t;
    if (tz_validate(r)) { RET(TZ_OUT_OF_RANGE); return; }
    if (tz_local(r, (int64_t)rd64(A2), &t)) { RET(TZ_OVERFLOW); return; }
    if (A0) {
        wr16(A0, (uint16_t)(int16_t)t.year); wr8(A0 + 2, (uint8_t)t.mon); wr8(A0 + 3, (uint8_t)t.mday);
        wr8(A0 + 4, (uint8_t)t.hour); wr8(A0 + 5, (uint8_t)t.min); wr8(A0 + 6, (uint8_t)t.sec); wr8(A0 + 7, 0);
    }
    if (A1) {                                                               /* [sdk 0x3918a8..0x3918c0] the four parts the SDK copies out */
        wr32(A1, (uint32_t)t.wday);
        wr32(A1 + 4, (uint32_t)t.yday);
        memcpy((void *)(uintptr_t)(A1 + 8), t.zone, 8);
        wr8(A1 + 15, 0);
        wr32(A1 + 16, 0);
        wr8(A1 + 16, (uint8_t)(t.isdst != 0));
        wr32(A1 + 20, (uint32_t)t.utoff);
    }
    RET(0);
}
static void time_ToCalendarTime(Ctx *c) { calendar_out(c, my_rule()); }    /* (CalendarTime *out, CalendarAdditionalInfo *info, const PosixTime &t) */
static void time_ToCalendarTimeRule(Ctx *c) { calendar_out(c, (const TzRule *)(uintptr_t)A3); }   /* ... , const TimeZoneRule &) */
static void time_LoadTimeZoneRule(Ctx *c) {                                 /* (TimeZoneRule *out, const LocationName &) */
    char name[0x25];
    memcpy(name, (void *)(uintptr_t)A1, 0x24); name[0x24] = 0;
    RET(tzrule_for_location((TzRule *)(uintptr_t)A0, name) ? TZ_NOT_FOUND : 0);
}
/* All instants whose local time is the calendar time: none inside a spring-forward gap, two in a fall-back hour (ascending). */
static int tz_to_posix(const TzRule *r, int year, int mon, int mday, int hour, int min, int sec, int64_t out[2]) {
    int n = 0;
    if (mon < 1 || mon > 12) return 0;
    int64_t utc = days_from_civil(year, mon, mday) * 86400 + hour * 3600 + min * 60 + sec;
    for (int i = 0; i < r->typecnt && n < 2; i++) {
        int64_t t = utc - r->ttis[i].utoff;
        TzTm b;
        if (n && out[0] == t) continue;
        if (tz_local(r, t, &b) || b.year != year || b.mon != mon || b.mday != mday || b.hour != hour || b.min != min || b.sec != sec) continue;
        out[n++] = t;
    }
    if (n == 2 && out[0] > out[1]) { int64_t s = out[0]; out[0] = out[1]; out[1] = s; }
    return n;
}
static void time_ToCalendarTimeInUtc(Ctx *c) {            /* returns CalendarTime (8 bytes) in x0 */
    __time64_t t = (__time64_t)rd64(A0);
    struct tm utc;
    uint64_t packed = 0;
    if (!_gmtime64_s(&utc, &t)) write_calendar((uint64_t)(uintptr_t)&packed, &utc);
    RET(packed);
}
static void time_ToPosixTimeFromUtc(Ctx *c) { struct tm t; read_calendar(A0, &t); t.tm_isdst = 0; RET((int64_t)_mkgmtime64(&t)); }
static void time_ToPosixTime(Ctx *c) {                    /* (int *outCount, PosixTime *out, int count, const CalendarTime &) */
    int64_t v[2];                                         /* [sdk 0x391970]: at most min(count, 2) results; the module answers 0 for a time that does not exist */
    const TzRule *r = my_rule();
    int max = (int32_t)A2 < 2 ? (int32_t)A2 : 2;
    int n = tz_to_posix(r, (int16_t)rd16(A3), (int8_t)rd8(A3 + 2), (int8_t)rd8(A3 + 3), (int8_t)rd8(A3 + 4), (int8_t)rd8(A3 + 5), (int8_t)rd8(A3 + 6), v);
    if (n > max) n = max < 0 ? 0 : max;
    for (int i = 0; i < n; i++) wr64(A1 + (uint64_t)i * 8, (uint64_t)v[i]);
    if (A0) wr32(A0, (uint32_t)n);
    RET(0);
}
static int days_in_month(int y, int m) {
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}
static void time_IsValidDate(Ctx *c) {
    int y = (int32_t)A0, m = (int32_t)A1, d = (int32_t)A2;
    RET(y >= 1 && m >= 1 && m <= 12 && d >= 1 && d <= days_in_month(y, m));
}
static void time_GetDayOfWeek(Ctx *c) {                   /* Sakamoto's method; 0 = Sunday */
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = (int32_t)A0, m = (int32_t)A1, d = (int32_t)A2;
    if (m < 1 || m > 12) { RET(0); return; }
    if (m < 3) y -= 1;
    RET((uint64_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7));
}
static void time_CalendarGreater(Ctx *c) {                /* operator>(const CalendarTime &, const CalendarTime &) */
    struct tm a, b;
    read_calendar(A0, &a);
    read_calendar(A1, &b);
    int ka[6] = {a.tm_year, a.tm_mon, a.tm_mday, a.tm_hour, a.tm_min, a.tm_sec};
    int kb[6] = {b.tm_year, b.tm_mon, b.tm_mday, b.tm_hour, b.tm_min, b.tm_sec};
    for (int i = 0; i < 6; i++) if (ka[i] != kb[i]) { RET(ka[i] > kb[i]); return; }
    RET(0);
}
static void time_SteadyNow(Ctx *c) {                      /* nanoseconds on a monotonic clock */
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    RET((uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart));
}
/* nn::time::ClockSnapshot (0xd0 bytes). The constructor is the SDK's own code (lifted, [sdk 0x38fc00]). The contents are
 * produced by the time system module ([sdk 0x38fce0] CreateWithStandardSystemClock = IStaticService::GetClockSnapshot(out,
 * type 1); [sdk 0x38f030] CalculateSpanBetween = IStaticService::CalculateSpanBetween), which is not in the dump: both are
 * re-implemented here after the reference implementation (suyu psc/time/static.cpp GetClockSnapshotImpl / CalculateSpanBetween,
 * layout psc/time/common.h, which agrees with the offsets the SDK constructor writes):
 *   +0x00 user SystemClockContext { s64 offset; SteadyClockTimePoint { s64 seconds; u8 sourceId[16] } }   +0x20 network context
 *   +0x40 user time (posix)   +0x48 network time   +0x50 user CalendarTime   +0x58 network CalendarTime
 *   +0x60 user CalendarAdditionalInfo (0x18)   +0x78 network CalendarAdditionalInfo
 *   +0x90 SteadyClockTimePoint now   +0xa8 LocationName (0x24)   +0xcc automatic correction enabled   +0xcd type
 * Host sources: the steady clock is the time since Windows started (GetTickCount64) with a source id made from the boot
 * time, so two snapshots of one Windows session compare by the steady clock, as on the console between RTC resets; the
 * system clocks are the Windows clock; the location name is the IANA name of the Windows time zone (ICU, Windows 10 1903+;
 * "UTC" if ICU is absent); automatic correction = Windows time synchronisation is on (W32Time Type != NoSync).
 * NOT VERIFIED against a console: the time point stored inside the two contexts (the moment the clock was last set; 0 here). */
static void steady_now(int64_t *seconds, uint8_t id[16]) {
    static uint8_t source[16];
    static volatile LONG ready;
    if (InterlockedCompareExchange(&ready, 1, 0) == 0) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t boot = ((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000 - GetTickCount64()) / 60000;   /* boot time, to the minute */
        uint64_t h = 0xcbf29ce484222325ull;
        for (int i = 0; i < 16; i++) { h = (h ^ ((boot >> ((i & 7) * 8)) & 0xff) ^ (uint64_t)i) * 0x100000001b3ull; source[i] = (uint8_t)(h >> 32); }
        source[6] = (uint8_t)((source[6] & 0x0f) | 0x40); source[8] = (uint8_t)((source[8] & 0x3f) | 0x80);          /* a version 4 UUID */
        ready = 2;
    }
    while (ready != 2) SwitchToThread();
    *seconds = (int64_t)(GetTickCount64() / 1000);
    memcpy(id, source, 16);
}
static void location_name(char out[0x24]) {
    static char name[0x24];
    static volatile LONG ready;
    if (InterlockedCompareExchange(&ready, 1, 0) == 0) {
        strcpy(name, "UTC");
        HMODULE icu = LoadLibraryA("icu.dll");
        typedef int (__cdecl *GetTz)(unsigned short *, int, int *);
        GetTz get = icu ? (GetTz)GetProcAddress(icu, "ucal_getDefaultTimeZone") : NULL;
        if (get) {
            unsigned short w[64];
            int err = 0, n = get(w, 64, &err);
            if (err <= 0 && n > 0 && n < 0x24) { for (int i = 0; i < n; i++) name[i] = (char)w[i]; name[n] = 0; }
        }
        ready = 2;
    }
    while (ready != 2) SwitchToThread();
    memset(out, 0, 0x24);
    strcpy(out, name);
}
static int auto_correction(void) {
    char type[32] = "";
    DWORD size = sizeof type;
    if (SHGetValueA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\W32Time\\Parameters", "Type", NULL, type, &size) != ERROR_SUCCESS) return 1;
    return _stricmp(type, "NoSync") != 0;
}
static void time_ToCalendarTime(Ctx *c);
static void time_Snapshot_create(Ctx *c) {                /* (ClockSnapshot *out) */
    uint64_t s = A0;
    int64_t now = now_posix(), tp;
    uint8_t id[16];
    steady_now(&tp, id);
    memset((void *)(uintptr_t)s, 0, 0xd0);
    for (int k = 0; k < 2; k++) {                          /* user and network clock: both are the Windows clock */
        uint64_t ctx = s + (uint64_t)k * 0x20;
        wr64(ctx, (uint64_t)(now - tp));
        wr64(ctx + 8, 0);
        memcpy((void *)(uintptr_t)(ctx + 0x10), id, 16);
        wr64(s + 0x40 + (uint64_t)k * 8, (uint64_t)now);
        c->x[0] = s + 0x50 + (uint64_t)k * 8; c->x[1] = s + 0x60 + (uint64_t)k * 0x18; c->x[2] = s + 0x40 + (uint64_t)k * 8;
        time_ToCalendarTime(c);
    }
    wr64(s + 0x90, (uint64_t)tp);
    memcpy((void *)(uintptr_t)(s + 0x98), id, 16);
    location_name((char *)(uintptr_t)(s + 0xa8));
    wr8(s + 0xcc, (uint8_t)auto_correction());
    wr8(s + 0xcd, 1);                                      /* ClockSnapshotType of this call ([sdk 0x38fd24] w2 = 1) */
    c->x[0] = s;
}
static void time_CalculateSpanBetween(Ctx *c) {           /* (TimeSpan *out, const ClockSnapshot &a, const ClockSnapshot &b) -> bool */
    uint64_t out = A0, a = A1, b = A2;
    int64_t seconds;
    if (memcmp((void *)(uintptr_t)(a + 0x98), (void *)(uintptr_t)(b + 0x98), 16) == 0) seconds = (int64_t)rd64(b + 0x90) - (int64_t)rd64(a + 0x90);
    else if (rd64(a + 0x48) && rd64(b + 0x48)) seconds = (int64_t)rd64(b + 0x48) - (int64_t)rd64(a + 0x48);      /* different steady clock sources: the network clocks */
    else { RET(0); return; }
    wr64(out, (uint64_t)(seconds * 1000000000LL));
    RET(1);
}
static void ret0(Ctx *c) { RET(0); }
static void ret1(Ctx *c) { RET(1); }

#define T(n) "_ZN2nn4time" n
static const ImportDef time_imports[] = {
    {T("13IsInitializedEv"), ret1}, {T("10InitializeEv"), ret0}, {T("8FinalizeEv"), ret0},
    {T("23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE"), time_GetCurrentTime},
    {T("26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE"), time_GetCurrentTime},
    {T("14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE"), time_ToCalendarTime},
    {T("14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE"), time_ToCalendarTimeRule},
    {T("19ToCalendarTimeInUtcERKNS0_9PosixTimeE"), time_ToCalendarTimeInUtc},
    {T("18ToPosixTimeFromUtcERKNS0_12CalendarTimeE"), time_ToPosixTimeFromUtc},
    {T("11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE"), time_ToPosixTime},
    {T("11IsValidDateEiii"), time_IsValidDate}, {T("12GetDayOfWeekEiii"), time_GetDayOfWeek},
    {T("gtERKNS0_12CalendarTimeES3_"), time_CalendarGreater},
    {T("19StandardSteadyClock3nowEv"), time_SteadyNow},
    {T("13ClockSnapshot29CreateWithStandardSystemClockEPS1_"), time_Snapshot_create},
    {T("20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_"), time_CalculateSpanBetween},
    {T("16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE"), time_LoadTimeZoneRule},
};

void register_time_imports(void) {
    register_imports(time_imports, (int)(sizeof time_imports / sizeof time_imports[0]));
}

/* Self-test (SSBU_SDK_SELFTEST): the rule built from the registry against Windows' own conversion
 * (SystemTimeToTzSpecificLocalTimeEx with the zone's dynamic information) for EVERY Windows time zone: every half hour of
 * the current year, every 2 h 59 min of 1990..2037; then the handlers on the PC's own zone. */
#define GP(p) ((uint64_t)(uintptr_t)(p))
int nntime_selftest(Ctx *c) {
    typedef BOOL (WINAPI *ToLocal)(const DYNAMIC_TIME_ZONE_INFORMATION *, const SYSTEMTIME *, SYSTEMTIME *);
    typedef DWORD (WINAPI *EnumTz)(DWORD, DYNAMIC_TIME_ZONE_INFORMATION *);
    ToLocal to_local = (ToLocal)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SystemTimeToTzSpecificLocalTimeEx");
    EnumTz enum_tz = (EnumTz)GetProcAddress(LoadLibraryA("advapi32.dll"), "EnumDynamicTimeZoneInformation");
    static TzRule r;
    static WinZone z;
    static uint8_t cal[8], info[0x18], cal2[8];
    static int64_t posix, back[2];
    static int32_t count;
    static char loc[0x24];
    int bad = 0, zones = 0, zones_now = 0, zones_other = 0;
    long long cases = 0, diff_now = 0, diff_other = 0, edge = 0;
    SYSTEMTIME st;
    GetSystemTime(&st);
    int this_year = st.wYear;
    int64_t y0 = days_from_civil(this_year, 1, 1) * 86400, y1 = days_from_civil(this_year + 1, 1, 1) * 86400;
    if (!to_local || !enum_tz) { printf("  time zone: Windows conversion functions not found\n"); return 1; }
    for (DWORD i = 0; ; i++) {
        DYNAMIC_TIME_ZONE_INFORMATION d;
        long long dn = 0, dother = 0;
        int64_t first_bad = 0;
        if (enum_tz(i, &d) != ERROR_SUCCESS) break;
        if (winzone_load(d.TimeZoneKeyName, &z)) { printf("  time zone: no registry data for %ls\n", d.TimeZoneKeyName); bad++; continue; }
        tzrule_build(&r, &z, NULL, 0, 0);
        zones++;
        for (int pass = 0; pass < 2; pass++) {
            int64_t from = pass ? days_from_civil(1990, 1, 1) * 86400 : y0, to = pass ? days_from_civil(2038, 1, 1) * 86400 : y1, step = pass ? 10740 : 1800;
            for (int64_t t = from; t < to; t += step) {
                TzTm u, l;
                SYSTEMTIME su, sl;
                tz_split(t, 0, &u);
                memset(&su, 0, sizeof su);
                su.wYear = (WORD)u.year; su.wMonth = (WORD)u.mon; su.wDay = (WORD)u.mday; su.wHour = (WORD)u.hour; su.wMinute = (WORD)u.min; su.wSecond = (WORD)u.sec;
                if (!to_local(&d, &su, &sl) || tz_local(&r, t, &l)) { bad++; break; }
                cases++;
                if (sl.wYear != l.year || sl.wMonth != l.mon || sl.wDay != l.mday || sl.wHour != l.hour || sl.wMinute != l.min || sl.wSecond != l.sec) {
                    int64_t into_year = t - days_from_civil(u.year, 1, 1) * 86400, to_next = days_from_civil(u.year + 1, 1, 1) * 86400 - t;
                    if (pass && (into_year < 86400 || to_next <= 86400)) { edge++; continue; }   /* within a day of 1 January: which year's entry Windows applies there */
                    if (pass) dother++; else dn++;
                    if (!first_bad) first_bad = t;
                }
            }
        }
        if (dn) zones_now++;
        if (dother) zones_other++;
        diff_now += dn; diff_other += dother;
        if ((dn || dother) && zones_now + zones_other <= 12) printf("  time zone: %ls: %lld differences in %d, %lld in 1990..2037 (first at posix %lld); %d transitions\n", d.TimeZoneKeyName, dn, this_year, dother, (long long)first_bad, r.timecnt);
    }
    printf("  time zone: %d Windows zones, %lld conversions compared with SystemTimeToTzSpecificLocalTimeEx: %lld different in %d (%d zones), %lld different in 1990..2037 (%d zones), plus %lld in 1990..2037 within one day of a 1 January (not counted: around a change of the registry entry, Windows and this rule can apply different years' entries for a few hours; the tz database agrees with either side, depending on the zone)\n",
           zones, cases, diff_now, this_year, zones_now, diff_other, zones_other, edge);
    bad += zones_now + zones_other;
    #define TCHECK(cond, text) do { if (!(cond)) { bad++; printf("  FAIL time zone: %s\n", text); } } while (0)
    /* the PC's own zone through the import handlers */
    const TzRule *mine = my_rule();
    long long own = 0, own_bad = 0, two = 0, none = 0;
    location_name(loc);
    if (getenv("SSBU_TZ_DUMP")) {                                   /* for tools_audit/patches/close4/tz_rule_check.py */
        FILE *f = fopen(getenv("SSBU_TZ_DUMP"), "w");
        if (f) {
            fprintf(f, "%s default %d %d %s\n", loc, mine->ttis[mine->defaulttype].utoff, mine->ttis[mine->defaulttype].isdst, mine->chars + mine->ttis[mine->defaulttype].desigidx);
            for (int k = 0; k < mine->timecnt; k++) fprintf(f, "%lld %d %d %s\n", (long long)mine->ats[k], mine->ttis[mine->types[k]].utoff, mine->ttis[mine->types[k]].isdst, mine->chars + mine->ttis[mine->types[k]].desigidx);
            fclose(f);
        }
    }
    for (int64_t t = y0; t < y1; t += 1800) {
        struct tm lt;
        __time64_t tt = (__time64_t)t;
        posix = t;
        c->x[0] = GP(cal); c->x[1] = GP(info); c->x[2] = GP(&posix);
        time_ToCalendarTime(c);
        own++;
        if (c->x[0] || _localtime64_s(&lt, &tt) || rd16(GP(cal)) != lt.tm_year + 1900 || cal[2] != lt.tm_mon + 1 || cal[3] != lt.tm_mday || cal[4] != lt.tm_hour || cal[5] != lt.tm_min || cal[6] != lt.tm_sec
            || (int)rd32(GP(info)) != lt.tm_wday || (int)rd32(GP(info) + 4) != lt.tm_yday || info[16] != (lt.tm_isdst > 0)) own_bad++;
        c->x[0] = GP(&count); c->x[1] = GP(back); c->x[2] = 2; c->x[3] = GP(cal);
        time_ToPosixTime(c);
        if (c->x[0] || count < 1 || (back[0] != t && !(count == 2 && back[1] == t))) own_bad++;
        if (count == 2) two++;
    }
    for (int k = 0; k < mine->timecnt; k++) {                       /* one minute after each spring-forward of the year: a time that does not exist */
        int before = k ? mine->types[k - 1] : mine->defaulttype;
        TzTm g;
        if (mine->ats[k] < y0 || mine->ats[k] >= y1 || mine->ttis[mine->types[k]].utoff <= mine->ttis[before].utoff) continue;
        tz_split(mine->ats[k] + 60, mine->ttis[before].utoff, &g);
        wr16(GP(cal2), (uint16_t)g.year); cal2[2] = (uint8_t)g.mon; cal2[3] = (uint8_t)g.mday; cal2[4] = (uint8_t)g.hour; cal2[5] = (uint8_t)g.min; cal2[6] = (uint8_t)g.sec;
        c->x[0] = GP(&count); c->x[1] = GP(back); c->x[2] = 2; c->x[3] = GP(cal2);
        time_ToPosixTime(c);
        TCHECK(c->x[0] == 0 && count == 0, "a calendar time inside the spring-forward gap must give 0 results");
        none++;
    }
    TCHECK(own_bad == 0, "ToCalendarTime / ToPosixTime of the PC's zone differ from the C runtime's localtime");
    {
        int changes = 0;
        for (int k = 0; k < mine->timecnt; k++) if (mine->ats[k] >= y0 && mine->ats[k] < y1) changes++;
        TCHECK(changes == 0 ? two == 0 && none == 0 : two > 0 && none > 0, "fall-back hour (2 results) / gap (0 results) not found where the zone has transitions");
    }
    posix = (int64_t)_time64(NULL);
    c->x[0] = GP(cal); c->x[1] = GP(info); c->x[2] = GP(&posix);
    time_ToCalendarTime(c);
    TCHECK(strcmp((char *)info + 8, "LOC") != 0 && info[8] != 0, "zone abbreviation");
    printf("  time zone: location %s, now %04d-%02d-%02d %02d:%02d:%02d %s (UTC%+d s, dst %d); rule: %d transitions, %d types, names", loc,
           rd16(GP(cal)), cal[2], cal[3], cal[4], cal[5], cal[6], (char *)info + 8, (int)rd32(GP(info) + 20), info[16], mine->timecnt, mine->typecnt);
    for (int p = 0; p < mine->charcnt; p += (int)strlen(mine->chars + p) + 1) printf(" %s", mine->chars + p);
    printf("; %lld half hours of %d equal to localtime, %lld with two instants, %lld gap checks\n", own, this_year, two, none);
    /* LoadTimeZoneRule + the form with a rule */
    memset(loc, 0, sizeof loc); strcpy(loc, "Asia/Tokyo");
    c->x[0] = GP(&r); c->x[1] = GP(loc);
    time_LoadTimeZoneRule(c);
    TCHECK(c->x[0] == 0, "LoadTimeZoneRule(Asia/Tokyo)");
    posix = 1700000000;                                             /* 2023-11-14 22:13:20 UTC = 2023-11-15 07:13:20 in Tokyo, a Wednesday */
    c->x[0] = GP(cal); c->x[1] = GP(info); c->x[2] = GP(&posix); c->x[3] = GP(&r);
    time_ToCalendarTimeRule(c);
    TCHECK(c->x[0] == 0 && rd16(GP(cal)) == 2023 && cal[2] == 11 && cal[3] == 15 && cal[4] == 7 && cal[5] == 13 && cal[6] == 20 && (int)rd32(GP(info) + 20) == 32400 && info[16] == 0 && rd32(GP(info)) == 3, "Tokyo conversion with a loaded rule");
    printf("  time zone: Asia/Tokyo rule: abbreviation %s\n", (char *)info + 8);
    r.typecnt = 200;
    c->x[0] = GP(cal); c->x[1] = GP(info); c->x[2] = GP(&posix); c->x[3] = GP(&r);
    time_ToCalendarTimeRule(c);
    TCHECK(c->x[0] == TZ_OUT_OF_RANGE, "a rule with too many types must be refused (0x70c74)");
    memset(loc, 0, sizeof loc); strcpy(loc, "Mars/Phobos");
    c->x[0] = GP(&r); c->x[1] = GP(loc);
    time_LoadTimeZoneRule(c);
    TCHECK(c->x[0] == TZ_NOT_FOUND, "LoadTimeZoneRule of an unknown location must give 0x7ba74");
    #undef TCHECK
    return bad;
}
