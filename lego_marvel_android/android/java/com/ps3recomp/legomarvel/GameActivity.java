package com.ps3recomp.legomarvel;

import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.Locale;

public class GameActivity extends NativeActivity {
    private static final String SHARED_CFG = "/storage/emulated/0/PS3/BLUS31161/settings.ini";
    private static int sActiveInstances = 0;
    private static boolean sNativeLinked = false;

    private static native long nativeGetFrameStats();
    private static native int nativeGetRenderHeight();
    private static native long nativeGetVirtualPadState();

    private HudOverlayView hudOverlay;
    private boolean overlayAttached = false;
    private boolean showFps = true;
    private boolean showTouch = true;

    private final Handler fpsHandler = new Handler(Looper.getMainLooper());
    private long lastSampleNs = 0L;
    private long lastFrames = -1L;

    private final Runnable fpsTick = new Runnable() {
        @Override
        public void run() {
            if (isFinishing() || isDestroyed() || hudOverlay == null || !showFps) return;
            updateFpsSample();
            fpsHandler.postDelayed(this, 500);
        }
    };

    private final Runnable padAnimTick = new Runnable() {
        @Override
        public void run() {
            if (isFinishing() || isDestroyed() || hudOverlay == null || !showTouch) return;
            if (sNativeLinked) {
                try {
                    hudOverlay.updatePadState(nativeGetVirtualPadState());
                } catch (Throwable ignored) {
                }
            }
            fpsHandler.postDelayed(this, 16);
        }
    };

    private static class HudOverlayView extends View {
        // PS3 CELL_PAD_CTRL_* bitmask constants (match cellPad.h)
        private static final int CTRL_SELECT   = 1 << 0;
        private static final int CTRL_L3       = 1 << 1;
        private static final int CTRL_R3       = 1 << 2;
        private static final int CTRL_START    = 1 << 3;
        private static final int CTRL_UP       = 1 << 4;
        private static final int CTRL_RIGHT    = 1 << 5;
        private static final int CTRL_DOWN     = 1 << 6;
        private static final int CTRL_LEFT     = 1 << 7;
        private static final int CTRL_L2       = 1 << 8;
        private static final int CTRL_R2       = 1 << 9;
        private static final int CTRL_L1       = 1 << 10;
        private static final int CTRL_R1       = 1 << 11;
        private static final int CTRL_TRIANGLE = 1 << 12;
        private static final int CTRL_CIRCLE   = 1 << 13;
        private static final int CTRL_CROSS    = 1 << 14;
        private static final int CTRL_SQUARE   = 1 << 15;

        private final boolean drawTouch;
        private final boolean drawFps;

        private final Paint ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint pressedFillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint pressedRingPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint knobPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint activeKnobPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint activeStickRingPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint stickGuidePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint accentRingPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint accentFillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        private final Paint fpsBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fpsBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fpsDotPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fpsTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        private float curFps = -1f;
        private String fpsLabel = "FPS: --  •  720p";

        // Default centered sticks: lx=128, ly=128, rx=128, ry=128, btns=0
        private long padState = (128L << 16) | (128L << 24) | (128L << 32) | (128L << 40);

        HudOverlayView(Context ctx, boolean drawTouch, boolean drawFps) {
            super(ctx);
            this.drawTouch = drawTouch;
            this.drawFps = drawFps;
            setClickable(false);
            setFocusable(false);

            ringPaint.setStyle(Paint.Style.STROKE);
            ringPaint.setStrokeWidth(2.6f);
            ringPaint.setColor(Color.argb(128, 255, 255, 255));

            fillPaint.setStyle(Paint.Style.FILL);
            fillPaint.setColor(Color.argb(54, 10, 14, 22));

            pressedFillPaint.setStyle(Paint.Style.FILL);
            pressedFillPaint.setColor(Color.argb(135, 88, 166, 255));

            pressedRingPaint.setStyle(Paint.Style.STROKE);
            pressedRingPaint.setStrokeWidth(3.2f);
            pressedRingPaint.setColor(Color.argb(235, 255, 255, 255));

            knobPaint.setStyle(Paint.Style.FILL);
            knobPaint.setColor(Color.argb(72, 255, 255, 255));

            activeKnobPaint.setStyle(Paint.Style.FILL);
            activeKnobPaint.setColor(Color.argb(165, 120, 195, 255));

            activeStickRingPaint.setStyle(Paint.Style.STROKE);
            activeStickRingPaint.setStrokeWidth(3.4f);
            activeStickRingPaint.setColor(Color.argb(220, 120, 195, 255));

            stickGuidePaint.setStyle(Paint.Style.STROKE);
            stickGuidePaint.setStrokeWidth(3.0f);
            stickGuidePaint.setStrokeCap(Paint.Cap.ROUND);
            stickGuidePaint.setColor(Color.argb(130, 120, 195, 255));

            accentRingPaint.setStyle(Paint.Style.STROKE);
            accentRingPaint.setStrokeWidth(3.0f);

            accentFillPaint.setStyle(Paint.Style.FILL);

            textPaint.setColor(Color.argb(200, 255, 255, 255));
            textPaint.setTypeface(Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD));
            textPaint.setTextAlign(Paint.Align.CENTER);

            fpsBgPaint.setStyle(Paint.Style.FILL);
            fpsBgPaint.setColor(Color.argb(185, 13, 17, 23));

            fpsBorderPaint.setStyle(Paint.Style.STROKE);
            fpsBorderPaint.setStrokeWidth(2f);
            fpsBorderPaint.setColor(Color.argb(165, 48, 54, 61));

            fpsDotPaint.setStyle(Paint.Style.FILL);
            fpsDotPaint.setColor(Color.rgb(88, 166, 255));

            fpsTextPaint.setColor(Color.rgb(240, 246, 252));
            fpsTextPaint.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
            fpsTextPaint.setTextAlign(Paint.Align.LEFT);
        }

        void updatePadState(long packed) {
            if (packed != this.padState) {
                this.padState = packed;
                invalidate();
            }
        }

        void updateFps(float fps, String label) {
            this.curFps = fps;
            this.fpsLabel = label;
            if (fps < 0f) {
                fpsDotPaint.setColor(Color.rgb(88, 166, 255));   // Booting (blue)
            } else if (fps >= 25f) {
                fpsDotPaint.setColor(Color.rgb(63, 185, 80));    // Smooth (green)
            } else if (fps >= 15f) {
                fpsDotPaint.setColor(Color.rgb(210, 153, 34));   // Moderate (amber)
            } else {
                fpsDotPaint.setColor(Color.rgb(248, 81, 73));    // Low (red)
            }
            invalidate();
        }

        private void drawCircleBtn(Canvas c, float cx, float cy, float r, String label, int ringColor, boolean pressed) {
            if (pressed) {
                accentFillPaint.setColor(Color.argb(120, Color.red(ringColor), Color.green(ringColor), Color.blue(ringColor)));
                c.drawCircle(cx, cy, r * 1.06f, accentFillPaint);
                accentRingPaint.setColor(Color.argb(255, Color.red(ringColor), Color.green(ringColor), Color.blue(ringColor)));
                c.drawCircle(cx, cy, r * 1.06f, accentRingPaint);
                textPaint.setColor(Color.WHITE);
            } else {
                c.drawCircle(cx, cy, r, fillPaint);
                accentRingPaint.setColor(ringColor);
                c.drawCircle(cx, cy, r, accentRingPaint);
                textPaint.setColor(ringColor);
            }
            textPaint.setTextSize(r * 0.78f);
            float ty = cy - (textPaint.descent() + textPaint.ascent()) * 0.5f;
            c.drawText(label, cx, ty, textPaint);
            textPaint.setColor(Color.argb(200, 255, 255, 255));
        }

        private void drawStick(Canvas c, float cx, float cy, float r, String label,
                               int axisX, int axisY, boolean active) {
            float nx = (axisX >= 128) ? ((axisX - 128) / 127f) : ((axisX - 128) / 128f);
            float ny = (axisY >= 128) ? ((axisY - 128) / 127f) : ((axisY - 128) / 128f);
            float len = (float) Math.hypot(nx, ny);
            if (len > 1f) {
                nx /= len;
                ny /= len;
            }
            float maxTravel = r * 0.56f;
            float kx = cx + nx * maxTravel;
            float ky = cy + ny * maxTravel;

            c.drawCircle(cx, cy, r, fillPaint);
            c.drawCircle(cx, cy, r, active ? activeStickRingPaint : ringPaint);

            if (len > 0.08f) {
                c.drawLine(cx, cy, kx, ky, stickGuidePaint);
            }

            float knobR = r * (active ? 0.42f : 0.39f);
            c.drawCircle(kx, ky, knobR, active ? activeKnobPaint : knobPaint);
            c.drawCircle(kx, ky, knobR, active ? pressedRingPaint : ringPaint);
            textPaint.setColor(active ? Color.WHITE : Color.argb(200, 255, 255, 255));
            textPaint.setTextSize(knobR * 0.85f);
            float ty = ky - (textPaint.descent() + textPaint.ascent()) * 0.5f;
            c.drawText(label, kx, ty, textPaint);
            textPaint.setColor(Color.argb(200, 255, 255, 255));
        }

        private void drawDpad(Canvas c, float cx, float cy, float arm, float halfThick, int btns) {
            float rad = halfThick * 0.38f;
            // Vertical bar (Up / Down)
            c.drawRoundRect(cx - halfThick, cy - arm, cx + halfThick, cy + arm, rad, rad, fillPaint);
            c.drawRoundRect(cx - halfThick, cy - arm, cx + halfThick, cy + arm, rad, rad, ringPaint);
            // Horizontal bar (Left / Right)
            c.drawRoundRect(cx - arm, cy - halfThick, cx + arm, cy + halfThick, rad, rad, fillPaint);
            c.drawRoundRect(cx - arm, cy - halfThick, cx + arm, cy + halfThick, rad, rad, ringPaint);

            // Highlight pressed directional arms
            if ((btns & CTRL_UP) != 0) {
                c.drawRoundRect(cx - halfThick, cy - arm, cx + halfThick, cy - halfThick * 0.2f, rad, rad, pressedFillPaint);
            }
            if ((btns & CTRL_DOWN) != 0) {
                c.drawRoundRect(cx - halfThick, cy + halfThick * 0.2f, cx + halfThick, cy + arm, rad, rad, pressedFillPaint);
            }
            if ((btns & CTRL_LEFT) != 0) {
                c.drawRoundRect(cx - arm, cy - halfThick, cx - halfThick * 0.2f, cy + halfThick, rad, rad, pressedFillPaint);
            }
            if ((btns & CTRL_RIGHT) != 0) {
                c.drawRoundRect(cx + halfThick * 0.2f, cy - halfThick, cx + arm, cy + halfThick, rad, rad, pressedFillPaint);
            }

            // Direction arrows
            textPaint.setTextSize(halfThick * 0.95f);
            float off = arm * 0.65f;
            float base = -(textPaint.descent() + textPaint.ascent()) * 0.5f;
            textPaint.setColor((btns & CTRL_UP) != 0 ? Color.WHITE : Color.argb(200, 255, 255, 255));
            c.drawText("▲", cx, cy - off + base, textPaint);
            textPaint.setColor((btns & CTRL_DOWN) != 0 ? Color.WHITE : Color.argb(200, 255, 255, 255));
            c.drawText("▼", cx, cy + off + base, textPaint);
            textPaint.setColor((btns & CTRL_LEFT) != 0 ? Color.WHITE : Color.argb(200, 255, 255, 255));
            c.drawText("◀", cx - off, cy + base, textPaint);
            textPaint.setColor((btns & CTRL_RIGHT) != 0 ? Color.WHITE : Color.argb(200, 255, 255, 255));
            c.drawText("▶", cx + off, cy + base, textPaint);
            textPaint.setColor(Color.argb(200, 255, 255, 255));
        }

        private void drawPill(Canvas c, float cx, float cy, float hw, float hh, String label, boolean pressed) {
            c.drawRoundRect(cx - hw, cy - hh, cx + hw, cy + hh, hh, hh, pressed ? pressedFillPaint : fillPaint);
            c.drawRoundRect(cx - hw, cy - hh, cx + hw, cy + hh, hh, hh, pressed ? pressedRingPaint : ringPaint);
            textPaint.setColor(pressed ? Color.WHITE : Color.argb(200, 255, 255, 255));
            textPaint.setTextSize(hh * 0.96f);
            float ty = cy - (textPaint.descent() + textPaint.ascent()) * 0.5f;
            c.drawText(label, cx, ty, textPaint);
            textPaint.setColor(Color.argb(200, 255, 255, 255));
        }

        private void drawFpsBadge(Canvas c, float w, float h, float unit) {
            float hh = Math.max(15f, unit * 0.022f);
            fpsTextPaint.setTextSize(hh * 0.92f);
            float textW = fpsTextPaint.measureText(fpsLabel);
            float dotR = hh * 0.30f;
            float padX = hh * 0.85f;
            float gap = hh * 0.55f;
            float totalW = padX + (dotR * 2f) + gap + textW + padX;
            float hw = totalW * 0.5f;

            float cx = w * 0.50f;
            float cy = Math.max(hh + 8f, h * 0.042f);
            float left = cx - hw;
            float right = cx + hw;
            float top = cy - hh;
            float bottom = cy + hh;

            c.drawRoundRect(left, top, right, bottom, hh, hh, fpsBgPaint);
            c.drawRoundRect(left, top, right, bottom, hh, hh, fpsBorderPaint);

            float dotCx = left + padX + dotR;
            c.drawCircle(dotCx, cy, dotR, fpsDotPaint);

            float textX = dotCx + dotR + gap;
            float textY = cy - (fpsTextPaint.descent() + fpsTextPaint.ascent()) * 0.5f;
            c.drawText(fpsLabel, textX, textY, fpsTextPaint);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth();
            float h = getHeight();
            if (w <= 0 || h <= 0) return;

            float unit = Math.min(w, h);

            if (drawTouch) {
                final int btns = (int) (padState & 0xFFFFL);
                final int lx = (int) ((padState >>> 16) & 0xFFL);
                final int ly = (int) ((padState >>> 24) & 0xFFL);
                final int rx = (int) ((padState >>> 32) & 0xFFL);
                final int ry = (int) ((padState >>> 40) & 0xFFL);
                final boolean leftActive = ((padState >>> 48) & 1L) != 0L;
                final boolean rightActive = ((padState >>> 49) & 1L) != 0L;

                // Shoulder triggers (L2 / L1 on left edge, R2 / R1 on right edge)
                float shW = unit * 0.085f;
                float shH = unit * 0.042f;
                drawPill(c, w * 0.075f, h * 0.14f, shW, shH, "L2", (btns & CTRL_L2) != 0);
                drawPill(c, w * 0.075f, h * 0.25f, shW, shH, "L1", (btns & CTRL_L1) != 0);
                drawPill(c, w * 0.925f, h * 0.14f, shW, shH, "R2", (btns & CTRL_R2) != 0);
                drawPill(c, w * 0.925f, h * 0.25f, shW, shH, "R1", (btns & CTRL_R1) != 0);

                // 4-way D-Pad (mid-left)
                drawDpad(c, w * 0.115f, h * 0.52f, unit * 0.125f, unit * 0.044f, btns);

                // Left & Right Analog Sticks (bottom-left & bottom-right)
                float stickR = unit * 0.135f;
                drawStick(c, w * 0.21f, h * 0.80f, stickR, "L", lx, ly, leftActive);
                drawStick(c, w * 0.79f, h * 0.80f, stickR, "R", rx, ry, rightActive);

                // Face buttons diamond (mid-right) with PlayStation accent colors
                float btnR = unit * 0.058f;
                float spread = unit * 0.115f;
                float fx = w * 0.885f;
                float fy = h * 0.52f;
                drawCircleBtn(c, fx, fy - spread, btnR, "△", Color.argb(195, 72, 219, 150), (btns & CTRL_TRIANGLE) != 0);
                drawCircleBtn(c, fx, fy + spread, btnR, "✕", Color.argb(195, 110, 175, 255), (btns & CTRL_CROSS) != 0);
                drawCircleBtn(c, fx - spread, fy, btnR, "□", Color.argb(195, 236, 130, 215), (btns & CTRL_SQUARE) != 0);
                drawCircleBtn(c, fx + spread, fy, btnR, "○", Color.argb(195, 255, 115, 115), (btns & CTRL_CIRCLE) != 0);

                // Bottom-center utility bar: L3, SELECT, START, R3 (keeps top HUD unobstructed)
                float botY = h * 0.91f;
                float sysH = unit * 0.036f;
                drawPill(c, w * 0.355f, botY, unit * 0.062f, sysH, "L3", (btns & CTRL_L3) != 0);
                drawPill(c, w * 0.450f, botY, unit * 0.080f, sysH, "SELECT", (btns & CTRL_SELECT) != 0);
                drawPill(c, w * 0.550f, botY, unit * 0.080f, sysH, "START", (btns & CTRL_START) != 0);
                drawPill(c, w * 0.645f, botY, unit * 0.062f, sysH, "R3", (btns & CTRL_R3) != 0);
            }

            if (drawFps) {
                drawFpsBadge(c, w, h, unit);
            }
        }
    }

    private boolean readBoolSetting(String key, boolean defVal) {
        Intent intent = getIntent();
        if (intent != null && intent.hasExtra(key)) {
            return intent.getBooleanExtra(key, defVal);
        }
        File[] cands = { new File(SHARED_CFG), new File(getFilesDir(), "settings.ini") };
        for (File f : cands) {
            if (!f.isFile()) continue;
            try (BufferedReader br = new BufferedReader(new FileReader(f))) {
                String line;
                while ((line = br.readLine()) != null) {
                    line = line.trim();
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    int eq = line.indexOf('=');
                    if (eq > 0 && key.equals(line.substring(0, eq).trim())) {
                        String val = line.substring(eq + 1).trim();
                        return !"0".equals(val) && !"false".equalsIgnoreCase(val);
                    }
                }
                break;
            } catch (Exception ignored) {
            }
        }
        return defVal;
    }

    private void updateFpsSample() {
        if (hudOverlay == null) return;
        long packed = 0L;
        int resH = 720;
        if (sNativeLinked) {
            try {
                packed = nativeGetFrameStats();
                int h = nativeGetRenderHeight();
                if (h > 0) resH = h;
            } catch (Throwable ignored) {
            }
        }
        long frames = (packed >>> 32) & 0xFFFFFFFFL;
        long nowNs = SystemClock.elapsedRealtimeNanos();

        if (lastSampleNs == 0L || lastFrames < 0L || frames < lastFrames) {
            lastSampleNs = nowNs;
            lastFrames = frames;
            hudOverlay.updateFps(-1f, String.format(Locale.US, "FPS: --  •  %dp", resH));
            return;
        }

        double dtSec = (nowNs - lastSampleNs) / 1_000_000_000.0;
        long df = frames - lastFrames;
        lastSampleNs = nowNs;
        lastFrames = frames;

        if (frames == 0L) {
            hudOverlay.updateFps(-1f, String.format(Locale.US, "FPS: -- (Boot)  •  %dp", resH));
            return;
        }

        if (dtSec > 0.05) {
            float fps = (float) (df / dtSec);
            if (df > 0L) {
                float ms = (float) (1000.0 * dtSec / df);
                hudOverlay.updateFps(fps,
                        String.format(Locale.US, "%.1f FPS  •  %.1f ms  •  %dp", fps, ms, resH));
            } else {
                hudOverlay.updateFps(0f,
                        String.format(Locale.US, "0.0 FPS  •  -- ms  •  %dp", resH));
            }
        }
    }

    private void applyImmersiveMode() {
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    private void attachHudOverlay() {
        if (hudOverlay == null || overlayAttached || isFinishing() || isDestroyed()) return;
        View decor = getWindow().getDecorView();
        if (decor == null || decor.getWindowToken() == null) return;
        try {
            WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.MATCH_PARENT,
                    WindowManager.LayoutParams.MATCH_PARENT,
                    WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                            | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                    PixelFormat.TRANSLUCENT);
            lp.token = decor.getWindowToken();
            lp.gravity = Gravity.TOP | Gravity.START;
            getWindowManager().addView(hudOverlay, lp);
            overlayAttached = true;
        } catch (Throwable ignored) {
            overlayAttached = false;
        }
    }

    private void detachHudOverlay() {
        fpsHandler.removeCallbacks(fpsTick);
        fpsHandler.removeCallbacks(padAnimTick);
        if (hudOverlay != null && overlayAttached) {
            try {
                getWindowManager().removeViewImmediate(hudOverlay);
            } catch (Throwable ignored) {
            }
            overlayAttached = false;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sActiveInstances++;
        super.onCreate(savedInstanceState);
        if (!sNativeLinked) {
            try {
                System.loadLibrary("ps3recomp_lego_marvel");
                sNativeLinked = true;
            } catch (Throwable ignored) {
            }
        }
        applyImmersiveMode();
        showTouch = readBoolSetting("touch_controls", true);
        showFps = readBoolSetting("show_fps", true);
        if (showTouch || showFps) {
            hudOverlay = new HudOverlayView(this, showTouch, showFps);
            getWindow().getDecorView().post(this::attachHudOverlay);
            if (showFps) {
                fpsHandler.postDelayed(fpsTick, 250);
            }
            if (showTouch) {
                fpsHandler.postDelayed(padAnimTick, 16);
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (showFps && hudOverlay != null) {
            fpsHandler.removeCallbacks(fpsTick);
            lastSampleNs = 0L;
            fpsHandler.postDelayed(fpsTick, 250);
        }
        if (showTouch && hudOverlay != null) {
            fpsHandler.removeCallbacks(padAnimTick);
            fpsHandler.postDelayed(padAnimTick, 16);
        }
    }

    @Override
    protected void onPause() {
        fpsHandler.removeCallbacks(fpsTick);
        fpsHandler.removeCallbacks(padAnimTick);
        super.onPause();
    }

    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();
        attachHudOverlay();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveMode();
            attachHudOverlay();
        }
    }

    @Override
    protected void onDestroy() {
        detachHudOverlay();
        sActiveInstances = Math.max(0, sActiveInstances - 1);
        super.onDestroy();
        // Let ActivityThread finish activityDestroyed IPC cleanly, then terminate the
        // isolated :game process so the next launch gets a fresh 4GB guest VM & Vulkan driver.
        if (!isChangingConfigurations() && sActiveInstances == 0) {
            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                if (sActiveInstances == 0) {
                    Process.killProcess(Process.myPid());
                }
            }, 120);
        }
    }
}
