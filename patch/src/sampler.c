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
#include <psapi.h>

#define MAX_THREADS 64
#define DEPTH       4                   /* frames kept per stack in the "top stacks" table */
#define UNWIND      48                  /* frames unwound per sample for the inclusive tables */
#define MAX_KEYS    8192
#define MAX_KV      65536

/* frame keys: exe function RVA | SELF_BIT + this DLL's function RVA | MOD_BIT + module base
 * (other modules, resolved after resume) | RAW_BIT + pc (to resolve after resume) */
#define MOD_BIT  (1ull << 63)
#define RAW_BIT  (1ull << 62)
#define SELF_BIT (1ull << 61)

volatile ULONGLONG g_last_tick_ms;     /* set by physics.c on every game tick */
volatile DWORD g_tick_tid;

typedef struct {
    DWORD tid;
    HANDLE h;
    char name[32];
    ULONGLONG cpu_100ns[2];             /* CPU time while stalled / in game */
    ULONGLONG wall_samples[2];
    ULONGLONG last_cpu;
    uintptr_t stack_hi;                 /* TEB StackBase: unwinding never goes past it */
    int dead;
    int faults;                         /* samples dropped: stack not readable */
} watched_t;

typedef struct { uint64_t key[DEPTH]; int thread; int cls; uint32_t count; } entry_t;

/* inclusive tables: K_INCL (a = function, counted once per sample even when recursive),
 * K_SELF (a = leaf), K_EDGE (a = caller, b = callee), K_ROOT (a = outermost frame),
 * K_PC (a = leaf exe function, b = rva of the sampled instruction) */
enum { K_INCL, K_SELF, K_EDGE, K_ROOT, K_PC };
typedef struct { uint64_t a, b; uint32_t count; uint16_t thread; uint8_t cls, kind; } kv_t;

typedef struct { uintptr_t base; uint32_t size; RUNTIME_FUNCTION *pdata; DWORD n; } image_t;

static watched_t th[MAX_THREADS];
static int n_th;
static DWORD main_tid;
static entry_t *table;                  /* open addressing on (frames, thread, class) */
static uint32_t n_entries;
static kv_t *kv;                        /* open addressing on (kind, a, b, thread, class) */
static uint32_t n_kv, kv_dropped;
static image_t exe_img, self_img;
static RUNTIME_FUNCTION *pdata;
static DWORD n_pdata;
static uint32_t image_size;
static ULONGLONG total_samples[2];

static const char *watch_names[] = { "streaming", "state_transition", "vehicle_save", "vehicle_tick",
                                     "physics", "http", "unnamed_thread", NULL };

/* .pdata lookup in one image (the exe or this DLL), lock free: safe while another thread is
 * suspended, unlike RtlLookupFunctionEntry which may take the loader lock */
static RUNTIME_FUNCTION *image_function(const image_t *im, uintptr_t pc)
{
    if (!im->n || pc < im->base || pc >= im->base + im->size) return NULL;
    DWORD rva = (DWORD)(pc - im->base);
    long lo = 0, hi = (long)im->n - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (rva < im->pdata[mid].BeginAddress) hi = mid - 1;
        else if (rva >= im->pdata[mid].EndAddress) lo = mid + 1;
        else return &im->pdata[mid];
    }
    return NULL;
}

static RUNTIME_FUNCTION *exe_function(uintptr_t pc) { return image_function(&exe_img, pc); }

/* every loaded module with a .pdata (ntdll, kernelbase, Wine's PE side of the drivers, ...),
 * sorted by base, so that waits and driver calls unwind back to their real exe caller. Each
 * module gets a reference when added: it stays mapped while its .pdata may be read. */
#define MAX_IMAGES 256
static image_t imgs[MAX_IMAGES];
static int n_imgs;

static image_t image_of(HMODULE m)
{
    uint8_t *b = (uint8_t *)m;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    image_t im = { (uintptr_t)b, nt->OptionalHeader.SizeOfImage, NULL, 0 };
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return im;
    IMAGE_DATA_DIRECTORY x = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (x.VirtualAddress && x.Size) {
        im.pdata = (RUNTIME_FUNCTION *)(b + x.VirtualAddress);
        im.n = x.Size / sizeof(RUNTIME_FUNCTION);
    }
    return im;
}

static void add_image(image_t im)
{
    int i = 0;
    while (i < n_imgs && imgs[i].base < im.base) i++;
    if (i < n_imgs && imgs[i].base == im.base) return;
    if (n_imgs >= MAX_IMAGES) return;
    memmove(&imgs[i + 1], &imgs[i], (n_imgs - i) * sizeof *imgs);
    imgs[i] = im;
    n_imgs++;
}

/* never while a watched thread is suspended: EnumProcessModules takes the loader lock */
static void refresh_images(void)
{
    static HMODULE mods[512];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    int n = (int)(need / sizeof *mods);
    if (n > 512) n = 512;
    for (int i = 0; i < n; i++) {
        int known = 0;
        for (int k = 0; k < n_imgs && !known; k++) known = imgs[k].base == (uintptr_t)mods[i];
        if (known) continue;
        HMODULE ref = NULL;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)mods[i], &ref) || ref != mods[i]) continue;
        image_t im = image_of(ref);
        if (im.n) add_image(im);
        else FreeLibrary(ref);
    }
}

static const image_t *find_image(uintptr_t pc)
{
    int lo = 0, hi = n_imgs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (pc < imgs[mid].base) hi = mid - 1;
        else if (pc >= imgs[mid].base + imgs[mid].size) lo = mid + 1;
        else return &imgs[mid];
    }
    return NULL;
}

static uint64_t primary_start(const image_t *im, RUNTIME_FUNCTION *rf)
{
    for (int k = 0; k < 8; k++) {      /* UNW_FLAG_CHAININFO: follow to the primary entry */
        uint8_t *ui = (uint8_t *)im->base + rf->UnwindData;
        if (!((ui[0] >> 3) & 0x4)) break;
        int codes = (ui[2] + 1) & ~1;
        rf = (RUNTIME_FUNCTION *)(ui + 4 + codes * 2);
    }
    return rf->BeginAddress;
}

/* a value found on the stack is taken as a return address only if a call instruction ends
 * right before it (stale pointers into the exe are common in ntdll / driver frames) */
static int after_call(uintptr_t pc)
{
    if (!exe_function(pc) || pc < (uintptr_t)g_base + 8) return 0;
    const uint8_t *p = (const uint8_t *)pc;
    if (p[-5] == 0xE8) return 1;                                   /* call rel32 */
    if (p[-6] == 0xFF && p[-5] == 0x15) return 1;                  /* call [rip+disp32] */
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) return 1;         /* call reg */
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0x10) return 1;         /* call [reg] */
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return 1;         /* call [reg+disp8] */
    if (p[-4] == 0xFF && p[-3] == 0x54) return 1;                  /* call [reg+sib+disp8] */
    if (p[-6] == 0xFF && (p[-5] & 0xF8) == 0x90) return 1;         /* call [reg+disp32] */
    if (p[-7] == 0xFF && p[-6] == 0x94) return 1;                  /* call [reg+sib+disp32] */
    return 0;
}

static void kv_add(int kind, uint64_t a, uint64_t b, int thread, int cls)
{
    uint64_t h = ((uint64_t)thread << 8 | (uint64_t)cls << 4 | (uint64_t)kind) * 0x9E3779B97F4A7C15ull;
    h = (h ^ a) * 0x9E3779B97F4A7C15ull;
    h = (h ^ b) * 0x9E3779B97F4A7C15ull;
    uint32_t i = (uint32_t)(h >> 40) & (MAX_KV - 1);
    for (uint32_t n = 0; n < MAX_KV; n++, i = (i + 1) & (MAX_KV - 1)) {
        kv_t *e = &kv[i];
        if (!e->count) {
            if (n_kv >= MAX_KV * 3 / 4) { kv_dropped++; return; }
            e->a = a; e->b = b; e->thread = (uint16_t)thread; e->cls = (uint8_t)cls; e->kind = (uint8_t)kind;
            e->count = 1;
            n_kv++;
            return;
        }
        if (e->a == a && e->b == b && e->thread == thread && e->cls == cls && e->kind == kind) { e->count++; return; }
    }
}

/* f[0] = leaf ... f[n-1] = outermost frame found */
static void record_stack(const uint64_t *f, int n, int thread, int cls)
{
    if (n <= 0) return;
    kv_add(K_SELF, f[0], 0, thread, cls);
    kv_add(K_ROOT, f[n - 1], 0, thread, cls);
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < i && !seen; j++) seen = f[j] == f[i];
        if (!seen) kv_add(K_INCL, f[i], 0, thread, cls);
        if (i + 1 < n) {
            seen = 0;
            for (int j = 0; j < i && !seen; j++) seen = f[j] == f[i] && f[j + 1] == f[i + 1];
            if (!seen) kv_add(K_EDGE, f[i + 1], f[i], thread, cls);
        }
    }
}

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

/* Unwinding reads the stack of the suspended thread. That stack can be gone: a thread that is
 * exiting during a world (un)load still answers SuspendThread / GetThreadContext with an rsp in
 * its freed stack (game crash 2026-11-14 12:24, read of decommitted memory in RtlVirtualUnwind).
 * So the unwinding runs under a guard: an access violation on the sampler thread while it is
 * set resumes at guard_escape, which jumps back to sample_thread; the sample is dropped. */
static DWORD sampler_tid;
static volatile LONG in_unwind;
static void *guard_buf[5];
static volatile LONG unwind_faults;

__attribute__((noinline, noreturn)) static void guard_escape(void) { __builtin_longjmp(guard_buf, 1); }

static LONG CALLBACK sampler_veh(EXCEPTION_POINTERS *ep)
{
    if (!in_unwind || GetCurrentThreadId() != sampler_tid) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_DATATYPE_MISALIGNMENT) return EXCEPTION_CONTINUE_SEARCH;
    in_unwind = 0;
    unwind_faults++;
    ep->ContextRecord->Rip = (DWORD64)(uintptr_t)guard_escape;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* returns the number of frames; *leaf_pc = rva of the leaf pc when the leaf is in the exe */
__attribute__((noinline)) static int unwind_suspended(watched_t *w, uint64_t *frames, uintptr_t *outside_out,
                                                       uint64_t *leaf_pc)
{
    int n = 0;
    uintptr_t outside = 0;
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(w->h, &c)) {
        int d = 0;
        if (c.Rip >= exe_img.base && c.Rip < exe_img.base + exe_img.size) *leaf_pc = c.Rip - exe_img.base;
        DWORD64 lo = c.Rsp;                      /* caller frames are all between rsp and StackBase */
        if (!find_image(c.Rip)) {
            /* code outside every known module (generated code, module loaded since the last
             * refresh): keep it, continue from the first plausible return address into the exe */
            outside = (uintptr_t)c.Rip;
            d = 1;
            DWORD64 *sp = (DWORD64 *)c.Rsp;
            MEMORY_BASIC_INFORMATION mbi;
            c.Rip = 0;
            if (VirtualQuery(sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
                DWORD64 *end = (DWORD64 *)((uint8_t *)mbi.BaseAddress + mbi.RegionSize);
                for (int i = 0; i < 2048 && sp + i < end; i++)
                    if (after_call(sp[i])) { c.Rip = sp[i]; c.Rsp = (DWORD64)(sp + i + 1); break; }
            }
        }
        for (int iter = 0; d < UNWIND && c.Rip; d++, iter++) {
            if (c.Rsp < lo || (w->stack_hi && c.Rsp >= w->stack_hi) || (c.Rsp & 7)) break;
            const image_t *im = find_image(c.Rip);
            RUNTIME_FUNCTION *rf = im ? image_function(im, c.Rip) : NULL;
            uint64_t key;
            if (!im) key = (uint64_t)c.Rip | RAW_BIT;                      /* resolved after resume */
            else if (im->base == exe_img.base) key = rf ? primary_start(im, rf) : ((uint64_t)c.Rip | RAW_BIT);
            else if (im->base == self_img.base) key = rf ? primary_start(im, rf) | SELF_BIT : ((uint64_t)c.Rip | RAW_BIT);
            else key = (uint64_t)im->base | MOD_BIT;                        /* other modules: per module */
            /* consecutive frames in one module (ntdll -> ntdll) count as one frame */
            if (d > 0 && (key & MOD_BIT) && frames[d - 1] == key) d--;
            frames[d] = key;
            if (!rf) {
                /* no unwind info = leaf function that has not moved rsp: return address at [rsp] */
                if (iter == 0 && !outside && im && w->stack_hi) {
                    c.Rip = *(DWORD64 *)c.Rsp;
                    c.Rsp += 8;
                    continue;
                }
                d++;
                break;
            }
            void *handler; DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, (DWORD64)im->base, c.Rip, rf, &c, &handler, &frame, NULL);
        }
        n = d;
    }
    *outside_out = outside;
    return n;
}

static void sample_thread(int idx, int cls)
{
    watched_t *w = &th[idx];
    static uint64_t frames[UNWIND];              /* static: kept across the guard's longjmp */
    static volatile int n;
    static uintptr_t outside;                    /* leaf outside the exe: resolved after resume */
    static uint64_t leaf_pc;
    if (w->dead) return;
    if (SuspendThread(w->h) == (DWORD)-1) {
        DWORD code;
        if (GetExitCodeThread(w->h, &code) && code != STILL_ACTIVE) w->dead = 1;
        return;
    }
    memset(frames, 0, sizeof frames);
    n = 0;
    outside = 0;
    leaf_pc = 0;
    if (!__builtin_setjmp(guard_buf)) {
        in_unwind = 1;
        n = unwind_suspended(w, frames, (uintptr_t *)&outside, &leaf_pc);
        in_unwind = 0;
    } else {
        n = -1;                                  /* faulted: the thread's stack is not readable */
    }
    w = &th[idx];
    ResumeThread(w->h);
    if (n < 0) {
        w->faults++;
        DWORD code;
        if (GetExitCodeThread(w->h, &code) && code != STILL_ACTIVE) w->dead = 1;
        return;
    }
    if (outside) frames[0] = module_key(outside);
    for (int d = 0; d < n; d++)
        if (frames[d] & RAW_BIT) frames[d] = module_key((uintptr_t)(frames[d] & ~RAW_BIT));
    record(frames, idx, cls);
    record_stack(frames, n, idx, cls);
    if (leaf_pc && n > 0 && !(frames[0] >> 61)) kv_add(K_PC, frames[0], leaf_pc, idx, cls);
    w->wall_samples[cls]++;
}

/* StackBase of a thread of this process, from its TEB (NT_TIB.StackBase at +8); 0 if unknown */
static uintptr_t thread_stack_base(HANDLE h)
{
    typedef struct { LONG status; void *teb; void *cid[2]; ULONG_PTR affinity; LONG prio, base_prio; } tbi_t;
    typedef LONG (WINAPI *nqit_t)(HANDLE, int, void *, ULONG, ULONG *);
    static nqit_t nqit;
    if (!nqit) nqit = (nqit_t)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    tbi_t bi;
    memset(&bi, 0, sizeof bi);
    if (!nqit || nqit(h, 0 /* ThreadBasicInformation */, &bi, sizeof bi, NULL) < 0 || !bi.teb) return 0;
    return (uintptr_t)((NT_TIB *)bi.teb)->StackBase;
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
        w->stack_hi = thread_stack_base(h);
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
    if (f & SELF_BIT && !(f & MOD_BIT)) { snprintf(out, n, "stwpatch+0x%llx", (unsigned long long)(f & ~SELF_BIT)); return; }
    if (!(f >> 63)) { snprintf(out, n, "exe+0x%llx", (unsigned long long)f); return; }
    HMODULE m = (HMODULE)(uintptr_t)(f & ~MOD_BIT);
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

static kv_t *kv_find(int kind, uint64_t a, uint64_t b, int thread, int cls)
{
    uint64_t h = ((uint64_t)thread << 8 | (uint64_t)cls << 4 | (uint64_t)kind) * 0x9E3779B97F4A7C15ull;
    h = (h ^ a) * 0x9E3779B97F4A7C15ull;
    h = (h ^ b) * 0x9E3779B97F4A7C15ull;
    uint32_t i = (uint32_t)(h >> 40) & (MAX_KV - 1);
    for (uint32_t n = 0; n < MAX_KV; n++, i = (i + 1) & (MAX_KV - 1)) {
        kv_t *e = &kv[i];
        if (!e->count) return NULL;
        if (e->a == a && e->b == b && e->thread == thread && e->cls == cls && e->kind == kind) return e;
    }
    return NULL;
}

static int by_kv_count(const void *a, const void *b)
{
    uint32_t x = (*(kv_t *const *)a)->count, y = (*(kv_t *const *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static int kv_gather(kv_t **out, int kind, int thread, int cls)
{
    int n = 0;
    for (uint32_t i = 0; i < MAX_KV; i++)
        if (kv[i].count && kv[i].kind == kind && kv[i].thread == thread && kv[i].cls == cls) out[n++] = &kv[i];
    qsort(out, n, sizeof *out, by_kv_count);
    return n;
}

static kv_t *g_edges[MAX_KV];
static int g_n_edges, g_tree_lines;

static void tree_node(FILE *f, uint64_t fn, uint32_t count, double total, int depth, uint64_t *path)
{
    char nm[64];
    frame_name(fn, nm, sizeof nm);
    fprintf(f, "  %5.1f%% %*s%s\n", 100.0 * count / total, depth * 2, "", nm);
    if (++g_tree_lines > 200 || depth >= 40) return;
    path[depth] = fn;
    for (int i = 0; i < g_n_edges; i++) {
        kv_t *e = g_edges[i];
        if (e->count < total * 0.01) break;                    /* sorted: the rest is smaller */
        if (e->a != fn) continue;
        int loop = 0;
        for (int d = 0; d <= depth && !loop; d++) loop = path[d] == e->b;
        if (!loop) tree_node(f, e->b, e->count, total, depth + 1, path);
    }
}

static void write_inclusive(FILE *f, int t, int cls, double total)
{
    static kv_t *incl[MAX_KV], *roots[MAX_KV];
    int ni = kv_gather(incl, K_INCL, t, cls);
    g_n_edges = kv_gather(g_edges, K_EDGE, t, cls);
    fprintf(f, "  inclusive time (function + everything it calls; self = in the function itself; caller = most frequent):\n");
    for (int i = 0; i < ni && i < 45 && incl[i]->count >= total * 0.005; i++) {
        uint64_t fn = incl[i]->a;
        kv_t *s = kv_find(K_SELF, fn, 0, t, cls);
        kv_t *best = NULL;
        for (int k = 0; k < g_n_edges; k++)
            if (g_edges[k]->b == fn) { best = g_edges[k]; break; }   /* sorted: first is the largest */
        char nm[64], cn[64] = "-";
        frame_name(fn, nm, sizeof nm);
        if (best) frame_name(best->a, cn, sizeof cn);
        fprintf(f, "  %5.1f%%  self %5.1f%%  %-22s  caller %s (%.0f%%)\n", 100.0 * incl[i]->count / total,
                s ? 100.0 * s->count / total : 0.0, nm, cn, best ? 100.0 * best->count / incl[i]->count : 0.0);
    }
    static kv_t *pcs[MAX_KV], *selfs[MAX_KV];
    int np = kv_gather(pcs, K_PC, t, cls), ns = kv_gather(selfs, K_SELF, t, cls), shown = 0;
    if (np) fprintf(f, "  hottest instructions of the top self functions (exe rva: %% of the thread):\n");
    for (int i = 0; np && i < ns && shown < 8 && selfs[i]->count >= total * 0.01; i++) {
        if (selfs[i]->a >> 61) continue;                       /* other modules, this DLL */
        shown++;
        char nm[64];
        frame_name(selfs[i]->a, nm, sizeof nm);
        fprintf(f, "    %s (self %.1f%%):", nm, 100.0 * selfs[i]->count / total);
        for (int j = 0, k = 0; j < np && k < 8; j++)
            if (pcs[j]->a == selfs[i]->a) {
                fprintf(f, " 0x%llx %.1f%%", (unsigned long long)pcs[j]->b, 100.0 * pcs[j]->count / total);
                k++;
            }
        fprintf(f, "\n");
    }
    int nr = kv_gather(roots, K_ROOT, t, cls);
    fprintf(f, "  call tree from the outermost frames (inclusive %% of the thread, branches >= 1%%):\n");
    g_tree_lines = 0;
    uint64_t path[48];
    for (int i = 0; i < nr && roots[i]->count >= total * 0.01; i++) {
        kv_t *r = kv_find(K_INCL, roots[i]->a, 0, t, cls);
        tree_node(f, roots[i]->a, r ? r->count : roots[i]->count, total, 0, path);
    }
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
            if (w->wall_samples[cls] >= 2000) write_inclusive(f, t, cls, (double)w->wall_samples[cls]);
        }
    }
    if (kv_dropped) fprintf(f, "\n(inclusive tables full: %u stack entries dropped)\n", kv_dropped);
    if (unwind_faults) fprintf(f, "\n%ld samples dropped: stack of the thread not readable (thread exiting)\n", unwind_faults);
    fclose(f);
}

static DWORD WINAPI sampler_thread(LPVOID p)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s", (const char *)p);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    sampler_tid = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, sampler_veh);   /* first: before crashguard logs the fault */
    ULONGLONG last_refresh = 0, last_report = GetTickCount64();
    for (;;) {
        Sleep((DWORD)g_cfg.sampler_ms);
        ULONGLONG now = GetTickCount64();
        if (now - last_refresh > 2000) { refresh_images(); refresh_threads(); last_refresh = now; }
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
    exe_img = (image_t){ (uintptr_t)g_base, image_size, pdata, n_pdata };
    HMODULE self = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)sampler_install, &self) && self) {
        uint8_t *sb = (uint8_t *)self;
        IMAGE_NT_HEADERS64 *snt = (IMAGE_NT_HEADERS64 *)(sb + ((IMAGE_DOS_HEADER *)sb)->e_lfanew);
        IMAGE_DATA_DIRECTORY sx = snt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        self_img = (image_t){ (uintptr_t)sb, snt->OptionalHeader.SizeOfImage,
                              (RUNTIME_FUNCTION *)(sb + sx.VirtualAddress), sx.Size / sizeof(RUNTIME_FUNCTION) };
        if (!sx.VirtualAddress) self_img.n = 0;
    }
    add_image(exe_img);
    if (self_img.n) add_image(self_img);
    table = VirtualAlloc(NULL, sizeof(entry_t) * MAX_KEYS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    kv = VirtualAlloc(NULL, sizeof(kv_t) * MAX_KV, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!table || !kv || !n_pdata) return 0;
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
