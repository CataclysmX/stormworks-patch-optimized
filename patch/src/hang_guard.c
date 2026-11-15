/* Hang guard ([fixes] hang_guard): two game bugs behind the freeze of 2026-11-15 13:52 (game bug,
 * with or without stwpatch; crash_dump/crash_15_11_cata_traite).
 *
 * 1. FUN_14028cb40 (main thread, ~60 Hz) refreshes a UI list of 16 numbered lines ("N: name",
 *    "N: empty", three bars each). Each line points to an object (name string {char *, uint32 len}
 *    at +0, +0x10 -> -> uint32 total at +8, +0x38, +0x3c, +0x54). That object can be stale: on
 *    2026-11-15 its name length read 0xE625AAA6. FUN_14028cf80 then scanned ~3.9 GB backwards for
 *    a '/' and asked a 3.86-billion-byte buffer.
 *    Fix: at the line's "object == NULL -> empty" test (0x14028cbc6), an object that is not
 *    readable, whose name length is absurd or whose name / +0x10 chain is not readable is shown as
 *    empty, the game's own branch.
 *
 * 2. FUN_14017ca10, the game's buffer reserve (strings and byte arrays), computes the capacity
 *    as "32, doubled while below the request" on 32 bits: any request above 2^31 overflows the
 *    capacity to 0 and the loop never ends (the freeze, until the game's hang detector raised
 *    0x80000101). No caller can use such a buffer (it then writes that many bytes), so the
 *    guard logs the request and its caller and raises an exception: the game's crash reporter
 *    writes its dump at once instead of a frozen game. Requests up to 2^31 run as before.
 *
 * Both sites are found by byte pattern (exactly one match each) and patched in DllMain, before
 * any game thread runs. */
#include "stwpatch.h"
#include <stdio.h>
#include <string.h>

#define NAME_MAX_LEN (1u << 16)          /* the names are file paths; the stale one read 3.86e9 */
#define EXC_HUGE_REQUEST 0xE0571E01u     /* raised by the reserve guard */

static volatile LONG bad_slots;

static int readable(const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p;
    if (a < 0x10000) return 0;
    while (n) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (end - a >= n) return 1;
        n -= end - a;
        a = end;
    }
    return 1;
}

/* called from the slot stub with the line's object; 0 = show the line as empty */
int hang_guard_slot_ok(const uint8_t *o)
{
    uint32_t len = 0;
    if (!readable(o, 0x58)) goto bad;
    const char *name = *(const char *const *)o;
    len = *(const uint32_t *)(o + 8);
    if (len > NAME_MAX_LEN || (len && !readable(name, len))) goto bad;
    const uint8_t *p = *(const uint8_t *const *)(o + 0x10);
    if (p) {
        if (!readable(p, 8)) goto bad;
        const uint8_t *q = *(const uint8_t *const *)p;
        if (q && !readable(q + 8, 4)) goto bad;
    }
    return 1;
bad:
    if (InterlockedIncrement(&bad_slots) <= 10)
        log_printf("hang guard: UI list line object %p is stale or corrupted (name length %u): shown as empty "
                   "(the game would have frozen on it)", (const void *)o, len);
    return 0;
}

/* called from the reserve stub, never returns */
__attribute__((noreturn)) void hang_guard_huge_request(uint32_t n, uint8_t *caller)
{
    log_printf("hang guard: the game asked a buffer of %u bytes (called from exe+0x%llx): its data is "
               "corrupted and the original code would freeze here forever; stopping the game instead",
               n, (unsigned long long)(caller - g_base));
    RaiseException(EXC_HUGE_REQUEST, EXCEPTION_NONCONTINUABLE, 0, NULL);
    for (;;) Sleep(1000);
}

static uint8_t *find_once(uint8_t *p, size_t n, const uint8_t *pat, const uint8_t *mask, size_t len, int *count)
{
    uint8_t *hit = NULL;
    *count = 0;
    for (size_t i = 0; i + len <= n; i++) {
        size_t k = 0;
        while (k < len && (mask[k] ? p[i + k] == pat[k] : 1)) k++;
        if (k == len) { hit = p + i; (*count)++; }
    }
    return hit;
}

static uint8_t *put_jmp_abs(uint8_t *s, const void *to)    /* jmp [rip+0]; dq to */
{
    *s++ = 0xFF; *s++ = 0x25; *s++ = 0; *s++ = 0; *s++ = 0; *s++ = 0;
    uint64_t v = (uint64_t)(uintptr_t)to;
    memcpy(s, &v, 8);
    return s + 8;
}

static uint8_t *put_mov_rax(uint8_t *s, const void *v)      /* mov rax, imm64 */
{
    *s++ = 0x48; *s++ = 0xB8;
    uint64_t x = (uint64_t)(uintptr_t)v;
    memcpy(s, &x, 8);
    return s + 8;
}

int hang_guard_install(void)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt), *text = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".text", 6)) text = &sec[i];
    if (!text) return 0;
    uint8_t *tb = g_base + text->VirtualAddress;
    size_t tn = text->Misc.VirtualSize;

    /* site 1: FUN_14017ca10 + 0xa, the capacity loop (18 bytes, no rip-relative operand) */
    static const uint8_t rpat[] = { 0xBB, 0x20, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x3B, 0xD3, 0x76, 0x06,
                                    0x03, 0xDB, 0x3B, 0xDA, 0x72, 0xFA };
    static const uint8_t rmask[sizeof rpat] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    /* site 2: FUN_14028cb40, "object = *slot; if (!object) goto empty" then two moves (19 bytes) */
    static const uint8_t spat[] = { 0x4D, 0x8B, 0x36, 0x4D, 0x85, 0xF6, 0x0F, 0x84, 0, 0, 0, 0,
                                    0x49, 0x8B, 0x07, 0x48, 0x89, 0x45, 0x78,
                                    0xF3, 0x45, 0x0F, 0x10, 0x46, 0x54 };
    static const uint8_t smask[sizeof spat] = { 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1,
                                                1, 1, 1, 1, 1, 1 };
    int nr, ns;
    uint8_t *r = find_once(tb, tn, rpat, rmask, sizeof rpat, &nr);
    uint8_t *s = find_once(tb, tn, spat, smask, sizeof spat, &ns);
    if (nr != 1) r = NULL;                                                /* each site on its own: */
    if (ns != 1) s = NULL;                                                /* the server has no UI list */
    if (!r && !s) {
        log_printf("hang guard: patterns found %d and %d times (expected 1 and 1), left unchanged (game updated?)", nr, ns);
        return 0;
    }
    uint8_t *stubs = VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stubs) return 0;

    /* reserve stub: the original loop, preceded by "request > 2^31 -> hang_guard_huge_request" */
    uint8_t *a = stubs, *p = a;
    static const uint8_t head[] = { 0xBB, 0x20, 0x00, 0x00, 0x00,         /* mov ebx, 32 */
                                    0x48, 0x8B, 0xF9,                     /* mov rdi, rcx */
                                    0x81, 0xFA, 0x00, 0x00, 0x00, 0x80,   /* cmp edx, 0x80000000 */
                                    0x77, 0x18,                           /* ja bad (+24: past the loop and the jmp back) */
                                    0x3B, 0xD3, 0x76, 0x06,               /* cmp edx, ebx; jbe +6 */
                                    0x03, 0xDB, 0x3B, 0xDA, 0x72, 0xFA }; /* add ebx, ebx; cmp ebx, edx; jb -6 */
    memcpy(p, head, sizeof head); p += sizeof head;
    p = put_jmp_abs(p, r ? r + sizeof rpat : NULL);                       /* back to "cmp ebx, [rcx+0xc]" */
    /* bad: rsp is 16-aligned here (push rdi + sub rsp, 20h after the call), caller at [rsp+28h] */
    *p++ = 0x8B; *p++ = 0xCA;                                             /* mov ecx, edx */
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x54; *p++ = 0x24; *p++ = 0x28;      /* mov rdx, [rsp+28h] */
    p = put_mov_rax(p, (void *)hang_guard_huge_request);
    *p++ = 0xFF; *p++ = 0xD0;                                             /* call rax */
    *p++ = 0xCC;                                                          /* int3: never reached */
    _Static_assert(16 + 0x18 == sizeof head + 14, "the ja must land on bad");

    /* slot stub: the original test, then hang_guard_slot_ok(r14) for a non-null object */
    uint8_t *b = stubs + 128;
    uint8_t *empty = s ? s + 12 + *(int32_t *)(s + 8) : NULL;             /* the je's target */
    p = b;
    static const uint8_t pre[] = { 0x4D, 0x8B, 0x36,                      /* mov r14, [r14] */
                                   0x4D, 0x85, 0xF6,                      /* test r14, r14 */
                                   0x74, 0x46,                            /* je empty (t+78) */
                                   0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,  /* push rax..r11 */
                                   0x48, 0x83, 0xEC, 0x28,                /* sub rsp, 28h: 16-aligned call */
                                   0x4C, 0x89, 0xF1 };                    /* mov rcx, r14 */
    memcpy(p, pre, sizeof pre); p += sizeof pre;
    p = put_mov_rax(p, (void *)hang_guard_slot_ok);
    static const uint8_t post[] = { 0xFF, 0xD0,                           /* call rax */
                                    0x48, 0x83, 0xC4, 0x28,               /* add rsp, 28h */
                                    0x85, 0xC0,                           /* test eax, eax (pops keep flags) */
                                    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58,  /* pop r11..rax */
                                    0x74, 0x15,                           /* je empty (t+78) */
                                    0x49, 0x8B, 0x07,                     /* mov rax, [r15] */
                                    0x48, 0x89, 0x45, 0x78 };             /* mov [rbp+78h], rax */
    memcpy(p, post, sizeof post); p += sizeof post;
    p = put_jmp_abs(p, s ? s + 19 : NULL);                                /* back to "movss xmm8, [r14+54h]" */
    if (p != b + 78) return 0;                                            /* the two je land here */
    p = put_jmp_abs(p, empty);
    FlushInstructionCache(GetCurrentProcess(), stubs, 256);

    int ok_r = r && patch_jump(r, sizeof rpat, a), ok_s = s && patch_jump(s, 19, b);
    log_printf("hang guard: reserve %s%llx (no endless loop above 2^31), UI list %s%llx (stale line objects shown empty)",
               ok_r ? "on at +0x" : "NOT patched, ", ok_r ? (unsigned long long)(r - g_base) : (unsigned long long)nr,
               ok_s ? "on at +0x" : "NOT patched, ", ok_s ? (unsigned long long)(s - g_base) : (unsigned long long)ns);
    return ok_r || ok_s;
}
