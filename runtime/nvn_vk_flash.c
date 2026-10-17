/* Track V session 13: the FLASH DETECTOR (included by nvn_vk_draw.c, render thread only). The owner reported "sometimes
 * white flashes on screen" from real play. Every presented frame the window texture is blitted a second time into a
 * 16x9 image, copied into a small host-visible buffer, and read 8 presents later (the command buffer that wrote it has
 * been waited for by then: there are 8 in flight). Cost: one blit of 144 texels and one copy of 576 bytes per frame.
 * A FLASH = 1 to 3 frames whose mean brightness is far above the frame before them AND near white (or more than half
 * of the 144 cells near white), followed by a frame back near the old level. Each one is printed ("nvn_vk_flash:")
 * and appended to <exe>_flash.txt next to the exe (at most 40 a run) with, for the frames around it: draws, passes,
 * clears, textures uploaded, pipelines linked / made at a draw, holds, images made, extra presents of a held loading
 * screen, the layout the window texture was in before the present blit (0 = never written: undefined content), the
 * list of the passes (size, format, window texture or not, draws in it) and the 16x9 brightness map.
 * SSBU_VK_FLASHLOG=0 off; =2 also reports every jump to near white that does NOT come back within 3 frames (a scene
 * change to a white screen is the game's own; for diagnosis). Default 1. Nothing here changes what is drawn. */
#define FL_W 16
#define FL_H 9
#define FL_RING 8
#define FL_HIST 6
#define FL_PASSES 48
typedef struct {
    LONG64 frame; int valid, draws, clears, npass, uploads, dlinks, holds, pipes, mods, images, fight, extra, layout, srgb, nowin, skipped;
    struct { unsigned short w, h, fmt, draws; unsigned char win, ncol, depth; } pass[FL_PASSES];
    unsigned char lum[FL_W * FL_H]; int mean, white;
} FlFrame;
static struct {
    int on, made, n, reports, hn;
    VkImage img; VMem imem; VkBuffer buf[FL_RING]; VMem bmem[FL_RING];
    FlFrame ring[FL_RING];                      /* by present number % FL_RING: waiting for its pixels */
    FlFrame hist[FL_HIST];                      /* the last frames analysed, oldest first */
    FlFrame cur;                                /* the frame being drawn */
    LONG64 up0, dl0, hold0, pipe0, mod0, img0, extra0;
    LONG64 jumps, flashes;
} g_fl = {.on = -1};

static int fl_on(void) { if (g_fl.on < 0) { const char *e = getenv("SSBU_VK_FLASHLOG"); g_fl.on = e ? atoi(e) : 1; } return g_fl.on; }
/* pass_begin: one entry per pass of the frame being drawn */
static void fl_pass(int w, int h, int fmt, int win, int ncol, int depth) {
    if (!fl_on()) return;
    int i = g_fl.cur.npass++;
    if (i >= FL_PASSES) return;
    g_fl.cur.pass[i].w = (unsigned short)w; g_fl.cur.pass[i].h = (unsigned short)h; g_fl.cur.pass[i].fmt = (unsigned short)fmt; g_fl.cur.pass[i].win = (unsigned char)win;
    g_fl.cur.pass[i].ncol = (unsigned char)ncol; g_fl.cur.pass[i].depth = (unsigned char)depth; g_fl.cur.pass[i].draws = (unsigned short)R.frame_draws;
}
static void fl_print(FILE *o, const FlFrame *f, const char *what) {
    fprintf(o, "  %s frame %lld: brightness %d of 255, %d of 144 cells near white | %d draws, %d passes, %d clears, %d draws left out | %d textures uploaded, %d pipelines linked at a draw, %d holds, %d pipelines and %d modules made, %d images made | %s, %d extra presents of a held loading screen before it | window texture: %s, layout before the blit %d (0 = never written)%s\n",
            what, (long long)f->frame, f->mean, f->white, f->draws, f->npass, f->clears, f->skipped, f->uploads, f->dlinks, f->holds, f->pipes, f->mods, f->images, f->fight ? "FIGHT frame" : "not a fight frame", f->extra,
            f->nowin ? "NONE (black is presented)" : "present", f->layout, f->srgb ? ", sRGB" : "");
    fprintf(o, "    passes (size, format, W = window texture, colour targets, D = depth, first draw number):");
    for (int i = 0; i < f->npass && i < FL_PASSES; i++) fprintf(o, " %dx%d/0x%x%s/%d%s@%d", f->pass[i].w, f->pass[i].h, f->pass[i].fmt, f->pass[i].win ? "/W" : "", f->pass[i].ncol, f->pass[i].depth ? "D" : "", f->pass[i].draws);
    fprintf(o, "\n");
    for (int y = 0; y < FL_H; y++) { fprintf(o, "    "); for (int x = 0; x < FL_W; x++) fprintf(o, "%02x ", f->lum[y * FL_W + x]); fprintf(o, "\n"); }
}
static void fl_report(int first, int count, int came_back) {
    char path[MAX_PATH + 16];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    FILE *fp = NULL;
    SYSTEMTIME st; GetLocalTime(&st);
    if (g_fl.reports >= 40) return;
    g_fl.reports++;
    if (n > 4) { strcpy(path + n - 4, "_flash.txt"); fp = fopen(path, "ab"); }
    for (int pass = 0; pass < 2; pass++) {
        FILE *o = pass ? fp : stdout;
        if (!o) continue;
        fprintf(o, "nvn_vk_flash: %s at frame %lld: %d frame(s) near white, brightness %d -> %d%s (%04d-%02d-%02d %02d:%02d:%02d)\n", came_back ? "WHITE FLASH" : "jump to near white (did not come back within 3 frames)",
                (long long)g_fl.hist[first].frame, count, g_fl.hist[first - 1].mean, g_fl.hist[first].mean, came_back ? " and back" : "", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        for (int i = first - 1; i < g_fl.hn; i++) fl_print(o, &g_fl.hist[i], i < first ? "before" : i < first + count ? "WHITE " : "after ");
        fflush(o);
    }
    if (fp) fclose(fp);
}
static int fl_bright(const FlFrame *a, const FlFrame *base) { return a->mean >= base->mean + 70 && (a->mean >= 170 || a->white >= 72); }
static void fl_analyse(FlFrame *f, const uint8_t *px) {
    int sum = 0, white = 0;
    static unsigned char gam[256]; static int gam_made;
    if (!gam_made) { for (int i = 0; i < 256; i++) { double v = pow((double)i / 255.0, 1.0 / 2.2) * 255.0 + 0.5; gam[i] = (unsigned char)(v > 255 ? 255 : v); } gam_made = 1; }
    for (int i = 0; i < FL_W * FL_H; i++) {
        int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
        if (f->srgb) { r = gam[r]; g = gam[g]; b = gam[b]; }          /* the 16x9 image is UNORM: a blit from an sRGB texture stored linear values */
        int l = (r * 2 + g * 5 + b) >> 3, mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        f->lum[i] = (unsigned char)l; sum += l; if (mn >= 225) white++;
    }
    f->mean = sum / (FL_W * FL_H); f->white = white;
    if (g_fl.hn == FL_HIST) { memmove(&g_fl.hist[0], &g_fl.hist[1], sizeof(FlFrame) * (FL_HIST - 1)); g_fl.hn--; }
    g_fl.hist[g_fl.hn++] = *f;
    int last = g_fl.hn - 1;
    /* hist[last] is the newest. A flash of k frames: hist[last-k .. last-1] bright against hist[last-k-1], hist[last] back */
    for (int k = 1; k <= 3; k++) {
        int base = last - k - 1, all = 1;
        if (base < 0) break;
        if (base > 0 && fl_bright(&g_fl.hist[base], &g_fl.hist[base - 1])) continue;      /* the run began earlier: counted with its true length */
        for (int i = base + 1; i < last; i++) all &= fl_bright(&g_fl.hist[i], &g_fl.hist[base]);
        if (all && g_fl.hist[last].mean <= g_fl.hist[base].mean + 30) { g_fl.flashes++; fl_report(base + 1, k, 1); break; }
    }
    if (g_fl.on >= 2 && g_fl.hn >= 5 && fl_bright(&g_fl.hist[last - 3], &g_fl.hist[last - 4]) && fl_bright(&g_fl.hist[last - 2], &g_fl.hist[last - 4]) && fl_bright(&g_fl.hist[last - 1], &g_fl.hist[last - 4]) && fl_bright(&g_fl.hist[last], &g_fl.hist[last - 4])
        && !(last >= 5 && fl_bright(&g_fl.hist[last - 4], &g_fl.hist[last - 5]))) { g_fl.jumps++; fl_report(last - 3, 4, 0); }
}
/* vk_present2, inside the command buffer of the present, right after the blit to the swapchain image. vt is in TRANSFER_SRC. */
static void fl_capture(VTex *vt, const int *s4, LONG64 frame, int layout_before) {
    if (!fl_on()) return;
    if (!g_fl.made) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent.width = FL_W; ci.extent.height = FL_H; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKC(vkCreateImage(V.device, &ci, NULL, &g_fl.img));
        vkGetImageMemoryRequirements(V.device, g_fl.img, &rq);
        g_fl.imem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, g_fl.img, g_fl.imem.mem, g_fl.imem.off));
        for (int i = 0; i < FL_RING; i++) { g_fl.buf[i] = vbuf_new(FL_W * FL_H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_fl.bmem[i]); if (g_fl.bmem[i].map) memset(g_fl.bmem[i].map, 0, FL_W * FL_H * 4); }
        g_fl.made = 1;
        printf("nvn_vk_flash: detector on: a 16x9 copy of every presented frame is read 8 frames later; a white flash of 1-3 frames is reported here and in <exe>_flash.txt (SSBU_VK_FLASHLOG=0 off, 2 = also jumps that stay)\n");
    }
    int k = g_fl.n % FL_RING;
    if (g_fl.ring[k].valid && g_fl.bmem[k].map) fl_analyse(&g_fl.ring[k], (const uint8_t *)g_fl.bmem[k].map);
    {   /* this frame's record */
        LONG64 holds = 0;
        for (int i = 0; i < H_N; i++) holds += g_hold_n[i];
        FlFrame *c = &g_fl.cur;
        c->frame = frame; c->valid = 1; c->draws = (int)R.frame_draws; c->clears = (int)R.frame_clears; c->skipped = (int)R.frame_skipped; c->fight = g_in_fight;
        c->uploads = (int)(R.uploads - g_fl.up0); c->dlinks = (int)(g_gpl_dlink - g_fl.dl0); c->holds = (int)(holds - g_fl.hold0); c->pipes = (int)(R.pipelines - g_fl.pipe0); c->mods = (int)(R.modules - g_fl.mod0); c->images = (int)(R.images - g_fl.img0);
        c->extra = (int)(g_ovl_presents - g_fl.extra0);
        c->layout = layout_before; c->nowin = vt == NULL; c->srgb = vt && (vt->fmt == VK_FORMAT_R8G8B8A8_SRGB || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB);
        g_fl.up0 = R.uploads; g_fl.dl0 = g_gpl_dlink; g_fl.hold0 = holds; g_fl.pipe0 = R.pipelines; g_fl.mod0 = R.modules; g_fl.img0 = R.images; g_fl.extra0 = g_ovl_presents;
        g_fl.ring[k] = *c;
        memset(c, 0, sizeof *c);
    }
    image_barrier(g_fl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (vt && vt->samples == 1) {
        VkImageBlit b;
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
        b.srcOffsets[0].x = s4[0]; b.srcOffsets[0].y = s4[1]; b.srcOffsets[1].x = s4[0] + s4[2]; b.srcOffsets[1].y = s4[1] + s4[3]; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = FL_W; b.dstOffsets[1].y = FL_H; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    } else {
        VkClearColorValue z; VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        memset(&z, 0, sizeof z);
        vkCmdClearColorImage(R.cmd, g_fl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
    }
    image_barrier(g_fl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    {
        VkBufferImageCopy c;
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent.width = FL_W; c.imageExtent.height = FL_H; c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(R.cmd, g_fl.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fl.buf[k], 1, &c);
    }
    g_fl.n++;
}
static void fl_shutdown(void) {
    if (!g_fl.made) return;
    printf("nvn_vk_flash: %lld presented frames looked at, %lld WHITE FLASHES (1-3 frames near white and back), %lld jumps that stayed (counted only with SSBU_VK_FLASHLOG=2)\n", (long long)g_fl.n, (long long)g_fl.flashes, (long long)g_fl.jumps);
    for (int i = 0; i < FL_RING; i++) { vkDestroyBuffer(V.device, g_fl.buf[i], NULL); vmem_free(g_fl.bmem[i]); }
    vkDestroyImage(V.device, g_fl.img, NULL); vmem_free(g_fl.imem);
    g_fl.made = 0;
}
