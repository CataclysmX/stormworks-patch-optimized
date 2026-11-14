/* Visual test of overlay.c without the game: a fake "Stormworks" window and fake live data.
 *   make tests && wine build/overlay_test.exe
 * then press F3 in that window (or: xdotool keydown F3; sleep 0.2; xdotool keyup F3). */
#include "../src/overlay.c"
#include "../src/menu.c"

stw_config g_cfg;
game_addrs g_addr;
char g_ini_path[MAX_PATH];
void mi_option_set(mi_option_t o, long v) { (void)o; (void)v; }
char g_last_exception[192] = "09:44:13 0xc0000005 at stormworks64.exe+0x58b280 (read 0x5de749b6110)";

void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

const char *stwpatch_build(void) { return "1.1 (test)"; }

static long long n;
void physics_live(phys_live_t *o)
{
    n++;
    memset(o, 0, sizeof *o);
    o->steps = n * 62; o->step_ms = n * 62 * 8.7; o->step_max_ms = 14.1;
    o->ticks = n * 60; o->tick_ms = n * 60 * 16.4; o->tick_max_ms = 28; o->ticks_slow = n * 3;
    o->wait_phys_n = n * 60; o->wait_phys_ms = n * 60 * 0.08; o->wait_veh_ms = n * 60 * 0.02;
    o->objects = 655; o->dynamic = 112; o->manifolds = 6355; o->constraints = 19; o->rate_hz = 60;
}

void narrowphase_live(narrow_live_t *o)
{
    memset(o, 0, sizeof *o);
    o->passes = n * 62; o->pairs = n * 62 * 1486; o->pass_ms = n * 62 * 6.9; o->tail_ms = n * 62 * 5.9;
    o->slow_ms = 6.91;
    o->slow_at = GetTickCount64();
    snprintf(o->slow_text, sizeof o->slow_text, "dynamic compound31 r=14.2 m at (1532, 12, -8840) vs "
             "static triangle mesh21 r=707.1 m at (1500, 0, -9000)");
}

long long vehicle_pool_ticks(void) { return n * 35000; }
void gl_live(gl_live_t *o)
{
    memset(o, 0, sizeof *o);
    o->frames = n * 55; o->draws = n * 55 * 9000; o->binds = n * 55 * 36000;
    o->redundant = n * 55 * 6000; o->unbind = n * 55 * 12000; o->textures = n * 55 * 4000; o->frame_ms = n * 55 * 18.0;
}
void net_live(int *p, long long *b, long long *r) { *p = 2; *b = n * 300 * 1024; *r = 0; }
void unstick_format(char *buf, size_t size) { snprintf(buf, size, "0 bodies without static collision now, 1 unstuck, 1 restored (total)"); }
void mi_process_info(size_t *a, size_t *b, size_t *c, size_t *rss, size_t *prss, size_t *commit, size_t *pcommit, size_t *f)
{
    (void)a; (void)b; (void)c; (void)f;
    *rss = *prss = 3ull << 30; *commit = 4ull << 30; *pcommit = 5ull << 30;
}

static LRESULT CALLBACK game_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(40, 90, 140));
        FillRect(dc, &r, b);
        DeleteObject(b);
        TextOutA(dc, r.right / 2 - 60, r.bottom / 2, "fake Stormworks", 15);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, msg, wp, lp);
}

int main(void)
{
    g_cfg.overlay_vk = VK_F3;
    g_cfg.menu_vk = VK_F4;
    g_cfg.unstick = 1; g_cfg.unstick_depth = 0.3; g_cfg.unstick_seconds = 1; g_cfg.unstick_max_radius = 3;
    g_cfg.compound_split = 1; g_cfg.phys_budget_ms = 12; g_cfg.narrow_min_pairs = 64; g_cfg.narrow_balance = 1;
    g_addr.narrow_hook_on = 1; g_addr.compound_split_on = 1;
    GetFullPathNameA("build\\overlay_test.ini", MAX_PATH, g_ini_path, NULL);
    g_cfg.narrow_threads = 7;
    g_cfg.vehicle_threads = 8;
    g_cfg.unstick = 1;
    g_cfg.fast_alloc = 1;
    g_cfg.net_monitor = 1;
    g_cfg.fix_editor_mirror = 1;
    g_cfg.phys_rate = -1;
    g_cfg.gl_stats = 1;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = game_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "fake_game";
    RegisterClassA(&wc);
    HWND g = CreateWindowExA(0, "fake_game", "Stormworks", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 1280, 720,
                             NULL, NULL, wc.hInstance, NULL);
    (void)g;
    overlay_install();
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
}
