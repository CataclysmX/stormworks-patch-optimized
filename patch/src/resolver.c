/* Finds every game address stwpatch needs in the running image, so the same DLL works on the
 * client (stormworks64.exe), the dedicated server (server64.exe) and future builds, as long as
 * the code keeps its shape. Same algorithms as tools/resolve.py, which validates them offline.
 *
 *   vtables    MSVC RTTI: TypeDescriptor name in .data -> CompleteObjectLocator in .rdata ->
 *              the vtable follows the pointer to the COL.
 *   functions  byte patterns of mm_thread_object::enqueue / mm_thread_task::wait, then the
 *              call targets inside them (_Mtx_lock, set_state, _Cnd_broadcast, _Mtx_unlock).
 *   profiler   call following "lea rcx/rdx, 'internalSingleStepSimulation'"; on builds where
 *              that call is a CProfileSample wrapper, the bts_enterFunc/leaveFunc pointers.
 * Anything not found stays NULL and the feature that needs it disables itself. */
#include "stwpatch.h"
#include <string.h>

game_addrs g_addr;

typedef struct { uint8_t *p; size_t n; } span_t;

static span_t section(const char *name)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (strncmp((char *)s->Name, name, 8) == 0)
            return (span_t){ g_base + s->VirtualAddress, s->Misc.VirtualSize };
    return (span_t){ NULL, 0 };
}

static int in_span(span_t s, const void *p) { return s.p && (uint8_t *)p >= s.p && (uint8_t *)p < s.p + s.n; }

/* pattern: bytes with mask (0 = wildcard). Returns the unique match, NULL if 0 or several. */
static uint8_t *find_unique(span_t s, const uint8_t *pat, const uint8_t *mask, size_t len, int *count)
{
    uint8_t *hit = NULL;
    int n = 0;
    for (size_t i = 0; s.p && i + len <= s.n; i++) {
        if (s.p[i] != pat[0]) continue;
        size_t k = 1;
        while (k < len && (!mask[k] || s.p[i + k] == pat[k])) k++;
        if (k == len) { hit = s.p + i; n++; }
    }
    if (count) *count = n;
    return n == 1 ? hit : NULL;
}

static uint8_t *rel32(uint8_t *insn, int opcode_len) { return insn + opcode_len + 4 + *(int32_t *)(insn + opcode_len); }

static uint8_t *follow_thunks(uint8_t *f)
{
    for (int i = 0; f && i < 4 && f[0] == 0xE9; i++) f = rel32(f, 1);
    return f;
}

static void *memfind(span_t s, const void *needle, size_t len)
{
    for (size_t i = 0; s.p && i + len <= s.n; i++)
        if (s.p[i] == *(const uint8_t *)needle && memcmp(s.p + i, needle, len) == 0) return s.p + i;
    return NULL;
}

static uint8_t *rtti_vtable(const char *type_name)
{
    span_t data = section(".data"), rdata = section(".rdata");
    uint8_t *name = memfind(data, type_name, strlen(type_name) + 1);
    if (!name) return NULL;
    uint32_t td = (uint32_t)(name - 0x10 - g_base);
    for (size_t i = 12; i + 12 <= rdata.n; i += 4) {
        uint32_t *col = (uint32_t *)(rdata.p + i - 12);
        if (col[3] != td || col[0] != 1 || col[1] != 0 || col[5] != (uint32_t)((uint8_t *)col - g_base)) continue;
        for (size_t j = 0; j + 8 <= rdata.n; j += 8)
            if (*(uint64_t *)(rdata.p + j) == (uint64_t)(uintptr_t)col) return rdata.p + j + 8;
    }
    return NULL;
}

/* first "lea r64,[rip+disp32]" in .text whose target is `target` */
static uint8_t *lea_ref(span_t text, const void *target)
{
    for (size_t i = 0; i + 7 <= text.n; i++) {
        uint8_t *p = text.p + i;
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05 && rel32(p, 3) == target)
            return p;
    }
    return NULL;
}

static uint8_t *find_string(const char *s)
{
    span_t rdata = section(".rdata");
    size_t len = strlen(s);
    for (size_t i = 1; rdata.p && i + len + 1 <= rdata.n; i++)
        if (rdata.p[i - 1] == 0 && rdata.p[i] == (uint8_t)s[0] && memcmp(rdata.p + i, s, len + 1) == 0)
            return rdata.p + i;
    return NULL;
}

static void resolve_threads(span_t text)
{
    static const uint8_t enq[] = { 0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10,0x48,0x89,0x70,0x18,0x57,
                                   0x48,0x83,0xEC,0x40,0x48,0x8B,0xEA,0x48,0x8B,0xF1,0xC6,0x82,0xB0,0x00,0x00,0x00,0x01 };
    static const uint8_t wait[] = { 0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x40,
                                    0x48,0x8B,0xF9,0x48,0x8D,0x59,0x10,0x33,0xF6 };
    uint8_t ones[64];
    memset(ones, 1, sizeof ones);
    int n;
    uint8_t *e = find_unique(text, enq, ones, sizeof enq, &n);
    if (e) {
        uint8_t *lock = NULL, *unlock = NULL;
        for (int i = 0; i < 0xe0 - 5; i++) {
            if (e[i] == 0xE8 && !lock && in_span(text, rel32(e + i, 1))) lock = rel32(e + i, 1);
            if (e[i] == 0xE9 && !unlock && in_span(text, rel32(e + i, 1))) unlock = rel32(e + i, 1);
            if (!memcmp(e + i, "\x48\x8B\xCD\xE8", 4)) g_addr.task_set_state = rel32(e + i + 3, 1);
            if (!memcmp(e + i, "\x48\x8D\x8E\x90\x00\x00\x00\xE8", 8)) g_addr.cnd_broadcast = rel32(e + i + 7, 1);
        }
        g_addr.thread_enqueue = e;
        g_addr.mtx_lock = lock;
        g_addr.mtx_unlock = unlock;
    }
    /* wait: several functions share the prologue; the right one waits on the condvar at +68h */
    for (size_t i = 0; i + sizeof wait <= text.n; i++) {
        uint8_t *p = text.p + i;
        if (p[0] != 0x48 || memcmp(p, wait, sizeof wait)) continue;
        for (int k = 0; k < 0x80 - 5; k++)
            if (!memcmp(p + k, "\x48\x8D\x4F\x68\xE8", 5)) { g_addr.task_wait = p; break; }
        if (g_addr.task_wait) break;
    }
}

static void resolve_profiler(span_t text)
{
    span_t data = section(".data");
    uint8_t *s = find_string("internalSingleStepSimulation");
    uint8_t *ref = s ? lea_ref(text, s) : NULL;
    uint8_t *start = NULL;
    for (int i = 7; ref && i < 7 + 24 && !start; i++)
        if (ref[i] == 0xE8) start = follow_thunks(rel32(ref + i, 1));
    s = find_string("dispatchAllCollisionPairs");
    ref = s ? lea_ref(text, s) : NULL;
    uint8_t *stop = NULL;
    for (int i = 0; ref && i < 0x80 && !stop; i++) {
        if (ref[i] != 0xE8) continue;
        uint8_t *t = rel32(ref + i, 1);
        for (int k = i + 5; k < i + 0x30; k++)
            if ((ref[k] == 0xE8 || ref[k] == 0xE9) && rel32(ref + k, 1) == t && t != start) { stop = follow_thunks(t); break; }
    }
    /* CProfileSample wrappers calling through bts_enterFunc / bts_leaveFunc */
    if (start && !memcmp(start, "\x40\x53\x48\x83\xEC\x20\x48\x8B\xD9\x48\x8B\xCA\xFF\x15", 14)) {
        g_addr.prof_start_ptr = (void **)rel32(start + 12, 2);
        start = *g_addr.prof_start_ptr;
    }
    if (stop && !memcmp(stop, "\x48\x83\xEC\x28\xFF\x15", 6)) {
        g_addr.prof_stop_ptr = (void **)rel32(stop + 4, 2);
        stop = *g_addr.prof_stop_ptr;
    }
    if (!in_span(text, start) || !in_span(text, stop)) return;
    g_addr.prof_start = start;
    g_addr.prof_stop = stop;
    /* thread index offset in the TLS block (mov r8d, imm32) and CurrentNode[64] (first lea into .data) */
    g_addr.prof_tls_offset = -1;
    for (int i = 0; i < 0x40; i++)
        if (start[i] == 0x41 && start[i + 1] == 0xB8) { g_addr.prof_tls_offset = *(int32_t *)(start + i + 2); break; }
    for (int i = 0; i < 0x80; i++) {
        uint8_t *p = start + i;
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05 && in_span(data, rel32(p, 3))) {
            uint8_t **cur = (uint8_t **)rel32(p, 3);
            /* CurrentNode[i] starts as &gRoots[i]; slots 62/63 are never used by the game's few threads */
            uint8_t *roots = cur[63] - 63 * 0x38;
            if (cur[62] == roots + 62 * 0x38 && in_span(data, roots)) g_addr.prof_roots = roots;
            break;
        }
    }
}

int resolve_addresses(void)
{
    span_t text = section(".text");
    g_addr.vt_dispatcher    = rtti_vtable(".?AVbtCollisionDispatcher@@");
    g_addr.vt_hashed_cache  = rtti_vtable(".?AVbtHashedOverlappingPairCache@@");
    g_addr.vt_softrigid     = rtti_vtable(".?AVbtSoftRigidDynamicsWorld@@");
    g_addr.vt_discrete      = rtti_vtable(".?AVbtDiscreteDynamicsWorld@@");
    g_addr.vt_vehicle_tick  = rtti_vtable(".?AUthreaded_tick_task@c_scene_vehicle_body@@");
    g_addr.vt_scene_update  = rtti_vtable(".?AUupdate_thread_task@c_scene@@");
    if (g_addr.vt_softrigid) g_addr.step_simulation = ((uint8_t **)g_addr.vt_softrigid)[12];
    if (g_addr.vt_dispatcher) {
        /* constructor: lea rax,[vtable] ... lea rax,[defaultNearCallback]; mov [reg+30h],rax */
        uint8_t *ref = lea_ref(text, g_addr.vt_dispatcher);
        for (int i = 0; ref && i < 0x80; i++)
            if (!memcmp(ref + i, "\x48\x8D\x05", 3) && ref[i + 7] == 0x48 && ref[i + 8] == 0x89 && ref[i + 10] == 0x30) {
                g_addr.default_near = rel32(ref + i, 3);
                break;
            }
    }
    resolve_threads(text);
    resolve_profiler(text);

    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY td = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (td.VirtualAddress)
        g_addr.exe_tls_index = (uint32_t *)((IMAGE_TLS_DIRECTORY64 *)(g_base + td.VirtualAddress))->AddressOfIndex;

#define R(f) (g_addr.f ? (unsigned long long)((uint8_t *)g_addr.f - g_base) : 0ull)
    log_printf("resolver: dispatcher vt 0x%llx, pair cache vt 0x%llx, world vt 0x%llx/0x%llx, step 0x%llx, near 0x%llx",
               R(vt_dispatcher), R(vt_hashed_cache), R(vt_softrigid), R(vt_discrete), R(step_simulation), R(default_near));
    log_printf("resolver: vehicle tick vt 0x%llx, scene task vt 0x%llx, enqueue 0x%llx, wait 0x%llx, set_state 0x%llx, "
               "mtx 0x%llx/0x%llx, cnd 0x%llx", R(vt_vehicle_tick), R(vt_scene_update), R(thread_enqueue), R(task_wait),
               R(task_set_state), R(mtx_lock), R(mtx_unlock), R(cnd_broadcast));
    log_printf("resolver: profiler start 0x%llx stop 0x%llx (%s), roots 0x%llx, tls offset %d",
               R(prof_start), R(prof_stop), g_addr.prof_start_ptr ? "via zone pointers" : "direct",
               R(prof_roots), g_addr.prof_tls_offset);
#undef R
    return 1;
}
