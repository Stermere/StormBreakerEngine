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
    return TRUE;
}

void thread_bind(int index) {
    InitOnceExecuteOnce(&GroupsOnce, probe_groups_once, NULL, NULL);

    /* One group is the ordinary case, and there the default affinity already covers
     * the whole machine - binding could only take choices away from the scheduler. */
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
void thread_bind(int index) { (void)index; }

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
