/* Debug overlay (F3 by default, [misc] overlay_key): a small semi-transparent panel over the
 * top-left of the game window with what stwpatch measures, refreshed 4 times a second.
 *
 * It is a separate topmost, layered, click-through window that never takes the focus (the game
 * keeps keyboard and mouse), drawn with GDI: no hook in the game's renderer, so it works the same
 * with Vulkan or OpenGL, under Proton or on Windows. Not visible over an exclusive fullscreen
 * mode on Windows (use borderless). The key is read with GetAsyncKeyState and only counts while
 * the game window is in the foreground; the panel hides itself when the game loses the focus. */
#include "stwpatch.h"
#include <psapi.h>
#include <mimalloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TICK_MS     250
#define STATS_TICKS 4                   /* figures recomputed every second */
#define MAX_LINES   20
#define LINE_LEN    200
#define PAD         8

static HWND  game_wnd, panel;
static HFONT font;
static int   shown;
static char  lines[MAX_LINES][LINE_LEN];
static int   n_lines;
static COLORREF line_color[MAX_LINES];

static BOOL CALLBACK find_game(HWND h, LPARAM lp)
{
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || h == panel || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    GetClientRect(h, &r);
    LONG *best = (LONG *)lp;
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > *best) { *best = area; game_wnd = h; }
    return TRUE;
}

static void add(COLORREF c, const char *fmt, ...)
{
    if (n_lines >= MAX_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lines[n_lines], LINE_LEN, fmt, ap);
    va_end(ap);
    line_color[n_lines++] = c;
}

#define WHITE  RGB(235, 235, 235)
#define GREY   RGB(160, 160, 160)
#define GREEN  RGB(120, 230, 120)
#define YELLOW RGB(250, 210, 90)
#define RED    RGB(255, 110, 100)

/* one-second differences of the cumulative counters */
static void refresh_text(void)
{
    static phys_live_t pp;
    static narrow_live_t pn;
    static long long pv, pbytes;
    static ULONGLONG pt;
    phys_live_t p;
    narrow_live_t nl;
    physics_live(&p);
    narrowphase_live(&nl);
    long long veh = vehicle_pool_ticks();
    int players = 0;
    long long bytes = 0, refused = 0;
    if (g_cfg.net_monitor) net_live(&players, &bytes, &refused);
    ULONGLONG now = GetTickCount64();
    double sec = pt ? (now - pt) / 1000.0 : 0;
    if (sec <= 0) sec = 1;

    n_lines = 0;
    add(WHITE, "stwpatch v%s   git %s   [F%d: hide]", stwpatch_build(), STWPATCH_GIT, g_cfg.overlay_vk - VK_F1 + 1);

    long long ticks = p.ticks - pp.ticks, steps = p.steps - pp.steps;
    if (pt && ticks > 0) {
        double avg = (p.tick_ms - pp.tick_ms) / ticks, slow = 100.0 * (p.ticks_slow - pp.ticks_slow) / ticks;
        add(slow > 10 ? RED : slow > 2 ? YELLOW : GREEN, "Simulation   %5.1f ticks/s   avg %5.1f ms   max %5.1f ms   %3.0f%% slow (slow motion)",
            ticks / sec, avg, p.tick_max_ms, slow);
    } else {
        add(GREY, "Simulation   no game tick (menu, editor or loading)");
    }
    if (pt && steps > 0) {
        double step = (p.step_ms - pp.step_ms) / steps;
        add(step > 12 ? RED : step > 6 ? YELLOW : GREEN, "Physics      %5.1f steps/s  step %5.2f ms  max %5.2f ms   rate %.0f Hz (%s)",
            steps / sec, step, p.step_max_ms, p.rate_hz,
            g_cfg.phys_rate > 0 ? "forced" : g_cfg.phys_rate < 0 ? "stwpatch governor" : "game");
        add(WHITE, "World        %d objects   %d dynamic bodies   %d contact manifolds   %d constraints",
            p.objects, p.dynamic, p.manifolds, p.constraints);
    } else {
        add(GREY, "Physics      not stepping");
    }
    if (pt && ticks > 0) {
        long long wn = p.wait_phys_n - pp.wait_phys_n;
        add(WHITE, "Main thread  waits physics %.2f ms   vehicle ticks %.2f ms   per tick",
            wn ? (p.wait_phys_ms - pp.wait_phys_ms) / wn : 0.0, (p.wait_veh_ms - pp.wait_veh_ms) / ticks);
    }
    if (g_cfg.narrow_threads > 0) {
        long long passes = nl.passes - pn.passes;
        double pass_ms = nl.pass_ms - pn.pass_ms, tail = nl.tail_ms - pn.tail_ms;
        if (pt && passes > 0)
            add(WHITE, "Narrowphase  %d threads   %.0f pairs   pass %.2f ms   %.0f%% waiting for the last worker",
                g_cfg.narrow_threads + 1, (double)(nl.pairs - pn.pairs) / passes, pass_ms / passes,
                pass_ms > 0 ? 100.0 * tail / pass_ms : 0.0);
        else
            add(GREY, "Narrowphase  %d threads, idle", g_cfg.narrow_threads + 1);
    } else {
        add(GREY, "Narrowphase  off");
    }
    if (nl.slow_ms > 0 && now - nl.slow_at < 3000)
        add(nl.slow_ms > 3 ? RED : YELLOW, "  slow pair  %.2f ms: %s", nl.slow_ms, nl.slow_text);
    if (g_cfg.vehicle_threads > 0)
        add(WHITE, "Vehicles     %d threads   %.0f body ticks/s", g_cfg.vehicle_threads, pt ? (veh - pv) / sec : 0.0);
    else
        add(GREY, "Vehicles     game's single thread");
    if (g_cfg.unstick) {
        char u[160];
        unstick_format(u, sizeof u);
        add(WHITE, "Unstick      %s", u);
    }
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
    if (g_cfg.fast_alloc) {
        size_t rss, peak_rss, commit, peak_commit;
        mi_process_info(NULL, NULL, NULL, &rss, &peak_rss, &commit, &peak_commit, NULL);
        add(WHITE, "Memory       private %.2f GB   working set %.2f GB   mimalloc commit %.2f GB (peak %.2f), purge %s",
            pm.PrivateUsage / 1073741824.0, pm.WorkingSetSize / 1073741824.0, commit / 1073741824.0,
            peak_commit / 1073741824.0, g_cfg.alloc_purge ? "on" : "off");
    } else {
        add(WHITE, "Memory       private %.2f GB   working set %.2f GB   (Windows heap)",
            pm.PrivateUsage / 1073741824.0, pm.WorkingSetSize / 1073741824.0);
    }
    if (g_cfg.net_monitor && players)
        add(refused > 0 ? YELLOW : WHITE, "Network      %d remote players   %.0f KB/s sent   %lld sends refused (buffer full)",
            players, pt ? (bytes - pbytes) / 1024.0 / sec : 0.0, refused);
    add(WHITE, "Fixes        editor mirror delete %s   unstick %s",
        g_cfg.fix_editor_mirror ? "on" : "off", g_cfg.unstick ? "on" : "off");
    if (g_last_exception[0]) add(YELLOW, "Last exception  %s", g_last_exception);

    pp = p;
    pn = nl;
    pv = veh;
    pbytes = bytes;
    pt = now;
}

static void layout(void)
{
    RECT gr;
    if (!game_wnd || !GetClientRect(game_wnd, &gr)) return;
    POINT o = { 0, 0 };
    ClientToScreen(game_wnd, &o);
    HDC dc = GetDC(panel);
    HGDIOBJ old = SelectObject(dc, font);
    int w = 0, h = 0;
    for (int i = 0; i < n_lines; i++) {
        SIZE s;
        GetTextExtentPoint32A(dc, lines[i], (int)strlen(lines[i]), &s);
        if (s.cx > w) w = s.cx;
        h += s.cy;
    }
    SelectObject(dc, old);
    ReleaseDC(panel, dc);
    /* below the top-left corner, where overlays like MangoHud usually sit */
    SetWindowPos(panel, HWND_TOPMOST, o.x + 12, o.y + (gr.bottom - gr.top) / 6, w + 2 * PAD, h + 2 * PAD,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH bg = CreateSolidBrush(RGB(16, 18, 22));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        int y = PAD;
        for (int i = 0; i < n_lines; i++) {
            SIZE s;
            GetTextExtentPoint32A(dc, lines[i], (int)strlen(lines[i]), &s);
            SetTextColor(dc, line_color[i]);
            TextOutA(dc, PAD, y, lines[i], (int)strlen(lines[i]));
            y += s.cy;
        }
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static int game_focused(void)
{
    HWND f = GetForegroundWindow();
    DWORD pid = 0;
    if (f) GetWindowThreadProcessId(f, &pid);
    return f && pid == GetCurrentProcessId() && f != panel;
}

static DWORD WINAPI overlay_thread(LPVOID arg)
{
    (void)arg;
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "stwpatch_overlay";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);
    panel = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            wc.lpszClassName, "stwpatch", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    if (!panel) { log_printf("overlay: window creation failed (%lu)", GetLastError()); return 0; }
    SetLayeredWindowAttributes(panel, 0, 215, LWA_ALPHA);
    font = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SetTimer(panel, 1, TICK_MS, NULL);
    log_printf("overlay: ready, toggle with F%d while the game has the focus", g_cfg.overlay_vk - VK_F1 + 1);

    int was_down = 0, tick = 0;
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (m.message != WM_TIMER) { TranslateMessage(&m); DispatchMessageA(&m); continue; }
        if (!game_wnd || !IsWindow(game_wnd)) {
            LONG best = 0;
            game_wnd = NULL;
            EnumWindows(find_game, (LPARAM)&best);
            if (!game_wnd) continue;
        }
        int focused = game_focused();
        int down = (GetAsyncKeyState(g_cfg.overlay_vk) & 0x8000) != 0;
        if (down && !was_down && focused) {
            shown = !shown;
            tick = 0;
        }
        was_down = down;
        if (!shown || !focused) {
            if (IsWindowVisible(panel)) ShowWindow(panel, SW_HIDE);
            continue;
        }
        if (tick++ % STATS_TICKS == 0) refresh_text();
        layout();
        InvalidateRect(panel, NULL, FALSE);
    }
    return 0;
}

int overlay_install(void)
{
    HANDLE h = CreateThread(NULL, 0, overlay_thread, NULL, 0, NULL);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}
