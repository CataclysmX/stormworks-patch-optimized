/* Test of the OpenGL unbind filter (glstats.c) against a real driver: a window and a legacy GL
 * context under Wine, GL functions reached through this exe's OPENGL32 imports and
 * wglGetProcAddress, like the game. Checks that every state the program can observe is the one it
 * asked for (binding queries right after a kept-back unbind, transparent calls in between, element
 * buffer and VAO, client-side attribute pointer after an unbind, live switch), that unbinds are
 * really not sent, and that a triangle drawn with the game's bind/draw/unbind pattern gives the
 * same pixels with and without the filter.   make tests && wine build/glfilter_test.exe */
#include "../src/glstats.c"
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>

stw_config g_cfg;
game_addrs g_addr;
uint8_t *g_base;
void log_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("  log: "); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
void *patch_ptr(void **slot, void *value)
{
    DWORD old;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    void *p = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return p;
}
ULONGLONG *iat_find(const char *dll, const char *fn)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(g_base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(g_base + d->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA64 *names = (void *)(g_base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (void *)(g_base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            if (!strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData))->Name, fn)) return &iat->u1.Function;
        }
    }
    return NULL;
}

#define GL_ARRAY_BUFFER_BINDING          0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING  0x8895
#define GL_STATIC_DRAW                   0x88E4
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F
typedef void (*gen_t)(int, unsigned *);
typedef void (*bind_t)(unsigned, unsigned);
typedef void (*data_t)(unsigned, ptrdiff_t, const void *, unsigned);
typedef void (*vao_t)(unsigned);
typedef void (*enable_attr_t)(unsigned);
typedef void (*attr_ptr_t)(unsigned, int, unsigned, unsigned char, int, const void *);
typedef void (*get_attr_t)(unsigned, unsigned, int *);

static gen_t gen_buffers, gen_vaos;
static bind_t bind_buffer;
static data_t buffer_data;
static vao_t bind_vao;
static enable_attr_t enable_attr, disable_attr;
static attr_ptr_t attr_ptr;
static get_attr_t get_attr;
static int fails;

static int binding(unsigned which) { int v = -1; glGetIntegerv(which, &v); return v; }

static void expect(const char *what, long long got, long long want)
{
    printf("%-66s %s (%lld)\n", what, got == want ? "ok" : "FAILED", got);
    if (got != want) fails++;
}

/* one triangle with the game's pattern: bind VBO, bind IBO, attrib pointer, draw, unbind both */
static unsigned vbo_tri, ibo_tri;
static void draw_triangle(void)
{
    bind_buffer(GL_ARRAY_BUFFER, vbo_tri);
    bind_buffer(GL_ELEMENT_ARRAY_BUFFER, ibo_tri);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, 0);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0);
    glDisableClientState(GL_VERTEX_ARRAY);
    bind_buffer(GL_ARRAY_BUFFER, 0);
    bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static unsigned char pixels_on[64 * 64 * 4], pixels_off[64 * 64 * 4];
static void render(unsigned char *out)
{
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 0.5f, 0.25f);
    for (int i = 0; i < 200; i++) draw_triangle();
    glFinish();
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, out);
    SwapBuffers(wglGetCurrentDC());                      /* a frame, like the game (SwapBuffers import) */
}

int main(void)
{
    g_base = (uint8_t *)GetModuleHandleA(NULL);
    g_cfg.gl_stats = 1;
    g_cfg.gl_filter = 1;
    glstats_install();                                   /* hooks before the context, like in the game */
    expect("filter installed", g_addr.gl_filter_on, 1);
    if (!g_addr.gl_filter_on || !real_get_proc) { printf("RESULT: FAILED\n"); return 1; }

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "glfilter_test";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND w = CreateWindowA("glfilter_test", "stwpatch glfilter test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);
    wglMakeCurrent(dc, rc);
    printf("GL: %s / %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));

    gen_buffers = (gen_t)wglGetProcAddress("glGenBuffers");
    bind_buffer = (bind_t)wglGetProcAddress("glBindBuffer");
    buffer_data = (data_t)wglGetProcAddress("glBufferData");
    gen_vaos = (gen_t)wglGetProcAddress("glGenVertexArrays");
    bind_vao = (vao_t)wglGetProcAddress("glBindVertexArray");
    enable_attr = (enable_attr_t)wglGetProcAddress("glEnableVertexAttribArray");
    disable_attr = (enable_attr_t)wglGetProcAddress("glDisableVertexAttribArray");
    attr_ptr = (attr_ptr_t)wglGetProcAddress("glVertexAttribPointer");
    get_attr = (get_attr_t)wglGetProcAddress("glGetVertexAttribiv");
    expect("glBindBuffer is the filter's hook", bind_buffer == hk_bind_buffer, 1);
    expect("glEnableVertexAttribArray left unwrapped (transparent)", (void *)enable_attr == (void *)real_get_proc("glEnableVertexAttribArray"), 1);

    unsigned b[3];
    gen_buffers(3, b);
    unsigned A = b[0], B = b[1], E = b[2];
    float v[6] = { -1, -1, 3, -1, -1, 3 };
    unsigned short idx[3] = { 0, 1, 2 };
    bind_buffer(GL_ARRAY_BUFFER, A); buffer_data(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    bind_buffer(GL_ELEMENT_ARRAY_BUFFER, E); buffer_data(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    bind_buffer(GL_ARRAY_BUFFER, B); buffer_data(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    vbo_tri = A; ibo_tri = E;

    long long e0;
    bind_buffer(GL_ARRAY_BUFFER, A);
    e0 = cur.elided;
    bind_buffer(GL_ARRAY_BUFFER, 0);
    expect("unbind kept back", cur.elided - e0, 1);
    expect("query right after it sees 0 (applied by the glGetIntegerv thunk)", binding(GL_ARRAY_BUFFER_BINDING), 0);

    bind_buffer(GL_ARRAY_BUFFER, A); bind_buffer(GL_ARRAY_BUFFER, 0);
    e0 = cur.elided;
    bind_buffer(GL_ARRAY_BUFFER, B);
    expect("unbind then bind B: B bound", binding(GL_ARRAY_BUFFER_BINDING), B);

    bind_buffer(GL_ARRAY_BUFFER, A);
    e0 = cur.elided;
    bind_buffer(GL_ARRAY_BUFFER, 0);
    enable_attr(1);
    disable_attr(1);
    bind_buffer(GL_ARRAY_BUFFER, A);
    expect("unbind, attrib enables, rebind same: 2 calls not sent", cur.elided - e0, 2);
    expect("  and A still bound", binding(GL_ARRAY_BUFFER_BINDING), A);

    bind_buffer(GL_ELEMENT_ARRAY_BUFFER, E); bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    expect("element buffer: query after unbind sees 0", binding(GL_ELEMENT_ARRAY_BUFFER_BINDING), 0);

    unsigned vao[2];
    if (gen_vaos && bind_vao) {
        gen_vaos(2, vao);
        bind_vao(vao[0]);
        bind_buffer(GL_ELEMENT_ARRAY_BUFFER, E);
        bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);              /* kept back... */
        bind_vao(vao[1]);                                      /* ...applied to vao[0] before the switch */
        bind_buffer(GL_ELEMENT_ARRAY_BUFFER, E);
        bind_vao(vao[0]);
        expect("VAO: unbind applied to the right VAO before the switch", binding(GL_ELEMENT_ARRAY_BUFFER_BINDING), 0);
        bind_vao(vao[1]);
        expect("VAO: the other VAO keeps its element buffer", binding(GL_ELEMENT_ARRAY_BUFFER_BINDING), E);
        bind_vao(0);
    }

    static float client[6];
    bind_buffer(GL_ARRAY_BUFFER, A); bind_buffer(GL_ARRAY_BUFFER, 0);
    attr_ptr(0, 2, GL_FLOAT, 0, 0, client);                    /* client memory: needs the unbind */
    int ab = -1;
    get_attr(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &ab);
    expect("client-side attribute pointer after an unbind: buffer 0", ab, 0);

    bind_buffer(GL_ARRAY_BUFFER, A); bind_buffer(GL_ARRAY_BUFFER, 0);
    g_cfg.gl_filter = 0;                                       /* switched off live, unbind pending */
    bind_buffer(GL_ARRAY_BUFFER, B);
    expect("switched off live with an unbind pending: B bound", binding(GL_ARRAY_BUFFER_BINDING), B);
    bind_buffer(GL_ARRAY_BUFFER, 0);
    expect("  filter off: unbind sent at once", binding(GL_ARRAY_BUFFER_BINDING), 0);
    g_cfg.gl_filter = 1;
    expect("no error so far", glGetError(), 0);

    gl_live_t t0, t1, t2;
    gl_live(&t0);
    render(pixels_on);                                   /* SwapBuffers folds the frame into the totals */
    gl_live(&t1);
    g_cfg.gl_filter = 0;
    render(pixels_off);
    gl_live(&t2);
    g_cfg.gl_filter = 1;
    int diff = memcmp(pixels_on, pixels_off, sizeof pixels_on);
    expect("200 triangles: same pixels with and without the filter", diff, 0);
    expect("  pixel under the triangle drawn (red channel)", pixels_on[(10 * 64 + 10) * 4], 255);
    long long binds = t1.binds - t0.binds, kept = t1.elided - t0.elided;
    printf("  filter on : %lld buffer binds asked, %lld not sent (unbinds + rebinds of the same buffer)\n", binds, kept);
    printf("  filter off: %lld buffer binds asked, %lld not sent\n", t2.binds - t1.binds, t2.elided - t1.elided);
    expect("  filter on: 4 of the 4 binds per triangle not sent (after the first)", kept >= 4 * 199, 1);
    expect("  filter off: all sent", t2.elided - t1.elided, 0);
    expect("no GL error", glGetError(), 0);

    /* looking a function up again reuses its thunk (before 1.2.1 each lookup leaked 128 bytes) */
    long d0, r0, d1, r1;
    glstats_thunks(&d0, &r0);
    gen_t g = (gen_t)wglGetProcAddress("glGenBuffers"), g1 = g;
    int same = 1;
    for (int i = 0; i < 6000; i++) { g1 = (gen_t)wglGetProcAddress("glGenBuffers"); if (g1 != g) same = 0; }
    glstats_thunks(&d1, &r1);
    printf("  6001 lookups of glGenBuffers: %ld new thunk(s), %ld lookups counted\n", d1 - d0, r1 - r0);
    expect("6001 lookups of one function: the same thunk every time", same, 1);
    expect("  at most one thunk made for them", d1 - d0 <= 1, 1);
    /* the game asks wglGetProcAddress for thousands of functions: the thunk pool must grow */
    int distinct_ok = 1;
    void *prev = NULL;
    for (int i = 0; i < 6000; i++) {
        void *t = make_thunk((void *)(uintptr_t)(0x10000 + 16 * i), filter_flush, 0);  /* never called */
        if (!t || t == prev) distinct_ok = 0;
        prev = t;
    }
    glstats_thunks(&d1, &r1);
    expect("6000 distinct functions: 6000 thunks, pool grown past 256 KB", distinct_ok && d1 - d0 >= 6000, 1);
    expect("  filter still available", g_addr.gl_filter_on && g_cfg.gl_filter, 1);
    unsigned extra = 0;
    bind_buffer(GL_ARRAY_BUFFER, A); bind_buffer(GL_ARRAY_BUFFER, 0);          /* pending, then a late thunk */
    g(1, &extra);
    expect("  the last thunk works (and applied the pending unbind)", extra != 0 && binding(GL_ARRAY_BUFFER_BINDING) == 0, 1);
    printf("  thunks: %ld wrapped, %ld transparent\n", n_wrapped, n_transparent);

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    printf("RESULT: %s\n", fails ? "FAILED" : "OK");
    return fails != 0;
}
