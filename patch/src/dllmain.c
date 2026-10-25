#include "stwpatch.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

stw_config g_cfg;
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
    GetPrivateProfileStringA("misc", "version_suffix", "Cata-ptimised", g_cfg.version_suffix, sizeof g_cfg.version_suffix, ini);
    g_cfg.net_monitor         = GetPrivateProfileIntA("network", "monitor", 1, ini);
    g_cfg.net_send_buffer_kb  = GetPrivateProfileIntA("network", "send_buffer_kb", 4096, ini);
    g_cfg.net_rate_max_kb     = GetPrivateProfileIntA("network", "send_rate_max_kb", 4096, ini);
    g_cfg.net_rate_min_kb     = GetPrivateProfileIntA("network", "send_rate_min_kb", 512, ini);
    if (g_cfg.sampler_ms && g_cfg.sampler_ms < 2) g_cfg.sampler_ms = 2;
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
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    read_config(dir);
    log_printf("stwpatch loaded in %s, image base %p, vehicle threads=%d, heap_track=%d",
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
    if (g_cfg.narrow_threads > 0) {
        narrowphase_install();
    }
    if (g_cfg.phys_profile || g_cfg.solver_iterations > 0 || g_cfg.solver_residual > 0 || g_cfg.phys_rate) {
        physics_install();
    }

    if (g_cfg.sampler_ms) sampler_install();
    if (g_cfg.net_monitor) net_install();
    if (!is_server) branding_install();
    crashguard_stage("game-start");
    HANDLE h = CreateThread(NULL, 0, late_init, NULL, 0, NULL);
    if (h) CloseHandle(h);
    return TRUE;
}
