#include "uncprobe.h"

#ifdef UNC_PROBE

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "board.h"
#include "nnue.h"
#include "search.h"
#include "timeman.h"
#include "tt.h"

/*
 * uncprobe.c - the `probe unc` command. See uncprobe.h for why it exists.
 *
 * It searches a corpus (the bench positions by default, so the tree is the one every other
 * measurement here is taken on), records the signal unc_scale() consumes at every node
 * that consults it, holds the mapping neutral while it does, and reports the constants
 * that re-centre it. The distribution is NODE-WEIGHTED because a margin is applied per
 * node, so the average scale the search runs under is the average over nodes rather than
 * over the positions in a file.
 *
 * The histogram is written from the search worker and read after it is joined, so no
 * synchronisation is needed - the same argument bench.c makes for search_nodes().
 */

/* One bucket per centipawn of signal. Sixteen thousand covers every sigma any net has
 * produced by a wide margin, and anything past it still counts in the mean and the maximum,
 * both accumulated exactly; only the percentiles would notice, and the report says so. */
#define UNC_PROBE_BUCKETS 16384

/* Twelve because that is the depth E20 and E21 centred the mapping's constants on, so a
 * new measurement is comparable to the ones the shipped constants came from. */
#define UNC_PROBE_DEPTH 12

static bool ProbeLive;
static uint64_t Hist[UNC_PROBE_BUCKETS];
static uint64_t Samples, SignalSum, Overflow;
static int SignalMax;

int unc_probe(int signal, int scale) {
    if (signal < 0)
        signal = 0;

    ++Samples;
    SignalSum += (uint64_t)signal;
    if (signal > SignalMax)
        SignalMax = signal;

    if (signal < UNC_PROBE_BUCKETS)
        ++Hist[signal];
    else
        ++Overflow;

    return ProbeLive ? scale : 100;
}

/* A measured distribution, compacted to the values that actually occurred. Both the run
 * just taken and a reference loaded from disk arrive in this shape, so every statistic
 * below is written once and applies to either. */
typedef struct {
    int *value;
    uint64_t *count;
    int n;
    uint64_t samples;
    uint64_t sum;
    int max;
    uint64_t overflow;

    int base, slope, cap, grain;
    bool sigma;
    char net[32];
    int depth;
} Dist;

static void dist_free(Dist *d) {
    free(d->value);
    free(d->count);
    d->value = NULL;
    d->count = NULL;
    d->n     = 0;
}

static bool dist_reserve(Dist *d, int n) {
    d->value = (int *)calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    d->count = (uint64_t *)calloc((size_t)(n > 0 ? n : 1), sizeof(uint64_t));
    if (!d->value || !d->count) {
        dist_free(d);
        return false;
    }
    return true;
}

/* The run just taken, compacted out of the bucket array. The constants that were live when
 * it was taken travel with it: for a reference run that is the whole point, since they are
 * the mapping the fit was made under. */
static bool dist_from_histogram(Dist *d, const UncMapping *m) {
    int distinct = 0;
    for (int i = 0; i < UNC_PROBE_BUCKETS; ++i)
        if (Hist[i])
            ++distinct;

    memset(d, 0, sizeof(*d));
    if (!dist_reserve(d, distinct))
        return false;

    for (int i = 0; i < UNC_PROBE_BUCKETS; ++i)
        if (Hist[i]) {
            d->value[d->n] = i;
            d->count[d->n] = Hist[i];
            ++d->n;
        }

    d->samples  = Samples;
    d->sum      = SignalSum;
    d->max      = SignalMax;
    d->overflow = Overflow;
    d->base     = m->base;
    d->slope    = m->slope;
    d->cap      = m->cap;
    d->grain    = m->grain;
    d->sigma    = m->sigma;
    d->depth    = 0;
    return true;
}

/* The signal below which `q` of the NODES fall. Overflow samples sit above every bucket,
 * so they are answered with the true maximum. */
static int dist_percentile(const Dist *d, double q) {
    if (d->samples == 0)
        return 0;

    const uint64_t target = (uint64_t)(q * (double)d->samples);
    uint64_t seen         = 0;
    for (int i = 0; i < d->n; ++i) {
        seen += d->count[i];
        if (seen >= target)
            return d->value[i];
    }
    return d->max;
}

/* Exactly what unc_scale() computes, and it must stay exactly that: the truncation in the
 * division is part of the mapping, not a rounding detail. */
static int map_scale(int base, int slope, int cap, int grain, int signal) {
    const int v = base + (int)((int64_t)signal * slope / grain);
    return v > cap ? cap : v;
}

typedef struct {
    double mean;
    double sd;
    double cappedPct;
    int median, p25, p75;
} ScaleStats;

/* Overflow samples are above every bucket, so the mapping pins them at the cap - which is
 * where any signal that large lands anyway. The mapping is monotone non-decreasing, so a
 * percentile of the scale is the mapping of that percentile of the signal. */
static void scale_stats(const Dist *d, int base, int slope, int cap, ScaleStats *out) {
    memset(out, 0, sizeof(*out));
    if (d->samples == 0)
        return;

    double sum = 0.0, sumsq = 0.0;
    uint64_t capped = 0;
    for (int i = 0; i < d->n; ++i) {
        const double v = (double)map_scale(base, slope, cap, d->grain, d->value[i]);
        const double w = (double)d->count[i];
        sum += v * w;
        sumsq += v * v * w;
        if (map_scale(base, slope, cap, d->grain, d->value[i]) >= cap)
            capped += d->count[i];
    }

    if (d->overflow) {
        const double w = (double)d->overflow;
        sum += (double)cap * w;
        sumsq += (double)cap * (double)cap * w;
        capped += d->overflow;
    }

    const double n = (double)d->samples;
    out->mean      = sum / n;
    const double v = sumsq / n - out->mean * out->mean;
    out->sd        = v > 0.0 ? sqrt(v) : 0.0;
    out->cappedPct = 100.0 * (double)capped / n;

    out->median = map_scale(base, slope, cap, d->grain, dist_percentile(d, 0.50));
    out->p25    = map_scale(base, slope, cap, d->grain, dist_percentile(d, 0.25));
    out->p75    = map_scale(base, slope, cap, d->grain, dist_percentile(d, 0.75));
}

#define BASE_SWEEP_MAX  200
#define SLOPE_SWEEP_MAX 64

/* The base that puts the node-weighted mean scale closest to `target` at this slope. Swept
 * rather than solved because the cap makes the mean a piecewise function of the base, and
 * the range is deliberately wider than the sweep seat's own: an answer outside it is
 * exactly the thing worth knowing, and a clamp would report a fit that does not fit. */
static int best_base(const Dist *d, int slope, int cap, double target) {
    int best        = 0;
    double bestDiff = 1e18;
    for (int base = 0; base <= BASE_SWEEP_MAX; ++base) {
        ScaleStats s;
        scale_stats(d, base, slope, cap, &s);
        const double diff = fabs(s.mean - target);
        if (diff < bestDiff) {
            bestDiff = diff;
            best     = base;
        }
    }
    return best;
}

/* A CSV with a commented header rather than JSON, so it can be plotted or diffed without a
 * parser. What makes it worth keeping is the header: a distribution without the constants
 * that were live when it was taken cannot be used as a reference. */
static bool dist_write(const Dist *d, const char *path, int positions) {
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("probe unc: cannot write %s\n", path);
        return false;
    }

    fprintf(f, "# stormbreaker unc-probe 1\n");
    fprintf(f, "# branch=%s net=%s depth=%d positions=%d mode=%s\n", d->sigma ? "sigma" : "corr",
            d->net[0] ? d->net : "-", d->depth, positions, ProbeLive ? "live" : "neutral");
    fprintf(f, "# base=%d slope=%d cap=%d grain=%d\n", d->base, d->slope, d->cap, d->grain);
    fprintf(f, "# samples=%llu sum=%llu max=%d overflow=%llu\n", (unsigned long long)d->samples,
            (unsigned long long)d->sum, d->max, (unsigned long long)d->overflow);
    fprintf(f, "signal,count\n");
    for (int i = 0; i < d->n; ++i)
        fprintf(f, "%d,%llu\n", d->value[i], (unsigned long long)d->count[i]);

    fclose(f);
    return true;
}

/* Two passes: count the rows, then fill. The file is a few thousand lines, and a growable
 * array here would be more code than a second fopen. */
static bool dist_read(Dist *d, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("probe unc: cannot read %s\n", path);
        return false;
    }

    memset(d, 0, sizeof(*d));
    d->grain = 1;

    char line[256];
    int rows = 0;
    while (fgets(line, sizeof(line), f))
        if (line[0] != '#' && line[0] != 's')
            ++rows;
    rewind(f);

    if (!dist_reserve(d, rows)) {
        fclose(f);
        printf("probe unc: out of memory reading %s\n", path);
        return false;
    }

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            char branch[16];
            unsigned long long samples, sum, overflow;
            if (sscanf(line, "# base=%d slope=%d cap=%d grain=%d", &d->base, &d->slope, &d->cap,
                       &d->grain) == 4)
                continue;
            if (sscanf(line, "# samples=%llu sum=%llu max=%d overflow=%llu", &samples, &sum,
                       &d->max, &overflow) == 4) {
                d->samples  = samples;
                d->sum      = sum;
                d->overflow = overflow;
                continue;
            }
            if (sscanf(line, "# branch=%15s net=%31s depth=%d", branch, d->net, &d->depth) == 3)
                d->sigma = strcmp(branch, "sigma") == 0;
            continue;
        }

        int value;
        unsigned long long count;
        if (sscanf(line, "%d,%llu", &value, &count) == 2 && d->n < rows) {
            d->value[d->n] = value;
            d->count[d->n] = count;
            ++d->n;
        }
    }
    fclose(f);

    if (d->samples == 0 || d->n == 0) {
        printf("probe unc: %s holds no samples\n", path);
        dist_free(d);
        return false;
    }
    return true;
}

/* Mirrors bench_run(): a fixed hash, cleared between positions, so the tree is a function
 * of the corpus and the depth alone. */
static void probe_position(const char *fen, int depth, int *ok, int *bad) {
    Position pos;
    memset(&pos, 0, sizeof(pos));

    if (!board_set_fen(&pos, fen)) {
        printf("probe unc: unparseable position: %s\n", fen);
        ++*bad;
        return;
    }

    tt_clear();
    search_clear();

    SearchLimits limits;
    search_limits_clear(&limits);
    limits.depth = depth;

    search_start(&pos, &limits);
    search_wait();
    ++*ok;
}

/* An EPD line is a FEN followed by operations. Cutting it after the fourth field and
 * supplying the clocks takes both spellings without a second parser; the halfmove clock a
 * probe throws away is worth nothing to it. */
static bool epd_to_fen(const char *line, char *out, size_t outSize) {
    int field       = 0;
    const char *c   = line;
    const char *end = NULL;

    while (*c && field < 4) {
        while (*c == ' ' || *c == '\t')
            ++c;
        if (!*c || *c == '\n' || *c == '\r')
            break;
        while (*c && *c != ' ' && *c != '\t' && *c != '\n' && *c != '\r')
            ++c;
        ++field;
        end = c;
    }
    if (field < 4 || !end)
        return false;

    const size_t len = (size_t)(end - line);
    if (len + 5 >= outSize)
        return false;

    memcpy(out, line, len);
    strcpy(out + len, " 0 1");
    return true;
}

static int probe_corpus(const char *epdPath, int depth, int *positions) {
    int ok = 0, bad = 0;

    tt_resize(16);

    if (!epdPath) {
        for (int i = 0; i < bench_position_count(); ++i)
            probe_position(bench_position(i), depth, &ok, &bad);
    } else {
        FILE *f = fopen(epdPath, "r");
        if (!f) {
            printf("probe unc: cannot read %s\n", epdPath);
            return 1;
        }
        char line[512], fen[256];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '\n' || line[0] == '\r' || line[0] == '#')
                continue;
            if (!epd_to_fen(line, fen, sizeof(fen))) {
                printf("probe unc: not a position: %s", line);
                ++bad;
                continue;
            }
            probe_position(fen, depth, &ok, &bad);
        }
        fclose(f);
    }

    *positions = ok;
    if (ok == 0) {
        printf("probe unc: no position searched\n");
        return 1;
    }
    return bad ? 1 : 0;
}

static const char *signal_name(bool sigma) { return sigma ? "sigma" : "|correction|"; }

static void report_signal(const Dist *d) {
    printf("signal      : %s, centipawns\n", signal_name(d->sigma));
    printf("  samples   : %llu nodes\n", (unsigned long long)d->samples);
    printf("  mean      : %.1f\n", (double)d->sum / (double)d->samples);
    printf("  percentile: p10 %d   p25 %d   median %d   p75 %d   p90 %d   p99 %d   max %d\n",
           dist_percentile(d, 0.10), dist_percentile(d, 0.25), dist_percentile(d, 0.50),
           dist_percentile(d, 0.75), dist_percentile(d, 0.90), dist_percentile(d, 0.99), d->max);
    if (d->overflow)
        printf("  note      : %llu samples above %d cp are exact in the mean and the maximum "
               "only\n",
               (unsigned long long)d->overflow, UNC_PROBE_BUCKETS - 1);
}

static void report_mapping(const Dist *d, int base, int slope, int cap, const char *label) {
    ScaleStats s;
    scale_stats(d, base, slope, cap, &s);
    printf("  %-9s min(%d + %s * %d / %d, %d)\n", label, base, signal_name(d->sigma), slope,
           d->grain, cap);
    printf("             mean %6.1f   sd %5.1f   p25 %3d   median %3d   p75 %3d   at cap %5.1f%%\n",
           s.mean, s.sd, s.p25, s.median, s.p75, s.cappedPct);
}

/* The slope is the conditioning strength - how many percent of margin one centipawn of
 * predicted error buys - and it is the thing a person has to choose. The base is not a
 * choice once the slope is fixed, since the centring rule pins it, so the table fixes the
 * mean at 100 in every row and shows what the spread does. */
static void report_sweep(const Dist *d, int cap, int currentSlope) {
    static const int slopes[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 32, 48, 64};

    printf("\nre-centred to a node-weighted mean of 100, at each conditioning strength:\n");
    printf("  slope   base     sd    p25  median  p75   at cap\n");
    for (size_t i = 0; i < sizeof(slopes) / sizeof(slopes[0]); ++i) {
        const int slope = slopes[i];
        if (slope > SLOPE_SWEEP_MAX)
            continue;

        const int base = best_base(d, slope, cap, 100.0);
        ScaleStats s;
        scale_stats(d, base, slope, cap, &s);
        printf("  %5d   %4d   %5.1f   %4d   %4d   %4d   %5.1f%%%s\n", slope, base, s.sd, s.p25,
               s.median, s.p75, s.cappedPct, slope == currentSlope ? "   <- current slope" : "");
    }
}

/* Carrying a fit across a retrain. The reference is a run of this probe on the net the
 * constants were fitted for, and its mean and spread under those constants are what the
 * SPSA fit was actually looking at - so reproducing them on the new net's distribution is
 * what "the same margins" means once the signal has moved. */
static void report_reference(const Dist *now, const Dist *ref, int cap) {
    ScaleStats target;
    scale_stats(ref, ref->base, ref->slope, ref->cap, &target);

    printf("\nreference: %s, net %s, %llu nodes, under its own constants %d/%d/%d\n",
           signal_name(ref->sigma), ref->net[0] ? ref->net : "unnamed",
           (unsigned long long)ref->samples, ref->base, ref->slope, ref->cap);
    printf("  it produced   mean %.1f   sd %.1f   p25 %d   median %d   p75 %d   at cap %.1f%%\n",
           target.mean, target.sd, target.p25, target.median, target.p75, target.cappedPct);

    if (ref->sigma != now->sigma) {
        printf("  REFUSED: the reference measured %s and this run measured %s. The two are "
               "different signals on different scales and matching them means nothing.\n",
               signal_name(ref->sigma), signal_name(now->sigma));
        return;
    }

    int bestBase = 0, bestSlope = 0;
    double bestErr = 1e18;
    for (int slope = 0; slope <= SLOPE_SWEEP_MAX; ++slope)
        for (int base = 0; base <= BASE_SWEEP_MAX; ++base) {
            ScaleStats s;
            scale_stats(now, base, slope, cap, &s);
            const double dm  = s.mean - target.mean;
            const double ds  = s.sd - target.sd;
            const double err = dm * dm + ds * ds;
            if (err < bestErr) {
                bestErr   = err;
                bestBase  = base;
                bestSlope = slope;
            }
        }

    ScaleStats got;
    scale_stats(now, bestBase, bestSlope, cap, &got);
    printf("  matched by    base %d   slope %d\n", bestBase, bestSlope);
    printf("                mean %.1f   sd %.1f   p25 %d   median %d   p75 %d   at cap %.1f%%\n",
           got.mean, got.sd, got.p25, got.median, got.p75, got.cappedPct);
    printf("\n  %s %d   %s %d\n", now->sigma ? "UNC_SIGMA_BASE" : "UNC_SCALE_BASE", bestBase,
           now->sigma ? "UNC_SIGMA_SLOPE" : "UNC_SCALE_SLOPE", bestSlope);
    printf("  option.%s=%d option.%s=%d\n", now->sigma ? "UncSigmaBase" : "UncScaleBase", bestBase,
           now->sigma ? "UncSigmaSlope" : "UncScaleSlope", bestSlope);
}

static char *next_arg(char **cursor) {
    char *s = *cursor;
    while (*s == ' ' || *s == '\t')
        ++s;
    if (!*s) {
        *cursor = s;
        return NULL;
    }

    char *end = s;
    while (*end && *end != ' ' && *end != '\t')
        ++end;
    if (*end)
        *end++ = '\0';
    *cursor = end;
    return s;
}

static void usage(void) {
    printf("usage: probe unc [depth] [-epd <file>] [-o <file>] [-ref <file>] [-cap <n>] [-live]\n");
    printf("  depth   search depth for every position          (default %d)\n", UNC_PROBE_DEPTH);
    printf("  -epd    corpus to search instead of the bench positions\n");
    printf("  -o      write the measured distribution, to be someone's -ref later\n");
    printf("  -ref    a run of this probe on the net the constants were fitted for\n");
    printf("  -cap    evaluate against this cap instead of the compiled UncScaleMax\n");
    printf("  -live   leave the mapping in circuit; measures the tree it shapes, not the "
           "distribution\n");
}

int unc_probe_command(char *args) {
    int depth          = UNC_PROBE_DEPTH;
    const char *epd    = NULL;
    const char *out    = NULL;
    const char *refArg = NULL;
    int capOverride    = 0;

    ProbeLive    = false;
    char *cursor = args;
    for (char *tok = next_arg(&cursor); tok; tok = next_arg(&cursor)) {
        if (tok[0] != '-') {
            depth = atoi(tok);
            if (depth < 1 || depth > 60) {
                printf("probe unc: depth %s is not between 1 and 60\n", tok);
                return 1;
            }
            continue;
        }
        if (strcmp(tok, "-live") == 0) {
            ProbeLive = true;
            continue;
        }
        if (strcmp(tok, "-help") == 0 || strcmp(tok, "-h") == 0) {
            usage();
            return 0;
        }

        char *val = next_arg(&cursor);
        if (!val) {
            usage();
            return 1;
        }
        if (strcmp(tok, "-epd") == 0)
            epd = val;
        else if (strcmp(tok, "-o") == 0)
            out = val;
        else if (strcmp(tok, "-ref") == 0)
            refArg = val;
        else if (strcmp(tok, "-cap") == 0)
            capOverride = atoi(val);
        else {
            usage();
            return 1;
        }
    }

    UncMapping m;
    search_unc_mapping(&m);
    const int cap = capOverride > 0 ? capOverride : m.cap;

    memset(Hist, 0, sizeof(Hist));
    Samples = SignalSum = Overflow = 0;
    SignalMax                      = 0;

    int positions       = 0;
    const int64_t start = time_ms();
    int failed          = probe_corpus(epd, depth, &positions);
    const int64_t ms    = time_ms() - start;

    if (failed && positions == 0)
        return 1;

    Dist now;
    if (!dist_from_histogram(&now, &m)) {
        printf("probe unc: out of memory\n");
        return 1;
    }
    now.depth = depth;
#ifdef EVAL_NNUE
    snprintf(now.net, sizeof(now.net), "%.12s", nnue_hash());
#else
    snprintf(now.net, sizeof(now.net), "classical");
#endif

    printf("\n===========================\n");
    printf("unc probe   : %s, %d positions, depth %d, %lldms\n", now.net, positions, depth,
           (long long)ms);
    printf("scaling     : %s\n",
           ProbeLive ? "LIVE - the mapping shaped the tree this was measured on"
                     : "neutral - the mapping could not shape the tree it was measured on");
    if (now.samples == 0) {
        printf("no node consulted unc_scale(). Nothing to report.\n");
        dist_free(&now);
        return 1;
    }

    report_signal(&now);

    printf("\nmapping:\n");
    report_mapping(&now, m.base, m.slope, cap, "shipped ");
    const int centred = best_base(&now, m.slope, cap, 100.0);
    report_mapping(&now, centred, m.slope, cap, "centred ");

    report_sweep(&now, cap, m.slope);

    Dist ref;
    bool haveRef = false;
    if (refArg && (haveRef = dist_read(&ref, refArg)))
        report_reference(&now, &ref, cap);

    if (!haveRef) {
        printf("\nre-centring at the shipped slope (E20's rule, mean 100):\n");
        printf("  %s %d   %s %d\n", now.sigma ? "UNC_SIGMA_BASE" : "UNC_SCALE_BASE", centred,
               now.sigma ? "UNC_SIGMA_SLOPE" : "UNC_SCALE_SLOPE", m.slope);
        printf("  option.%s=%d option.%s=%d\n", now.sigma ? "UncSigmaBase" : "UncScaleBase",
               centred, now.sigma ? "UncSigmaSlope" : "UncScaleSlope", m.slope);
        printf("\n  Pass -ref <a run on the net the constants were fitted for> to match its\n"
               "  spread as well, which is what carries an SPSA fit across a retrain.\n");
    }

    if (out && !dist_write(&now, out, positions))
        failed = 1;

    if (haveRef)
        dist_free(&ref);
    dist_free(&now);
    return failed ? 1 : 0;
}

/*
 * `probe err`: is the net ever confident and wrong, and how badly? Where `probe unc`
 * measures what the mapping READS, this pairs the signal at a node with the error the
 * search went on to find there - the quantity every margin insures against.
 *
 * The question is about the TAIL, not the average: a margin does not care what the typical
 * error is at a given confidence, it cares how often the error exceeds it. The error is
 * recorded SIGNED because the two directions are different prunes - reverse futility bets
 * the evaluation is not too high, futility and delta that it is not too low.
 */
#define ERR_BUCKETS 4096

/* Band edges, upper-exclusive, in centipawns of signal. Finer where the shipped mapping's
 * floor lives and the interesting question is; the last bands only have to exist. */
static const int SigmaEdges[] = {8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 1024, 1 << 30};
#define SIGMA_BANDS ((int)(sizeof(SigmaEdges) / sizeof(SigmaEdges[0])))

static uint64_t ErrNeg[SIGMA_BANDS][ERR_BUCKETS];
static uint64_t ErrPos[SIGMA_BANDS][ERR_BUCKETS];
static uint64_t BandNodes[SIGMA_BANDS], BandDecisive[SIGMA_BANDS];
static uint64_t OverNeg[SIGMA_BANDS], OverPos[SIGMA_BANDS];
static uint64_t BandSigmaSum[SIGMA_BANDS];
static int BandErrMin[SIGMA_BANDS], BandErrMax[SIGMA_BANDS];
static int ResidMinDepth = 1;
static int ResidMaxDepth = 99;
static bool ResidExactOnly;

static int sigma_band(int signal) {
    for (int i = 0; i < SIGMA_BANDS; ++i)
        if (signal < SigmaEdges[i])
            return i;
    return SIGMA_BANDS - 1;
}

/* A mate or tablebase score is not an evaluation error of a size, and letting an
 * 8,000,000cp "residual" into the percentiles would make every band look identical - so it
 * is counted on its own, which is the more useful number anyway. Overflow is kept on its
 * own side: counted without its sign it lands in the wrong tail, and in a thin band that
 * is the whole answer. */
void unc_probe_residual(int signal, int err, bool exact, bool decisive, int depth) {
    if (depth < ResidMinDepth || depth > ResidMaxDepth || (ResidExactOnly && !exact))
        return;

    const int b = sigma_band(signal < 0 ? 0 : signal);
    ++BandNodes[b];
    BandSigmaSum[b] += (uint64_t)(signal < 0 ? 0 : signal);

    if (decisive) {
        ++BandDecisive[b];
        return;
    }

    if (err < BandErrMin[b])
        BandErrMin[b] = err;
    if (err > BandErrMax[b])
        BandErrMax[b] = err;

    const int mag = err < 0 ? -err : err;
    if (mag >= ERR_BUCKETS) {
        ++(err < 0 ? OverNeg : OverPos)[b];
        return;
    }
    if (err < 0)
        ++ErrNeg[b][mag];
    else
        ++ErrPos[b][mag];
}

/* What a band's percentiles are drawn from: all it saw but the decisive scores. */
static uint64_t band_sized(int b) { return BandNodes[b] - BandDecisive[b]; }

/* The signed error at quantile `q` of a band, walking the two half-histograms from the most
 * negative error upward. Overflow sits at both ends and is answered with the band's true
 * extreme, and at least one observation is required or the target is met before the walk
 * starts. */
static int band_quantile(int b, double q) {
    const uint64_t n = band_sized(b);
    if (n == 0)
        return 0;

    uint64_t target = (uint64_t)(q * (double)n);
    if (target < 1)
        target = 1;

    uint64_t seen = OverNeg[b];
    if (seen >= target)
        return -(ERR_BUCKETS - 1);

    for (int i = ERR_BUCKETS - 1; i >= 1; --i) {
        seen += ErrNeg[b][i];
        if (seen >= target)
            return -i;
    }
    for (int i = 0; i < ERR_BUCKETS; ++i) {
        seen += ErrPos[b][i];
        if (seen >= target)
            return i;
    }
    return BandErrMax[b];
}

/* The share of a band's observations at or beyond `k` centipawns, on the side `k`'s sign
 * names. This is the number a margin actually buys against. */
static double band_exceedance(int b, int k) {
    const uint64_t n = band_sized(b);
    if (n == 0)
        return 0.0;

    uint64_t hit = k < 0 ? OverNeg[b] : OverPos[b];
    if (k < 0) {
        for (int i = -k; i < ERR_BUCKETS; ++i)
            hit += ErrNeg[b][i];
    } else {
        for (int i = k; i < ERR_BUCKETS; ++i)
            hit += ErrPos[b][i];
    }
    return 100.0 * (double)hit / (double)n;
}

/* Below this a band cannot support the quantile the tables ask of it, and a number computed
 * from forty observations reads exactly like one from forty thousand. Marked rather than
 * hidden: which bands are thin is itself something to know. */
#define BAND_MIN_OBS 400

static void resid_usage(void) {
    printf("usage: probe err [depth] [-epd <file>] [-mindepth <n>] [-exact]\n");
    printf("  depth      search depth for every position          (default %d)\n", UNC_PROBE_DEPTH);
    printf("  -epd       corpus to search instead of the bench positions\n");
    printf("  -mindepth  ignore nodes shallower than this         (default 1)\n");
    printf("  -maxdepth  ignore nodes deeper than this            (default 99)\n");
    printf("  -exact     only nodes whose score is an exact value, not a bound\n");
}

int unc_probe_err_command(char *args) {
    int depth       = UNC_PROBE_DEPTH;
    const char *epd = NULL;

    ResidMinDepth  = 1;
    ResidMaxDepth  = 99;
    ResidExactOnly = false;

    char *cursor = args;
    for (char *tok = next_arg(&cursor); tok; tok = next_arg(&cursor)) {
        if (tok[0] != '-') {
            depth = atoi(tok);
            if (depth < 1 || depth > 60) {
                printf("probe err: depth %s is not between 1 and 60\n", tok);
                return 1;
            }
            continue;
        }
        if (strcmp(tok, "-exact") == 0) {
            ResidExactOnly = true;
            continue;
        }
        if (strcmp(tok, "-help") == 0 || strcmp(tok, "-h") == 0) {
            resid_usage();
            return 0;
        }

        char *val = next_arg(&cursor);
        if (!val) {
            resid_usage();
            return 1;
        }
        if (strcmp(tok, "-epd") == 0)
            epd = val;
        else if (strcmp(tok, "-mindepth") == 0)
            ResidMinDepth = atoi(val);
        else if (strcmp(tok, "-maxdepth") == 0)
            ResidMaxDepth = atoi(val);
        else {
            resid_usage();
            return 1;
        }
    }

    memset(ErrNeg, 0, sizeof(ErrNeg));
    memset(ErrPos, 0, sizeof(ErrPos));
    memset(BandNodes, 0, sizeof(BandNodes));
    memset(BandDecisive, 0, sizeof(BandDecisive));
    memset(OverNeg, 0, sizeof(OverNeg));
    memset(OverPos, 0, sizeof(OverPos));
    memset(BandSigmaSum, 0, sizeof(BandSigmaSum));
    for (int b = 0; b < SIGMA_BANDS; ++b) {
        BandErrMin[b] = 0;
        BandErrMax[b] = 0;
    }

    UncMapping m;
    search_unc_mapping(&m);

    int positions       = 0;
    const int64_t start = time_ms();
    const int failed    = probe_corpus(epd, depth, &positions);
    const int64_t ms    = time_ms() - start;
    if (failed && positions == 0)
        return 1;

    uint64_t total = 0, totalDecisive = 0;
    for (int b = 0; b < SIGMA_BANDS; ++b) {
        total += BandNodes[b];
        totalDecisive += BandDecisive[b];
    }

    printf("\n===========================\n");
#ifdef EVAL_NNUE
    printf("residual probe: net %.12s, %d positions, depth %d, %lldms\n", nnue_hash(), positions,
           depth, (long long)ms);
#else
    printf("residual probe: classical, %d positions, depth %d, %lldms\n", positions, depth,
           (long long)ms);
#endif
    printf("pairs         : %s at a node, against (search - staticEval) at the same node\n",
           m.sigma ? "sigma" : "|correction|");
    printf("filters       : depth %d-%d%s; in-check and singular-verification nodes never pair\n",
           ResidMinDepth, ResidMaxDepth, ResidExactOnly ? ", exact scores only" : "");
    printf("scaling       : neutral, so the mapping did not shape the tree\n");

    if (total == 0) {
        printf("no node paired. Nothing to report.\n");
        return 1;
    }
    printf("paired        : %llu nodes, %llu of them a mate or tablebase result\n\n",
           (unsigned long long)total, (unsigned long long)totalDecisive);

    bool thin = false;
    printf("signal band     nodes   mean  |    p1    p5   p50    p95    p99  |  worst   <=-100"
           "  >=+100 | mate/TB\n");
    for (int b = 0; b < SIGMA_BANDS; ++b) {
        if (BandNodes[b] == 0)
            continue;

        const int lo       = b == 0 ? 0 : SigmaEdges[b - 1];
        const double share = 100.0 * (double)BandNodes[b] / (double)total;
        const double meanS = (double)BandSigmaSum[b] / (double)BandNodes[b];

        char band[24];
        if (b == SIGMA_BANDS - 1)
            snprintf(band, sizeof(band), "%5d+       ", lo);
        else
            snprintf(band, sizeof(band), "%5d - %5d", lo, SigmaEdges[b] - 1);
        if (band_sized(b) < BAND_MIN_OBS)
            band[strlen(band) - 1] = '*';

        printf("%s  %5.1f%%  %5.0f  | %5d %5d %5d  %5d  %5d  | %6d  %5.2f%%  %5.2f%% | %5.2f%%\n",
               band, share, meanS, band_quantile(b, 0.01), band_quantile(b, 0.05),
               band_quantile(b, 0.50), band_quantile(b, 0.95), band_quantile(b, 0.99),
               BandErrMin[b], band_exceedance(b, -100), band_exceedance(b, 100),
               100.0 * (double)BandDecisive[b] / (double)BandNodes[b]);
        if (band_sized(b) < BAND_MIN_OBS)
            thin = true;
    }

    if (thin)
        printf("(a band marked * has fewer than %d sized observations: its p1 and p99 are"
               " noise)\n",
               BAND_MIN_OBS);

    int refDown = 0, refUp = 0;
    {
        uint64_t sized = 0;
        for (int b = 0; b < SIGMA_BANDS; ++b)
            sized += band_sized(b);

        const uint64_t targetLo = (uint64_t)(0.01 * (double)sized);
        const uint64_t targetHi = (uint64_t)(0.99 * (double)sized);
        uint64_t seen           = 0;
        for (int i = ERR_BUCKETS - 1; i >= 1; --i) {
            for (int b = 0; b < SIGMA_BANDS; ++b)
                seen += ErrNeg[b][i];
            if (!refDown && seen >= targetLo)
                refDown = i;
        }
        for (int i = 0; i < ERR_BUCKETS; ++i) {
            for (int b = 0; b < SIGMA_BANDS; ++b)
                seen += ErrPos[b][i];
            if (!refUp && seen >= targetHi)
                refUp = i;
        }
    }

    printf("\nwhat a 1%% error rate costs in each band, against what the mapping charges\n");
    printf("(100 = the whole tree; the mapping is min(%d + signal * %d / %d, %d))\n", m.base,
           m.slope, m.grain, m.cap);
    printf("signal band     empirical down   empirical up   mapping\n");
    for (int b = 0; b < SIGMA_BANDS; ++b) {
        if (band_sized(b) == 0)
            continue;

        const int lo    = b == 0 ? 0 : SigmaEdges[b - 1];
        const int down  = -band_quantile(b, 0.01);
        const int up    = band_quantile(b, 0.99);
        const int meanS = (int)((double)BandSigmaSum[b] / (double)BandNodes[b]);
        int mapped      = m.base + meanS * m.slope / m.grain;
        if (mapped > m.cap)
            mapped = m.cap;

        char band[24];
        if (b == SIGMA_BANDS - 1)
            snprintf(band, sizeof(band), "%5d+       ", lo);
        else
            snprintf(band, sizeof(band), "%5d - %5d", lo, SigmaEdges[b] - 1);
        if (band_sized(b) < BAND_MIN_OBS)
            band[strlen(band) - 1] = '*';

        printf("%s        %6.0f         %6.0f      %4d\n", band,
               refDown ? 100.0 * (double)down / (double)refDown : 0.0,
               refUp ? 100.0 * (double)up / (double)refUp : 0.0, mapped);
    }
    printf("\nthe whole tree's 1%% margins: %d cp down, %d cp up\n", refDown, refUp);

    return failed ? 1 : 0;
}

#endif
