/* Stress test of compound.c: a fake btCompoundCollisionAlgorithm::processCollision walks N
 * children through hk_child (as the patched ProcessChildShape entry would), the fake
 * ProcessChildShape swaps the btManifoldResult wrappers like Bullet does, helper threads call
 * compound_assist() like idle narrowphase workers. Checks: every child exactly once per call,
 * private result copies in parallel runs, the caller's result untouched, the inline cases
 * (few children, shared manifold, nested compound).   make tests && wine build/compound_test.exe */
#include "../src/compound.c"
#include <stdio.h>
#include <stdlib.h>

stw_config g_cfg;
game_addrs g_addr;
uint8_t *g_base;
void log_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
void *make_trampoline(uint8_t *t, size_t s, const uint8_t *e) { (void)t; (void)s; (void)e; return NULL; }
int patch_jump(uint8_t *at, size_t len, void *to) { (void)at; (void)len; (void)to; return 0; }
void *patch_ptr(void **slot, void *value) { void *p = *slot; *slot = value; return p; }

typedef struct { void *vt; void *manifold; void *body0, *body1; int ids[4]; double threshold; } result_t;
static void *fake_result_vt[4];
static char outer_w0, outer_w1, compound_obj;   /* wrappers the near callback passes */

#define MAX_N 6000
static volatile LONG processed[MAX_N], nested_processed[64];
static volatile LONG wrong_result, bad_restore;
static volatile LONG64 on_helpers;
static DWORD owner_tid;
static result_t *expected_result;              /* the caller's result, for serial runs */
static int expect_copy;

static void spin(int n) { volatile int x = 0; for (int i = 0; i < n; i++) x = x + i; (void)x; }

static void fake_process(void *algo, void *w0, void *w1, void *info, void *result);

/* like btCompoundLeafCallback::ProcessChildShape: swaps body0Wrap while the child runs */
static void fake_child(uint8_t *cb, void *shape, int index)
{
    result_t *r = *(result_t **)(cb + CB_RESULT);
    if (shape == (void *)2) {                       /* a child of a nested compound */
        InterlockedIncrement(&nested_processed[index]);
        return;
    }
    if (expect_copy ? r == expected_result : r != expected_result) InterlockedIncrement(&wrong_result);
    void *saved = r->body0;
    char local;
    r->body0 = &local;
    spin(300 + (index % 7) * 200);
    if (index == 5 && shape == (void *)1) fake_process(NULL, NULL, NULL, NULL, r);   /* nested compound */
    if (r->body0 != &local) InterlockedIncrement(&bad_restore);
    r->body0 = saved;
    InterlockedIncrement(&processed[index]);
    if (GetCurrentThreadId() != owner_tid) InterlockedIncrement64(&on_helpers);
}

static int walk_n;
static void *walk_shared;
/* like processCollision: the tree walk calls ProcessChildShape for every overlapping child */
static void fake_process(void *algo, void *w0, void *w1, void *info, void *result)
{
    (void)w0; (void)w1; (void)info;
    uint8_t cb[CB_SIZE];
    memset(cb, 0, sizeof cb);
    *(void **)(cb + 0x00) = &compound_obj;
    *(void **)(cb + CB_RESULT) = result;
    *(void **)(cb + CB_SHARED_MF) = algo ? walk_shared : NULL;
    if (!algo) {                                     /* nested: 64 children, must run inline */
        for (int i = 0; i < 64; i++) hk_child(cb, (void *)2, i);
        return;
    }
    for (int i = 0; i < walk_n; i++) hk_child(cb, (void *)1, i);
}

static volatile LONG stop;
static DWORD WINAPI helper(LPVOID p)
{
    (void)p;
    while (!stop) if (!compound_assist()) YieldProcessor();
    return 0;
}

static int fails;
static void run(const char *what, int n, void *shared, int expect_parallel, int iterations)
{
    long long helped0 = on_helpers;
    walk_n = n;
    walk_shared = shared;
    for (int it = 0; it < iterations; it++) {
        memset((void *)processed, 0, sizeof processed);
        memset((void *)nested_processed, 0, sizeof nested_processed);
        result_t r = { fake_result_vt, NULL, &outer_w0, &outer_w1, { 0 }, 0 };
        expected_result = &r;
        expect_copy = expect_parallel;
        hk_process((void *)1, NULL, NULL, NULL, &r);
        for (int i = 0; i < n; i++)
            if (processed[i] != 1) { printf("  %s: child %d processed %ld times\n", what, i, processed[i]); fails++; break; }
        if (n > 5) for (int i = 0; i < 64; i++)
            if (nested_processed[i] != 1) { printf("  %s: nested child %d processed %ld times\n", what, i, nested_processed[i]); fails++; break; }
        if (r.body0 != &outer_w0 || r.body1 != &outer_w1) { printf("  %s: caller's result changed\n", what); fails++; }
    }
    printf("%-44s %s (children on helper threads: %lld)\n", what, fails ? "FAILED so far" : "ok", (long long)(on_helpers - helped0));
    if (expect_parallel && on_helpers == helped0) { printf("  %s: helpers never ran\n", what); fails++; }
}

int main(void)
{
    g_cfg.compound_split = 1;
    tls = TlsAlloc();
    vt_result = fake_result_vt;
    orig_process = fake_process;
    orig_child = fake_child;
    owner_tid = GetCurrentThreadId();
    HANDLE h[3];
    for (int i = 0; i < 3; i++) h[i] = CreateThread(NULL, 0, helper, NULL, 0, NULL);

    run("no child", 0, NULL, 0, 50);
    run("47 children: inline, original order", 47, NULL, 0, 200);
    run("48 children: split", 48, NULL, 1, 200);
    run("500 children: split", 500, NULL, 1, 200);
    run("5000 children: split", 5000, NULL, 1, 40);
    run("shared manifold: inline", 500, (void *)0x1234, 0, 50);
    if (wrong_result) { printf("children saw the wrong btManifoldResult %ld times\n", wrong_result); fails++; }
    if (bad_restore) { printf("wrapper swaps interleaved %ld times\n", bad_restore); fails++; }

    stop = 1;
    WaitForMultipleObjects(3, h, TRUE, INFINITE);
    char buf[160];
    compound_format(buf, sizeof buf);
    printf("stats: %s\nRESULT: %s\n", buf, fails ? "FAILED" : "OK");
    return fails != 0;
}
