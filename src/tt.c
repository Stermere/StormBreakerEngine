/* tt.c - transposition table storage. */
#include "tt.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Four 16-byte entries per 64-byte cache line: one cluster, one cache miss. */
#define TT_CLUSTER_SIZE 4

_Static_assert(sizeof(TTEntry) == 16, "TTEntry must stay 16 bytes for cache-line packing");

typedef struct {
    TTEntry entry[TT_CLUSTER_SIZE];
} TTCluster;

static TTCluster *Table;
static size_t ClusterCount;
static size_t SizeMb;
static uint8_t Generation;

/* The generation lives in the top 6 bits of genBound, so it advances in steps of 4
 * and wraps after 64 searches. Ages are computed modulo that cycle. */
#define GENERATION_DELTA 4
#define GENERATION_MASK  0xFCu

/* High 64 bits of a 64x64 multiply. */
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

/* A multiply-shift rather than `key % ClusterCount`, a hardware division on the critical
 * path; unlike a power-of-two mask it does not discard half of Hash. Shifted left by 16
 * because key_verifier() takes the top bits, which must not also index the cluster. */
static inline size_t cluster_index(Key key) {
    return (size_t)mul_hi64(key << 16, (uint64_t)ClusterCount);
}

static inline uint16_t key_verifier(Key key) { return (uint16_t)(key >> 48); }

/*
 * A mate score means "mate in N plies from HERE", so the same position reached at a
 * different distance from the root carries a different absolute score - store the
 * absolute one and the engine announces mates that are a transposition away from
 * evaporating. Tablebase scores are ply-relative for the same reason, which is why
 * these tests run against the TB band edge rather than the mate one.
 */
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

bool tt_resize(size_t mb) {
    if (mb == 0)
        mb = 1;

    const size_t bytes    = mb * 1024ULL * 1024ULL;
    const size_t clusters = bytes / sizeof(TTCluster);

    TTCluster *fresh = (TTCluster *)calloc(clusters, sizeof(TTCluster));
    if (!fresh)
        return false;

    free(Table);
    Table        = fresh;
    ClusterCount = clusters;
    SizeMb       = mb;
    Generation   = 0;
    return true;
}

void tt_free(void) {
    free(Table);
    Table        = NULL;
    ClusterCount = 0;
    SizeMb       = 0;
}

void tt_clear(void) {
    if (Table)
        memset(Table, 0, ClusterCount * sizeof(TTCluster));
    Generation = 0;
}

void tt_new_search(void) { Generation += GENERATION_DELTA; }

size_t tt_size_mb(void) { return SizeMb; }

int tt_hashfull(void) {
    if (!Table || ClusterCount == 0)
        return 0;

    /* Sample the first 1000 entries rather than walking a multi-gigabyte table; that is
     * what `info hashfull` means by convention. */
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
            /* An entry the search keeps hitting is worth more than its depth suggests, so
             * drag it forward to the current generation and out of replacement's sights. */
            e->genBound = (uint8_t)(Generation | (e->genBound & 3));
            *out        = *e;
            return true;
        }
    }
    return false;
}

/* How many searches ago this entry was written, modulo the generation cycle. */
static inline int entry_age(const TTEntry *e) {
    return (int)((uint8_t)(Generation - (e->genBound & GENERATION_MASK)) / GENERATION_DELTA);
}

/* Depth discounted by age, so a deep entry from three searches ago cannot squat. */
static inline int replace_priority(const TTEntry *e) { return (int)e->depth - 8 * entry_age(e); }

void tt_store(Key key, Move m, Value value, Value eval, Depth depth, Bound bound, bool pv,
              int ply) {
    if (!Table)
        return;

    assert(bound != BOUND_NONE);
    assert(depth >= 0);

    TTCluster *const cluster = &Table[cluster_index(key)];
    const uint16_t key16     = key_verifier(key);

    TTEntry *replace = &cluster->entry[0];

    for (int i = 0; i < TT_CLUSTER_SIZE; ++i) {
        TTEntry *const e = &cluster->entry[i];

        /* Its own slot, or an empty one: nothing to weigh up. */
        if (e->key16 == key16 || (e->genBound & 3) == BOUND_NONE) {
            replace = e;
            break;
        }
        if (replace_priority(e) < replace_priority(replace))
            replace = e;
    }

    const bool sameSlot = replace->key16 == key16;

    /* A node that failed low has no best move to report. Keep whatever a previous, and
     * possibly deeper, search found here rather than erasing it. */
    if (!sameSlot || m != MOVE_NONE)
        replace->move = (uint16_t)m;

    /* Sticky within the slot, and written even when the value below is not refreshed: a
     * shallow re-visit that declines to overwrite the score has learned nothing that
     * unsays the flag. */
    replace->pv = (uint8_t)(pv || (sameSlot && replace->pv));

    /* Overwrite when the slot is not already ours, when the new result is exact, or when
     * the new search is not meaningfully shallower. The four-ply slack matters: without
     * it a reduced-depth re-search never refreshes the entry, so its generation goes
     * stale while it is still in use and replacement evicts what the search relies on. */
    if (!sameSlot || bound == BOUND_EXACT || (Depth)replace->depth < depth + 4) {
        const Value stored = value_to_tt(value, ply);
        assert(stored >= INT16_MIN && stored <= INT16_MAX);

        replace->key16    = key16;
        replace->value    = (int16_t)stored;
        replace->eval     = (int16_t)eval;
        replace->depth    = (uint8_t)(depth > 255 ? 255 : depth);
        replace->genBound = (uint8_t)(Generation | (unsigned)bound);
    }
}

void tt_prefetch(Key key) {
    (void)key;
#if defined(__GNUC__)
    if (Table && ClusterCount)
        __builtin_prefetch(&Table[cluster_index(key)]);
#endif
}
