#define MODULE NALLOC

#include <stack.hpp>
#include <nalloc.hpp>
#include <new>

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

dbg iptr slabs_in_use;
dbg iptr total_slabs_used;

dbg cnt bytes_in_use;
dbg cnt max_bytes_in_use;

dbg __thread cnt linrefs_held;

#define MALLOC_TYPE(s, ...) type(#s, s, NULL, NULL)
static const type malloctypes[] = { MAP(MALLOC_TYPE, _,
        16, 32, 48, 64, 80, 96, 112, 128,
        192, 256, 384, 512, 1024, MAX_BLOCK)
};

#define MALLOC_HERITAGE(i, ...)                 \
    heritage(&malloctypes[i], 32, 1, new_slabs)
static heritage malloc_heritages[] = {
    ITERATE(MALLOC_HERITAGE, _, 14)
};

void *(linalloc)(heritage *h){
    if(poisoned())
        return NULL;
    
    slab *s = cof(h->slabs.pop(), slab, sanc);
    if(!s && !(s = slab_new(h))){
        EOOR();
        return NULL;
    }

    block *b = alloc_from_slab(s, h);
    if(!slab_fully_hot(s) || !recover_hot_blocks(s))
        h->slabs.push(s->sanc);
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
        return (block *)(void *) &blocks_of(s)[h->t->size * --s->contig_blocks];
    return mustp(cof(s->local_blocks.pop(), block, sanc));
}

static
bool slab_fully_hot(const slab *s){
    return !s->contig_blocks && !s->local_blocks.peek();
}

struct hotst{
    uptr lost:1;
    uptr size:WORDBITS - 1;

    operator uptr(){
        return *(uptr *) this;
    }
};

static
err (recover_hot_blocks)(slab *s){
    /* assert(!PUN(hotst, lfstack_gen(&s->hot_blocks)).lost); */
    struct top h = s->hot_blocks;
    while(!s->hot_blocks.clear_cas_won((hotst){.lost = !h.peek()}, h))
        continue;
    if(!h.peek())
        return EARG;
    s->local_blocks = h;
    return 0;
}

void (linfree)(lineage *l){
    block *b = l;
    *b = block();

    slab *s = slab_of(b);
    heritage *her = s->her;
    assert(profile_upd_free(s->tx.t->size), 1);
    
    for(struct top h = s->hot_blocks;;){
        hotst st = PUN(hotst, h.gen);
        if(!st.lost){
            if(!s->hot_blocks.push_cas_won(l->sanc, rup(st, .size++), h))
                continue;
            if(fills_slab(st.size + 1, s->tx_na.t->size)){
                assert(!stack_peek(&s->local_blocks));
                
                s->contig_blocks = st.size + 1;
                s->hot_blocks = lfstack();
                slab_ref_down(s);
            }
            return;
        }else if(!s->hot_blocks.clear_cas_won((hotst){}, h))
            continue;

        assert(!stack_peek(&s->local_blocks));
        if(xadd_iff_less(1, &her->nslabs, her->max_slabs) < her->max_slabs)
            break;
        h = top();
    }
        
    s->local_blocks.push(b->sanc);
    her->slabs.push(s->sanc);
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

void *malloc(size size) noexcept{
    if(!size)
        return NULL;
    if(size > MAX_BLOCK)
        return NULL;
    block *b = (block *) linalloc(malloc_heritage_of(size));
    if(b)
        assertl(2, magics_valid(b, malloc_heritage_of(size)->t->size));
    return b;
}

void free(void *b) noexcept{
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
    slab *s = cof(h->free_slabs->pop(), slab, sanc);
    if(!s){
        s = h->new_slabs(h->slab_alloc_batch);
        if(!s)
            return NULL;
        assert(xadd(h->slab_alloc_batch, &total_slabs_used), 1);
        assert(aligned_pow2(s, SLAB_SIZE));
        
        new((void *) s) slab();
        for(slab *si = s + 1; si != &s[h->slab_alloc_batch]; si++){
            new((void *) si) slab();
            h->free_slabs->push(si->sanc);
        }
    }
    assert(xadd(1, &slabs_in_use) >= 0);
    assert(!s->tx.linrefs);
    assert(!lfstack_peek(&s->hot_blocks));
    
    s->her = h;
    if(s->tx_na.t != h->t){
        cnt nb = s->contig_blocks = slab_max_blocks(s);
        if(h->t->lin_init)
            for(cnt b = 0; b < nb; b++)
                h->t->lin_init((block *)(void *) &blocks_of(s)[b * h->t->size]);
        else
            for(cnt b = 0; b < nb; b++)
                assertl(2, write_magics((block *) &blocks_of(s)[b * h->t->size],
                                        h->t->size));
    }
    s->tx = {h->t, 1};

    xadd(1, &h->nslabs);
    return s;
}

static
void (slab_ref_down)(slab *s){
    assert(s->tx.t);
    if(must(xadd((uptr) -1, &s->tx_na.linrefs)) == 1){
        assert(!lfstack_peek(&s->hot_blocks));
        assert(xadd(-1, &slabs_in_use));
        s->her->free_slabs->push(s->sanc);
    }
}

err (linref_up)(const volatile void *l, type *t){
    assert(l);
    if(t->has_special_ref(l, true))
        return if_dbg(linrefs_held++),
               0;
    if(l < heap_start() || l > heap_end())
        return EARG();
    
    slab *s = slab_of((lineage *) l);
    for(slab::tyx tx = s->tx;;){
        if(tx.t != t || !tx.linrefs)
            return EARG("Wrong type.");
        log(LINREF_VERB, "linref up! % % %", l, t, tx.linrefs);
        assert(tx.linrefs > 0);
        if(s->tx.compare_exchange_strong(tx, {t, tx.linrefs + 1}))
            return if_dbg(linrefs_held++),
                   0;
    }
}

void (linref_down)(const volatile void *l, type *t){
    assert(linrefs_held--);
    if(!t->has_special_ref(l, false))
        slab_ref_down(slab_of((lineage *) l));
}

static
cnt slab_max_blocks(const slab *s){
    return MAX_BLOCK / s->tx_na.t->size;
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

void *calloc(size nb, size bs) noexcept{
    u8 *b = (u8 *)malloc(nb * bs);
    if(b)
        memset(b, 0, nb * bs);
    return b;
}

void *realloc(void *o, size size) noexcept{
    u8 *b = (u8 *)malloc(size);
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
    assert(linrefs_held++, 1);
    return 0;
}

void fake_linref_down(void){
    assert(linrefs_held--);
}

void linref_account_open(linref_account *a){
    assert(a->baseline = linrefs_held, 1);
}

void linref_account_close(linref_account *a){
    if(LINREF_ACCOUNT_DBG)
        assert(linrefs_held == a->baseline);
}

void byte_account_open(byte_account *a){
    assert(a->baseline = bytes_in_use, 1);
}

void byte_account_close(byte_account *a){
    assert(a->baseline == bytes_in_use);
}

#pragma GCC visibility pop
