/*
 * tt.c - transposition table storage.
 *
 * Allocation, clearing and statistics are implemented so the Hash UCI option
 * is honest from day one. probe/store are TODO - see tt.h.
 */
#include "tt.h"

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

bool tt_resize(size_t mb) {
    if (mb == 0)
        mb = 1;

    const size_t bytes    = mb * 1024ULL * 1024ULL;
    const size_t clusters = bytes / sizeof(TTCluster);

    TTCluster *fresh = (TTCluster *)calloc(clusters, sizeof(TTCluster));
    if (!fresh)
        return false; /* keep the existing table rather than running with none */

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

void tt_new_search(void) { Generation += 4; /* low 2 bits belong to Bound */ }

size_t tt_size_mb(void) { return SizeMb; }

int tt_hashfull(void) {
    if (!Table || ClusterCount == 0)
        return 0;

    /* Sample the first 1000 entries rather than walking a multi-gigabyte
     * table; this is what `info hashfull` means by convention. */
    int used        = 0;
    const int probe = 1000;
    for (int i = 0; i < probe; ++i) {
        const TTEntry *e = &Table[i / TT_CLUSTER_SIZE % ClusterCount].entry[i % TT_CLUSTER_SIZE];
        if (e->genBound != 0)
            ++used;
    }
    return used;
}

/* ---------------------------------------------------------------- TODO --- */

bool tt_probe(Key key, TTEntry *out) {
    (void)key;
    (void)out;
    /* TODO(engine): index with `key % ClusterCount`, scan the cluster for a
     * matching key16, and convert mate scores back to absolute. */
    return false;
}

void tt_store(Key key, Move m, Value value, Value eval, Depth depth, Bound bound, int ply) {
    (void)key;
    (void)m;
    (void)value;
    (void)eval;
    (void)depth;
    (void)bound;
    (void)ply;
    /* TODO(engine): depth-preferred replacement, biased against stale
     * generations. Make mate scores ply-relative before storing. */
}

void tt_prefetch(Key key) {
    (void)key;
#if defined(__GNUC__)
    if (Table && ClusterCount)
        __builtin_prefetch(&Table[(size_t)key % ClusterCount]);
#endif
}
