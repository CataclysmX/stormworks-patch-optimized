/* Unstick: small bodies (wheels, loose blocks) caught under the terrain.
 *
 * Game bug, with or without stwpatch: a wheel or a block can end up below a terrain (or other
 * static) mesh while still attached to its vehicle. Bullet's triangle contacts are two-sided, so
 * once the body is more than halfway through, the terrain pushes it further DOWN while the vehicle
 * pulls it up: deep contacts against many triangles every step, a jittering vehicle and a
 * physics step that gets expensive.
 *
 * After the narrowphase (all contact manifolds are up to date, the solver has not run yet), every
 * manifold between a dynamic rigid body and static/kinematic geometry is inspected:
 *   stuck contact  penetration deeper than unstick_depth AND the static side pushes the body
 *                  down (contact normal towards the body against the gravity's "up")
 *   good contact   the static side pushes the body up: it is above the surface
 * A body with stuck contacts and no good one for unstick_seconds, whose bounding radius is at most
 * unstick_max_radius (so never a whole vehicle), has its contacts with static geometry dropped
 * (manifold point count set to 0) from then on: it passes through the terrain, still collides
 * with every dynamic body, and its vehicle no longer fights the ground. Its collision with static
 * geometry comes back as soon as the static side pushes it up again (back above the surface) for
 * 0.2 s, or after 2 s without any contact with static geometry.
 *
 * Bullet layouts (double precision, checked on the 2026-10-29 build):
 *   btCollisionDispatcher  m_manifoldsPtr: size +0x14, data +0x20
 *   btPersistentManifold   points +0x10 (stride 0x170), m_body0 +0x5d0, m_body1 +0x5d8,
 *                          m_cachedPoints +0x5e0       (constructor FUN_1400d7c10)
 *   btManifoldPoint        m_normalWorldOnB +0x80, m_distance1 +0xa0, m_userPersistentData +0xd8
 *                          (btManifoldResult::addContactPoint FUN_1400f4230)
 *   btCollisionObject      m_collisionShape +0x188, m_collisionFlags +0x1a0 (needsResponse:
 *                          0x803 = static/kinematic, 4 = no contact response), m_internalType +0x1f0
 *   btCollisionShape       vtable[3] getAngularMotionDisc (bounding radius)
 *   btDiscreteDynamicsWorld m_gravity +0x1e0 (getGravity = vtable[18]), read by physics.c */
#include "stwpatch.h"
#include <stdio.h>
#include <string.h>

#define DISP_NUM_MANIFOLDS  0x14
#define DISP_MANIFOLDS      0x20
#define MF_POINTS           0x10
#define MF_POINT_STRIDE     0x170
#define MF_BODY0            0x5d0
#define MF_BODY1            0x5d8
#define MF_NUM_POINTS       0x5e0
#define PT_NORMAL_ON_B      0x80
#define PT_DISTANCE         0xa0
#define PT_USER_DATA        0xd8
#define CO_SHAPE            0x188
#define CO_FLAGS            0x1a0
#define CO_INTERNAL_TYPE    0x1f0
#define CO_RIGID_BODY       2
#define CF_STATIC_KINEMATIC 0x803
#define CF_NO_RESPONSE      4

#define TABLE_SIZE   4096                /* power of two */
#define RELEASE_MS   200
#define EXPIRE_MS    2000
#define FORGET_MS    5000
#define LOG_MAX      100

typedef double (*motion_disc_t)(void *shape);

typedef struct {
    uint8_t  *body, *shape;
    double    radius;                    /* < 0: not computed yet */
    ULONGLONG stuck_since, ok_since, last_contact, last_seen;
    double    depth;                     /* deepest stuck penetration seen */
    uint32_t  pass;                      /* last pass that touched the entry */
    uint8_t   saw_stuck, saw_ok, disabled, too_big;
} entry_t;

volatile double g_world_up[3] = { 0, 1, 0 };   /* -gravity, normalised; written by physics.c */

static entry_t   table[TABLE_SIZE];
static uint32_t  pass_id;
static ULONGLONG last_sweep;
static SRWLOCK   lock = SRWLOCK_INIT;
static int       n_logged;
static volatile LONG64 stat_unstuck, stat_restored;

static entry_t *find(uint8_t *body, int create)
{
    size_t h = ((uintptr_t)body >> 4) * 0x9E3779B97F4A7C15ull >> 52;   /* 12 bits */
    for (size_t k = 0; k < TABLE_SIZE; k++) {
        entry_t *e = &table[(h + k) & (TABLE_SIZE - 1)];
        if (e->body == body) return e;
        if (!e->body) {
            if (!create) return NULL;
            memset(e, 0, sizeof *e);
            e->body = body;
            e->radius = -1;
            return e;
        }
    }
    return NULL;                         /* full: ignore this body */
}

/* drop entries not seen for a while (rebuilds the table: linear probing has no deletion) */
static void sweep(ULONGLONG now)
{
    static entry_t keep[TABLE_SIZE];
    int n = 0;
    for (int i = 0; i < TABLE_SIZE; i++) {
        entry_t *e = &table[i];
        if (!e->body) continue;
        if (e->disabled && now - e->last_contact > EXPIRE_MS) {
            e->disabled = 0;
            InterlockedIncrement64(&stat_restored);
            if (n_logged < LOG_MAX) {
                n_logged++;
                log_printf("unstick: body %p, no contact with static geometry for %d s: collision restored",
                           e->body, EXPIRE_MS / 1000);
            }
        }
        if (e->disabled || now - e->last_seen < FORGET_MS) keep[n++] = *e;
    }
    memset(table, 0, sizeof table);
    for (int i = 0; i < n; i++) {
        entry_t *e = find(keep[i].body, 1);
        if (e) *e = keep[i];
    }
}

static inline int is_static(uint8_t *co) { return (*(int *)(co + CO_FLAGS) & CF_STATIC_KINEMATIC) != 0; }

static inline int is_dynamic_rigid(uint8_t *co)
{
    int f = *(int *)(co + CO_FLAGS);
    return !(f & (CF_STATIC_KINEMATIC | CF_NO_RESPONSE)) && (*(int *)(co + CO_INTERNAL_TYPE) & CO_RIGID_BODY);
}

/* manifold -> (dynamic body, sign so that sign * normalWorldOnB points towards it) */
static uint8_t *dynamic_side(uint8_t *m, double *sign)
{
    uint8_t *b0 = *(uint8_t **)(m + MF_BODY0), *b1 = *(uint8_t **)(m + MF_BODY1);
    if (!b0 || !b1) return NULL;
    if (is_static(b1) && is_dynamic_rigid(b0)) { *sign = 1; return b0; }    /* normal on B points to A */
    if (is_static(b0) && is_dynamic_rigid(b1)) { *sign = -1; return b1; }
    return NULL;
}

void unstick_after_dispatch(void *disp)
{
    static int was_on;
    if (!g_cfg.unstick || !disp) { was_on = 0; return; }
    if (!was_on) {                       /* (re)enabled from the F4 menu: forget old states */
        was_on = 1;
        if (TryAcquireSRWLockExclusive(&lock)) { memset(table, 0, sizeof table); ReleaseSRWLockExclusive(&lock); }
    }
    int n = *(int *)((uint8_t *)disp + DISP_NUM_MANIFOLDS);
    uint8_t **mf = *(uint8_t ***)((uint8_t *)disp + DISP_MANIFOLDS);
    if (!TryAcquireSRWLockExclusive(&lock)) return;
    ULONGLONG now = GetTickCount64();
    /* expiry first: a body without static contact for EXPIRE_MS gets its collision back before
     * this pass looks at its new contacts (and even when there is no manifold at all) */
    if (now - last_sweep > 250) {
        last_sweep = now;
        sweep(now);
    }
    if (n <= 0 || !mf) { ReleaseSRWLockExclusive(&lock); return; }
    uint32_t pass = ++pass_id;
    double up[3] = { g_world_up[0], g_world_up[1], g_world_up[2] };
    double depth_lim = -g_cfg.unstick_depth;

    /* 1. classify contacts per dynamic body */
    for (int i = 0; i < n; i++) {
        uint8_t *m = mf[i];
        int np = m ? *(int *)(m + MF_NUM_POINTS) : 0;
        if (np <= 0 || np > 4) continue;
        double sign;
        uint8_t *body = dynamic_side(m, &sign);
        if (!body) continue;
        int stuck = 0, ok = 0;
        double deepest = 0;
        for (int p = 0; p < np; p++) {
            uint8_t *pt = m + MF_POINTS + (size_t)p * MF_POINT_STRIDE;
            double *nb = (double *)(pt + PT_NORMAL_ON_B), d = *(double *)(pt + PT_DISTANCE);
            double s = sign * (nb[0] * up[0] + nb[1] * up[1] + nb[2] * up[2]);
            if (s > 0.5) ok = 1;
            else if (s < -0.5 && d < depth_lim) { stuck = 1; if (d < deepest) deepest = d; }
        }
        entry_t *e = find(body, stuck);
        if (!e) continue;
        uint8_t *shape = *(uint8_t **)(body + CO_SHAPE);
        if (e->shape != shape) {                  /* another body at the same address */
            uint8_t *b = e->body;
            memset(e, 0, sizeof *e);
            e->body = b;
            e->shape = shape;
            e->radius = -1;
        }
        if (e->pass != pass) { e->pass = pass; e->saw_stuck = e->saw_ok = 0; }
        e->saw_stuck |= stuck;
        e->saw_ok |= ok;
        if (deepest < e->depth) e->depth = deepest;
        e->last_contact = e->last_seen = now;
    }

    /* 2. state changes, 3. drop the contacts of disabled bodies */
    for (int i = 0; i < n; i++) {
        uint8_t *m = mf[i];
        int np = m ? *(int *)(m + MF_NUM_POINTS) : 0;
        if (np <= 0 || np > 4) continue;
        double sign;
        uint8_t *body = dynamic_side(m, &sign);
        entry_t *e = body ? find(body, 0) : NULL;
        if (!e) continue;
        if (e->pass == pass) {
            e->pass = pass - 1;                   /* decide once per body and pass */
            if (!e->disabled) {
                if (e->saw_stuck && !e->saw_ok) {
                    if (!e->stuck_since) e->stuck_since = now;
                    if (!e->too_big && now - e->stuck_since >= (ULONGLONG)(g_cfg.unstick_seconds * 1000)) {
                        if (e->radius < 0 && e->shape)
                            e->radius = ((motion_disc_t)(*(void ***)e->shape)[3])(e->shape);
                        if (e->radius > g_cfg.unstick_max_radius) {
                            e->too_big = 1;
                        } else {
                            e->disabled = 1;
                            e->ok_since = 0;
                            InterlockedIncrement64(&stat_unstuck);
                            if (n_logged < LOG_MAX) {
                                n_logged++;
                                log_printf("unstick: body %p (shape type %d, radius %.2f m) pushed down into "
                                           "static geometry for %.1f s, %.2f m deep: its contacts with static "
                                           "geometry are now ignored", body, *(int *)(e->shape + 8), e->radius,
                                           (now - e->stuck_since) / 1000.0, -e->depth);
                            }
                        }
                    }
                } else {
                    e->stuck_since = 0;
                    e->depth = 0;
                }
            } else if (e->saw_ok && !e->saw_stuck) {
                if (!e->ok_since) e->ok_since = now;
                if (now - e->ok_since >= RELEASE_MS) {
                    e->disabled = 0;
                    e->stuck_since = 0;
                    e->depth = 0;
                    InterlockedIncrement64(&stat_restored);
                    if (n_logged < LOG_MAX) {
                        n_logged++;
                        log_printf("unstick: body %p back above the surface: collision restored", body);
                    }
                }
            } else {
                e->ok_since = 0;
            }
        }
        if (e->disabled) {
            int owned = 0;
            for (int p = 0; p < np; p++)
                owned |= *(void **)(m + MF_POINTS + (size_t)p * MF_POINT_STRIDE + PT_USER_DATA) != NULL;
            if (!owned) *(int *)(m + MF_NUM_POINTS) = 0;   /* clearManifold without user data */
        }
    }
    ReleaseSRWLockExclusive(&lock);
}

void unstick_format(char *buf, size_t size)
{
    int disabled = 0;
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < TABLE_SIZE; i++) disabled += table[i].body && table[i].disabled;
    ReleaseSRWLockShared(&lock);
    snprintf(buf, size, "%d bodies without static collision now, %lld unstuck, %lld restored (total)",
             disabled, (long long)stat_unstuck, (long long)stat_restored);
}
