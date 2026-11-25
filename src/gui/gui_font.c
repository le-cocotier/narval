/**
 * @file gui_font.c
 * @brief See gui_font.h: a font family's bold, italic and bold italic
 * faces, found beside the regular one.
 * @details SDL_ttf draws bold and italic by thickening and slanting the
 * regular face. A family's real italic can be another design altogether
 * (Victor Mono's is cursive), and its real bold is better hinted. Fonts
 * come as one file per face ("VictorMono-Italic.ttf"), named differently
 * from family to family ("-It", "-Oblique", "BoldItalic"), or as one
 * collection (Menlo.ttc), or as a variable font whose named instances
 * are the weights (VictorMono[wght].ttf, VictorMono-Italic[wght].ttf):
 * the faces are told apart by the family and style names inside them,
 * not by the file names.
 */

#include "gui_font.h"

#include "../../include/galloc.h"
#include "../../include/os.h"

#include <stdio.h>
#include <string.h>

#define MAX_CANDIDATES 64  // font files of the family opened, at most
#define MAX_INSTANCES 64   // a variable font's named instances looked at

/**
 * @brief Splits a style name into its weight and slant: lower case, only
 * letters and digits, "italic" (2) or "oblique" (1) taken out, and the
 * names of the regular weight ("regular", "book", "normal", "roman") read
 * as "". @return The slant, 0 for upright.
 */
static int styleParse(const char *style, char *weight, const size_t size)
{
    static const char *const regular[] = {"regular", "book", "normal", "roman", "plain"};
    char *at;
    size_t n = 0;
    size_t i;
    int slant = 0;

    for (; style && *style && n + 1 < size; style++)
        if ((*style >= 'a' && *style <= 'z') || (*style >= '0' && *style <= '9'))
            weight[n++] = *style;
        else if (*style >= 'A' && *style <= 'Z')
            weight[n++] = (char) (*style - 'A' + 'a');
    weight[n] = '\0';
    if ((at = strstr(weight, "italic")) != NULL)
    {
        slant = 2;
        memmove(at, at + 6, strlen(at + 6) + 1);
    }
    else if ((at = strstr(weight, "oblique")) != NULL)
    {
        slant = 1;
        memmove(at, at + 7, strlen(at + 7) + 1);
    }
    for (i = 0; i < sizeof(regular) / sizeof(regular[0]); i++)
        if (!strcmp(weight, regular[i]))
            weight[0] = '\0';
    return slant;
}

int guiStyleScore(const char *regular, const char *style, const int bold, const int italic)
{
    char regular_weight[64];
    char weight[64];
    int slant;

    if (!regular || !style)
        return 0;
    if (styleParse(regular, regular_weight, sizeof(regular_weight)) != 0)
        return 0;
    slant = styleParse(style, weight, sizeof(weight));
    if (italic ? slant == 0 : slant != 0)
        return 0;
    if (bold)
        return strcmp(weight, "bold") ? 0 : italic ? slant : 2;
    if (!strcmp(weight, regular_weight))
        return slant + 2;
    return weight[0] ? 0 : slant;
}

// Whether `style` names the regular face: upright, plain weight.
static int isRegular(const char *style)
{
    char weight[64];
    return styleParse(style, weight, sizeof(weight)) == 0 && !weight[0];
}

TTF_Font *guiOpenFace(const char *path, const long index, const float size)
{
    SDL_PropertiesID props;
    TTF_Font *font;

    if (index == 0)
        return TTF_OpenFont(path, size);
    props = SDL_CreateProperties();
    if (!props)
        return NULL;
    SDL_SetStringProperty(props, TTF_PROP_FONT_CREATE_FILENAME_STRING, path);
    SDL_SetFloatProperty(props, TTF_PROP_FONT_CREATE_SIZE_FLOAT, size);
    SDL_SetNumberProperty(props, TTF_PROP_FONT_CREATE_FACE_NUMBER, index);
    font = TTF_OpenFontWithProperties(props);
    SDL_DestroyProperties(props);
    return font;
}

// Keeps `path`'s face `index` for each style it suits better than what was
// found so far. @return Its number of faces (a collection's), 0 if it
// can't be opened.
static int consider(const char *path, const long index, const char *family, const char *regular,
                    const float size, GuiFace faces[GUI_FACES])
{
    TTF_Font *font = guiOpenFace(path, index, size);
    const char *name;
    int score;
    int count;
    int k;

    if (!font)
        return 0;
    name = TTF_GetFontFamilyName(font);
    for (k = 1; name && !strcmp(name, family) && k < GUI_FACES; k++)
    {
        // k: 1 bold, 2 italic, 3 bold italic.
        score = guiStyleScore(regular, TTF_GetFontStyleName(font), k != 2, k != 1);
        if (score > faces[k].score)
        {
            gfree(faces[k].path);
            faces[k].path = gstrdup(path);
            faces[k].index = index;
            faces[k].score = score;
        }
    }
    count = TTF_GetNumFontFaces(font);
    TTF_CloseFont(font);
    return count;
}

// Every face of `path` and every named instance of its first one, but
// the first face itself when `skip_first`.
static void considerFile(const char *path, const int skip_first, const char *family, const char *regular,
                         const float size, GuiFace faces[GUI_FACES])
{
    TTF_Font *font;
    int count;
    long k;

    font = guiOpenFace(path, 0, size);
    if (!font)
        return;
    count = TTF_GetNumFontFaces(font);
    TTF_CloseFont(font);
    for (k = skip_first ? 1 : 0; k < count; k++)
        consider(path, k, family, regular, size, faces);
    for (k = 1; k <= MAX_INSTANCES && consider(path, k << 16, family, regular, size, faces); k++)
        ;
}

// A variable font opens at its default instance, Victor Mono's at "Thin":
// its "Regular" instance into faces[0] if it has one. @return The style
// name the others are looked for against (galloc'd).
static char *regularInstance(const char *path, TTF_Font *first, const float size, GuiFace faces[GUI_FACES])
{
    const char *style = TTF_GetFontStyleName(first);
    TTF_Font *font;
    long k;

    if (!style || isRegular(style))
        return gstrdup(style ? style : "");
    for (k = 1; k <= MAX_INSTANCES && (font = guiOpenFace(path, k << 16, size)) != NULL; k++)
    {
        if (isRegular(TTF_GetFontStyleName(font)))
        {
            faces[0].path = gstrdup(path);
            faces[0].index = k << 16;
            faces[0].score = 1;
            style = NULL;
        }
        TTF_CloseFont(font);
        if (!style)
            return gstrdup("Regular");
    }
    return gstrdup(style);
}

static int isFontFile(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!SDL_strcasecmp(dot, ".ttf") || !SDL_strcasecmp(dot, ".otf") || !SDL_strcasecmp(dot, ".ttc"));
}

// Whether `name` starts with `prefix`, ignoring case; never for "".
static int startsWith(const char *name, const char *prefix)
{
    const size_t n = strlen(prefix);
    return n > 0 && !SDL_strncasecmp(name, prefix, n);
}

void guiFindFaces(const char *path, TTF_Font *first, const float size, GuiFace faces[GUI_FACES])
{
    const char *family = TTF_GetFontFamilyName(first);
    const char *slash;
    const char *base;
    char *regular;
    char dir[OS_PATH_MAX];
    char stem[256];
    char compact[256];
    char file[OS_PATH_MAX];
    OsDirEntry *entries;
    size_t n = 0;
    int count = 0;
    int looked = 0;
    int i;

    memset(faces, 0, GUI_FACES * sizeof(*faces));
    if (!path || !family || !TTF_GetFontStyleName(first))
        return;
    regular = regularInstance(path, first, size, faces);
    if (!regular)
        return;
    // Its own file's other faces and instances.
    considerFile(path, 1, family, regular, size, faces);

    // The files beside it named like it ("VictorMono" from
    // "VictorMono-Regular.ttf") or like its family ("Victor Mono").
    slash = strrchr(path, '/');
    if (!slash || (strrchr(path, '\\') && strrchr(path, '\\') > slash))
        slash = strrchr(path, '\\');
    base = slash ? slash + 1 : path;
    if (!slash)
        snprintf(dir, sizeof(dir), ".");
    else if (slash == path)
        snprintf(dir, sizeof(dir), "/");
    else
        snprintf(dir, sizeof(dir), "%.*s", (int) (slash - path), path);
    for (n = 0; base[n] && !strchr("-_[. ", base[n]) && n + 1 < sizeof(stem); n++)
        stem[n] = base[n];
    stem[n] = '\0';
    for (n = 0, i = 0; family[i] && n + 1 < sizeof(compact); i++)
        if (family[i] != ' ')
            compact[n++] = family[i];
    compact[n] = '\0';
    entries = os_list_dir(dir, &count);
    for (i = 0; entries && i < count && looked < MAX_CANDIDATES; i++)
    {
        if (entries[i].is_dir || !isFontFile(entries[i].name) || !strcmp(entries[i].name, base) ||
            !(startsWith(entries[i].name, stem) || startsWith(entries[i].name, compact)))
            continue;
        looked++;
        if (snprintf(file, sizeof(file), "%s/%s", dir, entries[i].name) >= (int) sizeof(file))
            continue;
        considerFile(file, 0, family, regular, size, faces);
    }
    if (entries)
        os_free_dir(entries, count);
    gfree(regular);
}

void guiFacesFree(GuiFace faces[GUI_FACES])
{
    int k;
    for (k = 0; k < GUI_FACES; k++)
    {
        gfree(faces[k].path);
        faces[k].path = NULL;
    }
}
