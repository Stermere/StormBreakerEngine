/*
 * thread.h - minimal portable threading primitives.
 *
 * Win32 threads on Windows, pthreads elsewhere. Deliberately not C11 <threads.h>:
 * MinGW-w64 does not ship it, and winpthreads would mean an extra DLL.
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

/* False if the thread could not be created, leaving *handle unspecified. */
bool thread_create(ThreadHandle *handle, ThreadEntry fn, void *arg);

void thread_join(ThreadHandle handle);

int thread_hardware_concurrency(void);

/* Only for parking a thread on an external event, such as a `stop` that has not
 * arrived - never inside the search. */
void thread_sleep_ms(int ms);

void mutex_init(Mutex *m);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);

void cond_init(CondVar *cv);
void cond_destroy(CondVar *cv);

/* Must be called with `m` held; re-acquires it before returning. */
void cond_wait(CondVar *cv, Mutex *m);
void cond_signal(CondVar *cv);
void cond_broadcast(CondVar *cv);

#endif
