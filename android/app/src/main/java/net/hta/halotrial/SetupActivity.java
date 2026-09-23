package net.hta.halotrial;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Enumeration;

/**
 * First-run helper: the user picks their own Trial map via the system document
 * picker, we copy it into app-private storage, then NativeActivity loads it
 * with no extra permissions. Android 11+ hides Android/data/ from file
 * managers and All-files access is a special setting, so a picker is the
 * reliable remote-test path.
 */
public class SetupActivity extends Activity {
    private static final String TAG = "halo-trial-android";
    private static final int REQ_PICK_MAP = 1;
    private static final int REQ_PICK_BITM = 2;
    private static final int REQ_PICK_SND = 3;
    private static final int REQ_PICK_UI = 4;

    private TextView status;
    private TextView lanAddress;
    private Button play;
    private EditText serverAddress;
    private Button botsButton, skillButton;
    private int bots = 3, skill = 1;
    /* Halo's own four difficulty names. */
    private static final String[] SKILLS = { "Easy", "Normal", "Heroic", "Legendary" };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF11151C);
        int pad = dp(20);
        root.setPadding(pad, pad, pad, pad);
        root.setGravity(Gravity.CENTER_HORIZONTAL);

        Button pick = btn("Pick bloodgulch.map", 0xFF2B6CF6);
        pick.setOnClickListener(v -> pickFile(REQ_PICK_MAP));
        Button pickBm = btn("Pick bitmaps.map (textures)", 0xFF2B6CF6);
        pickBm.setOnClickListener(v -> pickFile(REQ_PICK_BITM));
        Button pickSnd = btn("Pick sounds.map (audio)", 0xFF2B6CF6);
        pickSnd.setOnClickListener(v -> pickFile(REQ_PICK_SND));
        Button pickUi = btn("Pick ui.map (main menu)", 0xFF2B6CF6);
        pickUi.setOnClickListener(v -> pickFile(REQ_PICK_UI));

        android.content.SharedPreferences prefs = getSharedPreferences("hta", MODE_PRIVATE);
        bots = Math.max(0, Math.min(7, prefs.getInt("bots", 3)));
        skill = Math.max(0, Math.min(3, prefs.getInt("skill", 1)));
        botsButton = btn("", 0xFF3C4656);
        botsButton.setOnClickListener(v -> { bots = (bots + 1) % 8; showBotSettings(); });
        skillButton = btn("", 0xFF3C4656);
        skillButton.setOnClickListener(v -> { skill = (skill + 1) % 4; showBotSettings(); });
        showBotSettings();

        play = btn("Back to main menu", 0xFF2A7A3A);
        play.setOnClickListener(v -> {
            if (existingMap() == null && !builtInData()) { status.setText("Pick a map first."); return; }
            openMenu();
        });
        Button host = btn("Host LAN game", 0xFF2A7A3A);
        host.setOnClickListener(v -> launchGame(false, true));
        serverAddress = new EditText(this);
        serverAddress.setSingleLine(true);
        serverAddress.setHint("Host device IPv4 (example: 192.168.1.10)");
        serverAddress.setTextColor(Color.WHITE);
        serverAddress.setHintTextColor(0xFF94A0B4);
        Button join = btn("Join LAN server", 0xFF2A7A3A);
        join.setOnClickListener(v -> launchGame(true, false));

        status = tv("", 14, 0xFF8FB6FF, false);
        lanAddress = tv("", 14, 0xFF8FB6FF, false);

        root.addView(tv("Halo: MP", 26, 0xFFE6E9EF, true));
        root.addView(space(12));
        root.addView(tv(
                "This app does not bundle any Halo files.\n"
                        + "Pick bloodgulch.map, then bitmaps.map (79 MB), "
                        + "sounds.map (76 MB) and ui.map, from the same Trial maps "
                        + "folder. Without bitmaps.map the world stays untextured; "
                        + "without sounds.map it stays silent; without ui.map there "
                        + "is no main menu.",
                15, 0xFF94A0B4, false));
        root.addView(space(24));
        root.addView(pick);
        root.addView(space(10));
        root.addView(pickBm);
        root.addView(space(10));
        root.addView(pickSnd);
        root.addView(space(10));
        root.addView(pickUi);
        root.addView(space(10));
        root.addView(botsButton);
        root.addView(space(6));
        root.addView(skillButton);
        root.addView(space(10));
        root.addView(play);
        root.addView(space(8));
        root.addView(host);
        root.addView(lanAddress);
        root.addView(space(8));
        root.addView(serverAddress);
        root.addView(join);
        root.addView(space(16));
        root.addView(status);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);

        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        /* With the data in place, the main menu is the front door; this
         * screen is SETTINGS, reached from it. */
        if (!getIntent().getBooleanExtra("settings", false)
                && (builtInData() || existingMap() != null)) {
            openMenu();
        }
    }

    private void openMenu() {
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra("menu", 1);
        i.putExtra("bots", bots);
        i.putExtra("skill", skill);
        startActivity(i);
        finish();
    }

    private void refresh() {
        String ip = localLanAddress();
        lanAddress.setText(ip == null
                ? "Connect to Wi-Fi, then use this phone's IPv4 address to join."
                : "Hosting address: " + ip + ":32270  (same Wi-Fi on both devices)");
        File map = existingMap();
        File bitm = existingBitmaps();
        File snd = existingSounds();
        if (builtInData()) {
            play.setEnabled(true);
            play.setAlpha(1f);
            status.setText("Trial data is built into this APK. Tap Play.");
        } else if (map != null) {
            play.setEnabled(true);
            play.setAlpha(1f);
            String msg = "Map ready: " + map.getName() + " ("
                    + (map.length() / 1024 / 1024) + " MB).";
            msg += bitm != null
                    ? " Textures: bitmaps.map (" + (bitm.length() / 1024 / 1024) + " MB)."
                    : " No bitmaps.map — untextured.";
            msg += snd != null
                    ? " Audio: sounds.map (" + (snd.length() / 1024 / 1024) + " MB)."
                    : " No sounds.map — silent.";
            msg += new File(destDir(), "ui.map").length() > 0
                    ? " Menu: ui.map." : " No ui.map — no main menu.";
            if (bitm != null && snd != null) msg += " Tap Play.";
            status.setText(msg);
        } else {
            play.setEnabled(false);
            play.setAlpha(0.4f);
            status.setText("No map in app storage yet.");
        }
    }

    private String localLanAddress() {
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            if (interfaces == null) return null;
            while (interfaces.hasMoreElements()) {
                NetworkInterface iface = interfaces.nextElement();
                if (!iface.isUp() || iface.isLoopback()) continue;
                String name = iface.getName();
                if (name == null || (!name.startsWith("wlan") && !name.startsWith("ap"))) continue;
                Enumeration<InetAddress> addresses = iface.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    InetAddress address = addresses.nextElement();
                    if (address instanceof Inet4Address && address.isSiteLocalAddress())
                        return address.getHostAddress();
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "Could not read Wi-Fi IPv4 address", e);
        }
        return null;
    }

    private File destDir() {
        File ext = getExternalFilesDir(null);
        return ext != null ? ext : getFilesDir();
    }

    private File existingMap() {
        File dir = destDir();
        File named = new File(dir, "bloodgulch.map");
        if (named.isFile() && named.length() > 0) return named;
        File[] files = dir.listFiles();
        if (files == null) return null;
        for (File f : files) {
            String n = f.getName();
            if (n.length() > 4
                    && n.substring(n.length() - 4).equalsIgnoreCase(".map")
                    && f.isFile() && f.length() > 0
                    && !n.equalsIgnoreCase("bitmaps.map")
                    && !n.equalsIgnoreCase("sounds.map")
                    && !n.equalsIgnoreCase("ui.map")) {
                return f;
            }
        }
        return null;
    }

    /* A personal build carries the owner's own maps, uncompressed, under
     * assets/maps/ (publish_apk.sh --with-assets). openFd only succeeds on an
     * uncompressed asset, which is also what the native side needs. */
    private boolean builtInData() {
        try {
            getAssets().openFd("maps/bloodgulch.map").close();
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private File existingBitmaps() {
        File f = new File(destDir(), "bitmaps.map");
        return (f.isFile() && f.length() > 0) ? f : null;
    }

    private File existingSounds() {
        File f = new File(destDir(), "sounds.map");
        return (f.isFile() && f.length() > 0) ? f : null;
    }

    private void pickFile(int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, req);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_PICK_MAP && requestCode != REQ_PICK_BITM
                && requestCode != REQ_PICK_SND && requestCode != REQ_PICK_UI) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            status.setText("Pick cancelled.");
            return;
        }
        String name = (requestCode == REQ_PICK_BITM) ? "bitmaps.map"
                    : (requestCode == REQ_PICK_SND)  ? "sounds.map"
                    : (requestCode == REQ_PICK_UI)   ? "ui.map"
                    : "bloodgulch.map";
        Uri uri = data.getData();
        File dest = new File(destDir(), name);
        File tmp = new File(destDir(), name + ".part");
        status.setText("Copying " + name + "\u2026");
        try {
            destDir().mkdirs();
            copyUri(uri, tmp);
            if (dest.exists() && !dest.delete()) {
                throw new java.io.IOException("could not replace existing file");
            }
            if (!tmp.renameTo(dest)) {
                throw new java.io.IOException("could not finalize copy");
            }
            Log.i(TAG, "[assets] copied " + dest.getAbsolutePath()
                    + " (" + dest.length() + " bytes)");
            refresh();
        } catch (Exception e) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            Log.e(TAG, "[assets] copy failed", e);
            status.setText("Copy failed: " + e.getMessage());
            Toast.makeText(this, "Copy failed: " + e.getMessage(),
                    Toast.LENGTH_LONG).show();
        }
    }

    private void copyUri(Uri uri, File dest) throws Exception {
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(dest)) {
            if (in == null) throw new java.io.IOException("could not open selected file");
            byte[] buf = new byte[256 * 1024];
            long total = 0;
            int n;
            while ((n = in.read(buf)) >= 0) {
                out.write(buf, 0, n);
                total += n;
            }
            out.flush();
            if (total < 1024) {
                throw new java.io.IOException("file too small (" + total + " bytes)");
            }
        }
    }

    private void launchGame() {
        launchGame(false, false);
    }

    private void launchGame(boolean join, boolean host) {
        if (existingMap() == null && !builtInData()) {
            status.setText("Pick a map first.");
            return;
        }
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra("bots", bots);
        i.putExtra("skill", skill);
        if (join) {
            String ip = serverAddress.getText().toString().trim();
            if (!ip.matches("[0-9.]{7,15}")) {
                status.setText("Enter the desktop server's numeric IPv4 address.");
                return;
            }
            i.putExtra("net_host", ip);
        }
        if (host) {
            i.putExtra("net_host", "127.0.0.1");
            i.putExtra("net_hosting", true);
        }
        startActivity(i);
        finish();
    }

    private void showBotSettings() {
        botsButton.setText(bots == 0 ? "Bots: none (practice alone)" : "Bots: " + bots);
        skillButton.setText("Bot difficulty: " + SKILLS[skill]);
        getSharedPreferences("hta", MODE_PRIVATE).edit()
                .putInt("bots", bots).putInt("skill", skill).apply();
    }

    private TextView tv(String text, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setGravity(Gravity.CENTER_HORIZONTAL);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private Button btn(String text, int bg) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setBackgroundColor(bg);
        b.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        b.setLayoutParams(lp);
        return b;
    }

    private View space(int dps) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(dps)));
        return v;
    }

    private int dp(int dps) {
        return Math.round(dps * getResources().getDisplayMetrics().density);
    }
}
