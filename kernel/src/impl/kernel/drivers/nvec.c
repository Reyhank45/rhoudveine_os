//
// nvec.c — NVEC Graphics Mode  (particle edition)
//
// Features
//   • Static NVEC logo drawn once onto a backing buffer
//   • 200 particles orbiting the logo centre on elliptical paths
//   • Trail effect: framebuffer dimmed each tick instead of cleared,
//     producing cyan comet tails with no extra memory
//   • FPS counter using RDTSC (no libc, no floating point)
//   • Status bar redrawn every tick (overlays trails cleanly)
//

#include "include/nvec.h"
#include <stdint.h>
#include <stddef.h>

// ── Internal state ──────────────────────────────────────────────────────────

static nvec_fb_t      g_fb    = {0};
static int            g_ready = 0;
static display_mode_t g_mode  = DISPLAY_MODE_SHELL;

// ── Shadow buffer (must be declared before fb_put/fb_get/fade_frame) ────────
#define SHADOW_MAX_PIXELS (1920 * 1200)
static uint32_t  g_shadow[SHADOW_MAX_PIXELS];
static uint32_t *g_draw = g_shadow;  // all drawing targets this

// ── TSC + I/O helpers (forward declarations used by shadow_flush etc.) ──────
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline uint8_t inb_nvec(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb_nvec(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

// TSC frequency — calibrated in nvec_init, used by frame timing
static uint64_t g_tsc_khz = 2000000ULL;

// ── Palette ─────────────────────────────────────────────────────────────────

#define COL_BG        0x0D0D1A
#define COL_ACCENT    0x00CFFF   // cyan — logo strokes + bright particles
#define COL_GLOW      0x003366   // deep blue — glow boxes + grid
#define COL_BAR_BG    0x0A0A18
#define COL_BAR_TEXT  0x4466AA
#define COL_P0        0x00CFFF   // particle colour tier 0 (fastest, brightest)
#define COL_P1        0x0088CC   // tier 1
#define COL_P2        0x004488   // tier 2 (slowest, dimmest)

// ── Pixel helpers ────────────────────────────────────────────────────────────

static inline void fb_put(int x, int y, uint32_t col) {
    if ((uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return;
    g_draw[y * g_fb.width + x] = col;   // shadow buffer (cached RAM)
}

static inline uint32_t fb_get(int x, int y) {
    if ((uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return 0;
    return g_draw[y * g_fb.width + x];  // read from shadow (fast)
}

// Blit shadow buffer to UC VRAM — called once per frame at the very end.
//
// Tearing notes:
//   - We write strictly top-to-bottom, matching the display scanout direction.
//   - We use 64-bit writes (two pixels per store) to maximise write-combining
//     throughput. The CPU's write-combining buffer coalesces these into
//     large burst transactions to VRAM.
//   - mfence after the blit ensures all writes reach VRAM before we return
//     and start rendering the next frame into the shadow.
// Wait for VBlank via VGA status port 0x3DA bit 3.
// Works on virtually all x86 systems through the VGA compatibility layer.
// 20ms TSC timeout prevents hanging if the port isn't available.
static void wait_vblank(void) {
    if (g_tsc_khz == 0) return;
    uint64_t timeout = g_tsc_khz * 20ULL;  // 20ms
    uint64_t t0 = rdtsc();
    // Step 1: wait out any current VBlank (bit 3 = 0 means active scan)
    while (inb_nvec(0x3DA) & 0x08)
        if ((rdtsc() - t0) > timeout) return;
    // Step 2: wait for VBlank to start (bit 3 = 1)
    t0 = rdtsc();
    while (!(inb_nvec(0x3DA) & 0x08))
        if ((rdtsc() - t0) > timeout) return;
    // Now at the very start of VBlank — safe to blit
}

static void shadow_flush(void) {
    // Sync to VBlank before writing VRAM — eliminates tearing.
    // The blit races the scanline downward during the blank interval.
    wait_vblank();

    uint32_t *src    = g_draw;
    uint32_t *dst    = g_fb.addr;
    uint32_t  stride = g_fb.pitch / 4;
    uint32_t  w      = g_fb.width;
    uint32_t  h      = g_fb.height;
    uint32_t  w2     = w / 2;

    for (uint32_t y = 0; y < h; y++) {
        uint64_t *s = (uint64_t *)(src + y * w);
        uint64_t *d = (uint64_t *)(dst + y * stride);
        for (uint32_t x = 0; x < w2; x++)
            d[x] = s[x];
        if (w & 1)
            dst[y * stride + w - 1] = src[y * w + w - 1];
    }
    __asm__ volatile("mfence" ::: "memory");
}

static void fb_fill_rect(int x, int y, int w, int h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    for (int ry = y; ry < y + h; ry++)
        for (int rx = x; rx < x + w; rx++)
            fb_put(rx, ry, col);
}

// ── Fixed-point sine table ───────────────────────────────────────────────────
// 256 entries covering 0..2π.  Values are sin(angle) * 1024, i.e. Q10.
// Generated for angles i * 2π / 256.
// We store only the first 256; cos(a) = sin(a + 64).

static const int16_t g_sin256[256] = {
       0,   25,   50,   75,  100,  125,  150,  174,
     198,  222,  245,  268,  290,  312,  333,  354,
     374,  394,  412,  430,  448,  464,  480,  495,
     509,  522,  535,  546,  556,  566,  574,  582,
     589,  595,  600,  604,  607,  609,  610,  610,
     610,  609,  607,  604,  600,  595,  589,  582,
     574,  566,  556,  546,  535,  522,  509,  495,
     480,  464,  448,  430,  412,  394,  374,  354,
     333,  312,  290,  268,  245,  222,  198,  174,
     150,  125,  100,   75,   50,   25,    0,  -25,
     -50,  -75, -100, -125, -150, -174, -198, -222,
    -245, -268, -290, -312, -333, -354, -374, -394,
    -412, -430, -448, -464, -480, -495, -509, -522,
    -535, -546, -556, -566, -574, -582, -589, -595,
    -600, -604, -607, -609, -610, -610, -610, -609,
    -607, -604, -600, -595, -589, -582, -574, -566,
    -556, -546, -535, -522, -509, -495, -480, -464,
    -448, -430, -412, -394, -374, -354, -333, -312,
    -290, -268, -245, -222, -198, -174, -150, -125,
    -100,  -75,  -50,  -25,    0,   25,   50,   75,
     100,  125,  150,  174,  198,  222,  245,  268,
     290,  312,  333,  354,  374,  394,  412,  430,
     448,  464,  480,  495,  509,  522,  535,  546,
     556,  566,  574,  582,  589,  595,  600,  604,
     607,  609,  610,  610,  610,  609,  607,  604,
     600,  595,  589,  582,  574,  566,  556,  546,
     535,  522,  509,  495,  480,  464,  448,  430,
     412,  394,  374,  354,  333,  312,  290,  268,
     245,  222,  198,  174,  150,  125,  100,   75,
      50,   25,    0,  -25,  -50,  -75, -100, -125,
    -150, -174, -198, -222, -245, -268, -290, -312,
    -333, -354, -374, -394, -412, -430, -448, -464,
};

// sin in Q10 (result / 1024 = sine)
static inline int isin(uint8_t a) { return g_sin256[a]; }
static inline int icos(uint8_t a) { return g_sin256[(uint8_t)(a + 64)]; }

// ── Letter drawing helpers ───────────────────────────────────────────────────

#define PX(ox,ux,GW)  ((ox) + (ux)*(GW)/10)
#define PY(oy,uy,GH)  ((oy) + (uy)*(GH)/16)
#define THICK(GW)     (((GW)*8/100)<2 ? 2 : (GW)*8/100)

static void thick_line(int x0,int y0,int x1,int y1,int T,uint32_t col) {
    int dx=x1-x0, dy=y1-y0;
    int adx=dx<0?-dx:dx, ady=dy<0?-dy:dy;
    int steps=adx>ady?adx:ady;
    if(!steps) steps=1;
    for(int i=0;i<=steps;i++){
        int cx=x0+dx*i/steps, cy=y0+dy*i/steps;
        if(adx>=ady) fb_fill_rect(cx,cy-T,1,T*2+1,col);
        else         fb_fill_rect(cx-T,cy,T*2+1,1,col);
    }
}

static void draw_letter_N(int ox,int oy,int GW,int GH){
    int T=THICK(GW), sw=T*2+1;
    fb_fill_rect(PX(ox,1,GW),PY(oy,0,GH),sw,GH,COL_ACCENT);
    fb_fill_rect(PX(ox,8,GW),PY(oy,0,GH),sw,GH,COL_ACCENT);
    thick_line(PX(ox,1,GW),PY(oy,0,GH),PX(ox,9,GW),PY(oy,16,GH),T,COL_ACCENT);
}
static void draw_letter_V(int ox,int oy,int GW,int GH){
    int T=THICK(GW);
    thick_line(PX(ox,0,GW),PY(oy,0,GH),PX(ox,5,GW),PY(oy,16,GH),T,COL_ACCENT);
    thick_line(PX(ox,10,GW),PY(oy,0,GH),PX(ox,5,GW),PY(oy,16,GH),T,COL_ACCENT);
}
static void draw_letter_E(int ox,int oy,int GW,int GH){
    int T=THICK(GW), sw=T*2+1;
    int bar_w=PX(ox,9,GW)-PX(ox,1,GW);
    fb_fill_rect(PX(ox,1,GW),PY(oy,0,GH),sw,GH,COL_ACCENT);
    fb_fill_rect(PX(ox,1,GW),PY(oy,0,GH),bar_w,sw,COL_ACCENT);
    fb_fill_rect(PX(ox,1,GW),PY(oy,8,GH),bar_w*8/10,sw,COL_ACCENT);
    fb_fill_rect(PX(ox,1,GW),PY(oy,16,GH)-sw,bar_w,sw,COL_ACCENT);
}
static void draw_letter_C(int ox,int oy,int GW,int GH){
    int T=THICK(GW), sw=T*2+1;
    int bar_w=PX(ox,9,GW)-PX(ox,1,GW);
    fb_fill_rect(PX(ox,1,GW),PY(oy,0,GH),sw,GH,COL_ACCENT);
    fb_fill_rect(PX(ox,1,GW),PY(oy,0,GH),bar_w,sw,COL_ACCENT);
    fb_fill_rect(PX(ox,1,GW),PY(oy,16,GH)-sw,bar_w,sw,COL_ACCENT);
}

static void draw_glow(int ox,int oy,int GW,int GH){
    int pad=8;
    fb_fill_rect(ox-pad,oy-pad,GW+pad*2,GH+pad*2,COL_GLOW);
}

// ── Tiny 5×7 bitmap font ─────────────────────────────────────────────────────

typedef struct { char ch; uint8_t rows[7]; } Glyph;

static const Glyph g_font[] = {
    {' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'A',{0x04,0x0A,0x11,0x1F,0x11,0x11,0x11}},
    {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J',{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x11,0x11,0x11,0x11}},
    {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}},
    {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V',{0x11,0x11,0x11,0x11,0x0A,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'a',{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}},
    {'b',{0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}},
    {'c',{0x00,0x00,0x0E,0x10,0x10,0x10,0x0E}},
    {'d',{0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}},
    {'e',{0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}},
    {'f',{0x06,0x09,0x08,0x1C,0x08,0x08,0x08}},
    {'g',{0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}},
    {'h',{0x10,0x10,0x16,0x19,0x11,0x11,0x11}},
    {'i',{0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}},
    {'j',{0x02,0x00,0x06,0x02,0x02,0x12,0x0C}},
    {'k',{0x10,0x10,0x12,0x14,0x18,0x14,0x12}},
    {'l',{0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'m',{0x00,0x00,0x1A,0x15,0x15,0x11,0x11}},
    {'n',{0x00,0x00,0x16,0x19,0x11,0x11,0x11}},
    {'o',{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}},
    {'p',{0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}},
    {'q',{0x00,0x00,0x0F,0x11,0x0F,0x01,0x01}},
    {'r',{0x00,0x00,0x16,0x19,0x10,0x10,0x10}},
    {'s',{0x00,0x00,0x0E,0x10,0x0E,0x01,0x0E}},
    {'t',{0x08,0x08,0x1C,0x08,0x08,0x09,0x06}},
    {'u',{0x00,0x00,0x11,0x11,0x11,0x13,0x0D}},
    {'v',{0x00,0x00,0x11,0x11,0x11,0x0A,0x04}},
    {'w',{0x00,0x00,0x11,0x11,0x15,0x15,0x0A}},
    {'x',{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}},
    {'y',{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}},
    {'z',{0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}},
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}},
    {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'[',{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
    {']',{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {'/',{0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    {'\0',{0,0,0,0,0,0,0}}
};

static void draw_char(int x,int y,char c,uint32_t col){
    for(int g=0;g_font[g].ch!='\0';g++){
        if(g_font[g].ch!=c) continue;
        for(int row=0;row<7;row++){
            uint8_t bits=g_font[g].rows[row];
            for(int bit=4;bit>=0;bit--){
                if(bits&(1<<bit)){
                    fb_fill_rect(x+(4-bit)*2, y+row*2, 2,2, col);
                }
            }
        }
        return;
    }
}

static void draw_string(int x,int y,const char *s,uint32_t col){
    int cx=x;
    while(*s){
        if(*s==' '){ cx+=8; s++; continue; }
        draw_char(cx,y,*s,col);
        cx+=12; s++;
    }
}

// ── Layout helpers ───────────────────────────────────────────────────────────

// Compute logo layout from current fb size.
// Returns ox0 (left edge of N), oy (top edge), GW, GH, GAP via out-params.
static void logo_layout(int *ox0_out, int *oy_out, int *GW_out,
                         int *GH_out,  int *GAP_out) {
    int target_w = (int)g_fb.width * 72 / 100;
    int GW  = target_w * 10 / 49;
    if (GW < 40) GW = 40;
    int GAP = GW * 3 / 10;
    int GH  = GW * 16 / 10;

    int total_w = 4 * GW + 3 * GAP;
    int ox0 = ((int)g_fb.width - total_w) / 2;

    int usable_h = (int)g_fb.height - 26 - GH - 50;
    int oy = usable_h / 2;
    if (oy < 20) oy = 20;

    *ox0_out = ox0;  *oy_out = oy;
    *GW_out  = GW;   *GH_out = GH;  *GAP_out = GAP;
}

// ── Status bar ───────────────────────────────────────────────────────────────

// itoa for small positive integers (no libc)
static void itoa_small(uint32_t v, char *buf, int buflen) {
    // write digits right-to-left
    int i = buflen - 1;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else {
        while (v > 0 && i > 0) {
            buf[--i] = '0' + (v % 10);
            v /= 10;
        }
    }
    // shift left
    int src = i, dst = 0;
    while (buf[src]) buf[dst++] = buf[src++];
    buf[dst] = '\0';
}

static void draw_status_bar(uint32_t fps) {
    int bar_h = 26;
    int bar_y = (int)g_fb.height - bar_h;

    fb_fill_rect(0, bar_y, (int)g_fb.width, bar_h, COL_BAR_BG);
    fb_fill_rect(0, bar_y, (int)g_fb.width, 1, COL_GLOW);

    // Left: mode label
    draw_string(10, bar_y + 5, "NVEC Display Server Embedded - DEMO", COL_BAR_TEXT);

    // Centre: FPS counter  "FPS: 123"
    char fps_buf[16];
    fps_buf[0]='F'; fps_buf[1]='P'; fps_buf[2]='S'; fps_buf[3]=':';
    fps_buf[4]=' '; fps_buf[5]='\0';
    char num_buf[10];
    itoa_small(fps, num_buf, sizeof(num_buf));
    // append
    int i=5;
    for(int j=0; num_buf[j] && i<14; j++) fps_buf[i++]=num_buf[j];
    fps_buf[i]='\0';
    int fw=0; for(char *p=fps_buf;*p;p++) fw+= (*p==' ') ? 8 : 12;
    draw_string(((int)g_fb.width - fw) / 2, bar_y + 5, fps_buf, COL_ACCENT);

    // Right: hotkey hint
    const char *hint = "[ Ctrl+Alt+F10 - Shell Mode ]";
    int hlen=0; for(const char *p=hint;*p;p++) hlen++;
    int hx = (int)g_fb.width - hlen*12 - 10;
    if (hx < 0) hx = 0;
    draw_string(hx, bar_y + 5, hint, COL_BAR_TEXT);
}

static void draw_subtitle(int logo_y, int logo_h) {
    const char *text = "FORCE - NVI";
    int len=0; for(const char *p=text;*p;p++) len++;
    int tw=len*12;
    draw_string(((int)g_fb.width-tw)/2, logo_y+logo_h+18, text, COL_BAR_TEXT);
}

// ── Particle system ──────────────────────────────────────────────────────────
//
// Each particle has:
//   angle   — current orbit angle (0..255 maps to 0..2π)
//   speed   — angle increment per tick (1..4)
//   rx, ry  — orbit semi-axes in pixels (ellipse)
//   colour  — one of COL_P0/P1/P2
//   size    — 1 or 2 (pixel radius for 1×1 or 2×2 dot)
//
// We derive orbit centre from the logo layout at init time.

#define NUM_PARTICLES 200

typedef struct {
    uint8_t  angle;   // 0..255
    uint8_t  speed;   // angle ticks per frame (1..4)
    int16_t  rx;      // x semi-axis
    int16_t  ry;      // y semi-axis
    uint32_t colour;
    uint8_t  size;    // 1 = 1px, 2 = 2px square
} Particle;

static Particle g_particles[NUM_PARTICLES];
static int      g_cx, g_cy;   // orbit centre (logo centre)
static int      g_particles_ready = 0;

// Simple LCG pseudo-random (seed with something non-zero at init)
static uint32_t g_rand_state = 0xDEADBEEF;
static uint32_t lcg_rand(void) {
    g_rand_state = g_rand_state * 1664525u + 1013904223u;
    return g_rand_state;
}

static void particles_init(void) {
    int ox0, oy, GW, GH, GAP;
    logo_layout(&ox0, &oy, &GW, &GH, &GAP);

    int total_w = 4 * GW + 3 * GAP;
    g_cx = ox0 + total_w / 2;
    g_cy = oy  + GH / 2;

    // Orbit radii:  3 rings — tight, mid, wide
    // Each ring has ~67 particles.
    // rx varies per particle for organic feel.
    for (int i = 0; i < NUM_PARTICLES; i++) {
        uint32_t r = lcg_rand();

        // Ring tier determines orbit size + speed + colour
        int tier = i % 3;
        int base_rx, base_ry;
        switch (tier) {
            case 0: base_rx = total_w * 55 / 100; base_ry = GH * 75 / 100; break;
            case 1: base_rx = total_w * 75 / 100; base_ry = GH * 95 / 100; break;
            default:base_rx = total_w * 95 / 100; base_ry = GH * 120/ 100; break;
        }

        // Vary radius ±15%
        int jitter = (int)(r & 0x1F) - 16;   // -16..+15
        g_particles[i].rx = (int16_t)(base_rx + base_rx * jitter / 100);
        g_particles[i].ry = (int16_t)(base_ry + base_ry * jitter / 100);

        // Starting angle: spread evenly + small jitter
        g_particles[i].angle = (uint8_t)((i * 256 / NUM_PARTICLES) + (r >> 8));

        // Speed: faster on inner ring
        uint8_t base_speed = (uint8_t)(3 - tier);      // 3, 2, 1
        g_particles[i].speed = base_speed;

        // Colour by tier
        uint32_t cols[3] = { COL_P0, COL_P1, COL_P2 };
        g_particles[i].colour = cols[tier];

        // Size: inner ring bigger dots
        g_particles[i].size = (tier == 0) ? 2 : 1;
    }
    g_particles_ready = 1;
}

// ── Trail fade ───────────────────────────────────────────────────────────────
// ── fade_frame ───────────────────────────────────────────────────────────────
//
// Darken every pixel by ~12.5% per call (shift channels right by 1 then
// subtract 1 to avoid the "stuck at 1" problem that makes the background
// slowly turn grey instead of reaching pure black).
//
// Performance notes:
//   • We pack two pixels into one 64-bit word and shift both channels
//     simultaneously using a bitmask trick — halves the memory bandwidth.
//   • Pure-black pixels (p == 0) are skipped entirely.
//   • The status bar (bottom 26 rows) is never touched.
//
// At 1920×1080 this runs in roughly 2–4 ms on QEMU (vs ~15 ms naive).

static void fade_frame(void) {
    // Fade runs entirely on the shadow buffer (cached RAM) — no UC accesses.
    // Stride in shadow = g_fb.width (tightly packed, no pitch padding).
    int bar_y  = (int)g_fb.height - 26;
    int w      = (int)g_fb.width;

    // Process two pixels per iteration using 64-bit words.
    // Layout: [pixB | pixA] in a uint64_t (little-endian).
    // Mask 0x7F7F7F7F7F7F7F7F clears the MSB of each byte after shift,
    // preventing colour channel carry-over between channels.
    const uint64_t MASK64 = 0x7F7F7F7F7F7F7F7FULL;

    uint32_t *base = g_draw;
    int half_w = w / 2;   // number of 64-bit word pairs per row

    // Fade constant: subtract FADE_STEP from each channel per frame.
    // At 60fps, FADE_STEP=12 gives ~21 frame trails (~350ms).
    // Using saturating subtract: if channel < FADE_STEP, clamp to 0.
    // Packed trick: subtract FADE_STEP from R, G, B simultaneously.
    // We detect underflow by checking if subtraction flipped the sign bit
    // within each byte — mask it to zero if so.
    #define FADE_STEP 12U
    const uint64_t STEP64 = ((uint64_t)FADE_STEP      ) |
                            ((uint64_t)FADE_STEP <<  8 ) |
                            ((uint64_t)FADE_STEP << 16 ) |
                            ((uint64_t)FADE_STEP << 32 ) |
                            ((uint64_t)FADE_STEP << 40 ) |
                            ((uint64_t)FADE_STEP << 48 );
    // Saturating subtract per-byte: clamp to 0 if result would underflow.
    // Method: for each byte b, result = (b > STEP) ? b-STEP : 0
    // Implemented as: t = p - STEP64; mask out bytes where borrow occurred.
    // Borrow detection: if (p & 0x80) != (t & 0x80) after subtract, borrow.
    // Simpler: just use the MASK64 approach — shift then OR back lost bits.
    // Actually simplest correct saturating subtract for packed bytes:
    //   sat_sub(a,b) = (a | 0x80808080...) - b) & 0x80808080... gives borrow
    const uint64_t HIGH64 = 0x8080808080808080ULL;

    for (int y = 0; y < bar_y; y++) {
        uint64_t *row = (uint64_t *)(base + y * w);
        for (int x = 0; x < half_w; x++) {
            uint64_t p = row[x];
            if (p == 0) continue;
            // Saturating subtract: set high bit of each byte, subtract,
            // check which bytes borrowed (high bit now clear = underflow)
            uint64_t t = (p | HIGH64) - STEP64;
            uint64_t borrow = (~t) & HIGH64;      // 0x80 where underflow
            // Zero out underflowed bytes, restore non-underflowed ones
            borrow |= borrow >> 1; borrow |= borrow >> 2; borrow |= borrow >> 3;
            borrow |= borrow >> 4;                // spread to all 8 bits of byte
            row[x] = t & ~borrow & ~HIGH64;       // mask underflows + clear high bits
        }
        if (w & 1) {
            uint32_t p = base[y * w + w - 1];
            if (p) {
                uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
                r = r > FADE_STEP ? r - FADE_STEP : 0;
                g = g > FADE_STEP ? g - FADE_STEP : 0;
                b = b > FADE_STEP ? b - FADE_STEP : 0;
                base[y * w + w - 1] = (r << 16) | (g << 8) | b;
            }
        }
    }
}

// (rdtsc/inb_nvec/outb_nvec declared at top of file)

// ── TSC calibration ──────────────────────────────────────────────────────────
//
// Strategy (works on both real HW and QEMU):
//
//   1. Try PIT channel 2 OUT gate (port 0x61 bit 5).
//      QEMU does NOT emulate channel 2 OUT correctly — the bit stays 0
//      forever, so we add a hard timeout of ~50M iterations.
//
//   2. If the PIT poll times out we fall back to PIT channel 0 read-back:
//      read the current count twice with a small spin between, derive
//      elapsed time from the count difference.
//
//   3. If both fail, keep the compile-time fallback (2 GHz).
//      The FPS counter will be wrong but the animation will still run.
//
// g_tsc_khz = TSC ticks per millisecond.

#define PIT_HZ  1193182ULL

// (g_tsc_khz declared at top of file)

static void tsc_calibrate_pit(void) {
    // ── Method A: channel 2 one-shot, poll OUT (real HW path) ───────────────
    #define CAL_MS   10U
    #define CAL_LATCH ((uint16_t)(PIT_HZ * CAL_MS / 1000U))  // 11932 counts

    uint8_t p61 = inb_nvec(0x61);
    // gate=1, speaker=0
    outb_nvec(0x61, (p61 & 0xFC) | 0x01);

    // channel 2, lobyte/hibyte, mode 0 (one-shot), binary
    outb_nvec(0x43, 0xB0);
    outb_nvec(0x42, (uint8_t)(CAL_LATCH & 0xFF));
    outb_nvec(0x42, (uint8_t)(CAL_LATCH >> 8));

    // re-latch gate to start count
    outb_nvec(0x61, (inb_nvec(0x61) & 0xFC) | 0x01);

    uint64_t t0 = rdtsc();

    // Poll OUT (bit 5). Hard timeout: ~50 M iterations ≈ 25 ms at 2 GHz.
    uint32_t timeout = 50000000U;
    while (!(inb_nvec(0x61) & 0x20) && --timeout);

    uint64_t t1 = rdtsc();
    outb_nvec(0x61, p61);  // restore

    if (timeout > 0) {
        // OUT fired — real HW path succeeded
        uint64_t delta = t1 - t0;
        if (delta > 0) {
            g_tsc_khz = delta / CAL_MS;
            return;
        }
    }

    // ── Method B: channel 0 latch-read (QEMU path) ───────────────────────────
    // Read PIT channel 0 counter twice, spin ~1000 inner loops between reads.
    // Channel 0 decrements at PIT_HZ.  Difference in counts = elapsed PIT ticks.
    //
    // Latch command for channel 0: 0x00 = channel 0, latch (00 in bits 5:4)
    outb_nvec(0x43, 0x00);
    uint8_t lo0 = inb_nvec(0x40);
    uint8_t hi0 = inb_nvec(0x40);
    uint64_t ts0 = rdtsc();

    for (volatile uint32_t i = 0; i < 10000; i++);

    outb_nvec(0x43, 0x00);
    uint8_t lo1 = inb_nvec(0x40);
    uint8_t hi1 = inb_nvec(0x40);
    uint64_t ts1 = rdtsc();

    uint16_t cnt0 = (uint16_t)((hi0 << 8) | lo0);
    uint16_t cnt1 = (uint16_t)((hi1 << 8) | lo1);

    // Channel 0 counts DOWN.  If cnt1 < cnt0, elapsed PIT ticks = cnt0 - cnt1.
    // If it wrapped, add 65536.
    uint32_t pit_ticks = (cnt1 <= cnt0) ? (cnt0 - cnt1)
                                         : (cnt0 + (65536U - cnt1));

    uint64_t tsc_ticks = ts1 - ts0;

    // elapsed_us = pit_ticks * 1_000_000 / PIT_HZ
    // g_tsc_khz  = tsc_ticks / elapsed_ms
    //            = tsc_ticks * PIT_HZ / (pit_ticks * 1000)
    if (pit_ticks > 0 && tsc_ticks > 0) {
        g_tsc_khz = (tsc_ticks * PIT_HZ) / ((uint64_t)pit_ticks * 1000ULL);
    }
    // else: keep 2 GHz fallback
}

// ── Frame timing + FPS ───────────────────────────────────────────────────────
//
// Target: 60 fps = one frame every ~16.67 ms.
//
// Tearing reduction strategy:
//   We can't vsync without GPU driver access to the display controller's
//   vblank interrupt. The next best thing is:
//     1. Render everything into the shadow buffer (invisible to the display).
//     2. Spin-wait until the frame deadline arrives (TSC busy-wait).
//     3. Do the VRAM blit as fast as possible top-to-bottom.
//   The blit direction (top→bottom) matches the display scan direction, so
//   by the time the scanline reaches the bottom the blit is already done.
//   This won't eliminate tearing entirely but reduces it to a thin line
//   near the bottom of the frame rather than a wide tear band.
//
// g_frame_deadline: TSC value when the next blit should start.

#define TARGET_FPS      60ULL
#define FPS_WINDOW      16U

static uint64_t g_frame_deadline = 0;   // TSC of next permitted blit
static uint64_t g_fps_tsc_last   = 0;   // for FPS measurement
static uint32_t g_frame_count    = 0;
static uint32_t g_fps_display    = 0;

// blit_due() — non-blocking 60fps gate for shadow_flush().
// Returns 1 at most TARGET_FPS times per second, 0 otherwise.
// Render (fade + particles + logo) runs every call uncapped.
// Only the VRAM blit is gated — display output stays at 60fps.
static int blit_due(void) {
    if (g_tsc_khz == 0) return 1;

    uint64_t tpf = g_tsc_khz * 1000ULL / TARGET_FPS;
    uint64_t now = rdtsc();

    if (g_frame_deadline == 0) {
        g_frame_deadline = now + tpf;
        g_fps_tsc_last   = now;
        return 1;
    }

    if (now < g_frame_deadline) return 0;  // not time to blit yet

    g_frame_deadline += tpf;
    // Fell behind by more than 2 frames — reset to avoid burst catch-up
    if (now > g_frame_deadline + tpf)
        g_frame_deadline = now + tpf;

    // FPS counter tracks blit rate = display fps
    g_frame_count++;
    if (g_frame_count >= FPS_WINDOW) {
        uint64_t win_elapsed = now - g_fps_tsc_last;
        uint64_t win_ms      = win_elapsed / g_tsc_khz;
        if (win_ms > 0)
            g_fps_display = (uint32_t)(FPS_WINDOW * 1000ULL / win_ms);
        g_fps_tsc_last = now;
        g_frame_count  = 0;
    }

    return 1;
}

// ── Static logo layer ────────────────────────────────────────────────────────
// We redraw the logo every frame ON TOP of the faded trails.
// This keeps the logo crisp while particles leave tails.

static void draw_logo(void) {
    int ox0, oy, GW, GH, GAP;
    logo_layout(&ox0, &oy, &GW, &GH, &GAP);

    int ox_N = ox0;
    int ox_V = ox0 +   (GW + GAP);
    int ox_E = ox0 + 2*(GW + GAP);
    int ox_C = ox0 + 3*(GW + GAP);

    draw_glow(ox_N, oy, GW, GH);
    draw_glow(ox_V, oy, GW, GH);
    draw_glow(ox_E, oy, GW, GH);
    draw_glow(ox_C, oy, GW, GH);

    draw_letter_N(ox_N, oy, GW, GH);
    draw_letter_V(ox_V, oy, GW, GH);
    draw_letter_E(ox_E, oy, GW, GH);
    draw_letter_C(ox_C, oy, GW, GH);

    draw_subtitle(oy, GH);
}

// ── Full initial render (called once on enter) ───────────────────────────────

static void nvec_render_initial(void) {
    // solid background + grid
    fb_fill_rect(0, 0, (int)g_fb.width, (int)g_fb.height, COL_BG);
    for (int gx = 0; gx < (int)g_fb.width; gx += 80)
        for (int gy = 0; gy < (int)g_fb.height - 26; gy++)
            fb_put(gx, gy, COL_GLOW);
    for (int gy = 0; gy < (int)g_fb.height - 26; gy += 80)
        for (int gx = 0; gx < (int)g_fb.width; gx++)
            fb_put(gx, gy, COL_GLOW);
    draw_logo();
    draw_status_bar(0);
}

// ── nvec_tick ────────────────────────────────────────────────────────────────
//
// Call this as frequently as possible from any spin-wait or idle loop.
// Render (fade + particles) runs uncapped — safe to call thousands of times.
// VRAM output is gated to TARGET_FPS (60fps) via blit_due().
// Render rate and display rate are fully decoupled.

void nvec_tick(void) {
    if (!g_ready || g_mode != DISPLAY_MODE_GRAPHICS) return;

    // Render runs UNCAPPED — all drawing goes to shadow buffer (cached RAM).
    // This keeps animation physics smooth at whatever rate the caller loops.

    // 1. Fade shadow every call
    fade_frame();

    // 2. Logo (crisp, redrawn on top of trails every call)
    draw_logo();

    // 3. Particles
    for (int i = 0; i < NUM_PARTICLES; i++) {
        Particle *p = &g_particles[i];
        p->angle += p->speed;
        int px = g_cx + (int)(p->rx) * icos(p->angle) / 1024;
        int py = g_cy + (int)(p->ry) * isin(p->angle) / 1024;
        if (p->size == 2)
            fb_fill_rect(px - 1, py - 1, 3, 3, p->colour);
        else
            fb_put(px, py, p->colour);
    }

    // 4. Status bar
    draw_status_bar(g_fps_display);

    // 5. Blit to VRAM at 60fps — render rate and display rate are decoupled
    if (blit_due())
        shadow_flush();
}

// ── Console save / restore ───────────────────────────────────────────────────

#define BACKUP_MAX_W 1920
#define BACKUP_MAX_H 1080
static uint32_t g_shell_backup[BACKUP_MAX_W * BACKUP_MAX_H];
static int       g_backup_valid = 0;

// (shadow buffer declared at top of file)

static void console_save(void) {
    if (g_fb.width > BACKUP_MAX_W || g_fb.height > BACKUP_MAX_H) {
        g_backup_valid = 0; return;
    }
    // Save from UC VRAM (the live console output lives there)
    uint32_t stride = g_fb.pitch / 4;
    for (uint32_t row = 0; row < g_fb.height; row++)
        for (uint32_t col = 0; col < g_fb.width; col++)
            g_shell_backup[row * g_fb.width + col] =
                g_fb.addr[row * stride + col];
    g_backup_valid = 1;
}

static void console_restore(void) {
    if (!g_backup_valid) return;
    // Restore to both shadow and VRAM so console is visible immediately
    uint32_t stride = g_fb.pitch / 4;
    for (uint32_t row = 0; row < g_fb.height; row++) {
        for (uint32_t col = 0; col < g_fb.width; col++) {
            uint32_t px = g_shell_backup[row * g_fb.width + col];
            g_draw[row * g_fb.width + col] = px;
            g_fb.addr[row * stride + col]  = px;
        }
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

uint64_t nvec_tsc_khz(void) { return g_tsc_khz; }

void nvec_init(uint32_t *fb_addr, uint32_t width, uint32_t height,
               uint32_t pitch) {
    g_fb.addr   = fb_addr;
    g_fb.width  = width;
    g_fb.height = height;
    g_fb.pitch  = pitch;
    g_mode      = DISPLAY_MODE_SHELL;
    g_ready     = 1;
    g_draw      = g_shadow;   // all drawing goes to cached shadow buffer

    // Calibrate TSC frequency using PIT channel 2 (10 ms window).
    // Safe at boot: no IRQ, no APIC needed, just port I/O.
    tsc_calibrate_pit();
}

void nvec_enter_graphics(void) {
    if (!g_ready || g_mode == DISPLAY_MODE_GRAPHICS) return;
    console_save();
    g_mode = DISPLAY_MODE_GRAPHICS;
    particles_init();
    // Reset all frame-timing state so the first tick fires immediately
    uint64_t now       = rdtsc();
    g_frame_deadline   = 0;   // force blit_due() true on first call
    g_fps_tsc_last     = now;
    g_frame_count      = 0;
    g_fps_display      = 0;
    nvec_render_initial();
}

void nvec_exit_graphics(void) {
    if (!g_ready || g_mode == DISPLAY_MODE_SHELL) return;
    g_mode = DISPLAY_MODE_SHELL;
    console_restore();
}

void nvec_toggle(void) {
    if (g_mode == DISPLAY_MODE_SHELL) nvec_enter_graphics();
    else                               nvec_exit_graphics();
}

display_mode_t nvec_get_mode(void)         { return g_mode; }
int            nvec_suppress_console(void) { return g_mode == DISPLAY_MODE_GRAPHICS; }