#ifndef PROJECT_SEARCH_H_
#define PROJECT_SEARCH_H_

// ─── Project-wide search ────────────────────────────────────────────────────
// Grep the whole project, put the hits in a buffer, and jump from a hit to
// the place in the file it came from. The results buffer is an ordinary
// read-only scratch buffer (EXPLORER_BUFF), so it scrolls, splits and shows
// up in the buffer list like anything else — the only special thing about
// it is that focusing it activates the "search" prefix, where ENTER means
// "go to the result under the cursor".

// Prompts for a pattern, searches from the project root down, fills the
// "*search*" buffer with the hits and shows it in the active pane.
void projectSearchPrompt(void);

// Runs a search without prompting (same result buffer and display).
// `root` may be NULL, meaning the current project (the working directory
// without one).
// Returns the number of matches, or -1 if the search itself failed.
int projectSearchRun(const char *pattern, const char *root);

// Jumps to whatever result the cursor is sitting on in the results buffer.
// A no-op on a header row, on a stale row (the results were replaced by a
// newer search), or when the active buffer isn't the results buffer.
// Opens the file in the *other* pane when the screen is split, so the
// results stay visible; otherwise in place.
void projectSearchGotoCursor(void);

// One entry of a location list shown in the results buffer.
typedef struct {
  const char *abs_path;
  int line;         // 1-based
  int col;          // 0-based character the jump lands on
  const char *text; // what to show for it (the line's text, a message, ...)
} SearchLocation;

// Shows a list of locations (LSP references, diagnostics, ...) in the same
// "*search*" buffer, with the same ENTER-to-jump, under `title`.
void projectSearchShowLocations(const char *title, const SearchLocation *locs, int n);

// The files (absolute paths, galloc'd, in grep's order) containing
// `pattern`, searched like projectSearchRun() but without touching the
// "*search*" results. Returns how many, or -1. Free each path and the array.
int projectSearchFiles(const char *pattern, const char *root, char ***out_files);

// The id of a buffer showing `abs_path`, opening the file if no buffer has
// it yet. -1 if it can't be opened.
int projectSearchOpenFile(const char *abs_path);

// One hit of projectGrep(): galloc'd strings, freed by projectGrepFree().
typedef struct {
  char *abs_path;
  int line;   // 1-based
  char *text; // the line, leading blanks dropped, tabs as spaces, capped
} ProjectGrepHit;

// The lines containing `pattern` (literally), searched like
// projectSearchRun() — git grep in a repository (untracked files
// included, ignored ones not), the project walker elsewhere — but
// returned instead of shown: "*search*" is left alone.
// At most `max` hits (negative: the usual cap). `root` NULL or "": the
// current project (the working directory without one). Returns how many
// are in *out_hits, or -1 for an empty pattern or no root.
int projectGrep(const char *pattern, const char *root, int max, ProjectGrepHit **out_hits);
void projectGrepFree(ProjectGrepHit *hits, int n);

// Frees the stored results. Called on shutdown.
void projectSearchFree(void);

#endif
