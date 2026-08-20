package io.github.zilewang7.smallwindow;

import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

import io.github.libxposed.api.XposedInterface;
import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface;

public class MainHook extends XposedModule {
    private static final String TAG = "SmallWindowInputFilter";
    private SmallWindowInputFilter inputFilter;

    public MainHook() {
        super();
    }

    @Override
    public void onSystemServerStarting(XposedModuleInterface.SystemServerStartingParam param) {
        log("onSystemServerStarting");
        hookClass("com.android.server.input.InputManagerService", param.getClassLoader());
    }

    private void hookClass(String className, ClassLoader classLoader) {
        try {
            Class<?> cls = Class.forName(className, false, classLoader);
            for (Constructor<?> constructor : cls.getDeclaredConstructors()) {
                log("hooking " + className + " constructor " + constructor);
                hook(constructor).setId(className + "_ctor").intercept(this::onImsConstructed);
            }
            for (Method method : cls.getDeclaredMethods()) {
                String name = method.getName();
                if (name.equals("setInputFilter") && method.getParameterCount() == 1) {
                    log("hooking " + className + "." + name);
                    hook(method).setId(className + "_" + name).intercept(this::onSetInputFilter);
                }
            }
        } catch (Throwable throwable) {
            Log.e(TAG, "failed to hook " + className, throwable);
        }
    }

    private Object onImsConstructed(XposedInterface.Chain chain) throws Throwable {
        Object result = chain.proceed();
        Object ims = chain.getThisObject();
        if (ims != null) {
            installInputFilter(ims);
        }
        return result;
    }

    private Object onSetInputFilter(XposedInterface.Chain chain) throws Throwable {
        Object filter = chain.getArg(0);
        Log.i(TAG, "setInputFilter filter=" + filter
                + " executable=" + chain.getExecutable());
        return chain.proceed();
    }

    private void installInputFilter(Object ims) {
        try {
            Class<?> iInputFilterClass = Class.forName("android.view.IInputFilter");
            Method setter = null;
            for (Method method : ims.getClass().getDeclaredMethods()) {
                if (method.getName().equals("setInputFilter")
                        && method.getParameterCount() == 1) {
                    setter = method;
                    break;
                }
            }
            if (setter == null) {
                Log.e(TAG, "setInputFilter method not found");
                return;
            }
            setter.setAccessible(true);
            if (inputFilter == null) {
                inputFilter = new SmallWindowInputFilter(Looper.getMainLooper());
            }
            setter.invoke(ims, inputFilter);
            Log.i(TAG, "installed SmallWindowInputFilter");
        } catch (Throwable throwable) {
            Log.e(TAG, "installInputFilter failed", throwable);
        }
    }

    private void log(String message) {
        Log.i(TAG, message);
    }
}
