/* Debug overlay (F3 by default, [misc] overlay_key): a small semi-transparent panel over the
 * top-left of the game window with what stwpatch measures, refreshed 4 times a second.
 *
 * It is a separate topmost, layered, click-through window that never takes the focus (the game
 * keeps keyboard and mouse), drawn with GDI: no hook in the game's renderer, so it works the same
 * with Vulkan or OpenGL, under Proton or on Windows. Not visible over an exclusive fullscreen
 * mode on Windows (use borderless). The key comes from a low-level keyboard hook (the game reads
 * raw input: under Proton GetAsyncKeyState never sees its keys), with GetAsyncKeyState as a
 * fallback, and only counts while the game window is in the foreground; the panel hides itself
 * when the game loses the focus.
 *
 * F4 ([misc] menu_key) opens the settings menu (menu.c) in a second panel at the top right:
 * up/down select, left/right change, Enter toggles or saves. While it is open these keys are
 * taken by the hook (not passed on), as long as the game window is in the foreground. */
#include "stwpatch.h"
#include <psapi.h>
#include <mimalloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TICK_MS     40                  /* key polling: a tap lasts ~100 ms */
#define DRAW_TICKS  6                   /* panel redrawn every 240 ms */
#define STATS_TICKS 25                  /* figures recomputed every second */
#define MAX_LINES   28
#define LINE_LEN    200
#define PAD         8

typedef struct {
    HWND wnd;
    int right;                          /* anchored at the top right of the game (menu) */
    char lines[MAX_LINES][LINE_LEN];    /* UTF-8 */
    COLORREF color[MAX_LINES];
    int n;
} panel_t;

static HWND    game_wnd;
static HFONT   font;
static int     shown, menu_open, menu_sel;
static panel_t stats_p, menu_p = { .right = 1 };
static panel_t *cur = &stats_p;         /* where add() writes */

static BOOL CALLBACK find_game(HWND h, LPARAM lp)
{
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || h == stats_p.wnd || h == menu_p.wnd || !IsWindowVisible(h) ||
        GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    GetClientRect(h, &r);
    LONG *best = (LONG *)lp;
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > *best) { *best = area; game_wnd = h; }
    return TRUE;
}

static void add(COLORREF c, const char *fmt, ...)
{
    if (cur->n >= MAX_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cur->lines[cur->n], LINE_LEN, fmt, ap);
    va_end(ap);
    cur->color[cur->n++] = c;
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

    cur = &stats_p;
    cur->n = 0;
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
    if (g_cfg.gl_stats) {
        static gl_live_t pg;
        gl_live_t g;
        gl_live(&g);
        double f = (double)(g.frames - pg.frames);
        if (pt && f > 0)
            add(WHITE, "OpenGL       %.0f fps   %.0f draws/frame   %.0f buffer binds (%.0f%% removable)   %.0f texture binds",
                f / sec, (g.draws - pg.draws) / f, (g.binds - pg.binds) / f,
                g.binds > pg.binds ? 100.0 * ((g.redundant - pg.redundant) + (g.unbind - pg.unbind)) / (g.binds - pg.binds) : 0.0,
                (g.textures - pg.textures) / f);
        pg = g;
    }
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

static int widen(const char *utf8, wchar_t *out, int max)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, max);
    return n > 0 ? n - 1 : 0;
}

static void layout(panel_t *p)
{
    RECT gr;
    if (!game_wnd || !GetClientRect(game_wnd, &gr)) return;
    POINT o = { 0, 0 };
    ClientToScreen(game_wnd, &o);
    HDC dc = GetDC(p->wnd);
    HGDIOBJ old = SelectObject(dc, font);
    int w = 0, h = 0;
    wchar_t wl[LINE_LEN];
    for (int i = 0; i < p->n; i++) {
        SIZE s;
        int len = widen(p->lines[i], wl, LINE_LEN);
        GetTextExtentPoint32W(dc, wl, len, &s);
        if (s.cx > w) w = s.cx;
        h += s.cy;
    }
    SelectObject(dc, old);
    ReleaseDC(p->wnd, dc);
    int x = p->right ? o.x + (gr.right - gr.left) - (w + 2 * PAD) - 12 : o.x + 12;
    /* stats below the top-left corner (where overlays like MangoHud sit), menu at the top right */
    SetWindowPos(p->wnd, HWND_TOPMOST, x, o.y + (gr.bottom - gr.top) / 6, w + 2 * PAD, h + 2 * PAD,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        panel_t *p = (panel_t *)GetWindowLongPtrA(h, GWLP_USERDATA);
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
        wchar_t wl[LINE_LEN];
        for (int i = 0; p && i < p->n; i++) {
            SIZE s;
            int len = widen(p->lines[i], wl, LINE_LEN);
            GetTextExtentPoint32W(dc, wl, len, &s);
            SetTextColor(dc, p->color[i]);
            TextOutW(dc, PAD, y, wl, len);
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

static volatile LONG key_presses;      /* overlay key-downs seen by the low-level hook */
static volatile LONG menu_presses;     /* menu key-downs */
static volatile LONG menu_shown;       /* the menu is on screen: navigation keys are ours */
static volatile LONG q_head, q_tail;   /* navigation keys, hook -> overlay thread */
static int q_keys[64];

static int game_focused(void)
{
    HWND f = GetForegroundWindow();
    DWORD pid = 0;
    if (f) GetWindowThreadProcessId(f, &pid);
    return f && pid == GetCurrentProcessId() && f != stats_p.wnd && f != menu_p.wnd;
}

static LRESULT CALLBACK keyboard_ll(int code, WPARAM wp, LPARAM lp)
{
    static int down, menu_down;
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        int vk = (int)k->vkCode, is_down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (vk == g_cfg.overlay_vk) {
            if (is_down && !down) InterlockedIncrement(&key_presses);   /* not the auto-repeat */
            down = is_down;
        }
        if (vk == g_cfg.menu_vk && !(k->flags & LLKHF_ALTDOWN)) {        /* never Alt+F4 */
            if (is_down && !menu_down) InterlockedIncrement(&menu_presses);
            menu_down = is_down;
        }
        if (menu_shown && (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN) &&
            game_focused()) {
            LONG h = q_head;
            if (is_down && h - q_tail < 64) {                            /* auto-repeat included */
                q_keys[h & 63] = vk;
                InterlockedExchange(&q_head, h + 1);
            }
            return 1;                                                    /* not passed on */
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void render_menu(void)
{
    cur = &menu_p;
    cur->n = 0;
    char key[8];
    snprintf(key, sizeof key, "F%d", g_cfg.menu_vk - VK_F1 + 1);
    add(WHITE, "stwpatch v%s — réglages           [%s : fermer]", stwpatch_build(), key);
    add(GREY, " ");
    int n = menu_item_count();
    for (int i = 0; i < n; i++) {
        char line[160];
        int kind;
        menu_line(i, line, sizeof line, &kind);
        COLORREF c = i == menu_sel ? YELLOW : kind == 1 ? GREY : kind == 2 ? GREEN : WHITE;
        if (kind == 2) add(GREY, " ");
        add(c, "%s %s", i == menu_sel ? ">" : " ", line);
    }
    add(GREY, " ");
    if (menu_status()[0]) add(GREEN, "%s", menu_status());
    add(GREY, "↑↓ choisir   ←→ régler   Entrée : basculer / valider   * = au prochain lancement");
    add(GREY, "Dans un siège, ouvrez plutôt le menu en pause : les flèches pilotent aussi.");
    cur = &stats_p;
}

static HWND make_panel(HINSTANCE inst, const char *cls, panel_t *p)
{
    HWND h = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             cls, "stwpatch", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    if (!h) return NULL;
    SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)p);
    SetLayeredWindowAttributes(h, 0, 215, LWA_ALPHA);
    p->wnd = h;
    return h;
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
    if (!make_panel(inst, wc.lpszClassName, &stats_p) || !make_panel(inst, wc.lpszClassName, &menu_p)) {
        log_printf("overlay: window creation failed (%lu)", GetLastError());
        return 0;
    }
    font = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SetTimer(stats_p.wnd, 1, TICK_MS, NULL);
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)keyboard_ll, &self);
    HHOOK hook = SetWindowsHookExA(WH_KEYBOARD_LL, keyboard_ll, self, 0);
    log_printf("overlay: ready, F%d stats, F%d settings, while the game has the focus (keyboard hook %s)",
               g_cfg.overlay_vk - VK_F1 + 1, g_cfg.menu_vk - VK_F1 + 1, hook ? "on" : "FAILED, polling only");

    int was_down = 0, tick = 0, n_toggles = 0, n_unfocused = 0, menu_was_open = 0;
    LONG seen_presses = 0, seen_menu = 0;
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
        /* the hook's presses; without the hook, bit 15 (down now) / bit 0 (pressed since the
         * previous call). Never both: one press would toggle twice (seen on 2026-11-14) */
        LONG presses = key_presses;
        SHORT ks = g_cfg.overlay_vk ? GetAsyncKeyState(g_cfg.overlay_vk) : 0;
        int down = (ks & 0x8000) != 0;
        int pressed = g_cfg.overlay_vk && (hook ? presses != seen_presses : (down && !was_down) || (ks & 1));
        seen_presses = presses;
        LONG mp = menu_presses;
        int menu_pressed = g_cfg.menu_vk && mp != seen_menu;
        seen_menu = mp;
        if ((pressed || menu_pressed) && !focused && n_unfocused++ < 3) {
            HWND f = GetForegroundWindow();
            DWORD pid = 0;
            if (f) GetWindowThreadProcessId(f, &pid);
            log_printf("overlay: key seen but the game is not in the foreground (foreground %p, pid %lu; game %p)",
                       (void *)f, pid, (void *)game_wnd);
        }
        if (pressed && focused) {
            shown = !shown;
            tick = 0;
            if (n_toggles++ < 4) {
                RECT r;
                GetClientRect(game_wnd, &r);
                log_printf("overlay: %s (game window %ldx%ld)", shown ? "shown" : "hidden", r.right, r.bottom);
            }
        }
        if (menu_pressed && focused) menu_open = !menu_open;
        was_down = down;

        /* menu: keys from the hook, redrawn at every tick while open */
        InterlockedExchange(&menu_shown, menu_open && focused);
        int changed = 0;
        while (q_tail != q_head) {
            int vk = q_keys[q_tail & 63];
            InterlockedIncrement(&q_tail);
            int n = menu_item_count();
            if (vk == VK_UP) menu_sel = (menu_sel + n - 1) % n;
            else if (vk == VK_DOWN) menu_sel = (menu_sel + 1) % n;
            else menu_change(menu_sel, vk == VK_LEFT ? -1 : vk == VK_RIGHT ? 1 : 0);
            changed = 1;
        }
        if (menu_open && focused) {
            if (changed || !menu_was_open || tick % DRAW_TICKS == 0) {
                render_menu();
                layout(&menu_p);
                InvalidateRect(menu_p.wnd, NULL, FALSE);
            }
            menu_was_open = 1;
        } else {
            menu_was_open = 0;
            if (IsWindowVisible(menu_p.wnd)) ShowWindow(menu_p.wnd, SW_HIDE);
        }

        if (!shown || !focused) {
            if (IsWindowVisible(stats_p.wnd)) ShowWindow(stats_p.wnd, SW_HIDE);
            tick++;
            continue;
        }
        if (tick % STATS_TICKS == 0) refresh_text();
        if (tick++ % DRAW_TICKS == 0) {
            layout(&stats_p);
            InvalidateRect(stats_p.wnd, NULL, FALSE);
        }
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
