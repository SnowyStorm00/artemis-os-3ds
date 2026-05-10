

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <math.h>

#include "font8x8.h"
#include "buildinfo.h"

#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240

#define SFX_RATE      32000
#define SFX_CHANNELS  4
#define SFX_CHANNEL0  0

typedef enum {
    SFX_STARTUP = 0,
    SFX_CLICK,
    SFX_DING,
    SFX_ERROR,
    SFX_FLAG,
    SFX_REVEAL,
    SFX_SHUTDOWN,
    SFX_COUNT
} SfxId;

typedef struct {
    int16_t *data;
    int      nsamples;
} SfxClip;

static SfxClip     g_sfx[SFX_COUNT];
static ndspWaveBuf g_sfx_wb[SFX_CHANNELS];
static int         g_sfx_next_ch = 0;
static int         g_audio_ready = 0;
static char        g_audio_status[64] = "not init";

static void sfx_tone(int16_t *out, int nsamples, int offset,
                     double freq, double amp, int wave) {
    double two_pi = 6.28318530717958647692;
    for (int i = 0; i < nsamples; i++) {
        double t = (double)i / (double)SFX_RATE;
        double env;

        double attack = 0.005;
        if (t < attack) env = t / attack;
        else            env = exp(-(t - attack) * 4.0);
        double v = 0.0;
        if (freq > 0.0) {
            double phase = two_pi * freq * t;
            if (wave == 0)       v = sin(phase);
            else if (wave == 1)  v = (sin(phase) >= 0.0) ? 1.0 : -1.0;
            else                 v = 2.0 / 3.14159265 * asin(sin(phase));
        }
        double s = v * env * amp;
        if (s >  1.0) s =  1.0;
        if (s < -1.0) s = -1.0;
        int16_t sample = (int16_t)(s * 30000.0);
        out[offset + i] += sample;
    }
}

static int16_t *sfx_build_sweep(const double *notes, int count, int per_ms,
                                double amp, int wave, int *out_n) {
    int per_samples = (SFX_RATE * per_ms) / 1000;
    int total = per_samples * count;
    int16_t *buf = (int16_t *)linearAlloc(total * sizeof(int16_t));
    if (!buf) { *out_n = 0; return NULL; }
    memset(buf, 0, total * sizeof(int16_t));
    for (int i = 0; i < count; i++) {
        sfx_tone(buf, per_samples, i * per_samples, notes[i], amp, wave);
    }
    *out_n = total;
    return buf;
}

static int16_t *sfx_build_blip(double freq, int ms, double amp, int wave, int *out_n) {
    int n = (SFX_RATE * ms) / 1000;
    int16_t *buf = (int16_t *)linearAlloc(n * sizeof(int16_t));
    if (!buf) { *out_n = 0; return NULL; }
    memset(buf, 0, n * sizeof(int16_t));
    sfx_tone(buf, n, 0, freq, amp, wave);
    *out_n = n;
    return buf;
}

static int16_t *sfx_build_chord(const double *notes, int count, int ms,
                                double amp, int wave, int *out_n) {
    int n = (SFX_RATE * ms) / 1000;
    int16_t *buf = (int16_t *)linearAlloc(n * sizeof(int16_t));
    if (!buf) { *out_n = 0; return NULL; }
    memset(buf, 0, n * sizeof(int16_t));
    for (int i = 0; i < count; i++) {
        sfx_tone(buf, n, 0, notes[i], amp / count, wave);
    }
    *out_n = n;
    return buf;
}

static void sfx_build_all(void) {
    int n;

    {
        double a[] = { 523.25, 659.25 };
        double b[] = { 659.25, 783.99, 987.77 };
        int na, nb;
        int16_t *ba = sfx_build_chord(a, 2, 220, 0.45, 2, &na);
        int16_t *bb = sfx_build_chord(b, 3, 420, 0.45, 2, &nb);
        int total = na + nb;
        int16_t *buf = (int16_t *)linearAlloc(total * sizeof(int16_t));
        if (buf) {
            memset(buf, 0, total * sizeof(int16_t));
            memcpy(buf, ba, na * sizeof(int16_t));
            memcpy(buf + na, bb, nb * sizeof(int16_t));
        }
        if (ba) linearFree(ba);
        if (bb) linearFree(bb);
        g_sfx[SFX_STARTUP].data = buf;
        g_sfx[SFX_STARTUP].nsamples = total;
    }

    g_sfx[SFX_CLICK].data = sfx_build_blip(1760.0, 35, 0.18, 1, &n);
    g_sfx[SFX_CLICK].nsamples = n;

    {
        double notes[] = { 1046.5, 1318.5, 1568.0 };
        g_sfx[SFX_DING].data = sfx_build_sweep(notes, 3, 80, 0.30, 2, &n);
        g_sfx[SFX_DING].nsamples = n;
    }

    {
        double notes[] = { 196.0, 207.65 };
        g_sfx[SFX_ERROR].data = sfx_build_chord(notes, 2, 380, 0.35, 1, &n);
        g_sfx[SFX_ERROR].nsamples = n;
    }

    {
        double notes[] = { 1200.0, 900.0 };
        g_sfx[SFX_FLAG].data = sfx_build_sweep(notes, 2, 40, 0.22, 1, &n);
        g_sfx[SFX_FLAG].nsamples = n;
    }

    g_sfx[SFX_REVEAL].data = sfx_build_blip(440.0, 40, 0.18, 0, &n);
    g_sfx[SFX_REVEAL].nsamples = n;

    {
        double notes[] = { 987.77, 783.99, 659.25, 523.25 };
        g_sfx[SFX_SHUTDOWN].data = sfx_build_sweep(notes, 4, 110, 0.40, 2, &n);
        g_sfx[SFX_SHUTDOWN].nsamples = n;
    }
}

static void sfx_init(void) {
    memset(g_sfx, 0, sizeof(g_sfx));
    memset(g_sfx_wb, 0, sizeof(g_sfx_wb));

    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        snprintf(g_audio_status, sizeof(g_audio_status),
                 "run DSP1.3dsx to enable");
        g_audio_ready = 0;
        return;
    }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    float mix[12] = {0};
    mix[0] = 1.0f; mix[1] = 1.0f;

    for (int i = 0; i < SFX_CHANNELS; i++) {
        int ch = SFX_CHANNEL0 + i;
        ndspChnReset(ch);
        ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
        ndspChnSetRate(ch, (float)SFX_RATE);
        ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
        ndspChnSetMix(ch, mix);
    }

    sfx_build_all();
    snprintf(g_audio_status, sizeof(g_audio_status), "ready");
    g_audio_ready = 1;
}

static void sfx_exit(void) {
    if (!g_audio_ready) return;
    for (int i = 0; i < SFX_CHANNELS; i++) {
        ndspChnWaveBufClear(SFX_CHANNEL0 + i);
    }
    ndspExit();
    for (int i = 0; i < SFX_COUNT; i++) {
        if (g_sfx[i].data) linearFree(g_sfx[i].data);
        g_sfx[i].data = NULL;
    }
    g_audio_ready = 0;
}

static void sfx_play(SfxId id) {
    if (!g_audio_ready) return;
    if (id < 0 || id >= SFX_COUNT) return;
    SfxClip *clip = &g_sfx[id];
    if (!clip->data || clip->nsamples <= 0) return;

    int slot = g_sfx_next_ch;
    g_sfx_next_ch = (g_sfx_next_ch + 1) % SFX_CHANNELS;

    ndspWaveBuf *wb = &g_sfx_wb[slot];
    int ch = SFX_CHANNEL0 + slot;

    memset(wb, 0, sizeof(*wb));
    wb->data_vaddr = clip->data;
    wb->nsamples   = clip->nsamples;
    wb->looping    = false;
    ndspChnWaveBufAdd(ch, wb);
}

static const char *sfx_status(void) { return g_audio_status; }

#define TASKBAR_H 20
#define DESK_H    (TOP_H - TASKBAR_H)

#define KB_Y      120
#define KB_H      120
#define PAD_Y     0
#define PAD_H     120
#define KEY_COLS  10
#define KEY_ROWS  5
#define KEY_W     (BOT_W / KEY_COLS)
#define KEY_H     (KB_H / KEY_ROWS)

#define ARTEMIS_DIR "sdmc:/3ds/artemis"
#define MAX_FILES   48
#define FNAME_MAX   28

typedef struct { uint8_t r, g, b; } Color;

static const Color C_DESKTOP  = {0,   128, 128};
static const Color C_TASKBAR  = {192, 192, 192};
static const Color C_HI       = {255, 255, 255};
static const Color C_LO       = {64,  64,  64};
static const Color C_BLACK    = {0,   0,   0};
static const Color C_WIN      = {255, 255, 255};
static const Color C_TITLE    = {0,   0,   128};
static const Color C_TITLE_INACTIVE = {128, 128, 128};
static const Color C_TITLETXT = {255, 255, 255};
static const Color C_TEXT     = {0,   0,   0};
static const Color C_BTN      = {192, 192, 192};
static const Color C_MENU     = {192, 192, 192};
static const Color C_MENU_HI  = {0,   0,   128};
static const Color C_KEY      = {230, 230, 230};
static const Color C_KEYDN    = {150, 150, 180};
static const Color C_KEYSPEC  = {210, 210, 230};
static const Color C_PAD      = {180, 180, 210};
static const Color C_PADDOT   = {130, 130, 160};
static const Color C_CALC_LCD = {190, 220, 180};
static const Color C_FIELD    = {255, 255, 220};
static const Color C_DIM      = {110, 110, 110};

static inline void put_px(uint8_t *fb, int fb_w, int fb_h, int x, int y, Color c) {
    if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h) return;
    int off = (x * fb_h + (fb_h - 1 - y)) * 3;
    fb[off + 0] = c.b;
    fb[off + 1] = c.g;
    fb[off + 2] = c.r;
}

static void fill_rect(uint8_t *fb, int fb_w, int fb_h,
                      int x, int y, int w, int h, Color c) {
    if (x < 0)          { w += x; x = 0; }
    if (y < 0)          { h += y; y = 0; }
    if (x + w > fb_w)   w = fb_w - x;
    if (y + h > fb_h)   h = fb_h - y;
    if (w <= 0 || h <= 0) return;

    for (int i = 0; i < w; i++) {
        uint8_t *p = fb + (((x + i) * fb_h) + (fb_h - 1 - (y + h - 1))) * 3;
        for (int k = 0; k < h; k++) {
            *p++ = c.b;
            *p++ = c.g;
            *p++ = c.r;
        }
    }
}

static void hline(uint8_t *fb, int fb_w, int fb_h, int x, int y, int w, Color c) {
    for (int i = 0; i < w; i++) put_px(fb, fb_w, fb_h, x + i, y, c);
}

static void vline(uint8_t *fb, int fb_w, int fb_h, int x, int y, int h, Color c) {
    for (int j = 0; j < h; j++) put_px(fb, fb_w, fb_h, x, y + j, c);
}

static void bevel(uint8_t *fb, int fb_w, int fb_h, int x, int y, int w, int h,
                  int raised) {
    Color a = raised ? C_HI : C_LO;
    Color b = raised ? C_LO : C_HI;
    hline(fb, fb_w, fb_h, x, y, w, a);
    vline(fb, fb_w, fb_h, x, y, h, a);
    hline(fb, fb_w, fb_h, x, y + h - 1, w, b);
    vline(fb, fb_w, fb_h, x + w - 1, y, h, b);
}

static void draw_char(uint8_t *fb, int fb_w, int fb_h, int x, int y, char ch, Color c) {
    if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7F) ch = '?';
    const uint8_t *g = font8x8_basic[(int)ch - 0x20];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) put_px(fb, fb_w, fb_h, x + col, y + row, c);
        }
    }
}

static void draw_text(uint8_t *fb, int fb_w, int fb_h, int x, int y,
                      const char *s, Color c) {
    int ox = x;
    while (*s) {
        if (*s == '\n') { y += 10; x = ox; }
        else            { draw_char(fb, fb_w, fb_h, x, y, *s, c); x += 8; }
        s++;
    }
}

static const char *CURSOR[] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#....######",
    "#...#       ",
    "#..#        ",
    "###         ",
};
#define CURSOR_H ((int)(sizeof(CURSOR) / sizeof(CURSOR[0])))
#define CURSOR_W 12

static void draw_cursor(uint8_t *fb, int x, int y) {
    for (int j = 0; j < CURSOR_H; j++) {
        for (int i = 0; i < CURSOR_W; i++) {
            char c = CURSOR[j][i];
            if (c == '#')      put_px(fb, TOP_W, TOP_H, x + i, y + j, C_BLACK);
            else if (c == '.') put_px(fb, TOP_W, TOP_H, x + i, y + j, C_HI);
        }
    }
}

typedef enum {
    WK_NOTEPAD = 0,
    WK_CALC    = 1,
    WK_ABOUT   = 2,
    WK_FILES   = 3,
    WK_PAINT   = 4,
    WK_MINES   = 5
} WKind;
#define NUM_WINS 6

static const char * const *icon_for_kind(int kind);
static const char * const *icon_for_shutdown(void);
static const char * const *icon_small_for_kind(int kind);
static void draw_icon(uint8_t *fb, int fb_w, int fb_h, int x, int y,
                      const char * const *map);
static void draw_icon_small(uint8_t *fb, int fb_w, int fb_h, int x, int y,
                            const char * const *map);
#define ICON_SZ       16
#define ICON_SMALL_SZ 8

typedef struct {
    WKind kind;
    int  open;
    int  x, y, w, h;
    char title[32];

    char text[2048];
    int  text_len;

    char filename[FNAME_MAX];
    int  focus_filename;
    char status[48];
    int  status_ttl;

    long long calc_cur;
    long long calc_prev;
    int       calc_op;
    int       calc_reset;
    int       calc_error;

    char files[MAX_FILES][FNAME_MAX];
    int  files_count;
    int  files_sel;
    int  files_scroll;

    int  paint_tool;
    int  paint_color;
    int  paint_last_x;
    int  paint_last_y;
    int  paint_line_sx;
    int  paint_line_sy;
    uint8_t paint_px[192 * 128 / 2];

    uint8_t mine_cells[9 * 9];
    int  mine_state;
    int  mine_flags;
    int  mine_elapsed;
    int  mine_start_tick;
    int  mine_first_click;
} Window;

static void mine_clear_board(Window *w);

static Window g_wins[NUM_WINS];
static int    g_z[NUM_WINS] = {WK_ABOUT, WK_FILES, WK_CALC, WK_MINES, WK_PAINT, WK_NOTEPAD};

static int g_drag_idx   = -1;
static int g_drag_off_x = 0;
static int g_drag_off_y = 0;

static void bring_to_front(int k) {
    int pos = -1;
    for (int i = 0; i < NUM_WINS; i++) if (g_z[i] == k) { pos = i; break; }
    if (pos < 0 || pos == NUM_WINS - 1) return;
    int v = g_z[pos];
    for (int i = pos; i < NUM_WINS - 1; i++) g_z[i] = g_z[i + 1];
    g_z[NUM_WINS - 1] = v;
}

static int topmost_open_any(void) {
    for (int i = NUM_WINS - 1; i >= 0; i--) {
        if (g_wins[g_z[i]].open) return g_z[i];
    }
    return -1;
}

static int topmost_open_input_window(void) {
    for (int i = NUM_WINS - 1; i >= 0; i--) {
        Window *w = &g_wins[g_z[i]];
        if (!w->open) continue;
        if (w->kind == WK_NOTEPAD) return g_z[i];
        if (w->kind == WK_PAINT && w->focus_filename) return g_z[i];
    }
    return -1;
}

static void set_status(Window *w, const char *s) {
    strncpy(w->status, s, sizeof(w->status) - 1);
    w->status[sizeof(w->status) - 1] = '\0';
    w->status_ttl = 120;
}

static void ensure_artemis_dir(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(ARTEMIS_DIR, 0777);
}

static int save_note(Window *w) {
    if (w->filename[0] == '\0') return 0;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", ARTEMIS_DIR, w->filename);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (w->text_len > 0) fwrite(w->text, 1, w->text_len, f);
    fclose(f);
    return 1;
}

static int load_note(Window *w, const char *fname) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", ARTEMIS_DIR, fname);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int cap = (int)sizeof(w->text) - 1;
    int n = (int)fread(w->text, 1, cap, f);
    if (n < 0) n = 0;
    w->text[n] = '\0';
    w->text_len = n;
    fclose(f);
    strncpy(w->filename, fname, FNAME_MAX - 1);
    w->filename[FNAME_MAX - 1] = '\0';
    return 1;
}

static int delete_note(const char *fname) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", ARTEMIS_DIR, fname);
    return remove(path) == 0;
}

static void files_refresh(Window *w) {
    w->files_count = 0;
    DIR *d = opendir(ARTEMIS_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        if (w->files_count >= MAX_FILES) break;
        size_t nlen = strlen(n);
        if (nlen > FNAME_MAX - 1) nlen = FNAME_MAX - 1;
        memcpy(w->files[w->files_count], n, nlen);
        w->files[w->files_count][nlen] = '\0';
        w->files_count++;
    }
    closedir(d);
    if (w->files_sel >= w->files_count) w->files_sel = w->files_count - 1;
    if (w->files_sel < 0) w->files_sel = 0;
}

#define PAINT_CW   192
#define PAINT_CH   128
#define PAINT_BYTES (PAINT_CW * PAINT_CH / 2)

extern const char *ICON_TOOL_PEN[];
extern const char *ICON_TOOL_BRUSH[];
extern const char *ICON_TOOL_ERASER[];
extern const char *ICON_TOOL_FILL[];
extern const char *ICON_TOOL_LINE[];
extern const char *ICON_TOOL_TOUCH[];
extern int g_paint_touch_draw;

extern int g_uptime_ticks;

static const Color PAINT_PAL[16] = {
    {255, 255, 255},
    {0,   0,   0  },
    {128, 128, 128},
    {192, 192, 192},
    {128, 0,   0  },
    {255, 0,   0  },
    {255, 128, 0  },
    {255, 220, 60 },
    {0,   128, 0  },
    {120, 200, 120},
    {0,   128, 128},
    {60,  180, 220},
    {0,   0,   128},
    {80,  80,  220},
    {128, 0,   128},
    {240, 140, 200},
};

static inline void paint_set(Window *w, int x, int y, int col) {
    if ((unsigned)x >= (unsigned)PAINT_CW) return;
    if ((unsigned)y >= (unsigned)PAINT_CH) return;
    int idx = y * PAINT_CW + x;
    int byte = idx / 2;
    uint8_t b = w->paint_px[byte];
    if (idx & 1) b = (uint8_t)((b & 0x0F) | ((col & 0x0F) << 4));
    else         b = (uint8_t)((b & 0xF0) | (col & 0x0F));
    w->paint_px[byte] = b;
}

static inline int paint_get(Window *w, int x, int y) {
    if ((unsigned)x >= (unsigned)PAINT_CW) return 0;
    if ((unsigned)y >= (unsigned)PAINT_CH) return 0;
    int idx = y * PAINT_CW + x;
    uint8_t b = w->paint_px[idx / 2];
    return (idx & 1) ? (b >> 4) : (b & 0x0F);
}

static void paint_fill_all(Window *w, int col) {
    uint8_t v = (uint8_t)((col & 0x0F) | ((col & 0x0F) << 4));
    memset(w->paint_px, v, PAINT_BYTES);
}

static void paint_brush(Window *w, int x, int y, int radius, int col) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius) {
                paint_set(w, x + dx, y + dy, col);
            }
        }
    }
}

static void paint_line(Window *w, int x0, int y0, int x1, int y1,
                       int radius, int col) {
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        if (radius <= 0) paint_set(w, x0, y0, col);
        else             paint_brush(w, x0, y0, radius, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void paint_flood(Window *w, int sx, int sy, int new_col) {
    int old = paint_get(w, sx, sy);
    if (old == new_col) return;

    static int stack[PAINT_CW * PAINT_CH];
    int sp = 0;
    stack[sp++] = sy * PAINT_CW + sx;
    while (sp > 0) {
        int p = stack[--sp];
        int x = p % PAINT_CW;
        int y = p / PAINT_CW;
        if (x < 0 || x >= PAINT_CW || y < 0 || y >= PAINT_CH) continue;
        if (paint_get(w, x, y) != old) continue;

        int lx = x;
        while (lx > 0 && paint_get(w, lx - 1, y) == old) lx--;
        int rx = x;
        while (rx < PAINT_CW - 1 && paint_get(w, rx + 1, y) == old) rx++;
        for (int i = lx; i <= rx; i++) {
            paint_set(w, i, y, new_col);
        }

        if (y > 0) {
            for (int i = lx; i <= rx; i++) {
                if (paint_get(w, i, y - 1) == old) {
                    if (sp < (int)(sizeof(stack) / sizeof(stack[0])))
                        stack[sp++] = (y - 1) * PAINT_CW + i;
                }
            }
        }
        if (y < PAINT_CH - 1) {
            for (int i = lx; i <= rx; i++) {
                if (paint_get(w, i, y + 1) == old) {
                    if (sp < (int)(sizeof(stack) / sizeof(stack[0])))
                        stack[sp++] = (y + 1) * PAINT_CW + i;
                }
            }
        }
    }
}

static int paint_save(Window *w) {
    if (w->filename[0] == '\0') return 0;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", ARTEMIS_DIR, w->filename);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const char magic[4] = { 'A', 'P', 'T', '1' };
    uint16_t cw = PAINT_CW, ch = PAINT_CH;
    uint8_t pal[16 * 3];
    for (int i = 0; i < 16; i++) {
        pal[i * 3 + 0] = PAINT_PAL[i].r;
        pal[i * 3 + 1] = PAINT_PAL[i].g;
        pal[i * 3 + 2] = PAINT_PAL[i].b;
    }
    int ok = 1;
    ok = ok && fwrite(magic,        1, 4,           f) == 4;
    ok = ok && fwrite(&cw,          1, 2,           f) == 2;
    ok = ok && fwrite(&ch,          1, 2,           f) == 2;
    ok = ok && fwrite(pal,          1, sizeof(pal), f) == sizeof(pal);
    ok = ok && fwrite(w->paint_px,  1, PAINT_BYTES, f) == PAINT_BYTES;
    fclose(f);
    return ok;
}

static int paint_load(Window *w, const char *fname) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", ARTEMIS_DIR, fname);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4] = {0};
    uint16_t cw = 0, ch = 0;
    uint8_t pal[16 * 3];
    int ok = 1;
    ok = ok && fread(magic, 1, 4,           f) == 4;
    ok = ok && fread(&cw,   1, 2,           f) == 2;
    ok = ok && fread(&ch,   1, 2,           f) == 2;
    ok = ok && fread(pal,   1, sizeof(pal), f) == sizeof(pal);
    ok = ok && memcmp(magic, "APT1", 4) == 0;
    ok = ok && cw == PAINT_CW && ch == PAINT_CH;
    if (ok) {
        if (fread(w->paint_px, 1, PAINT_BYTES, f) != PAINT_BYTES) ok = 0;
    }
    fclose(f);
    if (ok) {
        size_t n = strlen(fname);
        if (n > FNAME_MAX - 1) n = FNAME_MAX - 1;
        memcpy(w->filename, fname, n);
        w->filename[n] = '\0';
    }
    return ok;
}

static void notepad_reset(void) {
    Window *w = &g_wins[WK_NOTEPAD];
    w->kind = WK_NOTEPAD;
    w->open = 1;
    w->x = 30;  w->y = 20;
    w->w = 260; w->h = 170;
    strcpy(w->title, "Notes");
    w->text[0] = '\0';
    w->text_len = 0;
    strcpy(w->filename, "untitled.txt");
    w->focus_filename = 0;
    w->status[0] = '\0';
    w->status_ttl = 0;
    bring_to_front(WK_NOTEPAD);
}

static void about_reset(void) {
    Window *w = &g_wins[WK_ABOUT];
    w->kind = WK_ABOUT;
    w->open = 1;
    w->x = 100; w->y = 40;
    w->w = 210; w->h = 140;
    strcpy(w->title, "About Artemis OS");
    char about[512];
    snprintf(about, sizeof(about),
        "Artemis OS 3DS\n"
        "v1.4 build %s\n"
        "\n"
        "A tiny Windows-like\n"
        "shell for the 3DS.\n"
        "\n"
        "Audio: %s\n"
        "Saves: sdmc:/3ds/artemis/",
        BUILD_NUMBER,
        sfx_status());
    int n = (int)strlen(about);
    if (n > (int)sizeof(w->text) - 1) n = (int)sizeof(w->text) - 1;
    memcpy(w->text, about, n);
    w->text[n] = '\0';
    w->text_len = n;
    bring_to_front(WK_ABOUT);
}

static void calc_reset(void) {
    Window *w = &g_wins[WK_CALC];
    w->kind = WK_CALC;
    w->open = 1;
    w->x = 200; w->y = 40;
    w->w = 150; w->h = 160;
    strcpy(w->title, "Calculator");
    w->calc_cur = 0; w->calc_prev = 0;
    w->calc_op = 0; w->calc_reset = 1; w->calc_error = 0;
    bring_to_front(WK_CALC);
}

static void files_reset(void) {
    Window *w = &g_wins[WK_FILES];
    w->kind = WK_FILES;
    w->open = 1;
    w->x = 60;  w->y = 25;
    w->w = 200; w->h = 175;
    strcpy(w->title, "File Explorer");
    w->files_sel = 0;
    w->files_scroll = 0;
    w->status[0] = '\0';
    w->status_ttl = 0;
    files_refresh(w);
    bring_to_front(WK_FILES);
}

static void paint_reset(void) {
    Window *w = &g_wins[WK_PAINT];
    w->kind = WK_PAINT;
    w->open = 1;
    w->x = 30;  w->y = 10;
    w->w = 228; w->h = 200;
    strcpy(w->title, "Paint");
    strcpy(w->filename, "untitled.apt");
    w->focus_filename = 0;
    w->status[0] = '\0';
    w->status_ttl = 0;
    w->paint_tool   = 0;
    w->paint_color  = 1;
    w->paint_last_x = -1;
    w->paint_last_y = -1;
    w->paint_line_sx = -1;
    w->paint_line_sy = -1;
    paint_fill_all(w, 0);
    bring_to_front(WK_PAINT);
}

static void mines_reset(void) {
    Window *w = &g_wins[WK_MINES];
    w->kind = WK_MINES;
    w->open = 1;

    w->w = 164; w->h = 200;
    w->x = 120;
    w->y = 20;
    strcpy(w->title, "Minesweeper");
    mine_clear_board(w);
    bring_to_front(WK_MINES);
}

static void open_app(WKind k) {
    switch (k) {
        case WK_NOTEPAD: notepad_reset(); break;
        case WK_CALC:    calc_reset();    break;
        case WK_ABOUT:   about_reset();   break;
        case WK_FILES:   files_reset();   break;
        case WK_PAINT:   paint_reset();   break;
        case WK_MINES:   mines_reset();   break;
    }
}

static int is_valid_filename_char(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) return 1;
    if (c == '.' || c == '_' || c == '-') return 1;
    return 0;
}

static void notepad_putc(Window *w, char c) {
    if (w->focus_filename) {
        if (c == '\b') {
            int n = (int)strlen(w->filename);
            if (n > 0) w->filename[n - 1] = '\0';
            return;
        }
        if (c == '\n') { w->focus_filename = 0; return; }
        if (!is_valid_filename_char(c)) return;
        int n = (int)strlen(w->filename);
        if (n < FNAME_MAX - 1) {
            w->filename[n] = c;
            w->filename[n + 1] = '\0';
        }
        return;
    }

    if (w->kind != WK_NOTEPAD) return;

    if (c == '\b') {
        if (w->text_len > 0) {
            w->text_len--;
            w->text[w->text_len] = '\0';
        }
        return;
    }
    if (w->text_len < (int)sizeof(w->text) - 1) {
        w->text[w->text_len++] = c;
        w->text[w->text_len]   = '\0';
    }
}

static long long calc_apply(long long a, long long b, int op, int *err) {
    *err = 0;
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/':
            if (b == 0) { *err = 1; return 0; }
            return a / b;
    }
    return b;
}

static const char *CALC_LABELS[5][4] = {
    { "C",  "N",  "B",  "/" },
    { "7",  "8",  "9",  "*" },
    { "4",  "5",  "6",  "-" },
    { "1",  "2",  "3",  "+" },
    { "0",  "0",  ".",  "=" },
};

static void calc_client_rect(Window *w, int *cx, int *cy, int *cw, int *ch) {
    *cx = w->x + 4;
    *cy = w->y + 20;
    *cw = w->w - 8;
    *ch = w->h - 24;
}

static void calc_button_rect(Window *w, int row, int col,
                             int *bx, int *by, int *bw, int *bh) {
    int cx, cy, cw, ch;
    calc_client_rect(w, &cx, &cy, &cw, &ch);
    int display_h = 18;
    int grid_y = cy + display_h + 3;
    int grid_h = ch - display_h - 5;
    int cell_w = cw / 4;
    int cell_h = grid_h / 5;
    *bx = cx + col * cell_w + 1;
    *by = grid_y + row * cell_h + 1;
    *bw = cell_w - 2;
    *bh = cell_h - 2;
    if (row == 4 && col == 0) *bw = cell_w * 2 - 2;
}

static void calc_input(Window *w, char code) {
    if (w->calc_error && code != 'C') return;
    if (code >= '0' && code <= '9') {
        if (w->calc_reset) { w->calc_cur = 0; w->calc_reset = 0; }
        if (w->calc_cur > 99999999LL || w->calc_cur < -99999999LL) return;
        long long d = code - '0';
        if (w->calc_cur < 0) w->calc_cur = w->calc_cur * 10 - d;
        else                 w->calc_cur = w->calc_cur * 10 + d;
        return;
    }
    switch (code) {
        case 'C':
            w->calc_cur = 0; w->calc_prev = 0;
            w->calc_op = 0; w->calc_reset = 1;
            w->calc_error = 0;
            break;
        case 'N':
            w->calc_cur = -w->calc_cur;
            break;
        case 'B':
            if (!w->calc_reset) w->calc_cur /= 10;
            break;
        case '+': case '-': case '*': case '/':
            if (w->calc_op && !w->calc_reset) {
                int err;
                long long r = calc_apply(w->calc_prev, w->calc_cur, w->calc_op, &err);
                if (err) { w->calc_error = 1; w->calc_cur = 0; return; }
                w->calc_cur = r;
            }
            w->calc_prev = w->calc_cur;
            w->calc_op = code;
            w->calc_reset = 1;
            break;
        case '=':
            if (w->calc_op) {
                int err;
                long long r = calc_apply(w->calc_prev, w->calc_cur, w->calc_op, &err);
                if (err) { w->calc_error = 1; w->calc_cur = 0; return; }
                w->calc_cur = r;
                w->calc_op = 0;
                w->calc_reset = 1;
            }
            break;
        case '.': break;
    }
}

static int calc_hit_button(Window *w, int mx, int my, int *row, int *col) {
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            if (r == 4 && c == 1) continue;
            int bx, by, bw, bh;
            calc_button_rect(w, r, c, &bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                *row = r; *col = c;
                return 1;
            }
        }
    }
    return 0;
}

static void win_close_rect(Window *w, int *cx, int *cy, int *cw, int *ch) {
    *cw = 14; *ch = 10;
    *cx = w->x + w->w - *cw - 3;
    *cy = w->y + 2;
}

static int win_hit(Window *w, int mx, int my) {
    if (!w->open) return 0;
    int cx, cy, cw, ch;
    win_close_rect(w, &cx, &cy, &cw, &ch);
    if (mx >= cx && mx < cx + cw && my >= cy && my < cy + ch) return 1;
    if (mx >= w->x && mx < w->x + w->w &&
        my >= w->y && my < w->y + 17) return 2;
    if (mx >= w->x && mx < w->x + w->w &&
        my >= w->y && my < w->y + w->h) return 3;
    return 0;
}

typedef enum {
    NP_MISS = 0, NP_FNAME, NP_NEW, NP_OPEN, NP_SAVE, NP_BODY
} NpHit;

static void notepad_layout(Window *w,
                           int *tb_x, int *tb_y, int *tb_w, int *tb_h,
                           int *fn_x, int *fn_w,
                           int *bn_x, int *bn_w, int *bn_gap,
                           int *bd_x, int *bd_y, int *bd_w, int *bd_h) {
    *tb_x = w->x + 5;
    *tb_y = w->y + 20;
    *tb_w = w->w - 10;
    *tb_h = 15;

    int btn_w = 28;
    int gap   = 2;
    int btns_total = 3 * btn_w + 2 * gap;
    *fn_x = *tb_x;
    *fn_w = *tb_w - btns_total - 3;
    *bn_x = *fn_x + *fn_w + 3;
    *bn_w = btn_w;
    *bn_gap = gap;

    *bd_x = w->x + 5;
    *bd_y = *tb_y + *tb_h + 2;
    *bd_w = w->w - 10;
    *bd_h = w->h - (*bd_y - w->y) - 5;
}

static NpHit notepad_inner_hit(Window *w, int mx, int my) {
    int tb_x, tb_y, tb_w, tb_h, fn_x, fn_w, bn_x, bn_w, bn_gap;
    int bd_x, bd_y, bd_w, bd_h;
    notepad_layout(w, &tb_x, &tb_y, &tb_w, &tb_h,
                   &fn_x, &fn_w, &bn_x, &bn_w, &bn_gap,
                   &bd_x, &bd_y, &bd_w, &bd_h);

    if (my >= tb_y && my < tb_y + tb_h) {
        if (mx >= fn_x && mx < fn_x + fn_w) return NP_FNAME;
        int bx = bn_x;
        for (int i = 0; i < 3; i++) {
            if (mx >= bx && mx < bx + bn_w) {
                if (i == 0) return NP_NEW;
                if (i == 1) return NP_OPEN;
                return NP_SAVE;
            }
            bx += bn_w + bn_gap;
        }
    }
    if (mx >= bd_x && mx < bd_x + bd_w && my >= bd_y && my < bd_y + bd_h)
        return NP_BODY;
    return NP_MISS;
}

static void draw_text_block(uint8_t *fb, int px, int py, int pw, int ph,
                            const char *text, int text_len,
                            int draw_caret, int caret_blink) {
    int chars_per_line = (pw - 6) / 8;
    if (chars_per_line < 1) chars_per_line = 1;
    int max_lines = (ph - 4) / 10;

    int col = 0, line = 0;
    int tx = px + 3, ty = py + 2;
    for (int i = 0; i < text_len && line < max_lines; i++) {
        char ch = text[i];
        if (ch == '\n') { line++; col = 0; continue; }
        if (col >= chars_per_line) { line++; col = 0; if (line >= max_lines) break; }
        draw_char(fb, TOP_W, TOP_H, tx + col * 8, ty + line * 10, ch, C_TEXT);
        col++;
    }
    if (draw_caret && caret_blink && line < max_lines) {
        int cx = tx + col * 8;
        int cy = ty + line * 10;
        vline(fb, TOP_W, TOP_H, cx, cy, 8, C_TEXT);
    }
}

static int g_caret_tick = 0;

static void draw_notepad_body(uint8_t *fb, Window *w, int is_focused) {
    int tb_x, tb_y, tb_w, tb_h, fn_x, fn_w, bn_x, bn_w, bn_gap;
    int bd_x, bd_y, bd_w, bd_h;
    notepad_layout(w, &tb_x, &tb_y, &tb_w, &tb_h,
                   &fn_x, &fn_w, &bn_x, &bn_w, &bn_gap,
                   &bd_x, &bd_y, &bd_w, &bd_h);

    Color fcol = w->focus_filename ? C_FIELD : C_WIN;
    fill_rect(fb, TOP_W, TOP_H, fn_x, tb_y, fn_w, tb_h, fcol);
    bevel(fb, TOP_W, TOP_H, fn_x, tb_y, fn_w, tb_h, 0);
    int max_chars = (fn_w - 6) / 8;
    int fn_len = (int)strlen(w->filename);
    const char *shown = w->filename;
    char buf[FNAME_MAX];
    if (fn_len > max_chars) {
        int off = fn_len - max_chars;
        memcpy(buf, w->filename + off, max_chars);
        buf[max_chars] = '\0';
        shown = buf;
    }
    draw_text(fb, TOP_W, TOP_H, fn_x + 3, tb_y + 3, shown, C_TEXT);
    if (w->focus_filename && is_focused && (g_caret_tick < 30)) {
        int caret_x = fn_x + 3 + (int)strlen(shown) * 8;
        vline(fb, TOP_W, TOP_H, caret_x, tb_y + 3, 8, C_TEXT);
    }

    const char *labels[3] = { "New", "Opn", "Sav" };
    int bx = bn_x;
    for (int i = 0; i < 3; i++) {
        fill_rect(fb, TOP_W, TOP_H, bx, tb_y, bn_w, tb_h, C_BTN);
        bevel(fb, TOP_W, TOP_H, bx, tb_y, bn_w, tb_h, 1);
        int lw = (int)strlen(labels[i]) * 8;
        int tx = bx + (bn_w - lw) / 2;
        draw_text(fb, TOP_W, TOP_H, tx, tb_y + 4, labels[i], C_TEXT);
        bx += bn_w + bn_gap;
    }

    fill_rect(fb, TOP_W, TOP_H, bd_x, bd_y, bd_w, bd_h, C_WIN);
    bevel(fb, TOP_W, TOP_H, bd_x, bd_y, bd_w, bd_h, 0);

    int body_focus = is_focused && !w->focus_filename;
    draw_text_block(fb, bd_x, bd_y, bd_w, bd_h, w->text, w->text_len,
                    body_focus, g_caret_tick < 30);

    if (w->status_ttl > 0 && w->status[0]) {
        int sy = bd_y + bd_h - 10;
        fill_rect(fb, TOP_W, TOP_H, bd_x + 2, sy, bd_w - 4, 10, C_TASKBAR);
        draw_text(fb, TOP_W, TOP_H, bd_x + 4, sy + 1, w->status, C_TITLE);
    }
}

static void draw_about_body(uint8_t *fb, Window *w) {
    int px = w->x + 5, py = w->y + 20;
    int pw = w->w - 10, ph = w->h - 25;
    fill_rect(fb, TOP_W, TOP_H, px, py, pw, ph, C_WIN);
    bevel(fb, TOP_W, TOP_H, px, py, pw, ph, 0);
    draw_text_block(fb, px, py, pw, ph, w->text, w->text_len, 0, 0);
}

static void draw_calc_body(uint8_t *fb, Window *w) {
    int cx, cy, cw, ch;
    calc_client_rect(w, &cx, &cy, &cw, &ch);

    int dh = 16;
    fill_rect(fb, TOP_W, TOP_H, cx, cy, cw, dh, C_CALC_LCD);
    bevel(fb, TOP_W, TOP_H, cx, cy, cw, dh, 0);

    char buf[32];
    if (w->calc_error) snprintf(buf, sizeof(buf), "Error");
    else               snprintf(buf, sizeof(buf), "%lld", w->calc_cur);
    int tw = (int)strlen(buf) * 8;
    draw_text(fb, TOP_W, TOP_H, cx + cw - tw - 4, cy + 4, buf, C_TEXT);
    if (w->calc_op) {
        char op[2] = { (char)w->calc_op, 0 };
        draw_text(fb, TOP_W, TOP_H, cx + 4, cy + 4, op, C_TEXT);
    }

    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            if (r == 4 && c == 1) continue;
            int bx, by, bw, bh;
            calc_button_rect(w, r, c, &bx, &by, &bw, &bh);
            fill_rect(fb, TOP_W, TOP_H, bx, by, bw, bh, C_BTN);
            bevel(fb, TOP_W, TOP_H, bx, by, bw, bh, 1);
            const char *lab = CALC_LABELS[r][c];
            const char *show = lab;
            if (!strcmp(lab, "B")) show = "<-";
            if (!strcmp(lab, "N")) show = "+/-";
            int lw = (int)strlen(show) * 8;
            int tx = bx + (bw - lw) / 2;
            int ty = by + (bh - 8) / 2;
            Color col = C_TEXT;
            char ch0 = lab[0];
            if (ch0 == '+' || ch0 == '-' || ch0 == '*' ||
                ch0 == '/' || ch0 == '=')
                col = C_TITLE;
            draw_text(fb, TOP_W, TOP_H, tx, ty, show, col);
        }
    }
}

typedef enum {
    PA_MISS = 0,
    PA_FNAME,
    PA_NEW, PA_OPEN, PA_SAVE,
    PA_TOOL,
    PA_COLOR,
    PA_CANVAS,
    PA_TITLE
} PaHit;

typedef struct {

    int  tb_x, tb_y, tb_w, tb_h;
    int  fn_x, fn_w;
    int  bn_x, bn_w, bn_gap;

    int  tc_x, tc_y, tc_w, tc_h;
    int  tc_btn_h;

    int  cv_x, cv_y, cv_w, cv_h;

    int  pl_x, pl_y, pl_w, pl_h;
    int  pl_sw;
} PaintLayout;

static const char *PAINT_TOOLS[] = {
    "Pen", "Brush", "Eraser", "Fill", "Line"
};
#define PAINT_NUM_TOOLS ((int)(sizeof(PAINT_TOOLS) / sizeof(PAINT_TOOLS[0])))

static const char * const *paint_tool_icon(int i) {
    switch (i) {
        case 0: return ICON_TOOL_PEN;
        case 1: return ICON_TOOL_BRUSH;
        case 2: return ICON_TOOL_ERASER;
        case 3: return ICON_TOOL_FILL;
        case 4: return ICON_TOOL_LINE;
        case 5: return ICON_TOOL_TOUCH;
    }
    return ICON_TOOL_PEN;
}

static void paint_layout(Window *w, PaintLayout *L) {
    L->tb_x = w->x + 5;
    L->tb_y = w->y + 20;
    L->tb_w = w->w - 10;
    L->tb_h = 15;

    int btn_w = 28, gap = 2;
    int btns_total = 3 * btn_w + 2 * gap;
    L->fn_x = L->tb_x;
    L->fn_w = L->tb_w - btns_total - 3;
    L->bn_x = L->fn_x + L->fn_w + 3;
    L->bn_w = btn_w;
    L->bn_gap = gap;

    int col_w = 16;
    L->tc_x = w->x + 5;
    L->tc_y = L->tb_y + L->tb_h + 2;
    L->tc_w = col_w;
    L->tc_h = PAINT_CH;

    L->tc_btn_h = L->tc_h / (PAINT_NUM_TOOLS + 1);

    L->cv_x = L->tc_x + L->tc_w + 2;
    L->cv_y = L->tc_y;
    L->cv_w = PAINT_CW;
    L->cv_h = PAINT_CH;

    L->pl_x = L->cv_x;
    L->pl_y = L->cv_y + L->cv_h + 2;
    L->pl_w = PAINT_CW;
    L->pl_h = 10;
    L->pl_sw = L->pl_w / 16;
}

static void draw_paint_canvas(uint8_t *fb, Window *w, int px, int py) {

    for (int y = 0; y < PAINT_CH; y++) {
        for (int x = 0; x < PAINT_CW; x++) {
            int c = paint_get(w, x, y);
            put_px(fb, TOP_W, TOP_H, px + x, py + y, PAINT_PAL[c]);
        }
    }

    bevel(fb, TOP_W, TOP_H, px - 1, py - 1, PAINT_CW + 2, PAINT_CH + 2, 0);
}

static void draw_paint_body(uint8_t *fb, Window *w, int is_focused) {
    PaintLayout L;
    paint_layout(w, &L);

    Color fcol = w->focus_filename ? C_FIELD : C_WIN;
    fill_rect(fb, TOP_W, TOP_H, L.fn_x, L.tb_y, L.fn_w, L.tb_h, fcol);
    bevel(fb, TOP_W, TOP_H, L.fn_x, L.tb_y, L.fn_w, L.tb_h, 0);
    int max_chars = (L.fn_w - 6) / 8;
    int fn_len = (int)strlen(w->filename);
    const char *shown = w->filename;
    char fbuf[FNAME_MAX];
    if (fn_len > max_chars) {
        int off = fn_len - max_chars;
        memcpy(fbuf, w->filename + off, max_chars);
        fbuf[max_chars] = '\0';
        shown = fbuf;
    }
    draw_text(fb, TOP_W, TOP_H, L.fn_x + 3, L.tb_y + 3, shown, C_TEXT);
    if (w->focus_filename && is_focused && (g_caret_tick < 30)) {
        int caret_x = L.fn_x + 3 + (int)strlen(shown) * 8;
        vline(fb, TOP_W, TOP_H, caret_x, L.tb_y + 3, 8, C_TEXT);
    }
    const char *tb_labels[3] = { "New", "Opn", "Sav" };
    int bx = L.bn_x;
    for (int i = 0; i < 3; i++) {
        fill_rect(fb, TOP_W, TOP_H, bx, L.tb_y, L.bn_w, L.tb_h, C_BTN);
        bevel(fb, TOP_W, TOP_H, bx, L.tb_y, L.bn_w, L.tb_h, 1);
        int lw = (int)strlen(tb_labels[i]) * 8;
        int tx = bx + (L.bn_w - lw) / 2;
        draw_text(fb, TOP_W, TOP_H, tx, L.tb_y + 4, tb_labels[i], C_TEXT);
        bx += L.bn_w + L.bn_gap;
    }

    for (int i = 0; i < PAINT_NUM_TOOLS + 1; i++) {
        int by = L.tc_y + i * L.tc_btn_h + 1;
        int bh = L.tc_btn_h - 2;
        int is_touch_btn = (i == PAINT_NUM_TOOLS);
        int sel = is_touch_btn ? g_paint_touch_draw : (w->paint_tool == i);
        fill_rect(fb, TOP_W, TOP_H, L.tc_x, by, L.tc_w, bh, C_BTN);
        bevel(fb, TOP_W, TOP_H, L.tc_x, by, L.tc_w, bh, sel ? 0 : 1);
        int icon_x = L.tc_x + (L.tc_w - ICON_SMALL_SZ) / 2;
        int icon_y = by + (bh - ICON_SMALL_SZ) / 2;
        draw_icon_small(fb, TOP_W, TOP_H, icon_x, icon_y, paint_tool_icon(i));
    }

    draw_paint_canvas(fb, w, L.cv_x, L.cv_y);

    for (int i = 0; i < 16; i++) {
        int sx = L.pl_x + i * L.pl_sw;
        int sel = (w->paint_color == i);
        fill_rect(fb, TOP_W, TOP_H, sx, L.pl_y, L.pl_sw - 1, L.pl_h,
                  PAINT_PAL[i]);
        Color edge = sel ? (Color){255, 255, 0} : C_BLACK;
        hline(fb, TOP_W, TOP_H, sx, L.pl_y, L.pl_sw - 1, edge);
        hline(fb, TOP_W, TOP_H, sx, L.pl_y + L.pl_h - 1, L.pl_sw - 1, edge);
        vline(fb, TOP_W, TOP_H, sx, L.pl_y, L.pl_h, edge);
        vline(fb, TOP_W, TOP_H, sx + L.pl_sw - 2, L.pl_y, L.pl_h, edge);
    }

    if (w->status_ttl > 0 && w->status[0]) {
        int sy = L.pl_y + L.pl_h + 1;
        draw_text(fb, TOP_W, TOP_H, L.tc_x, sy, w->status, C_TITLE);
    }
}

static PaHit paint_inner_hit(Window *w, int mx, int my,
                             int *out_a, int *out_b) {
    PaintLayout L;
    paint_layout(w, &L);

    if (my >= L.tb_y && my < L.tb_y + L.tb_h) {
        if (mx >= L.fn_x && mx < L.fn_x + L.fn_w) return PA_FNAME;
        int bx = L.bn_x;
        for (int i = 0; i < 3; i++) {
            if (mx >= bx && mx < bx + L.bn_w) {
                if (i == 0) return PA_NEW;
                if (i == 1) return PA_OPEN;
                return PA_SAVE;
            }
            bx += L.bn_w + L.bn_gap;
        }
    }

    if (mx >= L.tc_x && mx < L.tc_x + L.tc_w &&
        my >= L.tc_y && my < L.tc_y + L.tc_h) {
        int rel = my - L.tc_y;
        int idx = rel / L.tc_btn_h;
        if (idx >= 0 && idx < PAINT_NUM_TOOLS + 1) { *out_a = idx; return PA_TOOL; }
    }

    if (mx >= L.pl_x && mx < L.pl_x + L.pl_w &&
        my >= L.pl_y && my < L.pl_y + L.pl_h) {
        int rel = mx - L.pl_x;
        int idx = rel / L.pl_sw;
        if (idx >= 0 && idx < 16) { *out_a = idx; return PA_COLOR; }
    }

    if (mx >= L.cv_x && mx < L.cv_x + L.cv_w &&
        my >= L.cv_y && my < L.cv_y + L.cv_h) {
        *out_a = mx - L.cv_x;
        *out_b = my - L.cv_y;
        return PA_CANVAS;
    }
    return PA_MISS;
}

static void paint_stroke(Window *w, int cx, int cy, int start) {
    int col = w->paint_color;
    int eraser_col = 0;
    switch (w->paint_tool) {
        case 0:
            if (!start && w->paint_last_x >= 0)
                paint_line(w, w->paint_last_x, w->paint_last_y, cx, cy, 0, col);
            else
                paint_set(w, cx, cy, col);
            w->paint_last_x = cx; w->paint_last_y = cy;
            break;
        case 1:
            if (!start && w->paint_last_x >= 0)
                paint_line(w, w->paint_last_x, w->paint_last_y, cx, cy, 2, col);
            else
                paint_brush(w, cx, cy, 2, col);
            w->paint_last_x = cx; w->paint_last_y = cy;
            break;
        case 2:
            if (!start && w->paint_last_x >= 0)
                paint_line(w, w->paint_last_x, w->paint_last_y, cx, cy, 2, eraser_col);
            else
                paint_brush(w, cx, cy, 2, eraser_col);
            w->paint_last_x = cx; w->paint_last_y = cy;
            break;
        case 3:
            if (start) paint_flood(w, cx, cy, col);
            break;
        case 4:
            if (start) {
                if (w->paint_line_sx < 0) {
                    w->paint_line_sx = cx; w->paint_line_sy = cy;
                } else {
                    paint_line(w, w->paint_line_sx, w->paint_line_sy, cx, cy, 0, col);
                    w->paint_line_sx = -1; w->paint_line_sy = -1;
                }
            }
            break;
    }
}

static void paint_stroke_end(Window *w) {
    w->paint_last_x = -1;
    w->paint_last_y = -1;
}

typedef struct {
    int cv_x, cv_y, cv_w, cv_h;
    int tr_x, tr_y, tr_h;
    int tb_sz, tb_gap;
    int fa_x, fa_y, fa_w, fa_h, fa_gap;
    int pl_x, pl_y, pl_w, pl_h, pl_sw;
} PaintBotLayout;

static void paint_bot_layout(PaintBotLayout *L) {
    L->cv_x = (BOT_W - PAINT_CW) / 2;
    L->cv_y = 0;
    L->cv_w = PAINT_CW;
    L->cv_h = PAINT_CH;

    L->tr_y = L->cv_y + L->cv_h + 4;
    L->tr_h = 24;
    L->tb_sz = 20;
    L->tb_gap = 2;
    L->tr_x = 6;

    int tools_w = (PAINT_NUM_TOOLS + 1) * (L->tb_sz + L->tb_gap);
    L->fa_y = L->tr_y + (L->tr_h - 16) / 2;
    L->fa_h = 16;
    L->fa_w = 32;
    L->fa_gap = 2;
    L->fa_x = L->tr_x + tools_w + 4;

    L->pl_y = L->tr_y + L->tr_h + 4;
    L->pl_h = 18;
    L->pl_sw = 18;
    L->pl_w  = 16 * L->pl_sw;
    L->pl_x  = (BOT_W - L->pl_w) / 2;
}

typedef enum {
    PB_MISS = 0,
    PB_CANVAS, PB_TOOL, PB_COLOR,
    PB_NEW, PB_OPEN, PB_SAVE
} PbHit;

static PbHit paint_bot_hit(int tx, int ty, int *out_a, int *out_b) {
    PaintBotLayout L;
    paint_bot_layout(&L);

    if (tx >= L.cv_x && tx < L.cv_x + L.cv_w &&
        ty >= L.cv_y && ty < L.cv_y + L.cv_h) {
        *out_a = tx - L.cv_x;
        *out_b = ty - L.cv_y;
        return PB_CANVAS;
    }

    for (int i = 0; i < PAINT_NUM_TOOLS + 1; i++) {
        int bx = L.tr_x + i * (L.tb_sz + L.tb_gap);
        int by = L.tr_y + (L.tr_h - L.tb_sz) / 2;
        if (tx >= bx && tx < bx + L.tb_sz &&
            ty >= by && ty < by + L.tb_sz) {
            *out_a = i;
            return PB_TOOL;
        }
    }

    {
        int bx = L.fa_x;
        for (int i = 0; i < 3; i++) {
            if (tx >= bx && tx < bx + L.fa_w &&
                ty >= L.fa_y && ty < L.fa_y + L.fa_h) {
                if (i == 0) return PB_NEW;
                if (i == 1) return PB_OPEN;
                return PB_SAVE;
            }
            bx += L.fa_w + L.fa_gap;
        }
    }

    if (tx >= L.pl_x && tx < L.pl_x + L.pl_w &&
        ty >= L.pl_y && ty < L.pl_y + L.pl_h) {
        int rel = tx - L.pl_x;
        int idx = rel / L.pl_sw;
        if (idx < 0) idx = 0;
        if (idx > 15) idx = 15;
        *out_a = idx;
        return PB_COLOR;
    }

    return PB_MISS;
}

static void draw_paint_bottom(uint8_t *bot, Window *w) {
    PaintBotLayout L;
    paint_bot_layout(&L);

    fill_rect(bot, BOT_W, BOT_H, 0, 0, BOT_W, BOT_H, C_TASKBAR);

    for (int y = 0; y < PAINT_CH; y++) {
        for (int x = 0; x < PAINT_CW; x++) {
            int c = paint_get(w, x, y);
            put_px(bot, BOT_W, BOT_H, L.cv_x + x, L.cv_y + y, PAINT_PAL[c]);
        }
    }
    bevel(bot, BOT_W, BOT_H, L.cv_x - 1, L.cv_y, L.cv_w + 2, L.cv_h + 1, 0);

    for (int i = 0; i < PAINT_NUM_TOOLS + 1; i++) {
        int bx = L.tr_x + i * (L.tb_sz + L.tb_gap);
        int by = L.tr_y + (L.tr_h - L.tb_sz) / 2;
        int is_touch_btn = (i == PAINT_NUM_TOOLS);
        int sel = is_touch_btn ? g_paint_touch_draw : (w->paint_tool == i);
        fill_rect(bot, BOT_W, BOT_H, bx, by, L.tb_sz, L.tb_sz, C_BTN);
        bevel(bot, BOT_W, BOT_H, bx, by, L.tb_sz, L.tb_sz, sel ? 0 : 1);
        int ix = bx + (L.tb_sz - ICON_SMALL_SZ) / 2;
        int iy = by + (L.tb_sz - ICON_SMALL_SZ) / 2;
        draw_icon_small(bot, BOT_W, BOT_H, ix, iy, paint_tool_icon(i));
    }

    const char *fa_labels[3] = { "New", "Opn", "Sav" };
    {
        int bx = L.fa_x;
        for (int i = 0; i < 3; i++) {
            fill_rect(bot, BOT_W, BOT_H, bx, L.fa_y, L.fa_w, L.fa_h, C_BTN);
            bevel(bot, BOT_W, BOT_H, bx, L.fa_y, L.fa_w, L.fa_h, 1);
            int lw = (int)strlen(fa_labels[i]) * 8;
            int tx = bx + (L.fa_w - lw) / 2;
            draw_text(bot, BOT_W, BOT_H, tx, L.fa_y + (L.fa_h - 8) / 2,
                      fa_labels[i], C_TEXT);
            bx += L.fa_w + L.fa_gap;
        }
    }

    for (int i = 0; i < 16; i++) {
        int sx = L.pl_x + i * L.pl_sw;
        int sel = (w->paint_color == i);
        fill_rect(bot, BOT_W, BOT_H, sx, L.pl_y, L.pl_sw - 1, L.pl_h, PAINT_PAL[i]);
        Color edge = sel ? (Color){255, 255, 0} : C_BLACK;
        hline(bot, BOT_W, BOT_H, sx, L.pl_y, L.pl_sw - 1, edge);
        hline(bot, BOT_W, BOT_H, sx, L.pl_y + L.pl_h - 1, L.pl_sw - 1, edge);
        vline(bot, BOT_W, BOT_H, sx, L.pl_y, L.pl_h, edge);
        vline(bot, BOT_W, BOT_H, sx + L.pl_sw - 2, L.pl_y, L.pl_h, edge);
    }

    int info_y = L.pl_y + L.pl_h + 4;
    draw_text(bot, BOT_W, BOT_H, 6, info_y, w->filename, C_TEXT);
    if (w->status_ttl > 0 && w->status[0]) {
        draw_text(bot, BOT_W, BOT_H, 6, info_y + 10, w->status, C_TITLE);
    }
}

#define MINE_COLS 9
#define MINE_ROWS 9
#define MINE_COUNT 10

#define MINE_BIT_MINE     0x01
#define MINE_BIT_REVEALED 0x02
#define MINE_BIT_FLAGGED  0x04

static int mine_idx(int cx, int cy) { return cy * MINE_COLS + cx; }

static int mine_in_bounds(int cx, int cy) {
    return cx >= 0 && cx < MINE_COLS && cy >= 0 && cy < MINE_ROWS;
}

static int mine_neighbors(Window *w, int cx, int cy) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int x = cx + dx, y = cy + dy;
            if (!mine_in_bounds(x, y)) continue;
            if (w->mine_cells[mine_idx(x, y)] & MINE_BIT_MINE) n++;
        }
    }
    return n;
}

static void mine_clear_board(Window *w) {
    memset(w->mine_cells, 0, sizeof(w->mine_cells));
    w->mine_state = 0;
    w->mine_flags = 0;
    w->mine_elapsed = 0;
    w->mine_first_click = 1;
    w->mine_start_tick = g_uptime_ticks;
}

static void mine_seed(Window *w, int avoid_x, int avoid_y) {
    int total = MINE_COLS * MINE_ROWS;
    int placed = 0;

    while (placed < MINE_COUNT) {
        int r = rand() % total;
        int cx = r % MINE_COLS;
        int cy = r / MINE_COLS;
        if (w->mine_cells[r] & MINE_BIT_MINE) continue;
        if (abs(cx - avoid_x) <= 1 && abs(cy - avoid_y) <= 1) continue;
        w->mine_cells[r] |= MINE_BIT_MINE;
        placed++;
    }
}

static int mine_check_win(Window *w) {
    for (int i = 0; i < MINE_COLS * MINE_ROWS; i++) {
        uint8_t c = w->mine_cells[i];
        if (!(c & MINE_BIT_MINE) && !(c & MINE_BIT_REVEALED)) return 0;
    }
    return 1;
}

static void mine_reveal(Window *w, int cx, int cy) {
    if (!mine_in_bounds(cx, cy)) return;
    uint8_t c = w->mine_cells[mine_idx(cx, cy)];
    if (c & MINE_BIT_REVEALED) return;
    if (c & MINE_BIT_FLAGGED) return;
    if (c & MINE_BIT_MINE) {
        w->mine_cells[mine_idx(cx, cy)] |= MINE_BIT_REVEALED;
        w->mine_state = 2;

        for (int i = 0; i < MINE_COLS * MINE_ROWS; i++) {
            if (w->mine_cells[i] & MINE_BIT_MINE)
                w->mine_cells[i] |= MINE_BIT_REVEALED;
        }
        sfx_play(SFX_ERROR);
        return;
    }

    int stack[MINE_COLS * MINE_ROWS];
    int sp = 0;
    stack[sp++] = mine_idx(cx, cy);
    while (sp > 0) {
        int p = stack[--sp];
        int x = p % MINE_COLS;
        int y = p / MINE_COLS;
        if (w->mine_cells[p] & MINE_BIT_REVEALED) continue;
        if (w->mine_cells[p] & MINE_BIT_FLAGGED) continue;
        if (w->mine_cells[p] & MINE_BIT_MINE) continue;
        w->mine_cells[p] |= MINE_BIT_REVEALED;
        if (mine_neighbors(w, x, y) != 0) continue;

        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                int nx = x + dx, ny = y + dy;
                if (!mine_in_bounds(nx, ny)) continue;
                if (sp < (int)(sizeof(stack) / sizeof(stack[0])))
                    stack[sp++] = mine_idx(nx, ny);
            }
        }
    }
    if (mine_check_win(w)) { w->mine_state = 1; sfx_play(SFX_DING); }
    else                   { sfx_play(SFX_REVEAL); }
}

static void mine_toggle_flag(Window *w, int cx, int cy) {
    if (!mine_in_bounds(cx, cy)) return;
    if (w->mine_state != 0) return;
    int i = mine_idx(cx, cy);
    uint8_t c = w->mine_cells[i];
    if (c & MINE_BIT_REVEALED) return;
    if (c & MINE_BIT_FLAGGED) {
        w->mine_cells[i] &= ~MINE_BIT_FLAGGED;
        w->mine_flags--;
    } else {
        w->mine_cells[i] |= MINE_BIT_FLAGGED;
        w->mine_flags++;
    }
    sfx_play(SFX_FLAG);
}

#define MINE_CELL 16
#define MINE_HEADER_H 22

typedef struct {
    int  board_x, board_y;
    int  cell;
    int  face_x, face_y, face_sz;
    int  mines_x, mines_y, mines_w, mines_h;
    int  time_x, time_y, time_w, time_h;
} MineLayout;

static void mine_layout(Window *w, MineLayout *L) {
    L->cell = MINE_CELL;
    int bw = MINE_COLS * L->cell;
    int bh = MINE_ROWS * L->cell;
    int client_x = w->x + 5;
    int client_y = w->y + 20;
    int client_w = w->w - 10;
    L->board_x = client_x + (client_w - bw) / 2;
    L->board_y = client_y + MINE_HEADER_H + 2;

    int counter_w = 34, counter_h = 16;
    int header_y = client_y + 2;
    L->mines_x = client_x + 4;
    L->mines_y = header_y + (MINE_HEADER_H - counter_h) / 2;
    L->mines_w = counter_w;
    L->mines_h = counter_h;

    L->time_w  = counter_w;
    L->time_h  = counter_h;
    L->time_x  = client_x + client_w - counter_w - 4;
    L->time_y  = L->mines_y;

    L->face_sz = 18;
    L->face_x  = client_x + (client_w - L->face_sz) / 2;
    L->face_y  = header_y + (MINE_HEADER_H - L->face_sz) / 2;

    (void)bh;
}

typedef enum { MH_MISS = 0, MH_CELL, MH_FACE } MineHitKind;

static MineHitKind mine_hit(Window *w, int mx, int my, int *out_cx, int *out_cy) {
    MineLayout L;
    mine_layout(w, &L);
    if (mx >= L.face_x && mx < L.face_x + L.face_sz &&
        my >= L.face_y && my < L.face_y + L.face_sz) return MH_FACE;
    int rx = mx - L.board_x;
    int ry = my - L.board_y;
    if (rx < 0 || ry < 0) return MH_MISS;
    int cx = rx / L.cell;
    int cy = ry / L.cell;
    if (!mine_in_bounds(cx, cy)) return MH_MISS;
    *out_cx = cx;
    *out_cy = cy;
    return MH_CELL;
}

static Color mine_num_color(int n) {
    switch (n) {
        case 1: return (Color){0,   0,   255};
        case 2: return (Color){0,   128, 0  };
        case 3: return (Color){255, 0,   0  };
        case 4: return (Color){0,   0,   128};
        case 5: return (Color){128, 0,   0  };
        case 6: return (Color){0,   128, 128};
        case 7: return (Color){0,   0,   0  };
        case 8: return (Color){128, 128, 128};
    }
    return (Color){0, 0, 0};
}

static void draw_counter(uint8_t *fb, int x, int y, int w, int h, int value) {
    fill_rect(fb, TOP_W, TOP_H, x, y, w, h, C_BLACK);
    bevel(fb, TOP_W, TOP_H, x, y, w, h, 0);

    int v = value;
    if (v > 999) v = 999;
    if (v < -99) v = -99;
    char buf[5];
    if (v < 0) snprintf(buf, sizeof(buf), "-%02d", -v);
    else       snprintf(buf, sizeof(buf), "%03d", v);
    int tx = x + (w - 3 * 8) / 2;
    int ty = y + (h - 8) / 2;
    Color red = {255, 40, 40};
    draw_text(fb, TOP_W, TOP_H, tx, ty, buf, red);
}

static void draw_mine_face(uint8_t *fb, int x, int y, int sz, int state) {

    fill_rect(fb, TOP_W, TOP_H, x, y, sz, sz, C_BTN);
    bevel(fb, TOP_W, TOP_H, x, y, sz, sz, 1);
    Color yellow = {245, 215, 90};
    int fx = x + 2, fy = y + 2, fsz = sz - 4;
    for (int j = 0; j < fsz; j++) {
        int dy = j - fsz / 2;
        for (int i = 0; i < fsz; i++) {
            int dx = i - fsz / 2;
            if (dx * dx + dy * dy <= (fsz / 2) * (fsz / 2))
                put_px(fb, TOP_W, TOP_H, fx + i, fy + j, yellow);
        }
    }

    put_px(fb, TOP_W, TOP_H, x + 5, y + 6, C_BLACK);
    put_px(fb, TOP_W, TOP_H, x + sz - 6, y + 6, C_BLACK);

    if (state == 2) {

        put_px(fb, TOP_W, TOP_H, x + 4, y + 5, C_BLACK);
        put_px(fb, TOP_W, TOP_H, x + 6, y + 7, C_BLACK);
        put_px(fb, TOP_W, TOP_H, x + sz - 7, y + 5, C_BLACK);
        put_px(fb, TOP_W, TOP_H, x + sz - 5, y + 7, C_BLACK);
        hline(fb, TOP_W, TOP_H, x + 6, y + sz - 5, sz - 12, C_BLACK);
    } else if (state == 1) {

        hline(fb, TOP_W, TOP_H, x + 4, y + 5, sz - 8, C_BLACK);
        hline(fb, TOP_W, TOP_H, x + 4, y + 7, sz - 8, C_BLACK);
        hline(fb, TOP_W, TOP_H, x + 5, y + sz - 6, sz - 10, C_BLACK);
        put_px(fb, TOP_W, TOP_H, x + 4, y + sz - 7, C_BLACK);
        put_px(fb, TOP_W, TOP_H, x + sz - 5, y + sz - 7, C_BLACK);
    } else {

        hline(fb, TOP_W, TOP_H, x + 5, y + sz - 6, sz - 10, C_BLACK);
        put_px(fb, TOP_W, TOP_H, x + 4, y + sz - 7, C_BLACK);
        put_px(fb, TOP_W, TOP_H, x + sz - 5, y + sz - 7, C_BLACK);
    }
}

static void draw_mine_cell(uint8_t *fb, int x, int y, int cell,
                           uint8_t c, int neighbors, int exploded) {
    int revealed = (c & MINE_BIT_REVEALED) != 0;
    int flagged  = (c & MINE_BIT_FLAGGED)  != 0;
    int is_mine  = (c & MINE_BIT_MINE)     != 0;

    if (revealed) {
        Color bg = exploded ? (Color){200, 40, 40} : (Color){208, 208, 208};
        fill_rect(fb, TOP_W, TOP_H, x, y, cell, cell, bg);
        hline(fb, TOP_W, TOP_H, x, y, cell, C_LO);
        vline(fb, TOP_W, TOP_H, x, y, cell, C_LO);
        if (is_mine) {

            int cx = x + cell / 2, cy = y + cell / 2;
            for (int dy = -3; dy <= 3; dy++) {
                for (int dx = -3; dx <= 3; dx++) {
                    if (dx * dx + dy * dy <= 9)
                        put_px(fb, TOP_W, TOP_H, cx + dx, cy + dy, C_BLACK);
                }
            }
            put_px(fb, TOP_W, TOP_H, cx - 1, cy - 1, C_HI);
        } else if (neighbors > 0) {
            char s[2] = { (char)('0' + neighbors), 0 };
            draw_text(fb, TOP_W, TOP_H, x + (cell - 8) / 2,
                      y + (cell - 8) / 2, s, mine_num_color(neighbors));
        }
    } else {
        fill_rect(fb, TOP_W, TOP_H, x, y, cell, cell, C_BTN);
        bevel(fb, TOP_W, TOP_H, x, y, cell, cell, 1);
        if (flagged) {

            Color red = {200, 40, 40};
            int fx = x + 4, fy = y + 3;
            for (int j = 0; j < 6; j++) {
                hline(fb, TOP_W, TOP_H, fx, fy + j, 6 - j, red);
            }
            vline(fb, TOP_W, TOP_H, x + 4, y + 3, cell - 6, C_BLACK);
            hline(fb, TOP_W, TOP_H, x + 3, y + cell - 4, cell - 6, C_BLACK);
        }
    }
}

static void draw_mines_body(uint8_t *fb, Window *w) {
    MineLayout L;
    mine_layout(w, &L);

    int client_x = w->x + 5;
    int client_y = w->y + 20;
    int client_w = w->w - 10;
    fill_rect(fb, TOP_W, TOP_H, client_x, client_y, client_w, MINE_HEADER_H + 4, C_BTN);
    bevel(fb, TOP_W, TOP_H, client_x, client_y, client_w, MINE_HEADER_H + 4, 0);

    int mines_left = MINE_COUNT - w->mine_flags;
    draw_counter(fb, L.mines_x, L.mines_y, L.mines_w, L.mines_h, mines_left);
    draw_counter(fb, L.time_x,  L.time_y,  L.time_w,  L.time_h,  w->mine_elapsed);
    draw_mine_face(fb, L.face_x, L.face_y, L.face_sz, w->mine_state);

    int bw = MINE_COLS * L.cell;
    int bh = MINE_ROWS * L.cell;
    bevel(fb, TOP_W, TOP_H, L.board_x - 2, L.board_y - 2, bw + 4, bh + 4, 0);

    for (int y = 0; y < MINE_ROWS; y++) {
        for (int x = 0; x < MINE_COLS; x++) {
            int i = mine_idx(x, y);
            uint8_t c = w->mine_cells[i];
            int n = (c & MINE_BIT_REVEALED) ? mine_neighbors(w, x, y) : 0;
            int exploded = (w->mine_state == 2 && (c & MINE_BIT_MINE) &&
                            (c & MINE_BIT_REVEALED));
            draw_mine_cell(fb, L.board_x + x * L.cell, L.board_y + y * L.cell,
                           L.cell, c, n, exploded);
        }
    }
}
typedef enum {
    FE_MISS = 0, FE_LIST, FE_OPEN, FE_DEL, FE_REFRESH
} FeHit;

static void files_layout(Window *w,
                         int *lx, int *ly, int *lw, int *lh,
                         int *by_btn, int *bh_btn) {
    *lx = w->x + 5;
    *ly = w->y + 20;
    *lw = w->w - 10;
    *lh = w->h - 25 - 20;
    *by_btn = *ly + *lh + 2;
    *bh_btn = 16;
}

static const char *FE_LABELS[3] = { "Open", "Del", "Refresh" };
static const int   FE_WIDTHS[3] = {  40,    32,    56        };

static void draw_files_body(uint8_t *fb, Window *w) {
    int lx, ly, lw, lh, bty, bth;
    files_layout(w, &lx, &ly, &lw, &lh, &bty, &bth);

    fill_rect(fb, TOP_W, TOP_H, lx, ly, lw, lh, C_WIN);
    bevel(fb, TOP_W, TOP_H, lx, ly, lw, lh, 0);

    int line_h = 10;
    int rows = (lh - 4) / line_h;
    if (rows < 1) rows = 1;
    if (w->files_sel < w->files_scroll) w->files_scroll = w->files_sel;
    if (w->files_sel >= w->files_scroll + rows) w->files_scroll = w->files_sel - rows + 1;

    if (w->files_count == 0) {
        draw_text(fb, TOP_W, TOP_H, lx + 4, ly + 4, "(no files)", C_DIM);
        draw_text(fb, TOP_W, TOP_H, lx + 4, ly + 16,
                  "Save from Notes to", C_DIM);
        draw_text(fb, TOP_W, TOP_H, lx + 4, ly + 26,
                  "populate this list.", C_DIM);
    } else {
        int count_shown = w->files_count - w->files_scroll;
        if (count_shown > rows) count_shown = rows;
        for (int i = 0; i < count_shown; i++) {
            int idx = w->files_scroll + i;
            int ry = ly + 2 + i * line_h;
            if (idx == w->files_sel) {
                fill_rect(fb, TOP_W, TOP_H, lx + 2, ry - 1, lw - 4, line_h, C_MENU_HI);
                draw_text(fb, TOP_W, TOP_H, lx + 4, ry, w->files[idx], C_TITLETXT);
            } else {
                draw_text(fb, TOP_W, TOP_H, lx + 4, ry, w->files[idx], C_TEXT);
            }
        }
    }

    int btx = lx;
    for (int i = 0; i < 3; i++) {
        fill_rect(fb, TOP_W, TOP_H, btx, bty, FE_WIDTHS[i], bth, C_BTN);
        bevel(fb, TOP_W, TOP_H, btx, bty, FE_WIDTHS[i], bth, 1);
        int lwlab = (int)strlen(FE_LABELS[i]) * 8;
        int tx = btx + (FE_WIDTHS[i] - lwlab) / 2;
        draw_text(fb, TOP_W, TOP_H, tx, bty + 4, FE_LABELS[i], C_TEXT);
        btx += FE_WIDTHS[i] + 3;
    }

    if (w->status_ttl > 0 && w->status[0]) {
        draw_text(fb, TOP_W, TOP_H, btx + 4, bty + 4, w->status, C_TITLE);
    }
}

static FeHit files_inner_hit(Window *w, int mx, int my, int *out_idx) {
    int lx, ly, lw, lh, bty, bth;
    files_layout(w, &lx, &ly, &lw, &lh, &bty, &bth);

    if (mx >= lx && mx < lx + lw && my >= ly && my < ly + lh) {
        int line_h = 10;
        int rows = (lh - 4) / line_h;
        int rel = (my - (ly + 2)) / line_h;
        if (rel >= 0 && rel < rows) {
            int idx = w->files_scroll + rel;
            if (idx >= 0 && idx < w->files_count) {
                *out_idx = idx;
                return FE_LIST;
            }
        }
    }
    int btx = lx;
    FeHit codes[3] = { FE_OPEN, FE_DEL, FE_REFRESH };
    for (int i = 0; i < 3; i++) {
        if (mx >= btx && mx < btx + FE_WIDTHS[i] &&
            my >= bty && my < bty + bth) return codes[i];
        btx += FE_WIDTHS[i] + 3;
    }
    return FE_MISS;
}

static void draw_window(uint8_t *fb, Window *w, int is_focused) {
    if (!w->open) return;
    int x = w->x, y = w->y, ww = w->w, hh = w->h;

    fill_rect(fb, TOP_W, TOP_H, x, y, ww, hh, C_BTN);
    bevel(fb, TOP_W, TOP_H, x, y, ww, hh, 1);
    bevel(fb, TOP_W, TOP_H, x + 1, y + 1, ww - 2, hh - 2, 1);

    Color title_col = is_focused ? C_TITLE : C_TITLE_INACTIVE;
    fill_rect(fb, TOP_W, TOP_H, x + 3, y + 3, ww - 6, 14, title_col);

    draw_icon_small(fb, TOP_W, TOP_H, x + 5, y + 5, icon_small_for_kind(w->kind));
    draw_text(fb, TOP_W, TOP_H, x + 17, y + 6, w->title, C_TITLETXT);

    int cx, cy, cw, ch;
    win_close_rect(w, &cx, &cy, &cw, &ch);
    fill_rect(fb, TOP_W, TOP_H, cx, cy, cw, ch, C_BTN);
    bevel(fb, TOP_W, TOP_H, cx, cy, cw, ch, 1);
    draw_text(fb, TOP_W, TOP_H, cx + 3, cy + 1, "X", C_TEXT);

    switch (w->kind) {
        case WK_NOTEPAD: draw_notepad_body(fb, w, is_focused); break;
        case WK_ABOUT:   draw_about_body(fb, w);               break;
        case WK_CALC:    draw_calc_body(fb, w);                break;
        case WK_FILES:   draw_files_body(fb, w);               break;
        case WK_PAINT:   draw_paint_body(fb, w, is_focused);   break;
        case WK_MINES:   draw_mines_body(fb, w);               break;
    }
}

static int g_start_open = 0;
int g_uptime_ticks = 0;

typedef struct { int x, y, w, h; } Rect;
static Rect g_task_rects[NUM_WINS];

static void draw_taskbar(uint8_t *fb) {
    int ty = TOP_H - TASKBAR_H;
    fill_rect(fb, TOP_W, TOP_H, 0, ty, TOP_W, TASKBAR_H, C_TASKBAR);
    bevel(fb, TOP_W, TOP_H, 0, ty, TOP_W, TASKBAR_H, 1);

    int sx = 4, sy = ty + 3, sw = 60, sh = TASKBAR_H - 6;
    fill_rect(fb, TOP_W, TOP_H, sx, sy, sw, sh, C_BTN);
    bevel(fb, TOP_W, TOP_H, sx, sy, sw, sh, g_start_open ? 0 : 1);
    {
        const char *label = "Start";
        int lw = (int)strlen(label) * 8;
        int tx = sx + (sw - lw) / 2;
        if (g_start_open) { tx += 1; }
        draw_text(fb, TOP_W, TOP_H, tx, sy + 4, label, C_TEXT);
    }

    int clock_w = 50;
    int batt_w  = 18;
    int tray_gap = 3;
    int tray_w = clock_w + tray_gap + batt_w;
    int tray_x = TOP_W - tray_w - 4;
    int clock_x = tray_x;
    int batt_x  = clock_x + clock_w + tray_gap;
    int task_area_end = tray_x - 4;

    int open_count = 0;
    for (int i = 0; i < NUM_WINS; i++) if (g_wins[i].open) open_count++;

    int bx = sx + sw + 6;
    if (open_count > 0) {
        int total_avail = task_area_end - bx;
        int gap = 4;
        int desired = 82;
        int max_each = (total_avail - gap * (open_count - 1)) / open_count;
        int bw = desired;
        if (max_each < desired) bw = max_each;
        if (bw < 24) bw = 24;

        int top = topmost_open_any();
        for (int i = 0; i < NUM_WINS; i++) {
            Window *w = &g_wins[i];
            g_task_rects[i].w = 0;
            if (!w->open) continue;
            if (bx + bw > task_area_end) break;

            int bh = sh;
            fill_rect(fb, TOP_W, TOP_H, bx, sy, bw, bh, C_BTN);
            bevel(fb, TOP_W, TOP_H, bx, sy, bw, bh, (i == top) ? 0 : 1);
            draw_icon_small(fb, TOP_W, TOP_H, bx + 3, sy + 3, icon_small_for_kind(w->kind));

            int text_space = bw - 14;
            int max_chars = text_space / 8;
            if (max_chars < 0) max_chars = 0;
            char short_title[16];
            int n = (int)strlen(w->title);
            if (n > max_chars) n = max_chars;
            if (n > (int)sizeof(short_title) - 1) n = (int)sizeof(short_title) - 1;
            memcpy(short_title, w->title, n);
            short_title[n] = '\0';
            draw_text(fb, TOP_W, TOP_H, bx + 14, sy + 4, short_title, C_TEXT);

            g_task_rects[i].x = bx;
            g_task_rects[i].y = sy;
            g_task_rects[i].w = bw;
            g_task_rects[i].h = bh;
            bx += bw + gap;
        }
    } else {
        for (int i = 0; i < NUM_WINS; i++) g_task_rects[i].w = 0;
    }

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);

    int clock_y = ty + 3;
    int clock_h = TASKBAR_H - 6;
    fill_rect(fb, TOP_W, TOP_H, clock_x, clock_y, clock_w, clock_h, C_BTN);
    bevel(fb, TOP_W, TOP_H, clock_x, clock_y, clock_w, clock_h, 0);
    int clock_text_w = (int)strlen(buf) * 8;
    draw_text(fb, TOP_W, TOP_H,
              clock_x + (clock_w - clock_text_w) / 2,
              clock_y + (clock_h - 8) / 2,
              buf, C_TEXT);

    {
        u8 level = 0;
        u8 charging = 0;
        PTMU_GetBatteryLevel(&level);
        PTMU_GetBatteryChargeState(&charging);
        int lvl_max = 5;
        if (level > lvl_max) level = lvl_max;
        int by = clock_y;
        int bh = clock_h;
        int bx = batt_x;
        int bw = batt_w - 2;

        fill_rect(fb, TOP_W, TOP_H, bx, by, bw, bh, C_BTN);
        bevel(fb, TOP_W, TOP_H, bx, by, bw, bh, 0);

        fill_rect(fb, TOP_W, TOP_H, bx + bw, by + 3, 2, bh - 6, C_LO);

        int inner_x = bx + 2;
        int inner_y = by + 2;
        int inner_w = bw - 4;
        int inner_h = bh - 4;
        if (inner_w > 0 && inner_h > 0) {
            int fill_w = (inner_w * level + lvl_max / 2) / lvl_max;
            Color fc;
            if (charging)      fc = (Color){ 255, 220, 60 };
            else if (level >= 4) fc = (Color){ 40,  160, 40 };
            else if (level >= 2) fc = (Color){ 60,  170, 220 };
            else                 fc = (Color){ 210, 40,  40 };
            if (fill_w > 0)
                fill_rect(fb, TOP_W, TOP_H, inner_x, inner_y, fill_w, inner_h, fc);
        }
    }
}

static int start_button_hit(int mx, int my) {
    int ty = TOP_H - TASKBAR_H;
    return (mx >= 4 && mx < 64 && my >= ty + 3 && my < ty + TASKBAR_H - 3);
}

static int task_button_hit(int mx, int my) {
    for (int i = 0; i < NUM_WINS; i++) {
        Rect *r = &g_task_rects[i];
        if (r->w == 0) continue;
        if (mx >= r->x && mx < r->x + r->w &&
            my >= r->y && my < r->y + r->h)
            return i;
    }
    return -1;
}

#define MENU_W      150
#define MENU_ITEM_H 20

static const char *MENU_ITEMS[] = {
    "Notes",
    "Paint",
    "Calculator",
    "Minesweeper",
    "File Explorer",
    "About...",
    "Shut Down",
};
#define MENU_COUNT ((int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0])))

static const int MENU_KINDS[] = {
    WK_NOTEPAD, WK_PAINT, WK_CALC, WK_MINES, WK_FILES, WK_ABOUT, -1
};

static void menu_rect(int *x, int *y, int *w, int *h) {
    *w = MENU_W;
    *h = MENU_COUNT * MENU_ITEM_H + 6;
    *x = 2;
    *y = TOP_H - TASKBAR_H - *h;
}

static int menu_item_at(int mx, int my) {
    int mx0, my0, mw, mh;
    menu_rect(&mx0, &my0, &mw, &mh);
    if (mx < mx0 || mx >= mx0 + mw || my < my0 || my >= my0 + mh) return -1;
    int rel = my - (my0 + 3);
    if (rel < 0) return -1;
    int idx = rel / MENU_ITEM_H;
    if (idx < 0 || idx >= MENU_COUNT) return -1;
    return idx;
}

static void draw_start_menu(uint8_t *fb, int cursor_x, int cursor_y) {
    if (!g_start_open) return;
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);
    fill_rect(fb, TOP_W, TOP_H, x, y, w, h, C_MENU);
    bevel(fb, TOP_W, TOP_H, x, y, w, h, 1);

    fill_rect(fb, TOP_W, TOP_H, x + 2, y + 2, 14, h - 4, C_TITLE);
    {
        const char *ribbon = "Artemis OS";
        int n = (int)strlen(ribbon);
        int avail = h - 6;
        int step = n > 0 ? avail / n : 10;
        if (step > 10) step = 10;
        int y0 = y + 4 + (avail - step * n) / 2;
        for (int i = 0; i < n; i++) {
            char s[2] = { ribbon[i], 0 };
            draw_text(fb, TOP_W, TOP_H, x + 5, y0 + i * step, s, C_TITLETXT);
        }
    }

    int hover = menu_item_at(cursor_x, cursor_y);
    for (int i = 0; i < MENU_COUNT; i++) {
        int iy = y + 3 + i * MENU_ITEM_H;
        Color txt = C_TEXT;
        if (i == hover) {
            fill_rect(fb, TOP_W, TOP_H, x + 18, iy, w - 22, MENU_ITEM_H, C_MENU_HI);
            txt = C_TITLETXT;
        }
        const char * const *ic = (MENU_KINDS[i] == -1)
                                   ? icon_for_shutdown()
                                   : icon_for_kind(MENU_KINDS[i]);
        int icon_y = iy + (MENU_ITEM_H - ICON_SZ) / 2;
        draw_icon(fb, TOP_W, TOP_H, x + 20, icon_y, ic);
        draw_text(fb, TOP_W, TOP_H, x + 42, iy + (MENU_ITEM_H - 8) / 2,
                  MENU_ITEMS[i], txt);
    }
}

#define K_SHIFT -1
#define K_BKSP  -2
#define K_RET   -3
#define K_SPACE -4

typedef struct {
    int  x, y, w, h;
    int  code;
    const char *label;
    int  special;
} Key;

static Key g_keys[48];
static int g_nkeys = 0;
static int g_shift = 0;
static int g_pressed_key = -1;

static char g_lbl_pool[64][4];

static void add_key(int gx, int gy, int span, int code,
                    const char *label, int special) {
    Key *k = &g_keys[g_nkeys++];
    k->x = gx * KEY_W;
    k->y = KB_Y + gy * KEY_H;
    k->w = KEY_W * span;
    k->h = KEY_H;
    k->code  = code;
    k->label = label;
    k->special = special;
}

static void build_keyboard(void) {
    g_nkeys = 0;
    int lb = 0;

    const char *row0 = "1234567890";
    const char *row1 = "qwertyuiop";
    const char *row2 = "asdfghjkl";
    const char *row3 = "zxcvbnm";

    for (int i = 0; i < 10; i++) {
        g_lbl_pool[lb][0] = row0[i]; g_lbl_pool[lb][1] = '\0';
        add_key(i, 0, 1, row0[i], g_lbl_pool[lb], 0); lb++;
    }
    for (int i = 0; i < 10; i++) {
        g_lbl_pool[lb][0] = row1[i]; g_lbl_pool[lb][1] = '\0';
        add_key(i, 1, 1, row1[i], g_lbl_pool[lb], 0); lb++;
    }
    for (int i = 0; i < 9; i++) {
        g_lbl_pool[lb][0] = row2[i]; g_lbl_pool[lb][1] = '\0';
        add_key(i, 2, 1, row2[i], g_lbl_pool[lb], 0); lb++;
    }
    add_key(9, 2, 1, K_BKSP, "<-", 1);

    add_key(0, 3, 1, K_SHIFT, "Sh", 1);
    for (int i = 0; i < 7; i++) {
        g_lbl_pool[lb][0] = row3[i]; g_lbl_pool[lb][1] = '\0';
        add_key(1 + i, 3, 1, row3[i], g_lbl_pool[lb], 0); lb++;
    }
    add_key(8, 3, 2, K_RET, "Ret", 1);

    add_key(0, 4, 10, K_SPACE, "Space", 1);
}

static int key_at(int tx, int ty) {
    for (int i = 0; i < g_nkeys; i++) {
        Key *k = &g_keys[i];
        if (tx >= k->x && tx < k->x + k->w && ty >= k->y && ty < k->y + k->h)
            return i;
    }
    return -1;
}

static void draw_keyboard(uint8_t *fb) {
    fill_rect(fb, BOT_W, BOT_H, 0, KB_Y, BOT_W, KB_H, C_TASKBAR);
    for (int i = 0; i < g_nkeys; i++) {
        Key *k = &g_keys[i];
        int dn = (i == g_pressed_key) || (k->code == K_SHIFT && g_shift);
        Color base = k->special ? C_KEYSPEC : C_KEY;
        Color c    = dn ? C_KEYDN : base;
        fill_rect(fb, BOT_W, BOT_H, k->x + 2, k->y + 2, k->w - 4, k->h - 4, c);
        bevel(fb, BOT_W, BOT_H, k->x + 2, k->y + 2, k->w - 4, k->h - 4, dn ? 0 : 1);

        const char *lab = k->label;
        char upper[4] = {0};
        if (g_shift && k->code >= 'a' && k->code <= 'z') {
            upper[0] = (char)(k->code - 'a' + 'A');
            lab = upper;
        } else if (g_shift && k->code >= '0' && k->code <= '9') {
            static const char sym[] = "!@#$%?&*.,";
            upper[0] = sym[k->code - '0'];
            lab = upper;
        }
        int lw = (int)strlen(lab) * 8;
        int tx = k->x + (k->w - lw) / 2;
        int ty = k->y + (k->h - 8) / 2;
        draw_text(fb, BOT_W, BOT_H, tx, ty, lab, C_TEXT);
    }
}

static int g_cursor_x = TOP_W / 2;
static int g_cursor_y = TOP_H / 2;

static void draw_trackpad(uint8_t *fb) {
    fill_rect(fb, BOT_W, BOT_H, 0, PAD_Y, BOT_W, PAD_H, C_PAD);
    bevel(fb, BOT_W, BOT_H, 4, PAD_Y + 4, BOT_W - 8, PAD_H - 8, 0);
    for (int y = PAD_Y + 20; y < PAD_Y + PAD_H - 12; y += 14) {
        for (int x = 16; x < BOT_W - 16; x += 14) {
            put_px(fb, BOT_W, BOT_H, x,     y,     C_PADDOT);
            put_px(fb, BOT_W, BOT_H, x + 1, y,     C_PADDOT);
            put_px(fb, BOT_W, BOT_H, x,     y + 1, C_PADDOT);
            put_px(fb, BOT_W, BOT_H, x + 1, y + 1, C_PADDOT);
        }
    }
}

static Color icon_pal(char c) {
    switch (c) {
        case 'K': return (Color){0, 0, 0};
        case 'W': return (Color){255, 255, 255};
        case 'N': return (Color){0, 0, 128};
        case 'B': return (Color){60, 120, 220};
        case 'L': return (Color){200, 200, 200};
        case 'D': return (Color){100, 100, 100};
        case 'G': return (Color){140, 190, 130};
        case 'Y': return (Color){245, 215, 90};
        case 'O': return (Color){200, 150, 40};
        case 'R': return (Color){200, 40, 40};
        case 'S': return (Color){150, 150, 150};
        default:  return (Color){0, 0, 0};
    }
}

static void draw_icon(uint8_t *fb, int fb_w, int fb_h, int x, int y,
                      const char * const *map) {
    for (int j = 0; j < ICON_SZ; j++) {
        const char *row = map[j];
        for (int i = 0; i < ICON_SZ; i++) {
            char c = row[i];
            if (c == '.' || c == '\0') continue;
            put_px(fb, fb_w, fb_h, x + i, y + j, icon_pal(c));
        }
    }
}

static void draw_icon_small(uint8_t *fb, int fb_w, int fb_h, int x, int y,
                            const char * const *map) {
    for (int j = 0; j < ICON_SMALL_SZ; j++) {
        const char *row = map[j];
        for (int i = 0; i < ICON_SMALL_SZ; i++) {
            char c = row[i];
            if (c == '.' || c == '\0') continue;
            put_px(fb, fb_w, fb_h, x + i, y + j, icon_pal(c));
        }
    }
}

static const char *ICON_NOTES[ICON_SZ] = {
    "................",
    ".KKKKKKKKKKK....",
    ".KWWWWWWWWWSK...",
    ".KWWWWWWWWWWSK..",
    ".KNNNNNNNNWWWSK.",
    ".KWWWWWWWWWWWWK.",
    ".KNNNNNNNNNNNWK.",
    ".KWWWWWWWWWWWWK.",
    ".KNNNNNNNNNNNWK.",
    ".KWWWWWWWWWWWWK.",
    ".KNNNNNNNNNNNWK.",
    ".KWWWWWWWWWWWWK.",
    ".KNNNNNNNNNNNWK.",
    ".KWWWWWWWWWWWWK.",
    ".KKKKKKKKKKKKKK.",
    "................",
};

static const char *ICON_CALC[ICON_SZ] = {
    "................",
    ".KKKKKKKKKKKKKK.",
    ".KLLLLLLLLLLLLK.",
    ".KLGGGGGGGGGGLK.",
    ".KLGGGGGGGGGGLK.",
    ".KLLLLLLLLLLLLK.",
    ".KLWWLWWLWWLRRK.",
    ".KLLLLLLLLLLLLK.",
    ".KLWWLWWLWWLRRK.",
    ".KLLLLLLLLLLLLK.",
    ".KLWWLWWLWWLRRK.",
    ".KLLLLLLLLLLLLK.",
    ".KLWWLWWLWWLRRK.",
    ".KLLLLLLLLLLLLK.",
    ".KKKKKKKKKKKKKK.",
    "................",
};

static const char *ICON_FILES[ICON_SZ] = {
    "................",
    "................",
    "...KKKKKK.......",
    "...KOOOOK.......",
    ".KKKOOOOKKKKKKK.",
    ".KYYYYYYYYYYYYK.",
    ".KYYYYYYYYYYYYK.",
    ".KYYYYYYYYYYYYK.",
    ".KYYYYYYYYYYYYK.",
    ".KYYYYYYYYYYYYK.",
    ".KYYYYYYYYYYYYK.",
    ".KYYYYYYYYYYYYK.",
    ".KYYYYYYYYYYYYK.",
    ".KOOOOOOOOOOOOK.",
    ".KKKKKKKKKKKKKK.",
    "................",
};

static const char *ICON_ABOUT[ICON_SZ] = {
    "................",
    ".....KKKKKK.....",
    "...KKBBBBBBKK...",
    "..KBBBBBBBBBBK..",
    ".KBBBBBWWBBBBBK.",
    ".KBBBBBWWBBBBBK.",
    ".KBBBBBBBBBBBBK.",
    ".KBBBBBWWBBBBBK.",
    ".KBBBBBWWBBBBBK.",
    ".KBBBBBWWBBBBBK.",
    ".KBBBBBWWBBBBBK.",
    ".KBBBBBWWBBBBBK.",
    ".KBBBBBBBBBBBBK.",
    "..KBBBBBBBBBBK..",
    "...KKBBBBBBKK...",
    ".....KKKKKK.....",
};

static const char *ICON_PAINT[ICON_SZ] = {
    "................",
    "....KKKKKKK.....",
    "...KWWWWWWWKK...",
    "..KWWRRRWGGGWK..",
    ".KWWRRRWWGGGWWK.",
    ".KWWRRRWWGGGWWK.",
    ".KWWWWWWWWWWWWK.",
    ".KWWBBBWWYYYWWK.",
    ".KWWBBBWWYYYWWK.",
    ".KWWWWWWWWWWWWK.",
    "..KWWWWWWWWWWK..",
    "...KWWWWWWWWWK..",
    "....KWWWWWWWWK..",
    "....KKWWWWWWK...",
    "......KKKKKK....",
    "................",
};

static const char *ICON_MINES[ICON_SZ] = {
    "................",
    "................",
    ".........KK.....",
    ".........KRK....",
    "..........KYK...",
    "....KKKK..KR....",
    "...KKKKKKKK.....",
    "..KKKKWWKKKK....",
    "..KKKKKKKKKK....",
    "..KKKKKKKKKK....",
    "..KKKKKKKKKK....",
    "..KKKKKKKKKK....",
    "...KKKKKKKK.....",
    "....KKKKKK......",
    "................",
    "................",
};

static const char * const *icon_for_kind(int kind) {
    switch (kind) {
        case WK_NOTEPAD: return ICON_NOTES;
        case WK_CALC:    return ICON_CALC;
        case WK_FILES:   return ICON_FILES;
        case WK_ABOUT:   return ICON_ABOUT;
        case WK_PAINT:   return ICON_PAINT;
        case WK_MINES:   return ICON_MINES;
    }
    return ICON_ABOUT;
}

static const char *ICON_NOTES_S[ICON_SMALL_SZ] = {
    "KKKKKKK.",
    "KWWWWWSK",
    "KNNNNWSK",
    "KWWWWWWK",
    "KNNNNNWK",
    "KWWWWWWK",
    "KNNNNNWK",
    "KKKKKKKK",
};

static const char *ICON_CALC_S[ICON_SMALL_SZ] = {
    "KKKKKKKK",
    "KGGGGGGK",
    "KGGGGGGK",
    "KKKKKKKK",
    "KWLWLWRK",
    "KLLLLLLK",
    "KWLWLWRK",
    "KKKKKKKK",
};

static const char *ICON_FILES_S[ICON_SMALL_SZ] = {
    "........",
    "KKK.....",
    "KYYKKKKK",
    "KYYYYYYK",
    "KYYYYYYK",
    "KYYYYYYK",
    "KOOOOOOK",
    "KKKKKKKK",
};

static const char *ICON_ABOUT_S[ICON_SMALL_SZ] = {
    ".KKKKKK.",
    "KBBWWBBK",
    "KBBBBBBK",
    "KBBWWBBK",
    "KBBWWBBK",
    "KBBWWBBK",
    "KBBBBBBK",
    ".KKKKKK.",
};

static const char *ICON_PAINT_S[ICON_SMALL_SZ] = {
    ".KKKKKK.",
    "KRRKGGKK",
    "KRRKGGWK",
    "KWWWWWWK",
    "KBBKYYWK",
    "KBBKYYWK",
    "KWWWWWK.",
    ".KKKKKK.",
};

static const char *ICON_MINES_S[ICON_SMALL_SZ] = {
    "......KR",
    ".....KYK",
    ".....K..",
    ".KKKK...",
    "KKKKKKK.",
    "KKKWKKK.",
    "KKKKKKK.",
    ".KKKKK..",
};

const char *ICON_TOOL_PEN[ICON_SMALL_SZ] = {
    "......KK",
    ".....KYK",
    "....KYYK",
    "...KYWLK",
    "..KLWLK.",
    ".KLLLK..",
    "KKKKK...",
    ".KK.....",
};

const char *ICON_TOOL_BRUSH[ICON_SMALL_SZ] = {
    "......KK",
    ".....KKK",
    "....KNNK",
    "...KNNK.",
    "..KBBNK.",
    ".KKBBBK.",
    "KLWWBK..",
    "KKKKK...",
};

const char *ICON_TOOL_ERASER[ICON_SMALL_SZ] = {
    "........",
    "..KKKKK.",
    ".KRRRRRK",
    "KRRRRRRK",
    "KWWWWWWK",
    "KWWWWWK.",
    ".KKKKK..",
    "........",
};

const char *ICON_TOOL_FILL[ICON_SMALL_SZ] = {
    ".KK.....",
    ".KB.....",
    "KKBK....",
    "KBBBK...",
    "KBBBBKK.",
    ".KBBBBBK",
    "..KBBBBK",
    "...KKKK.",
};

const char *ICON_TOOL_LINE[ICON_SMALL_SZ] = {
    "........",
    ".KK.....",
    "KKKK....",
    ".KKKK...",
    "..KKKK..",
    "...KKKK.",
    "....KKKK",
    ".....KK.",
};

const char *ICON_TOOL_TOUCH[ICON_SMALL_SZ] = {
    ".....KK.",
    "....KLK.",
    "...KLLK.",
    "..KLLK..",
    ".KWLK...",
    ".KWK....",
    "KKKKKKKK",
    "DDDDDDDD",
};

static const char * const *icon_small_for_kind(int kind) {
    switch (kind) {
        case WK_NOTEPAD: return ICON_NOTES_S;
        case WK_CALC:    return ICON_CALC_S;
        case WK_FILES:   return ICON_FILES_S;
        case WK_ABOUT:   return ICON_ABOUT_S;
        case WK_PAINT:   return ICON_PAINT_S;
        case WK_MINES:   return ICON_MINES_S;
    }
    return ICON_ABOUT_S;
}

static const char *ICON_POWER[ICON_SZ] = {
    "................",
    ".......KK.......",
    ".......KK.......",
    "..KK...KK...KK..",
    ".KRRK..KK..KRRK.",
    ".KRRK..KK..KRRK.",
    "KRRK........KRRK",
    "KRRK........KRRK",
    "KRRK........KRRK",
    "KRRK........KRRK",
    ".KRRK......KRRK.",
    ".KRRK......KRRK.",
    "..KRRKKKKKKRRK..",
    "...KRRRRRRRRK...",
    "....KKKKKKKK....",
    "................",
};

static const char * const *icon_for_shutdown(void) {
    return ICON_POWER;
}

typedef struct {
    int x, y;
    const char *name;
    WKind kind;
    const char * const *icon;
} DesktopIcon;

static const DesktopIcon g_icons[] = {
    { 12,  10, "Notes", WK_NOTEPAD, ICON_NOTES  },
    { 12,  58, "Calc",  WK_CALC,    ICON_CALC   },
    { 12, 106, "Files", WK_FILES,   ICON_FILES  },
    { 12, 154, "About", WK_ABOUT,   ICON_ABOUT  },
    { 56,  10, "Paint", WK_PAINT,   ICON_PAINT  },
    { 56,  58, "Mines", WK_MINES,   ICON_MINES  },
};
#define NUM_ICONS ((int)(sizeof(g_icons) / sizeof(g_icons[0])))

static void draw_desktop(uint8_t *fb) {
    fill_rect(fb, TOP_W, TOP_H, 0, 0, TOP_W, DESK_H, C_DESKTOP);
    for (int i = 0; i < NUM_ICONS; i++) {
        const DesktopIcon *ic = &g_icons[i];
        draw_icon(fb, TOP_W, TOP_H, ic->x, ic->y, ic->icon);

        int lw = (int)strlen(ic->name) * 8;
        int tx = ic->x + (ICON_SZ - lw) / 2;
        int ty = ic->y + ICON_SZ + 2;
        draw_text(fb, TOP_W, TOP_H, tx + 1, ty + 1, ic->name, C_BLACK);
        draw_text(fb, TOP_W, TOP_H, tx,     ty,     ic->name, C_TITLETXT);
    }
}

static int desktop_icon_hit(int mx, int my, WKind *out_kind) {
    for (int i = 0; i < NUM_ICONS; i++) {
        const DesktopIcon *ic = &g_icons[i];

        if (mx >= ic->x && mx < ic->x + ICON_SZ &&
            my >= ic->y && my < ic->y + ICON_SZ + 12) {
            *out_kind = ic->kind;
            return 1;
        }
    }
    return 0;
}

static int has_suffix_ci(const char *s, const char *suffix) {
    int sl = (int)strlen(s);
    int xl = (int)strlen(suffix);
    if (xl > sl) return 0;
    for (int i = 0; i < xl; i++) {
        char a = s[sl - xl + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return 0;
    }
    return 1;
}

static void file_explorer_open_selected(Window *fe) {
    if (fe->files_count == 0) return;
    if (fe->files_sel < 0 || fe->files_sel >= fe->files_count) return;
    const char *name = fe->files[fe->files_sel];
    if (has_suffix_ci(name, ".apt")) {
        Window *pt = &g_wins[WK_PAINT];
        if (!pt->open) paint_reset();
        if (paint_load(pt, name)) { set_status(pt, "Loaded."); sfx_play(SFX_DING); }
        else                      { set_status(pt, "Load failed."); sfx_play(SFX_ERROR); }
        bring_to_front(WK_PAINT);
    } else {
        Window *np = &g_wins[WK_NOTEPAD];
        if (!np->open) notepad_reset();
        if (load_note(np, name)) { set_status(np, "Loaded."); sfx_play(SFX_DING); }
        else                     { set_status(np, "Load failed."); sfx_play(SFX_ERROR); }
        bring_to_front(WK_NOTEPAD);
    }
}

int g_quit_flag = 0;
static int g_paint_stroking = 0;
int g_paint_touch_draw = 0;

static void do_click(int mx, int my) {
    sfx_play(SFX_CLICK);
    if (g_drag_idx >= 0) { g_drag_idx = -1; return; }

    if (g_start_open) {
        int idx = menu_item_at(mx, my);
        if (idx >= 0) {
            g_start_open = 0;
            int kind = MENU_KINDS[idx];
            if (kind == -1) {
                sfx_play(SFX_SHUTDOWN);

                svcSleepThread(700 * 1000 * 1000LL);
                g_quit_flag = 1;
            } else {
                open_app((WKind)kind);
            }
            return;
        }
        g_start_open = 0;
    }

    if (start_button_hit(mx, my)) { g_start_open = !g_start_open; return; }

    int tb = task_button_hit(mx, my);
    if (tb >= 0) {
        if (!g_wins[tb].open) open_app((WKind)tb);
        else                  bring_to_front(tb);
        return;
    }

    for (int i = NUM_WINS - 1; i >= 0; i--) {
        int k = g_z[i];
        Window *w = &g_wins[k];
        if (!w->open) continue;
        int h = win_hit(w, mx, my);
        if (!h) continue;
        if (h == 1) { w->open = 0; return; }
        bring_to_front(k);
        if (h == 2) {
            g_drag_idx   = k;
            g_drag_off_x = mx - w->x;
            g_drag_off_y = my - w->y;
            return;
        }
        if (w->kind == WK_CALC) {
            int r, c;
            if (calc_hit_button(w, mx, my, &r, &c)) {
                const char *lab = CALC_LABELS[r][c];
                char code = lab[0];
                calc_input(w, code);
            }
        } else if (w->kind == WK_NOTEPAD) {
            NpHit nh = notepad_inner_hit(w, mx, my);
            switch (nh) {
                case NP_FNAME:
                    w->focus_filename = 1;
                    break;
                case NP_NEW:
                    w->text[0] = '\0';
                    w->text_len = 0;
                    strcpy(w->filename, "untitled.txt");
                    w->focus_filename = 0;
                    set_status(w, "New file.");
                    break;
                case NP_SAVE:
                    w->focus_filename = 0;
                    if (save_note(w)) { set_status(w, "Saved."); sfx_play(SFX_DING); }
                    else              { set_status(w, "Save failed."); sfx_play(SFX_ERROR); }
                    break;
                case NP_OPEN:
                    open_app(WK_FILES);
                    break;
                case NP_BODY:
                    w->focus_filename = 0;
                    break;
                default: break;
            }
        } else if (w->kind == WK_FILES) {
            int idx = -1;
            FeHit fh = files_inner_hit(w, mx, my, &idx);
            switch (fh) {
                case FE_LIST:
                    if (idx == w->files_sel) {
                        file_explorer_open_selected(w);
                    } else {
                        w->files_sel = idx;
                    }
                    break;
                case FE_OPEN:
                    file_explorer_open_selected(w);
                    break;
                case FE_DEL:
                    if (w->files_count > 0 && w->files_sel < w->files_count) {
                        if (delete_note(w->files[w->files_sel]))
                            set_status(w, "Deleted.");
                        else
                            set_status(w, "Delete failed.");
                        files_refresh(w);
                    }
                    break;
                case FE_REFRESH:
                    files_refresh(w);
                    set_status(w, "Refreshed.");
                    break;
                default: break;
            }
        } else if (w->kind == WK_PAINT) {
            int a = 0, b = 0;
            PaHit ph = paint_inner_hit(w, mx, my, &a, &b);
            switch (ph) {
                case PA_FNAME:
                    w->focus_filename = 1;
                    break;
                case PA_NEW:
                    paint_fill_all(w, 0);
                    strcpy(w->filename, "untitled.apt");
                    w->focus_filename = 0;
                    w->paint_line_sx = -1;
                    w->paint_line_sy = -1;
                    set_status(w, "New canvas.");
                    break;
                case PA_SAVE:
                    w->focus_filename = 0;
                    if (paint_save(w)) { set_status(w, "Saved."); sfx_play(SFX_DING); }
                    else               { set_status(w, "Save failed."); sfx_play(SFX_ERROR); }
                    break;
                case PA_OPEN:
                    open_app(WK_FILES);
                    break;
                case PA_TOOL:
                    if (a == PAINT_NUM_TOOLS) {

                        g_paint_touch_draw = !g_paint_touch_draw;
                        set_status(w, g_paint_touch_draw ? "Touch-draw on." : "Touch-draw off.");
                    } else {
                        w->paint_tool = a;
                    }
                    w->focus_filename = 0;
                    w->paint_line_sx = -1;
                    w->paint_line_sy = -1;
                    break;
                case PA_COLOR:
                    w->paint_color = a;
                    w->focus_filename = 0;
                    break;
                case PA_CANVAS:

                    w->focus_filename = 0;
                    paint_stroke(w, a, b, 1);
                    g_paint_stroking = 1;
                    break;
                default: break;
            }
        } else if (w->kind == WK_MINES) {
            int cx = 0, cy = 0;
            MineHitKind mh = mine_hit(w, mx, my, &cx, &cy);
            if (mh == MH_FACE) {
                mine_clear_board(w);
            } else if (mh == MH_CELL) {
                if (w->mine_state != 0) {

                    mine_clear_board(w);
                } else {
                    if (w->mine_first_click) {
                        mine_seed(w, cx, cy);
                        w->mine_first_click = 0;
                        w->mine_start_tick = g_uptime_ticks;
                    }
                    mine_reveal(w, cx, cy);
                }
            }
        }
        return;
    }

    WKind ik;
    if (desktop_icon_hit(mx, my, &ik)) { open_app(ik); return; }
}

static void kbd_press(int ki) {
    Key *k = &g_keys[ki];
    g_pressed_key = ki;
    sfx_play(SFX_CLICK);
    if (k->code == K_SHIFT) { g_shift = !g_shift; return; }

    char c;
    switch (k->code) {
        case K_BKSP:  c = '\b'; break;
        case K_RET:   c = '\n'; break;
        case K_SPACE: c = ' ';  break;
        default:
            c = (char)k->code;
            if (g_shift && c >= 'a' && c <= 'z') {
                c = c - 'a' + 'A';
            } else if (g_shift && c >= '0' && c <= '9') {

                static const char sym[] = "!@#$%?&*.,";
                c = sym[c - '0'];
            }
            break;
    }

    int tw = topmost_open_input_window();
    if (tw >= 0) notepad_putc(&g_wins[tw], c);
}

int main(void) {
    gfxInitDefault();
    ptmuInit();
    sfx_init();
    ensure_artemis_dir();

    memset(g_wins, 0, sizeof(g_wins));
    g_wins[WK_NOTEPAD].kind = WK_NOTEPAD;
    g_wins[WK_CALC].kind    = WK_CALC;
    g_wins[WK_ABOUT].kind   = WK_ABOUT;
    g_wins[WK_FILES].kind   = WK_FILES;
    g_wins[WK_PAINT].kind   = WK_PAINT;
    g_wins[WK_MINES].kind   = WK_MINES;

    build_keyboard();

    int prev_touching     = 0;
    int touch_start_x     = 0;
    int touch_start_y     = 0;
    int touch_prev_x      = 0;
    int touch_prev_y      = 0;
    int touch_started_pad = 0;
    int touch_moved       = 0;

    sfx_play(SFX_STARTUP);

    while (aptMainLoop() && !g_quit_flag) {
        g_uptime_ticks++;
        g_caret_tick = (g_caret_tick + 1) % 60;

        for (int i = 0; i < NUM_WINS; i++) {
            if (g_wins[i].status_ttl > 0) g_wins[i].status_ttl--;
        }

        {
            Window *mw = &g_wins[WK_MINES];
            if (mw->open && mw->mine_state == 0 && !mw->mine_first_click) {
                mw->mine_elapsed = (g_uptime_ticks - mw->mine_start_tick) / 60;
                if (mw->mine_elapsed > 999) mw->mine_elapsed = 999;
            }
        }

        hidScanInput();
        uint32_t kDown = hidKeysDown();
        uint32_t kHeld = hidKeysHeld();

        if (kDown & KEY_SELECT) break;
        if (kDown & KEY_START)  g_start_open = !g_start_open;
        if (kDown & KEY_B) {
            int top = topmost_open_any();
            if (top >= 0) g_wins[top].open = 0;
        }

        int dx = 0, dy = 0;
        if (kHeld & KEY_DLEFT)  dx -= 2;
        if (kHeld & KEY_DRIGHT) dx += 2;
        if (kHeld & KEY_DUP)    dy -= 2;
        if (kHeld & KEY_DDOWN)  dy += 2;
        circlePosition cp;
        hidCircleRead(&cp);
        if (cp.dx >  40) dx += 3;
        if (cp.dx < -40) dx -= 3;
        if (cp.dy >  40) dy -= 3;
        if (cp.dy < -40) dy += 3;
        g_cursor_x += dx;
        g_cursor_y += dy;

        if (kDown & KEY_A) do_click(g_cursor_x, g_cursor_y);
        if (kDown & KEY_L) do_click(g_cursor_x, g_cursor_y);

        if (kDown & KEY_R) {
            int top = topmost_open_any();
            if (top == WK_MINES) {
                Window *mw = &g_wins[top];
                int cx = 0, cy = 0;
                if (mine_hit(mw, g_cursor_x, g_cursor_y, &cx, &cy) == MH_CELL) {
                    mine_toggle_flag(mw, cx, cy);
                }
            }
        }

        if (g_paint_stroking) {
            int still_held = (kHeld & KEY_A) || (kHeld & KEY_L) ||
                             ((kHeld & KEY_TOUCH) && touch_started_pad);
            if (!still_held) {
                g_paint_stroking = 0;
                Window *pw = &g_wins[WK_PAINT];
                if (pw->open) paint_stroke_end(pw);
            } else {
                Window *pw = &g_wins[WK_PAINT];
                if (pw->open && pw->kind == WK_PAINT) {
                    PaintLayout L;
                    paint_layout(pw, &L);
                    int cx = g_cursor_x - L.cv_x;
                    int cy = g_cursor_y - L.cv_y;
                    if (cx >= 0 && cx < L.cv_w && cy >= 0 && cy < L.cv_h) {
                        paint_stroke(pw, cx, cy, 0);
                    }
                }
            }
        }

        int touching = (kHeld & KEY_TOUCH) ? 1 : 0;
        touchPosition tp = {0, 0};
        if (touching) hidTouchRead(&tp);

        int tdraw_active_in = (g_paint_touch_draw && g_wins[WK_PAINT].open);

        if (tdraw_active_in) {
            Window *pw = &g_wins[WK_PAINT];
            if (touching && !prev_touching) {
                int a = 0, b = 0;
                PbHit h = paint_bot_hit(tp.px, tp.py, &a, &b);
                switch (h) {
                    case PB_CANVAS:
                        paint_stroke(pw, a, b, 1);
                        g_paint_stroking = 1;
                        break;
                    case PB_TOOL:
                        if (a == PAINT_NUM_TOOLS) {
                            g_paint_touch_draw = 0;
                            set_status(pw, "Touch-draw off.");
                        } else {
                            pw->paint_tool = a;
                            pw->paint_line_sx = -1;
                            pw->paint_line_sy = -1;
                        }
                        break;
                    case PB_COLOR:
                        pw->paint_color = a;
                        break;
                    case PB_NEW:
                        paint_fill_all(pw, 0);
                        strcpy(pw->filename, "untitled.apt");
                        pw->paint_line_sx = -1;
                        pw->paint_line_sy = -1;
                        set_status(pw, "New canvas.");
                        break;
                    case PB_OPEN:

                        g_paint_touch_draw = 0;
                        open_app(WK_FILES);
                        break;
                    case PB_SAVE:
                        if (paint_save(pw)) { set_status(pw, "Saved."); sfx_play(SFX_DING); }
                        else                { set_status(pw, "Save failed."); sfx_play(SFX_ERROR); }
                        break;
                    default: break;
                }
            } else if (touching && prev_touching) {

                if (g_paint_stroking) {
                    PaintBotLayout L;
                    paint_bot_layout(&L);
                    int cx = tp.px - L.cv_x;
                    int cy = tp.py - L.cv_y;
                    if (cx >= 0 && cx < PAINT_CW && cy >= 0 && cy < PAINT_CH) {
                        paint_stroke(pw, cx, cy, 0);
                    }
                }
            } else if (!touching && prev_touching) {
                if (g_paint_stroking) {
                    paint_stroke_end(pw);
                    g_paint_stroking = 0;
                }
            }
            prev_touching = touching;
        } else if (touching && !prev_touching) {
            touch_start_x = tp.px;
            touch_start_y = tp.py;
            touch_prev_x  = tp.px;
            touch_prev_y  = tp.py;
            touch_moved   = 0;
            touch_started_pad = (tp.py < PAD_Y + PAD_H) ? 1 : 0;
            if (!touch_started_pad) {
                int ki = key_at(tp.px, tp.py);
                if (ki >= 0) kbd_press(ki);
            } else {

                int top = topmost_open_any();
                if (top == WK_PAINT) {
                    Window *pw = &g_wins[WK_PAINT];
                    PaintLayout L;
                    paint_layout(pw, &L);
                    int cx = g_cursor_x - L.cv_x;
                    int cy = g_cursor_y - L.cv_y;
                    if (cx >= 0 && cx < L.cv_w && cy >= 0 && cy < L.cv_h) {
                        paint_stroke(pw, cx, cy, 1);
                        g_paint_stroking = 1;
                    }
                }
            }
        } else if (touching && prev_touching) {
            if (touch_started_pad) {
                g_cursor_x += tp.px - touch_prev_x;
                g_cursor_y += tp.py - touch_prev_y;
                if (abs(tp.px - touch_start_x) > 4 ||
                    abs(tp.py - touch_start_y) > 4) touch_moved = 1;
            }
            touch_prev_x = tp.px;
            touch_prev_y = tp.py;
        } else if (!touching && prev_touching) {
            if (touch_started_pad && !touch_moved && !g_paint_stroking) {
                do_click(g_cursor_x, g_cursor_y);
            }
            g_pressed_key = -1;
        }
        prev_touching = touching;

        if (g_drag_idx >= 0) {
            Window *w = &g_wins[g_drag_idx];
            w->x = g_cursor_x - g_drag_off_x;
            w->y = g_cursor_y - g_drag_off_y;
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            if (w->x + w->w > TOP_W)          w->x = TOP_W - w->w;
            if (w->y + w->h > DESK_H - 4)     w->y = DESK_H - 4 - w->h;
            if (!w->open) g_drag_idx = -1;
        }

        if (g_cursor_x < 0)         g_cursor_x = 0;
        if (g_cursor_x > TOP_W - 2) g_cursor_x = TOP_W - 2;
        if (g_cursor_y < 0)         g_cursor_y = 0;
        if (g_cursor_y > TOP_H - 2) g_cursor_y = TOP_H - 2;

        uint8_t *top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        uint8_t *bot = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);

        draw_desktop(top);
        int focused = topmost_open_any();
        for (int i = 0; i < NUM_WINS; i++) {
            int k = g_z[i];
            draw_window(top, &g_wins[k], k == focused);
        }
        draw_taskbar(top);
        draw_start_menu(top, g_cursor_x, g_cursor_y);
        draw_cursor(top, g_cursor_x, g_cursor_y);

        int tdraw_active = (g_paint_touch_draw && g_wins[WK_PAINT].open);
        if (tdraw_active) {
            draw_paint_bottom(bot, &g_wins[WK_PAINT]);
        } else {
            draw_trackpad(bot);
            draw_keyboard(bot);
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    sfx_exit();
    ptmuExit();
    gfxExit();
    return 0;
}
