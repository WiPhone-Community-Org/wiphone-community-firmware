package io.github.nikguy321.nhcompanion;

/** What the service is doing, for the screen. Written by the service, read by the activity (one process). */
final class Status {
    private Status() {}

    static final int MODE_NONE = 0;
    static final int MODE_RECEIVER = 1;    // the receiver's own sentences, relayed
    static final int MODE_MADE = 2;        // GGA/RMC made from Android's fix (the receiver gives no text)

    static volatile boolean running;
    static volatile boolean portBusy;
    static volatile boolean gpsOn;
    static volatile String problem;        // a reason sharing cannot work, or null
    static volatile int devices;
    static volatile String deviceList = "";
    static volatile int mode = MODE_NONE;
    static volatile int satsUsed = -1;
    static volatile float accuracyM = -1f;
    static volatile long lastFixAt;        // SystemClock.elapsedRealtime() of the last GPS fix, 0 = none
    static volatile long nmeaCount;        // NMEA messages from Android, since the app started
    static volatile long gpsFixCount;      // GPS fixes from Android
    static volatile boolean mock;          // the last fix came from a mock-location app: not sent
    static volatile long refused;
    static volatile String lastRefusal;

    /** Debug builds only (adb): make GGA/RMC from Android's fix even when the receiver gives text. */
    static volatile boolean forceMade;
    /** Debug builds only (adb): let devices in from any network, to test sharing through Doze from the emulator. */
    static volatile boolean debugAnyNetwork;
}
