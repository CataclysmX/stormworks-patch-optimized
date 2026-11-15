#include "stwpatch.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <psapi.h>
#include <mimalloc.h>
#include <tlhelp32.h>
size_t _mi_current_thread_count(void);   /* mimalloc internal: threads with a heap */

stw_config g_cfg;
char g_ini_path[MAX_PATH];
DWORD g_main_tid;
uint8_t *g_base;
const char *g_prefix = "stwpatch";
ULONGLONG g_start_ms;

static CRITICAL_SECTION g_log_cs;
static FILE *g_log;

void log_printf(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_log_cs);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

/* Copies `stolen` bytes of position-independent prologue into an executable stub followed
 * by an absolute jump back, then overwrites the prologue with an absolute jump to `hook`
 * (done by the caller through patch_jump). Returns NULL if the bytes are not the expected ones. */
void *make_trampoline(uint8_t *target, size_t stolen, const uint8_t *expect)
{
    if (memcmp(target, expect, stolen) != 0) return NULL;
    uint8_t *t = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, target, stolen);
    uint8_t *j = t + stolen;
    j[0] = 0xff; j[1] = 0x25; *(uint32_t *)(j + 2) = 0;   /* jmp [rip+0] */
    *(uint64_t *)(j + 6) = (uint64_t)(target + stolen);
    return t;
}

int patch_jump(uint8_t *at, size_t len, void *to)
{
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    at[0] = 0xff; at[1] = 0x25; *(uint32_t *)(at + 2) = 0;   /* jmp [rip+0] */
    *(uint64_t *)(at + 6) = (uint64_t)to;
    for (size_t i = 14; i < len; i++) at[i] = 0x90;
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return 1;
}

void *patch_ptr(void **slot, void *value)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return NULL;
    void *prev = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return prev;
}

static void read_config(const char *dir)
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%s\\stwpatch.ini", dir);
    snprintf(g_ini_path, sizeof g_ini_path, "%s", ini);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    /* main thread + physics thread are already busy: give the rest, capped at 8 */
    int def = (int)si.dwNumberOfProcessors - 3;
    if (def < 2) def = 2;
    if (def > 8) def = 8;
    g_cfg.vehicle_threads     = GetPrivateProfileIntA("vehicle", "threads", def, ini);
    g_cfg.vehicle_thread_prio = GetPrivateProfileIntA("vehicle", "priority", THREAD_PRIORITY_ABOVE_NORMAL, ini);
    g_cfg.heap_track          = GetPrivateProfileIntA("memory", "track", 0, ini);
    char alloc[32];
    GetPrivateProfileStringA("memory", "allocator", "mimalloc", alloc, sizeof alloc, ini);
    g_cfg.fast_alloc          = _stricmp(alloc, "mimalloc") == 0;
    g_cfg.alloc_purge         = GetPrivateProfileIntA("memory", "allocator_purge", 0, ini);
    g_cfg.heap_sample         = GetPrivateProfileIntA("memory", "sample", 64, ini);
    g_cfg.heap_report_sec     = GetPrivateProfileIntA("memory", "report_seconds", 60, ini);
    g_cfg.timer_1ms           = GetPrivateProfileIntA("misc", "timer_1ms", 1, ini);
    g_cfg.phys_profile        = GetPrivateProfileIntA("physics", "profile", 1, ini);
    g_cfg.phys_report_sec     = GetPrivateProfileIntA("physics", "report_seconds", 10, ini);
    g_cfg.solver_iterations   = GetPrivateProfileIntA("physics", "solver_iterations", 0, ini);
    char buf[64];
    GetPrivateProfileStringA("physics", "solver_residual", "0", buf, sizeof buf, ini);
    g_cfg.solver_residual     = atof(buf);
    if (g_cfg.phys_report_sec < 2) g_cfg.phys_report_sec = 2;
    int ndef = (int)si.dwNumberOfProcessors / 2 - 1;
    if (ndef < 2) ndef = 2;
    if (ndef > 8) ndef = 8;
    g_cfg.narrow_threads      = GetPrivateProfileIntA("physics", "narrowphase_threads", ndef, ini);
    g_cfg.narrow_min_pairs    = GetPrivateProfileIntA("physics", "narrowphase_min_pairs", 64, ini);
    g_cfg.narrow_balance      = GetPrivateProfileIntA("physics", "narrowphase_balance", 1, ini);
    g_cfg.compound_split      = GetPrivateProfileIntA("physics", "compound_split", 1, ini);
    g_cfg.gl_stats            = GetPrivateProfileIntA("profile", "gl_stats", 1, ini);
    g_cfg.editor_wireframe    = GetPrivateProfileIntA("editor", "wireframe", 1, ini);
    g_cfg.fps_cap             = GetPrivateProfileIntA("misc", "fps_cap", 0, ini);
    g_cfg.gl_filter           = GetPrivateProfileIntA("misc", "gl_filter", 1, ini);
    if (g_cfg.narrow_threads > 32) g_cfg.narrow_threads = 32;
    g_cfg.phys_rate           = GetPrivateProfileIntA("physics", "rate", 0, ini);
    GetPrivateProfileStringA("physics", "budget_ms", "12", buf, sizeof buf, ini);
    g_cfg.phys_budget_ms      = atof(buf);
    if (g_cfg.phys_budget_ms < 2) g_cfg.phys_budget_ms = 2;
    if (g_cfg.vehicle_threads > 64) g_cfg.vehicle_threads = 64;
    if (g_cfg.heap_sample < 1) g_cfg.heap_sample = 1;
    if (g_cfg.heap_report_sec < 5) g_cfg.heap_report_sec = 5;
    g_cfg.safe_mode           = GetPrivateProfileIntA("misc", "safe_mode", 1, ini);
    g_cfg.sampler_ms          = GetPrivateProfileIntA("profile", "sampler_ms", 0, ini);
    g_cfg.fix_editor_mirror   = GetPrivateProfileIntA("fixes", "editor_mirror_delete", 1, ini);
    g_cfg.unstick             = GetPrivateProfileIntA("fixes", "unstick_enabled", 0, ini);
    {   /* [misc] overlay_key = F1..F12 (default F3), empty or 0 = no overlay */
        char key[16];
        GetPrivateProfileStringA("misc", "overlay_key", "F3", key, sizeof key, ini);
        int fk = (key[0] == 'F' || key[0] == 'f') ? atoi(key + 1) : 0;
        g_cfg.overlay_vk = fk >= 1 && fk <= 12 ? VK_F1 + fk - 1 : 0;
        GetPrivateProfileStringA("misc", "menu_key", "F4", key, sizeof key, ini);   /* settings menu */
        fk = (key[0] == 'F' || key[0] == 'f') ? atoi(key + 1) : 0;
        g_cfg.menu_vk = fk >= 1 && fk <= 12 ? VK_F1 + fk - 1 : 0;
    }
    GetPrivateProfileStringA("fixes", "unstick_depth", "0.3", buf, sizeof buf, ini);
    g_cfg.unstick_depth       = atof(buf) > 0.05 ? atof(buf) : 0.05;
    GetPrivateProfileStringA("fixes", "unstick_seconds", "1", buf, sizeof buf, ini);
    g_cfg.unstick_seconds     = atof(buf) > 0.1 ? atof(buf) : 0.1;
    GetPrivateProfileStringA("fixes", "unstick_max_radius", "3", buf, sizeof buf, ini);
    g_cfg.unstick_max_radius  = atof(buf);
    GetPrivateProfileStringA("misc", "version_suffix", "Cata-ptimised", g_cfg.version_suffix, sizeof g_cfg.version_suffix, ini);
    g_cfg.net_monitor         = GetPrivateProfileIntA("network", "monitor", 1, ini);
    g_cfg.net_send_buffer_kb  = GetPrivateProfileIntA("network", "send_buffer_kb", 4096, ini);
    g_cfg.net_rate_max_kb     = GetPrivateProfileIntA("network", "send_rate_max_kb", 4096, ini);
    g_cfg.net_rate_min_kb     = GetPrivateProfileIntA("network", "send_rate_min_kb", 512, ini);
    if (g_cfg.sampler_ms && g_cfg.sampler_ms < 2) g_cfg.sampler_ms = 2;
}

const char *stwpatch_build(void)
{
    /* __DATE__ "Nov 14 2026", __TIME__ "09:45:12": the build of this DLL (one compiler run) */
    static char s[48];
    if (!s[0]) {
        static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char *d = __DATE__, *t = __TIME__;
        int m = 0;
        for (int i = 0; i < 12; i++) if (!strncmp(months + 3 * i, d, 3)) m = i + 1;
        snprintf(s, sizeof s, "%s (%02d/%02d/%.2s %.5s)", STWPATCH_VERSION, atoi(d + 4), m, d + 9, t);
    }
    return s;
}

static DWORD WINAPI late_init(LPVOID p)
{
    (void)p;
    if (g_cfg.timer_1ms) {
        HMODULE w = LoadLibraryA("winmm.dll");
        typedef UINT (WINAPI *tbp_t)(UINT);
        tbp_t tbp = w ? (tbp_t)(void *)GetProcAddress(w, "timeBeginPeriod") : NULL;
        log_printf("timer: timeBeginPeriod(1) %s", tbp && tbp(1) == 0 ? "ok" : "failed");
    }
    return 0;
}

/* every 60 s: process and system memory in the log, so that a leak or an out-of-memory end
 * shows in stwpatch.log (2026-11-15: a friend's game died after "a memory leak" and the log had
 * no memory figure at all). Growth is given since the last line and since the first one. */
static DWORD WINAPI memory_monitor(LPVOID arg)
{
    (void)arg;
    double first = -1, last = 0;
    for (;;) {
        Sleep(60000);
        PROCESS_MEMORY_COUNTERS_EX pm;
        memset(&pm, 0, sizeof pm);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm)) continue;
        double priv = pm.PrivateUsage / 1048576.0;
        if (first < 0) first = last = priv;
        char heap[128] = "Windows heap";
        if (g_cfg.fast_alloc && !g_cfg.heap_track) {
            size_t rss, peak_rss, commit, peak_commit;
            mi_process_info(NULL, NULL, NULL, &rss, &peak_rss, &commit, &peak_commit, NULL);
            snprintf(heap, sizeof heap, "mimalloc commit %.0f MB (peak %.0f), purge %s, %zu thread heaps",
                     commit / 1048576.0, peak_commit / 1048576.0, g_cfg.alloc_purge ? "on" : "off",
                     _mi_current_thread_count());
        }
        int threads = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te = { .dwSize = sizeof te };
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == GetCurrentProcessId()) threads++;
            CloseHandle(snap);
        }
        MEMORYSTATUSEX ms = { .dwLength = sizeof ms };
        GlobalMemoryStatusEx(&ms);
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        long thunks = 0, lookups = 0;
        glstats_thunks(&thunks, &lookups);
        log_printf("memory: private %.0f MB (%+.0f since last, %+.0f since the first line), working set %.0f MB, %s; "
                   "system: RAM free %.1f of %.1f GB, commit %.1f of %.1f GB; %d threads, %lu handles, %lu GDI objects; "
                   "GL thunks %ld (%ld lookups)",
                   priv, priv - last, priv - first, pm.WorkingSetSize / 1048576.0, heap,
                   ms.ullAvailPhys / 1073741824.0, ms.ullTotalPhys / 1073741824.0,
                   (ms.ullTotalPageFile - ms.ullAvailPageFile) / 1073741824.0, ms.ullTotalPageFile / 1073741824.0,
                   threads, handles, GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), thunks, lookups);
        last = priv;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH) {
        /* reserved != NULL: the process is exiting normally (not a FreeLibrary) */
        if (g_base && reserved) crashguard_clean_exit();
        return TRUE;
    }
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    char dir[MAX_PATH];
    GetModuleFileNameA(inst, dir, sizeof dir);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;

    /* only act inside the 64-bit game client or dedicated server, never in other hosts */
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *name = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
    int is_server = _stricmp(name, "server64.exe") == 0;
    if (!is_server && _stricmp(name, "stormworks64.exe") != 0) return TRUE;
    /* one copy per process: both the OpenAL64 and steam_api64 proxies may be installed */
    if (FindAtomA("stwpatch_active")) return TRUE;          /* local atom table: per process */
    AddAtomA("stwpatch_active");

    InitializeCriticalSection(&g_log_cs);
    char logpath[MAX_PATH];
    if (is_server) g_prefix = "stwpatch_server";
    snprintf(logpath, sizeof logpath, "%s\\%s.log", dir, g_prefix);
    /* append, so a session survives a relaunch; start over past 2 MiB */
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int big = GetFileAttributesExA(logpath, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 2u << 20);
    g_log = fopen(logpath, big ? "w" : "a");
    if (g_log) fprintf(g_log, "\n======== new session ========\n");

    g_start_ms = GetTickCount64();
    g_main_tid = GetCurrentThreadId();
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    read_config(dir);
    log_printf("stwpatch v%s, git %s, loaded in %s, image base %p, vehicle threads=%d, heap_track=%d",
               stwpatch_build(), STWPATCH_GIT,
               name, g_base, g_cfg.vehicle_threads, g_cfg.heap_track);
    if (crashguard_init(inst, dir)) return TRUE;
    crashguard_stage("resolve");
    resolve_addresses();
    crashguard_stage("hooks");

    /* DllMain of a static import runs before the exe's CRT starts: hooks see every allocation
     * and no game thread exists yet, so patching code here is race-free. */
    if (g_cfg.heap_track) heap_track_install();          /* diagnostic, needs the system heap */
    else if (g_cfg.fast_alloc) fastalloc_install();
    /* code patches use hard-coded RVAs: only for the analysed build. The heap tracker works
     * through the import table and is build independent. */
    if (g_cfg.vehicle_threads > 0) {
        vehicle_pool_install();
    }
    if (g_cfg.narrow_threads > 0 || g_cfg.unstick) {
        narrowphase_install();      /* the dispatch hook also runs unstick */
    }
    if (g_cfg.narrow_threads > 0 && g_cfg.compound_split) compound_split_install();
    if (g_cfg.phys_profile || g_cfg.solver_iterations > 0 || g_cfg.solver_residual > 0 || g_cfg.phys_rate) {
        physics_install();
    }

    if (g_cfg.sampler_ms) sampler_install();
    if (g_cfg.net_monitor) net_install();
    if (!is_server) branding_install();
    if (!is_server && g_cfg.fix_editor_mirror) editor_fix_install();
    if (!is_server) editor_wire_install();
    if (!is_server && (g_cfg.overlay_vk || g_cfg.menu_vk)) overlay_install();
    if (!is_server) glstats_install();                  /* frame limiter, OpenGL statistics */
    crashguard_stage("game-start");
    HANDLE h = CreateThread(NULL, 0, late_init, NULL, 0, NULL);
    if (h) CloseHandle(h);
    h = CreateThread(NULL, 0, memory_monitor, NULL, 0, NULL);
    if (h) CloseHandle(h);
    return TRUE;
}
