/* thread.c - Win32 / pthreads implementations of the thread.h shim. */

/*
 * -std=c17 sets __STRICT_ANSI__, which glibc reads as "ISO C and nothing else" and
 * so hides every POSIX declaration below; the request has to come before the first
 * header, because feature test macros are consulted only once. Darwin is excluded
 * deliberately - there _POSIX_C_SOURCE subtracts from the default visibility, and
 * would take the BSD-only _SC_NPROCESSORS_ONLN with it.
 */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "thread.h"

#include <stdlib.h>
#include <string.h>

/* Each search frame carries a MAX_MOVES move list, so a line running to MAX_PLY needs
 * on the order of half a megabyte, more under a sanitizer - and the Win32 default of
 * 1 MB is uncomfortably close. Reserved address space is committed only as it is
 * touched, so asking for more costs nothing. */
#define THREAD_STACK_BYTES (8u * 1024u * 1024u)

#if defined(_WIN32)

typedef struct {
    ThreadEntry fn;
    void *arg;
} ThreadStart;

/* Win32 entry points must return DWORD and use the stdcall ABI, so the caller's
 * void(void*) is smuggled through this trampoline. */
static DWORD WINAPI thread_trampoline(LPVOID param) {
    ThreadStart *start = (ThreadStart *)param;
    ThreadEntry fn     = start->fn;
    void *arg          = start->arg;
    HeapFree(GetProcessHeap(), 0, start);
    fn(arg);
    return 0;
}

bool thread_create(ThreadHandle *handle, ThreadEntry fn, void *arg) {
    ThreadStart *start = (ThreadStart *)HeapAlloc(GetProcessHeap(), 0, sizeof(ThreadStart));
    if (!start)
        return false;

    start->fn  = fn;
    start->arg = arg;

    *handle = CreateThread(NULL, THREAD_STACK_BYTES, thread_trampoline, start, 0, NULL);
    if (!*handle) {
        HeapFree(GetProcessHeap(), 0, start);
        return false;
    }
    return true;
}

void thread_join(ThreadHandle handle) {
    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
}

/*
 * Every processor group, not the one this process happens to be in. GetSystemInfo
 * reports the CURRENT group only, so on a two-group 128-core machine it answers 64
 * and the engine would size itself to half the box.
 */
int thread_hardware_concurrency(void) {
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
}

/*
 * The processor groups and their real affinity masks. Read from the OS rather than
 * synthesised as the low `count` bits of a word: a group's active processors need
 * not be contiguous once any of them is parked or offline, and a mask naming a
 * processor that is not there is rejected outright, leaving the thread where it was.
 */
#define MAX_GROUPS 64

typedef struct {
    KAFFINITY mask;
    int cpus;
} ProcGroup;

static ProcGroup Groups[MAX_GROUPS];
static int GroupCount;
static int GroupCpuTotal;
static INIT_ONCE GroupsOnce = INIT_ONCE_STATIC_INIT;

/*
 * The physical cores, in the order the OS reports them - which is group by group, so
 * indexing this fills a group before moving to the next one, the same policy the group
 * binding below follows.
 *
 * `mask` is the logical processors that share this core. On a machine with SMT there are
 * two, and they share everything that matters to a search: the level 1 and 2 caches, and
 * the execution units the evaluation's vector work is bound by.
 */
/* 64 groups of 64 logical processors is the most Windows addresses, so this covers
 * every core on the largest machine that can exist even without SMT. */
#define MAX_CORES 4096

typedef struct {
    WORD group;
    KAFFINITY mask;
} PhysCore;

static PhysCore Cores[MAX_CORES];
static int CoreCount;

static void probe_cores(void) {
    DWORD len = 0;

    if (GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return;

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *const info =
        (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)malloc(len);
    if (!info)
        return;

    if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) {
        /* One variable-sized record per core, walked by its own Size field - unlike the
         * RelationGroup query above, which answers with a single record. */
        const char *const end = (const char *)info + len;

        const char *at = (const char *)info;

        while (at < end && CoreCount < MAX_CORES) {
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *const e =
                (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(const void *)at;

            if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount >= 1) {
                Cores[CoreCount].group = e->Processor.GroupMask[0].Group;
                Cores[CoreCount].mask  = e->Processor.GroupMask[0].Mask;
                ++CoreCount;
            }
            at += e->Size;
        }

        /* If the list did not fit, CoreCount is a lie: it would read as a small machine
         * and strand the pool on the cores that happened to fit. Disable core binding and
         * let the group binding below place these threads. */
        if (at < end)
            CoreCount = 0;
    }

    free(info);
}

static void probe_groups(void) {
    DWORD len = 0;

    /* Asks for the size first: the record is variably sized, one entry per group. */
    if (GetLogicalProcessorInformationEx(RelationGroup, NULL, &len) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return;

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info =
        (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)malloc(len);
    if (!info)
        return;

    if (GetLogicalProcessorInformationEx(RelationGroup, info, &len)) {
        const int groups = (int)info->Group.ActiveGroupCount;

        for (int g = 0; g < groups && g < MAX_GROUPS; ++g) {
            Groups[GroupCount].mask = info->Group.GroupInfo[g].ActiveProcessorMask;
            Groups[GroupCount].cpus = (int)info->Group.GroupInfo[g].ActiveProcessorCount;
            GroupCpuTotal += Groups[GroupCount].cpus;
            ++GroupCount;
        }
    }

    free(info);
}

/* Threads are bound as they start, so several can reach this at once and the
 * accumulation in probe_groups() is not something two of them may do at the same
 * time. InitOnceExecuteOnce rather than a flag: a flag IS the race. */
static BOOL CALLBACK probe_groups_once(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;

    probe_groups();
    probe_cores();
    return TRUE;
}

/*
 * Give this thread a physical core of its own.
 *
 * Left to itself the scheduler will seat two search threads on the two halves of one
 * physical core while another core sits idle, and eight threads on eight cores measured
 * 9% slower when it does: the pair share a level 1 cache and the vector units the
 * network runs on, so the second thread is not getting a core, it is getting a share of
 * one.
 *
 * SetThreadIdealProcessorEx is asked first and is not enough on its own - it is accepted,
 * reports success, and changes the placement by 0.1%, which is to say not at all. The
 * affinity below is what actually moves the threads apart; the hint is kept because it
 * tells the scheduler which half of the core to prefer, which the mask does not say.
 *
 * `index / CoreCount` is which sibling: every core gets a thread before any core gets a
 * second one, which is the order that matters on a pool the size of the machine.
 */
static bool bind_to_core(int index) {
    if (CoreCount <= 0)
        return false;

    const PhysCore *const core = &Cores[index % CoreCount];

    int siblings = 0;
    for (int i = 0; i < 64; ++i)
        if (core->mask & ((KAFFINITY)1 << i))
            ++siblings;

    if (siblings <= 0)
        return false;

    int want = (index / CoreCount) % siblings;

    for (int i = 0; i < 64; ++i) {
        if (!(core->mask & ((KAFFINITY)1 << i)))
            continue;

        if (want-- == 0) {
            PROCESSOR_NUMBER pn;
            memset(&pn, 0, sizeof(pn));
            pn.Group  = core->group;
            pn.Number = (BYTE)i;

            /* Survivable either way: a refused hint leaves the thread exactly where the
             * scheduler would have put it anyway. */
            SetThreadIdealProcessorEx(GetCurrentThread(), &pn, NULL);

            /*
             * The mask is the whole physical core rather than the one logical processor
             * named above, which costs nothing - no two threads are given the same core
             * until every core has one - and leaves the pair free for the thread to move
             * between, so a processor busy with device interrupts does not stall it.
             */
            GROUP_AFFINITY ga;
            memset(&ga, 0, sizeof(ga));
            ga.Group = core->group;
            ga.Mask  = core->mask;

            return SetThreadGroupAffinity(GetCurrentThread(), &ga, NULL) != 0;
        }
    }

    return false;
}

void thread_bind(int index, int poolSize) {
    InitOnceExecuteOnce(&GroupsOnce, probe_groups_once, NULL, NULL);

    /*
     * Only once the pool is at least as large as the machine.
     *
     * Below that there are cores to spare, and the scheduler spreading threads over all
     * of them, with the idle ones' thermal headroom - beats confining them to
     * the first few: a pool of four on eight cores measured 6% SLOWER bound than free,
     * and a pool of one is the whole engine on one thread (bench, datagen, one SPRT
     * game) with nothing to be separated from in any case.
     *
     * At or above the core count binding leaves no core idle, which is also what makes
     * it safe when the engine does not own the machine: index % CoreCount spreads every
     * instance across all the cores evenly, so two engines running at once oversubscribe
     * the way the scheduler would have anyway rather than piling onto the first core.
     */
    if (CoreCount > 0 && poolSize >= CoreCount && bind_to_core(index))
        return; /* That mask already names a group; the one below would only widen it. */

    /* One group is the ordinary case, and there the default affinity already covers
     * the whole machine - a mask could only take choices away from the scheduler. */
    if (GroupCount <= 1 || GroupCpuTotal <= 0)
        return;

    /* Fill each group in turn rather than interleaving: threads that share a group
     * share a NUMA node on every machine that has more than one group, and the
     * transposition table traffic between them is the whole cost of Lazy SMP. */
    int slot = index % GroupCpuTotal;
    for (int g = 0; g < GroupCount; ++g) {
        if (slot < Groups[g].cpus) {
            GROUP_AFFINITY affinity;
            memset(&affinity, 0, sizeof(affinity));
            affinity.Group = (WORD)g;
            affinity.Mask  = Groups[g].mask;

            /* Failure is survivable - the thread keeps the affinity it had, which is
             * every processor in its own group - so nothing is reported here. */
            SetThreadGroupAffinity(GetCurrentThread(), &affinity, NULL);
            return;
        }
        slot -= Groups[g].cpus;
    }
}

void thread_sleep_ms(int ms) { Sleep((DWORD)ms); }

void mutex_init(Mutex *m) { InitializeCriticalSection(m); }
void mutex_destroy(Mutex *m) { DeleteCriticalSection(m); }
void mutex_lock(Mutex *m) { EnterCriticalSection(m); }
void mutex_unlock(Mutex *m) { LeaveCriticalSection(m); }

void cond_init(CondVar *cv) { InitializeConditionVariable(cv); }
void cond_destroy(CondVar *cv) { (void)cv; }
void cond_wait(CondVar *cv, Mutex *m) { SleepConditionVariableCS(cv, m, INFINITE); }
void cond_signal(CondVar *cv) { WakeConditionVariable(cv); }
void cond_broadcast(CondVar *cv) { WakeAllConditionVariable(cv); }

#else

#include <time.h>
#include <unistd.h>

typedef struct {
    ThreadEntry fn;
    void *arg;
} ThreadStart;

static void *thread_trampoline(void *param) {
    ThreadStart *start = (ThreadStart *)param;
    ThreadEntry fn     = start->fn;
    void *arg          = start->arg;
    free(start);
    fn(arg);
    return NULL;
}

bool thread_create(ThreadHandle *handle, ThreadEntry fn, void *arg) {
    ThreadStart *start = (ThreadStart *)malloc(sizeof(ThreadStart));
    if (!start)
        return false;

    start->fn  = fn;
    start->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK_BYTES);

    const int rc = pthread_create(handle, &attr, thread_trampoline, start);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        free(start);
        return false;
    }
    return true;
}

void thread_join(ThreadHandle handle) { pthread_join(handle, NULL); }

int thread_hardware_concurrency(void) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* Nothing to do: there is no equivalent of a processor group, so a thread can
 * already be scheduled anywhere, and pinning it would only stop the kernel moving
 * it off a core somebody else is using. */
void thread_bind(int index, int poolSize) {
    (void)index;
    (void)poolSize;
}

/* nanosleep() rather than usleep(): POSIX.1-2008 removed the latter, so asking for
 * that level at the top of this file is precisely what makes it unavailable. */
void thread_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void mutex_init(Mutex *m) { pthread_mutex_init(m, NULL); }
void mutex_destroy(Mutex *m) { pthread_mutex_destroy(m); }
void mutex_lock(Mutex *m) { pthread_mutex_lock(m); }
void mutex_unlock(Mutex *m) { pthread_mutex_unlock(m); }

void cond_init(CondVar *cv) { pthread_cond_init(cv, NULL); }
void cond_destroy(CondVar *cv) { pthread_cond_destroy(cv); }
void cond_wait(CondVar *cv, Mutex *m) { pthread_cond_wait(cv, m); }
void cond_signal(CondVar *cv) { pthread_cond_signal(cv); }
void cond_broadcast(CondVar *cv) { pthread_cond_broadcast(cv); }

#endif
