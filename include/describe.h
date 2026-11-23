#ifndef NARVAL_DESCRIBE_H
#define NARVAL_DESCRIBE_H

#include "data.h"

// Self-documentation (describe.c): what a key runs, what is bound where.

// (describe-key): reads a key sequence, following prefixes (CTRL-x ...),
// and says in the status bar what it runs.
Lisp describeKey(Lisp args, LispError *e, LispContext ctx);
// (key-bindings): every binding, as a list of (prefix key command)
// strings, in the order they're matched. describe-bindings (init.lisp)
// shows it in a buffer.
Lisp keyBindings(Lisp args, LispError *e, LispContext ctx);
// (command-names) — every function in the global environment, sorted,
// except the core Lisp library's (map, assoc, ...): the editor's commands
// and the ones packages and the config define.
Lisp commandNames(Lisp args, LispError *e, LispContext ctx);
// Records the names defined so far as the core library, for command-names
// to leave out. Called once, right after the library loads.
void commandNamesMarkLibrary(void);
// (describe-function name): whether `name` is a built-in or a Lisp
// function (with its definition), and the keys bound to it, as a list of
// lines for a help buffer.
Lisp describeFunction(Lisp args, LispError *e, LispContext ctx);

#endif // NARVAL_DESCRIBE_H
