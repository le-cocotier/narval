#ifndef GUI_DRAW_H_
#define GUI_DRAW_H_

// narval-gui's drawn shapes (gui_draw.c): what a window draws better than
// a font does. Each is a white image whose alpha is the shape, tinted when
// drawn, as the font's glyphs are.

#include <SDL3/SDL.h>

// Box drawing (U+2500-U+257F) and block elements (U+2580-U+259F): drawn
// to fill the cell exactly, so lines meet across rows whatever the line
// spacing, and rounded corners are true arcs.
int guiIsCellGlyph(Uint32 codepoint);
// The glyph for `codepoint` in a `w` x `h` pixel cell. NULL if it can't
// be made. Free it with SDL_DestroySurface().
SDL_Surface *guiCellGlyph(Uint32 codepoint, int w, int h);
// Where light lines run in a `w` x `h` cell, as they are drawn: their
// centres from the cell's corner, their thickness, and the radius of a
// rounded corner (╭) on those centres.
void guiCellLines(int w, int h, float *x, float *y, float *thickness, float *radius);

// A rounded rectangle `2 * radius + 1` pixels square, filled, its edges
// anti-aliased: stretched over a box with SDL_RenderTexture9Grid().
SDL_Surface *guiRoundedRect(int radius);
// A soft shadow for a box with corners of `radius`, fading out over
// `blur` pixels around it: `2 * (radius + blur) + 1` pixels square, for
// SDL_RenderTexture9Grid() too.
SDL_Surface *guiShadow(int radius, int blur);

#endif // GUI_DRAW_H_
