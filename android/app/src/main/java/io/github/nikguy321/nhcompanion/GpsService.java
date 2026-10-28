package io.github.nikguy321.nhcompanion;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.location.GnssStatus;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.location.OnNmeaMessageListener;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

import java.io.IOException;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Shares the GPS on port 10110 while it runs, as a foreground service with a notification.
 *
 * <p>The port is always open while sharing is on, which costs next to nothing. The GPS itself is
 * switched on only while a device is connected, and for {@link #LINGER_MS} after the last one
 * leaves, so a device that reconnects (or an e-reader asked again) finds it already warm. While
 * the GPS is on, a partial wake lock keeps the CPU running so the sentences keep flowing with the
 * screen off.
 *
 * <p>Sentences: the receiver's own NMEA, relayed (GGA, RMC, GSA, GST, VTG). Only on a phone whose
 * receiver gives Android no NMEA text are GGA and RMC made from Android's fix instead. With no fix
 * and no text, a "no fix" GGA and RMC every few seconds keep the device's link alive.
 *
 * <p>Nothing is ever sent anywhere else, and positions are never written to the log.
 */
public final class GpsService extends Service {
    static final String TAG = "NHCompanion";
    static final String ACTION_STOP = "io.github.nikguy321.nhcompanion.STOP";
    static final int PORT = 10110;
    static final String CHANNEL = "sharing";

    private static final int NOTIFICATION_ID = 1;
    private static final int MAX_DEVICES = 8;
    private static final long LINGER_MS = 5 * 60_000L;
    private static final long WARMUP_MS = 3_000L;        // after the GPS starts, wait this long for receiver text
    private static final long RECEIVER_QUIET_MS = 2_500L; // no receiver GGA this long: make our own from the fix
    private static final long KEEPALIVE_MS = 4_000L;     // nothing sent this long: send "no fix" (the WiPhone gives up at 10 s)
    private static final long FIX_FRESH_MS = 3_000L;
    private static final long PORT_RETRY_MS = 10_000L;

    private HandlerThread thread;
    private Handler handler;
    private Hub hub;
    private LocationManager lm;
    private PowerManager.WakeLock wake;
    private boolean gpsOn;
    private long gpsOnAt;
    private long lastReceiverGga;
    private volatile long lastSent;
    private long lastDeviceAt;
    private int satsUsed = -1;
    private String shownText;
    private long shownAt;
    /** Set first thing in onDestroy: nothing may redraw the notification after the service has gone. */
    private volatile boolean destroyed;
    // The diagnostics timeline (Diag).
    private long lastGpsData;               // last NMEA text or GPS fix from Android
    private boolean quietLogged;
    private int lastDeviceCount;
    private BroadcastReceiver systemEvents;

    static void start(Context c) {
        Prefs.setEnabled(c, true);
        c.startForegroundService(new Intent(c, GpsService.class));
    }

    static void stop(Context c) {
        Prefs.setEnabled(c, false);
        c.stopService(new Intent(c, GpsService.class));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        ensureChannels(this);
        if (!goForeground(build("Starting", "Opening port " + PORT))) {
            if (Prefs.enabled(this)) {
                BootReceiver.remind(this);    // still meant to be on: one tap restarts it
            }
            stopSelf();
            return;
        }
        Status.running = true;
        Status.problem = null;
        lm = getSystemService(LocationManager.class);
        final PowerManager pm = getSystemService(PowerManager.class);
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "NHCompanion:gps");
        wake.setReferenceCounted(false);
        thread = new HandlerThread("gps");
        thread.start();
        handler = new Handler(thread.getLooper());
        hub = new Hub(PORT, MAX_DEVICES, events);
        handler.post(openPort);
        handler.post(tick);
        listenToSystem();
        Diag.add("sharing on");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            Prefs.setEnabled(this, false);
            stopSelf();
            return START_NOT_STICKY;
        }
        if (intent == null && !Setup.backgroundLocation(this)) {
            // Android restarted it after killing the process. Started from the background like this,
            // it may read the GPS only with "all the time" location: ask for a tap instead of
            // running blind (the devices would see "no fix" for ever).
            BootReceiver.remind(this);
            stopSelf();
            return START_NOT_STICKY;
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        destroyed = true;
        Status.running = false;
        Status.devices = 0;
        Status.deviceList = "";
        Status.gpsOn = false;
        Status.mode = Status.MODE_NONE;
        if (systemEvents != null) {
            unregisterReceiver(systemEvents);
            systemEvents = null;
        }
        Diag.add("sharing off");
        if (handler != null) {
            handler.removeCallbacksAndMessages(null);
            hub.stop();                       // closes the port and every device now
            handler.post(this::stopGps);      // the GPS was started on this thread; stop it there
            thread.quitSafely();
            final NotificationManager nm = getSystemService(NotificationManager.class);
            nm.cancel(NOTIFICATION_ID);
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    // ------------------------------------------------------------------ the port

    private final Runnable openPort = new Runnable() {
        @Override
        public void run() {
            if (destroyed) {
                return;
            }
            try {
                hub.start();
                Status.portBusy = false;
                Log.i(TAG, "port " + PORT + " open");
            } catch (IOException e) {
                Status.portBusy = true;
                Log.w(TAG, "port " + PORT + " busy: " + e.getMessage());
                handler.postDelayed(this, PORT_RETRY_MS);
            }
            refreshNotification(true);
        }
    };

    private final Hub.Events events = new Hub.Events() {
        @Override
        public String admit(Socket s) {
            final Set<String> ifs = new HashSet<>();
            final Set<String> addrs = new HashSet<>();
            Setup.upstream(GpsService.this, ifs, addrs);
            String localIf = null;
            int localPrefix = -1;
            boolean remoteIsOwn = false;
            try {
                final NetworkInterface ni = NetworkInterface.getByInetAddress(s.getLocalAddress());
                if (ni != null) {
                    localIf = ni.getName();
                    for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                        if (ia.getAddress() != null && Policy.key(ia.getAddress().getAddress())
                                .equals(Policy.key(s.getLocalAddress().getAddress()))) {
                            localPrefix = ia.getNetworkPrefixLength();
                        }
                    }
                }
                remoteIsOwn = NetworkInterface.getByInetAddress(s.getInetAddress()) != null;
            } catch (Exception e) {
                // unknown interface: the address checks still apply
            }
            final String who = s.getInetAddress().getHostAddress();
            final String why = Policy.refuse(s.getLocalAddress().getAddress(), localIf, localPrefix,
                    s.getInetAddress().getAddress(), remoteIsOwn, ifs, addrs, BuildConfig.DEBUG);
            if (why != null && BuildConfig.DEBUG && Status.debugAnyNetwork) {
                Log.i(TAG, "debug: letting in " + who + " on " + localIf + " (would refuse: " + why + ")");
                return null;
            }
            if (why != null) {
                Status.refused++;
                Status.lastRefusal = who + ": " + why;
                Diag.add("refused " + who + ": " + why);
            } else {
                Diag.add("device " + who + " connected (" + localIf + ")");
            }
            return why;
        }

        @Override
        public void changed() {
            final Handler h = handler;
            if (h != null) {
                h.post(evaluate);
            }
        }
    };

    // ------------------------------------------------------------------ the GPS

    /** GPS on while a device is connected (and LINGER_MS after); status and notification. */
    private final Runnable evaluate = () -> {
        if (destroyed) {
            return;
        }
        final long now = SystemClock.elapsedRealtime();
        final int n = hub.clientCount();
        if (n > 0) {
            lastDeviceAt = now;
        }
        if (n < lastDeviceCount) {
            Diag.add("a device left (" + n + " connected)");
        }
        lastDeviceCount = n;
        if (n > 0 && !gpsOn) {
            startGps();
        } else if (n == 0 && gpsOn && now - lastDeviceAt > LINGER_MS) {
            stopGps();
        }
        if (gpsOn) {
            wake.acquire(2 * 60_000L);      // renewed every second while the GPS is on
        }
        Status.devices = n;
        final StringBuilder b = new StringBuilder();
        for (Hub.Info i : hub.snapshot()) {
            if (b.length() > 0) {
                b.append('\n');
            }
            final long mins = (System.currentTimeMillis() - i.sinceMs) / 60_000L;
            b.append(i.address).append("  ·  ").append(mins).append(" min, ").append(i.bytes / 1024).append(" KB");
        }
        Status.deviceList = b.toString();
        refreshNotification(false);
    };

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (destroyed) {
                return;
            }
            evaluate.run();
            watchForQuiet();
            if (hub.clientCount() > 0 && SystemClock.elapsedRealtime() - lastSent > KEEPALIVE_MS) {
                final long utc = System.currentTimeMillis();
                // GGA only: a "V" RMC stamped with this second could veto the device's first real fix
                // if that fix carries the same second.
                send(Nmea.ggaNoFix(utc));
            }
            handler.postDelayed(this, 1000);
        }
    };

    private void startGps() {
        if (destroyed) {
            return;
        }
        if (lm == null || !Setup.preciseLocation(this)) {
            Status.problem = "Precise location is not allowed";
            return;
        }
        try {
            requestGps();
        } catch (SecurityException e) {
            Status.problem = "Location permission was taken away";
            Log.w(TAG, "no location permission", e);
            removeGps();
            return;
        } catch (IllegalArgumentException e) {
            Status.problem = "This phone has no GPS";
            Log.w(TAG, "no GPS provider", e);
            removeGps();
            return;
        }
        Status.problem = null;
        gpsOn = true;
        Status.gpsOn = true;
        gpsOnAt = SystemClock.elapsedRealtime();
        lastReceiverGga = 0;
        lastGpsData = 0;
        quietLogged = false;
        wake.acquire(2 * 60_000L);
        Diag.add("GPS on" + (Setup.samsung() ? (Setup.fullGnssTracking(this)
                ? " (full GNSS measurements on: Samsung keeps it running when still)"
                : " (full GNSS measurements OFF: Samsung may stop it when the phone lies still)") : ""));
    }

    /**
     * The registrations themselves: the GPS, its NMEA text and satellite status. The caller checks
     * precise location first and catches the SecurityException.
     */
    @android.annotation.SuppressLint("MissingPermission")
    private void requestGps() {
        lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 0f, locationListener, thread.getLooper());
        lm.addNmeaListener(nmeaListener, handler);
        lm.registerGnssStatusCallback(gnssCallback, handler);
    }

    /**
     * No GPS data for a while although devices are connected: say so. On a Samsung phone this is
     * usually its "stop GPS by sensor" rule (Setup.fullGnssTracking).
     */
    private void watchForQuiet() {
        if (!gpsOn || hub.clientCount() == 0) {
            return;
        }
        final long now = SystemClock.elapsedRealtime();
        final long quiet = now - Math.max(lastGpsData, gpsOnAt);
        if (quiet > 8_000L && !quietLogged) {
            quietLogged = true;
            Diag.add("GPS data stopped: Android is not sending any");
        }
    }

    /** NMEA text or a fix arrived. */
    private void gpsData() {
        final long now = SystemClock.elapsedRealtime();
        if (quietLogged) {
            quietLogged = false;
            Diag.add("GPS data back after " + (now - lastGpsData) / 1000 + " s");
        }
        lastGpsData = now;
    }

    /** Screen, Doze and power saving, for the timeline. */
    private void listenToSystem() {
        final PowerManager pm = getSystemService(PowerManager.class);
        systemEvents = new BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent i) {
                final String a = i.getAction();
                if (Intent.ACTION_SCREEN_OFF.equals(a)) {
                    Diag.add("screen off");
                } else if (Intent.ACTION_SCREEN_ON.equals(a)) {
                    Diag.add("screen on");
                } else if (PowerManager.ACTION_DEVICE_IDLE_MODE_CHANGED.equals(a)) {
                    Diag.add(pm.isDeviceIdleMode() ? "Doze: deep sleep" : "Doze: awake");
                } else if (PowerManager.ACTION_POWER_SAVE_MODE_CHANGED.equals(a)) {
                    Diag.add(pm.isPowerSaveMode() ? "power saving ON" : "power saving off");
                }
            }
        };
        final IntentFilter f = new IntentFilter();
        f.addAction(Intent.ACTION_SCREEN_OFF);
        f.addAction(Intent.ACTION_SCREEN_ON);
        f.addAction(PowerManager.ACTION_DEVICE_IDLE_MODE_CHANGED);
        f.addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(systemEvents, f, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(systemEvents, f);
        }
    }

    private void stopGps() {
        if (!gpsOn) {
            return;
        }
        removeGps();
        gpsOn = false;
        Status.gpsOn = false;
        Status.mode = Status.MODE_NONE;
        Status.satsUsed = -1;
        satsUsed = -1;
        if (wake.isHeld()) {
            wake.release();
        }
        Diag.add("GPS off");
    }

    private void removeGps() {
        if (lm == null) {
            return;
        }
        try {
            lm.removeUpdates(locationListener);
            lm.removeNmeaListener(nmeaListener);
            lm.unregisterGnssStatusCallback(gnssCallback);
        } catch (RuntimeException e) {
            // already gone
        }
    }

    private void send(String text) {
        hub.broadcast(text);
        lastSent = SystemClock.elapsedRealtime();
    }

    private final OnNmeaMessageListener nmeaListener = (message, timestamp) -> {
        if (Status.forceMade) {
            return;
        }
        gpsData();
        Status.nmeaCount++;
        final List<String> lines = Nmea.relayable(message);
        for (String line : lines) {
            final long now = SystemClock.elapsedRealtime();
            if ("GGA".equals(Nmea.typeOf(line))) {
                lastReceiverGga = now;
                setMode(Status.MODE_RECEIVER);
            } else if (now - lastReceiverGga >= RECEIVER_QUIET_MS) {
                continue;                  // a chip with no GGA: GGA and RMC are made from the fix instead
            }
            send(line);
        }
    };

    @SuppressWarnings("deprecation")
    private static boolean isMock(Location l) {
        return Build.VERSION.SDK_INT >= 31 ? l.isMock() : l.isFromMockProvider();
    }

    /** Height above mean sea level (GGA's meaning), or NaN: Android before 14 gives only the ellipsoid height. */
    private static double mslAltitude(Location l) {
        if (Build.VERSION.SDK_INT >= 34 && l.hasMslAltitude()) {
            return l.getMslAltitudeMeters();
        }
        return Double.NaN;
    }

    private static void setMode(int m) {
        if (Status.mode != m) {
            Status.mode = m;
            Diag.add(m == Status.MODE_RECEIVER ? "sending the GPS chip's own sentences" : "sending sentences made from Android's fix");
        }
    }

    /**
     * Satellites used in this fix, or -1 (an empty GGA field) when Android does not say. A GPS fix
     * made from no satellites is not a thing, so a count of 0 beside one means "not reported"
     * (the emulator does this), and writing 00 would make the device call a good fix poor.
     */
    private int satellites(Location l) {
        if (satsUsed > 0) {
            return satsUsed;
        }
        final Bundle x = l.getExtras();
        final int fromExtras = x == null ? 0 : x.getInt("satellites", 0);
        return fromExtras > 0 ? fromExtras : -1;
    }

    private final LocationListener locationListener = new LocationListener() {
        @Override
        public void onLocationChanged(Location l) {
            final long now = SystemClock.elapsedRealtime();
            if (isMock(l)) {
                // A fake-GPS app's position must never reach the devices as a measured fix.
                Status.mock = true;
                return;
            }
            Status.mock = false;
            gpsData();
            Status.gpsFixCount++;
            Status.lastFixAt = now;
            Status.accuracyM = l.hasAccuracy() ? l.getAccuracy() : -1f;
            if (now - gpsOnAt < WARMUP_MS || now - lastReceiverGga < RECEIVER_QUIET_MS) {
                return;                        // the receiver's own text is (or may yet be) on its way
            }
            final double msl = mslAltitude(l);
            final long t = l.getTime();
            send(Nmea.gga(t, l.getLatitude(), l.getLongitude(), satellites(l),
                    Nmea.hdopFromAccuracy(l.hasAccuracy() ? l.getAccuracy() : -1f), msl)
                    + Nmea.rmc(t, l.getLatitude(), l.getLongitude(),
                    l.hasSpeed() ? l.getSpeed() : Double.NaN, l.hasBearing() ? l.getBearing() : Double.NaN));
            setMode(Status.MODE_MADE);
        }

        // Abstract before Android 11: an older phone needs them all.
        @Override
        public void onStatusChanged(String provider, int status, Bundle extras) {}

        @Override
        public void onProviderEnabled(String provider) {
            Status.problem = null;
        }

        @Override
        public void onProviderDisabled(String provider) {
            Status.problem = "Location is switched off on this phone";
        }
    };

    private final GnssStatus.Callback gnssCallback = new GnssStatus.Callback() {
        @Override
        public void onSatelliteStatusChanged(GnssStatus status) {
            int used = 0;
            for (int i = 0; i < status.getSatelliteCount(); i++) {
                if (status.usedInFix(i)) {
                    used++;
                }
            }
            satsUsed = used;
            Status.satsUsed = used;
        }

        @Override
        public void onStarted() {
            Diag.add("GPS engine started");
        }

        @Override
        public void onStopped() {
            satsUsed = -1;
            Status.satsUsed = -1;
            Diag.add("GPS engine stopped");
        }
    };


    // ------------------------------------------------------------------ the notification

    static void ensureChannels(Context c) {
        final NotificationManager nm = c.getSystemService(NotificationManager.class);
        final NotificationChannel ch = new NotificationChannel(CHANNEL, "Sharing location", NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("Shown while the GPS is shared on the hotspot. Android stops apps without it.");
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
        final NotificationChannel r = new NotificationChannel(BootReceiver.CHANNEL, "Reminders", NotificationManager.IMPORTANCE_DEFAULT);
        r.setDescription("A tap to start sharing again after the phone restarts.");
        nm.createNotificationChannel(r);
    }

    private boolean goForeground(Notification n) {
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION);
            } else {
                startForeground(NOTIFICATION_ID, n);
            }
            return true;
        } catch (RuntimeException e) {
            // Android 14+: no location permission, or started from the background where it may not be
            Status.problem = "Android would not let sharing start: open the app and tap Share";
            Log.w(TAG, "startForeground refused", e);
            return false;
        }
    }

    private Notification build(String title, String text) {
        final Intent open = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        final PendingIntent openPi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE);
        final PendingIntent stopPi = PendingIntent.getService(this, 1,
                new Intent(this, GpsService.class).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE);
        final Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_notify)
                .setContentTitle(title)
                .setContentText(text)
                .setContentIntent(openPi)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setShowWhen(false)
                .setCategory(Notification.CATEGORY_SERVICE)
                .addAction(new Notification.Action.Builder(null, "Stop sharing", stopPi).build());
        if (Build.VERSION.SDK_INT >= 31) {
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        }
        return b.build();
    }

    /** The words for now; the notification is redrawn when they change (a fix's numbers at most every 15 s). */
    private void refreshNotification(boolean force) {
        final String title;
        final String text;
        final boolean numbersOnly;
        final int n = Status.devices;
        final boolean fix = Status.lastFixAt != 0 && SystemClock.elapsedRealtime() - Status.lastFixAt < FIX_FRESH_MS;
        if (Status.portBusy) {
            title = "Port " + PORT + " is in use";
            text = "Another app is sharing on it (gpsdRelay?). Stop that app's server.";
            numbersOnly = false;
        } else if (Status.problem != null) {
            title = "Not sharing";
            text = Status.problem;
            numbersOnly = false;
        } else if (n == 0) {
            title = "Ready";
            text = "Waiting for a device on your hotspot";
            numbersOnly = false;
        } else if (!fix) {
            title = "Sharing with " + n + (n == 1 ? " device" : " devices");
            text = "Looking for satellites (a window helps)";
            numbersOnly = false;
        } else {
            title = "Sharing with " + n + (n == 1 ? " device" : " devices");
            final StringBuilder t = new StringBuilder("GPS fix");
            if (Status.satsUsed > 0) {
                t.append(", ").append(Status.satsUsed).append(" satellites");
            }
            if (Status.accuracyM > 0) {
                t.append(", ±").append(Math.round(Status.accuracyM)).append(" m");
            }
            text = t.toString();
            numbersOnly = true;
        }
        final String key = title + "|" + text;
        final long now = SystemClock.elapsedRealtime();
        if (!force && key.equals(shownText)) {
            return;
        }
        if (!force && numbersOnly && shownText != null && shownText.startsWith(title + "|GPS fix")
                && now - shownAt < 15_000L) {
            return;
        }
        if (destroyed) {
            return;
        }
        shownText = key;
        shownAt = now;
        final NotificationManager nm = getSystemService(NotificationManager.class);
        nm.notify(NOTIFICATION_ID, build(title, text));
    }
}
