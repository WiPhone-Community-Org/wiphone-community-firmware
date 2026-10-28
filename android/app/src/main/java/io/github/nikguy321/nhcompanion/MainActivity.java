package io.github.nikguy321.nhcompanion;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.location.LocationManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

/**
 * The one screen: share the GPS on or off, what it is doing, and a setup list that does every
 * step it can by itself ("Set up everything") and opens the right Settings page for the rest.
 */
@android.annotation.SuppressLint("SetTextI18n")   // English only for now; the words live here, not in resources
public final class MainActivity extends Activity {
    static final String EXTRA_START = "start";
    static final String GUIDE_URL =
            "https://github.com/Nikguy321/wiphone-meshtastic/blob/main/docs/phone-location.md";

    private static final int RC_LOCATION = 1;
    private static final int RC_NOTIFY = 2;
    private static final int RC_BACKGROUND = 3;
    private static final int RC_BATTERY = 4;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean night;
    private Switch share;
    private boolean updating;
    private TextView statusTitle;
    private TextView statusBody;
    private TextView details;
    private TextView warnings;
    private Row rowLocation;
    private Row rowNotify;
    private Row rowBattery;
    private Row rowBackground;
    private Row rowHotspot;
    private Row rowStill;
    private CheckBox atBoot;
    private TextView counts;
    private TextView timeline;
    /** "Set up everything" in progress: the step to continue from when Android hands control back. */
    private int setupStep = -1;
    /** Android's notification question was asked during this run of the flow (it may answer at once). */
    private boolean notifyAsked;

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            refresh();
            ui.postDelayed(this, 1000);
        }
    };

    // ------------------------------------------------------------------ life cycle

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        night = (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_YES;
        GpsService.ensureChannels(this);
        debugExtras(getIntent());
        setContentView(buildUi());
        if (saved != null) {
            setupStep = saved.getInt("setupStep", -1);     // rotated mid-setup: carry on from there
        } else {
            resumeFromReminder(getIntent());
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("setupStep", setupStep);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        debugExtras(intent);
        resumeFromReminder(intent);
    }

    /**
     * The "sharing is paused" reminder's tap: start sharing again, once, and only if the user still
     * has it switched on. Another app can open this screen with the same extra, so it can never
     * turn sharing on by itself; and a screen reopened from Recents carries the old extra.
     */
    private void resumeFromReminder(Intent i) {
        if (i == null || !i.getBooleanExtra(EXTRA_START, false)
                || (i.getFlags() & Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0) {
            return;
        }
        i.removeExtra(EXTRA_START);
        if (Prefs.enabled(this)) {
            startSharing();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Sharing is on but the service is not running (killed, or the phone restarted): start it again.
        if (Prefs.enabled(this) && Setup.preciseLocation(this) && !Status.running) {
            GpsService.start(this);
        }
        ui.post(poll);
    }

    @Override
    protected void onPause() {
        ui.removeCallbacks(poll);
        super.onPause();
    }

    /**
     * Debug builds only, from adb: "--ez made true" makes GGA/RMC from Android's fix; "--ez anynet
     * true" lets devices in from any network (the emulator's own side, to test through Doze).
     */
    private void debugExtras(Intent i) {
        if (!BuildConfig.DEBUG || i == null) {
            return;
        }
        if (i.hasExtra("made")) {
            Status.forceMade = i.getBooleanExtra("made", false);
        }
        if (i.hasExtra("anynet")) {
            Status.debugAnyNetwork = i.getBooleanExtra("anynet", false);
        }
    }

    // ------------------------------------------------------------------ sharing

    private void startSharing() {
        if (!Setup.preciseLocation(this)) {
            setupStep = 0;
            continueSetup();
            return;
        }
        GpsService.start(this);
        refresh();
    }

    // ------------------------------------------------------------------ set up everything

    /** One step at a time; each step that needs Android's own screen returns here when done. */
    private void continueSetup() {
        while (setupStep >= 0) {
            switch (setupStep++) {
                case 0:
                    if (!Setup.preciseLocation(this)) {
                        askLocation();
                        return;
                    }
                    break;
                case 1:
                    // Only Android's own question here. Switched off in Settings (or "don't ask again"),
                    // the row's own button opens Settings; the flow does not stop for it.
                    if (Build.VERSION.SDK_INT >= 33
                            && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
                            && !notifyAsked) {
                        notifyAsked = true;
                        requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, RC_NOTIFY);
                        return;
                    }
                    break;
                case 2:
                    if (!Setup.unrestrictedBattery(this)) {
                        askBattery();
                        return;
                    }
                    break;
                case 3:
                    if (Build.VERSION.SDK_INT >= 29 && Setup.preciseLocation(this) && !Setup.backgroundLocation(this)) {
                        askBackground();
                        return;
                    }
                    break;
                case 4:
                    if (Setup.preciseLocation(this)) {
                        GpsService.start(this);
                    }
                    break;
                case 5:
                    if (Setup.samsung() && !Setup.fullGnssTracking(this)) {
                        explainFullGnss();
                        return;
                    }
                    break;
                default:
                    setupStep = -1;
                    if (Setup.preciseLocation(this) && Setup.hotspotAddress(this) == null) {
                        Toast.makeText(this, "Last step: turn on Mobile Hotspot", Toast.LENGTH_LONG).show();
                        openHotspotSettings();
                    }
                    refresh();
                    return;
            }
        }
    }

    /**
     * Samsung's "stop GPS when still" rule (Setup.fullGnssTracking). Apps cannot change that
     * setting, so this explains it and opens Developer options, or About phone when they are off.
     */
    private void explainFullGnss() {
        final boolean dev = Settings.Global.getInt(getContentResolver(), Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, 0) == 1;
        new AlertDialog.Builder(this)
                .setTitle("Keep the GPS on while the phone lies still")
                .setMessage("Samsung phones switch the GPS off for every app a few minutes after the phone is "
                        + "put down with the screen off, unless it is charging. Your devices then lose the position.\n\n"
                        + "The fix is one switch in Developer options: \"Force full GNSS measurements\". It uses a "
                        + "little more battery, only while something uses the GPS.\n\n"
                        + (dev ? "On the next screen, find \"Force full GNSS measurements\" and turn it on."
                        : "Developer options are hidden. On the next screen: Software information → tap "
                        + "\"Build number\" 7 times. Then Settings → Developer options → "
                        + "\"Force full GNSS measurements\". Leave Developer options on afterwards: turning "
                        + "them off resets the switch."))
                .setPositiveButton(dev ? "Open Developer options" : "Open About phone", (d, w) -> {
                    if (!dev || !tryStart(new Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS))) {
                        tryStart(new Intent(Settings.ACTION_DEVICE_INFO_SETTINGS));
                    }
                })
                .setNegativeButton("Not now", (d, w) -> continueSetup())
                .setOnCancelListener(d -> continueSetup())
                .show();
    }

    private void askLocation() {
        requestPermissions(new String[] {Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.ACCESS_COARSE_LOCATION}, RC_LOCATION);
    }

    private void askNotifications() {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, RC_NOTIFY);
        } else {
            openAppNotificationSettings();     // granted, but switched off in Settings
        }
    }

    @android.annotation.SuppressLint("BatteryLife")   // sideloaded, not on Play: the one-tap dialog is the point
    private void askBattery() {
        try {
            final Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:" + getPackageName()));
            startActivityForResult(i, RC_BATTERY);
        } catch (ActivityNotFoundException e) {
            openAppSettings();
            setupStep = -1;
        }
    }

    private void askBackground() {
        new AlertDialog.Builder(this)
                .setTitle("Start after a restart?")
                .setMessage("With location allowed \"all the time\", sharing starts by itself after the phone "
                        + "restarts. Otherwise a notification asks you to tap once.\n\nOn the next screen choose "
                        + "\"Allow all the time\".")
                .setPositiveButton("Continue", (d, w) -> {
                    if (Build.VERSION.SDK_INT >= 29) {
                        requestPermissions(new String[] {Manifest.permission.ACCESS_BACKGROUND_LOCATION}, RC_BACKGROUND);
                    }
                })
                .setNegativeButton("Skip", (d, w) -> continueSetup())
                .setOnCancelListener(d -> continueSetup())
                .show();
    }

    @Override
    public void onRequestPermissionsResult(int rc, String[] perms, int[] results) {
        super.onRequestPermissionsResult(rc, perms, results);
        if (rc == RC_LOCATION && !Setup.preciseLocation(this)) {
            final boolean approximate = checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
                    == PackageManager.PERMISSION_GRANTED;
            if (approximate || !shouldShowRequestPermissionRationale(Manifest.permission.ACCESS_FINE_LOCATION)) {
                // Approximate chosen, or "don't ask again": only Settings can change it now.
                Toast.makeText(this, approximate ? "Turn on \"Use precise location\"" : "Allow location: Permissions > Location",
                        Toast.LENGTH_LONG).show();
                openAppSettings();
            }
            setupStep = -1;
        } else if (rc == RC_NOTIFY && setupStep < 0 && !Setup.notifications(this)
                && !shouldShowRequestPermissionRationale(Manifest.permission.POST_NOTIFICATIONS)) {
            openAppNotificationSettings();     // the row's button, after "don't ask again"
        }
        refresh();
        if (setupStep >= 0) {
            continueSetup();
        }
    }

    @Override
    protected void onActivityResult(int rc, int result, Intent data) {
        super.onActivityResult(rc, result, data);
        refresh();
        if (rc == RC_BATTERY && setupStep >= 0) {
            continueSetup();
        }
    }

    // ------------------------------------------------------------------ Settings pages

    private void openAppSettings() {
        tryStart(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + getPackageName())));
    }

    private void openAppNotificationSettings() {
        if (!tryStart(new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                .putExtra(Settings.EXTRA_APP_PACKAGE, getPackageName()))) {
            openAppSettings();
        }
    }

    /** Android has no public "hotspot settings" page; these are the usual names, best first. */
    private void openHotspotSettings() {
        if (tryStart(new Intent().setClassName("com.android.settings", "com.android.settings.TetherSettings"))) {
            return;
        }
        if (tryStart(new Intent().setClassName("com.android.settings", "com.android.settings.Settings$TetherSettingsActivity"))) {
            return;
        }
        if (tryStart(new Intent(Settings.ACTION_WIRELESS_SETTINGS))) {
            return;
        }
        tryStart(new Intent(Settings.ACTION_SETTINGS));
    }

    private boolean tryStart(Intent i) {
        try {
            startActivity(i);
            return true;
        } catch (ActivityNotFoundException | SecurityException e) {
            return false;
        }
    }

    // ------------------------------------------------------------------ the screen

    private void refresh() {
        final boolean on = Prefs.enabled(this);
        updating = true;
        share.setChecked(on);
        updating = false;

        final int n = Status.devices;
        final boolean fix = Status.lastFixAt != 0 && SystemClock.elapsedRealtime() - Status.lastFixAt < 3_000L;
        final String devs = n + (n == 1 ? " device" : " devices");
        if (!on) {
            statusTitle.setText("Off");
            statusBody.setText("Turn on to share this phone's GPS with devices on its hotspot.");
        } else if (!Status.running) {
            statusTitle.setText("Starting");
            statusBody.setText(Status.problem != null ? Status.problem : "");
        } else if (Status.portBusy) {
            statusTitle.setText("Port " + GpsService.PORT + " is in use");
            statusBody.setText("Another app is already sharing on it, probably gpsdRelay. Turn off that app's server.");
        } else if (Status.problem != null) {
            statusTitle.setText("Not sharing");
            statusBody.setText(Status.problem);
        } else if (n == 0) {
            statusTitle.setText("Ready");
            statusBody.setText("Waiting for a device on your hotspot. The GPS stays off until one connects.");
        } else if (!fix) {
            statusTitle.setText("Sharing with " + devs);
            statusBody.setText("Looking for satellites. Near a window or outside helps.");
        } else {
            statusTitle.setText("Sharing with " + devs);
            final StringBuilder b = new StringBuilder("GPS fix");
            if (Status.satsUsed > 0) {
                b.append(": ").append(Status.satsUsed).append(" satellites");
            }
            if (Status.accuracyM > 0) {
                b.append(", ±").append(Math.round(Status.accuracyM)).append(" m");
            }
            statusBody.setText(b);
        }

        final StringBuilder d = new StringBuilder();
        if (on && Status.running) {
            d.append("Port ").append(GpsService.PORT);
            if (Status.mode == Status.MODE_RECEIVER) {
                d.append(" · the GPS chip's own sentences");
            } else if (Status.mode == Status.MODE_MADE) {
                d.append(" · sentences made from Android's fix");
            } else if (Status.gpsOn) {
                d.append(" · GPS on");
            }
            if (!Status.deviceList.isEmpty()) {
                d.append('\n').append(Status.deviceList);
            }
            if (Status.refused > 0 && Status.lastRefusal != null) {
                d.append("\nRefused ").append(Status.refused).append(": ").append(Status.lastRefusal);
            }
        }
        details.setText(d);
        details.setVisibility(d.length() == 0 ? View.GONE : View.VISIBLE);

        final StringBuilder w = new StringBuilder();
        final LocationManager lm = getSystemService(LocationManager.class);
        if (lm != null && Build.VERSION.SDK_INT >= 28 && !lm.isLocationEnabled()) {
            w.append("Location is switched off on this phone. Turn it on in quick settings.\n");
        }
        final PowerManager pm = getSystemService(PowerManager.class);
        if (Status.mock) {
            w.append("A mock-location app is active: its positions are not shared.\n");
        }
        if (pm != null && pm.isPowerSaveMode()) {
            w.append("Power saving is on: it can turn the GPS off while the screen is off.\n");
        }
        warnings.setText(w.toString().trim());
        warnings.setVisibility(w.length() == 0 ? View.GONE : View.VISIBLE);

        final boolean precise = Setup.preciseLocation(this);
        final boolean approximate = !precise && checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
                == PackageManager.PERMISSION_GRANTED;
        rowLocation.set(precise, approximate
                ? "Approximate only. Open Settings and turn on \"Use precise location\"."
                : "So devices get the GPS position, not a rough area.");
        if (rowNotify != null) {
            rowNotify.set(Setup.notifications(this), "Shows that sharing is on. Without it, Samsung phones may stop the app when the screen locks.");
        }
        rowBattery.set(Setup.unrestrictedBattery(this), "Lets sharing continue with the screen off.");
        if (rowBackground != null) {
            rowBackground.set(Setup.backgroundLocation(this),
                    "Optional. Starts sharing by itself after the phone restarts. Without it, tap the reminder.");
        }
        if (rowStill != null) {
            rowStill.set(Setup.fullGnssTracking(this), Setup.fullGnssTracking(this)
                    ? "On: \"Force full GNSS measurements\" is set in Developer options."
                    : "Samsung switches the GPS off when the phone lies still with the screen off. "
                    + "Turn on \"Force full GNSS measurements\" in Developer options to stop that.");
        }
        final String hs = Setup.hotspotAddress(this);
        rowHotspot.set(hs != null, hs != null
                ? "On. Devices find this phone at " + hs + "."
                : "Turn on Mobile Hotspot: band 2.4 GHz, security WPA2, never turn off automatically.");
        rowHotspot.button.setVisibility(View.VISIBLE);    // always useful: the settings page
        atBoot.setChecked(Prefs.startAtBoot(this));
        counts.setText("From Android since the app started: " + Status.nmeaCount + " NMEA messages, "
                + Status.gpsFixCount + " GPS fixes");
        final String t = Diag.text();
        timeline.setText(t.isEmpty() ? "(nothing yet)" : t);
    }

    private View buildUi() {
        final ScrollView scroll = new ScrollView(this);
        scroll.setFitsSystemWindows(true);
        final LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(20), dp(16), dp(20), dp(24));
        scroll.addView(col);

        final TextView title = text("NH Devices Companion", 24, true);
        col.addView(title);
        final TextView sub = text("For the WiPhone, the X4 e-reader and other devices on NH firmware.", 14, false);
        sub.setAlpha(0.75f);
        col.addView(sub, margins(0, dp(2), 0, dp(16)));

        // ---- location sharing card
        final LinearLayout card = cardLayout();
        col.addView(card);
        final TextView cardTitle = text("Share this phone's GPS", 18, true);
        card.addView(cardTitle);
        final TextView cardHint = text("Devices on your hotspot read it like a GPS receiver. Nothing goes to the internet.", 14, false);
        cardHint.setAlpha(0.75f);
        card.addView(cardHint, margins(0, dp(2), 0, dp(8)));
        share = new Switch(this);
        share.setText("Share location on the hotspot");
        share.setTextSize(16);
        share.setOnCheckedChangeListener((b, checked) -> {
            if (updating) {
                return;
            }
            if (checked) {
                startSharing();
            } else {
                GpsService.stop(this);
            }
            refresh();
        });
        tintSwitch(share);
        card.addView(share, margins(0, 0, 0, dp(8)));
        statusTitle = text("", 17, true);
        statusTitle.setTextColor(accent());
        card.addView(statusTitle);
        statusBody = text("", 15, false);
        card.addView(statusBody);
        details = text("", 13, false);
        details.setAlpha(0.7f);
        card.addView(details, margins(0, dp(6), 0, 0));
        warnings = text("", 14, false);
        warnings.setTextColor(night ? 0xFFFFB74D : 0xFFA15C00);
        card.addView(warnings, margins(0, dp(8), 0, 0));

        // ---- setup
        final LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        final TextView setupTitle = text("Setup", 18, true);
        head.addView(setupTitle, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        final Button all = new Button(this);
        all.setText("Set up everything");
        all.setOnClickListener(v -> {
            setupStep = 0;
            notifyAsked = false;
            continueSetup();
        });
        head.addView(all);
        col.addView(head, margins(0, dp(20), 0, dp(4)));

        rowLocation = addRow(col, "Precise location", "Allow", v -> {
            if (checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED
                    && !Setup.preciseLocation(this)) {
                openAppSettings();
            } else {
                askLocation();
            }
        });
        if (Build.VERSION.SDK_INT >= 33) {
            rowNotify = addRow(col, "Notifications", "Allow", v -> askNotifications());
        }
        rowBattery = addRow(col, "Battery: unrestricted", "Allow", v -> askBattery());
        if (Build.VERSION.SDK_INT >= 29) {
            rowBackground = addRow(col, "Location all the time", "Allow", v -> askBackground());
        }
        if (Setup.samsung()) {
            rowStill = addRow(col, "Samsung: GPS on while the phone lies still", "Open", v -> explainFullGnss());
        }
        rowHotspot = addRow(col, "Mobile Hotspot", "Settings", v -> openHotspotSettings());

        atBoot = new CheckBox(this);
        atBoot.setText("Start sharing again after the phone restarts");
        atBoot.setOnCheckedChangeListener((b, c) -> Prefs.setStartAtBoot(this, c));
        atBoot.setButtonTintList(onOff(accent(), night ? 0xFF9E9E9E : 0xFF757575));
        col.addView(atBoot, margins(0, dp(12), 0, 0));

        // ---- diagnostics (what Android did to the GPS; never a position)
        final TextView diagTitle = text("Diagnostics", 18, true);
        col.addView(diagTitle, margins(0, dp(20), 0, dp(2)));
        final TextView diagHint = text("What Android did to the GPS and to this app, newest first. "
                + "After a test with the phone locked, a screenshot of this shows what happened.", 13, false);
        diagHint.setAlpha(0.7f);
        col.addView(diagHint);
        counts = text("", 12, false);
        counts.setAlpha(0.7f);
        col.addView(counts, margins(0, dp(4), 0, dp(4)));
        timeline = text("", 12, false);
        timeline.setTypeface(Typeface.MONOSPACE);
        col.addView(timeline);

        // ---- footer
        final Button guide = new Button(this);
        guide.setText("Guide and troubleshooting");
        guide.setOnClickListener(v -> tryStart(new Intent(Intent.ACTION_VIEW, Uri.parse(GUIDE_URL))));
        col.addView(guide, margins(0, dp(16), 0, 0));
        final TextView about = text("Version " + BuildConfig.VERSION_NAME + ". Open source. This app makes no internet "
                + "connections: only devices on this phone's hotspot can read the position, and only while sharing is on.",
                12, false);
        about.setAlpha(0.6f);
        col.addView(about, margins(0, dp(8), 0, 0));
        return scroll;
    }

    // ------------------------------------------------------------------ small view helpers

    private final class Row {
        final TextView mark;
        final TextView hint;
        final Button button;

        Row(TextView mark, TextView hint, Button button) {
            this.mark = mark;
            this.hint = hint;
            this.button = button;
        }

        void set(boolean ok, String hintText) {
            mark.setText(ok ? "✓" : "○");
            mark.setTextColor(ok ? accent() : (night ? 0xFFBDBDBD : 0xFF757575));
            hint.setText(hintText);
            button.setVisibility(ok ? View.GONE : View.VISIBLE);
        }
    }

    private Row addRow(LinearLayout parent, String title, String buttonText, View.OnClickListener action) {
        final LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        final TextView mark = text("○", 20, true);
        mark.setGravity(Gravity.CENTER);
        row.addView(mark, new LinearLayout.LayoutParams(dp(32), LinearLayout.LayoutParams.WRAP_CONTENT));
        final LinearLayout words = new LinearLayout(this);
        words.setOrientation(LinearLayout.VERTICAL);
        words.addView(text(title, 16, false));
        final TextView hint = text("", 13, false);
        hint.setAlpha(0.7f);
        words.addView(hint);
        row.addView(words, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        final Button b = new Button(this);
        b.setText(buttonText);
        b.setOnClickListener(action);
        row.addView(b);
        parent.addView(row, margins(0, dp(6), 0, dp(6)));
        return new Row(mark, hint, b);
    }

    private LinearLayout cardLayout() {
        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        final GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(16));
        bg.setColor(night ? 0xFF1F2A22 : 0xFFEEF4EF);
        card.setBackground(bg);
        return card;
    }

    /** Checked / unchecked colours. Samsung's dark theme drew the "on" switch and a ticked box near-invisibly. */
    private static ColorStateList onOff(int on, int off) {
        return new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {on, off});
    }

    private void tintSwitch(Switch sw) {
        final int off = night ? 0xFF9E9E9E : 0xFF757575;
        sw.setThumbTintList(onOff(accent(), night ? 0xFFE0E0E0 : 0xFFFAFAFA));
        sw.setTrackTintList(onOff((accent() & 0x00FFFFFF) | 0x80000000, (off & 0x00FFFFFF) | 0x80000000));
    }

    private int accent() {
        return night ? 0xFF81C784 : 0xFF2E7D32;
    }

    private TextView text(String s, int sp, boolean bold) {
        final TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        if (bold) {
            t.setTypeface(Typeface.DEFAULT_BOLD);
        }
        return t;
    }

    private LinearLayout.LayoutParams margins(int l, int t, int r, int b) {
        final LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        p.setMargins(l, t, r, b);
        return p;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
