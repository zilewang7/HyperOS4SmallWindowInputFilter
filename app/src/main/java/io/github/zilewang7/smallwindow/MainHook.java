package io.github.zilewang7.smallwindow;

import io.github.libxposed.api.XposedModule;

/**
 * Java entry retained for libxposed module metadata compatibility.
 *
 * HyperOS 4 MiuiHome does not run on ART. The active implementation is the
 * arm64 native entry declared in META-INF/xposed/native_init.list.
 */
public class MainHook extends XposedModule {
    public MainHook() {
        super();
    }
}
