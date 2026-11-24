/**
 * @file os_window.c
 * @brief libnarval-os-window: include/os.h's narval-gui part, the window
 * as the system draws it around narval-gui's own drawing.
 * @details The title bar takes the theme's background, so the window reads
 * as one surface: on macOS a transparent title bar over the window's
 * background colour, on Windows 11 the caption colour. Text on it follows
 * the colour's lightness (dark or light appearance, dark mode).
 */

#include "../../include/os.h"

// Whether white text reads better than black on `rgb`.
static int isDark(const unsigned int rgb)
{
    return ((rgb >> 16 & 0xFF) * 299 + (rgb >> 8 & 0xFF) * 587 + (rgb & 0xFF) * 114) / 1000 < 128;
}

#if defined(__APPLE__)

#include <objc/message.h>
#include <objc/runtime.h>

// Objective-C messages from C: objc_msgSend cast to each method's type.
static id msgId(id target, const char *selector)
{
    return ((id (*)(id, SEL)) objc_msgSend)(target, sel_registerName(selector));
}

static id classMsgId(const char *cls, const char *selector)
{
    return ((id (*)(Class, SEL)) objc_msgSend)(objc_getClass(cls), sel_registerName(selector));
}

static void msgSetId(id target, const char *selector, id value)
{
    ((void (*)(id, SEL, id)) objc_msgSend)(target, sel_registerName(selector), value);
}

void os_window_titlebar(void *native, const unsigned int rgb)
{
    id window = (id) native;
    id pool;
    id color;
    id name;
    id appearance;

    if (!window || !objc_getClass("NSColor") || !objc_getClass("NSAppearance"))
        return;
    pool = msgId(classMsgId("NSAutoreleasePool", "alloc"), "init");
    color = ((id (*)(Class, SEL, double, double, double, double)) objc_msgSend)(
        objc_getClass("NSColor"), sel_registerName("colorWithSRGBRed:green:blue:alpha:"),
        (double) (rgb >> 16 & 0xFF) / 255.0, (double) (rgb >> 8 & 0xFF) / 255.0, (double) (rgb & 0xFF) / 255.0,
        1.0);
    name = ((id (*)(Class, SEL, const char *)) objc_msgSend)(
        objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"),
        isDark(rgb) ? "NSAppearanceNameDarkAqua" : "NSAppearanceNameAqua");
    appearance = ((id (*)(Class, SEL, id)) objc_msgSend)(objc_getClass("NSAppearance"),
                                                         sel_registerName("appearanceNamed:"), name);
    ((void (*)(id, SEL, BOOL)) objc_msgSend)(window, sel_registerName("setTitlebarAppearsTransparent:"), YES);
    msgSetId(window, "setBackgroundColor:", color);
    if (appearance)
        msgSetId(window, "setAppearance:", appearance);
    msgId(pool, "drain");
}

#elif defined(_WIN32)

#include <windows.h>
#include <dwmapi.h>

// Not in every SDK's dwmapi.h yet: Windows 10 2004's and Windows 11's.
#define NARVAL_DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define NARVAL_DWMWA_CAPTION_COLOR 35

void os_window_titlebar(void *native, const unsigned int rgb)
{
    const HWND hwnd = (HWND) native;
    const BOOL dark = isDark(rgb) ? TRUE : FALSE;
    const COLORREF color = RGB(rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF);

    if (!hwnd)
        return;
    // Each fails harmlessly where Windows doesn't know it.
    DwmSetWindowAttribute(hwnd, NARVAL_DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, NARVAL_DWMWA_CAPTION_COLOR, &color, sizeof(color));
}

#else

// Linux and the BSDs: the window manager draws the title bar its own way.
void os_window_titlebar(void *native, const unsigned int rgb)
{
    (void) native;
    (void) isDark(rgb);
}

#endif
