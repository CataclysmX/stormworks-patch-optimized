/* Parallel vehicle tick.
 *
 * The game queues one c_scene_vehicle_body::threaded_tick_task per vehicle body per tick on a
 * single "vehicle_tick" mm_thread_object, so every body is simulated serially. Each body keeps
 * its task and waits for it (task destructor) before queueing the next one, and the tick only
 * touches per-body data plus mutex-protected shared grids. We divert those tasks (identified by
 * vtable) from mm_thread_object::enqueue to a pool of N workers and complete them with the same
 * protocol as the game's worker loop (FUN_14017d800). Every other task goes to the original. */
#include "stwpatch.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef void (*enqueue_t)(void *thread_obj, uint8_t *task, void *r8, void *r9);
typedef void (*set_state_t)(uint8_t *task, int state);
typedef int  (*mtx_lock_t)(void *mtx);
typedef void (*mtx_unlock_t)(void *mtx);
typedef void (*cnd_broadcast_t)(void *cnd);

static enqueue_t       orig_enqueue;
static set_state_t     task_set_state;
static mtx_lock_t      mtx_lock;
static mtx_unlock_t    mtx_unlock;
static cnd_broadcast_t cnd_broadcast;
static void           *vt_vehicle_tick;

static SRWLOCK            q_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE q_cv = CONDITION_VARIABLE_INIT;
static uint8_t          **q_buf;
static unsigned           q_cap, q_head, q_count;

static volatile LONG64 stat_tasks;

static void q_push(uint8_t *task)
{
    AcquireSRWLockExclusive(&q_lock);
    if (q_count == q_cap) {
        unsigned ncap = q_cap ? q_cap * 2 : 256;
        uint8_t **nb = malloc(sizeof *nb * ncap);
        for (unsigned i = 0; i < q_count; i++) nb[i] = q_buf[(q_head + i) % q_cap];
        free(q_buf);
        q_buf = nb; q_cap = ncap; q_head = 0;
    }
    q_buf[(q_head + q_count) % q_cap] = task;
    q_count++;
    ReleaseSRWLockExclusive(&q_lock);
    WakeConditionVariable(&q_cv);
}

static uint8_t *q_pop(void)
{
    AcquireSRWLockExclusive(&q_lock);
    while (q_count == 0) SleepConditionVariableSRW(&q_cv, &q_lock, INFINITE, 0);
    uint8_t *t = q_buf[q_head];
    q_head = (q_head + 1) % q_cap;
    q_count--;
    ReleaseSRWLockExclusive(&q_lock);
    return t;
}

static void task_complete(uint8_t *task)
{
    /* Unlike the game's loop we clear the pending flag under the task mutex, so the waiting
     * destructor cannot free the task between the flag store and our lock. */
    mtx_lock(task + TASK_OFF_MUTEX);
    *(volatile uint8_t *)(task + TASK_OFF_PENDING) = 0;
    InterlockedExchange((volatile LONG *)(task + TASK_OFF_STATE), 1);
    cnd_broadcast(task + TASK_OFF_CONDVAR);
    mtx_unlock(task + TASK_OFF_MUTEX);
}

static void name_thread(int i)
{
    typedef HRESULT (WINAPI *std_t)(HANDLE, PCWSTR);
    std_t f = (std_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    wchar_t name[32];
    swprintf(name, 32, L"stw-vehicle-%d", i);
    if (f) f(GetCurrentThread(), name);
}

static DWORD WINAPI worker(LPVOID arg)
{
    name_thread((int)(intptr_t)arg);
    for (;;) {
        uint8_t *task = q_pop();
        void (*run)(uint8_t *) = (*(void (***)(uint8_t *))task)[1];
        run(task);
        task_complete(task);
        InterlockedIncrement64(&stat_tasks);
    }
    return 0;
}

static void hook_enqueue(void *thread_obj, uint8_t *task, void *r8, void *r9)
{
    if (*(void **)task != vt_vehicle_tick) {
        orig_enqueue(thread_obj, task, r8, r9);
        return;
    }
    *(volatile uint8_t *)(task + TASK_OFF_PENDING) = 1;
    task_set_state(task, 0);
    q_push(task);
}

static DWORD WINAPI stats_thread(LPVOID p)
{
    (void)p;
    for (;;) {
        Sleep(30000);
        log_printf("vehicle pool: %lld body ticks executed", (long long)stat_tasks);
    }
    return 0;
}

int vehicle_pool_install(void)
{
    /* mov rax,rsp / mov [rax+8],rbx / mov [rax+10h],rbp / mov [rax+18h],rsi / push rdi */
    static const uint8_t prologue[16] = {
        0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48,
        0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57 };
    static const uint8_t set_state_sig[5] = { 0x48, 0x89, 0x5c, 0x24, 0x08 };

    uint8_t *enq = g_addr.thread_enqueue;
    if (!enq || !g_addr.vt_vehicle_tick || !g_addr.task_set_state || !g_addr.mtx_lock || !g_addr.mtx_unlock ||
        !g_addr.cnd_broadcast) {
        log_printf("vehicle pool: DISABLED, game functions not found in this build");
        return 0;
    }
    vt_vehicle_tick = g_addr.vt_vehicle_tick;
    task_set_state  = (set_state_t)g_addr.task_set_state;
    mtx_lock        = (mtx_lock_t)g_addr.mtx_lock;
    mtx_unlock      = (mtx_unlock_t)g_addr.mtx_unlock;
    cnd_broadcast   = (cnd_broadcast_t)g_addr.cnd_broadcast;

    if (memcmp(task_set_state, set_state_sig, sizeof set_state_sig) != 0) {
        log_printf("vehicle pool: DISABLED, unknown game build (set_state mismatch)");
        return 0;
    }
    orig_enqueue = (enqueue_t)make_trampoline(enq, sizeof prologue, prologue);
    if (!orig_enqueue) {
        log_printf("vehicle pool: DISABLED, unknown game build (enqueue prologue mismatch)");
        return 0;
    }
    for (int i = 0; i < g_cfg.vehicle_threads; i++) {
        HANDLE h = CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)i, 0, NULL);
        if (!h) continue;
        SetThreadPriority(h, g_cfg.vehicle_thread_prio);
        CloseHandle(h);
    }
    HANDLE h = CreateThread(NULL, 0, stats_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    if (!patch_jump(enq, sizeof prologue, hook_enqueue)) {
        log_printf("vehicle pool: DISABLED, VirtualProtect failed");
        return 0;
    }
    log_printf("vehicle pool: %d workers, enqueue hooked at %p", g_cfg.vehicle_threads, enq);
    return 1;
}
