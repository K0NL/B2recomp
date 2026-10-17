/* nn::album: the console's screenshot / video album, kept in <userdata>\album (<userdata> = %SSBU_USERDATA% or ..\userdata).
 *
 * LIFTED from the SDK (sdklift_gen.c), not here: MovieMakerMovieParameter (constructor, setters, GetAudioBitRate),
 * GetMovieMakerRequiredWorkMemorySize / Alignment, GetAlbumFileCreatedTime, ConvertAlbumEntry::ConvertToCapsrv.
 *
 * What the SDK does ([sdk] = sdk.elf 13.0.4 addresses) and what answers for the system module here:
 *  - nn::album is a thin layer over nn::capsrv (service caps:u, [suyu] src/core/hle/service/caps/caps_u.cpp) and, for the
 *    movie maker, nn::grc.  AlbumFileEntry is capsrv's ApplicationAlbumEntry unchanged ([sdk] 0x50a50 / 0x50a70 copy 0x20
 *    bytes): +0 u64 size, +8 u64 hash, +0x10 AlbumFileDateTime {u16 year, u8 month, day, hour, minute, second, id}
 *    ([sdk] 0x50960 GetAlbumFileCreatedTime reads exactly these bytes), +0x18 storage, +0x19 content type, +0x1f = 1
 *    ([suyu] caps_types.h).  Content types: 0 screenshot, 1 movie, 3 extra movie ([sdk] 0x4f388 w3 = 0, 0x4f200 w3 = 3).
 *  - Initialize [sdk 0x50a10] = LibraryState::Initialize (reference count, 0x50c40) + EnsureAlbumAvailable (0x50cf0: opens
 *    the capsrv sessions once, aborts on failure).  Finalize [sdk 0x50a40 -> 0x50c80] drops the count.
 *  - GetAlbumScreenshotFileList [sdk 0x4f320] / GetExtraMovieFileList [sdk 0x4f1a0]: capsrv::GetAlbumContentsFileList-
 *    ForApplication(out, entries, max, content, begin = 1970-01-01, end = 3000-01-01) = caps:u command 142
 *    ([sdk] 0xd9500 vtable +0x70; [suyu] GetAlbumFileList3AaeAruid).  A service failure is NOT reported: *out = 0 and
 *    the function returns success ([sdk] 0x4f290..0x4f29c).  Here: the album folder is enumerated.
 *  - LoadAlbumImage / LoadAlbumThumbnailImage (all forms) [sdk 0x4f670.., 0x4fe30..]: capsrv Load...ForApplication
 *    ([sdk] 0xd9950 / 0xd9b70: caps:u 110 / 120) which zeroes the outputs first, returns width / height / attribute
 *    (0x40 bytes) / application data (0x400 bytes + u32 size) and ALWAYS clears the work buffer ([sdk] 0xd9ad4); the
 *    album layer copies min(user data size, caller's size) and stores the full size ([sdk] 0x4f9cc..0x4f9f4), turns the
 *    attribute into AlbumFileAttribute {u64 (attribute word 0 == 2), u64 0} ([sdk] 0x4fb60), and maps the Result with
 *    ConvertCapsrvResult ([sdk] 0x50aa0, table 0x9fe760): module 206 description 21 -> 0xdcd6, 22 -> 0xccd6,
 *    23 -> 0xd6d6, 25 -> 0xced6, 27 -> 0xd2d6, 10..19 -> 0xd6d6, 26 / 30 -> 0xded6, anything else -> 0xcad6.
 *    The picture is 1280x720 RGBA8 and the thumbnail 320x180 ([suyu] caps_manager.cpp LoadAlbumScreenShotImage /
 *    Thumbnail; [game] 0x71036e2090 passes a 0x384000-byte buffer and 0x7d000 of work memory, 0x71036e1df8 0xa000 work).
 *    [suyu] does not implement 110 / 120 for caps:u; the decoding here follows its caps:a code (decode, resize).
 *  - DeleteExtraMovieFile [sdk 0x4f2b0]: capsrv Delete...(entry, 3) (caps:u 103); ANY failure becomes 0xd6d6.
 *  - Movie streams [sdk 0x505f0, 0x50770, 0x50800, 0x50850]: capsrv accessor session (0xd9d70..0xda250): Open(handle*,
 *    entry), Close(handle), GetSize(size*, handle), Read(read*, buffer, size, handle, offset); results through
 *    ConvertCapsrvResult.  [game] 0x71036f66bc reads at offsets rounded down to 0x40000 with sizes rounded UP to 0x40000
 *    (at most 0x80000), i.e. past the end of the data: the tail is zero-filled here and *read is the number of real bytes.
 *  - Movie maker [sdk 0x50e70 InitializeMovieMaker .. 0x51b40 EncodeMovieMakerAudioSample]: state at 0xc42264 (mutex),
 *    +0x14 initialized, +0x24 grc MovieLayer, +0x8d running.  Initialize hands the work memory to am
 *    (IApplicationFunctions::CreateMovieMaker) and opens a 1280x720 grc movie layer; Start = grc::StartMovieFile(layer,
 *    the 0x80-byte parameter) and sets "running"; the game renders into the layer's native window
 *    (GetMovieMakerNativeWindow, 0x51580) and feeds PCM with EncodeMovieMakerAudioSample (loops until every byte is
 *    taken, 0x51b74); Finish(userData, size, thumbnail RGBA, size, ImageSize) [sdk 0x517f0; game 0x71036ebaec passes the
 *    0x384000-byte 1280x720 picture] = RequestFinishMovieFile, wait, CompleteFinishMovieFile -> the album entry;
 *    Abort / Finalize = grc::AbortMovieFile.  grc results (module 212, descriptions 901..905) are mapped to 0xccd6 /
 *    0xced6 / 0xd2d6 / 0xdcd6, anything else to 0xcad6 ([sdk] 0x51700..0x51790).  PrecheckToStartMovieMaker(size)
 *    [sdk 0x51610] = capsrv::PrecheckToCreateContentsForApplication(3, size) (caps:u 130).
 *    [suyu] has no grc movie maker (am application_functions.cpp: {1000, nullptr, "CreateMovieMaker"}), so the
 *    encoder's behaviour is this port's: Media Foundation sink writer, H.264 + AAC in an .mp4, parameters from the
 *    MovieMakerMovieParameter ([sdk] 0x51d00: +0x14 video bit rate 8000000, +0x18 width 1280, +0x1c height 720,
 *    +0x20 frame rate 30, +0x24 key frame interval 30, +0x28 audio bit rate 128000, +0x2c sample rate 48000,
 *    +0x30 channels 2, +0x34 sample format 2).
 *    FRAME SOURCE = OPEN: the pictures must come from the graphics backend.  GetMovieMakerNativeWindow returns
 *    album_movie_native_window(); nvn_gl.c has to recognise an nvnWindow built on that value and, in its present job,
 *    read the presented texture back (RGBA8) and call album_movie_push_frame() instead of blitting to the screen and
 *    pacing.  Until the backend announces that with album_movie_frame_source_attach(), PrecheckToStartMovieMaker
 *    answers 0xcad6 (the SDK's "any other failure"), so the game reports an error instead of recording a video
 *    without pictures.  [game] 0x71036eb1e0: StartMovieMaker is only reached after a successful precheck.
 *
 * Files: screenshots <date>_<time>_<nn>.bmp (32-bit BMP; the console stores JPEG, the game never sees the file, only
 * decoded RGBA), their application data <same>.userdata.bin; movies <date>_<time>_<nn>.mp4 with <same>.thumb.bmp and
 * <same>.userdata.bin.  The entry's hash (a keyed hash on the console, [suyu] caps_types.h comment) is this port's own
 * FNV-1a over the date and content type; an entry whose hash does not match is refused as "file not found". */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define A6 (c->x[6])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(v) ((uint64_t)(uintptr_t)(v))

#define CAPS_FILE_NOT_FOUND ((uint32_t)(206 | (23 << 9)))
#define CAPS_OUT_OF_RANGE ((uint32_t)(206 | (8 << 9)))
const char *fs_user_root(void);                           /* nnfs.c: %SSBU_USERDATA% or ..\userdata */
static char g_album_dir[MAX_PATH];
static const char *album_dir(void) {
    if (!g_album_dir[0]) snprintf(g_album_dir, sizeof g_album_dir, "%s\\album", fs_user_root());
    return g_album_dir;
}
static volatile LONG g_shot_counter;

/* Writes a top-down RGBA8 picture as a 32-bit BMP (BGRA, bottom-up). Returns 0 on success. */
static int write_bmp(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    memset(&fh, 0, sizeof fh);
    memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)w * (DWORD)h * 4;
    ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    uint8_t *row = malloc((size_t)w * 4);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *s = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = s[x * 4 + 3]; }
        fwrite(row, 4, (size_t)w, f);
    }
    free(row);
    int bad = ferror(f);
    fclose(f);
    return bad;
}
static uint32_t save_screenshot(const uint8_t *pixels, uint64_t size, const void *user, uint64_t user_size, const char *what) {
    int w, h;
    if (size == 1280ull * 720 * 4) { w = 1280; h = 720; }
    else if (size == 1920ull * 1080 * 4) { w = 1920; h = 1080; }
    else {
        printf("%s: unexpected pixel data size %llu (neither 1280x720 nor 1920x1080 RGBA8): not saved\n", what, (unsigned long long)size);
        return CAPS_OUT_OF_RANGE;
    }
    if (!pixels) return CAPS_OUT_OF_RANGE;
    SYSTEMTIME t;
    char path[MAX_PATH], base[MAX_PATH];
    GetLocalTime(&t);
    CreateDirectoryA(album_dir(), NULL);
    snprintf(base, sizeof base, "%s\\%04d%02d%02d_%02d%02d%02d_%02ld", album_dir(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             InterlockedIncrement(&g_shot_counter) % 100);
    snprintf(path, sizeof path, "%s.bmp", base);
    if (write_bmp(path, pixels, w, h)) { printf("%s: cannot write %s\n", what, path); return CAPS_OUT_OF_RANGE; }
    if (user && user_size && user_size <= 0x400) {         /* the application data that travels with the picture */
        char upath[MAX_PATH];
        snprintf(upath, sizeof upath, "%s.userdata.bin", base);
        FILE *f = fopen(upath, "wb");
        if (f) { fwrite(user, 1, (size_t)user_size, f); fclose(f); }
    }
    printf("%s: %dx%d screenshot saved to %s\n", what, w, h, path);
    trace_printf("     %s -> %s\n", what, path);
    return 0;
}
/* (const void *data, size_t size, ImageSize, AlbumReportOption) */
static void album_SaveScreenshot(Ctx *c) { RET(save_screenshot((const uint8_t *)(uintptr_t)A0, A1, NULL, 0, "nn::album::SaveScreenshot")); }
/* (data, size, ImageSize, ImageOrientation, AlbumReportOption, const void *userData, size_t userDataSize) */
static void album_SaveScreenshotWithUserData(Ctx *c) {
    RET(save_screenshot((const uint8_t *)(uintptr_t)A0, A1, (const void *)(uintptr_t)A5, A6, "nn::album::SaveScreenshotWithUserData"));
}
/* (data, size, ImageSize, AlbumReportOption, const account::Uid &, const char *text) */
static void album_SaveAndShareScreenshot(Ctx *c) {
    uint32_t r = save_screenshot((const uint8_t *)(uintptr_t)A0, A1, NULL, 0, "nn::album::SaveAndShareScreenshot");
    if (!r) printf("nn::album::SaveAndShareScreenshot: sharing needs a network; the picture was only saved\n");
    RET(r);
}

/* ================================================================ the album folder as capsrv entries */
#define CAPS_RESULT(desc) ((uint32_t)(206 | ((desc) << 9)))
#define CONTENT_SCREENSHOT 0
#define CONTENT_EXTRA_MOVIE 3
typedef struct { uint16_t year; uint8_t month, day, hour, minute, second, id; } AlbumDate;
typedef struct { uint64_t size, hash; AlbumDate date; uint8_t storage, content, pad[5], one; } AlbumEntry;      /* 0x20 bytes */

/* [sdk] 0x50aa0 ConvertCapsrvResult (the abort for module 1 / description 106 is not modelled: never produced here) */
static uint32_t convert_capsrv_result(uint32_t r) {
    static const uint32_t table[7] = {0xdcd6, 0xccd6, 0xd6d6, 0xdcd6, 0xced6, 0xdcd6, 0xd2d6};                  /* [sdk] 0x9fe760 */
    if (!r) return 0;
    uint32_t module = r & 0x1ff, desc = (r >> 9) & 0x1fff;
    if (module == 2) return desc == 0x35f200 >> 9 ? 0xdcd6 : 0xcad6;
    if (module != 206) return 0xcad6;
    if (desc >= 21 && desc < 28 && ((0x57 >> (desc - 21)) & 1)) return table[desc - 21];
    if (desc >= 10 && desc < 20) return 0xd6d6;
    if (desc == 26 || desc == 30) return 0xded6;
    return 0xcad6;
}
static uint64_t entry_hash(const AlbumDate *d, int content) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = (const uint8_t *)d;
    for (int i = 0; i < 8; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return (h ^ (uint64_t)content) * 0x100000001b3ull;
}
static const char *content_ext(int content) { return content == CONTENT_SCREENSHOT ? "bmp" : content == CONTENT_EXTRA_MOVIE ? "mp4" : NULL; }
static void entry_base(char *out, size_t n, const AlbumDate *d) {
    snprintf(out, n, "%s\\%04d%02d%02d_%02d%02d%02d_%02d", album_dir(), d->year, d->month, d->day, d->hour, d->minute, d->second, d->id);
}
/* "<yyyymmdd>_<hhmmss>_<nn>.<ext>" -> date; 0 when the name is not one of ours */
static int parse_name(const char *name, const char *ext, AlbumDate *d) {
    int v[7], used = 0;
    char tail[8];
    if (strlen(name) != 18 + 1 + strlen(ext)) return 0;
    for (int i = 0; i < 18; i++) if (i == 8 || i == 15 ? name[i] != '_' : name[i] < '0' || name[i] > '9') return 0;
    if (sscanf(name, "%4d%2d%2d_%2d%2d%2d_%2d.%7s%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], tail, &used) != 8 || _stricmp(tail, ext)) return 0;
    if (v[0] < 1970 || v[0] >= 3000 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59 || v[5] > 59) return 0;
    d->year = (uint16_t)v[0]; d->month = (uint8_t)v[1]; d->day = (uint8_t)v[2]; d->hour = (uint8_t)v[3]; d->minute = (uint8_t)v[4]; d->second = (uint8_t)v[5]; d->id = (uint8_t)v[6];
    return 1;
}
static uint64_t file_size(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ~0ull;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}
static void make_entry(AlbumEntry *e, const AlbumDate *d, int content, uint64_t size) {
    memset(e, 0, sizeof *e);
    e->size = size; e->hash = entry_hash(d, content); e->date = *d; e->storage = 0; e->content = (uint8_t)content; e->one = 1;
}
static int entry_cmp(const void *a, const void *b) {
    const AlbumDate *x = &((const AlbumEntry *)a)->date, *y = &((const AlbumEntry *)b)->date;
    return x->year != y->year ? (int)x->year - (int)y->year : memcmp(&x->month, &y->month, 6);
}
/* caps:u 142: the files of one content type, oldest first; at most `max` entries */
static int list_entries(AlbumEntry *out, int max, int content) {
    char pattern[MAX_PATH], path[MAX_PATH];
    const char *ext = content_ext(content);
    WIN32_FIND_DATAA fd;
    int n = 0;
    if (!ext || max <= 0) return 0;
    snprintf(pattern, sizeof pattern, "%s\\*.%s", album_dir(), ext);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        AlbumDate d;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !parse_name(fd.cFileName, ext, &d)) continue;
        snprintf(path, sizeof path, "%s\\%s", album_dir(), fd.cFileName);
        if (n < max) make_entry(&out[n++], &d, content, file_size(path));
    } while (n < max && FindNextFileA(h, &fd));
    FindClose(h);
    qsort(out, (size_t)n, sizeof *out, entry_cmp);
    return n;
}
/* entry -> the file's path without extension; a capsrv Result when the entry is not a file of the album */
static uint32_t entry_file(char *base, size_t n, uint64_t entry_addr, int content, char *path, size_t pn) {
    AlbumEntry e;
    if (!entry_addr) return CAPS_RESULT(23);
    memcpy(&e, (const void *)(uintptr_t)entry_addr, sizeof e);
    if (e.storage > 1) return CAPS_RESULT(13);                                       /* [suyu] ResultInvalidStorage */
    if (e.date.month < 1 || e.date.month > 12 || e.date.day < 1 || e.date.day > 31 || e.date.hour > 23 || e.date.minute > 59 || e.date.second > 59)
        return CAPS_RESULT(12);                                                      /* [suyu] ResultInvalidTimestamp */
    if (e.content != content || e.hash != entry_hash(&e.date, e.content)) return CAPS_RESULT(23);
    entry_base(base, n, &e.date);
    snprintf(path, pn, "%s.%s", base, content_ext(content));
    if (file_size(path) == ~0ull) return CAPS_RESULT(23);                            /* [suyu] ResultFileNotFound */
    return 0;
}

/* ---- pictures */
static uint8_t *read_bmp(const char *path, int *w, int *h) {                         /* 32-bit BMP -> top-down RGBA8 (malloc) */
    FILE *f = fopen(path, "rb");
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    uint8_t *px = NULL;
    if (!f) return NULL;
    if (fread(&fh, sizeof fh, 1, f) == 1 && fread(&ih, sizeof ih, 1, f) == 1 && fh.bfType == 0x4d42 && ih.biBitCount == 32 && ih.biCompression == BI_RGB &&
        ih.biWidth > 0 && ih.biWidth <= 4096 && ih.biHeight && abs(ih.biHeight) <= 4096) {
        int ww = ih.biWidth, hh = abs(ih.biHeight);
        px = malloc((size_t)ww * hh * 4);
        fseek(f, (long)fh.bfOffBits, SEEK_SET);
        for (int y = 0; px && y < hh; y++) {
            uint8_t *row = px + (size_t)(ih.biHeight > 0 ? hh - 1 - y : y) * ww * 4;
            if (fread(row, 4, (size_t)ww, f) != (size_t)ww) { free(px); px = NULL; break; }
            for (int x = 0; x < ww; x++) { uint8_t t = row[x * 4]; row[x * 4] = row[x * 4 + 2]; row[x * 4 + 2] = t; }
        }
        *w = ww; *h = hh;
    }
    fclose(f);
    return px;
}
/* area average (each destination pixel = mean of the source pixels it covers) */
static void scale_rgba(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    if (sw == dw && sh == dh) { memcpy(dst, src, (size_t)sw * sh * 4); return; }
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
            unsigned acc[4] = {0, 0, 0, 0}, cnt;
            if (x1 <= x0) x1 = x0 + 1;
            cnt = (unsigned)((x1 - x0) * (y1 - y0));
            for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { const uint8_t *p = src + ((size_t)yy * sw + xx) * 4; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; }
            uint8_t *q = dst + ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; k++) q[k] = (uint8_t)((acc[k] + cnt / 2) / cnt);
        }
    }
}
/* capsrv Load...ForApplication ([sdk] 0xd9950 / 0xd9b70) + the album layer above it.  `thumb`: 320x180 instead of 1280x720. */
static uint32_t load_image(uint64_t out_w, uint64_t out_h, uint64_t out_attr, uint64_t out_user_size, uint64_t user, uint64_t user_cap,
                           uint64_t buf, uint64_t buf_size, uint64_t work, uint64_t work_size, uint64_t entry, int thumb) {
    char base[MAX_PATH], path[MAX_PATH + 16];
    int w = thumb ? 320 : 1280, h = thumb ? 180 : 720, sw = 0, sh = 0, content = CONTENT_SCREENSHOT;
    uint8_t app[0x400];
    uint32_t app_size = 0, r;
    uint8_t *px = NULL;
    if (out_w) wr32(out_w, 0);                                                       /* [sdk] 0xd9998..0xd99d4: outputs cleared first */
    if (out_h) wr32(out_h, 0);
    if (entry && thumb && rd8(entry + 0x19) == CONTENT_EXTRA_MOVIE) content = CONTENT_EXTRA_MOVIE;
    r = entry_file(base, sizeof base, entry, content, path, sizeof path);
    if (!r && buf_size < (uint64_t)w * h * 4) r = CAPS_RESULT(30);                    /* [suyu] ResultReadBufferShortage */
    if (!r) {
        if (content == CONTENT_EXTRA_MOVIE) snprintf(path, sizeof path, "%s.thumb.bmp", base);
        px = read_bmp(path, &sw, &sh);
        if (!px) r = CAPS_RESULT(content == CONTENT_EXTRA_MOVIE ? 23 : 14);          /* [suyu] ResultInvalidFileContents */
    }
    if (!r) {
        scale_rgba(px, sw, sh, (uint8_t *)(uintptr_t)buf, w, h);
        snprintf(path, sizeof path, "%s.userdata.bin", base);
        FILE *f = fopen(path, "rb");
        if (f) { app_size = (uint32_t)fread(app, 1, sizeof app, f); fclose(f); }
        if (out_w) wr32(out_w, (uint32_t)w);
        if (out_h) wr32(out_h, (uint32_t)h);
        if (user && app_size) memcpy((void *)(uintptr_t)user, app, app_size < user_cap ? app_size : (size_t)user_cap);      /* [sdk] 0x4f9cc */
        if (out_user_size) wr64(out_user_size, app_size);                            /* [sdk] 0x4f9ec */
        if (out_attr) { wr64(out_attr, 0); wr64(out_attr + 8, 0); }                  /* [sdk] 0x4fb60: (attribute word 0 == 2), 0 */
    }
    free(px);
    if (work && work_size) memset((void *)(uintptr_t)work, 0, (size_t)work_size);    /* [sdk] 0xd9ad4 */
    return convert_capsrv_result(r);
}
#define STK(i) rd64(c->x[31] + 8 * (i))
/* (int *w, int *h, void *buf, size_t, void *work, size_t, const AlbumFileEntry &) */
static void album_Load7(Ctx *c) { RET(load_image(A0, A1, 0, 0, 0, 0, A2, A3, A4, A5, A6, 0)); }
/* (int *w, int *h, size_t *outUserDataSize, void *userData, size_t, void *buf, size_t, void *work, [sp] size_t, [sp+8] entry) */
static void album_Load10(Ctx *c) { RET(load_image(A0, A1, 0, A2, A3, A4, A5, A6, c->x[7], STK(0), STK(1), 0)); }
static void album_Thumb10(Ctx *c) { RET(load_image(A0, A1, 0, A2, A3, A4, A5, A6, c->x[7], STK(0), STK(1), 1)); }
/* (int *w, int *h, AlbumFileAttribute *, void *buf, size_t, void *work, size_t, entry) */
static void album_LoadThumbAttr(Ctx *c) { RET(load_image(A0, A1, A2, 0, 0, 0, A3, A4, A5, A6, c->x[7], 1)); }
/* (int *w, int *h, AlbumFileAttribute *, size_t *outUserDataSize, void *userData, size_t, void *buf, size_t, [sp] work, [sp+8] size, [sp+16] entry) */
static void album_LoadThumbAttrUser(Ctx *c) { RET(load_image(A0, A1, A2, A3, A4, A5, A6, c->x[7], STK(0), STK(1), STK(2), 1)); }

/* ---- library state [sdk 0x50c40 / 0x50c80] and the lists */
static volatile LONG g_album_refs;
static void album_Initialize(Ctx *c) { (void)c; InterlockedIncrement(&g_album_refs); }
static void album_Finalize(Ctx *c) { (void)c; if (InterlockedDecrement(&g_album_refs) < 0) InterlockedExchange(&g_album_refs, 0); }       /* [sdk] 0x50cd0 */
static void file_list(Ctx *c, int content) {                                         /* (int *outCount, AlbumFileEntry *, int max) */
    int n = A1 ? list_entries((AlbumEntry *)(uintptr_t)A1, (int)A2, content) : 0;
    if (A0) wr32(A0, (uint32_t)n);
    RET(0);                                                                          /* [sdk] 0x4f41c: always success */
}
static void album_GetAlbumScreenshotFileList(Ctx *c) { file_list(c, CONTENT_SCREENSHOT); }
static void album_GetExtraMovieFileList(Ctx *c) { file_list(c, CONTENT_EXTRA_MOVIE); }
static void album_DeleteExtraMovieFile(Ctx *c) {                                     /* [sdk] 0x4f2b0: any failure -> 0xd6d6 */
    char base[MAX_PATH], path[MAX_PATH + 16];
    uint32_t r = entry_file(base, sizeof base, A0, CONTENT_EXTRA_MOVIE, path, sizeof path);
    if (!r && !DeleteFileA(path)) r = CAPS_RESULT(23);
    if (!r) {
        snprintf(path, sizeof path, "%s.thumb.bmp", base); DeleteFileA(path);
        snprintf(path, sizeof path, "%s.userdata.bin", base); DeleteFileA(path);
        trace_printf("     nn::album::DeleteExtraMovieFile: %s.mp4 deleted\n", base);
    }
    RET(r ? 0xd6d6 : 0);
}

/* ---- movie read streams (capsrv accessor session, [sdk] 0xd9d70..0xda250) */
#define MAX_STREAMS 8
static struct { HANDLE file; uint64_t size; } g_streams[MAX_STREAMS];
static SRWLOCK g_stream_lock = SRWLOCK_INIT;
static void album_OpenAlbumMovieStream(Ctx *c) {                                     /* (MovieStreamHandle *out, entry): [sdk] 0x50724 stores the handle even on failure */
    char base[MAX_PATH], path[MAX_PATH + 16];
    uint64_t handle = 0;
    uint32_t r = entry_file(base, sizeof base, A1, CONTENT_EXTRA_MOVIE, path, sizeof path);
    if (!r) {
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) r = CAPS_RESULT(23);
        else {
            LARGE_INTEGER size;
            GetFileSizeEx(f, &size);
            AcquireSRWLockExclusive(&g_stream_lock);
            for (int i = 0; i < MAX_STREAMS && !handle; i++) if (!g_streams[i].file) { g_streams[i].file = f; g_streams[i].size = (uint64_t)size.QuadPart; handle = (uint64_t)i + 1; }
            ReleaseSRWLockExclusive(&g_stream_lock);
            if (!handle) { CloseHandle(f); r = CAPS_RESULT(1401); }                  /* [suyu] ResultFileCountLimit; the console's limit of open streams is not known */
        }
    }
    if (A0) wr64(A0, handle);
    RET(convert_capsrv_result(r));
}
static int stream_index(uint64_t handle) { return handle >= 1 && handle <= MAX_STREAMS && g_streams[handle - 1].file ? (int)handle - 1 : -1; }
static void album_GetAlbumMovieStreamSize(Ctx *c) {                                  /* (size_t *out, handle): [sdk] 0xda120 stores only on success */
    AcquireSRWLockShared(&g_stream_lock);
    int i = stream_index(A1);
    if (i >= 0 && A0) wr64(A0, g_streams[i].size);
    ReleaseSRWLockShared(&g_stream_lock);
    RET(convert_capsrv_result(i < 0 ? CAPS_RESULT(23) : 0));
}
static void album_ReadAlbumMovieStream(Ctx *c) {                                     /* (size_t *outRead, void *buf, size_t size, handle, int64_t offset) */
    uint32_t r = 0;
    uint64_t got = 0;
    AcquireSRWLockShared(&g_stream_lock);
    int i = stream_index(A3);
    if (i < 0 || (int64_t)A4 < 0) r = CAPS_RESULT(i < 0 ? 23 : 8);                   /* [suyu] ResultOutOfRange */
    else if (A1 && A2) {
        uint8_t *dst = (uint8_t *)(uintptr_t)A1;
        while (got < A2 && A4 + got < g_streams[i].size) {
            OVERLAPPED ov;
            DWORD n = 0, want = (DWORD)(A2 - got > 0x1000000 ? 0x1000000 : A2 - got);
            memset(&ov, 0, sizeof ov);
            ov.Offset = (DWORD)(A4 + got); ov.OffsetHigh = (DWORD)((A4 + got) >> 32);
            if (!ReadFile(g_streams[i].file, dst + got, want, &n, &ov) || !n) break;
            got += n;
        }
        if (got < A2) memset(dst + got, 0, (size_t)(A2 - got));
    }
    ReleaseSRWLockShared(&g_stream_lock);
    if (!r && A0) wr64(A0, got);                                                     /* [sdk] 0xda200 stores only on success */
    RET(convert_capsrv_result(r));
}
static void album_CloseAlbumMovieStream(Ctx *c) {                                    /* (handle): void; [sdk] 0xda064 aborts on a failure */
    AcquireSRWLockExclusive(&g_stream_lock);
    int i = stream_index(A0);
    if (i >= 0) { CloseHandle(g_streams[i].file); g_streams[i].file = NULL; g_streams[i].size = 0; }
    ReleaseSRWLockExclusive(&g_stream_lock);
    if (i < 0) printf("nn::album::CloseAlbumMovieStream: handle %llu is not open (the SDK aborts here)\n", (unsigned long long)A0);
}

/* ================================================================ movie maker: Media Foundation sink writer */
#define ALBUM_MOVIE_ERROR 0xcad6u
static struct {
    CRITICAL_SECTION lock;
    LONG lock_ready;
    int initialized, running, frame_source;                  /* [sdk] 0xc42278, 0xc422f1; frame_source: see the file header */
    uint32_t error;
    IMFSinkWriter *writer;
    DWORD video, audio;
    int width, height, fps, rate, channels, has_audio;
    int64_t frames, audio_frames;
    uint8_t *nv12, *first_frame;                             /* first_frame: 320x180 RGBA of the first picture (thumbnail when Finish gives none) */
    char tmp_path[MAX_PATH];
} g_mm;
static char g_mm_window_token[16] = "album movie";
static _Thread_local int t_album_com;
static void mm_lock(void) {
    if (InterlockedCompareExchange(&g_mm.lock_ready, 1, 0) == 0) { InitializeCriticalSection(&g_mm.lock); g_mm.lock_ready = 2; }
    while (g_mm.lock_ready != 2) SwitchToThread();
    EnterCriticalSection(&g_mm.lock);
    if (!t_album_com) { CoInitializeEx(NULL, COINIT_MULTITHREADED); t_album_com = 1; }
}
static void mm_unlock(void) { LeaveCriticalSection(&g_mm.lock); }
/* for the graphics backend (nvn_gl.c): the value GetMovieMakerNativeWindow hands to the game, and "pictures will be delivered" */
uint64_t album_movie_native_window(void) { return (uint64_t)(uintptr_t)g_mm_window_token; }
void album_movie_frame_source_attach(void) { g_mm.frame_source = 1; }

static HRESULT mm_set_video(IMFMediaType *t, const GUID *sub, int w, int h, int fps) {
    HRESULT hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, sub);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, ((UINT64)(UINT32)w << 32) | (UINT32)h);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, ((UINT64)(UINT32)fps << 32) | 1);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    return hr;
}
static HRESULT mm_set_audio(IMFMediaType *t, const GUID *sub, int rate, int channels, int bit_rate) {
    HRESULT hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, sub);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    if (SUCCEEDED(hr) && bit_rate) {                         /* the AAC encoder takes 96 / 128 / 160 / 192 kbit/s */
        int bytes = bit_rate / 8 <= 12000 ? 12000 : bit_rate / 8 <= 16000 ? 16000 : bit_rate / 8 <= 20000 ? 20000 : 24000;
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)bytes);
    } else if (SUCCEEDED(hr)) {
        hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)channels * 2);
        if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(rate * channels * 2));
    }
    return hr;
}
static void mm_release_writer(int delete_file) {
    if (g_mm.writer) { IMFSinkWriter_Release(g_mm.writer); g_mm.writer = NULL; }
    free(g_mm.nv12); g_mm.nv12 = NULL;
    if (delete_file && g_mm.tmp_path[0]) DeleteFileA(g_mm.tmp_path);
}
/* grc::StartMovieFile: opens the encoder for the 0x80-byte MovieMakerMovieParameter. Returns an HRESULT. */
static HRESULT mm_open(const uint8_t *param) {
    WCHAR wpath[MAX_PATH];
    IMFMediaType *t = NULL;
    int32_t v[10];
    memcpy(v, param + 0x14, sizeof v);                       /* video bit rate, width, height, fps, key interval, audio bit rate, rate, channels, format */
    g_mm.width = v[1]; g_mm.height = v[2]; g_mm.fps = v[3]; g_mm.rate = v[6]; g_mm.channels = v[7];
    g_mm.frames = g_mm.audio_frames = 0;
    if (g_mm.width <= 0 || g_mm.height <= 0 || g_mm.width > 1920 || g_mm.height > 1080 || (g_mm.width & 1) || (g_mm.height & 1) || g_mm.fps <= 0 || g_mm.fps > 60) return E_INVALIDARG;
    g_mm.has_audio = g_mm.rate > 0 && (g_mm.channels == 1 || g_mm.channels == 2) && v[8] == 2;      /* [sdk] 0x51d2c: sample format 2 = 16-bit PCM */
    CreateDirectoryA(album_dir(), NULL);
    snprintf(g_mm.tmp_path, sizeof g_mm.tmp_path, "%s\\recording.tmp.mp4", album_dir());
    MultiByteToWideChar(CP_ACP, 0, g_mm.tmp_path, -1, wpath, MAX_PATH);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(wpath, NULL, NULL, &g_mm.writer);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = mm_set_video(t, &MFVideoFormat_H264, g_mm.width, g_mm.height, g_mm.fps);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT32(t, &MF_MT_AVG_BITRATE, (UINT32)(v[0] > 0 ? v[0] : 8000000));
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_AddStream(g_mm.writer, t, &g_mm.video);
    if (t) { IMFMediaType_Release(t); t = NULL; }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = mm_set_video(t, &MFVideoFormat_NV12, g_mm.width, g_mm.height, g_mm.fps);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(g_mm.writer, g_mm.video, t, NULL);
    if (t) { IMFMediaType_Release(t); t = NULL; }
    if (SUCCEEDED(hr) && g_mm.has_audio) {
        hr = MFCreateMediaType(&t);
        if (SUCCEEDED(hr)) hr = mm_set_audio(t, &MFAudioFormat_AAC, g_mm.rate, g_mm.channels, v[5] > 0 ? v[5] : 128000);
        if (SUCCEEDED(hr)) hr = IMFSinkWriter_AddStream(g_mm.writer, t, &g_mm.audio);
        if (t) { IMFMediaType_Release(t); t = NULL; }
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
        if (SUCCEEDED(hr)) hr = mm_set_audio(t, &MFAudioFormat_PCM, g_mm.rate, g_mm.channels, 0);
        if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(g_mm.writer, g_mm.audio, t, NULL);
        if (t) { IMFMediaType_Release(t); t = NULL; }
    }
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_BeginWriting(g_mm.writer);
    if (SUCCEEDED(hr)) g_mm.nv12 = malloc((size_t)g_mm.width * g_mm.height * 3 / 2);
    if (FAILED(hr)) { printf("nn::album movie maker: the encoder cannot be opened (HRESULT %08lx)\n", (unsigned long)hr); mm_release_writer(1); }
    return hr;
}
static HRESULT mm_write(DWORD stream, const void *data, size_t size, int64_t time, int64_t duration) {
    IMFMediaBuffer *b = NULL;
    IMFSample *s = NULL;
    BYTE *p = NULL;
    HRESULT hr = MFCreateMemoryBuffer((DWORD)size, &b);
    if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(b, &p, NULL, NULL);
    if (SUCCEEDED(hr)) { memcpy(p, data, size); IMFMediaBuffer_Unlock(b); hr = IMFMediaBuffer_SetCurrentLength(b, (DWORD)size); }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
    if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(s, b);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(s, time);
    if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(s, duration);
    if (SUCCEEDED(hr)) hr = IMFSinkWriter_WriteSample(g_mm.writer, stream, s);
    if (s) IMFSample_Release(s);
    if (b) IMFMediaBuffer_Release(b);
    return hr;
}
/* One picture of the movie layer (what the game presented to the movie maker's native window): RGBA8, `stride` bytes per
 * row, first row = top unless bottom_up. Any size: it is scaled to the movie's. Returns 0 when it was taken. */
int album_movie_push_frame(const uint8_t *rgba, int w, int h, int stride, int bottom_up) {
    int ok = 0;
    if (!rgba || w <= 0 || h <= 0) return 1;
    mm_lock();
    if (g_mm.running && g_mm.writer && !g_mm.error) {
        int mw = g_mm.width, mh = g_mm.height;
        uint8_t *tight = malloc((size_t)w * h * 4), *scaled = NULL;
        const uint8_t *src = tight;
        for (int y = 0; y < h; y++) memcpy(tight + (size_t)y * w * 4, rgba + (size_t)(bottom_up ? h - 1 - y : y) * stride, (size_t)w * 4);
        if (w != mw || h != mh) { scaled = malloc((size_t)mw * mh * 4); scale_rgba(tight, w, h, scaled, mw, mh); src = scaled; }
        if (!g_mm.first_frame) { g_mm.first_frame = malloc(320 * 180 * 4); scale_rgba(src, mw, mh, g_mm.first_frame, 320, 180); }
        uint8_t *Y = g_mm.nv12, *UV = g_mm.nv12 + (size_t)mw * mh;                   /* BT.601, limited range */
        for (int y = 0; y < mh; y++) for (int x = 0; x < mw; x++) {
            const uint8_t *p = src + ((size_t)y * mw + x) * 4;
            Y[(size_t)y * mw + x] = (uint8_t)((66 * p[0] + 129 * p[1] + 25 * p[2] + 128 + (16 << 8)) >> 8);
        }
        for (int y = 0; y < mh; y += 2) for (int x = 0; x < mw; x += 2) {
            int r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) { const uint8_t *p = src + ((size_t)(y + (k >> 1)) * mw + x + (k & 1)) * 4; r += p[0]; g += p[1]; b += p[2]; }
            r = (r + 2) >> 2; g = (g + 2) >> 2; b = (b + 2) >> 2;
            UV[(size_t)(y / 2) * mw + x] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128 + (128 << 8)) >> 8);
            UV[(size_t)(y / 2) * mw + x + 1] = (uint8_t)((112 * r - 94 * g - 18 * b + 128 + (128 << 8)) >> 8);
        }
        HRESULT hr = mm_write(g_mm.video, g_mm.nv12, (size_t)mw * mh * 3 / 2, g_mm.frames * 10000000ll / g_mm.fps, 10000000ll / g_mm.fps);
        if (FAILED(hr)) { printf("nn::album movie maker: video frame %lld refused (HRESULT %08lx)\n", (long long)g_mm.frames, (unsigned long)hr); g_mm.error = ALBUM_MOVIE_ERROR; }
        else { g_mm.frames++; ok = 1; }
        free(tight); free(scaled);
    }
    mm_unlock();
    return !ok;
}
static void mm_abort(void) {                                                          /* grc::AbortMovieFile: [sdk] 0x5149c, 0x5155c */
    if (g_mm.running) { mm_release_writer(1); g_mm.running = 0; trace_printf("     nn::album movie maker: recording aborted\n"); }
    free(g_mm.first_frame); g_mm.first_frame = NULL;
}
static void album_InitializeMovieMaker(Ctx *c) {                                      /* (void *work, size_t): Result [sdk 0x50e70 returns w24] */
    mm_lock();
    g_mm.initialized = 1; g_mm.running = 0; g_mm.error = 0;
    mm_unlock();
    RET(0);
}
static void album_FinalizeMovieMaker(Ctx *c) { (void)c; mm_lock(); mm_abort(); g_mm.initialized = 0; g_mm.error = 0; mm_unlock(); }       /* [sdk] 0x51440 */
static void album_AbortMovieMaker(Ctx *c) { (void)c; mm_lock(); mm_abort(); mm_unlock(); }                                             /* [sdk] 0x51510 */
static void album_IsMovieMakerRunning(Ctx *c) { mm_lock(); int r = g_mm.initialized && g_mm.running; mm_unlock(); RET(r); }            /* [sdk] 0x50e00 */
static void album_CheckMovieMakerError(Ctx *c) { mm_lock(); uint32_t r = g_mm.error; mm_unlock(); RET(r); }                            /* [sdk] 0x51a60 */
static void album_GetMovieMakerNativeWindow(Ctx *c) { if (A0) wr64(A0, album_movie_native_window()); RET(0); }                         /* [sdk] 0x51580 */
static void album_PrecheckToStartMovieMaker(Ctx *c) {                                 /* (size_t size) [sdk 0x51610] */
    ULARGE_INTEGER avail;
    uint32_t r = 0;
    if (!g_mm.frame_source) {
        trace_printf("     nn::album::PrecheckToStartMovieMaker refused (0xcad6): the graphics backend delivers no movie pictures\n");
        RET(ALBUM_MOVIE_ERROR);
        return;
    }
    CreateDirectoryA(album_dir(), NULL);
    if (!GetDiskFreeSpaceExA(album_dir(), &avail, NULL, NULL) || avail.QuadPart < A0) r = ALBUM_MOVIE_ERROR;
    RET(r);
}
static void album_StartMovieMaker(Ctx *c) {                                           /* (const MovieMakerMovieParameter &) [sdk 0x51680] */
    uint32_t r = ALBUM_MOVIE_ERROR;
    mm_lock();
    if (g_mm.initialized && !g_mm.running && g_mm.frame_source && A0) {
        free(g_mm.first_frame); g_mm.first_frame = NULL;
        g_mm.error = 0;
        if (SUCCEEDED(mm_open((const uint8_t *)(uintptr_t)A0))) { g_mm.running = 1; r = 0; }             /* [sdk] 0x5175c */
    }
    mm_unlock();
    if (!r) trace_printf("     nn::album::StartMovieMaker: recording %dx%d %d fps to %s\n", g_mm.width, g_mm.height, g_mm.fps, g_mm.tmp_path);
    RET(r);
}
static void album_EncodeMovieMakerAudioSample(Ctx *c) {                               /* (const void *pcm, size_t size) [sdk 0x51b40] */
    uint32_t r = 0;
    if (!A1) { RET(0); return; }                                                      /* [sdk] 0x51b58 */
    mm_lock();
    if (!g_mm.running || !g_mm.writer) r = ALBUM_MOVIE_ERROR;
    else if (g_mm.has_audio && !g_mm.error) {
        int frame = g_mm.channels * 2;
        int64_t n = (int64_t)(A1 / (uint64_t)frame);
        HRESULT hr = n ? mm_write(g_mm.audio, (const void *)(uintptr_t)A0, (size_t)(n * frame), g_mm.audio_frames * 10000000ll / g_mm.rate, n * 10000000ll / g_mm.rate) : S_OK;
        if (FAILED(hr)) { printf("nn::album movie maker: audio refused (HRESULT %08lx)\n", (unsigned long)hr); g_mm.error = r = ALBUM_MOVIE_ERROR; }
        else g_mm.audio_frames += n;
    }
    mm_unlock();
    RET(r);
}
/* FinishMovieMaker(entry out | NULL, userData, size, thumbnail RGBA, size, ImageSize) [sdk 0x517f0] */
static uint32_t mm_finish(uint64_t out_entry, uint64_t user, uint64_t user_size, uint64_t thumb, uint64_t thumb_size, uint32_t image_size) {
    uint32_t r = ALBUM_MOVIE_ERROR;
    mm_lock();
    if (g_mm.running && g_mm.writer) {
        HRESULT hr = g_mm.error ? E_FAIL : IMFSinkWriter_Finalize(g_mm.writer);
        if (FAILED(hr)) { printf("nn::album::FinishMovieMaker: the movie cannot be completed (HRESULT %08lx)\n", (unsigned long)hr); mm_abort(); }      /* [sdk] 0x518f8 */
        else {
            char base[MAX_PATH], path[MAX_PATH + 16];
            AlbumDate d;
            SYSTEMTIME t;
            mm_release_writer(0);
            GetLocalTime(&t);
            d.year = t.wYear; d.month = (uint8_t)t.wMonth; d.day = (uint8_t)t.wDay; d.hour = (uint8_t)t.wHour; d.minute = (uint8_t)t.wMinute; d.second = (uint8_t)t.wSecond;
            for (d.id = 0; d.id < 100; d.id++) { entry_base(base, sizeof base, &d); snprintf(path, sizeof path, "%s.mp4", base); if (file_size(path) == ~0ull) break; }
            if (d.id < 100 && MoveFileA(g_mm.tmp_path, path)) {
                static uint8_t thumb_px[320 * 180 * 4];
                const uint8_t *th = g_mm.first_frame;
                if (thumb && image_size == 0 && thumb_size >= 1280ull * 720 * 4) { scale_rgba((const uint8_t *)(uintptr_t)thumb, 1280, 720, thumb_px, 320, 180); th = thumb_px; }    /* [sdk] 0x50ba0: ImageSize 0 = 1280x720 */
                if (th) { char tp[MAX_PATH + 16]; snprintf(tp, sizeof tp, "%s.thumb.bmp", base); write_bmp(tp, th, 320, 180); }
                if (user && user_size && user_size <= 0x400) {
                    char up[MAX_PATH + 16];
                    snprintf(up, sizeof up, "%s.userdata.bin", base);
                    FILE *f = fopen(up, "wb");
                    if (f) { fwrite((const void *)(uintptr_t)user, 1, (size_t)user_size, f); fclose(f); }
                }
                if (out_entry) { AlbumEntry e; make_entry(&e, &d, CONTENT_EXTRA_MOVIE, file_size(path)); memcpy((void *)(uintptr_t)out_entry, &e, sizeof e); }
                printf("nn::album::FinishMovieMaker: %lld pictures, %lld audio frames saved to %s\n", (long long)g_mm.frames, (long long)g_mm.audio_frames, path);
                r = 0;
            } else DeleteFileA(g_mm.tmp_path);
            g_mm.running = 0;                                                        /* [sdk] 0x5196c */
            free(g_mm.first_frame); g_mm.first_frame = NULL;
        }
    }
    mm_unlock();
    return r;
}
static void album_FinishMovieMaker2(Ctx *c) { RET(mm_finish(0, A0, A1, 0, 0, 0)); }                       /* (userData, size) [sdk 0x51a00] */
static void album_FinishMovieMaker5(Ctx *c) { RET(mm_finish(0, A0, A1, A2, A3, (uint32_t)A4)); }          /* (userData, size, thumbnail, size, ImageSize) [sdk 0x51a30] */

#define AL(n) "_ZN2nn5album" n
static const ImportDef album_imports[] = {
    {AL("14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE"), album_SaveScreenshot},
    {AL("26SaveScreenshotWithUserDataEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionES2_m"), album_SaveScreenshotWithUserData},
    {AL("22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc"), album_SaveAndShareScreenshot},
    {AL("14LoadAlbumImageEPiS1_PvmS2_mRKNS0_14AlbumFileEntryE"), album_Load7},
    {AL("14LoadAlbumImageEPiS1_PmPvmS3_mS3_mRKNS0_14AlbumFileEntryE"), album_Load10},
    {AL("23LoadAlbumThumbnailImageEPiS1_PmPvmS3_mS3_mRKNS0_14AlbumFileEntryE"), album_Thumb10},
    {AL("10InitializeEv"), album_Initialize}, {AL("8FinalizeEv"), album_Finalize}, {AL("19IsMovieMakerRunningEv"), album_IsMovieMakerRunning},
    {AL("26GetAlbumScreenshotFileListEPiPNS0_14AlbumFileEntryEi"), album_GetAlbumScreenshotFileList},
    {AL("21GetExtraMovieFileListEPiPNS0_14AlbumFileEntryEi"), album_GetExtraMovieFileList},
    {AL("23LoadAlbumThumbnailImageEPiS1_PNS0_18AlbumFileAttributeEPvmS4_mRKNS0_14AlbumFileEntryE"), album_LoadThumbAttr},
    {AL("23LoadAlbumThumbnailImageEPiS1_PNS0_18AlbumFileAttributeEPmPvmS5_mS5_mRKNS0_14AlbumFileEntryE"), album_LoadThumbAttrUser},
    {AL("20DeleteExtraMovieFileERKNS0_14AlbumFileEntryE"), album_DeleteExtraMovieFile},
    {AL("20OpenAlbumMovieStreamEPNS0_17MovieStreamHandleERKNS0_14AlbumFileEntryE"), album_OpenAlbumMovieStream},
    {AL("23GetAlbumMovieStreamSizeEPmNS0_17MovieStreamHandleE"), album_GetAlbumMovieStreamSize},
    {AL("20ReadAlbumMovieStreamEPmPvmNS0_17MovieStreamHandleEl"), album_ReadAlbumMovieStream},
    {AL("21CloseAlbumMovieStreamENS0_17MovieStreamHandleE"), album_CloseAlbumMovieStream},
    {AL("25PrecheckToStartMovieMakerEm"), album_PrecheckToStartMovieMaker},
    {AL("20InitializeMovieMakerEPvm"), album_InitializeMovieMaker}, {AL("18FinalizeMovieMakerEv"), album_FinalizeMovieMaker}, {AL("15AbortMovieMakerEv"), album_AbortMovieMaker},
    {AL("15StartMovieMakerERKNS0_24MovieMakerMovieParameterE"), album_StartMovieMaker},
    {AL("16FinishMovieMakerEPKvm"), album_FinishMovieMaker2}, {AL("16FinishMovieMakerEPKvmS2_mNS0_9ImageSizeE"), album_FinishMovieMaker5},
    {AL("27EncodeMovieMakerAudioSampleEPKvm"), album_EncodeMovieMakerAudioSample},
    {AL("20CheckMovieMakerErrorEv"), album_CheckMovieMakerError},
    {AL("25GetMovieMakerNativeWindowEPPv"), album_GetMovieMakerNativeWindow},
};

void register_nnalbum_imports(void) {
    int n = (int)(sizeof album_imports / sizeof album_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(album_imports[i].name)) printf("nnalbum: \"%s\" is not an import of the game (typo?)\n", album_imports[i].name);
    register_imports(album_imports, n);
}

#define ST(label, cond) do { int ok_ = (cond); printf("  album: %s: %s\n", label, ok_ ? "ok" : "FAILED"); bad += !ok_; cases++; } while (0)
/* lists, pictures, streams, delete and the movie maker, in a folder of its own under %TEMP% */
static int album_selftest_files(Ctx *c) {
    int bad = 0, cases = 0;
    char saved_dir[MAX_PATH], tmp[MAX_PATH], path[MAX_PATH + 32];
    uint64_t saved_sp = c->x[31], stack[4];
    AlbumEntry list[4], forged;
    uint32_t w = 9, h = 9, n = 9;
    uint64_t user_size = 99, attr[2] = {7, 7}, handle = 0, got = 0, size = 0;
    uint8_t user_in[16], user_out[0x200], work[64];
    strcpy(saved_dir, g_album_dir);
    GetTempPathA(MAX_PATH, tmp);
    snprintf(g_album_dir, MAX_PATH, "%sssbu_album_selftest2_%lu", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(g_album_dir, NULL);
    uint8_t *px = malloc(1280 * 720 * 4), *out = malloc(1280 * 720 * 4), *block = malloc(0x40000);
    for (int i = 0; i < 1280 * 720; i++) { px[i * 4] = (uint8_t)(i % 1280); px[i * 4 + 1] = (uint8_t)(i / 1280); px[i * 4 + 2] = (uint8_t)(i >> 3); px[i * 4 + 3] = 0xff; }
    for (int i = 0; i < 16; i++) user_in[i] = (uint8_t)(0xa0 + i);

    c->x[0] = P(&n); c->x[1] = P(list); c->x[2] = 4; album_GetAlbumScreenshotFileList(c);
    int ok = c->x[0] == 0 && n == 0;
    n = 9; c->x[0] = P(&n); c->x[1] = P(list); c->x[2] = 4; album_GetExtraMovieFileList(c);
    ST("empty folder: both lists report 0 entries, success [sdk 0x4f320, 0x4f1a0]", ok && c->x[0] == 0 && n == 0);

    c->x[0] = P(px); c->x[1] = 1280 * 720 * 4; c->x[2] = 0; c->x[3] = 0; c->x[4] = 0; c->x[5] = P(user_in); c->x[6] = 16; album_SaveScreenshotWithUserData(c);
    ok = c->x[0] == 0;
    memset(list, 0xee, sizeof list);
    c->x[0] = P(&n); c->x[1] = P(list); c->x[2] = 4; album_GetAlbumScreenshotFileList(c);
    ImportFn created = import_lookup(AL("23GetAlbumFileCreatedTimeERKNS0_14AlbumFileEntryE"));
    uint64_t when = 0;
    if (created && n == 1) { c->x[0] = P(&list[0]); created(c); when = c->x[0]; }
    ST("a saved screenshot is listed: size, date read by the SDK's GetAlbumFileCreatedTime (lifted), content 0, byte 0x1f = 1",
       ok && n == 1 && list[0].size == 54 + 1280 * 720 * 4 && list[0].content == 0 && list[0].one == 1 && (when & 0xffff) == list[0].date.year &&
       ((when >> 16) & 0xff) == list[0].date.month && ((when >> 48) & 0xff) == list[0].date.second && list[1].size == 0xeeeeeeeeeeeeeeeeull);
    n = 9; c->x[0] = P(&n); c->x[1] = P(list + 1); c->x[2] = 3; album_GetExtraMovieFileList(c);
    ST("the screenshot is not in the extra movie list", n == 0);

    memset(work, 0x55, sizeof work); memset(out, 0, 1280 * 720 * 4);
    c->x[0] = P(&w); c->x[1] = P(&h); c->x[2] = P(out); c->x[3] = 1280 * 720 * 4; c->x[4] = P(work); c->x[5] = sizeof work; c->x[6] = P(&list[0]); album_Load7(c);
    ST("LoadAlbumImage: 1280x720, the saved pixels exactly, work memory cleared [sdk 0x4f670, 0xd9ad4]",
       c->x[0] == 0 && w == 1280 && h == 720 && !memcmp(out, px, 1280 * 720 * 4) && work[0] == 0 && work[63] == 0);
    w = h = 9; memset(user_out, 0, sizeof user_out);
    stack[0] = 0; stack[1] = P(&list[0]); c->x[31] = P(stack);
    c->x[0] = P(&w); c->x[1] = P(&h); c->x[2] = P(&user_size); c->x[3] = P(user_out); c->x[4] = 8; c->x[5] = P(out); c->x[6] = 1280 * 720 * 4; c->x[7] = 0; album_Load10(c);
    ST("LoadAlbumImage with user data: full size 16 reported, min(16, 8) bytes copied [sdk 0x4f9cc]",
       c->x[0] == 0 && w == 1280 && user_size == 16 && !memcmp(user_out, user_in, 8) && user_out[8] == 0);
    w = h = 9;
    c->x[0] = P(&w); c->x[1] = P(&h); c->x[2] = P(out); c->x[3] = 1280 * 720 * 4 - 1; c->x[4] = 0; c->x[5] = 0; c->x[6] = P(&list[0]); album_Load7(c);
    ST("LoadAlbumImage into a short buffer: 0xded6, sizes 0 [sdk 0x50b58 table]", c->x[0] == 0xded6 && w == 0 && h == 0);
    w = h = 9;
    c->x[0] = P(&w); c->x[1] = P(&h); c->x[2] = P(attr); c->x[3] = P(out); c->x[4] = 320 * 180 * 4; c->x[5] = P(work); c->x[6] = sizeof work; c->x[7] = P(&list[0]); album_LoadThumbAttr(c);
    unsigned mean = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) mean += px[(y * 1280 + x) * 4];
    ST("LoadAlbumThumbnailImage: 320x180, pixel 0 = mean of the 4x4 block, attribute {0, 0}",
       c->x[0] == 0 && w == 320 && h == 180 && out[0] == (mean + 8) / 16 && attr[0] == 0 && attr[1] == 0);
    forged = list[0]; forged.hash ^= 1;
    c->x[0] = P(&w); c->x[1] = P(&h); c->x[2] = P(out); c->x[3] = 1280 * 720 * 4; c->x[4] = 0; c->x[5] = 0; c->x[6] = P(&forged); album_Load7(c);
    ok = c->x[0] == 0xd6d6;
    forged = list[0]; forged.date.year = 1999; forged.hash = entry_hash(&forged.date, 0);
    c->x[6] = P(&forged); c->x[0] = P(&w); c->x[1] = P(&h); c->x[2] = P(out); c->x[3] = 1280 * 720 * 4; c->x[4] = 0; c->x[5] = 0; album_Load7(c);
    ST("an entry with a wrong hash / of a file that does not exist: 0xd6d6 [sdk 0x9fe760: description 23]", ok && c->x[0] == 0xd6d6);
    c->x[0] = P(&list[0]); album_DeleteExtraMovieFile(c);
    entry_base(path, MAX_PATH, &list[0].date); strcat(path, ".bmp");
    ST("DeleteExtraMovieFile on a screenshot entry: 0xd6d6, the file stays [sdk 0x4f2f4]", c->x[0] == 0xd6d6 && file_size(path) != ~0ull);
    ST("ConvertCapsrvResult [sdk 0x50aa0]: 21, 22, 25, 27, 14, 26, 3, other module",
       convert_capsrv_result(CAPS_RESULT(21)) == 0xdcd6 && convert_capsrv_result(CAPS_RESULT(22)) == 0xccd6 && convert_capsrv_result(CAPS_RESULT(25)) == 0xced6 &&
       convert_capsrv_result(CAPS_RESULT(27)) == 0xd2d6 && convert_capsrv_result(CAPS_RESULT(14)) == 0xd6d6 && convert_capsrv_result(CAPS_RESULT(26)) == 0xded6 &&
       convert_capsrv_result(CAPS_RESULT(3)) == 0xcad6 && convert_capsrv_result(0x1234) == 0xcad6 && convert_capsrv_result(CAPS_RESULT(24)) == 0xcad6);

    /* movie maker */
    uint8_t param[0x80];
    ImportFn ctor = import_lookup(AL("24MovieMakerMovieParameterC1Ev"));
    int16_t *pcm = calloc(48000 * 2, 2);
    for (int i = 0; i < 48000; i++) pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)((i % 109) * 300 - 16000);
    c->x[0] = P(param); if (ctor) ctor(c);
    g_mm.frame_source = 0;
    c->x[0] = 0x100000; album_PrecheckToStartMovieMaker(c);
    ok = c->x[0] == 0xcad6;
    c->x[0] = P(param); album_StartMovieMaker(c);
    ST("without a picture source the precheck and Start answer 0xcad6 (file header: OPEN)", ok && c->x[0] == 0xcad6 && ctor);
    c->x[0] = 0; c->x[1] = 0; album_InitializeMovieMaker(c);
    ok = c->x[0] == 0;
    album_IsMovieMakerRunning(c); ok = ok && c->x[0] == 0;
    album_CheckMovieMakerError(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(pcm); c->x[1] = 400; album_EncodeMovieMakerAudioSample(c); ok = ok && c->x[0] == 0xcad6;
    c->x[0] = P(pcm); c->x[1] = 0; album_EncodeMovieMakerAudioSample(c);
    ST("InitializeMovieMaker: success, not running, no error; audio before Start 0xcad6, 0 bytes -> success [sdk 0x51b58]", ok && c->x[0] == 0);
    album_movie_frame_source_attach();
    c->x[0] = 0x100000; album_PrecheckToStartMovieMaker(c); ok = c->x[0] == 0;
    c->x[0] = ~0ull >> 1; album_PrecheckToStartMovieMaker(c); ok = ok && c->x[0] == 0xcad6;
    c->x[0] = P(param); album_StartMovieMaker(c); ok = ok && c->x[0] == 0;
    album_IsMovieMakerRunning(c); ok = ok && c->x[0] == 1;
    c->x[0] = P(&handle); album_GetMovieMakerNativeWindow(c);
    ST("with a picture source: precheck, Start, running [sdk 0x5175c], native window = album_movie_native_window()", ok && c->x[0] == 0 && handle && handle == album_movie_native_window());
    ok = 1;
    for (int f = 0; f < 30; f++) {
        for (int i = 0; i < 640 * 360; i++) out[i * 4] = (uint8_t)(i + f * 8);
        ok = ok && !album_movie_push_frame(out, 640, 360, 640 * 4, f & 1);
        c->x[0] = P(pcm + f * 1600 * 2); c->x[1] = 1600 * 4; album_EncodeMovieMakerAudioSample(c); ok = ok && c->x[0] == 0;
    }
    album_CheckMovieMakerError(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(user_in); c->x[1] = 16; c->x[2] = P(px); c->x[3] = 1280 * 720 * 4; c->x[4] = 0; album_FinishMovieMaker5(c);
    ok = ok && c->x[0] == 0;
    album_IsMovieMakerRunning(c);
    ST("30 pictures (scaled from 640x360) + 1 s of PCM encoded, FinishMovieMaker with user data and thumbnail, not running afterwards [sdk 0x5196c]", ok && c->x[0] == 0);
    n = 9; c->x[0] = P(&n); c->x[1] = P(list); c->x[2] = 4; album_GetExtraMovieFileList(c);
    ST("the movie is in the extra movie list (content 3, more than 1000 bytes)", n == 1 && list[0].content == 3 && list[0].size > 1000 && list[0].size != ~0ull);
    w = h = 9; user_size = 0; memset(user_out, 0, sizeof user_out); attr[0] = attr[1] = 7;
    stack[0] = 0; stack[1] = 0; stack[2] = P(&list[0]); c->x[31] = P(stack);
    c->x[0] = P(&w); c->x[1] = P(&h); c->x[2] = P(attr); c->x[3] = P(&user_size); c->x[4] = P(user_out); c->x[5] = 0x200; c->x[6] = P(out); c->x[7] = 320 * 180 * 4; album_LoadThumbAttrUser(c);
    ST("the movie's thumbnail (Finish's picture, 320x180) and user data load", n == 1 && c->x[0] == 0 && w == 320 && h == 180 && out[0] == (mean + 8) / 16 && user_size == 16 && !memcmp(user_out, user_in, 16));
    c->x[0] = P(&handle); c->x[1] = P(&list[0]); album_OpenAlbumMovieStream(c);
    ok = n == 1 && c->x[0] == 0 && handle;
    c->x[0] = P(&size); c->x[1] = handle; album_GetAlbumMovieStreamSize(c); ok = ok && c->x[0] == 0 && size == list[0].size;
    memset(block, 0x77, 0x40000);
    c->x[0] = P(&got); c->x[1] = P(block); c->x[2] = 0x40000; c->x[3] = handle; c->x[4] = 0; album_ReadAlbumMovieStream(c);
    ok = ok && c->x[0] == 0 && got == (size < 0x40000 ? size : 0x40000) && !memcmp(block + 4, "ftyp", 4) && (size >= 0x40000 || block[0x3ffff] == 0);
    c->x[0] = P(&got); c->x[1] = P(block); c->x[2] = 0x1000; c->x[3] = handle; c->x[4] = 0x40000000; album_ReadAlbumMovieStream(c);
    ok = ok && c->x[0] == 0 && got == 0 && block[0] == 0;
    c->x[0] = handle; album_CloseAlbumMovieStream(c);
    c->x[0] = P(&size); c->x[1] = handle; album_GetAlbumMovieStreamSize(c);
    ST("movie stream: open, size = entry size, block read starts with an MP4 'ftyp' box, tail zero-filled, read past the end 0 bytes, closed handle refused", ok && c->x[0] == 0xd6d6);
    c->x[0] = P(&list[0]); album_DeleteExtraMovieFile(c); ok = c->x[0] == 0;
    c->x[0] = P(&list[0]); album_DeleteExtraMovieFile(c); ok = ok && c->x[0] == 0xd6d6;
    n = 9; c->x[0] = P(&n); c->x[1] = P(list); c->x[2] = 4; album_GetExtraMovieFileList(c);
    ST("DeleteExtraMovieFile: success, a second time 0xd6d6, list empty again", ok && n == 0);
    c->x[0] = P(param); album_StartMovieMaker(c); ok = c->x[0] == 0;
    album_AbortMovieMaker(c);
    album_IsMovieMakerRunning(c); ok = ok && c->x[0] == 0;
    c->x[0] = 0; c->x[1] = 0; album_FinishMovieMaker2(c); ok = ok && c->x[0] == 0xcad6;
    album_FinalizeMovieMaker(c);
    snprintf(path, sizeof path, "%s\\recording.tmp.mp4", g_album_dir);
    ST("AbortMovieMaker: not running, nothing left in the folder, Finish afterwards 0xcad6", ok && file_size(path) == ~0ull && !g_mm.initialized);
    g_mm.frame_source = 0;

    /* remove what the test wrote (its own files only) */
    c->x[0] = P(&n); c->x[1] = P(list); c->x[2] = 4; album_GetAlbumScreenshotFileList(c);
    for (uint32_t i = 0; i < n; i++) {
        char base[MAX_PATH];
        entry_base(base, sizeof base, &list[i].date);
        snprintf(path, sizeof path, "%s.bmp", base); DeleteFileA(path);
        snprintf(path, sizeof path, "%s.userdata.bin", base); DeleteFileA(path);
    }
    RemoveDirectoryA(g_album_dir);
    strcpy(g_album_dir, saved_dir);
    c->x[31] = saved_sp;
    free(px); free(out); free(block); free(pcm);
    printf("  album: %d cases, %d failed\n", cases, bad);
    return bad;
}

int nnalbum_selftest(Ctx *c) {
    int bad = 0, ok;
    /* MovieMakerMovieParameter (lifted): 0x80 bytes ([sdk] 0x516a4 StartMovieMaker copies 0x80) */
    uint8_t param[0x100];
    memset(param, 0xee, sizeof param);
    ImportFn ctor = import_lookup(AL("24MovieMakerMovieParameterC1Ev")), set = import_lookup(AL("24MovieMakerMovieParameter15SetAudioBitRateEi")),
             get = import_lookup("_ZNK2nn5album24MovieMakerMovieParameter15GetAudioBitRateEv"), size = import_lookup(AL("35GetMovieMakerRequiredWorkMemorySizeEv"));
    if (!ctor || !set || !get || !size) { printf("  album: lifted MovieMakerMovieParameter functions are missing: FAILED\n"); return 1; }
    c->x[0] = P(param); ctor(c);
    c->x[0] = P(param); get(c);
    uint32_t default_rate = (uint32_t)c->x[0];
    c->x[0] = P(param); c->x[1] = 96000; set(c);
    c->x[0] = P(param); get(c);
    ok = c->x[0] == 96000;
    for (int i = 0x80; i < 0x100; i++) if (param[i] != 0xee) ok = 0;
    size(c);
    printf("  album: MovieMakerMovieParameter (lifted): default audio bit rate %u, set/get 96000, inside 0x80 bytes; work memory 0x%llx: %s\n",
           default_rate, (unsigned long long)c->x[0], ok ? "ok" : "FAILED");
    bad += !ok;
    /* screenshot: a 1280x720 picture round-trips through the BMP writer */
    char saved_dir[MAX_PATH], tmp[MAX_PATH], pattern[MAX_PATH];
    strcpy(saved_dir, g_album_dir);
    GetTempPathA(MAX_PATH, tmp);
    snprintf(g_album_dir, MAX_PATH, "%sssbu_album_selftest", tmp);
    uint8_t *px = malloc(1280 * 720 * 4);
    for (int i = 0; i < 1280 * 720; i++) { px[i * 4] = (uint8_t)i; px[i * 4 + 1] = (uint8_t)(i >> 8); px[i * 4 + 2] = (uint8_t)(i >> 16); px[i * 4 + 3] = 0xff; }
    c->x[0] = P(px); c->x[1] = 1280 * 720 * 4; c->x[2] = 0; c->x[3] = 0; album_SaveScreenshot(c);
    ok = c->x[0] == 0;
    snprintf(pattern, MAX_PATH, "%s\\*.bmp", g_album_dir);
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pattern, &fd);
    if (hf == INVALID_HANDLE_VALUE) ok = 0;
    else {
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%s\\%s", g_album_dir, fd.cFileName);
        FindClose(hf);
        FILE *f = fopen(path, "rb");
        uint8_t head[54], last_row[16];
        if (!f || fread(head, 1, 54, f) != 54 || fread(last_row, 1, 16, f) != 16) ok = 0;
        else {
            /* the file's first row is the picture's last row; pixel (0, 719) = index 719 * 1280 */
            int idx = 719 * 1280;
            ok = ok && head[0] == 'B' && *(int32_t *)(head + 18) == 1280 && *(int32_t *)(head + 22) == 720 &&
                 last_row[0] == (uint8_t)(idx >> 16) && last_row[1] == (uint8_t)(idx >> 8) && last_row[2] == (uint8_t)idx && last_row[3] == 0xff;
        }
        if (f) fclose(f);
        DeleteFileA(path);
    }
    RemoveDirectoryA(g_album_dir);
    c->x[0] = P(px); c->x[1] = 1000; c->x[2] = 0; c->x[3] = 0; album_SaveScreenshot(c);
    ok = ok && c->x[0] != 0;
    strcpy(g_album_dir, saved_dir);
    free(px);
    printf("  album: SaveScreenshot writes a correct 1280x720 BMP and refuses a wrong size: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    bad += album_selftest_files(c);
    return bad;
}
