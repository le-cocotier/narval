
#ifndef TERMINAL_H_
#define TERMINAL_H_

/* includes */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "utf8.h"
#include "debug_log.h"
#include "os.h"

/**
 * @brief Where the editor's screen is. Standalone, that is the terminal
 * narval runs in. In daemon mode (daemon.c) it is the client window being
 * served at the moment: drawing, key reads and the window size all go
 * through these three, never to os_term_*() directly.
 * With narval-gui (src/gui/) it is a window of its own.
 */
typedef struct
{
    void (*write)(const void *buf, size_t n);
    // os_stream_read() semantics: >0 bytes, 0 nothing in time, -1 never.
    long (*read_keys)(void *buf, size_t n, int timeout_ms);
    int (*size)(int *rows, int *cols);
    // Optional (NULL: the terminal's way). The run loop's wait, os_wait()
    // semantics, where streams[0] stands for the keys: a window's keys come
    // from its event queue, not from a stream.
    int (*wait)(OsStream *const *streams, int n, int timeout_ms, int *ready);
    // Optional: what terminalRestore() does instead (closing a window).
    void (*restore)(void);
    // 1: a window (narval-gui), which draws popups' shadows itself, soft
    // ones, where a terminal gets a band of dark cells (popup_ui.c).
    int draws_shadows;
} ScreenDriver;

// NULL: back to the real terminal.
void screenSetDriver(const ScreenDriver *driver);
void screenWrite(const void *buf, size_t n);
long screenReadKeys(void *buf, size_t n, int timeout_ms);
// The run loop's wait: keys (streams[0]) and every other stream.
int screenWait(OsStream *const *streams, int n, int timeout_ms, int *ready);
// The screen's draws_shadows.
int screenDrawsShadows(void);

/**
 * \fn void die(const char *s)
 * \brief Exit program and return s error message.
 * \param s Error string
 * @note Marked noreturn: it always exit()s. Without this a static analyzer
 * (and a human skimming the caller) has to assume execution can continue
 * past a `foo == NULL ? die(...) : ...` guard, which reads as a real null
 * dereference a few lines later even though it can never actually happen.
 * */

void die(const char *s) __attribute__((noreturn));

void disableRawMode();
// Restore the terminal for the shell: reset attributes, clear, show cursor,
// leave raw mode. Called once on the way out of editorRunLoop().
void terminalRestore(void);

void enableRawMode();

Utf8Char* editorReadKey();

int getWindowSize(int *rows, int *cols);

char *keyToString(const Utf8Char* key);

// Modified keys (MOD_KEY in define.h).
// The key for `base` (a character or a key code) with KEY_MOD_* `mods`,
// in its one canonical form: Ctrl+a is the byte 0x01, Shift+a is "A",
// Alt+Shift+a is ALT-"A", Shift+F5 is SHIFT_FUNCTION_KEY + 4.
void keyMake(Utf8Char *key, int mods, unsigned char base);
// The other way round: `key`'s modifiers and the key without them (0x01
// gives KEY_MOD_CTRL and 'a'). A multi-byte character has no modifiers.
int keySplit(const Utf8Char *key, unsigned char *base);
// What a modified key used to be before keys carried every modifier,
// tried when it isn't bound: Ctrl+Up acts as Up, Alt+a types "a" (an
// AZERTY "{" with Option as Meta arrives as Alt+...), Alt+f and Alt+b as
// Alt+Right / Alt+Left (what Terminal.app sends for Option+arrows).
// @return 0 if `key` has no such fallback.
int keyFallback(const Utf8Char *key, Utf8Char *out);
// The bytes an xterm sends for `key`, for a program in a terminal pane.
// @return their length, 0 if it has none.
int keyEscapeSequence(const Utf8Char *key, char *out, size_t size);
// `name` as keyToString() would spell it: modifiers in CTRL-ALT-SHIFT
// order, with OPTION-/META-/OPT- and Emacs's C-/M-/S- accepted
// ("M-x" -> "ALT-x", "SHIFT-ALT-a" -> "ALT-A", "C-M-a" -> "CTRL-ALT-a").
// A name without modifiers is copied as is.
void keyCanonicalName(const char *name, char *out, size_t size);

// appDebug(), appTrace(), appDebugLogPath(), appDebugClose(): debug_log.h.

#endif
