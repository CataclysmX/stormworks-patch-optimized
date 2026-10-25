/* Visual version label: "v1.15.23 64Bit" -> "v1.15.23 64Bit Cata-ptimised".
 *
 * The main-menu label is a UI text element built by FUN_140261f80 from the version literal and
 * " 64Bit". Only the suffix part of that one function is changed: its length immediates and the
 * lea that loads " 64Bit" (redirected to our text, stored in the unused tail of .rdata so the
 * rip-relative displacement fits). The version literal itself and its eleven other uses (network
 * compatibility checks, saves, exported vehicles, support requests) are untouched, so multiplayer
 * still sees exactly the original version.
 *
 *   C7 45 xx 06 00 00 00     mov  dword [rbp+xx], 6          length of " 64Bit"
 *   41 8D 55 07              lea  edx, [r13+7]               buffer size
 *   48 8D 4D xx              lea  rcx, [rbp+xx]
 *   E8 rel32                 call string_reserve
 *   8B 7D xx                 mov  edi, [rbp+xx]
 *   4C 8D 47 01              lea  r8, [rdi+1]
 *   48 8D 15 rel32           lea  rdx, [" 64Bit"]           */
#include "stwpatch.h"
#include <string.h>

static int write_code(uint8_t *at, const void *src, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, src, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

int branding_install(void)
{
    const char *suffix = g_cfg.version_suffix;
    size_t extra = strlen(suffix);
    if (!extra) return 0;
    char text[96];
    size_t len = 6 + 1 + extra;                         /* " 64Bit" + ' ' + suffix */
    if (len + 2 > 127 || len >= sizeof text) { log_printf("branding: suffix too long"); return 0; }
    memcpy(text, " 64Bit ", 7);
    memcpy(text + 7, suffix, extra + 1);

    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt), *text_s = NULL, *rdata = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!strncmp((char *)sec[i].Name, ".text", 8)) text_s = &sec[i];
        if (!strncmp((char *)sec[i].Name, ".rdata", 8)) rdata = &sec[i];
    }
    if (!text_s || !rdata) return 0;

    /* the original " 64Bit" literal */
    uint8_t *rp = g_base + rdata->VirtualAddress, *orig = NULL;
    for (size_t i = 1; i + 7 <= rdata->Misc.VirtualSize; i++)
        if (rp[i - 1] == 0 && !memcmp(rp + i, " 64Bit", 7)) { orig = rp + i; break; }
    if (!orig) { log_printf("branding: version label not found (game updated?)"); return 0; }

    /* the code site: lea rdx,[orig] preceded by the expected sequence */
    uint8_t *tp = g_base + text_s->VirtualAddress, *site = NULL;
    for (size_t i = 0x20; i + 7 <= text_s->Misc.VirtualSize; i++) {
        uint8_t *p = tp + i;
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || p + 7 + *(int32_t *)(p + 3) != orig) continue;
        uint8_t *m = p - 0x1B;                        /* start of "mov dword [rbp+xx], 6" */
        if (m[0] == 0xC7 && m[1] == 0x45 && !memcmp(m + 3, "\x06\x00\x00\x00", 4) &&
            !memcmp(m + 7, "\x41\x8D\x55\x07", 4) && m[0x0F] == 0xE8 && !memcmp(m + 0x17, "\x4C\x8D\x47\x01", 4)) {
            site = m;
            break;
        }
    }
    if (!site) { log_printf("branding: version label code not recognised, left unchanged"); return 0; }

    /* our text in the zero padding after .rdata's data, inside its last mapped page */
    uint32_t used = rdata->Misc.VirtualSize, page_end = (used + 0xFFF) & ~0xFFFu;
    if (page_end - used < len + 16) { log_printf("branding: no room for the label text"); return 0; }
    uint8_t *dst = g_base + rdata->VirtualAddress + ((used + 15) & ~15u);
    for (size_t i = 0; i < len + 1; i++) if (dst[i]) { log_printf("branding: padding not empty, left unchanged"); return 0; }
    write_code(dst, text, len + 1);

    int32_t n32 = (int32_t)len;
    uint8_t n8 = (uint8_t)(len + 1);
    uint8_t *lea = site + 0x1B;
    int32_t disp = (int32_t)(dst - (lea + 7));
    if (!write_code(site + 3, &n32, 4) || !write_code(site + 10, &n8, 1) || !write_code(lea + 3, &disp, 4)) return 0;
    log_printf("branding: version label now ends with \"%s\" (network version untouched)", text);
    return 1;
}
