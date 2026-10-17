/* Round 17 (track V session 17), TEST ONLY: the inputs of every draw of ONE tagged game frame, printed the same way by both
 * back ends, so that a draw that paints wrongly in Vulkan can be compared with the same draw in OpenGL.
 *   SSBU_DRAWTAG=1 with SSBU_SHOT_FIGHT=<n>[,..]: the submissions of the game frame whose present is saved as fight_<n>.bmp
 *   are tagged (nvn_gl.c queue_submit, on the game's thread); every draw command of them prints one "DT" line:
 *   command data, hashes of the bytes in GUEST memory at the moment the back end prepares the draw (index range, every
 *   vertex stream an attribute names, every uniform / storage range), the program codes' hashes, the vertex layout.
 *   SSBU_DRAWTAG_DUMP=<i>[,<i>..]: the raw bytes of those draw numbers go to files dt_<back end>_<fight>_<draw>_*.bin in
 *   SSBU_SHOT_DIR. Nothing here runs unless SSBU_DRAWTAG is set. No game content. */
#ifndef NVN_DT_H
#define NVN_DT_H
static uint64_t dt_fnv(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 0xcbf29ce484222325ULL; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }
static int dt_dump_wanted(int idx) {
    static int list[64], n = -1;
    if (n < 0) { const char *e = getenv("SSBU_DRAWTAG_DUMP"); n = 0; while (e && *e && n < 64) { list[n++] = atoi(e); e = strchr(e, ','); if (e) e++; } }
    for (int i = 0; i < n; i++) if (list[i] == idx) return 1;
    return 0;
}
static void dt_dump(const char *be, int fight, int idx, const char *what, int a, int b, const void *p, size_t n) {
    char path[400];
    const char *dir = getenv("SSBU_SHOT_DIR");
    snprintf(path, sizeof path, "%s%sdt_%s_%05d_%04d_%s%d_%d.bin", dir ? dir : "", dir ? "/" : "", be, fight, idx, what, a, b);
    FILE *fp = fopen(path, "wb");
    if (fp) { fwrite(p, 1, n, fp); fclose(fp); }
}
/* host: guest address -> host pointer (the back end's P()). sd: stride / divisor pairs of the 32 streams. */
static void dt_line(const char *be, int fight, int idx, int ok, unsigned prim, int itype, int count, uint64_t ib, long long first_or_basev, unsigned binst, unsigned inst,
                    Prog *const *prog, int nattr, const uint32_t *vattr, const Range *vbuf, const uint32_t *sd, const Range (*ubo)[16], const Range (*ssbo)[32], void *(*host)(uint64_t)) {
    int dump = dt_dump_wanted(idx);
    printf("DT %s %d %d ok=%d prim=%u itype=%d count=%d first_or_basev=%lld binst=%u inst=%u", be, fight, idx, ok, prim, itype, count, first_or_basev, binst, inst);
    if (itype >= 0 && ib && count > 0) {
        const uint8_t *p = host(ib);
        size_t n = (size_t)count << itype;
        uint32_t lo = 0xffffffffu, hi = 0, nmax = 0, maxv = itype == 0 ? 0xffu : itype == 1 ? 0xffffu : 0xffffffffu;
        for (int i = 0; i < count; i++) {
            uint32_t v = itype == 0 ? p[i] : itype == 1 ? ((const uint16_t *)p)[i] : ((const uint32_t *)p)[i];
            if (v == maxv) { nmax++; continue; }
            if (v < lo) lo = v; if (v > hi) hi = v;
        }
        printf(" ib=%llx+%llu:%016llx idx_min=%u idx_max=%u idx_restart_values=%u", (unsigned long long)ib, (unsigned long long)n, (unsigned long long)dt_fnv(p, n), lo, hi, nmax);
        if (dump) dt_dump(be, fight, idx, "ib", itype, count, p, n);
    }
    for (int s = 0; s < 2; s++)
        if (prog[s]) printf(" %s=#%d:%d:%016llx", s ? "fs" : "vs", prog[s]->serial, prog[s]->st[s].code_len, (unsigned long long)(prog[s]->st[s].code && prog[s]->st[s].code_len > 0 ? dt_fnv(prog[s]->st[s].code, (size_t)prog[s]->st[s].code_len) : 0));
    printf(" | attrs");
    uint32_t streams = 0;
    for (int i = 0; i < 16 && i < nattr; i++) if (vattr[i] & 0xff) { printf(" %d:0x%x/s%u/+%u", i, vattr[i] & 0xff, (vattr[i] >> 8) & 31, vattr[i] >> 14); streams |= 1u << ((vattr[i] >> 8) & 31); }
    printf(" | streams");
    for (int i = 0; i < 16; i++)
        if ((streams >> i & 1) && vbuf[i].addr && vbuf[i].size) {
            const void *p = host(vbuf[i].addr);
            printf(" s%d=%llx+%llu/stride%u/div%u:%016llx", i, (unsigned long long)vbuf[i].addr, (unsigned long long)vbuf[i].size, sd[i * 2], sd[i * 2 + 1], (unsigned long long)dt_fnv(p, (size_t)vbuf[i].size));
            if (dump) dt_dump(be, fight, idx, "vb", i, (int)sd[i * 2], p, (size_t)vbuf[i].size);
        } else if (streams >> i & 1) printf(" s%d=none", i);
    printf(" | ubo");
    for (int s = 0; s < 2; s++) for (int n = 0; n < 16; n++)
        if (prog[s] && ubo[s][n].addr && ubo[s][n].size) {
            const void *p = host(ubo[s][n].addr);
            printf(" %d.%d=%llx+%llu:%016llx", s, n, (unsigned long long)ubo[s][n].addr, (unsigned long long)ubo[s][n].size, (unsigned long long)dt_fnv(p, (size_t)ubo[s][n].size));
            if (dump) dt_dump(be, fight, idx, s ? "fubo" : "vubo", n, (int)ubo[s][n].size, p, (size_t)ubo[s][n].size);
        }
    printf(" | ssbo");
    for (int s = 0; s < 2; s++) for (int n = 0; n < 32; n++)
        if (prog[s] && ssbo[s][n].addr && ssbo[s][n].size) printf(" %d.%d=%llx+%llu:%016llx", s, n, (unsigned long long)ssbo[s][n].addr, (unsigned long long)ssbo[s][n].size, (unsigned long long)dt_fnv(host(ssbo[s][n].addr), (size_t)ssbo[s][n].size));
    printf("\n");
}
#endif
