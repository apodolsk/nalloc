#pragma once

#include <stack_cpp.hpp>

struct block{
    sanchor sanc;
} aliasing;
typedef block lineage;

struct type{
    const char *name;
    const size size;
    void (*lin_init)(lineage *b);
    bool (*has_special_ref)(const volatile void *l, bool up);

    constexpr
    type(const char *name, ::size size,
         void (*li)(lineage *b),
         bool (*hsr)(const volatile void *l, bool up))
    : name(name), size(size), lin_init(li), has_special_ref(hsr){}
    
};
/* #define type(t, li, hsr) type(#t, sizeof(t), li, hsr) */


struct heritage{
    lfstack slabs;
    cnt nslabs = 0;

    static lfstack shared_free_slabs;

    const type *const t;
    lfstack *const free_slabs = &shared_free_slabs;
    const cnt max_slabs;
    const cnt slab_alloc_batch;
    struct slab *(*const new_slabs)(cnt nslabs);

    constexpr
    heritage(const type *t, cnt ms, cnt sab, struct slab *(*ns)(cnt nslabs))
        : t(t), max_slabs(ms), slab_alloc_batch(sab), new_slabs(ns) {}

    
};

struct posix_heritage : heritage{
    posix_heritage(const type *t) : heritage(t, 16, 2, ::new_slabs){}
};

struct slabfooter{
    sanchor sanc;
    stack local_blocks;
    cnt contig_blocks;
    heritage *volatile her;
    align(CACHELINE_SIZE)
    lfstack hot_blocks;
    struct tyx {
        const type *t;
        iptr linrefs;
    };
    std::atomic<tyx> tx;
} slabfooter;
#define MAX_BLOCK (SLAB_SIZE - sizeof(slabfooter))

struct align(SLAB_SIZE) slab{
    u8 blocks[MAX_BLOCK];

    sanchor sanc;
    stack local_blocks;
    cnt contig_blocks = 0;
    heritage *volatile her = NULL;
    align(CACHELINE_SIZE)
    lfstack hot_blocks;
    struct tyx {
        const type *t;
        iptr linrefs;
    };
    union{
        std::atomic<tyx> tx;
        tyx tx_na;
    };
};
#define MIN_ALIGN (sizeof(lineage))

extern lfstack shared_free_slabs;

dbg extern iptr slabs_used;
dbg extern cnt bytes_used;

typedef void (*linit)(void *);


checked void *linalloc(heritage *h);
void linfree(lineage *l);

checked err linref_up(const volatile void *l, type *t);
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
void linref_account_close(linref_account *a);

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

