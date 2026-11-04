/*
 * ps3recomp - cellFiber HLE implementation
 *
 * The SDK's scheduler-based PPU fibers (see cellFiber.h). RunFibers runs on the calling PPU thread and switches to one
 * fiber at a time; a fiber runs the guest entry through the PPU thread trampoline on its own host stack with its own
 * ppu_context, whose r1 points into the guest stack the caller passed to CreateFiber. A fiber hands control back to
 * RunFibers when it blocks (WaitSignal, JoinFiber), yields or ends.
 *
 * Not modelled: the scheduler attribute and fiber attribute contents (both are zeroed on initialisation and ignored),
 * and the fiber context-switch trace/ufiber utilities of the SDK.
 */

/* Darwin gates the ucontext routines AND the shape of ucontext_t itself on _XOPEN_SOURCE, so this has to come before
 * the first system header any include below reaches -- see the static assertion further down for what defining it
 * late costs. Nothing above this line. */
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 600
#  define _DARWIN_C_SOURCE 1   /* and back: _XOPEN_SOURCE alone hides MAP_ANONYMOUS, which vm.h uses */
#endif

#include "cellFiber.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_write32, vm_memset: guest EA -> host */
#include "../../runtime/syscalls/sys_ppu_thread.h"   /* ppu_context, g_ppu_thread_entry_trampoline */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__EMSCRIPTEN__)
/* Emscripten ships ucontext.h's declarations with no implementation: there is no getcontext, makecontext,
 * setcontext or swapcontext anywhere in its musl, so the POSIX branch below is linkable-looking and then
 * fails in wasm-ld. The engines have no way to swap a saved machine context either -- a wasm function's
 * stack is not addressable that way. What Emscripten does have is emscripten/fiber.h, built on Asyncify:
 * a "swap" unwinds the current stack into a per-fiber buffer and replays it on the way back. That is a
 * third mechanism, not a variation on the two below, so it gets its own branch rather than being folded
 * into the POSIX one. Note that it requires the link to run the Asyncify pass (-sASYNCIFY); without it
 * emscripten_fiber_swap links and then aborts at the first switch.
 *
 * <pthread.h> is included for the lock and condition variable the two host branches share. */
#include <pthread.h>
#include <emscripten/fiber.h>
#else
#include <pthread.h>
#include <ucontext.h>

#if defined(__APPLE__)
/* getcontext() points uc_mcontext at ucontext_t's own __mcontext_data member and writes uc_mcsize (816) bytes there.
 * That member is declared only when _XOPEN_SOURCE was defined before <sys/_types/_ucontext.h> was first pulled in;
 * without it the struct is the 64-byte header alone and every getcontext, swapcontext and makecontext writes 752
 * bytes off the end of it. The define above is the fix; this is the tripwire, because moving an #include up here
 * would silently undo it. */
_Static_assert(sizeof(ucontext_t) > 64,
               "ucontext_t has no embedded machine state: _XOPEN_SOURCE was "
               "defined too late and getcontext will write past the struct");
#endif
#endif

extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;

/* Host stack of each fiber. The guest stack is separate: it holds the guest's own frames, this one the host frames of
 * the lifted code and of the trampoline. */
#define FIBER_HOST_STACK   (4u * 1024u * 1024u)
#ifdef __EMSCRIPTEN__
/* Asyncify buffer per context (a fiber's, and RunFibers' for the scheduler). Only Emscripten has one: on the other
 * two branches a suspended fiber's frames stay on its own host stack, while here every swap copies them into this
 * buffer and replays them on the way back. Sized like the host stack -- a chain deep enough to overflow the buffer
 * would have overflowed the stack it mirrors. Measured at -O2 on an instrumented 1000-deep chain: ~44 bytes per
 * frame, so 4 MiB is ~95,000 frames. */
#define FIBER_ASYNCIFY_STACK (4u * 1024u * 1024u)
#endif
/* Smallest guest stack CreateFiber accepts. */
#define FIBER_MIN_STACK    0x400u
/* Guest stack bytes kept free above the initial r1 (the ABI's back chain and register save area). */
#define FIBER_STACK_TOP_PAD 0x100u
/* r3 for the fiber's thread id when it is created outside a PPU thread. */
#define FIBER_FALLBACK_THREAD_ID 0x7FFF0000ull

/* ---------------------------------------------------------------------------
 * State
 *
 * One lock covers all of it, and one condition variable wakes everyone who waits on it: a RunFibers loop with nothing
 * to run (its fibers wait for a signal another thread sends), and JoinFiber callers outside any fiber. Guest
 * addresses name fibers and schedulers, never host pointers, so a waiter that wakes after FinalizeScheduler finds the
 * object gone instead of freed memory.
 * -----------------------------------------------------------------------*/

enum { F_READY, F_RUNNING, F_WAIT_SIGNAL, F_WAIT_JOIN, F_DONE };

typedef struct Sched Sched;
typedef struct Fiber Fiber;

struct Fiber {
    Fiber*  next;                /* every fiber of every scheduler */
    Sched*  sched;
    u32     ea;                  /* the guest CellFiberPpu */
    u32     opd;
    u32     priority;
    u32     stack_ea;
    u32     stack_size;
    u64     arg;
    u64     seq;                 /* position in the run queue: creation order, then the last yield */
    int     state;
    int     signal_pending;      /* a SendSignal arrived before the WaitSignal */
    u32     join_ea;             /* F_WAIT_JOIN: the fiber waited for */
    s32     status;
#ifdef _WIN32
    void*   host_fiber;
#elif defined(__EMSCRIPTEN__)
    emscripten_fiber_t fiber;    /* its own context: c stack, entry and asyncify stack */
    void*   host_stack;
    void*   asyncify_stack;      /* where a swap stores the frames it unwinds off this stack */
#else
    ucontext_t uc;
    void*   host_stack;
#endif
    ppu_context ctx;
};

struct Sched {
    Sched*  next;
    u32     ea;                  /* the guest CellFiberPpuScheduler */
    int     running;             /* a thread is inside RunFibers */
#ifdef _WIN32
    void*   back;                /* RunFibers' fiber while it runs one */
#elif defined(__EMSCRIPTEN__)
    emscripten_fiber_t back;     /* RunFibers' context while it runs one */
    void*   back_asyncify;       /* and its asyncify stack */
#else
    ucontext_t back;             /* RunFibers' context while it runs one */
#endif
};

static Sched* s_scheds;
static Fiber* s_fibers;
static u64    s_seq;

/* The fiber running on this host thread, if any. */
static PPU_THREAD_LOCAL Fiber* t_cur;

#ifdef _WIN32
static SRWLOCK            s_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_cv   = CONDITION_VARIABLE_INIT;
#  define LOCK()    AcquireSRWLockExclusive(&s_lock)
#  define UNLOCK()  ReleaseSRWLockExclusive(&s_lock)
#  define WAIT()    SleepConditionVariableSRW(&s_cv, &s_lock, INFINITE, 0)
#  define WAKE()    WakeAllConditionVariable(&s_cv)
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv   = PTHREAD_COND_INITIALIZER;
#  define LOCK()    pthread_mutex_lock(&s_lock)
#  define UNLOCK()  pthread_mutex_unlock(&s_lock)
#  define WAIT()    pthread_cond_wait(&s_cv, &s_lock)
#  define WAKE()    pthread_cond_broadcast(&s_cv)
#endif

/* Lookups; the lock is held. */
static Sched* find_sched(u32 ea)
{
    for (Sched* s = s_scheds; s; s = s->next)
        if (s->ea == ea) return s;
    return NULL;
}

static Fiber* find_fiber(u32 ea)
{
    for (Fiber* f = s_fibers; f; f = f->next)
        if (f->ea == ea) return f;
    return NULL;
}

/* The READY fiber of the scheduler with the lowest priority number; creation (or last yield) order among equals. */
static Fiber* pick_runnable(const Sched* s)
{
    Fiber* best = NULL;
    for (Fiber* f = s_fibers; f; f = f->next) {
        if (f->sched != s || f->state != F_READY) continue;
        if (!best || f->priority < best->priority ||
            (f->priority == best->priority && f->seq < best->seq))
            best = f;
    }
    return best;
}

/* Every fiber of the scheduler has ended (or it has none). */
static int all_done(const Sched* s)
{
    for (const Fiber* f = s_fibers; f; f = f->next)
        if (f->sched == s && f->state != F_DONE) return 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Host context switching: ucontext on POSIX, fibers on Windows. RunFibers' own context is the "back" one every fiber
 * returns to when it blocks, yields or ends.
 * -----------------------------------------------------------------------*/

static void fiber_body(Fiber* f);

#ifdef _WIN32
static VOID CALLBACK fiber_entry(void* p)
{
    fiber_body((Fiber*)p);
}

static int host_create(Fiber* f)
{
    f->host_fiber = CreateFiberEx(0, FIBER_HOST_STACK, 0, fiber_entry, f);
    return f->host_fiber != NULL;
}

/* Releases the host stack. The fiber must not be the one running, nor be resumed again. */
static void host_destroy(Fiber* f)
{
    if (f->host_fiber) DeleteFiber(f->host_fiber);
    f->host_fiber = NULL;
}

/* RunFibers -> fiber. The calling thread becomes a fiber itself on first use. */
static void host_resume(Sched* s, Fiber* f)
{
    if (!IsThreadAFiber()) ConvertThreadToFiber(NULL);
    s->back = GetCurrentFiber();
    SwitchToFiber(f->host_fiber);
}

static void host_yield(Fiber* f)
{
    SwitchToFiber(f->sched->back);
}

/* No per-scheduler host state on this branch. */
static int host_sched_create(Sched* s) { (void)s; return 1; }
static void host_sched_destroy(Sched* s) { (void)s; }
#elif defined(__EMSCRIPTEN__)
/* Probes the *same* contract the ucontext branch gets from getcontext/makecontext: a fresh context with its own
 * stack, entered once at fiber_entry, that can be swapped to and from arbitrarily often. What each swap means
 * differs, and the difference is not cosmetic:
 *
 *   - swapcontext saves and restores the machine context (registers and stack pointer) in place: the suspended
 *     frames stay on the fiber's own stack, untouched, until someone swaps back. There is no machine context to
 *     save in wasm, so emscripten_fiber_swap instead *unwinds* the outgoing stack. Asyncify has instrumented every
 *     function between the swap and the wasm export above it; on the way out each one spills its live locals into
 *     the outgoing context's asyncify_data buffer and returns. Switching back replays that buffer, which restores
 *     the frames and continues after the swap call. The observable contract is the same -- "swapcontext returns
 *     when someone swaps back to you" -- and no code runs twice (measured: a side effect on every level of a
 *     five-deep chain fires exactly once across a suspend and a resume). Where the frames live between those two
 *     points does differ, and it is what a later change can trip over: while the fiber is suspended its frames are
 *     in the buffer and its stack below the swap point is dead; on resume the frames are rebuilt at the same
 *     addresses (measured: same frame address and an intact 4 KiB local after a resume). So the stack is the same
 *     stack, but only once the fiber is running. And every function between the wasm export and the swap has to
 *     have been instrumented for any of this to work, which is why neither -sASYNCIFY_ONLY nor -sASYNCIFY_REMOVE
 *     is set here: they would have to name this whole path, and getting it partly wrong fails at the swap, in the
 *     guest, a long way from the build.
 *
 *   - That buffer is the asyncify stack, a separate allocation per context, and it is why each context needs two
 *     allocations here where the POSIX path needs one. It has no ucontext counterpart at all.
 *
 *   - makecontext sets a function pointer plus arguments to be called *once*, on first entry. emscripten_fiber_init
 *     does the same (entry + user_data), and the runtime NULLs entry after the first call, so later swaps rewind
 *     instead of re-entering. fiber_entry's signature is void(*)(void*) rather than makecontext's variadic
 *     argument list: the Fiber* travels in user_data. (The two-unsigned split the ucontext branch uses is a
 *     makecontext artifact -- it cannot pass a pointer portably -- and has no counterpart here.)
 *
 *   - Unlike setcontext, the swap in fiber_leave returns. See the note there.
 *
 * Requires the link to run the Asyncify pass (-sASYNCIFY). Without it emscripten_fiber_swap is the stub from
 * libasync.js that abort()s with "please compile your program with async support", so the link still succeeds and
 * the first switch is what fails -- see cmake/toolchains/emscripten-wasm64.cmake.
 *
 * One known upstream wrinkle, and it is not this file's to fix: any switch cycle leaves Emscripten's
 * runtimeKeepaliveCounter one higher than it found it (measured: 0 before, 1 after, with libasync.js alone and no
 * cellFiber code involved). A pthread that runs fibers and then *returns* from its entry function therefore never
 * reaches _emscripten_thread_exit, because invokeEntryPoint's finish() sees keepRuntimeAlive() and bails, and a
 * pthread_join on it hangs. Returning through pthread_exit() instead is unaffected. Nothing here can balance the
 * counter without reaching into Emscripten internals; it is recorded so the next person to see a join hang after
 * SPURS work has the cause, rather than a stack trace that points at the scheduler. */
static void fiber_entry(void* p)
{
    fiber_body((Fiber*)p);
}

static int host_create(Fiber* f)
{
    /* Two allocations, both of them the same size as the host stack: the asyncify buffer has to be able to hold
     * everything that would have been on the C stack when the fiber suspends, so making it smaller is a bet that
     * the fiber never gets deep. (Measured on a 1000-deep instrumented chain at -O2: 44 bytes of buffer per frame,
     * so at this size the bet would only be lost by a chain four orders of magnitude past anything the guest stack
     * can hold.) */
    f->host_stack     = malloc(FIBER_HOST_STACK);
    f->asyncify_stack = malloc(FIBER_ASYNCIFY_STACK);
    if (!f->host_stack || !f->asyncify_stack) {
        free(f->host_stack);
        free(f->asyncify_stack);
        f->host_stack = f->asyncify_stack = NULL;
        return 0;
    }
    emscripten_fiber_init(&f->fiber, fiber_entry, f,
                          f->host_stack, FIBER_HOST_STACK,
                          f->asyncify_stack, FIBER_ASYNCIFY_STACK);
    return 1;
}

/* Releases the host stack and the asyncify buffer. The fiber must not be the one running, nor be resumed again. */
static void host_destroy(Fiber* f)
{
    free(f->host_stack);
    free(f->asyncify_stack);
    f->host_stack = NULL;
    f->asyncify_stack = NULL;
}

/* RunFibers -> fiber.
 *
 * The back context is re-captured on every resume rather than once when the scheduler is created, because what it
 * records is the *current thread's* stack: emscripten_fiber_init_from_current_context() reads the stack base and
 * limit that Emscripten's stack-limit bookkeeping holds for the calling thread, and FinishContextSwitch installs
 * those limits again on every switch into the back context. A scheduler is only prevented from running on two
 * threads at once, not from being created on one thread and driven by another, so capturing at creation time would
 * hand RunFibers a stack limit belonging to a different thread. Measured with -sSTACK_OVERFLOW_CHECK=2: capturing
 * the back context on the creating thread and swapping from another aborts with "Attempt to set SP to 0x... with
 * stack limits [0x... - 0x...]" -- the worker's SP is outside the creator's recorded range. Capturing on the
 * swapping thread is clean.
 *
 * Nothing is lost by doing it here: the swap writes the outgoing stack pointer into the same struct a moment later,
 * so the capture is a handful of stores. It does not need to reset the asyncify cursor either -- a once-captured
 * back context still rewinds correctly, because each unwind starts again at the front of its buffer (measured:
 * 20,000 resumes of a once-captured back context through a 4 KiB buffer at varying stack depths, no overflow). */
static void host_resume(Sched* s, Fiber* f)
{
    emscripten_fiber_init_from_current_context(&s->back, s->back_asyncify, FIBER_ASYNCIFY_STACK);
    emscripten_fiber_swap(&s->back, &f->fiber);
}

static void host_yield(Fiber* f)
{
    emscripten_fiber_swap(&f->fiber, &f->sched->back);
}

/* The back context's asyncify buffer belongs to the scheduler (one RunFibers at a time per scheduler), so it is
 * allocated with it: host_create()'s counterpart on the scheduler side, with the same failure contract. */
static int host_sched_create(Sched* s)
{
    s->back_asyncify = malloc(FIBER_ASYNCIFY_STACK);
    return s->back_asyncify != NULL;
}

static void host_sched_destroy(Sched* s)
{
    free(s->back_asyncify);
    s->back_asyncify = NULL;
}
#else
static void fiber_entry(unsigned hi, unsigned lo)
{
    fiber_body((Fiber*)(((uintptr_t)hi << 32) | lo));
}

static int host_create(Fiber* f)
{
    f->host_stack = malloc(FIBER_HOST_STACK);
    if (!f->host_stack) return 0;
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp   = f->host_stack;
    f->uc.uc_stack.ss_size = FIBER_HOST_STACK;
    f->uc.uc_link          = NULL;
    uintptr_t p = (uintptr_t)f;
    makecontext(&f->uc, (void (*)(void))fiber_entry, 2, (unsigned)(p >> 32), (unsigned)(p & 0xFFFFFFFFu));
    return 1;
}

/* Releases the host stack. The fiber must not be the one running, nor be resumed again. */
static void host_destroy(Fiber* f)
{
    free(f->host_stack);
    f->host_stack = NULL;
}

static void host_resume(Sched* s, Fiber* f)
{
    swapcontext(&s->back, &f->uc);
}

static void host_yield(Fiber* f)
{
    swapcontext(&f->uc, &f->sched->back);
}

/* No per-scheduler host state on this branch. */
static int host_sched_create(Sched* s) { (void)s; return 1; }
static void host_sched_destroy(Sched* s) { (void)s; }
#endif

/* The fiber has ended with `status`: record it and make the fibers that joined it runnable. The lock is held. */
static void fiber_finish_locked(Fiber* f, s32 status)
{
    f->status = status;
    f->state  = F_DONE;
    for (Fiber* g = s_fibers; g; g = g->next)
        if (g->state == F_WAIT_JOIN && g->join_ea == f->ea) g->state = F_READY;
}

/* Leaves the fiber for good, back into RunFibers. */
static void fiber_leave(Fiber* f)
{
#ifdef _WIN32
    SwitchToFiber(f->sched->back);
#elif defined(__EMSCRIPTEN__)
    /* A swap, not a setcontext: it returns if this context is ever resumed again, which is exactly the case the
     * abort() below is here for. Nothing resumes an ended fiber -- RunFibers frees it as soon as the state is
     * F_DONE and host_destroy() has released these buffers -- so the return path must not be reachable, and if the
     * accounting is ever wrong this says so instead of jumping into freed memory. */
    emscripten_fiber_swap(&f->fiber, &f->sched->back);
#else
    setcontext(&f->sched->back);
#endif
    abort();                     /* an ended fiber is never resumed */
}

/* The fiber's first and last frame, on its own host stack. */
static void fiber_body(Fiber* f)
{
    f->ctx.gpr[1] = (f->stack_ea + f->stack_size - FIBER_STACK_TOP_PAD) & ~0xFull;
    f->ctx.gpr[3] = f->arg;
    f->ctx.cia    = f->opd;
    if (g_ppu_thread_entry_trampoline)
        g_ppu_thread_entry_trampoline(&f->ctx);   /* runs the entry to completion; its r3 is the status */

    LOCK();
    fiber_finish_locked(f, (s32)f->ctx.gpr[3]);
    UNLOCK();
    WAKE();
    fiber_leave(f);
}

/* ---------------------------------------------------------------------------
 * Initialisation
 * -----------------------------------------------------------------------*/

s32 _cellFiberPpuInitialize(void)
{
    return CELL_OK;
}

s32 _cellFiberPpuSchedulerAttributeInitialize(CellFiberPpuSchedulerAttribute* attr, u32 sdkVersion)
{
    (void)sdkVersion;
    if (!attr) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    vm_memset((u32)(uintptr_t)attr, 0, CELL_FIBER_PPU_SCHEDULER_ATTRIBUTE_SIZE);
    return CELL_OK;
}

s32 _cellFiberPpuAttributeInitialize(CellFiberPpuAttribute* attr, u32 sdkVersion)
{
    (void)sdkVersion;
    if (!attr) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    vm_memset((u32)(uintptr_t)attr, 0, CELL_FIBER_PPU_ATTRIBUTE_SIZE);
    return CELL_OK;
}

s32 cellFiberPpuInitializeScheduler(CellFiberPpuScheduler* scheduler, const CellFiberPpuSchedulerAttribute* attr)
{
    (void)attr;
    const u32 ea = (u32)(uintptr_t)scheduler;
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    if (ea % CELL_FIBER_PPU_SCHEDULER_ALIGN) return (s32)CELL_FIBER_ERROR_ALIGN;

    LOCK();
    s32 rc = CELL_OK;
    if (find_sched(ea)) {
        rc = (s32)CELL_FIBER_ERROR_STAT;
    } else {
        Sched* s = (Sched*)calloc(1, sizeof *s);
        if (!s) {
            rc = (s32)CELL_FIBER_ERROR_NOMEM;
        } else if (!host_sched_create(s)) {
            /* Emscripten's back-context asyncify buffer; a no-op everywhere else. */
            rc = (s32)CELL_FIBER_ERROR_NOMEM;
            free(s);
        } else {
            s->ea   = ea;
            s->next = s_scheds;
            s_scheds = s;
        }
    }
    UNLOCK();
    return rc;
}

s32 cellFiberPpuFinalizeScheduler(CellFiberPpuScheduler* scheduler)
{
    const u32 ea = (u32)(uintptr_t)scheduler;
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    LOCK();
    Sched* s = find_sched(ea);
    s32 rc = CELL_OK;
    if (!s) {
        rc = (s32)CELL_FIBER_ERROR_INVAL;
    } else if (s->running) {
        rc = (s32)CELL_FIBER_ERROR_STAT;
    } else {
        /* Fibers that never ran to their end are dropped with their host stacks. Nothing resumes them: RunFibers is
         * not running, and a JoinFiber waiting for one finds it gone and fails with INVAL. */
        for (Fiber** pf = &s_fibers; *pf;) {
            Fiber* f = *pf;
            if (f->sched == s) {
                *pf = f->next;
                host_destroy(f);
                free(f);
            } else {
                pf = &f->next;
            }
        }
        for (Sched** ps = &s_scheds; *ps; ps = &(*ps)->next) {
            if (*ps == s) { *ps = s->next; break; }
        }
        host_sched_destroy(s);
        free(s);
    }
    UNLOCK();
    WAKE();
    return rc;
}

s32 cellFiberPpuCreateFiber(CellFiberPpuScheduler* scheduler, CellFiberPpu* fiber, u32 entry, u64 arg,
                            u32 priority, void* eaStack, u32 sizeStack, const CellFiberPpuAttribute* attr)
{
    (void)attr;
    const u32 sched_ea = (u32)(uintptr_t)scheduler;
    const u32 fiber_ea = (u32)(uintptr_t)fiber;
    const u32 stack_ea = (u32)(uintptr_t)eaStack;
    if (!sched_ea || !fiber_ea || !entry || !stack_ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;
    if (fiber_ea % CELL_FIBER_PPU_ALIGN) return (s32)CELL_FIBER_ERROR_ALIGN;
    if (sizeStack < FIBER_MIN_STACK) return (s32)CELL_FIBER_ERROR_INVAL;

    Fiber* f = (Fiber*)calloc(1, sizeof *f);
    if (!f) return (s32)CELL_FIBER_ERROR_NOMEM;
    f->ea         = fiber_ea;
    f->opd        = entry;
    f->arg        = arg;
    f->priority   = priority;
    f->stack_ea   = stack_ea;
    f->stack_size = sizeStack;
    f->state      = F_READY;

    /* r13 stays the creator's, so the fiber's TLS is the creating thread's (0 lets the trampoline pick a block). */
    const ppu_context* parent = g_active_ctx;
    f->ctx.gpr[13]   = parent ? parent->gpr[13] : 0;
    f->ctx.thread_id = parent ? parent->thread_id : FIBER_FALLBACK_THREAD_ID;

    if (!host_create(f)) {
        free(f);
        return (s32)CELL_FIBER_ERROR_NOMEM;
    }

    LOCK();
    Sched* s = find_sched(sched_ea);
    s32 rc = CELL_OK;
    if (!s) {
        rc = (s32)CELL_FIBER_ERROR_INVAL;
    } else if (find_fiber(fiber_ea)) {
        rc = (s32)CELL_FIBER_ERROR_STAT;
    } else {
        f->sched = s;
        f->seq   = s_seq++;
        f->next  = s_fibers;
        s_fibers = f;
    }
    UNLOCK();
    if (rc != CELL_OK) {
        host_destroy(f);
        free(f);
        return rc;
    }
    WAKE();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Running
 * -----------------------------------------------------------------------*/

s32 cellFiberPpuRunFibers(CellFiberPpuScheduler* scheduler)
{
    const u32 ea = (u32)(uintptr_t)scheduler;
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    LOCK();
    Sched* s = find_sched(ea);
    if (!s) { UNLOCK(); return (s32)CELL_FIBER_ERROR_INVAL; }
    if (s->running || t_cur) { UNLOCK(); return (s32)CELL_FIBER_ERROR_STAT; }
    s->running = 1;

    for (;;) {
        Fiber* f = pick_runnable(s);
        if (!f) {
            if (all_done(s)) break;
            WAIT();              /* its fibers wait for a signal, or a join, that another thread delivers */
            continue;
        }
        f->state = F_RUNNING;
        UNLOCK();

        ppu_context* saved_active = g_active_ctx;
        t_cur        = f;
        g_active_ctx = &f->ctx;
        host_resume(s, f);       /* back here when f blocks, yields or ends */
        g_active_ctx = saved_active;
        t_cur        = NULL;

        LOCK();
        if (f->state == F_DONE)
            host_destroy(f);     /* off its stack now; the Fiber stays for JoinFiber's status */
    }
    s->running = 0;
    UNLOCK();
    return CELL_OK;
}

s32 cellFiberPpuWaitSignal(void)
{
    Fiber* f = t_cur;
    if (!f) return (s32)CELL_FIBER_ERROR_PERM;

    LOCK();
    if (f->signal_pending) {
        f->signal_pending = 0;
        UNLOCK();
        return CELL_OK;
    }
    f->state = F_WAIT_SIGNAL;
    UNLOCK();
    host_yield(f);               /* RunFibers runs the others until SendSignal makes this one ready */
    return CELL_OK;
}

s32 cellFiberPpuSendSignal(CellFiberPpu* fiber, u32* numWorker)
{
    const u32 ea = (u32)(uintptr_t)fiber;
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    LOCK();
    Fiber* f = find_fiber(ea);
    if (!f) { UNLOCK(); return (s32)CELL_FIBER_ERROR_INVAL; }
    if (f->state == F_WAIT_SIGNAL)  f->state = F_READY;
    else if (f->state != F_DONE)    f->signal_pending = 1;
    UNLOCK();
    WAKE();
    if (numWorker) vm_write32((u32)(uintptr_t)numWorker, 0);
    return CELL_OK;
}

s32 cellFiberPpuYield(void)
{
    Fiber* f = t_cur;
    if (!f) return (s32)CELL_FIBER_ERROR_PERM;

    LOCK();
    f->state = F_READY;
    f->seq   = s_seq++;          /* behind the other fibers of its priority */
    UNLOCK();
    host_yield(f);
    return CELL_OK;
}

s32 cellFiberPpuExit(s32 status)
{
    Fiber* f = t_cur;
    if (!f) return (s32)CELL_FIBER_ERROR_PERM;

    LOCK();
    fiber_finish_locked(f, status);
    UNLOCK();
    WAKE();
    fiber_leave(f);
    return CELL_OK;              /* not reached */
}

CellFiberPpu* cellFiberPpuSelf(void)
{
    const Fiber* f = t_cur;
    return (CellFiberPpu*)(uintptr_t)(f ? f->ea : 0);
}

/* Waits for the fiber to end and returns its exit status. Called from a fiber it hands control back to RunFibers
 * meanwhile (blocking the host thread would stop the scheduler that has to run the fiber waited for); from any other
 * thread it blocks. */
s32 cellFiberPpuJoinFiber(CellFiberPpu* fiber, s32* status)
{
    const u32 ea = (u32)(uintptr_t)fiber;
    if (!ea) return (s32)CELL_FIBER_ERROR_NULL_POINTER;

    Fiber* self = t_cur;
    LOCK();
    Fiber* f = find_fiber(ea);
    if (!f || f == self) { UNLOCK(); return (s32)CELL_FIBER_ERROR_INVAL; }
    if (self && f->state != F_DONE) {
        self->state   = F_WAIT_JOIN;
        self->join_ea = ea;
        UNLOCK();
        host_yield(self);        /* ready again once the fiber has ended */
        LOCK();
        self->join_ea = 0;
    }
    while ((f = find_fiber(ea)) != NULL && f->state != F_DONE)
        WAIT();
    s32 rc = CELL_OK;
    if (!f) rc = (s32)CELL_FIBER_ERROR_INVAL;      /* the scheduler was finalized under the join */
    else if (status) vm_write32((u32)(uintptr_t)status, (u32)f->status);
    UNLOCK();
    return rc;
}
