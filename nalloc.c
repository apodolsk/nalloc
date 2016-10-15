/* See nalloc.h first. */

/* To save some jumping around:

   ----TYPES:
   
   uptr, cnt, idx - uintptr_t
   dptr - __int128_t on x86-64
   err - int. 0 is the absence of error.
   
   ----MACROS:

   EARG/EARG() = -1

   PUN(t, e):
     Expands to a value of type t whose representation is the
     reinterpreted representation of e.
   
   cof(p, container_t, field):
     Expands to container_t *ret | if p, &ret->field == p, else ret == NULL.

   cof_aligned_pow2(p, t):
     Expands to p aligned down to a multiple of sizeof(t). sizeof(t) must
     be power of 2.

   rup(orig, changes...):
     Functional-style record update. Expands to ret | ret == val for each
     arg of the form ".field = val" in changes, and the fields of ret and
     orig are otherwise equal. orig will be unmodified, except as a side
     effect of evaluating a val.

   mustp/must(e):
     Like assert(e), except e is evaluated even with debugging off.

   ----'FUNCTIONS':

   cas2_won(dptr n, volatile dptr *p, dptr *old):
     A wrapper around __atomic_compare_exchange(p, old, n, ...).
     Arguments are punned to dptr[*] via macro wrapper.
     (Exists mainly because __atomic_compare_exchange() is still
     miscompiled on non-integral types. gcc will ICE and clang will
     complain about C structs being non-trivially-copyable.)
   
   bool lfstack_clear_cas_won(uptr new_gen, lfstack *p, lfstack *old):
     Atomically set *p = (lfstack){.top = NULL, .gen=new_gen} iff *p ==
     *old, setting *old = *p upon failure.
     new_gen is punned to uptr.
   
   bool lfstack_push_cas_won(sanchor *new_top, uptr new_gen,
                             lfstack *p, lfstack *old)
     Atomically push new_top to p and set p->gen = new_gen iff *p == old,
     setting *old = *p upon failure.
     new_gen is punned to uptr.
                             
 */

#define MODULE NALLOC

#include <stack.h>
#include <list.h>
#include <nalloc.h>
#include <thread.h>

/* Hides nalloc from certain libc functions. Useful for debugging. */
#pragma GCC visibility push(hidden)

#define LINREF_ACCOUNT_DBG 0
#define NALLOC_MAGIC_INT 0x01FA110C
#define LINREF_VERB 2

static slab *slab_new(heritage *h);
static void slab_ref_down(slab *s);
static cnt slab_max_blocks(const slab *s);

static block *alloc_from_slab(slab *s, heritage *h);
static bool slab_fully_hot(const slab *s);
static err recover_hot_blocks(slab *s);
static bool fills_slab(cnt blocks, size bs);

static slab *slab_of(const block *b);
static u8 *blocks_of(slab *s);

static err write_magics(block *b, size bytes);         
static err magics_valid(block *b, size bytes);

static void profile_upd_free(size s);
static void profile_upd_alloc(size s);

#define slab_new(as...) trace(NALLOC, 2, slab_new, as)
#define slab_ref_down(as...) trace(NALLOC, LINREF_VERB, slab_ref_down, as)

lfstack shared_free_slabs = LFSTACK;

dbg iptr slabs_in_use;
dbg iptr total_slabs_used;

dbg cnt bytes_in_use;
dbg cnt max_bytes_in_use;

/* Note the MAP and ITERATE macros. Cool, huh? */
#define MALLOC_TYPE(s, ...) {#s, s, NULL, NULL}
static const type malloctypes[] = { MAP(MALLOC_TYPE, _,
        16, 32, 48, 64, 80, 96, 112, 128,
        192, 256, 384, 512, 1024, MAX_BLOCK)
};

#define MALLOC_HERITAGE(i, ...)                 \
    HERITAGE(&malloctypes[i], 32, 1, new_slabs)
static heritage malloc_heritages[] = {
    ITERATE(MALLOC_HERITAGE, _, 14)
};

/* Fetch a slab s from h->slabs or else allocate a new one. Since all
   slabs on h->slabs must contain a free block, it must be possible to
   allocate from s in both cases.

   To preserve this h->slabs property, don't return s to h->slabs iff it
   becomes empty after allocation. Instead, mark it as lost, "leak" it,
   and let the next linfree() onto s return it to h->slabs.

   There must be a "next" linfree() onto s because the process of checking
   for emptiness and marking s is atomic. (If it weren't, all blocks on s
   might be freed between the check and the mark.)

   The key to this atomicity is that, until s is marked as lost, blocks
   will be returned only to s->hot_blocks. If s->contig_blocks and
   s->local_blocks are empty, then s is empty iff s->hot_blocks is
   empty. A single CAS suffices to mark s->hot_blocks lost iff it's empty.
*/
void *(linalloc)(heritage *h){
    if(poisoned())
        return NULL;
    
    slab *s = cof(lfstack_pop(&h->slabs), slab, sanc);
    if(!s && !(s = slab_new(h)))
        return EOOR(), NULL;

    block *b = alloc_from_slab(s, h);
    if(!slab_fully_hot(s) || !recover_hot_blocks(s))
        lfstack_push(&s->sanc, &h->slabs);
    else
        must(xadd(-1, &h->nslabs));

    assert(b);
    assert(aligned_pow2(b, MIN_ALIGN));
    assert(profile_upd_alloc(h->t->size), 1);
    
    return b;
}

/* It's also true that every slab on h->slabs has a free block in
   s->contig_blocks or s->local_blocks. alloc_from_slab() exploits this,
   as does linfree(). Luckily, it's easy to preserve this because it's
   optimal to clear out s->hot_blocks when checking for emptiness in
   recover_hot_blocks().
*/
static
block *(alloc_from_slab)(slab *s, heritage *h){
    if(s->contig_blocks)
        return (void *) &blocks_of(s)[h->t->size * --s->contig_blocks];
    return mustp(cof(stack_pop(&s->local_blocks), block, sanc));
}

static
bool slab_fully_hot(const slab *s){
    return !s->contig_blocks && !stack_peek(&s->local_blocks);
}

typedef struct{
    uptr lost:1;
    uptr size:WORDBITS - 1;
} hotst;

static
err (recover_hot_blocks)(slab *s){
    assert(!PUN(hotst, lfstack_gen(&s->hot_blocks)).lost);
    struct lfstack h = lfstack_read(&s->hot_blocks);
    while(!lfstack_clear_cas_won((hotst){.lost = !lfstack_peek(&h)},
                                 &s->hot_blocks, &h))
        continue;
    if(!lfstack_peek(&h))
        return EARG;
    s->local_blocks = lfstack_convert(&h);
    return 0;
}

/* Find the slab s containing b. Push b to s->hot_blocks iff s isn't lost,
   and clear s's lost flag otherwise. I refer to the flag here as
   "s->lost".

   If clearing s->lost, decide whether to s should be freed or returned to
   its heritage.

   In the first case:

   Restart, pushing b to s->hot_blocks like a normal linfree(). The thread
   which frees the last block in s will free s. There's a "last" block
   because all blocks will be in s->hot_blocks and s->hot_blocks.gen
   stores an always-accurate size field. The "last" is the one whose push
   fills s->hot_blocks, according to the size field.

   All blocks will be in s->hot_blocks because only linalloc() adds blocks
   to other sets. But no linalloc() will find s in a heritage until s is
   freed because:
   - Let T1 be the time you cleared s->lost.
   - s was empty and thus not on a heritage at T1, and only a linfree()
     which clears s->lost (or frees s) could add it to a heritage. So some
     other linfree() must have cleared s->lost. Let T2 be the first time
     this happened.
   - Only linalloc() sets s->lost, so some linalloc() L must have done so
     between T1 and T2.
   - L must have found s in a heritage after T1 (*), so some linfree()
     between T1 and T2 must have added s to a heritage and thus cleared
     s->lost. But T2 was the first time this happened.
     - (*) needs a bit more proof. 

   In the second case:

   Push b to s->local_blocks, push s to s->her->slabs, and return. The
   subtlety is that neither stack_push() nor lfstack_push() allow
   concurrent pushes of the same node. Luckily, no other thread will write
   to s->sanc, b->sanc, or s->local_blocks until you finish.

   This is because:
   - s won't be freed until you finish because you didn't add b to
     s->hot_blocks.
   - So, as in the other case, no linalloc() will find s in a heritage.
   - As in the other case, threads in linfree() will write only to
     s->hot_blocks until s is freed or a linalloc() sets s->lost.

*/
void (linfree)(lineage *l){
    block *b = l;
    *b = (block){SANCHOR};

    slab *s = slab_of(b);
    heritage *her = s->her;
    assert(profile_upd_free(s->tx.t->size), 1);
    
    for(struct lfstack h = lfstack_read(&s->hot_blocks);;){
        hotst st = PUN(hotst, lfstack_gen(&h));
        if(!st.lost){
            if(!lfstack_push_cas_won(&l->sanc, rup(st, .size++),
                                     &s->hot_blocks, &h))
                continue;
            if(fills_slab(st.size + 1, s->tx.t->size)){
                assert(!stack_peek(&s->local_blocks));
                
                s->contig_blocks = st.size + 1;
                s->hot_blocks = (lfstack) LFSTACK;
                slab_ref_down(s);
            }
            return;
        }else if(!lfstack_clear_cas_won((hotst){}, &s->hot_blocks, &h))
            continue;

        assert(!stack_peek(&s->local_blocks));
        if(xadd_iff_less(1, &her->nslabs, her->max_slabs) < her->max_slabs)
            break;
        h = (lfstack) LFSTACK;
    }
        
    stack_push(&b->sanc, &s->local_blocks);
    lfstack_push(&s->sanc, &her->slabs);
}

/* Avoids division. Subtracts bs to handle padding between last block and
   footer. */
static bool fills_slab(cnt blocks, size bs){
    assert(blocks * bs <= MAX_BLOCK);
    return blocks * bs > MAX_BLOCK - bs;
}

static
heritage *malloc_heritage_of(size size){
    for(uint i = 0; i < ARR_LEN(malloctypes); i++)
        if(malloctypes[i].size >= size)
            return &malloc_heritages[i];
    EWTF("Size is assumed < MAX_BLOCK, but ");
}

void *(malloc)(size size){
    if(!size)
        return TODO(), NULL;
    if(size > MAX_BLOCK)
        return TODO(), NULL;
    block *b = (linalloc)(malloc_heritage_of(size));
    if(b)
        assertl(2, magics_valid(b, malloc_heritage_of(size)->t->size));
    return b;
}

void (free)(void *b){
    lineage *l = (lineage *) b;
    if(!b)
        return;
    assertl(2, write_magics(l, slab_of(l)->tx.t->size));
    (linfree)(l);
}

/* Fairly straightforward. 

   Note that slab allocations are batched. 

   Note also that block initialization is batched. linref_up() is entirely
   slab-oriented, so h->lin_init() sets up "type invariants" on all blocks
   before linref_up() is allowed to succeed on the slab. This could be
   avoided through careful use of contig_blocks, but I don't do this.
*/
static
slab *(slab_new)(heritage *h){
    slab *s = cof(lfstack_pop(h->free_slabs), slab, sanc);
    if(!s){
        s = h->new_slabs(h->slab_alloc_batch);
        if(!s)
            return NULL;
        assert(xadd(h->slab_alloc_batch, &total_slabs_used), 1);
        assert(aligned_pow2(s, SLAB_SIZE));
        
        s->slabfooter = (slabfooter) SLABFOOTER;
        for(slab *si = s + 1; si != &s[h->slab_alloc_batch]; si++){
            si->slabfooter = (slabfooter) SLABFOOTER;
            lfstack_push(&si->sanc, h->free_slabs);
        }
    }
    assert(xadd(1, &slabs_in_use) >= 0);
    assert(!s->tx.linrefs);
    assert(!lfstack_peek(&s->hot_blocks));
    
    s->her = h;
    if(s->tx.t != h->t){
        s->tx = (tyx){h->t};
        
        cnt nb = s->contig_blocks = slab_max_blocks(s);
        if(h->t->lin_init)
            for(cnt b = 0; b < nb; b++)
                h->t->lin_init((void *) &blocks_of(s)[b * h->t->size]);
        else
            for(cnt b = 0; b < nb; b++)
                assertl(2, write_magics((block *) &blocks_of(s)[b * h->t->size],
                                        h->t->size));
    }
    s->tx.linrefs = 1;

    xadd(1, &h->nslabs);
    return s;
}

static
void (slab_ref_down)(slab *s){
    assert(s->tx.t);
    if(must(xadd((uptr) -1, &s->tx.linrefs)) == 1){
        assert(!lfstack_peek(&s->hot_blocks));
        assert(xadd(-1, &slabs_in_use));
        lfstack_push(&s->sanc, s->her->free_slabs);
    }
}

err (linref_up)(const volatile void *l, type *t){
    assert(l);
    if(t->has_special_ref(l, true))
        return if_dbg(T->nallocin.linrefs_held++),
               0;
    if(l < heap_start() || l > heap_end())
        return EARG();
    
    slab *s = slab_of((void *) l);
    for(tyx tx = s->tx;;){
        if(tx.t != t || !tx.linrefs)
            return EARG("Wrong type.");
        log(LINREF_VERB, "linref up! % % %", l, t, tx.linrefs);
        assert(tx.linrefs > 0);
        if(cas2_won(((tyx){t, tx.linrefs + 1}), &s->tx, &tx))
            return if_dbg(T->nallocin.linrefs_held++),
                   0;
    }
}

void (linref_down)(const volatile void *l, type *t){
    assert(T->nallocin.linrefs_held--);
    if(!t->has_special_ref(l, false))
        slab_ref_down(slab_of((void *) l));
}

static
cnt slab_max_blocks(const slab *s){
    return MAX_BLOCK / s->tx.t->size;
}

static constfun 
slab *(slab_of)(const block *b){
    assert(b);
    return cof_aligned_pow2((void *) b, slab);
}

static constfun 
u8 *(blocks_of)(slab *s){
    return s->blocks;
}

void *(smalloc)(size size){
    return (malloc)(size);
};

void (sfree)(void *b, size size){
    assert(slab_of(b)->tx.t->size >= size);
    (free)(b);
}

void *(calloc)(size nb, size bs){
    u8 *b = (malloc)(nb * bs);
    if(b)
        memset(b, 0, nb * bs);
    return b;
}

void *(realloc)(void *o, size size){
    u8 *b = (malloc)(size);
    if(!b)
        return NULL;
    if(o)
        memcpy(b, o, size);
    free(o);
    return b;
}

static
int write_magics(block *b, size bytes){
    int *magics = (int *) (b + 1);
    for(size i = 0; i < (bytes - sizeof(*b))/sizeof(*magics); i++)
        magics[i] = NALLOC_MAGIC_INT;
    return 1;
}

static
int magics_valid(block *b, size bytes){
    int *magics = (int *) (b + 1);
    for(size i = 0; i < (bytes - sizeof(*b))/sizeof(*magics); i++)
        assert(magics[i] == NALLOC_MAGIC_INT);
    return 1;
}

void profile_upd_alloc(size s){
    cnt u = xadd(s, &bytes_in_use);
    for(cnt mb = max_bytes_in_use; mb < u;)
        if(cas_won(u, &max_bytes_in_use, &mb))
            break;
}

void profile_upd_free(size s){
    xadd(-s, &bytes_in_use);
}

void nalloc_profile_report(void){
    ppl(0, total_slabs_used, slabs_in_use, bytes_in_use, max_bytes_in_use);
}

err fake_linref_up(void){
    assert(T->nallocin.linrefs_held++, 1);
    return 0;
}

void fake_linref_down(void){
    assert(T->nallocin.linrefs_held--);
}

void linref_account_open(linref_account *a){
    assert(a->baseline = T->nallocin.linrefs_held, 1);
}

void linref_account_close(linref_account *a){
    if(LINREF_ACCOUNT_DBG)
        assert(T->nallocin.linrefs_held == a->baseline);
}

void byte_account_open(byte_account *a){
    assert(a->baseline = bytes_in_use, 1);
}

void byte_account_close(byte_account *a){
    assert(a->baseline == bytes_in_use);
}

#pragma GCC visibility pop
