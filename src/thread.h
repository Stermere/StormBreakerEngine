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
/* Processor groups are a Windows 7 API, and a machine with more than 64 logical
 * processors cannot be used without them - so the declarations are not optional
 * here, however old a default the toolchain picks. */
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
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

/* Logical processors this process may use - every processor group on Windows, not
 * just the one it was launched into. */
int thread_hardware_concurrency(void);

/*
 * Places the CALLING thread, which must be worker number `index`, on the machine.
 *
 * Only Windows does anything: a process is confined to a single processor group
 * unless a thread asks for another by name, so on a 128-core machine every thread
 * would pile onto the first 64 cores and half the machine would sit idle. Every
 * other platform schedules across the whole machine already, and pinning there
 * would take away the scheduler's ability to migrate a thread off a busy core.
 */
void thread_bind(int index);

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
