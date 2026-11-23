#ifndef NARVAL_STATUS_BAR_H
#define NARVAL_STATUS_BAR_H

#include "data.h"

// The status bar (status_bar.c): the second-to-last screen row, built
// from segments init.lisp can reorder, drop or add to with
// (status-bar-segments left right) and (status-bar-add-segment name fn).

// Draws it at its row (E.screenrows + 1). Called by editorRefreshScreen().
void editorDrawStatusBar(struct abuf *ab);

// Registers status-bar-segments and status-bar-add-segment.
void statusBarRegisterBuiltins(void);

// The Lisp functions of the added segments, for the garbage collector.
void statusBarVisitLispRoots(LispRootVisitor visit, LispContext ctx);

#endif // NARVAL_STATUS_BAR_H
