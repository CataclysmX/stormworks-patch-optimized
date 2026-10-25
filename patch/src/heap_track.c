/* Sampling allocation tracker.
 *
 * The exe links the CRT statically, so every malloc/new of the game ends in kernel32 HeapAlloc
 * through the exe's IAT. One allocation out of g_cfg.heap_sample (and every allocation of 256 KiB
 * or more) receives a 16-byte header naming its call site (the first two return addresses that
 * fall inside game code, outside the CRT). Live bytes per site are reported periodically; a site
 * whose live bytes keep growing while the game state is steady is a leak candidate, and its RVA
 * can be decompiled directly. Untracked blocks carry no header and pass straight through. */
#include "stwpatch.h"
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

#define HDR_MAGIC   0x5354575041544348ull   /* "STWPATCH" */
#define BIG_ALLOC   (256u * 1024u)
#define SLOTS       (1u << 16)

typedef struct { uint64_t magic; uint32_t slot; uint32_t size; } hdr_t;

typedef struct {
    volatile LONG64 key;            /* (rva1 << 32) | rva2, 0 = free */
    volatile LONG64 live_bytes;
    volatile LONG64 live_count;
    volatile LONG64 allocs;
} site_t;

static site_t *sites;
static LONG64 *baseline;
static volatile LONG overflow;
static unsigned sample_ctr;

typedef LPVOID (WINAPI *heap_alloc_t)(HANDLE, DWORD, SIZE_T);
typedef LPVOID (WINAPI *heap_realloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef BOOL   (WINAPI *heap_free_t)(HANDLE, DWORD, LPVOID);
typedef SIZE_T (WINAPI *heap_size_t)(HANDLE, DWORD, LPCVOID);

static heap_alloc_t   real_alloc;
static heap_realloc_t real_realloc;
static heap_free_t    real_free;
static heap_size_t    real_size;

static inline uint64_t magic_for(const void *user) { return HDR_MAGIC ^ (uint64_t)(uintptr_t)user; }

static inline hdr_t *tracked_hdr(LPCVOID p)
{
    if (!p || ((uintptr_t)p & 15)) return NULL;
    hdr_t *h = (hdr_t *)p - 1;
    return h->magic == magic_for(p) ? h : NULL;
}

static uint32_t site_slot(uint64_t key)
{
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 48) & (SLOTS - 1);
    for (unsigned n = 0; n < SLOTS; n++, i = (i + 1) & (SLOTS - 1)) {
        LONG64 k = sites[i].key;
        if (k == (LONG64)key) return i;
        if (k == 0) {
            LONG64 prev = InterlockedCompareExchange64(&sites[i].key, (LONG64)key, 0);
            if (prev == 0 || prev == (LONG64)key) return i;
        }
    }
    InterlockedIncrement(&overflow);
    return 0;
}

static uint64_t capture_site(void)
{
    void *frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
    uint32_t got[2] = { 0, 0 };
    int k = 0;
    for (USHORT i = 0; i < n && k < 2; i++) {
        uintptr_t a = (uintptr_t)frames[i];
        if (a < (uintptr_t)g_base) continue;
        uintptr_t rva = a - (uintptr_t)g_base;
        if (rva < 0x1000 || rva >= RVA_CRT_END + 0x400000) continue;   /* outside the exe */
        if (rva >= RVA_CRT_BEGIN && rva < RVA_CRT_END) continue;       /* CRT / operator new */
        got[k++] = (uint32_t)rva;
    }
    return ((uint64_t)got[0] << 32) | got[1] | 1ull << 63;   /* bit 63 keeps key != 0 */
}

static LPVOID tag(LPVOID raw, SIZE_T size, uint32_t slot)
{
    hdr_t *h = raw;
    void *user = h + 1;
    h->magic = magic_for(user);
    h->slot = slot;
    h->size = size > 0xffffffffu ? 0xffffffffu : (uint32_t)size;
    InterlockedAdd64(&sites[slot].live_bytes, (LONG64)h->size);
    InterlockedIncrement64(&sites[slot].live_count);
    InterlockedIncrement64(&sites[slot].allocs);
    return user;
}

static void untag(hdr_t *h)
{
    InterlockedAdd64(&sites[h->slot].live_bytes, -(LONG64)h->size);
    InterlockedDecrement64(&sites[h->slot].live_count);
    h->magic = 0;
}

static LPVOID WINAPI hk_alloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    if (size < BIG_ALLOC && (++sample_ctr % (unsigned)g_cfg.heap_sample) != 0)
        return real_alloc(heap, flags, size);
    LPVOID raw = real_alloc(heap, flags, size + sizeof(hdr_t));
    if (!raw) return NULL;
    return tag(raw, size, site_slot(capture_site()));
}

static BOOL WINAPI hk_free(HANDLE heap, DWORD flags, LPVOID p)
{
    hdr_t *h = tracked_hdr(p);
    if (!h) return real_free(heap, flags, p);
    untag(h);
    return real_free(heap, flags, h);
}

static LPVOID WINAPI hk_realloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size)
{
    hdr_t *h = tracked_hdr(p);
    if (!h) return real_realloc(heap, flags, p, size);
    uint32_t slot = h->slot;
    hdr_t saved = *h;
    untag(h);
    LPVOID raw = real_realloc(heap, flags, h, size + sizeof(hdr_t));
    if (!raw) {                      /* block unchanged: restore its accounting */
        *h = saved;
        InterlockedAdd64(&sites[slot].live_bytes, (LONG64)saved.size);
        InterlockedIncrement64(&sites[slot].live_count);
        return NULL;
    }
    InterlockedDecrement64(&sites[slot].allocs);   /* tag() counts it again */
    return tag(raw, size, slot);
}

static SIZE_T WINAPI hk_size(HANDLE heap, DWORD flags, LPCVOID p)
{
    hdr_t *h = tracked_hdr(p);
    if (!h) return real_size(heap, flags, p);
    SIZE_T s = real_size(heap, flags, h);
    return s == (SIZE_T)-1 ? s : s - sizeof(hdr_t);
}

static int cmp_live(const void *a, const void *b)
{
    LONG64 x = sites[*(const uint32_t *)a].live_bytes, y = sites[*(const uint32_t *)b].live_bytes;
    return x < y ? 1 : x > y ? -1 : 0;
}

static LONG64 growth(uint32_t i) { return sites[i].live_bytes - baseline[i]; }

static int cmp_growth(const void *a, const void *b)
{
    LONG64 x = growth(*(const uint32_t *)a), y = growth(*(const uint32_t *)b);
    return x < y ? 1 : x > y ? -1 : 0;
}

static DWORD WINAPI report_thread(LPVOID p)
{
    char *path = p;
    uint32_t *idx = malloc(sizeof *idx * SLOTS);
    int round = 0;
    for (;;) {
        Sleep((DWORD)g_cfg.heap_report_sec * 1000);
        round++;
        uint32_t n = 0;
        LONG64 total = 0;
        for (uint32_t i = 0; i < SLOTS; i++)
            if (sites[i].key) { idx[n++] = i; total += sites[i].live_bytes; }
        if (round == 2)   /* first report covers loading; growth is measured from the second one */
            for (uint32_t i = 0; i < SLOTS; i++) baseline[i] = sites[i].live_bytes;

        PROCESS_MEMORY_COUNTERS_EX pm = { .cb = sizeof pm };
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        if (!pm.PrivateUsage) pm.PrivateUsage = pm.PagefileUsage;   /* Wine leaves it at 0 */

        FILE *f = fopen(path, "w");
        if (!f) continue;
        double scale = g_cfg.heap_sample;
        fprintf(f, "report #%d (every %ds), sample 1/%d (allocations >= 256 KiB always tracked)\n",
                round, g_cfg.heap_report_sec, g_cfg.heap_sample);
        fprintf(f, "process private bytes: %.1f MiB, working set: %.1f MiB\n",
                pm.PrivateUsage / 1048576.0, pm.WorkingSetSize / 1048576.0);
        fprintf(f, "sampled live heap: %.1f MiB (x%d ~ %.1f MiB estimate), sites=%u overflow=%ld\n\n",
                total / 1048576.0, g_cfg.heap_sample, total * scale / 1048576.0, n, overflow);

        if (round >= 2) {
            qsort(idx, n, sizeof *idx, cmp_growth);
            fprintf(f, "== top growth since report #2 (leak candidates) ==\n");
            fprintf(f, "%14s %14s %12s %10s   %-10s %-10s\n", "growth(sampl)", "live(sampl)", "live blocks", "allocs", "site rva", "caller rva");
            for (uint32_t k = 0; k < n && k < 40; k++) {
                uint32_t i = idx[k];
                if (growth(i) <= 0) break;
                uint64_t key = (uint64_t)sites[i].key;
                fprintf(f, "%14lld %14lld %12lld %10lld   0x%08x 0x%08x\n", (long long)growth(i),
                        (long long)sites[i].live_bytes, (long long)sites[i].live_count,
                        (long long)sites[i].allocs, (uint32_t)(key >> 32) & 0x7fffffff, (uint32_t)key);
            }
            fprintf(f, "\n");
        }
        qsort(idx, n, sizeof *idx, cmp_live);
        fprintf(f, "== top live ==\n");
        fprintf(f, "%14s %12s %10s   %-10s %-10s\n", "live(sampl)", "live blocks", "allocs", "site rva", "caller rva");
        for (uint32_t k = 0; k < n && k < 40; k++) {
            uint32_t i = idx[k];
            if (sites[i].live_bytes <= 0) break;
            uint64_t key = (uint64_t)sites[i].key;
            fprintf(f, "%14lld %12lld %10lld   0x%08x 0x%08x\n", (long long)sites[i].live_bytes,
                    (long long)sites[i].live_count, (long long)sites[i].allocs,
                    (uint32_t)(key >> 32) & 0x7fffffff, (uint32_t)key);
        }
        fclose(f);
        log_printf("heap: private %.0f MiB, sampled live %.1f MiB", pm.PrivateUsage / 1048576.0, total / 1048576.0);
    }
    return 0;
}

static ULONGLONG *find_iat(const char *dll, const char *fn)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
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

int heap_track_install(void)
{
    sites = VirtualAlloc(NULL, sizeof(site_t) * SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    baseline = VirtualAlloc(NULL, sizeof(LONG64) * SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!sites || !baseline) return 0;
    /* slot 0 collects everything if the table ever overflows */
    sites[0].key = 1;

    /* all four or none: a tagged block reaching an unhooked HeapFree would corrupt the heap */
    ULONGLONG *ia = find_iat("KERNEL32.dll", "HeapAlloc");
    ULONGLONG *ifr = find_iat("KERNEL32.dll", "HeapFree");
    ULONGLONG *ir = find_iat("KERNEL32.dll", "HeapReAlloc");
    ULONGLONG *is = find_iat("KERNEL32.dll", "HeapSize");
    if (!ia || !ifr || !ir || !is) {
        log_printf("heap tracker: DISABLED, imports not found");
        return 0;
    }
    set_iat(ifr, hk_free, (void **)&real_free);
    set_iat(ir, hk_realloc, (void **)&real_realloc);
    set_iat(is, hk_size, (void **)&real_size);
    set_iat(ia, hk_alloc, (void **)&real_alloc);
    static char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    char *s = strrchr(path, '\\');
    sprintf(s ? s + 1 : path, "%s_heap.txt", g_prefix);
    HANDLE h = CreateThread(NULL, 0, report_thread, path, 0, NULL);
    if (h) CloseHandle(h);
    log_printf("heap tracker: enabled, 1/%d sampling, report every %ds -> %s",
               g_cfg.heap_sample, g_cfg.heap_report_sec, path);
    return 1;
}
