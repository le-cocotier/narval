/**
 * @file test_config_reload.c
 * @brief Unit tests for reloading the config (editorReloadConfig(),
 * init.c): what a reload forgets, what it loads again, and how it reports
 * errors.
 * @details Runs initEditor() on a config of its own in the directory
 * meson.build gives it (NARVAL_HOME, NARVAL_DATA_DIR and HOME all point
 * there, in the build directory), then rewrites init.lisp and reloads.
 */

#include "test.h"
#include "../include/builtins.h"
#include "../include/config_load.h"
#include "../include/galloc.h"
#include "../include/init.h"
#include "../include/lisp.h"
#include "../include/lisp_gc.h"
#include "../include/os.h"
#include "../include/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_dir[OS_PATH_MAX];

static const char *INIT_V1 =
    "(define TAB-LENGTH 4)\n"
    "(define GUI-FONT-SIZE 20)\n"
    "(define my-old-var 1)\n"
    "(define (narval-version) 'replaced)\n"
    "(add-prefix \"user\")\n"
    "(add-prefix \"mine\" #t)\n"
    "(map-key \"CTRL-x\" '(editor-set-prefix \"user\") \"no-prefix\")\n"
    "(map-key \"z\" '(editor-set-prefix \"mine\") \"user\")\n"
    "(map-key \"q\" '(editor-set-prefix \"no-prefix\") \"mine\")\n"
    "(editor-set-theme-color \"background\" 1 2 3)\n"
    "(lsp-server \"zz\" \"zz\" \"zz-ls\" \"--stdio\")\n"
    "(add-hook \"config-reloaded\" (lambda () '()))\n"
    "(status-bar-add-segment \"mine\" (lambda () \"x\"))\n"
    "(add-package \"pk\")\n";

// No TAB-LENGTH, no "mine", no theme, no package; one form that fails.
static const char *INIT_V2 =
    "(define my-new-var 2)\n"
    "(add-prefix \"user\")\n"
    "(map-key \"y\" editor-save \"user\")\n"
    "(this-function-does-not-exist)\n";

static void writeFile(const char *rel, const char *text) {
  char path[OS_PATH_MAX];
  char parent[OS_PATH_MAX];
  char err[256];
  FILE *f;
  if ((size_t)snprintf(path, sizeof(path), "%s/%s", s_dir, rel) >= sizeof(path))
    exit(1);
  utilDirname(path, parent, sizeof(parent));
  os_mkdirs(parent, err, sizeof(err));
  f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "can't write %s\n", path);
    exit(1);
  }
  fputs(text, f);
  fclose(f);
}

static Lisp eval(const char *code) {
  LispError error = LISP_ERROR_NONE;
  Lisp result = lisp_eval(lisp_read(code, &error, E.ctx), &error, E.ctx);
  return error == LISP_ERROR_NONE ? result : lisp_make_string2("<error>", E.ctx);
}

static int defined(const char *name) {
  int present = 0;
  lisp_env_lookup(lisp_env(E.ctx), lisp_make_symbol(name, E.ctx), &present);
  return present;
}

static int prefixId(const char *name) {
  int i;
  for (i = 0; i <= E.number_of_prefix; i++)
    if (!strcmp(E.prefix[i].prefix_name, name))
      return E.prefix[i].prefix_id;
  return -1;
}

static int bound(const char *key, const char *prefix) {
  const int id = prefixId(prefix);
  int i;
  for (i = 0; i < E.number_of_keybinds; i++)
    if (E.key_binds[i].prefix_id == id && !strcmp(E.key_binds[i].key_sequence, key))
      return 1;
  return 0;
}

static int hooksFor(const char *event) {
  int i;
  int n = 0;
  for (i = 0; i < E.number_of_hooks; i++)
    n += !strcmp(E.hooks[i].event_name, event);
  return n;
}

static void testFirstLoad(void) {
  CHECK_INT(configErrorCount(), 0);
  CHECK_INT(E.constantes.TAB_LENGTH, 4);
  CHECK_INT(E.constantes.GUI_FONT_SIZE, 20);
  CHECK(defined("my-old-var"));
  CHECK(defined("pk-function"));
  CHECK(lisp_is_true(eval("(package-loaded? \"pk\")")));
  CHECK(lisp_type(eval("(narval-version)")) != LISP_STRING); // replaced
  CHECK(bound("z", "user"));
  CHECK(bound("q", "mine"));
  CHECK(prefixId("mine") > 3);
  CHECK(strstr(E.theme.BACKGROUND_COLOR, "2;1;2;3m") != NULL);
  CHECK_INT(E.number_of_lsp_servers, 1);
  CHECK_INT(E.number_of_languages, 1);
  CHECK_INT(hooksFor("config-reloaded"), 1);
}

static void testReloadSameConfig(void) {
  int mine = prefixId("mine");
  // In the sticky "mine" prefix when the reload happens: still in it after.
  E.prefix_state = mine;
  CHECK_INT(editorReloadConfig(), 0);
  CHECK_INT(prefixId("mine"), mine);
  CHECK_INT(E.prefix_state, mine);
  // Everything registered once, not twice.
  CHECK_INT(E.number_of_lsp_servers, 1);
  CHECK_INT(E.number_of_languages, 1);
  CHECK_INT(hooksFor("config-reloaded"), 1);
  CHECK(bound("z", "user"));
  CHECK_INT(E.constantes.TAB_LENGTH, 4);
  // The package was evaluated again (pk/init.lisp sets this afresh).
  CHECK(defined("pk-function"));
  CHECK_INT(lisp_int(eval("pk-loads")), 1);
  E.prefix_state = 0;
}

static void testReloadChangedConfig(void) {
  Lisp first;
  writeFile("config/init.lisp", INIT_V2);
  CHECK_INT(editorReloadConfig(), 1);
  CHECK_INT(configErrorCount(), 1);

  // Settings init.lisp no longer sets are back to their defaults.
  CHECK_INT(E.constantes.TAB_LENGTH, 2);
  CHECK_INT(E.constantes.GUI_FONT_SIZE, 14);
  CHECK_INT(E.global_constantes.TAB_LENGTH, 2);

  // Definitions: the old ones are gone, the new ones are there, and the
  // builtin the old config had replaced is the builtin again.
  CHECK(!defined("my-old-var"));
  CHECK(!defined("pk-function"));
  CHECK(defined("my-new-var"));
  CHECK(defined("map-key"));
  first = eval("(narval-version)");
  CHECK(lisp_type(first) == LISP_STRING);

  // Keys and prefixes: only what the new config binds (and, since it had
  // an error, the emergency bindings).
  CHECK(!bound("z", "user"));
  CHECK_INT(prefixId("mine"), -1);
  CHECK(bound("y", "user"));
  CHECK(bound("ENTER", "no-prefix"));
  CHECK(bound("CTRL-x", "no-prefix"));

  // Registrations: none left.
  CHECK(strstr(E.theme.BACKGROUND_COLOR, "2;40;44;52m") != NULL);
  CHECK_INT(E.number_of_lsp_servers, 0);
  CHECK_INT(E.number_of_languages, 0);
  CHECK_INT(hooksFor("config-reloaded"), 0);
  CHECK(lisp_type(eval("(package-loaded? \"pk\")")) == LISP_BOOL &&
        !lisp_is_true(eval("(package-loaded? \"pk\")")));
}

static void testReloadBack(void) {
  writeFile("config/init.lisp", INIT_V1);
  CHECK_INT(editorReloadConfig(), 0);
  CHECK_INT(configErrorCount(), 0); // the last load's errors only
  CHECK_INT(E.constantes.TAB_LENGTH, 4);
  CHECK(defined("pk-function"));
  CHECK(lisp_is_true(eval("(package-loaded? \"pk\")")));
  CHECK_INT(E.number_of_languages, 1);
  CHECK(bound("z", "user"));
  CHECK(!bound("y", "user"));
}

// (config-reload) only asks: the reload happens at the run loop's next
// tick, where editorRunPendingConfigReload() is called.
static void testDeferred(void) {
  writeFile("config/init.lisp", INIT_V2);
  CHECK(lisp_is_true(eval("(config-reload)")));
  CHECK(defined("my-old-var")); // not yet
  editorRunPendingConfigReload();
  CHECK(!defined("my-old-var"));
  CHECK(defined("my-new-var"));
  editorRunPendingConfigReload(); // nothing pending: does nothing
  CHECK(defined("my-new-var"));
}

static void collect(void) {
  int i;
  // Something to move: garbage first, so the live objects change place.
  for (i = 0; i < 2000; i++)
    eval("(list 1 2 3 \"garbage\")");
  lisp_collect(lisp_null(), E.ctx);
}

int main(void) {
  const char *home = getenv("NARVAL_HOME");
  testInit();
  gfree(E.status_msg); // initEditor() allocates its own
  E.status_msg = NULL;
  if (!home || !*home) {
    fprintf(stderr, "NARVAL_HOME isn't set (meson.build sets it)\n");
    return 1;
  }
  snprintf(s_dir, sizeof(s_dir), "%s", home);
  writeFile("config/init.lisp", INIT_V1);
  writeFile("packages/pk/init.lisp",
            "(define pk-loads 1)\n"
            "(define (pk-function) 'pk)\n"
            "(syntax-define-language \"pk\" \"#\" \"\" \"\" \"\\\"\")\n");

  E.batch = 1;
  initEditor();
  // A collection between each step, as the run loop does between ticks:
  // the snapshot a reload restores has to survive it (lisp_gc.c's roots).
  testFirstLoad();
  collect();
  testReloadSameConfig();
  collect();
  testReloadChangedConfig();
  collect();
  testReloadBack();
  collect();
  testDeferred();
  return testDone();
}
