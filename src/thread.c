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

int thread_hardware_concurrency(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors > 0 ? (int)info.dwNumberOfProcessors : 1;
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
