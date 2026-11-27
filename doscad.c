// SPDX-FileCopyrightText: 2026 Joaquim Serra
// SPDX-FileCopyrightText: 2026 Michael Solis
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Developed by Claude Opus 5.5 in high effort mode.
//
// DOSCAD.DRV drawing code: the PCB386 display driver entries on a VESA 256-colour
// banked mode, compiled with gcc -m16 and called from doscad.asm.
//
// Flow:
//  1. dispatch() hides the software arrow, runs the entry g_entry with the
//     stack arguments in args[], and shows the arrow again.
//  2. Entry 0 loads the 8x8 font from the user's VGA640.DRV (DOSCADFONT=path,
//     else next to the ORCADDD driver, else %ORCADESP%\DRV), then picks the VESA mode (DOSCADVRES=WxH, else the largest 8bpp
//     mode that fits) and the scale (DOSCADVSCALE=S, else 2 from 1080 lines
//     up and 1 below), sets the mode and the
//     16-colour DAC, and resets the screen state.
//  3. Drawing entries work in logical pixels (physical / S) exactly like the
//     original drivers; every logical pixel becomes an S x S block of physical pixels.
//  4. Pixels go to banked VRAM at A000 through GS (set by doscad.asm).
//  5. With DOSCADVFPS=1, dispatch() times each PCB386 redraw (a board clear to
//     the next cursor set at a real position) and writes "WxH | 256 | N fps"
//     at the bottom right.
//  6. While the driver is open, INT 16h goes through int16_hook (doscad.asm):
//     a program that only polls for keys is halted until the next interrupt,
//     so the emulator idles (DOSCADIDLE=0 turns it off). Entry 1 restores
//     the vector.
//  7. DRAFT (SDT386+) zooms out with SetScale (entry 10): above 1, host
//     coordinates are clipped first and then divided by the scale (to_screen),
//     as VGA640.DRV does with its division table; bitmaps keep their pixel
//     size and text is drawn with VGA640's stroke font (draw_stroke_text).
//  8. ESP asks the screen size (entry 6) before it opens the driver and lays
//     its screens out for it, so entry 6 picks the mode first (pick_mode).
//     ESP and its configuration programs (*_C.EXE) read DOSCADESPRES=WxH, when
//     set, instead of DOSCADVRES, and take the largest scale that fits 640x480
//     (not DOSCADVSCALE).
//  9. Programs started from ESP find INT 16h reset to the vector the hook
//     replaced; dispatch() puts the hook back (keep_keyboard_hook).
// 10. Text at a text scale other than 1, under SetScale above 1, or a single
//     control character uses the stroke font read from the user's VGA640.DRV
//     with the 8x8 font (load_strokes, contributed by Michael Solis); without
//     it the driver falls back to the 8x8 font reduced (draw_text_small).
// 11. The image and the stroke font block stay resident for the DOS session,
//     shared by every program (stub.asm): ESP may call the driver PCB386 or
//     DRAFT loaded after they end. Each load unpacks a fresh image but keeps
//     the INT 16h hook's state, so hook_keyboard() may find its own hook in
//     the vector.

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef short i16;
typedef int i32;
typedef long long i64;
typedef unsigned long long u64;

// ---- Interface with doscad.asm --------------------------------------------

/// Registers for int86(): int 10h, or int 21h when intno is 0x21.
struct regs
{
    u32 eax, ebx, ecx, edx, esi, edi;
    u16 es;
    u8 intno;
    u8 pad;
    u16 flags;
};

extern void int86(struct regs *r);
extern void int16_hook(void);
extern u16 old16[2];
extern char image_end[];
extern u16 args[32];
extern u16 ret_ax, ret_bx, ret_cx, ret_dx, g_entry;

/// The 8x8 font: 224 glyphs of 8 bytes (characters 0x20-0xff), rows bottom-up.
/// It is OrCAD's, so it is not shipped: entry 0 reads it from the user's VGA640.DRV.
#define FONT_SIZE 1792
static u8 FONT[FONT_SIZE];
static int font_loaded;

/// The stroke font of VGA640.DRV (its text routine at 0x1a3a): a 256-word table of glyph
/// pointers, a 32-word table of the alternate glyphs for flag 43, then the glyphs, each a
/// list of words ended by 0. It is 5348 bytes at 0x5280 of the user's VGA640.DRV, read
/// into a DOS block at start-up (the image has no room left in its 64 KB). The pointers
/// keep the original's values: block offset = pointer - 0x460. The block belongs to DOS,
/// like the image's (stub.asm), and its segment is kept in the image block's last
/// paragraph, so that every load of the resident image finds it again (stroke_block).
#define STROKE_FILE_OFFSET 0x5280
#define STROKE_SIZE 5348
#define STROKE_BASE 0x460
#define STROKE_ALT 0x200
static u16 stroke_segment;

// ---- C runtime (no libgcc) -------------------------------------------------

/// Copies n bytes (gcc may emit calls to it for structure copies).
void *memcpy(void *d, const void *s, u32 n)
{
    u8 *dd = d;
    const u8 *ss = s;
    while (n--) {
        *dd++ = *ss++;
    }
    return d;
}

/// Fills n bytes with c (gcc may emit calls to it).
void *memset(void *d, int c, u32 n)
{
    u8 *dd = d;
    while (n--) {
        *dd++ = (u8)c;
    }
    return d;
}

/// Divides two 64-bit numbers truncating toward zero, like Rust's `/`.
static i64 sdiv64(i64 n, i64 d)
{
    int negative = 0;
    u64 un = (u64)n;
    u64 ud = (u64)d;
    u64 q;
    // Both in 32 bits with a positive divisor (nearly every call): one idiv,
    // which truncates toward zero as well.
    i32 n32 = (i32)n;
    i32 d32 = (i32)d;
    if (n == n32 && d == d32 && d32 > 0) {
        return n32 / d32;
    }
    if (n < 0) {
        un = -(u64)n;
        negative ^= 1;
    }
    if (d < 0) {
        ud = -(u64)d;
        negative ^= 1;
    }
    if ((un >> 32) == 0 && (ud >> 32) == 0) {
        // Both fit in 32 bits: one divl.
        q = (u32)un / (u32)ud;
    } else if ((ud >> 32) == 0) {
        // Divisor fits in 32 bits: long division in two divl steps.
        u32 high = (u32)(un >> 32);
        u32 qhigh = high / (u32)ud;
        u32 rem = high % (u32)ud;
        u32 qlow;
        u32 rlow;
        __asm__("divl %4" : "=a"(qlow), "=d"(rlow) : "a"((u32)un), "d"(rem), "rm"((u32)ud));
        q = ((u64)qhigh << 32) | qlow;
    } else {
        // Wide divisor: shift and subtract.
        u64 r = 0;
        q = 0;
        for (int bit = 63; bit >= 0; bit--) {
            r = (r << 1) | ((un >> bit) & 1);
            if (r >= ud) {
                r -= ud;
                q |= (u64)1 << bit;
            }
        }
    }
    return negative ? -(i64)q : (i64)q;
}

/// Floor division for a positive divisor, like Rust's `div_euclid`.
static i64 div_floor(i64 n, i64 d)
{
    // Both in 32 bits (nearly every call): one idiv gives the truncated quotient and
    // the remainder, whose sign is n's; a negative remainder means one less.
    i32 n32 = (i32)n;
    i32 d32 = (i32)d;
    if (n == n32 && d == d32 && d32 > 0) {
        i32 q32 = n32 / d32;
        if (n32 % d32 < 0) {
            q32--;
        }
        return q32;
    }
    i64 q = sdiv64(n, d);
    if (n - q * d < 0) {
        q--;
    }
    return q;
}

static u8 peek8(u16 seg, u16 off)
{
    u8 v;
    __asm__ volatile("movw %w1, %%fs\n\tmovb %%fs:(%k2), %0" : "=q"(v) : "r"(seg), "r"((u32)off));
    return v;
}

static void poke8(u16 seg, u16 off, u8 v)
{
    __asm__ volatile("movw %w0, %%fs\n\tmovb %2, %%fs:(%k1)" : : "r"(seg), "r"((u32)off), "q"(v) : "memory");
}

static void poke16(u16 seg, u16 off, u16 v)
{
    __asm__ volatile("movw %w0, %%fs\n\tmovw %w2, %%fs:(%k1)" : : "r"(seg), "r"((u32)off), "r"(v) : "memory");
}

/// Writes one byte to an I/O port.
static void out8(u16 port, u8 v)
{
    __asm__ volatile("outb %b0, %w1" : : "a"(v), "Nd"(port));
}

static u16 code_segment(void)
{
    u16 s;
    __asm__("movw %%cs, %0" : "=r"(s));
    return s;
}

/// Reads one byte from an I/O port.
static u8 in8(u16 port)
{
    u8 v;
    __asm__ volatile("inb %w1, %b0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline u16 peek16(u16 seg, u16 off)
{
    u16 v;
    __asm__ volatile("movw %w1, %%fs\n\tmovw %%fs:(%k2), %0" : "=r"(v) : "r"(seg), "r"((u32)off));
    return v;
}

/// PIT clock: 1193182 counts per second.
#define PIT_HZ 1193182u

/// Puts PIT channel 0 in mode 2 (rate generator) with the BIOS divisor 65536, so its count
/// falls by one per PIT clock (mode 3, the BIOS default, falls by two, twice per period).
/// The rate of IRQ 0 stays 18.2 Hz.
static void set_timer_mode(u8 mode)
{
    out8(0x43, (u8)(0x30 | (mode << 1)));
    out8(0x40, 0);
    out8(0x40, 0);
}

/// Returns the time in PIT clocks, modulo 2^32 (about one hour): the BIOS tick count
/// (0040:006C) times 65536 plus the clocks elapsed in the current tick.
static u32 read_time(void)
{
    // Latch channel 0 and read its count, low byte first.
    out8(0x43, 0x00);
    u32 low = in8(0x40);
    u32 high = in8(0x40);
    u32 elapsed = (0x10000u - (low | (high << 8))) & 0xffffu;
    u32 ticks = peek16(0x40, 0x6c);
    // Interrupts are off in the driver: when IRQ 0 is pending (IRR bit 0) the count has
    // wrapped but the BIOS tick is not counted yet.
    out8(0x20, 0x0a);
    if ((in8(0x20) & 1) != 0 && elapsed < 0x8000u) {
        ticks++;
    }
    return (ticks << 16) | elapsed;
}

/// Signed 16-bit argument at [bp+x].
static i32 arg(int x)
{
    return (i16)args[(x - 6) / 2];
}

/// Signed 32-bit argument at [bp+x].
static i32 arg32(int x)
{
    int i = (x - 6) / 2;
    return (i32)((u32)args[i] | ((u32)args[i + 1] << 16));
}

static i32 imin(i32 a, i32 b)
{
    return a < b ? a : b;
}

static i32 imax(i32 a, i32 b)
{
    return a > b ? a : b;
}

static i32 iabs(i32 a)
{
    return a < 0 ? -a : a;
}

// ---- Constants -------------------------------------------------------------

enum { WRITE_REPLACE = 0, WRITE_OR = 1, WRITE_XOR = 2 };
#define NO_CURSOR 0x7fff
#define CROSSHAIR_COLOUR 8
#define DRIVER_NAME_Y 0x1c
#define BOARD_LIMIT 33000000
#define BLOCK_PARAGRAPHS 0x400
#define MAX_SCALE 4
#define ROW_BUFFER 4096
/// Version of DOSCAD.DRV (semver); build.sh prints it.
/// Start-up and shut-down code: built for size, so the drawing code keeps -O2 within the
/// 64 KB image.
#define COLD __attribute__((cold))
#define DOSCAD_VERSION "1.7.1"

static const u8 COLOUR_TO_VGA[16] = {0, 1, 2, 3, 4, 5, 6, 8, 7, 9, 10, 11, 12, 13, 14, 15};
static const u8 DASH[4] = {1, 1, 0, 0};
static const u8 DAC[48] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x2a, 0x00, 0x00, 0x2a, 0x2a, 0x2a, 0x00, 0x00, 0x2a,
    0x00, 0x2a, 0x2a, 0x15, 0x00, 0x2a, 0x2a, 0x2a, 0x15, 0x15, 0x15, 0x15, 0x15, 0x3f, 0x15, 0x3f,
    0x15, 0x15, 0x3f, 0x3f, 0x3f, 0x15, 0x15, 0x3f, 0x15, 0x3f, 0x3f, 0x3f, 0x15, 0x3f, 0x3f, 0x3f,
};
static const u8 ARROW_OR[15] = {0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xf0, 0xd8, 0x98, 0x0c, 0x0c, 0x06, 0x06, 0x00};
static const u8 ARROW_AND[15] = {0x3f, 0x1f, 0x0f, 0x07, 0x03, 0x01, 0x00, 0x00, 0x03, 0x03, 0x61, 0xe1, 0xf0, 0xf0, 0xf0};

// ---- State -----------------------------------------------------------------

/// One entry of the begin/end screen-drawing stack.
struct saved_drawing
{
    i32 colour_number;
    i32 pen[2];
    i32 origin[2];
    i32 viewport[4];
    i32 host_scale;
};

static int opened;
/// DOSCADVFPS=1: the frame rate overlay is on.
static int show_fps;
/// DOSCADIDLE (default 1): halt the CPU in the keyboard poll loop.
static int idle_halt, keyboard_hooked;
static u16 block_segment;
static int block_allocated;
static u16 mode_number;
static i32 phys_w, phys_h, scale;
// pitch, granularity_kb and current_bank are read by the span kernel in doscad.asm.
i32 pitch;
u32 granularity_kb;
static u32 vram_bytes;
int current_bank = -1;
static i32 width, height, halves;
static int cursor_visible, crosshair;
static i32 cursor[2], cursor_place[2], origin[2], viewport[4], pen[2];
static i32 window[4];
static i32 zoom = 1;
/// Entry 10 (SetScale): the host's coordinates are this many times larger than the
/// screen; 1 is the plain case. SDT386+ sets 2 and more when it shows a whole sheet.
static i32 host_scale = 1;
static u8 colour;
static i32 colour_number;
static int highlight, rectangle_saved;
static u8 write_mode;
// flag42 is VGA640's byte at DS:0x42: set by a colour above 15 (entry 8) or by
// entry 39 with 0xfe; entry 7 then returns 0xff (DRAFT checks this: "V4.20 or later
// video driver"), and the next text uses the full 8-bit character set.
static int flag42, flag43;
static struct saved_drawing drawing_stack[16];
static int drawing_depth;
static u32 saved_offset;
static i32 saved_rect[4];
static u8 row_buffer[ROW_BUFFER];
static u8 arrow_background[15 * MAX_SCALE][8 * MAX_SCALE];
static int arrow_drawn;
static i32 arrow_at[2];
static char driver_name[96];
static u8 text_buffer[256];
static u8 vbe_info[512];
static u8 mode_info[256];

// ---- VRAM ------------------------------------------------------------------

static void set_bank(int bank)
{
    if (bank == current_bank) {
        return;
    }
    struct regs r = {0};
    r.eax = 0x4f05;
    r.ebx = 0;
    r.edx = (u32)bank * 64 / granularity_kb;
    int86(&r);
    current_bank = bank;
}

static u8 vram_read(u32 off)
{
    set_bank((int)(off >> 16));
    return *(volatile u8 __seg_gs *)(u32)(u16)off;
}

static void vram_write(u32 off, u8 v)
{
    set_bank((int)(off >> 16));
    *(volatile u8 __seg_gs *)(u32)(u16)off = v;
}

// The span kernel (doscad.asm): 32-bit near calls with the arguments in eax, edx, ecx.
// span_fill writes physical pixels left..right_exclusive-1 of row y, in pieces that
// stay inside one 64 KB bank; pixel_put writes one pixel. Both use span_colour (the
// colour in all four bytes) and span_mode (WRITE_REPLACE, WRITE_OR or WRITE_XOR).
extern void span_fill(i32 y, i32 left, i32 right_exclusive) __attribute__((regparm(3)));
extern void pixel_put(i32 x, i32 y) __attribute__((regparm(3)));
u32 span_colour;
u8 span_mode;

static inline __attribute__((always_inline)) void span_style(u8 col, u8 mode)
{
    span_colour = (u32)col * 0x01010101u;
    span_mode = mode;
}

static void write_pixel(i32 x, i32 y, u8 col, u8 mode)
{
    span_style(col, mode);
    pixel_put(x, y);
}

/// Writes physical pixels left..right_exclusive-1 of row y.
static void fill_physical_row(i32 y, i32 left, i32 right_exclusive, u8 col, u8 mode)
{
    span_style(col, mode);
    span_fill(y, left, right_exclusive);
}

// ---- Geometry helpers --------------------------------------------------------

/// Logical to physical coordinate.
static i32 tp(i32 l)
{
    return l * scale;
}

static void viewport_offset(i32 off[2])
{
    off[0] = origin[0] - viewport[0];
    off[1] = origin[1] - viewport[1];
}

/// Host coordinate to screen pixel along one axis (0 = x, 1 = y), with SetScale applied.
static i32 to_screen(i32 v, int axis)
{
    i32 d = v - viewport[axis];
    if (host_scale > 1) {
        d /= host_scale;
    }
    return d + origin[axis];
}

/// The viewport on the screen when SetScale is above 1 (same as clip_rectangle at 1).
static void scaled_clip(i32 c[4])
{
    c[0] = imax(origin[0], 0);
    c[1] = imax(origin[1], 0);
    c[2] = imin(to_screen(viewport[2], 0), width - 1);
    c[3] = imin(to_screen(viewport[3], 1), height - 1);
}

static void clip_rectangle(i32 c[4])
{
    i32 o[2];
    viewport_offset(o);
    c[0] = imax(viewport[0] + o[0], 0);
    c[1] = imax(viewport[1] + o[1], 0);
    c[2] = imin(viewport[2] + o[0], width - 1);
    c[3] = imin(viewport[3] + o[1], height - 1);
}

static void physical_clip_rectangle(i32 p[4])
{
    i32 c[4];
    clip_rectangle(c);
    p[0] = tp(c[0]);
    p[1] = tp(c[1]);
    p[2] = tp(c[2] + 1) - 1;
    p[3] = tp(c[3] + 1) - 1;
}

/// Writes one logical pixel (an S x S block) inside the logical clip.
static void plot(const i32 clip[4], i32 x, i32 y, u8 col, u8 mode)
{
    if (x < clip[0] || x > clip[2] || y < clip[1] || y > clip[3]) {
        return;
    }
    for (i32 py = tp(y); py < tp(y + 1); py++) {
        fill_physical_row(py, tp(x), tp(x + 1), col, mode);
    }
}

/// Writes one physical pixel inside the physical clip.
static void plot_physical(const i32 clip[4], i32 x, i32 y, u8 col, u8 mode)
{
    if (x < clip[0] || x > clip[2] || y < clip[1] || y > clip[3]) {
        return;
    }
    write_pixel(x, y, col, mode);
}

// ---- Software arrow ------------------------------------------------------------

static void hide_arrow(void)
{
    if (!arrow_drawn) {
        return;
    }
    arrow_drawn = 0;
    for (i32 r = 0; r < 15 * scale; r++) {
        for (i32 c = 0; c < 8 * scale; c++) {
            i32 x = arrow_at[0] + c;
            i32 y = arrow_at[1] + r;
            if (x < phys_w && y < phys_h) {
                vram_write((u32)y * pitch + x, arrow_background[r][c]);
            }
        }
    }
}

static void show_arrow(void)
{
    if (!opened || !cursor_visible) {
        return;
    }
    arrow_at[0] = tp(cursor[0]);
    arrow_at[1] = tp(cursor[1]);
    arrow_drawn = 1;
    for (i32 r = 0; r < 15 * scale; r++) {
        for (i32 c = 0; c < 8 * scale; c++) {
            i32 x = arrow_at[0] + c;
            i32 y = arrow_at[1] + r;
            if (x >= phys_w || y >= phys_h) {
                continue;
            }
            u32 off = (u32)y * pitch + x;
            u8 old = vram_read(off);
            arrow_background[r][c] = old;
            u8 bit = (u8)(0x80 >> (c / scale));
            if (ARROW_AND[r / scale] & bit) {
                continue;
            }
            vram_write(off, (ARROW_OR[r / scale] & bit) ? 15 : 0);
        }
    }
}

// ---- Colour and drawing state ----------------------------------------------------

static void set_colour(i32 n)
{
    if (n > 15) {
        flag42 = 1;
        return;
    }
    i32 m = (highlight && n != 0) ? 15 : n;
    colour_number = m;
    colour = COLOUR_TO_VGA[m];
}

static void set_highlight_mode(void)
{
    highlight = 1;
    colour = COLOUR_TO_VGA[15];
    write_mode = WRITE_XOR;
}

static void begin_screen_drawing(void)
{
    if (drawing_depth >= 16) {
        return;
    }
    struct saved_drawing *s = &drawing_stack[drawing_depth++];
    s->colour_number = colour_number;
    s->pen[0] = pen[0];
    s->pen[1] = pen[1];
    s->origin[0] = origin[0];
    s->origin[1] = origin[1];
    for (int i = 0; i < 4; i++) {
        s->viewport[i] = viewport[i];
    }
    s->host_scale = host_scale;
}

static void end_screen_drawing(void)
{
    if (drawing_depth == 0) {
        return;
    }
    struct saved_drawing *s = &drawing_stack[--drawing_depth];
    pen[0] = s->pen[0];
    pen[1] = s->pen[1];
    origin[0] = s->origin[0];
    origin[1] = s->origin[1];
    for (int i = 0; i < 4; i++) {
        viewport[i] = s->viewport[i];
    }
    host_scale = s->host_scale;
    set_colour(s->colour_number);
}

// ---- Rectangles ----------------------------------------------------------------

static void fill_rectangle(const i32 r[4], u8 col, u8 mode)
{
    i32 o[2];
    i32 c[4];
    i32 l, t, rr, b;
    if (host_scale > 1) {
        // Clip in host coordinates, then scale both corners (as the original driver).
        if (r[0] > r[2] || r[1] > r[3] || r[0] > viewport[2] || r[2] < viewport[0] || r[1] > viewport[3]
            || r[3] < viewport[1]) {
            return;
        }
        scaled_clip(c);
        l = imax(to_screen(imax(r[0], viewport[0]), 0), c[0]);
        t = imax(to_screen(imax(r[1], viewport[1]), 1), c[1]);
        rr = imin(to_screen(imin(r[2], viewport[2]), 0), c[2]);
        b = imin(to_screen(imin(r[3], viewport[3]), 1), c[3]);
    } else {
        viewport_offset(o);
        clip_rectangle(c);
        l = imax(r[0] + o[0], c[0]);
        t = imax(r[1] + o[1], c[1]);
        rr = imin(r[2] + o[0], c[2]);
        b = imin(r[3] + o[1], c[3]);
    }
    if (l > rr || t > b) {
        return;
    }
    for (i32 y = tp(t); y < tp(b + 1); y++) {
        fill_physical_row(y, tp(l), tp(rr + 1), col, mode);
    }
}

/// Copies the rows of the saved rectangle between the screen and off-screen VRAM.
static void copy_saved_rows(int to_screen)
{
    i32 w = saved_rect[2] - saved_rect[0];
    if (w <= 0 || w > ROW_BUFFER) {
        return;
    }
    u32 store = saved_offset;
    for (i32 y = saved_rect[1]; y < saved_rect[3]; y++) {
        u32 screen_off = (u32)y * pitch + saved_rect[0];
        u32 from = to_screen ? store : screen_off;
        u32 to = to_screen ? screen_off : store;
        for (i32 i = 0; i < w; i++) {
            row_buffer[i] = vram_read(from + i);
        }
        for (i32 i = 0; i < w; i++) {
            vram_write(to + i, row_buffer[i]);
        }
        store += w;
    }
}

static void save_rectangle(const i32 r[4])
{
    rectangle_saved = 1;
    write_mode = WRITE_OR;
    i32 left = r[0] & ~7;
    i32 bytes = (r[2] - left) / 8 + 1;
    i32 right = left + bytes * 8;
    saved_rect[0] = imin(imax(tp(left), 0), phys_w);
    saved_rect[2] = imin(imax(tp(right), 0), phys_w);
    saved_rect[1] = imin(imax(tp(r[1]), 0), phys_h);
    saved_rect[3] = imin(imax(tp(r[3] + 1), 0), phys_h);
    u32 need = (u32)(saved_rect[2] - saved_rect[0]) * (u32)imax(saved_rect[3] - saved_rect[1], 0);
    if (saved_offset + need > vram_bytes) {
        // Not enough off-screen memory: nothing is kept.
        saved_rect[2] = saved_rect[0];
        return;
    }
    copy_saved_rows(0);
}

static void restore_rectangle(void)
{
    if (!rectangle_saved) {
        return;
    }
    rectangle_saved = 0;
    write_mode = WRITE_OR;
    copy_saved_rows(1);
}

// ---- Cursor ----------------------------------------------------------------------

/// XORs an exclusive logical rectangle in the crosshair colour.
static void xor_logical_line(const i32 r[4])
{
    i32 l = imin(imax(tp(r[0]), 0), phys_w);
    i32 t = imin(imax(tp(r[1]), 0), phys_h);
    i32 rr = imin(imax(tp(r[2]), 0), phys_w);
    i32 b = imin(imax(tp(r[3]), 0), phys_h);
    if (l >= rr) {
        return;
    }
    for (i32 y = t; y < b; y++) {
        fill_physical_row(y, l, rr, COLOUR_TO_VGA[CROSSHAIR_COLOUR], WRITE_XOR);
    }
}

static void vertical_line(i32 x, i32 r[4])
{
    i32 o[2];
    viewport_offset(o);
    r[0] = x + o[0];
    r[1] = viewport[1] + o[1];
    r[2] = x + o[0] + 1;
    r[3] = viewport[3] + o[1] + 1;
}

static void horizontal_line(i32 y, i32 r[4])
{
    i32 o[2];
    viewport_offset(o);
    r[0] = viewport[0] + o[0];
    r[1] = y + o[1];
    r[2] = viewport[2] + o[0] + 1;
    r[3] = y + o[1] + 1;
}

static void move_crosshair(i32 mode, const i32 place[2])
{
    i32 r[4];
    highlight = 0;
    set_colour(CROSSHAIR_COLOUR);
    i32 old[2] = {cursor_place[0], cursor_place[1]};
    if (old[0] != place[0]) {
        if (cursor_visible) {
            vertical_line(old[0], r);
            xor_logical_line(r);
        }
        if (mode) {
            vertical_line(place[0], r);
            xor_logical_line(r);
        }
    }
    if (old[1] != place[1]) {
        if (cursor_visible) {
            horizontal_line(old[1], r);
            xor_logical_line(r);
        }
        if (mode) {
            horizontal_line(place[1], r);
            xor_logical_line(r);
        }
    }
    write_mode = WRITE_OR;
}

static int arrow_fits(const i32 place[2])
{
    i32 o[2];
    if (place[0] < viewport[0] || place[1] < viewport[1]) {
        return 0;
    }
    (void)o;
    i32 sx = to_screen(place[0], 0);
    i32 sy = to_screen(place[1], 1);
    return sx + 7 <= width - 1 && sy < height - 1;
}

static void place_arrow(i32 mode, const i32 place[2])
{
    if (mode && arrow_fits(place)) {
        cursor_place[0] = place[0];
        cursor_place[1] = place[1];
        cursor[0] = to_screen(place[0], 0);
        cursor[1] = to_screen(place[1], 1);
        cursor_visible = 1;
        return;
    }
    cursor_visible = 0;
    cursor_place[0] = NO_CURSOR;
    cursor_place[1] = NO_CURSOR;
    cursor[0] = NO_CURSOR;
    cursor[1] = NO_CURSOR;
}

static void set_cursor(i32 mode, i32 x, i32 y)
{
    i32 place[2] = {NO_CURSOR, NO_CURSOR};
    if (mode) {
        place[0] = x;
        place[1] = y;
    }
    if (crosshair && (place[0] != cursor_place[0] || place[1] != cursor_place[1])) {
        move_crosshair(mode, place);
    }
    place_arrow(mode, place);
}

static void set_cursor_style(i32 style)
{
    set_cursor(0, 0, 0);
    crosshair = style == 1;
}

// ---- Text ------------------------------------------------------------------------

// At scale 2..4 the glyph is smoothed with Scale2x/Scale3x instead of drawn as blocks.
// Masks are n x n bytes (0/1), rows in
// the glyph's order (bottom first); pixels outside a mask count as clear.

static u8 glyph_mask_small[16 * 16];
static u8 glyph_mask_big[32 * 32];

/// One pixel of an n x n mask; 0 outside it.
static int mask_pixel(const u8 *m, i32 n, i32 r, i32 c)
{
    if (r < 0 || c < 0 || r >= n || c >= n) {
        return 0;
    }
    return m[r * n + c];
}

/// Scale2x (EPX): n x n mask into 2n x 2n.
static void scale2x(const u8 *m, i32 n, u8 *out)
{
    for (i32 r = 0; r < n; r++) {
        for (i32 c = 0; c < n; c++) {
            int e = mask_pixel(m, n, r, c);
            int b = mask_pixel(m, n, r - 1, c);
            int d = mask_pixel(m, n, r, c - 1);
            int f = mask_pixel(m, n, r, c + 1);
            int h = mask_pixel(m, n, r + 1, c);
            int q[4] = {e, e, e, e};
            if (b != h && d != f) {
                if (d == b) { q[0] = d; }
                if (b == f) { q[1] = f; }
                if (d == h) { q[2] = d; }
                if (h == f) { q[3] = f; }
            }
            i32 w = 2 * n;
            out[(2 * r) * w + 2 * c] = (u8)q[0];
            out[(2 * r) * w + 2 * c + 1] = (u8)q[1];
            out[(2 * r + 1) * w + 2 * c] = (u8)q[2];
            out[(2 * r + 1) * w + 2 * c + 1] = (u8)q[3];
        }
    }
}

/// Scale3x: n x n mask into 3n x 3n.
static void scale3x(const u8 *m, i32 n, u8 *out)
{
    for (i32 r = 0; r < n; r++) {
        for (i32 c = 0; c < n; c++) {
            int a = mask_pixel(m, n, r - 1, c - 1);
            int b = mask_pixel(m, n, r - 1, c);
            int cc = mask_pixel(m, n, r - 1, c + 1);
            int d = mask_pixel(m, n, r, c - 1);
            int e = mask_pixel(m, n, r, c);
            int f = mask_pixel(m, n, r, c + 1);
            int g = mask_pixel(m, n, r + 1, c - 1);
            int h = mask_pixel(m, n, r + 1, c);
            int i = mask_pixel(m, n, r + 1, c + 1);
            int k[9] = {e, e, e, e, e, e, e, e, e};
            if (b != h && d != f) {
                if (d == b) { k[0] = d; }
                if ((d == b && e != cc) || (b == f && e != a)) { k[1] = b; }
                if (b == f) { k[2] = f; }
                if ((d == b && e != g) || (d == h && e != a)) { k[3] = d; }
                if ((b == f && e != i) || (h == f && e != cc)) { k[5] = f; }
                if (d == h) { k[6] = d; }
                if ((d == h && e != i) || (h == f && e != g)) { k[7] = h; }
                if (h == f) { k[8] = f; }
            }
            i32 w = 3 * n;
            for (i32 j = 0; j < 9; j++) {
                out[(3 * r + j / 3) * w + 3 * c + j % 3] = (u8)k[j];
            }
        }
    }
}

/// Draws the glyph smoothed at the current scale; returns 0 (nothing drawn) for scale 1.
static int draw_scaled_glyph(const i32 clip[4], i32 x, i32 y, const u8 *glyph)
{
    if (scale < 2 || scale > 4) {
        return 0;
    }
    u8 base[64];
    for (i32 r = 0; r < 8; r++) {
        for (i32 c = 0; c < 8; c++) {
            base[r * 8 + c] = (glyph[r] & (0x80 >> c)) ? 1 : 0;
        }
    }
    const u8 *m = glyph_mask_big;
    if (scale == 2) {
        scale2x(base, 8, glyph_mask_big);
    } else if (scale == 3) {
        scale3x(base, 8, glyph_mask_big);
    } else {
        scale2x(base, 8, glyph_mask_small);
        scale2x(glyph_mask_small, 16, glyph_mask_big);
    }
    i32 n = 8 * scale;
    i32 pc[4] = {tp(clip[0]), tp(clip[1]), tp(clip[2] + 1) - 1, tp(clip[3] + 1) - 1};
    i32 left = tp(x);
    i32 bottom = tp(y + 1) - 1;
    for (i32 r = 0; r < n; r++) {
        for (i32 c = 0; c < n; c++) {
            if (m[r * n + c]) {
                plot_physical(pc, left + c, bottom - r, colour, write_mode);
            }
        }
    }
    return 1;
}

static void draw_character(const i32 clip[4], i32 x, i32 y, u8 ch, int fcs)
{
    u8 code = fcs ? ch : (u8)(ch & 0x7f);
    if (code < 0x20) {
        return;
    }
    const u8 *glyph = &FONT[(code - 0x20) * 8];
    if (!draw_scaled_glyph(clip, x, y, glyph)) {
        // Scale 1: one block per set bit.
        for (i32 r = 0; r < 8; r++) {
            for (i32 c = 0; c < 8; c++) {
                if (glyph[r] & (0x80 >> c)) {
                    plot(clip, x + c, y - r, colour, write_mode);
                }
            }
        }
    }
    if (!fcs && (ch & 0x80)) {
        // Characters with bit 7 set get an overbar.
        i32 bar_y = (x % 8 == 0) ? y - 10 : y - 9;
        for (i32 c = 0; c < 8; c++) {
            plot(clip, x + c, bar_y, colour, write_mode);
        }
    }
}

COLD static i32 draw_driver_text(i32 x, i32 y, const char *text)
{
    i32 clip[4];
    clip_rectangle(clip);
    i32 left = x;
    for (const char *p = text;; p++) {
        u8 code = (u8)*p & 0x7f;
        if (code == 0) {
            break;
        }
        if (code >= 0x20) {
            draw_character(clip, left, y, code, 0);
        }
        left += 8;
    }
    return left;
}

COLD static void draw_driver_name(void)
{
    draw_driver_text(0, DRIVER_NAME_Y, driver_name);
}

/// Text while SetScale is above 1: every set pixel of the 8x8 glyph is mapped like a
/// host coordinate, so the text shrinks with the sheet. Only used when the stroke font
/// could not be read (draw_stroke_text draws it otherwise, as the original does).
COLD static void draw_text_small(i32 x, i32 y, const u8 *t, i32 len, int fcs)
{
    i32 sc[4];
    scaled_clip(sc);
    for (i32 i = 0; i < len; i++) {
        u8 code = fcs ? t[i] : (u8)(t[i] & 0x7f);
        if (code < 0x20) {
            continue;
        }
        const u8 *glyph = &FONT[(code - 0x20) * 8];
        for (i32 r = 0; r < 8; r++) {
            for (i32 c = 0; c < 8; c++) {
                if (glyph[r] & (0x80 >> c)) {
                    plot(sc, to_screen(x + i * 8 + c, 0), to_screen(y - r, 1), colour, write_mode);
                }
            }
        }
    }
}

static void draw_text(i32 x, i32 y, const u8 *t, i32 len)
{
    int fcs = flag42;
    flag42 = 0;
    if (len == 0) {
        return;
    }
    if (host_scale > 1) {
        draw_text_small(x, y, t, len, fcs);
        return;
    }
    if (x + len * 8 < viewport[0] || x > viewport[2] || y > viewport[3] || y - 7 < viewport[1]) {
        return;
    }
    i32 o[2];
    i32 clip[4];
    viewport_offset(o);
    clip_rectangle(clip);
    i32 left = x;
    for (i32 i = 0; i < len; i++) {
        if (left >= viewport[0]) {
            if (left + 8 > viewport[2]) {
                break;
            }
            draw_character(clip, left + o[0], y + o[1], t[i], fcs);
        }
        left += 8;
    }
}

static void draw_text_at(i32 x, i32 y, const u8 *t, i32 len)
{
    pen[0] = x;
    pen[1] = y;
    draw_text(x, y, t, len);
}

static void draw_text_relative(i32 dx, i32 dy, const u8 *t, i32 len)
{
    pen[0] += dx;
    pen[1] += dy;
    draw_text(pen[0], pen[1], t, len);
}

static void draw_line(i32 x1, i32 y1, i32 x2, i32 y2, int dashed);

/// Entry 29 (VGA640 0x0362): a line from the pen by (dx, dy); the pen moves to its end.
static void relative_line(i32 dx, i32 dy)
{
    draw_line(pen[0], pen[1], pen[0] + dx, pen[1] + dy, 0);
    pen[0] += dx;
    pen[1] += dy;
}

/// The stroke font text of VGA640.DRV (entries 39 and 40, code at 0x1a3a): used when the
/// text scale is not 1, a single control character is drawn, or SetScale is above 1. The
/// glyph steps are multiplied by the text scale in host units and drawn with entry 29's
/// line, so a zoomed-out sheet scales them like every other line. A negative scale writes
/// downwards, one 10-unit row per character. A character with bit 7 gets an overbar,
/// unless flag 42 is set. The pen starts at the text position (the caller put it there)
/// and returns to it.
COLD static void draw_stroke_text(const u8 *t, i32 len, i32 scale)
{
    i32 start[2] = {pen[0], pen[1]};
    int fcs = flag42;
    int alt = flag43;
    i32 s = scale < 0 ? -scale : scale;
    i32 step[2] = {0, 0};
    if (len != 0 && scale != 0) {
        if (scale < 0) {
            step[0] = -8 * s;
            step[1] = 10 * s;
        }
        for (i32 i = 0; i < len; i++) {
            if (i != 0) {
                pen[0] += step[0];
                pen[1] += step[1];
            }
            u16 code = t[i];
            if (!fcs) {
                if (code & 0x80) {
                    i32 keep[2] = {pen[0], pen[1]};
                    pen[1] -= 8 * s;
                    relative_line(8 * s, 0);
                    pen[0] = keep[0];
                    pen[1] = keep[1];
                }
                code &= 0x7f;
            }
            u16 index = (u16)(code * 2);
            u16 at = index;
            if (alt) {
                alt = 0;
                if (index < 0x40) {
                    at = (u16)(STROKE_ALT + index);
                }
            }
            u16 glyph = (u16)(peek16(stroke_segment, at) - STROKE_BASE);
            if (glyph >= STROKE_SIZE) {
                continue;
            }
            for (u16 p = glyph; p + 1 < STROKE_SIZE; p += 2) {
                u16 w = peek16(stroke_segment, p);
                if (w == 0) {
                    break;
                }
                i32 dx = (signed char)(w & 0xff);
                i32 dy = (signed char)(((w >> 8) & 0x7f) << 1) >> 1;
                if (w & 0x8000) {
                    relative_line(dx * s, dy * s);
                } else {
                    pen[0] += dx * s;
                    pen[1] += dy * s;
                }
            }
        }
    }
    pen[0] = start[0];
    pen[1] = start[1];
    flag42 = 0;
    flag43 = 0;
}

/// Copies a NUL-terminated far string; returns its length.
static i32 fetch_c_string(u16 seg, u16 off, i32 maxlen)
{
    i32 limit = imin(maxlen + 1, 0xff);
    i32 n = 0;
    while (n < limit) {
        u8 c = peek8(seg, (u16)(off + n));
        if (c == 0) {
            break;
        }
        text_buffer[n++] = c;
    }
    return n;
}

/// Copies a Pascal far string (length byte first); returns its length.
static i32 fetch_pascal_string(u16 seg, u16 off)
{
    i32 n = peek8(seg, off);
    for (i32 i = 0; i < n; i++) {
        text_buffer[i] = peek8(seg, (u16)(off + 1 + i));
    }
    return n;
}

// ---- Grid ------------------------------------------------------------------------

static i64 board_to_pixels(i64 d, i64 z)
{
    i64 h = sdiv64(z, 2);
    return d >= 0 ? sdiv64(d + h, z) : sdiv64(d - h, z);
}

static void board_to_physical(i64 x, i64 y, i64 p[2])
{
    p[0] = board_to_pixels((x - window[0] + (i64)origin[0] * zoom) * halves, 2 * (i64)zoom);
    p[1] = board_to_pixels((y - window[1] + (i64)origin[1] * zoom) * halves, 2 * (i64)zoom);
}

static void draw_grid(i32 spacing, i32 divisor)
{
    if (spacing <= 0 || divisor <= 0) {
        return;
    }
    i32 clip[4];
    physical_clip_rectangle(clip);
    i32 dot = (halves + 1) / 2;
    i64 step = spacing;
    i64 parts = divisor;
    i64 left = imax(window[0], 0);
    i64 top = imax(window[1], 0);
    i64 right = imin(window[2], BOARD_LIMIT);
    i64 bottom = imin(window[3], BOARD_LIMIT);
    for (i64 row = sdiv64(top * parts + step - 1, step);; row++) {
        i64 y = sdiv64(row * step, parts);
        if (y > bottom) {
            return;
        }
        for (i64 column = sdiv64(left * parts + step - 1, step);; column++) {
            i64 x = sdiv64(column * step, parts);
            if (x > right) {
                break;
            }
            i64 p[2];
            board_to_physical(x, y, p);
            for (i32 dy = 0; dy < dot; dy++) {
                for (i32 dx = 0; dx < dot; dx++) {
                    plot_physical(clip, (i32)p[0] + dx, (i32)p[1] + dy, colour, write_mode);
                }
            }
        }
    }
}

// ---- Lines -----------------------------------------------------------------------

/// Where a line is drawn: logical (S x S blocks) or physical pixels.
struct space
{
    int physical;
    i32 clip[4];
    i32 origin[2];
};

static void logical_space(struct space *s)
{
    i32 o[2];
    i32 c[4];
    viewport_offset(o);
    clip_rectangle(c);
    s->physical = 0;
    s->clip[0] = c[0] - o[0];
    s->clip[1] = c[1] - o[1];
    s->clip[2] = c[2] - o[0];
    s->clip[3] = c[3] - o[1];
    s->origin[0] = o[0];
    s->origin[1] = o[1];
}

/// Screen pixels inside the viewport, for shapes drawn while SetScale is above 1.
static void scaled_space(struct space *s)
{
    scaled_clip(s->clip);
    s->physical = 0;
    s->origin[0] = 0;
    s->origin[1] = 0;
}

static void plot_space(const struct space *s, i32 x, i32 y)
{
    // Physical space (scale 1, or wide lines in screen pixels): straight to the kernel.
    if (s->physical) {
        if (x < s->clip[0] || x > s->clip[2] || y < s->clip[1] || y > s->clip[3]) {
            return;
        }
        span_style(colour, write_mode);
        pixel_put(x + s->origin[0], y + s->origin[1]);
        return;
    }
    i32 c[4] = {s->clip[0] + s->origin[0], s->clip[1] + s->origin[1], s->clip[2] + s->origin[0],
                s->clip[3] + s->origin[1]};
    i32 sx = x + s->origin[0];
    i32 sy = y + s->origin[1];
    if (s->physical) {
        plot_physical(c, sx, sy, colour, write_mode);
    } else {
        plot(c, sx, sy, colour, write_mode);
    }
}

static void draw_run(const struct space *s, i32 y, i32 x1, i32 x2)
{
    if (x1 > x2) {
        return;
    }
    i32 row = y + s->origin[1];
    i32 l = x1 + s->origin[0];
    i32 r = x2 + s->origin[0];
    if (s->physical) {
        fill_physical_row(row, l, r + 1, colour, write_mode);
        return;
    }
    for (i32 py = tp(row); py < tp(row + 1); py++) {
        fill_physical_row(py, tp(l), tp(r + 1), colour, write_mode);
    }
}

static int outcode(const i32 c[4], const i32 p[2])
{
    int code = 0;
    if (p[1] > c[3]) {
        code = 8;
    } else if (p[1] < c[1]) {
        code = 4;
    }
    if (p[0] > c[2]) {
        code |= 2;
    } else if (p[0] < c[0]) {
        code |= 1;
    }
    return code;
}

static void move_onto_edge(const i32 c[4], i32 first[2], const i32 second[2], int code)
{
    i64 x = first[0];
    i64 y = first[1];
    i64 dx = (i64)second[0] - x;
    i64 dy = (i64)second[1] - y;
    if (code & 8) {
        first[0] = (i32)(x + sdiv64(dx * (c[3] - y), dy));
        first[1] = c[3];
    } else if (code & 4) {
        first[0] = (i32)(x + sdiv64(dx * (c[1] - y), dy));
        first[1] = c[1];
    } else if (code & 2) {
        first[0] = c[2];
        first[1] = (i32)(y + sdiv64(dy * (c[2] - x), dx));
    } else {
        first[0] = c[0];
        first[1] = (i32)(y + sdiv64(dy * (c[0] - x), dx));
    }
}

/// Cohen-Sutherland clipping; returns 0 when nothing is visible.
static int clip_segment(const i32 c[4], i32 first[2], i32 second[2])
{
    int sc = outcode(c, second);
    for (;;) {
        int fc = outcode(c, first);
        if (fc & sc) {
            return 0;
        }
        if (fc == 0 && sc == 0) {
            return 1;
        }
        if (fc == 0) {
            i32 t0 = first[0];
            i32 t1 = first[1];
            first[0] = second[0];
            first[1] = second[1];
            second[0] = t0;
            second[1] = t1;
            fc = sc;
            sc = 0;
        }
        move_onto_edge(c, first, second, fc);
    }
}

/// Plots a point of a Bresenham walk; counter < 0 means a solid line.
static void plot_dashed(const struct space *s, i32 x, i32 y, i32 *counter)
{
    if (*counter < 0) {
        plot_space(s, x, y);
        return;
    }
    i32 idx = *counter;
    *counter = (idx + 1) % 4;
    if (DASH[idx]) {
        plot_space(s, x, y);
    }
}

static void x_major(const struct space *s, i32 x1, i32 y1, i32 x2, i32 y2, i32 *counter)
{
    i32 cols = iabs(x2 - x1);
    i32 rows = iabs(y2 - y1);
    i32 straight = 2 * rows;
    i32 err = 2 * rows - cols;
    i32 diag = 2 * rows - 2 * cols;
    if (x1 > x2) {
        i32 t = x1;
        x1 = x2;
        x2 = t;
        t = y1;
        y1 = y2;
        y2 = t;
    }
    i32 sy = y1 <= y2 ? 1 : -1;
    i32 x = x1;
    i32 y = y1;
    for (;;) {
        plot_dashed(s, x, y, counter);
        if (x >= x2) {
            return;
        }
        x++;
        if (err > 0) {
            y += sy;
            err += diag;
        } else {
            err += straight;
        }
    }
}

static void y_major(const struct space *s, i32 x1, i32 y1, i32 x2, i32 y2, i32 *counter)
{
    i32 rows = iabs(y2 - y1);
    i32 cols = iabs(x2 - x1);
    i32 straight = 2 * cols;
    i32 err = 2 * cols - rows;
    i32 diag = 2 * cols - 2 * rows;
    if (y1 > y2) {
        i32 t = x1;
        x1 = x2;
        x2 = t;
        t = y1;
        y1 = y2;
        y2 = t;
    }
    i32 sx = x1 <= x2 ? 1 : -1;
    i32 x = x1;
    i32 y = y1;
    for (;;) {
        plot_dashed(s, x, y, counter);
        if (y >= y2) {
            return;
        }
        y++;
        if (err > 0) {
            x += sx;
            err += diag;
        } else {
            err += straight;
        }
    }
}

static void draw_diagonal(const struct space *s, i32 x1, i32 y1, i32 x2, i32 y2)
{
    i32 step = (x2 - x1 == y2 - y1) ? 1 : -1;
    if (y2 < y1) {
        i32 t = x1;
        x1 = x2;
        x2 = t;
        t = y1;
        y1 = y2;
        y2 = t;
    }
    i32 x = x1;
    for (i32 y = y1; y <= y2; y++) {
        plot_space(s, x, y);
        x += step;
    }
}

static void solid_bresenham(const struct space *s, i32 x1, i32 y1, i32 x2, i32 y2)
{
    i32 solid = -1;
    if (y1 == y2) {
        draw_run(s, y1, x1, x2);
    } else if (x1 == x2) {
        for (i32 y = y1; y <= y2; y++) {
            plot_space(s, x1, y);
        }
    } else if (iabs(x2 - x1) == iabs(y2 - y1)) {
        draw_diagonal(s, x1, y1, x2, y2);
    } else if (iabs(x2 - x1) > iabs(y2 - y1)) {
        x_major(s, x1, y1, x2, y2, &solid);
    } else {
        y_major(s, x1, y1, x2, y2, &solid);
    }
}

static void draw_solid_segment(const struct space *s, const i32 start[2], const i32 end[2])
{
    const i32 *c = s->clip;
    i32 first[2];
    i32 last[2];
    if (start[0] == end[0]) {
        if (start[0] < c[0] || start[0] > c[2]) {
            return;
        }
        i32 top = imin(start[1], end[1]);
        i32 bottom = imax(start[1], end[1]);
        if (top > c[3] || bottom < c[1]) {
            return;
        }
        first[0] = start[0];
        first[1] = imax(top, c[1]);
        last[0] = start[0];
        last[1] = imin(bottom, c[3]);
    } else if (start[1] == end[1]) {
        if (start[1] < c[1] || start[1] > c[3]) {
            return;
        }
        i32 left = imin(start[0], end[0]);
        i32 right = imax(start[0], end[0]);
        if (left > c[2] || right < c[0]) {
            return;
        }
        first[0] = imax(left, c[0]);
        first[1] = start[1];
        last[0] = imin(right, c[2]);
        last[1] = start[1];
    } else {
        first[0] = start[0];
        first[1] = start[1];
        last[0] = end[0];
        last[1] = end[1];
        if (!clip_segment(c, first, last)) {
            return;
        }
    }
    if ((first[0] == last[0] && last[1] < first[1]) || (first[1] == last[1] && last[0] < first[0])) {
        i32 t0 = first[0];
        i32 t1 = first[1];
        first[0] = last[0];
        first[1] = last[1];
        last[0] = t0;
        last[1] = t1;
    }
    solid_bresenham(s, first[0], first[1], last[0], last[1]);
}

static void draw_dashed_segment(const struct space *s, const i32 start[2], const i32 end[2])
{
    i32 first[2] = {start[0], start[1]};
    i32 last[2] = {end[0], end[1]};
    if (!clip_segment(s->clip, first, last)) {
        return;
    }
    if (first[0] == last[0] && first[1] == last[1]) {
        plot_space(s, first[0], first[1]);
        return;
    }
    i32 counter = 0;
    if (iabs(last[0] - first[0]) >= iabs(last[1] - first[1])) {
        x_major(s, first[0], first[1], last[0], last[1], &counter);
    } else {
        y_major(s, first[0], first[1], last[0], last[1], &counter);
    }
}

static void draw_line(i32 x1, i32 y1, i32 x2, i32 y2, int dashed)
{
    struct space s;
    i32 start[2] = {x1, y1};
    i32 end[2] = {x2, y2};
    if (host_scale > 1) {
        // Clip in host coordinates, scale the ends, draw in screen pixels.
        i32 hc[4] = {viewport[0], viewport[1], viewport[2], viewport[3]};
        if (!clip_segment(hc, start, end)) {
            return;
        }
        start[0] = to_screen(start[0], 0);
        start[1] = to_screen(start[1], 1);
        end[0] = to_screen(end[0], 0);
        end[1] = to_screen(end[1], 1);
        scaled_space(&s);
    } else {
        logical_space(&s);
    }
    if (dashed) {
        draw_dashed_segment(&s, start, end);
    } else {
        draw_solid_segment(&s, start, end);
    }
}

// ---- Wide line (entries 19 and 20) ---------------------------------------------------

/// Three parallel solid lines, one host unit apart at SetScale 1 and 2 (the original
/// driver's table); a single line above 2. Leaves the pen at the end.
COLD static void wide_line_relative(i32 dx, i32 dy)
{
    static const signed char OFFS[64] = {
        0, -1, 0, -1, 0, 1, 0, 1, -1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 0, -1, 0, 1, 1, 0, 0, -1, 1, 0, -1, 0, 0, 1,
        0, -2, 0, -2, 0, 2, 0, 2, -2, 0, -2, 0, 2, 0, 2, 0, -2, 0, 0, -2, 0, 2, 2, 0, 0, -2, 2, 0, -2, 0, 0, 2};
    i32 sx = pen[0];
    i32 sy = pen[1];
    if (host_scale > 2) {
        draw_line(sx, sy, sx + dx, sy + dy, 0);
    } else if (dx == 0) {
        draw_line(sx - host_scale, sy, sx - host_scale, sy + dy, 0);
        draw_line(sx + host_scale, sy, sx + host_scale, sy + dy, 0);
        draw_line(sx, sy, sx, sy + dy, 0);
    } else if (dy == 0) {
        draw_line(sx, sy - host_scale, sx + dx, sy - host_scale, 0);
        draw_line(sx, sy + host_scale, sx + dx, sy + host_scale, 0);
        draw_line(sx, sy, sx + dx, sy, 0);
    } else {
        int index;
        if (iabs(dx) < iabs(dy)) {
            index = 0;
        } else if (iabs(dx) > iabs(dy)) {
            index = 8;
        } else if (dx == dy) {
            index = 24;
        } else {
            index = 16;
        }
        if (host_scale == 2) {
            index += 32;
        }
        draw_line(sx + OFFS[index], sy + OFFS[index + 1], sx + dx + OFFS[index + 2], sy + dy + OFFS[index + 3], 0);
        draw_line(sx + OFFS[index + 4], sy + OFFS[index + 5], sx + dx + OFFS[index + 6], sy + dy + OFFS[index + 7], 0);
        draw_line(sx, sy, sx + dx, sy + dy, 0);
    }
    pen[0] = sx + dx;
    pen[1] = sy + dy;
}

// ---- Bitmap ----------------------------------------------------------------------

/// Positive a / k rounded up (VGA640's table lookup at index a + k - 1).
static i32 ceil_div(i32 a, i32 k)
{
    return (a + k - 1) / k;
}

/// Entry 32, as VGA640.DRV 0x1227: `rows` rows of `bpr` bytes as they appear on the
/// screen, at the pen. The pixels keep their size at any SetScale; only the position
/// is scaled. The clip works in whole bytes (a byte cut by the left or right edge of the
/// viewport is not drawn), the amounts cut in host units divided by the scale and
/// rounded up. `function` 1-7 turns and mirrors the source bitmap, whose size is
/// (xsize + 1) x (ysize + 1) pixels, as VGA640 0x1409 does into its buffer (checked
/// against it under an emulator for every code); function 0 draws the data as it is.
COLD static void draw_bitmap(i32 rows, i32 bpr, u16 seg, u16 off, i32 xsize, i32 ysize, i32 function)
{
    i32 k = host_scale;
    i32 w = bpr * 8;
    if (pen[0] > viewport[2] || pen[1] > viewport[3] || pen[0] + w * k < viewport[0]
        || pen[1] + rows * k < viewport[1]) {
        return;
    }
    i32 below = ceil_div(imax(pen[1] + rows * k - 1 - viewport[3], 0), k);
    i32 above = ceil_div(imax(viewport[1] - pen[1], 0), k);
    i32 right = ceil_div(imax(pen[0] + w * k - 1 - viewport[2], 0), k);
    i32 left = ceil_div(imax(viewport[0] - pen[0], 0), k);
    i32 first_row = above;
    i32 shown_rows = rows - below - above;
    i32 first_byte = 0;
    i32 bytes = bpr;
    if (left != 0 || right != 0) {
        first_byte = (left + 7) / 8;
        bytes = bpr - (left + 7) / 8 - (right + 7) / 8;
    }
    if (bytes <= 0 || shown_rows <= 0) {
        return;
    }
    function &= 7;
    i32 stride = function != 0 ? xsize / 8 + 1 : bpr;
    i32 clip[4];
    if (k > 1) {
        scaled_clip(clip);
    } else {
        clip_rectangle(clip);
    }
    i32 px = to_screen(pen[0], 0);
    i32 py = to_screen(pen[1], 1);
    for (i32 y = first_row; y < first_row + shown_rows; y++) {
        for (i32 x = first_byte * 8; x < (first_byte + bytes) * 8; x++) {
            // Source pixel (column j, row i) shown at (x, y).
            i32 i;
            i32 j;
            switch (function) {
            case 0: j = x; i = y; break;
            case 1: i = x; j = xsize - y; break;
            case 2: j = xsize - x; i = ysize - y; break;
            case 3: i = ysize - x; j = y; break;
            case 4: j = xsize - x; i = y; break;
            case 5: i = x; j = y; break;
            case 6: j = x; i = ysize - y; break;
            default: i = ysize - x; j = xsize - y; break;
            }
            // Code 6 copies whole source rows, padding bits included (VGA640).
            if (function != 0 && (i < 0 || i > ysize || j < 0 || j > (function == 6 ? stride * 8 - 1 : xsize))) {
                continue;
            }
            if (peek8(seg, (u16)(off + i * stride + (j >> 3))) & (0x80 >> (j & 7))) {
                plot(clip, px + x, py + y, colour, write_mode);
            }
        }
    }
}

// ---- Board circle ----------------------------------------------------------------

/// Midpoint circle state, in physical pixels.
struct circle
{
    i32 k, y, decision, inc_x, inc_y;
    i64 pixel[2];
    i64 board[2];
    // Offsets from the centre whose board point lies inside the window, per axis:
    // lo..hi (the window test of each point without 64-bit multiplications), and
    // the physical clip. Set by circle_bounds(), which every user must call.
    i64 lo[2];
    i64 hi[2];
    i32 clip[4];
};

/// Is c->board[axis] + offset steps before the window (which < 0), after it
/// (which > 0) or inside it (which == 0)? The bounds come from circle_bounds().
static int circle_axis_test(const struct circle *c, int axis, i32 offset, int which)
{
    if (which < 0) {
        return offset < c->lo[axis];
    }
    if (which > 0) {
        return offset > c->hi[axis];
    }
    return offset >= c->lo[axis] && offset <= c->hi[axis];
}

/// The board point of an offset is board * halves + offset * 2 * zoom; it is inside
/// the window, window[axis] * halves ..= window[axis + 2] * halves, exactly for
/// offsets from ceil((window[axis] - board) * halves / (2 * zoom)) to
/// floor((window[axis + 2] - board) * halves / (2 * zoom)).
static void circle_bounds(struct circle *c)
{
    i64 d = 2 * (i64)zoom;
    for (int axis = 0; axis < 2; axis++) {
        c->lo[axis] = -div_floor(((i64)c->board[axis] - window[axis]) * halves, d);
        c->hi[axis] = div_floor(((i64)window[axis + 2] - c->board[axis]) * halves, d);
    }
    physical_clip_rectangle(c->clip);
}

static int circle_advance(struct circle *c)
{
    int lowered = 0;
    c->inc_x += 2;
    if (c->decision >= 0) {
        c->inc_y += 4;
        c->decision += c->inc_y;
        c->y--;
        lowered = 1;
    } else {
        c->inc_y += 2;
        c->decision += c->inc_x;
    }
    c->k++;
    return lowered;
}

static void circle_span(const struct circle *c, i32 row_offset, i32 half)
{
    if (circle_axis_test(c, 0, -half, 1) || circle_axis_test(c, 0, half, -1) || !circle_axis_test(c, 1, row_offset, 0)) {
        return;
    }
    const i32 *pc = c->clip;
    i32 row = (i32)c->pixel[1] + row_offset;
    if (row < pc[1] || row > pc[3]) {
        return;
    }
    i32 cx = (i32)c->pixel[0];
    i32 l = imax(cx - half, pc[0]);
    i32 r = imin(cx + half, pc[2]);
    if (l > r) {
        return;
    }
    fill_physical_row(row, l, r + 1, colour, write_mode);
}

static void circle_point(const struct circle *c, i32 ox, i32 oy)
{
    if (!circle_axis_test(c, 0, ox, 0) || !circle_axis_test(c, 1, oy, 0)) {
        return;
    }
    plot_physical(c->clip, (i32)c->pixel[0] + ox, (i32)c->pixel[1] + oy, colour, write_mode);
}

static void circle_eight(const struct circle *c)
{
    i32 k = c->k;
    i32 y = c->y;
    circle_point(c, k, y);
    circle_point(c, y, k);
    circle_point(c, y, -k);
    circle_point(c, -y, -k);
    circle_point(c, -y, k);
    circle_point(c, k, -y);
    circle_point(c, -k, -y);
    circle_point(c, -k, y);
}

static void draw_circle(i32 x, i32 y, i32 radius, i32 mode)
{
    i64 steps = sdiv64((i64)radius * halves, 2 * (i64)zoom);
    if (steps <= 0) {
        return;
    }
    struct circle c;
    c.pixel[0] = div_floor(((i64)x - window[0] + (i64)origin[0] * zoom) * halves + zoom, 2 * (i64)zoom);
    c.pixel[1] = div_floor(((i64)y - window[1] + (i64)origin[1] * zoom) * halves + zoom, 2 * (i64)zoom);
    c.board[0] = x;
    c.board[1] = y;
    circle_bounds(&c);
    c.k = 0;
    c.y = (i32)steps;
    c.decision = 1 - (i32)steps;
    c.inc_x = 1;
    c.inc_y = 1 - 2 * (i32)steps;
    if (mode == 0) {
        for (;;) {
            circle_eight(&c);
            circle_advance(&c);
            if (c.k >= c.y) {
                break;
            }
        }
        if (c.k == c.y) {
            circle_eight(&c);
        }
        return;
    }
    for (;;) {
        if (c.k == 0) {
            circle_span(&c, 0, c.y);
        } else {
            circle_span(&c, -c.k, c.y);
            circle_span(&c, c.k, c.y);
        }
        i32 before_k = c.k;
        i32 before_y = c.y;
        if (circle_advance(&c)) {
            circle_span(&c, -before_y, before_k);
            circle_span(&c, before_y, before_k);
        }
        if (c.k >= c.y) {
            break;
        }
    }
    if (c.k == c.y) {
        circle_span(&c, -c.k, c.y);
        circle_span(&c, c.k, c.y);
    }
}

// ---- Wide shapes (translated from wide_line.rs / arcs.rs) -----------------------

static void draw_wide_line(i32 x1, i32 y1, i32 x2, i32 y2, i32 w, i32 mode);
static void draw_circle_or_stadium(const struct space *s, i32 x, i32 y, i32 rx, i32 ry,
                                   int filled, int dashed);
static void pixel_quarter_arc(const struct space *s, const i32 c[2], const i32 ends[4], i32 r,
                              i32 w, int dashed);

// Pixel space with the viewport origin: board shapes are drawn at full resolution.
// Flattened (its helpers inlined here only): every wide line asks for it.
static void __attribute__((flatten)) physical_space(struct space *s)
{
    i32 o[2];
    i32 c[4];
    viewport_offset(o);
    physical_clip_rectangle(c);
    s->physical = 1;
    s->origin[0] = tp(o[0]);
    s->origin[1] = tp(o[1]);
    for (int i = 0; i < 4; i++) {
        s->clip[i] = c[i] - s->origin[i & 1];
    }
}

static void space_line(const struct space *s, i32 x1, i32 y1, i32 x2, i32 y2, int dashed)
{
    // A solid horizontal line (every row of a filled shape): the clipping of
    // draw_solid_segment() for that case, straight to the run.
    if (!dashed && y1 == y2) {
        const i32 *c = s->clip;
        if (y1 < c[1] || y1 > c[3]) {
            return;
        }
        i32 left = imin(x1, x2);
        i32 right = imax(x1, x2);
        if (left > c[2] || right < c[0]) {
            return;
        }
        if (s->physical) {
            // span_fill ignores an empty run (left >= right_exclusive), like draw_run
            span_style(colour, write_mode);
            span_fill(y1 + s->origin[1], imax(left, c[0]) + s->origin[0], imin(right, c[2]) + s->origin[0] + 1);
            return;
        }
        draw_run(s, y1, imax(left, c[0]), imin(right, c[2]));
        return;
    }
    i32 a[2] = {x1, y1};
    i32 b[2] = {x2, y2};
    if (dashed) {
        draw_dashed_segment(s, a, b);
    } else {
        draw_solid_segment(s, a, b);
    }
}

static void fill_space_rectangle(const struct space *s, i32 l, i32 t, i32 r, i32 b)
{
    l = imax(l, s->clip[0]);
    t = imax(t, s->clip[1]);
    r = imin(r, s->clip[2]);
    b = imin(b, s->clip[3]);
    if (l > r || t > b) {
        return;
    }
    if (s->physical) {
        span_style(colour, write_mode);
        for (i32 y = t; y <= b; y++) {
            span_fill(y + s->origin[1], l + s->origin[0], r + s->origin[0] + 1);
        }
        return;
    }
    for (i32 y = t; y <= b; y++) {
        draw_run(s, y, l, r);
    }
}

static i32 board_to_space(i64 p, int axis)
{
    i64 t = p - window[axis];
    // Inside the view (nearly every call) the whole sum fits in 32 bits: the same
    // quotient without 64-bit arithmetic (|t| < 2^24, halves <= 8, zoom < 2^24).
    if (t > -0x1000000 && t < 0x1000000 && zoom < 0x1000000) {
        return ((i32)t * halves + (zoom / 2) * 2) / (2 * zoom);
    }
    return (i32)sdiv64(t * halves + (zoom / 2) * 2, 2 * (i64)zoom);
}

static i32 board_to_steps(i32 d)
{
    return (i32)sdiv64((i64)d * halves, 2 * (i64)zoom);
}

static i32 at_least_one(i32 v)
{
    return v == 0 ? 1 : v;
}

static i64 steps_to_board(i32 st)
{
    // st * zoom below 2^28 (st >= 0, one 32x32 multiply): the sum fits in 32 bits and is
    // positive, so the floor division is the truncating one.
    u64 product = (u64)(u32)st * (u32)zoom;
    if (product < 0x10000000u) {
        return ((i32)product * 4 + halves) / (2 * halves);
    }
    return div_floor((i64)st * 4 * zoom + halves, 2 * (i64)halves);
}

static i32 board_outcode(const i64 area[4], const i64 p[2])
{
    i32 code = 0;
    if (p[1] > area[3]) {
        code = 8;
    } else if (p[1] < area[1]) {
        code = 4;
    }
    if (p[0] > area[2]) {
        code |= 2;
    } else if (p[0] < area[0]) {
        code |= 1;
    }
    return code;
}

static int clip_board_segment(const i64 area[4], i64 a[2], i64 b[2])
{
    i32 ca = board_outcode(area, a);
    for (;;) {
        i32 cb = board_outcode(area, b);
        if ((ca | cb) == 0) {
            return 1;
        }
        if ((ca & cb) != 0) {
            return 0;
        }
        if (cb == 0) {
            for (int i = 0; i < 2; i++) {
                i64 t = a[i];
                a[i] = b[i];
                b[i] = t;
            }
            cb = ca;
            ca = 0;
        }
        if (cb & 8) {
            b[0] += sdiv64((a[0] - b[0]) * (area[3] - b[1]), a[1] - b[1]);
            b[1] = area[3];
        } else if (cb & 4) {
            b[0] += sdiv64((a[0] - b[0]) * (area[1] - b[1]), a[1] - b[1]);
            b[1] = area[1];
        } else if (cb & 2) {
            b[1] += sdiv64((a[1] - b[1]) * (area[2] - b[0]), a[0] - b[0]);
            b[0] = area[2];
        } else {
            b[1] += sdiv64((a[1] - b[1]) * (area[0] - b[0]), a[0] - b[0]);
            b[0] = area[0];
        }
    }
}

static void round_ends(const struct space *s, i32 mode, i32 h, const i32 a[2], const i32 b[2])
{
    if (mode != 2 || h < 2) {
        return;
    }
    draw_circle_or_stadium(s, a[0], a[1], h, h, 1, 0);
    draw_circle_or_stadium(s, b[0], b[1], h, h, 1, 0);
}

static void swap_points(i32 a[2], i32 b[2])
{
    for (int i = 0; i < 2; i++) {
        i32 t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

static void horizontal_wide_line(const struct space *s, i32 mode, i32 h, i32 a[2], i32 b[2])
{
    if (a[0] < b[0]) {
        swap_points(a, b);
    }
    i32 y = b[1];
    if (mode == 0) {
        space_line(s, b[0], y - h, a[0], y - h, 0);
        space_line(s, a[0], y - h, a[0], y + h, 0);
        space_line(s, a[0], y + h, b[0], y + h, 0);
        space_line(s, b[0], y + h, b[0], y - h, 0);
        return;
    }
    if (mode == 3) {
        draw_circle_or_stadium(s, (a[0] + b[0]) >> 1, y, (iabs(a[0] - b[0]) >> 1) + h, h, 0, 0);
        return;
    }
    fill_space_rectangle(s, b[0], y - h, a[0], y + h);
    round_ends(s, mode, h, a, b);
}

static void vertical_wide_line(const struct space *s, i32 mode, i32 h, i32 a[2], i32 b[2])
{
    if (a[1] < b[1]) {
        swap_points(a, b);
    }
    i32 x = b[0];
    if (mode == 0) {
        space_line(s, x - h, b[1], x - h, a[1], 0);
        space_line(s, x - h, a[1], x + h, a[1], 0);
        space_line(s, x + h, a[1], x + h, b[1], 0);
        space_line(s, x + h, b[1], x - h, b[1], 0);
        return;
    }
    if (mode == 3) {
        draw_circle_or_stadium(s, x, (a[1] + b[1]) >> 1, h, (iabs(a[1] - b[1]) >> 1) + h, 0, 0);
        return;
    }
    fill_space_rectangle(s, x - h, b[1], a[0] + h, a[1]);
    round_ends(s, mode, h, a, b);
}

static void perpendicular_offsets(i32 w, const i32 a[2], const i32 b[2], i32 vec[2])
{
    u32 limit = (u32)w * (u32)w;
    i32 dx = a[0] - b[0];
    i32 dy = a[1] - b[1];
    i32 sx = dx < 0 ? -1 : 1;
    i32 sy = dy < 0 ? -1 : 1;
    i32 cols = iabs(dx);
    i32 rows = iabs(dy);
    i32 x = b[0];
    i32 y = b[1];
    int x_major = cols > rows;
    i32 err = x_major ? 2 * rows - cols : 2 * cols - rows;
    vec[0] = 0;
    vec[1] = 0;
    for (;;) {
        u32 ex = (u32)(x - b[0]);
        u32 ey = (u32)(y - b[1]);
        if (ex * ex + ey * ey >= limit) {
            return;
        }
        vec[0] = x - b[0];
        vec[1] = y - b[1];
        if (x_major) {
            if (err >= 0) {
                y += sy;
                err -= 2 * cols;
            }
            x += sx;
            err += 2 * rows;
        } else {
            if (err >= 0) {
                x += sx;
                err -= 2 * rows;
            }
            y += sy;
            err += 2 * cols;
        }
    }
}

static void sideways_offsets(i32 w, const i32 a[2], const i32 b[2], i32 out[2])
{
    i32 along_x;
    i32 along_y;
    if (iabs(b[0] - b[1]) == iabs(a[0] - a[1])) {
        along_x = (i32)((u32)w * 0x1b9f / 10000);
        along_y = along_x;
    } else {
        i32 v[2];
        perpendicular_offsets(w, a, b, v);
        along_x = iabs(v[0]);
        along_y = iabs(v[1]);
    }
    out[0] = along_y >> 1;
    out[1] = along_x >> 1;
}

static void fit_offsets_to_half(i32 o[2], i32 h)
{
    i32 target = h * h;
    i32 best_diff = target;
    // VGA640 0x3d32 keeps the best increment, (0, 0) at first: when no candidate is
    // closer than the target itself, the offsets stay as they are.
    i32 best[2] = {o[0], o[1]};
    for (i32 ax = 0; ax <= 1; ax++) {
        for (i32 ay = 0; ay <= 1; ay++) {
            i32 px = o[0] + ax;
            i32 py = o[1] + ay;
            i32 sum = px * px + py * py;
            if (sum == target) {
                o[0] = px;
                o[1] = py;
                return;
            }
            if (iabs(target - sum) < best_diff) {
                best_diff = iabs(target - sum);
                best[0] = px;
                best[1] = py;
            }
        }
    }
    o[0] = best[0];
    o[1] = best[1];
}

// p: p1 p2 p3 p4 as p[0..1], p[2..3], p[4..5], p[6..7].
static void wide_line_corners(const i32 a[2], const i32 b[2], const i32 o[2], i32 p[8])
{
    i32 ax = o[0];
    i32 ay = o[1];
    if (b[1] > a[1]) {
        p[0] = b[0] + ax;
        p[1] = b[1] + ay;
        p[2] = b[0] - ax;
        p[3] = b[1] - ay;
        p[4] = a[0] + ax;
        p[5] = a[1] + ay;
        p[6] = a[0] - ax;
        p[7] = a[1] - ay;
    } else {
        p[0] = a[0] - ax;
        p[1] = a[1] + ay;
        p[2] = b[0] - ax;
        p[3] = b[1] + ay;
        p[4] = a[0] + ax;
        p[5] = a[1] - ay;
        p[6] = b[0] + ax;
        p[7] = b[1] - ay;
    }
}

static void end_arc(const struct space *s, int dashed, i32 h, const i32 c[2], i32 e1x, i32 e1y,
                    i32 e2x, i32 e2y)
{
    i32 ends[4] = {e1x, e1y, e2x, e2y};
    pixel_quarter_arc(s, c, ends, h, 1, dashed);
}

static void draw_round_outline(const struct space *s, int dashed, i32 h, const i32 a[2],
                               const i32 b[2], const i32 p[8])
{
    const i32 *bottom = b[1] > a[1] ? b : a;
    const i32 *top = b[1] > a[1] ? a : b;
    i32 bx = bottom[0];
    i32 by = bottom[1];
    i32 tx = top[0];
    i32 ty = top[1];
    end_arc(s, dashed, h, bottom, p[0], p[1], bx, by + h);
    end_arc(s, dashed, h, top, p[6], p[7], tx, ty - h);
    if (tx >= bx) {
        end_arc(s, dashed, h, bottom, bx, by + h, bx - h, by);
        end_arc(s, dashed, h, bottom, p[2], p[3], bx - h, by);
        end_arc(s, dashed, h, top, p[4], p[5], tx + h, ty);
        end_arc(s, dashed, h, top, tx + h, ty, tx, ty - h);
        space_line(s, p[0], p[1], p[4], p[5], dashed);
        space_line(s, p[2], p[3], p[6], p[7], dashed);
    } else {
        end_arc(s, dashed, h, bottom, bx, by + h, bx + h, by);
        end_arc(s, dashed, h, bottom, p[4], p[5], bx + h, by);
        end_arc(s, dashed, h, top, p[2], p[3], tx - h, ty);
        end_arc(s, dashed, h, top, tx - h, ty, tx, ty - h);
        space_line(s, p[0], p[1], p[2], p[3], dashed);
        space_line(s, p[4], p[5], p[6], p[7], dashed);
    }
}

struct edge {
    i32 x, y, cols, rows, cols2, rows2, step, err;
};

static void edge_start(struct edge *e, i32 x, i32 y, i32 cols, i32 rows, i32 step)
{
    e->x = x;
    e->y = y;
    e->cols = cols;
    e->rows = rows;
    e->cols2 = cols * 2;
    e->rows2 = rows * 2;
    e->step = step;
    e->err = cols > rows ? e->rows2 - cols : e->cols2 - rows;
}

static void first_left_edge(struct edge *e, const i32 p[8])
{
    if (p[1] == p[3]) {
        edge_start(e, p[2], p[3], p[6] - p[2], iabs(p[7] - p[3]), 1);
        return;
    }
    edge_start(e, p[0], p[1], -(p[2] - p[0]), iabs(p[3] - p[1]), -1);
    while (e->err < 0) {
        e->x--;
        e->err += e->rows2;
    }
}

static void first_right_edge(struct edge *e, const i32 p[8])
{
    if (p[1] == p[5]) {
        edge_start(e, p[4], p[5], -(p[6] - p[4]), iabs(p[7] - p[5]), -1);
        return;
    }
    edge_start(e, p[0], p[1], p[4] - p[0], iabs(p[5] - p[1]), 1);
    while (e->err < 0) {
        e->x++;
        e->err += e->rows2;
    }
}

static void step_edge(struct edge *e)
{
    if (e->cols > e->rows) {
        i32 start = e->y;
        while (e->y == start) {
            if (e->err >= 0) {
                e->y--;
                e->err -= e->cols2;
            }
            e->x += e->step;
            e->err += e->rows2;
        }
        return;
    }
    if (e->err >= 0) {
        e->x += e->step;
        e->err -= e->rows2;
    }
    e->y--;
    e->err += e->cols2;
}

static void step_left_edge(struct edge *e, const i32 p[8])
{
    int shallow = e->cols > e->rows;
    step_edge(e);
    if (shallow && e->step == -1) {
        while (e->err < 0) {
            e->x--;
            e->err += e->rows2;
        }
    }
    if (e->y != p[3]) {
        return;
    }
    edge_start(e, p[2], p[3], iabs(p[6] - p[2]), iabs(p[7] - p[3]), 1);
    e->err = e->cols2 - e->rows;
}

static void step_right_edge(struct edge *e, const i32 p[8])
{
    int shallow = e->cols > e->rows;
    step_edge(e);
    if (shallow && e->step == 1) {
        while (e->err < 0) {
            e->x++;
            e->err += e->rows2;
        }
    }
    if (e->y != p[5]) {
        return;
    }
    edge_start(e, p[4], p[5], iabs(p[6] - p[4]), iabs(p[7] - p[5]), -1);
}

static void fill_quadrilateral(const struct space *s, const i32 p[8], int dashed)
{
    struct edge left;
    struct edge right;
    first_left_edge(&left, p);
    first_right_edge(&right, p);
    i32 rows = iabs(p[1] - p[7]) + 2;
    for (i32 i = 0; i <= rows; i++) {
        space_line(s, left.x, left.y, right.x, right.y, dashed);
        if (left.y == p[7]) {
            return;
        }
        step_left_edge(&left, p);
        step_right_edge(&right, p);
    }
}

static void slanted_wide_line(const struct space *s, i32 mode, int dashed, i32 width, i32 a[2],
                              i32 b[2])
{
    if (b[0] > a[0]) {
        swap_points(a, b);
    }
    i32 o[2];
    sideways_offsets(width, a, b, o);
    if (mode == 3) {
        fit_offsets_to_half(o, width >> 1);
    }
    i32 p[8];
    wide_line_corners(a, b, o, p);
    if (mode == 3) {
        draw_round_outline(s, dashed, width >> 1, a, b, p);
        return;
    }
    if (mode == 0) {
        space_line(s, p[0], p[1], p[2], p[3], dashed);
        space_line(s, p[2], p[3], p[6], p[7], dashed);
        space_line(s, p[6], p[7], p[4], p[5], dashed);
        space_line(s, p[4], p[5], p[0], p[1], dashed);
        return;
    }
    fill_quadrilateral(s, p, dashed);
    round_ends(s, mode, width >> 1, a, b);
}

static void draw_pixel_wide_line(const struct space *s, i32 mode, i32 width, const i32 a_in[2],
                                 const i32 b_in[2])
{
    int dashed = (mode & 4) != 0;
    mode &= 3;
    width |= 1;
    i32 half = width >> 1;
    i32 a[2] = {a_in[0], a_in[1]};
    i32 b[2] = {b_in[0], b_in[1]};
    if (half == 0) {
        space_line(s, b[0], b[1], a[0], a[1], dashed);
        return;
    }
    if (imin(a[0], b[0]) - half > s->clip[2] || imax(a[0], b[0]) + half < s->clip[0]) {
        return;
    }
    if (imin(a[1], b[1]) - half > s->clip[3] || imax(a[1], b[1]) + half < s->clip[1]) {
        return;
    }
    if (a[1] == b[1]) {
        horizontal_wide_line(s, mode, half, a, b);
    } else if (a[0] == b[0]) {
        vertical_wide_line(s, mode, half, a, b);
    } else {
        slanted_wide_line(s, mode, dashed, width, a, b);
    }
}

static void draw_wide_line(i32 x1, i32 y1, i32 x2, i32 y2, i32 w, i32 mode)
{
    struct space s;
    // Fast path for the common case, with the same results as the general path below:
    // - every input within 2^28 (top 3 bits of value + 2^28 clear), so the sums fit in
    //   32 bits and w * halves too (halves <= 8);
    // - both ends inside the view widened by w: clip_board_segment would keep them;
    // - each end within 2^24 of the window corner, zoom below 2^24: board_to_space's
    //   32-bit branch, inlined.
    u32 range = ((u32)x1 + 0x10000000u) | ((u32)y1 + 0x10000000u) | ((u32)x2 + 0x10000000u) |
                ((u32)y2 + 0x10000000u) | ((u32)w + 0x10000000u) |
                ((u32)window[0] + 0x10000000u) | ((u32)window[1] + 0x10000000u) |
                ((u32)window[2] + 0x10000000u) | ((u32)window[3] + 0x10000000u);
    if ((range & 0xe0000000u) == 0 && halves <= 8 && zoom < 0x1000000 &&
        x1 >= window[0] - w && x1 <= window[2] + w && x2 >= window[0] - w &&
        x2 <= window[2] + w && y1 >= window[1] - w && y1 <= window[3] + w &&
        y2 >= window[1] - w && y2 <= window[3] + w) {
        i32 t[4] = {x2 - window[0], y2 - window[1], x1 - window[0], y1 - window[1]};
        if ((u32)(t[0] + 0xffffff) < 0x1ffffff && (u32)(t[1] + 0xffffff) < 0x1ffffff &&
            (u32)(t[2] + 0xffffff) < 0x1ffffff && (u32)(t[3] + 0xffffff) < 0x1ffffff) {
            i32 round = (zoom / 2) * 2;
            i32 twice = 2 * zoom;
            i32 pa[2] = {(t[0] * halves + round) / twice, (t[1] * halves + round) / twice};
            i32 pb[2] = {(t[2] * halves + round) / twice, (t[3] * halves + round) / twice};
            // sdiv64 truncates toward zero, like the 32-bit division
            i32 pw = w * halves / twice;
            physical_space(&s);
            draw_pixel_wide_line(&s, mode, pw, pa, pb);
            return;
        }
    }
    i64 area[4] = {(i64)window[0] - w, (i64)window[1] - w, (i64)window[2] + w,
                   (i64)window[3] + w};
    i64 b[2] = {x1, y1};
    i64 a[2] = {x2, y2};
    if (!clip_board_segment(area, a, b)) {
        return;
    }
    physical_space(&s);
    i32 pa[2] = {board_to_space(a[0], 0), board_to_space(a[1], 1)};
    i32 pb[2] = {board_to_space(b[0], 0), board_to_space(b[1], 1)};
    i32 pw = (i32)sdiv64((i64)w * halves, 2 * (i64)zoom);
    draw_pixel_wide_line(&s, mode, pw, pa, pb);
}

// ---- Circles and stadiums in a space ---------------------------------------------

struct stadium {
    const struct space *s;
    i32 x, y, shape, e;
    int dashed;
};

static void stadium_line(const struct stadium *st, i32 x1, i32 y1, i32 x2, i32 y2)
{
    space_line(st->s, st->x + x1, st->y + y1, st->x + x2, st->y + y2, st->dashed);
}

static void fill_end_circle(const struct stadium *st, i32 r)
{
    i32 k = 0;
    i32 sy = r;
    i32 d = 1 - r;
    i32 ix = 1;
    i32 iy = 1 - 2 * r;
    for (;;) {
        if (k == 0) {
            stadium_line(st, -sy, 0, sy, 0);
        } else {
            stadium_line(st, -sy, -k, sy, -k);
            stadium_line(st, -sy, k, sy, k);
        }
        ix += 2;
        if (d < 0) {
            iy += 2;
            d += ix;
        } else {
            stadium_line(st, -k, -sy, k, -sy);
            stadium_line(st, -k, sy, k, sy);
            iy += 4;
            d += iy;
            sy--;
        }
        k++;
        if (k >= sy) {
            break;
        }
    }
    if (k == sy) {
        stadium_line(st, -sy, -k, sy, -k);
        stadium_line(st, -sy, k, sy, k);
    }
}

static void stadium_point(const struct stadium *st, i32 ox, i32 oy)
{
    plot_space(st->s, st->x + ox, st->y + oy);
}

static void plot_stadium_points(const struct stadium *st, i32 k, i32 y)
{
    i32 e = st->e;
    if (st->shape == 0) {
        stadium_point(st, k, y);
        stadium_point(st, y, k);
        stadium_point(st, y, -k);
        stadium_point(st, -y, -k);
        stadium_point(st, -y, k);
        stadium_point(st, k, -y);
        stadium_point(st, -k, -y);
        stadium_point(st, -k, y);
    } else if (st->shape == 1) {
        stadium_point(st, k + e, y);
        stadium_point(st, y + e, k);
        stadium_point(st, y + e, -k);
        stadium_point(st, -y - e, -k);
        stadium_point(st, -y - e, k);
        stadium_point(st, k + e, -y);
        stadium_point(st, -k - e, -y);
        stadium_point(st, -k - e, y);
    } else {
        stadium_point(st, k, y + e);
        stadium_point(st, y, k + e);
        stadium_point(st, y, -k - e);
        stadium_point(st, -y, -k - e);
        stadium_point(st, -y, k + e);
        stadium_point(st, k, -y - e);
        stadium_point(st, -k, -y - e);
        stadium_point(st, -k, y + e);
    }
}

static void outline_circle_points(const struct stadium *st, i32 r)
{
    i32 k = 0;
    i32 sy = r;
    i32 d = 1 - r;
    i32 ix = 1;
    i32 iy = 1 - 2 * r;
    for (;;) {
        plot_stadium_points(st, k, sy);
        ix += 2;
        if (d < 0) {
            iy += 2;
            d += ix;
        } else {
            iy += 4;
            d += iy;
            sy--;
        }
        k++;
        if (k >= sy) {
            break;
        }
    }
    if (k == sy) {
        plot_stadium_points(st, k, sy);
    }
}

static void draw_circle_or_stadium(const struct space *s, i32 x, i32 y, i32 rx, i32 ry,
                                   int filled, int dashed)
{
    if (x - rx > s->clip[2] || x + rx < s->clip[0]) {
        return;
    }
    if (y - ry > s->clip[3] || y + ry < s->clip[1]) {
        return;
    }
    struct stadium st;
    st.s = s;
    st.x = x;
    st.y = y;
    st.dashed = dashed;
    st.shape = rx == ry ? 0 : (rx > ry ? 1 : -1);
    i32 larger = imax(rx, ry);
    i32 smaller = imin(rx, ry);
    if (larger == 0 || smaller == 0) {
        return;
    }
    st.e = larger - smaller;
    if (filled) {
        if (st.shape == 0) {
            fill_end_circle(&st, smaller);
        }
        return;
    }
    if (st.shape == 1) {
        i32 reach = rx - ry;
        stadium_line(&st, -reach, ry, reach, ry);
        stadium_line(&st, -reach, -ry, reach, -ry);
    } else if (st.shape == -1) {
        i32 reach = ry - rx;
        stadium_line(&st, rx, -reach, rx, reach);
        stadium_line(&st, -rx, -reach, -rx, reach);
    }
    outline_circle_points(&st, smaller);
}

// ---- Quarter arcs ----------------------------------------------------------------

struct quarter {
    i32 centre[2];
    i32 end_steps[4];
    int left, up;
    int board;
    struct circle cc;
    const struct space *s;
    int dashed;
};

struct walk {
    i32 x, y, rad, iy, ix, err;
};

static void new_walk(struct walk *w, i32 r)
{
    w->x = r;
    w->y = 0;
    w->rad = r;
    w->iy = 0;
    w->ix = 4 * r;
    w->err = 2 * r - 2;
}

static void plot_quarter_point(const struct quarter *q, i32 x, i32 y)
{
    i32 ox = q->left ? -x : x;
    i32 oy = q->up ? -y : y;
    if (q->board) {
        circle_point(&q->cc, ox, oy);
    } else {
        plot_space(q->s, q->centre[0] + ox, q->centre[1] + oy);
    }
}

static void draw_thin_quarter(const struct quarter *q, i32 steps)
{
    struct walk w;
    new_walk(&w, steps);
    int drawing = 0;
    while (w.x >= 0) {
        if (drawing) {
            plot_quarter_point(q, w.x, w.y);
        }
        if (w.x == q->end_steps[0] && w.y == q->end_steps[1]) {
            drawing = !drawing;
        }
        if (w.x == q->end_steps[2] && w.y == q->end_steps[3]) {
            drawing = !drawing;
        }
        if (w.err > 0) {
            w.y++;
            w.iy += 4;
            w.err -= w.iy;
        } else {
            w.x--;
            w.ix -= 4;
            w.err += w.ix;
        }
    }
    plot_quarter_point(q, q->end_steps[2], q->end_steps[3]);
    plot_quarter_point(q, q->end_steps[0], q->end_steps[1]);
}

static void step_outer_walk(struct walk *w)
{
    while (w->err <= 0) {
        w->x--;
        w->ix -= 4;
        w->err += w->ix;
    }
    w->y++;
    w->iy += 4;
    w->err -= w->iy;
}

static void step_inner_walk(struct walk *w)
{
    if (w->y >= w->rad) {
        w->y++;
        return;
    }
    if (w->err > 0) {
        w->y++;
        w->iy += 4;
        w->err -= w->iy;
    }
    while (w->err <= 0) {
        w->x--;
        if (w->x <= 0) {
            w->x = 0;
            return;
        }
        w->ix -= 4;
        w->err += w->ix;
    }
}

static void quarter_offset(const struct quarter *q, const struct walk *w, i64 out[2])
{
    if (q->board) {
        out[0] = steps_to_board(w->x);
        out[1] = steps_to_board(w->y);
    } else {
        out[0] = w->x;
        out[1] = w->y;
    }
    if (q->left) {
        out[0] = -out[0];
    }
    if (q->up) {
        out[1] = -out[1];
    }
}

static void draw_radial_line(const struct quarter *q, const struct walk *inner,
                             const struct walk *outer)
{
    i64 io[2];
    i64 oo[2];
    quarter_offset(q, inner, io);
    quarter_offset(q, outer, oo);
    i32 ix = (i32)(q->centre[0] + io[0]);
    i32 iy = (i32)(q->centre[1] + io[1]);
    i32 ox = (i32)(q->centre[0] + oo[0]);
    i32 oy = (i32)(q->centre[1] + oo[1]);
    if (q->board) {
        draw_wide_line(ix, iy, ox, oy, 0, 0);
    } else {
        space_line(q->s, ix, iy, ox, oy, q->dashed);
    }
}

static void draw_wide_quarter(const struct quarter *q, i32 steps, i32 pw)
{
    i32 h = pw >> 1;
    struct walk inner;
    struct walk outer;
    new_walk(&inner, steps - h);
    new_walk(&outer, steps + h);
    if (outer.rad < 2) {
        return;
    }
    draw_radial_line(q, &inner, &outer);
    while (outer.y != outer.rad) {
        step_outer_walk(&outer);
        step_inner_walk(&inner);
        draw_radial_line(q, &inner, &outer);
    }
}

static void draw_quarter(const struct quarter *q, i32 steps, i32 pw)
{
    if (pw <= 1) {
        draw_thin_quarter(q, steps);
    } else {
        draw_wide_quarter(q, steps, pw);
    }
}

static void draw_quarter_arc(const i32 c[2], const i32 ends[4], i32 r, i32 w)
{
    i32 low_x = imin(ends[0], ends[2]);
    i32 high_x = imax(ends[0], ends[2]);
    i32 low_y = imin(ends[1], ends[3]);
    i32 high_y = imax(ends[1], ends[3]);
    if ((i64)low_x > (i64)window[2] + w || (i64)high_x < (i64)window[0] - w) {
        return;
    }
    if ((i64)low_y > (i64)window[3] + w || (i64)high_y < (i64)window[1] - w) {
        return;
    }
    i32 pw = at_least_one(board_to_steps(w));
    i32 steps = at_least_one(board_to_steps(r));
    struct quarter q;
    q.board = 1;
    q.s = 0;
    q.dashed = 0;
    q.centre[0] = c[0];
    q.centre[1] = c[1];
    q.cc.pixel[0] = div_floor(((i64)c[0] - window[0] + (i64)origin[0] * zoom) * halves + zoom,
                              2 * (i64)zoom);
    q.cc.pixel[1] = div_floor(((i64)c[1] - window[1] + (i64)origin[1] * zoom) * halves + zoom,
                              2 * (i64)zoom);
    q.cc.board[0] = c[0];
    q.cc.board[1] = c[1];
    circle_bounds(&q.cc);
    q.cc.k = 0;
    q.cc.y = steps;
    q.cc.decision = 1 - steps;
    q.cc.inc_x = 1;
    q.cc.inc_y = 1 - 2 * steps;
    i32 sgn[4];
    for (int i = 0; i < 4; i++) {
        i32 diff = ends[i] - c[i & 1];
        q.end_steps[i] = diff != 0 ? steps : 0;
        sgn[i] = diff == 0 ? 0 : (diff < 0 ? -steps : steps);
    }
    q.left = (sgn[0] | sgn[2]) < 0;
    q.up = (sgn[1] | sgn[3]) < 0;
    draw_quarter(&q, steps, pw);
}

static void pixel_quarter_arc(const struct space *s, const i32 c[2], const i32 ends[4], i32 r,
                              i32 w, int dashed)
{
    i32 low_x = imin(ends[0], ends[2]);
    i32 high_x = imax(ends[0], ends[2]);
    i32 low_y = imin(ends[1], ends[3]);
    i32 high_y = imax(ends[1], ends[3]);
    if (low_x > s->clip[2] + w || high_x < s->clip[0] - w) {
        return;
    }
    if (low_y > s->clip[3] + w || high_y < s->clip[1] - w) {
        return;
    }
    i32 pw = at_least_one(w);
    i32 steps = at_least_one(r);
    struct quarter q;
    q.board = 0;
    q.s = s;
    q.dashed = dashed;
    q.centre[0] = c[0];
    q.centre[1] = c[1];
    i32 d[4];
    for (int i = 0; i < 4; i++) {
        d[i] = ends[i] - c[i & 1];
        q.end_steps[i] = iabs(d[i]);
    }
    q.left = (d[0] | d[2]) < 0;
    q.up = (d[1] | d[3]) < 0;
    draw_quarter(&q, steps, pw);
}

// ---- Wide arcs (entry 18) --------------------------------------------------------

static i32 place_end_on_circle(const i32 c[2], i32 e[2], i32 r)
{
    if (c[0] == e[0]) {
        if (c[1] < e[1]) {
            e[1] = c[1] + r;
            return 3;
        }
        e[1] = c[1] - r;
        return 1;
    }
    if (c[1] == e[1]) {
        if (c[0] < e[0]) {
            e[0] = c[0] + r;
            return 0;
        }
        e[0] = c[0] - r;
        return 2;
    }
    return 0;
}

static void quarter4(const i32 c[2], i32 e0, i32 e1, i32 e2, i32 e3, i32 r, i32 w)
{
    i32 ends[4] = {e0, e1, e2, e3};
    draw_quarter_arc(c, ends, r, w);
}

static void draw_arc_outline(const i32 c[2], const i32 ends[4], i32 r, i32 w, i32 mode)
{
    i32 f[2] = {ends[0], ends[1]};
    i32 sc[2] = {ends[2], ends[3]};
    i32 flags = place_end_on_circle(c, f, r) | (place_end_on_circle(c, sc, r) << 2);
    i32 half = w >> 1;
    if (flags == 4 || flags == 6 || flags == 0xc || flags == 0xe) {
        swap_points(f, sc);
    }
    static const signed char sides[4][6] = {
        {-1, 1, 1, -1, -1, 1},
        {1, 1, -1, -1, 1, 1},
        {1, -1, -1, 1, 1, -1},
        {-1, -1, 1, 1, -1, -1},
    };
    int row = -1;
    if (flags == 1 || flags == 4) {
        row = 0;
    } else if (flags == 9 || flags == 6) {
        row = 1;
    } else if (flags == 0xb || flags == 0xe) {
        row = 2;
    } else if (flags == 3 || flags == 0xc) {
        row = 3;
    }
    i32 base[6] = {sc[0], f[1], sc[0], f[1], f[0], sc[1]};
    i32 v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = base[i] + (row < 0 ? 0 : sides[row][i] * half);
    }
    i32 ix = v[0];
    i32 iy = v[1];
    i32 ox = v[2];
    i32 oy = v[3];
    i32 rx = v[4];
    i32 ry = v[5];
    quarter4(c, f[0], iy, ix, sc[1], r - half, 1);
    quarter4(c, f[0], oy, ox, sc[1], r + half, 1);
    if (mode != 3) {
        draw_wide_line(ix, c[1], ox, c[1], 0, 0);
        draw_wide_line(c[0], iy, c[0], oy, 0, 0);
        return;
    }
    i32 fc[2] = {c[0], f[1]};
    quarter4(fc, c[0], iy, rx, f[1], half, 1);
    quarter4(fc, rx, f[1], c[0], oy, half, 1);
    i32 scc[2] = {sc[0], c[1]};
    quarter4(scc, ix, c[1], sc[0], ry, half, 1);
    quarter4(scc, sc[0], ry, ox, c[1], half, 1);
}

static void draw_wide_arc(i32 cx, i32 cy, i32 x1, i32 y1, i32 x2, i32 y2, i32 r, i32 w, i32 mode)
{
    i32 c[2] = {cx, cy};
    i32 ends[4] = {x1, y1, x2, y2};
    if ((mode == 0 || mode == 3) && w >= 3) {
        draw_arc_outline(c, ends, r, w, mode);
        return;
    }
    draw_quarter_arc(c, ends, r, w);
    if (mode == 2) {
        draw_circle(x1, y1, w >> 1, 1);
        draw_circle(x2, y2, w >> 1, 1);
    }
}

// ---- VESA setup ------------------------------------------------------------------

static int get_mode_info(u16 mode)
{
    struct regs r = {0};
    r.eax = 0x4f01;
    r.ecx = mode;
    r.edi = (u32)(u16)(u32)mode_info;
    r.es = code_segment();
    int86(&r);
    return (r.eax & 0xffff) == 0x004f;
}

static u16 info16(int off)
{
    return (u16)(mode_info[off] | (mode_info[off + 1] << 8));
}

/// Finds "key" (a name followed by '=') in the caller's environment.
/// Returns 1 and the value's place (segment, offset), or 0 when the variable is not set.
COLD static int find_variable(const char *key, u16 *env, u16 *value)
{
    // The caller's PSP holds the segment of its environment at 0x2c.
    struct regs r = {0};
    r.eax = 0x6200;
    r.intno = 0x21;
    int86(&r);
    u16 psp = (u16)r.ebx;
    *env = (u16)(peek8(psp, 0x2c) | (peek8(psp, 0x2d) << 8));
    // NAME=value strings, each ended by 0; an empty string ends the block.
    u16 p = 0;
    while (peek8(*env, p) != 0) {
        int i = 0;
        while (key[i] && peek8(*env, (u16)(p + i)) == (u8)key[i]) {
            i++;
        }
        if (key[i] == 0) {
            *value = (u16)(p + i);
            return 1;
        }
        while (peek8(*env, p) != 0) {
            p++;
        }
        p++;
    }
    return 0;
}

/// VGA640.DRV of OrCAD 386+: its size, the place of its name and of its 8x8 font.
#define VGA640_SIZE 29712
#define VGA640_NAME_OFFSET 0x67e8
#define VGA640_FONT_OFFSET 0x6850
static const char VGA640_NAME[] = "IBM VGA or Compatible 640x480 16 color";
#define VGA640_NAME_LENGTH (sizeof(VGA640_NAME) - 1)
#define PATH_LENGTH 128
static char font_path[PATH_LENGTH];

/// Calls int 21h with AX = function, BX, CX and DX (DX an offset in our segment).
/// Returns 0 when DOS sets the carry flag, else 1; *ax gets AX.
static int dos_call(u16 function, u16 bx, u16 cx, u16 dx, u16 *ax)
{
    struct regs r = {0};
    r.intno = 0x21;
    r.eax = function;
    r.ebx = bx;
    r.ecx = cx;
    r.edx = dx;
    int86(&r);
    *ax = (u16)r.eax;
    return (r.flags & 1) == 0;
}

/// Moves the file pointer of handle to offset from the start. Returns 1 on success.
COLD static int dos_seek(u16 handle, u32 offset)
{
    struct regs r = {0};
    r.intno = 0x21;
    r.eax = 0x4200;
    r.ebx = handle;
    r.ecx = offset >> 16;
    r.edx = offset & 0xffff;
    int86(&r);
    return (r.flags & 1) == 0;
}

/// Reads count bytes at offset of the open file into buffer. Returns 1 when all came.
COLD static int read_at(u16 handle, u32 offset, u8 *buffer, u16 count)
{
    u16 ax = 0;
    if (!dos_seek(handle, offset)) {
        return 0;
    }
    if (!dos_call(0x3f00, handle, count, (u16)(u32)buffer, &ax)) {
        return 0;
    }
    return ax == count;
}

COLD static u16 dos_allocate(u16 paragraphs, u16 *largest);
COLD static u16 dos_allocate_resident(u16 paragraphs, const char *name);
COLD static void dos_free(u16 seg);

/// Paragraphs of the stroke font block.
#define STROKE_PARAGRAPHS ((STROKE_SIZE + 15) / 16)

/// The last paragraph of the image block (stub.asm: the build's identity, then at
/// offset 12 the segment of the stroke font block, 0 while there is none).
static u16 identity_segment(void)
{
    return (u16)(code_segment() + (((u16)(u32)image_end + 15) >> 4) + 1);
}

/// Returns 1 when the MCB before seg is a resident block of ours: owner 8 (DOS) and
/// the given number of paragraphs.
static int resident_block(u16 seg, u16 paragraphs)
{
    u8 type = peek8((u16)(seg - 1), 0);
    return (type == 'M' || type == 'Z') && peek16((u16)(seg - 1), 1) == 8 &&
           peek16((u16)(seg - 1), 3) == paragraphs;
}

/// Reads the stroke font of the open VGA640.DRV into a DOS block (stroke_segment stays 0
/// when it cannot: text then falls back to the small 8x8 text). Done once per DOS
/// session: a later load of the image takes the block it finds through the image block.
COLD static void load_strokes(u16 handle)
{
    if (stroke_segment) {
        return;
    }
    u16 id = identity_segment();
    u16 kept = peek16(id, 12);
    if (kept && resident_block(kept, STROKE_PARAGRAPHS)) {
        stroke_segment = kept;
        return;
    }
    u16 seg = dos_allocate_resident(STROKE_PARAGRAPHS, "DOSCADFN");
    if (seg == 0) {
        return;
    }
    for (u16 done = 0; done < STROKE_SIZE; done += 1024) {
        u16 count = (u16)imin(1024, STROKE_SIZE - done);
        if (!read_at(handle, STROKE_FILE_OFFSET + done, row_buffer, count)) {
            dos_free(seg);
            return;
        }
        for (u16 i = 0; i < count; i++) {
            poke8(seg, (u16)(done + i), row_buffer[i]);
        }
    }
    stroke_segment = seg;
    poke16(id, 12, seg);
}

/// Reads the font from the file named in font_path after checking that it is VGA640.DRV.
/// Returns 1 when FONT holds the font.
COLD static int read_font_file(void)
{
    u16 handle = 0;
    u16 ax = 0;
    // Open for reading only: the OrCAD folder may be read-only.
    if (!dos_call(0x3d00, 0, 0, (u16)(u32)font_path, &handle)) {
        return 0;
    }
    int ok = 1;
    // The size: seek to the end (AX = 4202h), DX:AX is the length.
    struct regs r = {0};
    r.intno = 0x21;
    r.eax = 0x4202;
    r.ebx = handle;
    int86(&r);
    u32 size = ((r.edx & 0xffff) << 16) | (r.eax & 0xffff);
    if ((r.flags & 1) || size != VGA640_SIZE) {
        ok = 0;
    }
    // The driver name, so that a different file of the same size is not taken.
    u8 name[VGA640_NAME_LENGTH];
    if (ok && !read_at(handle, VGA640_NAME_OFFSET, name, VGA640_NAME_LENGTH)) {
        ok = 0;
    }
    for (u16 i = 0; ok && i < VGA640_NAME_LENGTH; i++) {
        if (name[i] != (u8)VGA640_NAME[i]) {
            ok = 0;
        }
    }
    if (ok && !read_at(handle, VGA640_FONT_OFFSET, FONT, FONT_SIZE)) {
        ok = 0;
    }
    if (ok) {
        load_strokes(handle);
    }
    dos_call(0x3e00, handle, 0, 0, &ax);
    return ok;
}

/// Copies the value at env:p into font_path up to its last '\' (keep_directory)
/// or whole, then appends tail. Returns 0 when it does not fit.
COLD static int make_font_path(u16 env, u16 p, int keep_directory, const char *tail)
{
    int n = 0;
    int cut = 0;
    while (peek8(env, (u16)(p + n)) != 0) {
        if (n >= PATH_LENGTH - 1) {
            return 0;
        }
        font_path[n] = (char)peek8(env, (u16)(p + n));
        n++;
        // After the last '\' or ':' comes the file name, dropped for a directory.
        if (font_path[n - 1] == '\\' || font_path[n - 1] == ':') {
            cut = n;
        }
    }
    if (keep_directory) {
        n = cut;
    }
    // A directory value written without its final '\' gets one.
    if (*tail && n > 0 && font_path[n - 1] != '\\' && font_path[n - 1] != ':') {
        font_path[n++] = '\\';
    }
    while (*tail) {
        if (n >= PATH_LENGTH - 1) {
            return 0;
        }
        font_path[n++] = *tail++;
    }
    font_path[n] = 0;
    return 1;
}

/// Loads the font once: DOSCADFONT (the full path of a VGA640.DRV), else VGA640.DRV
/// next to the driver named in ORCADDD, else %ORCADESP%\DRV\VGA640.DRV.
/// Returns 1 when FONT holds the font.
COLD static int load_font(void)
{
    u16 env = 0;
    u16 p = 0;
    if (font_loaded) {
        return 1;
    }
    // An explicit path is the only one tried: a wrong one must not be hidden.
    if (find_variable("DOSCADFONT=", &env, &p)) {
        font_loaded = make_font_path(env, p, 0, "") && read_font_file();
        return font_loaded;
    }
    if (find_variable("ORCADDD=", &env, &p) && make_font_path(env, p, 1, "VGA640.DRV")) {
        font_loaded = read_font_file();
    }
    if (!font_loaded && find_variable("ORCADESP=", &env, &p)
        && make_font_path(env, p, 0, "DRV\\VGA640.DRV")) {
        font_loaded = read_font_file();
    }
    return font_loaded;
}

/// Says in text mode that the font is missing, and waits for a key so it can be read
/// before PCB386 goes on.
COLD static void report_missing_font(void)
{
    static const char message[] =
        "\r\nDOSCAD.DRV: cannot read the 8x8 font from OrCAD's VGA640.DRV.\r\n"
        "Put VGA640.DRV next to DOSCAD.DRV, or set DOSCADFONT=<path of VGA640.DRV>.\r\n"
        "Press a key.\r\n$";
    u16 ax = 0;
    dos_call(0x0900, 0, 0, (u16)(u32)message, &ax);
    // DOS function 08h: one key, without echo (int86 runs only int 10h and 21h).
    dos_call(0x0800, 0, 0, 0, &ax);
}

/// Reads a decimal number at env:*p and moves *p past its digits (0 when there are none).
COLD static i32 read_number(u16 env, u16 *p)
{
    i32 v = 0;
    for (;;) {
        u8 c = peek8(env, *p);
        if (c < '0' || c > '9') {
            return v;
        }
        v = v * 10 + (c - '0');
        (*p)++;
    }
}

/// 1 when the environment string at env:p, in capitals, is text and then ends.
COLD static int env_name_is(u16 env, u16 p, const char *text)
{
    for (; *text; text++, p++) {
        u8 c = peek8(env, p);
        if (c >= 'a' && c <= 'z') {
            c = (u8)(c - 32);
        }
        if (c != (u8)*text) {
            return 0;
        }
    }
    return peek8(env, p) == 0;
}

/// 1 when the caller is ESP (ESPMAIN.EXE, its launcher ORCAD.EXE) or one of ESP's
/// configuration programs (ESP_C, PCB_C, SDT_C, PLD_C, VST_C: *_C.EXE), all drawn for
/// 640x480. The program's path follows its environment block (after the empty string
/// and a word count).
COLD static int caller_is_esp(void)
{
    struct regs r = {0};
    r.eax = 0x6200;
    r.intno = 0x21;
    int86(&r);
    u16 psp = (u16)r.ebx;
    u16 env = (u16)(peek8(psp, 0x2c) | (peek8(psp, 0x2d) << 8));
    u16 p = 0;
    while (peek8(env, p) != 0 || peek8(env, (u16)(p + 1)) != 0) {
        p++;
    }
    p += 4;
    // The file name: after the last '\' or ':'.
    u16 name = p;
    u16 q = p;
    for (; peek8(env, q) != 0 && q - p < 128; q++) {
        if (peek8(env, q) == '\\' || peek8(env, q) == ':') {
            name = (u16)(q + 1);
        }
    }
    return env_name_is(env, name, "ESPMAIN.EXE") || env_name_is(env, name, "ORCAD.EXE") ||
           (q - name > 6 && env_name_is(env, (u16)(q - 6), "_C.EXE"));
}

/// Reads the resolution from DOSCADVRES ("WxH") and the scale from DOSCADVSCALE ("S");
/// a variable not set leaves its values as they are. ESP takes DOSCADESPRES instead when
/// it is set, and always scale -1: the largest that fits 640x480, so its text keeps the
/// size it has on the 640x480 screen ESP was drawn for (set by pick_mode).
COLD static void read_requested_mode(i32 *w, i32 *h, i32 *s)
{
    u16 env = 0;
    u16 p = 0;
    int esp = caller_is_esp();
    if ((esp && find_variable("DOSCADESPRES=", &env, &p)) ||
        find_variable("DOSCADVRES=", &env, &p)) {
        i32 want_w = read_number(env, &p);
        u8 c = peek8(env, p);
        // Without the 'x' and a height the value is ignored: the largest mode is used.
        if (c == 'x' || c == 'X') {
            p++;
            *w = want_w;
            *h = read_number(env, &p);
        }
    }
    if (esp) {
        *s = -1;
    } else if (find_variable("DOSCADVSCALE=", &env, &p)) {
        i32 want_s = read_number(env, &p);
        if (want_s > 0) {
            *s = want_s;
        }
    }
    // The frame rate overlay is off unless DOSCADVFPS=1.
    show_fps = 0;
    if (find_variable("DOSCADVFPS=", &env, &p)) {
        show_fps = (read_number(env, &p) == 1);
    }
    // The idle halt is on unless DOSCADIDLE=0.
    idle_halt = 1;
    if (find_variable("DOSCADIDLE=", &env, &p)) {
        idle_halt = (read_number(env, &p) != 0);
    }
}

/// Hooks INT 16h (int16_hook in doscad.asm) while the driver is open.
COLD static void hook_keyboard(void)
{
    if (keyboard_hooked || !idle_halt) {
        return;
    }
    struct regs g = {0};
    g.intno = 0x21;
    g.eax = 0x3516;
    int86(&g);
    // The vector may already be this hook: the image is resident and shared, and the ESP
    // launcher puts back the vector ESP had (this hook) after DRAFT or PCB386 unhooked it.
    // old16 still holds the vector the hook replaced then (kept across loads, stub.asm);
    // taking the hook itself as old16 would make every INT 16h call recurse for ever.
    if ((u16)g.ebx == (u16)(u32)int16_hook && g.es == code_segment()) {
        if (old16[1] != 0 && !(old16[0] == (u16)(u32)int16_hook && old16[1] == g.es)) {
            keyboard_hooked = 1;
        }
        return;
    }
    old16[0] = (u16)g.ebx;
    old16[1] = g.es;
    struct regs s = {0};
    s.intno = 0x21;
    s.eax = 0x2516;
    s.edx = (u32)(u16)(u32)int16_hook;
    int86(&s);
    keyboard_hooked = 1;
}

/// Puts the hook back when the INT 16h vector has been reset to the one it replaced: the
/// ESP launcher does that to the programs it starts (DRAFT, PCB_C, SDT_C). A vector set to
/// anything else is left alone. Interrupts are off while the C code runs.
static void keep_keyboard_hook(void)
{
    if (keyboard_hooked && peek16(0, 0x58) == old16[0] && peek16(0, 0x5a) == old16[1]) {
        u16 cs_now;
        __asm__ volatile("mov %%cs, %0" : "=r"(cs_now));
        poke16(0, 0x58, (u16)(u32)int16_hook);
        poke16(0, 0x5a, cs_now);
    }
}

/// Restores INT 16h if the vector is still the hook's.
COLD static void unhook_keyboard(void)
{
    if (!keyboard_hooked) {
        return;
    }
    struct regs g = {0};
    g.intno = 0x21;
    g.eax = 0x3516;
    int86(&g);
    u16 cs_now;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs_now));
    if ((u16)g.ebx == (u16)(u32)int16_hook && g.es == cs_now) {
        __asm__ volatile("pushw %%ds\n\tmov %0, %%ds\n\t"
                         "mov $0x2516, %%ax\n\tint $0x21\n\tpopw %%ds"
                         :
                         : "r"(old16[1]), "d"(old16[0])
                         : "ax", "memory");
    }
    keyboard_hooked = 0;
}

/// Finds the requested 8bpp mode; when the BIOS lacks it, the largest mode that fits in
/// the request, or the largest mode of all when none fits or nothing was requested.
/// Returns 0 when there is no 8bpp mode.
COLD static u16 choose_mode(i32 want_w, i32 want_h)
{
    struct regs r = {0};
    vbe_info[0] = 'V';
    vbe_info[1] = 'B';
    vbe_info[2] = 'E';
    vbe_info[3] = '2';
    r.eax = 0x4f00;
    r.edi = (u32)(u16)(u32)vbe_info;
    r.es = code_segment();
    int86(&r);
    if ((r.eax & 0xffff) != 0x004f) {
        return 0;
    }
    vram_bytes = (u32)(vbe_info[0x12] | (vbe_info[0x13] << 8)) << 16;
    u16 list_off = (u16)(vbe_info[0x0e] | (vbe_info[0x0f] << 8));
    u16 list_seg = (u16)(vbe_info[0x10] | (vbe_info[0x11] << 8));
    u16 best = 0;
    u32 best_area = 0;
    // The largest mode no wider and no taller than the request.
    u16 best_fit = 0;
    u32 best_fit_area = 0;
    for (u16 i = 0;; i += 2) {
        u16 mode = (u16)(peek8(list_seg, (u16)(list_off + i)) | (peek8(list_seg, (u16)(list_off + i + 1)) << 8));
        if (mode == 0xffff) {
            break;
        }
        if (!get_mode_info(mode)) {
            continue;
        }
        if ((info16(0) & 0x11) != 0x11 || mode_info[0x1b] != 4 || mode_info[0x19] != 8) {
            continue;
        }
        u32 w = info16(0x12);
        u32 h = info16(0x14);
        if ((i32)w == want_w && (i32)h == want_h) {
            return mode;
        }
        if (w * h > best_area) {
            best_area = w * h;
            best = mode;
        }
        if ((i32)w <= want_w && (i32)h <= want_h && w * h > best_fit_area) {
            best_fit_area = w * h;
            best_fit = mode;
        }
    }
    if (best_fit != 0) {
        return best_fit;
    }
    return best;
}

static void append_number(char **p, u32 v)
{
    char digits[10];
    int n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        *(*p)++ = digits[--n];
    }
}

static void append_text(char **p, const char *t)
{
    while (*t) {
        *(*p)++ = *t++;
    }
}

/// Chooses the mode open_screen will set and the screen size in driver pixels, without
/// setting it: ESP asks the size (entry 6) before it opens the driver (entry 0).
COLD static int pick_mode(void)
{
    i32 want_w = 0;
    i32 want_h = 0;
    // 0: no DOSCADVSCALE, the scale follows the mode's height below; -1: ESP's.
    i32 s = 0;
    read_requested_mode(&want_w, &want_h, &s);
    mode_number = choose_mode(want_w, want_h);
    if (mode_number == 0 || !get_mode_info(mode_number)) {
        return 0;
    }
    phys_w = info16(0x12);
    phys_h = info16(0x14);
    pitch = info16(0x10);
    granularity_kb = info16(4);
    if (granularity_kb == 0) {
        granularity_kb = 64;
    }
    // Automatic scale: 2 from 1080 lines up, where the 8x8 text gets too
    // small; 1 below, as the original drivers up to 1280x1024.
    if (s == 0) {
        s = (phys_h >= 1080) ? 2 : 1;
    } else if (s < 0) {
        s = imin(phys_w / 640, phys_h / 480);
    }
    scale = imin(imax(s, 1), MAX_SCALE);
    width = phys_w / scale;
    height = phys_h / scale;
    halves = 2 * scale;
    saved_offset = (((u32)phys_h * pitch) + 0xffff) & ~0xffffu;
    return 1;
}

COLD static int open_screen(void)
{
    if (!pick_mode()) {
        return 0;
    }
    struct regs r = {0};
    r.eax = 0x4f02;
    r.ebx = mode_number;
    int86(&r);
    current_bank = -1;
    // The 16 VGA colours of VGA640.DRV.
    // Written through the DAC ports: index 0, then red, green, blue per entry.
    out8(0x3c8, 0);
    for (int i = 0; i < 48; i++) {
        out8(0x3c9, DAC[i]);
    }
    char *p = driver_name;
    // The banner of the start screen: name, version and the mode actually set.
    append_text(&p, "Variable resolution advanced VESA driver  v" DOSCAD_VERSION "  ");
    append_number(&p, phys_w);
    *p++ = 'x';
    append_number(&p, phys_h);
    append_text(&p, " 256 colours");
    *p = 0;
    return 1;
}

static void reset_state(void)
{
    highlight = 0;
    colour_number = 15;
    colour = COLOUR_TO_VGA[15];
    cursor_visible = 0;
    crosshair = 0;
    cursor[0] = cursor[1] = NO_CURSOR;
    cursor_place[0] = cursor_place[1] = NO_CURSOR;
    flag42 = 0;
    flag43 = 0;
    write_mode = WRITE_OR;
    host_scale = 1;
    drawing_depth = 0;
    rectangle_saved = 0;
    arrow_drawn = 0;
}

// ---- Memory block (entries 2 and 3) ----------------------------------------------

COLD static u16 dos_allocate(u16 paragraphs, u16 *largest)
{
    struct regs r = {0};
    r.eax = 0x4800;
    r.ebx = paragraphs;
    r.intno = 0x21;
    int86(&r);
    if (r.flags & 1) {
        *largest = (u16)r.ebx;
        return 0;
    }
    return (u16)r.eax;
}

/// Allocates a block that outlives the program: in upper memory when DOS has it, else
/// at the top of conventional memory (last fit), then gives it to DOS (MCB owner 8)
/// under the 8-character name. Returns its segment, or 0.
COLD static u16 dos_allocate_resident(u16 paragraphs, const char *name)
{
    struct regs r = {0};
    r.intno = 0x21;
    r.eax = 0x5800;
    int86(&r);
    u16 strategy = (u16)r.eax;
    r.eax = 0x5802;
    int86(&r);
    u16 link = (u16)(r.eax & 0xff);
    r.eax = 0x5803;
    r.ebx = 1;
    int86(&r);
    r.eax = 0x5801;
    r.ebx = 0x82;
    int86(&r);
    u16 unused = 0;
    u16 seg = dos_allocate(paragraphs, &unused);
    r.eax = 0x5801;
    r.ebx = strategy;
    int86(&r);
    r.eax = 0x5803;
    r.ebx = link;
    int86(&r);
    if (seg) {
        poke16((u16)(seg - 1), 1, 8);
        for (u16 i = 0; i < 8; i++) {
            poke8((u16)(seg - 1), (u16)(8 + i), (u8)name[i]);
        }
    }
    return seg;
}

COLD static void dos_free(u16 seg)
{
    struct regs r = {0};
    r.eax = 0x4900;
    r.es = seg;
    r.intno = 0x21;
    int86(&r);
}

COLD static void allocate_block(void)
{
    if (block_allocated) {
        return;
    }
    // A plain DOS block of the calling program, freed with it.
    u16 unused = 0;
    block_segment = dos_allocate(BLOCK_PARAGRAPHS, &unused);
    block_allocated = block_segment != 0;
}

COLD static void entry_allocate(void)
{
    allocate_block();
    poke16((u16)args[(0xe - 6) / 2], (u16)args[(0xc - 6) / 2], block_segment);
    poke8((u16)args[(8 - 6) / 2], (u16)args[(6 - 6) / 2], 0);
    write_mode = WRITE_OR;
    ret_ax = 0;
}

COLD static void entry_bitmap_buffer(void)
{
    i32 rows = arg(0xa);
    i32 bpr = width / 8;
    if (rows > 0 && bpr * rows > 16 * 1024) {
        bpr = 16 * 1024 / rows;
    }
    u16 seg = (u16)args[1];
    u16 off = (u16)args[0];
    poke16(seg, off, 0);
    poke16(seg, (u16)(off + 2), block_segment);
    poke16(seg, (u16)(off + 4), (u16)bpr);
    for (i32 i = 0; i < bpr * rows; i++) {
        poke8(block_segment, (u16)i, 0);
    }
}

// ---- Dispatcher ------------------------------------------------------------------

static void rectangle_args(i32 r[4])
{
    r[0] = arg(0xc);
    r[1] = arg(0xa);
    r[2] = arg(8);
    r[3] = arg(6);
}

/// Entries 21 and 22 (VGA640 0x21bd): circle or stadium with radii rx, ry; the pen moves
/// to the centre. With SetScale above 1 the clip test is in host units and the radii are
/// divided by the scale (nothing drawn when one becomes 0).
COLD static void draw_circle_entry(i32 x, i32 y, i32 rx, i32 ry, int filled)
{
    pen[0] = x;
    pen[1] = y;
    struct space s;
    if (host_scale > 1) {
        if (x - rx > viewport[2] || x + rx < viewport[0] || y - ry > viewport[3] || y + ry < viewport[1]) {
            return;
        }
        scaled_space(&s);
        draw_circle_or_stadium(&s, to_screen(x, 0), to_screen(y, 1), rx / host_scale, ry / host_scale, filled,
                               0);
    } else {
        logical_space(&s);
        draw_circle_or_stadium(&s, x, y, rx, ry, filled, 0);
    }
}

/// Entry 17 (VGA640 0x0836): quarter arc of radius r and width w around c, from one end
/// to the other; the pen moves to c unless the arc is outside the viewport. With
/// SetScale above 1 the radius, the width and the distances to the ends are divided by
/// the scale (at least 1).
COLD static void draw_pixel_arc(const i32 c[2], const i32 e[4], i32 r, i32 w)
{
    if (imin(e[0], e[2]) > viewport[2] + w || imax(e[0], e[2]) < viewport[0] - w
        || imin(e[1], e[3]) > viewport[3] + w || imax(e[1], e[3]) < viewport[1] - w) {
        return;
    }
    pen[0] = c[0];
    pen[1] = c[1];
    struct space s;
    if (host_scale > 1) {
        i32 k = host_scale;
        i32 sc[2] = {to_screen(c[0], 0), to_screen(c[1], 1)};
        i32 se[4];
        for (int i = 0; i < 4; i++) {
            i32 d = e[i] - c[i & 1];
            i32 m = imax(iabs(d) / k, 1);
            se[i] = sc[i & 1] + (d == 0 ? 0 : (d < 0 ? -m : m));
        }
        scaled_space(&s);
        pixel_quarter_arc(&s, sc, se, imax(r / k, 1), imax(w / k, 1), 0);
    } else {
        logical_space(&s);
        pixel_quarter_arc(&s, c, e, r, w, 0);
    }
}

static void run_entry(void)
{
    i32 r[4];
    i32 n;
    switch (g_entry) {
    case 0:
        entry_allocate();
        if (opened) {
            return;
        }
        // Without the font nothing readable can be drawn: fail as without VESA.
        if (!load_font()) {
            report_missing_font();
            ret_ax = 1;
            return;
        }
        if (!open_screen()) {
            ret_ax = 1;
            return;
        }
        opened = 1;
        set_timer_mode(2);
        hook_keyboard();
        reset_state();
        ret_ax = 0;
        break;
    case 1:
        if (opened) {
            opened = 0;
            unhook_keyboard();
            set_timer_mode(3);
            struct regs t = {0};
            t.eax = 3;
            int86(&t);
        }
        /* fall through */
    case 3:
        if (block_allocated) {
            dos_free(block_segment);
            block_allocated = 0;
        }
        break;
    case 2:
        entry_allocate();
        break;
    case 4:
        ret_ax = 0;
        ret_dx = block_segment;
        break;
    case 5:
        draw_driver_name();
        break;
    case 6:
        // VGA640.DRV answers 639x479 even before it is opened; ESP asks then and lays
        // its screens out for that size.
        if (!opened) {
            pick_mode();
        }
        poke16((u16)args[3], (u16)args[2], (u16)(width - 1));
        poke16((u16)args[1], (u16)args[0], (u16)(height - 1));
        break;
    case 7:
        if (flag42 == 1) {
            ret_ax = 0xff;
            flag42 = 0;
        } else {
            ret_ax = (u16)colour_number;
        }
        break;
    case 8:
        // VGA640 reads only the low byte (mov bl, [bp+6]).
        set_colour(args[0] & 0xff);
        break;
    case 9:
        ret_ax = (u16)host_scale;
        break;
    case 10:
        host_scale = imax(imin(arg(6) & 0xffff, 255), 1);
        break;
    case 11:
        rectangle_args(viewport);
        break;
    case 12:
        origin[0] = arg(8);
        origin[1] = arg(6);
        break;
    case 13:
        set_cursor(arg(0xa) & 0xff, arg(8), arg(6));
        break;
    case 14:
        set_cursor_style(arg(6) & 0xff);
        break;
    case 15:
        begin_screen_drawing();
        break;
    case 16:
        end_screen_drawing();
        break;
    case 18:
        draw_wide_arc(arg32(0x24), arg32(0x20), arg32(0x1c), arg32(0x18), arg32(0x14),
                      arg32(0x10), arg32(0xc), arg32(8), arg(6) & 0xff);
        break;
    case 19:
        pen[0] = arg(0xc);
        pen[1] = arg(0xa);
        wide_line_relative(arg(8) - arg(0xc), arg(6) - arg(0xa));
        break;
    case 20:
        wide_line_relative(arg(8), arg(6));
        break;
    case 17:
        {
        i32 c[2] = {arg(0x14), arg(0x12)};
        i32 e[4] = {arg(0x10), arg(0xe), arg(0xc), arg(0xa)};
        draw_pixel_arc(c, e, arg(8), arg(6));
        }
        break;
    case 21:
        draw_circle_entry(arg(0xe), arg(0xc), arg(0xa), arg(8), (arg(6) & 0xff) != 0);
        break;
    case 22:
        draw_circle_entry(pen[0] + arg(0xe), pen[1] + arg(0xc), arg(0xa), arg(8), (arg(6) & 0xff) != 0);
        break;
    case 24:
        draw_line(pen[0], pen[1], pen[0] + arg(8), pen[1] + arg(6), 1);
        pen[0] += arg(8);
        pen[1] += arg(6);
        break;
    case 23:
    case 28:
        draw_line(arg(0xc), arg(0xa), arg(8), arg(6), g_entry == 23);
        pen[0] = arg(8);
        pen[1] = arg(6);
        break;
    case 27:
        window[0] = arg32(0x16);
        window[1] = arg32(0x12);
        window[2] = arg32(0xe);
        window[3] = arg32(0xa);
        zoom = imax(arg32(6), 1);
        break;
    case 29:
        relative_line(arg(8), arg(6));
        break;
    case 30:
        pen[0] = arg(8);
        pen[1] = arg(6);
        break;
    case 31:
        pen[0] += arg(8);
        pen[1] += arg(6);
        break;
    case 32:
        pen[0] += arg(0xe);
        pen[1] += arg(0xc);
        draw_bitmap(arg(0x12), arg(0x10), args[(0x16 - 6) / 2], args[(0x14 - 6) / 2], arg(0xa), arg(8), arg(6) & 0xff);
        break;
    case 33:
    case 34:
        n = fetch_c_string(args[(0xc - 6) / 2], args[(0xa - 6) / 2], arg(0xe));
        if (g_entry == 33) {
            draw_text_at(arg(8), arg(6), text_buffer, n);
        } else {
            draw_text_relative(arg(8), arg(6), text_buffer, n);
        }
        break;
    case 35:
    case 36:
        n = fetch_c_string(args[(0xe - 6) / 2], args[(0xc - 6) / 2], arg(0x10));
        if (g_entry == 35) {
            draw_text_at(arg(0xa), arg(8), text_buffer, n);
        } else {
            draw_text_relative(arg(0xa), arg(8), text_buffer, n);
        }
        break;
    case 37:
    case 38:
        n = fetch_pascal_string(args[(0xc - 6) / 2], args[(0xa - 6) / 2]);
        if (g_entry == 37) {
            draw_text_at(arg(8), arg(6), text_buffer, n);
        } else {
            draw_text_relative(arg(8), arg(6), text_buffer, n);
        }
        break;
    case 39:
        n = fetch_pascal_string(args[(0xe - 6) / 2], args[(0xc - 6) / 2]);
        if (n == 0) {
            // VGA640 0x196a: an empty string only sets or clears flags by the byte after
            // its length; the pen stays (DRAFT's prompt line moves it, calls this, then
            // draws relative text).
            u8 mark = peek8(args[(0xe - 6) / 2], (u16)(args[(0xc - 6) / 2] + 1));
            if (mark == 0xff) {
                flag43 = 1;
            } else if (mark == 0xfe) {
                flag42 = 1;
            } else {
                flag42 = 0;
                flag43 = 0;
            }
        } else if (stroke_segment && (arg(6) != 1 || host_scale != 1 || (n == 1 && text_buffer[0] <= 0x20))) {
            // VGA640 0x1990: the stroke font (text scale 0 only moves the pen).
            pen[0] = arg(0xa);
            pen[1] = arg(8);
            draw_stroke_text(text_buffer, n, arg(6));
        } else if ((arg(6) & 0xff) == 1) {
            draw_text_at(arg(0xa), arg(8), text_buffer, n);
        } else {
            // Text scale 0 (hidden) or other: the original driver still moves the pen.
            pen[0] = arg(0xa);
            pen[1] = arg(8);
        }
        break;
    case 40:
        n = fetch_pascal_string(args[(0xe - 6) / 2], args[(0xc - 6) / 2]);
        if (stroke_segment && (arg(6) != 1 || host_scale != 1 || (n == 1 && text_buffer[0] <= 0x20))) {
            // VGA640 0x1a24: the pen moves, then the stroke font draws from there.
            pen[0] += arg(0xa);
            pen[1] += arg(8);
            draw_stroke_text(text_buffer, n, arg(6));
        } else {
            if ((arg(6) & 0xff) == 1 && !(n == 1 && text_buffer[0] <= 0x20)) {
                draw_text_relative(arg(0xa), arg(8), text_buffer, n);
            } else {
                // The original driver always moves the pen, drawn or not.
                pen[0] += arg(0xa);
                pen[1] += arg(8);
            }
            flag42 = 0;
            flag43 = 0;
        }
        break;
    case 41:
        if ((args[0] & 0xff) == 0xff) {
            draw_grid(arg32(8), arg32(0xc));
        } else {
            draw_wide_line(arg32(0x18), arg32(0x14), arg32(0x10), arg32(0xc), arg32(8), arg(6) & 0xff);
        }
        break;
    case 42:
        draw_circle(arg32(0x10), arg32(0xc), arg32(8), arg(6) & 0xff);
        break;
    case 43:
        rectangle_args(r);
        fill_rectangle(r, 0, WRITE_REPLACE);
        break;
    case 44:
        rectangle_args(r);
        fill_rectangle(r, colour, write_mode);
        break;
    case 45:
        rectangle_args(r);
        fill_rectangle(r, COLOUR_TO_VGA[15], WRITE_XOR);
        break;
    case 46:
        set_highlight_mode();
        break;
    case 47:
        highlight = 0;
        write_mode = WRITE_OR;
        break;
    case 48:
        highlight = 0;
        write_mode = WRITE_XOR;
        break;
    case 49:
        rectangle_args(r);
        save_rectangle(r);
        break;
    case 50:
        restore_rectangle();
        break;
    case 51:
        entry_bitmap_buffer();
        break;
    default:
        break;
    }
}

#ifdef DOSCAD_TRACE
/// One traced driver call (16 bytes), appended in blocks to DOSCAD.TRC.
struct trace_record
{
    u16 entry;
    u16 a[3];
    u32 time;
    u32 done;
};
#define TRACE_RECORDS 200
static struct trace_record trace[TRACE_RECORDS];
static int trace_count;
static int trace_file_made;
static u32 trace_last;

/// Appends the records to DOSCAD.TRC (created at the first write) and empties the buffer.
static void trace_flush(void)
{
    static const char name[] = "C:\\DOSCAD.TRC";
    struct regs r = {0};
    r.intno = 0x21;
    r.edx = (u32)(u16)(u32)name;
    if (trace_file_made) {
        r.eax = 0x3d02;
    } else {
        r.eax = 0x3c00;
    }
    int86(&r);
    if (r.flags & 1) {
        trace_count = 0;
        return;
    }
    trace_file_made = 1;
    u16 handle = (u16)r.eax;
    struct regs m = {0};
    m.intno = 0x21;
    m.eax = 0x4202;
    m.ebx = handle;
    int86(&m);
    struct regs w = {0};
    w.intno = 0x21;
    w.eax = 0x4000;
    w.ebx = handle;
    w.ecx = (u32)trace_count * sizeof(struct trace_record);
    w.edx = (u32)(u16)(u32)trace;
    int86(&w);
    struct regs c = {0};
    c.intno = 0x21;
    c.eax = 0x3e00;
    c.ebx = handle;
    int86(&c);
    trace_count = 0;
}

static void trace_entry(void)
{
    u32 now = read_time();
    if (trace_count == TRACE_RECORDS || (trace_count > 0 && now - trace_last > PIT_HZ)) {
        trace_flush();
        now = read_time();
    }
    trace_last = now;
    struct trace_record *t = &trace[trace_count++];
    t->entry = g_entry;
    for (int i = 0; i < 3; i++) {
        t->a[i] = args[i];
    }
    t->time = now;
}
#endif

// ---- Frame rate overlay (DOSCADVFPS=1) --------------------------------------------

static int frame_running;
static u32 frame_start;
/// Frames per second of the last redraw, in tenths; 0 before the first one ends.
static u32 frame_tenths;

/// Times PCB386 redraws: one starts at a clear of at least half the screen height
/// (entry 43, the board area) and ends at the next cursor set (entry 13) to a real
/// position, which PCB386 makes when it waits for the user again. The cursor sets to
/// 0,0 right after the clear only hide the cursor while PCB386 draws (DOSCAD.TRC).
static void time_frame(void)
{
    if (g_entry == 43 && arg(6) - arg(0xa) >= height / 2) {
        frame_running = 1;
        frame_start = read_time();
        return;
    }
    if (g_entry == 13 && frame_running && (arg(6) != 0 || arg(8) != 0)) {
        frame_running = 0;
        u32 clocks = read_time() - frame_start;
        if (clocks == 0) {
            clocks = 1;
        }
        frame_tenths = (PIT_HZ * 10u + clocks / 2) / clocks;
    }
}

/// Writes "WxH | 256 | N fps" (one decimal below 10 fps) in white on black at the
/// bottom right of the screen, leaving the PCB386 drawing state as it was.
COLD static void draw_fps_overlay(void)
{
    char text[40];
    char *p = text;
    append_number(&p, phys_w);
    *p++ = 'x';
    append_number(&p, phys_h);
    append_text(&p, " | 256 | ");
    append_number(&p, frame_tenths / 10);
    if (frame_tenths < 100) {
        *p++ = '.';
        append_number(&p, frame_tenths % 10);
    }
    append_text(&p, " fps");
    *p = 0;
    i32 len = (i32)(p - text);
    // Whole screen as the viewport, origin at the top left.
    i32 saved_viewport[4];
    i32 saved_origin[2];
    for (int i = 0; i < 4; i++) {
        saved_viewport[i] = viewport[i];
    }
    saved_origin[0] = origin[0];
    saved_origin[1] = origin[1];
    u8 saved_colour = colour;
    u8 saved_mode = write_mode;
    i32 saved_scale = host_scale;
    host_scale = 1;
    viewport[0] = 0;
    viewport[1] = 0;
    viewport[2] = width - 1;
    viewport[3] = height - 1;
    origin[0] = 0;
    origin[1] = 0;
    // A black box one pixel wider than the text, then the glyphs (their bottom row is y).
    i32 box[4] = {width - 8 * len - 2, height - 10, width - 1, height - 1};
    fill_rectangle(box, 0, WRITE_REPLACE);
    colour = COLOUR_TO_VGA[15];
    write_mode = WRITE_REPLACE;
    draw_driver_text(width - 8 * len - 1, height - 2, text);
    for (int i = 0; i < 4; i++) {
        viewport[i] = saved_viewport[i];
    }
    origin[0] = saved_origin[0];
    origin[1] = saved_origin[1];
    colour = saved_colour;
    write_mode = saved_mode;
    host_scale = saved_scale;
}

/// Called by doscad.asm for every entry.
void dispatch(void)
{
#ifdef DOSCAD_TRACE
    trace_entry();
#endif
    // (the tests of hide_arrow and show_arrow, here: most calls need neither)
    if (opened && arrow_drawn) {
        hide_arrow();
    }
    keep_keyboard_hook();
    run_entry();
    if (opened && show_fps) {
        time_frame();
        // Redrawn at every cursor set: PCB386 may have drawn over it meanwhile.
        if (g_entry == 13 && frame_tenths != 0) {
            draw_fps_overlay();
        }
    }
    if (opened && cursor_visible) {
        show_arrow();
    }
#ifdef DOSCAD_TRACE
    trace[trace_count - 1].done = read_time();
#endif
}
