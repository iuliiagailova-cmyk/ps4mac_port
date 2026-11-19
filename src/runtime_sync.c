/* Event flags (sceKernel*EventFlag) and POSIX semaphores (sem_*), first needed by
 * Ratchet & Clank (CUSA01047). Semantics follow the PS4 kernel as shadPS4 documents them
 * (core/libraries/kernel/threads/event_flag.cpp, semaphore.cpp):
 *   - an event flag holds 64 bits; a waiter waits for all (AND) or any (OR) of its bits and
 *     may clear all bits or its own bits when it is released;
 *   - a single-thread flag (attribute 0x10) admits one waiter at a time (EPERM otherwise);
 *   - POSIX semaphores are a pointer to a runtime object in the guest's sem_t; errors set the
 *     guest errno and return -1, sem_timedwait takes an absolute CLOCK_REALTIME deadline.
 * Author: Iuliia Gailova. */
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define E_PERM 1
#define E_SRCH 3
#define E_ACCES 13
#define E_FAULT 14
#define E_INVAL 22
#define E_AGAIN 35
#define E_TIMEDOUT 60
#define E_NAMETOOLONG 63
#define E_OVERFLOW 84
#define E_CANCELED 85

/* Deadlines use CLOCK_REALTIME on macOS and Windows, as runtime_sema.c explains. */
#if defined(_WIN32) || defined(__APPLE__)
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(WAIT_CLOCK,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static struct timespec at_ns(uint64_t ns) {
    return (struct timespec){.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)};
}
static pthread_cond_t make_cond(void) {
    pthread_cond_t c;
    pthread_condattr_t a;
    pthread_condattr_init(&a);
#if !defined(_WIN32) && !defined(__APPLE__)
    pthread_condattr_setclock(&a,CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&c,&a);
    pthread_condattr_destroy(&a);
    return c;
}

/* ---- Event flags ---- */
#define EVF_MAGIC 0x45564631u /* 'EVF1' */
#define EVF_SINGLE 0x10u
#define EVF_MULTI 0x20u
#define EVF_WAIT_AND 0x01u
#define EVF_WAIT_OR 0x02u
#define EVF_CLEAR_ALL 0x10u
#define EVF_CLEAR_PAT 0x20u
typedef struct EventFlag {
    uint32_t magic, attr;
    char name[32];
    uint64_t bits;
    unsigned waiters;
    int deleted;
    pthread_mutex_t lock;
    pthread_cond_t changed;
} EventFlag;
static unsigned event_flags_created;

static int evf_met(uint64_t bits,uint64_t want,uint32_t mode) {
    return (mode&0xf)==EVF_WAIT_AND ? (bits&want)==want : (bits&want)!=0;
}
static void evf_clear(EventFlag *f,uint64_t want,uint32_t mode) {
    if ((mode&0xf0)==EVF_CLEAR_ALL) f->bits=0;
    else if ((mode&0xf0)==EVF_CLEAR_PAT) f->bits&=~want;
}
static ABI int32_t evf_create(EventFlag **out,const char *name,uint32_t attr,uint64_t initial,const void *options) {
    if (!out || !name || options || attr>(EVF_MULTI|0x2)) return ERR(E_INVAL);
    if (strlen(name)>=32) return ERR(E_NAMETOOLONG);
    if ((attr&0xf)>2 || ((attr&0xf0)!=0 && (attr&0xf0)!=EVF_SINGLE && (attr&0xf0)!=EVF_MULTI)) return ERR(E_INVAL);
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    f->magic=EVF_MAGIC; f->attr=attr; f->bits=initial;
    snprintf(f->name,sizeof(f->name),"%s",name);
    pthread_mutex_init(&f->lock,NULL);
    f->changed=make_cond();
    *out=f; ++event_flags_created;
    printf("Runtime: event flag '%s' (attr %#x, initial %#llx)\n",f->name,attr,(unsigned long long)initial);
    return 0;
}
static int evf_valid(const EventFlag *f) { return f && f->magic==EVF_MAGIC; }
static ABI int32_t evf_delete(EventFlag *f) {
    if (!evf_valid(f)) return ERR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    f->deleted=1; f->magic=0;
    pthread_cond_broadcast(&f->changed);
    const int idle=f->waiters==0;
    pthread_mutex_unlock(&f->lock);
    /* A deleted flag with waiters is left allocated: they wake up and return EACCES. */
    if (idle) { pthread_cond_destroy(&f->changed); pthread_mutex_destroy(&f->lock); free(f); }
    return 0;
}
static ABI int32_t evf_set(EventFlag *f,uint64_t pattern) {
    if (!evf_valid(f)) return ERR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    f->bits|=pattern;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static ABI int32_t evf_clear_bits(EventFlag *f,uint64_t pattern) {
    if (!evf_valid(f)) return ERR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    f->bits&=pattern; /* sceKernelClearEventFlag keeps the bits set in the pattern */
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static int32_t evf_wait_common(EventFlag *f,uint64_t want,uint32_t mode,uint64_t *result,uint32_t *timeout,int poll) {
    if (!evf_valid(f)) return ERR(E_SRCH);
    if (!want || ((mode&0xf)!=EVF_WAIT_AND && (mode&0xf)!=EVF_WAIT_OR) ||
        ((mode&0xf0)!=0 && (mode&0xf0)!=EVF_CLEAR_ALL && (mode&0xf0)!=EVF_CLEAR_PAT)) return ERR(E_INVAL);
    pthread_mutex_lock(&f->lock);
    if (evf_met(f->bits,want,mode)) {
        if (result) *result=f->bits;
        evf_clear(f,want,mode);
        pthread_mutex_unlock(&f->lock);
        return 0;
    }
    if (poll) {
        if (result) *result=f->bits;
        pthread_mutex_unlock(&f->lock);
        return ERR(16); /* EBUSY */
    }
    if ((f->attr&0xf0)!=EVF_MULTI && f->waiters) {
        pthread_mutex_unlock(&f->lock);
        return ERR(E_PERM);
    }
    ++f->waiters;
    const uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
    const struct timespec end=at_ns(deadline);
    int32_t status=0;
    while (!f->deleted && !evf_met(f->bits,want,mode)) {
        if (timeout) {
            if (pthread_cond_timedwait(&f->changed,&f->lock,&end)==ETIMEDOUT && !evf_met(f->bits,want,mode)) {
                status=ERR(E_TIMEDOUT);
                break;
            }
        } else {
            pthread_cond_wait(&f->changed,&f->lock);
        }
    }
    --f->waiters;
    if (f->deleted) status=ERR(E_ACCES);
    if (result) *result=f->bits;
    if (!status) evf_clear(f,want,mode);
    if (timeout) {
        const uint64_t now=now_ns();
        *timeout=status ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
    }
    const int free_now=f->deleted && !f->waiters;
    pthread_mutex_unlock(&f->lock);
    if (free_now) { pthread_cond_destroy(&f->changed); pthread_mutex_destroy(&f->lock); free(f); }
    return status;
}
static ABI int32_t evf_wait(EventFlag *f,uint64_t want,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    return evf_wait_common(f,want,mode,result,timeout,0);
}
static ABI int32_t evf_poll(EventFlag *f,uint64_t want,uint32_t mode,uint64_t *result) {
    return evf_wait_common(f,want,mode,result,NULL,1);
}

/* ---- POSIX semaphores ---- */
#define SEM_MAGIC 0x53454D31u /* 'SEM1' */
#define SEM_VALUE_MAX 0x7fffffff
typedef struct PosixSem {
    uint32_t magic;
    int32_t value;
    pthread_mutex_t lock;
    pthread_cond_t posted;
} PosixSem;
static unsigned semaphores_created;
static int sem_fail(int guest_errno) { *runtime_errno()=guest_errno; return -1; }
static PosixSem *sem_get(PosixSem **sem) {
    return sem && *sem && (*sem)->magic==SEM_MAGIC ? *sem : NULL;
}
static ABI int32_t psem_init(PosixSem **sem,int32_t shared,uint32_t value) {
    (void)shared;
    if (!sem || value>SEM_VALUE_MAX) return sem_fail(E_INVAL);
    PosixSem *s=calloc(1,sizeof(*s));
    if (!s) return sem_fail(12);
    s->magic=SEM_MAGIC; s->value=(int32_t)value;
    pthread_mutex_init(&s->lock,NULL);
    s->posted=make_cond();
    *sem=s; ++semaphores_created;
    return 0;
}
static ABI int32_t psem_destroy(PosixSem **sem) {
    PosixSem *s=sem_get(sem);
    if (!s) return sem_fail(E_INVAL);
    s->magic=0;
    pthread_cond_destroy(&s->posted); pthread_mutex_destroy(&s->lock);
    free(s); *sem=NULL;
    return 0;
}
static ABI int32_t psem_post(PosixSem **sem) {
    PosixSem *s=sem_get(sem);
    if (!s) return sem_fail(E_INVAL);
    pthread_mutex_lock(&s->lock);
    if (s->value==SEM_VALUE_MAX) { pthread_mutex_unlock(&s->lock); return sem_fail(E_OVERFLOW); }
    ++s->value;
    pthread_cond_signal(&s->posted);
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static ABI int32_t psem_trywait(PosixSem **sem) {
    PosixSem *s=sem_get(sem);
    if (!s) return sem_fail(E_INVAL);
    pthread_mutex_lock(&s->lock);
    const int ok=s->value>0;
    if (ok) --s->value;
    pthread_mutex_unlock(&s->lock);
    return ok ? 0 : sem_fail(E_AGAIN);
}
/* Guest timespec: 64-bit seconds and nanoseconds (FreeBSD amd64). */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static int32_t psem_wait_until(PosixSem **sem,const GuestTimespec *abstime) {
    PosixSem *s=sem_get(sem);
    if (!s) return sem_fail(E_INVAL);
    if (abstime && (abstime->nsec<0 || abstime->nsec>=1000000000)) return sem_fail(E_INVAL);
    uint64_t deadline=0;
    if (abstime) {
        /* The guest deadline is CLOCK_REALTIME; convert it to the wait clock. */
        struct timespec real;
        clock_gettime(CLOCK_REALTIME,&real);
        const int64_t left=(abstime->sec-(int64_t)real.tv_sec)*1000000000+(abstime->nsec-real.tv_nsec);
        deadline=now_ns()+(uint64_t)(left>0 ? left : 0);
    }
    const struct timespec end=at_ns(deadline);
    pthread_mutex_lock(&s->lock);
    while (s->value<=0) {
        if (abstime) {
            if (pthread_cond_timedwait(&s->posted,&s->lock,&end)==ETIMEDOUT && s->value<=0) {
                pthread_mutex_unlock(&s->lock);
                return sem_fail(E_TIMEDOUT);
            }
        } else {
            pthread_cond_wait(&s->posted,&s->lock);
        }
    }
    --s->value;
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static ABI int32_t psem_wait(PosixSem **sem) { return psem_wait_until(sem,NULL); }
static ABI int32_t psem_timedwait(PosixSem **sem,const GuestTimespec *abstime) {
    if (!abstime) return sem_fail(E_FAULT);
    return psem_wait_until(sem,abstime);
}
static ABI int32_t psem_getvalue(PosixSem **sem,int32_t *value) {
    PosixSem *s=sem_get(sem);
    if (!s || !value) return sem_fail(E_INVAL);
    pthread_mutex_lock(&s->lock);
    *value=s->value;
    pthread_mutex_unlock(&s->lock);
    return 0;
}

void runtime_sync_report(void) {
    if (event_flags_created || semaphores_created)
        printf("Runtime: %u event flags, %u POSIX semaphores created\n",event_flags_created,semaphores_created);
}

static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",evf_create}, {"sceKernelDeleteEventFlag",evf_delete},
    {"sceKernelSetEventFlag",evf_set}, {"sceKernelClearEventFlag",evf_clear_bits},
    {"sceKernelWaitEventFlag",evf_wait}, {"sceKernelPollEventFlag",evf_poll},
    {"sem_init",psem_init}, {"sem_destroy",psem_destroy}, {"sem_post",psem_post},
    {"sem_wait",psem_wait}, {"sem_trywait",psem_trywait}, {"sem_timedwait",psem_timedwait},
    {"sem_getvalue",psem_getvalue},
};
uintptr_t runtime_sync_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
