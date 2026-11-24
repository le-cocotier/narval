#ifndef GUI_H_
#define GUI_H_

#include <stddef.h>

// narval-gui: the editor in a window of its own (src/gui/gui_sdl.c, SDL3
// and SDL3_ttf). The window is one more ScreenDriver (terminal.h): the
// frames editorRefreshScreen() writes go through the terminal panes' ANSI
// parser (ansi_parser.c) into a grid of cells, drawn with a monospace
// font, and the window's key and mouse events become the bytes a terminal
// would send, read by the same key reader. Every command, key binding,
// theme and popup is the terminal narval's. What it draws better than a
// terminal can: box drawing and block characters fitted to the cell
// (gui_draw.c), popups lifted off the screen with a soft shadow, the
// cursor's glide, the title bar in the theme's colour (os_window.c).
//
// Settings (init.lisp): GUI-FONT-SIZE (points, settings.c), and GUI-FONT,
// a path to a .ttf/.ttc/.otf file. Without GUI-FONT the bundled DejaVu
// Sans Mono (assets/fonts/) is used, else the system's monospace font.

// Opens the window and makes it the screen. `font_path`: a font to try
// first (NULL for the usual search). @return 0, or -1 with the reason on
// stderr (no display, no font).
int guiOpen(const char *font_path);

// After init.lisp has loaded: applies GUI-FONT and GUI-FONT-SIZE.
void guiConfigure(void);

// The bytes narval's key reader gets for a key press, or 0 when the press
// is left to the text it types (SDL's text input: layouts, dead keys,
// AltGr). `keycode` is the SDL keycode, `shifted` the same key with Shift
// applied, `mods` KEY_MOD_* bits (define.h).
int guiKeyBytes(unsigned int keycode, unsigned int shifted, int mods, char *out, size_t size);

// Tests: draws the grid now, and the colour (0xRRGGBB) of the pixel at
// the middle of cell (`row`, `col`), -1 if it can't be read.
long guiCellPixel(int row, int col);

#endif // GUI_H_
