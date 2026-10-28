package io.github.nikguy321.nhcompanion;

import android.Manifest;
import android.app.NotificationManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.os.Build;
import android.os.PowerManager;
import android.provider.Settings;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

/** The checks behind the setup list, and the phone's networks as {@link Policy} needs them. */
final class Setup {
    private Setup() {}

    static boolean preciseLocation(Context c) {
        return c.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    /** "Allow all the time": needed to start by itself after the phone restarts (Android 10+). */
    static boolean backgroundLocation(Context c) {
        if (Build.VERSION.SDK_INT < 29) {
            return preciseLocation(c);
        }
        return preciseLocation(c)
                && c.checkSelfPermission(Manifest.permission.ACCESS_BACKGROUND_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    static boolean notifications(Context c) {
        if (Build.VERSION.SDK_INT >= 33
                && c.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            return false;
        }
        final NotificationManager nm = c.getSystemService(NotificationManager.class);
        return nm == null || nm.areNotificationsEnabled();
    }

    static boolean unrestrictedBattery(Context c) {
        final PowerManager pm = c.getSystemService(PowerManager.class);
        return pm != null && pm.isIgnoringBatteryOptimizations(c.getPackageName());
    }

    static boolean samsung() {
        return "samsung".equalsIgnoreCase(Build.MANUFACTURER);
    }

    /**
     * Developer options → "Force full GNSS measurements". On a Samsung phone this is what keeps the
     * GPS running while the phone lies still with the screen off and no charger: Samsung's location
     * service ("OldStopController_FLP" in nsflp) otherwise switches the GPS off for every app once
     * the motion sensor says "stationary" ("onCheckGpsStopBySensor ... so stop gps"), and skips that
     * only when isMeasurementsMode is true, which is this setting. Proven 2026-10-11 on a Galaxy
     * Note 20 Ultra (Android 13): GPS stopped ~3 min after the phone went still; with the setting on
     * it ran 16 min without a gap. An app asking for raw measurements itself did not count.
     */
    static boolean fullGnssTracking(Context c) {
        return Settings.Global.getInt(c.getContentResolver(), "enable_gnss_raw_meas_full_tracking", 0) == 1;
    }

    /** Interface names and address keys of every network the phone USES (Wi-Fi, mobile data, VPN). */
    static void upstream(Context c, Set<String> ifs, Set<String> addrs) {
        final ConnectivityManager cm = c.getSystemService(ConnectivityManager.class);
        if (cm == null) {
            return;
        }
        @SuppressWarnings("deprecation") final Network[] all = cm.getAllNetworks();
        for (Network n : all) {
            final LinkProperties lp = cm.getLinkProperties(n);
            if (lp == null) {
                continue;
            }
            if (lp.getInterfaceName() != null) {
                ifs.add(lp.getInterfaceName());
            }
            for (LinkAddress la : lp.getLinkAddresses()) {
                addrs.add(Policy.key(la.getAddress().getAddress()));
            }
        }
    }

    /**
     * This phone's address on its hotspot, as "192.168.x.1", when an interface that is up, has a
     * private IPv4 address and is not a network the phone uses can be seen. A guess for the
     * screen only: Android gives apps no way to ask whether the hotspot is on.
     */
    static String hotspotAddress(Context c) {
        final Set<String> ifs = new HashSet<>();
        final Set<String> addrs = new HashSet<>();
        upstream(c, ifs, addrs);
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) {
                    continue;
                }
                for (InetAddress a : Collections.list(ni.getInetAddresses())) {
                    if (!(a instanceof Inet4Address)) {
                        continue;
                    }
                    final byte[] b = a.getAddress();
                    // Would a device on this interface's own subnet be let in? Then it is a tethering interface.
                    final byte[] neighbour = b.clone();
                    neighbour[3] ^= 1;
                    final String why = Policy.refuse(b, ni.getName(), -1, neighbour, false, ifs, addrs, false);
                    if (why == null && Policy.isPrivate(b)) {
                        return a.getHostAddress();
                    }
                }
            }
        } catch (Exception e) {
            // no interface list: say nothing rather than guess
        }
        return null;
    }
}
