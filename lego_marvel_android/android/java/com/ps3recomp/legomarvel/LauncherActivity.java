package com.ps3recomp.legomarvel;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class LauncherActivity extends Activity {

    private static final int REQ_PICK_DRIVER = 1001;

    private static final String GAME_ROOT     = "/storage/emulated/0/PS3/BLUS31161";
    private static final String SHARED_CFG    = GAME_ROOT + "/settings.ini";
    private static final String SHARED_LOGS   = GAME_ROOT + "/logs";
    private static final String DOWNLOAD_DIR  = "/storage/emulated/0/Download";

    private static final int COL_BG       = Color.parseColor("#0D1117");
    private static final int COL_CARD     = Color.parseColor("#161B22");
    private static final int COL_BORDER   = Color.parseColor("#30363D");
    private static final int COL_TEXT     = Color.parseColor("#F0F6FC");
    private static final int COL_MUTED    = Color.parseColor("#8B949E");
    private static final int COL_RED      = Color.parseColor("#E62429");
    private static final int COL_BLUE     = Color.parseColor("#1F6FEB");
    private static final int COL_GREEN    = Color.parseColor("#238636");
    private static final int COL_WARN     = Color.parseColor("#D29922");

    private static class DriverEntry {
        String id;
        String name;
        String detail;
        String dir;
        String lib;
        boolean isSystem;

        @Override
        public String toString() {
            return detail != null && !detail.isEmpty() ? (name + " (" + detail + ")") : name;
        }
    }

    private final List<DriverEntry> drivers = new ArrayList<>();
    private ArrayAdapter<DriverEntry> driverAdapter;

    private TextView statusBadgeText;
    private TextView logStatusText;
    private Spinner  driverSpinner;
    private Spinner  resSpinner;
    private Spinner  fpsSpinner;
    private Spinner  scalingSpinner;
    private Switch   swTurbo;
    private Switch   swGuestShaders;
    private Switch   swDirectPresent;
    private Switch   swSpuInterp;
    private Switch   swSkipFmv;
    private Switch   swTouch;
    private Switch   swShowFps;
    private Switch   swVerbose;

    private final String[] resLabels = {
            "720p (1280×720) — Native PS3",
            "540p (960×540) — Performa Tinggi",
            "480p (854×480) — Ultra Performa + Scaling",
            "1080p (1920×1080) — Resolusi Tinggi"
    };
    private final String[] resValues = { "720p", "540p", "480p", "1080p" };

    private final String[] fpsLabels = {
            "60 FPS (Unlocked)",
            "30 FPS (Stabil / Hemat Daya)",
            "Uncapped (Maksimal)"
    };
    private final String[] fpsValues = { "60", "30", "0" };

    private final String[] scalingLabels = {
            "Lossless Scaling (FSR1 + RCAS Tajam)",
            "CAS Sharpen (Ringan & Tajam)",
            "Integer / Nearest (Pixel Lossless)",
            "Bilinear (Standar)"
    };
    private final String[] scalingValues = { "fsr", "cas", "integer", "linear" };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        if (Build.VERSION.SDK_INT >= 21) {
            getWindow().setStatusBarColor(COL_BG);
            getWindow().setNavigationBarColor(COL_BG);
        }

        buildUi();
        autoImportFirstRunDrivers();
        refreshDriverList(null);
        loadSettingsIntoUi();
        refreshStatus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshStatus();
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private GradientDrawable roundBox(int bgColor, int strokeColor, int radiusDp) {
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(bgColor);
        gd.setCornerRadius(dp(radiusDp));
        if (strokeColor != 0) gd.setStroke(dp(1), strokeColor);
        return gd;
    }

    private LinearLayout makeCard(LinearLayout parent, String title) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(roundBox(COL_CARD, COL_BORDER, 12));
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(12);
        card.setLayoutParams(lp);

        if (title != null) {
            TextView tv = new TextView(this);
            tv.setText(title);
            tv.setTextColor(COL_TEXT);
            tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
            tv.setTypeface(Typeface.DEFAULT_BOLD);
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            tlp.bottomMargin = dp(10);
            tv.setLayoutParams(tlp);
            card.addView(tv);
        }
        parent.addView(card);
        return card;
    }

    private TextView makeLabel(LinearLayout parent, String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextColor(COL_MUTED);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(6);
        lp.bottomMargin = dp(4);
        tv.setLayoutParams(lp);
        parent.addView(tv);
        return tv;
    }

    private Spinner makeSpinner(LinearLayout parent, ArrayAdapter<?> adapter) {
        Spinner sp = new Spinner(this);
        sp.setAdapter(adapter);
        sp.setBackground(roundBox(Color.parseColor("#21262D"), COL_BORDER, 8));
        sp.setPadding(dp(10), dp(8), dp(10), dp(8));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(6);
        sp.setLayoutParams(lp);
        parent.addView(sp);
        return sp;
    }

    private Switch makeSwitchRow(LinearLayout parent, String title, String subtitle) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rlp.topMargin = dp(6);
        rlp.bottomMargin = dp(6);
        row.setLayoutParams(rlp);

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
        tlp.rightMargin = dp(8);
        texts.setLayoutParams(tlp);

        TextView t1 = new TextView(this);
        t1.setText(title);
        t1.setTextColor(COL_TEXT);
        t1.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        texts.addView(t1);

        if (subtitle != null && !subtitle.isEmpty()) {
            TextView t2 = new TextView(this);
            t2.setText(subtitle);
            t2.setTextColor(COL_MUTED);
            t2.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            texts.addView(t2);
        }

        Switch sw = new Switch(this);
        row.addView(texts);
        row.addView(sw);
        parent.addView(row);
        return sw;
    }

    private Button makeButton(String text, int bgColor, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        b.setBackground(roundBox(bgColor, 0, 8));
        b.setPadding(dp(12), dp(10), dp(12), dp(10));
        b.setOnClickListener(onClick);
        return b;
    }

    private ArrayAdapter<String> darkStringAdapter(String[] items) {
        return new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, items) {
            @Override
            public View getView(int pos, View convertView, ViewGroup parent) {
                TextView tv = (TextView) super.getView(pos, convertView, parent);
                tv.setTextColor(COL_TEXT);
                tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
                return tv;
            }
            @Override
            public View getDropDownView(int pos, View convertView, ViewGroup parent) {
                TextView tv = (TextView) super.getDropDownView(pos, convertView, parent);
                tv.setBackgroundColor(COL_CARD);
                tv.setTextColor(COL_TEXT);
                tv.setPadding(dp(14), dp(12), dp(14), dp(12));
                return tv;
            }
        };
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(COL_BG);
        scroll.setFillViewport(true);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(18), dp(16), dp(24));
        scroll.addView(root);

        // Header Card
        LinearLayout header = makeCard(root, null);
        TextView title = new TextView(this);
        title.setText("LEGO® Marvel Super Heroes");
        title.setTextColor(COL_TEXT);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 21);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        header.addView(title);

        TextView sub = new TextView(this);
        sub.setText("BLUS-31161 • Native ARM64 Port (ps3recomp + Vulkan)");
        sub.setTextColor(COL_MUTED);
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.bottomMargin = dp(10);
        sub.setLayoutParams(slp);
        header.addView(sub);

        statusBadgeText = new TextView(this);
        statusBadgeText.setTextColor(COL_TEXT);
        statusBadgeText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        statusBadgeText.setTypeface(Typeface.MONOSPACE);
        statusBadgeText.setBackground(roundBox(Color.parseColor("#0D1117"), COL_BORDER, 8));
        statusBadgeText.setPadding(dp(10), dp(8), dp(10), dp(8));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.bottomMargin = dp(12);
        statusBadgeText.setLayoutParams(blp);
        header.addView(statusBadgeText);

        Button playBtn = makeButton("▶   MAIN SEKARANG (LAUNCH GAME)", COL_RED, v -> launchGame());
        playBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        playBtn.setPadding(dp(16), dp(14), dp(16), dp(14));
        header.addView(playBtn);

        // Card 1: GPU Driver (AdrenoTools / Turnip)
        LinearLayout gpuCard = makeCard(root, "Driver GPU Vulkan (AdrenoTools / Turnip)");
        makeLabel(gpuCard, "Pilih Driver Vulkan Aktif");
        driverAdapter = new ArrayAdapter<DriverEntry>(this, android.R.layout.simple_spinner_item, drivers) {
            @Override
            public View getView(int pos, View convertView, ViewGroup parent) {
                TextView tv = (TextView) super.getView(pos, convertView, parent);
                tv.setTextColor(COL_TEXT);
                tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
                return tv;
            }
            @Override
            public View getDropDownView(int pos, View convertView, ViewGroup parent) {
                TextView tv = (TextView) super.getDropDownView(pos, convertView, parent);
                tv.setBackgroundColor(COL_CARD);
                tv.setTextColor(COL_TEXT);
                tv.setPadding(dp(14), dp(12), dp(14), dp(12));
                return tv;
            }
        };
        driverSpinner = makeSpinner(gpuCard, driverAdapter);

        LinearLayout drvBtns = new LinearLayout(this);
        drvBtns.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams dblp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        dblp.topMargin = dp(4);
        dblp.bottomMargin = dp(6);
        drvBtns.setLayoutParams(dblp);

        Button scanBtn = makeButton("Scan Download", COL_BLUE, v -> scanDownloadsForDrivers(true));
        Button pickBtn = makeButton("Import .zip", COL_GREEN, v -> openDriverPicker());
        Button delBtn  = makeButton("Hapus", Color.parseColor("#30363D"), v -> deleteSelectedDriver());

        LinearLayout.LayoutParams b1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.1f);
        b1.rightMargin = dp(6);
        scanBtn.setLayoutParams(b1);
        LinearLayout.LayoutParams b2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
        b2.rightMargin = dp(6);
        pickBtn.setLayoutParams(b2);
        LinearLayout.LayoutParams b3 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.7f);
        delBtn.setLayoutParams(b3);

        drvBtns.addView(scanBtn);
        drvBtns.addView(pickBtn);
        drvBtns.addView(delBtn);
        gpuCard.addView(drvBtns);

        swTurbo = makeSwitchRow(gpuCard, "Adreno Turbo Mode",
                "Paksa clock GPU Adreno maksimum (libadrenotools)");

        // Card 2: Graphics Settings
        LinearLayout gfxCard = makeCard(root, "Pengaturan Grafik (RSX Vulkan)");
        makeLabel(gfxCard, "Resolusi Render");
        resSpinner = makeSpinner(gfxCard, darkStringAdapter(resLabels));

        makeLabel(gfxCard, "Batas Frame Rate (FPS)");
        fpsSpinner = makeSpinner(gfxCard, darkStringAdapter(fpsLabels));

        makeLabel(gfxCard, "Lossless Scaling / Upscaler (GPU Shader)");
        scalingSpinner = makeSpinner(gfxCard, darkStringAdapter(scalingLabels));

        swGuestShaders = makeSwitchRow(gfxCard, "Guest Shaders (HLSL → SPIR-V)",
                "Terjemahkan shader NV4097 asli PS3 (wajib ON untuk grafis 3D)");
        swDirectPresent = makeSwitchRow(gfxCard, "Direct Vulkan Present",
                "Blit langsung ke swapchain tanpa readback CPU (lebih cepat)");
        swShowFps = makeSwitchRow(gfxCard, "Tampilkan FPS (HUD Overlay)",
                "Tampilkan counter FPS, waktu frame (ms), dan resolusi secara real-time");

        // Card 3: SPU, Media & Controls
        LinearLayout sysCard = makeCard(root, "SPU, Video & Kontrol");
        swSpuInterp = makeSwitchRow(sysCard, "SPU Interpreter Fallback",
                "Jalankan job SPU LZ/MultiStream yang belum di-lift (wajib ON)");
        swSkipFmv = makeSwitchRow(sysCard, "Lewati Intro FMV (PS3_NO_SAIL)",
                "Aktifkan jika cutscene pembuka hang atau macet");
        swTouch = makeSwitchRow(sysCard, "Kontrol Sentuh Virtual",
                "Tampilkan panduan tombol sentuh & input touchscreen (gamepad selalu aktif)");
        swVerbose = makeSwitchRow(sysCard, "Verbose Debug Log",
                "Catat semua panggilan detail (dapat menurunkan FPS)");

        // Card 4: Auto Logs & Diagnostics
        LinearLayout logCard = makeCard(root, "Log Otomatis & Crash Report");
        logStatusText = new TextView(this);
        logStatusText.setTextColor(COL_MUTED);
        logStatusText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        logStatusText.setTypeface(Typeface.MONOSPACE);
        LinearLayout.LayoutParams lslp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lslp.bottomMargin = dp(10);
        logStatusText.setLayoutParams(lslp);
        logCard.addView(logStatusText);

        LinearLayout logRow1 = new LinearLayout(this);
        logRow1.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams lr1 = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lr1.bottomMargin = dp(8);
        logRow1.setLayoutParams(lr1);

        Button btnLatest = makeButton("Lihat latest.log", COL_BLUE,
                v -> showLogDialog("latest.log", new File(SHARED_LOGS, "latest.log")));
        Button btnCrash  = makeButton("Lihat crash_last.log", COL_WARN,
                v -> showLogDialog("crash_last.log", new File(SHARED_LOGS, "crash_last.log")));
        LinearLayout.LayoutParams lb1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lb1.rightMargin = dp(6);
        btnLatest.setLayoutParams(lb1);
        btnCrash.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        logRow1.addView(btnLatest);
        logRow1.addView(btnCrash);
        logCard.addView(logRow1);

        LinearLayout logRow2 = new LinearLayout(this);
        logRow2.setOrientation(LinearLayout.HORIZONTAL);
        Button btnSave  = makeButton("Simpan Setting", COL_GREEN, v -> {
            saveSettingsFromUi();
            Toast.makeText(this, "Pengaturan disimpan ke settings.ini", Toast.LENGTH_SHORT).show();
        });
        Button btnReset = makeButton("Reset Default", Color.parseColor("#30363D"), v -> resetDefaults());
        LinearLayout.LayoutParams lb2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        lb2.rightMargin = dp(6);
        btnSave.setLayoutParams(lb2);
        btnReset.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        logRow2.addView(btnSave);
        logRow2.addView(btnReset);
        logCard.addView(logRow2);

        setContentView(scroll);
    }

    private boolean hasStoragePermission() {
        if (Build.VERSION.SDK_INT >= 30) {
            return Environment.isExternalStorageManager();
        }
        return true;
    }

    private void requestStoragePermission() {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivity(i);
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        }
    }

    private File findObbInDir(File dir) {
        if (dir == null || !dir.isDirectory()) return null;
        File exact = new File(dir, "main.2.com.ps3recomp.legomarvel.obb");
        if (exact.isFile()) return exact;
        File[] files = dir.listFiles();
        File fallback = null;
        if (files != null) {
            Arrays.sort(files);
            for (File f : files) {
                String n = f.getName().toLowerCase(Locale.US);
                if (f.isFile() && n.endsWith(".obb")) {
                    if (n.startsWith("main.")) return f;
                    if (fallback == null) fallback = f;
                }
            }
        }
        return fallback;
    }

    private File findInstalledObbFile() {
        File f = findObbInDir(getObbDir());
        if (f != null) return f;
        return findObbInDir(new File("/storage/emulated/0/Android/obb/com.ps3recomp.legomarvel"));
    }

    private File findFallbackObbFile() {
        File dl = new File(DOWNLOAD_DIR, "main.2.com.ps3recomp.legomarvel.obb");
        if (dl.isFile()) return dl;
        File gr = new File(GAME_ROOT, "main.2.com.ps3recomp.legomarvel.obb");
        if (gr.isFile()) return gr;
        return null;
    }

    private File findActiveObbFile() {
        File f = findInstalledObbFile();
        if (f != null) return f;
        return findFallbackObbFile();
    }

    private void refreshStatus() {
        File activeObb = findActiveObbFile();
        boolean permOk = hasStoragePermission();
        if (activeObb != null) {
            double gb = activeObb.length() / (1024.0 * 1024.0 * 1024.0);
            statusBadgeText.setText(String.format(Locale.US,
                    "Mode Data : OBB SIAP (%.2f GB)\nFile OBB  : %s\nStorage   : %s\nLog Dir   : %s",
                    gb,
                    activeObb.getName(),
                    permOk ? "Diizinkan (All Files)" : "OBB Native Ready",
                    SHARED_LOGS));
        } else {
            statusBadgeText.setText(String.format(Locale.US,
                    "Mode Data : OBB TIDAK DITEMUKAN\nLetakkan  : Android/obb/com.ps3recomp.legomarvel/\nStorage   : %s\nLog Dir   : %s",
                    permOk ? "Diizinkan (All Files)" : "PERLU IZIN / OBB",
                    SHARED_LOGS));
        }

        File latest = new File(SHARED_LOGS, "latest.log");
        File crash  = new File(SHARED_LOGS, "crash_last.log");
        SimpleDateFormat sdf = new SimpleDateFormat("HH:mm:ss", Locale.US);
        StringBuilder sb = new StringBuilder();
        if (latest.isFile()) {
            sb.append("latest.log     : ").append(latest.length() / 1024).append(" KB (")
              .append(sdf.format(new Date(latest.lastModified()))).append(")\n");
        } else {
            sb.append("latest.log     : belum ada\n");
        }
        if (crash.isFile()) {
            sb.append("crash_last.log : TERDETEKSI CRASH (")
              .append(sdf.format(new Date(crash.lastModified()))).append(")");
            logStatusText.setTextColor(Color.parseColor("#FF7B72"));
        } else {
            sb.append("crash_last.log : Bersih (tidak ada crash)");
            logStatusText.setTextColor(COL_MUTED);
        }
        logStatusText.setText(sb.toString());
    }

    private File driversRootDir() {
        File dir = new File(getFilesDir(), "drivers");
        if (!dir.exists()) dir.mkdirs();
        return dir;
    }

    private void autoImportFirstRunDrivers() {
        File[] existing = driversRootDir().listFiles();
        if (existing == null || existing.length == 0) {
            scanDownloadsForDrivers(false);
        }
    }

    private void scanDownloadsForDrivers(boolean showToast) {
        if (!hasStoragePermission()) {
            if (showToast) {
                Toast.makeText(this, "Berikan izin All Files Access terlebih dahulu", Toast.LENGTH_LONG).show();
                requestStoragePermission();
            }
            return;
        }
        File dl = new File(DOWNLOAD_DIR);
        File[] files = dl.listFiles();
        int imported = 0;
        String lastId = null;
        if (files != null) {
            Arrays.sort(files);
            for (File f : files) {
                String n = f.getName().toLowerCase(Locale.US);
                if (f.isFile() && n.endsWith(".zip") &&
                        (n.contains("turnip") || n.contains("adreno") || n.contains("vulkan") || n.contains("driver"))) {
                    try (InputStream is = new FileInputStream(f)) {
                        String id = importDriverZipStream(is, f.getName());
                        if (id != null) {
                            imported++;
                            lastId = id;
                        }
                    } catch (Exception ignored) {
                    }
                }
            }
        }
        refreshDriverList(lastId);
        if (showToast) {
            Toast.makeText(this,
                    imported > 0 ? ("Berhasil impor " + imported + " driver dari Download")
                                 : "Tidak ada paket driver Vulkan (.zip) baru di Download",
                    Toast.LENGTH_SHORT).show();
        }
    }

    private void openDriverPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/zip");
        startActivityForResult(intent, REQ_PICK_DRIVER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_DRIVER && resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try (InputStream is = getContentResolver().openInputStream(uri)) {
                String id = importDriverZipStream(is, "custom_" + System.currentTimeMillis());
                if (id != null) {
                    refreshDriverList(id);
                    saveSettingsFromUi();
                    Toast.makeText(this, "Driver berhasil diimpor!", Toast.LENGTH_SHORT).show();
                } else {
                    Toast.makeText(this, "Zip bukan paket driver AdrenoTools valid (tidak ada *.so)", Toast.LENGTH_LONG).show();
                }
            } catch (Exception e) {
                Toast.makeText(this, "Gagal impor driver: " + e.getMessage(), Toast.LENGTH_LONG).show();
            }
        }
    }

    private String importDriverZipStream(InputStream rawIn, String fallbackName) throws Exception {
        Map<String, byte[]> entries = new HashMap<>();
        try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(rawIn))) {
            ZipEntry ze;
            byte[] buf = new byte[16384];
            while ((ze = zis.getNextEntry()) != null) {
                if (ze.isDirectory()) continue;
                String base = new File(ze.getName()).getName();
                if (!base.endsWith(".so") && !base.equals("meta.json")) continue;
                ByteArrayOutputStream bos = new ByteArrayOutputStream();
                int r;
                while ((r = zis.read(buf)) > 0) bos.write(buf, 0, r);
                entries.put(base, bos.toByteArray());
            }
        }

        String libName = null;
        String dispName = fallbackName.replace(".zip", "");
        String detail = "Custom Vulkan";

        if (entries.containsKey("meta.json")) {
            JSONObject meta = new JSONObject(new String(entries.get("meta.json"), "UTF-8"));
            if (meta.has("name")) dispName = meta.getString("name");
            if (meta.has("libraryName")) libName = meta.getString("libraryName");
            String vendor = meta.optString("vendor", "");
            String ver = meta.optString("driverVersion", "");
            if (!vendor.isEmpty() || !ver.isEmpty()) detail = (vendor + " " + ver).trim();
        }
        if (libName == null) {
            for (String k : entries.keySet()) {
                if (k.endsWith(".so")) { libName = k; break; }
            }
        }
        if (libName == null || !entries.containsKey(libName)) return null;

        String id = dispName.replaceAll("[^A-Za-z0-9._-]", "_");
        File outDir = new File(driversRootDir(), id);
        if (!outDir.exists()) outDir.mkdirs();

        for (Map.Entry<String, byte[]> e : entries.entrySet()) {
            File outFile = new File(outDir, e.getKey());
            try (FileOutputStream fos = new FileOutputStream(outFile)) {
                fos.write(e.getValue());
            }
            if (e.getKey().endsWith(".so")) {
                outFile.setReadable(true, false);
                outFile.setExecutable(true, false);
            }
        }
        if (!entries.containsKey("meta.json")) {
            JSONObject meta = new JSONObject();
            meta.put("name", dispName);
            meta.put("vendor", detail);
            meta.put("libraryName", libName);
            try (FileWriter fw = new FileWriter(new File(outDir, "meta.json"))) {
                fw.write(meta.toString(2));
            }
        }
        return id;
    }

    private void refreshDriverList(String selectId) {
        String prevId = selectId;
        if (prevId == null && driverSpinner != null && driverSpinner.getSelectedItem() instanceof DriverEntry) {
            prevId = ((DriverEntry) driverSpinner.getSelectedItem()).id;
        }
        drivers.clear();

        DriverEntry sys = new DriverEntry();
        sys.id = "system";
        sys.name = "System Vulkan (Stock Adreno)";
        sys.detail = "Default Perangkat";
        sys.isSystem = true;
        drivers.add(sys);

        File[] dirs = driversRootDir().listFiles();
        if (dirs != null) {
            Arrays.sort(dirs);
            for (File d : dirs) {
                if (!d.isDirectory()) continue;
                File metaFile = new File(d, "meta.json");
                String name = d.getName();
                String detail = "Custom";
                String lib = null;
                if (metaFile.isFile()) {
                    try {
                        StringBuilder sb = new StringBuilder();
                        try (BufferedReader br = new BufferedReader(new FileReader(metaFile))) {
                            String line;
                            while ((line = br.readLine()) != null) sb.append(line);
                        }
                        JSONObject meta = new JSONObject(sb.toString());
                        name = meta.optString("name", name);
                        lib = meta.optString("libraryName", null);
                        String v = meta.optString("vendor", "");
                        String dv = meta.optString("driverVersion", "");
                        detail = (v + " " + dv).trim();
                    } catch (Exception ignored) {
                    }
                }
                if (lib == null) {
                    File[] kids = d.listFiles();
                    if (kids != null) {
                        for (File k : kids) {
                            if (k.getName().endsWith(".so")) { lib = k.getName(); break; }
                        }
                    }
                }
                if (lib != null && new File(d, lib).isFile()) {
                    DriverEntry e = new DriverEntry();
                    e.id = d.getName();
                    e.name = name;
                    e.detail = detail;
                    e.dir = d.getAbsolutePath();
                    e.lib = lib;
                    e.isSystem = false;
                    drivers.add(e);
                }
            }
        }
        driverAdapter.notifyDataSetChanged();
        if (prevId != null) {
            for (int i = 0; i < drivers.size(); i++) {
                if (drivers.get(i).id.equals(prevId)) {
                    driverSpinner.setSelection(i);
                    break;
                }
            }
        }
    }

    private void deleteSelectedDriver() {
        if (!(driverSpinner.getSelectedItem() instanceof DriverEntry)) return;
        DriverEntry e = (DriverEntry) driverSpinner.getSelectedItem();
        if (e.isSystem) {
            Toast.makeText(this, "Driver sistem tidak bisa dihapus", Toast.LENGTH_SHORT).show();
            return;
        }
        File d = new File(e.dir);
        File[] kids = d.listFiles();
        if (kids != null) for (File k : kids) k.delete();
        d.delete();
        refreshDriverList("system");
        saveSettingsFromUi();
        Toast.makeText(this, "Driver dihapus: " + e.name, Toast.LENGTH_SHORT).show();
    }

    private Map<String, String> readIni() {
        Map<String, String> map = new HashMap<>();
        File[] cands = { new File(SHARED_CFG), new File(getFilesDir(), "settings.ini") };
        for (File f : cands) {
            if (!f.isFile()) continue;
            try (BufferedReader br = new BufferedReader(new FileReader(f))) {
                String line;
                while ((line = br.readLine()) != null) {
                    line = line.trim();
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    int eq = line.indexOf('=');
                    if (eq > 0) {
                        map.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                    }
                }
                break;
            } catch (Exception ignored) {
            }
        }
        return map;
    }

    private void loadSettingsIntoUi() {
        Map<String, String> m = readIni();
        String res = m.getOrDefault("resolution", "720p");
        for (int i = 0; i < resValues.length; i++) if (resValues[i].equals(res)) resSpinner.setSelection(i);

        String fps = m.getOrDefault("fps_limit", "60");
        for (int i = 0; i < fpsValues.length; i++) if (fpsValues[i].equals(fps)) fpsSpinner.setSelection(i);

        String scaling = m.getOrDefault("scaling_mode", "fsr");
        for (int i = 0; i < scalingValues.length; i++) if (scalingValues[i].equals(scaling)) scalingSpinner.setSelection(i);

        swTurbo.setChecked("1".equals(m.getOrDefault("adreno_turbo", "0")));
        swGuestShaders.setChecked(!"0".equals(m.getOrDefault("guest_shaders", "1")));
        swDirectPresent.setChecked(!"0".equals(m.getOrDefault("direct_present", "1")));
        swShowFps.setChecked(!"0".equals(m.getOrDefault("show_fps", "1")));
        swSpuInterp.setChecked(!"0".equals(m.getOrDefault("spu_interp_fallback", "1")));
        swSkipFmv.setChecked("1".equals(m.getOrDefault("skip_fmv", "0")));
        swTouch.setChecked(!"0".equals(m.getOrDefault("touch_controls", "1")));
        swVerbose.setChecked("1".equals(m.getOrDefault("verbose_log", "0")));

        String mode = m.getOrDefault("driver_mode", "system");
        String dir = m.getOrDefault("driver_dir", "");
        if ("custom".equals(mode) && !dir.isEmpty()) {
            for (int i = 0; i < drivers.size(); i++) {
                if (!drivers.get(i).isSystem && dir.equals(drivers.get(i).dir)) {
                    driverSpinner.setSelection(i);
                    break;
                }
            }
        } else {
            driverSpinner.setSelection(0);
        }
    }

    private void saveSettingsFromUi() {
        DriverEntry drv = (DriverEntry) driverSpinner.getSelectedItem();
        if (drv == null) drv = drivers.get(0);

        StringBuilder sb = new StringBuilder();
        sb.append("# LEGO Marvel Super Heroes (BLUS31161) Android Launcher Settings\n");
        sb.append("resolution=").append(resValues[resSpinner.getSelectedItemPosition()]).append("\n");
        sb.append("fps_limit=").append(fpsValues[fpsSpinner.getSelectedItemPosition()]).append("\n");
        sb.append("scaling_mode=").append(scalingValues[scalingSpinner.getSelectedItemPosition()]).append("\n");
        sb.append("guest_shaders=").append(swGuestShaders.isChecked() ? "1" : "0").append("\n");
        sb.append("direct_present=").append(swDirectPresent.isChecked() ? "1" : "0").append("\n");
        sb.append("show_fps=").append(swShowFps.isChecked() ? "1" : "0").append("\n");
        sb.append("spu_interp_fallback=").append(swSpuInterp.isChecked() ? "1" : "0").append("\n");
        sb.append("skip_fmv=").append(swSkipFmv.isChecked() ? "1" : "0").append("\n");
        sb.append("touch_controls=").append(swTouch.isChecked() ? "1" : "0").append("\n");
        sb.append("verbose_log=").append(swVerbose.isChecked() ? "1" : "0").append("\n");
        sb.append("adreno_turbo=").append(swTurbo.isChecked() ? "1" : "0").append("\n");
        sb.append("hook_lib_dir=").append(getApplicationInfo().nativeLibraryDir).append("\n");
        if (drv.isSystem) {
            sb.append("driver_mode=system\n");
            sb.append("driver_name=").append(drv.name).append("\n");
        } else {
            sb.append("driver_mode=custom\n");
            sb.append("driver_name=").append(drv.name).append("\n");
            sb.append("driver_dir=").append(drv.dir).append("\n");
            sb.append("driver_lib=").append(drv.lib).append("\n");
        }

        String content = sb.toString();
        writeTextQuietly(new File(getFilesDir(), "settings.ini"), content);
        File shared = new File(SHARED_CFG);
        if (shared.getParentFile() != null) shared.getParentFile().mkdirs();
        writeTextQuietly(shared, content);
    }

    private void writeTextQuietly(File f, String txt) {
        try (FileWriter fw = new FileWriter(f)) {
            fw.write(txt);
        } catch (Exception ignored) {
        }
    }

    private void resetDefaults() {
        driverSpinner.setSelection(0);
        resSpinner.setSelection(0);
        fpsSpinner.setSelection(0);
        scalingSpinner.setSelection(0);
        swTurbo.setChecked(false);
        swGuestShaders.setChecked(true);
        swDirectPresent.setChecked(true);
        swShowFps.setChecked(true);
        swSpuInterp.setChecked(true);
        swSkipFmv.setChecked(false);
        swTouch.setChecked(true);
        swVerbose.setChecked(false);
        saveSettingsFromUi();
        Toast.makeText(this, "Pengaturan dikembalikan ke default", Toast.LENGTH_SHORT).show();
    }

    private void launchGame() {
        boolean hasNativeObb = findObbInDir(getObbDir()) != null;
        if (!hasStoragePermission() && !hasNativeObb) {
            Toast.makeText(this, "Pasang file OBB di Android/obb/com.ps3recomp.legomarvel/ atau aktifkan izin All Files Access", Toast.LENGTH_LONG).show();
            requestStoragePermission();
            return;
        }
        if (findActiveObbFile() == null) {
            Toast.makeText(this, "File OBB tidak ditemukan! Taruh main.2.com.ps3recomp.legomarvel.obb di Android/obb/com.ps3recomp.legomarvel/", Toast.LENGTH_LONG).show();
            return;
        }
        saveSettingsFromUi();
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra("touch_controls", swTouch.isChecked());
        intent.putExtra("show_fps", swShowFps.isChecked());
        startActivity(intent);
    }

    private void showLogDialog(String title, File file) {
        String text;
        if (!file.isFile()) {
            text = "File belum ada:\n" + file.getAbsolutePath();
        } else {
            try {
                long len = file.length();
                long maxRead = 32768;
                try (FileInputStream fis = new FileInputStream(file)) {
                    if (len > maxRead) fis.skip(len - maxRead);
                    byte[] data = new byte[(int) Math.min(len, maxRead)];
                    int read = 0;
                    while (read < data.length) {
                        int n = fis.read(data, read, data.length - read);
                        if (n <= 0) break;
                        read += n;
                    }
                    text = (len > maxRead ? "... (menampilkan 32 KB terakhir) ...\n\n" : "")
                            + new String(data, 0, read, "UTF-8");
                }
            } catch (Exception e) {
                text = "Gagal membaca log: " + e.getMessage();
            }
        }

        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(COL_BG);
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextColor(COL_TEXT);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        tv.setPadding(dp(12), dp(12), dp(12), dp(12));
        tv.setTextIsSelectable(true);
        sv.addView(tv);

        new AlertDialog.Builder(this)
                .setTitle(title)
                .setView(sv)
                .setPositiveButton("Tutup", null)
                .show();
    }
}
