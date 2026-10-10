/* tt.c - transposition table storage. */
#include "tt.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Four 16-byte entries per 64-byte cache line: one cluster, one cache miss. */
#define TT_CLUSTER_SIZE 4

_Static_assert(sizeof(TTEntry) == 16, "TTEntry must stay 16 bytes for cache-line packing");

typedef struct {
    TTEntry entry[TT_CLUSTER_SIZE];
} TTCluster;

static TTCluster *Table;

/* What free() is owed; Table is aligned inside it. */
static void *TableBlock;

static size_t ClusterCount;
static size_t SizeMb;

/* Relaxed: a stale read only costs one entry some replacement priority. */
static _Atomic uint8_t Generation;

static inline uint8_t generation(void) {
    return atomic_load_explicit(&Generation, memory_order_relaxed);
}

/* The generation is the top 6 bits of genBound: steps of 4, wrapping after 64 searches. */
#define GENERATION_DELTA 4
#define GENERATION_MASK  0xFCu

static inline uint64_t mul_hi64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    return (uint64_t)(((unsigned __int128)a * (unsigned __int128)b) >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    return __umulh(a, b);
#else
    const uint64_t aLo = (uint32_t)a, aHi = a >> 32;
    const uint64_t bLo = (uint32_t)b, bHi = b >> 32;
    const uint64_t mid = aHi * bLo + ((aLo * bLo) >> 32);
    return aHi * bHi + (mid >> 32) + ((aLo * bHi + (uint32_t)mid) >> 32);
#endif
}

/* Multiply-shift instead of `%`, and any size works. The top 16 bits are the verifier,
 * so they are shifted out of the index. */
static inline size_t cluster_index(Key key) {
    return (size_t)mul_hi64(key << 16, (uint64_t)ClusterCount);
}

static inline uint16_t key_verifier(Key key) { return (uint16_t)(key >> 48); }

/* Mate and TB scores are stored relative to the node, not the root, so they stay right
 * when the position is reached at another ply. */
static Value value_to_tt(Value v, int ply) {
    if (v == VALUE_NONE)
        return VALUE_NONE;
    if (v >= VALUE_TB_WIN_IN_MAX_PLY)
        return v + ply;
    if (v <= VALUE_TB_LOSS_IN_MAX_PLY)
        return v - ply;
    return v;
}

Value tt_value_from_tt(Value v, int ply) {
    if (v == VALUE_NONE)
        return VALUE_NONE;
    if (v >= VALUE_TB_WIN_IN_MAX_PLY)
        return v - ply;
    if (v <= VALUE_TB_LOSS_IN_MAX_PLY)
        return v + ply;
    return v;
}

/* Over-allocated by a line so clusters can be cache-aligned. calloc, so the zeroed pages
 * are faulted in lazily rather than written at startup. */
bool tt_resize(size_t mb) {
    if (mb == 0)
        mb = 1;

    const size_t bytes    = mb * 1024ULL * 1024ULL;
    const size_t clusters = bytes / sizeof(TTCluster);

    void *const block = calloc(clusters * sizeof(TTCluster) + 64u, 1);
    if (!block)
        return false;

    free(TableBlock);
    TableBlock = block;
    Table =
        (TTCluster *)(void *)((unsigned char *)block + ((64u - ((uintptr_t)block & 63u)) & 63u));
    ClusterCount = clusters;
    SizeMb       = mb;
    atomic_store_explicit(&Generation, 0, memory_order_relaxed);
    return true;
}

void tt_free(void) {
    free(TableBlock);
    TableBlock   = NULL;
    Table        = NULL;
    ClusterCount = 0;
    SizeMb       = 0;
}

void tt_clear(void) {
    if (Table)
        memset(Table, 0, ClusterCount * sizeof(TTCluster));
    atomic_store_explicit(&Generation, 0, memory_order_relaxed);
}

void tt_new_search(void) {
    atomic_fetch_add_explicit(&Generation, GENERATION_DELTA, memory_order_relaxed);
}

size_t tt_size_mb(void) { return SizeMb; }

int tt_hashfull(void) {
    if (!Table || ClusterCount == 0)
        return 0;

    /* The first 1000 entries, by convention. */
    int used        = 0;
    const int probe = 1000;
    for (int i = 0; i < probe; ++i) {
        const TTEntry *e = &Table[i / TT_CLUSTER_SIZE % ClusterCount].entry[i % TT_CLUSTER_SIZE];
        if (e->genBound != 0)
            ++used;
    }
    return used;
}

bool tt_probe(Key key, TTEntry *out) {
    if (!Table)
        return false;

    TTCluster *const cluster = &Table[cluster_index(key)];
    const uint16_t key16     = key_verifier(key);

    for (int i = 0; i < TT_CLUSTER_SIZE; ++i) {
        TTEntry *const e = &cluster->entry[i];

        if (e->key16 == key16 && (e->genBound & 3) != BOUND_NONE) {
            /* Refresh the generation of an entry still in use. */
            e->genBound = (uint8_t)(generation() | (e->genBound & 3));
            *out        = *e;
            return true;
        }
    }
    return false;
}

static inline int entry_age(const TTEntry *e, uint8_t gen) {
    return (int)((uint8_t)(gen - (e->genBound & GENERATION_MASK)) / GENERATION_DELTA);
}

/* Depth discounted by age, so a deep entry from three searches ago cannot squat. */
static inline int replace_priority(const TTEntry *e, uint8_t gen) {
    return (int)e->depth - 8 * entry_age(e, gen);
}

void tt_store(Key key, Move m, Value value, Value eval, Depth depth, Bound bound, bool pv,
              int ply) {
    if (!Table)
        return;

    assert(bound != BOUND_NONE);
    assert(depth >= 0);

    TTCluster *const cluster = &Table[cluster_index(key)];
    const uint16_t key16     = key_verifier(key);
    const uint8_t gen        = generation();

    TTEntry *replace = &cluster->entry[0];

    for (int i = 0; i < TT_CLUSTER_SIZE; ++i) {
        TTEntry *const e = &cluster->entry[i];

        if (e->key16 == key16 || (e->genBound & 3) == BOUND_NONE) {
            replace = e;
            break;
        }
        if (replace_priority(e, gen) < replace_priority(replace, gen))
            replace = e;
    }

    const bool sameSlot = replace->key16 == key16;

    /* Keep the previous move when this node has none (a fail low). */
    if (!sameSlot || m != MOVE_NONE)
        replace->move = (uint16_t)m;

    replace->pv = (uint8_t)(pv || (sameSlot && replace->pv));

    /* The four-ply slack lets reduced re-searches refresh an entry still in use. */
    if (!sameSlot || bound == BOUND_EXACT || (Depth)replace->depth < depth + 4) {
        const Value stored = value_to_tt(value, ply);
        assert(stored >= INT16_MIN && stored <= INT16_MAX);

        replace->key16    = key16;
        replace->value    = (int16_t)stored;
        replace->eval     = (int16_t)eval;
        replace->depth    = (uint8_t)(depth > 255 ? 255 : depth);
        replace->genBound = (uint8_t)(gen | (unsigned)bound);
    }
}

void tt_prefetch(Key key) {
    (void)key;
#if defined(__GNUC__)
    if (Table && ClusterCount)
        __builtin_prefetch(&Table[cluster_index(key)]);
#endif
}
