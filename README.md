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

On HyperOS 4 this was broken: the second finger cancels the recents drag and the window snaps back to the foreground app. This module restores the old behavior by filtering input events inside `system_server`.

## How it works

- The HyperOS 4 launcher (`com.miui.home`) is a native binary (`hyos_spawner` / `libapp_launcher.so`) with no Java code, so it cannot be hooked directly with LSPosed.
- The module hooks `com.android.server.input.InputManagerService` in `system_server` and installs an `android.view.InputFilter`.
- The filter watches touch streams that start as a single-finger `ACTION_DOWN` in the bottom gesture zone (height × 0.82, or `height - 350px`, whichever is larger).
- For such streams:
  - The second finger's `ACTION_POINTER_DOWN` is swallowed and only its coordinates are recorded, so the launcher never sees a multi-touch stream and never cancels the drag.
  - While both fingers are down, only the thumb's movements are forwarded (as a single pointer).
  - When the thumb lifts, the filter synthesizes a smooth ~180 ms glide of the same pointer from the thumb position to the index-finger position.
  - When the index finger lifts, the filter sends the final `ACTION_UP` at the target position, so the launcher drops the window into small-window mode.
- The glide and the one-frame (16 ms) hold before the drop keep the perceived velocity low, preventing the gesture from being classified as a swipe-up-to-home.

## Compatibility

Tested on:

- Xiaomi 17 Pro Max (`2509FPN0BC`)
- HyperOS `OS4.0.0.20.XPBCNXM`, Android 17 (SDK 37)
- LSPosed IT (API 102) with Zygisk Next on APatch

The module only injects into `system_server` and `com.android.systemui`. It may work on other HyperOS 4 devices with the same native-launcher behavior, but it is not guaranteed.

## Requirements

- Android 16+ (the APK targets SDK 36+)
- LSPosed with libxposed API 102 or newer
- Root (Magisk / APatch / KernelSU) with LSPosed working

## Build

Requirements:

- JDK 17
- Android SDK Platform 37 (`platforms;android-37`) and Build Tools `37.0.0`

```bash
./gradlew :app:assembleDebug --no-daemon        # debug
./gradlew :app:assembleRelease --no-daemon      # release (signed when KEYSTORE_* env vars are set)
./gradlew :app:assembleDiag --no-daemon         # diagnostics (file logging enabled)
```

Artifacts follow the `smallwindow-<versionName>[_debug[_diag]].apk` naming convention, e.g. `smallwindow-1.1.0.apk` (release), `smallwindow-1.1.0_debug.apk` (debug) and `smallwindow-1.1.0_debug_diag.apk` (diagnostics). Release assets sort first because GitHub lists assets alphabetically.

GitHub Actions builds the debug artifact on every push, and pushing a `v*` tag creates a GitHub Release carrying the signed release APK (`smallwindow-*.apk`) and the diagnostics APK (`smallwindow-*_debug_diag.apk`) (see `.github/workflows/build.yml` and `.github/workflows/release.yml`). A published release is also automatically synced to the official module repository repo `Xposed-Modules-Repo/io.github.zilewang7.smallwindow` with tag `VersionCode-VersionName` (see `.github/workflows/sync-store.yml`).

## Install

1. Build the APK (or download it from GitHub Actions / Releases).
2. Install it on the phone:
   ```bash
   adb push app-debug.apk /data/local/tmp/smallwindow.apk
   adb shell 'su -c "pm install -r /data/local/tmp/smallwindow.apk"'
   ```
3. Open the LSPosed manager, enable the module and make sure the scope includes `system` (and `com.android.systemui`).
4. Reboot the phone.

The hook lives in `system_server`, so the module only takes effect after a reboot.

## Debugging

Watch the filter's decisions:

```bash
adb logcat -s SmallWindowInputFilter:I
```

Without a PC: install the diagnostics build (`smallwindow-*_debug_diag.apk`, attached to every release), reproduce the gesture, and share `/data/system/smallwindow_filter.log` (root-readable; export with a root file manager or `su -c 'cp /data/system/smallwindow_filter.log /sdcard/Download/'`).

Key log lines:

- `candidate stream down ...` — a bottom-edge single-finger stream was detected.
- `second finger down targetX=... targetY=...` — the second finger was swallowed and recorded.
- `thumb up -> start synthetic glide to second finger` — the thumb was released first.
- `glide finished, targetX=... targetY=...` — the synthetic glide reached the target.
- `index up, dropping into small window ...` — the final drop `ACTION_UP` was sent.

## Tuning

All timing constants are at the top of `SmallWindowInputFilter.java`:

| Constant | Default | Meaning |
| --- | --- | --- |
| `TELEPORT_DURATION_MS` | `180L` | How long the synthetic glide from thumb to index finger takes. |
| `MIN_DROP_HOLD_MS` | `16L` | Minimum one-frame hold at the target before the drop `UP`. |
| `TICK_MS` | `16L` | Frame interval of the synthetic glide. |

If your device still misclassifies the gesture as swipe-up-to-home, increase `TELEPORT_DURATION_MS` or `MIN_DROP_HOLD_MS`.

## Project layout

```
.
├── .github/workflows/build.yml   # CI build
├── app/
│   ├── build.gradle
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── java/io.github.zilewang7.smallwindow/
│       │   ├── MainHook.java              # LSPosed entry, hooks InputManagerService
│       │   └── SmallWindowInputFilter.java # The InputFilter implementation
│       └── resources/META-INF/xposed/     # LSPosed module metadata
│   ├── stubs/
│   │   ├── inputfilter-stubs.jar          # Compile-only stub for android.view.InputFilter
│   │   └── src/android/view/InputFilter.java # Source of the stub
├── build.gradle
├── settings.gradle
└── gradle/wrapper/                        # Gradle wrapper
```

`android.view.InputFilter` is not part of the public Android SDK, so a minimal compile-only stub is used. The stub is never packaged into the APK.

## License

[MIT](LICENSE)

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

HyperOS 4 上这个手势被破坏：第二根手指按下会取消多任务拖拽，窗口弹回前台应用。本模块通过在 `system_server` 中过滤输入事件来恢复旧行为。

## 原理

- HyperOS 4 的桌面（`com.miui.home`）是纯原生程序（`hyos_spawner` / `libapp_launcher.so`），没有 Java 代码，因此无法用 LSPosed 直接 hook。
- 模块 hook `system_server` 中的 `com.android.server.input.InputManagerService`，并安装一个 `android.view.InputFilter`。
- 过滤器会识别从屏幕底部手势区域开始的单指 `ACTION_DOWN` 触摸流（判定区域：`高度 × 0.82` 与 `高度 - 350px` 两者中的较大值以上）。
- 对这类触摸流：
  - 吞掉第二根手指的 `ACTION_POINTER_DOWN`，只记录其坐标，桌面永远看不到多指流，也就不会取消拖拽；
  - 双指按住期间只转发拇指的单指移动；
  - 拇指抬起时，过滤器合成一段约 180 ms 的平滑滑动，让系统看到的“同一根手指”从拇指位置滑到食指位置；
  - 食指抬起时注入最终 `ACTION_UP`，桌面将窗口挂成小窗。
- 平滑滑动和落地前的 16 ms 一帧停留可降低系统感知到的速度，避免被识别成“上滑回桌面”。

## 兼容性

已在以下环境测试：

- 小米 17 Pro Max（`2509FPN0BC`）
- HyperOS `OS4.0.0.20.XPBCNXM`，Android 17（SDK 37）
- LSPosed IT（API 102）+ Zygisk Next + APatch

模块只注入 `system_server` 与 `com.android.systemui`。其他具有相同原生桌面行为的 HyperOS 4 设备可能也能用，但不保证。

## 要求

- Android 16+（APK targetSdk 36+）
- LSPosed，libxposed API 102 或更高
- Root（Magisk / APatch / KernelSU）且 LSPosed 正常工作

## 构建

依赖：

- JDK 17
- Android SDK Platform 37（`platforms;android-37`）与 Build Tools `37.0.0`

```bash
./gradlew :app:assembleDebug --no-daemon        # debug
./gradlew :app:assembleRelease --no-daemon      # release（设置了 KEYSTORE_* 环境变量时使用正式签名）
./gradlew :app:assembleDiag --no-daemon         # 诊断版（开启文件日志）
```

产物命名遵循 `smallwindow-<versionName>[_debug[_diag]].apk`，例如 `smallwindow-1.1.0.apk`（正式版）、`smallwindow-1.1.0_debug.apk`（debug）和 `smallwindow-1.1.0_debug_diag.apk`（诊断版）。GitHub 按字母序排列附件，正式版会排在最前。

GitHub Actions 会在每次 push 时构建 debug 产物；推送 `v*` 标签时，会自动创建同时附带签名正式版 APK（`smallwindow-*.apk`）与诊断版 APK（`smallwindow-*_debug_diag.apk`）的 GitHub Release（见 `.github/workflows/build.yml` 和 `.github/workflows/release.yml`）。发布后还会自动同步到官方模块仓库 `Xposed-Modules-Repo/io.github.zilewang7.smallwindow`，使用 `VersionCode-VersionName` 格式的 tag（见 `.github/workflows/sync-store.yml`）。

## 安装

1. 构建 APK（或从 GitHub Actions / Releases 下载）；
2. 安装到手机：
   ```bash
   adb push app-debug.apk /data/local/tmp/smallwindow.apk
   adb shell 'su -c "pm install -r /data/local/tmp/smallwindow.apk"'
   ```
3. 打开 LSPosed 管理器，启用模块，并确认作用域包含 `system`（以及 `com.android.systemui`）；
4. 重启手机。

Hook 运行在 `system_server` 中，因此模块必须重启后才能生效。

## 调试

查看过滤器的决策日志：

```bash
adb logcat -s SmallWindowInputFilter:I
```

不用电脑时：安装诊断版（`smallwindow-*_debug_diag.apk`，每个 Release 都附带），复现手势后分享 `/data/system/smallwindow_filter.log`（root 可读；可用 root 文件管理器或 `su -c 'cp /data/system/smallwindow_filter.log /sdcard/Download/'` 导出）。

关键日志：

- `candidate stream down ...` —— 检测到底部单指触摸流；
- `second finger down targetX=... targetY=...` —— 第二指被吞掉并记录坐标；
- `thumb up -> start synthetic glide to second finger` —— 拇指先松开；
- `glide finished, targetX=... targetY=...` —— 合成滑动到达目标位置；
- `index up, dropping into small window ...` —— 已注入最终的挂小窗 `ACTION_UP`。

## 调参

所有时间参数都位于 `SmallWindowInputFilter.java` 顶部：

| 常量 | 默认值 | 含义 |
| --- | --- | --- |
| `TELEPORT_DURATION_MS` | `180L` | 从拇指位置滑到食指位置的合成滑动时长。 |
| `MIN_DROP_HOLD_MS` | `16L` | 落地前在目标位置的最小一帧停留时间。 |
| `TICK_MS` | `16L` | 合成滑动的帧间隔。 |

如果你的设备仍会误识别成“上滑回桌面”，请增大 `TELEPORT_DURATION_MS` 或 `MIN_DROP_HOLD_MS`。

## 项目结构

```
.
├── .github/workflows/build.yml   # CI 构建
├── app/
│   ├── build.gradle
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── java/io.github.zilewang7.smallwindow/
│       │   ├── MainHook.java              # LSPosed 入口，hook InputManagerService
│       │   └── SmallWindowInputFilter.java # InputFilter 实现
│       └── resources/META-INF/xposed/     # LSPosed 模块元数据
│   ├── stubs/
│   │   ├── inputfilter-stubs.jar          # 仅编译用的 android.view.InputFilter 桩
│   │   └── src/android/view/InputFilter.java # 桩源码
├── build.gradle
├── settings.gradle
└── gradle/wrapper/                        # Gradle wrapper
```

`android.view.InputFilter` 不属于公开 Android SDK，因此使用一个最小化的编译期桩。该桩不会被打进 APK。

## 许可证

[MIT](LICENSE)
