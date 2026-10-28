package io.github.nikguy321.nhcompanion;

import android.util.Log;

import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Iterator;
import java.util.Locale;

/**
 * A short timeline of what Android did to the GPS and to this app: screen on/off, Doze, power
 * saving, the GPS engine starting and stopping, sentences stopping and coming back, devices
 * connecting. Shown on the screen so a test with the phone locked can be read afterwards from a
 * screenshot. Never a position.
 */
final class Diag {
    private Diag() {}

    private static final int MAX = 60;
    private static final ArrayDeque<String> EVENTS = new ArrayDeque<>();
    private static final SimpleDateFormat TIME = new SimpleDateFormat("HH:mm:ss", Locale.US);

    static synchronized void add(String what) {
        EVENTS.addLast(TIME.format(new Date()) + "  " + what);
        while (EVENTS.size() > MAX) {
            EVENTS.removeFirst();
        }
        Log.i(GpsService.TAG, "diag: " + what);
    }

    /** Newest first. */
    static synchronized String text() {
        final StringBuilder b = new StringBuilder();
        final Iterator<String> it = EVENTS.descendingIterator();
        while (it.hasNext()) {
            if (b.length() > 0) {
                b.append('\n');
            }
            b.append(it.next());
        }
        return b.toString();
    }
}
