/*
 * Afterburner Bridge - MSI Afterburner sensors inside Corsair iCUE 5
 *
 * iCUE only offers a fixed list of sensors, and things like GPU power draw
 * or framerate are not in it. This plugin reads MSI Afterburner's shared
 * memory and republishes whatever you monitor there as an iCUE device, so
 * the values can be used on the dashboard, as alert thresholds and as
 * triggers for lighting effects.
 *
 * It talks to iCUE through the undocumented device-plugin interface, the
 * same one ASUS and NVIDIA use to bring their hardware in. Nothing in the
 * iCUE installation is modified.
 *
 * Build (MinGW-w64, cross or native):
 *   windres afterburner-bridge.rc -O coff -o ab_rc.o
 *   gcc -O1 -shared -o AfterburnerBridge.dll afterburner-bridge.c ab_rc.o \
 *       -static-libgcc
 *
 * Install: see install.ps1. The plugin must be Authenticode-signed or the
 * iCUE host refuses to load it; the script signs it with a certificate it
 * generates locally.
 *
 * ---------------------------------------------------------------------
 * FOUR RULES FOR ANYTHING HANDED TO THE HOST
 *
 * These are not style preferences. Breaking any of them crashes the plugin
 * container, and the crash usually surfaces far from the cause.
 *
 *   1. Never hand over static storage. The host frees everything it
 *      receives, so it must come from the heap.
 *   2. Allocate with ucrtbase's malloc, resolved at runtime - not the one
 *      this binary is linked against. A MinGW build uses msvcrt; freeing an
 *      msvcrt block from the host's ucrtbase heap corrupts it.
 *   3. Never pass NULL where the host expects a string. It calls strlen
 *      without checking.
 *   4. Over-allocate (see SLACK) and zero the block. If the host reads a
 *      field this code does not fill, it must find zeroes rather than
 *      allocator metadata that it would then treat as a pointer.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* Plugin identity. The INI file is <AB_BASE>.ini next to the DLL. */
#define AB_BASE          "AfterburnerBridge"
#define DEVICE_ID_PREFIX "AFTERBURNER-BRIDGE"
#define DEVICE_NAME      "Afterburner Bridge"
#define AB_VERSION       "1.0"

/* Artwork file name prefix; see stub_getDeviceInfo. The installer puts
 * <IMAGE_PREFIX>-thumbnail.png and <IMAGE_PREFIX>-device_view.png next to
 * the DLL. */
#define IMAGE_PREFIX     "AfterburnerBridge"

#define IFACE_SIZE 0x98   /* size of the interface struct the host expects */
#define SLACK      512    /* over-allocation on every block we hand over   */

/* ------------------------------------------------------------ logging */

/* Defined further down, but the log needs them first. */
static char g_dll_dir[MAX_PATH];
static char g_base[64];
static char g_ini[MAX_PATH];

static int cfg_int(const char *sect, const char *key, int def);

static CRITICAL_SECTION g_cs;
static LONG g_init_done = 0;
static char g_logpath[MAX_PATH];

static void log_init(void)
{
    if (InterlockedCompareExchange(&g_init_done, 1, 0) != 0) {
        while (g_init_done == 1) Sleep(1);
        return;
    }
    InitializeCriticalSection(&g_cs);

    /* The log always goes to the temporary folder. The plugin folder would
     * be the obvious place, but who starts the container decides whether it
     * is writable, so the file would move around depending on how iCUE was
     * launched, and looking for it in the wrong place wastes more time than
     * the tidiness is worth. */
    char c[2][MAX_PATH], t[MAX_PATH];
    const char *base = g_base[0] ? g_base : AB_BASE;

    if (!cfg_int("log", "enabled", 1)) { g_init_done = 2; return; }

    if (GetTempPathA(sizeof(t), t)) snprintf(c[0], MAX_PATH, "%s%s.log", t, base);
    else                            snprintf(c[0], MAX_PATH, "C:\\Windows\\Temp\\%s.log", base);
    snprintf(c[1], MAX_PATH, "C:\\ProgramData\\%s.log", base);

    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(c[i], "a");
        if (f) { fclose(f); strcpy(g_logpath, c[i]); break; }
    }
    g_init_done = 2;
}

static void plog(const char *fmt, ...)
{
    log_init();
    if (!g_logpath[0]) return;
    EnterCriticalSection(&g_cs);
    FILE *f = fopen(g_logpath, "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d t=%lu] ", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentThreadId());
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_cs);
}

/* Some entry points are called dozens of times per second, so each one gets
 * a line budget and then goes quiet. Without this the log grows by tens of
 * megabytes in minutes. */
#define HOT_BUDGET 400
static LONG g_hot_slot = 0, g_hot_prop = 0;

static void hlog(LONG *budget, const char *fmt, ...)
{
    LONG n = InterlockedIncrement(budget);
    if (n > HOT_BUDGET) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (n == HOT_BUDGET) plog("%s   [line budget reached, silencing this one]", buf);
    else                 plog("%s", buf);
}

/* ------------------------------------------------- memory sanity checks */

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return 0;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    uintptr_t e = (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize;
    return ((uintptr_t)p + n <= e);
}

/* getDevicePropertyInfo is called from several places in the host. Some
 * pass both out-parameters, some pass fewer arguments and leave stale
 * values in the register and stack slots - pointers into this DLL or into
 * the host's string pool have both been observed. Writing through those
 * would fault, so every out-parameter is checked first. */
static int writable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return 0;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY |
               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & rw)) return 0;
    uintptr_t e = (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize;
    return ((uintptr_t)p + n <= e);
}

/* ------------------------------------------------- the host's allocator */

/* Rule 2: the host frees our blocks with ucrtbase's free, so they have to
 * come from ucrtbase's malloc. */
static void *(*ucrt_malloc)(size_t) = NULL;

static void *halloc(size_t n)
{
    if (!ucrt_malloc) {
        HMODULE h = GetModuleHandleA("ucrtbase.dll");
        if (!h) h = LoadLibraryA("ucrtbase.dll");
        if (h) ucrt_malloc = (void *(*)(size_t))(void *)GetProcAddress(h, "malloc");
        plog("ucrtbase!malloc = %p", (void *)ucrt_malloc);
    }
    void *p = ucrt_malloc ? ucrt_malloc(n) : NULL;
    if (p) memset(p, 0, n);   /* rule 4 */
    return p;
}

static char *hstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)halloc(n + SLACK);
    if (p) memcpy(p, s, n);
    return p;
}


/* ----------------------------------------------------------- settings */

/* Windows already has an INI parser (GetPrivateProfileString), so settings
 * are read one key at a time with a default. A missing file means every
 * default applies, which is a valid configuration. */

static void cfg_init(HINSTANCE self)
{
    char full[MAX_PATH];
    snprintf(g_base, sizeof(g_base), "%s", AB_BASE);
    if (!GetModuleFileNameA(self, full, MAX_PATH)) { g_dll_dir[0] = 0; return; }
    snprintf(g_dll_dir, sizeof(g_dll_dir), "%s", full);
    char *slash = strrchr(g_dll_dir, '\\');
    if (slash) *slash = 0;
    snprintf(g_ini, sizeof(g_ini), "%s\\" AB_BASE ".ini", g_dll_dir);
}

static void cfg_str(const char *sect, const char *key,
                    const char *def, char *out, size_t n)
{
    if (!g_ini[0]) { snprintf(out, n, "%s", def); return; }
    GetPrivateProfileStringA(sect, key, def, out, (DWORD)n, g_ini);
    /* INI values keep surrounding whitespace; trim both ends. */
    char *p = out; while (*p == ' ' || *p == '\t') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
    size_t len = strlen(out);
    while (len && (out[len-1] == ' ' || out[len-1] == '\t')) out[--len] = 0;
}

static int cfg_int(const char *sect, const char *key, int def)
{
    if (!g_ini[0]) return def;
    return (int)GetPrivateProfileIntA(sect, key, def, g_ini);
}

static int cfg_bool(const char *sect, const char *key, int def)
{
    char v[16];
    cfg_str(sect, key, def ? "true" : "false", v, sizeof(v));
    if (!_stricmp(v, "true")  || !_stricmp(v, "yes") || !_stricmp(v, "on")  || !strcmp(v, "1")) return 1;
    if (!_stricmp(v, "false") || !_stricmp(v, "no")  || !_stricmp(v, "off") || !strcmp(v, "0")) return 0;
    return def;
}

/* The log goes next to the DLL. [log] enabled = 0 turns it off entirely.
 * log_init has two fallbacks for the case where that folder is not
 * writable by the host process. */
/* True when iCUE has a sensor type whose label matches this unit: degrees,
 * RPM, V, A, W. Everything else (%, MHz, MB, FPS...) has to borrow a type,
 * and iCUE then prints that type's label. */
static int unit_native(const char *u)
{
    if (!u || !u[0]) return 0;
    if (!_stricmp(u, "W") || !_stricmp(u, "V") || !_stricmp(u, "A") ||
        !_stricmp(u, "RPM") || !_stricmp(u, "C")) return 1;
    /* degree sign, Latin-1 (0xB0) or UTF-8 (0xC2 0xB0) */
    return ((unsigned char)u[0] == 0xB0 || (unsigned char)u[0] == 0xC2);
}

/* -------------------------------------------- host interface structures */

/* Zone entry, 32 bytes. The host reads +0x00 as an id and +0x10 as a kind;
 * the two pointer slots must never be NULL (rule 3). Only used by the
 * device view, which this plugin does not publish - kept because the host
 * may still ask for one. */
typedef struct {
    int32_t  id;
    int32_t  _04;
    char    *label;
    int32_t  kind;
    int32_t  _14;
    char    *label2;
} Zone;

/* Counted list. Where the host consumes one it requires count > 0 and a
 * non-NULL items pointer, and it reads both before checking either. */
typedef struct {
    int32_t  count;
    int32_t  _pad;
    void    *items;
} CountedList;

/* Boxed string: 16 bytes holding a char*, NOT a char* itself. Several
 * fields are like this. Passing a plain char* makes the host construct a
 * QString from the first eight characters read as an address, which faults
 * with the string's own bytes visible in the register dump. */
typedef struct {
    char *str;
    char *extra;
} StrBox;

typedef StrBox DeviceImage;

/* CorsairPluginDeviceInfo, 56 bytes. */
typedef struct {
    char        *name;       /* +0x00 shown in iCUE                        */
    int32_t      type;       /* +0x08 device category; see announce_thread */
    int32_t      _0c;
    CountedList *ledPos;     /* +0x10 LED positions                        */
    StrBox      *s18;        /* +0x18 boxed, purpose unidentified          */
    char        *viewKey;    /* +0x20 device id, echoed to getDeviceView    */
    int32_t      views;      /* +0x28 number of device views               */
    int32_t      _2c;
    StrBox      *s30;        /* +0x30 boxed, purpose unidentified          */
} DeviceInfo;

/* CorsairPluginDeviceView, 32 bytes. */
typedef struct {
    DeviceImage *image;
    DeviceImage *maskImage;
    CountedList *ledZones;
    CountedList *actionZones;
} DeviceView;

/* ------------------------------------------------------- host callback */

typedef void (*device_status_cb)(void *ctx, const char *devId,
                                 unsigned char connected);
static void            *g_ctx = NULL;
static device_status_cb g_cb  = NULL;
static LONG             g_started = 0;

static const char *device_id(void);

static DWORD WINAPI announce_thread(LPVOID u)
{
    (void)u;
    /* The host is not ready to receive the announcement the instant it
     * subscribes. */
    Sleep(3000);
    if (!g_cb) { plog("no callback registered, nothing to announce"); return 0; }

    const char *devid = device_id();
    plog("announcing \"%s\" as connected", devid);
    g_cb(g_ctx, devid, 1);
    return 0;
}

/* -------------------------------------------------------- constructors */

/* SHA-256 of a file, as lowercase hex. iCUE hashes the image itself and
 * throws the picture away unless the plugin hands over the same digest, so
 * this is not optional. bcrypt is resolved at runtime to keep the build a
 * single gcc call with no extra libraries. */
static int sha256_file_hex(const char *path, char *out, size_t n)
{
    typedef LONG (WINAPI *pOpen)(void **, const wchar_t *, const wchar_t *, ULONG);
    typedef LONG (WINAPI *pClose)(void *, ULONG);
    typedef LONG (WINAPI *pGetProp)(void *, const wchar_t *, unsigned char *, ULONG, ULONG *, ULONG);
    typedef LONG (WINAPI *pCreate)(void *, void **, unsigned char *, ULONG, unsigned char *, ULONG, ULONG);
    typedef LONG (WINAPI *pData)(void *, unsigned char *, ULONG, ULONG);
    typedef LONG (WINAPI *pFinish)(void *, unsigned char *, ULONG, ULONG);
    typedef LONG (WINAPI *pDestroy)(void *);

    if (n < 65) return 0;
    out[0] = 0;

    HMODULE b = LoadLibraryA("bcrypt.dll");
    if (!b) return 0;

    pOpen    Open    = (pOpen)   (void *)GetProcAddress(b, "BCryptOpenAlgorithmProvider");
    pClose   Close   = (pClose)  (void *)GetProcAddress(b, "BCryptCloseAlgorithmProvider");
    pGetProp GetProp = (pGetProp)(void *)GetProcAddress(b, "BCryptGetProperty");
    pCreate  Create  = (pCreate) (void *)GetProcAddress(b, "BCryptCreateHash");
    pData    Data    = (pData)   (void *)GetProcAddress(b, "BCryptHashData");
    pFinish  Finish  = (pFinish) (void *)GetProcAddress(b, "BCryptFinishHash");
    pDestroy Destroy = (pDestroy)(void *)GetProcAddress(b, "BCryptDestroyHash");
    if (!Open || !Close || !GetProp || !Create || !Data || !Finish || !Destroy) return 0;

    int ok = 0;
    void *alg = NULL, *h = NULL;
    unsigned char *obj = NULL;
    FILE *f = NULL;

    if (Open(&alg, L"SHA256", NULL, 0) != 0) goto done;

    ULONG objlen = 0, got = 0;
    if (GetProp(alg, L"ObjectLength", (unsigned char *)&objlen, sizeof(objlen), &got, 0) != 0) goto done;

    obj = (unsigned char *)malloc(objlen);
    if (!obj) goto done;
    if (Create(alg, &h, obj, objlen, NULL, 0, 0) != 0) goto done;

    f = fopen(path, "rb");
    if (!f) goto done;

    unsigned char buf[16384];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
        if (Data(h, buf, (ULONG)r, 0) != 0) goto done;

    unsigned char digest[32];
    if (Finish(h, digest, sizeof(digest), 0) != 0) goto done;

    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    ok = 1;

done:
    if (f)   fclose(f);
    if (h)   Destroy(h);
    if (obj) free(obj);
    if (alg) Close(alg, 0);
    FreeLibrary(b);
    return ok;
}

/* An image as the host wants it: a path relative to the plugin's own folder,
 * plus the SHA-256 of the file that path resolves to. An absolute path is
 * rejected outright ("invalid filePath"). */
static DeviceImage *make_image(const char *name)
{
    DeviceImage *im = (DeviceImage *)halloc(SLACK);
    if (!im) return NULL;

    char full[MAX_PATH], hex[80];
    snprintf(full, sizeof(full), "%s\\%s", g_dll_dir, name);
    if (!sha256_file_hex(full, hex, sizeof(hex))) {
        hex[0] = 0;
        plog("  image \"%s\": cannot hash %s", name, full);
    } else {
        plog("  image \"%s\" sha256 %s", name, hex);
    }

    im->str   = hstrdup(name);
    im->extra = hstrdup(hex);
    return im;
}

static CountedList *make_list(int n)
{
    CountedList *l = (CountedList *)halloc(SLACK);
    if (!l) return NULL;
    Zone *arr = (Zone *)halloc(SLACK);     /* contiguous, stride 32 */
    if (!arr) return NULL;
    for (int i = 0; i < n; i++) {
        arr[i].id     = i + 1;
        arr[i].kind   = 0;
        arr[i].label  = hstrdup("");
        arr[i].label2 = hstrdup("");
    }
    l->count = n;
    l->items = arr;      /* never NULL, even when n is 0 */
    return l;
}

/* -------------------------------------------- plugin interface methods */

/* Slots whose purpose is known but which this plugin does not implement.
 * Returning an empty string rather than 0 is deliberate: some of them are
 * expected to return a char* (rule 3). */
static uint64_t generic_stub(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    hlog(&g_hot_slot, "unimplemented slot called  rcx=%016llX rdx=%016llX r8=%016llX r9=%016llX",
         (unsigned long long)a1, (unsigned long long)a2,
         (unsigned long long)a3, (unsigned long long)a4);
    return (uint64_t)(uintptr_t)hstrdup("");
}

/* One stub per unimplemented slot, so the log names the slot instead of
 * saying "some slot". Each gets its own line budget: some of these are
 * called continuously. */
#define SLOT_STUB(off, name)                                                  \
static LONG g_hot_##name = 0;                                                 \
static uint64_t slot_##name(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)\
{                                                                             \
    hlog(&g_hot_##name, "slot " #off " (" #name ")  rcx=%016llX rdx=%016llX "  \
         "r8=%016llX r9=%016llX",                                             \
         (unsigned long long)a1, (unsigned long long)a2,                       \
         (unsigned long long)a3, (unsigned long long)a4);                      \
    return (uint64_t)(uintptr_t)hstrdup("");                                   \
}

SLOT_STUB(0x08, setLedsColors)
SLOT_STUB(0x18, unsubscribe)
SLOT_STUB(0x28, freeDeviceInfo)
SLOT_STUB(0x30, freeDeviceView)

static DeviceInfo *stub_getDeviceInfo(const char *deviceId)
{
    const char *id = (deviceId && readable(deviceId, 1)) ? deviceId : device_id();
    plog("getDeviceInfo(\"%s\")", id);

    DeviceInfo *info = (DeviceInfo *)halloc(SLACK);
    if (!info) { plog("  allocation failed"); return NULL; }

    char devname[128];
    cfg_str("device", "name", DEVICE_NAME, devname, sizeof(devname));
    info->name = hstrdup(devname);

    /* Device type 4 is mandatory, not cosmetic: of the values 0 to 25 it is
     * the only one for which iCUE grants the sensors page. It also makes
     * iCUE label the device "GPU", and the two cannot be separated. */
    info->type = 4;

    /* No LEDs and no views. This, rather than the lighting bit in
     * GetFeatures, is what keeps iCUE from attaching a lighting section to
     * a device that has nothing to light. With views at 0 the host never
     * asks for a device view at all. */
    info->ledPos  = make_list(0);

    /* No views. This, rather than the lighting bit in GetFeatures, is what
     * keeps iCUE from attaching a lighting section to a device that has
     * nothing to light. The device picture does not need one: it comes from
     * the two image fields below. */
    info->views   = 0;

    /* iCUE builds the device artwork file names from this field, as
     * <viewKey>-thumbnail.png and <viewKey>-device_view.png inside the
     * plugin folder. It has to stay constant, so it cannot be the device
     * id: that one carries a fingerprint of the sensor list and changes
     * whenever the list does. The host notes the mismatch in its own log
     * and carries on. */
    info->viewKey = hstrdup(IMAGE_PREFIX);

    /* +0x18 is the thumbnail, the small picture in iCUE's device list, and
     * +0x30 is what its log calls the promo image, the large one on the
     * device's own page. They want different framing, so they are separate
     * files. Both are paths relative to the plugin's own folder, each
     * carrying the SHA-256 of the file; see make_image. */
    info->s18 = make_image(IMAGE_PREFIX "-thumbnail.png");
    info->s30 = make_image(IMAGE_PREFIX "-promo.png");

    if (!info->ledPos || !info->s18 || !info->s30) {
        plog("  allocation failed"); return NULL;
    }
    return info;
}

/* Not reached while DeviceInfo.views is 0, but the host is entitled to ask.
 *
 * The layout of this structure was never pinned down. Of the shapes tried,
 * only one left iCUE standing: two boxed images followed by two empty zone
 * lists. The others either took the process down at startup or made it drop
 * the device altogether. Since the device picture turned out to come from
 * getDeviceInfo instead, a view buys nothing here and is not published. */
static DeviceView *stub_getDeviceView(const char *key, int viewIndex)
{
    plog("getDeviceView(\"%s\", %d)",
         (key && readable(key, 1)) ? key : "<unreadable>", viewIndex);
    if (viewIndex != 0) return NULL;

    DeviceView *v = (DeviceView *)halloc(SLACK);
    if (!v) return NULL;

    v->image       = make_image(IMAGE_PREFIX "-promo.png");
    v->maskImage   = make_image(IMAGE_PREFIX "-promo.png");
    v->ledZones    = make_list(0);
    v->actionZones = make_list(0);

    if (!v->image || !v->maskImage || !v->ledZones || !v->actionZones) {
        plog("  allocation failed"); return NULL;
    }
    return v;
}

static uint64_t stub_subscribe(void *ctx, device_status_cb cb)
{
    plog("subscribeForDeviceConnectionStatusChanges(ctx=%p, cb=%p)", ctx, (void *)cb);
    g_ctx = ctx; g_cb = cb;
    if (InterlockedCompareExchange(&g_started, 1, 0) == 0) {
        HANDLE h = CreateThread(NULL, 0, announce_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return 0;
}

static uint64_t stub_setMode(int mode)
{
    plog("setMode(%d)", mode);
    return 0;
}

/* --------------------------------------------------- device properties
 *
 * Sensors are published as indexed device properties:
 *
 *   0x70  bool  getDevicePropertyInfo(id, prop, idx, int* type, int* flags)
 *   0x78  void* readDevicePropertyData(id, prop, idx, void*)
 *   0x80  bool  writeDevicePropertyData(...)
 *   0x88  void  freePropertyData(int prop, void* data)
 *
 * The host reads SensorsCount, then walks indices 0..count-1 asking for the
 * type, name, value and range of each one. Everything read through 0x78 is
 * a heap block the host frees through 0x88.
 */

typedef struct { int32_t *ids; int32_t count; } PropList;

/* cue::dev::plugin::DevicePropertyId - only the ones used here. */
enum {
    P_PROPERTIES_LIST = 1,
    P_SENSORS_COUNT   = 4,
    P_SENSOR_TYPE     = 5,
    P_SENSOR_NAME     = 6,
    P_SENSOR_VALUE    = 7,
    P_SENSOR_MIN      = 11,
    P_SENSOR_MAX      = 12
};

static const int32_t k_props[] = {
    P_PROPERTIES_LIST, P_SENSORS_COUNT, P_SENSOR_TYPE,
    P_SENSOR_NAME, P_SENSOR_VALUE, P_SENSOR_MIN, P_SENSOR_MAX
};
#define N_PROPS ((int)(sizeof(k_props)/sizeof(k_props[0])))

/* The data type reported through the outType parameter tells the host how
 * to read the block returned for that property. It also decides how the
 * number is displayed: a value declared int has no decimals to print.
 *
 *   0  bool      read as a byte
 *   1  int       *(int32_t*)block
 *   2  double    *(double*)block
 *   3  QString   boxed: the host reads *(char**)block
 *   4  LedColor  { int32 ledId; uint32 argb }
 *
 * Anything above 4 makes the host discard the property.
 */
#define DT_BOOL   0
#define DT_INT    1
#define DT_DOUBLE 2
#define DT_STRING 3

/* cue::dev::plugin::PropertyFlag */
#define PF_READ    1
#define PF_INDEXED 4

/* cue::dev::plugin::SensorType - the complete set. These six are the only
 * units iCUE can label. */
#define ST_TEMPERATURE 1
#define ST_FAN_RPM     2
#define ST_PUMP_RPM    3
#define ST_VOLTAGE     4
#define ST_CURRENT     5
#define ST_POWER       6

/* ------------------------------------------------------------ sensors
 *
 * The list is not hardcoded: it is built at startup from whatever MSI
 * Afterburner is monitoring. Ticking a new entry in Afterburner's
 * Monitoring tab and restarting iCUE is all it takes to publish it.
 */

#define MAX_SENSORS 48

typedef struct {
    char   name[64];    /* shown in iCUE, may carry the real unit      */
    char   src[64];     /* Afterburner's own name, used to find it again */
    char   units[16];
    int    type;
    double vmin, vmax;
    int    slot;        /* index in Afterburner's shared memory, -1 = NVML */
    int    is_int;      /* publish without decimals                     */
} Sensor;

static Sensor g_sen[MAX_SENSORS];
static int    g_nsen = -1;      /* -1 until the list has been built */

static void sensors_ensure(void);

static int sensor_count(void)
{
    sensors_ensure();
    return g_nsen > 0 ? g_nsen : 1;
}

static const Sensor *sdef(int idx)
{
    sensors_ensure();
    if (idx < 0 || idx >= g_nsen) return &g_sen[0];
    return &g_sen[idx];
}

static const char *sensor_name(int idx)  { return sdef(idx)->name; }
static int         sensor_type(int idx)  { return sdef(idx)->type; }
static double      sensor_min(int idx)   { return sdef(idx)->vmin; }
static double      sensor_max(int idx)   { return sdef(idx)->vmax; }
static int         sensor_isint(int idx) { return sdef(idx)->is_int; }

/* ----------------------------------------------------- NVML fallback
 *
 * When Afterburner is not running there is nothing to publish, which would
 * leave iCUE with an empty device. NVIDIA's management library - the one
 * behind nvidia-smi, installed with the driver - at least provides GPU
 * power draw. Resolved at runtime, so a machine without it still works.
 */

typedef int (*fn_nvmlInit)(void);
typedef int (*fn_nvmlGetHandle)(unsigned int, void **);
typedef int (*fn_nvmlGetPower)(void *, unsigned int *);

static fn_nvmlGetPower nvml_getPower = NULL;
static void           *nvml_dev      = NULL;
static LONG            nvml_state    = 0;   /* 0 untried, 1 busy, 2 ready, 3 failed */

static void nvml_setup(void)
{
    if (InterlockedCompareExchange(&nvml_state, 1, 0) != 0) {
        while (nvml_state == 1) Sleep(1);
        return;
    }

    HMODULE h = LoadLibraryA("nvml.dll");
    if (!h) h = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!h) {
        plog("NVML: nvml.dll not found (error %lu)", (unsigned long)GetLastError());
        nvml_state = 3; return;
    }

    fn_nvmlInit      init = (fn_nvmlInit)     (void *)GetProcAddress(h, "nvmlInit_v2");
    fn_nvmlGetHandle geth = (fn_nvmlGetHandle)(void *)GetProcAddress(h, "nvmlDeviceGetHandleByIndex_v2");
    nvml_getPower         = (fn_nvmlGetPower) (void *)GetProcAddress(h, "nvmlDeviceGetPowerUsage");
    if (!init) init = (fn_nvmlInit)(void *)GetProcAddress(h, "nvmlInit");
    if (!geth) geth = (fn_nvmlGetHandle)(void *)GetProcAddress(h, "nvmlDeviceGetHandleByIndex");

    if (!init || !geth || !nvml_getPower) {
        plog("NVML: missing exports (init=%p handle=%p power=%p)",
             (void *)init, (void *)geth, (void *)nvml_getPower);
        nvml_state = 3; return;
    }

    int rc = init();
    if (rc != 0) { plog("NVML: nvmlInit failed (%d)", rc); nvml_state = 3; return; }

    rc = geth(0, &nvml_dev);
    if (rc != 0 || !nvml_dev) {
        plog("NVML: no GPU at index 0 (%d)", rc); nvml_state = 3; return;
    }

    plog("NVML: ready");
    nvml_state = 2;
}

static double nvml_watts(void)
{
    static double    cached = 0.0;
    static ULONGLONG when   = 0;

    nvml_setup();
    if (nvml_state != 2) return 0.0;

    ULONGLONG now = GetTickCount64();
    if (when && now - when < 250) return cached;

    unsigned int mw = 0;
    int rc = nvml_getPower(nvml_dev, &mw);
    if (rc != 0) { plog("NVML: power read failed (%d)", rc); return cached; }

    cached = (double)mw / 1000.0;
    when   = now;
    return cached;
}

/* ------------------------------------------- MSI Afterburner shared memory
 *
 * Afterburner publishes everything it monitors in a shared memory section
 * named MAHMSharedMemory - the same numbers its graphs and the RivaTuner
 * overlay show, framerate included.
 *
 * Header:  dwSignature 'MAHM' | dwVersion | dwHeaderSize | dwNumEntries |
 *          dwEntrySize | time | ...
 * Entry:   five fixed-length strings (szSrcName, szSrcUnits,
 *          szLocalizedSrcName, szLocalizedSrcUnits, szRecommendedFormat)
 *          then float data, float minLimit, float maxLimit,
 *          DWORD dwFlags, DWORD dwGpu, DWORD dwSrcId.
 *
 * Nothing here is a hardcoded offset: the header gives the entry count and
 * size, and the string field length follows from (dwEntrySize - 24) / 5.
 * The three fields used are all ahead of `time`, whose width is ambiguous.
 */

#define MAHM_SIG 0x4D41484DU   /* 'MAHM' as a multi-character constant */

static HANDLE               mahm_map   = NULL;
static const unsigned char *mahm_base  = NULL;
static LONG                 mahm_state = 0;   /* 0 untried, 1 busy, 2 ready, 3 failed */

static void mahm_open(void)
{
    if (InterlockedCompareExchange(&mahm_state, 1, 0) != 0) {
        while (mahm_state == 1) Sleep(1);
        return;
    }
    /* The plugin container normally runs in the user's session, where the
     * plain name resolves. The prefixed names cover the case where it does
     * not. */
    static const char *names[] = {
        "MAHMSharedMemory", "Global\\MAHMSharedMemory", "Local\\MAHMSharedMemory"
    };
    DWORD sess = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sess);
    plog("MAHM: plugin container is in session %lu", (unsigned long)sess);

    for (int i = 0; i < 3; i++) {
        HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, names[i]);
        if (!h) {
            plog("MAHM: cannot open \"%s\" (error %lu)",
                 names[i], (unsigned long)GetLastError());
            continue;
        }
        const unsigned char *b =
            (const unsigned char *)MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
        if (!b) {
            plog("MAHM: cannot map \"%s\" (error %lu)",
                 names[i], (unsigned long)GetLastError());
            CloseHandle(h); continue;
        }
        mahm_map = h; mahm_base = b;
        plog("MAHM: opened \"%s\", signature 0x%08X", names[i], *(const uint32_t *)b);
        mahm_state = 2;
        return;
    }
    plog("MAHM: shared memory not reachable - is Afterburner running?");
    mahm_state = 3;
}

typedef struct {
    const unsigned char *base;
    uint32_t hsz, n, esz, slen;
} MahmView;

static int mahm_view(MahmView *v)
{
    mahm_open();
    if (mahm_state != 2 || !mahm_base) return 0;
    const unsigned char *p = mahm_base;
    if (*(const uint32_t *)p != MAHM_SIG) return 0;   /* Afterburner closed */
    v->base = p;
    v->hsz  = *(const uint32_t *)(p + 0x08);
    v->n    = *(const uint32_t *)(p + 0x0C);
    v->esz  = *(const uint32_t *)(p + 0x10);
    if (v->esz < 40 || v->n == 0 || v->n > 4096) return 0;
    v->slen = (v->esz - 24) / 5;
    return 1;
}

static const unsigned char *mahm_entry(const MahmView *v, uint32_t i)
{
    return v->base + v->hsz + (size_t)i * v->esz;
}

/* RivaTuner writes FLT_MAX for framerate when it is not measuring any
 * application; a few other entries do the same when idle. Treat anything
 * that far out as zero. */
static double mahm_sane(float f)
{
    double v = (double)f;
    if (!(v > -1.0e30 && v < 1.0e30)) return 0.0;
    return v;
}

/* From the unit the user wants iCUE to display, to the sensor type that
 * produces it. Returns 0 when the string is not recognised. */
static int type_from_icue_unit(const char *v)
{
    if (!v || !v[0]) return 0;
    if (!_stricmp(v, "C") || !_stricmp(v, "temperature")) return ST_TEMPERATURE;
    if ((unsigned char)v[0] == 0xB0 || (unsigned char)v[0] == 0xC2) return ST_TEMPERATURE;
    if (!_stricmp(v, "RPM")     || !_stricmp(v, "fan"))     return ST_FAN_RPM;
    if (!_stricmp(v, "pump"))                               return ST_PUMP_RPM;
    if (!_stricmp(v, "V")       || !_stricmp(v, "voltage")) return ST_VOLTAGE;
    if (!_stricmp(v, "A")       || !_stricmp(v, "current")) return ST_CURRENT;
    if (!_stricmp(v, "W")       || !_stricmp(v, "power"))   return ST_POWER;
    return 0;
}

/* From an Afterburner unit to the iCUE sensor type to publish it as. */
static int type_from_units(const char *u)
{
    /* [units] in the INI remaps a unit iCUE has no label for. It does not
     * apply to units iCUE already gets right. */
    if (u && u[0] && !unit_native(u)) {
        char want[32];
        cfg_str("units", u, "", want, sizeof(want));
        if (want[0]) {
            int ty = type_from_icue_unit(want);
            if (ty) { plog("config: [units] %s = \"%s\"", u, want); return ty; }
            plog("config: [units] %s = \"%s\" not recognised, ignoring", u, want);
        }
    }

    if (!u || !u[0])             return ST_TEMPERATURE;
    if (!_stricmp(u, "W"))       return ST_POWER;
    if (!_stricmp(u, "V"))       return ST_VOLTAGE;
    if (!_stricmp(u, "A"))       return ST_CURRENT;
    if ((unsigned char)u[0] == 0xB0 || (unsigned char)u[0] == 0xC2) return ST_TEMPERATURE;
    if (!_stricmp(u, "C"))       return ST_TEMPERATURE;

    /* RPM as a fan speed, which is the correct label. It also makes iCUE
     * attach its Cooling and Alerts pages to the device: the two come from
     * this one type and cannot be separated. */
    if (!_stricmp(u, "RPM"))     return ST_FAN_RPM;

    return ST_TEMPERATURE;
}

/* iCUE will not display a value that falls outside the declared range, so
 * when Afterburner does not give a usable maximum, pick a generous one from
 * the unit. */
static double fallback_max(const char *u, double val, double lo)
{
    double m = 100.0;
    if (u && u[0]) {
        if      (!_stricmp(u, "%"))   m = 100.0;
        else if (!_stricmp(u, "V"))   m = 3.0;
        else if (!_stricmp(u, "A"))   m = 200.0;
        else if (!_stricmp(u, "W"))   m = 1000.0;
        else if (!_stricmp(u, "RPM")) m = 6000.0;
        else if (!_stricmp(u, "MHz")) m = 10000.0;
        else if (!_stricmp(u, "MB"))  m = 131072.0;
        else if (!_stricmp(u, "GB"))  m = 256.0;
        else if (!_stricmp(u, "FPS")) m = 1000.0;
        else if ((unsigned char)u[0] == 0xB0 ||
                 (unsigned char)u[0] == 0xC2) m = 120.0;
    }
    while (m <= val) m *= 4.0;
    if (m <= lo) m = lo + 100.0;
    return m;
}

/* Builds the sensor list once. It stays frozen for the lifetime of the
 * process: the host reads SensorsCount at startup and does not cope with
 * the number changing underneath it. New entries appear after an iCUE
 * restart. */
static void sensors_ensure(void)
{
    static LONG state = 0;
    if (InterlockedCompareExchange(&state, 1, 0) != 0) {
        while (state == 1) Sleep(1);
        return;
    }

    int show_nc      = cfg_bool("sensors", "show_not_compatible", 1);
    int unit_in_name = cfg_int("display", "unit_in_name", 1);
    plog("config: %s  show_not_compatible=%d  unit_in_name=%d",
         g_ini[0] ? g_ini : "(defaults)", show_nc, unit_in_name);

    MahmView v;
    int k = 0;
    if (mahm_view(&v)) {
        plog("Afterburner exposes %u entries", v.n);
        for (uint32_t i = 0; i < v.n && k < MAX_SENSORS; i++) {
            const unsigned char *e = mahm_entry(&v, i);
            const char *nm = (const char *)e;
            const char *un = (const char *)(e + v.slen);
            if (!nm[0]) continue;

            if (!show_nc && !unit_native(un)) {
                plog("  skipping \"%s\" [%s] - unit not supported by iCUE", nm, un);
                continue;
            }

            const float *f = (const float *)(e + 5 * (size_t)v.slen);
            double val = mahm_sane(f[0]);

            /* Entries with no natural ceiling - RAM, clocks, framerate -
             * carry FLT_MAX as their limit. */
            int lo_ok = (f[1] > -1.0e30f && f[1] < 1.0e30f);
            int hi_ok = (f[2] > -1.0e30f && f[2] < 1.0e30f);
            double lo = lo_ok ? (double)f[1] : 0.0;
            double hi = hi_ok ? (double)f[2] : 0.0;

            /* szRecommendedFormat, e.g. "%.3f": Afterburner already knows
             * how many decimals each entry deserves. */
            const char *fmt = (const char *)(e + 4 * (size_t)v.slen);
            int decimals = 1;
            const char *dot = strchr(fmt, '.');
            if (dot && dot[1] >= '0' && dot[1] <= '9') decimals = dot[1] - '0';

            Sensor *s = &g_sen[k];
            snprintf(s->units, sizeof(s->units), "%s", un);
            snprintf(s->src,   sizeof(s->src),   "%s", nm);
            s->type = type_from_units(un);
            s->slot = (int)i;

            /* When the type had to be borrowed, iCUE prints that type's
             * label. The name is free text, so the real unit goes there. */
            int unit_mismatch = unit_in_name && un[0] &&
                                s->type == ST_TEMPERATURE &&
                                (unsigned char)un[0] != 0xB0 &&
                                (unsigned char)un[0] != 0xC2 &&
                                _stricmp(un, "C") != 0;
            if (unit_mismatch) snprintf(s->name, sizeof(s->name), "%s (%s)", nm, un);
            else               snprintf(s->name, sizeof(s->name), "%s", nm);

            s->vmin = lo;
            s->vmax = (hi > lo) ? hi : fallback_max(un, val, lo);
            /* Afterburner's declared maximum can still be below the live
             * value - a CPU boosting past its rated clock, for instance. */
            if (val > s->vmax) s->vmax = val * 1.25;

            /* Framerate and fan speed are whole numbers whatever the
             * recommended format says. */
            s->is_int = (decimals == 0) ||
                        !_stricmp(un, "FPS") || !_stricmp(un, "RPM");

            plog("  %2d: \"%s\" [%s] type=%d range %.1f..%.1f %s (now %.3f)",
                 k, s->name, s->units, s->type, s->vmin, s->vmax,
                 s->is_int ? "integer" : "decimal", val);
            k++;
        }
    }

    if (k == 0) {
        Sensor *s = &g_sen[0];
        snprintf(s->name,  sizeof(s->name),  "GPU Power");
        snprintf(s->src,   sizeof(s->src),   "GPU Power");
        snprintf(s->units, sizeof(s->units), "W");
        s->type = ST_POWER; s->slot = -1; s->vmin = 0.0; s->vmax = 600.0;
        k = 1;
        plog("Afterburner unavailable, publishing GPU Power from NVML only");
    }

    g_nsen = k;
    state  = 2;
}

/* ---------------------------------------------------------- device id
 *
 * iCUE caches a device's sensor list and then reads the values back by
 * position. If the list changes - a different Monitoring selection, a
 * different setting here - the cached names stay paired with values that
 * now belong to other sensors, and the dashboard shows one sensor's number
 * under another's name.
 *
 * So the device id carries a fingerprint of the list. A different list
 * means a different id, which iCUE treats as a new device with nothing
 * cached. An unchanged list keeps the same id and the same device.
 */
static char g_devid[80];

static const char *device_id(void)
{
    if (g_devid[0]) return g_devid;
    sensors_ensure();

    uint64_t h = 1469598103934665603ULL;          /* FNV-1a, 64 bit */
    for (int i = 0; i < g_nsen; i++) {
        for (const char *p = g_sen[i].src; *p; p++) {
            h ^= (unsigned char)*p;
            h *= 1099511628211ULL;
        }
        /* the type too, so remapping a unit also yields a new device */
        h ^= (uint64_t)(unsigned)g_sen[i].type; h *= 1099511628211ULL;
        h ^= '|';                                h *= 1099511628211ULL;
    }
    snprintf(g_devid, sizeof(g_devid), "%s-%08X",
             DEVICE_ID_PREFIX, (unsigned)(h ^ (h >> 32)));
    plog("device id \"%s\" (fingerprint of %d sensors)", g_devid, g_nsen);
    return g_devid;
}

/* Values are cached briefly: the host polls often, and there is no point
 * re-reading shared memory for every call. */
static double sensor_value(int idx)
{
    static double    cache[MAX_SENSORS];
    static ULONGLONG when[MAX_SENSORS];

    sensors_ensure();
    if (idx < 0 || idx >= g_nsen) return 0.0;

    ULONGLONG now = GetTickCount64();
    if (when[idx] && now - when[idx] < 200) return cache[idx];

    const Sensor *d = &g_sen[idx];
    double v = 0.0;

    if (d->slot < 0) {
        v = nvml_watts();
    } else {
        MahmView mv;
        if (mahm_view(&mv) && (uint32_t)d->slot < mv.n) {
            const unsigned char *e = mahm_entry(&mv, (uint32_t)d->slot);
            /* Afterburner may have reordered its entries since startup, so
             * fall back to looking the name up. */
            if (strcmp((const char *)e, d->src) != 0) {
                e = NULL;
                for (uint32_t i = 0; i < mv.n; i++) {
                    const unsigned char *c = mahm_entry(&mv, i);
                    if (strcmp((const char *)c, d->src) == 0) { e = c; break; }
                }
                if (!e) plog("\"%s\" is no longer exposed by Afterburner", d->src);
            }
            if (e) v = mahm_sane(*(const float *)(e + 5 * (size_t)mv.slen));
        }
    }

    cache[idx] = v;
    when[idx]  = now;
    return v;
}

static int datatype_for(int prop, int idx)
{
    switch (prop) {
        case P_PROPERTIES_LIST: return DT_BOOL;   /* the host special-cases this */
        case P_SENSORS_COUNT:   return DT_INT;
        case P_SENSOR_TYPE:     return DT_INT;
        case P_SENSOR_NAME:     return DT_STRING;
        case P_SENSOR_VALUE:
        case P_SENSOR_MIN:
        case P_SENSOR_MAX:      return sensor_isint(idx) ? DT_INT : DT_DOUBLE;
        default:                return DT_BOOL;
    }
}

static int indexed_prop(int prop)
{
    return (prop == P_SENSOR_TYPE || prop == P_SENSOR_NAME ||
            prop == P_SENSOR_VALUE || prop == P_SENSOR_MIN ||
            prop == P_SENSOR_MAX);
}

static int supported(int prop)
{
    for (int i = 0; i < N_PROPS; i++) if (k_props[i] == prop) return 1;
    return 0;
}

/* Safety net. The host enumerates sensors by asking for higher and higher
 * indices, so a wrong count would have it spin. Reporting nothing beyond
 * the declared sensor count stops that, and the overall call budget is a
 * second line of defence. */
#define CALL_BUDGET 20000
static volatile LONG g_calls = 0;

static PropList *g_proplist = NULL;

/* 0x70 */
static unsigned char stub_propInfo(const char *devId, int prop, int idx,
                                   void *outType, void *outFlags)
{
    const char *who = (devId && readable(devId, 1)) ? devId : "?";

    LONG n = InterlockedIncrement(&g_calls);
    if (n == CALL_BUDGET)
        plog("call budget reached; reporting every property as unsupported from now on");
    if (n >= CALL_BUDGET) return 0;

    if (!supported(prop) ||
        (indexed_prop(prop) && (idx < 0 || idx >= sensor_count()))) {
        hlog(&g_hot_prop, "propertyInfo(%s prop=%d idx=%d) -> false", who, prop, idx);
        return 0;
    }

    int dt = datatype_for(prop, idx);
    int fl = PF_READ | (indexed_prop(prop) ? PF_INDEXED : 0);
    if (writable(outType, 4))  *(int32_t *)outType  = dt;
    if (writable(outFlags, 4)) *(int32_t *)outFlags = fl;
    hlog(&g_hot_prop, "propertyInfo(%s prop=%d idx=%d) -> true type=%d flags=%d",
         who, prop, idx, dt, fl);
    return 1;
}

/* 0x78 */
static void *stub_readProp(const char *devId, int prop, int idx, void *p4)
{
    (void)devId; (void)p4;

    if (prop == P_PROPERTIES_LIST) {
        if (!g_proplist) {
            PropList *pl = (PropList *)halloc(SLACK);
            int32_t  *a  = (int32_t *)halloc(SLACK);
            if (!pl || !a) return NULL;
            for (int i = 0; i < N_PROPS; i++) a[i] = k_props[i];
            pl->ids = a; pl->count = N_PROPS;
            g_proplist = pl;
                }
        hlog(&g_hot_prop, "read propertiesList -> %d properties", N_PROPS);
        return g_proplist;
    }

    void *blk = halloc(SLACK);
    if (!blk) return NULL;

    switch (prop) {
        case P_SENSORS_COUNT:
            *(int32_t *)blk = sensor_count();
            hlog(&g_hot_prop, "read sensorsCount -> %d", sensor_count());
            break;

        case P_SENSOR_TYPE:
            *(int32_t *)blk = sensor_type(idx);
            hlog(&g_hot_prop, "read sensorType[%d] -> %d", idx, sensor_type(idx));
            break;

        case P_SENSOR_NAME:
            *(char **)blk = hstrdup(sensor_name(idx));   /* boxed */
            hlog(&g_hot_prop, "read sensorName[%d] -> \"%s\"", idx, sensor_name(idx));
            break;

        case P_SENSOR_VALUE:
        case P_SENSOR_MIN:
        case P_SENSOR_MAX: {
            double d = (prop == P_SENSOR_VALUE) ? sensor_value(idx)
                     : (prop == P_SENSOR_MIN)   ? sensor_min(idx)
                                                : sensor_max(idx);
            if (sensor_isint(idx)) {
                double r = (d < 0.0) ? d - 0.5 : d + 0.5;
                *(int32_t *)blk = (int32_t)r;
                hlog(&g_hot_prop, "read prop=%d idx=%d -> %d", prop, idx, *(int32_t *)blk);
            } else {
                *(double *)blk = d;
                hlog(&g_hot_prop, "read prop=%d idx=%d -> %.2f", prop, idx, d);
            }
            break;
        }

        default:
            *(char **)blk = hstrdup("");
            hlog(&g_hot_prop, "read prop=%d idx=%d -> empty", prop, idx);
            break;
    }
    return blk;
}

/* 0x80 - every sensor here is read-only. */
static unsigned char stub_writeProp(const char *devId, int prop, int idx, void *val)
{
    (void)devId; (void)val;
    plog("write prop=%d idx=%d refused: sensors are read-only", prop, idx);
    return 0;
}

/* 0x88 - the host owns the blocks and frees them itself. */
static void stub_freeProp(int prop, void *data)
{
    hlog(&g_hot_prop, "freePropertyData(prop=%d, %p)", prop, data);
}

/* ------------------------------------------------------------- exports */

__declspec(dllexport) int CorsairPluginGetAPIVersion(void)
{
    return 106;
}

__declspec(dllexport) int CorsairPluginGetFeatures(void)
{
    /* 0x04 DetachedMode      mandatory: without it the host validates the
     *                        plugin and never runs it
     * 0x10 DeviceProperties  the sensors
     *
     * 0x40 DeviceQuickLightingZones is deliberately absent. It does not in
     * fact control the lighting section - that follows from the device
     * declaring LEDs - and this plugin has none. */
    plog("GetFeatures -> 0x14 (DetachedMode | DeviceProperties)");
    return 0x14;
}

/* ------------------------------------------------------- crash reporting

 * Declaring a device view crashes the host, and the host keeps no log we can
 * read, so the plugin records the fault itself. This handler runs before any
 * other, on the faulting thread, and only writes a line: it does not try to
 * swallow the exception, because a fault here means the host was reading a
 * structure we got wrong and continuing would be worse.
 *
 * What matters in the line is the module the instruction belongs to and its
 * offset inside it: a fault inside Qt, reading a small address, says the host
 * followed one of our pointers to a field we left zero. */
static LONG CALLBACK fault_logger(EXCEPTION_POINTERS *ep);
static LONG CALLBACK fault_logger(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;

    /* First chance only, and only for the ones that mean a broken structure.
     * Anything else (C++ exceptions above all, which Qt throws routinely)
     * would fill the log with noise. */
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        r->ExceptionCode != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
        r->ExceptionCode != EXCEPTION_DATATYPE_MISALIGNMENT &&
        r->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    static LONG once = 0;
    if (InterlockedIncrement(&once) > 4) return EXCEPTION_CONTINUE_SEARCH;

    void *pc = r->ExceptionAddress;
    char mod[MAX_PATH] = "?";
    uintptr_t off = 0;
    HMODULE h = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &h) && h) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(h, full, sizeof(full))) {
            const char *b = strrchr(full, '\\');
            snprintf(mod, sizeof(mod), "%s", b ? b + 1 : full);
        }
        off = (uintptr_t)pc - (uintptr_t)h;
    }

    const char *what = "read";
    uintptr_t addr = 0;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        what = r->ExceptionInformation[0] == 1 ? "write" :
               r->ExceptionInformation[0] == 8 ? "execute" : "read";
        addr = (uintptr_t)r->ExceptionInformation[1];
    }

    plog("FAULT code=%08lX at %s+0x%llX (%p), tried to %s %016llX",
         (unsigned long)r->ExceptionCode, mod, (unsigned long long)off, pc,
         what, (unsigned long long)addr);
    return EXCEPTION_CONTINUE_SEARCH;
}

__declspec(dllexport) void *CorsairPluginGetInstance(void)
{
    unsigned char *p = (unsigned char *)halloc(IFACE_SIZE + SLACK);
    if (!p) return NULL;

    *(void **)(p + 0x00) = (void *)stub_getDeviceInfo;
    *(void **)(p + 0x08) = (void *)slot_setLedsColors;
    *(void **)(p + 0x10) = (void *)stub_subscribe;
    *(void **)(p + 0x18) = (void *)slot_unsubscribe;
    *(void **)(p + 0x20) = (void *)stub_getDeviceView;
    *(void **)(p + 0x28) = (void *)slot_freeDeviceInfo;
    *(void **)(p + 0x30) = (void *)slot_freeDeviceView;
    *(void **)(p + 0x50) = (void *)stub_setMode;

    /* Order matters here and is easy to get wrong: 0x70 takes five
     * arguments and returns bool, 0x78 takes four and returns a pointer.
     * Swapped, the host dereferences the bool. */
    *(void **)(p + 0x70) = (void *)stub_propInfo;
    *(void **)(p + 0x78) = (void *)stub_readProp;
    *(void **)(p + 0x80) = (void *)stub_writeProp;
    *(void **)(p + 0x88) = (void *)stub_freeProp;

    return p;
}

__declspec(dllexport) void __cdecl CorsairPluginFreeInstance(void *inst)
{
    /* The host frees it; touching its heap here would be rule 2 in reverse. */
    (void)inst;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        cfg_init(self);
        plog("---- " AB_BASE " " AB_VERSION " loaded, pid %lu",
             (unsigned long)GetCurrentProcessId());
        plog("     settings: %s", g_ini[0] ? g_ini : "(none, using defaults)");
        AddVectoredExceptionHandler(1, fault_logger);
    } else if (reason == DLL_PROCESS_DETACH) {
        plog("---- unloaded, pid %lu", (unsigned long)GetCurrentProcessId());
    }
    return TRUE;
}
