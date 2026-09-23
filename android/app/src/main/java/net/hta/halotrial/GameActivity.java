package net.hta.halotrial;

import android.app.NativeActivity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Bitmap;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.view.DisplayCutout;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.EditText;
import android.text.InputType;

import java.util.ArrayList;
import java.util.List;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.util.Enumeration;
import java.net.InetAddress;

/**
 * NativeActivity plus a COD-Mobile-style touch HUD: visible stick, a large
 * fire button, and jump. Look is anywhere that isn't a button.
 *
 * The game runs fullscreen: status bar and navigation bar hidden, and the
 * surface extended under the display cutout. A swipe from an edge brings the
 * bars back transiently, then they hide again.
 */
public class GameActivity extends NativeActivity {
    static {
        System.loadLibrary("hta_native");
    }

    private HudOverlay hud;
    private final ShellMenu shell = new ShellMenu(this);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        goFullscreen();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            /* Transient bars come back after a system gesture, a notification
             * shade pull, or returning from the recents screen. Re-hide every
             * time focus returns, or the game ends up letterboxed again. */
            goFullscreen();
            getWindow().getDecorView().post(this::attachHud);
        }
    }

    /** Status bar and nav bar hidden; surface extended under the cutout. */
    private void goFullscreen() {
        Window w = getWindow();
        if (w == null) return;

        /* Let the surface reach the short edges, under the camera hole.
         * Without this the renderer is letterboxed away from the cutout and
         * the HUD has a dead strip down one side. */
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams a = w.getAttributes();
            a.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            w.setAttributes(a);
        }

        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
            WindowInsetsController ctl = w.getInsetsController();
            if (ctl != null) {
                ctl.hide(WindowInsets.Type.systemBars());
                ctl.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            w.getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                  | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    @Override
    protected void onDestroy() {
        detachHud();
        super.onDestroy();
        if (returnToMenu) {
            Intent i = new Intent(getApplicationContext(), SetupActivity.class);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            getApplicationContext().startActivity(i);
        }
    }

    private void attachHud() {
        if (hud != null || getWindow() == null || getWindow().getDecorView().getWindowToken() == null)
            return;
        hud = new HudOverlay(this);
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                /* No FLAG_LAYOUT_INSET_DECOR: that insets the overlay by the
                 * system bars, which would park the HUD inside a frame the
                 * game no longer has. */
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        lp.token = getWindow().getDecorView().getWindowToken();
        lp.setTitle("hta-hud");
        if (Build.VERSION.SDK_INT >= 28) {
            lp.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        try {
            getWindowManager().addView(hud, lp);
            nativeHudReady(true);
        } catch (RuntimeException e) {
            hud = null;
            nativeHudReady(false);
        }
    }

    private void detachHud() {
        if (hud == null) return;
        try { getWindowManager().removeView(hud); } catch (RuntimeException ignored) {}
        hud = null;
        nativeHudReady(false);
    }

    static native void nativeHudReady(boolean ready);
    static native void nativeHudMove(float x, float y);
    static native void nativeHudLook(float dx, float dy);
    static native void nativeHudJump(boolean down);
    static native void nativeHudFire(boolean down);
    static native void nativeHudCrouch(boolean down);
    static native void nativeHudReload();
    static native void nativeHudMelee();
    static native void nativeHudSwap();
    static native void nativeHudZoom();
    static native void nativeHudGrenade();
    /* Debug actions, by number rather than one entry point each, so adding
     * the next one is a case in the switch and nothing else.
     *   0  hand over the next weapon in the cache's roster
     */
    static native void nativeHudDebug(int action);
    static native String nativeDebugText();
    static native String nativeAmmoText();
    static native int nativeVehicleMode();
    static native String nativeVehicleText();
    static native void nativeHudAlt(boolean down);
    static native int nativeDamageFlash();
    static native int nativeNetStatus();
    /* Banner, place, kill feed and scoreboard, separated by 0x1E. */
    static native String nativeGameText();
    /* 1 while the main menu is up: the overlay draws no controls and hands
     * touches to the menu instead. */
    static native int nativeMenuMode();
    static native int nativePaused();
    static native void nativeResume();
    static native void nativePause();

    private boolean returnToMenu;

    /** Pause screen: leave this game for the main menu. The new activity
     *  starts only after this one's native thread has exited, so two game
     *  threads never share the native state. */
    void backToMenu() {
        returnToMenu = true;
        finish();
    }
    static native void nativeMenuTouch(int action, float x, float y);
    static native String nativeShellText();
    static native int[] nativeShellArt(int which);
    static native void nativeShellScreen(int screen);
    static native void nativeShellSound(int which);
    static native void nativeStartMatch(int[] config, String host, String name);
    static native String nativeLanScan(String targets, int port, int milliseconds);

    public void openSolo() { runOnUiThread(() -> { shell.open(2); if (hud != null) hud.invalidate(); }); }
    public void openMultiplayer() { runOnUiThread(() -> { shell.open(1); if (hud != null) hud.invalidate(); }); }

    static volatile boolean creditsUp;

    /** Called from native when SETTINGS is chosen. */
    public void openSettings() {
        runOnUiThread(() -> {
            Intent i = new Intent(this, SetupActivity.class);
            i.putExtra("settings", true);
            startActivity(i);
            finish();
        });
    }

    /** Called from native when CREDITS is chosen. */
    public void showCredits() {
        creditsUp = true;
    }

    private static final class ShellMenu {
        private final GameActivity owner;
        private final Paint panel = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint row = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint title = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Bitmap[] art = new Bitmap[16];
        private final boolean[] artRead = new boolean[16];
        private String[] words;
        private int screen;
        private int bots = 3, skill = 1, kills = 25, minutes = 0, respawn = 5;
        /* hta_game_mode: free-for-all Slayer, Team Slayer, CTF. Solo only
         * until the LAN snapshot carries teams and flags. */
        private int gametype = 0, captures = 3;
        private static final String[] GAMETYPES = { "SLAYER", "TEAM SLAYER", "CAPTURE THE FLAG" };
        /* HTA_VROSTER_*: none, the map's defaults, all, or one kind. */
        private int vehicles = 2;
        private static final String[] VEHICLE_SETS = { "NONE", "DEFAULT", "ALL",
                "WARTHOGS", "GHOSTS", "SCORPIONS", "ROCKET WARTHOGS", "BANSHEES" };
        private int maxPlayers = 8, port = 32270;
        private String serverName = "Halo", address = "";
        private String[] lanGames = new String[0];
        private boolean scanning;

        ShellMenu(GameActivity owner) {
            this.owner = owner;
            panel.setColor(0xC8102038);
            row.setColor(0xAA234567);
            title.setColor(0xFFE3F2FF);
            title.setTypeface(Typeface.DEFAULT_BOLD);
            title.setTextAlign(Paint.Align.CENTER);
            text.setColor(0xFFFFFFFF);
            text.setTextAlign(Paint.Align.CENTER);
        }

        void open(int next) {
            screen = next;
            nativeShellScreen(next == 2 ? 1 : 2);
            nativeShellSound(1);
            if (words == null) {
                String raw = nativeShellText();
                words = raw == null ? new String[0] : raw.split("\u001e", -1);
            }
            if (next == 5) scan();
        }

        private String word(int n, String fallback) {
            return n < words.length && !words[n].isEmpty() ? words[n] : fallback;
        }

        private Bitmap image(int n) {
            if (artRead[n]) return art[n];
            artRead[n] = true;
            int[] data = nativeShellArt(n);
            if (data == null || data.length < 3) return null;
            int w = data[0], h = data[1];
            if (w < 1 || h < 1 || (long) w * h != data.length - 2) return null;
            art[n] = Bitmap.createBitmap(data, 2, w, w, h, Bitmap.Config.ARGB_8888);
            return art[n];
        }

        private String heading() {
            switch (screen) {
            case 1: return word(46, "MULTIPLAYER");
            case 2: return "SINGLEPLAYER";
            case 3: return "CREATE GAME";
            case 4: return word(0, "JOIN GAME");
            case 5: return "LAN GAMES";
            default: return "INTERNET GAME";
            }
        }

        private String[] rows() {
            switch (screen) {
            case 1: return new String[] { word(1, "CREATE GAME"), word(0, "JOIN GAME"), word(18, "BACK") };
            case 2: return new String[] { "GAME: " + GAMETYPES[gametype],
                    "BOTS: " + bots, "BOT SKILL: " + skillName(),
                    gametype == 2 ? "CAPTURES TO WIN: " + (captures == 0 ? "NONE" : captures)
                                  : word(21, "KILLS TO WIN") + " " + (kills == 0 ? "NONE" : kills),
                    "TIME LIMIT: " + (minutes == 0 ? "NONE" : minutes + " MIN"),
                    word(22, "RESPAWN TIME") + " " + respawn + " SEC",
                    "VEHICLES: " + VEHICLE_SETS[vehicles],
                    word(12, "START GAME"), word(18, "BACK") };
            case 3: return new String[] { word(10, "SERVER NAME") + ": " + serverName,
                    word(11, "MAX PLAYERS") + ": " + maxPlayers,
                    "GAME: " + GAMETYPES[gametype],
                    "BOTS: " + bots, "BOT SKILL: " + skillName(),
                    gametype == 2 ? "CAPTURES TO WIN: " + (captures == 0 ? "NONE" : captures)
                                  : word(21, "KILLS TO WIN") + " " + (kills == 0 ? "NONE" : kills),
                    "TIME LIMIT: " + (minutes == 0 ? "NONE" : minutes + " MIN"),
                    word(22, "RESPAWN TIME") + " " + respawn + " SEC",
                    "VEHICLES: " + VEHICLE_SETS[vehicles],
                    word(12, "START GAME"), word(18, "BACK") };
            case 4: return new String[] { word(3, "LAN"), word(2, "INTERNET") + " / DIRECT IP", word(18, "BACK") };
            case 5: {
                int count = Math.min(5, lanGames.length);
                String[] r = new String[count + 2];
                for (int i = 0; i < count; i++) {
                    String[] f = lanGames[i].split("\t", -1);
                    r[i] = f.length >= 5 ? f[2] + "  " + f[3] + "/" + f[4] +
                            (gameFull(f) ? "  FULL" : "  " + f[0]) : lanGames[i];
                }
                r[count] = scanning ? "SCANNING..." : word(30, "REFRESH");
                r[count + 1] = word(18, "BACK");
                return r;
            }
            default: return new String[] { word(16, "SERVER ADDRESS") + ": " +
                    (address.isEmpty() ? "TAP TO ENTER" : address), word(31, "JOIN GAME"), word(18, "BACK") };
            }
        }

        private String skillName() {
            String[] fallback = { "Easy", "Normal", "Heroic", "Legendary" };
            return word(26 + skill, fallback[skill]);
        }

        void draw(Canvas c, int w, int h) {
            if (screen == 0) return;
            String[] r = rows();
            float scale = Math.min(w, h);
            float left = w * 0.18f, right = w * 0.82f;
            c.drawRoundRect(left, h * 0.08f, right, h * 0.94f, 18f, 18f, panel);
            int headArt = screen == 1 ? 0 : screen == 3 ? 4 : screen == 5 ? 1 : screen == 6 ? 2 : -1;
            Bitmap header = headArt >= 0 ? image(headArt) : null;
            if (header != null) {
                float aspect = (float) header.getWidth() / header.getHeight();
                float hh = h * 0.11f;
                c.drawBitmap(header, null, new Rect((int)(w * 0.5f - hh * aspect * 0.5f),
                        (int)(h * 0.10f), (int)(w * 0.5f + hh * aspect * 0.5f),
                        (int)(h * 0.10f + hh)), null);
            } else {
                title.setTextSize(scale * 0.075f);
                c.drawText(heading(), w * 0.5f, h * 0.20f, title);
            }
            float step = 0.65f / Math.max(8, r.length);
            text.setTextSize(scale * (r.length >= 7 ? 0.044f : 0.055f));
            for (int i = 0; i < r.length; i++) {
                float y = h * (0.255f + i * step);
                Bitmap background = image(8);
                if (background != null) c.drawBitmap(background, null,
                        new Rect((int)left + 12, (int)y, (int)right - 12, (int)(y + h * step * 0.86f)), null);
                c.drawRoundRect(left + 12, y, right - 12, y + h * step * 0.86f, 8, 8, row);
                c.drawText(r[i], w * 0.5f, y + h * step * 0.58f, text);
            }
            if (screen == 5 && !scanning && lanGames.length == 0) {
                text.setTextSize(scale * 0.035f);
                c.drawText("No LAN games found. Tap REFRESH to scan again.",
                        w * 0.5f, h * 0.77f, text);
            }
        }

        void drawMainSolo(Canvas c, int w, int h) {
            // The Trial calls this slot CAMPAIGN. Until campaign maps work,
            // its action is a configurable solo Slayer match.
            row.setColor(0xE0193457);
            c.drawRoundRect(w * 0.35f, h * 0.515f, w * 0.65f, h * 0.583f,
                    8, 8, row);
            row.setColor(0xAA234567);
            title.setTextSize(Math.min(w, h) * 0.055f);
            c.drawText("SINGLEPLAYER", w * 0.5f, h * 0.563f, title);
        }

        void tap(float x, float y) {
            if (screen == 0 || x < 0.18f || x > 0.82f) return;
            String[] r = rows();
            float step = 0.65f / Math.max(8, r.length);
            int i = (int)((y - 0.255f) / step);
            if (y < 0.255f || i < 0 || i >= r.length || y > 0.255f + (i + 0.86f) * step) return;
            nativeShellSound(1);
            switch (screen) {
            case 1:
                if (i == 0) open(3); else if (i == 1) open(4); else back();
                break;
            case 2:
                if (i == 0) gametype = (gametype + 1) % GAMETYPES.length;
                else if (i == 1) bots = (bots + 1) % 8;
                else if (i == 2) skill = (skill + 1) % 4;
                else if (i == 3) {
                    if (gametype == 2) captures = next(captures, new int[] { 1, 3, 5, 10, 0 });
                    else kills = next(kills, new int[] { 0, 10, 25, 50, 100 });
                }
                else if (i == 4) minutes = next(minutes, new int[] { 0, 10, 15, 20, 30, 45 });
                else if (i == 5) respawn = next(respawn, new int[] { 2, 5, 10, 15 });
                else if (i == 6) vehicles = (vehicles + 1) % VEHICLE_SETS.length;
                else if (i == 7) start(0, ""); else back();
                break;
            case 3:
                if (i == 0) edit(false);
                else if (i == 1) maxPlayers = maxPlayers == 8 ? 2 : maxPlayers + 1;
                else if (i == 2) gametype = (gametype + 1) % GAMETYPES.length;
                else if (i == 3) bots = (bots + 1) % 8;
                else if (i == 4) skill = (skill + 1) % 4;
                else if (i == 5) {
                    if (gametype == 2) captures = next(captures, new int[] { 1, 3, 5, 10, 0 });
                    else kills = next(kills, new int[] { 0, 10, 25, 50, 100 });
                }
                else if (i == 6) minutes = next(minutes, new int[] { 0, 10, 15, 20, 30, 45 });
                else if (i == 7) respawn = next(respawn, new int[] { 2, 5, 10, 15 });
                else if (i == 8) vehicles = (vehicles + 1) % VEHICLE_SETS.length;
                else if (i == 9) start(1, "127.0.0.1"); else back();
                break;
            case 4:
                if (i == 0) open(5); else if (i == 1) open(6); else back();
                break;
            case 5:
                if (i < r.length - 2) {
                    String[] f = lanGames[i].split("\t", -1);
                    if (f.length >= 2) {
                        if (gameFull(f)) {
                            new AlertDialog.Builder(owner).setMessage("This game is full. Refresh to find an open game.")
                                    .setPositiveButton("OK", null).show();
                            break;
                        }
                        try { port = Integer.parseInt(f[1]); } catch (NumberFormatException ignored) { port = 32270; }
                        start(2, f[0]);
                    }
                } else if (i == r.length - 2) scan(); else back();
                break;
            case 6:
                if (i == 0) edit(true);
                else if (i == 1) {
                    if (validIPv4(address)) start(2, address);
                    else new AlertDialog.Builder(owner).setMessage("Enter a valid server IPv4 address.")
                            .setPositiveButton("OK", null).show();
                }
                else if (i == 2) back();
                break;
            default: break;
            }
            if (owner.hud != null) owner.hud.invalidate();
        }

        private int next(int current, int[] values) {
            for (int i = 0; i < values.length; i++)
                if (values[i] == current) return values[(i + 1) % values.length];
            return values[0];
        }

        private boolean gameFull(String[] fields) {
            if (fields.length < 5) return false;
            try { return Integer.parseInt(fields[3]) >= Integer.parseInt(fields[4]); }
            catch (NumberFormatException ignored) { return false; }
        }

        private boolean validIPv4(String value) {
            String[] parts = value.split("\\.", -1);
            if (parts.length != 4) return false;
            for (String part : parts) {
                if (part.isEmpty() || part.length() > 3) return false;
                for (int k = 0; k < part.length(); k++)
                    if (part.charAt(k) < '0' || part.charAt(k) > '9') return false;
                if (Integer.parseInt(part) > 255) return false;
            }
            return true;
        }

        private void start(int mode, String host) {
            // A joiner plays whatever the host chose; the host's GAME says.
            int type = mode == 2 ? 0 : gametype;
            nativeStartMatch(new int[] { mode, bots, skill, type == 2 ? captures : kills, minutes, respawn,
                    maxPlayers, port, vehicles, type }, host, serverName);
            screen = 0;
        }

        private void back() {
            nativeShellSound(2);
            if (screen == 1 || screen == 2) { screen = 0; nativeShellScreen(0); }
            else if (screen == 3 || screen == 4) open(1);
            else open(4);
        }

        private void edit(boolean ip) {
            EditText input = new EditText(owner);
            input.setSingleLine(true);
            input.setInputType(ip ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI
                    : InputType.TYPE_CLASS_TEXT);
            input.setText(ip ? address : serverName);
            new AlertDialog.Builder(owner).setTitle(ip ? "Server IPv4 address" : "Server name")
                    .setView(input).setPositiveButton("OK", (d, which) -> {
                        String value = input.getText().toString().trim();
                        if (ip) address = value; else if (!value.isEmpty()) serverName = value;
                        if (owner.hud != null) owner.hud.invalidate();
                    }).setNegativeButton("Cancel", null).show();
        }

        private void scan() {
            if (scanning) return;
            scanning = true;
            lanGames = new String[0];
            new Thread(() -> {
                StringBuilder targets = new StringBuilder("255.255.255.255");
                try {
                    Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
                    while (interfaces != null && interfaces.hasMoreElements()) {
                        NetworkInterface iface = interfaces.nextElement();
                        if (!iface.isUp() || iface.isLoopback()) continue;
                        for (InterfaceAddress ia : iface.getInterfaceAddresses()) {
                            InetAddress broadcast = ia.getBroadcast();
                            if (broadcast != null) targets.append(',').append(broadcast.getHostAddress());
                        }
                    }
                } catch (Exception ignored) { }
                String result = nativeLanScan(targets.toString(), 32270, 1500);
                owner.runOnUiThread(() -> {
                    lanGames = result == null || result.isEmpty() ? new String[0] : result.split("\n");
                    scanning = false;
                    if (owner.hud != null) owner.hud.invalidate();
                });
            }, "halo-lan-scan").start();
        }
    }

    private static final class HudOverlay extends View {
        private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint thumb = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fireP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint jumpP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint crouchP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint reloadP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint meleeP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint swapP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint zoomP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint debug = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint ammo = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint dbgP = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint banner = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint feed = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint board = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint boardBg = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint damage = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint waypoint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint waypointText = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final android.graphics.Path marker = new android.graphics.Path();

        private float stickCx, stickCy, stickR, stickTx, stickTy;
        private float fireCx, fireCy, fireR;
        private float jumpCx, jumpCy, jumpR;
        private float crouchCx, crouchCy, crouchR;
        private float reloadCx, reloadCy, reloadR;
        private float meleeCx, meleeCy, meleeR;
        private float swapCx, swapCy, swapR;
        private float zoomCx, zoomCy, zoomR;
        private float nadeCx, nadeCy, nadeR;
        /* A debug pad on the left edge, clear of the stick below it and the
         * readout above it. Deliberately small and dull -- it is not part of
         * the game. */
        private float dbgCx, dbgCy, dbgR;
        private float pauseCx, pauseCy, pauseR;
        private int stickPtr = -1, firePtr = -1, jumpPtr = -1, crouchPtr = -1;
        private int reloadPtr = -1, meleePtr = -1, swapPtr = -1, zoomPtr = -1;
        private int nadePtr = -1, dbgPtr = -1;
        private final float[] lastX = new float[16];
        private final float[] lastY = new float[16];

        private final GameActivity owner;
        private final Paint pauseBtn = new Paint(Paint.ANTI_ALIAS_FLAG);

        HudOverlay(GameActivity a) {
            super(a);
            owner = a;
            pauseBtn.setColor(0xCC1C3A66);
            setClickable(true);
            ring.setStyle(Paint.Style.STROKE);
            ring.setStrokeWidth(4f);
            ring.setColor(0x66FFFFFF);
            fill.setColor(0x22000000);
            thumb.setColor(0x99FFFFFF);
            fireP.setColor(0xCCE23B3B);
            jumpP.setColor(0xCC2B6CF6);
            crouchP.setColor(0xCC555C66);
            reloadP.setColor(0xCCB08420);
            meleeP.setColor(0xCC6E3BA8);
            swapP.setColor(0xCC2E7D6B);
            zoomP.setColor(0xCC3C5A8C);
            dbgP.setColor(0x99202830);
            label.setColor(0xFFFFFFFF);
            ammo.setColor(0xF2FFFFFF);
            ammo.setTextAlign(Paint.Align.RIGHT);
            ammo.setTypeface(Typeface.DEFAULT_BOLD);
            debug.setColor(0xCC00FF88);
            label.setTextAlign(Paint.Align.CENTER);
            label.setTypeface(Typeface.DEFAULT_BOLD);
            /* Halo's HUD messages are its pale blue. */
            banner.setColor(0xFFB8D8FF);
            banner.setTextAlign(Paint.Align.CENTER);
            banner.setTypeface(Typeface.DEFAULT_BOLD);
            banner.setShadowLayer(4f, 0f, 2f, 0xFF000000);
            feed.setColor(0xFFDDE8F5);
            feed.setShadowLayer(3f, 0f, 1f, 0xFF000000);
            board.setColor(0xFFE6E9EF);
            board.setTypeface(Typeface.MONOSPACE);
            boardBg.setColor(0xB0101820);
            damage.setColor(0xFFFF3030);
        }

        @Override
        protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            float m = Math.min(w, h);
            stickR = m * 0.13f;
            stickCx = w * 0.16f;
            stickCy = h * 0.78f;
            stickTx = stickCx;
            stickTy = stickCy;
            fireR = m * 0.095f;
            fireCx = w * 0.84f;
            fireCy = h * 0.62f;
            jumpR = m * 0.07f;
            jumpCx = w * 0.91f;
            jumpCy = h * 0.84f;
            crouchR = m * 0.062f;
            crouchCx = w * 0.78f;
            crouchCy = h * 0.86f;
            reloadR = m * 0.058f;
            reloadCx = w * 0.665f;
            reloadCy = h * 0.90f;
            meleeR = m * 0.058f;
            meleeCx = w * 0.70f;
            meleeCy = h * 0.72f;
            swapR = m * 0.058f;
            swapCx = w * 0.60f;
            swapCy = h * 0.90f;
            zoomR = m * 0.058f;
            zoomCx = w * 0.625f;
            zoomCy = h * 0.72f;
            nadeR = m * 0.058f;
            nadeCx = w * 0.725f;
            nadeCy = h * 0.90f;
            pauseR = m * 0.04f;
            pauseCx = w * 0.955f;
            pauseCy = h * 0.09f;
            dbgR = m * 0.045f;
            dbgCx = w * 0.035f;
            dbgCy = h * 0.42f;
            label.setTextSize(m * 0.032f);
            banner.setTextSize(m * 0.062f);
            feed.setTextSize(m * 0.034f);
            board.setTextSize(m * 0.038f);
            ammo.setTextSize(m * 0.085f);
            excludeFromSystemGestures();
        }

        /* With the navigation bar hidden, an edge swipe is a system gesture
         * (back / home), not a look or a stick drag. Claim the edges the
         * controls actually sit on. The platform caps how much of an edge an
         * app may take, and ignores the excess; these rects are well inside
         * that, and the top of each edge is left to the system. */
        private void excludeFromSystemGestures() {
            if (Build.VERSION.SDK_INT < 29) return;
            int w = getWidth(), h = getHeight();
            if (w <= 0 || h <= 0) return;
            List<Rect> rects = new ArrayList<>(2);
            rects.add(new Rect(0, h / 2, (int) (stickCx + stickR * 1.4f), h));
            rects.add(new Rect((int) (crouchCx - crouchR * 1.4f), h / 2, w, h));
            setSystemGestureExclusionRects(rects);
        }

        private static boolean in(float x, float y, float cx, float cy, float r) {
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r;
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (GameActivity.nativeMenuMode() != 0) {
                int a = e.getActionMasked();
                if (creditsUp) {
                    if (a == MotionEvent.ACTION_UP) creditsUp = false;
                    invalidate();
                    return true;
                }
                if (owner.shell.screen != 0) {
                    if (a == MotionEvent.ACTION_UP && getWidth() > 0 && getHeight() > 0)
                        owner.shell.tap(e.getX() / getWidth(), e.getY() / getHeight());
                    return true;
                }
                int code = a == MotionEvent.ACTION_DOWN ? 0
                         : a == MotionEvent.ACTION_MOVE ? 1
                         : (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) ? 2 : -1;
                if (code >= 0 && getWidth() > 0 && getHeight() > 0)
                    GameActivity.nativeMenuTouch(code, e.getX() / getWidth(), e.getY() / getHeight());
                return true;
            }
            if (GameActivity.nativePaused() != 0) {
                if (e.getActionMasked() == MotionEvent.ACTION_UP) {
                    float x = e.getX(), y = e.getY(), w = getWidth(), h = getHeight();
                    if (x > w * 0.35f && x < w * 0.65f) {
                        if (y > h * 0.44f && y < h * 0.56f) GameActivity.nativeResume();
                        else if (y > h * 0.60f && y < h * 0.72f) owner.backToMenu();
                    }
                }
                invalidate();
                return true;
            }
            int action = e.getActionMasked();
            int idx = e.getActionIndex();
            int id = e.getPointerId(idx);
            float x = e.getX(idx), y = e.getY(idx);

            switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                remember(id, x, y);
                if (in(x, y, fireCx, fireCy, fireR * 1.15f) && firePtr < 0) {
                    firePtr = id;
                    GameActivity.nativeHudFire(true);
                } else if (in(x, y, jumpCx, jumpCy, jumpR * 1.15f) && jumpPtr < 0) {
                    jumpPtr = id;
                    GameActivity.nativeHudJump(true);
                } else if (in(x, y, crouchCx, crouchCy, crouchR * 1.15f) && crouchPtr < 0) {
                    crouchPtr = id;
                    GameActivity.nativeHudCrouch(true);
                } else if (in(x, y, reloadCx, reloadCy, reloadR * 1.15f) && reloadPtr < 0) {
                    reloadPtr = id;
                    GameActivity.nativeHudReload();
                } else if (in(x, y, meleeCx, meleeCy, meleeR * 1.15f) && meleePtr < 0) {
                    meleePtr = id;
                    GameActivity.nativeHudMelee();
                } else if (in(x, y, swapCx, swapCy, swapR * 1.15f) && swapPtr < 0) {
                    swapPtr = id;
                    GameActivity.nativeHudSwap();
                } else if (in(x, y, zoomCx, zoomCy, zoomR * 1.15f) && zoomPtr < 0) {
                    zoomPtr = id;
                    GameActivity.nativeHudZoom();
                } else if (in(x, y, pauseCx, pauseCy, pauseR * 1.3f)) {
                    GameActivity.nativePause();
                } else if (in(x, y, dbgCx, dbgCy, dbgR * 1.25f) && dbgPtr < 0) {
                    dbgPtr = id;
                    GameActivity.nativeHudDebug(0);
                } else if (in(x, y, nadeCx, nadeCy, nadeR * 1.15f) && nadePtr < 0) {
                    nadePtr = id;
                    /* In a vehicle with a second gun, NADE is that trigger,
                     * held like FIRE: the Scorpion's machine gun, the
                     * Banshee's fuel rod. */
                    nadeAlt = (GameActivity.nativeVehicleMode() & 16) != 0;
                    if (nadeAlt) GameActivity.nativeHudAlt(true);
                    else GameActivity.nativeHudGrenade();
                } else if ((in(x, y, stickCx, stickCy, stickR * 1.4f) || x < getWidth() * 0.38f)
                        && stickPtr < 0) {
                    stickPtr = id;
                    updateStick(x, y);
                }
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int pid = e.getPointerId(i);
                    float px = e.getX(i), py = e.getY(i);
                    if (pid == stickPtr) updateStick(px, py);
                    else {
                        /* Fire, jump, and empty-space drags all look so you
                         * can aim while holding FIRE (second finger or drag). */
                        float dx = px - lastOf(pid, true, px);
                        float dy = py - lastOf(pid, false, py);
                        if (dx != 0f || dy != 0f)
                            GameActivity.nativeHudLook(dx, dy);
                    }
                    remember(pid, px, py);
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (action == MotionEvent.ACTION_CANCEL) {
                    releaseStick();
                    releaseFire();
                    releaseJump();
                    releaseCrouch();
                    reloadPtr = -1;
                    meleePtr = -1;
                    swapPtr = -1;
                    zoomPtr = -1;
                    releaseNade();
                    dbgPtr = -1;
                } else {
                    if (id == stickPtr) releaseStick();
                    if (id == firePtr) releaseFire();
                    if (id == jumpPtr) releaseJump();
                    if (id == crouchPtr) releaseCrouch();
                    if (id == reloadPtr) reloadPtr = -1;
                    if (id == meleePtr) meleePtr = -1;
                    if (id == swapPtr) swapPtr = -1;
                    if (id == zoomPtr) zoomPtr = -1;
                    if (id == nadePtr) releaseNade();
                    if (id == dbgPtr) dbgPtr = -1;
                }
                break;
            default:
                break;
            }
            invalidate();
            return true;
        }

        private void remember(int id, float x, float y) {
            int i = ((id % 16) + 16) % 16;
            lastX[i] = x;
            lastY[i] = y;
        }

        private float lastOf(int id, boolean x, float fallback) {
            int i = ((id % 16) + 16) % 16;
            return x ? lastX[i] : lastY[i];
        }

        private void updateStick(float x, float y) {
            float dx = x - stickCx, dy = y - stickCy;
            float len = (float) Math.hypot(dx, dy);
            if (len > stickR) {
                dx *= stickR / len;
                dy *= stickR / len;
                len = stickR;
            }
            stickTx = stickCx + dx;
            stickTy = stickCy + dy;
            float nx = dx / stickR, ny = dy / stickR;
            GameActivity.nativeHudMove(nx, -ny);
        }

        private void releaseStick() {
            stickPtr = -1;
            stickTx = stickCx;
            stickTy = stickCy;
            GameActivity.nativeHudMove(0f, 0f);
        }

        private void releaseFire() {
            firePtr = -1;
            GameActivity.nativeHudFire(false);
        }

        private void releaseJump() {
            jumpPtr = -1;
            GameActivity.nativeHudJump(false);
        }

        private void releaseCrouch() {
            crouchPtr = -1;
            GameActivity.nativeHudCrouch(false);
        }

        private boolean nadeAlt;
        private void releaseNade() {
            nadePtr = -1;
            if (nadeAlt) GameActivity.nativeHudAlt(false);
            nadeAlt = false;
        }

        private void drawCredits(Canvas c) {
            int w = getWidth(), h = getHeight();
            c.drawRect(w * 0.15f, h * 0.12f, w * 0.85f, h * 0.88f, boardBg);
            String[] lines = {
                "HALO: MP",
                "",
                "Halo: Combat Evolved by Bungie; PC Trial by Gearbox Software.",
                "Every map, model, sound and word here comes from your own",
                "copy of the Trial, read at runtime.",
                "",
                "This engine: C, Vulkan and AAudio, GPLv3.",
                "Tag layouts from Invader. Ogg Vorbis by stb_vorbis.",
                "",
                "Tap to return."
            };
            float y = h * 0.22f;
            for (String l : lines) {
                c.drawText(l, w * 0.5f, y, l.startsWith("HALO") ? banner : label);
                y += label.getTextSize() * 1.9f;
            }
        }

        /* The game's words: the announcer's banner across the middle, where
         * you stand along the top, who killed whom down the left, and at the
         * end of a game the scoreboard. */
        private void drawGame(Canvas c) {
            String g = null;
            try { g = nativeGameText(); } catch (Throwable ignored) { }
            if (g == null || g.isEmpty()) return;
            String[] part = g.split("\u001e", -1);
            int w = getWidth(), h = getHeight();
            if (part.length > 0 && !part[0].isEmpty())
                c.drawText(part[0], w * 0.5f, h * 0.30f, banner);
            if (part.length > 1 && !part[1].isEmpty()) {
                float saved = banner.getTextSize();
                banner.setTextSize(feed.getTextSize());
                c.drawText(part[1], w * 0.5f, h * 0.075f, banner);
                banner.setTextSize(saved);
            }
            if (part.length > 2 && !part[2].isEmpty()) {
                String[] lines = part[2].split("\n");
                float y = h * 0.20f;
                for (String l : lines) {
                    if (l.isEmpty()) continue;
                    c.drawText(l, w * 0.09f, y, feed);
                    y += feed.getTextSize() * 1.3f;
                }
            }
            if (part.length > 4 && !part[4].isEmpty()) {
                float saved = banner.getTextSize();
                banner.setTextSize(feed.getTextSize() * 1.1f);
                c.drawText(part[4], w * 0.5f, h * 0.64f, banner);
                banner.setTextSize(saved);
            }
            if (part.length > 5 && !part[5].isEmpty()) drawWaypoints(c, part[5], w, h);
            if (part.length > 3 && !part[3].isEmpty()) {
                String[] rows = part[3].split("\n");
                float rowH = board.getTextSize() * 1.45f;
                float top = h * 0.38f, left = w * 0.22f, right = w * 0.78f;
                c.drawRect(left, top - rowH, right, top + rowH * (rows.length + 0.4f), boardBg);
                float[] col = { left + w * 0.015f, left + w * 0.07f, left + w * 0.30f,
                                left + w * 0.38f, left + w * 0.46f, left + w * 0.54f };
                String[] head = { "Place", "Name", "Score", "Kills", "Assists", "Deaths" };
                for (int k = 0; k < head.length; k++) c.drawText(head[k], col[k], top, board);
                float y = top + rowH;
                for (String r : rows) {
                    String[] f = r.split("\t");
                    for (int k = 0; k < f.length && k < col.length; k++) c.drawText(f[k], col[k], y, board);
                    y += rowH;
                }
            }
        }

        /* A flag's waypoint: a downward chevron in its team's colour over
         * where it is, the distance under it; off screen, pinned to the
         * edge. "x,y,team,metres,onscreen,state;" per flag. */
        private void drawWaypoints(Canvas c, String spec, int w, int h) {
            float size = Math.min(w, h) * 0.028f;
            waypointText.setTextAlign(Paint.Align.CENTER);
            waypointText.setTextSize(size * 1.05f);
            waypointText.setFakeBoldText(true);
            for (String one : spec.split(";")) {
                String[] f = one.split(",");
                if (f.length < 6) continue;
                float x, y; int team, state; String metres;
                try {
                    x = Float.parseFloat(f[0]) * w; y = Float.parseFloat(f[1]) * h;
                    team = Integer.parseInt(f[2]); state = Integer.parseInt(f[5]);
                    metres = f[3];
                } catch (NumberFormatException e) { continue; }
                int colour = team == 0 ? 0xFFE0402E : 0xFF3E7BFF;
                // A flag away from home blinks: somebody has it or it is lying out.
                int alpha = state == 0 ? 0xD0 : ((System.currentTimeMillis() / 300) % 2 == 0 ? 0xFF : 0x90);
                waypoint.setColor((colour & 0x00FFFFFF) | (alpha << 24));
                waypoint.setStyle(Paint.Style.FILL);
                marker.reset();
                marker.moveTo(x - size, y - size * 1.2f);
                marker.lineTo(x + size, y - size * 1.2f);
                marker.lineTo(x, y);
                marker.close();
                c.drawPath(marker, waypoint);
                waypointText.setColor(0xE0FFFFFF);
                c.drawText(metres + "m", x, y + size * 1.3f, waypointText);
            }
        }

        @Override
        protected void onDraw(Canvas c) {
            if (GameActivity.nativeMenuMode() != 0) {
                if (creditsUp) drawCredits(c);
                else if (owner.shell.screen != 0) owner.shell.draw(c, getWidth(), getHeight());
                else owner.shell.drawMainSolo(c, getWidth(), getHeight());
                postInvalidateDelayed(100);
                return;
            }
            if (GameActivity.nativePaused() != 0) {
                /* Halo's pause: the world held behind a veil, two choices. */
                int w = getWidth(), h = getHeight();
                c.drawRect(0, 0, w, h, boardBg);
                c.drawText("PAUSED", w * 0.5f, h * 0.34f, banner);
                c.drawRect(w * 0.35f, h * 0.44f, w * 0.65f, h * 0.56f, pauseBtn);
                c.drawRect(w * 0.35f, h * 0.60f, w * 0.65f, h * 0.72f, pauseBtn);
                c.drawText("RESUME GAME", w * 0.5f, h * 0.515f, label);
                c.drawText("QUIT TO MAIN MENU", w * 0.5f, h * 0.675f, label);
                drawGame(c);
                postInvalidateDelayed(100);
                return;
            }
            int vehicleMode = GameActivity.nativeVehicleMode();
            int netStatus = GameActivity.nativeNetStatus();
            if (netStatus != 0) {
                String connection = netStatus == 1 ? "CONNECTING TO GAME..." :
                        netStatus == 2 ? "CONNECTED" :
                        netStatus == 3 ? "HOSTING · WAITING FOR PLAYER" :
                        netStatus == 4 ? "HOSTING · PLAYER JOINED" :
                        netStatus == 6 ? "WAITING FOR MATCH STATE" :
                        netStatus == 7 ? "MAPS DO NOT MATCH" :
                        netStatus == 8 ? "GAME IS FULL" : "NETWORK UNAVAILABLE";
                c.drawText(connection, getWidth() * 0.5f, getHeight() * 0.135f, label);
            }
            int hit = GameActivity.nativeDamageFlash();
            if (hit > 0) {
                int w = getWidth(), h = getHeight();
                float edge = Math.min(w, h) * 0.035f;
                damage.setStyle(Paint.Style.FILL);
                damage.setAlpha(hit * 35 / 255);
                c.drawRect(0, 0, w, h, damage);
                damage.setStyle(Paint.Style.STROKE);
                damage.setStrokeWidth(edge);
                damage.setAlpha(hit * 190 / 255);
                c.drawRect(edge * 0.5f, edge * 0.5f,
                        w - edge * 0.5f, h - edge * 0.5f, damage);
                damage.setStyle(Paint.Style.FILL);
            }
            int seatMode = vehicleMode & 15;
            if (vehicleMode != 0) {
                String vt = null;
                try { vt = nativeVehicleText(); } catch (Throwable ignored) { }
                if (vt != null && !vt.isEmpty()) {
                    float savedSize = label.getTextSize();
                    label.setTextSize(Math.min(getWidth(), getHeight()) * 0.030f);
                    c.drawText(vt, getWidth() * 0.5f, getHeight() * 0.18f, label);
                    label.setTextSize(savedSize);
                }
            }
            c.drawCircle(stickCx, stickCy, stickR, fill);
            c.drawCircle(stickCx, stickCy, stickR, ring);
            c.drawCircle(stickTx, stickTy, stickR * 0.38f, thumb);

            c.drawCircle(pauseCx, pauseCy, pauseR, dbgP);
            c.drawCircle(pauseCx, pauseCy, pauseR, ring);
            c.drawText("II", pauseCx, pauseCy + label.getTextSize() * 0.35f, label);
            c.drawCircle(dbgCx, dbgCy, dbgR, dbgP);
            c.drawCircle(dbgCx, dbgCy, dbgR, ring);
            c.drawText("DBG", dbgCx, dbgCy + label.getTextSize() * 0.35f, label);

            c.drawCircle(fireCx, fireCy, fireR, fireP);
            c.drawCircle(fireCx, fireCy, fireR, ring);
            c.drawText("FIRE", fireCx, fireCy + label.getTextSize() * 0.35f, label);

            c.drawCircle(jumpCx, jumpCy, jumpR, jumpP);
            c.drawCircle(jumpCx, jumpCy, jumpR, ring);
            c.drawText(seatMode == 2 ? "BRAKE" : "JUMP", jumpCx, jumpCy + label.getTextSize() * 0.35f, label);

            c.drawCircle(crouchCx, crouchCy, crouchR, crouchP);
            c.drawCircle(crouchCx, crouchCy, crouchR, ring);
            c.drawText("CROUCH", crouchCx, crouchCy + label.getTextSize() * 0.35f, label);

            c.drawCircle(reloadCx, reloadCy, reloadR, reloadP);
            c.drawCircle(reloadCx, reloadCy, reloadR, ring);
            c.drawText("RELOAD", reloadCx, reloadCy + label.getTextSize() * 0.35f, label);

            c.drawCircle(meleeCx, meleeCy, meleeR, meleeP);
            c.drawCircle(meleeCx, meleeCy, meleeR, ring);
            c.drawText("MELEE", meleeCx, meleeCy + label.getTextSize() * 0.35f, label);

            c.drawCircle(swapCx, swapCy, swapR, swapP);
            c.drawCircle(swapCx, swapCy, swapR, ring);
            c.drawText(seatMode >= 2 ? "EXIT" :
                    seatMode == 1 ? "GET IN" : "SWAP", swapCx, swapCy + label.getTextSize() * 0.35f, label);

            c.drawCircle(zoomCx, zoomCy, zoomR, zoomP);
            c.drawCircle(zoomCx, zoomCy, zoomR, ring);
            c.drawText("ZOOM", zoomCx, zoomCy + label.getTextSize() * 0.35f, label);
            c.drawCircle(nadeCx, nadeCy, nadeR, zoomP);
            c.drawCircle(nadeCx, nadeCy, nadeR, ring);
            c.drawText((vehicleMode & 16) != 0 ? "ALT" : "NADE", nadeCx, nadeCy + label.getTextSize() * 0.35f, label);

            /* Ammo, big and bottom-right: loaded / reserve, "--" while the
             * magazine is out. */
            String a = null;
            try { a = nativeAmmoText(); } catch (Throwable ignored) { }
            if (a != null && a.length() > 0)
                c.drawText(a, getWidth() - 28f, getHeight() * 0.30f, ammo);

            drawGame(c);

            /* Position readout, so a bug report screenshot carries coordinates.
             * Keep it clear of the camera cutout: the status bar no longer
             * covers these digits, but the punch-hole still would, and an
             * unreadable X is an unmeasurable bug report. */
            String t = null;
            try { t = nativeDebugText(); } catch (Throwable ignored) { }
            if (t != null && t.length() > 0) {
                debug.setTextSize(label.getTextSize() * 0.8f);
                float left = 24f, top = 0f;
                if (Build.VERSION.SDK_INT >= 28) {
                    WindowInsets wi = getRootWindowInsets();
                    DisplayCutout dc = (wi != null) ? wi.getDisplayCutout() : null;
                    if (dc != null) {
                        left = Math.max(left, dc.getSafeInsetLeft() + 16f);
                        top = dc.getSafeInsetTop();
                    }
                }
                c.drawText(t, left, top + debug.getTextSize() * 2.2f, debug);
            }
            postInvalidateDelayed(100);
        }
    }
}
