#pragma once

#include <list.h>
#include <stack.h>

typedef struct {
    sanchor sanc;
} block;
typedef block lineage;

typedef const struct type{
    const char *const name;
    const size size;
    checked err (*linref_up)(volatile void *l, const struct type *t);
    void (*linref_down)(volatile void *l);
    void (*lin_init)(lineage *b);
} type;
#define TYPE(t, lu, ld, li) {#t, sizeof(t), lu, ld, li}

typedef struct heritage{
    lfstack slabs;
    lfstack *free_slabs;
    cnt max_slabs;
    cnt slab_alloc_batch;
    type *t;
    struct slab *(*new_slabs)(cnt nslabs);
} heritage;
#define HERITAGE(t, ms, sab, ns, override...)       \
    {LFSTACK, &shared_free_slabs, ms, sab, t, ns, override}
#define KERN_HERITAGE(t) HERITAGE(t, 16, 2, new_slabs)
#define POSIX_HERITAGE(t) KERN_HERITAGE(t)

typedef struct tyx tyx;

typedef volatile struct slabfooter{
    sanchor sanc;
    stack free_blocks;
    cnt contig_blocks;
    heritage *volatile her;
    volatile struct align(sizeof(dptr)) tyx {
        type *t;
        iptr linrefs;
    } tx;
    lfstack hot_blocks;
} slabfooter;

#define MAX_BLOCK (SLAB_SIZE - sizeof(slabfooter))

typedef struct align(SLAB_SIZE) slab{
    u8 blocks[MAX_BLOCK];
    slabfooter;
} slab;

#define SLAB {.free_blocks = STACK, .hot_blocks = LFSTACK}
/* TODO */
/* #define pudef (slab, "(slab){%, refs:%, contig:%, free:%, hot:%}",   \ */
/*                a->tx.t, a->tx.linrefs, a->contig_blocks,                \ */
/*                stack_size(&a->free_blocks),                             \ */
/*                lfstack_size(&a->hot_blocks)) */
/* #include <pudef.h> */

extern lfstack shared_free_slabs;

dbg extern iptr slabs_used;
dbg extern cnt bytes_used;

typedef void (*linit)(void *);

/* If ret != 0 and no subsequent linfree(l) has been called, then
   !linref_up(l, h->t) and linalloc(h') != ret for all h'. */
checked void *linalloc(heritage *h);
void linfree(lineage *l);

/* If !ret and no subseqent linref_down(l) has been called, then for the
   maximal set H of heritages s.t h->t == t for h in H:
   - linalloc(h') != l for h' not in H.
   - linref_up(l, t') != 0 for all t' != t.

   Also, t->lin_init(l) was called and no nalloc function has written to
   the t->size - sizeof(lineage) bytes following l. (Even if linfree(l)
   was called)

   If you use this right, you can figure that l has the type you associate
   with t.
*/
checked err linref_up(volatile void *l, type *t);
void linref_down(volatile void *l);

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
   linref_up(...) or fake_linref_up(...) by the calling thread T was
   followed by a [fake_]linref_down(...) by T. */
void linref_account_close(linref_account *a);

/* Good for balancing linref accounts when ref ownership moves between
   threads. */
err fake_linref_up(void);
void fake_linref_down(void);

typedef struct{
    int linrefs_held;
} nalloc_info;
#define NALLOC_INFO {}

#define linref_account(balance, e...)({                 \
        linref_account laccount = (linref_account){};   \
        linref_account_open(&laccount);                 \
        typeof(e) account_expr = e;                     \
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
        typeof(e) account_expr = e;                     \
        baccount.baseline += balance;                   \
        byte_account_close(&baccount);                  \
        account_expr;                                   \
    })                                                  \


#define pudef (type, "(typ){%}", a->name)
#include <pudef.h>
/* TODO */
/* #define pudef (heritage, "(her){%, nslabs:%}", a->t, a->slabs.size) */
/* #include <pudef.h> */

#undef LOG_NALLOC
#define LOG_NALLOC 0

#define smalloc(as...) trace(NALLOC, 1, smalloc, as)
#define sfree(as...) trace(NALLOC, 1, sfree, as)
#define malloc(as...) trace(NALLOC, 1, malloc, as)
#define smalloc(as...) trace(NALLOC, 1, smalloc, as)
#define realloc(as...) trace(NALLOC, 1, realloc, as)
#define calloc(as...) trace(NALLOC, 1, calloc, as)
#define free(p) trace(NALLOC, 1, free, (void *) p)
#define linalloc(as...) trace(NALLOC, 1, linalloc, as)
#define linfree(as...) trace(NALLOC, 1, linfree, as)
        
        
