#ifndef GALLOC_H_
#define GALLOC_H_

#include <stdio.h>

// galloc/grealloc are function-like macros so every call site automatically
// records where it allocated from — no changes needed at any of the ~270
// call sites across the codebase. Call the _impl functions directly only
// from inside gutils.c itself (see grealloc_impl's NULL-address fallback),
// where re-expanding the macro would attribute the allocation to gutils.c
// instead of the real caller.
void * galloc_impl(size_t size, const char * file, int line, const char * func);
void * gcalloc_impl(size_t num, size_t size, const char * file, int line, const char * func);
void * grealloc_impl(void * address, size_t size, const char * file, int line, const char * func);
void gfree(void * data);
// strdup() through galloc, attributed to the caller like galloc().
char * gstrdup_impl(const char * s, const char * file, int line, const char * func);

/**
 * Writes every block still tracked to `out`, grouped by the line that
 * allocated it (count, bytes, file:line, function), biggest first, then a
 * total. Returns the number of blocks. narval calls it at exit in APP_DEBUG
 * builds (~/.narval/memory-at-exit.log); call it anywhere else to see what
 * is allocated at that moment.
 */
int gReportMemory(FILE * out);

#define galloc(size) galloc_impl((size), __FILE__, __LINE__, __func__)
#define gcalloc(num, size) gcalloc_impl((num), (size), __FILE__, __LINE__, __func__)
#define grealloc(address, size) grealloc_impl((address), (size), __FILE__, __LINE__, __func__)
#define gstrdup(s) gstrdup_impl((s), __FILE__, __LINE__, __func__)

/*
  Struct that contains addresses of allocated memory.
  Linked list
*/

struct dataBlock_t {
  void * address;
  struct dataBlock_t * lastBlock;
  struct dataBlock_t * nextBlock;
  size_t size;
  const char * file;   // __FILE__ at the (re)allocation call site
  int line;             // __LINE__ at the (re)allocation call site
  const char * func;    // __func__ at the (re)allocation call site
  struct dataBlock_t * hashNext; // gutils.c's address index; not for inspection
};

// Most-recently-allocated tracked block, or NULL. Walk older blocks via
// ->lastBlock, holding gLock(). For read-only inspection — nothing about
// the list itself is safe to mutate through this pointer.
struct dataBlock_t * gBlocks(void);

// Number of blocks currently tracked (galloc/grealloc calls not yet
// matched by a gfree).
int gBlockCount(void);

// Copies up to `max` tracked blocks into `out`, newest first, under the
// lock, and returns how many it copied. Use this rather than walking
// gBlocks() when another thread may be allocating (narval-gui: SDL's
// allocations come through galloc too, see gui_sdl.c). The copies'
// list pointers are not meant to be followed.
int gSnapshot(struct dataBlock_t * out, int max);

// Bytes galloc spends on its own bookkeeping (a dataBlock_t per block and
// the address index), which no block's size includes.
size_t gTrackerOverhead(void);

// The lock every galloc call takes. Hold it to walk gBlocks() safely, and
// don't allocate or free while holding it.
void gLock(void);
void gUnlock(void);

#endif
