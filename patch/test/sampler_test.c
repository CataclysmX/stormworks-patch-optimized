/* Test of sampler.c's deep unwinding and inclusive report. A thread registered as the tick
 * thread runs a 10-level call chain level0 -> ... -> level9 -> burn; level5 sleeps half of the
 * time (leaf outside the exe, found back through the return-address scan). A short-lived
 * "physics" thread exits during the run (dead thread path). Checks, from the report:
 * inclusive % of the chain, self time of burn, the call tree order, the TEB stack bound.
 *   make tests && wine build/sampler_test.exe */
#include "../src/sampler.c"
#include <stdio.h>
#include <stdlib.h>

stw_config g_cfg;
uint8_t *g_base;
const char *g_prefix = "sampler_test";
void log_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

static volatile LONG stop;
static volatile uintptr_t worker_stack_base;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } else { printf("ok: " __VA_ARGS__); putchar('\n'); } } while (0)

__attribute__((noipa)) static void burn(int n)
{
    volatile double x = 1;
    for (int i = 0; i < n; i++) x = x * 1.0000001 + 1e-9;
}

/* the asm after each call keeps it out of tail position: every level keeps its frame */
#define LEVEL(k, body) __attribute__((noipa)) static void level##k(int p) { body; __asm__ volatile("" ::: "memory"); }
LEVEL(9, burn(200000))
LEVEL(8, level9(p))
LEVEL(7, level8(p))
LEVEL(6, level7(p))
LEVEL(5, if (p & 1) Sleep(1); else level6(p))
LEVEL(4, level5(p))
LEVEL(3, level4(p))
LEVEL(2, level3(p))
LEVEL(1, level2(p))
LEVEL(0, level1(p))

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    worker_stack_base = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
    for (int i = 0; !stop; i++) {
        level0(i);
        g_last_tick_ms = GetTickCount64();
    }
    return 0;
}

static DWORD WINAPI short_lived(LPVOID arg)
{
    (void)arg;
    Sleep(600);                                   /* sampled a while, then exits */
    return 0;
}

/* like a game thread caught while exiting: rsp points into memory that is reserved but not
 * committed, below the thread's StackBase (so the bound check lets the unwinding read it) */
static volatile LONG bad_stop, bad_running;
static volatile uintptr_t bad_rsp;
static DWORD WINAPI bad_stack(LPVOID arg)
{
    (void)arg;
    uintptr_t below = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackLimit;
    for (uintptr_t a = (below - 0x400000) & ~(uintptr_t)0xFFFF; a > 0x10000000 && !bad_rsp; a -= 0x100000)
        if (VirtualAlloc((void *)a, 0x10000, MEM_RESERVE, PAGE_READWRITE)) bad_rsp = a + 0x8000;
    if (!bad_rsp) return 0;
    bad_running = 1;
    __asm__ volatile(
        "mov %%rsp, %%r12\n\t"
        "mov %0, %%rsp\n\t"
        "1: pause\n\t"
        "cmpl $0, %1\n\t"
        "je 1b\n\t"
        "mov %%r12, %%rsp\n\t"
        :: "r"(bad_rsp), "m"(bad_stop) : "r12", "memory");
    return 0;
}

/* the in-game section of the tick thread: its last header in the report */
static const char *tick_section(const char *report)
{
    const char *sec = NULL;
    for (const char *s = report; (s = strstr(s, "== thread tick (simulation)")); s++) sec = s;
    return sec;
}

static double incl_of(const char *report, void *fn, double *self)
{
    char key[64];
    snprintf(key, sizeof key, "exe+0x%llx ", (unsigned long long)((uint8_t *)fn - g_base));
    const char *sec = tick_section(report);
    const char *s = sec ? strstr(sec, "inclusive time") : NULL;
    const char *end = s ? strstr(s, "call tree") : NULL;
    for (const char *l = s; l && l < end; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        double in, se;
        char nm[64];
        if (sscanf(l, " %lf%% self %lf%% %63s", &in, &se, nm) == 3 && !strncmp(nm, key, strlen(key) - 1) && nm[strlen(key) - 1] == 0) {
            if (self) *self = se;
            return in;
        }
    }
    return -1;
}

/* indentation of fn in the call tree of the tick thread, -1 if absent */
static int tree_depth(const char *report, void *fn)
{
    char key[64];
    snprintf(key, sizeof key, "exe+0x%llx\n", (unsigned long long)((uint8_t *)fn - g_base));
    const char *sec = tick_section(report);
    const char *t = sec ? strstr(sec, "call tree") : NULL;
    const char *end = t ? strstr(t, "\n== ") : NULL;
    const char *hit = t ? strstr(t, key) : NULL;
    if (!hit || (end && hit > end)) return -1;
    const char *ls = hit;
    while (ls > t && ls[-1] != '\n') ls--;
    const char *p = ls;
    while (*p == ' ' || (*p >= '0' && *p <= '9') || *p == '.') p++;
    if (*p != '%') return -1;
    p++;
    int spaces = 0;
    while (p[spaces] == ' ') spaces++;
    return spaces;
}

int main(void)
{
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    g_cfg.sampler_ms = 1;
    g_cfg.phys_report_sec = 1;
    HANDLE w = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    g_tick_tid = GetThreadId(w);
    g_last_tick_ms = GetTickCount64();
    HANDLE p = CreateThread(NULL, 0, short_lived, NULL, CREATE_SUSPENDED, NULL);
    typedef HRESULT (WINAPI *std_t)(HANDLE, PCWSTR);
    std_t std = (std_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    if (std) std(p, L"physics");
    ResumeThread(p);
    HANDLE b = CreateThread(NULL, 0, bad_stack, NULL, CREATE_SUSPENDED, NULL);
    if (std) std(b, L"streaming");
    ResumeThread(b);
    if (!sampler_install()) { printf("FAIL: sampler_install\n"); return 1; }
    Sleep(4500);
    stop = 1;
    bad_stop = 1;
    WaitForSingleObject(w, INFINITE);
    WaitForSingleObject(b, INFINITE);
    Sleep(1200);                                  /* one more report */

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    char *sl = strrchr(path, '\\');
    sprintf(sl ? sl + 1 : path, "sampler_test_sampler.txt");
    FILE *f = fopen(path, "r");   /* text mode: the report has CRLF line ends */
    CHECK(f != NULL, "report written (%s)", path);
    if (!f) return 1;
    static char rep[1 << 20];
    size_t n = fread(rep, 1, sizeof rep - 1, f);
    rep[n] = 0;
    fclose(f);

    int ti = -1, pi = -1;
    for (int i = 0; i < n_th; i++) {
        if (th[i].tid == g_tick_tid) ti = i;
        if (!strcmp(th[i].name, "physics")) pi = i;
    }
    CHECK(ti >= 0 && th[ti].stack_hi == worker_stack_base && worker_stack_base,
          "TEB stack base of the tick thread (%p vs %p)", ti >= 0 ? (void *)th[ti].stack_hi : NULL, (void *)worker_stack_base);
    CHECK(pi >= 0 && th[pi].dead, "short-lived physics thread seen and marked dead");
    int bi = -1;
    for (int i = 0; i < n_th; i++) if (!strcmp(th[i].name, "streaming")) bi = i;
    CHECK(bad_running && bi >= 0 && bad_rsp < th[bi].stack_hi,
          "thread with rsp in reserved memory below its StackBase (rsp %p)", (void *)bad_rsp);
    CHECK(bi >= 0 && th[bi].faults > 0 && unwind_faults >= th[bi].faults,
          "its unreadable stack faulted %d times, sampler survived and dropped those samples", bi >= 0 ? th[bi].faults : 0);
    CHECK(strstr(rep, "hottest instructions") != NULL, "hottest instructions listed");

    double s9 = 0, sburn = 0;
    double i0 = incl_of(rep, level0, NULL), i5 = incl_of(rep, level5, NULL), i6 = incl_of(rep, level6, NULL);
    double i9 = incl_of(rep, level9, &s9), ib = incl_of(rep, burn, &sburn);
    printf("inclusive: level0 %.1f, level5 %.1f, level6 %.1f, level9 %.1f (self %.1f), burn %.1f (self %.1f)\n",
           i0, i5, i6, i9, s9, ib, sburn);
    CHECK(i0 >= 85, "level0 holds the whole tick thread (%.1f%%)", i0);
    CHECK(i5 >= 85, "level5 holds its sleep too, leaf outside the exe (%.1f%%)", i5);
    CHECK(i6 > 10 && i6 < i5 - 10, "level6 only gets the burning half (%.1f%%)", i6);
    CHECK(ib > 10 && sburn > ib * 0.9, "burn: inclusive %.1f%%, nearly all self (%.1f%%)", ib, sburn);
    CHECK(s9 < 2, "level9 has (almost) no self time (%.1f%%)", s9);
    int prev = -1, ordered = 1;
    void *chain[] = { level0, level1, level2, level3, level4, level5, level6, level7, level8, level9, burn };
    for (int k = 0; k < 11; k++) {
        int d = tree_depth(rep, chain[k]);
        printf("  chain[%d] exe+0x%llx: tree depth %d\n", k, (unsigned long long)((uint8_t *)chain[k] - g_base), d);
        if (d <= prev) ordered = 0;
        prev = d;
    }
    CHECK(ordered, "call tree: level0 ... level9, burn, each one deeper");
    CHECK(strstr(rep, "== thread physics") != NULL, "dead thread still in the report");
    printf(fails ? "%d FAILED\n" : "all OK\n", fails);
    return fails != 0;
}
