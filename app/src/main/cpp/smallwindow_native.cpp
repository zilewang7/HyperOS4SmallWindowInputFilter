#include <android/choreographer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr char kLogTag[] = "SmallWindowNative";
constexpr char kSpawnerPath[] = "/system_ext/bin/hyos_spawner";
constexpr char kLauncherProcess[] = "com.miui.home";
constexpr char kLauncherName[] = "libapp_launcher.so";
constexpr char kHyperRuntimeName[] = "libhyper_os_flutter.so";
constexpr char kDataAppPrefix[] = "/data/app/";
constexpr char kSystemLauncherPrefix[] = "/product/priv-app/MiuiHome/";
constexpr char kDataLauncherTail[] =
        "/base.apk!/lib/arm64-v8a/libapp_launcher.so";
constexpr char kSystemLauncherTail[] =
        "/MiuiHome.apk!/lib/arm64-v8a/libapp_launcher.so";

constexpr size_t kMaxSegments = 16u;
constexpr size_t kMaxSlots = 16u;
constexpr size_t kMaxProtectedPages = 16u;
constexpr size_t kActionSearchInstructions = 12u;
constexpr size_t kCoordinateSearchInstructions = 8u;
constexpr size_t kPrimaryActionSearchInstructions = 128u;
constexpr size_t kPointerDownSearchInstructions = 192u;
constexpr size_t kFunctionStartSearchInstructions = 256u;
constexpr size_t kFunctionSetupInstructions = 32u;
constexpr size_t kCancelSearchInstructions = 64u;
constexpr size_t kCancelHelperInstructions = 48u;
constexpr uint32_t kCancelActionImmediate = 0x52800061u;
constexpr int64_t kTeleportDurationNanos = 180000000;
constexpr int64_t kMinimumDropHoldNanos = 16000000;

struct NativeApiEntries {
    uint32_t version;
    void (*hook_func)(void* target, void* replacement, void** backup);
    void (*unhook_func)(void* target);
};

using NativeOnLibraryLoaded = void (*)(const char* name, void* handle);
using DispatchFn = void (*)(void* monitor, void* event, void* lock,
                            uint32_t multi_finger_status);
using MotionActionFn = int32_t (*)(void* event);
using MotionIndexFn = size_t (*)(void* event);
using MotionCoordinateFn = float (*)(void* event, size_t pointer_index);
using MotionIdFn = int32_t (*)(void* event, size_t pointer_index);
using MotionDropFn = void (*)(void* event);
using NativeHookFn = void (*)(void* target, void* replacement, void** backup);
using MadviseFn = int (*)(void* address, size_t length, int advice);

struct Segment {
    uintptr_t begin;
    uintptr_t end;
    uint32_t flags;
};

struct Image {
    uintptr_t base;
    const ElfW(Phdr)* phdr;
    size_t phnum;
    Segment segments[kMaxSegments];
    size_t segment_count;
    char path[512];
};

struct DynamicView {
    uintptr_t symbols;
    uintptr_t strings;
    size_t string_size;
    const ElfW(Rela)* plt_rela;
    size_t plt_rela_count;
    const ElfW(Rela)* dyn_rela;
    size_t dyn_rela_count;
};

struct PltEntries {
    uintptr_t addresses[kMaxSlots];
    size_t count;
};

struct HookPlan {
    void** slots[kMaxSlots];
    size_t slot_count;
    void* original;
};

struct GestureStructure {
    uintptr_t dispatch;
    uint32_t gesture_type_offset;
    uint32_t state_fallback_offset;
};

struct ProjectionState {
    bool active;
    bool handed_off;
    bool gliding;
    bool glide_complete;
    bool secondary_up;
    bool frame_pending;
    void* inner;
    void* lock;
    uint32_t multi_finger_status;
    int32_t primary_id;
    int32_t secondary_id;
    int64_t down_time;
    int64_t last_event_time;
    int64_t glide_start_nanos;
    int64_t last_move_nanos;
    float start_x;
    float start_y;
    float target_x;
    float target_y;
    float last_x;
    float last_y;
};

using MotionNewFn = void* (*)(int64_t down_time, int64_t event_time,
                              int32_t action, int32_t pointer_id,
                              float x, float y);

NativeHookFn g_native_hook = nullptr;
DispatchFn g_original_dispatch = nullptr;
MotionActionFn g_motion_get_action = nullptr;
MotionIndexFn g_motion_get_action_index = nullptr;
MotionIndexFn g_motion_get_pointer_count = nullptr;
MotionCoordinateFn g_motion_get_raw_x = nullptr;
MotionCoordinateFn g_motion_get_raw_y = nullptr;
MotionIdFn g_motion_get_pointer_id = nullptr;
MotionDropFn g_motion_drop = nullptr;
MotionNewFn g_motion_new = nullptr;
int64_t (*g_motion_get_down_time)(void* event) = nullptr;
int64_t (*g_motion_get_event_time)(void* event) = nullptr;
uint32_t g_gesture_type_offset = 0u;
uint32_t g_state_fallback_offset = 0x20u;
thread_local ProjectionState g_projection{};
thread_local uintptr_t g_projection_epoch = 1u;
MadviseFn g_original_madvise = nullptr;
uintptr_t g_guarded_runtime_base = 0u;
volatile uint32_t g_guard_state = 0u;
volatile uint32_t g_launcher_state = 0u;
volatile uint32_t g_page_lock = 0u;
uintptr_t g_protected_pages[kMaxProtectedPages]{};
size_t g_protected_page_count = 0u;

size_t PageSize() {
    static size_t value = 0u;
    if (value == 0u) {
        const long queried = sysconf(_SC_PAGESIZE);
        value = queried > 0 ? static_cast<size_t>(queried) : size_t{4096};
    }
    return value;
}

uintptr_t PageStart(uintptr_t address) {
    return address & ~(static_cast<uintptr_t>(PageSize()) - 1u);
}

const char* BaseName(const char* path) {
    if (path == nullptr) return nullptr;
    const char* slash = strrchr(path, '/');
    return slash == nullptr ? path : slash + 1;
}

bool EndsWith(const char* value, const char* suffix) {
    if (value == nullptr || suffix == nullptr) return false;
    const size_t value_length = strlen(value);
    const size_t suffix_length = strlen(suffix);
    return suffix_length <= value_length &&
            memcmp(value + value_length - suffix_length,
                   suffix, suffix_length) == 0;
}

bool IsLauncherLibraryPath(const char* path) {
    if (path == nullptr) return false;
    return strcmp(path, kLauncherName) == 0 ||
            (strncmp(path, kDataAppPrefix,
                     sizeof(kDataAppPrefix) - 1u) == 0 &&
             EndsWith(path, kDataLauncherTail)) ||
            (strncmp(path, kSystemLauncherPrefix,
                     sizeof(kSystemLauncherPrefix) - 1u) == 0 &&
             EndsWith(path, kSystemLauncherTail));
}

bool ReadSmallFile(const char* path, char* output, size_t capacity) {
    if (capacity < 2u) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t result;
    do {
        result = read(fd, output, capacity - 1u);
    } while (result < 0 && errno == EINTR);
    close(fd);
    if (result <= 0 || static_cast<size_t>(result) >= capacity) return false;
    output[result] = '\0';
    return true;
}

bool IsLauncherHyosProcess() {
    char executable[128]{};
    const ssize_t length = readlink("/proc/self/exe", executable,
                                    sizeof(executable) - 1u);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(executable)) {
        return false;
    }
    executable[length] = '\0';
    char process[64]{};
    return strcmp(executable, kSpawnerPath) == 0 &&
            ReadSmallFile("/proc/self/cmdline", process, sizeof(process)) &&
            strcmp(process, kLauncherProcess) == 0;
}

void FileLog(const char* message) {
#if SMALLWINDOW_DIAGNOSTIC
    constexpr const char* paths[] = {
            "/data/user_de/0/com.miui.home/cache/smallwindow_native.log",
            "/data/user/0/com.miui.home/cache/smallwindow_native.log",
    };
    for (const char* path : paths) {
        const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                            0644);
        if (fd < 0) continue;
        char line[512]{};
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        int length = snprintf(line, sizeof(line), "%lld.%03lld %s\n",
                static_cast<long long>(ts.tv_sec),
                static_cast<long long>(ts.tv_nsec / 1000000), message);
        if (length > 0) {
            if (static_cast<size_t>(length) >= sizeof(line)) {
                length = static_cast<int>(sizeof(line) - 1u);
            }
            const ssize_t ignored = write(fd, line, static_cast<size_t>(length));
            (void) ignored;
        }
        close(fd);
        return;
    }
#else
    (void) message;
#endif
}

void Log(int priority, const char* message) {
    __android_log_write(priority, kLogTag, message);
    FileLog(message);
}

bool RangeInImage(const Image& image, uintptr_t address, size_t size,
                  uint32_t required_flags = 0u) {
    if (size > UINTPTR_MAX - address) return false;
    const uintptr_t end = address + size;
    for (size_t i = 0u; i < image.segment_count; ++i) {
        const Segment& segment = image.segments[i];
        if (address >= segment.begin && end <= segment.end &&
                (segment.flags & required_flags) == required_flags) {
            return true;
        }
    }
    return false;
}

struct FindImageRequest {
    const char* name;
    uintptr_t base;
    Image result;
    size_t matches;
};

int FindImageCallback(dl_phdr_info* info, size_t, void* opaque) {
    auto* request = static_cast<FindImageRequest*>(opaque);
    const char* path = info->dlpi_name;
    bool matches = false;
    if (request->base != 0u) {
        matches = static_cast<uintptr_t>(info->dlpi_addr) == request->base;
    } else if (request->name != nullptr && path != nullptr) {
        matches = strchr(request->name, '/') != nullptr
                ? strcmp(request->name, path) == 0
                : strcmp(request->name, BaseName(path)) == 0;
    }
    if (!matches) return 0;
    ++request->matches;
    if (request->matches != 1u || info->dlpi_phnum == 0u ||
            info->dlpi_phnum > 128u) {
        return 0;
    }
    Image& image = request->result;
    image.base = static_cast<uintptr_t>(info->dlpi_addr);
    image.phdr = info->dlpi_phdr;
    image.phnum = info->dlpi_phnum;
    if (path != nullptr) {
        const size_t length = strnlen(path, sizeof(image.path));
        if (length >= sizeof(image.path)) return 0;
        memcpy(image.path, path, length + 1u);
    }
    for (size_t i = 0u; i < image.phnum; ++i) {
        const ElfW(Phdr)& phdr = image.phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0u ||
                image.segment_count >= kMaxSegments ||
                phdr.p_vaddr > UINTPTR_MAX - image.base ||
                phdr.p_memsz > UINTPTR_MAX - (image.base + phdr.p_vaddr)) {
            continue;
        }
        const uintptr_t begin = image.base + phdr.p_vaddr;
        image.segments[image.segment_count++] = {
                begin, begin + phdr.p_memsz, phdr.p_flags};
    }
    return 0;
}

bool FindImage(const char* name, uintptr_t base, Image* output) {
    if (output == nullptr || (name == nullptr && base == 0u)) return false;
    FindImageRequest request{name, base, {}, 0u};
    dl_iterate_phdr(FindImageCallback, &request);
    if (request.matches != 1u || request.result.segment_count == 0u) {
        return false;
    }
    *output = request.result;
    return true;
}

uintptr_t RuntimeAddress(const Image& image, ElfW(Addr) value) {
    if (value == 0u) return 0u;
    const uintptr_t address = static_cast<uintptr_t>(value);
    if (RangeInImage(image, address, 1u)) return address;
    return address <= UINTPTR_MAX - image.base ? image.base + address : 0u;
}

bool BuildDynamicView(const Image& image, DynamicView* output) {
    if (output == nullptr) return false;
    uintptr_t dynamic_address = 0u;
    size_t dynamic_count = 0u;
    for (size_t i = 0u; i < image.phnum; ++i) {
        const ElfW(Phdr)& phdr = image.phdr[i];
        if (phdr.p_type == PT_DYNAMIC && phdr.p_memsz >= sizeof(ElfW(Dyn)) &&
                phdr.p_vaddr <= UINTPTR_MAX - image.base) {
            dynamic_address = image.base + phdr.p_vaddr;
            dynamic_count = phdr.p_memsz / sizeof(ElfW(Dyn));
            break;
        }
    }
    if (dynamic_address == 0u || dynamic_count == 0u ||
            dynamic_count > SIZE_MAX / sizeof(ElfW(Dyn)) ||
            !RangeInImage(image, dynamic_address,
                          dynamic_count * sizeof(ElfW(Dyn)), PF_R)) {
        return false;
    }

    DynamicView view{};
    uintptr_t plt_rela = 0u;
    size_t plt_size = 0u;
    uintptr_t dyn_rela = 0u;
    size_t dyn_rela_size = 0u;
    bool plt_is_rela = false;
    const auto* dynamic = reinterpret_cast<const ElfW(Dyn)*>(dynamic_address);
    for (size_t i = 0u; i < dynamic_count; ++i) {
        const ElfW(Dyn)& entry = dynamic[i];
        if (entry.d_tag == DT_NULL) break;
        switch (entry.d_tag) {
            case DT_SYMTAB:
                view.symbols = RuntimeAddress(image, entry.d_un.d_ptr);
                break;
            case DT_STRTAB:
                view.strings = RuntimeAddress(image, entry.d_un.d_ptr);
                break;
            case DT_STRSZ:
                view.string_size = entry.d_un.d_val;
                break;
            case DT_JMPREL:
                plt_rela = RuntimeAddress(image, entry.d_un.d_ptr);
                break;
            case DT_PLTRELSZ:
                plt_size = entry.d_un.d_val;
                break;
            case DT_PLTREL:
                plt_is_rela = entry.d_un.d_val == DT_RELA;
                break;
            case DT_RELA:
                dyn_rela = RuntimeAddress(image, entry.d_un.d_ptr);
                break;
            case DT_RELASZ:
                dyn_rela_size = entry.d_un.d_val;
                break;
            default:
                break;
        }
    }
    if (view.symbols == 0u || view.strings == 0u || view.string_size == 0u ||
            !RangeInImage(image, view.strings, view.string_size, PF_R)) {
        return false;
    }
    if (plt_rela != 0u && plt_size != 0u && plt_is_rela &&
            plt_size % sizeof(ElfW(Rela)) == 0u &&
            RangeInImage(image, plt_rela, plt_size, PF_R)) {
        view.plt_rela = reinterpret_cast<const ElfW(Rela)*>(plt_rela);
        view.plt_rela_count = plt_size / sizeof(ElfW(Rela));
    }
    if (dyn_rela != 0u && dyn_rela_size != 0u &&
            dyn_rela_size % sizeof(ElfW(Rela)) == 0u &&
            RangeInImage(image, dyn_rela, dyn_rela_size, PF_R)) {
        view.dyn_rela = reinterpret_cast<const ElfW(Rela)*>(dyn_rela);
        view.dyn_rela_count = dyn_rela_size / sizeof(ElfW(Rela));
    }
    *output = view;
    return view.plt_rela_count != 0u || view.dyn_rela_count != 0u;
}

bool SymbolNameEquals(const Image& image, const DynamicView& view,
                      uint32_t symbol_index, const char* expected) {
    if (expected == nullptr) return false;
    const size_t offset =
            static_cast<size_t>(symbol_index) * sizeof(ElfW(Sym));
    if (offset > UINTPTR_MAX - view.symbols) return false;
    const uintptr_t address = view.symbols + offset;
    if (!RangeInImage(image, address, sizeof(ElfW(Sym)), PF_R)) return false;
    const auto* symbol = reinterpret_cast<const ElfW(Sym)*>(address);
    if (symbol->st_name >= view.string_size) return false;
    const char* name = reinterpret_cast<const char*>(view.strings) +
            symbol->st_name;
    const size_t remaining = view.string_size - symbol->st_name;
    const size_t length = strnlen(name, remaining);
    return length < remaining && strlen(expected) == length &&
            memcmp(name, expected, length) == 0;
}

bool CollectRelocations(const Image& image, const DynamicView& view,
                        const ElfW(Rela)* relocations, size_t count,
                        const char* symbol, HookPlan* plan) {
    for (size_t i = 0u; i < count; ++i) {
        const ElfW(Rela)& relocation = relocations[i];
        const uint32_t type = ELF64_R_TYPE(relocation.r_info);
        if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) {
            continue;
        }
        const uint32_t symbol_index = ELF64_R_SYM(relocation.r_info);
        if (!SymbolNameEquals(image, view, symbol_index, symbol)) continue;
        const uintptr_t address = RuntimeAddress(image, relocation.r_offset);
        if (address == 0u ||
                !RangeInImage(image, address, sizeof(void*))) {
            return false;
        }
        if (plan->slot_count >= kMaxSlots) return false;
        plan->slots[plan->slot_count++] = reinterpret_cast<void**>(address);
    }
    return true;
}

bool CollectSymbolSlots(const Image& image, const char* symbol,
                        HookPlan* output) {
    if (symbol == nullptr || output == nullptr) return false;
    DynamicView view{};
    HookPlan plan{};
    if (!BuildDynamicView(image, &view) ||
            !CollectRelocations(image, view, view.plt_rela,
                                view.plt_rela_count, symbol, &plan) ||
            !CollectRelocations(image, view, view.dyn_rela,
                                view.dyn_rela_count, symbol, &plan) ||
            plan.slot_count == 0u) {
        return false;
    }
    *output = plan;
    return true;
}

bool PreparePltHook(const Image& image, const char* symbol,
                    void* replacement, HookPlan* output) {
    if (replacement == nullptr || output == nullptr) return false;
    HookPlan plan{};
    if (!CollectSymbolSlots(image, symbol, &plan)) return false;
    for (size_t i = 0u; i < plan.slot_count; ++i) {
        void* current = __atomic_load_n(plan.slots[i], __ATOMIC_ACQUIRE);
        if (current == replacement) continue;
        if (current == nullptr) return false;
        if (plan.original == nullptr) {
            plan.original = current;
        } else if (plan.original != current) {
            return false;
        }
    }
    *output = plan;
    return true;
}

int ReadProtection(uintptr_t address) {
    FILE* maps = fopen("/proc/self/maps", "re");
    if (maps == nullptr) return -1;
    char line[768]{};
    int result = -1;
    while (fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long begin = 0u;
        unsigned long long end = 0u;
        char permissions[5]{};
        if (sscanf(line, "%llx-%llx %4s", &begin, &end, permissions) != 3 ||
                address < begin || address >= end) {
            continue;
        }
        result = (permissions[0] == 'r' ? PROT_READ : 0) |
                (permissions[1] == 'w' ? PROT_WRITE : 0) |
                (permissions[2] == 'x' ? PROT_EXEC : 0);
        break;
    }
    fclose(maps);
    return result;
}

bool WritePointer(void** slot, void* value) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
    const uintptr_t page = PageStart(address);
    const int protection = ReadProtection(address);
    if (protection < 0 ||
            mprotect(reinterpret_cast<void*>(page), PageSize(),
                     protection | PROT_WRITE) != 0) {
        return false;
    }
    __atomic_store_n(slot, value, __ATOMIC_RELEASE);
    return mprotect(reinterpret_cast<void*>(page), PageSize(), protection) == 0;
}

bool CommitPltHook(const HookPlan& plan, void* replacement) {
    void** changed[kMaxSlots]{};
    size_t changed_count = 0u;
    for (size_t i = 0u; i < plan.slot_count; ++i) {
        if (__atomic_load_n(plan.slots[i], __ATOMIC_ACQUIRE) == replacement) {
            continue;
        }
        if (!WritePointer(plan.slots[i], replacement)) {
            while (changed_count > 0u && plan.original != nullptr) {
                --changed_count;
                WritePointer(changed[changed_count], plan.original);
            }
            return false;
        }
        changed[changed_count++] = plan.slots[i];
    }
    return true;
}

bool AddProtectedPage(uintptr_t address) {
    const uintptr_t page = PageStart(address);
    while (__atomic_exchange_n(&g_page_lock, uint32_t{1},
                               __ATOMIC_ACQUIRE) != 0u) {
    }
    size_t count = __atomic_load_n(&g_protected_page_count,
                                   __ATOMIC_RELAXED);
    for (size_t i = 0u; i < count; ++i) {
        if (g_protected_pages[i] == page) {
            __atomic_store_n(&g_page_lock, uint32_t{0}, __ATOMIC_RELEASE);
            return true;
        }
    }
    if (count >= kMaxProtectedPages) {
        __atomic_store_n(&g_page_lock, uint32_t{0}, __ATOMIC_RELEASE);
        return false;
    }
    g_protected_pages[count] = page;
    __atomic_store_n(&g_protected_page_count, count + 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_page_lock, uint32_t{0}, __ATOMIC_RELEASE);
    return true;
}

int GuardedMadvise(void* address, size_t length, int advice) {
    MadviseFn original = __atomic_load_n(&g_original_madvise,
                                         __ATOMIC_ACQUIRE);
    if (original == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    if (advice != MADV_DONTNEED || length == 0u) {
        return original(address, length, advice);
    }
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    if (begin % PageSize() != 0u || length > UINTPTR_MAX - begin) {
        return original(address, length, advice);
    }
    const uintptr_t end = begin + length;
    uintptr_t cursor = begin;
    bool found = false;
    while (cursor < end) {
        uintptr_t next = end;
        const size_t count = __atomic_load_n(&g_protected_page_count,
                                              __ATOMIC_ACQUIRE);
        for (size_t i = 0u; i < count; ++i) {
            const uintptr_t page = g_protected_pages[i];
            if (page >= cursor && page < end && page < next) next = page;
        }
        if (next == end) break;
        if (next > cursor &&
                original(reinterpret_cast<void*>(cursor), next - cursor,
                         advice) != 0) {
            return -1;
        }
        cursor = next + PageSize();
        if (cursor > end) cursor = end;
        found = true;
    }
    if (!found) return original(address, length, advice);
    return cursor < end
            ? original(reinterpret_cast<void*>(cursor), end - cursor, advice)
            : 0;
}

bool EnsureMadviseGuard() {
    if (__atomic_load_n(&g_guard_state, __ATOMIC_ACQUIRE) == 2u) return true;
    Image runtime{};
    if (!FindImage(kHyperRuntimeName, 0u, &runtime)) return false;
    if (runtime.base == __atomic_load_n(&g_guarded_runtime_base,
                                        __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&g_guard_state, __ATOMIC_ACQUIRE) == 2u) {
        return true;
    }
    uint32_t expected = 0u;
    if (!__atomic_compare_exchange_n(&g_guard_state, &expected, uint32_t{1},
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return expected == 2u;
    }
    HookPlan plan{};
    if (!PreparePltHook(runtime, "madvise",
                        reinterpret_cast<void*>(GuardedMadvise), &plan)) {
        __atomic_store_n(&g_guard_state, uint32_t{3}, __ATOMIC_RELEASE);
        Log(ANDROID_LOG_ERROR, "failed to resolve HYOS madvise call site");
        return false;
    }
    for (size_t i = 0u; i < plan.slot_count; ++i) {
        if (!AddProtectedPage(reinterpret_cast<uintptr_t>(plan.slots[i]))) {
            __atomic_store_n(&g_guard_state, uint32_t{3}, __ATOMIC_RELEASE);
            return false;
        }
    }
    if (plan.original != nullptr) {
        __atomic_store_n(&g_original_madvise,
                         reinterpret_cast<MadviseFn>(plan.original),
                         __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&g_original_madvise, __ATOMIC_ACQUIRE) == nullptr ||
            !CommitPltHook(plan, reinterpret_cast<void*>(GuardedMadvise))) {
        __atomic_store_n(&g_guard_state, uint32_t{3}, __ATOMIC_RELEASE);
        Log(ANDROID_LOG_ERROR, "failed to install HYOS hook-page guard");
        return false;
    }
    __atomic_store_n(&g_guarded_runtime_base, runtime.base,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_guard_state, uint32_t{2}, __ATOMIC_RELEASE);
    Log(ANDROID_LOG_INFO, "installed HYOS hook-page guard");
    return true;
}

bool AddSignedOffset(uintptr_t base, int64_t offset, uintptr_t* output) {
    if (output == nullptr) return false;
    if (offset >= 0) {
        const uint64_t positive = static_cast<uint64_t>(offset);
        if (positive > UINTPTR_MAX - base) return false;
        *output = base + static_cast<uintptr_t>(positive);
        return true;
    }
    const uint64_t magnitude = static_cast<uint64_t>(-(offset + 1)) + 1u;
    if (magnitude > base) return false;
    *output = base - static_cast<uintptr_t>(magnitude);
    return true;
}

bool ReadInstruction(const Image& image, uintptr_t address,
                     uint32_t* output) {
    if (output == nullptr || address % sizeof(uint32_t) != 0u ||
            !RangeInImage(image, address, sizeof(uint32_t), PF_R | PF_X)) {
        return false;
    }
    memcpy(output, reinterpret_cast<const void*>(address), sizeof(*output));
    return true;
}

bool DecodeBlTarget(uint32_t instruction, uintptr_t pc, uintptr_t* output) {
    if ((instruction & 0xfc000000u) != 0x94000000u) return false;
    int64_t immediate = static_cast<int64_t>(instruction & 0x03ffffffu);
    if ((immediate & (int64_t{1} << 25)) != 0) {
        immediate -= int64_t{1} << 26;
    }
    return AddSignedOffset(pc, immediate * 4, output);
}

bool DecodeAdrpTarget(uint32_t instruction, uintptr_t pc,
                      uintptr_t* output) {
    if ((instruction & 0x9f000000u) != 0x90000000u) return false;
    int64_t immediate = static_cast<int64_t>(
            ((instruction >> 29) & 0x3u) |
            (((instruction >> 5) & 0x7ffffu) << 2));
    if ((immediate & (int64_t{1} << 20)) != 0) {
        immediate -= int64_t{1} << 21;
    }
    return AddSignedOffset(pc & ~uintptr_t{0xfff}, immediate * 4096,
                           output);
}

bool DecodePltSlot(const Image& image, uintptr_t address,
                   uintptr_t* output) {
    uint32_t adrp = 0u;
    uint32_t load = 0u;
    uint32_t add = 0u;
    uint32_t branch = 0u;
    if (!ReadInstruction(image, address, &adrp) ||
            !ReadInstruction(image, address + 4u, &load) ||
            !ReadInstruction(image, address + 8u, &add) ||
            !ReadInstruction(image, address + 12u, &branch) ||
            (adrp & 0x1fu) != 16u ||
            (load & 0xffc003ffu) != 0xf9400211u ||
            (add & 0xffc003ffu) != 0x91000210u ||
            branch != 0xd61f0220u) {
        return false;
    }
    const uintptr_t load_offset =
            static_cast<uintptr_t>((load >> 10) & 0xfffu) * 8u;
    const uintptr_t add_offset =
            static_cast<uintptr_t>((add >> 10) & 0xfffu);
    uintptr_t page = 0u;
    if (load_offset != add_offset || !DecodeAdrpTarget(adrp, address, &page) ||
            load_offset > UINTPTR_MAX - page) {
        return false;
    }
    *output = page + load_offset;
    return true;
}

bool AddPltEntry(uintptr_t address, PltEntries* entries) {
    if (entries == nullptr) return false;
    for (size_t i = 0u; i < entries->count; ++i) {
        if (entries->addresses[i] == address) return true;
    }
    if (entries->count >= kMaxSlots) return false;
    entries->addresses[entries->count++] = address;
    return true;
}

bool FindPltEntries(const Image& image, const HookPlan& slots,
                    PltEntries* output) {
    if (output == nullptr || slots.slot_count == 0u) return false;
    PltEntries entries{};
    for (size_t segment_index = 0u;
         segment_index < image.segment_count; ++segment_index) {
        const Segment& segment = image.segments[segment_index];
        if ((segment.flags & PF_X) == 0u) continue;
        uintptr_t cursor = (segment.begin + 3u) & ~uintptr_t{3u};
        while (cursor <= segment.end && segment.end - cursor >= 16u) {
            uintptr_t slot = 0u;
            if (DecodePltSlot(image, cursor, &slot)) {
                bool matches = false;
                for (size_t slot_index = 0u;
                     slot_index < slots.slot_count; ++slot_index) {
                    if (slot == reinterpret_cast<uintptr_t>(
                                    slots.slots[slot_index])) {
                        matches = true;
                        break;
                    }
                }
                if (matches && !AddPltEntry(cursor, &entries)) return false;
                uint32_t prefix = 0u;
                if (matches && cursor >= segment.begin + 4u &&
                        ReadInstruction(image, cursor - 4u, &prefix) &&
                        prefix == 0xd503245fu &&
                        !AddPltEntry(cursor - 4u, &entries)) {
                    return false;
                }
            }
            cursor += 4u;
        }
    }
    *output = entries;
    return entries.count != 0u;
}

bool FindImportedPltEntries(const Image& image, const char* symbol,
                            PltEntries* output) {
    HookPlan slots{};
    return CollectSymbolSlots(image, symbol, &slots) &&
            FindPltEntries(image, slots, output);
}

bool IsCallTo(uint32_t instruction, uintptr_t pc,
              const PltEntries& entries) {
    uintptr_t target = 0u;
    if (!DecodeBlTarget(instruction, pc, &target)) return false;
    for (size_t i = 0u; i < entries.count; ++i) {
        if (entries.addresses[i] == target) return true;
    }
    return false;
}

bool IsMovX(uint32_t instruction, uint32_t destination, uint32_t source) {
    return destination < 32u && source < 32u &&
            instruction == (0xaa0003e0u | (source << 16) | destination);
}

bool DecodeMovXToX0(uint32_t instruction, uint32_t* source) {
    if (source == nullptr ||
            (instruction & 0xffe0ffffu) != 0xaa0003e0u) {
        return false;
    }
    *source = (instruction >> 16) & 0x1fu;
    return *source != 31u;
}

bool IsMovW(uint32_t instruction, uint32_t destination, uint32_t source) {
    return destination < 32u && source < 32u &&
            instruction == (0x2a0003e0u | (source << 16) | destination);
}

bool DecodeMovWFromW0(uint32_t instruction, uint32_t* destination) {
    if (destination == nullptr ||
            (instruction & 0xffffffe0u) != 0x2a0003e0u) {
        return false;
    }
    *destination = instruction & 0x1fu;
    return *destination != 31u;
}

bool DecodeLdrWImmediate(uint32_t instruction, uint32_t* destination,
                         uint32_t* base, uint32_t* offset) {
    if (destination == nullptr || base == nullptr || offset == nullptr ||
            (instruction & 0xffc00000u) != 0xb9400000u) {
        return false;
    }
    *destination = instruction & 0x1fu;
    *base = (instruction >> 5) & 0x1fu;
    *offset = ((instruction >> 10) & 0xfffu) * sizeof(uint32_t);
    return *destination != 31u && *base != 31u;
}

bool DecodeLdrXImmediate(uint32_t instruction, uint32_t* destination,
                         uint32_t* base, uint32_t* offset) {
    if (destination == nullptr || base == nullptr || offset == nullptr ||
            (instruction & 0xffc00000u) != 0xf9400000u) {
        return false;
    }
    *destination = instruction & 0x1fu;
    *base = (instruction >> 5) & 0x1fu;
    *offset = ((instruction >> 10) & 0xfffu) * sizeof(uint64_t);
    return *destination != 31u && *base != 31u;
}

bool IsCbnzX(uint32_t instruction, uint32_t reg) {
    return reg < 32u && (instruction & 0xff000000u) == 0xb5000000u &&
            (instruction & 0x1fu) == reg;
}

bool DecodeAddXImmediate(uint32_t instruction, uint32_t* destination,
                         uint32_t* base, uint32_t* immediate) {
    if (destination == nullptr || base == nullptr || immediate == nullptr ||
            (instruction & 0xff800000u) != 0x91000000u) {
        return false;
    }
    *destination = instruction & 0x1fu;
    *base = (instruction >> 5) & 0x1fu;
    *immediate = (instruction >> 10) & 0xfffu;
    return *destination != 31u && *base != 31u;
}

bool IsCmpWImmediate(uint32_t instruction, uint32_t source,
                     uint32_t immediate) {
    return source < 32u && immediate < 4096u &&
            instruction == (0x7100001fu | (immediate << 10) |
                            (source << 5));
}

bool IsConditionalBranch(uint32_t instruction, uint32_t condition) {
    return condition < 16u &&
            (instruction & 0xff000010u) == 0x54000000u &&
            (instruction & 0xfu) == condition;
}

bool DecodeConditionalBranchTarget(uint32_t instruction, uintptr_t pc,
                                   uint32_t condition,
                                   uintptr_t* output) {
    if (!IsConditionalBranch(instruction, condition)) return false;
    int64_t immediate = static_cast<int64_t>((instruction >> 5) & 0x7ffffu);
    if ((immediate & (int64_t{1} << 18)) != 0) {
        immediate -= int64_t{1} << 19;
    }
    return AddSignedOffset(pc, immediate * 4, output);
}

bool HasCoordinateArguments(const Image& image, uintptr_t call,
                            uint32_t event_register) {
    for (size_t distance = 2u; distance <= 4u; ++distance) {
        if (call < distance * 4u) continue;
        uint32_t first = 0u;
        uint32_t second = 0u;
        const uintptr_t setup = call - distance * 4u;
        if (ReadInstruction(image, setup, &first) &&
                ReadInstruction(image, setup + 4u, &second) &&
                IsMovX(first, 0u, event_register) &&
                IsMovX(second, 1u, 31u)) {
            return true;
        }
    }
    return false;
}

bool FindX0SourceBeforeCall(const Image& image, uintptr_t call,
                            uint32_t* source) {
    if (source == nullptr) return false;
    for (size_t distance = 1u; distance <= 4u; ++distance) {
        const size_t bytes = distance * sizeof(uint32_t);
        if (call < bytes) break;
        uint32_t instruction = 0u;
        if (ReadInstruction(image, call - bytes, &instruction) &&
                DecodeMovXToX0(instruction, source)) {
            return true;
        }
    }
    return false;
}

struct ActionCapture {
    uintptr_t address;
    uint32_t event_register;
    uint32_t action_register;
    bool requires_previous_gate;
};

bool LocateActionCapture(const Image& image, uintptr_t action_call,
                         const PltEntries& action_entries,
                         ActionCapture* output) {
    if (output == nullptr || action_call < 4u) return false;
    uint32_t call = 0u;
    uint32_t compare = 0u;
    uint32_t branch = 0u;
    ActionCapture capture{};
    if (!FindX0SourceBeforeCall(image, action_call,
                                &capture.event_register) ||
            !ReadInstruction(image, action_call, &call) ||
            !IsCallTo(call, action_call, action_entries) ||
            !ReadInstruction(image, action_call + 4u, &compare) ||
            !ReadInstruction(image, action_call + 8u, &branch)) {
        return false;
    }

    uint32_t capture_instruction = 0u;
    if (IsCmpWImmediate(compare, 0u, 13u) &&
            IsConditionalBranch(branch, 2u) &&
            ReadInstruction(image, action_call + 12u,
                            &capture_instruction) &&
            DecodeMovWFromW0(capture_instruction,
                             &capture.action_register)) {
        capture.address = action_call + 12u;
        capture.requires_previous_gate = true;
        *output = capture;
        return true;
    }

    uint32_t condition = 0u;
    if (IsCmpWImmediate(compare, 0u, 13u) &&
            IsConditionalBranch(branch, 3u)) {
        condition = 3u;
    } else if (IsCmpWImmediate(compare, 0u, 12u) &&
            IsConditionalBranch(branch, 9u)) {
        condition = 9u;
    } else {
        return false;
    }

    uintptr_t branch_target = 0u;
    if (!DecodeConditionalBranchTarget(branch, action_call + 8u,
                                       condition, &branch_target) ||
            branch_target <= action_call + 8u ||
            branch_target - action_call >
                    kPrimaryActionSearchInstructions * sizeof(uint32_t) ||
            !ReadInstruction(image, branch_target, &capture_instruction) ||
            !DecodeMovWFromW0(capture_instruction,
                              &capture.action_register)) {
        return false;
    }
    capture.address = branch_target;
    capture.requires_previous_gate = false;
    *output = capture;
    return true;
}

struct CancelEvidence {
    uintptr_t anchor;
    uintptr_t helper_target;
    bool outlined;
};

bool HasCancelActionImmediate(const Image& image, uintptr_t set_call,
                              size_t instructions_before) {
    for (size_t back = 1u; back <= 3u && back <= instructions_before; ++back) {
        uint32_t argument = 0u;
        if (ReadInstruction(image, set_call - back * sizeof(uint32_t),
                            &argument) &&
                argument == kCancelActionImmediate) {
            return true;
        }
    }
    return false;
}

bool CancelHelperClones(const Image& image, uintptr_t target,
                        const PltEntries& clone_entries,
                        const PltEntries& set_action_entries) {
    for (size_t i = 0u; i < kCancelHelperInstructions; ++i) {
        const uintptr_t cursor = target + i * sizeof(uint32_t);
        uint32_t instruction = 0u;
        if (!ReadInstruction(image, cursor, &instruction)) return false;
        if (!IsCallTo(instruction, cursor, clone_entries)) continue;
        for (size_t k = 1u; k <= 5u; ++k) {
            const uintptr_t set_call = cursor + k * sizeof(uint32_t);
            if (!ReadInstruction(image, set_call, &instruction) ||
                    !IsCallTo(instruction, set_call, set_action_entries)) {
                continue;
            }
            if (HasCancelActionImmediate(image, set_call, k)) return true;
        }
    }
    return false;
}

// The launcher cancels the active drag once a second pointer goes down. Older
// builds inline that cancel body; newer builds outline it into a helper, so the
// evidence is either the inline setAction call or the call site of that helper.
bool LocateCancelEvidence(const Image& image, uintptr_t pointer_check,
                          const PltEntries& clone_entries,
                          const PltEntries& set_action_entries,
                          CancelEvidence* output) {
    if (output == nullptr) return false;
    for (size_t j = 2u; j <= kCancelSearchInstructions; ++j) {
        const uintptr_t clone_call = pointer_check + j * sizeof(uint32_t);
        uint32_t instruction = 0u;
        if (!ReadInstruction(image, clone_call, &instruction) ||
                !IsCallTo(instruction, clone_call, clone_entries)) {
            continue;
        }
        for (size_t k = 1u; k <= 5u; ++k) {
            const uintptr_t set_call = clone_call + k * sizeof(uint32_t);
            if (!ReadInstruction(image, set_call, &instruction) ||
                    !IsCallTo(instruction, set_call, set_action_entries)) {
                continue;
            }
            if (HasCancelActionImmediate(image, set_call, k)) {
                output->anchor = set_call;
                output->helper_target = 0u;
                output->outlined = false;
                return true;
            }
        }
    }
    for (size_t j = 2u; j <= kCancelSearchInstructions; ++j) {
        const uintptr_t call = pointer_check + j * sizeof(uint32_t);
        uint32_t instruction = 0u;
        if (!ReadInstruction(image, call, &instruction)) return false;
        uintptr_t target = 0u;
        if (!DecodeBlTarget(instruction, call, &target) ||
                !RangeInImage(image, target, sizeof(uint32_t), PF_X)) {
            continue;
        }
        if (CancelHelperClones(image, target, clone_entries,
                               set_action_entries)) {
            output->anchor = call;
            output->helper_target = target;
            output->outlined = true;
            return true;
        }
    }
    return false;
}

bool ValidateGestureCandidate(const Image& image, uintptr_t action_call,
                              const PltEntries& action_entries,
                              const PltEntries& raw_x_entries,
                              const PltEntries& raw_y_entries,
                              const PltEntries& clone_entries,
                              const PltEntries& set_action_entries,
                              CancelEvidence* cancel_evidence,
                              ActionCapture* action_capture) {
    if (cancel_evidence == nullptr || action_capture == nullptr) return false;
    uint32_t instruction = 0u;
    ActionCapture capture{};
    if (!LocateActionCapture(image, action_call, action_entries, &capture)) {
        return false;
    }

    uintptr_t raw_x_call = 0u;
    for (size_t i = 1u; i <= kActionSearchInstructions; ++i) {
        const uintptr_t candidate = capture.address + i * 4u;
        if (ReadInstruction(image, candidate, &instruction) &&
                IsCallTo(instruction, candidate, raw_x_entries) &&
                HasCoordinateArguments(image, candidate,
                                       capture.event_register)) {
            raw_x_call = candidate;
            break;
        }
    }
    if (raw_x_call == 0u) return false;

    uintptr_t raw_y_call = 0u;
    for (size_t i = 1u; i <= kCoordinateSearchInstructions; ++i) {
        const uintptr_t candidate = raw_x_call + i * 4u;
        if (ReadInstruction(image, candidate, &instruction) &&
                IsCallTo(instruction, candidate, raw_y_entries) &&
                HasCoordinateArguments(image, candidate,
                                       capture.event_register)) {
            raw_y_call = candidate;
            break;
        }
    }
    if (raw_y_call == 0u) return false;

    bool action_move_check = false;
    for (size_t i = 1u; i <= 4u; ++i) {
        if (ReadInstruction(image, raw_y_call + i * 4u, &instruction) &&
                IsCmpWImmediate(instruction, capture.action_register, 2u)) {
            action_move_check = true;
            break;
        }
    }
    if (!action_move_check) return false;

    for (size_t i = 1u; i <= kPointerDownSearchInstructions; ++i) {
        const uintptr_t pointer_check = raw_y_call + i * 4u;
        uint32_t branch = 0u;
        if (!ReadInstruction(image, pointer_check, &instruction) ||
                !IsCmpWImmediate(instruction, capture.action_register, 5u) ||
                !ReadInstruction(image, pointer_check + 4u, &branch) ||
                !IsConditionalBranch(branch, 1u)) {
            continue;
        }
        CancelEvidence evidence{};
        if (!LocateCancelEvidence(image, pointer_check, clone_entries,
                                  set_action_entries, &evidence)) {
            continue;
        }
        *cancel_evidence = evidence;
        *action_capture = capture;
        return true;
    }
    return false;
}

bool LocatePrimaryActionDecision(const Image& image,
                                 uintptr_t secondary_action_call,
                                 const PltEntries& action_entries,
                                 uintptr_t* output) {
    if (output == nullptr || secondary_action_call < 4u) return false;
    ActionCapture capture{};
    if (!LocateActionCapture(image, secondary_action_call, action_entries,
                             &capture)) {
        return false;
    }
    if (!capture.requires_previous_gate) {
        *output = capture.address;
        return true;
    }

    uint32_t instruction = 0u;
    const uint32_t event_register = capture.event_register;
    const uintptr_t expected_branch_target = capture.address;
    uintptr_t match = 0u;
    size_t match_count = 0u;
    for (size_t distance = 1u;
         distance <= kPrimaryActionSearchInstructions; ++distance) {
        const size_t byte_distance = distance * 4u;
        if (secondary_action_call < byte_distance + 4u) break;
        const uintptr_t candidate = secondary_action_call - byte_distance;
        uint32_t setup = 0u;
        uint32_t compare = 0u;
        uint32_t branch = 0u;
        uintptr_t branch_target = 0u;
        if (!ReadInstruction(image, candidate - 4u, &setup) ||
                !IsMovX(setup, 0u, event_register) ||
                !ReadInstruction(image, candidate, &instruction) ||
                !IsCallTo(instruction, candidate, action_entries) ||
                !ReadInstruction(image, candidate + 4u, &compare) ||
                !IsCmpWImmediate(compare, 0u, 13u) ||
                !ReadInstruction(image, candidate + 8u, &branch) ||
                !DecodeConditionalBranchTarget(branch, candidate + 8u, 3u,
                                               &branch_target) ||
                branch_target != expected_branch_target) {
            continue;
        }
        match = candidate + 4u;
        ++match_count;
        if (match_count > 1u) return false;
    }
    if (match_count != 1u) return false;
    *output = match;
    return true;
}

bool ValidateDispatchSetup(const Image& image, uintptr_t start,
                           uintptr_t action_call,
                           const ActionCapture& capture,
                           uint32_t state_register,
                           uint32_t* state_fallback) {
    if (start >= action_call || capture.event_register < 19u ||
            capture.event_register > 28u || state_register < 19u ||
            state_register > 28u) {
        return false;
    }
    uint32_t stack_setup = 0u;
    if (!ReadInstruction(image, start, &stack_setup) ||
            (stack_setup & 0xffc003ffu) != 0xd10003ffu) {
        return false;
    }
    const uint32_t stack_size = (stack_setup >> 10) & 0xfffu;
    if (stack_size < 64u) return false;

    bool saved_frame = false;
    bool established_frame = false;
    bool loaded_inner = false;
    bool saved_event = false;
    bool saved_state = false;
    bool saved_status = false;
    uint32_t frame_offset = 0u;
    uint32_t monitor_register = 0u;
    uint32_t fallback_offset = 0u;
    const size_t instruction_count =
            static_cast<size_t>((action_call - start) / sizeof(uint32_t));
    for (size_t i = 1u; i < instruction_count; ++i) {
        uint32_t instruction = 0u;
        if (!ReadInstruction(image, start + i * sizeof(uint32_t),
                             &instruction) ||
                (instruction & 0xfffffc1fu) == 0xd65f0000u) {
            return false;
        }
        if ((instruction & 0xffc07fffu) == 0xa9007bfdu) {
            const uint32_t offset =
                    ((instruction >> 15) & 0x7fu) * sizeof(uint64_t);
            if (offset + 2u * sizeof(uint64_t) <= stack_size) {
                saved_frame = true;
                frame_offset = offset;
            }
        }
        if ((instruction & 0xffc003ffu) == 0x910003fdu &&
                ((instruction >> 10) & 0xfffu) == frame_offset) {
            established_frame = true;
        }
        uint32_t destination = 0u;
        uint32_t base = 0u;
        uint32_t offset = 0u;
        if (DecodeLdrXImmediate(instruction, &destination, &base, &offset) &&
                destination == 8u && base == 0u && offset >= 0x200u &&
                offset <= 0x400u) {
            loaded_inner = true;
        }
        saved_event = saved_event ||
                IsMovX(instruction, capture.event_register, 1u);
        saved_state = saved_state || IsMovX(instruction, state_register, 2u);
        for (uint32_t register_index = 19u;
             register_index <= 28u && !saved_status; ++register_index) {
            saved_status = IsMovW(instruction, register_index, 3u);
        }
        if (monitor_register == 0u) {
            for (uint32_t register_index = 19u; register_index <= 28u;
                 ++register_index) {
                if (IsMovX(instruction, register_index, 0u)) {
                    monitor_register = register_index;
                    break;
                }
            }
        }
        // Some launcher builds accept a null state argument and substitute an
        // object embedded in the monitor instead; that offset moves between
        // releases, so it is decoded from the guard rather than hardcoded.
        if (fallback_offset == 0u && monitor_register != 0u &&
                IsCbnzX(instruction, state_register)) {
            for (size_t look = 1u; look <= 2u && fallback_offset == 0u;
                 ++look) {
                uint32_t next = 0u;
                uint32_t add_destination = 0u;
                uint32_t add_base = 0u;
                uint32_t immediate = 0u;
                if (ReadInstruction(image,
                                    start + (i + look) * sizeof(uint32_t),
                                    &next) &&
                        DecodeAddXImmediate(next, &add_destination, &add_base,
                                            &immediate) &&
                        add_destination == state_register &&
                        add_base == monitor_register && immediate > 0u &&
                        immediate <= 0x1000u) {
                    fallback_offset = immediate;
                }
            }
        }
    }
    if (!(saved_frame && established_frame && loaded_inner && saved_event &&
          saved_state && saved_status)) {
        return false;
    }
    if (state_fallback != nullptr) {
        *state_fallback = fallback_offset;
    }
    return true;
}

bool FindDispatchStart(const Image& image, uintptr_t action_call,
                       const ActionCapture& capture,
                       uint32_t state_register, uintptr_t* output,
                       uint32_t* state_fallback) {
    if (output == nullptr) return false;
    uintptr_t match = 0u;
    uint32_t matched_fallback = 0u;
    size_t match_count = 0u;
    for (size_t distance = 1u;
         distance <= kFunctionStartSearchInstructions; ++distance) {
        const size_t bytes = distance * sizeof(uint32_t);
        if (action_call < bytes) break;
        uintptr_t candidate = action_call - bytes;
        uint32_t fallback = 0u;
        if (!ValidateDispatchSetup(image, candidate, action_call, capture,
                                   state_register, &fallback)) {
            continue;
        }
        uint32_t prefix = 0u;
        if (candidate >= sizeof(uint32_t) &&
                ReadInstruction(image, candidate - sizeof(uint32_t),
                                &prefix) &&
                prefix == 0xd503245fu) {
            candidate -= sizeof(uint32_t);
        }
        match = candidate;
        matched_fallback = fallback;
        ++match_count;
        if (match_count > 1u) return false;
    }
    if (match_count != 1u) return false;
    *output = match;
    if (state_fallback != nullptr) {
        *state_fallback = matched_fallback;
    }
    return true;
}

bool DecodeGestureTypeOffset(const Image& image, uintptr_t cancel_anchor,
                             uint32_t* output, uint32_t* state_register) {
    if (output == nullptr || state_register == nullptr) return false;
    uint32_t matched_offset = 0u;
    uint32_t matched_register = 0u;
    size_t match_count = 0u;
    for (size_t distance = 1u;
         distance <= kFunctionSetupInstructions; ++distance) {
        const size_t bytes = distance * sizeof(uint32_t);
        if (cancel_anchor < bytes) break;
        const uintptr_t candidate = cancel_anchor - bytes;
        uint32_t load = 0u;
        uint32_t destination = 0u;
        uint32_t base = 0u;
        uint32_t offset = 0u;
        if (!ReadInstruction(image, candidate, &load) ||
                !DecodeLdrWImmediate(load, &destination, &base, &offset) ||
                base < 19u || base > 28u || offset > 0x1000u) {
            continue;
        }
        bool compared = false;
        for (size_t look = 1u; look <= 4u && !compared; ++look) {
            uint32_t compare = 0u;
            if (ReadInstruction(image, candidate + look * sizeof(uint32_t),
                                &compare) &&
                    IsCmpWImmediate(compare, destination, 1u)) {
                compared = true;
            }
        }
        if (!compared) continue;
        matched_offset = offset;
        matched_register = base;
        ++match_count;
        if (match_count > 1u) return false;
    }
    if (match_count != 1u) return false;
    *output = matched_offset;
    *state_register = matched_register;
    return true;
}

bool LocateGestureStructure(const Image& launcher, GestureStructure* output) {
    if (output == nullptr || !IsLauncherLibraryPath(launcher.path)) {
        return false;
    }
    PltEntries action_entries{};
    PltEntries raw_x_entries{};
    PltEntries raw_y_entries{};
    PltEntries clone_entries{};
    PltEntries set_action_entries{};
    if (!FindImportedPltEntries(
                    launcher, "input_MotionEvent_getActionMasked",
                    &action_entries) ||
            !FindImportedPltEntries(launcher, "input_MotionEvent_getRawX",
                                   &raw_x_entries) ||
            !FindImportedPltEntries(launcher, "input_MotionEvent_getRawY",
                                   &raw_y_entries) ||
            !FindImportedPltEntries(launcher, "input_MotionEvent_clone",
                                   &clone_entries) ||
            !FindImportedPltEntries(launcher, "input_MotionEvent_setAction",
                                   &set_action_entries)) {
        return false;
    }

    GestureStructure match{};
    size_t match_count = 0u;
    for (size_t segment_index = 0u;
         segment_index < launcher.segment_count; ++segment_index) {
        const Segment& segment = launcher.segments[segment_index];
        if ((segment.flags & PF_X) == 0u) continue;
        uintptr_t cursor = (segment.begin + 3u) & ~uintptr_t{3u};
        while (cursor <= segment.end && segment.end - cursor >= 4u) {
            uint32_t instruction = 0u;
            CancelEvidence cancel_evidence{};
            uintptr_t primary_decision = 0u;
            uint32_t state_register = 0u;
            ActionCapture action_capture{};
            GestureStructure candidate{};
            if (ReadInstruction(launcher, cursor, &instruction) &&
                    IsCallTo(instruction, cursor, action_entries) &&
                    ValidateGestureCandidate(
                            launcher, cursor, action_entries, raw_x_entries,
                            raw_y_entries, clone_entries, set_action_entries,
                            &cancel_evidence, &action_capture) &&
                    LocatePrimaryActionDecision(
                            launcher, cursor, action_entries,
                            &primary_decision) &&
                    DecodeGestureTypeOffset(
                            launcher, cancel_evidence.anchor,
                            &candidate.gesture_type_offset,
                            &state_register) &&
                    FindDispatchStart(
                            launcher, cursor, action_capture,
                            state_register, &candidate.dispatch,
                            &candidate.state_fallback_offset)) {
                if (match_count == 0u) {
                    match = candidate;
                    match_count = 1u;
                } else if (candidate.dispatch != match.dispatch ||
                        candidate.gesture_type_offset !=
                                match.gesture_type_offset) {
                    return false;
                }
            }
            cursor += 4u;
        }
    }
    if (match_count != 1u) return false;
    *output = match;
    return true;
}

bool ResolveMotionApi(void* handle) {
    if (handle == nullptr) return false;
    g_motion_get_action = reinterpret_cast<MotionActionFn>(
            dlsym(handle, "input_MotionEvent_getActionMasked"));
    g_motion_get_action_index = reinterpret_cast<MotionIndexFn>(
            dlsym(handle, "input_MotionEvent_getActionIndex"));
    g_motion_get_pointer_count = reinterpret_cast<MotionIndexFn>(
            dlsym(handle, "input_MotionEvent_getPointerCount"));
    g_motion_get_raw_x = reinterpret_cast<MotionCoordinateFn>(
            dlsym(handle, "input_MotionEvent_getRawX"));
    g_motion_get_raw_y = reinterpret_cast<MotionCoordinateFn>(
            dlsym(handle, "input_MotionEvent_getRawY"));
    g_motion_get_pointer_id = reinterpret_cast<MotionIdFn>(
            dlsym(handle, "input_MotionEvent_getPointerId"));
    g_motion_get_down_time = reinterpret_cast<int64_t (*)(void*)>(
            dlsym(handle, "input_MotionEvent_getDownTime"));
    g_motion_get_event_time = reinterpret_cast<int64_t (*)(void*)>(
            dlsym(handle, "input_MotionEvent_getEventTime"));
    g_motion_new = reinterpret_cast<MotionNewFn>(
            dlsym(handle, "input_MotionEvent_new"));
    g_motion_drop = reinterpret_cast<MotionDropFn>(
            dlsym(handle, "input_MotionEvent_drop"));
    return g_motion_get_action != nullptr &&
            g_motion_get_action_index != nullptr &&
            g_motion_get_pointer_count != nullptr &&
            g_motion_get_raw_x != nullptr &&
            g_motion_get_raw_y != nullptr &&
            g_motion_get_pointer_id != nullptr &&
            g_motion_get_down_time != nullptr &&
            g_motion_get_event_time != nullptr &&
            g_motion_new != nullptr && g_motion_drop != nullptr;
}

int32_t ReadGestureType(void* monitor, void* state) {
    int32_t value = 0;
    if (state == nullptr && monitor != nullptr) {
        state = static_cast<uint8_t*>(monitor) + g_state_fallback_offset;
    }
    if (state != nullptr && g_gesture_type_offset != 0u) {
        memcpy(&value,
               static_cast<uint8_t*>(state) + g_gesture_type_offset,
               sizeof(value));
    }
    return value;
}

bool FindPointerIndex(void* event, int32_t pointer_id, size_t* output) {
    if (event == nullptr || output == nullptr) return false;
    const size_t count = g_motion_get_pointer_count(event);
    for (size_t i = 0u; i < count; ++i) {
        if (g_motion_get_pointer_id(event, i) == pointer_id) {
            *output = i;
            return true;
        }
    }
    return false;
}

int64_t MonotonicNanos() {
    timespec now{};
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0
            ? static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec
            : 0;
}

void ResetProjection() {
    ++g_projection_epoch;
    if (g_projection_epoch == 0u) ++g_projection_epoch;
    g_projection = {};
}

bool DispatchProjected(void* monitor, void* lock,
                       uint32_t multi_finger_status, int32_t action,
                       float x, float y, int64_t event_time,
                       int64_t dispatch_nanos) {
    if (g_original_dispatch == nullptr || g_motion_new == nullptr ||
            g_motion_drop == nullptr) {
        return false;
    }
    if (event_time <= g_projection.last_event_time) {
        event_time = g_projection.last_event_time + 1;
    }
    void* projected = g_motion_new(
            g_projection.down_time, event_time, action,
            g_projection.primary_id, x, y);
    if (projected == nullptr) return false;
    g_projection.last_event_time = event_time;
    g_projection.last_x = x;
    g_projection.last_y = y;
    if (action == 2) g_projection.last_move_nanos = dispatch_nanos;
    g_original_dispatch(monitor, projected, lock, multi_finger_status);
    g_motion_drop(projected);
    return true;
}

void OnProjectionFrame(int64_t frame_time_nanos, void* data);

bool ScheduleProjectionFrame() {
    if (g_projection.frame_pending) return true;
    AChoreographer* choreographer = AChoreographer_getInstance();
    if (choreographer == nullptr) return false;
    g_projection.frame_pending = true;
    AChoreographer_postFrameCallback64(
            choreographer, OnProjectionFrame,
            reinterpret_cast<void*>(g_projection_epoch));
    return true;
}

void FinishProjectionAfterHold(int64_t frame_time_nanos) {
    if (!g_projection.active || !g_projection.handed_off ||
            !g_projection.secondary_up) {
        return;
    }
    if (frame_time_nanos - g_projection.last_move_nanos <
                    kMinimumDropHoldNanos &&
            ScheduleProjectionFrame()) {
        return;
    }
    const bool dispatched = DispatchProjected(
            g_projection.inner, g_projection.lock,
            g_projection.multi_finger_status, 1,
            g_projection.target_x, g_projection.target_y,
            frame_time_nanos / 1000000LL, frame_time_nanos);
    if (dispatched) FileLog("dropped Home drag from second pointer");
    ResetProjection();
}

void OnProjectionFrame(int64_t frame_time_nanos, void* data) {
    const uintptr_t epoch = reinterpret_cast<uintptr_t>(data);
    if (epoch != g_projection_epoch) return;
    g_projection.frame_pending = false;
    if (!g_projection.active || !g_projection.handed_off) return;

    if (!g_projection.gliding) {
        FinishProjectionAfterHold(frame_time_nanos);
        return;
    }

    const int64_t elapsed = frame_time_nanos -
            g_projection.glide_start_nanos;
    float progress = static_cast<float>(elapsed) /
            static_cast<float>(kTeleportDurationNanos);
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    const float remaining = 1.0f - progress;
    const float eased = 1.0f - remaining * remaining * remaining;
    const float x = g_projection.start_x +
            (g_projection.target_x - g_projection.start_x) * eased;
    const float y = g_projection.start_y +
            (g_projection.target_y - g_projection.start_y) * eased;
    if (!DispatchProjected(
                g_projection.inner, g_projection.lock,
                g_projection.multi_finger_status, 2, x, y,
                frame_time_nanos / 1000000LL, frame_time_nanos)) {
        ResetProjection();
        return;
    }

    if (progress < 1.0f && ScheduleProjectionFrame()) return;
    g_projection.gliding = false;
    g_projection.glide_complete = true;
    FileLog("finished smooth Home handoff glide");
    if (g_projection.secondary_up && !ScheduleProjectionFrame()) {
        FinishProjectionAfterHold(frame_time_nanos);
    }
}

void HookDispatch(void* monitor, void* event, void* lock,
                  uint32_t multi_finger_status) {
    if (g_original_dispatch == nullptr || event == nullptr) return;
    const int32_t action = g_motion_get_action(event);
    const size_t count = g_motion_get_pointer_count(event);
    const int64_t event_time = g_motion_get_event_time(event);
    const int64_t now_nanos = MonotonicNanos();

#if SMALLWINDOW_DIAGNOSTIC
    if (action != 2 || g_projection.active) {
        int32_t lock_type = -1;
        int32_t monitor_type = -1;
        if (lock != nullptr && g_gesture_type_offset != 0u) {
            memcpy(&lock_type,
                   static_cast<uint8_t*>(lock) + g_gesture_type_offset,
                   sizeof(lock_type));
        }
        if (monitor != nullptr && g_gesture_type_offset != 0u) {
            memcpy(&monitor_type,
                   static_cast<uint8_t*>(monitor) + g_state_fallback_offset +
                           g_gesture_type_offset,
                   sizeof(monitor_type));
        }
        char trace[224]{};
        snprintf(trace, sizeof(trace),
                 "dispatch action=%d count=%zu lock=%d type_lock=%d "
                 "type_fallback=%d inner=%d active=%d",
                 action, count, lock != nullptr ? 1 : 0, lock_type,
                 monitor_type, g_projection.inner == monitor ? 1 : 0,
                 g_projection.active ? 1 : 0);
        FileLog(trace);
    }
#endif

    if (action == 0) {
        if (g_projection.active) {
            DispatchProjected(
                    g_projection.inner, g_projection.lock,
                    g_projection.multi_finger_status, 1,
                    g_projection.last_x, g_projection.last_y,
                    event_time, now_nanos);
        }
        ResetProjection();
        if (count == 1u) {
            g_projection.inner = monitor;
            g_projection.primary_id = g_motion_get_pointer_id(event, 0u);
            g_projection.down_time = g_motion_get_down_time(event);
            g_projection.last_event_time = event_time;
            g_projection.last_move_nanos = now_nanos;
            g_projection.last_x = g_motion_get_raw_x(event, 0u);
            g_projection.last_y = g_motion_get_raw_y(event, 0u);
        }
        g_original_dispatch(monitor, event, lock, multi_finger_status);
        return;
    }

    if (!g_projection.active && action == 2 && count == 1u &&
            g_projection.inner == monitor) {
        g_projection.last_event_time = event_time;
        g_projection.last_move_nanos = now_nanos;
        g_projection.last_x = g_motion_get_raw_x(event, 0u);
        g_projection.last_y = g_motion_get_raw_y(event, 0u);
    }

    if (!g_projection.active && action == 5 && count == 2u &&
            g_projection.inner == monitor &&
            ReadGestureType(monitor, lock) == 1) {
        const size_t secondary_index = g_motion_get_action_index(event);
        size_t primary_index = 0u;
        if (secondary_index < count &&
                FindPointerIndex(event, g_projection.primary_id,
                                 &primary_index) &&
                primary_index != secondary_index) {
            g_projection.active = true;
            g_projection.secondary_id =
                    g_motion_get_pointer_id(event, secondary_index);
            g_projection.target_x =
                    g_motion_get_raw_x(event, secondary_index);
            g_projection.target_y =
                    g_motion_get_raw_y(event, secondary_index);
            FileLog("captured second pointer for Home handoff");
            if (DispatchProjected(
                        monitor, lock, multi_finger_status, 2,
                        g_motion_get_raw_x(event, primary_index),
                        g_motion_get_raw_y(event, primary_index),
                        event_time, now_nanos)) {
                return;
            }
            ResetProjection();
        }
    }

    if (g_projection.active && g_projection.inner == monitor) {
        if (action == 2) {
            const int32_t followed_id = g_projection.handed_off
                    ? g_projection.secondary_id : g_projection.primary_id;
            size_t followed_index = 0u;
            if (FindPointerIndex(event, followed_id, &followed_index)) {
                const float x = g_motion_get_raw_x(event, followed_index);
                const float y = g_motion_get_raw_y(event, followed_index);
                if (g_projection.gliding) {
                    g_projection.target_x = x;
                    g_projection.target_y = y;
                    return;
                }
                if (DispatchProjected(
                            monitor, lock, multi_finger_status, 2,
                            x, y, event_time, now_nanos)) {
                    return;
                }
            }
        } else if (action == 6) {
            const size_t lifted_index = g_motion_get_action_index(event);
            if (lifted_index < count) {
                const int32_t lifted_id =
                        g_motion_get_pointer_id(event, lifted_index);
                const int32_t remaining_id = lifted_id == g_projection.primary_id
                        ? g_projection.secondary_id : g_projection.primary_id;
                size_t remaining_index = 0u;
                if (FindPointerIndex(event, remaining_id, &remaining_index)) {
                    const float x = g_motion_get_raw_x(event, remaining_index);
                    const float y = g_motion_get_raw_y(event, remaining_index);
                    if (lifted_id == g_projection.primary_id) {
                        g_projection.handed_off = true;
                        g_projection.gliding = true;
                        g_projection.glide_complete = false;
                        g_projection.secondary_up = false;
                        g_projection.inner = monitor;
                        g_projection.lock = lock;
                        g_projection.multi_finger_status =
                                multi_finger_status;
                        g_projection.start_x =
                                g_motion_get_raw_x(event, lifted_index);
                        g_projection.start_y =
                                g_motion_get_raw_y(event, lifted_index);
                        g_projection.target_x = x;
                        g_projection.target_y = y;
                        g_projection.glide_start_nanos = now_nanos;
                        FileLog("started smooth Home handoff glide");
                        if (!ScheduleProjectionFrame()) {
                            g_projection.gliding = false;
                            g_projection.glide_complete = true;
                            DispatchProjected(
                                    monitor, lock, multi_finger_status, 2,
                                    x, y, event_time, now_nanos);
                        }
                        return;
                    }
                    const bool dispatched = DispatchProjected(
                            monitor, lock, multi_finger_status, 2,
                            x, y, event_time, now_nanos);
                    ResetProjection();
                    if (dispatched) return;
                }
            }
        } else if (action == 1 && g_projection.handed_off) {
            size_t secondary_index = 0u;
            if (FindPointerIndex(event, g_projection.secondary_id,
                                 &secondary_index)) {
                g_projection.target_x =
                        g_motion_get_raw_x(event, secondary_index);
                g_projection.target_y =
                        g_motion_get_raw_y(event, secondary_index);
            }
            g_projection.secondary_up = true;
            g_projection.inner = monitor;
            g_projection.lock = lock;
            g_projection.multi_finger_status = multi_finger_status;
            if (!g_projection.gliding && g_projection.glide_complete &&
                    now_nanos - g_projection.last_move_nanos >=
                            kMinimumDropHoldNanos) {
                const bool dispatched = DispatchProjected(
                        monitor, lock, multi_finger_status, 1,
                        g_projection.target_x, g_projection.target_y,
                        event_time, now_nanos);
                ResetProjection();
                if (dispatched) {
                    FileLog("dropped Home drag from second pointer");
                    return;
                }
            }
            if (!ScheduleProjectionFrame()) {
                DispatchProjected(
                        monitor, lock, multi_finger_status, 1,
                        g_projection.target_x, g_projection.target_y,
                        event_time, now_nanos);
                FileLog("dropped Home drag without frame scheduler");
                ResetProjection();
            }
            return;
        } else if (action == 3) {
            const bool dispatched = DispatchProjected(
                    monitor, lock, multi_finger_status, 3,
                    g_projection.last_x, g_projection.last_y,
                    event_time, now_nanos);
            ResetProjection();
            if (dispatched) return;
        }
    }

    if (action == 1 || action == 3) ResetProjection();
    g_original_dispatch(monitor, event, lock, multi_finger_status);
}

bool InstallLauncherHook(const Image& launcher, void* handle) {
    if (__atomic_load_n(&g_launcher_state, __ATOMIC_ACQUIRE) == 2u) {
        return true;
    }
    uint32_t expected = 0u;
    if (!__atomic_compare_exchange_n(&g_launcher_state, &expected,
                                     uint32_t{1}, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return expected == 2u;
    }
    GestureStructure structure{};
    if (!LocateGestureStructure(launcher, &structure) ||
            !ResolveMotionApi(handle) || g_native_hook == nullptr ||
            !AddProtectedPage(structure.dispatch) || !EnsureMadviseGuard()) {
        __atomic_store_n(&g_launcher_state, uint32_t{3}, __ATOMIC_RELEASE);
        Log(ANDROID_LOG_WARN,
            "MiuiHome gesture structure missing or ambiguous; no hook");
        return false;
    }
    void* original = nullptr;
    g_gesture_type_offset = structure.gesture_type_offset;
    g_state_fallback_offset = structure.state_fallback_offset != 0u
            ? structure.state_fallback_offset
            : 0x20u;
    g_native_hook(reinterpret_cast<void*>(structure.dispatch),
                  reinterpret_cast<void*>(HookDispatch), &original);
    if (original == nullptr) {
        g_gesture_type_offset = 0u;
        g_state_fallback_offset = 0x20u;
        __atomic_store_n(&g_launcher_state, uint32_t{3}, __ATOMIC_RELEASE);
        Log(ANDROID_LOG_ERROR, "failed to install launcher dispatch hook");
        return false;
    }
    g_original_dispatch = reinterpret_cast<DispatchFn>(original);
    __atomic_store_n(&g_launcher_state, uint32_t{2}, __ATOMIC_RELEASE);
    Log(ANDROID_LOG_INFO, "MiuiHome gesture structure matched; hook ready");
    return true;
}

void TryInstallLauncherHook(const char* hint, void* handle) {
    if (handle == nullptr) return;
    Image launcher{};
    if ((hint == nullptr || !FindImage(hint, 0u, &launcher)) &&
            !FindImage(kLauncherName, 0u, &launcher)) {
        return;
    }
    InstallLauncherHook(launcher, handle);
}

void OnLibraryLoaded(const char* name, void* handle) {
    if (name == nullptr || !IsLauncherHyosProcess()) return;
    if (EndsWith(name, kHyperRuntimeName)) {
        EnsureMadviseGuard();
    }
    if (IsLauncherLibraryPath(name)) {
        TryInstallLauncherHook(name, handle);
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"), unused))
NativeOnLibraryLoaded native_init(const NativeApiEntries* entries) {
    if (entries == nullptr || entries->hook_func == nullptr ||
            entries->unhook_func == nullptr || !IsLauncherHyosProcess()) {
        return nullptr;
    }
    g_native_hook = entries->hook_func;
    Log(ANDROID_LOG_INFO, "native entry initialized in MiuiHome HYOS child");
    return OnLibraryLoaded;
}
