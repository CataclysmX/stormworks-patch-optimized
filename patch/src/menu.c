/* F4 settings menu: the model (items, live apply, save to stwpatch.ini); overlay.c draws it and
 * feeds it the keys. Live items change g_cfg, which the patches read at each use (unstick and the
 * narrowphase at every dispatch, the physics rate and solver at every step, the compound split
 * at every pair, mimalloc's purge option at every free); the others need a restart and are shown
 * as such. Labels are UTF-8 (overlay.c draws with TextOutW). */
#include "stwpatch.h"
#include <mimalloc.h>
#include <stdio.h>
#include <string.h>

enum { M_BOOL, M_INT, M_DOUBLE, M_RATE, M_FPS, M_SAVE };

typedef struct {
    const char *label;
    int kind;
    void *value;
    double min, max, step;
    const char *section, *key;
    int restart;                  /* 1 = applies at the next launch */
    double def;                   /* value of a fresh install (stwpatch.ini template); AUTO_THREADS_* = computed */
} item_t;

#define AUTO_VEHICLE_THREADS -1001
#define AUTO_NARROW_THREADS  -1002

static const item_t items[] = {
    { "Roues / blocs coincés sous la map", M_BOOL, &g_cfg.unstick, 0, 1, 1, "fixes", "unstick", 0, 1 },
    { "   profondeur minimale (m)", M_DOUBLE, &g_cfg.unstick_depth, 0.05, 2, 0.05, "fixes", "unstick_depth", 0, 0.3 },
    { "   durée avant coupure (s)", M_DOUBLE, &g_cfg.unstick_seconds, 0.2, 10, 0.1, "fixes", "unstick_seconds", 0, 1 },
    { "   rayon maximal du bloc (m)", M_DOUBLE, &g_cfg.unstick_max_radius, 0.5, 30, 0.5, "fixes", "unstick_max_radius", 0, 3 },
    { "Découpage des gros véhicules", M_BOOL, &g_cfg.compound_split, 0, 1, 1, "physics", "compound_split", 0, 1 },
    { "Limite d'images par seconde", M_FPS, &g_cfg.fps_cap, 0, 0, 0, "misc", "fps_cap", 0, 0 },
    { "Fréquence physique", M_RATE, &g_cfg.phys_rate, 0, 0, 0, "physics", "rate", 0, -1 },
    { "   budget du gouverneur (ms)", M_DOUBLE, &g_cfg.phys_budget_ms, 2, 16.5, 0.5, "physics", "budget_ms", 0, 12 },
    { "Itérations du solveur (0 = jeu)", M_INT, &g_cfg.solver_iterations, 0, 30, 1, "physics", "solver_iterations", 0, 0 },
    { "Collisions : paires min. en parallèle", M_INT, &g_cfg.narrow_min_pairs, 16, 1024, 16, "physics", "narrowphase_min_pairs", 0, 64 },
    { "Collisions : paires lourdes d'abord", M_BOOL, &g_cfg.narrow_balance, 0, 1, 1, "physics", "narrowphase_balance", 0, 1 },
    { "Atelier : contours fil de fer", M_BOOL, &g_cfg.editor_wireframe, 0, 1, 1, "editor", "wireframe", 0, 1 },
    { "Rendre la mémoire à Windows (crashs)", M_BOOL, &g_cfg.alloc_purge, 0, 1, 1, "memory", "allocator_purge", 0, 0 },
    { "Correctif suppression en symétrie", M_BOOL, &g_cfg.fix_editor_mirror, 0, 1, 1, "fixes", "editor_mirror_delete", 1, 1 },
    { "Statistiques OpenGL", M_BOOL, &g_cfg.gl_stats, 0, 1, 1, "profile", "gl_stats", 1, 1 },
    { "Échantillonneur (ms, 0 = off)", M_INT, &g_cfg.sampler_ms, 0, 20, 1, "profile", "sampler_ms", 1, 0 },
    { "Threads véhicules (0 = jeu)", M_INT, &g_cfg.vehicle_threads, 0, 64, 1, "vehicle", "threads", 1, AUTO_VEHICLE_THREADS },
    { "Threads collisions (0 = off)", M_INT, &g_cfg.narrow_threads, 0, 32, 1, "physics", "narrowphase_threads", 1, AUTO_NARROW_THREADS },
    { "Enregistrer dans stwpatch.ini", M_SAVE, NULL, 0, 0, 0, NULL, NULL, 0, 0 },
};
#define N_ITEMS ((int)(sizeof items / sizeof items[0]))

static const int rates[] = { -1, 0, 60, 120, 180 };
static const int fps_caps[] = { 0, 30, 45, 60, 75, 90, 120, 144, 165 };
static int  dirty;
static char status[160];

int menu_item_count(void) { return N_ITEMS; }
int menu_dirty(void) { return dirty; }
const char *menu_status(void) { return status; }

/* a live item whose hook is not installed in this session only takes effect after a restart */
static int needs_restart(const item_t *it)
{
    if (it->restart) return 1;
    if (it->value == &g_cfg.compound_split) return !g_addr.compound_split_on;
    if (!strcmp(it->section, "fixes") && !strncmp(it->key, "unstick", 7)) return !g_addr.narrow_hook_on;
    if (!strncmp(it->key, "narrowphase_", 12)) return !g_addr.narrow_hook_on;
    if (it->value == &g_cfg.alloc_purge) return !g_cfg.fast_alloc;
    if (it->value == &g_cfg.editor_wireframe) return !g_addr.editor_wire_on;
    return 0;
}

static int auto_threads(int which)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = which == AUTO_VEHICLE_THREADS ? (int)si.dwNumberOfProcessors - 3 : (int)si.dwNumberOfProcessors / 2 - 1;
    return n < 2 ? 2 : n > 8 ? 8 : n;              /* same rule as dllmain.c read_config */
}

static void format_value(int kind, double x, char *buf, size_t n)
{
    switch (kind) {
    case M_BOOL:   snprintf(buf, n, "%s", x != 0 ? "OUI" : "non"); break;
    case M_INT:    snprintf(buf, n, "%d", (int)x); break;
    case M_DOUBLE: snprintf(buf, n, "%.2f", x); break;
    case M_FPS:
        if (x > 0) snprintf(buf, n, "%d images/s", (int)x);
        else snprintf(buf, n, "aucune");
        break;
    case M_RATE: {
        int r = (int)x;
        if (r < 0) snprintf(buf, n, "gouverneur stwpatch");
        else if (r == 0) snprintf(buf, n, "jeu");
        else snprintf(buf, n, "%d Hz", r);
        break;
    }
    default: buf[0] = 0;
    }
}

static double current(const item_t *it)
{
    if (!it->value) return 0;                    /* the save line has no value */
    return it->kind == M_DOUBLE ? *(double *)it->value : (double)*(int *)it->value;
}

static void value_text(const item_t *it, char *buf, size_t n) { format_value(it->kind, current(it), buf, n); }

static void default_text(const item_t *it, char *buf, size_t n)
{
    if (it->def == AUTO_VEHICLE_THREADS || it->def == AUTO_NARROW_THREADS)
        snprintf(buf, n, "auto, %d", auto_threads((int)it->def));
    else
        format_value(it->kind, it->def, buf, n);
}

/* kind: 0 live, 1 needs a restart, 2 action */
void menu_line(int i, char *buf, size_t n, int *kind)
{
    const item_t *it = &items[i];
    char v[48];
    value_text(it, v, sizeof v);
    if (it->kind == M_SAVE) {
        snprintf(buf, n, "%s%s", it->label, dirty ? "  (modifications non enregistrées)" : "");
        *kind = 2;
        return;
    }
    int restart = needs_restart(it);
    /* pad to 42 characters, not bytes: accented letters take 2 bytes in UTF-8 */
    int chars = 0;
    for (const unsigned char *c = (const unsigned char *)it->label; *c; c++) chars += (*c & 0xC0) != 0x80;
    char d[48];
    default_text(it, d, sizeof d);
    int vchars = 0;
    for (const unsigned char *c = (const unsigned char *)v; *c; c++) vchars += (*c & 0xC0) != 0x80;
    snprintf(buf, n, "%s%*s %s%*s (défaut : %s)%s", it->label, chars < 42 ? 42 - chars : 0, "", v,
             vchars < 20 ? 20 - vchars : 0, "", d, restart ? "  *" : "");
    *kind = restart;
}

static void apply(const item_t *it)
{
    if (it->value == &g_cfg.editor_wireframe) editor_wire_apply();
    if (it->value == &g_cfg.alloc_purge && g_cfg.fast_alloc)
        mi_option_set(mi_option_purge_delay, g_cfg.alloc_purge ? 10 : -1);
}

static void save(void)
{
    char v[48];
    int ok = 1;
    for (int i = 0; i < N_ITEMS; i++) {
        const item_t *it = &items[i];
        if (!it->key) continue;
        if (it->kind == M_DOUBLE) snprintf(v, sizeof v, "%g", *(double *)it->value);
        else snprintf(v, sizeof v, "%d", *(int *)it->value);
        ok &= WritePrivateProfileStringA(it->section, it->key, v, g_ini_path) != 0;
    }
    dirty = !ok;
    snprintf(status, sizeof status, ok ? "enregistré dans stwpatch.ini" : "échec de l'écriture de stwpatch.ini");
    log_printf("menu: settings %s %s", ok ? "saved to" : "could not be written to", g_ini_path);
}

/* dir: -1 / +1 (left / right), 0 (Enter) */
void menu_change(int i, int dir)
{
    const item_t *it = &items[i];
    if (it->kind == M_SAVE) { if (dir == 0) save(); return; }
    switch (it->kind) {
    case M_BOOL:
        *(int *)it->value = dir == 0 ? !*(int *)it->value : dir > 0;
        break;
    case M_INT: {
        if (dir == 0) return;
        int x = *(int *)it->value + dir * (int)it->step;
        *(int *)it->value = x < it->min ? (int)it->min : x > it->max ? (int)it->max : x;
        break;
    }
    case M_DOUBLE: {
        if (dir == 0) return;
        double x = *(double *)it->value + dir * it->step;
        x = x < it->min ? it->min : x > it->max ? it->max : x;
        *(double *)it->value = (double)(long long)(x / it->step + 0.5) * it->step;   /* no 0.30000000004 */
        break;
    }
    case M_RATE:
    case M_FPS: {
        const int *list = it->kind == M_RATE ? rates : fps_caps;
        int n = it->kind == M_RATE ? (int)(sizeof rates / sizeof rates[0]) : (int)(sizeof fps_caps / sizeof fps_caps[0]);
        int k = 0;
        while (k < n && list[k] != *(int *)it->value) k++;
        if (k == n) k = 0;
        k = (k + (dir < 0 ? n - 1 : 1)) % n;
        *(int *)it->value = list[k];
        break;
    }
    }
    dirty = 1;
    status[0] = 0;
    apply(it);
    char v[48];
    value_text(it, v, sizeof v);
    log_printf("menu: %s.%s = %s%s", it->section, it->key, v, needs_restart(it) ? " (next launch)" : "");
}
