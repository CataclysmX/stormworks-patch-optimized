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
 * only; counters are plain. */
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
static struct { long long draws, binds, redundant, unbind, attrib, programs, textures; } cur;
static unsigned bound[2] = { UNKNOWN, UNKNOWN }, pending_zero[2];
/* last complete frames, for the overlay and the log (written by the render thread) */
static SRWLOCK lock = SRWLOCK_INIT;
static gl_live_t totals;
static LARGE_INTEGER qpf, last_swap;

static inline int slot(unsigned target)
{
    return target == GL_ARRAY_BUFFER ? 0 : target == GL_ELEMENT_ARRAY_BUFFER ? 1 : -1;
}

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
    real_bind_buffer(target, buffer);
}

static void hk_bind_vao(unsigned vao)
{
    bound[1] = UNKNOWN;                          /* the element buffer belongs to the VAO */
    pending_zero[1] = 0;
    real_bind_vao(vao);
}

static void hk_attrib_ptr(unsigned index, int size, unsigned type, unsigned char norm, int stride, const void *ptr)
{
    cur.attrib++;
    pending_zero[0] = 0;                         /* uses the array buffer binding */
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
    real_draw_elements(mode, count, type, indices);
}

static void WINAPI hk_draw_arrays(unsigned mode, int first, int count)
{
    cur.draws++;
    pending_zero[0] = pending_zero[1] = 0;
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
    if (last_swap.QuadPart) totals.frame_ms += (now.QuadPart - last_swap.QuadPart) * 1000.0 / qpf.QuadPart;
    ReleaseSRWLockExclusive(&lock);
    last_swap = now;
    memset(&cur, 0, sizeof cur);
    return real_swap(dc);
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
    return p;
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
            log_printf("opengl: per frame %.0f draws, %.0f buffer binds (%.0f redundant, %.0f unbind+rebind), "
                       "%.0f attrib pointers, %.0f texture binds, %.0f programs; %.1f ms per frame",
                       (t.draws - prev.draws) / f, binds, (t.redundant - prev.redundant) / f,
                       (t.unbind - prev.unbind) / f, (t.attrib - prev.attrib) / f,
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
    if (!g_cfg.gl_stats) return 1;
    hook("OPENGL32.dll", "wglGetProcAddress", hk_get_proc, (void **)&real_get_proc);
    hook("OPENGL32.dll", "glDrawElements", hk_draw_elements, (void **)&real_draw_elements);
    hook("OPENGL32.dll", "glDrawArrays", hk_draw_arrays, (void **)&real_draw_arrays);
    hook("OPENGL32.dll", "glBindTexture", hk_bind_texture, (void **)&real_bind_texture);
    if (!real_get_proc) {
        log_printf("opengl stats: not installed (imports not found)");
        return 1;
    }
    HANDLE h = CreateThread(NULL, 0, log_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    log_printf("opengl stats: on (counting only, every call passed on unchanged)");
    return 1;
}
