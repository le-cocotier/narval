/**
 * @file gui_sdl.c
 * @brief narval-gui's window: a ScreenDriver (terminal.h) on SDL3.
 * @details The editor doesn't know it is in a window. editorRefreshScreen()
 * writes its frames as it would to a terminal; they go through the
 * terminal panes' ANSI parser (ansi_parser.c) into s_screen, a buffer
 * whose rows are the window's grid, and the grid is drawn when the run
 * loop next waits. Keys and mouse clicks go the other way, as the bytes a
 * terminal sends (keyEscapeSequence(), SGR mouse reports), into a queue
 * the editor's key reader (utf8.c) reads through read_keys(). So every
 * command, key binding, prompt and theme is the terminal narval's.
 *
 * Text is drawn with SDL3_ttf, one texture per glyph and style, tinted to
 * each cell's colour; HiDPI screens get the font at their pixel scale.
 */

#include "../../include/gui.h"

#include "../../include/ansi_parser.h"
#include "../../include/buffer.h"
#include "../../include/builtins.h"
#include "../../include/data.h"
#include "../../include/define.h"
#include "../../include/editor_op.h"
#include "../../include/galloc.h"
#include "../../include/lisp.h"
#include "../../include/os.h"
#include "../../include/split_screen.h"
#include "../../include/subprocess.h"
#include "../../include/terminal.h"
#include "../../include/utf8.h"
#include "../../include/utils.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

#define GUI_DEFAULT_FONT_SIZE 14
#define GUI_START_COLS 110
#define GUI_START_ROWS 34
#define GUI_PADDING 4          // logical pixels around the grid
#define GUI_MIN_FRAME_MS 12    // while keys are queued (a paste), at most ~80 frames a second
#define GUI_STREAM_SLICE_MS 8  // how often other streams are polled while waiting on the window
#define GUI_FONT_STYLES 4      // regular, bold, italic, bold italic
#define GUI_MAX_DROPPED 64     // files opened at once (dropped, or from Finder's Open With)

typedef struct
{
    Uint32 codepoint;  // 0: empty slot
    int style;
    SDL_Texture *texture;  // NULL: the font has nothing to draw for it
    int w;
    int h;
} Glyph;

typedef struct
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *fonts[GUI_FONT_STYLES];
    char *font_path;     // galloc'd: what fonts[] were opened from
    float font_size;     // points, as asked (GUI-FONT-SIZE)
    float scale;         // pixels per logical pixel
    int cell_w;          // pixels
    int cell_h;
    int ascent;
    int pad;             // pixels
    int rows;
    int cols;

    struct buffer_t screen;  // the grid: rows written by ansi_parser.c
    int dirty;
    Uint64 last_present;

    // Bytes for the key reader, as a terminal would send them.
    char *keys;
    size_t keys_len;
    size_t keys_cap;
    size_t keys_pos;
    // A whole key press was read: the next read with nothing queued says
    // "nothing" at once instead of waiting, so a lone ESC is ESC at once.
    int key_boundary;
    // A key press already sent as a key: the text SDL types for it next
    // (Option+x's "≈") isn't typed too.
    int drop_text;
    float wheel;  // trackpad scrolling, accumulated to whole notches

    Glyph *glyphs;
    int glyph_cap;
    int glyph_count;

    // Files dropped on the window or opened with it from the Finder,
    // opened once the run loop is back at its wait (galloc'd paths).
    char *dropped[GUI_MAX_DROPPED];
    int n_dropped;
    char *cwd;  // E.cwd when started from a launcher in "/": the home folder

    int focused;
    int open;
} Gui;

static Gui G;

/***********************************************************************************************************************
 *
 *                                                 Keys
 *
 **********************************************************************************************************************/

static void queueBytes(const char *bytes, const size_t n)
{
    char *grown;
    size_t cap;

    if (n == 0)
        return;
    if (G.keys_pos == G.keys_len)
        G.keys_pos = G.keys_len = 0;
    if (G.keys_len + n > G.keys_cap)
    {
        cap = G.keys_cap ? G.keys_cap : 256;
        while (cap < G.keys_len + n)
            cap *= 2;
        grown = grealloc(G.keys, cap);
        if (!grown)
            return;
        G.keys = grown;
        G.keys_cap = cap;
    }
    memcpy(G.keys + G.keys_len, bytes, n);
    G.keys_len += n;
}

/**
 * @brief The narval key code for a key that types nothing (Enter, arrows,
 * F5, ...), or 0 for one that types a character.
 */
static unsigned char specialKey(const unsigned int keycode)
{
    switch (keycode)
    {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return '\r';
    case SDLK_TAB:       return '\t';
    case SDLK_BACKSPACE: return BACKSPACE;
    case SDLK_ESCAPE:    return '\x1b';
    case SDLK_DELETE:    return DEL_KEY;
    case SDLK_HOME:      return BEG_LINE;
    case SDLK_END:       return END_LINE;
    case SDLK_PAGEUP:    return PAGE_UP;
    case SDLK_PAGEDOWN:  return PAGE_DOWN;
    case SDLK_UP:        return ARROW_UP;
    case SDLK_DOWN:      return ARROW_DOWN;
    case SDLK_LEFT:      return ARROW_LEFT;
    case SDLK_RIGHT:     return ARROW_RIGHT;
    default:
        break;
    }
    if (keycode >= SDLK_F1 && keycode <= SDLK_F12)
        return (unsigned char) (FUNCTION_KEY + (keycode - SDLK_F1));
    return 0;
}

int guiKeyBytes(const unsigned int keycode, const unsigned int shifted, int mods, char *out,
                const size_t size)
{
    unsigned char base = specialKey(keycode);
    Utf8Char key;

    if (!base)
    {
        // A character key types its character (SDL's text input) unless
        // Ctrl or Alt is held: then it is a key, Ctrl+x, Alt+x. And
        // Shift+Space, SHIFT-SPACE (typing a space when it isn't bound).
        if (keycode == ' ' && mods == KEY_MOD_SHIFT)
            return snprintf(out, size, "\x1b[27;2;32~");
        if (!(mods & (KEY_MOD_CTRL | KEY_MOD_ALT)) || keycode < ' ' || keycode >= 0x7F)
            return 0;
        base = (unsigned char) keycode;
        if (base >= 'A' && base <= 'Z')
            base = (unsigned char) (base | 0x20);
        // Not a letter: Shift is in the character (Ctrl+Shift+1 is Ctrl+!),
        // except on Space (CTRL-SHIFT-SPACE).
        if (!(base >= 'a' && base <= 'z') && base != ' ' && (mods & KEY_MOD_SHIFT))
        {
            if (shifted >= ' ' && shifted < 0x7F)
                base = (unsigned char) shifted;
            mods &= ~KEY_MOD_SHIFT;
        }
    }
    keyMake(&key, mods, base);
    // A key with modifiers no control byte carries (Alt+[, Ctrl+Alt+i,
    // Shift+Space, Alt+Enter): as xterm's modifyOtherKeys reports it,
    // which says exactly which key it was. ESC + the key, a terminal's
    // way for Alt, reads as something else when the key is "[" or "O"
    // (the start of an escape sequence), ESC, TAB or ENTER.
    if (IS_MOD_KEY(&key) && key.character[2] < 0x80)
        return snprintf(out, size, "\x1b[27;%d;%d~", 1 + key.character[1], key.character[2]);
    return keyEscapeSequence(&key, out, size);
}

// xterm's SGR mouse report for a press or release at a pixel position.
static void queueMouse(const int button, const float x, const float y, const int press)
{
    char report[48];
    int col;
    int row;
    int n;

    if (!E.constantes.MOUSE || G.cell_w <= 0 || G.cell_h <= 0)
        return;
    col = (int) ((x * G.scale - (float) G.pad) / (float) G.cell_w);
    row = (int) ((y * G.scale - (float) G.pad) / (float) G.cell_h);
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col >= G.cols) col = G.cols - 1;
    if (row >= G.rows) row = G.rows - 1;
    n = snprintf(report, sizeof(report), "\x1b[<%d;%d;%d%c", button, col + 1, row + 1,
                 press ? 'M' : 'm');
    queueBytes(report, (size_t) n);
}

// Cmd/Super+V: the clipboard typed in, as a terminal pastes.
static void pasteClipboard(void)
{
    char *text = SDL_GetClipboardText();
    char *out;
    size_t i;
    size_t n = 0;

    if (!text)
        return;
    out = galloc(strlen(text) + 1);
    if (out)
    {
        for (i = 0; text[i]; i++)
        {
            if (text[i] == '\r' && text[i + 1] == '\n')
                continue;
            out[n++] = text[i] == '\n' ? '\r' : text[i];
        }
        queueBytes(out, n);
        gfree(out);
    }
    SDL_free(text);
}

// The window's close button: what CTRL-x CTRL-c does (editorQuit()), with
// its warning about unsaved changes. Pressed again, it quits.
static void requestQuit(void)
{
    LispError error = LISP_ERROR_NONE;
    editorQuit(lisp_null(), &error, E.ctx);
}

static void handleKeyDown(const SDL_KeyboardEvent *k)
{
    const unsigned int plain = SDL_GetKeyFromScancode(k->scancode, SDL_KMOD_NONE, true);
    // Shift applied as typing applies it (Shift+1 is "!"): key events
    // otherwise get the unshifted key.
    const unsigned int shifted = SDL_GetKeyFromScancode(k->scancode, SDL_KMOD_SHIFT, false);
    unsigned int keycode = plain;
    char bytes[32];
    int mods = 0;
    int n;

    G.drop_text = 0;
    // Cmd (macOS) / Super: the window's own shortcuts, never the editor's.
    if (k->mod & SDL_KMOD_GUI)
    {
        if (plain == SDLK_V)
            pasteClipboard();
        else if (plain == SDLK_Q)
            requestQuit();
        G.drop_text = 1;
        return;
    }
    if (!plain || specialKey(k->key))
        keycode = k->key;
    if (k->mod & SDL_KMOD_SHIFT)
        mods |= KEY_MOD_SHIFT;
    if (k->mod & SDL_KMOD_CTRL)
        mods |= KEY_MOD_CTRL;
    // Left Alt (Option) is Meta; right Alt types characters (AltGr, and
    // Option+e for é on a Mac). AltGr also holds Ctrl on Windows.
    if (k->mod & SDL_KMOD_LALT)
        mods |= KEY_MOD_ALT;
    if (k->mod & SDL_KMOD_RALT)
        mods &= ~KEY_MOD_CTRL;
    n = guiKeyBytes(keycode, shifted, mods, bytes, sizeof(bytes));
    if (n > 0)
    {
        queueBytes(bytes, (size_t) n);
        G.drop_text = 1;
    }
}

static void handleText(const char *text)
{
    if (G.drop_text)
    {
        G.drop_text = 0;
        return;
    }
    queueBytes(text, strlen(text));
}

/***********************************************************************************************************************
 *
 *                                                 Font and grid
 *
 **********************************************************************************************************************/

static void glyphCacheClear(void)
{
    int i;
    for (i = 0; i < G.glyph_cap; i++)
        if (G.glyphs[i].texture)
            SDL_DestroyTexture(G.glyphs[i].texture);
    gfree(G.glyphs);
    G.glyphs = NULL;
    G.glyph_cap = 0;
    G.glyph_count = 0;
}

static void fontsClose(void)
{
    int i;
    glyphCacheClear();
    for (i = 0; i < GUI_FONT_STYLES; i++)
    {
        if (G.fonts[i])
            TTF_CloseFont(G.fonts[i]);
        G.fonts[i] = NULL;
    }
}

/**
 * @brief Opens `path` at G.font_size for the window's pixel scale, in the
 * four styles, and measures the cell. @return 0, -1 if it can't be opened
 * or isn't usable (no "M").
 */
static int fontsOpen(const char *path)
{
    static const int styles[GUI_FONT_STYLES] = {
        TTF_STYLE_NORMAL, TTF_STYLE_BOLD, TTF_STYLE_ITALIC, TTF_STYLE_BOLD | TTF_STYLE_ITALIC};
    TTF_Font *fonts[GUI_FONT_STYLES] = {NULL, NULL, NULL, NULL};
    const float px = G.font_size * G.scale;
    int advance = 0;
    int minx;
    int maxx;
    int miny;
    int maxy;
    int i;

    if (!path || !path[0])
        return -1;
    for (i = 0; i < GUI_FONT_STYLES; i++)
    {
        fonts[i] = TTF_OpenFont(path, px);
        if (!fonts[i])
            break;
        TTF_SetFontStyle(fonts[i], styles[i]);
        TTF_SetFontHinting(fonts[i], TTF_HINTING_LIGHT_SUBPIXEL);
    }
    if (i < GUI_FONT_STYLES || !TTF_GetGlyphMetrics(fonts[0], 'M', &minx, &maxx, &miny, &maxy, &advance) ||
        advance <= 0)
    {
        for (i = 0; i < GUI_FONT_STYLES; i++)
            if (fonts[i])
                TTF_CloseFont(fonts[i]);
        return -1;
    }
    fontsClose();
    memcpy(G.fonts, fonts, sizeof(fonts));
    G.cell_w = advance;
    G.cell_h = TTF_GetFontLineSkip(fonts[0]);
    if (G.cell_h < TTF_GetFontHeight(fonts[0]))
        G.cell_h = TTF_GetFontHeight(fonts[0]);
    G.ascent = TTF_GetFontAscent(fonts[0]);
    if (G.font_path != path)
    {
        gfree(G.font_path);
        G.font_path = gstrdup(path);
    }
    G.dirty = 1;
    return 0;
}

// `dir`/assets/fonts/DejaVuSansMono.ttf, if `dir` is set.
static int fontsOpenBundled(const char *dir)
{
    char path[OS_PATH_MAX];
    if (!dir)
        return -1;
    snprintf(path, sizeof(path), "%s/assets/fonts/DejaVuSansMono.ttf", dir);
    return fontsOpen(path);
}

/**
 * @brief The first font that opens: `first`, the bundled one (in
 * ~/.narval, then where narval is installed), the system's monospace one.
 */
static int fontsFind(const char *first)
{
    static const char *const system_fonts[] = {
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Monaco.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
    };
    static const char *const windows_fonts[] = {"consola.ttf", "cour.ttf"};
    const char *windir = getenv("WINDIR");
    char path[OS_PATH_MAX];
    size_t i;

    if (first && fontsOpen(first) == 0)
        return 0;
    if (fontsOpenBundled(utilNarvalDir()) == 0 || fontsOpenBundled(utilDataDir()) == 0)
        return 0;
    for (i = 0; i < sizeof(system_fonts) / sizeof(system_fonts[0]); i++)
        if (fontsOpen(system_fonts[i]) == 0)
            return 0;
    for (i = 0; windir && i < sizeof(windows_fonts) / sizeof(windows_fonts[0]); i++)
    {
        snprintf(path, sizeof(path), "%s/Fonts/%s", windir, windows_fonts[i]);
        if (fontsOpen(path) == 0)
            return 0;
    }
    return -1;
}

// Rows and columns that fit the window now; the parser wraps and clamps
// to them, as a terminal of that size would.
static void gridMeasure(void)
{
    int w = 0;
    int h = 0;

    if (!G.window || G.cell_w <= 0 || G.cell_h <= 0)
        return;
    SDL_GetWindowSizeInPixels(G.window, &w, &h);
    G.cols = (w - 2 * G.pad) / G.cell_w;
    G.rows = (h - 2 * G.pad) / G.cell_h;
    if (G.cols < 10) G.cols = 10;
    if (G.rows < 4) G.rows = 4;
    G.screen.term_state.pane_width = G.cols;
    G.screen.term_state.pane_height = G.rows;
}

// The window's scale changed (moved to another screen): same font, its
// size in the new pixels, same number of rows and columns if they fit.
static void scaleChanged(void)
{
    const float scale = SDL_GetWindowDisplayScale(G.window);
    char *path;

    if (scale <= 0.0f || scale == G.scale)
        return;
    G.scale = scale;
    G.pad = (int) (GUI_PADDING * G.scale + 0.5f);
    path = G.font_path ? gstrdup(G.font_path) : NULL;
    if (fontsFind(path) != 0)
        fprintf(stderr, "narval-gui: no font for the new display scale\n");
    gfree(path);
    gridMeasure();
}

/***********************************************************************************************************************
 *
 *                                                 Drawing
 *
 **********************************************************************************************************************/

// xterm's 256 colours: 16 named, a 6x6x6 cube, 24 greys.
static Uint32 paletteColor(const int index)
{
    static const Uint32 named[16] = {
        0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
        0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
    };
    static const int levels[6] = {0, 95, 135, 175, 215, 255};
    int grey;

    if (index < 16)
        return named[index];
    if (index < 232)
        return (Uint32) (levels[(index - 16) / 36] << 16 | levels[(index - 16) / 6 % 6] << 8 |
                         levels[(index - 16) % 6]);
    grey = 8 + (index - 232) * 10;
    return (Uint32) (grey << 16 | grey << 8 | grey);
}

// A cell colour (TermCellAttr's encoding), `fallback` for the default.
static Uint32 cellColor(const unsigned int color, const Uint32 fallback)
{
    if (color == 0)
        return fallback;
    if (color & TERM_COLOR_RGB)
        return color & 0xFFFFFF;
    return paletteColor((int) (color - 1) & 0xFF);
}

static Uint32 themeColor(const char *ansi, const Uint32 fallback)
{
    int r;
    int g;
    int b;
    if (!ansi || !strstr(ansi, ";2;"))
        return fallback;
    utilThemeRgb(ansi, &r, &g, &b);
    return (Uint32) ((r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF));
}

static Uint32 blend(const Uint32 a, const Uint32 b)
{
    return (((a >> 16 & 0xFF) + (b >> 16 & 0xFF)) / 2) << 16 |
           (((a >> 8 & 0xFF) + (b >> 8 & 0xFF)) / 2) << 8 | ((a & 0xFF) + (b & 0xFF)) / 2;
}

static void setColor(const Uint32 rgb)
{
    SDL_SetRenderDrawColor(G.renderer, (Uint8) (rgb >> 16), (Uint8) (rgb >> 8), (Uint8) rgb, 255);
}

static void fillCells(const int row, const int col, const int width, const Uint32 rgb)
{
    SDL_FRect r;
    r.x = (float) (G.pad + col * G.cell_w);
    r.y = (float) (G.pad + row * G.cell_h);
    r.w = (float) (width * G.cell_w);
    r.h = (float) G.cell_h;
    setColor(rgb);
    SDL_RenderFillRect(G.renderer, &r);
}

static unsigned int glyphHash(const Uint32 codepoint, const int style)
{
    return (codepoint * 2654435761u) ^ (unsigned int) style;
}

static int glyphCacheGrow(void)
{
    Glyph *old = G.glyphs;
    const int old_cap = G.glyph_cap;
    const int cap = old_cap ? old_cap * 2 : 1024;
    Glyph *glyphs = gcalloc((size_t) cap, sizeof(Glyph));
    unsigned int slot;
    int i;

    if (!glyphs)
        return -1;
    for (i = 0; i < old_cap; i++)
    {
        if (!old[i].codepoint)
            continue;
        slot = glyphHash(old[i].codepoint, old[i].style) & (unsigned int) (cap - 1);
        while (glyphs[slot].codepoint)
            slot = (slot + 1) & (unsigned int) (cap - 1);
        glyphs[slot] = old[i];
    }
    gfree(old);
    G.glyphs = glyphs;
    G.glyph_cap = cap;
    return 0;
}

// The texture for a character in a style, rendered white the first time.
static const Glyph *glyphGet(const Uint32 codepoint, const int style)
{
    static const SDL_Color white = {255, 255, 255, 255};
    unsigned int slot;
    SDL_Surface *surface;
    Glyph *glyph;

    if ((G.glyph_count + 1) * 10 > G.glyph_cap * 7 && glyphCacheGrow() != 0)
        return NULL;
    slot = glyphHash(codepoint, style) & (unsigned int) (G.glyph_cap - 1);
    while (G.glyphs[slot].codepoint)
    {
        if (G.glyphs[slot].codepoint == codepoint && G.glyphs[slot].style == style)
            return &G.glyphs[slot];
        slot = (slot + 1) & (unsigned int) (G.glyph_cap - 1);
    }
    glyph = &G.glyphs[slot];
    glyph->codepoint = codepoint;
    glyph->style = style;
    G.glyph_count++;
    surface = TTF_RenderGlyph_Blended(G.fonts[style], codepoint, white);
    if (!surface)
        return glyph;
    glyph->texture = SDL_CreateTextureFromSurface(G.renderer, surface);
    glyph->w = surface->w;
    glyph->h = surface->h;
    SDL_DestroySurface(surface);
    if (glyph->texture)
        SDL_SetTextureBlendMode(glyph->texture, SDL_BLENDMODE_BLEND);
    return glyph;
}

static void drawGlyph(const Utf8Char *c, const int row, const int col, const int style,
                      const Uint32 rgb)
{
    const Uint32 codepoint = utf8CharCodepoint(c);
    const Glyph *glyph;
    SDL_FRect dst;

    if (codepoint <= ' ' || codepoint == 0x7F)
        return;
    glyph = glyphGet(codepoint, style);
    if (!glyph || !glyph->texture)
        return;
    dst.x = (float) (G.pad + col * G.cell_w);
    dst.y = (float) (G.pad + row * G.cell_h);
    dst.w = (float) glyph->w;
    dst.h = (float) glyph->h;
    SDL_SetTextureColorMod(glyph->texture, (Uint8) (rgb >> 16), (Uint8) (rgb >> 8), (Uint8) rgb);
    SDL_RenderTexture(G.renderer, glyph->texture, NULL, &dst);
}

/**
 * @brief Draws one row of the grid: backgrounds, then characters (a wide
 * one over two cells, a combining mark over the cell before it), then
 * underlines. Cells left at the default background are the clear's.
 */
static void drawRow(const Row *line, const int row, const Uint32 fg0, const Uint32 bg0)
{
    TermCellAttr attr;
    Uint32 fg;
    Uint32 bg;
    SDL_FRect underline;
    int col = 0;
    int last_col = 0;
    int width;
    int style;
    int i;

    for (i = 0; i < line->size && col < G.cols; i++)
    {
        width = utf8CharWidth(&line->characters[i]);
        attr = line->attrs ? line->attrs[i] : (TermCellAttr) {0};
        fg = cellColor(attr.fg, fg0);
        bg = cellColor(attr.bg, bg0);
        if (attr.reverse)
        {
            fg ^= bg;
            bg ^= fg;
            fg ^= bg;
        }
        if (attr.dim)
            fg = blend(fg, bg);
        style = (attr.bold ? 1 : 0) | (attr.italic ? 2 : 0);
        if (width == 0)
        {
            drawGlyph(&line->characters[i], row, last_col, style, fg);
            continue;
        }
        if (bg != bg0)
            fillCells(row, col, width, bg);
        drawGlyph(&line->characters[i], row, col, style, fg);
        if (attr.underline)
        {
            underline.x = (float) (G.pad + col * G.cell_w);
            underline.y = (float) (G.pad + row * G.cell_h + G.ascent + 1);
            underline.w = (float) (width * G.cell_w);
            underline.h = G.scale > 1.5f ? 2.0f : 1.0f;
            setColor(fg);
            SDL_RenderFillRect(G.renderer, &underline);
        }
        last_col = col;
        col += width;
    }
}

// The character at screen column `col` of `line`, its index (-1: past the
// end) and the column it starts at.
static int charAtColumn(const Row *line, const int col, int *start)
{
    int x = 0;
    int width;
    int i;

    for (i = 0; i < line->size; i++)
    {
        width = utf8CharWidth(&line->characters[i]);
        if (width > 0 && col < x + width)
        {
            *start = x;
            return i;
        }
        x += width;
    }
    *start = col;
    return -1;
}

// The cursor: a block in the text's colour, the character in the cell's
// colour over it; an outline while the window isn't focused.
static void drawCursor(const Uint32 fg0, const Uint32 bg0)
{
    const TermParserState *ts = &G.screen.term_state;
    const int row = ts->cursor_y - ts->screen_top_row;
    const Row *line;
    TermCellAttr attr = {0};
    Uint32 fg;
    Uint32 bg;
    SDL_FRect box;
    int index = -1;
    int start = ts->cursor_x;
    int width = 1;

    if (ts->cursor_hidden || row < 0 || row >= G.rows || ts->cursor_x >= G.cols)
        return;
    line = ts->cursor_y < G.screen.numrows ? &G.screen.row[ts->cursor_y] : NULL;
    if (line)
        index = charAtColumn(line, ts->cursor_x, &start);
    if (index >= 0)
    {
        width = utf8CharWidth(&line->characters[index]);
        if (line->attrs)
            attr = line->attrs[index];
    }
    fg = cellColor(attr.fg, fg0);
    bg = cellColor(attr.bg, bg0);
    if (attr.reverse)
    {
        fg ^= bg;
        bg ^= fg;
        fg ^= bg;
    }
    if (!G.focused)
    {
        box.x = (float) (G.pad + start * G.cell_w);
        box.y = (float) (G.pad + row * G.cell_h);
        box.w = (float) (width * G.cell_w);
        box.h = (float) G.cell_h;
        setColor(fg);
        SDL_RenderRect(G.renderer, &box);
        return;
    }
    fillCells(row, start, width, fg);
    if (index >= 0)
        drawGlyph(&line->characters[index], row, start,
                  (attr.bold ? 1 : 0) | (attr.italic ? 2 : 0), bg);
}

// Draws the grid into the renderer's back buffer (present() shows it).
static void render(void)
{
    const Uint32 fg0 = themeColor(E.theme.COLOR_DEFAULT, 0xabb2bf);
    const Uint32 bg0 = themeColor(E.theme.BACKGROUND_COLOR, 0x282c34);
    const TermParserState *ts = &G.screen.term_state;
    int row;

    if (!G.open)
        return;
    setColor(bg0);
    SDL_RenderClear(G.renderer);
    for (row = 0; row < G.rows; row++)
        if (ts->screen_top_row + row < G.screen.numrows)
            drawRow(&G.screen.row[ts->screen_top_row + row], row, fg0, bg0);
    drawCursor(fg0, bg0);
}

static void present(void)
{
    if (!G.open)
        return;
    render();
    SDL_RenderPresent(G.renderer);
    G.dirty = 0;
    G.last_present = SDL_GetTicks();
}

/***********************************************************************************************************************
 *
 *                                                 Events
 *
 **********************************************************************************************************************/

static void handleEvent(const SDL_Event *ev)
{
    switch (ev->type)
    {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        requestQuit();
        break;
    case SDL_EVENT_KEY_DOWN:
        handleKeyDown(&ev->key);
        break;
    case SDL_EVENT_KEY_UP:
        G.drop_text = 0;  // the key's text, if any, came before
        break;
    case SDL_EVENT_TEXT_INPUT:
        handleText(ev->text.text);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.button >= SDL_BUTTON_LEFT && ev->button.button <= SDL_BUTTON_RIGHT)
            queueMouse(ev->button.button - SDL_BUTTON_LEFT, ev->button.x, ev->button.y,
                       ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        G.wheel += ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev->wheel.y : ev->wheel.y;
        while (G.wheel >= 1.0f || G.wheel <= -1.0f)
        {
            queueMouse(G.wheel > 0 ? 64 : 65, ev->wheel.mouse_x, ev->wheel.mouse_y, 1);
            G.wheel += G.wheel > 0 ? -1.0f : 1.0f;
        }
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        scaleChanged();
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        gridMeasure();
        G.dirty = 1;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        G.focused = ev->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        G.dirty = 1;
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        G.dirty = 1;
        break;
    case SDL_EVENT_DROP_FILE:
        if (ev->drop.data && G.n_dropped < GUI_MAX_DROPPED)
            G.dropped[G.n_dropped++] = gstrdup(ev->drop.data);
        break;
    default:
        break;
    }
}

static void pumpEvents(void)
{
    SDL_Event ev;
    while (G.open && SDL_PollEvent(&ev))
        handleEvent(&ev);
}

static int keysQueued(void)
{
    return G.keys_pos < G.keys_len;
}

// Draws what changed, but not every frame of a paste being typed in.
static void presentIfDirty(void)
{
    if (G.dirty && (!keysQueued() || SDL_GetTicks() - G.last_present >= GUI_MIN_FRAME_MS))
        present();
}

/***********************************************************************************************************************
 *
 *                                                 The ScreenDriver
 *
 **********************************************************************************************************************/

static void guiWrite(const void *buf, const size_t n)
{
    if (!G.open || n == 0)
        return;
    ansiParserFeed(&G.screen, (const unsigned char *) buf, n);
    G.dirty = 1;
}

static long guiReadKeys(void *buf, const size_t n, const int timeout_ms)
{
    const Uint64 deadline = SDL_GetTicks() + (Uint64) (timeout_ms > 0 ? timeout_ms : 0);
    size_t take;
    Sint64 left;

    for (;;)
    {
        if (!G.open)
            return 0;
        pumpEvents();
        if (keysQueued())
        {
            take = G.keys_len - G.keys_pos;
            if (take > n)
                take = n;
            memcpy(buf, G.keys + G.keys_pos, take);
            G.keys_pos += take;
            G.key_boundary = !keysQueued();
            return (long) take;
        }
        // The rest of a key press is always queued with it: nothing more
        // is coming for this one.
        if (G.key_boundary)
        {
            G.key_boundary = 0;
            return 0;
        }
        presentIfDirty();
        left = timeout_ms < 0 ? -1 : (Sint64) (deadline - SDL_GetTicks());
        if (timeout_ms >= 0 && (left <= 0 || SDL_GetTicks() >= deadline))
            return 0;
        SDL_WaitEventTimeout(NULL, (Sint32) left);
    }
}

static int guiSize(int *rows, int *cols)
{
    *rows = G.rows;
    *cols = G.cols;
    return 0;
}

/**
 * @brief The run loop's wait. The keys are the window's events; the other
 * streams (terminal panes, language servers) are polled between them,
 * every few milliseconds while there are any.
 */
// Opens the files dropped since the last call, each in the active pane as
// editor-open-file does; the last one is left showing.
static void openDropped(void)
{
    Utf8Char *path;
    int i;

    for (i = 0; i < G.n_dropped; i++)
    {
        path = stringToUtf8((unsigned char *) G.dropped[i]);
        if (path)
            splitScreenGetActivePane()->buffer_id = bufferCreate(path, READ_AND_WRITE, FILE_BUFF);
        gfree(path);
        gfree(G.dropped[i]);
        G.dropped[i] = NULL;
    }
    G.n_dropped = 0;
}

static int guiWait(OsStream *const *streams, const int n, const int timeout_ms, int *ready)
{
    const Uint64 deadline = SDL_GetTicks() + (Uint64) (timeout_ms > 0 ? timeout_ms : 0);
    Sint64 left;
    int count;
    int i;

    for (;;)
    {
        for (i = 0; i < n; i++)
            ready[i] = 0;
        pumpEvents();
        if (!G.open)
            return 0;
        // Here no prompt or Lisp code is running: a buffer can be opened.
        if (G.n_dropped)
        {
            openDropped();
            return 0;  // the run loop redraws
        }
        G.key_boundary = 0;
        count = n > 1 ? os_wait(streams + 1, n - 1, 0, ready + 1) : 0;
        if (count < 0)
            count = 0;
        if (keysQueued())
        {
            ready[0] = 1;
            count++;
        }
        presentIfDirty();
        if (count > 0 || E.exit_requested)
            return count;
        left = (Sint64) (deadline - SDL_GetTicks());
        if (SDL_GetTicks() >= deadline)
            return 0;
        if (n > 1 && left > GUI_STREAM_SLICE_MS)
            left = GUI_STREAM_SLICE_MS;
        SDL_WaitEventTimeout(NULL, (Sint32) left);
    }
}

static void freeScreenRows(void)
{
    int i;
    ansiParserRelease(&G.screen);
    for (i = 0; i < G.screen.numrows; i++)
    {
        gfree(G.screen.row[i].characters);
        gfree(G.screen.row[i].attrs);
    }
    gfree(G.screen.row);
    G.screen.row = NULL;
    G.screen.numrows = 0;
}

// terminalRestore(): the window closes, and everything it held is freed.
static void guiClose(void)
{
    int i;

    if (!G.open && !G.window)
        return;
    G.open = 0;
    for (i = 0; i < G.n_dropped; i++)
        gfree(G.dropped[i]);
    G.n_dropped = 0;
    if (E.cwd == G.cwd)
        E.cwd = NULL;
    gfree(G.cwd);
    G.cwd = NULL;
    fontsClose();
    gfree(G.font_path);
    G.font_path = NULL;
    if (G.window)
        SDL_StopTextInput(G.window);
    if (G.renderer)
        SDL_DestroyRenderer(G.renderer);
    if (G.window)
        SDL_DestroyWindow(G.window);
    G.renderer = NULL;
    G.window = NULL;
    TTF_Quit();
    SDL_Quit();
    freeScreenRows();
    gfree(G.keys);
    G.keys = NULL;
    G.keys_len = G.keys_cap = G.keys_pos = 0;
}

// The window's icon (the taskbar's on Linux; macOS and Windows take the
// app's): assets/icons/narval.bmp in ~/.narval, else where narval is
// installed.
static void setWindowIcon(void)
{
    const char *dirs[2];
    char path[OS_PATH_MAX];
    SDL_Surface *icon = NULL;
    int i;

    dirs[0] = utilNarvalDir();
    dirs[1] = utilDataDir();
    for (i = 0; i < 2 && !icon; i++)
    {
        if (!dirs[i])
            continue;
        snprintf(path, sizeof(path), "%s/assets/icons/narval.bmp", dirs[i]);
        icon = SDL_LoadBMP(path);
    }
    if (!icon)
        return;
    SDL_SetWindowIcon(G.window, icon);
    SDL_DestroySurface(icon);
}

/**
 * @brief Started from the Dock, Spotlight or the Finder, a macOS app gets
 * launchd's PATH (/usr/bin:/bin:/usr/sbin:/sbin): no Homebrew, so no
 * language servers, git or compilers from it. This takes the PATH your
 * login shell sets up instead, as a terminal would have given it. Not
 * when started from a terminal (TERM is set): that PATH is already right.
 */
static void importLoginShellPath(void)
{
    static const char begin[] = "__NARVAL_PATH__";
    static const char end[] = "__NARVAL_END__";
    const char *shell = getenv("SHELL");
    char *argv[5];
    char *out;
    char *from;
    char *to;
    int status = 0;

    if (strcmp(SDL_GetPlatform(), "macOS") != 0 || getenv("TERM"))
        return;
    argv[0] = (char *) (shell && shell[0] ? shell : "/bin/zsh");
    argv[1] = "-ilc";  // login and interactive: .zprofile and .zshrc
    argv[2] = "printf '__NARVAL_PATH__%s__NARVAL_END__' \"$PATH\"";
    argv[3] = NULL;
    out = runArgvCaptureQuiet(argv, 5000, 64 * 1024, &status);
    from = out ? strstr(out, begin) : NULL;
    to = from ? strstr(from, end) : NULL;
    if (to && to > from + sizeof(begin) - 1)
    {
        *to = '\0';
        SDL_setenv_unsafe("PATH", from + sizeof(begin) - 1, 1);
    }
    gfree(out);
}

// Started from a launcher, the working directory is "/": the open-file
// prompt and new terminal panes start in the home folder instead.
static void startInHome(void)
{
    char *cwd = os_getcwd();

    if (cwd && strcmp(cwd, "/") == 0 && !E.cwd)
    {
        G.cwd = os_home_dir();
        E.cwd = G.cwd;
    }
    gfree(cwd);
}

static const ScreenDriver s_gui_driver = {guiWrite, guiReadKeys, guiSize, guiWait, guiClose};

int guiOpen(const char *font_path)
{
    memset(&G, 0, sizeof(G));
    G.font_size = GUI_DEFAULT_FONT_SIZE;
    SDL_SetAppMetadata("narval", NARVAL_VERSION, "dev.narval.editor");
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "narval-gui: can't open a window: %s\n", SDL_GetError());
        return -1;
    }
    if (!TTF_Init())
    {
        fprintf(stderr, "narval-gui: can't load fonts: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    G.window = SDL_CreateWindow("narval", 960, 640,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                    SDL_WINDOW_HIDDEN);
    G.renderer = G.window ? SDL_CreateRenderer(G.window, NULL) : NULL;
    if (!G.renderer)
    {
        fprintf(stderr, "narval-gui: can't open a window: %s\n", SDL_GetError());
        G.open = 1;
        guiClose();
        return -1;
    }
    G.open = 1;
    SDL_SetRenderVSync(G.renderer, 1);
    G.scale = SDL_GetWindowDisplayScale(G.window);
    if (G.scale <= 0.0f)
        G.scale = 1.0f;
    G.pad = (int) (GUI_PADDING * G.scale + 0.5f);
    if (fontsFind(font_path) != 0)
    {
        fprintf(stderr, "narval-gui: no monospace font found; set GUI-FONT in init.lisp\n");
        guiClose();
        return -1;
    }
    SDL_SetWindowSize(G.window,
                      (int) ((float) (GUI_START_COLS * G.cell_w + 2 * G.pad) / G.scale + 0.5f),
                      (int) ((float) (GUI_START_ROWS * G.cell_h + 2 * G.pad) / G.scale + 0.5f));
    SDL_SetWindowMinimumSize(G.window, 240, 120);
    setWindowIcon();
    importLoginShellPath();
    startInHome();
    gridMeasure();
    SDL_ShowWindow(G.window);
    SDL_StartTextInput(G.window);
    G.focused = 1;
    pumpEvents();
    gridMeasure();
    screenSetDriver(&s_gui_driver);
    return 0;
}

void guiConfigure(void)
{
    const float size = (float) (E.constantes.GUI_FONT_SIZE > 0 ? E.constantes.GUI_FONT_SIZE
                                                                : GUI_DEFAULT_FONT_SIZE);
    int present = 0;
    Lisp font;
    const char *wanted = NULL;
    char *path;

    if (!G.open)
        return;
    font = lisp_env_lookup(lisp_env(E.ctx), lisp_make_symbol("GUI-FONT", E.ctx), &present);
    if (present && lisp_type(font) == LISP_STRING && lisp_string(font)[0])
        wanted = lisp_string(font);
    if (size == G.font_size && (!wanted || (G.font_path && !strcmp(wanted, G.font_path))))
        return;
    G.font_size = size;
    if (wanted && fontsOpen(wanted) == 0)
    {
        gridMeasure();
        return;
    }
    if (wanted)
        editorSetStatusMessage("GUI-FONT: can't open %s", wanted);
    path = G.font_path ? gstrdup(G.font_path) : NULL;
    fontsFind(path);
    gfree(path);
    gridMeasure();
}

long guiCellPixel(const int row, const int col)
{
    SDL_Rect at;
    SDL_Surface *pixels;
    Uint8 r = 0;
    Uint8 g = 0;
    Uint8 b = 0;
    Uint8 a = 0;

    if (!G.open)
        return -1;
    render();
    at.x = G.pad + col * G.cell_w + G.cell_w / 2;
    at.y = G.pad + row * G.cell_h + G.cell_h / 2;
    at.w = 1;
    at.h = 1;
    pixels = SDL_RenderReadPixels(G.renderer, &at);
    if (!pixels)
        return -1;
    if (!SDL_ReadSurfacePixel(pixels, 0, 0, &r, &g, &b, &a))
    {
        SDL_DestroySurface(pixels);
        return -1;
    }
    SDL_DestroySurface(pixels);
    return (long) r << 16 | (long) g << 8 | (long) b;
}
