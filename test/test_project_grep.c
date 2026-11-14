/**
 * @file test_project_grep.c
 * @brief Unit tests for projectGrep() (project_search.c), the search
 * behind (project-grep) and the dashboard's TODO list, and for the state
 * names (lsp-clients) reports
 * @details Works in a scratch directory under the system's temporary one,
 * so the search doesn't land inside narval's own repository. Without git, the walker searches;
 * with it, a repository made on the spot checks the git grep path too.
 */

#include "test.h"
#include "../include/builtins.h"
#include "../include/galloc.h"
#include "../include/os.h"
#include "../include/project_search.h"

#include <stdlib.h>

static char s_scratch[1024];

static int writeFile(const char *dir, const char *name, const char *text) {
  char path[1200];
  FILE *f;
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  f = fopen(path, "wb");
  if (!f)
    return 0;
  fputs(text, f);
  fclose(f);
  return 1;
}

static void makeTree(const char *dir) {
  char err[256];
  char sub[1100];
  os_delete_tree(dir, err, sizeof(err));
  snprintf(sub, sizeof(sub), "%s/src", dir);
  CHECK_INT(os_mkdirs(sub, err, sizeof(err)), 0);
  CHECK(writeFile(dir, "main.c",
                  "int main(void) {\n"
                  "\t// TODO: parse the arguments\n"
                  "  return 0;\n"
                  "}\n"));
  CHECK(writeFile(dir, "src/util.c",
                  "/* nothing here */\n"
                  "\n"
                  "    /* TODO   tabs\tand spaces */\n"
                  "int f(void); // FIXME later\n"));
  { // fputs would stop at the NUL
    char path[1200];
    FILE *f;
    snprintf(path, sizeof(path), "%s/notes.bin", dir);
    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
      fwrite("TODO\0binary\n", 1, 12, f);
      fclose(f);
    }
  }
}

/** @brief The hit for `suffix`, or NULL. */
static const ProjectGrepHit *hitFor(const ProjectGrepHit *hits, int n, const char *suffix) {
  size_t len = strlen(suffix);
  size_t plen;
  int i;
  for (i = 0; i < n; i++) {
    plen = strlen(hits[i].abs_path);
    if (plen >= len && strcmp(hits[i].abs_path + plen - len, suffix) == 0)
      return &hits[i];
  }
  return NULL;
}

static void checkTodos(const char *root) {
  ProjectGrepHit *hits = NULL;
  const ProjectGrepHit *hit;
  int n = projectGrep("TODO", root, -1, &hits);
  CHECK_INT(n, 2); // not in the binary file
  hit = hitFor(hits, n, "/main.c");
  CHECK(hit != NULL);
  if (hit) {
    CHECK_INT(hit->line, 2);
    CHECK_STR(hit->text, "// TODO: parse the arguments"); // leading tab dropped
    CHECK(os_path_is_absolute(hit->abs_path));
  }
  hit = hitFor(hits, n, "/src/util.c");
  CHECK(hit != NULL);
  if (hit) {
    CHECK_INT(hit->line, 3);
    CHECK_STR(hit->text, "/* TODO   tabs and spaces */"); // a tab is one space
  }
  projectGrepFree(hits, n);

  // Literal, not a regex: "f(void)" matches itself only.
  n = projectGrep("f(void)", root, -1, &hits);
  CHECK_INT(n, 1);
  projectGrepFree(hits, n);

  // max caps the hits.
  n = projectGrep("TODO", root, 1, &hits);
  CHECK_INT(n, 1);
  projectGrepFree(hits, n);

  n = projectGrep("nowhere to be found", root, -1, &hits);
  CHECK_INT(n, 0);
  CHECK(hits == NULL);

  // A trailing slash on the root changes nothing.
  {
    char slashed[1100];
    snprintf(slashed, sizeof(slashed), "%s/", root);
    n = projectGrep("FIXME", slashed, -1, &hits);
    CHECK_INT(n, 1);
    if (n == 1)
      CHECK(strstr(hits[0].abs_path, "//") == NULL);
    projectGrepFree(hits, n);
  }
}

static void testPlainTree(void) {
  char dir[1100];
  char err[256];
  snprintf(dir, sizeof(dir), "%s/plain", s_scratch);
  makeTree(dir);
  checkTodos(dir);
  os_delete_tree(dir, err, sizeof(err));
}

static void testGitTree(void) {
#ifndef _WIN32
  char dir[1100];
  char command[1400];
  char err[256];
  ProjectGrepHit *hits = NULL;
  int n;
  if (system("git --version >/dev/null 2>&1") != 0)
    return; // no git: the walker test above covers the search
  snprintf(dir, sizeof(dir), "%s/repo", s_scratch);
  makeTree(dir);
  snprintf(command, sizeof(command),
           "cd '%s' && git init -q . && git add main.c src/util.c notes.bin", dir);
  CHECK_INT(system(command), 0);
  checkTodos(dir);
  // A file not added yet is searched; an ignored one isn't.
  CHECK(writeFile(dir, "scratch.c", "// TODO untracked\n"));
  CHECK(writeFile(dir, "ignored.c", "// TODO ignored\n"));
  CHECK(writeFile(dir, ".gitignore", "ignored.c\n"));
  n = projectGrep("TODO", dir, -1, &hits);
  CHECK_INT(n, 3);
  CHECK(hitFor(hits, n, "/scratch.c") != NULL);
  CHECK(hitFor(hits, n, "/ignored.c") == NULL);
  projectGrepFree(hits, n);
  os_delete_tree(dir, err, sizeof(err));
#endif
}

static void testEmptyPattern(void) {
  ProjectGrepHit *hits = NULL;
  CHECK_INT(projectGrep("", s_scratch, -1, &hits), -1);
  CHECK_INT(projectGrep(NULL, s_scratch, -1, &hits), -1);
  CHECK(hits == NULL);
}

static void testLspStateNames(void) {
  CHECK_STR(lspStateName(LSP_NOT_STARTED), "off");
  CHECK_STR(lspStateName(LSP_INITIALIZING), "starting");
  CHECK_STR(lspStateName(LSP_READY), "ready");
  CHECK_STR(lspStateName(LSP_SHUTTING_DOWN), "stopping");
  CHECK_STR(lspStateName(LSP_SHUTDOWN), "stopped");
}

int main(void) {
  const char *tmp = getenv("TMPDIR");
  char err[256];
#ifdef _WIN32
  if (!tmp || !*tmp)
    tmp = getenv("TEMP");
#endif
  if (!tmp || !*tmp)
    tmp = "/tmp";
  testInit();
  snprintf(s_scratch, sizeof(s_scratch), "%s", tmp);
  while (strlen(s_scratch) > 1 && s_scratch[strlen(s_scratch) - 1] == '/')
    s_scratch[strlen(s_scratch) - 1] = '\0'; // macOS's TMPDIR ends in one
  snprintf(s_scratch + strlen(s_scratch), sizeof(s_scratch) - strlen(s_scratch),
           "/narval-project-grep-%ld", os_getpid());
  CHECK_INT(os_mkdirs(s_scratch, err, sizeof(err)), 0);
  testEmptyPattern();
  testPlainTree();
  testGitTree();
  testLspStateNames();
  os_delete_tree(s_scratch, err, sizeof(err));
  return testDone();
}
