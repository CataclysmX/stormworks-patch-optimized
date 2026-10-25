/* Sampling profiler: where does the game spend its time while loading?
 *
 * A thread wakes every `interval_ms`, suspends each watched game thread (the main thread and the
 * threads the game names: streaming, state_transition, vehicle_save, physics, http, ...), reads
 * its context and unwinds a few frames with RtlVirtualUnwind. Addresses inside the exe are mapped
 * to their function through the exe's .pdata (RUNTIME_FUNCTION table, chained entries followed to
 * the primary function), other addresses to their module. Each sample is classed as
 *   "loading / stall": the main thread has not completed a game tick for 150 ms or more
 *   "in game"        : ticks are flowing
 * and per-thread CPU time is accounted, to tell CPU-bound work from waiting on I/O or locks.
 * Report: <prefix>_sampler.txt, rewritten every report_seconds, cumulative for the session. */
#include "stwpatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#define MAX_THREADS 32
#define DEPTH       4
#define MAX_KEYS    8192

volatile ULONGLONG g_last_tick_ms;     /* set by physics.c on every game tick */
volatile DWORD g_tick_tid;

typedef struct {
    DWORD tid;
    HANDLE h;
    char name[32];
    ULONGLONG cpu_100ns[2];             /* CPU time while stalled / in game */
    ULONGLONG wall_samples[2];
    ULONGLONG last_cpu;
} watched_t;

typedef struct { uint64_t key[DEPTH]; int thread; int cls; uint32_t count; } entry_t;

static watched_t th[MAX_THREADS];
static int n_th;
static DWORD main_tid;
static entry_t *table;                  /* open addressing on (frames, thread, class) */
static uint32_t n_entries;
static RUNTIME_FUNCTION *pdata;
static DWORD n_pdata;
static uint32_t image_size;
static ULONGLONG total_samples[2];

static const char *watch_names[] = { "streaming", "state_transition", "vehicle_save", "vehicle_tick",
                                     "physics", "http", "unnamed_thread", NULL };

/* .pdata lookup in the exe, lock free (safe while another thread is suspended) */
static RUNTIME_FUNCTION *exe_function(uintptr_t pc)
{
    if (pc < (uintptr_t)g_base || pc >= (uintptr_t)g_base + image_size) return NULL;
    DWORD rva = (DWORD)(pc - (uintptr_t)g_base);
    long lo = 0, hi = (long)n_pdata - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (rva < pdata[mid].BeginAddress) hi = mid - 1;
        else if (rva >= pdata[mid].EndAddress) lo = mid + 1;
        else return &pdata[mid];
    }
    return NULL;
}

static uint64_t primary_start(RUNTIME_FUNCTION *rf)
{
    for (int k = 0; k < 8; k++) {      /* UNW_FLAG_CHAININFO: follow to the primary entry */
        uint8_t *ui = g_base + rf->UnwindData;
        if (!((ui[0] >> 3) & 0x4)) break;
        int codes = (ui[2] + 1) & ~1;
        rf = (RUNTIME_FUNCTION *)(ui + 4 + codes * 2);
    }
    return rf->BeginAddress;
}

static int in_exe(uintptr_t pc) { return pc >= (uintptr_t)g_base && pc < (uintptr_t)g_base + image_size; }

/* after resume: frame value = exe function start RVA, or module base | (1<<63) */
static uint64_t module_key(uintptr_t pc)
{
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &m) && m)
        return (uint64_t)(uintptr_t)m | (1ull << 63);
    return 1ull << 63;
}

static void record(const uint64_t *frames, int thread, int cls)
{
    uint64_t h = (uint64_t)thread * 31 + (uint64_t)cls;
    for (int i = 0; i < DEPTH; i++) h = (h ^ frames[i]) * 0x9E3779B97F4A7C15ull;
    uint32_t i = (uint32_t)(h >> 40) & (MAX_KEYS - 1);
    for (uint32_t n = 0; n < MAX_KEYS; n++, i = (i + 1) & (MAX_KEYS - 1)) {
        entry_t *e = &table[i];
        if (e->count == 0) {
            if (n_entries >= MAX_KEYS * 3 / 4) return;   /* table full: drop rare stacks */
            memcpy(e->key, frames, sizeof e->key);
            e->thread = thread; e->cls = cls; e->count = 1;
            n_entries++;
            return;
        }
        if (e->thread == thread && e->cls == cls && !memcmp(e->key, frames, sizeof e->key)) { e->count++; return; }
    }
}

static void sample_thread(int idx, int cls)
{
    watched_t *w = &th[idx];
    uint64_t frames[DEPTH] = { 0 };
    uintptr_t outside = 0;                       /* leaf outside the exe: resolved after resume */
    if (SuspendThread(w->h) == (DWORD)-1) return;
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(w->h, &c)) {
        int d = 0;
        if (!in_exe(c.Rip)) {
            /* ntdll / driver / OpenAL ...: keep the module, continue from the first exe return
             * address found on the stack (heuristic, no unwind info lookups outside the exe) */
            outside = (uintptr_t)c.Rip;
            d = 1;
            DWORD64 *sp = (DWORD64 *)c.Rsp;
            MEMORY_BASIC_INFORMATION mbi;
            c.Rip = 0;
            if (VirtualQuery(sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
                DWORD64 *end = (DWORD64 *)((uint8_t *)mbi.BaseAddress + mbi.RegionSize);
                for (int i = 0; i < 2048 && sp + i < end; i++)
                    if (exe_function(sp[i])) { c.Rip = sp[i]; c.Rsp = (DWORD64)(sp + i + 1); break; }
            }
        }
        for (; d < DEPTH && c.Rip; d++) {
            RUNTIME_FUNCTION *rf = exe_function(c.Rip);
            if (!rf) { frames[d] = (uint64_t)c.Rip | (1ull << 62); break; }   /* resolved after resume */
            frames[d] = primary_start(rf);
            void *handler; DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, (DWORD64)g_base, c.Rip, rf, &c, &handler, &frame, NULL);
        }
    }
    ResumeThread(w->h);
    if (outside) frames[0] = module_key(outside);
    for (int d = 0; d < DEPTH; d++)
        if (frames[d] & (1ull << 62)) frames[d] = module_key((uintptr_t)(frames[d] & ~(1ull << 62)));
    record(frames, idx, cls);
    w->wall_samples[cls]++;
}

static void refresh_threads(void)
{
    typedef HRESULT (WINAPI *gtd_t)(HANDLE, PWSTR *);
    static gtd_t gtd;
    if (!gtd) gtd = (gtd_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { .dwSize = sizeof te };
    DWORD me = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok && n_th < MAX_THREADS; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != me || te.th32ThreadID == self) continue;
        int known = 0;
        for (int i = 0; i < n_th; i++)
            if (th[i].tid == te.th32ThreadID) {
                known = 1;
                if (te.th32ThreadID == g_tick_tid && strncmp(th[i].name, "tick", 4))
                    snprintf(th[i].name, sizeof th[i].name, "tick (simulation)");
            }
        if (known) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        char name[32] = "";
        if (te.th32ThreadID == g_tick_tid) strcpy(name, "tick (simulation)");
        else if (te.th32ThreadID == main_tid) strcpy(name, "main (render)");
        else if (gtd) {
            PWSTR wn = NULL;
            if (SUCCEEDED(gtd(h, &wn)) && wn) {
                WideCharToMultiByte(CP_UTF8, 0, wn, -1, name, sizeof name, NULL, NULL);
                LocalFree(wn);
            }
        }
        int watch = te.th32ThreadID == main_tid || te.th32ThreadID == g_tick_tid;
        for (int k = 0; !watch && watch_names[k]; k++) if (!strcmp(name, watch_names[k])) watch = 1;
        if (!watch) { CloseHandle(h); continue; }
        watched_t *w = &th[n_th++];
        memset(w, 0, sizeof *w);
        w->tid = te.th32ThreadID; w->h = h;
        snprintf(w->name, sizeof w->name, "%s", name);
    }
    CloseHandle(snap);
}

static ULONGLONG thread_cpu(HANDLE h)
{
    FILETIME a, b, k, u;
    if (!GetThreadTimes(h, &a, &b, &k, &u)) return 0;
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

static void frame_name(uint64_t f, char *out, size_t n)
{
    if (!f) { snprintf(out, n, "-"); return; }
    if (!(f >> 63)) { snprintf(out, n, "exe+0x%llx", (unsigned long long)f); return; }
    HMODULE m = (HMODULE)(uintptr_t)(f & ~(1ull << 63));
    char p[MAX_PATH] = "?";
    if (m) GetModuleFileNameA(m, p, sizeof p);
    const char *b = strrchr(p, '\\');
    snprintf(out, n, "%s", b ? b + 1 : p);
}

static int by_count(const void *a, const void *b)
{
    uint32_t x = (*(entry_t *const *)a)->count, y = (*(entry_t *const *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void write_report(const char *path)
{
    static entry_t *sorted[MAX_KEYS];
    FILE *f = fopen(path, "w");
    if (!f) return;
    static const char *cls_name[2] = { "LOADING / STALL (no game tick for >= 150 ms)", "IN GAME (ticks flowing)" };
    fprintf(f, "stwpatch sampler, every %d ms; samples: loading %llu, in game %llu\n",
            g_cfg.sampler_ms, total_samples[0], total_samples[1]);
    fprintf(f, "exe functions are RVAs (Ghidra: 0x140000000 + rva). 'cpu' = share of wall time the thread was running.\n");
    for (int cls = 0; cls < 2; cls++) {
        fprintf(f, "\n################ %s ################\n", cls_name[cls]);
        for (int t = 0; t < n_th; t++) {
            watched_t *w = &th[t];
            if (!w->wall_samples[cls]) continue;
            double wall_100ns = (double)w->wall_samples[cls] * g_cfg.sampler_ms * 10000.0;
            fprintf(f, "\n== thread %s (%lu): %llu samples, cpu %.0f%%\n", w->name, w->tid, w->wall_samples[cls],
                    100.0 * w->cpu_100ns[cls] / (wall_100ns > 0 ? wall_100ns : 1));
            /* leaf functions */
            int n = 0;
            for (uint32_t i = 0; i < MAX_KEYS; i++)
                if (table[i].count && table[i].thread == t && table[i].cls == cls) sorted[n++] = &table[i];
            qsort(sorted, n, sizeof *sorted, by_count);
            fprintf(f, "  top stacks (leaf <- caller <- caller <- caller):\n");
            for (int i = 0; i < n && i < 25; i++) {
                char s[DEPTH][64];
                for (int d = 0; d < DEPTH; d++) frame_name(sorted[i]->key[d], s[d], sizeof s[d]);
                fprintf(f, "  %5.1f%%  %-22s <- %-22s <- %-22s <- %s\n", 100.0 * sorted[i]->count / w->wall_samples[cls],
                        s[0], s[1], s[2], s[3]);
            }
            /* aggregate by leaf function */
            static uint64_t leaf[MAX_KEYS]; static uint32_t sum[MAX_KEYS];
            int u = 0;
            for (int i = 0; i < n; i++) {
                int k = 0;
                while (k < u && leaf[k] != sorted[i]->key[0]) k++;
                if (k == u) { leaf[u] = sorted[i]->key[0]; sum[u] = 0; u++; }
                sum[k] += sorted[i]->count;
            }
            /* samples whose leaf is outside the exe (ntdll, drivers...), by first exe function */
            static uint64_t via[MAX_KEYS]; static uint32_t vsum[MAX_KEYS];
            int nv = 0;
            for (int i = 0; i < n; i++) {
                if (!(sorted[i]->key[0] >> 63)) continue;
                uint64_t k = sorted[i]->key[1];
                int j = 0;
                while (j < nv && via[j] != k) j++;
                if (j == nv) { via[nv] = k; vsum[nv] = 0; nv++; }
                vsum[j] += sorted[i]->count;
            }
            if (nv) fprintf(f, "  time outside the exe, by calling exe function:\n");
            for (int pass = 0; pass < 12; pass++) {
                int best = -1;
                for (int k = 0; k < nv; k++) if (vsum[k] && (best < 0 || vsum[k] > vsum[best])) best = k;
                if (best < 0) break;
                char nm[64];
                frame_name(via[best], nm, sizeof nm);
                fprintf(f, "  %5.1f%%  %s\n", 100.0 * vsum[best] / w->wall_samples[cls], nm);
                vsum[best] = 0;
            }
            fprintf(f, "  top leaf functions:\n");
            for (int pass = 0; pass < 15; pass++) {
                int best = -1;
                for (int k = 0; k < u; k++) if (sum[k] && (best < 0 || sum[k] > sum[best])) best = k;
                if (best < 0) break;
                char nm[64];
                frame_name(leaf[best], nm, sizeof nm);
                fprintf(f, "  %5.1f%%  %s\n", 100.0 * sum[best] / w->wall_samples[cls], nm);
                sum[best] = 0;
            }
        }
    }
    fclose(f);
}

static DWORD WINAPI sampler_thread(LPVOID p)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s", (const char *)p);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ULONGLONG last_refresh = 0, last_report = GetTickCount64();
    for (;;) {
        Sleep((DWORD)g_cfg.sampler_ms);
        ULONGLONG now = GetTickCount64();
        if (now - last_refresh > 2000) { refresh_threads(); last_refresh = now; }
        int cls = (now - g_last_tick_ms >= 150) ? 0 : 1;
        total_samples[cls]++;
        for (int i = 0; i < n_th; i++) {
            ULONGLONG cpu = thread_cpu(th[i].h);
            if (cpu && th[i].last_cpu && cpu >= th[i].last_cpu) th[i].cpu_100ns[cls] += cpu - th[i].last_cpu;
            if (cpu) th[i].last_cpu = cpu;
            sample_thread(i, cls);
        }
        if (now - last_report > (ULONGLONG)g_cfg.phys_report_sec * 1000) {
            write_report(path);
            last_report = now;
        }
    }
    return 0;
}

int sampler_install(void)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    image_size = nt->OptionalHeader.SizeOfImage;
    pdata = (RUNTIME_FUNCTION *)(g_base + ex.VirtualAddress);
    n_pdata = ex.Size / sizeof(RUNTIME_FUNCTION);
    table = VirtualAlloc(NULL, sizeof(entry_t) * MAX_KEYS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!table || !n_pdata) return 0;
    main_tid = GetCurrentThreadId();             /* DllMain of a static import runs on the main thread */
    static char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    char *s = strrchr(path, '\\');
    sprintf(s ? s + 1 : path, "%s_sampler.txt", g_prefix);
    HANDLE h = CreateThread(NULL, 0, sampler_thread, path, 0, NULL);
    if (h) CloseHandle(h);
    log_printf("sampler: every %d ms, %lu functions in .pdata -> %s", g_cfg.sampler_ms, n_pdata, path);
    return 1;
}
