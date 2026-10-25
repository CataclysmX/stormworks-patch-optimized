/* Physics instrumentation and solver tuning.
 *
 * - stepSimulation is hooked through its vtable slot (12) in both world vtables. It runs on the
 *   "physics" thread. Bullet's own profiler (BT_PROFILE is compiled in) resets its per-thread tree
 *   at the start of every step, so right after the original returns the tree holds exactly the
 *   timings of that step: we fold them into per-phase accumulators.
 * - mm_thread_task::wait is hooked to time how long the main thread blocks on the physics task,
 *   and the threaded_tick_task destructor (vtable slot 0) to time the block on vehicle ticks.
 *   Together they tell which thread really limits the tick rate.
 * - Solver knobs live in btContactSolverInfo at world+0xa8: m_numIterations (+0x28) and
 *   m_leastSquaresResidualThreshold (+0xb0), checked by solveGroupCacheFriendlyIterations
 *   (FUN_14012e370) after each iteration for an early exit. The game leaves both at Bullet's
 *   defaults (10 iterations, threshold 0 = never exit early). */
#include "stwpatch.h"
#include <stdio.h>
#include <string.h>
#include <intrin.h>

#define WORLD_SOLVER_INFO   0xa8
#define SI_NUM_ITERATIONS   0x28
#define SI_RESIDUAL         0xb0
#define WORLD_NUM_OBJECTS   0x0c
#define WORLD_DISPATCHER    0x28
#define DISP_NUM_MANIFOLDS  0x14
#define WORLD_NUM_CONSTR    0x1a4
#define WORLD_NUM_DYNAMIC   0x1c4

/* CProfileNode */
#define PN_NAME     0x00
#define PN_CALLS    0x08
#define PN_TIME_MS  0x0c
#define PN_CHILD    0x20
#define PN_SIBLING  0x28
#define PN_STRIDE   0x38

typedef int  (*step_t)(void *world, double time_step, int max_sub, double fixed);
typedef void (*wait_t)(uint8_t *task);
typedef void *(*dtor_t)(uint8_t *task, unsigned flags);

static step_t orig_step;
static wait_t orig_wait;
static dtor_t orig_tick_dtor;
static void  *vt_scene_update;

static LARGE_INTEGER qpf;
static SRWLOCK stat_lock = SRWLOCK_INIT;

#define MAX_PHASES 96
typedef struct {
    const char *name;       /* Bullet sample name (static string in the exe) */
    int depth;
    double ms;
    long long calls;
} phase_t;

#define MAX_WORLDS 4
typedef struct { void *w; long long steps; double ms, dynamic, manifolds; } world_stat_t;

static struct {
    world_stat_t world[MAX_WORLDS];
    long long steps;
    double step_ms, step_max_ms;
    double objects, dynamic, manifolds, constraints;
    double wait_phys_ms, wait_veh_ms;
    double tick_ms, tick_max_ms;            /* simulation thread: time between two ticks */
    long long ticks, ticks_slow;            /* ticks longer than 1/60 s = slow motion */
    long long wait_phys_n;
    phase_t ph[MAX_PHASES];
    int nph;
} st;

static int root_index = -1;
static int logged_solver;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)qpf.QuadPart;
}

static phase_t *phase(const char *name, int depth)
{
    for (int i = 0; i < st.nph; i++)
        if (st.ph[i].name == name && st.ph[i].depth == depth) return &st.ph[i];
    if (st.nph == MAX_PHASES) return NULL;
    phase_t *p = &st.ph[st.nph++];
    p->name = name; p->depth = depth;
    return p;
}

/* The client resets the profiler tree at the start of every step, the server build never does:
 * fold the difference with the value seen after the previous step (a drop means a reset). */
#define MAX_NODES 256
static struct { uint8_t *node; int calls; float ms; } last_seen[MAX_NODES];
static int n_seen;

static void fold(uint8_t *node, int depth)
{
    for (; node && depth < 8; node = *(uint8_t **)(node + PN_SIBLING)) {
        int calls = *(int *)(node + PN_CALLS);
        float ms = *(float *)(node + PN_TIME_MS);
        int k = 0;
        while (k < n_seen && last_seen[k].node != node) k++;
        if (k == n_seen && n_seen < MAX_NODES) { last_seen[n_seen].node = node; last_seen[n_seen].calls = 0; last_seen[n_seen].ms = 0; n_seen++; }
        int dc = calls; float dm = ms;
        if (k < n_seen) {
            if (calls >= last_seen[k].calls && ms >= last_seen[k].ms) { dc -= last_seen[k].calls; dm -= last_seen[k].ms; }
            last_seen[k].calls = calls; last_seen[k].ms = ms;
        }
        if (dc > 0) {
            phase_t *p = phase(*(const char **)(node + PN_NAME), depth);
            if (p) { p->ms += dm; p->calls += dc; }
        }
        fold(*(uint8_t **)(node + PN_CHILD), depth + 1);
    }
}

/* Bullet keeps its profiler thread index in the exe's TLS block (+8 client, +4 server). */
static uint8_t *thread_profile_root(void)
{
    if (!g_addr.exe_tls_index || !g_addr.prof_roots || g_addr.prof_tls_offset < 0) return NULL;
    uint8_t **tls_array = (uint8_t **)__readgsqword(0x58);   /* TEB.ThreadLocalStoragePointer */
    if (!tls_array) return NULL;
    uint8_t *block = tls_array[*g_addr.exe_tls_index];
    if (!block) return NULL;
    int idx = *(int *)(block + g_addr.prof_tls_offset);
    if (idx < 0 || idx >= 64) return NULL;
    if (root_index != idx) {
        root_index = idx;
        log_printf("physics: stepping thread uses Bullet profiler root #%d", idx);
    }
    return g_addr.prof_roots + (size_t)idx * PN_STRIDE;
}

static void apply_solver_knobs(uint8_t *world)
{
    uint8_t *si = world + WORLD_SOLVER_INFO;
    if (!logged_solver) {
        logged_solver = 1;
        log_printf("physics: solver defaults: iterations=%d residual_threshold=%g",
                   *(int *)(si + SI_NUM_ITERATIONS), *(double *)(si + SI_RESIDUAL));
    }
    if (g_cfg.solver_iterations > 0) *(int *)(si + SI_NUM_ITERATIONS) = g_cfg.solver_iterations;
    if (g_cfg.solver_residual > 0) *(double *)(si + SI_RESIDUAL) = g_cfg.solver_residual;
}

/* Physics rate. The game picks 180/120/60 Hz itself (0x140743916): it lowers the rate when the main
 * thread waits more than 1 ms on physics and raises it under 0.5 ms, every 900 ticks, but only
 * from samples taken on its deferred-wait path, so under load it can stay at 180 Hz (3 Bullet
 * substeps per tick). The stwpatch governor applies the same idea on what the main thread really
 * waits (hook_wait), every 120 steps:
 *   wait > 1 ms, or step > budget_ms      -> one rate down
 *   wait < 0.2 ms and step x (next/current) < budget_ms, and no rate drop in the last 20 s -> up */
static const double rates[3] = { 60.0, 120.0, 180.0 };
static int gov_level = -1;
static double gov_step_ms, gov_wait_ms;
static int gov_steps, gov_waits;
static ULONGLONG gov_last_drop, gov_backoff_ms = 20000;
static double gov_failed_step[3];       /* step time (at the lower rate) of the last failed raise, per level */
static int gov_raised_from = -1;        /* level we just raised from, to detect a failed attempt */
static double gov_prev_step;            /* step time measured just before that raise */
static volatile double cur_rate;

static double choose_fixed(double fixed)
{
    if (g_cfg.phys_rate > 0) return 1.0 / g_cfg.phys_rate;
    if (g_cfg.phys_rate == 0) return fixed;
    if (gov_level < 0) {
        double game = fixed > 0 ? 1.0 / fixed : 60.0;
        gov_level = game > 150 ? 2 : game > 90 ? 1 : 0;
    }
    return 1.0 / rates[gov_level];
}

static void governor_wait(double ms)           /* main thread, after each wait on the physics task */
{
    gov_wait_ms += ms;
    gov_waits++;
}

static void governor_update(double dt)         /* physics thread, after each step */
{
    if (g_cfg.phys_rate >= 0 || gov_level < 0) return;
    gov_step_ms += dt;
    if (++gov_steps < 120) return;
    double step = gov_step_ms / gov_steps, wait = gov_waits ? gov_wait_ms / gov_waits : 0;
    gov_step_ms = gov_wait_ms = 0;
    gov_steps = gov_waits = 0;
    int prev = gov_level;
    ULONGLONG now = GetTickCount64();
    if ((wait > 1.0 || step > g_cfg.phys_budget_ms) && gov_level > 0) {
        gov_level--;
        if (gov_raised_from == gov_level) {
            /* the last raise did not hold: remember how heavy the scene was, back off longer */
            gov_failed_step[gov_level] = gov_prev_step;
            if (gov_backoff_ms < 160000) gov_backoff_ms *= 2;
        }
        gov_raised_from = -1;
        gov_last_drop = now;
    } else if (gov_level < 2 && wait < 0.2 && now - gov_last_drop > gov_backoff_ms &&
               step * rates[gov_level + 1] / rates[gov_level] < g_cfg.phys_budget_ms &&
               (!gov_failed_step[gov_level] || step < gov_failed_step[gov_level] * 0.85)) {
        gov_raised_from = gov_level;
        gov_prev_step = step;
        gov_level++;
    } else if (gov_raised_from >= 0 && now - gov_last_drop > 60000) {
        gov_raised_from = -1;                   /* the raise held for a minute: success */
        gov_failed_step[gov_level - 1] = 0;
        gov_backoff_ms = 20000;
    }
    if (gov_level != prev)
        log_printf("physics governor: step %.2f ms, main thread wait %.2f ms -> rate %.0f Hz", step, wait, rates[gov_level]);
}

static int hook_step(void *world, double time_step, int max_sub, double fixed)
{
    uint8_t *w = world;
    apply_solver_knobs(w);
    fixed = choose_fixed(fixed);
    cur_rate = fixed > 0 ? 1.0 / fixed : 0;
    if (!g_cfg.phys_profile && g_cfg.phys_rate >= 0) return orig_step(world, time_step, max_sub, fixed);

    double t0 = now_ms();
    int r = orig_step(world, time_step, max_sub, fixed);
    double dt = now_ms() - t0;
    governor_update(dt);
    if (!g_cfg.phys_profile) return r;

    uint8_t *disp = *(uint8_t **)(w + WORLD_DISPATCHER);
    AcquireSRWLockExclusive(&stat_lock);
    st.steps++;
    st.step_ms += dt;
    if (dt > st.step_max_ms) st.step_max_ms = dt;
    st.objects += *(int *)(w + WORLD_NUM_OBJECTS);
    st.dynamic += *(int *)(w + WORLD_NUM_DYNAMIC);
    st.constraints += *(int *)(w + WORLD_NUM_CONSTR);
    if (disp) st.manifolds += *(int *)(disp + DISP_NUM_MANIFOLDS);
    world_stat_t *ws = NULL;
    for (int i = 0; i < MAX_WORLDS && !ws; i++)
        if (st.world[i].w == w || !st.world[i].w) { ws = &st.world[i]; ws->w = w; }
    if (ws) { ws->steps++; ws->ms += dt; ws->dynamic += *(int *)(w + WORLD_NUM_DYNAMIC);
              if (disp) ws->manifolds += *(int *)(disp + DISP_NUM_MANIFOLDS); }
    uint8_t *root = thread_profile_root();
    if (root) fold(*(uint8_t **)(root + PN_CHILD), 0);
    ReleaseSRWLockExclusive(&stat_lock);
    return r;
}

static void hook_wait(uint8_t *task)
{
    if (*(void **)task != vt_scene_update) { orig_wait(task); return; }
    double t0 = now_ms();
    orig_wait(task);
    double dt = now_ms() - t0;
    AcquireSRWLockExclusive(&stat_lock);
    st.wait_phys_ms += dt;
    st.wait_phys_n++;
    governor_wait(dt);
    {
        static double last_tick_end;
        double t = now_ms();
        if (last_tick_end > 0) {
            double iv = t - last_tick_end;
            if (iv < 1500) {                   /* longer gaps are loading, reported separately */
                st.tick_ms += iv; st.ticks++;
                if (iv > st.tick_max_ms) st.tick_max_ms = iv;
                if (iv > 1000.0 / 60 + 0.5) st.ticks_slow++;
            }
        }
        last_tick_end = t;
    }
    /* loading times: first tick after launch, and every stall of 1.5 s or more */
    static ULONGLONG start_ms;
    ULONGLONG now = GetTickCount64();
    if (!g_last_tick_ms) {
        if (!start_ms) start_ms = g_start_ms;
        log_printf("loading: first game tick %.1f s after launch", (now - start_ms) / 1000.0);
        crashguard_stage("stable");             /* startup survived: no safe mode on the next launch */
    } else if (now - g_last_tick_ms >= 1500) {
        log_printf("loading: %.1f s without game tick (save / vehicle loading or freeze)", (now - g_last_tick_ms) / 1000.0);
    }
    g_last_tick_ms = now;
    g_tick_tid = GetCurrentThreadId();
    ReleaseSRWLockExclusive(&stat_lock);
}

static void *hook_tick_dtor(uint8_t *task, unsigned flags)
{
    double t0 = now_ms();
    void *r = orig_tick_dtor(task, flags);
    double dt = now_ms() - t0;
    AcquireSRWLockExclusive(&stat_lock);
    st.wait_veh_ms += dt;
    ReleaseSRWLockExclusive(&stat_lock);
    return r;
}

typedef typeof(st) stats_t;

static void write_report(FILE *f, const stats_t *s, const char *narrow)
{
    double n = (double)s->steps, sec = g_cfg.phys_report_sec;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "stwpatch physics report %02d:%02d:%02d, last %ds\n\n", t.wHour, t.wMinute, t.wSecond,
            g_cfg.phys_report_sec);
    fprintf(f, "physics steps/s      : %.1f   (60 = real time; lower = the game runs in slow motion)\n", n / sec);
    fprintf(f, "step time            : avg %.2f ms   max %.2f ms   (budget at 60 Hz: 16.7 ms)\n",
            s->step_ms / n, s->step_max_ms);
    fprintf(f, "main thread waits    : physics %.2f ms/tick   vehicle ticks %.2f ms/tick\n",
            s->wait_phys_n ? s->wait_phys_ms / s->wait_phys_n : 0.0, s->wait_veh_ms / n);
    if (s->ticks)
        fprintf(f, "simulation ticks     : avg %.2f ms, max %.1f ms, %.1f%% longer than 16.7 ms (= slow motion); "
                   "own work %.2f ms/tick + waiting physics\n",
                s->tick_ms / s->ticks, s->tick_max_ms, 100.0 * s->ticks_slow / s->ticks,
                s->tick_ms / s->ticks - (s->wait_phys_n ? s->wait_phys_ms / s->wait_phys_n : 0.0));
    fprintf(f, "world                : %.0f objects, %.0f dynamic bodies, %.0f contact manifolds, %.0f constraints\n",
            s->objects / n, s->dynamic / n, s->manifolds / n, s->constraints / n);
    fprintf(f, "solver               : iterations=%s residual=%s\n",
            g_cfg.solver_iterations > 0 ? "patched" : "default",
            g_cfg.solver_residual > 0 ? "patched" : "default");
    fprintf(f, "physics rate         : %.0f Hz now (%s)\n", cur_rate,
            g_cfg.phys_rate > 0 ? "forced" : g_cfg.phys_rate < 0 ? "stwpatch governor" : "game");
    fprintf(f, "narrowphase          : %s\n", narrow);
    fputc('\n', f);
    for (int i = 0; i < MAX_WORLDS && s->world[i].w; i++)
        fprintf(f, "world %p : %lld steps (%.1f/s), avg %.2f ms, %.0f dyn bodies, %.0f manifolds\n",
                s->world[i].w, s->world[i].steps, s->world[i].steps / sec, s->world[i].ms / s->world[i].steps,
                s->world[i].dynamic / s->world[i].steps, s->world[i].manifolds / s->world[i].steps);
    double top = 0;
    for (int i = 0; i < s->nph; i++) if (s->ph[i].depth == 0) top += s->ph[i].ms;
    fprintf(f, "\nBullet phases (avg ms per step, %% of step), outside profiled phases: %.2f ms\n",
            (s->step_ms - top) / n);
    for (int i = 0; i < s->nph; i++) {
        if (s->ph[i].ms <= 0) continue;
        fprintf(f, "  %*s%-*s %8.3f ms %5.1f%%  (%.1f calls/step)\n", s->ph[i].depth * 2, "",
                44 - s->ph[i].depth * 2, s->ph[i].name ? s->ph[i].name : "?",
                s->ph[i].ms / n, 100.0 * s->ph[i].ms / (s->step_ms > 0 ? s->step_ms : 1),
                s->ph[i].calls / n);
    }
}

static void sibling_path(char *out, const char *path, const char *name)
{
    strcpy(out, path);
    char *sl = strrchr(out, '\\');
    strcpy(sl ? sl + 1 : out, name);
}

static DWORD WINAPI report_thread(LPVOID p)
{
    const char *path = p;
    char peak_path[MAX_PATH], csv_path[MAX_PATH];
    char name[64];
    snprintf(name, sizeof name, "%s_physics_peak.txt", g_prefix);
    sibling_path(peak_path, path, name);
    snprintf(name, sizeof name, "%s_physics.csv", g_prefix);
    sibling_path(csv_path, path, name);
    double peak = 0;
    FILE *csv = fopen(csv_path, "a");
    if (csv) { fprintf(csv, "# new session\ntime,steps_per_s,step_ms,step_max_ms,wait_phys_ms,dyn_bodies,manifolds,constraints,rate_hz\n"); fflush(csv); }
    for (;;) {
        Sleep((DWORD)g_cfg.phys_report_sec * 1000);
        AcquireSRWLockExclusive(&stat_lock);
        stats_t s = st;
        memset(&st, 0, sizeof st);
        for (int i = 0; i < s.nph; i++) {   /* keep the phase order stable between reports */
            st.ph[i].name = s.ph[i].name; st.ph[i].depth = s.ph[i].depth;
        }
        st.nph = s.nph;
        ReleaseSRWLockExclusive(&stat_lock);
        if (!s.steps) continue;

        double n = (double)s.steps, sec = g_cfg.phys_report_sec, avg = s.step_ms / n;
        char narrow[512];
        narrowphase_format(narrow, sizeof narrow);
        FILE *f = fopen(path, "w");
        if (f) { write_report(f, &s, narrow); fclose(f); }
        if (avg > peak) {        /* the heaviest period survives going back to the menu */
            peak = avg;
            if ((f = fopen(peak_path, "w"))) { write_report(f, &s, narrow); fclose(f); }
        }
        if (csv) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(csv, "%02d:%02d:%02d,%.1f,%.2f,%.2f,%.2f,%.0f,%.0f,%.0f,%.0f\n", t.wHour, t.wMinute, t.wSecond,
                    n / sec, avg, s.step_max_ms, s.wait_phys_n ? s.wait_phys_ms / s.wait_phys_n : 0.0,
                    s.dynamic / n, s.manifolds / n, s.constraints / n, cur_rate);
            fflush(csv);
        }
        log_printf("physics: %.1f steps/s, step %.2f ms, waits phys %.2f / veh %.2f ms, %.0f dyn bodies, %.0f manifolds; "
                   "ticks avg %.1f max %.0f ms, %.0f%% slow",
                   n / sec, avg, s.wait_phys_n ? s.wait_phys_ms / s.wait_phys_n : 0.0,
                   s.wait_veh_ms / n, s.dynamic / n, s.manifolds / n,
                   s.ticks ? s.tick_ms / s.ticks : 0.0, s.tick_max_ms, s.ticks ? 100.0 * s.ticks_slow / s.ticks : 0.0);
    }
    return 0;
}

int physics_install(void)
{
    /* mov [rsp+8],rbx / mov [rsp+10h],rsi / push rdi / sub rsp,40h */
    static const uint8_t wait_prologue[15] = {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x40 };
    QueryPerformanceFrequency(&qpf);
    if (!g_addr.vt_softrigid || !g_addr.vt_discrete) {
        log_printf("physics: DISABLED, Bullet world vtables not found in this build");
        return 0;
    }
    void **slot_soft = (void **)g_addr.vt_softrigid + 12;
    void **slot_disc = (void **)g_addr.vt_discrete + 12;
    uint8_t *step = g_addr.step_simulation;
    /* both world classes share btDiscreteDynamicsWorld::stepSimulation; it reads m_localTime at +0x200 */
    if (*slot_soft != step || *slot_disc != step) {
        log_printf("physics: DISABLED, stepSimulation vtable slots not as expected");
        return 0;
    }
    orig_step = (step_t)step;
    patch_ptr(slot_soft, hook_step);
    patch_ptr(slot_disc, hook_step);

    if (g_cfg.phys_profile || g_cfg.phys_rate < 0 || g_cfg.sampler_ms) {
        vt_scene_update = g_addr.vt_scene_update;
        uint8_t *wait = g_addr.task_wait;
        orig_wait = wait && vt_scene_update ? (wait_t)make_trampoline(wait, sizeof wait_prologue, wait_prologue) : NULL;
        if (orig_wait) patch_jump(wait, sizeof wait_prologue, hook_wait);
        else log_printf("physics: wait hook skipped (not found)");

        if (g_addr.vt_vehicle_tick) {
            void **dtor_slot = (void **)g_addr.vt_vehicle_tick;
            orig_tick_dtor = (dtor_t)*dtor_slot;
            patch_ptr(dtor_slot, hook_tick_dtor);
        }

        static char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, sizeof path);
        char *s = strrchr(path, '\\');
        sprintf(s ? s + 1 : path, "%s_physics.txt", g_prefix);
        HANDLE h = CreateThread(NULL, 0, report_thread, path, 0, NULL);
        if (h) CloseHandle(h);
    }
    log_printf("physics: stepSimulation hooked (profile=%d, solver_iterations=%d, solver_residual=%g, rate=%d, budget=%.1f ms)",
               g_cfg.phys_profile, g_cfg.solver_iterations, g_cfg.solver_residual, g_cfg.phys_rate, g_cfg.phys_budget_ms);
    return 1;
}
