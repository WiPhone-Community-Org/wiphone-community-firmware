package io.github.nikguy321.nhcompanion;

import android.content.Context;
import android.content.SharedPreferences;

/** Two settings: sharing on or off, and whether to start again after the phone restarts. */
final class Prefs {
    private Prefs() {}

    private static SharedPreferences p(Context c) {
        return c.getSharedPreferences("settings", Context.MODE_PRIVATE);
    }

    static boolean enabled(Context c) {
        return p(c).getBoolean("enabled", false);
    }

    static void setEnabled(Context c, boolean on) {
        p(c).edit().putBoolean("enabled", on).apply();
    }

    static boolean startAtBoot(Context c) {
        return p(c).getBoolean("start_at_boot", true);
    }

    static void setStartAtBoot(Context c, boolean on) {
        p(c).edit().putBoolean("start_at_boot", on).apply();
    }
}
