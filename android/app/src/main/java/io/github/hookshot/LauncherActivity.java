package io.github.hookshot;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.OpenableColumns;
import android.system.Os;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The start screen. The game runs from a folder, as on a PC: image.bin (in the APK, copied out once) and the romfs of the
 * player's own .nup (unpacked once, cpp/nup_extract.c). A GPU driver (a Turnip .so, or the .zip it comes in) is copied to
 * the app's own storage, the one place code may be loaded from, and named to the game through the environment
 * (host/posix/win32_posix.c LoadLibraryA). The last run's log is shown, since a crash takes the whole app with it.
 */
public class LauncherActivity extends Activity {
    static { System.loadLibrary("hookshot_setup"); }

    static native String nativeExtract(int fd, String outDir);
    static native long nativeDone();
    static native long nativeTotal();
    static native void nativeCancel();

    private static final int PICK_NUP = 1, PICK_DRIVER = 2;
    private static final long NEED_BYTES = 28L << 30;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private SharedPreferences prefs;
    private File game, drivers;
    private TextView gameText, driverText, logText;
    private Button nupButton, driverButton, systemButton, launchButton;
    private volatile boolean busy;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences("hookshot", MODE_PRIVATE);
        game = getExternalFilesDir(null);
        drivers = new File(getFilesDir(), "driver");
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(48, 32, 48, 32);
        TextView title = text(box, "Hookshot", 24);
        title.setGravity(Gravity.CENTER_HORIZONTAL);
        gameText = text(box, "", 16);
        nupButton = button(box, "Select your .nup file", v -> pick(PICK_NUP));
        driverText = text(box, "", 16);
        driverButton = button(box, "Select a GPU driver (.so or .zip)", v -> pick(PICK_DRIVER));
        systemButton = button(box, "Use the system driver", v -> { prefs.edit().remove("driver").apply(); refresh(); });
        launchButton = button(box, "Launch", v -> launch());
        button(box, "Copy the log", v -> ((ClipboardManager) getSystemService(CLIPBOARD_SERVICE))
                .setPrimaryClip(ClipData.newPlainText("hookshot log", logText.getText())));
        button(box, "Save the full log to Downloads", v -> saveLog());
        logText = text(box, "", 11);
        logText.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(box);
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        new Thread(() -> { String log = lastLog(); ui.post(() -> logText.setText(log)); }).start();
    }

    private TextView text(LinearLayout box, String s, int size) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(size);
        t.setPadding(0, 16, 0, 8);
        box.addView(t);
        return t;
    }

    private Button button(LinearLayout box, String s, android.view.View.OnClickListener on) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(on);
        box.addView(b);
        return b;
    }

    private boolean romfsReady() { return new File(game, "romfs.ok").isFile(); }

    private void refresh() {
        String driver = prefs.getString("driver", null);
        if (driver != null && !new File(drivers, driver).isFile()) driver = null;
        gameText.setText(romfsReady() ? "Game: unpacked and ready (" + game + ")"
                : "Game: not unpacked yet. Select your .nup (version 131072); it is unpacked once, about 27 GB.");
        driverText.setText("GPU driver: " + (driver != null ? driver : "the system's"));
        nupButton.setEnabled(!busy);
        driverButton.setEnabled(!busy);
        systemButton.setEnabled(!busy && driver != null);
        launchButton.setEnabled(!busy && romfsReady());
    }

    private void pick(int what) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, what);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        if (request == PICK_NUP) unpack(data.getData());
        if (request == PICK_DRIVER) installDriver(data.getData());
    }

    private String nameOf(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception e) { /* (a provider with no names: the path's end) */ }
        String p = uri.getLastPathSegment();
        return p == null ? "file" : p.substring(p.lastIndexOf('/') + 1);
    }

    private void unpack(Uri uri) {
        long free = new StatFs(game.getPath()).getAvailableBytes();
        if (!new File(game, "romfs").isDirectory() && free < NEED_BYTES) {
            gameText.setText("Not enough free storage: " + (free >> 30) + " GB free, about 28 GB needed.");
            return;
        }
        final int fd;
        try {
            ParcelFileDescriptor p = getContentResolver().openFileDescriptor(uri, "r");
            fd = p.detachFd();
        } catch (Exception e) {
            gameText.setText("Cannot open that file: " + e.getMessage());
            return;
        }
        busy = true;
        refresh();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new File(game, "romfs.ok").delete();
        Runnable tick = new Runnable() {
            @Override public void run() {
                if (!busy) return;
                long done = nativeDone(), total = nativeTotal();
                gameText.setText(total > 0 ? "Unpacking: " + (done >> 20) + " of " + (total >> 20) + " MB (" + (done * 100 / total) + "%). Keep the app open."
                        : "Reading the .nup...");
                ui.postDelayed(this, 500);
            }
        };
        ui.post(tick);
        new Thread(() -> {
            String err = nativeExtract(fd, game.getPath());
            if (err == null) {
                try { new File(game, "romfs.ok").createNewFile(); } catch (Exception e) { err = "cannot write to " + game; }
            }
            final String shown = err;
            ui.post(() -> {
                busy = false;
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                refresh();
                if (shown != null) gameText.setText("The unpack stopped: " + shown + "\nSelecting the .nup again carries on from where it was.");
            });
        }).start();
    }

    private static void copy(InputStream in, File to) throws Exception {
        try (OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
        }
    }

    /** A driver .so, or a driver .zip (its .so files; meta.json's libraryName names the one to load) */
    private void installDriver(Uri uri) {
        String name = nameOf(uri), chosen = null;
        try {
            File[] old = drivers.listFiles();
            if (old != null) for (File f : old) f.delete();
            drivers.mkdirs();
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (name.toLowerCase().endsWith(".zip")) {
                    String first = null, meta = null;
                    ZipInputStream zip = new ZipInputStream(in);
                    for (ZipEntry e; (e = zip.getNextEntry()) != null;) {
                        String n = new File(e.getName()).getName();
                        if (e.isDirectory() || n.isEmpty()) continue;
                        if (n.endsWith(".so")) {
                            copy(zip, new File(drivers, n));
                            if (first == null) first = n;
                        } else if (n.equals("meta.json")) {
                            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
                            byte[] buf = new byte[4096];
                            for (int k; (k = zip.read(buf)) > 0;) b.write(buf, 0, k);
                            Matcher m = Pattern.compile("\"libraryName\"\\s*:\\s*\"([^\"]+)\"").matcher(b.toString("UTF-8"));
                            if (m.find()) meta = m.group(1);
                        }
                    }
                    chosen = meta != null && new File(drivers, meta).isFile() ? meta : first;
                } else if (name.endsWith(".so")) {
                    copy(in, new File(drivers, name));
                    chosen = name;
                }
            }
        } catch (Exception e) {
            driverText.setText("That driver could not be read: " + e.getMessage());
            return;
        }
        if (chosen == null) {
            driverText.setText("No driver was found in " + name + " (a .so, or a .zip holding one).");
            return;
        }
        prefs.edit().putString("driver", chosen).apply();
        refresh();
    }

    /** image.bin out of the APK, once per installed build */
    private void placeImage() throws Exception {
        long stamp = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        File image = new File(game, "image.bin"), mark = new File(game, "image.stamp");
        if (image.isFile() && mark.isFile() && mark.length() == Long.toString(stamp).length()) {
            try (BufferedReader r = new BufferedReader(new java.io.FileReader(mark))) {
                if (Long.toString(stamp).equals(r.readLine())) return;
            }
        }
        try (InputStream in = getAssets().open("image.bin")) { copy(in, image); }
        try (OutputStream o = new FileOutputStream(mark)) { o.write(Long.toString(stamp).getBytes()); }
    }

    private void launch() {
        busy = true;
        refresh();
        gameText.setText("Starting...");
        new Thread(() -> {
            String err = null;
            try {
                placeImage();
                String driver = prefs.getString("driver", null);
                Os.setenv("HOOKSHOT_DIR", game.getPath(), true);
                if (driver != null && new File(drivers, driver).isFile()) {
                    Os.setenv("HOOKSHOT_VK_DRIVER", driver, true);
                    Os.setenv("HOOKSHOT_VK_DRIVER_DIR", drivers.getPath() + "/", true);
                    Os.setenv("HOOKSHOT_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir + "/", true);
                } else {
                    Os.unsetenv("HOOKSHOT_VK_DRIVER");
                }
            } catch (Exception e) {
                err = e.toString();
            }
            final String shown = err;
            ui.post(() -> {
                busy = false;
                refresh();
                if (shown != null) { gameText.setText("Could not start: " + shown); return; }
                startActivity(new Intent(this, HookshotActivity.class));
            });
        }).start();
    }

    private static String tail(File f, int bytes) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long n = Math.min(bytes, r.length());
            byte[] b = new byte[(int) n];
            r.seek(r.length() - n);
            r.readFully(b);
            return new String(b, "UTF-8");
        } catch (Exception e) {
            return "(none)";
        }
    }

    /** The log's lines about the renderer, the window and memory (its start: what a black screen is explained by), the first of them */
    private String rendererLines(File f) {
        StringBuilder s = new StringBuilder();
        int n = 0;
        try (BufferedReader r = new BufferedReader(new java.io.FileReader(f))) {
            for (String line; (line = r.readLine()) != null && n < 120;) {
                if (line.contains("vk:") || line.contains("display:") || line.contains("mem:") || line.contains("hookshot v")
                        || line.contains("cannot") || line.contains("WARNING")) {
                    s.append(line.length() > 320 ? line.substring(0, 320) : line).append('\n');
                    n++;
                }
            }
        } catch (Exception e) {
            return "(none)\n";
        }
        return n == 0 ? "(none)\n" : s.toString();
    }

    /** hookshot_log.txt whole (its first 2 MB and last 300 KB when it is longer) as a file in Downloads, to send on */
    private void saveLog() {
        new Thread(() -> {
            String said;
            File f = new File(game, "hookshot_log.txt");
            try {
                android.content.ContentValues v = new android.content.ContentValues();
                v.put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, "hookshot_log_" + (System.currentTimeMillis() / 1000) + ".txt");
                v.put(android.provider.MediaStore.MediaColumns.MIME_TYPE, "text/plain");
                Uri to = getContentResolver().insert(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
                final long head = 2L << 20, last = 300L << 10;
                try (OutputStream out = getContentResolver().openOutputStream(to); RandomAccessFile r = new RandomAccessFile(f, "r")) {
                    byte[] buf = new byte[1 << 16];
                    long len = r.length(), left = Math.min(len, head);
                    for (int k; left > 0 && (k = r.read(buf, 0, (int) Math.min(buf.length, left))) > 0; left -= k) out.write(buf, 0, k);
                    if (len > head + last) {
                        out.write("\n[... the middle of the log is left out ...]\n".getBytes());
                        r.seek(len - last);
                    }
                    for (int k; r.getFilePointer() < len && (k = r.read(buf)) > 0;) out.write(buf, 0, k);
                    out.write(("\n" + logText.getText()).getBytes("UTF-8"));
                }
                said = "Saved to Downloads (hookshot_log_....txt).";
            } catch (Exception e) {
                said = "The log could not be saved: " + e;
            }
            final String shown = said;
            ui.post(() -> driverText.setText(shown));
        }).start();
    }

    /** What the last run left: the game's own log, and what Android logged for this app (its crashes too) */
    private String lastLog() {
        File log = new File(game, "hookshot_log.txt");
        StringBuilder s = new StringBuilder("--- renderer, window, memory (from the start of the last run) ---\n").append(rendererLines(log));
        s.append("--- hookshot_log.txt (the end of the last run) ---\n").append(tail(log, 4000));
        s.append("\n--- Android's log for this app ---\n");
        try {
            Process p = Runtime.getRuntime().exec(new String[] {"logcat", "-d", "-t", "250", "-v", "brief", "hookshot:V", "SDL:V", "AndroidRuntime:E", "linker:W", "libc:F", "DEBUG:F", "*:S"});
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                for (String line; (line = r.readLine()) != null;) s.append(line).append('\n');
            }
        } catch (Exception e) {
            s.append("(not readable: ").append(e).append(")\n");
        }
        return s.toString();
    }
}
