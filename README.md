# HyperOS4 SmallWindow Input Filter

[![build](https://github.com/zilewang7/HyperOS4SmallWindowInputFilter/actions/workflows/build.yml/badge.svg)](https://github.com/zilewang7/HyperOS4SmallWindowInputFilter/actions/workflows/build.yml)

An [LSPosed](https://github.com/LSPosed/LSPosed) module that restores the old HyperOS two-finger small-window gesture on the Xiaomi 17 Pro Max running HyperOS 4 (Android 17).

English | [中文](#中文)

## What it does

On older HyperOS/MIUI versions, while an app is foreground you could:

1. Swipe up from the bottom edge with your thumb and keep holding (enters recents drag, the window follows your thumb).
2. Press anywhere with your index finger (nothing visible happens yet).
3. Release your thumb — the window glides to the index-finger position over 180 ms.
4. Move the index finger if needed, then release it to drop the window into small-window mode there.

On HyperOS 4 this was broken: the second finger cancels the recents drag and the window snaps back to the foreground app. This module restores the launcher-native two-finger route.

## How it works

- HyperOS 4 MiuiHome (`com.miui.home`) is a native HYOS application spawned by `/system_ext/bin/hyos_spawner`; it does not run on ART.
- Zygisk Next 1.5.0+ exposes the HYOS application-specialization lifecycle. LSPosed IT 2.1.1+ uses it to load this APK's `META-INF/xposed/native_init.list` entry into the scoped MiuiHome process.
- The arm64 payload validates the process and launcher image, resolves imported MotionEvent APIs, and scans AArch64 executable code for one unambiguous gesture-dispatch structure before installing anything.
- Inside that matched launcher boundary, physical two-finger input is projected into one continuous drag stream. The stream keeps the thumb pointer ID and down time, follows the thumb until release, then uses Choreographer frames and cubic ease-out to glide for 180 ms toward the live index-finger target.
- Releasing the index finger early waits for the glide to finish. The final `UP` is emitted only after the target has remained stable for at least one 16 ms frame; cancellation and new-gesture paths invalidate pending frame callbacks and end the active synthetic stream.
- HYOS may discard APK-backed runtime pages. The module guards the hooked pages from `MADV_DONTNEED` without pinning a `hyos_spawner` Build ID or MiuiHome version.
- Missing or ambiguous launcher structures fail closed and preserve Xiaomi's original behavior.

This runtime architecture follows the current native integration in [MiuiBackGestureHook](https://github.com/wxxsfxyzm/MiuiBackGestureHook).

## Compatibility

Validated official configurations:

- Xiaomi 17 Pro Max (`2509FPN0BC`), Android 17 (SDK 37), arm64-v8a
- HyperOS `OS4.0.0.20.XPBCNXM` with MiuiHome `801025361` / `RELEASE-8.01.02.5361-260807-08161657-R`
- HyperOS `OS4.0.0.25.XPBCNXM` with MiuiHome `801025459` / `RELEASE-8.01.02.5459-260807-08242024-R`

Official HyperOS 4 launcher updates are matched by validated native behavior structure rather than a version whitelist. Modified official ROMs and ported/custom ROMs are outside this project's support and issue-triage boundary; report their runtime or gesture differences to the ROM maintainer.

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
- `MiuiHome gesture structure matched; hook ready` — the unique launcher behavior structure matched and the hook is active.
- `captured second pointer for Home handoff` — the internal single-pointer projection started.
- `started smooth Home handoff glide` / `finished smooth Home handoff glide` — the frame-driven 180 ms handoff ran to completion.
- `dropped Home drag from second pointer` — the final stable-frame `UP` was delivered.
- `MiuiHome gesture structure missing or ambiguous; no hook` — structural validation failed and the launcher was left unchanged.

## Compatibility matching

The runtime does not contain a MiuiHome version whitelist or per-version fixed offsets. It resolves required imported MotionEvent entry points and validates the surrounding AArch64 control-flow and data-access structure. Exactly one complete candidate is required; missing or ambiguous matches fail closed. Official launcher updates can therefore continue to work when the validated behavior structure is preserved, while changed structures require analysis before the matcher is updated.

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
3. 松开拇指——窗口在 180 ms 内平滑移动到食指位置；
4. 可继续移动食指调整位置，松开食指后窗口在该位置挂成小窗。

HyperOS 4 上这个手势被破坏：第二根手指按下会取消多任务拖拽，窗口弹回前台应用。本模块恢复桌面原生的双指处理路径。

## 原理

- HyperOS 4 桌面（`com.miui.home`）是由 `/system_ext/bin/hyos_spawner` 孵化的原生 HYOS 应用，不运行在 ART 上。
- Zygisk Next 1.5.0+ 提供 HYOS 应用 specialize 生命周期；LSPosed IT 2.1.1+ 利用该能力，按 `com.miui.home` 作用域将本 APK 的 `native_init.list` 入口加载到桌面进程。
- arm64 payload 在执行任何修改前会校验进程与桌面镜像，解析导入的 MotionEvent API，并扫描 AArch64 可执行代码，要求手势分发结构唯一且完整匹配。
- 在匹配到的桌面内部边界中，物理双指输入会投影成连续的单指拖拽流，保留拇指的 pointer ID 与 down time。拇指松开后，模块通过 Choreographer 逐帧执行 180 ms 三次缓出动画，并实时跟随食指目标位置。
- 食指提前松开时会等待动画完成；目标位置至少稳定一个 16 ms 帧后才发送最终 `UP`。取消或新手势会使待执行的帧回调失效并结束当前合成输入流。
- HYOS 可能丢弃 APK 映射的运行时代码页。模块会保护 hook 页不受 `MADV_DONTNEED` 影响，且不固定 `hyos_spawner` Build ID 或 MiuiHome 版本。
- 桌面结构缺失或匹配不唯一时，模块会失败关闭，完全保留小米原行为。

Native runtime 架构参考 [MiuiBackGestureHook](https://github.com/wxxsfxyzm/MiuiBackGestureHook) 的最新实现。

## 兼容性

已验证的官方配置：

- 小米 17 Pro Max（`2509FPN0BC`），Android 17（SDK 37），arm64-v8a
- HyperOS `OS4.0.0.20.XPBCNXM`，MiuiHome `801025361` / `RELEASE-8.01.02.5361-260807-08161657-R`
- HyperOS `OS4.0.0.25.XPBCNXM`，MiuiHome `801025459` / `RELEASE-8.01.02.5459-260807-08242024-R`

官方 HyperOS 4 桌面更新通过已验证的 native 行为结构匹配，不使用版本白名单。**官改包、移植包及其他第三方修改系统不在本项目的支持和问题受理范围内；这类系统的问题请向对应系统维护者反馈。**

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
- `MiuiHome gesture structure matched; hook ready` —— 唯一的桌面行为结构匹配成功，hook 已生效；
- `captured second pointer for Home handoff` —— 内部单指投影已开始；
- `started smooth Home handoff glide` / `finished smooth Home handoff glide` —— 逐帧 180 ms 换指动画已完整执行；
- `dropped Home drag from second pointer` —— 稳定帧后的最终 `UP` 已发送；
- `MiuiHome gesture structure missing or ambiguous; no hook` —— 结构校验失败，未修改桌面行为。

## 兼容性匹配

运行时不包含 MiuiHome 版本白名单或按版本固定的偏移。模块解析所需的 MotionEvent 导入入口，并验证周围的 AArch64 控制流和数据访问结构；只有完整候选恰好唯一时才安装 hook，缺失或歧义都会失败关闭。因此，只要已验证的行为结构保持不变，官方桌面更新可以继续兼容；结构发生变化时则需要先完成分析再更新匹配器。

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
