/* OpenGL call statistics (measurement only: every call is passed on unchanged).
 *
 * The sampler shows the main thread spending 55 % of its time in OPENGL32.dll, most of it from
 * FUN_14092ae80, which draws one mesh with six calls: bind vertex buffer, bind index buffer,
 * attribute setup, glDrawElements, unbind both buffers. Under Proton every call also crosses the
 * Windows/Linux boundary. Before filtering anything, this counts per frame (SwapBuffers to
 * SwapBuffers) the draws, the buffer binds and how many of them change nothing:
 *   redundant  glBindBuffer(target, b) while b is already bound to target
 *   unbind     glBindBuffer(target, 0) followed by a bind of target before any draw: removable
 * glDrawElements, glBindTexture and SwapBuffers are exe imports; glBindBuffer, glBindVertexArray,
 * glVertexAttribPointer and glUseProgram come from wglGetProcAddress (hooked before the game
 * creates its context: the DLL is loaded before the exe starts). A VAO change makes the element
 * buffer binding unknown, so its next bind is never counted as redundant. Main (render) thread
 * only; counters are plain.
 *
 * Filter ([misc] gl_filter): half the binds were glBindBuffer(target, 0) followed by another bind
 * of that target (31 000 per frame in the workbench). glBindBuffer(ARRAY / ELEMENT_ARRAY, 0) is
 * not sent: the unbind is kept pending, per thread. The next bind of that target cancels it (and
 * is skipped too when it binds the buffer the driver still has bound); any other GL call that may
 * depend on buffer bindings applies it first. For that, every GL function the game gets (its
 * OPENGL32 imports and wglGetProcAddress results) goes through a generated thunk: "nothing pending?
 * jump to the real function", else save the argument registers (rcx rdx r8 r9 xmm0-3, the stack
 * is untouched), apply the pending unbind, restore, jump. Functions that cannot depend on buffer
 * bindings (uniforms, textures, render states, attribute enables...) are not wrapped, otherwise
 * the unbind would be applied before every glUniform. Context switches, VAO binds and buffer
 * deletions also forget which buffer the driver has bound. */
#include "stwpatch.h"
#include <string.h>

#define GL_ARRAY_BUFFER         0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define UNKNOWN                 0xFFFFFFFFu

typedef void (*bind_buffer_t)(unsigned target, unsigned buffer);
typedef void (*bind_vao_t)(unsigned vao);
typedef void (*attrib_ptr_t)(unsigned index, int size, unsigned type, unsigned char norm, int stride, const void *ptr);
typedef void (*use_program_t)(unsigned program);
typedef void (WINAPI *draw_elements_t)(unsigned mode, int count, unsigned type, const void *indices);
typedef void (WINAPI *draw_arrays_t)(unsigned mode, int first, int count);
typedef void (WINAPI *bind_texture_t)(unsigned target, unsigned texture);
typedef BOOL (WINAPI *swap_buffers_t)(HDC dc);
typedef PROC (WINAPI *get_proc_t)(LPCSTR name);
static void filter_flush(void);
static void filter_forget(int which);

static bind_buffer_t   real_bind_buffer;
static bind_vao_t      real_bind_vao;
static attrib_ptr_t    real_attrib_ptr;
static use_program_t   real_use_program;
static draw_elements_t real_draw_elements;
static draw_arrays_t   real_draw_arrays;
static bind_texture_t  real_bind_texture;
static swap_buffers_t  real_swap;
static get_proc_t      real_get_proc;

/* current frame */
static struct { long long draws, binds, redundant, unbind, attrib, programs, textures, elided; } cur;
static unsigned bound[2] = { UNKNOWN, UNKNOWN }, pending_zero[2];
/* last complete frames, for the overlay and the log (written by the render thread) */
static SRWLOCK lock = SRWLOCK_INIT;
static gl_live_t totals;
static LARGE_INTEGER qpf, last_swap;

static inline int slot(unsigned target)
{
    return target == GL_ARRAY_BUFFER ? 0 : target == GL_ELEMENT_ARRAY_BUFFER ? 1 : -1;
}

/* ---- filter state, per thread (GL bindings belong to the thread's context) ---- */
typedef struct { unsigned actual[2]; int pending[2]; } fstate_t;   /* actual: what the driver has */
static DWORD ftls = TLS_OUT_OF_INDEXES;
static volatile LONG pending_any;              /* threads with a pending unbind: the thunks' test */
static volatile LONG n_wrapped, n_transparent;  /* GL functions behind a thunk / called directly */

static fstate_t *fstate(void)
{
    if (ftls == TLS_OUT_OF_INDEXES) return NULL;
    fstate_t *f = TlsGetValue(ftls);
    if (!f) {
        f = VirtualAlloc(NULL, sizeof *f, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!f) return NULL;
        f->actual[0] = f->actual[1] = UNKNOWN;
        TlsSetValue(ftls, f);
    }
    return f;
}

static void filter_flush(void)
{
    fstate_t *f = ftls != TLS_OUT_OF_INDEXES ? TlsGetValue(ftls) : NULL;
    if (!f || !(f->pending[0] | f->pending[1])) return;
    if (f->pending[0]) { f->pending[0] = 0; real_bind_buffer(GL_ARRAY_BUFFER, 0); f->actual[0] = 0; }
    if (f->pending[1]) { f->pending[1] = 0; real_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0); f->actual[1] = 0; }
    InterlockedDecrement(&pending_any);
}

/* which: 1 = element buffer (VAO change), 3 = both (context switch, buffer deletion) */
static void filter_forget(int which)
{
    filter_flush();
    fstate_t *f = ftls != TLS_OUT_OF_INDEXES ? TlsGetValue(ftls) : NULL;
    if (!f) return;
    if (which & 2) f->actual[0] = UNKNOWN;
    if (which & 1) f->actual[1] = UNKNOWN;
}

static void filter_forget_all(void) { filter_forget(3); }

static void hk_bind_buffer(unsigned target, unsigned buffer)
{
    cur.binds++;
    int s = slot(target);
    if (s >= 0) {
        if (pending_zero[s]) cur.unbind++;          /* the previous bind to 0 changed nothing */
        pending_zero[s] = 0;
        if (bound[s] == buffer) cur.redundant++;
        else if (buffer == 0) pending_zero[s] = 1;
        bound[s] = buffer;
    }
    fstate_t *f = s >= 0 && g_addr.gl_filter_on ? fstate() : NULL;
    if (!f || !g_cfg.gl_filter) {                /* not filtered (or switched off live) */
        filter_flush();
        if (f) f->actual[s] = buffer;
        real_bind_buffer(target, buffer);
        return;
    }
    if (buffer == 0) {                           /* keep the unbind pending */
        if (f->actual[s] == 0 && !f->pending[s]) { real_bind_buffer(target, 0); return; }
        if (!f->pending[s]) {
            if (!(f->pending[0] | f->pending[1])) InterlockedIncrement(&pending_any);
            f->pending[s] = 1;
        }
        cur.elided++;
        return;
    }
    if (f->pending[s]) {                         /* the pending unbind changed nothing */
        f->pending[s] = 0;
        if (!(f->pending[0] | f->pending[1])) InterlockedDecrement(&pending_any);
        if (f->actual[s] == buffer) { cur.elided++; return; }   /* and the driver still has it */
    }
    f->actual[s] = buffer;
    real_bind_buffer(target, buffer);
}

static void hk_bind_vao(unsigned vao)
{
    bound[1] = UNKNOWN;                          /* the element buffer belongs to the VAO */
    pending_zero[1] = 0;
    filter_forget(1);
    real_bind_vao(vao);
}

static void hk_attrib_ptr(unsigned index, int size, unsigned type, unsigned char norm, int stride, const void *ptr)
{
    cur.attrib++;
    pending_zero[0] = 0;                         /* uses the array buffer binding */
    filter_flush();
    real_attrib_ptr(index, size, type, norm, stride, ptr);
}

static void hk_use_program(unsigned program)
{
    cur.programs++;
    real_use_program(program);
}

static void WINAPI hk_draw_elements(unsigned mode, int count, unsigned type, const void *indices)
{
    cur.draws++;
    pending_zero[0] = pending_zero[1] = 0;       /* a draw uses the bindings: the unbind mattered */
    filter_flush();
    real_draw_elements(mode, count, type, indices);
}

static void WINAPI hk_draw_arrays(unsigned mode, int first, int count)
{
    cur.draws++;
    pending_zero[0] = pending_zero[1] = 0;
    filter_flush();
    real_draw_arrays(mode, first, count);
}

static void WINAPI hk_bind_texture(unsigned target, unsigned texture)
{
    cur.textures++;
    real_bind_texture(target, texture);
}

/* frame limiter ([misc] fps_cap, live from the F4 menu): if the game simulates on its render
 * thread, frames beyond what the screen needs take time from the simulation ticks */
static void frame_limit(void)
{
    static LARGE_INTEGER deadline;
    int cap = g_cfg.fps_cap;
    if (cap <= 0) { deadline.QuadPart = 0; return; }
    LONGLONG period = qpf.QuadPart / cap;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!deadline.QuadPart || now.QuadPart - deadline.QuadPart > period) deadline = now;   /* late: resync */
    deadline.QuadPart += period;
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG left = deadline.QuadPart - now.QuadPart;
        if (left <= 0) break;
        double ms = left * 1000.0 / qpf.QuadPart;
        if (ms > 2.0) Sleep((DWORD)(ms - 1.5));             /* 1 ms timer granularity (timeBeginPeriod) */
        else YieldProcessor();
    }
}

static BOOL WINAPI hk_swap(HDC dc)
{
    static int reported;
    if (!reported && g_addr.gl_filter_on && totals.frames == 100) {
        reported = 1;
        log_printf("opengl filter: %ld GL functions behind a thunk, %ld called directly (no buffer binding use)",
                   n_wrapped, n_transparent);
    }
    filter_flush();
    frame_limit();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    AcquireSRWLockExclusive(&lock);
    totals.frames++;
    totals.draws += cur.draws;
    totals.binds += cur.binds;
    totals.redundant += cur.redundant;
    totals.unbind += cur.unbind;
    totals.attrib += cur.attrib;
    totals.programs += cur.programs;
    totals.textures += cur.textures;
    totals.elided += cur.elided;
    if (last_swap.QuadPart) totals.frame_ms += (now.QuadPart - last_swap.QuadPart) * 1000.0 / qpf.QuadPart;
    ReleaseSRWLockExclusive(&lock);
    last_swap = now;
    memset(&cur, 0, sizeof cur);
    return real_swap(dc);
}

/* ---- generated thunks ---- */
#define THUNK_SIZE  128
#define THUNK_BLOCK (256 * 1024)                /* grown as needed: games load thousands of GL functions */
static uint8_t *thunk_pool;
static int      thunk_used, thunk_failed;
static SRWLOCK  thunk_lock = SRWLOCK_INIT;     /* wglGetProcAddress may be called from several threads */

static uint8_t *put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); return p + 8; }

/* always = 1: no fast path (the thunk must run its call even with nothing pending) */
static void *make_thunk(void *real, void (*fn)(void), int always)
{
    AcquireSRWLockExclusive(&thunk_lock);
    if (!thunk_pool || thunk_used + THUNK_SIZE > THUNK_BLOCK) {
        thunk_pool = VirtualAlloc(NULL, THUNK_BLOCK, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        thunk_used = 0;
        if (!thunk_pool) { thunk_failed = 1; ReleaseSRWLockExclusive(&thunk_lock); return NULL; }
    }
    uint8_t *t = thunk_pool + thunk_used, *p = t;
    thunk_used += THUNK_SIZE;
    ReleaseSRWLockExclusive(&thunk_lock);
    if (!always) {
        *p++ = 0x48; *p++ = 0xB8; p = put64(p, (uint64_t)(uintptr_t)&pending_any);   /* mov rax, &pending_any */
        *p++ = 0x83; *p++ = 0x38; *p++ = 0x00;                                         /* cmp dword [rax], 0 */
        *p++ = 0x75; *p++ = 0x0C;                                                      /* jne slow */
        *p++ = 0x48; *p++ = 0xB8; p = put64(p, (uint64_t)(uintptr_t)real);           /* mov rax, real */
        *p++ = 0xFF; *p++ = 0xE0;                                                      /* jmp rax */
    }
    static const uint8_t save[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51,                         /* push rcx, rdx, r8, r9 */
        0x48, 0x83, 0xEC, 0x68,                                     /* sub rsp, 68h (aligned, shadow space) */
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,   /* movdqu [rsp+20h..], xmm0-3 */
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50 };
    static const uint8_t restore[] = {
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,   /* movdqu xmm0-3, [rsp+20h..] */
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
        0x48, 0x83, 0xC4, 0x68,                                     /* add rsp, 68h */
        0x41, 0x59, 0x41, 0x58, 0x5A, 0x59 };                       /* pop r9, r8, rdx, rcx */
    memcpy(p, save, sizeof save); p += sizeof save;
    *p++ = 0x48; *p++ = 0xB8; p = put64(p, (uint64_t)(uintptr_t)fn);                 /* mov rax, fn */
    *p++ = 0xFF; *p++ = 0xD0;                                                          /* call rax */
    memcpy(p, restore, sizeof restore); p += sizeof restore;
    *p++ = 0x48; *p++ = 0xB8; p = put64(p, (uint64_t)(uintptr_t)real);               /* mov rax, real */
    *p++ = 0xFF; *p++ = 0xE0;                                                          /* jmp rax */
    FlushInstructionCache(GetCurrentProcess(), t, THUNK_SIZE);
    return t;
}

/* GL functions that cannot use or change GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER bindings:
 * called as they are, so a pending unbind survives them */
static int transparent(const char *n)
{
    static const char *prefix[] = { "glUniform", "glProgramUniform", "glGetUniform", "glVertexAttrib1", "glVertexAttrib2",
                                    "glVertexAttrib3", "glVertexAttrib4", "glStencil", "glBlend", "glTexParameter",
                                    "glClear", "glDepth", NULL };
    static const char *exact[] = { "glActiveTexture", "glBindTexture", "glBindSampler", "glUseProgram", "glEnable",
                                   "glDisable", "glEnablei", "glDisablei", "glEnableVertexAttribArray",
                                   "glDisableVertexAttribArray", "glVertexAttribDivisor", "glPolygonMode",
                                   "glPolygonOffset", "glCullFace", "glFrontFace", "glColorMask", "glViewport",
                                   "glScissor", "glLineWidth", "glPointSize", "glGetError", "glHint",
                                   "glGetAttribLocation", "glBindFramebuffer", "glDrawBuffer", "glDrawBuffers",
                                   "glReadBuffer", "glPixelStorei", "glSampleCoverage", NULL };
    for (int i = 0; prefix[i]; i++) if (!strncmp(n, prefix[i], strlen(prefix[i]))) return 1;
    for (int i = 0; exact[i]; i++) if (!strcmp(n, exact[i])) return 1;
    return 0;
}

/* functions after which the driver's buffer bindings are not what we last sent */
static int forgets_bindings(const char *n)
{
    return !strcmp(n, "wglMakeCurrent") || !strcmp(n, "wglDeleteContext") || !strcmp(n, "wglMakeContextCurrentARB") ||
           !strncmp(n, "glDeleteBuffers", 15) || !strncmp(n, "glDeleteVertexArrays", 20) ||
           !strncmp(n, "glBindVertexArray", 17) || !strcmp(n, "glPopClientAttrib");
}

/* the wrapper every other GL function goes through when the filter is on */
static void *filter_wrap(const char *n, void *p)
{
    if (!g_addr.gl_filter_on || !p) return p;
    if (transparent(n)) { InterlockedIncrement(&n_transparent); return p; }
    void *t = forgets_bindings(n) ? make_thunk(p, filter_forget_all, 1) : make_thunk(p, filter_flush, 0);
    if (!t) {
        /* an unwrapped function could see a pending unbind: off for good this session (the F4
         * menu then shows it as needing a restart), pending unbinds applied at the next call */
        g_addr.gl_filter_on = 0;
        g_cfg.gl_filter = 0;
        log_printf("opengl filter: OFF for this session (no memory for a thunk, %s unwrapped)", n);
        return p;
    }
    InterlockedIncrement(&n_wrapped);
    return t;
}

#define WRAP(name, real, hook) \
    if (!strcmp(n, name) && p) { real = (void *)p; return (PROC)(void *)hook; }

static PROC WINAPI hk_get_proc(LPCSTR n)
{
    PROC p = real_get_proc(n);
    if (!n || ((uintptr_t)n >> 16) == 0) return p;        /* ordinal */
    WRAP("glBindBuffer", real_bind_buffer, hk_bind_buffer)
    WRAP("glBindBufferARB", real_bind_buffer, hk_bind_buffer)
    WRAP("glBindVertexArray", real_bind_vao, hk_bind_vao)
    WRAP("glVertexAttribPointer", real_attrib_ptr, hk_attrib_ptr)
    WRAP("glUseProgram", real_use_program, hk_use_program)
    return (PROC)filter_wrap(n, (void *)p);
}

void gl_live(gl_live_t *out)
{
    AcquireSRWLockShared(&lock);
    *out = totals;
    ReleaseSRWLockShared(&lock);
}

static DWORD WINAPI log_thread(LPVOID arg)
{
    (void)arg;
    gl_live_t prev;
    memset(&prev, 0, sizeof prev);
    for (;;) {
        Sleep(30000);
        gl_live_t t;
        gl_live(&t);
        double f = (double)(t.frames - prev.frames);
        if (f >= 30) {
            double binds = (t.binds - prev.binds) / f;
            log_printf("opengl: per frame %.0f draws, %.0f buffer binds (%.0f redundant, %.0f unbind+rebind, %.0f not sent), "
                       "%.0f attrib pointers, %.0f texture binds, %.0f programs; %.1f ms per frame",
                       (t.draws - prev.draws) / f, binds, (t.redundant - prev.redundant) / f,
                       (t.unbind - prev.unbind) / f, (t.elided - prev.elided) / f, (t.attrib - prev.attrib) / f,
                       (t.textures - prev.textures) / f, (t.programs - prev.programs) / f,
                       (t.frame_ms - prev.frame_ms) / f);
        }
        prev = t;
    }
    return 0;
}

static void hook(const char *dll, const char *fn, void *hk, void **real)
{
    ULONGLONG *slot_ = iat_find(dll, fn);
    if (slot_) *real = patch_ptr((void **)slot_, hk);
}

int glstats_install(void)
{
    QueryPerformanceFrequency(&qpf);
    hook("GDI32.dll", "SwapBuffers", hk_swap, (void **)&real_swap);    /* frames + frame limiter */
    if (!real_swap) {
        log_printf("opengl stats: not installed (SwapBuffers import not found)");
        return 0;
    }
    if (g_cfg.fps_cap > 0) log_printf("frame limiter: %d fps", g_cfg.fps_cap);
    if (!g_cfg.gl_stats && !g_cfg.gl_filter) return 1;
    if (g_cfg.gl_filter) {
        ftls = TlsAlloc();
        g_addr.gl_filter_on = ftls != TLS_OUT_OF_INDEXES;
    }
    hook("OPENGL32.dll", "wglGetProcAddress", hk_get_proc, (void **)&real_get_proc);
    hook("OPENGL32.dll", "glDrawElements", hk_draw_elements, (void **)&real_draw_elements);
    hook("OPENGL32.dll", "glDrawArrays", hk_draw_arrays, (void **)&real_draw_arrays);
    hook("OPENGL32.dll", "glBindTexture", hk_bind_texture, (void **)&real_bind_texture);
    if (!real_get_proc) {
        log_printf("opengl stats: not installed (imports not found)");
        return 1;
    }
    if (g_addr.gl_filter_on) {
        /* every other OPENGL32 import goes through a thunk too (glGetIntegerv, glDrawElements...) */
        int n = 0;
        IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(g_base + dir.VirtualAddress); d->Name; d++) {
            if (_stricmp((char *)(g_base + d->Name), "OPENGL32.dll") != 0) continue;
            IMAGE_THUNK_DATA64 *names = (void *)(g_base + d->OriginalFirstThunk);
            IMAGE_THUNK_DATA64 *iat = (void *)(g_base + d->FirstThunk);
            for (; names->u1.AddressOfData; names++, iat++) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                const char *fn = (char *)((IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData))->Name;
                void *cur_fn = (void *)iat->u1.Function;
                if (cur_fn == (void *)hk_get_proc || cur_fn == (void *)hk_draw_elements ||
                    cur_fn == (void *)hk_draw_arrays || cur_fn == (void *)hk_bind_texture) continue;
                void *w = filter_wrap(fn, cur_fn);
                if (w != cur_fn) { patch_ptr((void **)&iat->u1.Function, w); n++; }
            }
        }
        if (g_addr.gl_filter_on)
            log_printf("opengl filter: on (unbinds kept pending; %d imports wrapped, more as the game loads them)", n);
    }
    HANDLE h = CreateThread(NULL, 0, log_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    log_printf("opengl stats: on (counting only, every call passed on unchanged)");
    return 1;
}
