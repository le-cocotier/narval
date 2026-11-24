/**
 * @file which_key.c
 * @brief Emacs which-key style discoverability panel — shows what you can
 * press next while a key prefix is pending, and which commands match what
 * you've typed at the M-x prompt.
 */

#include "../include/which_key.h"
#include "../include/galloc.h"

#include "../include/append_buffer.h"
#include "../include/buffer.h"
#include "../include/data.h"
#include "../include/input.h"
#include "../include/popup_ui.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"

#include "../include/os.h"
#include "../include/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern EditorConfig E;

// Same tiny abuf helpers the other overlay modules (lsp_ui, list_popup,
// buffer_switcher) each keep locally.
static void ab_move(struct abuf *ab, const int row, int col) {
    char buf[32];
    const int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    abAppend(ab, buf, n);
}

static void ab_sgr(struct abuf *ab, const char *codes) {
    // 48, not the 32 every other popup module's copy of this helper uses:
    // this file's ab_sgr is fed PopupStyle's combined bold+bg+fg codes
    // (up to ~35 chars), not the short bare color codes ("33", "1;31")
    // everything else here passes. 32 silently truncated them — the
    // clamp below stopped that from being a buffer over-read, but the
    // codes still came out cut off mid-escape-sequence and broke the
    // panel's colors until the actual buffer was sized to fit.
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "\x1b[%sm", codes);
    if (n >= (int) sizeof(buf)) n = (int) sizeof(buf) - 1; // defensive: never over-read on truncation
    abAppend(ab, buf, n);
}

#define AB_RESET(ab) ab_sgr(ab, "0")

#define WK_MAX_ROWS       8    // panel never eats more than this many rows
#define WK_ENTRY_MAX     64
#define WK_COMMAND_ENTRY_MAX 96 // an M-x entry: command, tab, its keys
#define WK_MIN_COL_WIDTH 18

// How long a prefix must stay pending before the panel appears. Long enough
// that a fluent "CTRL-x CTRL-s" doesn't flash the panel on every save, short
// enough that hesitating gets you help. Emacs' which-key defaults to 1s;
// this is snappier because the panel is small.
#define WK_DELAY_MS 400

// ─── caches ────────────────────────────────────────────────────────────────
//
// Both of these exist to keep per-frame work down to plain strcmp/strncmp.
// Resolving a Lisp value back to the symbol it's bound to means walking the
// global environment into an alist, which allocates — unacceptable at the
// ~20 redraws/second the panel is drawn at.

static char **g_bind_names = NULL;  // parallel to E.key_binds
static int g_bind_names_count = 0;

static char **g_commands = NULL;    // M-x candidates
static int g_command_count = 0;
static char **g_command_keys = NULL; // parallel to g_commands: its keys, or NULL

static long monotonic_ms(void) {
    return (long) os_monotonic_ms();
}

/**
 * @brief Reverse-lookup: the symbol name a value is bound to in the global
 * environment, or NULL.
 * @details map-key receives its command already evaluated — `editor-quit`
 * arrives as the function object itself, not the symbol — so the printable
 * name has to be recovered by searching the environment for a binding with
 * this exact value. lisp_equal compares C function pointers for LISP_FUNC
 * and object identity for lambdas, which is exactly the notion of "same
 * command" wanted here.
 */
static const char *symbolNameForValue(const Lisp value, const Lisp alist) {
    const LispType want = lisp_type(value);
    Lisp it;
    Lisp pair;
    Lisp key;
    Lisp val;
    int same;
    if (want != LISP_FUNC && want != LISP_LAMBDA)
        return NULL; // only commands are worth naming

    for (it = alist; lisp_is_pair(it); it = lisp_cdr(it)) {
        pair = lisp_car(it);
        if (!lisp_is_pair(pair))
            continue;
        key = lisp_car(pair);
        if (lisp_type(key) != LISP_SYMBOL)
            continue;

        val = lisp_cdr(pair);
        if (lisp_type(val) != want)
            continue;

        // Deliberately NOT lisp_equal(): its LISP_FUNC branch is
        // `lisp_func(a) == lisp_func(b) && a.type == b.type`, and both
        // calls are evaluated before that type guard is reached — so
        // comparing a function against the first non-function binding in
        // the environment trips lisp_func()'s assert (and reads the wrong
        // union member outright once asserts are compiled out). Having
        // already matched the types above, compare the payloads directly.
        same = (want == LISP_FUNC) ? (lisp_func(val) == lisp_func(value))
                                       : lisp_eq(val, value);
        if (same)
            return lisp_symbol_string(key);
    }
    return NULL;
}

static void freeStrings(char ***arr, int *count) {
    int i;
    if (*arr) {
        for (i = 0; i < *count; i++)
            gfree((*arr)[i]);
        gfree(*arr);
    }
    *arr = NULL;
    *count = 0;
}

void whichKeyInvalidate(void) { freeStrings(&g_bind_names, &g_bind_names_count); }

/**
 * @brief Fill g_bind_names with a printable command name per keybinding.
 * @details Two shapes show up in E.key_binds[].command: a quoted list like
 * '(editor-set-prefix "user"), whose head symbol is the name (plus its
 * string argument, which is what distinguishes one prefix-setter from
 * another), and a bare evaluated function, which needs the reverse lookup.
 */
static void buildBindNames(void) {
    Lisp alist;
    int i;
    char name[WK_ENTRY_MAX];
    Lisp cmd;
    Lisp head;
    Lisp rest;
    Lisp arg;
    const char *resolved;
    if (g_bind_names && g_bind_names_count == E.number_of_keybinds)
        return; // still current

    freeStrings(&g_bind_names, &g_bind_names_count);
    if (E.number_of_keybinds <= 0)
        return;

    g_bind_names = gcalloc((size_t) E.number_of_keybinds, sizeof(char *));
    if (!g_bind_names)
        return;
    g_bind_names_count = E.number_of_keybinds;

    alist = lisp_table_to_alist(lisp_car(lisp_env(E.ctx)), E.ctx);

    for (i = 0; i < E.number_of_keybinds; i++) {
        cmd = E.key_binds[i].command;

        name[0] = '?';
        name[1] = '\0';

        if (lisp_is_pair(cmd)) {
            head = lisp_car(cmd);
            if (lisp_type(head) == LISP_SYMBOL) {
                rest = lisp_cdr(cmd);
                arg = lisp_is_pair(rest) ? lisp_car(rest) : lisp_null();
                if (lisp_type(arg) == LISP_STRING)
                    snprintf(name, sizeof(name), "%s %s",
                             lisp_symbol_string(head), lisp_string(arg));
                else
                    snprintf(name, sizeof(name), "%s", lisp_symbol_string(head));
            }
        } else {
            resolved = symbolNameForValue(cmd, alist);
            if (resolved)
                snprintf(name, sizeof(name), "%s", resolved);
        }
        g_bind_names[i] = utilStrdup(name);
    }
}

const char *whichKeyBindingName(const int index) {
    buildBindNames();
    if (!g_bind_names || index < 0 || index >= g_bind_names_count)
        return NULL;
    return g_bind_names[index];
}

/**
 * @brief The keys that set prefix `prefix_id`, as typed from no prefix
 * ("CTRL-x p" for "project"), into `out`; 0 when nothing reaches it.
 * @details Several keys can set one prefix (CTRL-x sets "user" from no
 * prefix and from the debugger): the shortest route wins.
 */
static int prefixKeys(const int prefix_id, const int depth, char *out, const size_t size) {
    char want[WK_ENTRY_MAX];
    char outer[WK_ENTRY_MAX];
    char route[WK_ENTRY_MAX];
    const int star = starPrefixId();
    int found = 0;
    int i;
    out[0] = '\0';
    if (prefix_id == 0)
        return 1; // no-prefix: nothing to type first
    if (depth > 4 || !g_bind_names)
        return 0;
    snprintf(want, sizeof(want), "editor-set-prefix %s", E.prefix[prefix_id].prefix_name);
    for (i = 0; i < E.number_of_keybinds; i++) {
        if (E.key_binds[i].prefix_id == star || !g_bind_names[i] ||
            strcmp(g_bind_names[i], want) != 0)
            continue;
        if (!prefixKeys(E.key_binds[i].prefix_id, depth + 1, outer, sizeof(outer)))
            continue;
        snprintf(route, sizeof(route), "%s%s%s", outer, outer[0] ? " " : "",
                 E.key_binds[i].key_sequence);
        if (!found || strlen(route) < strlen(out)) {
            snprintf(out, size, "%s", route);
            found = 1;
        }
    }
    return found;
}

/**
 * @brief Fill g_command_keys: for each M-x candidate, the shortest key
 * sequence that runs it from no prefix, in the buffer M-x was opened from
 * (another language's bindings don't count).
 */
static void buildCommandKeys(void) {
    const struct buffer_t *buf = bufferFindById(splitScreenGetActivePane()->buffer_id);
    const int star = starPrefixId();
    char outer[WK_ENTRY_MAX];
    char full[WK_ENTRY_MAX];
    char **slot;
    int i;
    int lo;
    int hi;
    int mid;
    int cmp;
    if (g_command_count <= 0)
        return;
    g_command_keys = gcalloc((size_t) g_command_count, sizeof(char *));
    if (!g_command_keys)
        return;
    buildBindNames();
    if (!g_bind_names)
        return;
    for (i = 0; i < E.number_of_keybinds; i++) {
        if (!g_bind_names[i] || keyBindScope(&E.key_binds[i], buf) == 0)
            continue;
        if (E.key_binds[i].prefix_id == star)
            outer[0] = '\0';
        else if (!prefixKeys(E.key_binds[i].prefix_id, 0, outer, sizeof(outer)))
            continue;
        // g_commands is sorted: binary search for this binding's command
        lo = 0;
        hi = g_command_count - 1;
        slot = NULL;
        while (lo <= hi) {
            mid = (lo + hi) / 2;
            cmp = strcmp(g_commands[mid], g_bind_names[i]);
            if (cmp == 0) {
                slot = &g_command_keys[mid];
                break;
            }
            if (cmp < 0)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        if (!slot)
            continue;
        snprintf(full, sizeof(full), "%s%s%s", outer, outer[0] ? " " : "",
                 strcmp(E.key_binds[i].key_sequence, " ") == 0 ? "SPACE"
                                                                : E.key_binds[i].key_sequence);
        if (!*slot || strlen(full) < strlen(*slot)) {
            gfree(*slot);
            *slot = utilStrdup(full);
        }
    }
}

static int isWordStart(const char *name, const char *p) {
    return p == name || p[-1] == '-' || p[-1] == '_';
}

/**
 * @brief How well a command name matches what's typed at M-x: lower is
 * better, -1 for no match.
 * @details The name itself, then names starting with it (what M-x always
 * listed), then the rest holding the typed letters in order, tightest
 * first: letters that start a word earn a bonus, so "sv" finds
 * editor-save before dashboard-activate. A package's internals (name--part)
 * only show for what they start with, as before.
 */
static int commandScore(const char *name, const char *typed) {
    const size_t typed_len = strlen(typed);
    const char *s;
    const char *p;
    const char *q;
    int best = -1;
    int bonus;
    int score;
    if (typed_len == 0)
        return 0;
    if (strcmp(name, typed) == 0)
        return 0;
    if (strncmp(name, typed, typed_len) == 0)
        return 1;
    if (strstr(name, "--"))
        return -1;
    for (s = name; *s; s++) {
        if (*s != typed[0])
            continue;
        bonus = isWordStart(name, s);
        for (p = s + 1, q = typed + 1; *p && *q; p++)
            if (*p == *q) {
                bonus += isWordStart(name, p);
                q++;
            }
        if (*q)
            break; // and no later start can match either
        score = 2 + 2 * (int) (p - s) - 3 * bonus;
        if (score < 2)
            score = 2;
        if (best < 0 || score < best)
            best = score;
    }
    return best;
}

typedef struct {
    int index; // into g_commands
    int score;
} RankedCommand;

static int compareRanked(const void *a, const void *b) {
    const RankedCommand *x = a;
    const RankedCommand *y = b;
    if (x->score != y->score)
        return x->score - y->score;
    return x->index - y->index; // alphabetical among equals
}

/**
 * @brief The M-x candidates matching `typed`, best first, into a gcalloc'd
 * array the caller frees; *count gets its length. NULL when none match.
 */
static RankedCommand *rankCommands(const char *typed, int *count) {
    RankedCommand *ranked;
    int i;
    int score;
    *count = 0;
    if (!g_commands || g_command_count == 0)
        return NULL;
    ranked = gcalloc((size_t) g_command_count, sizeof(RankedCommand));
    if (!ranked)
        return NULL;
    for (i = 0; i < g_command_count; i++) {
        score = commandScore(g_commands[i], typed);
        if (score < 0)
            continue;
        ranked[*count].index = i;
        ranked[*count].score = score;
        (*count)++;
    }
    if (*count == 0) {
        gfree(ranked);
        return NULL;
    }
    qsort(ranked, (size_t) *count, sizeof(RankedCommand), compareRanked);
    return ranked;
}

char *whichKeyCommandPick(const char *typed) {
    RankedCommand *ranked;
    int count;
    int pick;
    char *name;
    ranked = rankCommands(typed, &count);
    if (!ranked)
        return NULL;
    pick = E.prompt_command_selected;
    if (pick >= count)
        pick = count - 1;
    if (pick < 0)
        pick = 0;
    name = utilStrdup(g_commands[ranked[pick].index]);
    gfree(ranked);
    return name;
}

void whichKeyBuildCommandList(void) {
    Lisp alist;
    int cap;
    Lisp it;
    Lisp pair;
    Lisp key;
    Lisp val;
    char **grown;
    int i;
    char *k;
    int j;
    whichKeyFreeCommandList();

    alist = lisp_table_to_alist(lisp_car(lisp_env(E.ctx)), E.ctx);

    cap = 64;
    g_commands = gcalloc((size_t) cap, sizeof(char *));
    if (!g_commands)
        return;

    for (it = alist; lisp_is_pair(it); it = lisp_cdr(it)) {
        pair = lisp_car(it);
        if (!lisp_is_pair(pair))
            continue;
        key = lisp_car(pair);
        val = lisp_cdr(pair);
        if (lisp_type(key) != LISP_SYMBOL)
            continue;
        if (lisp_type(val) != LISP_FUNC && lisp_type(val) != LISP_LAMBDA)
            continue;

        if (g_command_count >= cap) {
            cap *= 2;
            grown = grealloc(g_commands, (size_t) cap * sizeof(char *));
            if (!grown)
                break;
            g_commands = grown;
        }
        g_commands[g_command_count++] = utilStrdup(lisp_symbol_string(key));
    }

    // Alphabetical: the env walk yields hash-table order, which looks random.
    for (i = 1; i < g_command_count; i++) {
        k = g_commands[i];
        j = i - 1;
        while (j >= 0 && strcmp(g_commands[j], k) > 0) {
            g_commands[j + 1] = g_commands[j];
            j--;
        }
        g_commands[j + 1] = k;
    }
    buildCommandKeys();
}

void whichKeyFreeCommandList(void) {
    int count = g_command_count;
    freeStrings(&g_command_keys, &count);
    freeStrings(&g_commands, &g_command_count);
}

/**
 * @brief Width of a UTF-8 string in terminal columns.
 * @details Counting bytes instead would inflate every entry containing the
 * "→" separator by 2 (3 bytes, 1 column), which both overestimates the
 * column width — dropping a 2-column layout to 1 — and misaligns the
 * "%-*s" padding that assumes bytes == columns.
 * @note Combining marks and double-width CJK are not accounted for; entries
 * here are key names and command symbols, which are ASCII plus the arrow.
 */
static int displayWidth(const char *s) {
    int w = 0;
    const unsigned char *p;
    for (p = (const unsigned char *) s; *p; p++)
        if (*p == '\t')
            w += 2; // drawn as two spaces (see abAppendPadded)
        else if ((*p & 0xC0) != 0x80) // skip UTF-8 continuation bytes
            w++;
    return w;
}

/**
 * @brief Append `s` followed by enough spaces to fill `width` columns.
 * @details A tab in `s` splits it in two: it's drawn as two spaces, and
 * what follows it in `dim` (an SGR, then `base` again), for an M-x
 * entry's keys. NULL `dim` draws the tab as plain spaces.
 * @return columns actually written.
 */
static int abAppendPadded(struct abuf *ab, const char *s, const int width, const int budget,
                          const char *dim, const char *base) {
    int w = displayWidth(s);
    int bytes;
    int cols;
    const unsigned char *p;
    const unsigned char *start;
    int pad;
    int i;
    int dimmed = 0;
    if (w > budget) w = budget; // caller guarantees budget >= 0
    bytes = 0; cols = 0;
    start = (const unsigned char *) s;
    for (p = start; *p; p++) {
        if (*p == '\t') {
            abAppend(ab, (const char *) start, bytes);
            for (i = 0; i < 2 && cols < w; i++, cols++)
                abAppend(ab, " ", 1);
            if (dim) {
                ab_sgr(ab, dim);
                dimmed = 1;
            }
            start = p + 1;
            bytes = 0;
            continue;
        }
        if ((*p & 0xC0) != 0x80) {
            if (cols >= w) break;
            cols++;
        }
        bytes++;
    }
    abAppend(ab, (const char *) start, bytes);
    if (dimmed && base)
        ab_sgr(ab, base);
    pad = width - cols;
    if (pad > budget - cols) pad = budget - cols;
    for (i = 0; i < pad; i++) abAppend(ab, " ", 1);
    return cols + (pad > 0 ? pad : 0);
}

/**
 * @brief Draw `count` "label" strings as a multi-column grid filling the
 * bottom of the pane area, plus a title line.
 * @param selected an entry to highlight, or -1. When the list is longer
 * than the panel, the page holding it is shown.
 */
static void drawPanel(struct abuf *ab, const char *title,
                      char **entries, const int count, const int selected) {
    int longest;
    int i;
    int len;
    int col_w;
    int cols;
    int rows;
    int truncated;
    PopupStyle style;
    int top;
    int shown;
    int c;
    char header[192];
    int r;
    int used;
    int idx;
    int budget;
    int first;
    if (count <= 0)
        return;

    longest = 0;
    for (i = 0; i < count; i++) {
        len = displayWidth(entries[i]);
        if (len > longest)
            longest = len;
    }
    col_w = longest + 2;
    if (col_w < WK_MIN_COL_WIDTH)
        col_w = WK_MIN_COL_WIDTH;
    if (col_w > E.screencols)
        col_w = E.screencols;

    cols = E.screencols / col_w;
    if (cols < 1)
        cols = 1;

    rows = (count + cols - 1) / cols;
    truncated = 0;
    if (rows > WK_MAX_ROWS) {
        rows = WK_MAX_ROWS;
        truncated = 1;
    }

    popupStyleInit(&style);

    // Sit directly above the status bar, which is drawn at E.screenrows + 1.
    // -1 for the accent rule separating the panel from the buffer above it.
    top = E.screenrows - rows - 1;
    if (top < 1)
        top = 1;

    shown = rows * cols;
    if (shown > count)
        shown = count;
    first = selected > 0 ? selected / shown * shown : 0;

    // A thin accent-colored rule, so the panel reads as a distinct surface
    // dropped over the editor rather than text that happens to appear at
    // the bottom of it.
    ab_move(ab, top, 1);
    ab_sgr(ab, style.chrome);
    for (c = 0; c < E.screencols; c++) abAppend(ab, "─", 3);
    AB_RESET(ab);

    ab_move(ab, top + 1, 1);
    ab_sgr(ab, style.title);
    if (truncated && first + shown < count)
        snprintf(header, sizeof(header), " %s  (+%d more)", title,
                 count - first - shown);
    else
        snprintf(header, sizeof(header), " %s", title);
    abAppendPadded(ab, header, E.screencols, E.screencols, NULL, NULL);
    AB_RESET(ab);

    for (r = 0; r < rows; r++) {
        ab_move(ab, top + 2 + r, 1);
        ab_sgr(ab, style.row);
        used = 0;
        for (c = 0; c < cols; c++) {
            // Column-major so the list reads top-to-bottom, like Emacs'.
            idx = first + c * rows + r;
            if (idx >= count)
                break;
            budget = E.screencols - used;
            if (budget <= 0)
                break;
            if (idx == selected)
                ab_sgr(ab, style.row_sel);
            used += abAppendPadded(ab, entries[idx], col_w, budget, style.dim_fg,
                                   idx == selected ? style.row_sel : style.row);
            if (idx == selected) {
                AB_RESET(ab);
                ab_sgr(ab, style.row);
            }
        }
        for (; used < E.screencols; used++)
            abAppend(ab, " ", 1);
        AB_RESET(ab);
    }
}

/**
 * @brief M-x help: the commands matching what's been typed, best first,
 * each with the keys that run it; the one ENTER runs highlighted.
 */
static void drawCommandHelp(struct abuf *ab) {
    char *typed;
    RankedCommand *ranked;
    char **entries;
    char *storage;
    char *slot;
    const char *keys;
    int n;
    int i;
    int selected;
    char title[192];
    if (!g_commands || g_command_count == 0)
        return;

    typed = E.prompt_input ? (char *) utf8ToString(E.prompt_input) : NULL;
    ranked = rankCommands(typed ? typed : "", &n);
    if (!ranked) {
        gfree(typed);
        return;
    }
    entries = gcalloc((size_t) n, sizeof(char *));
    storage = gcalloc((size_t) n, WK_COMMAND_ENTRY_MAX);
    if (!entries || !storage) {
        gfree(entries);
        gfree(storage);
        gfree(ranked);
        gfree(typed);
        return;
    }
    for (i = 0; i < n; i++) {
        slot = storage + (size_t) i * WK_COMMAND_ENTRY_MAX;
        keys = g_command_keys ? g_command_keys[ranked[i].index] : NULL;
        snprintf(slot, WK_COMMAND_ENTRY_MAX, "%s%s%s", g_commands[ranked[i].index], keys ? "\t" : "",
                 keys ? keys : "");
        entries[i] = slot;
    }

    // Nothing typed yet: ENTER does nothing, so nothing is highlighted.
    selected = -1;
    if (typed && typed[0]) {
        selected = E.prompt_command_selected < n ? E.prompt_command_selected : n - 1;
        snprintf(title, sizeof(title),
                 " M-x — %d command%s matching %s  (ENTER runs the highlighted one, arrows choose)",
                 n, n == 1 ? "" : "s", typed);
    } else {
        snprintf(title, sizeof(title), " M-x — %d command%s", n, n == 1 ? "" : "s");
    }
    drawPanel(ab, title, entries, n, selected);

    gfree(entries);
    gfree(storage);
    gfree(ranked);
    gfree(typed);
}

/**
 * @brief Pending-prefix help: every key bound under the active prefix.
 */
static void drawPrefixHelp(struct abuf *ab) {
    char **entries;
    char *storage;
    int n;
    int i;
    char *slot;
    char title[128];
    const struct buffer_t *buf;
    int scope;
    int shadowed;
    int j;
    buildBindNames();
    if (!g_bind_names)
        return;

    entries = gcalloc((size_t) E.number_of_keybinds, sizeof(char *));
    storage = gcalloc((size_t) E.number_of_keybinds, WK_COMMAND_ENTRY_MAX);
    if (!entries || !storage) {
        gfree(entries);
        gfree(storage);
        return;
    }

    buf = bufferFindById(splitScreenGetActivePane()->buffer_id);
    n = 0;
    for (i = 0; i < E.number_of_keybinds; i++) {
        if (E.key_binds[i].prefix_id != E.prefix_state)
            continue;
        // Only what the key would actually run in this buffer: skip other
        // languages' bindings, and a global one a language binding shadows.
        scope = keyBindScope(&E.key_binds[i], buf);
        if (scope == 0)
            continue;
        shadowed = 0;
        for (j = 0; scope == 1 && j < E.number_of_keybinds && !shadowed; j++)
            shadowed = j != i && E.key_binds[j].prefix_id == E.prefix_state &&
                       strcmp(E.key_binds[j].key_sequence, E.key_binds[i].key_sequence) == 0 &&
                       keyBindScope(&E.key_binds[j], buf) == 2;
        if (shadowed)
            continue;
        slot = storage + (size_t) n * WK_COMMAND_ENTRY_MAX;
        snprintf(slot, WK_COMMAND_ENTRY_MAX, "%s → %s", E.key_binds[i].key_sequence,
                 g_bind_names[i] ? g_bind_names[i] : "?");
        entries[n++] = slot;
    }

    if (n > 0) {
        snprintf(title, sizeof(title), " %s — %d key%s (CTRL-g to cancel)",
                 E.prefix[E.prefix_state].prefix_name, n, n == 1 ? "" : "s");
        drawPanel(ab, title, entries, n, -1);
    }

    gfree(entries);
    gfree(storage);
}

void whichKeyDraw(struct abuf *ab) {
    EditorPane *active;
    struct buffer_t *buf;
    long now;
    char title[128];
    // TAB completion in a prompt with several matches: they're the answer
    // to "what can I type?", shown whatever the which-key setting.
    if (E.prompt_matches && E.prompt_match_count > 1) {
        if (E.prompt_match_selected >= 0)
            snprintf(title, sizeof(title), " %d/%d matches (TAB for the next)",
                     E.prompt_match_selected + 1, E.prompt_match_count);
        else
            snprintf(title, sizeof(title), " %d matches (TAB to cycle)",
                     E.prompt_match_count);
        drawPanel(ab, title, E.prompt_matches, E.prompt_match_count,
                  E.prompt_match_selected);
        return;
    }
    if (!E.constantes.WHICH_KEY)
        return;

    // The M-x prompt is explicitly asking "which command?", so its
    // candidate list shows straight away — no settle delay.
    if (E.prompt_is_command) {
        drawCommandHelp(ab);
        return;
    }

    // A prompt opened by a chord (CTRL-x f) runs with the prefix still
    // set: the keys under it are no help for what to type, and would
    // cover the prompt's own panel.
    if (E.prefix_state == 0 || E.prompt_input)
        return;

    // A terminal pane parks prefix_state on "term" for as long as it's
    // focused (nothing but ESCAPE is bound there, so the usual auto-reset
    // never fires). That's ambient state rather than a pending chord —
    // showing a permanent panel over the shell would just eat rows.
    active = splitScreenGetActivePane();
    buf = active ? bufferFindById(active->buffer_id) : NULL;
    if (buf && buf->type == TERMINAL_BUFF)
        return;
    // A sticky prefix (explorer, search, dired, ...) is the same kind of
    // ambient state: it's the mode of the buffer you're in, not a chord
    // waiting for its second key, so it never "settles" into needing help.
    if (E.prefix[E.prefix_state].sticky)
        return;

    // Only once the prefix has been pending a moment, so fluent chords
    // don't strobe the panel on their way through.
    now = monotonic_ms();
    if (E.prefix_state != E.which_key_prefix) {
        E.which_key_prefix = E.prefix_state;
        E.which_key_since = now;
    }
    if (now - E.which_key_since < WK_DELAY_MS)
        return;

    drawPrefixHelp(ab);
}
