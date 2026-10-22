/* Track O (options), first set: config\graphics.ini. See gfx_options.c and plan\phases\O1_options.md. */
#ifndef GFX_OPTIONS_H
#define GFX_OPTIONS_H
typedef struct {
    char path[260];
    int renderer;            /* 0 Vulkan (default), 1 OpenGL                               env SSBU_RENDERER=vk|gl wins */
    int window_mode;         /* 0 windowed, 1 borderless (desktop size), 2 fullscreen (borderless, topmost, covering the monitor) */
    int width, height;       /* client size of the windowed mode */
    int present;             /* 0 clock (mailbox, else immediate, paced by our 60 Hz clock), 1 vsync on (fifo), 2 vsync off (immediate), 3 mailbox;   env SSBU_VK_PRESENT wins */
    char gpu[64];            /* "" = discrete first; an index of the printed list, or a part of the device name   env SSBU_VK_DEVICE wins */
    float scale;             /* internal resolution scale asked for (Vulkan: nvn_vk_draw.c rs_scale, 0.25 .. 2.0 since PR #8 (stephenjayakar); env SSBU_VK_SCALE wins) */
    int handheld;            /* 1: the game is told it runs handheld = it makes its own 1280x720 render targets instead of 1920x1080 */
    int aniso;               /* 0 = the game's own value; 2, 4, 8, 16 = at least that on samplers that filter between mip levels */
    int overlay;             /* 1: frame time in the title bar and a "frame-time:" log line every second */
    int shader_skip;         /* shader_wait: 0 hold (a draw waits for its shader or pipeline: the frame is late), 1 skip (near a fight the draw is left out until they are ready; nvn_vk_m4.c skip_on; env SSBU_VK_NOHOLD wins) */
    int profile;            /* 0 custom (the keys as written), 1 low: scale, aniso and present are set by the profile (gfx_options.c) */
    int shader_prepare;     /* track V session 14 (appended: older objects keep their offsets): 0 first_run (the game is held at its first frame until every shader of the game's data is prepared, once per GPU + driver), 1 background (as before), 2 off;  env SSBU_VK_PREPARE wins */
    int aspect_stretch;     /* [Recomps#1] aspect = keep (0, default: the picture keeps its shape, black bars on a wider or taller
                               window, e.g. an ultrawide monitor) | stretch (1: it fills the window); nvn_vk_draw.c present (Vulkan) */
    int frame_rate;         /* [Recomps#1] frame_rate = 60 (the game as made) .. 120: above 60 the lifted frame-rate patch is on, capped there */
    int hdr;                /* [Recomps#1] hdr = auto (0: HDR output while Windows HDR is on for the window's monitor) | off (1) | on (2: always);
                               nvn_vk_draw.c hdr_* (Vulkan on Windows; finding 147) */
    int hdr_scrgb;          /* [Recomps#1] hdr_format = hdr10 (0: 10-bit PQ, BT.2020) | scrgb (1: 16-bit floating point, linear) */
    float hdr_paper_white;  /* [Recomps#1] hdr_paper_white: the game's white in nits (80 .. 1000); 0 = auto: Windows' SDR content brightness */
    int hdr_srgb;           /* [Recomps#1] hdr_gamma = 2.2 (0: the game's values read as an SDR monitor shows them) | srgb (1: as Windows shows SDR apps) */
} GfxOptions;
const GfxOptions *gfx_options(void);      /* any thread; read once (the file is written with defaults when it does not exist) */
int gfx_pace_rate(void);                  /* [Recomps#1] the rate display.c paces the game at: 60, or frame_rate above 60 (SSBU_PACE_120=0: 60 and the lifted limiter) */
#endif
