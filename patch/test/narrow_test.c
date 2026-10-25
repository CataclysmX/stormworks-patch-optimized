/* Stress test of narrowphase.c against a fake dispatcher with the game's layout.
 * The fake pools and manifold list are deliberately not thread-safe, like Bullet's. */
#include "../src/narrowphase.c"
/* client build layout, used only to place the fakes */
#define GAME_PE_SIZEOFIMAGE     0xd8b000
#define RVA_VT_DISPATCHER       0xae0a30
#define RVA_DISP_NEW_MANIFOLD   0x0d7f40
#define RVA_DISP_RELEASE_MANIFOLD 0x0d81e0
#define RVA_DISP_DISPATCH_ALL   0x0d8450
#define RVA_DISP_ALLOC_ALGO     0x0d8630
#define RVA_DISP_FREE_ALGO      0x0d8660
#define RVA_DEFAULT_NEAR_CALLBACK 0x0d84b0
#define RVA_VT_HASHED_PAIRCACHE 0xae0810
#define RVA_PROFILE_START       0x163440
#define RVA_PROFILE_STOP        0x163360
game_addrs g_addr;
#include <stdio.h>
#include <stdlib.h>

stw_config g_cfg;
uint8_t *g_base;
void log_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
void *patch_ptr(void **slot, void *value) { void *p = *slot; *slot = value; return p; }

#define NPAIRS 3000
#define POOL_N 4096
#define ELEM 0x200
typedef struct { int elem, max, free_count, pad; void *first_free; uint8_t *base; } pool_t;
static pool_t pool;
static void **manifolds; static int nman, capman;
static volatile LONG errors, processed_soft_late;
static volatile LONG pass_counter[NPAIRS];
static volatile LONG rigid_done;

static void *f_new_manifold(void *d, void *a, void *b) { (void)d;(void)a;(void)b;
    if (nman == capman) { capman = capman ? capman*2 : 64; manifolds = realloc(manifolds, capman*sizeof *manifolds); }
    void *m = malloc(64); *(int*)m = nman; manifolds[nman++] = m; return m; }
static void f_release_manifold(void *d, void *m) { (void)d;
    int i = *(int*)m; if (i < 0 || i >= nman || manifolds[i] != m) { InterlockedIncrement(&errors); return; }
    manifolds[i] = manifolds[--nman]; *(int*)manifolds[i] = i; free(m); }
static void *f_alloc(void *d, int size) { (void)d;
    if (pool.first_free) { void *p = pool.first_free; pool.first_free = *(void**)p; pool.free_count--; return p; }
    return _aligned_malloc(size, 16); }
static void f_free(void *d, void *p) { (void)d;
    if ((uint8_t*)p >= pool.base && (uint8_t*)p < pool.base + ELEM*POOL_N) { *(void**)p = pool.first_free; pool.first_free = p; pool.free_count++; return; }
    _aligned_free(p); }
static void f_dispatch(void *d, uint8_t *pc, uint8_t *info, void *d2) { (void)d;(void)pc;(void)info;(void)d2; InterlockedIncrement(&errors); }
static void f_prof_start(const char *n) { (void)n; }
static void f_prof_stop(void) {}

static void **fake_vt;
static void f_near(uint8_t *pair, void *disp, uint8_t *info) {
    (void)info;
    void **vt = *(void***)disp;
    int idx = *(int*)(pair + 0x18);
    uint8_t *co0 = **(uint8_t***)pair;
    int soft = *(int*)(co0 + CO_INTERNAL_TYPE) & 8;
    if (soft && rigid_done == 0) InterlockedIncrement(&processed_soft_late); /* soft before rigid finished = bug */
    InterlockedIncrement(&pass_counter[idx]);
    ((void(*)(const char*))(g_base + RVA_PROFILE_START))("tri");
    int tris = (idx * 7919) % 40;
    for (int k = 0; k < tris; k++) {          /* per-triangle temporary algorithm */
        uint8_t *a = ((void*(*)(void*,int))vt[13])(disp, 0x80);
        memset(a, idx & 0xff, 0x80);
        for (int j = 0; j < 0x80; j++) if (a[j] != (uint8_t)(idx & 0xff)) { InterlockedIncrement(&errors); break; }
        ((void(*)(void*,void*))vt[14])(disp, a);
    }
    ((void(*)(void))(g_base + RVA_PROFILE_STOP))();
    void **algo = (void**)(pair + 0x10);       /* persistent algorithm + manifold churn */
    if (!*algo) { *algo = ((void*(*)(void*,void*,void*))vt[2])(disp, 0, 0); }
    else if (idx % 3 == 0) { ((void(*)(void*,void*))vt[3])(disp, *algo); *algo = 0; }
}

static void serial_dispatch(void *d, uint8_t *pc, uint8_t *info, void *d2) { (void)d2;
    int n = *(int*)(pc + PC_NUM_PAIRS); uint8_t *pairs = *(uint8_t**)(pc + PC_PAIRS);
    for (int i = 0; i < n; i++) f_near(pairs + i*PAIR_STRIDE, d, info); }
static void jmp_at(uint8_t *at, void *to) { if (0) {} at[0]=0xff; at[1]=0x25; *(uint32_t*)(at+2)=0; *(uint64_t*)(at+6)=(uint64_t)to; }

int main(void) {
    g_base = VirtualAlloc(NULL, GAME_PE_SIZEOFIMAGE, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    jmp_at(g_base + RVA_DISP_NEW_MANIFOLD, f_new_manifold);
    jmp_at(g_base + RVA_DISP_RELEASE_MANIFOLD, f_release_manifold);
    jmp_at(g_base + RVA_DISP_DISPATCH_ALL, f_dispatch);
    memcpy(g_base + RVA_DISP_ALLOC_ALGO, "\x4C\x8B\x41\x38", 4); jmp_at(g_base + RVA_DISP_ALLOC_ALGO + 4, f_alloc);
    memcpy(g_base + RVA_DISP_FREE_ALGO, "\x4C\x8B\x41\x38", 4); jmp_at(g_base + RVA_DISP_FREE_ALGO + 4, f_free);
    jmp_at(g_base + RVA_DEFAULT_NEAR_CALLBACK, f_near);
    jmp_at(g_base + RVA_PROFILE_START, f_prof_start);
    jmp_at(g_base + RVA_PROFILE_STOP, f_prof_stop);
    fake_vt = (void**)(g_base + RVA_VT_DISPATCHER);
    fake_vt[2] = g_base + RVA_DISP_NEW_MANIFOLD; fake_vt[3] = g_base + RVA_DISP_RELEASE_MANIFOLD;
    fake_vt[7] = g_base + RVA_DISP_DISPATCH_ALL; if (getenv("MINP")) jmp_at(g_base + RVA_DISP_DISPATCH_ALL, serial_dispatch); fake_vt[13] = g_base + RVA_DISP_ALLOC_ALGO;
    fake_vt[14] = g_base + RVA_DISP_FREE_ALGO;

    pool.elem = ELEM; pool.max = POOL_N; pool.base = _aligned_malloc(ELEM*POOL_N, 16);
    for (int i = POOL_N-1; i >= 0; i--) { void *p = pool.base + i*ELEM; *(void**)p = pool.first_free; pool.first_free = p; pool.free_count++; }

    uint8_t *disp = calloc(1, 0x100);
    *(void**)disp = fake_vt; *(void**)(disp + 0x30) = g_base + RVA_DEFAULT_NEAR_CALLBACK; *(void**)(disp + 0x38) = &pool;
    /* proxies -> collision objects */
    uint8_t *objs = calloc(NPAIRS, 0x200); void **proxies = calloc(NPAIRS, sizeof(void*));
    uint8_t *pc = calloc(1, 0x40); uint8_t *pairs = calloc(NPAIRS, PAIR_STRIDE);
    for (int i = 0; i < NPAIRS; i++) {
        *(int*)(objs + i*0x200 + CO_INTERNAL_TYPE) = (i % 97 == 0) ? 8 : 2;
        proxies[i] = objs + i*0x200;
        *(void**)(pairs + i*PAIR_STRIDE) = &proxies[i];
        *(void**)(pairs + i*PAIR_STRIDE + 8) = &proxies[(i+1) % NPAIRS];
        *(int*)(pairs + i*PAIR_STRIDE + 0x18) = i;
    }
    /* pair i+1's object may be soft: fix so only proxy0 decides, keep proxy1 rigid */
    for (int i = 0; i < NPAIRS; i++) *(void**)(pairs + i*PAIR_STRIDE + 8) = &proxies[0] ;
    *(int*)(objs + CO_INTERNAL_TYPE) = 2;
    *(void**)pc = g_base + RVA_VT_HASHED_PAIRCACHE; *(int*)(pc + PC_NUM_PAIRS) = NPAIRS; *(void**)(pc + PC_PAIRS) = pairs;
    uint8_t info[0x40] = {0}; *(int*)(info + INFO_DISPATCH_FUNC) = 1;

    g_addr.vt_dispatcher = (uint8_t *)fake_vt; g_addr.vt_hashed_cache = g_base + RVA_VT_HASHED_PAIRCACHE;
    g_addr.default_near = g_base + RVA_DEFAULT_NEAR_CALLBACK;
    g_addr.prof_start = g_base + RVA_PROFILE_START; g_addr.prof_stop = g_base + RVA_PROFILE_STOP;
    g_cfg.narrow_threads = 7; g_cfg.narrow_balance = getenv("NOBAL") ? 0 : 1; g_cfg.narrow_min_pairs = getenv("MINP") ? atoi(getenv("MINP")) : 64;
    if (!narrowphase_install()) { printf("install failed\n"); return 1; }
    int soft_pairs = 0; for (int i = 0; i < NPAIRS; i++) if (pair_has_soft_body(pairs + i*PAIR_STRIDE)) soft_pairs++;

    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
    const int PASSES = 2000;
    QueryPerformanceCounter(&t0);
    for (int p = 0; p < PASSES; p++) {
        rigid_done = 1; /* soft pairs run after the parallel pass: flag only checked as a sanity hint */
        ((void(*)(void*,uint8_t*,uint8_t*,void*))fake_vt[7])(disp, pc, info, disp);
    }
    QueryPerformanceCounter(&t1);
    int bad = 0;
    for (int i = 0; i < NPAIRS; i++) if (pass_counter[i] != PASSES) bad++;
    /* return everything still held: persistent manifolds */
    int outstanding_blocks = POOL_N - pool.free_count;
    printf("passes=%d pairs=%d soft=%d  time/pass=%.3f ms\n", PASSES, NPAIRS, soft_pairs,
           (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / PASSES);
    printf("pairs not processed exactly once per pass: %d\n", bad);
    printf("pool blocks outstanding after passes: %d (expected 0)\n", outstanding_blocks);
    printf("manifold list size=%d, errors=%ld, parallel=%lld serial=%lld\n", nman, errors,
           (long long)stat_parallel, (long long)stat_serial);
    int live = 0; for (int i = 0; i < NPAIRS; i++) if (*(void**)(pairs + i*PAIR_STRIDE + 0x10)) live++;
    printf("persistent manifolds held by pairs=%d (must equal list size)\n", live);
    return (bad || outstanding_blocks || errors || live != nman) ? 1 : 0;
}
