/* Resolver for the opengl32 proxy (server injection vector, see opengl32_thunks.S).
 * server64.exe imports nothing else that can be served from its own folder: KERNEL32, USER32,
 * COMDLG32, SHELL32 and WS2_32 are KnownDLLs, and steam_api64.dll is SHA-1 checked by the game. */
#include "stwpatch.h"

static HMODULE real_gl;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_real(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH - 16);
    if (n && n < MAX_PATH - 16) {
        lstrcatW(path, L"\\opengl32.dll");
        real_gl = LoadLibraryW(path);
    }
    return TRUE;
}

static int gl_missing(void) { return 0; }

void *gl_resolve(const char *name, void **slot)
{
    InitOnceExecuteOnce(&once, load_real, NULL, NULL);
    void *f = real_gl ? (void *)GetProcAddress(real_gl, name) : NULL;
    if (!f) f = (void *)gl_missing;
    InterlockedExchangePointer(slot, f);
    return f;
}
