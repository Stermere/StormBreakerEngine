/*
 * thread.h - minimal portable threading primitives.
 *
 * Win32 threads on Windows, pthreads elsewhere. Deliberately NOT C11
 * <threads.h>: MinGW-w64 does not ship it, and depending on winpthreads would
 * mean shipping an extra DLL or a much larger static binary.
 *
 * The engine needs exactly three things from a thread library today - start a
 * search worker, wait for it, and signal it to stop - so this stays a thin
 * shim rather than a thread pool. Lazy SMP will need condition variables to
 * park idle workers, which is why they are already wrapped here.
 */
#ifndef THREAD_H
#define THREAD_H

#include <stdbool.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef HANDLE ThreadHandle;
typedef CRITICAL_SECTION Mutex;
typedef CONDITION_VARIABLE CondVar;

#else
#include <pthread.h>

typedef pthread_t ThreadHandle;
typedef pthread_mutex_t Mutex;
typedef pthread_cond_t CondVar;
#endif

typedef void (*ThreadEntry)(void *);

/* Starts `fn(arg)` on a new thread. Returns false if the thread could not be
 * created, in which case *handle is unspecified. */
bool thread_create(ThreadHandle *handle, ThreadEntry fn, void *arg);

/* Blocks until the thread finishes, then releases its resources. */
void thread_join(ThreadHandle handle);

/* Number of hardware threads, clamped to at least 1. */
int thread_hardware_concurrency(void);

/* Yields the CPU for roughly `ms` milliseconds. Used only to park a thread
 * that is waiting on an external event (a `stop` that has not arrived yet) -
 * never inside the search. */
void thread_sleep_ms(int ms);

void mutex_init(Mutex *m);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);

void cond_init(CondVar *cv);
void cond_destroy(CondVar *cv);
/* Must be called with `m` held; re-acquires `m` before returning. */
void cond_wait(CondVar *cv, Mutex *m);
void cond_signal(CondVar *cv);
void cond_broadcast(CondVar *cv);

#endif /* THREAD_H */
