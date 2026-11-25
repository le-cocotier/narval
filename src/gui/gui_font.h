#ifndef GUI_FONT_H_
#define GUI_FONT_H_

// narval-gui's font faces (gui_font.c): a family's own bold, italic and
// bold italic, found beside the regular font, so its italic (Victor
// Mono's cursive one) is drawn instead of the regular one slanted.

#include <SDL3_ttf/SDL_ttf.h>

typedef struct
{
    char *path;  // galloc'd; NULL: none found
    long index;  // the face in a collection (.ttc), plus a variable font's
                 // named instance times 65536
    int score;   // how well it fits: guiStyleScore()
} GuiFace;

// How well a face whose style name is `style` (FreeType's: "Bold Italic",
// "Oblique", "Light Italic") serves as the `bold`/`italic` style of the
// face named `regular` ("Regular", "Book", "Light"): 0 not at all, more
// the better. The bold is "Bold"; the italic has the regular's weight, or
// else the plain one; an italic beats an oblique.
int guiStyleScore(const char *regular, const char *style, int bold, int italic);

// `path` at `size` points, face `index`: a collection's (.ttc) face, plus
// a variable font's named instance times 65536. NULL if it can't be
// opened.
TTF_Font *guiOpenFace(const char *path, long index, float size);

#define GUI_FACES 4

// The faces to draw with, given `first`, `path`'s first face: [0] the
// regular one when it isn't `first` (a variable font's "Regular" where its
// default is "Thin"), [1] bold, [2] italic, [3] bold italic. Looked for
// in the font's other faces and named instances, and in the fonts beside
// it whose file name starts like its own or like its family's name
// ("VictorMono-Italic.ttf" beside "VictorMono-Regular.ttf") and whose
// family is the same. Release with guiFacesFree().
void guiFindFaces(const char *path, TTF_Font *first, float size, GuiFace faces[GUI_FACES]);
void guiFacesFree(GuiFace faces[GUI_FACES]);

#endif // GUI_FONT_H_
