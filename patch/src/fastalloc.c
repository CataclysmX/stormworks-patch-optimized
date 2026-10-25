/* Faster allocator for the game: the exe links the CRT statically and every malloc/new ends in
 * kernel32 HeapAlloc on the process heap, through the exe's import table. Loading a save or a
 * vehicle is XML deserialisation (millions of small allocations; the sampler shows ~40% of the
 * main thread in ntdll during server start-up), and every tick allocates per-vehicle tasks.
 * The four heap imports are pointed at mimalloc (third_party/mimalloc, MIT, Microsoft).
 * Installed from DllMain, before the exe's CRT starts, so every CRT block comes from mimalloc;
 * a block mimalloc does not own (other heaps, anything allocated elsewhere) goes to the system. */
#include "stwpatch.h"
#include <mimalloc.h>

typedef LPVOID (WINAPI *heap_alloc_t)(HANDLE, DWORD, SIZE_T);
typedef LPVOID (WINAPI *heap_realloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef BOOL   (WINAPI *heap_free_t)(HANDLE, DWORD, LPVOID);
typedef SIZE_T (WINAPI *heap_size_t)(HANDLE, DWORD, LPCVOID);

static heap_alloc_t   real_alloc;
static heap_realloc_t real_realloc;
static heap_free_t    real_free;
static heap_size_t    real_size;
static HANDLE         process_heap;

static LPVOID WINAPI fa_alloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    if (heap != process_heap) return real_alloc(heap, flags, size);
    return (flags & HEAP_ZERO_MEMORY) ? mi_zalloc(size) : mi_malloc(size);
}

static BOOL WINAPI fa_free(HANDLE heap, DWORD flags, LPVOID p)
{
    if (!p) return TRUE;
    if (mi_is_in_heap_region(p)) { mi_free(p); return TRUE; }
    return real_free(heap, flags, p);
}

static LPVOID WINAPI fa_realloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size)
{
    if (!p || !mi_is_in_heap_region(p)) return real_realloc(heap, flags, p, size);
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) return mi_expand(p, size);
    return (flags & HEAP_ZERO_MEMORY) ? mi_rezalloc(p, size) : mi_realloc(p, size);
}

static SIZE_T WINAPI fa_size(HANDLE heap, DWORD flags, LPCVOID p)
{
    if (p && mi_is_in_heap_region(p)) return mi_usable_size(p);
    return real_size(heap, flags, p);
}

static ULONGLONG *find_iat(const char *dll, const char *fn)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(g_base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(g_base + d->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA64 *names = (void *)(g_base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (void *)(g_base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (void *)(g_base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, fn) == 0) return &iat->u1.Function;
        }
    }
    return NULL;
}

static void set_iat(ULONGLONG *slot, void *hook, void **orig)
{
    DWORD old;
    VirtualProtect(slot, 8, PAGE_READWRITE, &old);
    *orig = (void *)*slot;
    *slot = (ULONGLONG)hook;
    VirtualProtect(slot, 8, old, &old);
}

int fastalloc_install(void)
{
    ULONGLONG *ia = find_iat("KERNEL32.dll", "HeapAlloc"), *ifr = find_iat("KERNEL32.dll", "HeapFree");
    ULONGLONG *ir = find_iat("KERNEL32.dll", "HeapReAlloc"), *is = find_iat("KERNEL32.dll", "HeapSize");
    if (!ia || !ifr || !ir || !is) {
        log_printf("allocator: mimalloc NOT installed (heap imports not found)");
        return 0;
    }
    process_heap = GetProcessHeap();
    mi_option_disable(mi_option_show_errors);
    /* free/size/realloc first: once alloc is switched, every new block is a mimalloc block */
    set_iat(ifr, fa_free, (void **)&real_free);
    set_iat(ir, fa_realloc, (void **)&real_realloc);
    set_iat(is, fa_size, (void **)&real_size);
    set_iat(ia, fa_alloc, (void **)&real_alloc);
    log_printf("allocator: mimalloc %d installed for the CRT heap", mi_version());
    return 1;
}
