// clang-format off

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* This file is based on posix-host.c from LKL, modified to provide a host
 * interface for enclave environments. */

// clang-format off

#include <errno.h>
#include <futex.h>
#include <lkl_host.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include "syscall.h"

#include "lkl/iomem.h"
#include "lkl/jmp_buf.h"
#include "lkl/posix-host.h"
#include "lkl/setup.h"

#include "enclave/lthread.h"
#include "enclave/lthread_int.h"
#include "enclave/enclave_state.h"
#include "enclave/enclave_timer.h"
#include "enclave/enclave_util.h"
#include "shared/sgxlkl_enclave_config.h"
#include "enclave/sgxlkl_t.h"
#include "lkl/iomem.h"
#include "lkl/jmp_buf.h"
#include "openenclave/corelibc/oemalloc.h"
#include "openenclave/internal/print.h"
#include "syscall.h"

#define NSEC_PER_SEC 1000000000L

int enclave_futex_wait(int* uaddr, int val);

int enclave_futex_timedwait(
    int* uaddr,
    int val,
    const struct timespec* timeout);

int enclave_futex_wake(int* uaddr, int val);

static void panic(void)
{
    const sgxlkl_enclave_config_t* cfg = sgxlkl_enclave_state.config;
    sgxlkl_fail(
        "Kernel panic!%s Aborting...\n",
        cfg->kernel_verbose
            ? ""
            : " Run DEBUG build with SGXLKL_KERNEL_VERBOSE=1 for more "
              "information.");
}

static void terminate(int exit_status, int received_signal)
{
    /* Is the termination due to a received signal? */
    if (received_signal)
    {
        /* TODO: add missing error codes here */
        switch (received_signal)
        {
            case SIGSEGV:
                oe_host_printf("Segmentation fault\n");
                exit_status = 139;
                break;
            case SIGKILL:
                oe_host_printf("Killed\n");
                exit_status = 137;
                break;
            case SIGABRT:
                oe_host_printf("Aborted\n");
                exit_status = 134;
                break;
            case SIGTERM:
                oe_host_printf("Terminated\n");
                exit_status = 143;
                break;
            default:
                sgxlkl_error(
                    "Unhandled signal %i received. Aborting.\n",
                    received_signal);
                if (!exit_status)
                {
                    exit_status = 1;
                }
        }
    }

    sgxlkl_host_app_main_end();

    LKL_TRACE(
        "Shutting down SGX-LKL (exit_status=%i received_signal=%i)\n",
        exit_status,
        received_signal);
    lkl_terminate(exit_status);
}

static void print(const char* str, int len)
{
    oe_host_printf("%.*s", len, str);
}

/**
 * Mutex state.
 */
enum mutex_state
{
    /** Unlocked, can be acquired without blocking. */
    unlocked = 0,
    /** Locked, but not threads are waiting.  Can unlock without waking
     * anything, the first waiter must change the state. */
    locked_no_waiters = 1,
    /** Locked and has waiters.  When unlocking, must wake other threads.
     */
    locked_waiters = 2
};

struct lkl_mutex
{
    /**
     * The state of this mutex.  Used as the futex value.
     */
    _Atomic(enum mutex_state) flag;
    /**
     * If this is a recursive mutex, which thread owns it?
     */
    _Atomic(struct lthread*) owner;
    /**
     * Is this a recursive mutex? If this is false then the `owner` field
     * is unused.
     */
    bool is_recursive : 1;
    /**
     * The number of times a recursive mutex has been locked beyond the
     * initial lock.  This can be modified only with the mutex locked and
     * so doesn't need to be atomic.
     */
    int recursion_count : 31;
};

struct lkl_sem
{
    /**
     * Semaphore count.  This is a naive implementation that assumes all
     * semaphores have waiters.
     */
    _Atomic(int) count;
};

struct lkl_tls_key
{
    /**
     * The key used by the lthreads library.
     */
    long key;
};

#define WARN_UNLESS(exp)                                        \
    do                                                          \
    {                                                           \
        if (exp < 0)                                            \
            sgxlkl_fail("%s: %s\n", #exp, lkl_strerror(errno)); \
    } while (0)

static int _warn_pthread(int ret, char* str_exp)
{
    if (ret != 0)
        sgxlkl_fail("%s: %s\n", str_exp, lkl_strerror(ret));

    return ret;
}

/* pthread_* functions use the reverse convention */
#define WARN_PTHREAD(exp) _warn_pthread(exp, #exp)

static struct lkl_sem* sem_alloc(int count)
{
    struct lkl_sem* sem;

#ifdef LKL_SEM_UAF_CHECKS
    sem = paranoid_alloc(sizeof(struct lkl_sem));
#else
    sem = oe_calloc(1, sizeof(*sem));
    if (!sem)
        return NULL;
#endif

    sem->count = count;

    return sem;
}

static void sem_free(struct lkl_sem* sem)
{
#if LKL_SEM_UAF_CHECKS
    paranoid_dealloc(sem, sizeof(struct lkl_sem));
#else
    oe_free(sem);
#endif
}

/*
* sem_up/sem_down interaction
*
* Because sem_up and sem_down underpins a lot of sgx-lkl functionality, it is
* very important that they interact correctly. However, that interaction might
* not be immediately obvious. What follows is a description of how they
* interact.
*
* The semaphore implementation is modeling an old railroad switching model.
* In order to take an action, one withdraws a flag from a bucket. If there are
* no flags in the bucket, you wait until one is available.
*
* sem->count is the number of flags available at the current time
*
* sem_up adds a flag back into the bucket.
* sem_down removes a flag from the bucket allowing an action to be taken
*
* That is:
* - sem_down is ACQUIRE
* - sem_up is RELEASE
*
* If count is 0, that means there is somewhere between 0 and infinity waiters.
* We do not know from count if there are waiters, only that there might be some.
* A count above 0 doesn't mean that there are no waiters, only that waiters
* might not have "claimed a flag" by decrementing the count.
*
* The actual mechanics as seen in sem_up and sem_down are as follows:
*
* - maintain an atomic counter `count`
* - increment the count when releasing during `sem_up`
* - attempt to decrement the count to acquire during `sem_down`
* - any waiters sleep using `enclave_futex_wait`
* - when releasing, if there might be any waiters, wake them all using
*     `enclave_futex_wake`
* - any waiter that succeeds in decrementing the count before it hits 0
*     will acquire the semaphore and exit `sem_down`
* - all others waiters will go back to sleep via `enclave_futex_wait`
*
* See `sem_up` and `sem_down` for more particulars.
*
* Within this implementation, sem_up must wake all waiters otherwise, we could
* have a possible loss of a wake-up in some interleavings of sem_up and
* sem_down.
*
* Every sem_up calls must be paired with a sem_down call, otherwise, all
* guarantees are broken and "bad things will happen".
*
* There is nothing inherent in the implementation that makes the semaphore
* exclusive. You could use it to allow multiple items access to the controlled
* resource by setting an initial count of more than 1 during `sem_alloc`.
*
* Like-wise, sem_alloc can set the count to 0 which will act as a gate that
* waiters will block on until such time as it is released via a call to sem_up.
*
* Setting count to 1 via sem_alloc would result in an exclusive semaphore that
* can only have a single owner at a time.
*/
static void sem_up(struct lkl_sem* sem)
{
    // Increment the semaphore count.  If we are moving from 0 to non-zero,
    // there may be waiters.  Wake one up.
    if (atomic_fetch_add(&sem->count, 1) == 0)
    {
        enclave_futex_wake((int*)&sem->count, INT_MAX);
    }
}

static void sem_down(struct lkl_sem* sem)
{
    int count = sem->count;
    // Loop if the count is 0 or if we try to decrement it but fail.
    while ((count == 0) ||
           !atomic_compare_exchange_weak(&sem->count, &count, count - 1))
    {
        // If the value is non-zero, we lost a race, so try again (this
        // could be avoided by doing an atomic decrement and handling
        // the negative case, but this is the simplest possible
        // implementation).
        // If the value is 0, we need to wait until another thread
        // releases a value, so sleep and then reload the value of
        // count.
        if (count == 0)
        {
            enclave_futex_wait((int*)&sem->count, 0);
            count = sem->count;
        }
    }
}

static struct lkl_mutex* mutex_alloc(int recursive)
{
    struct lkl_mutex* mutex = oe_calloc(1, sizeof(struct lkl_mutex));

    if (!mutex)
        return NULL;

    mutex->is_recursive = recursive;

    return mutex;
}

static void mutex_lock(struct lkl_mutex* mutex)
{
    enum mutex_state state = unlocked;
    // Try to transition from from unlocked to locked with no waiters.  If
    // this works, return immediately, we've acquired the lock.  If not,
    // then we need to register ourself as a waiter.  This can spuriously
    // fail.  If it does, we hit the slow path when we don't need to, but
    // we are still correct.
    if (!atomic_compare_exchange_weak(&mutex->flag, &state, locked_no_waiters))
    {
        if (mutex->is_recursive && (mutex->owner == lthread_self()))
        {
            mutex->recursion_count++;
            return;
        }
        // Mark the mutex as having waiters.
        if (state != 2)
        {
            state = atomic_exchange(&mutex->flag, locked_waiters);
        }
        while (state != unlocked)
        {
            enclave_futex_wait((int*)&mutex->flag, locked_waiters);
            state = atomic_exchange(&mutex->flag, locked_waiters);
        }
    }
    // If this is a recursive mutex, update the owner to this thread.
    // Skip for non-recursive mutexes to avoid the lthread_self call.
    if (mutex->is_recursive)
    {
        mutex->owner = lthread_self();
    }
}

static void mutex_unlock(struct lkl_mutex* mutex)
{
    // If this is a recursive mutex, we may not actually unlock it.
    if (mutex->is_recursive)
    {
        // If we are just undoing a recursive lock, decrement the
        // counter.
        if (mutex->recursion_count > 0)
        {
            mutex->recursion_count--;
            return;
        }
        // Clear the owner.
        mutex->owner = 0;
    }
    if (atomic_fetch_sub(&mutex->flag, 1) != locked_no_waiters)
    {
        // Implicitly sequentially-consistent atomic
        mutex->flag = 0;
        // Wake up all waiting threads.  We could improve this to wake
        // only one thread if we kept track of the number of waiters,
        // though doing that in a non-racy way is non-trivial.
        enclave_futex_wake((int*)&mutex->flag, INT_MAX);
    }
}

static void mutex_free(struct lkl_mutex* _mutex)
{
    oe_free(_mutex);
}

static lkl_thread_t thread_create(void (*fn)(void*), void* arg)
{
    struct lthread* thread;
    int ret = lthread_create(&thread, NULL, (void* (*)(void*))fn, arg);
    if (ret)
    {
        sgxlkl_fail("lthread_create failed: %s\n", lkl_strerror(ret));
    }
    LKL_TRACE("created (thread=%p)\n", thread);
    return (lkl_thread_t)thread;
}

/**
 * Create an lthread to back a Linux task, created with a clone-family call
 * into the kernel.
 */
static lkl_thread_t thread_create_host(void* pc, void* sp, void* tls, struct lkl_tls_key* task_key, void* task_value)
{
    struct lthread* thread;
    // Create the thread.  The lthread layer will set up the threading data
    // structures and prepare the lthread to run with the specified instruction
    // and stack addresses.
    int ret = lthread_create_primitive(&thread, pc, sp, tls);
    if (ret)
    {
        sgxlkl_fail("lthread_create failed\n");
    }
    // Store the host task pointer.  LKL normally sets this lazily the first
    // time that a thread calls into the LKL.  Threads created via this
    // mechanism begin life in the kernel and so need to be associated with the
    // kernel task that created them.
    lthread_setspecific_remote(thread, task_key->key, task_value);
    // Mark the thread as runnable.  This must be done *after* the
    // `lthread_setspecific_remote` call, to ensure that the thread does not
    // run while we are modifying its TLS.
    __scheduler_enqueue(thread);
    return (lkl_thread_t)thread;
}

/**
 * Destroy the lthread backing a host task created with a clone-family call.
 * This is called after an `exit` system call.  The system call does not return
 * and the lthread backing the LKL thread that issued the task will not be
 * invoked again.
 */
static void thread_destroy_host(lkl_thread_t tid, struct lkl_tls_key* task_key)
{
    // Somewhat arbitrary size of the stack for teardown.  This must be big
    // enough that `host_thread_exit` can run and call any remaining TLS
    // destructors.  This can be removed once there is a clean mechanism for
    // destroying a not-running lthread without scheduling it.
    struct lthread *thr = (struct lthread*)tid;
    SGXLKL_VERBOSE("enter tid=%i\n", thr->tid);
    SGXLKL_ASSERT(thr->lt_join == NULL);
    // The thread is currently blocking on the LKL scheduler semaphore, remove
    // it from the sleeping list.
    _lthread_desched_sleep(thr);
    // Ensure that the enclave futex does not wake this thread up.  This thread
    // is currently sleeping on its scheduler semaphore.  If another semaphore
    // is allocated at this address this could get a spurious wakeup (which
    // would then dereference memory in the thread structure, which may also
    // have been reallocated and can corrupt the futex wait queue).
    futex_dequeue(thr);
    // Delete its task reference in TLS.  Without this, the thread's destructor
    // will call back into LKL and deadlock.
    lthread_setspecific_remote(thr, task_key->key, NULL);
    // Delete the thread.
    _lthread_free(thr);
}


static void thread_detach(void)
{
    LKL_TRACE("enter\n");
    lthread_detach();
}

static void thread_exit(void)
{
    LKL_TRACE("enter\n");
    lthread_exit(0);
}

static int thread_join(lkl_thread_t tid)
{
    LKL_TRACE("enter (tid=%li)\n", tid);

    struct lthread* lt = (struct lthread*) tid;

    int ret = lthread_join(lt, NULL, -1);
    if (ret)
    {
        sgxlkl_fail("lthread_join failed: %s\n", lkl_strerror(ret));
    }
    return 0;
}

static lkl_thread_t thread_self(void)
{
    return (lkl_thread_t)lthread_self();
}

static int thread_equal(lkl_thread_t a, lkl_thread_t b)
{
    return a == b;
}

static struct lkl_tls_key* tls_alloc(void (*destructor)(void*))
{
    LKL_TRACE("enter (destructor=%p)\n", destructor);
    struct lkl_tls_key* ret = oe_malloc(sizeof(struct lkl_tls_key));

    if (WARN_PTHREAD(lthread_key_create(&ret->key, destructor)))
    {
        oe_free(ret);
        return NULL;
    }
    return ret;
}

static void tls_free(struct lkl_tls_key* key)
{
    LKL_TRACE("enter (key=%p)\n", key);
    WARN_PTHREAD(lthread_key_delete(key->key));
    oe_free(key);
}

static int tls_set(struct lkl_tls_key* key, void* data)
{
    LKL_TRACE("enter (key=%p data=%p)\n", key, data);
    if (WARN_PTHREAD(lthread_setspecific(key->key, data)))
        return -1;
    return 0;
}

static void* tls_get(struct lkl_tls_key* key)
{
    return lthread_getspecific(key->key);
}

typedef struct sgxlkl_timer
{
    void (*callback_fn)(void*);
    void* callback_arg;
    unsigned long long delay_ns;
    unsigned long long next_delay_ns;
    struct lthread* thread;
    /**
     * Mutex used to protect access to this structure between threads setting
     * the timer and the thread that handles the callback.
     */
    struct lkl_mutex mtx;
    /**
     * Free-running counter used as a futex for wakeups.  The sleeping thread
     * reads the value with `mtx` held, releases `mtx`, then sleeps with the
     * read value as the expected version.  The waking thread increments this
     * counter with the `mtx` held before sending the futex wake.
     */
    _Atomic(int) wake;
    /** Flag indicating that the timer is armed. */
    _Atomic(bool) armed;
} sgxlkl_timer;

static void* timer_callback(void* _timer)
{
    sgxlkl_timer* timer = (sgxlkl_timer*)_timer;
    struct timespec timeout;

    if (timer == NULL || timer->callback_fn == NULL)
    {
        sgxlkl_fail("timer_callback() called with unitialised timer.\n");
    }

    mutex_lock(&timer->mtx);

    do
    {
        if (timer->delay_ns <= 0)
        {
            SGXLKL_VERBOSE("timer->delay_ns=%llu <= 0\n", timer->delay_ns);
            break;
        }

        timeout.tv_sec = timer->delay_ns / NSEC_PER_SEC;
        timeout.tv_nsec = timer->delay_ns % NSEC_PER_SEC;

        // Record the initial wake flag to before releasing the mutex.  We will
        // only ever be woken by a thread that holds the mutex, so this avoids a
        // race: the waking side will increment the counter and then wake us
        // with the mutex held, so `enclave_futex_timedwait` will return
        // immediately if the other thread increments the counter before
        // waking us.
        int wake = timer->wake;
        mutex_unlock(&timer->mtx);
        bool did_timeout =
            enclave_futex_timedwait((int*)&timer->wake, wake, &timeout) == -ETIMEDOUT;
        mutex_lock(&timer->mtx);

        // Check if the timer should shut down
        if (!timer->armed)
        {
            break;
        }

        // Check if the timer has triggered
        if (did_timeout)
        {
            timer->next_delay_ns = 0;
            timer->callback_fn(timer->callback_arg);
            // If the callback function itself resets the timer,
            // timer->next_delay_ns will be non-zero.
            if (timer->next_delay_ns)
            {
                timer->delay_ns = timer->next_delay_ns;
                timer->next_delay_ns = 0;
            }
        }

    } while (timer->armed);
    mutex_unlock(&timer->mtx);

    lthread_exit(NULL);
}

static void* timer_alloc(void (*fn)(void*), void* arg)
{
    sgxlkl_timer* timer = oe_calloc(sizeof(*timer), 1);

    if (timer == NULL)
    {
        sgxlkl_fail("LKL host op: timer_alloc() failed. OOM\n");
    }
    timer->callback_fn = fn;
    timer->callback_arg = arg;
    timer->armed = 0;
    timer->delay_ns = 0;
    timer->next_delay_ns = 0;

    return (void*)timer;
}

static int timer_set_oneshot(void* _timer, unsigned long ns)
{
    sgxlkl_timer* timer = (sgxlkl_timer*)_timer;

    // timer_set_oneshot is executed as part of the current timer's
    // callback. Do not try to acquire the lock we are already holding.
    if (timer->thread == lthread_self())
    {
        // Fail if the timer is being destroyed
        if (!timer->armed)
        {
            SGXLKL_VERBOSE("timer_set_oneshot() called on destroyed timer\n");
            return -1;
        }

        if (timer->next_delay_ns)
        {
            if (ns < timer->next_delay_ns)
            {
                timer->next_delay_ns = ns;
            }
        }
        else
        {
            timer->next_delay_ns = ns;
        }
    }
    else
    {
        mutex_lock(&timer->mtx);

        // Are we updating an armed timer or arming a new timer?
        if (timer->armed)
        {
            timer->delay_ns = ns;
            timer->wake++;
            enclave_futex_wake((int*)&timer->wake, 1);
        }
        else
        {
            timer->armed = true;
            timer->delay_ns = ns;
            timer->next_delay_ns = 0;

            int res = lthread_create(
                &(timer->thread), NULL, &timer_callback, (void*)timer);
            if (res != 0)
            {
                sgxlkl_fail("pthread_create(timer_thread) returned %d\n", res);
            }
        }

        mutex_unlock(&timer->mtx);
    }

    return 0;
}

static void timer_free(void* _timer)
{
    sgxlkl_timer* timer = (sgxlkl_timer*)_timer;
    if (timer == NULL)
    {
        sgxlkl_fail("timer_free() called with NULL\n");
    }

    mutex_lock(&timer->mtx);

    bool current_value = true;
    if (atomic_compare_exchange_strong(&timer->armed, &current_value, false))
    {
        timer->wake++;
        enclave_futex_wake((int*)&timer->wake, 1);
        mutex_unlock(&timer->mtx);

        void* exit_val = NULL;
        int res = lthread_join(timer->thread, &exit_val, -1);
        if (res != 0)
        {
            sgxlkl_warn("lthread_join(timer_thread) returned %d\n", res);
        }
    }
    else
    {
        SGXLKL_VERBOSE("timer->thread not armed\n");
        mutex_unlock(&timer->mtx);
    }

    oe_free(_timer);
}

static long _gettid(void)
{
    return (long)lthread_self();
}

/**
 * The allocation for kernel memory.
 */
static void *kernel_mem;
/**
 * The size of kernel heap area.
 */
static size_t kernel_mem_size;

/**
 * Allocate memory for LKL.  This is used in precisely two places as we build
 * LKL:
 *
 * 1. Allocating the kernel's memory.
 * 2. Allocating buffers for lkl_vprintf to use printing debug messages.
 *
 * We allocate the former from the `enclave_mmap` space, but smaller buffers
 * from the OE heap.
 */
static void *host_malloc(size_t size)
{
    // If we're allocating over 1MB, we're probably allocating the kernel heap.
    // Pull this out of the enclave mmap area: there isn't enough space in the
    // OE heap for it.
    if (size > 1024*1024)
    {
        SGXLKL_ASSERT(kernel_mem == NULL);
        kernel_mem = enclave_mmap(0, size, 0, PROT_READ | PROT_WRITE, 0);
        if (kernel_mem < 0)
        {
            // unable to mmap memory. return NULL that malloc returns on
            // failure

            return NULL;
        }
        kernel_mem_size = size;
        return kernel_mem;
    }

    return oe_malloc(size);
}

/**
 * Free memory allocated with `host_malloc`.
 */
static void host_free(void *ptr)
{
    if (ptr == kernel_mem)
    {
        enclave_munmap(kernel_mem, kernel_mem_size);
        kernel_mem = 0;
        kernel_mem_size = 0;
        return;
    }
    oe_free(ptr);
}

/**
 * Returns the information displayed in /proc/cpuinfo
 */
static unsigned int sgxlkl_cpuinfo_get(char *buffer, unsigned int buffer_len)
{
    const sgxlkl_enclave_config_t* econf = sgxlkl_enclave_state.config;

    int len;
    unsigned int total_len = 0;
    unsigned int current_core = 0;
    unsigned int num_cores = econf->ethreads;

    for (current_core = 0; current_core < num_cores; current_core++) {

        len = snprintf(buffer, buffer_len,
            "processor       : %u\n"
            "cpu family      : 6\n"
            "model           : 158\n"
            "model name      : Intel(R) Xeon(R) CPU E3-1280 v6 @ 3.90GHz\n"
            "stepping        : 9\n"
            "microcode       : 0xb4\n"
            "cpu MHz         : 800.063\n"
            "cache size      : 8192 KB\n"
            "physical id     : %u\n"
            "siblings        : %u\n"
            "core id         : %u\n"
            "cpu cores       : %u\n"
            "apicid          : 0\n"
            "initial apicid  : 0\n"
            "fpu             : yes\n"
            "fpu_exception   : yes\n"
            "cpuid level     : 22\n"
            "wp              : yes\n"
            "flags           : fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse3 clflush dts acpi mmx fxsr sse sse2 ss ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc art arch_perfmon pebs bt rep_good nopl xtopology nonstop_tsc cpuid aperfmperf tsc_known_freq pni pclmulqdq dtes64 monitor ds_cpl vmx smx est tm2 ssse3 sdbg fma cx16 xtpr pdcm pcid sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand lahf_lm aabm 3dnowprefetch cpuid_fault epb invpcid_single pti ssbd ibrs ibpb stibp tpr_shadow vnmi flexpriority ept vpid fsgsbase tsc_adjust bmi1 hle avx2 smep bmi2 erms invpcid rtm mpx rdseed adx smap clflushopt intel_pt xsaveopt xsavec xgetbv1 xsaves dtherm ida arat pln pts hwp hwp_notify hwp_act_window hwp_epp md_clear flush_l1d\n"
            "bugs            : cpu_meltdown spectre_v1 spectre_v2 spec_store_bypass l1tf mds swapgs\n"
            "bogomips        : 7824.00\n"
            "clflush size    : 64\n"
            "cache_alignment : 64\n"
            "address sizes   : 39 bits physical, 48 bits virtual\n"
            "power management: \n"
            "\n",
            current_core,
            current_core, num_cores,
            current_core, num_cores);

        if (len < 0) {
            // This can only happen if there is some sort of output error, which
            // shouldn't happen for snprintf().
            return 0;
        }

        if (len >= buffer_len) {
            buffer = NULL;
            buffer_len = 0;
        } else {
            buffer += len;
            buffer_len -= len;
        }

        total_len += len;
    }

    return total_len;
}

struct lkl_host_operations sgxlkl_host_ops = {
    .panic = panic,
    .terminate = terminate,
    .thread_create = thread_create,
    .thread_create_host = thread_create_host,
    .thread_destroy_host = thread_destroy_host,
    .thread_detach = thread_detach,
    .thread_exit = thread_exit,
    .thread_join = thread_join,
    .thread_self = thread_self,
    .thread_equal = thread_equal,
    .sem_alloc = sem_alloc,
    .sem_free = sem_free,
    .sem_up = sem_up,
    .sem_down = sem_down,
    .mutex_alloc = mutex_alloc,
    .mutex_free = mutex_free,
    .mutex_lock = mutex_lock,
    .mutex_unlock = mutex_unlock,
    .tls_alloc = tls_alloc,
    .tls_free = tls_free,
    .tls_set = tls_set,
    .tls_get = tls_get,
    .time = enclave_nanos,
    .timer_alloc = timer_alloc,
    .timer_set_oneshot = timer_set_oneshot,
    .timer_free = timer_free,
    .print = print,
    .mem_alloc = host_malloc,
    .mem_free = host_free,
    .ioremap = lkl_ioremap,
    .iomem_access = lkl_iomem_access,
    .virtio_devices = lkl_virtio_devs,
    .gettid = _gettid,
    .jmp_buf_set = sgxlkl_jmp_buf_set,
    .jmp_buf_longjmp = sgxlkl_jmp_buf_longjmp,
    .cpuinfo_get = sgxlkl_cpuinfo_get,
};
