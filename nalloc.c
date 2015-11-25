/**
 * TODO: need some heritage_destroy function, otherwise you likely leak
 * the slabs in local heritages when the thing to which they're local
 * dies.
 */

#define MODULE NALLOC

#include <stack.h>
#include <list.h>
#include <nalloc.h>
#include <thread.h>

/* Prevents system library functions from using nalloc. Useful for
   debugging. */
#pragma GCC visibility push(hidden)

#define LINREF_ACCOUNT_DBG 1
#define NALLOC_MAGIC_INT 0x01FA110C
#define LINREF_VERB 2
#ifndef NONALLOC

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

dbg iptr slabs_allocated;
dbg iptr slabs_used;

dbg cnt bytes_used;
dbg cnt max_bytes_used;

#define PTYPE(s, ...) {#s, s, NULL, NULL, NULL}
static const type polytypes[] = { MAP(PTYPE, _,
        16, 32, 48, 64, 80, 96, 112, 128,
        192, 256, 384, 512, 1024, MAX_BLOCK)
};

#define PHERITAGE(i, ...)                                           \
    HERITAGE(&polytypes[i], 32, 1, new_slabs)
static heritage poly_heritages[] = {
    ITERATE(PHERITAGE, _, 14)
};

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

static
block *(alloc_from_slab)(slab *s, heritage *h){
    if(s->contig_blocks)
        return (block *) &blocks_of(s)[h->t->size * --s->contig_blocks];
    return mustp(cof(stack_pop(&s->free_blocks), block, sanc));
}

static
bool slab_fully_hot(const slab *s){
    return !s->contig_blocks && !stack_peek(&s->free_blocks);
}

typedef struct{
    uptr lost:1;
    uptr size:WORDBITS - 1;
} hotst;
#define hotst(l, s) ((hotst){l, s})

static
err (recover_hot_blocks)(slab *s){
    assert(!PUN(hotst, lfstack_gen(&s->hot_blocks)).lost);
    struct lfstack h = s->hot_blocks;
    while(!lfstack_clear_cas_won(hotst(!lfstack_peek(&h), 0),
                                 &s->hot_blocks, &h))
        continue;
    if(!lfstack_peek(&h))
        return EARG;
    s->free_blocks = lfstack_convert(&h);
    return 0;
}

void (linfree)(lineage *l){
    block *b = l;
    *b = (block){SANCHOR};

    slab *s = slab_of(b);
    assert(profile_upd_free(s->tx.t->size), 1);
    
    for(struct lfstack h = s->hot_blocks;;){
        hotst st = PUN(hotst, lfstack_gen(&h));
        if(!lfstack_push_upd_won(&l->sanc, rup(st, .lost=0, .size++),
                                 &s->hot_blocks, &h))
            continue;
        if(!st.lost && !fills_slab(st.size + 1, s->tx.t->size))
            return;
        assert(!stack_peek(&s->free_blocks));

        struct heritage *her = s->her;
        if(xadd_iff(1, &her->nslabs, her->max_slabs) >= her->max_slabs){
            if(fills_slab(st.size + 1, s->tx.t->size)){
                s->contig_blocks = st.size + 1;
                s->hot_blocks = (lfstack) LFSTACK;
                slab_ref_down(s);
            }
        }else{
            while(!lfstack_clear_cas_won(hotst(0,0), &s->hot_blocks, &h))
                continue;
            s->free_blocks = lfstack_convert(&h);
            lfstack_push(&s->sanc, &her->slabs);
        }
        return;
    }
}

/* NB: avoids division. Subtracts bs to handle padding between last block
   and footer. */
static bool fills_slab(cnt blocks, size bs){
    assert(blocks * bs <= MAX_BLOCK);
    return blocks * bs > MAX_BLOCK - bs;
}

static
heritage *poly_heritage_of(size size){
    for(uint i = 0; i < ARR_LEN(polytypes); i++)
        if(polytypes[i].size >= size)
            return &poly_heritages[i];
    EWTF();
}

void *(malloc)(size size){
    if(!size)
        return TODO(), NULL;
    if(size > MAX_BLOCK)
        return TODO(), NULL;
    block *b = (linalloc)(poly_heritage_of(size));
    if(b)
        assertl(2, magics_valid(b, poly_heritage_of(size)->t->size));
    return b;
}

void (free)(void *b){
    lineage *l = (lineage *) b;
    if(!b)
        return;
    assertl(2, write_magics(l, slab_of(l)->tx.t->size));
    (linfree)(l);
}


static
slab *(slab_new)(heritage *h){
    slab *s = cof(lfstack_pop(h->free_slabs), slab, sanc);
    if(!s){
        s = h->new_slabs(h->slab_alloc_batch);
        if(!s)
            return NULL;
        assert(xadd(h->slab_alloc_batch, &slabs_allocated), 1);
        assert(aligned_pow2(s, SLAB_SIZE));
        
        s->slabfooter = (slabfooter) SLABFOOTER;
        for(slab *si = s + 1; si != &s[h->slab_alloc_batch]; si++){
            si->slabfooter = (slabfooter) SLABFOOTER;
            lfstack_push(&si->sanc, h->free_slabs);
        }
    }
    assert(xadd(1, &slabs_used) >= 0);
    assert(!s->tx.linrefs);
    assert(!lfstack_peek(&s->hot_blocks));
    
    s->her = h;
    if(s->tx.t != h->t){
        s->tx = (tyx){h->t};
        
        cnt nb = s->contig_blocks = slab_max_blocks(s);
        if(h->t->lin_init)
            for(cnt b = 0; b < nb; b++)
                h->t->lin_init((lineage *) &blocks_of(s)[b * h->t->size]);
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
        assert(xadd(-1, &slabs_used));
        lfstack_push(&s->sanc, s->her->free_slabs);
    }
}

err (linref_up)(volatile void *l, type *t){
    assert(l);
    if(l < heap_start() || l > heap_end())
        return EARG();
    
    slab *s = slab_of((void *) l);
    for(tyx tx = s->tx;;){
        if(tx.t != t || !tx.linrefs)
            return EARG("Wrong type.");
        log(LINREF_VERB, "linref up! % % %", l, t, tx.linrefs);
        assert(tx.linrefs > 0);
        if(cas2_won(((tyx){t, tx.linrefs + 1}), &s->tx, &tx))
            return T->nallocin.linrefs_held++, 0;
    }
}

void (linref_down)(volatile void *l){
    T->nallocin.linrefs_held--;
    slab *s = slab_of((void *) l);
    slab_ref_down(s);
}

static
cnt slab_max_blocks(const slab *s){
    return MAX_BLOCK / s->tx.t->size;
}

static constfun 
slab *(slab_of)(const block *b){
    assert(b);
    return cof_aligned_pow2(b, slab);
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
    if(b && o)
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

void *memalign(size align, size sz){
    TODO();
    assert(sz <= MAX_BLOCK
           && align < PAGE_SIZE
           && align * (sz / align) == align);
    if(!is_pow2(align) || align < sizeof(void *))
        return NULL;
    return malloc(sz);
}

int posix_memalign(void **mptr, size align, size sz){
    return (*mptr = memalign(align, sz)) ? 0 : -1;
}

void *pvalloc(size sz){
    EWTF();
}

void *aligned_alloc(size align, size sz){
    return memalign(align, sz);
}

void *valloc(size sz){
    EWTF();
}

void profile_upd_alloc(size s){
    cnt u = xadd(s, &bytes_used);
    for(cnt mb = max_bytes_used; mb < u;)
        if(cas_won(u, &max_bytes_used, &mb))
            break;
}

void profile_upd_free(size s){
    xadd(-s, &bytes_used);
}

/* TODO: keep track of size */
void nalloc_profile_report(void){
    ppl(0, slabs_allocated, slabs_used, bytes_used, max_bytes_used);
}

#endif  /* NONALLOC */

err fake_linref_up(void){
    T->nallocin.linrefs_held++;
    return 0;
}

void fake_linref_down(void){
    T->nallocin.linrefs_held--;
}

void linref_account_open(linref_account *a){
    assert(a->baseline = T->nallocin.linrefs_held, 1);
}

void linref_account_close(linref_account *a){
    if(LINREF_ACCOUNT_DBG)
        assert(T->nallocin.linrefs_held == a->baseline);
}

void byte_account_open(byte_account *a){
    assert(a->baseline = bytes_used, 1);
}

void byte_account_close(byte_account *a){
    assert(a->baseline == bytes_used);
}

#pragma GCC visibility pop
