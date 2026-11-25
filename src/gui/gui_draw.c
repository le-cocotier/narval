/**
 * @file gui_draw.c
 * @brief See gui_draw.h: box drawing, block elements, rounded boxes and
 * shadows, drawn for narval-gui instead of taken from the font; and the
 * image helpers for characters from other fonts (emoji).
 * @details A font draws "│" or "▎" no taller than its own line, so with
 * any line spacing a pane divider is a dotted line and the git gutter a
 * column of dashes. Drawn here they fill the cell, and a line in one cell
 * meets the same line in the next pixel for pixel. Shapes are coverage
 * masks (one alpha byte per pixel); curves and diagonals are sampled 4 x 4
 * per pixel for smooth edges.
 */

#include "gui_draw.h"

#include "../../include/galloc.h"

#include <math.h>
#include <string.h>

#define SUB 4  // samples per pixel side, for curves and diagonals

typedef struct
{
    int w;
    int h;
    Uint8 *a;  // w * h coverage, 0-255
} Mask;

// Each box drawing character's four arms, two bits each from bit 0: up,
// right, down, left. 0 none, 1 light, 2 heavy, 3 double. Generated from
// the characters' Unicode names; 0 for the dashed lines, arcs and
// diagonals, which are drawn on their own.
static const Uint8 s_arms[128] = {
    0x44, 0x88, 0x11, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x18, 0x24, 0x28,
    0x50, 0x90, 0x60, 0xa0, 0x05, 0x09, 0x06, 0x0a, 0x41, 0x81, 0x42, 0x82, 0x15, 0x19, 0x16, 0x25,
    0x26, 0x1a, 0x29, 0x2a, 0x51, 0x91, 0x52, 0x61, 0x62, 0x92, 0xa1, 0xa2, 0x54, 0x94, 0x58, 0x98,
    0x64, 0xa4, 0x68, 0xa8, 0x45, 0x85, 0x49, 0x89, 0x46, 0x86, 0x4a, 0x8a, 0x55, 0x95, 0x59, 0x99,
    0x56, 0x65, 0x66, 0x96, 0x5a, 0xa5, 0x69, 0x9a, 0xa9, 0xa6, 0x6a, 0xaa, 0x00, 0x00, 0x00, 0x00,
    0xcc, 0x33, 0x1c, 0x34, 0x3c, 0xd0, 0x70, 0xf0, 0x0d, 0x07, 0x0f, 0xc1, 0x43, 0xc3, 0x1d, 0x37,
    0x3f, 0xd1, 0x73, 0xf3, 0xdc, 0x74, 0xfc, 0xcd, 0x47, 0xcf, 0xdd, 0x77, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x01, 0x04, 0x10, 0x80, 0x02, 0x08, 0x20, 0x48, 0x21, 0x84, 0x12,
};

static int imax(const int a, const int b)
{
    return a > b ? a : b;
}

// Covers [x0, x1) x [y0, y1) fully, clipped to the mask.
static void maskRect(Mask *m, int x0, int y0, int x1, int y1, const Uint8 alpha)
{
    int x;
    int y;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > m->w) x1 = m->w;
    if (y1 > m->h) y1 = m->h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            if (m->a[y * m->w + x] < alpha)
                m->a[y * m->w + x] = alpha;
}

/***********************************************************************************************************************
 *
 *                                                 Box drawing
 *
 **********************************************************************************************************************/

typedef struct
{
    int cx;  // the vertical line's first column, the horizontal's first row
    int cy;
    int t1;  // light, heavy: thickness in pixels
    int t2;
    int g;   // a double line's two strokes: centres this far either side
} Lines;

static Lines linesFor(const int w, const int h)
{
    Lines l;
    l.t1 = imax(1, w / 8);
    l.t2 = l.t1 * 2 + (l.t1 == 1);
    l.g = l.t1 + imax(1, l.t1 / 2);
    l.cx = w / 2;
    l.cy = h / 2;
    return l;
}

static int weight(const Lines *l, const int arm)
{
    return arm == 2 ? l->t2 : l->t1;
}

// A horizontal stroke `t` thick centred on row `y`, from x0 to x1.
static void hStroke(Mask *m, const int x0, const int x1, const int y, const int t)
{
    maskRect(m, x0, y - t / 2, x1, y - t / 2 + t, 255);
}

static void vStroke(Mask *m, const int y0, const int y1, const int x, const int t)
{
    maskRect(m, x - t / 2, y0, x - t / 2 + t, y1, 255);
}

/**
 * @brief The four arms of a box drawing character. A light or heavy arm
 * reaches past the centre by half the crossing arms' thickness, so corners
 * are square; a double arm's two strokes stop at the crossing double
 * line's near or far stroke, the way ╔ ╦ ╬ are drawn.
 */
static void drawArms(Mask *m, const unsigned int arms)
{
    const Lines l = linesFor(m->w, m->h);
    const int up = (int) (arms & 3);
    const int right = (int) (arms >> 2 & 3);
    const int down = (int) (arms >> 4 & 3);
    const int left = (int) (arms >> 6 & 3);
    const int vd = up == 3 || down == 3;
    const int hd = left == 3 || right == 3;
    const int tv = imax(up && up != 3 ? weight(&l, up) : 0, down && down != 3 ? weight(&l, down) : 0);
    const int th = imax(left && left != 3 ? weight(&l, left) : 0, right && right != 3 ? weight(&l, right) : 0);
    const int cx = l.cx;
    const int cy = l.cy;
    const int g = l.g;
    const int lo = l.t1 / 2;          // a light stroke's extent before its centre
    const int hi = l.t1 - l.t1 / 2;   // and from its centre on
    int a;
    int b;

    if (right == 3)
    {
        a = up == 3 ? cx + g - lo : down == 3 ? cx - g - lo : (up || down) ? cx - tv / 2 : cx;
        b = down == 3 ? cx + g - lo : up == 3 ? cx - g - lo : (up || down) ? cx - tv / 2 : cx;
        hStroke(m, a, m->w, cy - g, l.t1);
        hStroke(m, b, m->w, cy + g, l.t1);
    }
    else if (right)
    {
        a = vd ? (up == 3 && down == 3 ? (left ? cx : cx + g - lo) : cx - g - lo)
               : (tv ? cx - tv / 2 : cx);
        hStroke(m, a, m->w, cy, weight(&l, right));
    }
    if (left == 3)
    {
        a = up == 3 ? cx - g + hi : down == 3 ? cx + g + hi : (up || down) ? cx + tv - tv / 2 : cx;
        b = down == 3 ? cx - g + hi : up == 3 ? cx + g + hi : (up || down) ? cx + tv - tv / 2 : cx;
        hStroke(m, 0, a, cy - g, l.t1);
        hStroke(m, 0, b, cy + g, l.t1);
    }
    else if (left)
    {
        a = vd ? (up == 3 && down == 3 ? (right ? cx : cx - g + hi) : cx + g + hi)
               : (tv ? cx + tv - tv / 2 : cx);
        hStroke(m, 0, a, cy, weight(&l, left));
    }
    if (up == 3)
    {
        a = left == 3 ? cy - g + hi : right == 3 ? cy + g + hi : (left || right) ? cy + th - th / 2 : cy;
        b = right == 3 ? cy - g + hi : left == 3 ? cy + g + hi : (left || right) ? cy + th - th / 2 : cy;
        vStroke(m, 0, a, cx - g, l.t1);
        vStroke(m, 0, b, cx + g, l.t1);
    }
    else if (up)
    {
        a = hd ? (left == 3 && right == 3 ? (down ? cy : cy - g + hi) : cy + g + hi)
               : (th ? cy + th - th / 2 : cy);
        vStroke(m, 0, a, cx, weight(&l, up));
    }
    if (down == 3)
    {
        a = left == 3 ? cy + g - lo : right == 3 ? cy - g - lo : (left || right) ? cy - th / 2 : cy;
        b = right == 3 ? cy + g - lo : left == 3 ? cy - g - lo : (left || right) ? cy - th / 2 : cy;
        vStroke(m, a, m->h, cx - g, l.t1);
        vStroke(m, b, m->h, cx + g, l.t1);
    }
    else if (down)
    {
        a = hd ? (left == 3 && right == 3 ? (up ? cy : cy + g - lo) : cy - g - lo)
               : (th ? cy - th / 2 : cy);
        vStroke(m, a, m->h, cx, weight(&l, down));
    }
}

// ┄ ┆ ┈ ┊ ╌ ╎ and their heavy forms: `n` dashes, each centred in its
// share of the cell, so dashes keep their rhythm across cells.
static void drawDashes(Mask *m, const int vertical, const int heavy, const int n)
{
    const Lines l = linesFor(m->w, m->h);
    const int t = heavy ? l.t2 : l.t1;
    const int size = vertical ? m->h : m->w;
    int gap;
    int i;
    int a;
    int b;

    for (i = 0; i < n; i++)
    {
        a = i * size / n;
        b = (i + 1) * size / n;
        gap = imax(1, (b - a) / 3);
        a += gap / 2;
        b -= gap - gap / 2;
        if (vertical)
            vStroke(m, a, b, l.cx, t);
        else
            hStroke(m, a, b, l.cy, t);
    }
}

// Sets each pixel's coverage from how many of its SUB x SUB samples
// `inside` accepts.
static void maskSample(Mask *m, int (*inside)(const Mask *, const void *, float, float), const void *data)
{
    int x;
    int y;
    int i;
    int j;
    int n;
    Uint8 alpha;

    for (y = 0; y < m->h; y++)
        for (x = 0; x < m->w; x++)
        {
            n = 0;
            for (j = 0; j < SUB; j++)
                for (i = 0; i < SUB; i++)
                    n += inside(m, data, (float) x + ((float) i + 0.5f) / SUB,
                                (float) y + ((float) j + 0.5f) / SUB);
            alpha = (Uint8) (n * 255 / (SUB * SUB));
            if (m->a[y * m->w + x] < alpha)
                m->a[y * m->w + x] = alpha;
        }
}

// ╭ ╮ ╯ ╰: a quarter circle as wide as the cell allows, then straight on
// to the edges, joining the light lines either side.
typedef struct
{
    float fx;  // where the lines' centres are
    float fy;
    float r;
    float half;
    int dx;  // the horizontal arm's side: 1 right, -1 left
    int dy;  // the vertical arm's: 1 down, -1 up
} Arc;

static int insideArc(const Mask *m, const void *data, const float x, const float y)
{
    const Arc *arc = data;
    const float acx = arc->fx + (float) arc->dx * arc->r;
    const float acy = arc->fy + (float) arc->dy * arc->r;
    const float ox = (x - acx) * (float) arc->dx;  // <= 0: on the corner's side of the centre
    const float oy = (y - acy) * (float) arc->dy;
    float d;

    (void) m;
    if (oy >= 0.0f && fabsf(x - arc->fx) <= arc->half)
        return 1;  // the straight part of the vertical arm
    if (ox >= 0.0f && fabsf(y - arc->fy) <= arc->half)
        return 1;
    if (ox > 0.0f || oy > 0.0f)
        return 0;
    d = sqrtf(ox * ox + oy * oy);
    return fabsf(d - arc->r) <= arc->half;
}

void guiCellLines(const int w, const int h, float *x, float *y, float *thickness, float *radius)
{
    const Lines l = linesFor(w, h);
    *x = (float) (l.cx - l.t1 / 2) + (float) l.t1 / 2.0f;
    *y = (float) (l.cy - l.t1 / 2) + (float) l.t1 / 2.0f;
    *thickness = (float) l.t1;
    *radius = fminf(fminf(*x, (float) w - *x), fminf(*y, (float) h - *y));
}

static void drawArc(Mask *m, const int dx, const int dy)
{
    Arc arc;
    float thickness;

    guiCellLines(m->w, m->h, &arc.fx, &arc.fy, &thickness, &arc.r);
    arc.half = thickness / 2.0f;
    arc.dx = dx;
    arc.dy = dy;
    maskSample(m, insideArc, &arc);
}

// ╱ ╲ ╳: corner to corner.
static int insideDiagonal(const Mask *m, const void *data, const float x, const float y)
{
    const int which = *(const int *) data;  // 1 ╱, 2 ╲, 3 both
    const float w = (float) m->w;
    const float h = (float) m->h;
    const float len = sqrtf(w * w + h * h);
    const float half = (float) linesFor(m->w, m->h).t1 / 2.0f + 0.25f;

    if ((which & 1) && fabsf(h * x + w * y - w * h) / len <= half)
        return 1;
    return (which & 2) && fabsf(h * x - w * y) / len <= half;
}

/***********************************************************************************************************************
 *
 *                                                 Block elements
 *
 **********************************************************************************************************************/

// ▀ ▁-▇ █ ▉-▏ ▐ ░ ▒ ▓ ▔ ▕ and the quadrants ▖-▟.
static void drawBlock(Mask *m, const Uint32 codepoint)
{
    // Quadrants U+2596-U+259F: bit 0 upper left, 1 upper right, 2 lower
    // left, 3 lower right.
    static const Uint8 quadrants[10] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
    const int w = m->w;
    const int h = m->h;
    int q;

    if (codepoint == 0x2580)
        maskRect(m, 0, 0, w, h / 2, 255);
    else if (codepoint >= 0x2581 && codepoint <= 0x2588)
        maskRect(m, 0, h - (int) (h * (codepoint - 0x2580) + 4) / 8, w, h, 255);
    else if (codepoint >= 0x2589 && codepoint <= 0x258F)
        maskRect(m, 0, 0, (int) (w * (0x2590 - codepoint) + 4) / 8, h, 255);
    else if (codepoint == 0x2590)
        maskRect(m, w / 2, 0, w, h, 255);
    else if (codepoint >= 0x2591 && codepoint <= 0x2593)
        maskRect(m, 0, 0, w, h, (Uint8) (64 * (codepoint - 0x2590)));
    else if (codepoint == 0x2594)
        maskRect(m, 0, 0, w, imax(1, (h + 4) / 8), 255);
    else if (codepoint == 0x2595)
        maskRect(m, w - imax(1, (w + 4) / 8), 0, w, h, 255);
    else if (codepoint >= 0x2596 && codepoint <= 0x259F)
    {
        q = quadrants[codepoint - 0x2596];
        if (q & 1) maskRect(m, 0, 0, w / 2, h / 2, 255);
        if (q & 2) maskRect(m, w / 2, 0, w, h / 2, 255);
        if (q & 4) maskRect(m, 0, h / 2, w / 2, h, 255);
        if (q & 8) maskRect(m, w / 2, h / 2, w, h, 255);
    }
}

/***********************************************************************************************************************
 *
 *                                                 Images
 *
 **********************************************************************************************************************/

// The mask as a white image with its coverage as alpha (and frees it).
static SDL_Surface *maskSurface(Mask *m)
{
    SDL_Surface *surface = SDL_CreateSurface(m->w, m->h, SDL_PIXELFORMAT_ARGB8888);
    Uint32 *row;
    int x;
    int y;

    if (surface)
        for (y = 0; y < m->h; y++)
        {
            row = (Uint32 *) ((Uint8 *) surface->pixels + y * surface->pitch);
            for (x = 0; x < m->w; x++)
                row[x] = (Uint32) m->a[y * m->w + x] << 24 | 0xFFFFFFu;
        }
    gfree(m->a);
    m->a = NULL;
    return surface;
}

static int maskInit(Mask *m, const int w, const int h)
{
    m->w = w;
    m->h = h;
    m->a = w > 0 && h > 0 ? gcalloc((size_t) w * (size_t) h, 1) : NULL;
    return m->a ? 0 : -1;
}

int guiIsCellGlyph(const Uint32 codepoint)
{
    return codepoint >= 0x2500 && codepoint <= 0x259F;
}

SDL_Surface *guiCellGlyph(const Uint32 codepoint, const int w, const int h)
{
    Mask m;
    int which;

    if (!guiIsCellGlyph(codepoint) || maskInit(&m, w, h) != 0)
        return NULL;
    if (codepoint >= 0x2580)
        drawBlock(&m, codepoint);
    else if (s_arms[codepoint - 0x2500])
        drawArms(&m, s_arms[codepoint - 0x2500]);
    else if (codepoint >= 0x2504 && codepoint <= 0x250B)
        drawDashes(&m, codepoint & 2, codepoint & 1, codepoint < 0x2508 ? 3 : 4);
    else if (codepoint >= 0x254C && codepoint <= 0x254F)
        drawDashes(&m, codepoint & 2, codepoint & 1, 2);
    else if (codepoint == 0x256D)
        drawArc(&m, 1, 1);
    else if (codepoint == 0x256E)
        drawArc(&m, -1, 1);
    else if (codepoint == 0x256F)
        drawArc(&m, -1, -1);
    else if (codepoint == 0x2570)
        drawArc(&m, 1, -1);
    else if (codepoint >= 0x2571 && codepoint <= 0x2573)
    {
        which = (int) (codepoint - 0x2570);
        maskSample(&m, insideDiagonal, &which);
    }
    return maskSurface(&m);
}

// Coverage of a pixel by a disc of `r` around (cx, cy).
static Uint8 discCoverage(const int x, const int y, const float cx, const float cy, const float r)
{
    int i;
    int j;
    int n = 0;
    float dx;
    float dy;

    for (j = 0; j < SUB; j++)
        for (i = 0; i < SUB; i++)
        {
            dx = (float) x + ((float) i + 0.5f) / SUB - cx;
            dy = (float) y + ((float) j + 0.5f) / SUB - cy;
            n += dx * dx + dy * dy <= r * r;
        }
    return (Uint8) (n * 255 / (SUB * SUB));
}

SDL_Surface *guiRoundedRect(const int radius)
{
    const int size = 2 * radius + 1;
    const float c = (float) size / 2.0f;
    Mask m;
    int x;
    int y;
    float px;
    float py;

    if (radius < 0 || maskInit(&m, size, size) != 0)
        return NULL;
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
        {
            // Inside the square's cross, full; in a corner, the disc's.
            px = fabsf((float) x + 0.5f - c);
            py = fabsf((float) y + 0.5f - c);
            if (px <= 0.5f || py <= 0.5f)
                m.a[y * size + x] = 255;
            else
                m.a[y * size + x] = discCoverage(x, y, x < radius ? (float) radius : (float) radius + 1.0f,
                                                 y < radius ? (float) radius : (float) radius + 1.0f,
                                                 (float) radius);
        }
    return maskSurface(&m);
}

SDL_Surface *guiShadow(const int radius, const int blur)
{
    const int edge = radius + blur;
    const int size = 2 * edge + 1;
    Mask m;
    int x;
    int y;
    float dx;
    float dy;
    float d;
    float t;

    if (radius < 0 || blur <= 0 || maskInit(&m, size, size) != 0)
        return NULL;
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
        {
            // Distance outside a rounded box whose corner circle is
            // centred `edge` pixels in from each side, minus its radius.
            dx = fmaxf(fabsf((float) x + 0.5f - (float) size / 2.0f) - 0.5f, 0.0f);
            dy = fmaxf(fabsf((float) y + 0.5f - (float) size / 2.0f) - 0.5f, 0.0f);
            d = sqrtf(dx * dx + dy * dy) - (float) radius;
            t = d <= 0.0f ? 1.0f : 1.0f - d / (float) blur;
            if (t < 0.0f)
                t = 0.0f;
            // Smoothstep, then squared: a soft falloff, darkest near the box.
            t = t * t * (3.0f - 2.0f * t);
            m.a[y * size + x] = (Uint8) (t * t * 255.0f + 0.5f);
        }
    return maskSurface(&m);
}

/***********************************************************************************************************************
 *
 *                                                 Images
 *
 **********************************************************************************************************************/

int guiHasColor(SDL_Surface *image)
{
    SDL_Surface *rgba;
    const Uint8 *p;
    int found = 0;
    int x;
    int y;

    if (!image)
        return 0;
    rgba = SDL_ConvertSurface(image, SDL_PIXELFORMAT_RGBA32);
    if (!rgba)
        return 0;
    for (y = 0; y < rgba->h && !found; y++)
        for (x = 0; x < rgba->w && !found; x++)
        {
            p = (const Uint8 *) rgba->pixels + y * rgba->pitch + x * 4;
            found = p[3] > 0 && (p[0] != p[1] || p[1] != p[2]);
        }
    SDL_DestroySurface(rgba);
    return found;
}

SDL_Surface *guiShrink(SDL_Surface *image, const int w, const int h)
{
    SDL_Surface *in;
    SDL_Surface *out;
    const Uint8 *p;
    Uint8 *q;
    float sx;
    float sy;
    float x0;
    float y0;
    float wy;
    float weight;
    float alpha;
    float sum[4];  // colour times alpha, and alpha, times the area covered
    float area;
    int x;
    int y;
    int i;
    int j;
    int i1;
    int j1;
    int k;

    if (!image || w <= 0 || h <= 0)
        return NULL;
    in = SDL_ConvertSurface(image, SDL_PIXELFORMAT_RGBA32);
    out = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!in || !out)
    {
        SDL_DestroySurface(in);
        SDL_DestroySurface(out);
        return NULL;
    }
    sx = (float) in->w / (float) w;
    sy = (float) in->h / (float) h;
    for (y = 0; y < h; y++)
    {
        y0 = (float) y * sy;
        j1 = (int) ceilf(y0 + sy);
        if (j1 > in->h) j1 = in->h;
        for (x = 0; x < w; x++)
        {
            // Every source pixel under this one, weighted by how much of it
            // is: colours by alpha too, so transparent pixels add no colour.
            x0 = (float) x * sx;
            i1 = (int) ceilf(x0 + sx);
            if (i1 > in->w) i1 = in->w;
            memset(sum, 0, sizeof(sum));
            area = 0.0f;
            for (j = (int) y0; j < j1; j++)
            {
                wy = fminf(y0 + sy, (float) j + 1.0f) - fmaxf(y0, (float) j);
                for (i = (int) x0; i < i1; i++)
                {
                    weight = wy * (fminf(x0 + sx, (float) i + 1.0f) - fmaxf(x0, (float) i));
                    p = (const Uint8 *) in->pixels + j * in->pitch + i * 4;
                    alpha = (float) p[3] * weight;
                    for (k = 0; k < 3; k++)
                        sum[k] += (float) p[k] * alpha;
                    sum[3] += alpha;
                    area += weight;
                }
            }
            q = (Uint8 *) out->pixels + y * out->pitch + x * 4;
            for (k = 0; k < 3; k++)
                q[k] = sum[3] > 0.0f ? (Uint8) (sum[k] / sum[3] + 0.5f) : 0;
            q[3] = area > 0.0f ? (Uint8) (sum[3] / area + 0.5f) : 0;
        }
    }
    SDL_DestroySurface(in);
    return out;
}
