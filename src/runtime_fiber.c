/* libSceFiber: cooperative fibers. A fiber runs on a stack of its own, and switching saves the
 * callee-saved registers, MXCSR and the x87 control word on the stack it leaves, then loads them
 * from the stack it enters (fiber_swap below).
 *
 * The stacks are host memory, not the context the game passes in: a fault on GPU-watched guest
 * memory is delivered on the faulting stack, and a write-protected stack cannot take it. The
 * game's context size is kept, with room added for the runtime's own calls.
 *
 * A fiber may be resumed on another thread than the one it left, so every read of per-thread
 * state after a switch goes through thread_state(), which the compiler cannot cache. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ERR_NULL ((int32_t)0x80590001)
#define ERR_ALIGNMENT ((int32_t)0x80590002)
#define ERR_RANGE ((int32_t)0x80590003)
#define ERR_INVALID ((int32_t)0x80590004)
#define ERR_PERMISSION ((int32_t)0x80590005)
#define ERR_STATE ((int32_t)0x80590006)

#define MAGIC_START 0xdef1649cu
#define MAGIC_END 0xb37592a0u
#define OPT_MAGIC 0xbb40e64du
#define CONTEXT_MINIMUM 512u
#define NAME_LENGTH 31
#define HOST_MARGIN (256u<<10)        /* runtime calls made from a fiber run on its stack too */
#define NO_CONTEXT_STACK (1u<<20)     /* a fiber without a context runs on the thread's stack on a PS4 */

enum { STATE_RUN=1, STATE_IDLE=2, STATE_TERMINATED=3 };

typedef ABI void (*FiberEntry)(uint64_t arg_on_initialize, uint64_t arg_on_run);

/* The game reserves 256 bytes (SceFiber) and only passes it back: the layout is ours. */
typedef struct Fiber {
    uint32_t magic_start;
    _Atomic uint32_t state;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[NAME_LENGTH+1];
    uint64_t rsp;                     /* saved stack pointer while suspended, 0 before the first run */
    uint8_t *stack;
    uint64_t stack_size;
    uint32_t flags, mxcsr;
    uint32_t magic_end;
} Fiber;
_Static_assert(sizeof(Fiber)<=256,"SceFiber is 256 bytes");

typedef struct {
    uint64_t size;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[NAME_LENGTH+1];
    uint64_t size_context_margin;
    uint8_t pad[48];
} FiberInfo;
_Static_assert(sizeof(FiberInfo)==128,"SceFiberInfo is 128 bytes");

typedef struct {
    uint64_t thread_rsp;              /* the thread's own context while one of its fibers runs */
    Fiber *current;
    Fiber *previous;                  /* the fiber just left: idle once its registers are saved */
    uint64_t transfer;                /* the argument handed over by the last switch */
} FiberThread;
static _Thread_local FiberThread fiber_thread;
static __attribute__((noinline)) FiberThread *thread_state(void) {
    FiberThread *t=&fiber_thread;
    __asm__ volatile("" : "+r"(t));
    return t;
}

/* fiber_swap(save, load): saves the current context to *save and continues the one at load. */
void fiber_swap(uint64_t *save, uint64_t load) __asm__("_ps4mac_fiber_swap");
__asm__(
    ".text\n.p2align 4\n_ps4mac_fiber_swap:\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  subq $16, %rsp\n  stmxcsr (%rsp)\n  fnstcw 4(%rsp)\n"
    "  movq %rsp, (%rdi)\n  movq %rsi, %rsp\n"
    "  ldmxcsr (%rsp)\n  fldcw 4(%rsp)\n  addq $16, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
    "  ret\n");

/* After a switch lands: the fiber left behind becomes idle, and the argument is returned. */
static uint64_t arrive(void) {
    FiberThread *t=thread_state();
    if (t->previous) { atomic_store(&t->previous->state,STATE_IDLE); t->previous=NULL; }
    return t->transfer;
}

static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return,uint64_t *arg_on_run);
static void fiber_start(void) {
    const uint64_t arg=arrive();
    Fiber *fiber=thread_state()->current;
    fiber->entry(fiber->arg_on_initialize,arg);
    /* A PS4 fiber must leave through sceFiberReturnToThread or a switch; returning is a game bug. */
    fprintf(stderr,"Fiber: '%s' returned from its entry function\n",fiber->name);
    for (;;) fiber_return_to_thread(0,NULL);
}

/* The stack pointer to load for this fiber: its saved one, or a fresh frame that enters fiber_start. */
static uint64_t resume_point(Fiber *fiber) {
    if (fiber->rsp) return fiber->rsp;
    uint64_t *top=(uint64_t *)(((uintptr_t)(fiber->stack+fiber->stack_size))&~(uintptr_t)15);
    top[-1]=0;                                   /* fiber_start's (never used) return address */
    top[-2]=(uint64_t)(uintptr_t)fiber_start;    /* fiber_swap's ret */
    for (int i=3;i<=8;++i) top[-i]=0;            /* rbp rbx r12 r13 r14 r15 */
    uint32_t *fpu=(uint32_t *)(top-10);
    fpu[0]=fiber->mxcsr; fpu[1]=0x037f; top[-9]=0;
    return (uint64_t)(uintptr_t)(top-10);
}

static int valid(const Fiber *fiber) {
    return fiber->magic_start==MAGIC_START && fiber->magic_end==MAGIC_END;
}

static ABI int32_t fiber_initialize(Fiber *fiber,const char *name,FiberEntry entry,uint64_t arg_on_initialize,
                                    void *addr_context,uint64_t size_context,const uint32_t *opt_param,
                                    uint32_t build_version) {
    if (!fiber || !name || !entry) return ERR_NULL;
    if ((uintptr_t)fiber&7 || (uintptr_t)addr_context&15 || (uintptr_t)opt_param&7) return ERR_ALIGNMENT;
    if (size_context && size_context<CONTEXT_MINIMUM) return ERR_RANGE;
    if (size_context&15 || !addr_context!=!size_context) return ERR_INVALID;
    if (opt_param && *opt_param!=OPT_MAGIC) return ERR_INVALID;
    const long page=sysconf(_SC_PAGESIZE);
    uint64_t size=size_context ? size_context+HOST_MARGIN : NO_CONTEXT_STACK;
    size=(size+(uint64_t)page-1)&~(uint64_t)(page-1);
    /* One page below the stack stays unmapped, so an overflow faults instead of writing past it. */
    uint8_t *guard=mmap(NULL,size+(uint64_t)page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if (guard==MAP_FAILED) return ERR_RANGE;
    mprotect(guard,(size_t)page,PROT_NONE);
    memset(fiber,0,sizeof(*fiber));
    strncpy(fiber->name,name,NAME_LENGTH);
    fiber->entry=entry; fiber->arg_on_initialize=arg_on_initialize;
    fiber->addr_context=addr_context; fiber->size_context=size_context;
    fiber->stack=guard+page; fiber->stack_size=size;
    /* SDK 3.50 and later start fibers with flush-to-zero and denormals-are-zero; older ones inherit. */
    fiber->mxcsr=build_version>=0x03500000u ? 0x9fc0u : __builtin_ia32_stmxcsr();
    fiber->magic_start=MAGIC_START; fiber->magic_end=MAGIC_END;
    atomic_store(&fiber->state,STATE_IDLE);
    return 0;
}

static ABI int32_t fiber_opt_param_initialize(uint32_t *opt_param) {
    if (!opt_param) return ERR_NULL;
    if ((uintptr_t)opt_param&7) return ERR_ALIGNMENT;
    *opt_param=OPT_MAGIC; return 0;
}

static ABI int32_t fiber_finalize(Fiber *fiber) {
    if (!fiber) return ERR_NULL;
    if ((uintptr_t)fiber&7) return ERR_ALIGNMENT;
    if (!valid(fiber)) return ERR_INVALID;
    uint32_t idle=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&fiber->state,&idle,STATE_TERMINATED)) return ERR_STATE;
    if (fiber->stack) {
        const long page=sysconf(_SC_PAGESIZE);
        munmap(fiber->stack-page,fiber->stack_size+(uint64_t)page);
        fiber->stack=NULL;
    }
    return 0;
}

static ABI int32_t fiber_run(Fiber *fiber,uint64_t arg_on_run_to,uint64_t *arg_on_return) {
    if (!fiber) return ERR_NULL;
    if ((uintptr_t)fiber&7) return ERR_ALIGNMENT;
    if (!valid(fiber)) return ERR_INVALID;
    FiberThread *t=thread_state();
    if (t->current) return ERR_PERMISSION;
    uint32_t idle=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&fiber->state,&idle,STATE_RUN)) return ERR_STATE;
    t->current=fiber; t->previous=NULL; t->transfer=arg_on_run_to;
    fiber_swap(&t->thread_rsp,resume_point(fiber));
    const uint64_t value=arrive();
    thread_state()->current=NULL;
    if (arg_on_return) *arg_on_return=value;
    return 0;
}

static ABI int32_t fiber_switch(Fiber *fiber,uint64_t arg_on_run_to,uint64_t *arg_on_run) {
    if (!fiber) return ERR_NULL;
    if ((uintptr_t)fiber&7) return ERR_ALIGNMENT;
    if (!valid(fiber)) return ERR_INVALID;
    FiberThread *t=thread_state();
    if (!t->current) return ERR_PERMISSION;
    uint32_t idle=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&fiber->state,&idle,STATE_RUN)) return ERR_STATE;
    Fiber *self=t->current;
    t->previous=self; t->current=fiber; t->transfer=arg_on_run_to;
    fiber_swap(&self->rsp,resume_point(fiber));
    const uint64_t value=arrive();
    if (arg_on_run) *arg_on_run=value;
    return 0;
}

static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return,uint64_t *arg_on_run) {
    FiberThread *t=thread_state();
    Fiber *self=t->current;
    if (!self) return ERR_PERMISSION;
    t->previous=self; t->transfer=arg_on_return;
    fiber_swap(&self->rsp,t->thread_rsp);
    const uint64_t value=arrive();
    if (arg_on_run) *arg_on_run=value;
    return 0;
}

static ABI int32_t fiber_get_self(Fiber **fiber) {
    if (!fiber) return ERR_NULL;
    Fiber *self=thread_state()->current;
    if (!self) return ERR_PERMISSION;
    *fiber=self; return 0;
}

static ABI int32_t fiber_get_info(Fiber *fiber,FiberInfo *info) {
    if (!fiber || !info) return ERR_NULL;
    if ((uintptr_t)fiber&7 || (uintptr_t)info&7) return ERR_ALIGNMENT;
    if (info->size!=sizeof(*info) || !valid(fiber)) return ERR_INVALID;
    info->entry=fiber->entry; info->arg_on_initialize=fiber->arg_on_initialize;
    info->addr_context=fiber->addr_context; info->size_context=fiber->size_context;
    memcpy(info->name,fiber->name,sizeof(info->name));
    info->size_context_margin=UINT64_MAX;          /* no context size check */
    return 0;
}

static ABI int32_t fiber_rename(Fiber *fiber,const char *name) {
    if (!fiber || !name) return ERR_NULL;
    if ((uintptr_t)fiber&7) return ERR_ALIGNMENT;
    if (!valid(fiber)) return ERR_INVALID;
    strncpy(fiber->name,name,NAME_LENGTH); fiber->name[NAME_LENGTH]=0;
    return 0;
}

static const RuntimeExport exports[]={
    {"_sceFiberInitializeImpl",fiber_initialize}, {"sceFiberOptParamInitialize",fiber_opt_param_initialize},
    {"sceFiberFinalize",fiber_finalize}, {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch},
    {"sceFiberReturnToThread",fiber_return_to_thread}, {"sceFiberGetSelf",fiber_get_self},
    {"sceFiberGetInfo",fiber_get_info}, {"sceFiberRename",fiber_rename},
};
uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
