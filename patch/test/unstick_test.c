/* Native test of unstick.c against fake Bullet objects laid out like the game's (see unstick.c).
 *   gcc -O1 -Wall -Itest/linux_shim -o /tmp/unstick_test test/unstick_test.c && /tmp/unstick_test */
#include "../src/unstick.c"
#include <stdio.h>
#include <stdlib.h>

stw_config g_cfg;
static ULONGLONG clock_ms;
ULONGLONG GetTickCount64(void) { return clock_ms; }
void log_printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("    log: "); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

typedef struct { void **vt; int type; } fake_shape;
static double radius_small(void *s) { (void)s; return 0.5; }
static double radius_big(void *s) { (void)s; return 12.0; }
static void *vt_small[4] = { 0, 0, 0, (void *)radius_small };
static void *vt_big[4] = { 0, 0, 0, (void *)radius_big };

static _Alignas(16) uint8_t terrain[0x200], wheel[0x200], hull[0x200];
static fake_shape s_wheel = { vt_small, 13 }, s_wheel2 = { vt_small, 8 }, s_hull = { vt_big, 31 };
static _Alignas(16) uint8_t man[3][0x610];
static uint8_t *list[3];
static _Alignas(16) uint8_t disp[0x40];
static int fails;

static void body(uint8_t *co, int flags, void *shape)
{
    memset(co, 0, 0x200);
    *(int *)(co + CO_FLAGS) = flags;
    *(int *)(co + CO_INTERNAL_TYPE) = flags ? 1 : CO_RIGID_BODY;
    *(void **)(co + CO_SHAPE) = shape;
}

/* manifold with np points; normal on B, distance */
static void manifold(uint8_t *m, uint8_t *b0, uint8_t *b1, int np, double ny, double nx, double d)
{
    memset(m, 0, 0x610);
    *(uint8_t **)(m + MF_BODY0) = b0;
    *(uint8_t **)(m + MF_BODY1) = b1;
    *(int *)(m + MF_NUM_POINTS) = np;
    for (int p = 0; p < np; p++) {
        uint8_t *pt = m + MF_POINTS + p * MF_POINT_STRIDE;
        double *n = (double *)(pt + PT_NORMAL_ON_B);
        n[0] = nx; n[1] = ny; n[2] = 0;
        *(double *)(pt + PT_DISTANCE) = d;
    }
}

static void set_list(int n) { for (int i = 0; i < 3; i++) list[i] = man[i]; *(int *)(disp + DISP_NUM_MANIFOLDS) = n; *(uint8_t ***)(disp + DISP_MANIFOLDS) = list; }

/* runs passes of 16 ms for ms milliseconds, rebuilding the manifolds each pass like the narrowphase */
static int run(int ms, void (*build)(void), int check_idx)
{
    int zeroed = 0;
    for (int t = 0; t < ms; t += 16) {
        clock_ms += 16;
        build();
        unstick_after_dispatch(disp);
        zeroed = *(int *)(list[check_idx] + MF_NUM_POINTS) == 0;
    }
    return zeroed;
}

static void expect(const char *what, int got, int want)
{
    printf("%-64s %s\n", what, got == want ? "ok" : "FAILED");
    if (got != want) fails++;
}

static void b_stuck(void)      { manifold(man[0], wheel, terrain, 3, -1, 0, -0.5); set_list(1); }
static void b_stuck_swap(void) { manifold(man[0], terrain, wheel, 3, 1, 0, -0.5); set_list(1); }    /* normal on B=wheel */
static void b_above(void)      { manifold(man[0], wheel, terrain, 2, 1, 0, -0.02); set_list(1); }
static void b_above_deep(void) { manifold(man[0], wheel, terrain, 2, 1, 0, -0.6); set_list(1); }
static void b_side(void)       { manifold(man[0], wheel, terrain, 3, 0, 1, -0.5); set_list(1); }
static void b_mixed(void)      { manifold(man[0], wheel, terrain, 2, -1, 0, -0.5); manifold(man[1], wheel, terrain, 2, 1, 0, -0.01); set_list(2); }
static void b_shallow(void)    { manifold(man[0], wheel, terrain, 3, -1, 0, -0.1); set_list(1); }
static void b_hull(void)       { manifold(man[0], hull, terrain, 4, -1, 0, -0.8); set_list(1); }
static void b_none(void)       { set_list(0); }
static void b_owned(void)
{
    b_stuck();
    *(void **)(man[0] + MF_POINTS + PT_USER_DATA) = (void *)1;
}
static void b_wheel_dyn(void)  { manifold(man[0], wheel, hull, 3, -1, 0, -0.5); set_list(1); }      /* dynamic vs dynamic */

static void reset(void)
{
    memset(table, 0, sizeof table);
    body(terrain, 1, NULL);
    body(wheel, 0, &s_wheel);
    body(hull, 0, &s_hull);
    clock_ms += 10000;
}

int main(void)
{
    g_cfg.unstick = 1;
    g_cfg.unstick_depth = 0.3;
    g_cfg.unstick_seconds = 1;
    g_cfg.unstick_max_radius = 3;

    reset();
    expect("stuck wheel, 0.8 s: contacts kept", run(800, b_stuck, 0), 0);
    expect("stuck wheel, +0.3 s: static contacts dropped", run(300, b_stuck, 0), 1);
    expect("still stuck: still dropped", run(500, b_stuck, 0), 1);
    expect("back above the surface 0.1 s: still dropped", run(100, b_above, 0), 1);
    expect("back above the surface +0.2 s: collision restored", run(200, b_above, 0), 0);
    expect("resting normally afterwards: kept", run(3000, b_above, 0), 0);

    reset();
    expect("stuck with bodies swapped in the manifold: dropped", run(1200, b_stuck_swap, 0), 1);

    reset();
    expect("deep but pushed UP (fast landing): kept", run(3000, b_above_deep, 0), 0);
    reset();
    expect("deep, sideways normal (wall): kept", run(3000, b_side, 0), 0);
    reset();
    expect("pushed down on one side, up on another: kept", run(3000, b_mixed, 0), 0);
    reset();
    expect("pushed down but shallow (0.1 m): kept", run(3000, b_shallow, 0), 0);
    reset();
    expect("whole vehicle (radius 12 m) stuck: kept", run(3000, b_hull, 0), 0);
    reset();
    expect("against a dynamic body: kept", run(3000, b_wheel_dyn, 0), 0);
    reset();
    expect("contact owns user data: kept", run(3000, b_owned, 0), 0);

    reset();
    run(1200, b_stuck, 0);
    run(2600, b_none, 0);
    expect("disabled, then 2.6 s without contact: collision restored", run(16, b_shallow, 0), 0);

    reset();
    run(1200, b_stuck, 0);
    *(void **)(wheel + CO_SHAPE) = &s_wheel2;     /* another body allocated at the same address */
    expect("new body at the same address: starts clean", run(16, b_above, 0), 0);
    expect("...and needs its own 1 s to be dropped", run(500, b_stuck, 0), 0);

    char buf[160];
    unstick_format(buf, sizeof buf);
    printf("report: %s\n", buf);
    printf("RESULT: %s\n", fails ? "FAILED" : "OK");
    return fails != 0;
}
