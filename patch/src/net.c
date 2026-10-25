/* Multiplayer (in-game hosting): measure and tune the Steam networking path.
 *
 * Game traffic goes through ISteamNetworkingMessages002 (SendMessageToUser x4 call sites,
 * ReceiveMessagesOnChannel, CloseSessionWithUser, GetSessionConnectionInfo). That API has no
 * per-session options: sessions use Steam's global networking config, which the game never
 * changes (no ISteamNetworkingUtils in the exe). The player list shows a per-player "ticks behind"
 * counter: < 5 synced, < 10 slow, < 50 desynced (FUN_140793e20).
 *
 * - The exe imports SteamInternal_FindOrCreateUserInterface; its IAT entry is redirected so the
 *   game gets a thin wrapper around the real ISteamNetworkingMessages002 that forwards all six
 *   methods and accounts bytes / messages / failed sends per remote player.
 * - Every report period, GetSessionConnectionInfo gives ping, quality, the send rate Steam
 *   actually uses, pending bytes and queue time per player -> <prefix>_net.txt.
 * - [network] options are applied once with SetGlobalConfigValueInt32 (public Steam API).
 * No Steam file and no Steam memory is modified. */
#include "stwpatch.h"
#include <stdio.h>
#include <string.h>

#define CFG_SEND_BUFFER_SIZE 9      /* ESteamNetworkingConfigValue */
#define CFG_SEND_RATE_MIN    10
#define CFG_SEND_RATE_MAX    11
#define SEND_RELIABLE        8      /* k_nSteamNetworkingSend_Reliable */
#define RESULT_OK            1
#define RESULT_LIMIT_EXCEEDED 25

typedef struct { int type; int size; uint8_t data[128]; } identity_t;   /* SteamNetworkingIdentity */

typedef struct {                    /* SteamNetConnectionRealTimeStatus_t */
    int state, ping;
    float quality_local, quality_remote, out_packets, out_bytes, in_packets, in_bytes;
    int send_rate, pending_unreliable, pending_reliable, sent_unacked_reliable;
    int64_t queue_usec;
    uint32_t reserved[16];
} rt_status_t;

typedef int  (*send_t)(void *self, const identity_t *id, const void *data, uint32_t cb, int flags, int channel);
typedef int  (*recv_t)(void *self, int channel, void **msgs, int max);
typedef char (*accept_t)(void *self, const identity_t *id);
typedef char (*close_t)(void *self, const identity_t *id);
typedef char (*close_ch_t)(void *self, const identity_t *id, int channel);
typedef int  (*info_t)(void *self, const identity_t *id, void *info, rt_status_t *status);

typedef void *(*find_iface_t)(int user, const char *version);
static find_iface_t real_find, real_find_gs;   /* client: ...UserInterface, dedicated server: ...GameServerInterface */

static void **real_msgs;            /* the real interface: vtable pointer at offset 0 */
static struct { void **vt; } wrapper;

#define MAX_PEERS 64
typedef struct {
    identity_t id;
    LONG64 bytes, msgs, reliable_bytes, fails, limit_fails;
    LONG64 last_bytes;
} peer_t;
static peer_t peers[MAX_PEERS];
static int n_peers;
static SRWLOCK peer_lock = SRWLOCK_INIT;

static peer_t *peer_for(const identity_t *id)
{
    size_t n = 8 + (id->size > 0 && id->size <= 128 ? id->size : 128);
    AcquireSRWLockShared(&peer_lock);
    for (int i = 0; i < n_peers; i++)
        if (!memcmp(&peers[i].id, id, n)) { ReleaseSRWLockShared(&peer_lock); return &peers[i]; }
    ReleaseSRWLockShared(&peer_lock);
    AcquireSRWLockExclusive(&peer_lock);
    peer_t *p = NULL;
    for (int i = 0; i < n_peers && !p; i++) if (!memcmp(&peers[i].id, id, n)) p = &peers[i];
    if (!p && n_peers < MAX_PEERS) { p = &peers[n_peers++]; memset(p, 0, sizeof *p); memcpy(&p->id, id, n); }
    ReleaseSRWLockExclusive(&peer_lock);
    return p;
}

static int w_send(void *self, const identity_t *id, const void *data, uint32_t cb, int flags, int channel)
{
    (void)self;
    int r = ((send_t)((void **)*real_msgs)[0])(real_msgs, id, data, cb, flags, channel);
    peer_t *p = peer_for(id);
    if (p) {
        InterlockedAdd64(&p->bytes, cb);
        InterlockedIncrement64(&p->msgs);
        if (flags & SEND_RELIABLE) InterlockedAdd64(&p->reliable_bytes, cb);
        if (r != RESULT_OK) InterlockedIncrement64(&p->fails);
        if (r == RESULT_LIMIT_EXCEEDED) InterlockedIncrement64(&p->limit_fails);
    }
    return r;
}
static int  w_recv(void *self, int ch, void **m, int max) { (void)self; return ((recv_t)((void **)*real_msgs)[1])(real_msgs, ch, m, max); }
static char w_accept(void *self, const identity_t *id)  { (void)self; return ((accept_t)((void **)*real_msgs)[2])(real_msgs, id); }
static char w_close(void *self, const identity_t *id)   { (void)self; return ((close_t)((void **)*real_msgs)[3])(real_msgs, id); }
static char w_close_ch(void *self, const identity_t *id, int ch) { (void)self; return ((close_ch_t)((void **)*real_msgs)[4])(real_msgs, id, ch); }
static int  w_info(void *self, const identity_t *id, void *info, rt_status_t *st) { (void)self; return ((info_t)((void **)*real_msgs)[5])(real_msgs, id, info, st); }

static void *wrapper_vt[6] = { (void *)w_send, (void *)w_recv, (void *)w_accept, (void *)w_close, (void *)w_close_ch, (void *)w_info };

/* ---- Steam global config (public flat API exported by steam_api64.dll) ---- */
typedef void *(*utils_accessor_t)(void);
typedef char  (*set_global_i32_t)(void *utils, int value, int32_t v);
typedef int   (*get_config_t)(void *utils, int value, int scope_type, intptr_t scope, int *out_type, void *result, size_t *cb);

static int32_t read_global(void *utils, get_config_t get, int value)
{
    int32_t v = -1; int type = 0; size_t cb = sizeof v;
    if (get && get(utils, value, 1 /* k_ESteamNetworkingConfig_Global */, 0, &type, &v, &cb) <= 0) return -1;
    return v;
}

static void apply_config(void)
{
    HMODULE sa = GetModuleHandleA("steam_api64.dll");
    utils_accessor_t acc = sa ? (utils_accessor_t)(void *)GetProcAddress(sa, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004") : NULL;
    set_global_i32_t set = sa ? (set_global_i32_t)(void *)GetProcAddress(sa, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32") : NULL;
    get_config_t get = sa ? (get_config_t)(void *)GetProcAddress(sa, "SteamAPI_ISteamNetworkingUtils_GetConfigValue") : NULL;
    void *utils = acc ? acc() : NULL;
    if (!utils || !set) { log_printf("network: Steam networking utils not available, config unchanged"); return; }
    log_printf("network: Steam defaults: send buffer %d KB, send rate min %d KB/s, max %d KB/s",
               read_global(utils, get, CFG_SEND_BUFFER_SIZE) / 1024, read_global(utils, get, CFG_SEND_RATE_MIN) / 1024,
               read_global(utils, get, CFG_SEND_RATE_MAX) / 1024);
    if (g_cfg.net_send_buffer_kb > 0) set(utils, CFG_SEND_BUFFER_SIZE, g_cfg.net_send_buffer_kb * 1024);
    if (g_cfg.net_rate_max_kb > 0)    set(utils, CFG_SEND_RATE_MAX, g_cfg.net_rate_max_kb * 1024);
    if (g_cfg.net_rate_min_kb > 0)    set(utils, CFG_SEND_RATE_MIN, g_cfg.net_rate_min_kb * 1024);
    log_printf("network: now: send buffer %d KB, send rate min %d KB/s, max %d KB/s",
               read_global(utils, get, CFG_SEND_BUFFER_SIZE) / 1024, read_global(utils, get, CFG_SEND_RATE_MIN) / 1024,
               read_global(utils, get, CFG_SEND_RATE_MAX) / 1024);
}

static void *wrap_if_messages(void *r, const char *version);

static void *hk_find(int user, const char *version) { return wrap_if_messages(real_find(user, version), version); }
static void *hk_find_gs(int user, const char *version) { return wrap_if_messages(real_find_gs(user, version), version); }

static void *wrap_if_messages(void *r, const char *version)
{
    if (r && version && !strcmp(version, "SteamNetworkingMessages002") && !real_msgs) {
        real_msgs = r;
        wrapper.vt = wrapper_vt;
        apply_config();                          /* Steam is initialised by now */
        log_printf("network: SteamNetworkingMessages002 wrapped (accounting per player)");
    }
    if (r == real_msgs && real_msgs) return &wrapper;
    return r;
}

static const char *id_text(const identity_t *id, char *buf, size_t n)
{
    if (id->type == 16 /* k_ESteamNetworkingIdentityType_SteamID */) snprintf(buf, n, "steam:%llu", *(unsigned long long *)id->data);
    else snprintf(buf, n, "type%d", id->type);
    return buf;
}

static DWORD WINAPI report_thread(LPVOID p)
{
    const char *path = p;
    static uint8_t info[2048];                   /* SteamNetConnectionInfo_t, not inspected */
    for (;;) {
        Sleep((DWORD)g_cfg.phys_report_sec * 1000);
        if (!real_msgs || !n_peers) continue;
        FILE *f = fopen(path, "w");
        if (!f) continue;
        SYSTEMTIME t;
        GetLocalTime(&t);
        double sec = g_cfg.phys_report_sec;
        fprintf(f, "stwpatch network report %02d:%02d:%02d (last %ds), %d remote players seen\n", t.wHour, t.wMinute,
                t.wSecond, g_cfg.phys_report_sec, n_peers);
        fprintf(f, "%-24s %6s %6s %9s %9s %9s %9s %9s %8s %7s\n", "player", "ping", "qual%", "sent KB/s", "steam KB/s",
                "rate KB/s", "pend rel", "pend unr", "queue ms", "dropped");
        for (int i = 0; i < n_peers; i++) {
            peer_t *pe = &peers[i];
            rt_status_t st;
            memset(&st, 0, sizeof st);
            int state = ((info_t)((void **)*real_msgs)[5])(real_msgs, &pe->id, info, &st);
            LONG64 b = pe->bytes;
            double kbs = (b - pe->last_bytes) / 1024.0 / sec;
            pe->last_bytes = b;
            char name[48];
            if (state <= 0 && kbs == 0) continue;     /* gone */
            fprintf(f, "%-24s %6d %6.0f %9.1f %9.1f %9.1f %9d %9d %8.1f %7lld\n", id_text(&pe->id, name, sizeof name),
                    st.ping, st.quality_local * 100, kbs, st.out_bytes / 1024, st.send_rate / 1024.0, st.pending_reliable,
                    st.pending_unreliable, st.queue_usec / 1000.0, (long long)pe->limit_fails);
            if (st.queue_usec > 200000 || pe->limit_fails)
                log_printf("network: %s queue %.0f ms, pending %d KB, send rate %.0f KB/s, %lld sends refused (buffer full)",
                           name, st.queue_usec / 1000.0, (st.pending_reliable + st.pending_unreliable) / 1024,
                           st.send_rate / 1024.0, (long long)pe->limit_fails);
        }
        fprintf(f, "\nqueue ms rising / sends refused = the host cannot push its updates fast enough to that player\n"
                   "(raise [network] send_rate_min_kb if the upload allows it); ping or quality bad = connection problem.\n");
        fclose(f);
    }
    return 0;
}

int net_install(void)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int hooked = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(g_base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(g_base + d->Name), "steam_api64.dll") != 0) continue;
        IMAGE_THUNK_DATA64 *names = (void *)(g_base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (void *)(g_base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const char *fn = (char *)((IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData))->Name;
            if (!strcmp(fn, "SteamInternal_FindOrCreateUserInterface")) {
                real_find = (find_iface_t)patch_ptr((void **)&iat->u1.Function, (void *)hk_find); hooked++;
            } else if (!strcmp(fn, "SteamInternal_FindOrCreateGameServerInterface")) {
                real_find_gs = (find_iface_t)patch_ptr((void **)&iat->u1.Function, (void *)hk_find_gs); hooked++;
            }
        }
    }
    if (!hooked) { log_printf("network: steam_api64 imports not found, monitoring disabled"); return 0; }
    static char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    char *s = strrchr(path, '\\');
    sprintf(s ? s + 1 : path, "%s_net.txt", g_prefix);
    HANDLE h = CreateThread(NULL, 0, report_thread, path, 0, NULL);
    if (h) CloseHandle(h);
    log_printf("network: monitoring installed (send buffer %d KB, rate max %d KB/s, rate min %d KB/s; 0 = Steam default)",
               g_cfg.net_send_buffer_kb, g_cfg.net_rate_max_kb, g_cfg.net_rate_min_kb);
    return 1;
}
