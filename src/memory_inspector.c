/**
 * @file memory_inspector.c
 * @brief Debug-only buffer that lists every allocation currently tracked
 * by gutils (galloc/grealloc), for spotting leaks while the editor runs.
 * @details Same shape as project_search.c's results buffer: an ordinary
 * read-only scratch buffer (EXPLORER_BUFF) named "*memory*", singleton by
 * buffer id, re-rendered in place on refresh rather than recreated.
 */

#include "../include/memory_inspector.h"

#include "../include/buffer.h"
#include "../include/builtins.h"
#include "../include/data.h"
#include "../include/editor_op.h"
#include "../include/galloc.h"
#include "../include/input.h"
#include "../include/os.h"
#include "../include/project.h"
#include "../include/project_search.h"
#include "../include/split_screen.h"
#include "../include/utf8.h"
#include "../include/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern EditorConfig E;

#define MEMORY_BUFFER_NAME "*memory*"
#define MEMORY_PREFIX_NAME "memory"

static int s_memory_buffer_id = -1;

typedef enum {
  SORT_SIZE_DESC, // biggest first — the usual "what's eating memory" view
  SORT_COUNT_DESC, // most blocks first
  SORT_RECENCY,   // newest allocation first (gutils' own list order)
  SORT_LOCATION,  // grouped by file:line, then func
  SORT_MODE_COUNT
} SortMode;

typedef enum {
  VIEW_SITES,  // one row per allocation site (file:line), like the exit report
  VIEW_BLOCKS, // one row per block
} ViewMode;

static SortMode s_sort_mode = SORT_SIZE_DESC;
static ViewMode s_view_mode = VIEW_SITES;

static const char *sortModeLabel(const SortMode mode) {
  switch (mode) {
  case SORT_SIZE_DESC:
    return "size";
  case SORT_COUNT_DESC:
    return "count";
  case SORT_LOCATION:
    return "location";
  default:
    return "recency";
  }
}

static int compareText(const char *a, const char *b) {
  return strcmp(a ? a : "", b ? b : "");
}

static int compareSite(const char *fa, const int la, const char *fna,
                       const char *fb, const int lb, const char *fnb) {
  const int c = compareText(fa, fb);
  if (c != 0)
    return c;
  if (la != lb)
    return la < lb ? -1 : 1;
  return compareText(fna, fnb);
}

static int compareSizeDesc(const void *a, const void *b) {
  const struct dataBlock_t *ba = a;
  const struct dataBlock_t *bb = b;
  if (ba->size != bb->size)
    return ba->size > bb->size ? -1 : 1;
  return 0;
}

static int compareRecency(const void *a, const void *b) {
  const struct dataBlock_t *ba = a;
  const struct dataBlock_t *bb = b;
  if (ba->seq != bb->seq)
    return ba->seq > bb->seq ? -1 : 1;
  return 0;
}

static int compareLocation(const void *a, const void *b) {
  const struct dataBlock_t *ba = a;
  const struct dataBlock_t *bb = b;
  return compareSite(ba->file, ba->line, ba->func, bb->file, bb->line,
                     bb->func);
}

static void clearBufferRows(Buffer *buf) {
  int i;
  for (i = 0; i < buf->numrows; i++) {
    gfree(buf->row[i].characters);
    gfree(buf->row[i].attrs);
  }
  gfree(buf->row);
  buf->row = NULL;
  buf->numrows = 0;
  buf->x = 0;
  buf->y = 0;
}

static void addRow(Buffer *buf, const char *text) {
  Utf8Char *u = stringToUtf8((unsigned char *)text);
  if (!u)
    return;
  bufferInsertRow(buf, buf->numrows, u, utf8StringLen(u) - 1);
  gfree(u);
}

static Buffer *openMemoryBuffer(void) {
  Buffer *buf = bufferFindById(s_memory_buffer_id);
  Utf8Char *name;
  if (!buf || buf->type != EXPLORER_BUFF) {
    name = stringToUtf8((unsigned char *)MEMORY_BUFFER_NAME);
    if (!name)
      return NULL;
    s_memory_buffer_id = bufferCreate(name, READ_ONLY, EXPLORER_BUFF);
    gfree(name);
    buf = bufferFindById(s_memory_buffer_id);
    if (!buf)
      return NULL;
    snprintf(buf->focus_prefix, sizeof(buf->focus_prefix), "%s",
             MEMORY_PREFIX_NAME);
  }
  return buf;
}

// ─── Snapshot ───────────────────────────────────────────────────────────────

// Copies the tracked blocks into a plain array (gSnapshot) so they can be
// sorted and grouped without touching the real list, whose insertion order
// galloc/gfree depend on. Copies rather than pointers: in narval-gui SDL
// allocates through galloc too, possibly from its own threads, so a block
// may be freed while the snapshot is on screen.
// The inspector's own blocks (this array, the baseline, the row map) are
// left out: they'd show up as changes every time the view is drawn.
static struct dataBlock_t *collectBlocks(int *out_count) {
  // Room for blocks allocated between the count and the copy (this array
  // is one of them).
  const int cap = gBlockCount() + 64;
  struct dataBlock_t *arr = galloc(sizeof(struct dataBlock_t) * (size_t)cap);
  const int n = arr ? gSnapshot(arr, cap) : 0;
  int kept = 0;
  int i;
  for (i = 0; i < n; i++)
    if (!arr[i].file || strcmp(arr[i].file, __FILE__) != 0)
      arr[kept++] = arr[i];
  *out_count = kept;
  return arr;
}

// One allocation site: every live block allocated at the same file:line.
typedef struct {
  const char *file; // __FILE__ literals: they live as long as the program
  int line;
  const char *func;
  long long count;
  long long bytes;
  unsigned long long newest; // the newest block's seq, for SORT_RECENCY
  long long dcount;          // change since the baseline
  long long dbytes;
} Site;

static int compareSiteLocation(const void *a, const void *b) {
  const Site *x = a;
  const Site *y = b;
  return compareSite(x->file, x->line, x->func, y->file, y->line, y->func);
}

// Groups `blocks` (sorted by location here) into sites, sorted by
// location. `*out_n` gets their number; galloc'd, NULL if there are none.
static Site *groupSites(struct dataBlock_t *blocks, const int n, int *out_n) {
  Site *sites;
  Site *s = NULL;
  int nsites = 0;
  int i;
  *out_n = 0;
  if (n <= 0)
    return NULL;
  qsort(blocks, (size_t)n, sizeof(*blocks), compareLocation);
  sites = galloc(sizeof(Site) * (size_t)n);
  if (!sites)
    return NULL;
  for (i = 0; i < n; i++) {
    if (!s || compareSite(s->file, s->line, s->func, blocks[i].file,
                          blocks[i].line, blocks[i].func) != 0) {
      s = &sites[nsites++];
      s->file = blocks[i].file;
      s->line = blocks[i].line;
      s->func = blocks[i].func;
      s->count = 0;
      s->bytes = 0;
      s->newest = 0;
    }
    s->count++;
    s->bytes += (long long)blocks[i].size;
    if (blocks[i].seq > s->newest)
      s->newest = blocks[i].seq;
  }
  *out_n = nsites;
  return sites;
}

// ─── Baseline ───────────────────────────────────────────────────────────────
// "b" marks one: the sites as they were then, and the allocation sequence
// number. From then on the sites view shows only what changed since, and
// the blocks view only the blocks allocated since and still alive: what a
// leak leaves behind after an open/close round trip.

static Site *s_base_sites = NULL; // sorted by location
static int s_base_count = 0;
static unsigned long long s_base_seq = 0;
static int s_has_base = 0;

static void clearBaseline(void) {
  gfree(s_base_sites);
  s_base_sites = NULL;
  s_base_count = 0;
  s_base_seq = 0;
  s_has_base = 0;
}

static const Site *baseSite(const Site *s) {
  if (!s_base_sites)
    return NULL;
  return bsearch(s, s_base_sites, (size_t)s_base_count, sizeof(Site),
                 compareSiteLocation);
}

// Fills dcount/dbytes, and appends the baseline's sites that are gone
// now (their blocks all freed) with zero counts. `*n` grows by those;
// `sites` may move.
static Site *diffSites(Site *sites, int *n) {
  const Site *base;
  Site *grown;
  int i;
  const int before = *n;
  for (i = 0; i < before; i++) {
    base = baseSite(&sites[i]);
    sites[i].dcount = sites[i].count - (base ? base->count : 0);
    sites[i].dbytes = sites[i].bytes - (base ? base->bytes : 0);
  }
  for (i = 0; i < s_base_count; i++) {
    if (bsearch(&s_base_sites[i], sites, (size_t)before, sizeof(Site),
                compareSiteLocation))
      continue;
    grown = grealloc(sites, sizeof(Site) * (size_t)(*n + 1));
    if (!grown)
      break;
    sites = grown;
    sites[*n] = s_base_sites[i];
    sites[*n].count = 0;
    sites[*n].bytes = 0;
    sites[*n].dcount = -s_base_sites[i].count;
    sites[*n].dbytes = -s_base_sites[i].bytes;
    (*n)++;
  }
  return sites;
}

static void markBaseline(void) {
  int nblocks;
  int nsites;
  struct dataBlock_t *blocks;
  clearBaseline();
  s_base_seq = gSequence();
  blocks = collectBlocks(&nblocks);
  s_base_sites = groupSites(blocks, nblocks, &nsites);
  s_base_count = nsites;
  gfree(blocks);
  s_has_base = 1;
}

// ─── Rendering ──────────────────────────────────────────────────────────────
// One function writes the view, to the buffer or to an exported file, and
// remembers which site each buffer row stands for, for ENTER.

typedef struct {
  const char *file;
  int line;
} RowSource;

static RowSource *s_row_source = NULL; // one per row of the buffer
static int s_row_source_count = 0;

typedef struct {
  Buffer *buf; // rows go here, or
  FILE *f;     // here
} Sink;

static void emit(Sink *sink, const char *text, const char *file,
                 const int line) {
  RowSource *grown;
  if (sink->f) {
    fprintf(sink->f, "%s\n", text);
    return;
  }
  grown = grealloc(s_row_source,
                   sizeof(RowSource) * (size_t)(s_row_source_count + 1));
  if (grown) {
    s_row_source = grown;
    s_row_source[s_row_source_count].file = file;
    s_row_source[s_row_source_count].line = line;
    s_row_source_count++;
  }
  addRow(sink->buf, text);
}

static int compareSiteView(const void *a, const void *b) {
  const Site *x = a;
  const Site *y = b;
  const long long xb = s_has_base ? x->dbytes : x->bytes;
  const long long yb = s_has_base ? y->dbytes : y->bytes;
  const long long xc = s_has_base ? x->dcount : x->count;
  const long long yc = s_has_base ? y->dcount : y->count;
  switch (s_sort_mode) {
  case SORT_SIZE_DESC:
    if (xb != yb)
      return xb > yb ? -1 : 1;
    break;
  case SORT_COUNT_DESC:
    if (xc != yc)
      return xc > yc ? -1 : 1;
    break;
  case SORT_RECENCY:
    if (x->newest != y->newest)
      return x->newest > y->newest ? -1 : 1;
    break;
  default:
    break;
  }
  return compareSiteLocation(a, b);
}

static int compareBlockView(const void *a, const void *b) {
  switch (s_sort_mode) {
  case SORT_SIZE_DESC:
  case SORT_COUNT_DESC: // a block is one block: by size
    return compareSizeDesc(a, b);
  case SORT_LOCATION:
    return compareLocation(a, b);
  default:
    return compareRecency(a, b);
  }
}

static const char *orUnknown(const char *s) { return s ? s : "?"; }

static void renderSites(Sink *sink, struct dataBlock_t *blocks,
                        const int nblocks) {
  int nsites;
  int shown = 0;
  int i;
  char row[256];
  Site *sites = groupSites(blocks, nblocks, &nsites);
  if (s_has_base)
    sites = diffSites(sites, &nsites);
  if (sites)
    qsort(sites, (size_t)nsites, sizeof(Site), compareSiteView);

  if (s_has_base)
    snprintf(row, sizeof(row), "%9s %13s  %8s %12s  %s", "+blocks", "+bytes",
             "blocks", "bytes", "allocated at");
  else
    snprintf(row, sizeof(row), "%8s %12s  %s", "blocks", "bytes",
             "allocated at");
  emit(sink, row, NULL, 0);
  for (i = 0; i < nsites; i++) {
    if (s_has_base) {
      if (sites[i].dcount == 0 && sites[i].dbytes == 0)
        continue; // unchanged since the baseline
      snprintf(row, sizeof(row), "%+9lld %+13lld  %8lld %12lld  %s:%d (%s)",
               sites[i].dcount, sites[i].dbytes, sites[i].count,
               sites[i].bytes, orUnknown(sites[i].file), sites[i].line,
               orUnknown(sites[i].func));
    } else {
      snprintf(row, sizeof(row), "%8lld %12lld  %s:%d (%s)", sites[i].count,
               sites[i].bytes, orUnknown(sites[i].file), sites[i].line,
               orUnknown(sites[i].func));
    }
    emit(sink, row, sites[i].file, sites[i].line);
    shown++;
  }
  if (s_has_base && shown == 0)
    emit(sink, "(nothing changed since the baseline)", NULL, 0);
  gfree(sites);
}

static void renderBlocks(Sink *sink, struct dataBlock_t *blocks,
                         const int nblocks) {
  int i;
  int shown = 0;
  char row[256];
  qsort(blocks, (size_t)nblocks, sizeof(*blocks), compareBlockView);
  for (i = 0; i < nblocks; i++) {
    if (s_has_base && blocks[i].seq <= s_base_seq)
      continue; // older than the baseline
    snprintf(row, sizeof(row), "%-14p %8zu bytes  %s:%d (%s)",
             blocks[i].address, blocks[i].size, orUnknown(blocks[i].file),
             blocks[i].line, orUnknown(blocks[i].func));
    emit(sink, row, blocks[i].file, blocks[i].line);
    shown++;
  }
  if (s_has_base && shown == 0)
    emit(sink, "(no block allocated since the baseline is still alive)", NULL,
         0);
}

// The header, the view, in that order.
static void render(Sink *sink) {
  int nblocks;
  int i;
  long long total = 0;
  long long base_total = 0;
  long long heap_blocks;
  long long heap_bytes;
  char line[256];
  struct dataBlock_t *blocks = collectBlocks(&nblocks);

  for (i = 0; i < nblocks; i++)
    total += (long long)blocks[i].size;
  for (i = 0; i < s_base_count; i++)
    base_total += s_base_sites[i].bytes;

  snprintf(line, sizeof(line), "%d tracked block%s, %lld bytes — %s, by %s",
           nblocks, nblocks == 1 ? "" : "s", total,
           s_view_mode == VIEW_SITES ? "per site" : "per block",
           sortModeLabel(s_sort_mode));
  emit(sink, line, NULL, 0);
  if (s_has_base) {
    snprintf(line, sizeof(line),
             "since the baseline: %+lld bytes%s", total - base_total,
             s_view_mode == VIEW_SITES
                 ? ", sites that changed"
                 : ", blocks allocated since and still alive");
    emit(sink, line, NULL, 0);
  }
  snprintf(line, sizeof(line),
           "+ %zu bytes of galloc bookkeeping, not in the total",
           gTrackerOverhead());
  emit(sink, line, NULL, 0);
  if (os_heap_in_use(&heap_blocks, &heap_bytes) == 0) {
    if (heap_blocks >= 0)
      snprintf(line, sizeof(line),
               "process heap: %lld blocks, %lld bytes in use, tracked or not",
               heap_blocks, heap_bytes);
    else
      snprintf(line, sizeof(line),
               "process heap: %lld bytes in use, tracked or not", heap_bytes);
    emit(sink, line, NULL, 0);
  }
  if (sink->buf) {
    emit(sink,
         "ENTER source, v sites/blocks, s sort, b baseline, B clear it, "
         "r refresh, w export, CTRL-x leave",
         NULL, 0);
  }
  emit(sink, "", NULL, 0);

  if (s_view_mode == VIEW_SITES)
    renderSites(sink, blocks, nblocks);
  else
    renderBlocks(sink, blocks, nblocks);
  gfree(blocks);
}

static void renderBuffer(Buffer *buf) {
  Sink sink;
  clearBufferRows(buf);
  gfree(s_row_source);
  s_row_source = NULL;
  s_row_source_count = 0;
  sink.buf = buf;
  sink.f = NULL;
  render(&sink);
  buf->dirty = 0;
}

// ─── Commands ───────────────────────────────────────────────────────────────

void memoryInspectorOpen(void) {
  Buffer *buf = openMemoryBuffer();
  EditorPane *active;
  struct prefix_t prefix;
  if (!buf)
    return;
  renderBuffer(buf);

  active = splitScreenGetActivePane();
  active->buffer_id = s_memory_buffer_id;
  active->cursor_y = buf->y;
  active->cursor_x = 0;
  active->y_offset = 0;
  active->x_offset = 0;

  prefix = find_prefix(MEMORY_PREFIX_NAME);
  if (prefix.prefix_id != -1)
    E.prefix_state = prefix.prefix_id;

  editorSetStatusMessage("%d tracked block%s — ENTER source, b baseline, "
                         "CTRL-x to leave",
                         gBlockCount(), gBlockCount() == 1 ? "" : "s");
}

void memoryInspectorRefresh(void) {
  Buffer *buf = bufferFindById(s_memory_buffer_id);
  EditorPane *active;
  if (!buf || buf->type != EXPLORER_BUFF)
    return;
  renderBuffer(buf);

  // The block count (and so buf->numrows) can shrink between refreshes.
  // Without this, the active pane's cursor/scroll from before the refresh
  // can point past the new row count and read out of bounds — unlike
  // memoryInspectorOpen(), which starts these at 0 already.
  active = splitScreenGetActivePane();
  if (active->buffer_id == s_memory_buffer_id) {
    active->cursor_y = buf->y;
    active->cursor_x = 0;
    active->y_offset = 0;
    active->x_offset = 0;
  }

  editorSetStatusMessage("%d tracked block%s", gBlockCount(),
                         gBlockCount() == 1 ? "" : "s");
}

void memoryInspectorCycleSort(void) {
  s_sort_mode = (SortMode)((s_sort_mode + 1) % SORT_MODE_COUNT);
  memoryInspectorRefresh();
}

void memoryInspectorToggleView(void) {
  s_view_mode = s_view_mode == VIEW_SITES ? VIEW_BLOCKS : VIEW_SITES;
  memoryInspectorRefresh();
}

void memoryInspectorMarkBaseline(void) {
  Buffer *buf = bufferFindById(s_memory_buffer_id);
  // Without the view's own rows, as every render counts (it clears them
  // first): otherwise they'd read as freed since the baseline.
  if (buf && buf->type == EXPLORER_BUFF)
    clearBufferRows(buf);
  markBaseline();
  memoryInspectorRefresh();
  editorSetStatusMessage("baseline marked: from now on, only what changes");
}

void memoryInspectorClearBaseline(void) {
  clearBaseline();
  memoryInspectorRefresh();
  editorSetStatusMessage("baseline cleared");
}

// `file` as __FILE__ gave it (relative to the build directory with meson)
// made absolute; NULL if no such file is found. galloc'd.
static char *sourcePath(const char *file) {
  char joined[4096];
  char *path = os_realpath(file); // absolute already, or relative to cwd
  if (path)
    return path;
#if defined(APP_DEBUG) && defined(NARVAL_BUILD_DIR)
  snprintf(joined, sizeof(joined), "%s/%s", NARVAL_BUILD_DIR, file);
  path = os_realpath(joined);
  if (path)
    return path;
#endif
  // Last resort: the project root, without the build dir's "../".
  while (strncmp(file, "../", 3) == 0)
    file += 3;
  path = projectRootOrCwd();
  if (!path)
    return NULL;
  snprintf(joined, sizeof(joined), "%s/%s", path, file);
  gfree(path);
  return os_realpath(joined);
}

void memoryInspectorGoto(void) {
  EditorPane *active = splitScreenGetActivePane();
  Buffer *buf = bufferFindById(active->buffer_id);
  const RowSource *src;
  char *path;
  int id;
  Buffer *target;
  int line;
  if (!buf || active->buffer_id != s_memory_buffer_id)
    return;
  if (buf->y < 0 || buf->y >= s_row_source_count)
    return;
  src = &s_row_source[buf->y];
  if (!src->file) {
    editorSetStatusMessage("ENTER on a block or site row opens its source");
    return;
  }
  if (src->line <= 0) {
    editorSetStatusMessage("%s: allocated inside a library, no source here",
                           src->file);
    return;
  }
  path = sourcePath(src->file);
  if (!path) {
    editorSetStatusMessage("can't find %s", src->file);
    return;
  }

  // Into the other pane when split, so the list stays on screen; as with
  // ENTER in *search*, never onto the explorer sidebar.
  if (splitScreenFocusOtherRealPane())
    active = splitScreenGetActivePane();
  id = projectSearchOpenFile(path);
  target = bufferFindById(id);
  if (!target) {
    editorSetStatusMessage("could not open %s", path);
    gfree(path);
    return;
  }
  active->buffer_id = id;
  line = src->line - 1;
  if (line >= target->numrows)
    line = target->numrows > 0 ? target->numrows - 1 : 0;
  target->y = line;
  target->x = 0;
  E.prefix_state = 0;
  editorSetStatusMessage("%s:%d", src->file, src->line);
  gfree(path);
}

void memoryInspectorExport(void) {
  Utf8Char *input =
      editorPrompt("Export memory snapshot to: %u (ESC to cancel)", NULL, 0);
  char *path;
  Sink sink;
  if (!input)
    return;

  path = (char *)utf8ToString(input);
  gfree(input);
  if (!path)
    return;
  if (!*path) {
    gfree(path);
    return;
  }

  sink.buf = NULL;
  sink.f = fopen(path, "w");
  if (!sink.f) {
    editorSetStatusMessage("could not open %s for writing", path);
    gfree(path);
    return;
  }
  render(&sink);
  fclose(sink.f);

  editorSetStatusMessage("wrote the %s view to %s",
                         s_view_mode == VIEW_SITES ? "sites" : "blocks", path);
  gfree(path);
}
