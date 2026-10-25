/* Crash diagnostics and automatic safe mode.
 *
 * - A vectored exception handler logs fatal-looking exceptions (access violation, illegal
 *   instruction, stack overflow, ...) with module+offset and the return addresses found on the
 *   stack, so a crash on a machine we cannot debug still says where it happened. It only logs and
 *   lets the exception continue; the game's own handler still writes crash.dmp.
 * - stwpatch.running records how far a session got ("stable" from the first game tick, or after
 *   120 s). A launch that finds it with a stage other than "stable" means the previous one died
 *   during startup: this launch runs with every patch off
 *   (the game should start normally) and the log says so. The next launch tries again. */
#include "stwpatch.h"
#include <stdio.h>
#include <string.h>

static char marker_path[MAX_PATH];
static volatile LONG logged;
static HMODULE self;

static void describe(uintptr_t a, char *out, size_t n)
{
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, '\\');
        snprintf(out, n, "%s+0x%llx", b ? b + 1 : name, (unsigned long long)(a - (uintptr_t)m));
    } else {
        snprintf(out, n, "0x%llx (no module: trampoline or heap)", (unsigned long long)a);
    }
}

static int in_module(HMODULE m, uintptr_t a)
{
    if (!m) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    return a >= (uintptr_t)m && a < (uintptr_t)m + nt->OptionalHeader.SizeOfImage;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO: case 0xC0000409 /* fast fail */:
    case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_DATATYPE_MISALIGNMENT:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&logged) > 8) return EXCEPTION_CONTINUE_SEARCH;
    char where[MAX_PATH + 32];
    describe((uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof where);
    CONTEXT *c = ep->ContextRecord;
    char extra[64] = "";
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        snprintf(extra, sizeof extra, " (%s 0x%llx)",
                 ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute" :
                 ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                 (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    log_printf("EXCEPTION 0x%08lx at %s%s, thread %lu (first chance, may be handled by the game)",
               code, where, extra, GetCurrentThreadId());
    log_printf("  rax=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx rsp=%llx", c->Rax, c->Rcx, c->Rdx, c->R8, c->R9, c->Rsp);
    /* return addresses into the exe or into stwpatch, scanning the stack */
    if (code != EXCEPTION_STACK_OVERFLOW) {
        uintptr_t *sp = (uintptr_t *)c->Rsp;
        int shown = 0;
        for (int i = 0; i < 512 && shown < 12; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            if (i % 64 == 0 && (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)) break;
            uintptr_t v = sp[i];
            if (in_module((HMODULE)g_base, v) || in_module(self, v)) {
                describe(v, where, sizeof where);
                log_printf("  stack[%d] %s", i, where);
                shown++;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void write_marker(const char *stage)
{
    FILE *f = fopen(marker_path, "w");
    if (f) { fprintf(f, "%s\n", stage); fclose(f); }
}

static DWORD WINAPI stable_timer(LPVOID p)
{
    (void)p;
    Sleep(120000);
    write_marker("stable");
    return 0;
}

/* Returns 1 when this launch must run without patches. */
int crashguard_init(HMODULE dll, const char *dir)
{
    self = dll;
    AddVectoredExceptionHandler(0, veh);
    snprintf(marker_path, sizeof marker_path, "%s\\%s.running", dir, g_prefix);
    char prev[32] = "";
    FILE *f = fopen(marker_path, "r");
    if (f) { if (!fgets(prev, sizeof prev, f)) prev[0] = 0; fclose(f); }
    prev[strcspn(prev, "\r\n")] = 0;
    int safe = 0;
    if (prev[0]) {
        if (strcmp(prev, "stable") == 0) {
            log_printf("note: the previous session did not exit cleanly (after startup)");
        } else if (strcmp(prev, "safe") == 0) {
            log_printf("note: the previous launch ran in safe mode; trying the patches again");
        } else if (g_cfg.safe_mode) {
            log_printf("WARNING: the previous launch died during startup (stage '%s'). "
                       "SAFE MODE: every patch is disabled for this launch. Send this log.", prev);
            safe = 1;
        }
    }
    write_marker(safe ? "safe" : "starting");
    if (!safe) {
        HANDLE h = CreateThread(NULL, 0, stable_timer, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return safe;
}

void crashguard_stage(const char *stage) { write_marker(stage); }

void crashguard_clean_exit(void) { DeleteFileA(marker_path); }
