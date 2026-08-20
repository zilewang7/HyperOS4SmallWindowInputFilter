package io.github.zilewang7.smallwindow;

import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputEvent;
import android.view.InputFilter;
import android.view.MotionEvent;

import java.io.File;
import java.io.FileWriter;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public class SmallWindowInputFilter extends InputFilter {
    private static final String TAG = "SmallWindowInputFilter";

    /**
     * Diagnostics build flag. When true, every decision is appended to
     * /sdcard/Download/smallwindow_filter.log so a tester can share the file
     * without a PC or logcat. Production builds keep this false.
     */
    private static final boolean ENABLE_FILE_LOG = false;

    /** Duration of the smooth synthetic glide from thumb position to target. */
    private static final long TELEPORT_DURATION_MS = 180L;

    /** Minimum perceived hold time at the target before the final drop UP.
     *  Kept at one frame (16ms) so the stationary target position is
     *  registered before the UP, without a visible pause. */
    private static final long MIN_DROP_HOLD_MS = 16L;

    /** Frame interval for the synthetic glide. */
    private static final long TICK_MS = 16L;

    private final Handler handler;
    private final Map<StreamKey, TeleportState> streams = new HashMap<>();

    private String fileLogPath;
    private boolean deviceInfoLogged;

    public SmallWindowInputFilter(Looper looper) {
        super(looper);
        handler = new Handler(looper);
    }

    @Override
    public void onInputEvent(InputEvent event, int policyFlags) {
        if (!(event instanceof MotionEvent)) {
            sendInputEvent(event, policyFlags);
            return;
        }
        MotionEvent motionEvent = (MotionEvent) event;
        int action = motionEvent.getActionMasked();
        StreamKey key = new StreamKey(
                motionEvent.getDeviceId(),
                motionEvent.getSource(),
                motionEvent.getDownTime());

        if (action == MotionEvent.ACTION_DOWN) {
            // A new physical gesture is starting. Make sure no synthetic stream is
            // still holding a pointer down in the system's view.
            endAllActiveStreamsForNewGesture();
            boolean bottom = isBottomSwipeStart(motionEvent);
            fileLog("REAL " + evt(motionEvent) + " -> " + (bottom ? "CANDIDATE" : "pass"));
            if (bottom) {
                TeleportState state = new TeleportState();
                state.key = key;
                state.primaryProps = new MotionEvent.PointerProperties();
                motionEvent.getPointerProperties(0, state.primaryProps);
                state.deviceId = motionEvent.getDeviceId();
                state.source = motionEvent.getSource();
                state.downTime = motionEvent.getDownTime();
                state.metaState = motionEvent.getMetaState();
                state.buttonState = motionEvent.getButtonState();
                state.xPrecision = motionEvent.getXPrecision();
                state.yPrecision = motionEvent.getYPrecision();
                state.edgeFlags = motionEvent.getEdgeFlags();
                state.flags = motionEvent.getFlags();
                state.lastX = motionEvent.getX(0);
                state.lastY = motionEvent.getY(0);
                streams.put(key, state);
                Log.i(TAG, "candidate stream down deviceId=" + key.deviceId
                        + " source=" + key.source + " downTime=" + key.downTime
                        + " y=" + motionEvent.getY(0)
                        + " pointerId=" + state.primaryProps.id);
            }
            sendInputEvent(event, policyFlags);
            return;
        }

        TeleportState state = streams.get(key);
        if (state == null) {
            sendInputEvent(event, policyFlags);
            return;
        }

        if (state.pendingUpEvent != null) {
            // The stream was already ended; flush the drop and swallow this event.
            Log.w(TAG, "event after pending drop, flushing pending UP");
            fileLog("REAL " + evt(motionEvent) + " -> after pending drop, flush");
            flushDrop(state);
            return;
        }

        fileLog("REAL " + evt(motionEvent));

        switch (action) {
            case MotionEvent.ACTION_POINTER_DOWN: {
                int actionIndex = motionEvent.getActionIndex();
                if (actionIndex <= 0) {
                    sendInputEvent(event, policyFlags);
                    return;
                }
                // Old behavior: second finger down just marks the target; nothing
                // changes on screen yet.
                state.secondDown = true;
                state.teleported = false;
                fileLog("  swallow second finger DOWN target=" + pt(motionEvent, actionIndex));
                Log.i(TAG, "second finger down targetX=" + motionEvent.getX(actionIndex)
                        + " targetY=" + motionEvent.getY(actionIndex));
                // Swallow: the system keeps seeing only the original thumb pointer.
                return;
            }
            case MotionEvent.ACTION_MOVE: {
                if (state.teleported && !state.interpDone) {
                    // The synthetic glide is still running. Real index-finger moves
                    // only update the target; the glide homes toward it.
                    state.targetX = motionEvent.getX(0);
                    state.targetY = motionEvent.getY(0);
                    fileLog("  glide running, target updated " + pt(motionEvent, 0));
                    return;
                }
                if (state.teleported) {
                    // After the glide, forward the real index-finger moves as the
                    // original thumb pointer so the window keeps following.
                    MotionEvent remapped = remapWithPrimaryProps(motionEvent,
                            MotionEvent.ACTION_MOVE, state, 0, 0f, 0f,
                            motionEvent.getEventTime());
                    sendInputEvent(remapped, policyFlags);
                    remapped.recycle();
                    state.lastMoveTime = motionEvent.getEventTime();
                    state.lastX = motionEvent.getX(0);
                    state.lastY = motionEvent.getY(0);
                    fileLog("  fwd index move as thumb " + pt(motionEvent, 0));
                } else if (state.secondDown) {
                    // Keep the window following the thumb while the second finger
                    // only marks a target. The system must only see one pointer.
                    MotionEvent thumbMove = remapWithPrimaryProps(motionEvent,
                            MotionEvent.ACTION_MOVE, state, 0, 0f, 0f,
                            motionEvent.getEventTime());
                    sendInputEvent(thumbMove, policyFlags);
                    thumbMove.recycle();
                    state.lastMoveTime = motionEvent.getEventTime();
                    state.lastX = motionEvent.getX(0);
                    state.lastY = motionEvent.getY(0);
                    fileLog("  fwd thumb move (second finger held) " + pt(motionEvent, 0));
                } else {
                    sendInputEvent(event, policyFlags);
                }
                return;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                int actionIndex = motionEvent.getActionIndex();
                if (actionIndex > 0) {
                    // The second finger lifted before the thumb: old behavior keeps
                    // the original drag unchanged.
                    if (state.secondDown) {
                        state.secondDown = false;
                        fileLog("  swallow second finger UP (before thumb), drag unchanged");
                        Log.i(TAG, "second finger up before thumb, drag unchanged");
                    }
                    // Always swallow: pointer 1 was never exposed to the system.
                    return;
                }
                // Primary (thumb) lifted while the second finger is still down.
                if (state.secondDown && !state.teleported) {
                    fileLog("  thumb up -> start synthetic glide to " + pt(motionEvent, 1));
                    Log.i(TAG, "thumb up -> start synthetic glide to second finger");
                    state.teleported = true;
                    state.secondDown = false;
                    state.interpDone = false;
                    state.indexUp = false;
                    state.teleportStartTime = motionEvent.getEventTime();
                    state.lastMoveTime = state.teleportStartTime;
                    state.startX = motionEvent.getX(0);
                    state.startY = motionEvent.getY(0);
                    state.targetX = motionEvent.getX(1);
                    state.targetY = motionEvent.getY(1);
                    state.lastX = state.startX;
                    state.lastY = state.startY;
                    state.interpRunnable = new InterpRunnable(state);
                    handler.post(state.interpRunnable);
                } else {
                    // Fallback: end the gesture with a clean single-pointer UP at
                    // the thumb position, never forward an unknown 2-pointer event.
                    fileLog("  unexpected primary UP -> clean UP");
                    Log.w(TAG, "unexpected primary pointer up, synthesizing clean UP");
                    MotionEvent up = remapWithPrimaryProps(motionEvent,
                            MotionEvent.ACTION_UP, state, 0, 0f, 0f,
                            motionEvent.getEventTime());
                    sendInputEvent(up, policyFlags);
                    up.recycle();
                    streams.remove(key);
                }
                return;
            }
            case MotionEvent.ACTION_UP: {
                if (state.teleported) {
                    state.indexUp = true;
                    long now = motionEvent.getEventTime();
                    if (!state.interpDone) {
                        // Let the synthetic glide finish, then it will schedule the
                        // final drop automatically.
                        fileLog("  index up during glide, finish glide then drop");
                        Log.i(TAG, "index up during glide, finish glide then drop");
                        return;
                    }
                    long hold = now - state.lastMoveTime;
                    if (hold >= MIN_DROP_HOLD_MS) {
                        fileLog("  index up, drop now (hold=" + hold + "ms)");
                        Log.i(TAG, "index up, dropping into small window (hold="
                                + hold + "ms)");
                        MotionEvent up = remapWithPrimaryProps(motionEvent,
                                MotionEvent.ACTION_UP, state, 0, 0f, 0f, now);
                        sendInputEvent(up, policyFlags);
                        up.recycle();
                        streams.remove(key);
                    } else {
                        fileLog("  index up too fast, delay drop (hold=" + hold + "ms)");
                        scheduleDelayedDrop(state, motionEvent, policyFlags, now);
                    }
                } else {
                    fileLog("  plain UP, forward and end");
                    sendInputEvent(event, policyFlags);
                    streams.remove(key);
                }
                return;
            }
            case MotionEvent.ACTION_CANCEL:
                fileLog("  CANCEL, forward and end");
                cancelStream(state);
                sendInputEvent(event, policyFlags);
                return;
            default:
                sendInputEvent(event, policyFlags);
        }
    }

    private void scheduleDelayedDrop(TeleportState state, MotionEvent source,
                                     int policyFlags, long now) {
        long desiredEventTime = state.lastMoveTime + MIN_DROP_HOLD_MS;
        long delay = desiredEventTime - now;
        if (delay < 0) {
            delay = 0;
            desiredEventTime = now;
        }
        fileLog("  delaying UP by " + delay + "ms");
        Log.i(TAG, "drop too fast, delaying UP by " + delay + "ms");
        MotionEvent up = remapWithPrimaryProps(source, MotionEvent.ACTION_UP,
                state, 0, 0f, 0f, desiredEventTime);
        state.pendingUpEvent = up;
        state.pendingPolicyFlags = policyFlags;
        state.pendingRunnable = new DropRunnable(state);
        handler.postDelayed(state.pendingRunnable, delay);
    }

    private boolean isBottomSwipeStart(MotionEvent event) {
        try {
            android.util.DisplayMetrics dm = android.content.res.Resources
                    .getSystem().getDisplayMetrics();
            float y = event.getY(0);
            float threshold = Math.max(dm.heightPixels * 0.82f,
                    dm.heightPixels - 350f);
            fileLog("  metrics " + dm.widthPixels + "x" + dm.heightPixels
                    + " dpi=" + dm.densityDpi + " threshold=" + threshold
                    + " y0=" + y + " count=" + event.getPointerCount());
            return event.getPointerCount() == 1 && y >= threshold;
        } catch (Throwable throwable) {
            Log.e(TAG, "isBottomSwipeStart failed", throwable);
            return false;
        }
    }

    private MotionEvent remapWithPrimaryProps(MotionEvent source, int newAction,
                                              TeleportState state,
                                              int sourcePointerIndex,
                                              float deltaX, float deltaY,
                                              long eventTime) {
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[1];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[1];
        props[0] = new MotionEvent.PointerProperties();
        props[0].id = state.primaryProps.id;
        props[0].toolType = state.primaryProps.toolType;
        coords[0] = new MotionEvent.PointerCoords();
        source.getPointerCoords(sourcePointerIndex, coords[0]);
        coords[0].x += deltaX;
        coords[0].y += deltaY;
        return MotionEvent.obtain(
                source.getDownTime(),
                eventTime,
                newAction,
                1,
                props,
                coords,
                source.getMetaState(),
                source.getButtonState(),
                source.getXPrecision(),
                source.getYPrecision(),
                source.getDeviceId(),
                source.getEdgeFlags(),
                source.getSource(),
                source.getFlags());
    }

    private MotionEvent synthesize(TeleportState state, int action,
                                   long eventTime, float x, float y) {
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[1];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[1];
        props[0] = new MotionEvent.PointerProperties();
        props[0].id = state.primaryProps.id;
        props[0].toolType = state.primaryProps.toolType;
        coords[0] = new MotionEvent.PointerCoords();
        coords[0].x = x;
        coords[0].y = y;
        coords[0].pressure = 1.0f;
        coords[0].size = 1.0f;
        coords[0].touchMajor = 1.0f;
        coords[0].touchMinor = 1.0f;
        coords[0].toolMajor = 1.0f;
        coords[0].toolMinor = 1.0f;
        return MotionEvent.obtain(
                state.downTime,
                eventTime,
                action,
                1,
                props,
                coords,
                state.metaState,
                state.buttonState,
                state.xPrecision,
                state.yPrecision,
                state.deviceId,
                state.edgeFlags,
                state.source,
                state.flags);
    }

    private void endAllActiveStreamsForNewGesture() {
        if (streams.isEmpty()) {
            return;
        }
        long now = SystemClock.uptimeMillis();
        List<TeleportState> toEnd = new ArrayList<>();
        Iterator<Map.Entry<StreamKey, TeleportState>> it =
                streams.entrySet().iterator();
        while (it.hasNext()) {
            TeleportState state = it.next().getValue();
            it.remove();
            if (state.pendingUpEvent != null) {
                handler.removeCallbacks(state.pendingRunnable);
                toEnd.add(state);
            } else {
                handler.removeCallbacks(state.interpRunnable);
                fileLog("  ending active synthetic stream for new gesture");
                Log.i(TAG, "ending active synthetic stream for new gesture");
                MotionEvent up = synthesize(state, MotionEvent.ACTION_UP, now,
                        state.teleported ? state.targetX : state.lastX,
                        state.teleported ? state.targetY : state.lastY);
                sendInputEvent(up, 0);
                up.recycle();
            }
        }
        for (TeleportState state : toEnd) {
            fileLog("  flushing pending drop for new gesture");
            Log.i(TAG, "flushing pending drop for new gesture");
            sendInputEvent(state.pendingUpEvent, state.pendingPolicyFlags);
            state.pendingUpEvent.recycle();
            state.pendingUpEvent = null;
        }
    }

    private void flushDrop(TeleportState state) {
        if (state.pendingUpEvent == null) {
            return;
        }
        handler.removeCallbacks(state.pendingRunnable);
        streams.remove(state.key);
        fileLog("  flushing pending drop");
        Log.i(TAG, "flushing pending drop");
        sendInputEvent(state.pendingUpEvent, state.pendingPolicyFlags);
        state.pendingUpEvent.recycle();
        state.pendingUpEvent = null;
    }

    private void cancelStream(TeleportState state) {
        handler.removeCallbacks(state.interpRunnable);
        if (state.pendingUpEvent != null) {
            handler.removeCallbacks(state.pendingRunnable);
            state.pendingUpEvent.recycle();
            state.pendingUpEvent = null;
        }
        streams.remove(state.key);
    }

    private final class InterpRunnable implements Runnable {
        private final TeleportState state;

        InterpRunnable(TeleportState state) {
            this.state = state;
        }

        @Override
        public void run() {
            if (streams.get(state.key) != state) {
                return;
            }
            long now = SystemClock.uptimeMillis();
            long elapsed = now - state.teleportStartTime;
            float t = elapsed / (float) TELEPORT_DURATION_MS;
            if (t < 0f) {
                t = 0f;
            }
            if (t > 1f) {
                t = 1f;
            }
            // Ease out: fast at first, slow when approaching the target. This
            // keeps the perceived velocity low and avoids a fling/back-home
            // classification.
            float eased = 1f - (1f - t) * (1f - t) * (1f - t);
            float x = state.startX + (state.targetX - state.startX) * eased;
            float y = state.startY + (state.targetY - state.startY) * eased;
            MotionEvent move = synthesize(state, MotionEvent.ACTION_MOVE, now, x, y);
            sendInputEvent(move, 0);
            move.recycle();
            state.lastMoveTime = now;
            state.lastX = x;
            state.lastY = y;
            fileLog("  synth MOVE t=" + t + " -> " + x + "," + y);

            if (t < 1f) {
                handler.postDelayed(this, TICK_MS);
                return;
            }

            state.interpDone = true;
            state.interpRunnable = null;
            fileLog("  glide finished at " + state.targetX + "," + state.targetY);
            Log.i(TAG, "glide finished, targetX=" + state.targetX
                    + " targetY=" + state.targetY);
            if (state.indexUp) {
                // The index finger was released during the glide. Hold the window
                // briefly at the target, then drop into small window.
                long hold = now - state.lastMoveTime;
                if (hold < MIN_DROP_HOLD_MS) {
                    long desiredEventTime = state.lastMoveTime + MIN_DROP_HOLD_MS;
                    long delay = desiredEventTime - now;
                    MotionEvent up = synthesize(state, MotionEvent.ACTION_UP,
                            desiredEventTime, state.targetX, state.targetY);
                    state.pendingUpEvent = up;
                    state.pendingPolicyFlags = 0;
                    state.pendingRunnable = new DropRunnable(state);
                    fileLog("  index already up, schedule drop in " + delay + "ms");
                    Log.i(TAG, "index already up, scheduling drop in " + delay + "ms");
                    handler.postDelayed(state.pendingRunnable, delay);
                } else {
                    MotionEvent up = synthesize(state, MotionEvent.ACTION_UP,
                            now, state.targetX, state.targetY);
                    sendInputEvent(up, 0);
                    up.recycle();
                    streams.remove(state.key);
                    fileLog("  drop UP now");
                }
            }
        }
    }

    private final class DropRunnable implements Runnable {
        private final TeleportState state;

        DropRunnable(TeleportState state) {
            this.state = state;
        }

        @Override
        public void run() {
            if (streams.get(state.key) != state) {
                return;
            }
            streams.remove(state.key);
            fileLog("  delayed drop UP delivered");
            Log.i(TAG, "delayed drop UP delivered");
            sendInputEvent(state.pendingUpEvent, state.pendingPolicyFlags);
            state.pendingUpEvent.recycle();
            state.pendingUpEvent = null;
        }
    }

    // ---- diagnostics file logging -------------------------------------------

    public void fileLogDeviceInfo() {
        fileLog("device " + Build.MANUFACTURER + " " + Build.MODEL
                + " sdk=" + Build.VERSION.SDK_INT
                + " fingerprint=" + Build.FINGERPRINT);
    }

    private void fileLog(String msg) {
        if (!ENABLE_FILE_LOG) {
            return;
        }
        try {
            if (fileLogPath == null) {
                fileLogPath = resolveLogPath();
                Log.i(TAG, "file log at " + fileLogPath);
            }
            FileWriter fw = new FileWriter(new File(fileLogPath), true);
            String ts = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US)
                    .format(new Date());
            fw.write(ts + " " + msg + "\n");
            fw.close();
        } catch (Throwable throwable) {
            Log.e(TAG, "fileLog failed", throwable);
        }
    }

    private String resolveLogPath() {
        String primary = "/data/media/0/Download/smallwindow_filter.log";
        try {
            File dir = new File("/data/media/0/Download");
            if (!dir.exists()) {
                dir.mkdirs();
            }
            FileWriter probe = new FileWriter(primary, true);
            probe.write("");  // probe write permission
            probe.close();
            return primary;
        } catch (Throwable primaryFailure) {
            Log.w(TAG, "cannot write " + primary + ", falling back", primaryFailure);
        }
        File fallback = new File("/data/local/tmp/smallwindow_filter.log");
        try {
            return fallback.getAbsolutePath();
        } catch (Throwable t) {
            return "/data/local/tmp/smallwindow_filter.log";
        }
    }

    private static String pt(MotionEvent e, int index) {
        return String.format(Locale.US, "%.0f,%.0f", e.getX(index), e.getY(index));
    }

    private static String evt(MotionEvent e) {
        StringBuilder sb = new StringBuilder();
        sb.append(actionName(e.getActionMasked()));
        sb.append(" idx=").append(e.getActionIndex());
        sb.append(" count=").append(e.getPointerCount());
        for (int i = 0; i < e.getPointerCount(); i++) {
            sb.append(String.format(Locale.US, " [p%d]%.0f,%.0f",
                    e.getPointerId(i), e.getX(i), e.getY(i)));
        }
        sb.append(" dev=").append(e.getDeviceId());
        sb.append(" src=0x").append(Integer.toHexString(e.getSource()));
        sb.append(" down=").append(e.getDownTime());
        sb.append(" t=").append(e.getEventTime());
        return sb.toString();
    }

    private static String actionName(int action) {
        switch (action) {
            case MotionEvent.ACTION_DOWN: return "DOWN";
            case MotionEvent.ACTION_MOVE: return "MOVE";
            case MotionEvent.ACTION_UP: return "UP";
            case MotionEvent.ACTION_CANCEL: return "CANCEL";
            case MotionEvent.ACTION_POINTER_DOWN: return "PTR_DOWN";
            case MotionEvent.ACTION_POINTER_UP: return "PTR_UP";
            default: return "ACT_" + action;
        }
    }

    private static final class TeleportState {
        StreamKey key;
        boolean secondDown;
        boolean teleported;
        boolean interpDone;
        boolean indexUp;
        long teleportStartTime;
        long lastMoveTime;
        float startX;
        float startY;
        float targetX;
        float targetY;
        float lastX;
        float lastY;
        MotionEvent.PointerProperties primaryProps;

        int deviceId;
        int source;
        long downTime;
        int metaState;
        int buttonState;
        float xPrecision;
        float yPrecision;
        int edgeFlags;
        int flags;

        MotionEvent pendingUpEvent;
        int pendingPolicyFlags;
        Runnable pendingRunnable;
        Runnable interpRunnable;
    }

    private static final class StreamKey {
        final int deviceId;
        final int source;
        final long downTime;

        StreamKey(int deviceId, int source, long downTime) {
            this.deviceId = deviceId;
            this.source = source;
            this.downTime = downTime;
        }

        @Override
        public boolean equals(Object o) {
            if (this == o) return true;
            if (!(o instanceof StreamKey)) return false;
            StreamKey streamKey = (StreamKey) o;
            return deviceId == streamKey.deviceId
                    && source == streamKey.source
                    && downTime == streamKey.downTime;
        }

        @Override
        public int hashCode() {
            int result = deviceId;
            result = 31 * result + source;
            result = 31 * result + (int) (downTime ^ (downTime >>> 32));
            return result;
        }
    }
}
