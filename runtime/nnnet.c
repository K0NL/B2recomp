/* Network-facing leftovers of an OFFLINE console, plus the run-time shader compiler entry points.
 *
 * LIFTED from the SDK (sdklift_gen.c), not here:
 *   nn::socket::InetAton / InetNtoa / InetNtop / InetPton (pure conversions),
 *   nn::bcat::DeliveryCacheProgress getters (GetStatus, GetResult, GetWholeTotal, GetWholeDownloaded),
 *   nn::friends::Friend / FriendPresence / Profile getters (plain field reads),
 *   nn::account::CachedNintendoAccountInfo getters, nn::ldn::MakeBroadcastAddress, nn::ngc::CountNumbers,
 *   nn::hid::SixAxisSensorState::GetQuaternion.
 * and, REPLACING do-nothing placeholders of nnsdk.c / nnmisc.c with the SDK's real constructors so that
 * the lifted getters read initialised objects: DeliveryCacheProgress C1 / D1 / Update ([sdk] 0xc4ab0: mutex
 * @0, service pointer @0x20 = null, 0x200 zero bytes @0x58, i.e. status 0 = "none"), DeliveryCacheFile C1,
 * DeliveryCacheDirectory C1, ec::ShopServiceAccessor::AsyncResponse C1 ([sdk] 0x18de00).
 *
 * Answered here:
 *  - nn::nifm leftovers, nn::socket / nnsocket*, nn::ssl / nnssl*, nn::nsd, nn::ldn, nn::ec::ShopServiceAccessor: see nnnet_c4a.inc.
 *  - nn::bcat::DeliveryCacheFile::Read: [sdk] 0xc47e0 a file that is not open gives 0xe7a (module 122,
 *    description 7); no file can be open because DeliveryCacheFile::Open fails (nnsdk.c).
 *  - nn::ec::ShopServiceAccessor::AsyncResponse: [sdk] 0x18e390 TryWait is true when no request is attached
 *    (pointer @0x18 null); [sdk] 0x18e1f8 Get then returns 0x190a4 (module 164, description 200). A
 *    request can never be attached: ShopServiceAccessor::Request fails first (nnnet_c4a.inc).
 *    nn::ec::ShopServiceAccessor::DefaultTimeout (a data import) gets its real 8 bytes from the SDK image.
 *  - glslc* (8 entry points of the run-time GLSL compiler): WEAK imports of the game that no module of this
 *    dump provides (sdk.elf has no such symbols), so on a console their GOT entries are null.
 *    [game] 0x710003a630 collects the eight pointers and 0x71000382f8 uses them only for a shader whose
 *    code type is 2 (GLSL source); every shader of the retail game is precompiled (code type 0), so the
 *    path is dead. If it were ever taken the handlers below stop with a clear message instead of
 *    returning garbage. A real implementation would have to compile GLSL to what the NVN backend takes
 *    (or hand the source straight to the OpenGL backend); nothing in the game needs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include "sdklift.h"

#pragma comment(lib, "ws2_32.lib")

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(v) ((uint64_t)(uintptr_t)(v))

/* ---------------------------------------------------------------- nn::bcat */
static void bcat_File_Read(Ctx *c) { if (A1) wr64(A1, 0); RET(0xe7au); }   /* (this, size_t *outSize, int64_t offset, void *buf, size_t size) */

/* ---------------------------------------------------------------- nn::ec */
#define EC_NO_RESPONSE 0x190a4u
static void ec_AsyncResponse_TryWait(Ctx *c) { RET(1); }
static void ec_AsyncResponse_Get(Ctx *c) { RET(EC_NO_RESPONSE); }                            /* (this, void *buf, size_t size) */

/* ---------------------------------------------------------------- glslc */
static void glslc_absent(Ctx *c) {
    printf("\nglslc: the game called an entry point of the run-time GLSL shader compiler. A retail console has no such library\n"
           "(the imports are weak and resolve to null there), so this path cannot run on a console either.\n");
    guest_trap("run-time shader compiler (glslc) called", c->x[30] - 4);
}

static const ImportDef net_imports[] = {
    {"_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm", bcat_File_Read},
    {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7TryWaitEv", ec_AsyncResponse_TryWait},
    {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse3GetEPvm", ec_AsyncResponse_Get},
    {"glslcCompile", glslc_absent}, {"glslcCompilePreSpecialized", glslc_absent}, {"glslcCompileSpecialized", glslc_absent},
    {"glslcFinalize", glslc_absent}, {"glslcGetDefaultOptions", glslc_absent}, {"glslcGetVersion", glslc_absent},
    {"glslcInitialize", glslc_absent}, {"glslcSetAllocator", glslc_absent},
};
/* the SDK's own constructors replace placeholders that left the objects uninitialised */
static const char *const lifted_replacements[] = {
    "_ZN2nn4bcat21DeliveryCacheProgressC1Ev", "_ZN2nn4bcat21DeliveryCacheProgressD1Ev", "_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv",
    "_ZN2nn4bcat17DeliveryCacheFileC1Ev", "_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev",
    "_ZN2nn2ec19ShopServiceAccessor13AsyncResponseC1Ev",
};

void register_nnnet_imports(void) {
    int n = (int)(sizeof net_imports / sizeof net_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(net_imports[i].name)) printf("nnnet: \"%s\" is not an import of the game (typo?)\n", net_imports[i].name);
    register_imports(net_imports, n);
    if (!sdk_image_ready()) return;
    for (int i = 0; i < (int)(sizeof lifted_replacements / sizeof lifted_replacements[0]); i++)
        if (!sdk_install(lifted_replacements[i])) printf("nnnet: %s was not lifted\n", lifted_replacements[i]);
    /* data import: nn::TimeSpan nn::ec::ShopServiceAccessor::DefaultTimeout ([sdk] 0xa39950, 8 bytes) */
    void *slot = import_data("_ZN2nn2ec19ShopServiceAccessor14DefaultTimeoutE");
    uint64_t src = sdk_symbol("_ZN2nn2ec19ShopServiceAccessor14DefaultTimeoutE");
    if (slot && src) memcpy(slot, (void *)(uintptr_t)src, 8);
}

/* ---------------------------------------------------------------- nn::friends: the client side as the SDK has it
 * Re-implemented from sdk.elf, instruction by instruction, with the friends system module (not in the dump) answering as
 * a console without friends and without a network (reference: suyu friend/friend.cpp + friend_interface.cpp: the
 * notification service exists and hands out an event; nothing ever signals it offline).
 *   detail::CheckUserStatus(uid) [sdk 0x1e0350]: an invalid Uid {0,0} is 0x479 (friends 121-0002); with the SDK's check flag
 *       set (.data 0xbfba40 = 1) the Uid must be one of the OPEN users (nn::account::ListOpenUsers), else 0x679 (121-0003).
 *   UserPresence::Initialize(uid) [sdk 0x1deff0]: CheckUserStatus; on success the Uid is stored at this+0.
 *   NotificationQueue::Initialize(uid) [sdk 0x1dec40]: CheckUserStatus; lock this (mutex at +0); create the notification
 *       service; its event handle is attached to the SystemEvent at this+0x38 (manual clear); Uid -> this+0x20;
 *       service -> this+0x30; unlock; success.
 *   NotificationQueue::Finalize() [sdk 0x1debf0]: lock; Uid at +0x20 cleared; if a service is held: release it,
 *       this+0x30 = 0, DestroySystemEvent(this+0x38); unlock.
 *   NotificationQueue::GetSystemEvent() [sdk 0x1dedf0]: lock; this+0x30 ? this+0x38 : NULL; unlock.
 * The service object is a token (the game never sees it; the SDK only keeps the pointer). */
void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
static uint8_t g_friends_service_token[16];
static void fr_lock(Ctx *c, uint64_t self, int lock) {
    ImportFn f = import_lookup(lock ? "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE" : "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");
    c->x[0] = self;
    if (f) f(c);
}
uint32_t fr_check_user(Ctx *c, uint64_t uid) {       /* also used by nnsdk.c (GetFriendList) */
    uint64_t lo = rd64(uid), hi = rd64(uid + 8);
    if (!lo && !hi) return 0x479;
    uint64_t flag = sdk_symbol("__nnDetailNintendoSdkRuntimeObjectFileRefer") ? SDK_BASE + 0xbfba40 : 0;      /* the SDK image's own flag byte */
    if (flag && rd8(flag)) {
        int account_user_is_open(uint64_t lo, uint64_t hi);       /* nnfs.c: the host's account model (ListOpenUsers is not an import of the game) */
        (void)c;
        return account_user_is_open(lo, hi) ? 0 : 0x679;
    }
    return 0;
}
static void friends_UserPresence_Initialize(Ctx *c) {     /* (this, const Uid &) -> Result */
    uint64_t self = A0, uid = A1;
    uint32_t r = fr_check_user(c, uid);
    if (!r) { wr64(self, rd64(uid)); wr64(self + 8, rd64(uid + 8)); }
    RET(r);
}
static void friends_NQ_Initialize(Ctx *c) {               /* (this, const Uid &) -> Result */
    uint64_t self = A0, uid = A1;
    uint32_t r = fr_check_user(c, uid);
    if (r) { RET(r); return; }
    fr_lock(c, self, 1);
    if (rd8(self + 0x60)) guest_trap("nn::friends::NotificationQueue::Initialize: [sdk 0x1ded68] abort path (flag at +0x60 set)", c->x[30] - 4);
    os_init_event(self + 0x38, 0);                         /* the service's event, readable handle attached with manual clear; never signalled offline */
    wr64(self + 0x20, rd64(uid)); wr64(self + 0x28, rd64(uid + 8));
    wr64(self + 0x30, P(g_friends_service_token));
    fr_lock(c, self, 0);
    RET(0);
}
static void friends_NQ_Finalize(Ctx *c) {
    uint64_t self = A0;
    fr_lock(c, self, 1);
    wr64(self + 0x20, 0); wr64(self + 0x28, 0);
    if (rd64(self + 0x30)) {
        ImportFn destroy = import_lookup("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        wr64(self + 0x30, 0);
        c->x[0] = self + 0x38;
        if (destroy) destroy(c);
    }
    fr_lock(c, self, 0);
    c->x[0] = self;
}
static void friends_NQ_GetSystemEvent(Ctx *c) {
    uint64_t self = A0, ev;
    fr_lock(c, self, 1);
    ev = rd64(self + 0x30) ? self + 0x38 : 0;
    fr_lock(c, self, 0);
    RET(ev);
}
static void c5a_register(void);                           /* nnsvc_c5a.inc: account, friends, bcat directory / file, ngc, pdm, pl VR */
static void c4a_register(void);                           /* nnnet_c4a.inc: nifm, sockets, ssl, nsd, ldn, ec accessor */
void nnnet_register_final(void) {                          /* after every other table (nnsdk2.c): replaces the do-nothing versions */
    static const ImportDef defs[] = {
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", friends_UserPresence_Initialize},
        {"_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", friends_NQ_Initialize},
        {"_ZN2nn7friends17NotificationQueue8FinalizeEv", friends_NQ_Finalize},
        {"_ZN2nn7friends17NotificationQueue14GetSystemEventEv", friends_NQ_GetSystemEvent},
    };
    register_imports(defs, (int)(sizeof defs / sizeof defs[0]));
    c4a_register();
    c5a_register();
}

/* ---------------------------------------------------------------- self-test */
static ImportFn h(const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("  no handler registered for %s\n", name); report_and_exit(3); }
    return f;
}
#include "nnnet_c4a.inc"
#include "nnsvc_c5a.inc"
int nnnet_selftest(Ctx *c) {
    int bad = 0, ok;
    bad += c4a_selftest(c);
    bad += c5a_selftest(c);
    /* nn::socket Inet* (lifted). [doc] nn::socket::Family::Af_Inet = 2. */
    uint8_t addr[16];
    char text[64];
    memset(addr, 0xee, sizeof addr);
    c->x[0] = 2; c->x[1] = P("192.168.1.20"); c->x[2] = P(addr); h("_ZN2nn6socket8InetPtonEiPKcPv")(c);
    ok = (int)c->x[0] == 1 && addr[0] == 192 && addr[1] == 168 && addr[2] == 1 && addr[3] == 20 && addr[4] == 0xee;
    c->x[0] = 2; c->x[1] = P("300.1.1.1"); c->x[2] = P(addr + 8); h("_ZN2nn6socket8InetPtonEiPKcPv")(c);
    ok = ok && (int)c->x[0] == 0;
    memset(text, 0x7e, sizeof text);
    c->x[0] = 2; c->x[1] = P(addr); c->x[2] = P(text); c->x[3] = 16; h("_ZN2nn6socket8InetNtopEiPKvPcj")(c);
    ok = ok && c->x[0] == P(text) && strcmp(text, "192.168.1.20") == 0 && text[16] == 0x7e;
    c->x[0] = 2; c->x[1] = P(addr); c->x[2] = P(text); c->x[3] = 4; h("_ZN2nn6socket8InetNtopEiPKvPcj")(c);
    ok = ok && c->x[0] == 0;                               /* buffer too small */
    printf("  socket: InetPton / InetNtop (lifted) 192.168.1.20 both ways, bad address and short buffer refused: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    uint32_t in = 0xeeeeeeee;
    c->x[0] = P("10.0.0.255"); c->x[1] = P(&in); h("_ZN2nn6socket8InetAtonEPKcPNS0_6InAddrE")(c);
    ok = (int)c->x[0] == 1 && in == 0xff00000au;           /* network byte order in memory: 0a 00 00 ff */
    c->x[0] = in; h("_ZN2nn6socket8InetNtoaE7in_addr")(c);
    ok = ok && c->x[0] && strcmp((const char *)(uintptr_t)c->x[0], "10.0.0.255") == 0;
    c->x[0] = P("not an address"); c->x[1] = P(&in); h("_ZN2nn6socket8InetAtonEPKcPNS0_6InAddrE")(c);
    ok = ok && (int)c->x[0] == 0;
    printf("  socket: InetAton / InetNtoa (lifted) 10.0.0.255 both ways, garbage refused: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    /* nn::bcat::DeliveryCacheProgress: the SDK's constructor, Update and getters on a fresh object */
    uint8_t *progress = malloc(0x400);
    memset(progress, 0xee, 0x400);
    c->x[0] = P(progress); h("_ZN2nn4bcat21DeliveryCacheProgressC1Ev")(c);
    c->x[0] = P(progress); h("_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv")(c);
    c->x[0] = P(progress); h("_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv")(c);
    ok = (int)c->x[0] == 0;
    c->x[0] = P(progress); h("_ZNK2nn4bcat21DeliveryCacheProgress13GetWholeTotalEv")(c);
    ok = ok && c->x[0] == 0;
    c->x[0] = P(progress); h("_ZNK2nn4bcat21DeliveryCacheProgress18GetWholeDownloadedEv")(c);
    ok = ok && c->x[0] == 0;
    c->x[0] = P(progress); h("_ZNK2nn4bcat21DeliveryCacheProgress9GetResultEv")(c);
    ok = ok && (uint32_t)c->x[0] == 0;
    for (int i = 0x258; i < 0x400; i++) if (progress[i] != 0xee) ok = 0;                   /* [sdk] 0x58 + 0x200 is the end of the object */
    c->x[0] = P(progress); h("_ZN2nn4bcat21DeliveryCacheProgressD1Ev")(c);
    printf("  bcat: DeliveryCacheProgress constructor / Update / getters / destructor (lifted): status 0, sizes 0, 0x258 bytes: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    free(progress);
    /* nn::ec */
    uint8_t response[0x100];
    uint64_t size = 123;
    memset(response, 0xee, sizeof response);
    c->x[0] = P(response); h("_ZN2nn2ec19ShopServiceAccessor13AsyncResponseC1Ev")(c);
    ok = rd64(P(response)) == P(response) && rd64(P(response) + 0x18) == 0 && response[0x48] == 0 && response[0x49] == 0xee;
    c->x[0] = P(response); ec_AsyncResponse_TryWait(c); ok = ok && c->x[0] == 1;
    c->x[0] = P(response); c->x[1] = P(&size); h("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7GetSizeEPm")(c); ok = ok && c->x[0] == EC_NO_RESPONSE && size == 123;
    void *slot = import_data("_ZN2nn2ec19ShopServiceAccessor14DefaultTimeoutE");
    int64_t timeout = 0;
    if (slot) memcpy(&timeout, slot, 8);
    printf("  ec: AsyncResponse constructor (lifted) + no-response answers; DefaultTimeout = %lld ns: %s\n", (long long)timeout, ok && timeout > 0 ? "ok" : "FAILED");
    bad += !(ok && timeout > 0);
    /* small lifted getters */
    static const uint16_t digits[] = {'a', '1', '2', 0xff13, 'b', 0};   /* U+FF13 = fullwidth digit three */
    c->x[0] = P(digits); h("_ZN2nn3ngc12CountNumbersEPKt")(c);
    int numbers = (int)c->x[0];
    c->x[0] = 0x0a000001u; c->x[1] = 0xffffff00u; h("_ZN2nn3ldn20MakeBroadcastAddressENS0_11Ipv4AddressENS0_10SubnetMaskE")(c);
    uint32_t broadcast = (uint32_t)c->x[0];
    ok = numbers >= 2 && broadcast == 0x0a0000ffu;
    printf("  ngc::CountNumbers(\"a12\\uff13b\") = %d; ldn::MakeBroadcastAddress(10.0.0.1, /24) = %08x: %s\n", numbers, broadcast, ok ? "ok" : "FAILED");
    bad += !ok;
    return bad;
}
