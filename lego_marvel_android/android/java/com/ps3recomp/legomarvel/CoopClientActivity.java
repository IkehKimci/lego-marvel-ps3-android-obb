package com.ps3recomp.legomarvel;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Dual-Screen 2-Phone Co-op Client (runs on Phone 2 — ZERO OBB required!).
 *
 * Supports BOTH:
 *   - Online Relay Mode (WebSocket TLS / Multi-Port TCP with Room Port + SHA-256 Password):
 *     Receives live H.264 video + 24 kHz Stereo IMA-ADPCM audio and sends Player 2 controller
 *     inputs (touch + physical USB/BT gamepad) over ONE single outbound connection!
 *   - LAN Direct Mode (local Wi-Fi / Hotspot TCP :31161 + UDP :31162).
 */
public class CoopClientActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "CoopClientActivity";

    private static final int MAGIC_LMV1 = CoopRelayLink.MAGIC_LMV1;
    private static final int PKT_CSD    = CoopRelayLink.PKT_CSD;
    private static final int PKT_H264   = CoopRelayLink.PKT_H264;
    private static final int PKT_JPEG   = CoopRelayLink.PKT_JPEG;
    private static final int PKT_AUDIO  = CoopRelayLink.PKT_AUDIO;
    private static final int PKT_PING   = CoopRelayLink.PKT_PING;
    private static final int PKT_INPUT  = CoopRelayLink.PKT_INPUT;
    private static final int PKT_STATUS = CoopRelayLink.PKT_STATUS;

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

    private boolean useRelay = true;
    private String hostIp = "";
    private String relayPort = "31161";
    private String relayPass = "";
    private boolean showTouch = true;
    private boolean autoHideTouch = true;
    private boolean nintendoLayout = false;
    private volatile boolean audioMuted = false;

    private SurfaceView surfaceView;
    private volatile Surface activeSurface;
    private volatile SurfaceHolder activeHolder;
    private P2OverlayView overlayView;

    private final AtomicBoolean running = new AtomicBoolean(false);
    private volatile Socket activeSocket;
    private volatile CoopRelayLink activeRelay;
    private Thread videoThread;
    private Thread inputThread;
    private Thread audioThread;
    private final ArrayBlockingQueue<byte[]> audioQueue = new ArrayBlockingQueue<>(8);

    // Non-destructive layered P2 controller state (touch + physical gamepad on Phone 2)
    private final Object padLock = new Object();
    private int hwKeyBtns = 0;
    private int hwHatBtns = 0;
    private int hwTrigBtns = 0;
    private int hwLx = 128, hwLy = 128, hwRx = 128, hwRy = 128;
    private int hwL2 = 0, hwR2 = 0;

    private int touchBtns = 0;
    private int touchLx = 128, touchLy = 128, touchRx = 128, touchRy = 128;
    private int touchL2 = 0, touchR2 = 0;
    private boolean touchLActive = false;
    private boolean touchRActive = false;
    private boolean hwPadActive = false;
    private boolean inputDirty = true;

    // Stream telemetry for HUD pill
    private volatile boolean connected = false;
    private volatile boolean splitActive = false;
    private volatile long decodedFrames = 0L;
    private volatile String lastConnError = "";
    private volatile String myLanIp = "";
    private long lastHudNs = 0L;
    private long lastHudFrames = 0L;
    private long lastBackMs = 0L;

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final Runnable hudTick = new Runnable() {
        @Override
        public void run() {
            if (!running.get() || overlayView == null) return;
            long nowNs = SystemClock.elapsedRealtimeNanos();
            long frames = decodedFrames;
            String label;
            int dotColor;
            if (!connected) {
                String err = lastConnError;
                String targetLabel = useRelay ? ("Room " + relayPort + " @ " + hostIp) : hostIp;
                String myPart = (!useRelay && myLanIp != null && !myLanIp.isEmpty()) ? (" • IP HP2: " + myLanIp) : "";
                if (err != null && !err.isEmpty()) {
                    label = "GAGAL (" + err + ") • " + targetLabel + myPart;
                    dotColor = Color.rgb(248, 81, 73);
                } else {
                    label = (useRelay ? "ONLINE MENYAMBUNG " : "P2 MENYAMBUNG KE ") + targetLabel + " ..." + myPart;
                    dotColor = Color.rgb(210, 153, 34);
                }
            } else {
                float fps = 0f;
                if (lastHudNs > 0L && nowNs > lastHudNs) {
                    double dt = (nowNs - lastHudNs) / 1_000_000_000.0;
                    if (dt > 0.05) fps = (float) ((frames - lastHudFrames) / dt);
                }
                String modeTag = splitActive ? "P2 SPLIT" : "P2 MIRROR";
                String padTag = hwPadActive ? "PAD • " : "";
                String sndTag = audioMuted ? "🔇" : "🔊";
                String endTag = useRelay ? ("ROOM " + relayPort) : hostIp;
                label = String.format(Locale.US, "%s  •  %s%s  •  %.1f FPS  •  %s",
                        sndTag, padTag, modeTag, fps, endTag);
                dotColor = (fps >= 18f) ? Color.rgb(63, 185, 80) : Color.rgb(88, 166, 255);
            }
            lastHudNs = nowNs;
            lastHudFrames = frames;
            overlayView.setStatus(label, dotColor);
            uiHandler.postDelayed(this, 500);
        }
    };

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        applyImmersiveMode();

        String nm = getIntent().getStringExtra("coop_net_mode");
        useRelay = !"lan".equalsIgnoreCase(nm);
        String ip = getIntent().getStringExtra("host_ip");
        if (ip != null && !ip.trim().isEmpty()) hostIp = ip.trim();
        String rp = getIntent().getStringExtra("relay_port");
        if (rp != null && !rp.trim().isEmpty()) relayPort = rp.trim();
        String pw = getIntent().getStringExtra("relay_pass");
        relayPass = (pw != null) ? pw : "";
        audioMuted = !getIntent().getBooleanExtra("coop_audio", true);
        myLanIp = useRelay ? "" : CoopStreamServer.getLocalIpv4(this);
        showTouch = getIntent().getBooleanExtra("touch_controls", true);
        autoHideTouch = getIntent().getBooleanExtra("gamepad_autohide", true);
        nintendoLayout = "nintendo".equals(getIntent().getStringExtra("gamepad_layout"));

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        surfaceView = new SurfaceView(this);
        FrameLayout.LayoutParams svLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER);
        surfaceView.setLayoutParams(svLp);
        surfaceView.getHolder().addCallback(this);
        root.addView(surfaceView);
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or_, ob) -> {
            int rw = r - l, rh = b - t;
            if (rw <= 0 || rh <= 0) return;
            int dw = rw, dh = rh;
            if ((long) rw * 9L > (long) rh * 16L) {
                dw = (int) ((rh * 16L + 4L) / 9L);
            } else {
                dh = (int) ((rw * 9L + 8L) / 16L);
            }
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
            if (lp.width != dw || lp.height != dh) {
                lp.width = dw;
                lp.height = dh;
                surfaceView.setLayoutParams(lp);
            }
        });

        overlayView = new P2OverlayView(this);
        overlayView.setLayoutParams(new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(overlayView);

        setContentView(root);

        running.set(true);
        startNetworking();
        uiHandler.postDelayed(hudTick, 250);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyImmersiveMode();
    }

    @Override
    protected void onDestroy() {
        running.set(false);
        uiHandler.removeCallbacks(hudTick);
        CoopRelayLink rl = activeRelay;
        activeRelay = null;
        if (rl != null) rl.close();
        Socket s = activeSocket;
        activeSocket = null;
        if (s != null) {
            try { s.close(); } catch (Throwable ignored) {}
        }
        audioQueue.clear();
        synchronized (padLock) {
            padLock.notifyAll();
        }
        super.onDestroy();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        activeHolder = holder;
        activeSurface = holder.getSurface();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        activeHolder = holder;
        activeSurface = holder.getSurface();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        activeSurface = null;
        activeHolder = null;
    }

    // ---- Physical USB / Bluetooth Gamepad Support on Phone 2 ----

    private static int axisToU8Raw(float v) {
        if (v < -1f) v = -1f;
        if (v > 1f) v = 1f;
        int out = (v >= 0f) ? (128 + Math.round(v * 127f)) : (128 + Math.round(v * 128f));
        if (out < 0) out = 0;
        if (out > 255) out = 255;
        return out;
    }

    private static void evalStickSquareGate(float x, float y, float dz, float outer, int[] outXY) {
        float len = (float) Math.hypot(x, y);
        if (len <= dz) {
            outXY[0] = 128;
            outXY[1] = 128;
            return;
        }
        float ux = x / len;
        float uy = y / len;
        float mag = (len - dz) / (outer - dz);
        if (mag > 1f) mag = 1f;
        float dom = Math.max(Math.abs(ux), Math.abs(uy));
        float sq = (dom > 1e-4f) ? (1f / dom) : 1f;
        float scale = mag * (1f + (sq - 1f) * mag);
        outXY[0] = axisToU8Raw(ux * scale);
        outXY[1] = axisToU8Raw(uy * scale);
    }

    private int mapKeyCodeToPs3(int kc, int src) {
        switch (kc) {
            case KeyEvent.KEYCODE_BUTTON_A:      return nintendoLayout ? CTRL_CIRCLE : CTRL_CROSS;
            case KeyEvent.KEYCODE_BUTTON_B:      return nintendoLayout ? CTRL_CROSS : CTRL_CIRCLE;
            case KeyEvent.KEYCODE_BUTTON_X:      return nintendoLayout ? CTRL_TRIANGLE : CTRL_SQUARE;
            case KeyEvent.KEYCODE_BUTTON_Y:      return nintendoLayout ? CTRL_SQUARE : CTRL_TRIANGLE;
            case KeyEvent.KEYCODE_BUTTON_L1:     return CTRL_L1;
            case KeyEvent.KEYCODE_BUTTON_R1:     return CTRL_R1;
            case KeyEvent.KEYCODE_BUTTON_L2:     return CTRL_L2;
            case KeyEvent.KEYCODE_BUTTON_R2:     return CTRL_R2;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return CTRL_L3;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return CTRL_R3;
            case KeyEvent.KEYCODE_BUTTON_START:  return CTRL_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return CTRL_SELECT;
            case KeyEvent.KEYCODE_DPAD_UP:       return CTRL_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:     return CTRL_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:     return CTRL_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:    return CTRL_RIGHT;
            case KeyEvent.KEYCODE_DPAD_CENTER:   return CTRL_CROSS;
            case KeyEvent.KEYCODE_BUTTON_1:      return CTRL_SQUARE;
            case KeyEvent.KEYCODE_BUTTON_2:      return CTRL_CROSS;
            case KeyEvent.KEYCODE_BUTTON_3:      return CTRL_CIRCLE;
            case KeyEvent.KEYCODE_BUTTON_4:      return CTRL_TRIANGLE;
            case KeyEvent.KEYCODE_BUTTON_5:      return CTRL_L1;
            case KeyEvent.KEYCODE_BUTTON_6:      return CTRL_R1;
            case KeyEvent.KEYCODE_BUTTON_7:      return CTRL_L2;
            case KeyEvent.KEYCODE_BUTTON_8:      return CTRL_R2;
            case KeyEvent.KEYCODE_BUTTON_9:      return CTRL_SELECT;
            case KeyEvent.KEYCODE_BUTTON_10:     return CTRL_START;
            case KeyEvent.KEYCODE_BUTTON_11:     return CTRL_L3;
            case KeyEvent.KEYCODE_BUTTON_12:     return CTRL_R3;
            case KeyEvent.KEYCODE_BACK:
                if ((src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                    (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                    return nintendoLayout ? CTRL_CROSS : CTRL_CIRCLE;
                }
                return 0;
            default:
                return 0;
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent ev) {
        int src = ev.getSource();
        int kc = ev.getKeyCode();
        if (kc == KeyEvent.KEYCODE_BUTTON_MODE) return true;
        int mask = mapKeyCodeToPs3(kc, src);
        if (mask != 0) {
            boolean down = (ev.getAction() == KeyEvent.ACTION_DOWN);
            synchronized (padLock) {
                if (down) hwKeyBtns |= mask;
                else      hwKeyBtns &= ~mask;
                hwPadActive = true;
                inputDirty = true;
                padLock.notifyAll();
            }
            if (overlayView != null) overlayView.postInvalidate();
            return true;
        }
        if (kc == KeyEvent.KEYCODE_BACK && ev.getAction() == KeyEvent.ACTION_DOWN) {
            long now = SystemClock.uptimeMillis();
            if (now - lastBackMs < 2000L) {
                finish();
            } else {
                lastBackMs = now;
                Toast.makeText(this, "Tekan Kembali sekali lagi untuk keluar Co-op P2", Toast.LENGTH_SHORT).show();
            }
            return true;
        }
        return super.dispatchKeyEvent(ev);
    }

    private static float clamp01(float v) {
        return v < 0f ? 0f : (v > 1f ? 1f : v);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        int src = ev.getSource();
        if ((src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
            (src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) {
            float fx = ev.getAxisValue(MotionEvent.AXIS_X);
            float fy = ev.getAxisValue(MotionEvent.AXIS_Y);
            float fz = ev.getAxisValue(MotionEvent.AXIS_Z);
            float frz = ev.getAxisValue(MotionEvent.AXIS_RZ);
            float frx = ev.getAxisValue(MotionEvent.AXIS_RX);
            float fry = ev.getAxisValue(MotionEvent.AXIS_RY);
            if (Math.abs(fz) < 0.04f && Math.abs(frz) < 0.04f &&
                (Math.abs(frx) > 0.08f || Math.abs(fry) > 0.08f)) {
                fz = frx;
                frz = fry;
            }
            float lt = Math.max(clamp01(ev.getAxisValue(MotionEvent.AXIS_LTRIGGER)),
                                clamp01(ev.getAxisValue(MotionEvent.AXIS_BRAKE)));
            float rt = Math.max(clamp01(ev.getAxisValue(MotionEvent.AXIS_RTRIGGER)),
                                clamp01(ev.getAxisValue(MotionEvent.AXIS_GAS)));
            float hx = ev.getAxisValue(MotionEvent.AXIS_HAT_X);
            float hy = ev.getAxisValue(MotionEvent.AXIS_HAT_Y);

            int[] lxy = new int[2];
            int[] rxy = new int[2];
            evalStickSquareGate(fx, fy, 0.08f, 0.88f, lxy);
            evalStickSquareGate(fz, frz, 0.08f, 0.88f, rxy);

            int hat = 0;
            if (hx < -0.45f) hat |= CTRL_LEFT;
            if (hx >  0.45f) hat |= CTRL_RIGHT;
            if (hy < -0.45f) hat |= CTRL_UP;
            if (hy >  0.45f) hat |= CTRL_DOWN;

            int trig = 0;
            if (lt > 0.25f) trig |= CTRL_L2;
            if (rt > 0.25f) trig |= CTRL_R2;

            synchronized (padLock) {
                hwLx = lxy[0]; hwLy = lxy[1];
                hwRx = rxy[0]; hwRy = rxy[1];
                hwL2 = Math.round(lt * 255f);
                hwR2 = Math.round(rt * 255f);
                hwHatBtns = hat;
                hwTrigBtns = trig;
                if (Math.abs(fx) > 0.12f || Math.abs(fy) > 0.12f ||
                    Math.abs(fz) > 0.12f || Math.abs(frz) > 0.12f ||
                    lt > 0.15f || rt > 0.15f || hat != 0) {
                    hwPadActive = true;
                }
                inputDirty = true;
                padLock.notifyAll();
            }
            if (overlayView != null) overlayView.postInvalidate();
            return true;
        }
        return super.dispatchGenericMotionEvent(ev);
    }

    // ---- Networking: Online Relay (Video+Audio+Input on 1 connection) OR LAN Direct ----

    private void startNetworking() {
        audioThread = new Thread(this::runAudioPlayerLoop, "coop-p2-audio");
        audioThread.setDaemon(true);
        audioThread.start();

        inputThread = new Thread(this::runInputSenderLoop, "coop-p2-input");
        inputThread.setDaemon(true);
        inputThread.start();

        videoThread = new Thread(this::runVideoClientLoop, "coop-p2-video");
        videoThread.setDaemon(true);
        videoThread.start();
    }

    private void runInputSenderLoop() {
        DatagramSocket udpSock = null;
        InetAddress lanDst = null;
        try {
            if (!useRelay) {
                udpSock = CoopStreamServer.createLanUdpSocket(this, hostIp);
                lanDst = InetAddress.getByName(hostIp);
            }
            byte[] buf = new byte[16];
            buf[0] = 'L'; buf[1] = 'M'; buf[2] = 'P'; buf[3] = '2';
            int seq = 0;

            while (running.get()) {
                int btns, lx, ly, rx, ry, l2, r2;
                synchronized (padLock) {
                    if (!inputDirty) {
                        try { padLock.wait(33); } catch (InterruptedException ignored) {}
                    }
                    inputDirty = false;
                    btns = hwKeyBtns | hwHatBtns | hwTrigBtns | touchBtns;
                    lx = touchLActive ? touchLx : hwLx;
                    ly = touchLActive ? touchLy : hwLy;
                    rx = touchRActive ? touchRx : hwRx;
                    ry = touchRActive ? touchRy : hwRy;
                    l2 = Math.max(hwL2, touchL2);
                    r2 = Math.max(hwR2, touchR2);
                    if ((btns & CTRL_L2) != 0 && l2 == 0) l2 = 255;
                    if ((btns & CTRL_R2) != 0 && r2 == 0) r2 = 255;
                }
                if (!running.get()) break;

                seq++;
                buf[4]  = (byte) (seq & 0xFF);
                buf[5]  = (byte) ((seq >>> 8) & 0xFF);
                buf[6]  = (byte) ((seq >>> 16) & 0xFF);
                buf[7]  = (byte) ((seq >>> 24) & 0xFF);
                buf[8]  = (byte) (btns & 0xFF);
                buf[9]  = (byte) ((btns >>> 8) & 0xFF);
                buf[10] = (byte) (lx & 0xFF);
                buf[11] = (byte) (ly & 0xFF);
                buf[12] = (byte) (rx & 0xFF);
                buf[13] = (byte) (ry & 0xFF);
                buf[14] = (byte) (l2 & 0xFF);
                buf[15] = (byte) (r2 & 0xFF);

                if (useRelay) {
                    CoopRelayLink rl = activeRelay;
                    if (rl != null && !rl.isClosed() && connected) {
                        try {
                            rl.sendPacket(PKT_INPUT, 0, 0, 0, buf, 0, 16);
                        } catch (Throwable ignored) {
                        }
                    }
                } else if (udpSock != null && lanDst != null) {
                    try {
                        udpSock.send(new DatagramPacket(buf, 16, lanDst, CoopStreamServer.INPUT_UDP_PORT));
                    } catch (Throwable ignored) {
                    }
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "Input sender stopped: " + t.getMessage());
        } finally {
            if (udpSock != null) {
                try { udpSock.close(); } catch (Throwable ignored) {}
            }
        }
    }

    // ---- 24 kHz Stereo IMA-ADPCM Audio Decoder & Low-Latency AudioTrack ----

    private static final int[] IMA_STEP_TABLE = {
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
            34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
            157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
            724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
            2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
            9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
            27086, 29794, 32767
    };
    private static final int[] IMA_INDEX_TABLE = {
            -1, -1, -1, -1, 2, 4, 6, 8,
            -1, -1, -1, -1, 2, 4, 6, 8
    };

    private void enqueueAudioPacket(byte[] src, int len) {
        if (audioMuted || len < 9) return;
        byte[] copy = new byte[len];
        System.arraycopy(src, 0, copy, 0, len);
        while (!audioQueue.offer(copy)) {
            audioQueue.poll(); // drop oldest chunk to keep audio latency low
        }
    }

    private void runAudioPlayerLoop() {
        AudioTrack track = null;
        try {
            int minBuf = AudioTrack.getMinBufferSize(
                    24000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
            int bufBytes = Math.max(minBuf, 512 * 4 * 4);
            track = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setSampleRate(24000)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                            .build())
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setBufferSizeInBytes(bufBytes)
                    .build();
            track.play();

            short[] pcm = new short[1024];
            while (running.get()) {
                byte[] blk = audioQueue.poll(120, TimeUnit.MILLISECONDS);
                if (blk == null || audioMuted || blk.length < 9) continue;
                int frames = blk.length - 8;
                if (pcm.length < frames * 2) pcm = new short[frames * 2];

                int pL = (short) ((blk[0] & 0xFF) | ((blk[1] & 0xFF) << 8));
                int iL = blk[2] & 0xFF;
                if (iL > 88) iL = 88;
                int pR = (short) ((blk[4] & 0xFF) | ((blk[5] & 0xFF) << 8));
                int iR = blk[6] & 0xFF;
                if (iR > 88) iR = 88;

                for (int n = 0; n < frames; n++) {
                    int packed = blk[8 + n] & 0xFF;
                    int cL = packed & 0x0F;
                    int stepL = IMA_STEP_TABLE[iL];
                    int dL = stepL >> 3;
                    if ((cL & 4) != 0) dL += stepL;
                    if ((cL & 2) != 0) dL += stepL >> 1;
                    if ((cL & 1) != 0) dL += stepL >> 2;
                    pL = ((cL & 8) != 0) ? (pL - dL) : (pL + dL);
                    if (pL > 32767) pL = 32767;
                    else if (pL < -32768) pL = -32768;
                    iL += IMA_INDEX_TABLE[cL];
                    if (iL < 0) iL = 0;
                    else if (iL > 88) iL = 88;

                    int cR = (packed >>> 4) & 0x0F;
                    int stepR = IMA_STEP_TABLE[iR];
                    int dR = stepR >> 3;
                    if ((cR & 4) != 0) dR += stepR;
                    if ((cR & 2) != 0) dR += stepR >> 1;
                    if ((cR & 1) != 0) dR += stepR >> 2;
                    pR = ((cR & 8) != 0) ? (pR - dR) : (pR + dR);
                    if (pR > 32767) pR = 32767;
                    else if (pR < -32768) pR = -32768;
                    iR += IMA_INDEX_TABLE[cR];
                    if (iR < 0) iR = 0;
                    else if (iR > 88) iR = 88;

                    pcm[n * 2]     = (short) pL;
                    pcm[n * 2 + 1] = (short) pR;
                }
                track.write(pcm, 0, frames * 2);
            }
        } catch (Throwable t) {
            Log.w(TAG, "Audio player stopped: " + t.getMessage());
        } finally {
            if (track != null) {
                try { track.stop(); } catch (Throwable ignored) {}
                try { track.release(); } catch (Throwable ignored) {}
            }
        }
    }

    private static String friendlyConnError(Throwable t) {
        if (t == null) return "Putus";
        String msg = t.getMessage();
        String low = (msg != null) ? msg.toLowerCase(Locale.US) : "";
        if (low.contains("password salah") || low.contains("host hp 1") || low.contains("port ") || low.contains("sedang bangun")) {
            return msg;
        }
        String tag = (low.contains("[wifi]")) ? "[WiFi] " : "";
        if (t instanceof java.net.UnknownHostException) {
            return "Alamat server tidak ditemukan — cek internet & penulisan domain";
        }
        if (t instanceof java.net.SocketTimeoutException || low.contains("timed out")) {
            return tag + "Timeout — server tidak merespon";
        }
        if (low.contains("econnrefused") || low.contains("refused")) {
            return tag + "Ditolak — pastikan Server / HP 1 aktif";
        }
        if (low.contains("enetunreach") || low.contains("ehostunreach") || low.contains("unreachable")) {
            return tag + "Jaringan tidak terjangkau";
        }
        if (msg != null && !msg.isEmpty()) {
            return msg.length() > 54 ? msg.substring(0, 54) : msg;
        }
        return t.getClass().getSimpleName();
    }

    private MediaCodec handleIncomingPacket(MediaCodec decoder, int[] decWH, long[] ptsHolder,
                                            MediaCodec.BufferInfo info,
                                            int type, int flags, int w, int h, byte[] payload, int len) throws Exception {
        splitActive = ((flags & 2) != 0);
        if (type == PKT_PING || len == 0) {
            return decoder;
        }
        if (type == PKT_STATUS) {
            if (len > 0) {
                String st = new String(payload, 0, len, StandardCharsets.UTF_8);
                Log.i(TAG, "Relay status: " + st);
            }
            return decoder;
        }
        if (type == PKT_AUDIO) {
            enqueueAudioPacket(payload, len);
            return decoder;
        }

        Surface surf = activeSurface;
        if (surf == null || !surf.isValid()) return decoder;

        if (type == PKT_JPEG) {
            Bitmap bmp = BitmapFactory.decodeByteArray(payload, 0, len);
            if (bmp != null) {
                SurfaceHolder holder = activeHolder;
                if (holder != null) {
                    Canvas c = null;
                    try {
                        c = holder.lockCanvas();
                        if (c != null) {
                            c.drawColor(Color.BLACK);
                            drawAspectFittedBitmap(c, bmp);
                        }
                    } finally {
                        if (c != null) holder.unlockCanvasAndPost(c);
                    }
                }
                bmp.recycle();
                decodedFrames++;
            }
            return decoder;
        }

        if (decoder == null || w != decWH[0] || h != decWH[1]) {
            if (decoder != null) {
                try { decoder.stop(); } catch (Throwable ignored) {}
                try { decoder.release(); } catch (Throwable ignored) {}
                decoder = null;
            }
            if (type == PKT_CSD || (flags & 1) != 0) {
                MediaFormat fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h);
                try { fmt.setInteger("low-latency", 1); } catch (Throwable ignored) {}
                decoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
                decoder.configure(fmt, surf, null, 0);
                decoder.start();
                decWH[0] = w;
                decWH[1] = h;
            } else {
                return null;
            }
        }

        int inIdx = decoder.dequeueInputBuffer(15_000);
        if (inIdx >= 0) {
            ByteBuffer inBuf = decoder.getInputBuffer(inIdx);
            if (inBuf != null) {
                inBuf.clear();
                inBuf.put(payload, 0, len);
                int cFlags = (type == PKT_CSD) ? MediaCodec.BUFFER_FLAG_CODEC_CONFIG : 0;
                decoder.queueInputBuffer(inIdx, 0, len, ptsHolder[0], cFlags);
                if (type != PKT_CSD) ptsHolder[0] += 33_333L;
            }
        }
        for (;;) {
            int outIdx = decoder.dequeueOutputBuffer(info, 2_000);
            if (outIdx == MediaCodec.INFO_TRY_AGAIN_LATER) break;
            if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) continue;
            if (outIdx < 0) break;
            decoder.releaseOutputBuffer(outIdx, true);
            decodedFrames++;
        }
        return decoder;
    }

    private void runVideoClientLoop() {
        while (running.get()) {
            while (running.get() && (activeSurface == null || !activeSurface.isValid())) {
                try { Thread.sleep(50); } catch (InterruptedException ignored) {}
            }
            if (!running.get()) break;

            if (useRelay) {
                runOnlineRelayClientSession();
            } else {
                runLanDirectClientSession();
            }

            if (running.get()) {
                long delayMs = (lastConnError != null && lastConnError.toLowerCase(Locale.US).contains("password")) ? 4000L : 1500L;
                try { Thread.sleep(delayMs); } catch (InterruptedException ignored) {}
            }
        }
    }

    private void runOnlineRelayClientSession() {
        CoopRelayLink link = null;
        MediaCodec decoder = null;
        try {
            link = CoopRelayLink.connect(hostIp, relayPort, relayPass, false, 10_000, 35_000);
            activeRelay = link;
            connected = true;
            lastConnError = "";
            Log.i(TAG, "Online Relay P2 connected to " + hostIp + " room=" + relayPort);

            CoopRelayLink.Packet pkt = new CoopRelayLink.Packet();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            int[] decWH = { 0, 0 };
            long[] ptsHolder = { 0L };

            while (running.get() && !link.isClosed()) {
                link.readPacket(pkt);
                decoder = handleIncomingPacket(decoder, decWH, ptsHolder, info,
                        pkt.type, pkt.flags, pkt.w, pkt.h, pkt.data, pkt.len);
            }
        } catch (Throwable t) {
            lastConnError = friendlyConnError(t);
            Log.w(TAG, "Online Relay reconnect: " + t.getMessage());
        } finally {
            connected = false;
            audioQueue.clear();
            if (decoder != null) {
                try { decoder.stop(); } catch (Throwable ignored) {}
                try { decoder.release(); } catch (Throwable ignored) {}
            }
            if (link != null) link.close();
            if (activeRelay == link) activeRelay = null;
        }
    }

    private void runLanDirectClientSession() {
        myLanIp = CoopStreamServer.getLocalIpv4(this);
        Socket sock = null;
        MediaCodec decoder = null;
        try {
            sock = CoopStreamServer.connectLanTcp(this, hostIp, CoopStreamServer.VIDEO_TCP_PORT, 3500);
            sock.setTcpNoDelay(true);
            sock.setReceiveBufferSize(256 * 1024);
            sock.setSoTimeout(6000);
            activeSocket = sock;

            sock.getOutputStream().write(new byte[]{'P', 'L', 'A', 'Y'});
            sock.getOutputStream().flush();

            connected = true;
            lastConnError = "";
            Log.i(TAG, "LAN Connected to Co-op Host at " + hostIp);

            DataInputStream dis = new DataInputStream(new BufferedInputStream(sock.getInputStream(), 65536));
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            byte[] payload = new byte[256 * 1024];
            int[] decWH = { 0, 0 };
            long[] ptsHolder = { 0L };

            while (running.get()) {
                int magic = dis.readInt();
                if (magic != MAGIC_LMV1) throw new IOException("Bad stream magic");
                int type = dis.readUnsignedByte();
                int flags = dis.readUnsignedByte();
                int w = dis.readUnsignedShort();
                int h = dis.readUnsignedShort();
                int len = dis.readInt();
                if (len < 0 || len > 4 * 1024 * 1024) throw new IOException("Invalid frame len " + len);
                if (len > 0) {
                    if (len > payload.length) payload = new byte[len + 16384];
                    dis.readFully(payload, 0, len);
                }
                decoder = handleIncomingPacket(decoder, decWH, ptsHolder, info,
                        type, flags, w, h, payload, len);
            }
        } catch (Throwable t) {
            lastConnError = friendlyConnError(t);
            Log.w(TAG, "LAN Co-op stream reconnect: " + t.getMessage());
        } finally {
            connected = false;
            audioQueue.clear();
            if (decoder != null) {
                try { decoder.stop(); } catch (Throwable ignored) {}
                try { decoder.release(); } catch (Throwable ignored) {}
            }
            if (sock != null) {
                try { sock.close(); } catch (Throwable ignored) {}
            }
            if (activeSocket == sock) activeSocket = null;
        }
    }

    private static void drawAspectFittedBitmap(Canvas c, Bitmap bmp) {
        int cw = c.getWidth(), ch = c.getHeight();
        int bw = bmp.getWidth(), bh = bmp.getHeight();
        if (cw <= 0 || ch <= 0 || bw <= 0 || bh <= 0) return;
        int dw = cw, dh = ch;
        if ((long) cw * bh > (long) ch * bw) {
            dw = (int) (((long) ch * bw) / bh);
        } else {
            dh = (int) (((long) cw * bh) / bw);
        }
        int left = (cw - dw) / 2;
        int top = (ch - dh) / 2;
        c.drawBitmap(bmp, null, new Rect(left, top, left + dw, top + dh), null);
    }

    // ---- Interactive DualShock 3 Multi-Touch & Status HUD Overlay for Phone 2 ----

    private class P2OverlayView extends View {
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

        private final Paint hudBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint hudBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint hudDotPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint hudTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        private int leftStickPid = -1;
        private int rightStickPid = -1;
        private String statusText = "P2 MENYAMBUNG...";

        P2OverlayView(Context ctx) {
            super(ctx);
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

            hudBgPaint.setStyle(Paint.Style.FILL);
            hudBgPaint.setColor(Color.argb(185, 13, 17, 23));
            hudBorderPaint.setStyle(Paint.Style.STROKE);
            hudBorderPaint.setStrokeWidth(2f);
            hudBorderPaint.setColor(Color.argb(165, 48, 54, 61));
            hudDotPaint.setStyle(Paint.Style.FILL);
            hudDotPaint.setColor(Color.rgb(210, 153, 34));
            hudTextPaint.setColor(Color.rgb(240, 246, 252));
            hudTextPaint.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
            hudTextPaint.setTextAlign(Paint.Align.LEFT);
        }

        void setStatus(String label, int dotColor) {
            this.statusText = label;
            hudDotPaint.setColor(dotColor);
            invalidate();
        }

        private boolean hitPill(float x, float y, float cx, float cy, float hw, float hh) {
            return Math.abs(x - cx) <= hw && Math.abs(y - cy) <= hh;
        }

        private boolean hitCircle(float x, float y, float cx, float cy, float r) {
            float dx = x - cx, dy = y - cy;
            return (dx * dx + dy * dy) <= (r * r);
        }

        @Override
        public boolean onTouchEvent(MotionEvent ev) {
            float w = getWidth();
            float h = getHeight();
            if (w <= 0f || h <= 0f) return true;
            int masked = ev.getActionMasked();
            if (masked == MotionEvent.ACTION_DOWN) {
                float tx = ev.getX(), ty = ev.getY();
                if (ty < h * 0.11f && tx > w * 0.22f && tx < w * 0.78f) {
                    audioMuted = !audioMuted;
                    if (audioMuted) audioQueue.clear();
                    Toast.makeText(CoopClientActivity.this,
                            audioMuted ? "🔇 Audio HP 2 di-Mute" : "🔊 Audio HP 2 Aktif",
                            Toast.LENGTH_SHORT).show();
                    return true;
                }
            }
            if (!showTouch) return false;
            float unit = Math.min(w, h);
            int actIdx = ev.getActionIndex();
            int upIdx = (masked == MotionEvent.ACTION_POINTER_UP || masked == MotionEvent.ACTION_UP ||
                         masked == MotionEvent.ACTION_CANCEL) ? actIdx : -1;

            float shW = unit * 0.085f * 1.22f;
            float shH = unit * 0.042f * 1.30f;
            float dpadCx = w * 0.115f, dpadCy = h * 0.52f, dpadArm = unit * 0.125f;
            float stickR = unit * 0.135f, stickRange = stickR * 0.72f;
            float lStickX = w * 0.21f, lStickY = h * 0.80f;
            float rStickX = w * 0.79f, rStickY = h * 0.80f;
            float fx = w * 0.885f, fy = h * 0.52f, spread = unit * 0.115f, btnHitR = unit * 0.058f * 1.32f;
            float botY = h * 0.91f, sysH = unit * 0.036f * 1.35f;

            int count = ev.getPointerCount();
            if (masked == MotionEvent.ACTION_CANCEL) {
                leftStickPid = -1;
                rightStickPid = -1;
            } else if (upIdx >= 0 && upIdx < count) {
                int pid = ev.getPointerId(upIdx);
                if (pid == leftStickPid) leftStickPid = -1;
                if (pid == rightStickPid) rightStickPid = -1;
            } else if ((masked == MotionEvent.ACTION_DOWN || masked == MotionEvent.ACTION_POINTER_DOWN) &&
                       actIdx >= 0 && actIdx < count) {
                float px = ev.getX(actIdx), py = ev.getY(actIdx);
                int pid = ev.getPointerId(actIdx);
                if (leftStickPid < 0 && hitCircle(px, py, lStickX, lStickY, stickR * 1.38f)) {
                    leftStickPid = pid;
                } else if (rightStickPid < 0 && hitCircle(px, py, rStickX, rStickY, stickR * 1.38f)) {
                    rightStickPid = pid;
                }
            }

            int btns = 0;
            int lx = 128, ly = 128, rx = 128, ry = 128;
            int l2 = 0, r2 = 0;
            boolean lSeen = false, rSeen = false;
            int[] tmpXY = new int[2];

            for (int i = 0; i < count; i++) {
                if (masked == MotionEvent.ACTION_CANCEL || i == upIdx) continue;
                float x = ev.getX(i), y = ev.getY(i);
                int pid = ev.getPointerId(i);

                if (pid == leftStickPid) {
                    evalStickSquareGate((x - lStickX) / stickRange, (y - lStickY) / stickRange, 0.10f, 1.0f, tmpXY);
                    lx = tmpXY[0]; ly = tmpXY[1]; lSeen = true; continue;
                }
                if (pid == rightStickPid) {
                    evalStickSquareGate((x - rStickX) / stickRange, (y - rStickY) / stickRange, 0.10f, 1.0f, tmpXY);
                    rx = tmpXY[0]; ry = tmpXY[1]; rSeen = true; continue;
                }
                if (hitPill(x, y, w * 0.075f, h * 0.14f, shW, shH)) { btns |= CTRL_L2; l2 = 255; continue; }
                if (hitPill(x, y, w * 0.075f, h * 0.25f, shW, shH)) { btns |= CTRL_L1; continue; }
                if (hitPill(x, y, w * 0.925f, h * 0.14f, shW, shH)) { btns |= CTRL_R2; r2 = 255; continue; }
                if (hitPill(x, y, w * 0.925f, h * 0.25f, shW, shH)) { btns |= CTRL_R1; continue; }

                if (hitPill(x, y, w * 0.355f, botY, unit * 0.062f * 1.18f, sysH)) { btns |= CTRL_L3; continue; }
                if (hitPill(x, y, w * 0.450f, botY, unit * 0.080f * 1.15f, sysH)) { btns |= CTRL_SELECT; continue; }
                if (hitPill(x, y, w * 0.550f, botY, unit * 0.080f * 1.15f, sysH)) { btns |= CTRL_START; continue; }
                if (hitPill(x, y, w * 0.645f, botY, unit * 0.062f * 1.18f, sysH)) { btns |= CTRL_R3; continue; }

                if (hitCircle(x, y, dpadCx, dpadCy, dpadArm * 1.28f)) {
                    float dx = x - dpadCx, dy = y - dpadCy, dead = dpadArm * 0.22f;
                    if (dx < -dead) btns |= CTRL_LEFT;
                    if (dx >  dead) btns |= CTRL_RIGHT;
                    if (dy < -dead) btns |= CTRL_UP;
                    if (dy >  dead) btns |= CTRL_DOWN;
                    continue;
                }

                boolean hitFace = false;
                if (hitCircle(x, y, fx, fy - spread, btnHitR)) { btns |= CTRL_TRIANGLE; hitFace = true; }
                if (hitCircle(x, y, fx, fy + spread, btnHitR)) { btns |= CTRL_CROSS;    hitFace = true; }
                if (hitCircle(x, y, fx - spread, fy, btnHitR)) { btns |= CTRL_SQUARE;   hitFace = true; }
                if (hitCircle(x, y, fx + spread, fy, btnHitR)) { btns |= CTRL_CIRCLE;   hitFace = true; }
                if (hitFace) continue;

                if (leftStickPid < 0 && hitCircle(x, y, lStickX, lStickY, stickR * 1.15f)) {
                    leftStickPid = pid;
                    evalStickSquareGate((x - lStickX) / stickRange, (y - lStickY) / stickRange, 0.10f, 1.0f, tmpXY);
                    lx = tmpXY[0]; ly = tmpXY[1]; lSeen = true; continue;
                }
                if (rightStickPid < 0 && hitCircle(x, y, rStickX, rStickY, stickR * 1.15f)) {
                    rightStickPid = pid;
                    evalStickSquareGate((x - rStickX) / stickRange, (y - rStickY) / stickRange, 0.10f, 1.0f, tmpXY);
                    rx = tmpXY[0]; ry = tmpXY[1]; rSeen = true; continue;
                }
            }
            if (!lSeen) leftStickPid = -1;
            if (!rSeen) rightStickPid = -1;

            synchronized (padLock) {
                hwPadActive = false;
                touchBtns = btns;
                touchLx = lx; touchLy = ly;
                touchRx = rx; touchRy = ry;
                touchL2 = l2; touchR2 = r2;
                touchLActive = lSeen;
                touchRActive = rSeen;
                inputDirty = true;
                padLock.notifyAll();
            }
            invalidate();
            return true;
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

        private void drawStick(Canvas c, float cx, float cy, float r, String label, int ax, int ay, boolean active) {
            float nx = (ax >= 128) ? ((ax - 128) / 127f) : ((ax - 128) / 128f);
            float ny = (ay >= 128) ? ((ay - 128) / 127f) : ((ay - 128) / 128f);
            float len = (float) Math.hypot(nx, ny);
            if (len > 1f) { nx /= len; ny /= len; }
            float maxTravel = r * 0.56f;
            float kx = cx + nx * maxTravel, ky = cy + ny * maxTravel;
            c.drawCircle(cx, cy, r, fillPaint);
            c.drawCircle(cx, cy, r, active ? activeStickRingPaint : ringPaint);
            if (len > 0.08f) c.drawLine(cx, cy, kx, ky, stickGuidePaint);
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
            c.drawRoundRect(cx - halfThick, cy - arm, cx + halfThick, cy + arm, rad, rad, fillPaint);
            c.drawRoundRect(cx - halfThick, cy - arm, cx + halfThick, cy + arm, rad, rad, ringPaint);
            c.drawRoundRect(cx - arm, cy - halfThick, cx + arm, cy + halfThick, rad, rad, fillPaint);
            c.drawRoundRect(cx - arm, cy - halfThick, cx + arm, cy + halfThick, rad, rad, ringPaint);
            if ((btns & CTRL_UP) != 0)    c.drawRoundRect(cx - halfThick, cy - arm, cx + halfThick, cy - halfThick * 0.2f, rad, rad, pressedFillPaint);
            if ((btns & CTRL_DOWN) != 0)  c.drawRoundRect(cx - halfThick, cy + halfThick * 0.2f, cx + halfThick, cy + arm, rad, rad, pressedFillPaint);
            if ((btns & CTRL_LEFT) != 0)  c.drawRoundRect(cx - arm, cy - halfThick, cx - halfThick * 0.2f, cy + halfThick, rad, rad, pressedFillPaint);
            if ((btns & CTRL_RIGHT) != 0) c.drawRoundRect(cx + halfThick * 0.2f, cy - halfThick, cx + arm, cy + halfThick, rad, rad, pressedFillPaint);
            textPaint.setTextSize(halfThick * 0.95f);
            float off = arm * 0.65f, base = -(textPaint.descent() + textPaint.ascent()) * 0.5f;
            textPaint.setColor((btns & CTRL_UP) != 0 ? Color.WHITE : Color.argb(200, 255, 255, 255));    c.drawText("▲", cx, cy - off + base, textPaint);
            textPaint.setColor((btns & CTRL_DOWN) != 0 ? Color.WHITE : Color.argb(200, 255, 255, 255));  c.drawText("▼", cx, cy + off + base, textPaint);
            textPaint.setColor((btns & CTRL_LEFT) != 0 ? Color.WHITE : Color.argb(200, 255, 255, 255));  c.drawText("◀", cx - off, cy + base, textPaint);
            textPaint.setColor((btns & CTRL_RIGHT) != 0 ? Color.WHITE : Color.argb(200, 255, 255, 255)); c.drawText("▶", cx + off, cy + base, textPaint);
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

        private void drawStatusPill(Canvas c, float w, float h, float unit) {
            float hh = Math.max(15f, unit * 0.022f);
            hudTextPaint.setTextSize(hh * 0.92f);
            float textW = hudTextPaint.measureText(statusText);
            float dotR = hh * 0.30f, padX = hh * 0.85f, gap = hh * 0.55f;
            float totalW = padX + (dotR * 2f) + gap + textW + padX;
            float cx = w * 0.50f, cy = Math.max(hh + 8f, h * 0.042f);
            float left = cx - totalW * 0.5f, right = cx + totalW * 0.5f;
            c.drawRoundRect(left, cy - hh, right, cy + hh, hh, hh, hudBgPaint);
            c.drawRoundRect(left, cy - hh, right, cy + hh, hh, hh, hudBorderPaint);
            float dotCx = left + padX + dotR;
            c.drawCircle(dotCx, cy, dotR, hudDotPaint);
            float textY = cy - (hudTextPaint.descent() + hudTextPaint.ascent()) * 0.5f;
            c.drawText(statusText, dotCx + dotR + gap, textY, hudTextPaint);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            if (w <= 0f || h <= 0f) return;
            float unit = Math.min(w, h);

            int btns, lx, ly, rx, ry;
            boolean lAct, rAct, hideTouch;
            synchronized (padLock) {
                btns = hwKeyBtns | hwHatBtns | hwTrigBtns | touchBtns;
                lx = touchLActive ? touchLx : hwLx;
                ly = touchLActive ? touchLy : hwLy;
                rx = touchRActive ? touchRx : hwRx;
                ry = touchRActive ? touchRy : hwRy;
                lAct = touchLActive || Math.abs(lx - 128) > 10 || Math.abs(ly - 128) > 10;
                rAct = touchRActive || Math.abs(rx - 128) > 10 || Math.abs(ry - 128) > 10;
                hideTouch = hwPadActive && autoHideTouch;
            }

            if (showTouch && !hideTouch) {
                float shW = unit * 0.085f, shH = unit * 0.042f;
                drawPill(c, w * 0.075f, h * 0.14f, shW, shH, "L2", (btns & CTRL_L2) != 0);
                drawPill(c, w * 0.075f, h * 0.25f, shW, shH, "L1", (btns & CTRL_L1) != 0);
                drawPill(c, w * 0.925f, h * 0.14f, shW, shH, "R2", (btns & CTRL_R2) != 0);
                drawPill(c, w * 0.925f, h * 0.25f, shW, shH, "R1", (btns & CTRL_R1) != 0);
                drawDpad(c, w * 0.115f, h * 0.52f, unit * 0.125f, unit * 0.044f, btns);
                float stickR = unit * 0.135f;
                drawStick(c, w * 0.21f, h * 0.80f, stickR, "L", lx, ly, lAct);
                drawStick(c, w * 0.79f, h * 0.80f, stickR, "R", rx, ry, rAct);
                float btnR = unit * 0.058f, spread = unit * 0.115f, fx = w * 0.885f, fy = h * 0.52f;
                drawCircleBtn(c, fx, fy - spread, btnR, "△", Color.argb(195, 72, 219, 150), (btns & CTRL_TRIANGLE) != 0);
                drawCircleBtn(c, fx, fy + spread, btnR, "✕", Color.argb(195, 110, 175, 255), (btns & CTRL_CROSS) != 0);
                drawCircleBtn(c, fx - spread, fy, btnR, "□", Color.argb(195, 236, 130, 215), (btns & CTRL_SQUARE) != 0);
                drawCircleBtn(c, fx + spread, fy, btnR, "○", Color.argb(195, 255, 115, 115), (btns & CTRL_CIRCLE) != 0);
                float botY = h * 0.91f, sysH = unit * 0.036f;
                drawPill(c, w * 0.355f, botY, unit * 0.062f, sysH, "L3", (btns & CTRL_L3) != 0);
                drawPill(c, w * 0.450f, botY, unit * 0.080f, sysH, "SELECT", (btns & CTRL_SELECT) != 0);
                drawPill(c, w * 0.550f, botY, unit * 0.080f, sysH, "START", (btns & CTRL_START) != 0);
                drawPill(c, w * 0.645f, botY, unit * 0.062f, sysH, "R3", (btns & CTRL_R3) != 0);
            }
            drawStatusPill(c, w, h, unit);
        }
    }
}
