#pragma once
#include <windows.h>
#include <stdint.h>

/* Every game address is found at run time by resolver.c (RTTI names + byte patterns), so the
 * same DLL serves stormworks64.exe, server64.exe and later builds. tools/resolve.py does the
 * same offline. Reference values for stormworks64.exe PE ts 0x6a9a9e20 are in tools/resolve.py. */
typedef struct {
    uint8_t *vt_dispatcher, *vt_hashed_cache, *vt_softrigid, *vt_discrete;
    uint8_t *vt_vehicle_tick, *vt_scene_update;
    uint8_t *step_simulation, *default_near;
    uint8_t *thread_enqueue, *task_wait, *task_set_state, *mtx_lock, *mtx_unlock, *cnd_broadcast;
    void *prof_start, *prof_stop;           /* Start_Profile(name) / Stop_Profile() */
    void **prof_start_ptr, **prof_stop_ptr; /* set when the build calls them through pointers */
    uint8_t *prof_roots;                    /* CProfileNode gRoots[64], stride 0x38 */
    int prof_tls_offset;                    /* thread index offset in the exe TLS block */
    uint32_t *exe_tls_index;
} game_addrs;
extern game_addrs g_addr;
int resolve_addresses(void);

/* CRT + runtime library span (client build), skipped when attributing allocations to game code. */
#define RVA_CRT_BEGIN           0x950000
#define RVA_CRT_END             0xa92000

/* mm_thread_task layout */
#define TASK_OFF_STATE          0x08
#define TASK_OFF_MUTEX          0x10
#define TASK_OFF_CONDVAR        0x68
#define TASK_OFF_PENDING        0xb0

typedef struct {
    int vehicle_threads;      /* 0 = keep the game's single vehicle_tick thread */
    int vehicle_thread_prio;  /* Win32 thread priority for pool workers */
    int heap_track;           /* 1 = allocation tracker (diagnostic, small overhead) */
    int fast_alloc;           /* 1 = mimalloc for the CRT heap */
    int heap_sample;          /* 1 allocation out of N gets a call stack */
    int heap_report_sec;      /* report period */
    int timer_1ms;            /* timeBeginPeriod(1) */
    int phys_profile;         /* Bullet phase timings + main-thread wait times */
    int phys_report_sec;
    int solver_iterations;    /* 0 = game default (Bullet: 10) */
    double solver_residual;   /* 0 = game default (no early exit) */
    int narrow_threads;       /* workers for the parallel narrowphase, 0 = off */
    int narrow_min_pairs;     /* below this many overlapping pairs stay serial */
    int narrow_balance;       /* 1 = heaviest pairs of the previous pass first */
    int phys_rate;            /* Bullet fixed rate: 0 game, 60/120/180 forced, -1 stwpatch governor */
    double phys_budget_ms;    /* governor: target step time per tick */
    int net_monitor;          /* 1 = per-player network accounting / report */
    int net_send_buffer_kb, net_rate_min_kb, net_rate_max_kb;   /* 0 = Steam default */
    char version_suffix[64];  /* appended to the main-menu version label, "" = off */
    int sampler_ms;           /* 0 = off; sampling period of the loading profiler */
    int safe_mode;            /* 1 = after a crash during startup, next launch runs unpatched */
} stw_config;

extern stw_config g_cfg;
extern uint8_t *g_base;
extern const char *g_prefix;   /* "stwpatch" (client) or "stwpatch_server": output file names */

int  crashguard_init(HMODULE dll, const char *dir);
void crashguard_stage(const char *stage);
void crashguard_clean_exit(void);
void log_printf(const char *fmt, ...);
int  vehicle_pool_install(void);
int  heap_track_install(void);
int  fastalloc_install(void);
int  physics_install(void);
int  narrowphase_install(void);
int  sampler_install(void);
int  net_install(void);
int  branding_install(void);
extern volatile ULONGLONG g_last_tick_ms;
extern volatile DWORD g_tick_tid;     /* the game thread that ticks (waits on physics) */
extern ULONGLONG g_start_ms;   /* GetTickCount64() when the DLL loaded */
void narrowphase_format(char *buf, size_t size);
int  patch_jump(uint8_t *at, size_t len, void *to);
void *patch_ptr(void **slot, void *value);
void *make_trampoline(uint8_t *target, size_t stolen, const uint8_t *expect);
