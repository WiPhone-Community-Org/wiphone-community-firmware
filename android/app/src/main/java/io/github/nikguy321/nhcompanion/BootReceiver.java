package io.github.nikguy321.nhcompanion;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/**
 * After the phone restarts (or the app is updated), start sharing again if it was on.
 *
 * <p>Android lets a location service started this way read the GPS only with "Allow all the
 * time". Without it the service would run blind (Android 13) or be refused (14+), so instead a
 * notification asks for one tap, which starts it from the foreground.
 */
public final class BootReceiver extends BroadcastReceiver {
    static final String CHANNEL = "reminders";
    private static final int REMINDER_ID = 2;

    @Override
    public void onReceive(Context c, Intent intent) {
        final String a = intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(a) && !Intent.ACTION_MY_PACKAGE_REPLACED.equals(a)) {
            return;
        }
        if (!Prefs.enabled(c) || !Prefs.startAtBoot(c)) {
            return;
        }
        if (Setup.backgroundLocation(c)) {
            try {
                c.startForegroundService(new Intent(c, GpsService.class));
                Log.i(GpsService.TAG, "started after " + a);
                return;
            } catch (RuntimeException e) {
                Log.w(GpsService.TAG, "could not start after " + a, e);
            }
        }
        remind(c);
    }

    /**
     * "Tap to share again": sharing is on but could not start by itself. The tap starts it from the
     * foreground, where Android allows it.
     */
    static void remind(Context c) {
        GpsService.ensureChannels(c);
        final PendingIntent open = PendingIntent.getActivity(c, REMINDER_ID,
                new Intent(c, MainActivity.class).putExtra(MainActivity.EXTRA_START, true),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        final Notification n = new Notification.Builder(c, CHANNEL)
                .setSmallIcon(R.drawable.ic_notify)
                .setContentTitle("Location sharing is paused")
                .setContentText("Tap to share your GPS on the hotspot again")
                .setContentIntent(open)
                .setAutoCancel(true)
                .build();
        c.getSystemService(NotificationManager.class).notify(REMINDER_ID, n);
    }
}
