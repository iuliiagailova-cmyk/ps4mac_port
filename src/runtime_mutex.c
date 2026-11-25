/* Linux pthread-backed opaque PS4 mutex handles. No guest threads yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
typedef struct { int type; } GuestAttr;
/* locked_seq: when the current holder took it (lock_seq order), for the late signal below. */
typedef struct { pthread_mutex_t native; uint64_t locked_seq; } GuestMutex;
static uint64_t lock_seq; /* orders mutex acquisitions and condition variable signals */
static void note_locked(GuestMutex *m) {
    __atomic_store_n(&m->locked_seq, __atomic_add_fetch(&lock_seq, 1, __ATOMIC_SEQ_CST), __ATOMIC_SEQ_CST);
}
static size_t created, locks, unlocks;
static int32_t orbis_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case EDEADLK: code=11; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
/* SPARTAN_HANG_DUMP: the last scePthread mutex/cond operations, kept in memory (no output while
 * the game runs, so the timing stays) and printed by the hang dump (runtime_mutex_dump_trace). */
enum { SYNC_TRACE = 4096 };
typedef struct { uint64_t ns; const void *handle, *other; char thread[16]; char op; int32_t result; } SyncEvent;
static SyncEvent sync_trace[SYNC_TRACE];
static uint64_t sync_next;
static int sync_tracing(void) {
    static int on = -1;
    if (on < 0) on = getenv("SPARTAN_HANG_DUMP") != NULL;
    return on;
}
static __thread char sync_thread_name[16];
static void sync_event(char op, const void *handle, const void *other, int32_t result) {
    if (!sync_tracing()) return;
    if (!sync_thread_name[0] && pthread_getname_np(pthread_self(), sync_thread_name, sizeof(sync_thread_name)))
        snprintf(sync_thread_name, sizeof(sync_thread_name), "?");
    if (!sync_thread_name[0]) snprintf(sync_thread_name, sizeof(sync_thread_name), "main");
    /* Idle job workers wait with a timeout all the time and would push the rest out. */
    if (!strncmp(sync_thread_name, "Job Thread", 10)) return;
    SyncEvent *e = &sync_trace[__atomic_fetch_add(&sync_next, 1, __ATOMIC_RELAXED) % SYNC_TRACE];
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    e->ns = (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
    e->handle = handle; e->other = other; e->op = op; e->result = result;
    memcpy(e->thread, sync_thread_name, sizeof(e->thread));
}
void runtime_mutex_dump_trace(FILE *out, unsigned count) {
    const uint64_t end = __atomic_load_n(&sync_next, __ATOMIC_RELAXED);
    const uint64_t start = end > count ? end - count : 0;
    fprintf(out, "Sync trace: last %llu of %llu mutex/cond operations (L lock, U unlock, T trylock, "
                 "W cond wait, w wait returned, l wait ended by an earlier signal, S signal, B broadcast, I mutex init, C cond init)\n",
            (unsigned long long)(end - start), (unsigned long long)end);
    for (uint64_t i = start; i < end; ++i) {
        const SyncEvent *e = &sync_trace[i % SYNC_TRACE];
        fprintf(out, "Sync trace: %llu.%06llu %-15.15s %c %p %p -> %#x\n",
                (unsigned long long)(e->ns / 1000000000u), (unsigned long long)(e->ns % 1000000000u / 1000),
                e->thread, e->op, e->handle, e->other, (unsigned)e->result);
    }
}
static ABI int32_t attr_init(GuestAttr **out) {
    if (!out) return orbis_error(EINVAL);
    GuestAttr *attr = malloc(sizeof(*attr));
    if (!attr) return orbis_error(ENOMEM);
    attr->type = 1; *out = attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*attr)->type = type; return 0;
}
/* Priority protocols need a PS4 scheduler; the value is validated and recorded only. */
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    return 0;
}
static ABI int32_t attr_destroy(GuestAttr **attr) {
    if (!attr || !*attr) return orbis_error(EINVAL);
    free(*attr); *attr = NULL; return 0;
}
static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return orbis_error(EINVAL);
    int type = attr ? (*attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *mutex = malloc(sizeof(*mutex));
    if (!mutex) return orbis_error(ENOMEM);
    pthread_mutexattr_t native_attr;
    int e = pthread_mutexattr_init(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    int native_type = type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 3 ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
    e = pthread_mutexattr_settype(&native_attr, native_type);
    if (!e) e = pthread_mutex_init(&mutex->native, &native_attr);
    pthread_mutexattr_destroy(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    mutex->locked_seq = 0;
    *out = mutex; ++created; sync_event('I', out, mutex, 0); return 0;
}
static pthread_mutex_t static_init = PTHREAD_MUTEX_INITIALIZER;
/* Statically initialized handles (0/1) are created once, even under contention. */
static int32_t ensure_mutex(GuestMutex **mutex) {
    if (!mutex) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)mutex, __ATOMIC_ACQUIRE);
    if (value == 2) return orbis_error(EINVAL);
    if (value >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*mutex < 2) {
        GuestMutex *created_mutex = NULL;
        e = mutex_init(&created_mutex, NULL, NULL);
        if (!e) __atomic_store_n(mutex, created_mutex, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t mutex_lock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(pthread_mutex_lock(&(*mutex)->native));
    if (!e) { ++locks; note_locked(*mutex); }
    sync_event('L', mutex, *mutex, e);
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(pthread_mutex_trylock(&(*mutex)->native));
    if (!e) { ++locks; note_locked(*mutex); }
    sync_event('T', mutex, *mutex, e);
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return orbis_error(EPERM);
    int32_t e = orbis_error(pthread_mutex_unlock(&(*mutex)->native));
    if (!e) ++unlocks;
    sync_event('U', mutex, *mutex, e);
    return e;
}
static ABI int32_t mutex_destroy(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return 0;
    int e = pthread_mutex_destroy(&(*mutex)->native);
    if (!e) { free(*mutex); *mutex = (GuestMutex *)(uintptr_t)2; }
    return orbis_error(e);
}
static int deadline_after(struct timespec *end, uint64_t usec) {
    if (clock_gettime(CLOCK_REALTIME, end)) return errno;
    uint64_t ns = (uint64_t)end->tv_nsec + (usec % 1000000) * 1000;
    end->tv_sec += (time_t)(usec / 1000000 + ns / 1000000000);
    end->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
static int32_t timed_error(int e) { return e == ETIMEDOUT ? (int32_t)UINT32_C(0x8002003c) : orbis_error(e); }
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    e = timed_error(pthread_mutex_timedlock(&(*mutex)->native, &end));
    if (!e) { ++locks; note_locked(*mutex); }
    return e;
}
/* Condition variables: opaque handles over host pthread_cond (CLOCK_REALTIME,
 * like FreeBSD's default), sharing the host mutex behind GuestMutex. */
typedef struct { pthread_cond_t native; uint64_t late_signal_ns, late_signal_seq; int waited; } GuestCond;
static size_t conds, waits, wakeups, late_wakeups;
/* The start handshake of a new thread, as the PS4 scheduler orders it. Ratchet & Clank's NpToolkit
 * start locks a mutex, creates 'Toolkit::NP' (priority 700), then waits on a new condition
 * variable without a predicate; the new thread signals it without taking that mutex. On PS4 the
 * new thread does not run before its creator waits; on the host it starts at once on another
 * core, and in ~2% of starts (1 in 10 with the main thread sampled) its signal came 0.2 ms before
 * the wait, and the game hung for good. So a signal sent before anyone has ever waited on the
 * condition variable ends its first wait, if it comes within 50 ms and the waiter has held its
 * mutex since before the signal (it was on its way to the wait). Any wider rule (a recent signal
 * ends the next wait) broke other waits of the game without a predicate (4 crashes in 30 starts).
 * SPARTAN_COND_LATE_SIGNAL=0 turns it off. */
enum { LATE_SIGNAL_NS = 50000000 };
static uint64_t cond_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec + 1; /* never 0 */
}
static int late_signals(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SPARTAN_COND_LATE_SIGNAL"); on = !(e && e[0] == '0'); }
    return on;
}
static void note_signal(GuestCond *c) {
    if (!late_signals() || __atomic_load_n(&c->waited, __ATOMIC_SEQ_CST)) return;
    __atomic_store_n(&c->late_signal_seq, __atomic_add_fetch(&lock_seq, 1, __ATOMIC_SEQ_CST), __ATOMIC_SEQ_CST);
    __atomic_store_n(&c->late_signal_ns, cond_now_ns(), __ATOMIC_SEQ_CST);
}
/* At the start of a wait: whether it is the first wait on c and a signal came before it, as above. */
static int take_late_signal(GuestCond *c, GuestMutex *m) {
    if (__atomic_exchange_n(&c->waited, 1, __ATOMIC_SEQ_CST)) return 0;
    const uint64_t at = __atomic_load_n(&c->late_signal_ns, __ATOMIC_SEQ_CST);
    if (!at || cond_now_ns() - at > LATE_SIGNAL_NS) return 0;
    if (__atomic_load_n(&m->locked_seq, __ATOMIC_SEQ_CST) >= __atomic_load_n(&c->late_signal_seq, __ATOMIC_SEQ_CST))
        return 0;
    ++late_wakeups;
    printf("Runtime: condition variable %p: its first wait ended by the signal sent %.2f ms before it "
           "(a thread start handshake)\n", (void *)c, (double)(cond_now_ns() - at) / 1e6);
    return 1;
}
static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return orbis_error(EINVAL);
    GuestCond *c = malloc(sizeof(*c));
    if (!c) return orbis_error(ENOMEM);
    int e = pthread_cond_init(&c->native, NULL);
    if (e) { free(c); return orbis_error(e); }
    c->late_signal_ns = c->late_signal_seq = 0;
    c->waited = 0;
    *out = c; ++conds; sync_event('C', out, c, 0); return 0;
}
static int32_t ensure_cond(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if (__atomic_load_n((uintptr_t *)cond, __ATOMIC_ACQUIRE) >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*cond < 2) {
        GuestCond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(cond, c, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if ((uintptr_t)*cond < 2) return 0;
    int e = pthread_cond_destroy(&(*cond)->native);
    if (!e) { free(*cond); *cond = NULL; }
    return orbis_error(e);
}
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3) return orbis_error(EINVAL);
    ++waits;
    sync_event('W', cond, mutex, 0);
    if (take_late_signal(*cond, *mutex)) { sync_event('l', cond, mutex, 0); return 0; }
    e = orbis_error(pthread_cond_wait(&(*cond)->native, &(*mutex)->native));
    note_locked(*mutex);
    sync_event('w', cond, mutex, e);
    return e;
}
static int32_t cond_wait_until(GuestCond **cond, GuestMutex **mutex, const struct timespec *end) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3 || !end) return orbis_error(EINVAL);
    ++waits;
    sync_event('W', cond, mutex, 1);
    if (take_late_signal(*cond, *mutex)) { sync_event('l', cond, mutex, 0); return 0; }
    e = timed_error(pthread_cond_timedwait(&(*cond)->native, &(*mutex)->native, end));
    note_locked(*mutex);
    sync_event('w', cond, mutex, e);
    return e;
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    return cond_wait_until(cond, mutex, &end);
}
static ABI int32_t cond_signal(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; sync_event('S', cond, *cond, 0); note_signal(*cond);
    return orbis_error(pthread_cond_signal(&(*cond)->native));
}
static ABI int32_t cond_broadcast(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; sync_event('B', cond, *cond, 0); note_signal(*cond);
    return orbis_error(pthread_cond_broadcast(&(*cond)->native));
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static ABI int32_t cond_abs_timedwait(GuestCond **cond, GuestMutex **mutex, const GuestTimespec *abs) {
    /* nsec of 1e9 or more is carried (Ratchet & Clank normalizes with "> 1e9", see runtime_sync.c). */
    if (!abs || abs->nsec < 0) return orbis_error(EINVAL);
    struct timespec end = {.tv_sec = (time_t)(abs->sec + abs->nsec / 1000000000), .tv_nsec = (long)(abs->nsec % 1000000000)};
    return cond_wait_until(cond, mutex, &end);
}
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
static ABI int32_t posix_attr_init(GuestAttr **out) { return posix_result(attr_init(out)); }
static ABI int32_t posix_attr_type(GuestAttr **attr,int type) { return posix_result(attr_type(attr,type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **attr) { return posix_result(attr_destroy(attr)); }
static ABI int32_t posix_mutex_init(GuestMutex **out,GuestAttr **attr) { return posix_result(mutex_init(out,attr,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) { return posix_result(mutex_lock(m)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock(m)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock(m)); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c, m)); }
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) { return posix_result(cond_abs_timedwait(c, m, t)); }
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }
static ABI int32_t condattr_init(void **attr) {
    if (!attr) return (int32_t)0x80020016; /* EINVAL */
    *attr=calloc(1,16);
    return *attr ? 0 : (int32_t)0x8002000C; /* ENOMEM */
}
static ABI int32_t condattr_destroy(void **attr) {
    if (!attr || !*attr) return (int32_t)0x80020016;
    free(*attr); *attr=NULL; return 0;
}
static ABI int32_t posix_condattr_init(void **attr) { return posix_result(condattr_init(attr)); }
uintptr_t runtime_mutex_resolve(const char *name) {
    if (!strcmp(name,"mKoTx03HRWA#libkernel")) return (uintptr_t)posix_condattr_init;
    if (!strcmp(name,"m5-2bsNfv7s#libkernel")) return (uintptr_t)condattr_init;
    if (!strcmp(name,"waPcxYiR3WA#libkernel")) return (uintptr_t)condattr_destroy;
    if (!strcmp(name,"0TyVk4MSLt0#libkernel")) return (uintptr_t)posix_cond_init;
    if (!strcmp(name,"RXXqi4CtF8w#libkernel")) return (uintptr_t)posix_cond_destroy;
    if (!strcmp(name,"Op8TBGY5KHg#libkernel")) return (uintptr_t)posix_cond_wait;
    if (!strcmp(name,"27bAgiJmOh0#libkernel")) return (uintptr_t)posix_cond_timedwait;
    if (!strcmp(name,"2MOy+rUfuhQ#libkernel")) return (uintptr_t)posix_cond_signal;
    if (!strcmp(name,"mkx2fVhNMsg#libkernel")) return (uintptr_t)posix_cond_broadcast;
    if (!strcmp(name,"2Tb92quprl0#libkernel")) return (uintptr_t)cond_init;
    if (!strcmp(name,"g+PZd2hiacg#libkernel")) return (uintptr_t)cond_destroy;
    if (!strcmp(name,"WKAXJ4XBPQ4#libkernel")) return (uintptr_t)cond_wait;
    if (!strcmp(name,"BmMjYxmew1w#libkernel")) return (uintptr_t)cond_timedwait;
    if (!strcmp(name,"kDh-NfxgMtE#libkernel")) return (uintptr_t)cond_signal;
    if (!strcmp(name,"JGgj7Uvrl+A#libkernel")) return (uintptr_t)cond_broadcast;
    if (!strcmp(name,"IafI2PxcPnQ#libkernel")) return (uintptr_t)mutex_timedlock;
    if (!strcmp(name,"dQHWEsJtoE4#libkernel")) return (uintptr_t)posix_attr_init;
    if (!strcmp(name,"mDmgMOGVUqg#libkernel")) return (uintptr_t)posix_attr_type;
    if (!strcmp(name,"HF7lK46xzjY#libkernel")) return (uintptr_t)posix_attr_destroy;
    if (!strcmp(name,"ttHNfU+qDBU#libkernel")) return (uintptr_t)posix_mutex_init;
    if (!strcmp(name,"ltCfaGr2JGE#libkernel")) return (uintptr_t)posix_mutex_destroy;
    if (!strcmp(name,"7H0iTOciTLo#libkernel")) return (uintptr_t)posix_mutex_lock;
    if (!strcmp(name,"K-jXhbt2gn4#libkernel")) return (uintptr_t)posix_mutex_trylock;
    if (!strcmp(name,"2Z+PpY6CaJg#libkernel")) return (uintptr_t)posix_mutex_unlock;
    if (!strcmp(name, "F8bUHwAG284#libkernel")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#libkernel")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#libkernel")) return (uintptr_t)attr_destroy;
    if (!strcmp(name, "1FGvU0i9saQ#libkernel")) return (uintptr_t)attr_protocol;
    if (!strcmp(name, "cmo1RIYva9o#libkernel")) return (uintptr_t)mutex_init;
    if (!strcmp(name, "9UK1vLZQft4#libkernel")) return (uintptr_t)mutex_lock;
    if (!strcmp(name, "upoVrzMHFeE#libkernel")) return (uintptr_t)mutex_trylock;
    if (!strcmp(name, "tn3VlD0hG60#libkernel")) return (uintptr_t)mutex_unlock;
    if (!strcmp(name, "2Of0f+3mhhE#libkernel")) return (uintptr_t)mutex_destroy;
    return 0;
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", created, locks, unlocks);
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu, first waits ended by an earlier signal=%zu\n",
           conds, waits, wakeups, late_wakeups);
}

