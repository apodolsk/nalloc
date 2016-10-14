#pragma once

#include <list.h>
#include <stack.h>

typedef struct {
    sanchor sanc;
} aliasing block;
typedef block lineage;

typedef const struct type{
    const char *const name;
    const size size;
    void (*lin_init)(lineage *b);
    bool (*has_special_ref)(const volatile void *l, bool up);
} type;
#define TYPE(t, li, hsr) {#t, sizeof(t), li, hsr}

typedef struct heritage{
    lfstack slabs;
    lfstack *free_slabs;
    cnt nslabs;
    cnt max_slabs;
    cnt slab_alloc_batch;
    type *t;
    struct slab *(*new_slabs)(cnt nslabs);
} heritage;
#define HERITAGE(t, ms, sab, ns, override...)       \
    {LFSTACK, &shared_free_slabs, 0, ms, sab, t, ns, override}
#define KERN_HERITAGE(t) HERITAGE(t, 16, 2, new_slabs)
#define POSIX_HERITAGE(t) KERN_HERITAGE(t)

typedef struct tyx tyx;

typedef volatile struct slabfooter{
    volatile struct align(sizeof(dptr)) tyx {
        type *t;
        iptr linrefs;
    } tx;
    sanchor sanc;
    stack local_blocks;
    cnt contig_blocks;
    heritage *volatile her;
    align(CACHELINE_SIZE)
    lfstack hot_blocks;
} slabfooter;
#define SLABFOOTER {.local_blocks = STACK, .hot_blocks = LFSTACK}
#define MAX_BLOCK (SLAB_SIZE - sizeof(slabfooter))

typedef struct align(SLAB_SIZE) slab{
    u8 blocks[MAX_BLOCK];
    union{
        /* Clang doesn't support -fplan9-extensions, so I emulate the part I
           like with an anonymous union and -fms-extensions. */
        struct slabfooter;
        slabfooter slabfooter;
    };
} slab;

#define MIN_ALIGN (sizeof(lineage))

extern lfstack shared_free_slabs;

dbg extern iptr slabs_used;
dbg extern cnt bytes_used;

typedef void (*linit)(void *);

/* Documentation conventions:
   - Time is a little tricky to talk about. Luckily, on x86, each function
     has a single completion point and a single order of completions is
     observed by all threads.
   - Every group of statements about a function is implicitly prefaced
     with:
     "For any call C returning ret and taking the named arguments, at all
      times after C completes and in all threads"
      true for all
     calls returning ret and taking Statements about a function reference the arguments and return value
     of a particular invocation. However, they're true at any time after
     the call returned.
   - Present tense refers to any time after the
   - */

/* Documentation will be a little weird because I'm trying to talk very
   precisely. */

/* Let type *t = h->t.

   Let in_obj(uptr p, uptr o, size_t s) == 1 iff o <= p < o + s.
   
   If ret and linfree(ret) didn't subsequently complete, then:
   - For any void *p | in_obj(p, ret, t->size):
     - linalloc(h') != p for any heritage *h'.
     - !linref_up(p, h->t).
     - t->lin_init(ret) returned and no nalloc function subsequently
       wrote to p.
     - in_obj(p, heap_start(), heap_end() - heap_start())
*/
checked void *linalloc(heritage *h);
void linfree(lineage *l);

/* Undefined iff:
   - heap_start() <= l <= heap_end() but l is on a page last mapped
     outside of nalloc.

   Otherwise:
   If !ret and more linref_up(l, t) calls returned 0 than linref_down(l,
   t) calls completed, then EITHER:
   - There exists void *o | in_obj(o, l, t->size) and:
     - If linalloc(h) == o, h->t == t.
     - For any void *p | in_obj(o, p, t->size):
       - !linref_up(p, t') iff t' == t.
     - t->lin_init(o) returned and no nalloc function subsequently wrote
       to memory between o + sizeof(lineage) and o + t->size.
   - OR !t->has_special_ref(l, true)

   If ret, linfree(o) must have completed for similarly defined o.

   In other words, linref_up(l, t) says:
   - l is inside an object that's been initialized according to t.
   - Every thread will agree that l "has type t".
   - If threads cooperate to maintain invariants of t in o, then nalloc
     won't ruin this by clobbering any part of o except its lineage
     header. This is true even if o is freed and reallocated.

   - A few subtleties:
     - *Every* byte of an object allocated with type t "has" type t. This
       means that each function on a data structure using container_of()
       and embedded traversal fields can't just call
       linref_up("node_type"). Instead, e.g. callers of lflist_enq(l) must
       pass the type * of the objects whose traversal fields are enqueued
       on the list. This type must be the same for all such objects.
     - There's no useful guarantee about when linref_up(p, t) must
       fail. In particular, it can succeed even though p was never ever
       allocated. This is because every byte of a *slab* has the same
       type, even the never-allocated ones.
*/
checked err linref_up(const volatile void *l, type *t);

/* Undefined unless, just before the call, there were more linref_up(l, t)
   calls which returned 0 than there were completed linref_down(l, t) calls.
*/
void linref_down(const volatile void *l, type *t);

checked void *smalloc(size size);
void sfree(void *b, size size);
checked void *malloc(size size);
void free(void *b);
void *calloc(size nb, size bs);
void *realloc(void *o, size size);

void nalloc_profile_report(void);

typedef struct{
    int baseline;
} linref_account;
void linref_account_open(linref_account *a);

/* Asserts that, since the last linref_account_open(a), every
   linref_up(...) or fake_linref_up(...) in T was followed by a unique
   [fake_]linref_down(...) in T. */
void linref_account_close(linref_account *a);

/* Good for balancing linref accounts when ref ownership moves between
   threads, or when has_special_ref() does something wonky. */
err fake_linref_up(void);
void fake_linref_down(void);

typedef struct{
    int linrefs_held;
} nalloc_tls;
#define NALLOC_TLS {}

#define linref_account(balance, e...)({                 \
        linref_account laccount = (linref_account){};   \
        linref_account_open(&laccount);                 \
        __auto_type account_expr = e;                   \
        laccount.baseline += balance;                   \
        linref_account_close(&laccount);                \
        account_expr;                                   \
    })                                                  \

typedef struct{
    cnt baseline;
} byte_account;
void byte_account_open(byte_account *a);
void byte_account_close(byte_account *a);

#define byte_account(balance, e...)({                   \
        byte_account baccount = (byte_account){};       \
        byte_account_open(&baccount);                   \
        __auto_type account_expr = e;                   \
        baccount.baseline += balance;                   \
        byte_account_close(&baccount);                  \
        account_expr;                                   \
    })                                                  \


#define pudef (struct type, "(typ){%}", a->name)
#include <pudef.h>
#define pudef (heritage, "(her){%}", a->t)
#include <pudef.h>

#define smalloc(as...) trace(NALLOC, 1, smalloc, as)
#define sfree(as...) trace(NALLOC, 1, sfree, as)
#define malloc(as...) trace(NALLOC, 1, malloc, as)
#define smalloc(as...) trace(NALLOC, 1, smalloc, as)
#define realloc(as...) trace(NALLOC, 1, realloc, as)
#define calloc(as...) trace(NALLOC, 1, calloc, as)
#define free(p) trace(NALLOC, 1, free, (void *) p)
#define linalloc(as...) trace(NALLOC, 1, linalloc, as)
#define linfree(as...) trace(NALLOC, 1, linfree, as)
        
#ifndef LOG_NALLOC
#define LOG_NALLOC 0
#endif
