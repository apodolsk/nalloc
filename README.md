- [Type-Stable Memory](#type-stable-memory)
- [Nalloc](#nalloc)
  - [Interface](#interface)
    - [Implementation](#implementation)


# Type-Stable Memory<a id="orgheadline1"></a>

TSM is a way to provide safe memory access for lockfree data structures.

Every heap address `H` &ldquo;has a type&rdquo; because a type ID can be computed from
`H`. `linalloc()` allocates memory of a given type. `linref_up(H, T)` ensures
that `H` has type `T` and isn&rsquo;t reallocated with a different type.

The point is that, unlike schemes which block reuse of referenced memory,
TSM allows `H` to be freed and reused by an allocation of type `T`.

The goal of TSM is safe memory access for complicated data structures. For
example, consider a doubly-linked list used in a lockfree kernel:

-   Suppose `lflist_del(A)` has read `P := A->prev` and decided to write
    `P->next` with a CAS. If `P` is freed and reallocated as a string, the CAS
    can spuriously succeed if `*P` is an unlucky string which resembles the
    expected list node. `linref_up(P, "list node")` prevents this.

`linref_up()`&rsquo;s guarantee is weak, but the usual stronger guarantee
turns out to be irrelevant in a robust program.

-   `lflist_enq()` can&rsquo;t make allocations because its callers can&rsquo;t
    really afford to handle allocation failures. So the list operates on
    &ldquo;anchors&rdquo; preallocated inside enqueuable objects. Successive enqueues of
    an object must reuse its anchor.
-   Lockfree progress would be impractical, at best, if `lflist_enq(A)`
    weren't free to modify `A` because some out of date lflist function were
    operating on `A`.
-   So every lflist function must be prepared to deal with any anchor
    being deleted and re-enqueued just before a `CAS`. Generally, they must at
    least make every `CAS` contingent on a version count that&rsquo;s been live for
    some critical interval.
-   `linref_up()` proves that such a count exists, and it wouldn&rsquo;t make
    the lflist simpler or more efficient if you knew that `A` also wasn&rsquo;t
    freed for this interval.

As I describe later, TSM has a simple and cheap refcounting
implementation. However, it should be compatible with other schemes for
keeping track of references.

# Nalloc<a id="orgheadline4"></a>

Nalloc is a TSM-enabled slab allocator. It allocates &ldquo;lineages&rdquo; from
&ldquo;heritages&rdquo; in O(1).

TSM is a minor addition to a slab allocator. Nalloc is just a basic slab
allocator plus a refcount and type object pointer per slab.

## Interface<a id="orgheadline3"></a>

A lineage is a block of memory. Lineage `L` has a type in the sense that a
pointer to a type object **Y** can be computed from `L`&rsquo;s address.

-   **Y** stores the size of lineages of its type; and a lineage initialization
    function `lin_init()`, to be run during allocation.
-   If `linref_up(L, Y)` succeeds, the following holds until `linref_down(L, Y)`:
    -   L was last allocated with `linalloc("Y")`, and thus `Y->lin_init(L)` completed.
    -   `linref_up(L, Y')` will succeed iff `Y' == Y` (in any thread).
    -   If `L` is freed and reallocated:
        -   It must be reallocated with type `Y`.
        -   nalloc won&rsquo;t write to any part of `L` other than its lowest
            word, and thus `Y->lin_init(L)` won&rsquo;t be called during this
            reallocation.
        -   NB: type-stability lets you avoid re-initialization after
            reallocation. I hear this is desirable.
-   In other words, all threads will agree that `L` has type `Y`, and has been
    initialized according to **Y**. If threads cooperate to make sure `L`
    satisfies some invariant of `Y`, nalloc won&rsquo;t ruin it by clobbering `L`.

-   It&rsquo;s called a lineage because it represents multiple generations of
    a C memory object.

A slab is an array of lineages, with some metadata. All lineages in a
given slab have the same type and thus size. All slabs have the same size
and are naturally aligned, so that the slab of a lineage can be computed
directly from its address.

-   Each slab stores a refcount and a pointer to struct type, and `L`&rsquo;s
    type is the type of its containing slab. `linref_up(L,_)` is a `CAS2` on
    `slab_of(L)`&rsquo;s type fields. That&rsquo;s basically all there is to implementing
    TSM: a refcount on slab rather than on individual blocks.

A heritage is a pool of slabs having the same type.

-   NB: they can be thread/CPU-local but wk doesn&rsquo;t exploit this.

nalloc defines some internal heritages with &ldquo;types&rdquo; meant to represent
plain char buffers of different size classes. `malloc()` is just a wrapper
around `linalloc()` with one these heritages.

### Implementation<a id="orgheadline2"></a>

Lockfree slab allocation is simple. nalloc elaborates on the basic idea of
using lfstacks to represent free lineages in a slab and free slabs in a
heritage.

`linalloc(H)` pops a slab `S` from heritage `H`, fetches a lineage from `S`,
and returns `S` to `H` iff `S` has lineages remaining. If not, `S` is
said to be &ldquo;lost&rdquo;.

-   NB: empty slabs aren&rsquo;t returned so that search is `O(1)` (uncontended).

`S` splits its free lineages into 3 sets. This is an old optimization that
made more sense in the past, but still has some benefit.

-   A &ldquo;main&rdquo; non-lockfree stack `S->local_blocks`.
-   An lfstack `S->hot_blocks`, on a separate cache line.
    -   NB: it accumulates freed lineages and is collected in batches. The
        point is to avoid locked instructions and shared cache line writes.
-   The maximal contiguous set of free lineages beginning at the start
    of the slab is represented only as a count `S->contig_blocks`. 
    -   NB: it makes slab initializion cheaper. It encourages cache- and
        prefetcher-friendly access patterns, not just inside nalloc.

Slabs are initialized with all lineages in `contig_blocks`.

`alloc_from_slab(S)` in `linalloc()` attempts to allocate from
`contig_blocks`, `local_blocks`, and `hot_blocks`, in that order.

-   If it empties local\_blocks, it uses a variant of `lstack_clear()` to
    transfer all lineages from `hot_blocks` onto `local_blocks` in `O(1)`.

`linfree(L)` pushes `L` to `slab_of(L)->hot_blocks`.

The main subtlety is that `linfree(L)` needs to know whether `S := slab_of(L)`
has been lost, in which case it must return `S` to its former heritage `H` or
arrange for it to be freed.

-   Multiple `linfree(L)` calls can find `S` lost, but only one should return
    it. More subtly, it&rsquo;s not simple for `linfree()` to agree with an
    in-flight `linalloc()` on whether `S` is actually lost.
-   The key observation is that `S->hot_blocks` is always cleared
    entirely with an `lfstack_clear()`, so it doesn&rsquo;t need a version counter.
-   So `S->hot_blocks.gen` instead stores an authoritative &ldquo;lost&rdquo; flag and the
    stack&rsquo;s size. The flag is set when `linalloc()` finds `S->hot_blocks` empty
    after also exhausting the other sets. It&rsquo;s unset by the next `linfree()`
    to `S`.
-   If this `linfree()` decides to free `S`, it suffices to not return it to
    `H`. No future allocations from `S` will happen, and the final `linfree()` to
    `S` will know to free it.

The biggest problem with TSM is reporting failure in `linref_up(P)` when
`P` isn&rsquo;t on the heap. 

-   wk can easily validate `P` because its heap is contiguous and
    non-shrinking.
-   The userspace heap isn&rsquo;t, since it uses non-fixed mappings and should
    return memory to the system. Further, the user may have made mappings
    outside of nalloc&rsquo;s control.
-   Unfortunately, the current solution is for userspace nalloc to just
    not return virtual memory to the kernel. It can reuse pages internally
    and remap free slabs as &ldquo;guard&rdquo; pages to return physical memory - though
    the current implementation doesn&rsquo;t.
-   This way, if `P` was previously on the heap, then `linref_up(P)` can expect
    a segfault because `P` couldn&rsquo;t have been remapped outside of nalloc.
-   If `P` was never on the heap, then `linref_up(P, Y)` can still spuriously
    succeed, but only if `P` is on a non-nalloc-mapped page that resembles a
    slab of type `Y`. I don&rsquo;t expect a program to generate such a `P` unless it
    takes pointers from untrusted sources. In that case, it&rsquo;s not secure.
