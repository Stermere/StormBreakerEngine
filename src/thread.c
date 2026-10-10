/* thread.c - Win32 / pthreads implementations of the thread.h shim. */

/* -std=c17 hides POSIX declarations from glibc; this must precede every header. Not on
 * Darwin, where it would hide _SC_NPROCESSORS_ONLN. */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "thread.h"

#include <stdlib.h>
#include <string.h>

/* A line to MAX_PLY, with singular re-entries, needs several MB of stack; the platform
 * defaults are 1-2 MB. Reserved, not committed. The Makefile gives the main thread the
 * same, since synchronous searches run on it. */
#define THREAD_STACK_BYTES (8u * 1024u * 1024u)

#if defined(_WIN32)

typedef struct {
    ThreadEntry fn;
    void *arg;
} ThreadStart;

/* Adapts void(void*) to Win32's DWORD WINAPI signature. */
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

/* All processor groups; GetSystemInfo counts only the current one. */
int thread_hardware_concurrency(void) {
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
}

/* Processor groups with their real masks, read from the OS: active processors need not
 * be contiguous, and a mask naming an absent one is rejected. */
#define MAX_GROUPS 64

typedef struct {
    KAFFINITY mask;
    int cpus;
} ProcGroup;

static ProcGroup Groups[MAX_GROUPS];
static int GroupCount;
static int GroupCpuTotal;
static INIT_ONCE GroupsOnce = INIT_ONCE_STATIC_INIT;

/* Physical cores in OS order (group by group); `mask` is the core's logical processors.
 * 4096 = the 64 x 64 logical processors Windows can address. */
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
        /* Variable-sized records, one per core. */
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

        /* A truncated list would strand the pool on the cores that fit: no core binding. */
        if (at < end)
            CoreCount = 0;
    }

    free(info);
}

static void probe_groups(void) {
    DWORD len = 0;

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

/* Threads bind as they start, concurrently, so the probe runs exactly once. */
static BOOL CALLBACK probe_groups_once(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once;
    (void)param;
    (void)context;

    probe_groups();
    probe_cores();
    return TRUE;
}

/* Gives this thread a physical core of its own: the scheduler otherwise pairs threads on
 * SMT siblings while cores sit idle (9% slower at 8 threads on 8 cores). The ideal-
 * processor hint alone changes nothing; the affinity mask does. Every core gets a thread
 * before any gets a second. */
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

            SetThreadIdealProcessorEx(GetCurrentThread(), &pn, NULL);

            /* The whole core, so the thread can dodge a sibling busy with interrupts. */
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

    /* Core binding only once the pool fills the machine: a smaller pool runs faster left to
     * the scheduler (4 threads on 8 cores, 6% slower bound). */
    if (CoreCount > 0 && poolSize >= CoreCount && bind_to_core(index))
        return;

    /* With one group the default affinity already covers the machine. */
    if (GroupCount <= 1 || GroupCpuTotal <= 0)
        return;

    /* Fill groups in turn: a group is a NUMA node, and TT traffic is Lazy SMP's cost. */
    int slot = index % GroupCpuTotal;
    for (int g = 0; g < GroupCount; ++g) {
        if (slot < Groups[g].cpus) {
            GROUP_AFFINITY affinity;
            memset(&affinity, 0, sizeof(affinity));
            affinity.Group = (WORD)g;
            affinity.Mask  = Groups[g].mask;

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

/* No processor groups here, and pinning would only constrain the kernel. */
void thread_bind(int index, int poolSize) {
    (void)index;
    (void)poolSize;
}

/* nanosleep: POSIX.1-2008, requested above, removed usleep. */
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
