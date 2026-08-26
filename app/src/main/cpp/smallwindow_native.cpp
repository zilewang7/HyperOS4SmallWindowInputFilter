#include <android/log.h>
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

// MiuiHome RELEASE-8.01.02.5361-260807-08161657-R.
constexpr uintptr_t kEntryOffset = 0xc94b80u;
constexpr uint8_t kEntryFingerprint[] = {
        0xff, 0xc3, 0x04, 0xd1, 0xfd, 0x7b, 0x0d, 0xa9,
        0xfc, 0x6f, 0x0e, 0xa9, 0xfa, 0x67, 0x0f, 0xa9,
        0xf8, 0x5f, 0x10, 0xa9, 0xf6, 0x57, 0x11, 0xa9,
        0xf4, 0x4f, 0x12, 0xa9, 0xfd, 0x43, 0x03, 0x91,
        0xf9, 0x03, 0x08, 0xaa, 0xe6, 0x17, 0xeb, 0x97,
        0xc0, 0x00, 0x00, 0x36, 0x86, 0x18, 0xeb, 0x97,
};

// Context around dispatch_to_inner_monitor's ACTION_POINTER_DOWN branch.
constexpr uintptr_t kCancelContextOffset = 0x9086f0u;
constexpr uint8_t kCancelContextFingerprint[] = {
        0x88, 0x66, 0x41, 0xb9, 0x09, 0x01, 0x38, 0x1e,
        0x0a, 0x05, 0x00, 0x51, 0x40, 0x01, 0x23, 0x1e,
        0x3f, 0x01, 0x08, 0x6b, 0x04, 0xa9, 0x40, 0x7a,
        0x08, 0xcc, 0x28, 0x1e, 0x88, 0x72, 0x40, 0xb9,
        0x1f, 0x05, 0x00, 0x71, 0x21, 0x10, 0x00, 0x54,
        0x80, 0x76, 0x40, 0xf9, 0xa0, 0x1b, 0x00, 0xb4,
        0x68, 0xf1, 0x24, 0x94, 0x61, 0x00, 0x80, 0x52,
        0xf9, 0x03, 0x00, 0xaa, 0x69, 0xf1, 0x24, 0x94,
};

// The 5361 inner monitor reads ACTION_POINTER_DOWN at this one call site
// before synthesizing CANCEL and terminating the Home recognizer. Returning
// MOVE only to that local decision keeps the real event unchanged for the
// downstream launcher/Flutter recents path.
constexpr uintptr_t kCancelDecisionReturnOffset = 0x9084bcu;

struct NativeApiEntries {
    uint32_t version;
    int (*hook_func)(void* target, void* replacement, void** backup);
    int (*unhook_func)(void* target);
};

using NativeOnLibraryLoaded = void (*)(const char* name, void* handle);
using MotionActionFn = int32_t (*)(void* event);
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

struct HookPlan {
    void** slots[kMaxSlots];
    size_t slot_count;
    void* original;
};

uintptr_t g_launcher_base = 0u;
MotionActionFn g_original_action_masked = nullptr;
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

bool PreparePltHook(const Image& image, const char* symbol,
                    void* replacement, HookPlan* output) {
    if (symbol == nullptr || replacement == nullptr || output == nullptr) {
        return false;
    }
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

bool ValidateLauncherProfile(const Image& launcher) {
    return IsLauncherLibraryPath(launcher.path) &&
            RangeInImage(launcher, launcher.base + kEntryOffset,
                         sizeof(kEntryFingerprint), PF_R | PF_X) &&
            RangeInImage(launcher, launcher.base + kCancelContextOffset,
                         sizeof(kCancelContextFingerprint), PF_R | PF_X) &&
            memcmp(reinterpret_cast<void*>(launcher.base + kEntryOffset),
                   kEntryFingerprint, sizeof(kEntryFingerprint)) == 0 &&
            memcmp(reinterpret_cast<void*>(launcher.base + kCancelContextOffset),
                   kCancelContextFingerprint,
                   sizeof(kCancelContextFingerprint)) == 0;
}

int32_t HookActionMasked(void* event) {
    MotionActionFn original = __atomic_load_n(&g_original_action_masked,
                                              __ATOMIC_ACQUIRE);
    if (original == nullptr) return -1;
    const int32_t action = original(event);
    const uintptr_t base = __atomic_load_n(&g_launcher_base,
                                           __ATOMIC_ACQUIRE);
    void* caller_ptr = __builtin_extract_return_addr(
            __builtin_return_address(0));
    const uintptr_t caller = reinterpret_cast<uintptr_t>(caller_ptr);
    if (action == 5 && base != 0u && caller >= base &&
            caller - base == kCancelDecisionReturnOffset) {
        FileLog("kept second pointer on Home recognizer path");
        return 2;
    }
    return action;
}

bool InstallLauncherHook(const Image& launcher) {
    if (!ValidateLauncherProfile(launcher)) {
        Log(ANDROID_LOG_WARN, "unsupported MiuiHome native profile; no hook");
        return false;
    }
    HookPlan action_plan{};
    if (!PreparePltHook(launcher, "input_MotionEvent_getActionMasked",
                        reinterpret_cast<void*>(HookActionMasked),
                        &action_plan)) {
        Log(ANDROID_LOG_ERROR, "failed to resolve launcher MotionEvent hook");
        return false;
    }
    for (size_t i = 0u; i < action_plan.slot_count; ++i) {
        if (!AddProtectedPage(
                    reinterpret_cast<uintptr_t>(action_plan.slots[i]))) {
            return false;
        }
    }
    if (!EnsureMadviseGuard()) {
        Log(ANDROID_LOG_ERROR, "launcher hook rejected without page guard");
        return false;
    }
    if (action_plan.original != nullptr) {
        __atomic_store_n(&g_original_action_masked,
                         reinterpret_cast<MotionActionFn>(action_plan.original),
                         __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&g_original_action_masked, __ATOMIC_ACQUIRE) ==
                    nullptr ||
            !CommitPltHook(action_plan,
                           reinterpret_cast<void*>(HookActionMasked))) {
        Log(ANDROID_LOG_ERROR, "failed to install launcher runtime hook");
        return false;
    }
    __atomic_store_n(&g_launcher_base, launcher.base, __ATOMIC_RELEASE);
    __atomic_store_n(&g_launcher_state, uint32_t{2}, __ATOMIC_RELEASE);
    Log(ANDROID_LOG_INFO, "MiuiHome 5361 HYOS runtime hook ready");
    return true;
}

void TryInstallLauncherHook(const char* hint) {
    Image launcher{};
    if ((hint == nullptr || !FindImage(hint, 0u, &launcher)) &&
            !FindImage(kLauncherName, 0u, &launcher)) {
        return;
    }
    InstallLauncherHook(launcher);
}

void OnLibraryLoaded(const char* name, void*) {
    if (name == nullptr || !IsLauncherHyosProcess()) return;
    if (EndsWith(name, kHyperRuntimeName)) {
        EnsureMadviseGuard();
        TryInstallLauncherHook(nullptr);
    }
    if (IsLauncherLibraryPath(name)) {
        TryInstallLauncherHook(name);
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"), unused))
NativeOnLibraryLoaded native_init(const NativeApiEntries* entries) {
    if (entries == nullptr || entries->hook_func == nullptr ||
            entries->unhook_func == nullptr || !IsLauncherHyosProcess()) {
        return nullptr;
    }
    Log(ANDROID_LOG_INFO, "native entry initialized in MiuiHome HYOS child");
    return OnLibraryLoaded;
}
