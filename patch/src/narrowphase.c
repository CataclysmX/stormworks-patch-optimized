/* Parallel Bullet narrowphase.
 *
 * btCollisionDispatcher::dispatchAllCollisionPairs (vtable slot 7) walks every overlapping pair
 * of the btHashedOverlappingPairCache and runs the near callback (contact generation) one pair at
 * a time on the physics thread. Pairs are independent: each owns its collision algorithm and
 * manifold, the GJK simplex solver is local to processCollision, EPA is stateless and the only
 * contact callback the game installs (gContactAddedCallback = FUN_1401a35c0) is pure.
 * What is shared, and therefore serialized here:
 *   - dispatcher pools and manifold list: getNewManifold (2), releaseManifold (3),
 *     allocateCollisionAlgorithm (13), freeCollisionAlgorithm (14) go through one lock. Mesh pairs
 *     allocate and free a temporary algorithm per triangle, so during a parallel pass each thread
 *     keeps pool blocks in a small private cache and returns them at the end of the pass.
 *   - Bullet's profiler assigns per-thread indices with an unsynchronized counter: workers take
 *     theirs once, under a lock, before their first pass.
 *   - pairs involving a soft body (ropes) write into the soft body: they are run serially after.
 * Only the discrete dispatch mode with the default near callback is parallelized; anything else
 * falls back to the original function. Contact order changes run to run (not deterministic). */
#include "stwpatch.h"
#include <intrin.h>
#include <string.h>
#include <stdio.h>

#define DISP_NEAR_CALLBACK  0x30
#define DISP_ALGO_POOL      0x38
#define POOL_ELEM_SIZE      0x00
#define POOL_MAX_ELEMS      0x04
#define POOL_BASE           0x18
#define PC_NUM_PAIRS        0x0c
#define PC_PAIRS            0x18
#define PAIR_STRIDE         0x20
#define CO_INTERNAL_TYPE    0x1f0
#define CO_SOFT_BODY        8
#define INFO_DISPATCH_FUNC  0x0c
#define DISPATCH_DISCRETE   1

#define CHUNK       8
#define CACHE_MAX   64

typedef void  (*dispatch_t)(void *disp, uint8_t *pair_cache, uint8_t *info, void *disp2);
typedef void  (*near_t)(uint8_t *pair, void *disp, uint8_t *info);
typedef void *(*new_manifold_t)(void *disp, void *b0, void *b1);
typedef void  (*release_manifold_t)(void *disp, void *m);
typedef void *(*alloc_algo_t)(void *disp, int size);
typedef void  (*free_algo_t)(void *disp, void *p);
typedef void  (*prof_start_t)(const char *name);
typedef void  (*prof_stop_t)(void);

static dispatch_t         orig_dispatch;
static new_manifold_t     orig_new_manifold;
static release_manifold_t orig_release_manifold;
static alloc_algo_t       orig_alloc;
static free_algo_t        orig_free;
static prof_start_t       prof_start;
static prof_stop_t        prof_stop;
static void              *vt_hashed_cache, *default_near;

static CRITICAL_SECTION pool_cs;
static SRWLOCK          prof_lock = SRWLOCK_INIT;
static DWORD            tls_index;

typedef struct {
    int active;              /* inside a parallel pass */
    int profiled;            /* Bullet profiler thread index taken */
    void *disp;
    int n;
    void *blocks[CACHE_MAX];
} thread_ctx;

/* current job */
static struct {
    uint8_t *pairs;
    int count;
    void *disp;
    uint8_t *info;
    near_t near_cb;
} job;
static volatile LONG job_next, job_open, in_flight, job_gen;
static SRWLOCK            wake_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE wake_cv = CONDITION_VARIABLE_INIT;
static SRWLOCK            job_lock = SRWLOCK_INIT;   /* one parallel pass at a time */

static volatile LONG64 stat_parallel, stat_serial, stat_pairs;
static volatile LONG64 stat_pass_ticks, stat_tail_ticks;   /* QPC ticks: whole pass, waiting for the last workers */

static thread_ctx *ctx(void) { return TlsGetValue(tls_index); }

static inline int in_pool(void *disp, void *p)
{
    uint8_t *pool = *(uint8_t **)((uint8_t *)disp + DISP_ALGO_POOL);
    uint8_t *base = *(uint8_t **)(pool + POOL_BASE);
    size_t span = (size_t)*(int *)(pool + POOL_ELEM_SIZE) * (size_t)*(int *)(pool + POOL_MAX_ELEMS);
    return (uint8_t *)p >= base && (uint8_t *)p < base + span;
}

static void *hk_new_manifold(void *disp, void *b0, void *b1)
{
    EnterCriticalSection(&pool_cs);
    void *m = orig_new_manifold(disp, b0, b1);
    LeaveCriticalSection(&pool_cs);
    return m;
}

static void hk_release_manifold(void *disp, void *m)
{
    EnterCriticalSection(&pool_cs);
    orig_release_manifold(disp, m);
    LeaveCriticalSection(&pool_cs);
}

static void *hk_alloc(void *disp, int size)
{
    thread_ctx *t = ctx();
    if (t && t->active && t->n && t->disp == disp) {
        uint8_t *pool = *(uint8_t **)((uint8_t *)disp + DISP_ALGO_POOL);
        if (size <= *(int *)(pool + POOL_ELEM_SIZE)) return t->blocks[--t->n];
    }
    EnterCriticalSection(&pool_cs);
    void *p = orig_alloc(disp, size);
    LeaveCriticalSection(&pool_cs);
    return p;
}

static void hk_free(void *disp, void *p)
{
    if (!p) return;
    thread_ctx *t = ctx();
    if (t && t->active && t->n < CACHE_MAX && (t->n == 0 || t->disp == disp) && in_pool(disp, p)) {
        t->disp = disp;
        t->blocks[t->n++] = p;
        return;
    }
    EnterCriticalSection(&pool_cs);
    orig_free(disp, p);
    LeaveCriticalSection(&pool_cs);
}

static void cache_flush(thread_ctx *t)
{
    if (!t->n) return;
    EnterCriticalSection(&pool_cs);
    while (t->n) orig_free(t->disp, t->blocks[--t->n]);
    LeaveCriticalSection(&pool_cs);
}

static inline int pair_has_soft_body(uint8_t *pair)
{
    uint8_t *p0 = *(uint8_t **)pair, *p1 = *(uint8_t **)(pair + 8);
    uint8_t *c0 = p0 ? *(uint8_t **)p0 : NULL, *c1 = p1 ? *(uint8_t **)p1 : NULL;
    return (c0 && (*(int *)(c0 + CO_INTERNAL_TYPE) & CO_SOFT_BODY)) ||
           (c1 && (*(int *)(c1 + CO_INTERNAL_TYPE) & CO_SOFT_BODY));
}

/* Load balancing. Pair costs are very uneven (a big vehicle against terrain or another big
 * vehicle costs thousands of small pairs) and stable from one step to the next. Each pass times
 * every pair with rdtsc; the next pass hands out the HEAVY_MAX heaviest pairs of the previous one
 * first, one at a time, then the rest in chunks, so no heavy pair is left alone at the end.
 * A pair is recognised across passes by its slot and its persistent collision algorithm. */
#define HEAVY_MAX 48
static uint64_t *pair_cost;          /* cycles, indexed like the pair array, rewritten each pass */
static uint32_t *pair_mark;          /* == pass_id: already handed out from the heavy list */
static int       cost_cap;
static uint32_t  pass_id;
static int       heavy_idx[HEAVY_MAX], n_heavy;
static void     *heavy_algo[HEAVY_MAX];
static volatile LONG heavy_next;
static volatile LONG64 stat_max_pair_cycles, stat_heavy_hits;
static double    cycles_per_ms;

static inline void run_pair(LONG i)
{
    uint8_t *pair = job.pairs + (size_t)i * PAIR_STRIDE;
    uint64_t t0 = __rdtsc();
    if (!pair_has_soft_body(pair)) job.near_cb(pair, job.disp, job.info);
    pair_cost[i] = __rdtsc() - t0;
}

static void run_chunks(void)
{
    for (;;) {
        LONG h = InterlockedIncrement(&heavy_next) - 1;
        if (h >= n_heavy) break;
        run_pair(heavy_idx[h]);
    }
    for (;;) {
        LONG i = InterlockedExchangeAdd(&job_next, CHUNK);
        if (i >= job.count) return;
        LONG end = i + CHUNK < job.count ? i + CHUNK : job.count;
        for (; i < end; i++)
            if (pair_mark[i] != pass_id) run_pair(i);
    }
}

/* physics thread, before waking the workers: map last pass's heavy pairs onto this pass */
static void plan_heavy(void)
{
    if (job.count > cost_cap) {
        int cap = job.count * 2;
        uint64_t *c = VirtualAlloc(NULL, (size_t)cap * sizeof *c, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        uint32_t *m = VirtualAlloc(NULL, (size_t)cap * sizeof *m, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!c || !m) { n_heavy = 0; heavy_next = 0; return; }
        if (pair_cost) { VirtualFree(pair_cost, 0, MEM_RELEASE); VirtualFree(pair_mark, 0, MEM_RELEASE); }
        pair_cost = c; pair_mark = m; cost_cap = cap;
        n_heavy = 0;                       /* indices from before the resize are meaningless */
    }
    pass_id++;
    int kept = 0;
    for (int k = 0; k < n_heavy; k++) {
        int i = heavy_idx[k];
        void *algo = i < job.count ? *(void **)(job.pairs + (size_t)i * PAIR_STRIDE + 0x10) : NULL;
        if (!algo || algo != heavy_algo[k]) continue;     /* pair array changed under that slot */
        heavy_idx[kept] = i; heavy_algo[kept] = algo; kept++;
        pair_mark[i] = pass_id;
    }
    n_heavy = kept;
    InterlockedAdd64(&stat_heavy_hits, kept);
    heavy_next = 0;
}

/* physics thread, after the pass: remember the heaviest pairs for the next one */
static void learn_heavy(void)
{
    int idx[HEAVY_MAX], n = 0;
    uint64_t lo = 0;
    for (int i = 0; i < job.count; i++) {
        uint64_t c = pair_cost[i];
        if (n == HEAVY_MAX && c <= lo) continue;
        int p = n < HEAVY_MAX ? n++ : HEAVY_MAX - 1;   /* sorted descending: drop the lightest */
        while (p > 0 && pair_cost[idx[p - 1]] < c) { idx[p] = idx[p - 1]; p--; }
        idx[p] = i;
        lo = pair_cost[idx[n - 1]];
    }
    for (int k = 0; k < n; k++) {
        heavy_idx[k] = idx[k];
        heavy_algo[k] = *(void **)(job.pairs + (size_t)idx[k] * PAIR_STRIDE + 0x10);
    }
    n_heavy = g_cfg.narrow_balance ? n : 0;
    if (n) InterlockedAdd64(&stat_max_pair_cycles, (LONG64)pair_cost[idx[0]]);
}

static thread_ctx *thread_ctx_init(void)
{
    thread_ctx *t = ctx();
    if (!t) {
        t = VirtualAlloc(NULL, sizeof *t, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        TlsSetValue(tls_index, t);
    }
    if (!t->profiled) {
        /* take a Bullet profiler thread index without racing other threads */
        AcquireSRWLockExclusive(&prof_lock);
        prof_start_t start = g_addr.prof_start_ptr ? (prof_start_t)*g_addr.prof_start_ptr : prof_start;
        prof_stop_t stop = g_addr.prof_stop_ptr ? (prof_stop_t)*g_addr.prof_stop_ptr : prof_stop;
        if (start && stop) { start("stwpatch narrowphase worker"); stop(); }
        ReleaseSRWLockExclusive(&prof_lock);
        t->profiled = 1;
    }
    return t;
}

static DWORD WINAPI worker(LPVOID arg)
{
    typedef HRESULT (WINAPI *std_t)(HANDLE, PCWSTR);
    std_t sd = (std_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    wchar_t name[32] = L"stw-collide-";
    name[12] = L'0' + (int)(intptr_t)arg % 10;
    if (sd) sd(GetCurrentThread(), name);

    thread_ctx *t = NULL;
    LONG seen = 0;
    for (;;) {
        AcquireSRWLockExclusive(&wake_lock);
        while (job_gen == seen) SleepConditionVariableSRW(&wake_cv, &wake_lock, INFINITE, 0);
        seen = job_gen;
        ReleaseSRWLockExclusive(&wake_lock);

        if (!t) t = thread_ctx_init();
        InterlockedIncrement(&in_flight);
        if (InterlockedCompareExchange(&job_open, 1, 1)) {
            t->active = 1;
            run_chunks();
            t->active = 0;
            cache_flush(t);
        }
        InterlockedDecrement(&in_flight);
    }
    return 0;
}

/* the game installs its own near callback, which is a bare "jmp defaultNearCallback" */
static int is_default_near(uint8_t *f)
{
    if (f == default_near) return 1;
    return f && f[0] == 0xE9 && f + 5 + *(int32_t *)(f + 1) == (uint8_t *)default_near;
}

enum { R_SMALL, R_CACHE, R_NEAR, R_MODE, R_BUSY, R_COUNT };
static volatile LONG64 stat_reason[R_COUNT];
static volatile LONG logged_reason[R_COUNT];
static const char *reason_name[R_COUNT] = { "few pairs", "pair cache type", "near callback", "dispatch mode", "busy" };

static void reject(int r, void *cache_vt, void *near_cb, int mode, int count)
{
    InterlockedIncrement64(&stat_reason[r]);
    if (r != R_SMALL && r != R_BUSY && !InterlockedExchange(&logged_reason[r], 1))
        log_printf("narrowphase: serial because of %s (pairs=%d cache vtable rva=0x%llx near rva=0x%llx mode=%d)",
                   reason_name[r], count, (unsigned long long)((uint8_t *)cache_vt - g_base),
                   (unsigned long long)((uint8_t *)near_cb - g_base), mode);
}

static void hk_dispatch(void *disp, uint8_t *pair_cache, uint8_t *info, void *disp2)
{
    int count = pair_cache ? *(int *)(pair_cache + PC_NUM_PAIRS) : 0;
    near_t near_cb = *(near_t *)((uint8_t *)disp + DISP_NEAR_CALLBACK);
    void *cache_vt = pair_cache ? *(void **)pair_cache : NULL;
    int mode = *(int *)(info + INFO_DISPATCH_FUNC);
    int r = count < g_cfg.narrow_min_pairs ? R_SMALL
          : cache_vt != vt_hashed_cache ? R_CACHE
          : !is_default_near((uint8_t *)near_cb) ? R_NEAR
          : mode != DISPATCH_DISCRETE ? R_MODE
          : !TryAcquireSRWLockExclusive(&job_lock) ? R_BUSY : -1;
    if (r >= 0) {
        reject(r, cache_vt, (void *)near_cb, mode, count);
        InterlockedIncrement64(&stat_serial);
        orig_dispatch(disp, pair_cache, info, disp2);
        return;
    }
    thread_ctx *t = thread_ctx_init();
    job.pairs = *(uint8_t **)(pair_cache + PC_PAIRS);
    job.count = count;
    plan_heavy();
    if (!pair_cost) {                       /* allocation failed: serial */
        ReleaseSRWLockExclusive(&job_lock);
        InterlockedIncrement64(&stat_serial);
        orig_dispatch(disp, pair_cache, info, disp2);
        return;
    }
    job.disp = disp;
    job.info = info;
    job.near_cb = near_cb;
    InterlockedExchange(&job_next, 0);
    InterlockedExchange(&job_open, 1);
    AcquireSRWLockExclusive(&wake_lock);
    InterlockedIncrement(&job_gen);
    ReleaseSRWLockExclusive(&wake_lock);
    WakeAllConditionVariable(&wake_cv);

    LARGE_INTEGER q0, q1, q2;
    QueryPerformanceCounter(&q0);
    t->active = 1;
    run_chunks();
    t->active = 0;
    InterlockedExchange(&job_open, 0);
    QueryPerformanceCounter(&q1);
    while (InterlockedCompareExchange(&in_flight, 0, 0) != 0) YieldProcessor();
    QueryPerformanceCounter(&q2);
    InterlockedAdd64(&stat_pass_ticks, q2.QuadPart - q0.QuadPart);
    InterlockedAdd64(&stat_tail_ticks, q2.QuadPart - q1.QuadPart);
    cache_flush(t);
    learn_heavy();

    /* soft-body pairs, serially, in their original order */
    for (int i = 0; i < count; i++) {
        uint8_t *pair = job.pairs + (size_t)i * PAIR_STRIDE;
        if (pair_has_soft_body(pair)) near_cb(pair, disp, info);
    }
    ReleaseSRWLockExclusive(&job_lock);
    InterlockedIncrement64(&stat_parallel);
    InterlockedAdd64(&stat_pairs, count);
}

/* Called once per report period: the counters it prints are deltas since the previous call. */
void narrowphase_format(char *buf, size_t size)
{
    static LONG64 last_par, last_ser, last_pairs, last_pass, last_tail, last_max, last_hits;
    LONG64 par = stat_parallel - last_par, ser = stat_serial - last_ser, pairs = stat_pairs - last_pairs;
    LONG64 pass = stat_pass_ticks - last_pass, tail = stat_tail_ticks - last_tail;
    LONG64 maxc = stat_max_pair_cycles - last_max, hits = stat_heavy_hits - last_hits;
    last_par = stat_parallel; last_ser = stat_serial; last_pairs = stat_pairs;
    last_pass = stat_pass_ticks; last_tail = stat_tail_ticks; last_max = stat_max_pair_cycles; last_hits = stat_heavy_hits;
    if (!g_cfg.narrow_threads) { snprintf(buf, size, "off"); return; }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    snprintf(buf, size, "%lld parallel passes (avg %.0f pairs, %.2f ms, of which %.0f%% waiting for the "
             "last worker; heaviest pair %.2f ms, %.0f heavy pairs scheduled first), %lld serial "
             "[total: few pairs %lld, cache %lld, near cb %lld, mode %lld, busy %lld]",
             (long long)par, par ? (double)pairs / par : 0.0, par ? pass * 1000.0 / f.QuadPart / par : 0.0,
             pass ? 100.0 * tail / pass : 0.0, par && cycles_per_ms ? maxc / cycles_per_ms / par : 0.0,
             par ? (double)hits / par : 0.0, (long long)ser,
             (long long)stat_reason[R_SMALL], (long long)stat_reason[R_CACHE], (long long)stat_reason[R_NEAR],
             (long long)stat_reason[R_MODE], (long long)stat_reason[R_BUSY]);
}

static DWORD WINAPI stats_thread(LPVOID p)
{
    (void)p;
    for (;;) {
        Sleep(30000);
        LONG64 par = stat_parallel, ser = stat_serial;
        log_printf("narrowphase: %lld parallel passes (avg %.0f pairs), %lld serial passes "
                   "[few pairs %lld, cache %lld, near cb %lld, mode %lld, busy %lld]",
                   (long long)par, par ? (double)stat_pairs / par : 0.0, (long long)ser,
                   (long long)stat_reason[R_SMALL], (long long)stat_reason[R_CACHE],
                   (long long)stat_reason[R_NEAR], (long long)stat_reason[R_MODE], (long long)stat_reason[R_BUSY]);
    }
    return 0;
}

int narrowphase_install(void)
{
    void **vt = (void **)g_addr.vt_dispatcher;
    if (!vt || !g_addr.vt_hashed_cache || !g_addr.default_near) {
        log_printf("narrowphase: DISABLED, Bullet dispatcher not found in this build");
        return 0;
    }
    /* algorithm pool at dispatcher+38h: allocateCollisionAlgorithm starts with mov r?,[rcx+38h] */
    uint8_t *alloc_fn = vt[13], *free_fn = vt[14];
    if (!(alloc_fn[0] & 0x40) || alloc_fn[1] != 0x8B || alloc_fn[3] != 0x38 ||
        !(free_fn[0] & 0x40) || free_fn[1] != 0x8B || free_fn[3] != 0x38) {
        log_printf("narrowphase: DISABLED, dispatcher layout differs from the analysed builds");
        return 0;
    }
    /* rdtsc rate, for the "heaviest pair" figure only */
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    uint64_t c0 = __rdtsc();
    Sleep(20);
    uint64_t c1 = __rdtsc();
    QueryPerformanceCounter(&q1);
    cycles_per_ms = (double)(c1 - c0) / ((double)(q1.QuadPart - q0.QuadPart) * 1000.0 / f.QuadPart);
    tls_index = TlsAlloc();
    if (tls_index == TLS_OUT_OF_INDEXES) return 0;
    InitializeCriticalSectionAndSpinCount(&pool_cs, 4000);
    vt_hashed_cache = g_addr.vt_hashed_cache;
    default_near    = g_addr.default_near;
    prof_start      = (prof_start_t)g_addr.prof_start;
    prof_stop       = (prof_stop_t)g_addr.prof_stop;

    for (int i = 0; i < g_cfg.narrow_threads; i++) {
        HANDLE h = CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)i, 0, NULL);
        if (!h) continue;
        SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(h);
    }
    HANDLE h = CreateThread(NULL, 0, stats_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);

    /* hook the shared-state slots before the dispatch slot that relies on them */
    orig_new_manifold     = patch_ptr(&vt[2], hk_new_manifold);
    orig_release_manifold = patch_ptr(&vt[3], hk_release_manifold);
    orig_alloc            = patch_ptr(&vt[13], hk_alloc);
    orig_free             = patch_ptr(&vt[14], hk_free);
    orig_dispatch         = patch_ptr(&vt[7], hk_dispatch);
    log_printf("narrowphase: %d workers + physics thread, parallel from %d pairs",
               g_cfg.narrow_threads, g_cfg.narrow_min_pairs);
    return 1;
}
