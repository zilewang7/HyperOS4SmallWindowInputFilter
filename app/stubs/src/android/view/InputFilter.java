package android.view;

import android.os.Looper;

public abstract class InputFilter {
    public InputFilter(Looper looper) {
    }

    public void onInputEvent(InputEvent event, int policyFlags) {
    }

    public void sendInputEvent(InputEvent event, int policyFlags) {
    }
}
