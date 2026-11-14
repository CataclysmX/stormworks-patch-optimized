/* Game bug fix: vehicle editor, deleting parts with symmetry on.
 *
 * c_game_state_vehicle_editor (vtable slot 12, FUN_1407f6c10 in the 2026-10-29 build) deletes the
 * selected parts and, when a symmetry axis is set, their mirrored parts (FUN_1407fc1c0 finds the
 * mirror; it is never the part itself nor another selected part). For each part it calls a
 * definition hook, FUN_14080c1c0(editor, part->definition at +0x58), and deletes the part with
 * FUN_1404c2a80, which removes it from its body through FUN_1404ac3d0 = the part's deleting
 * destructor. For the selected part the definition is read before the delete; for the mirrored
 * part it is read AFTER, from the freed object:
 *
 *   mov  [rsp+20h], REG        ; 5th argument: the mirrored part
 *   mov  rdx, [rsi+8]
 *   mov  rcx, [rsi+13C8h]
 *   call delete_part           ; frees REG
 *   mov  rdx, [REG+58h]        ; use after free (crash 2026-11-08 at stormworks64.exe+0x7f9b73)
 *   mov  rcx, rsi
 *   call definition_hook
 *   ...                        ; REG is overwritten next: its value is dead
 *
 * The Windows heap leaves the freed bytes in place, so the game usually reads the right pointer;
 * it crashes when the page was given back to the OS or the block reused in between.
 * Two sites: the selection (REG = r14) and a single part (REG = rdi). The fix loads the
 * definition into REG before the call (REG is callee-saved and the part travels on the stack)
 * and passes REG after it: same calls, same order, same value, no read of freed memory. */
#include "stwpatch.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *pattern;    /* hex bytes, ?? = any; ends with the instruction that kills REG */
    uint8_t load[4];        /* mov REG, [REG+58h] */
    uint8_t pass[4];        /* mov rdx, REG ; nop   (replaces mov rdx, [REG+58h]) */
} site_t;

#define OFF_CALL_DELETE 16
#define OFF_READ        21
#define OFF_CALL_HOOK   28

static const site_t sites[] = {
    { "selection",
      "4c 89 74 24 20 48 8b 56 08 48 8b 8e ?? ?? ?? ?? e8 ?? ?? ?? ?? 49 8b 56 58 48 8b ce e8 ?? ?? ?? ?? "
      "44 8b 75 ??",                                                     /* mov r14d, [rbp+xx] */
      { 0x4d, 0x8b, 0x76, 0x58 }, { 0x4c, 0x89, 0xf2, 0x90 } },
    { "single part",
      "48 89 7c 24 20 48 8b 56 08 48 8b 8e ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8b 57 58 48 8b ce e8 ?? ?? ?? ?? "
      "b9 c8 00 00 00 e8 ?? ?? ?? ?? 48 89 45 ?? 4c 8d 86 ?? ?? ?? ?? 48 8b d3 48 8b c8 e8 ?? ?? ?? ?? "
      "48 8b f8",                                                        /* mov rdi, rax */
      { 0x48, 0x8b, 0x7f, 0x58 }, { 0x48, 0x89, 0xfa, 0x90 } },
};
#define NSITES (sizeof sites / sizeof sites[0])

static size_t parse(const char *s, uint8_t *pat, uint8_t *mask, size_t max)
{
    size_t n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { pat[n] = 0; mask[n] = 0; }
        else { pat[n] = (uint8_t)strtoul((char[3]){ s[0], s[1], 0 }, NULL, 16); mask[n] = 1; }
        n++;
        s += 2;
    }
    return n;
}

static uint8_t *find(const uint8_t *text, size_t size, const uint8_t *pat, const uint8_t *mask, size_t len, int *count)
{
    uint8_t *hit = NULL;
    *count = 0;
    for (size_t i = 0; i + len <= size; i++) {
        if (text[i] != pat[0]) continue;
        size_t k = 1;
        while (k < len && (!mask[k] || text[i + k] == pat[k])) k++;
        if (k == len) { hit = (uint8_t *)text + i; (*count)++; }
    }
    return hit;
}

static uint8_t *call_target(uint8_t *call) { return call + 5 + *(int32_t *)(call + 1); }

int editor_fix_install(void)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt), *text = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!strncmp((char *)sec[i].Name, ".text", 8)) text = &sec[i];
    if (!text) return 0;

    uint8_t *at[NSITES];
    for (size_t i = 0; i < NSITES; i++) {
        uint8_t pat[96], mask[96];
        size_t len = parse(sites[i].pattern, pat, mask, sizeof pat);
        int n;
        at[i] = find(g_base + text->VirtualAddress, text->Misc.VirtualSize, pat, mask, len, &n);
        if (n != 1) {
            log_printf("fix: vehicle editor symmetric delete: '%s' site found %d times, left unchanged (game updated?)",
                       sites[i].name, n);
            return 0;
        }
    }
    /* both sites must call the same delete and the same hook */
    for (size_t i = 1; i < NSITES; i++)
        if (call_target(at[i] + OFF_CALL_DELETE) != call_target(at[0] + OFF_CALL_DELETE) ||
            call_target(at[i] + OFF_CALL_HOOK) != call_target(at[0] + OFF_CALL_HOOK)) {
            log_printf("fix: vehicle editor symmetric delete: sites call different functions, left unchanged");
            return 0;
        }

    uint8_t *stubs = VirtualAlloc(NULL, 64 * NSITES, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stubs) return 0;
    for (size_t i = 0; i < NSITES; i++) {
        /* stub: the 16 original bytes (no rip-relative operand), the load, jump back to the call */
        uint8_t *s = stubs + 64 * i;
        memcpy(s, at[i], OFF_CALL_DELETE);
        memcpy(s + OFF_CALL_DELETE, sites[i].load, 4);
        uint8_t *j = s + OFF_CALL_DELETE + 4;
        j[0] = 0xff; j[1] = 0x25; *(uint32_t *)(j + 2) = 0;               /* jmp [rip+0] */
        *(uint64_t *)(j + 6) = (uint64_t)(at[i] + OFF_CALL_DELETE);

        DWORD old;
        if (!VirtualProtect(at[i] + OFF_READ, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
        memcpy(at[i] + OFF_READ, sites[i].pass, 4);
        VirtualProtect(at[i] + OFF_READ, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at[i] + OFF_READ, 4);
        patch_jump(at[i], OFF_CALL_DELETE, s);
    }
    log_printf("fix: vehicle editor symmetric delete: mirrored part's definition read before the delete "
               "(sites +0x%llx, +0x%llx)",
               (unsigned long long)(at[0] - g_base), (unsigned long long)(at[1] - g_base));
    return 1;
}
