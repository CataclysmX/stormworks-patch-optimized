/* Test of hang_guard.c. The two game sites are rebuilt byte for byte in this exe's .text:
 *   fake_reserve(buf, n): FUN_14017ca10's prologue and capacity loop, returns the capacity
 *   fake_slot(slot, r15): FUN_14028cb40's "object = *slot; if (!object) goto empty; two moves;
 *                         movss xmm8, [object+54h]", returns [r15] (line drawn) or 0 (empty)
 * hang_guard_install() finds and patches them like in the game; then: normal capacities
 * unchanged, 2^31 still served, a request above 2^31 raises the guard's exception instead of
 * looping forever, valid objects drawn, stale / wild ones shown empty, registers kept.
 *   make tests && wine build/hang_guard_test.exe */
#include "../src/hang_guard.c"
#include <stdio.h>
#include <stdlib.h>

stw_config g_cfg;
uint8_t *g_base;
void log_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("  log: "); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
int patch_jump(uint8_t *at, size_t len, void *to)
{
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    at[0] = 0xff; at[1] = 0x25; *(uint32_t *)(at + 2) = 0;
    *(uint64_t *)(at + 6) = (uint64_t)to;
    for (size_t i = 14; i < len; i++) at[i] = 0x90;
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return 1;
}

uint32_t fake_reserve(void *buf, uint32_t n);
uint64_t fake_slot(void **slot, uint64_t *r15);
__asm__(".text\n.p2align 4\n.globl fake_reserve\nfake_reserve:\n"
        ".byte 0x48,0x89,0x5C,0x24,0x08\n"                 /* mov [rsp+8], rbx */
        ".byte 0x57\n"                                     /* push rdi */
        ".byte 0x48,0x83,0xEC,0x20\n"                      /* sub rsp, 20h */
        ".byte 0xBB,0x20,0x00,0x00,0x00\n"                 /* mov ebx, 32        <- site */
        ".byte 0x48,0x8B,0xF9\n"                           /* mov rdi, rcx */
        ".byte 0x3B,0xD3,0x76,0x06\n"                      /* cmp edx, ebx; jbe */
        ".byte 0x03,0xDB,0x3B,0xDA,0x72,0xFA\n"            /* add ebx, ebx; cmp ebx, edx; jb */
        ".byte 0x3B,0x59,0x0C\n"                           /* cmp ebx, [rcx+0ch]  (resume) */
        ".byte 0x89,0xD8\n"                                /* mov eax, ebx */
        ".byte 0x48,0x8B,0x5C,0x24,0x30\n"                 /* mov rbx, [rsp+30h] */
        ".byte 0x48,0x83,0xC4,0x20\n"                      /* add rsp, 20h */
        ".byte 0x5F,0xC3\n"                                /* pop rdi; ret */
        ".p2align 4\n.globl fake_slot\nfake_slot:\n"
        ".byte 0x55,0x41,0x56,0x41,0x57\n"                 /* push rbp, r14, r15 */
        ".byte 0x48,0x81,0xEC,0x90,0x00,0x00,0x00\n"       /* sub rsp, 90h (16-aligned body) */
        ".byte 0x48,0x89,0xE5\n"                           /* mov rbp, rsp */
        ".byte 0x44,0x0F,0x11,0x44,0x24,0x10\n"            /* movups [rsp+10h], xmm8 */
        ".byte 0x49,0x89,0xCE\n"                           /* mov r14, rcx */
        ".byte 0x49,0x89,0xD7\n"                           /* mov r15, rdx */
        ".byte 0x4D,0x8B,0x36,0x4D,0x85,0xF6\n"            /* mov r14, [r14]; test r14, r14   <- site */
        ".byte 0x0F,0x84,0x13,0x00,0x00,0x00\n"            /* je empty */
        ".byte 0x49,0x8B,0x07,0x48,0x89,0x45,0x78\n"       /* mov rax, [r15]; mov [rbp+78h], rax */
        ".byte 0xF3,0x45,0x0F,0x10,0x46,0x54\n"            /* movss xmm8, [r14+54h]  (resume) */
        ".byte 0x48,0x8B,0x45,0x78\n"                      /* mov rax, [rbp+78h] */
        ".byte 0xEB,0x02\n"                                /* jmp out */
        ".byte 0x31,0xC0\n"                                /* empty: xor eax, eax */
        ".byte 0x44,0x0F,0x10,0x44,0x24,0x10\n"            /* out: movups xmm8, [rsp+10h] */
        ".byte 0x48,0x81,0xC4,0x90,0x00,0x00,0x00\n"       /* add rsp, 90h */
        ".byte 0x41,0x5F,0x41,0x5E,0x5D,0xC3\n");          /* pop r15, r14, rbp; ret */

static int fails;
static void expect(const char *what, long long got, long long want)
{
    printf("%-72s %s (%lld)\n", what, got == want ? "ok" : "FAIL", got);
    if (got != want) fails++;
}

static void *jb[5];
static volatile LONG raised;
__attribute__((noinline, noreturn)) static void escape(void) { __builtin_longjmp(jb, 1); }
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXC_HUGE_REQUEST) return EXCEPTION_CONTINUE_SEARCH;
    raised++;
    ep->ContextRecord->Rip = (DWORD64)(uintptr_t)escape;
    return EXCEPTION_CONTINUE_EXECUTION;
}

typedef struct { uint8_t *data; uint32_t len, cap; } buf_t;

static uint8_t *object(const char *name, uint32_t len, void *p10)
{
    uint8_t *o = calloc(1, 0x58);
    *(const char **)o = name;
    *(uint32_t *)(o + 8) = len;
    *(void **)(o + 0x10) = p10;
    *(float *)(o + 0x54) = 1.5f;
    return o;
}

int main(void)
{
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    expect("both sites found and patched", hang_guard_install(), 1);
    expect("  reserve site now starts with jmp [rip]", ((uint8_t *)fake_reserve)[10] == 0xFF, 1);

    buf_t b = { 0, 0, 0 };
    expect("reserve 5 -> 32", fake_reserve(&b, 5), 32);
    expect("reserve 33 -> 64", fake_reserve(&b, 33), 64);
    expect("reserve 1000 -> 1024", fake_reserve(&b, 1000), 1024);
    expect("reserve 2^30+1 -> 2^31", fake_reserve(&b, (1u << 30) + 1), 1u << 31);
    expect("reserve 2^31 -> 2^31 (served as before)", fake_reserve(&b, 1u << 31), 1u << 31);
    AddVectoredExceptionHandler(1, veh);
    int returned = 0;
    if (!__builtin_setjmp(jb)) { fake_reserve(&b, 0xE625AAA7u); returned = 1; }
    expect("reserve 0xE625AAA7 (the crash): guard exception, no endless loop", raised == 1 && !returned, 1);
    raised = 0;
    if (!__builtin_setjmp(jb)) { fake_reserve(&b, 0x80000001u); returned = 1; }
    expect("reserve 2^31+1: guard exception", raised == 1 && !returned, 1);

    uint64_t mark = 0x1234567890ABCDEFull;
    void *slot = NULL;
    expect("UI line: no object -> empty (the game's own test)", fake_slot(&slot, &mark), 0);
    static const char name[] = "data/audio/engine_loop.ogg";
    slot = object(name, sizeof name - 1, NULL);
    expect("UI line: valid object -> drawn, moves done", fake_slot(&slot, &mark) == mark, 1);
    uint32_t total = 77;
    uint8_t *q = calloc(1, 16); *(uint32_t *)(q + 8) = total;
    uint8_t **p10 = calloc(1, 8); *p10 = q;
    slot = object(name, sizeof name - 1, p10);
    expect("UI line: valid object with its +10h chain -> drawn", fake_slot(&slot, &mark) == mark, 1);
    slot = object(name, 0xE625AAA6u, NULL);
    expect("UI line: name length 0xE625AAA6 (the crash) -> empty", fake_slot(&slot, &mark), 0);
    slot = object((const char *)(uintptr_t)0x7ff000000000ull, 12, NULL);
    expect("UI line: name pointer not readable -> empty", fake_slot(&slot, &mark), 0);
    slot = object(name, sizeof name - 1, (void *)(uintptr_t)0x7ff000001000ull);
    expect("UI line: +10h pointer not readable -> empty", fake_slot(&slot, &mark), 0);
    slot = (void *)(uintptr_t)0x7ff000002000ull;
    expect("UI line: object itself not readable -> empty (the original would fault)", fake_slot(&slot, &mark), 0);
    expect("stale lines counted", bad_slots, 4);
    printf("RESULT: %s\n", fails ? "FAILED" : "OK");
    return fails != 0;
}
