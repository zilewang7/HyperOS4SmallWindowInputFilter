# HyperOS4 SmallWindow Input Filter

[![build](https://github.com/zilewang7/HyperOS4SmallWindowInputFilter/actions/workflows/build.yml/badge.svg)](https://github.com/zilewang7/HyperOS4SmallWindowInputFilter/actions/workflows/build.yml)

An [LSPosed](https://github.com/LSPosed/LSPosed) module that restores the old HyperOS two-finger small-window gesture on the Xiaomi 17 Pro Max running HyperOS 4 (Android 17).

English | [中文](#中文)

## What it does

On older HyperOS/MIUI versions, while an app is foreground you could:

1. Swipe up from the bottom edge with your thumb and keep holding (enters recents drag, the window follows your thumb).
2. Press anywhere with your index finger (nothing visible happens yet).
3. Release your thumb — the window is dragged/teleported to the index-finger position.
4. Release your index finger — the window drops into small-window mode there.

On HyperOS 4 this was broken: the second finger cancels the recents drag and the window snaps back to the foreground app. This module restores the launcher-native two-finger route.

## How it works

- HyperOS 4 MiuiHome (`com.miui.home`) is a native HYOS application spawned by `/system_ext/bin/hyos_spawner`; it does not run on ART.
- Zygisk Next 1.5.0+ exposes the HYOS application-specialization lifecycle. LSPosed IT 2.1.1+ uses it to load this APK's `META-INF/xposed/native_init.list` entry into the scoped MiuiHome process.
- The arm64 payload validates the exact process, launcher image, entry point, and cancellation-path fingerprints before installing anything.
- MiuiHome 5361 locally interprets the second finger as a move only at the decision that would otherwise cancel the Home drag. The real multi-pointer event remains unchanged for the downstream launcher/Flutter recents path, allowing the launcher to perform its original handoff and drop behavior.
- HYOS may remap APK-backed runtime pages. The module protects its PLT hook pages from runtime cleanup and can reapply the caller-local hook without pinning a `hyos_spawner` Build ID.
- Unsupported or ambiguous launcher layouts fail closed and preserve Xiaomi's original behavior.

This runtime architecture follows the current native integration in [MiuiBackGestureHook](https://github.com/wxxsfxyzm/MiuiBackGestureHook).

## Compatibility

Validated target profile:

- Xiaomi 17 Pro Max (`2509FPN0BC`)
- HyperOS `OS4.0.0.20.XPBCNXM`, Android 17 (SDK 37)
- MiuiHome `801025361` / `RELEASE-8.01.02.5361-260807-08161657-R`
- arm64-v8a

Other official HyperOS 4 launcher builds require a separately verified native profile. Modified official ROMs and ported/custom ROMs are not supported; report their runtime or gesture differences to the ROM maintainer.

## Requirements

- Official HyperOS 4 / Android 17 firmware
- Zygisk Next 1.5.0 or newer
- LSPosed IT 2.1.1 or newer with HYOS-spawner support (libxposed API 102)
- Root (Magisk / APatch / KernelSU) with LSPosed working

## Build

Requirements:

- JDK 17
- Android SDK Platform 37 (`platforms;android-37.0`) and Build Tools `37.0.0`
- Android NDK `30.0.14904198` and CMake `3.22.1`

```bash
./gradlew :app:assembleDebug --no-daemon        # debug
./gradlew :app:assembleRelease --no-daemon      # release (signed when KEYSTORE_* env vars are set)
./gradlew :app:assembleDiag --no-daemon         # diagnostics (native file logging enabled)
```

Artifacts follow the `io.github.zilewang7.smallwindow-<versionName>[_debug[_diag]].apk` naming convention, e.g. `io.github.zilewang7.smallwindow-1.2.0.apk` (release), `io.github.zilewang7.smallwindow-1.2.0_debug.apk` (debug) and `io.github.zilewang7.smallwindow-1.2.0_debug_diag.apk` (diagnostics). The release build has no type suffix, so it sorts first in GitHub's alphabetical asset listing.

GitHub Actions builds the debug artifact on every push, and pushing a `v*` tag creates a GitHub Release carrying the signed release APK (`io.github.zilewang7.smallwindow-*.apk`) and the diagnostics APK (`io.github.zilewang7.smallwindow-*_debug_diag.apk`) (see `.github/workflows/build.yml` and `.github/workflows/release.yml`). A published release is also automatically synced to the official module repository repo `Xposed-Modules-Repo/io.github.zilewang7.smallwindow` with tag `VersionCode-VersionName` (see `.github/workflows/sync-store.yml`).

## Install

1. Build the APK (or download it from GitHub Actions / Releases).
2. Install it on the phone:
   ```bash
   adb push app-debug.apk /data/local/tmp/smallwindow.apk
   adb shell 'su -c "pm install -r /data/local/tmp/smallwindow.apk"'
   ```
3. Open the LSPosed manager, enable the module and make sure the scope includes `com.miui.home`.
4. Reboot the phone.

The native entry is loaded when a fresh MiuiHome HYOS child starts. A full reboot is the normal activation path.

## Debugging

Watch the native runtime log:

```bash
adb logcat -s SmallWindowNative:I
```

Without a PC: install the diagnostics build (`io.github.zilewang7.smallwindow-*_debug_diag.apk`, attached to every release), reproduce the gesture, and share `/data/user_de/0/com.miui.home/cache/smallwindow_native.log` (root-readable; the credential-encrypted fallback is `/data/user/0/com.miui.home/cache/smallwindow_native.log`).

Key log lines:

- `native entry initialized in MiuiHome HYOS child` — LSPosed loaded the APK native entry.
- `installed HYOS hook-page guard` — runtime page cleanup protection is active.
- `MiuiHome 5361 HYOS runtime hook ready` — the exact launcher profile matched and the hook is active.
- `kept second pointer on Home recognizer path` — the cancellation decision was bypassed for the gesture.
- `unsupported MiuiHome native profile; no hook` — the launcher build did not match and was left unchanged.

## Native profiles

Native offsets and immutable fingerprints live in `smallwindow_native.cpp`. A new official MiuiHome build must be analyzed and validated separately. Do not copy offsets from a nearby version or relax profile matching for modified/ported ROMs.

## Project layout

```
.
├── .github/workflows/             # Build, release and store sync
├── app/
│   ├── build.gradle               # Android/NDK variants and artifact names
│   └── src/main/
│       ├── cpp/
│       │   ├── CMakeLists.txt
│       │   └── smallwindow_native.cpp # MiuiHome HYOS native entry
│       ├── java/io.github.zilewang7.smallwindow/
│       │   └── MainHook.java      # libxposed Java metadata entry
│       └── resources/META-INF/xposed/
│           ├── native_init.list
│           ├── module.prop
│           └── scope.list         # com.miui.home
├── build.gradle
├── settings.gradle
└── gradle/wrapper/
```

The native payload exports only `native_init` and is packaged only for `arm64-v8a` with 16 KB ELF segment alignment.

## License

Project code is licensed under [MIT](LICENSE). Adapted native integration code
and notices are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## 中文

# HyperOS4 SmallWindow Input Filter（HyperOS 4 小窗输入过滤器）

这是一个 [LSPosed](https://github.com/LSPosed/LSPosed) 模块，用于在搭载 HyperOS 4（Android 17）的小米 17 Pro Max 上恢复旧版 HyperOS 的双指小窗手势。

[English](#hyperos4-smallwindow-input-filter) | 中文

## 功能

在旧版 HyperOS / MIUI 上，前台应用可以通过以下方式挂小窗：

1. 拇指从屏幕底部上滑进入多任务并按住不放；
2. 食指按住屏幕上任意位置（此时画面不发生变化）；
3. 松开拇指——窗口被“拖拽”到食指位置；
4. 松开食指——窗口在该位置挂成小窗。

HyperOS 4 上这个手势被破坏：第二根手指按下会取消多任务拖拽，窗口弹回前台应用。本模块恢复桌面原生的双指处理路径。

## 原理

- HyperOS 4 桌面（`com.miui.home`）是由 `/system_ext/bin/hyos_spawner` 孵化的原生 HYOS 应用，不运行在 ART 上。
- Zygisk Next 1.5.0+ 提供 HYOS 应用 specialize 生命周期；LSPosed IT 2.1.1+ 利用该能力，按 `com.miui.home` 作用域将本 APK 的 `native_init.list` 入口加载到桌面进程。
- arm64 payload 在执行任何修改前会校验进程身份、桌面镜像、入口地址和取消路径指纹。
- MiuiHome 5361 在一个局部判定中把第二指解释为 `MOVE`，不再触发取消；真实多指事件不变，仍交给后续 Launcher / Flutter 多任务路径完成原有的换指、移动和挂小窗行为。
- HYOS 可能重新映射 APK 内的运行时代码页。模块会保护 PLT hook 页，并在不固定 `hyos_spawner` Build ID 的前提下适配运行时重映射。
- 桌面布局不受支持或匹配不唯一时，模块会失败关闭，完全保留小米原行为。

Native runtime 架构参考 [MiuiBackGestureHook](https://github.com/wxxsfxyzm/MiuiBackGestureHook) 的最新实现。

## 兼容性

已验证目标 profile：

- 小米 17 Pro Max（`2509FPN0BC`）
- HyperOS `OS4.0.0.20.XPBCNXM`，Android 17（SDK 37）
- MiuiHome `801025361` / `RELEASE-8.01.02.5361-260807-08161657-R`
- arm64-v8a

其他官方 HyperOS 4 桌面版本需要单独分析并验证 native profile。**不支持官改包、移植包及其他第三方修改系统；这类系统的问题请向对应系统维护者反馈。**

## 要求

- 官方 HyperOS 4 / Android 17 固件
- Zygisk Next 1.5.0 或更高
- LSPosed IT 2.1.1 或更高，且支持 HYOS spawner（libxposed API 102）
- Root（Magisk / APatch / KernelSU）且 LSPosed 正常工作

## 构建

依赖：

- JDK 17
- Android SDK Platform 37（`platforms;android-37.0`）与 Build Tools `37.0.0`
- Android NDK `30.0.14904198` 与 CMake `3.22.1`

```bash
./gradlew :app:assembleDebug --no-daemon        # debug
./gradlew :app:assembleRelease --no-daemon      # release（设置了 KEYSTORE_* 环境变量时使用正式签名）
./gradlew :app:assembleDiag --no-daemon         # 诊断版（开启 native 文件日志）
```

产物命名遵循 `io.github.zilewang7.smallwindow-<versionName>[_debug[_diag]].apk`，例如 `io.github.zilewang7.smallwindow-1.2.0.apk`（正式版）、`io.github.zilewang7.smallwindow-1.2.0_debug.apk`（debug）和 `io.github.zilewang7.smallwindow-1.2.0_debug_diag.apk`（诊断版）。正式版不带类型后缀，在 GitHub 的字母序附件列表中会排在最前。

GitHub Actions 会在每次 push 时构建 debug 产物；推送 `v*` 标签时，会自动创建同时附带签名正式版 APK（`io.github.zilewang7.smallwindow-*.apk`）与诊断版 APK（`io.github.zilewang7.smallwindow-*_debug_diag.apk`）的 GitHub Release（见 `.github/workflows/build.yml` 和 `.github/workflows/release.yml`）。发布后还会自动同步到官方模块仓库 `Xposed-Modules-Repo/io.github.zilewang7.smallwindow`，使用 `VersionCode-VersionName` 格式的 tag（见 `.github/workflows/sync-store.yml`）。

## 安装

1. 构建 APK（或从 GitHub Actions / Releases 下载）；
2. 安装到手机：
   ```bash
   adb push app-debug.apk /data/local/tmp/smallwindow.apk
   adb shell 'su -c "pm install -r /data/local/tmp/smallwindow.apk"'
   ```
3. 打开 LSPosed 管理器，启用模块，并确认作用域包含 `com.miui.home`；
4. 重启手机。

新的 MiuiHome HYOS 子进程启动时才会加载 native entry，正常生效方式是完整重启。

## 调试

查看 native 运行日志：

```bash
adb logcat -s SmallWindowNative:I
```

不用电脑时：安装诊断版（`io.github.zilewang7.smallwindow-*_debug_diag.apk`，每个 Release 都附带），复现后分享 `/data/user_de/0/com.miui.home/cache/smallwindow_native.log`（root 可读；凭据加密存储的备用路径是 `/data/user/0/com.miui.home/cache/smallwindow_native.log`）。

关键日志：

- `native entry initialized in MiuiHome HYOS child` —— LSPosed 已加载 APK native entry；
- `installed HYOS hook-page guard` —— 运行时代码页清理保护已启用；
- `MiuiHome 5361 HYOS runtime hook ready` —— 桌面 profile 精确匹配且 hook 已生效；
- `kept second pointer on Home recognizer path` —— 已绕过本次第二指取消判定；
- `unsupported MiuiHome native profile; no hook` —— 桌面版本不匹配，未修改系统行为。

## Native profile

Native 偏移和不可变指纹位于 `smallwindow_native.cpp`。新的官方 MiuiHome 版本必须单独分析验证；不要复制相近版本偏移，也不要为了兼容官改或移植包而放宽匹配。

## 项目结构

```
.
├── .github/workflows/             # 构建、发布与商店同步
├── app/
│   ├── build.gradle               # Android/NDK 变体与产物命名
│   └── src/main/
│       ├── cpp/
│       │   ├── CMakeLists.txt
│       │   └── smallwindow_native.cpp # MiuiHome HYOS native entry
│       ├── java/io.github.zilewang7.smallwindow/
│       │   └── MainHook.java      # libxposed Java 元数据入口
│       └── resources/META-INF/xposed/
│           ├── native_init.list
│           ├── module.prop
│           └── scope.list         # com.miui.home
├── build.gradle
├── settings.gradle
└── gradle/wrapper/
```

Native payload 仅导出 `native_init`，只打包 `arm64-v8a`，ELF 段按 16 KB 对齐。

## 许可证

项目代码采用 [MIT](LICENSE) 许可证。改编的 native 集成代码及其声明见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
