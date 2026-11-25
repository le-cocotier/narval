#ifndef CONFIG_LOAD_H_
#define CONFIG_LOAD_H_

#include "lisp.h"

/**
 * Loading Lisp config files (config/init.lisp, every package's init.lisp)
 * one top-level form at a time, so a mistake in one form is reported with
 * its file and line and the rest of the file still loads.
 */

/**
 * Reads and evaluates the Lisp file at `path`, form by form. `label` is how
 * the file is named in error messages (e.g. "init.lisp",
 * "packages/git/init.lisp"). A file that can't be opened counts as one
 * error. Returns the number of errors this call recorded.
 * `read_to_end` (may be NULL) is set to 1 when every form of the file was
 * at least tried, 0 when loading stopped early: the file couldn't be
 * opened, or a ( or " was never closed and the rest of it was skipped.
 */
int configLoadFile(const char *path, const char *label, int *read_to_end);

/**
 * Records one error message (printf-style), shown by configShowErrors(),
 * and writes it to the debug log. Once the editor is up (an add-package
 * run from M-x or a project's config, say) it goes to the status bar
 * right away too.
 */
void configReportError(const char *fmt, ...);

/** Number of errors recorded since startup. */
int configErrorCount(void);

/**
 * Makes the recorded errors visible: the first one goes to the status bar
 * and the full list into a "*config-errors*" buffer. Does nothing when
 * there were none.
 */
void configShowErrors(void);

/** Frees the recorded messages. */
void configErrorsFree(void);

/**
 * While set, configReportError() only records an error (and logs it): a
 * reload shows them all at the end with configShowErrors() instead of one
 * status message per error.
 */
void configSetQuiet(int quiet);

/**
 * Remembers the global Lisp environment and macro table as they are before
 * any config has run: the builtins and the Lisp library. Call once, after
 * initBuiltins() and before init.lisp is loaded.
 */
void configSnapshotEnv(void);

/**
 * Puts the global environment and macro table back to that snapshot, so a
 * reload starts from what a fresh start would: whatever the config
 * (define)d is gone, and a builtin it redefined is the builtin again. The
 * environment keeps its identity (closures, project envs, buffer locals
 * still refer to it); only its table is replaced. Returns 0, or -1 when
 * no snapshot was taken.
 */
int configRestoreEnv(void);

/** The snapshot's tables, for the garbage collector (lisp_gc.c). */
void configVisitLispRoots(LispRootVisitor visit, LispContext ctx);

#endif // CONFIG_LOAD_H_
