/* Parallel children inside ONE compound collision pair.
 *
 * The parallel narrowphase splits the pairs of a pass between threads, but a single pair stays on
 * one thread. A big vehicle (btCompoundShape with thousands of children) lying on a terrain mesh
 * is one pair: on 2026-11-14 a 36.6 m compound against a 370 m triangle mesh cost 9.6-14 ms per
 * step while the other threads waited (85 % of the pass).
 *
 * btCompoundCollisionAlgorithm::processCollision (vtable[1], FUN_1400e30a0 in the 2026-10-29
 * build) walks the compound's AABB tree and calls btCompoundLeafCallback::ProcessChildShape
 * (FUN_1400e2960) for every child overlapping the other object. Children are independent: each
 * has its own slot in m_childCollisionAlgorithms and its own algorithm and manifold (the
 * callback's m_sharedManifold is NULL for a top-level pair); allocations go through the
 * dispatcher hooks that the parallel narrowphase already serializes. What they share is the
 * btManifoldResult, whose body wrappers ProcessChildShape swaps while a child runs.
 *
 * So: while processCollision runs, ProcessChildShape only records (child shape, index); when it
 * returns, the children are processed by this thread and by narrowphase workers that are idle
 * (they would otherwise wait for this pair), each with its own copy of the callback and of the
 * btManifoldResult. Fewer than MIN_CHILDREN children, a shared manifold, an unexpected result
 * class or a nested compound: the original order, on this thread. The only reordering is that
 * the algorithm's end-of-call cleanup (algorithms of children that no longer overlap) now runs
 * before the children instead of after; a child freed there gets a new algorithm when processed.
 *
 * Layouts (2026-10-29 build, ProcessChildShape):
 *   btCompoundLeafCallback (no vtable, 0x38): +0x00 compound wrapper, +0x08 other wrapper,
 *     +0x10 dispatcher, +0x18 dispatch info, +0x20 btManifoldResult*, +0x28 child algorithms,
 *     +0x30 shared manifold
 *   btManifoldResult (0x38): vtable, manifold, body0 wrapper, body1 wrapper, part/index ids,
 *     closest point threshold */
#include "stwpatch.h"
#include <stdio.h>
#include <string.h>

#define CB_SIZE        0x38
#define CB_RESULT      0x20
#define CB_SHARED_MF   0x30
#define RESULT_SIZE    0x38
#define MIN_CHILDREN   48
#define CHUNK_CHILDREN 8
#define MAX_JOBS       8

typedef void (*process_t)(void *algo, void *w0, void *w1, void *info, void *result);
typedef void (*child_t)(uint8_t *cb, void *shape, int index);

static process_t orig_process;
static child_t   orig_child;              /* trampoline to the original ProcessChildShape */
static void     *vt_result;               /* btManifoldResult vtable */
static DWORD     tls = TLS_OUT_OF_INDEXES;

typedef struct { void *shape; int index; } item_t;

/* per thread: what the current processCollision deferred */
typedef struct {
    int state;                            /* 0 idle, 1 collecting, 2 running children (no deferral) */
    int have_cb, n, cap;
    uint8_t cb[CB_SIZE];
    item_t *items;
} defer_t;

/* a pair's children offered to idle narrowphase workers */
typedef struct {
    volatile LONG busy;                   /* slot owned by a pair */
    volatile LONG open;                   /* helpers may join */
    volatile LONG next, done, inside;
    int n;
    const item_t *items;
    const uint8_t *cb;
} job_t;
static job_t jobs[MAX_JOBS];

static volatile LONG64 stat_pairs, stat_children, stat_helped;

static defer_t *defer_get(int create)
{
    if (tls == TLS_OUT_OF_INDEXES) return NULL;
    defer_t *d = TlsGetValue(tls);
    if (!d && create) {
        d = VirtualAlloc(NULL, sizeof *d, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (d) TlsSetValue(tls, d);
    }
    return d;
}

static int push(defer_t *d, void *shape, int index)
{
    if (d->n == d->cap) {
        int cap = d->cap ? d->cap * 2 : 1024;
        item_t *p = VirtualAlloc(NULL, (size_t)cap * sizeof *p, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return 0;
        if (d->items) {
            memcpy(p, d->items, (size_t)d->n * sizeof *p);
            VirtualFree(d->items, 0, MEM_RELEASE);
        }
        d->items = p;
        d->cap = cap;
    }
    d->items[d->n].shape = shape;
    d->items[d->n].index = index;
    d->n++;
    return 1;
}

/* a child run right away while collecting: a compound nested in it must not defer its own
 * children past it (their wrappers live in this child's stack frame) */
static void run_now(defer_t *d, uint8_t *cb, void *shape, int index)
{
    int saved = d->state;
    d->state = 2;
    orig_child(cb, shape, index);
    d->state = saved;
}

/* ProcessChildShape: record instead of running while our processCollision collects */
static void hk_child(uint8_t *cb, void *shape, int index)
{
    defer_t *d = defer_get(0);
    if (!d || d->state != 1) { orig_child(cb, shape, index); return; }
    if (*(void **)(cb + CB_SHARED_MF)) { run_now(d, cb, shape, index); return; }   /* one manifold for all */
    if (!d->have_cb) {
        memcpy(d->cb, cb, CB_SIZE);
        d->have_cb = 1;
    } else if (memcmp(d->cb, cb, CB_SIZE) != 0) {
        run_now(d, cb, shape, index);       /* another callback: not ours to defer */
        return;
    }
    if (!push(d, shape, index)) run_now(d, cb, shape, index);
}

/* take chunks of a job with private copies of the callback and the btManifoldResult */
static void work_on(job_t *j)
{
    uint8_t cb[CB_SIZE];
    _Alignas(16) uint8_t res[RESULT_SIZE];
    memcpy(cb, j->cb, CB_SIZE);
    memcpy(res, *(uint8_t **)(cb + CB_RESULT), RESULT_SIZE);
    *(uint8_t **)(cb + CB_RESULT) = res;
    defer_t *d = defer_get(1);
    int saved = d ? d->state : 0;
    if (d) d->state = 2;
    for (;;) {
        LONG i = InterlockedExchangeAdd(&j->next, CHUNK_CHILDREN);
        if (i >= j->n) break;
        LONG end = i + CHUNK_CHILDREN < j->n ? i + CHUNK_CHILDREN : j->n;
        for (LONG k = i; k < end; k++) orig_child(cb, j->items[k].shape, j->items[k].index);
        InterlockedExchangeAdd(&j->done, end - i);
    }
    if (d) d->state = saved;
}

/* narrowphase workers with nothing left to do in the pass: help a pair's children */
int compound_assist(void)
{
    int helped = 0;
    for (int s = 0; s < MAX_JOBS; s++) {
        job_t *j = &jobs[s];
        if (!j->open || j->next >= j->n) continue;
        InterlockedIncrement(&j->inside);
        if (j->open && j->next < j->n) {
            work_on(j);
            helped = 1;
            InterlockedIncrement64(&stat_helped);
        }
        InterlockedDecrement(&j->inside);
    }
    return helped;
}

static void run_serial(defer_t *d)
{
    d->state = 2;
    for (int k = 0; k < d->n; k++) orig_child(d->cb, d->items[k].shape, d->items[k].index);
    d->state = 0;
}

static void hk_process(void *algo, void *w0, void *w1, void *info, void *result)
{
    defer_t *d = g_cfg.compound_split ? defer_get(1) : NULL;   /* switched live from the F4 menu */
    if (!d || d->state != 0 || !result || *(void **)result != vt_result) {
        orig_process(algo, w0, w1, info, result);
        return;
    }
    d->state = 1;
    d->n = 0;
    d->have_cb = 0;
    orig_process(algo, w0, w1, info, result);
    d->state = 0;
    if (!d->n) return;
    if (d->n < MIN_CHILDREN) { run_serial(d); return; }

    job_t *j = NULL;
    for (int s = 0; s < MAX_JOBS && !j; s++)
        if (!jobs[s].busy && InterlockedCompareExchange(&jobs[s].busy, 1, 0) == 0) j = &jobs[s];
    if (!j) { run_serial(d); return; }
    j->items = d->items;
    j->n = d->n;
    j->cb = d->cb;
    j->next = 0;
    j->done = 0;
    MemoryBarrier();
    InterlockedExchange(&j->open, 1);
    work_on(j);                               /* the owner works too */
    while (j->done < j->n) YieldProcessor();  /* helpers finishing their chunks */
    InterlockedExchange(&j->open, 0);
    while (j->inside) YieldProcessor();       /* nobody reads items/cb any more */
    InterlockedIncrement64(&stat_pairs);
    InterlockedAdd64(&stat_children, d->n);
    InterlockedExchange(&j->busy, 0);
}

void compound_format(char *buf, size_t size)
{
    snprintf(buf, size, "%lld pairs split, %lld children, %lld helps (total)",
             (long long)stat_pairs, (long long)stat_children, (long long)stat_helped);
}

/* ProcessChildShape is a function called both by processCollision and by the AABB-tree walk
 * that processCollision calls (btDbvt::collideTVNoStackAlloc, with the callback inlined), whose
 * entry is the expected prologue (other common callees: __security_check_cookie, memset).
 * MSVC splits big functions into several .pdata fragments chained to the first one: every
 * fragment is scanned (the tree walk call is in a later fragment of processCollision). */

/* push rbp (REX form); push rbx; push r12; push r15; lea rbp,[rsp-258h]: 15 bytes, no rip-relative */
static const uint8_t child_prologue[15] = { 0x40, 0x55, 0x53, 0x41, 0x54, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24,
                                            0xA8, 0xFD, 0xFF, 0xFF };

static RUNTIME_FUNCTION *pdata;
static DWORD n_pdata;

static DWORD primary_of(const RUNTIME_FUNCTION *f)
{
    for (int guard = 0; guard < 32; guard++) {
        const uint8_t *ui = g_base + (f->UnwindData & ~1u);
        if (!((ui[0] >> 3) & 4)) return f->BeginAddress;          /* not UNW_FLAG_CHAININFO */
        f = (const RUNTIME_FUNCTION *)(ui + 4 + ((ui[2] + 1) & ~1) * 2);
    }
    return 0;
}

static int is_function_start(DWORD rva)
{
    DWORD lo = 0, hi = n_pdata;                 /* .pdata is sorted by BeginAddress */
    while (lo < hi) {
        DWORD m = (lo + hi) / 2;
        if (pdata[m].BeginAddress < rva) lo = m + 1; else hi = m;
    }
    return lo < n_pdata && pdata[lo].BeginAddress == rva && primary_of(&pdata[lo]) == rva;
}

/* direct call targets (function starts) in every fragment of the function at rva */
static int callees_of(DWORD rva, DWORD *out, int max)
{
    int n = 0;
    for (DWORD i = 0; i < n_pdata; i++) {
        if (primary_of(&pdata[i]) != rva) continue;
        for (DWORD p = pdata[i].BeginAddress; p + 5 <= pdata[i].EndAddress; p++) {
            if (g_base[p] != 0xE8) continue;
            DWORD t = p + 5 + *(int32_t *)(g_base + p + 1);
            if (n < max && is_function_start(t)) out[n++] = t;
        }
    }
    return n;
}

static uint8_t *find_child_fn(uint8_t *process)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    pdata = (RUNTIME_FUNCTION *)(g_base + dir.VirtualAddress);
    n_pdata = dir.Size / sizeof *pdata;
    DWORD calls[128], inner[128];
    int n = callees_of((DWORD)(process - g_base), calls, 128);
    uint8_t *found = NULL;
    for (int a = 0; a < n; a++) {
        int m = callees_of(calls[a], inner, 128);
        for (int k = 0; k < m; k++)
            for (int b = 0; b < n; b++) {
                if (b == a || calls[b] != inner[k]) continue;
                uint8_t *t = g_base + inner[k];
                if (memcmp(t, child_prologue, sizeof child_prologue) != 0) continue;
                if (found && found != t) return NULL;               /* ambiguous */
                found = t;
            }
    }
    return found;
}

int compound_split_install(void)
{
    void **vt = (void **)g_addr.vt_compound_algo;
    vt_result = g_addr.vt_manifold_result;
    if (!vt || !vt_result) {
        log_printf("compound split: DISABLED, Bullet compound algorithm not found in this build");
        return 0;
    }
    uint8_t *child = find_child_fn(vt[1]);
    if (!child) {
        log_printf("compound split: DISABLED, ProcessChildShape not recognised in this build");
        return 0;
    }
    tls = TlsAlloc();
    if (tls == TLS_OUT_OF_INDEXES) return 0;
    orig_child = (child_t)make_trampoline(child, sizeof child_prologue, child_prologue);
    if (!orig_child) return 0;
    patch_jump(child, sizeof child_prologue, hk_child);
    orig_process = (process_t)patch_ptr(&vt[1], hk_process);
    g_addr.compound_split_on = 1;
    log_printf("compound split: on (processCollision rva 0x%llx, ProcessChildShape rva 0x%llx, from %d children)",
               (unsigned long long)((uint8_t *)orig_process - g_base), (unsigned long long)(child - g_base), MIN_CHILDREN);
    return 1;
}
