/**
 * @file test_gui.c
 * @brief narval-gui's window (src/gui/gui_sdl.c), with SDL's offscreen
 * video driver: key presses in as the keys narval reads, frames out as
 * the cells drawn.
 * @details Run by meson with SDL_VIDEO_DRIVER=offscreen and the bundled
 * font as its argument.
 */

#include "test.h"
#include "../include/define.h"
#include "../include/galloc.h"
#include "../include/gui.h"
#include "../include/terminal.h"
#include "../include/utf8.h"
#include "../src/gui/gui_draw.h"
#include "../src/gui/gui_font.h"

#include <SDL3/SDL.h>

// guiKeyBytes()'s bytes for a key, as a C string.
static const char *keyBytes(const unsigned int keycode, const unsigned int shifted, const int mods) {
  static char out[32];
  const int n = guiKeyBytes(keycode, shifted, mods, out, sizeof(out) - 1);
  out[n > 0 ? n : 0] = '\0';
  return out;
}

static void testKeyBytes(void) {
  // Left to the text the key types.
  CHECK_STR(keyBytes('a', 'A', 0), "");
  CHECK_STR(keyBytes('a', 'A', KEY_MOD_SHIFT), "");
  // Keys, as a terminal sends them.
  CHECK_STR(keyBytes('x', 'X', KEY_MOD_ALT), "\x1b[27;3;120~");
  CHECK_STR(keyBytes('[', '{', KEY_MOD_ALT), "\x1b[27;3;91~");  // not ESC [, a CSI
  CHECK_STR(keyBytes(' ', ' ', KEY_MOD_SHIFT), "\x1b[27;2;32~");
  CHECK_STR(keyBytes('x', 'X', KEY_MOD_CTRL), "\x18");
  CHECK_STR(keyBytes(' ', ' ', KEY_MOD_CTRL), "");  // Ctrl+Space: the NUL byte
  CHECK_INT(guiKeyBytes(' ', ' ', KEY_MOD_CTRL, (char[8]){0}, 8), 1);
  CHECK_STR(keyBytes(SDLK_RETURN, SDLK_RETURN, 0), "\r");
  CHECK_STR(keyBytes(SDLK_BACKSPACE, SDLK_BACKSPACE, 0), "\x7f");
  CHECK_STR(keyBytes(SDLK_ESCAPE, SDLK_ESCAPE, 0), "\x1b");
  CHECK_STR(keyBytes(SDLK_TAB, SDLK_TAB, KEY_MOD_SHIFT), "\x1b[27;2;9~");
  CHECK_STR(keyBytes(SDLK_UP, SDLK_UP, 0), "\x1b[A");
  CHECK_STR(keyBytes(SDLK_RIGHT, SDLK_RIGHT, KEY_MOD_CTRL), "\x1b[1;5C");
  CHECK_STR(keyBytes(SDLK_F5, SDLK_F5, 0), "\x1b[15~");
  CHECK_STR(keyBytes(SDLK_DELETE, SDLK_DELETE, 0), "\x1b[3~");
  // Ctrl+1 and Ctrl+Shift+a have no control byte: modifyOtherKeys.
  CHECK_STR(keyBytes('1', '!', KEY_MOD_CTRL), "\x1b[27;5;49~");
  CHECK_STR(keyBytes('a', 'A', KEY_MOD_CTRL | KEY_MOD_SHIFT), "\x1b[27;6;97~");
  // Alt+Shift+1 is Alt+! (the shifted character, no Shift).
  CHECK_STR(keyBytes('1', '!', KEY_MOD_ALT | KEY_MOD_SHIFT), "\x1b[27;3;33~");
}

static void pushText(const char *text) {
  SDL_Event ev;
  SDL_zero(ev);
  ev.type = SDL_EVENT_TEXT_INPUT;
  ev.text.text = text;
  SDL_PushEvent(&ev);
}

// A key press as SDL reports it: down, the text it types (NULL: none), up.
static void pushKey(const SDL_Scancode scancode, const SDL_Keycode key, const SDL_Keymod mod,
                    const char *text) {
  SDL_Event ev;
  SDL_zero(ev);
  ev.type = SDL_EVENT_KEY_DOWN;
  ev.key.scancode = scancode;
  ev.key.key = key;
  ev.key.mod = mod;
  ev.key.down = true;
  SDL_PushEvent(&ev);
  if (text)
    pushText(text);
  ev.type = SDL_EVENT_KEY_UP;
  ev.key.down = false;
  SDL_PushEvent(&ev);
}

static void pushClick(const float x, const float y, const int down) {
  SDL_Event ev;
  SDL_zero(ev);
  ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  ev.button.button = SDL_BUTTON_LEFT;
  ev.button.down = down;
  ev.button.x = x;
  ev.button.y = y;
  SDL_PushEvent(&ev);
}

// The next key narval reads, by name.
static void checkNextKey(const int line, const char *name) {
  Utf8Char *key = readUtf8Char();
  testCheckStr(__FILE__, line, "the key read", keyToString(key), name);
  gfree(key);
}
#define CHECK_NEXT_KEY(name) checkNextKey(__LINE__, name)

static void testEvents(void) {
  Uint64 start;
  SDL_Event close_ev;
  int ready[1];
  OsStream *keys[1] = {NULL};

  // The "≈" Option+x types on a Mac isn't typed: it is ALT-x.
  pushKey(SDL_SCANCODE_X, SDLK_X, SDL_KMOD_LALT, "\xe2\x89\x88");
  pushKey(SDL_SCANCODE_A, SDLK_A, SDL_KMOD_NONE, "a");
  pushKey(SDL_SCANCODE_X, SDLK_X, SDL_KMOD_LCTRL, NULL);
  pushText("\xc3\xa9");  // é from a dead key or an input method: one character
  pushKey(SDL_SCANCODE_UP, SDLK_UP, SDL_KMOD_NONE, NULL);
  // AltGr (Ctrl + right Alt on Windows) types: not Ctrl+q.
  pushKey(SDL_SCANCODE_Q, SDLK_Q, SDL_KMOD_LCTRL | SDL_KMOD_RALT, "@");

  // The run loop's wait says there are keys.
  CHECK_INT(screenWait(keys, 1, 50, ready), 1);
  CHECK_INT(ready[0], 1);

  CHECK_NEXT_KEY("ALT-x");
  CHECK_NEXT_KEY("a");
  CHECK_NEXT_KEY("CTRL-x");
  CHECK_NEXT_KEY("\xc3\xa9");
  CHECK_NEXT_KEY("ARROW-UP");
  CHECK_NEXT_KEY("@");

  // A lone ESC is ESC, without waiting for more of a sequence.
  pushKey(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, SDL_KMOD_NONE, NULL);
  start = SDL_GetTicks();
  CHECK_NEXT_KEY("ESCAPE");
  CHECK(SDL_GetTicks() - start < 50);

  // A click lands on the cell under the pointer.
  E.constantes.MOUSE = 1;
  pushClick(200.0f, 100.0f, 1);
  pushClick(200.0f, 100.0f, 0);
  CHECK_NEXT_KEY("MOUSE-LEFT");
  CHECK_NEXT_KEY("MOUSE-OTHER");  // the release
  CHECK(E.mouse_col > 0 && E.mouse_row > 0);

  // Nothing queued: the wait times out.
  CHECK_INT(screenWait(keys, 1, 20, ready), 0);

  // The close button is CTRL-x CTRL-c: it asks the editor to quit.
  SDL_zero(close_ev);
  close_ev.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
  SDL_PushEvent(&close_ev);
  CHECK_INT(screenWait(keys, 1, 50, ready), 0);
  CHECK_INT(E.exit_requested, 1);
  E.exit_requested = 0;
}

/**
 * @brief Every key narval can name, with every combination of Ctrl, Alt
 * and Shift, pressed on the window's (US) keyboard: the key read must be
 * the one keyMake() names, whatever a terminal could or couldn't send.
 * A press that types (no Ctrl or Alt) comes with its text, as SDL sends
 * it, and so does Alt+key, as macOS sends Option+key's character.
 */
static void testEveryKey(void) {
  static const SDL_Keycode named[] = {
      SDLK_RETURN, SDLK_TAB,  SDLK_BACKSPACE, SDLK_ESCAPE, SDLK_DELETE, SDLK_HOME,
      SDLK_END,    SDLK_PAGEUP, SDLK_PAGEDOWN, SDLK_UP,    SDLK_DOWN,   SDLK_LEFT,
      SDLK_RIGHT,  SDLK_F1,   SDLK_F2,       SDLK_F3,     SDLK_F4,     SDLK_F5,
      SDLK_F6,     SDLK_F7,   SDLK_F8,       SDLK_F9,     SDLK_F10,    SDLK_F11,
      SDLK_F12};
  static const unsigned char named_base[] = {
      '\r', '\t', BACKSPACE, '\x1b', DEL_KEY, BEG_LINE, END_LINE, PAGE_UP, PAGE_DOWN,
      ARROW_UP, ARROW_DOWN, ARROW_LEFT, ARROW_RIGHT, FUNCTION_KEY, FUNCTION_KEY + 1,
      FUNCTION_KEY + 2, FUNCTION_KEY + 3, FUNCTION_KEY + 4, FUNCTION_KEY + 5, FUNCTION_KEY + 6,
      FUNCTION_KEY + 7, FUNCTION_KEY + 8, FUNCTION_KEY + 9, FUNCTION_KEY + 10, FUNCTION_KEY + 11};
  char text[2];
  char expected[48];
  Utf8Char want;
  Utf8Char *got;
  SDL_Keymod shift;
  SDL_Scancode scancode;
  SDL_Keymod sdl_mods;
  unsigned char base;
  int checked = 0;
  int mods;
  int c;
  int i;

  for (i = 0; i < (int)(sizeof(named) / sizeof(named[0])) + 0x7F - ' '; i++) {
    for (mods = 0; mods < 8; mods++) {
      sdl_mods = (mods & KEY_MOD_SHIFT ? SDL_KMOD_LSHIFT : 0) |
                 (mods & KEY_MOD_ALT ? SDL_KMOD_LALT : 0) | (mods & KEY_MOD_CTRL ? SDL_KMOD_LCTRL : 0);
      text[0] = text[1] = '\0';
      if (i < (int)(sizeof(named) / sizeof(named[0]))) {
        scancode = SDL_GetScancodeFromKey(named[i], NULL);
        base = named_base[i];
        keyMake(&want, mods, base);
      } else {
        // A character: the key that types it, Shift included ("!" is Shift+1).
        c = ' ' + (i - (int)(sizeof(named) / sizeof(named[0])));
        shift = SDL_KMOD_NONE;
        scancode = SDL_GetScancodeFromKey((SDL_Keycode)c, &shift);
        if (scancode == SDL_SCANCODE_UNKNOWN)
          continue;
        if (shift & SDL_KMOD_SHIFT) {
          if (!(mods & KEY_MOD_SHIFT))
            continue; // the same press as with Shift: counted there
          sdl_mods |= SDL_KMOD_LSHIFT;
        } else if (c >= 'a' && c <= 'z' && (mods & KEY_MOD_SHIFT) && !(mods & (KEY_MOD_CTRL | KEY_MOD_ALT))) {
          c -= 32; // Shift+a types "A"
        } else if ((mods & KEY_MOD_SHIFT) && !(c >= 'a' && c <= 'z') && c != ' ') {
          continue; // Shift+1 is "!", above
        }
        keyMake(&want, (shift & SDL_KMOD_SHIFT) ? mods & ~KEY_MOD_SHIFT : mods,
                (c >= 'A' && c <= 'Z' && !(mods & (KEY_MOD_CTRL | KEY_MOD_ALT))) ? (unsigned char)c
                                                                                    : (unsigned char)c);
        if (!(mods & KEY_MOD_CTRL) && mods != KEY_MOD_SHIFT)
          text[0] = (char)c;
        else if (mods == KEY_MOD_SHIFT && c != ' ')
          text[0] = (char)c;
      }
      snprintf(expected, sizeof(expected), "%s", keyToString(&want));
      pushKey(scancode, SDL_GetKeyFromScancode(scancode, sdl_mods, true), sdl_mods, text[0] ? text : NULL);
      got = readUtf8Char();
      if (strcmp(keyToString(got), expected) != 0) {
        fprintf(stderr, "%s:%d: pressed mods %d + scancode %d (%s): read \"%s\", expected \"%s\"\n",
                __FILE__, __LINE__, mods, (int)scancode, SDL_GetScancodeName(scancode), keyToString(got),
                expected);
        test_failures++;
      }
      gfree(got);
      // Whatever a wrong press left queued.
      while ((got = readUtf8Char()) != NULL && got->character[0]) {
        gfree(got);
      }
      gfree(got);
      checked++;
    }
  }
  CHECK(checked > 600);
}

static void testDrawing(void) {
  int rows = 0;
  int cols = 0;
  const char frame[] = "\x1b[?25l\x1b[2J\x1b[H"
                       "\x1b[38;2;255;0;0m\xe2\x96\x88\x1b[0m"  // a red full block at (0, 0)
                       "\x1b[2;3H\x1b[48;2;0;0;255m \x1b[0m"     // a blue background at (1, 2)
                       "\x1b[3;1H\x1b[7m\x1b[38;2;0;255;0m \x1b[0m"; // reversed: green background

  CHECK_INT(getWindowSize(&rows, &cols), 0);
  CHECK(rows >= 20 && cols >= 80);
  screenWrite(frame, sizeof(frame) - 1);
  CHECK_INT(guiCellPixel(0, 0), 0xff0000);
  CHECK_INT(guiCellPixel(1, 2), 0x0000ff);
  CHECK_INT(guiCellPixel(2, 0), 0x00ff00);
  CHECK_INT(guiCellPixel(5, 5), 0x282c34);  // the default background
  // The cursor, once shown: a block in the text's colour.
  screenWrite("\x1b[6;6H\x1b[?25h", 12);
  CHECK_INT(guiCellPixel(5, 5), 0xabb2bf);
}

// The coverage (alpha) of pixel (x, y) of a drawn glyph.
static int alphaAt(SDL_Surface *s, const int x, const int y) {
  Uint8 r, g, b, a;
  if (!SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a))
    return -1;
  return a;
}

// Box drawing fills its cell: lines meet the next cell's, whatever the
// line spacing; corners are arcs; block elements are exact fractions.
static void testCellGlyphs(void) {
  const int w = 9;
  const int h = 21;
  SDL_Surface *s;
  int y;
  int x;
  int full;

  CHECK(guiIsCellGlyph(0x2502) && guiIsCellGlyph(0x259F) && !guiIsCellGlyph('|'));
  s = guiCellGlyph(0x2502, w, h);  // │
  CHECK(s && s->w == w && s->h == h);
  for (y = 0, full = 0; s && y < h; y++)
    full += alphaAt(s, w / 2, y) == 255;
  CHECK_INT(full, h);
  CHECK_INT(s ? alphaAt(s, 0, 0) : -1, 0);
  SDL_DestroySurface(s);
  s = guiCellGlyph(0x2500, w, h);  // ─
  for (x = 0, full = 0; s && x < w; x++)
    full += alphaAt(s, x, h / 2) == 255;
  CHECK_INT(full, w);
  SDL_DestroySurface(s);
  s = guiCellGlyph(0x256D, w, h);  // ╭: down to the bottom edge, right to the right edge
  CHECK(s && alphaAt(s, w / 2, h - 1) == 255 && alphaAt(s, w - 1, h / 2) > 128);
  CHECK_INT(s ? alphaAt(s, 0, 0) : -1, 0);
  CHECK_INT(s ? alphaAt(s, w / 2, 0) : -1, 0);
  SDL_DestroySurface(s);
  s = guiCellGlyph(0x258E, w, h);  // ▎: the left quarter, top to bottom
  CHECK(s && alphaAt(s, 0, 0) == 255 && alphaAt(s, 1, h - 1) == 255 && alphaAt(s, w - 1, 0) == 0);
  SDL_DestroySurface(s);
  s = guiShadow(4, 10);
  CHECK(s && s->w == 29 && alphaAt(s, 14, 14) == 255 && alphaAt(s, 0, 0) == 0);
  SDL_DestroySurface(s);
}

// Characters from other fonts: colour emoji told from grey glyphs, and
// shrunk to a cell by averaging, transparent pixels adding no colour.
static void testImages(void) {
  SDL_Surface *big = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA32);
  SDL_Surface *small;
  Uint8 r = 0, g = 0, b = 0, a = 0;
  int y;

  CHECK(big != NULL);
  if (!big)
    return;
  SDL_FillSurfaceRect(big, NULL, SDL_MapSurfaceRGBA(big, 255, 255, 255, 255));
  CHECK_INT(guiHasColor(big), 0);  // a white glyph
  // Left half transparent black, right half opaque red.
  SDL_FillSurfaceRect(big, NULL, SDL_MapSurfaceRGBA(big, 0, 0, 0, 0));
  for (y = 0; y < 8; y++) {
    SDL_Rect right = {4, y, 4, 1};
    SDL_FillSurfaceRect(big, &right, SDL_MapSurfaceRGBA(big, 255, 0, 0, 255));
  }
  CHECK_INT(guiHasColor(big), 1);
  small = guiShrink(big, 1, 1);
  CHECK(small && small->w == 1 && small->h == 1);
  if (small)
    SDL_ReadSurfacePixel(small, 0, 0, &r, &g, &b, &a);
  CHECK_INT(r, 255);  // red, not darkened by the transparent black half
  CHECK_INT(g, 0);
  CHECK(a >= 127 && a <= 128);  // half covered
  SDL_DestroySurface(small);
  small = guiShrink(big, 2, 3);  // not a whole fraction: 4 x 2.67 pixels each
  CHECK(small && small->w == 2 && small->h == 3);
  CHECK_INT(small ? alphaAt(small, 0, 1) : -1, 0);
  CHECK_INT(small ? alphaAt(small, 1, 2) : -1, 255);
  SDL_DestroySurface(small);
  CHECK(guiShrink(NULL, 1, 1) == NULL && guiShrink(big, 0, 1) == NULL);
  SDL_DestroySurface(big);
}

// A family's own faces for bold and italic, told by their style names.
static void testFontFaces(void) {
  static const char dejavu[] = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
  GuiFace faces[GUI_FACES];
  TTF_Font *first;

  CHECK_INT(guiStyleScore("Regular", "Bold", 1, 0), 2);
  CHECK_INT(guiStyleScore("Regular", "SemiBold", 1, 0), 0);
  CHECK_INT(guiStyleScore("Regular", "Bold Italic", 1, 0), 0);
  CHECK_INT(guiStyleScore("Regular", "Italic", 0, 1), 4);
  CHECK_INT(guiStyleScore("Book", "Oblique", 0, 1), 3);  // DejaVu: an italic beats it
  CHECK_INT(guiStyleScore("Light", "Light Italic", 0, 1), 4);
  CHECK_INT(guiStyleScore("Medium", "Italic", 0, 1), 2);  // no Medium Italic: the plain one
  CHECK_INT(guiStyleScore("Medium", "Light Italic", 0, 1), 0);
  CHECK_INT(guiStyleScore("Regular", "Bold Italic", 1, 1), 2);
  CHECK_INT(guiStyleScore("Regular", "BoldOblique", 1, 1), 1);
  CHECK_INT(guiStyleScore("Regular", "Bold", 1, 1), 0);
  CHECK_INT(guiStyleScore("Italic", "Bold Italic", 1, 1), 0);  // an italic isn't a regular
  CHECK_INT(guiStyleScore(NULL, "Bold", 1, 0), 0);

  // DejaVu Sans Mono's Bold, Oblique and Bold Oblique beside it, where
  // the system has them.
  first = TTF_OpenFont(dejavu, 12.0f);
  if (!first)
    return;
  guiFindFaces(dejavu, first, 12.0f, faces);
  CHECK(faces[0].path == NULL);  // a static font: its first face is the regular one
  if (SDL_GetPathInfo("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", NULL))
    CHECK(faces[1].path && strstr(faces[1].path, "DejaVuSansMono-Bold.ttf"));
  if (SDL_GetPathInfo("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Oblique.ttf", NULL))
    CHECK(faces[2].path && strstr(faces[2].path, "DejaVuSansMono-Oblique.ttf"));
  guiFacesFree(faces);
  CHECK(faces[1].path == NULL);
  TTF_CloseFont(first);
}

static void writeStr(const char *s) {
  screenWrite(s, strlen(s));
}

// In the window: lines and popups as drawn, the cursor's glide.
static void testShapes(void) {
  const char lines[] = "\x1b[?25l\x1b[2J\x1b[H"
                       "\x1b[38;2;255;0;0m\xe2\x94\x82\r\n\xe2\x94\x82\x1b[0m";  // │ over │
  const char box[] = "\x1b[2J\x1b[9;11H\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xae"
                     "\x1b[10;11H\xe2\x94\x82   \xe2\x94\x82"
                     "\x1b[11;11H\xe2\x95\xb0\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xaf";
  long shadow;

  CHECK_INT(screenDrawsShadows(), 1);
  screenWrite(lines, sizeof(lines) - 1);
  CHECK_INT(guiCellPixel(0, 0), 0xff0000);
  CHECK_INT(guiCellPixel(1, 0), 0xff0000);
  // A popup's box floats: a shadow just outside it, none far away.
  screenWrite(box, sizeof(box) - 1);
  shadow = guiCellPixel(9, 15);
  CHECK(shadow >= 0 && (shadow >> 16 & 0xFF) < 0x28 && (shadow & 0xFF) < 0x34);
  CHECK_INT(guiCellPixel(9, 30), 0x282c34);
  CHECK_INT(guiCellPixel(9, 12), 0x282c34);  // inside: the popup's own background
  // The cursor glides to where it jumps, and lands there.
  writeStr("\x1b[2J\x1b[6;6H\x1b[?25h");
  CHECK_INT(guiCellPixel(5, 5), 0xabb2bf);
  writeStr("\x1b[16;31H");
  CHECK_INT(guiCellPixel(15, 30), 0x282c34);  // on its way
  SDL_Delay(150);
  CHECK_INT(guiCellPixel(15, 30), 0xabb2bf);
  // One column, as typing moves it: no glide.
  writeStr("\x1b[16;32H");
  CHECK_INT(guiCellPixel(15, 31), 0xabb2bf);
}

int main(const int argc, char **argv) {
  testInit();
  testKeyBytes();
  if (guiOpen(argc > 1 ? argv[1] : NULL) != 0) {
    fprintf(stderr, "test_gui: no window (SDL_VIDEO_DRIVER=offscreen?)\n");
    return 1;
  }
  testEvents();
  testEveryKey();
  testDrawing();
  testCellGlyphs();
  testImages();
  testFontFaces();
  testShapes();
  terminalRestore();  // closes the window
  CHECK_INT(guiCellPixel(0, 0), -1);
  gfree(E.status_msg);
  return testDone();
}
