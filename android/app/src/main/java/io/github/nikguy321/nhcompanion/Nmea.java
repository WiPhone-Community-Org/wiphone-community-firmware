package io.github.nikguy321.nhcompanion;

import java.util.ArrayList;
import java.util.List;

/**
 * NMEA 0183 text: the receiver's own sentences cleaned for relaying, and GGA/RMC made from an
 * Android fix when the receiver gives no text of its own.
 *
 * <p>Numbers are written with integer arithmetic only. Two bugs seen in phone NMEA apps cannot
 * happen here: String.format follows the phone's language (a German phone writes "47,5"), and
 * Double.toString switches to E notation for tiny minutes ("5.9E-5"), which one app then cuts
 * into a coordinate kilometres off.
 */
final class Nmea {
    private Nmea() {}

    /** Receiver sentence types passed on unchanged. GSV (satellites in view) and proprietary lines are dropped. */
    private static final String[] RELAYED = {"GGA", "RMC", "GSA", "GST", "VTG"};

    /** NMEA allows 82 characters; some receivers write longer GSA lines. The WiPhone reads up to 95. */
    static final int MAX_LINE = 90;

    static int checksum(CharSequence body) {
        int c = 0;
        for (int i = 0; i < body.length(); i++) {
            c ^= body.charAt(i);
        }
        return c & 0xFF;
    }

    /** "$" + body + "*HH" + CRLF. */
    static String wrap(String body) {
        final int c = checksum(body);
        return "$" + body + "*" + HEX[c >> 4] + HEX[c & 15] + "\r\n";
    }

    private static final char[] HEX = "0123456789ABCDEF".toCharArray();

    private static int hexVal(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    }

    /**
     * The receiver's text from Android's NMEA listener, cleaned. Android does not say whether a
     * message ends in CRLF or holds one sentence or several, so it is split at every '$'. A piece
     * is kept when its checksum is right and it is a GGA, RMC, GSA, GST or VTG from any talker.
     * Each kept line ends in CRLF.
     */
    static List<String> relayable(String raw) {
        final List<String> out = new ArrayList<>(2);
        if (raw == null) {
            return out;
        }
        int at = raw.indexOf('$');
        while (at >= 0) {
            final int next = raw.indexOf('$', at + 1);
            final String piece = trim(raw.substring(at, next < 0 ? raw.length() : next));
            final String line = clean(piece);
            if (line != null) {
                out.add(line);
            }
            at = next;
        }
        return out;
    }

    private static String trim(String s) {
        int end = s.length();
        while (end > 0 && s.charAt(end - 1) <= ' ') {
            end--;
        }
        return s.substring(0, end);
    }

    /** One "$...*HH" piece: the line with CRLF when it is checksum-good and relayed, else null. */
    private static String clean(String s) {
        final int n = s.length();
        if (n < 10 || n > MAX_LINE || s.charAt(0) != '$' || s.charAt(n - 3) != '*') {
            return null;
        }
        final String body = s.substring(1, n - 3);
        final int hi = hexVal(s.charAt(n - 2));
        final int lo = hexVal(s.charAt(n - 1));
        if (hi < 0 || lo < 0 || checksum(body) != ((hi << 4) | lo)) {
            return null;
        }
        if (body.charAt(0) == 'P' || body.length() < 6 || body.charAt(5) != ',') {
            return null;
        }
        for (int i = 0; i < body.length(); i++) {
            final char ch = body.charAt(i);
            if (ch < ' ' || ch > '~' || ch == '$' || ch == '*') {
                return null;
            }
        }
        final String type = body.substring(2, 5);
        for (String t : RELAYED) {
            if (t.equals(type)) {
                return s + "\r\n";
            }
        }
        return null;
    }

    /** "GGA", "RMC"... for a "$ttSSS," line, or null. */
    static String typeOf(String line) {
        if (line == null || line.length() < 7 || line.charAt(0) != '$' || line.charAt(6) != ',') {
            return null;
        }
        return line.substring(3, 6);
    }

    // ------------------------------------------------------------------ made from an Android fix

    /**
     * A GGA for a measured GPS fix. sats &lt; 0, or NaN hdop / altitude, leave that field empty.
     * The altitude must be above mean sea level (GGA's meaning); pass NaN when only the
     * ellipsoid height is known.
     */
    static String gga(long utcMs, double lat, double lon, int sats, double hdop, double mslAltM) {
        return wrap(join("GPGGA", time(utcMs), latField(lat), latHemi(lat), lonField(lon), lonHemi(lon),
                "1", sats < 0 ? "" : pad(Math.min(sats, 99), 2),
                Double.isNaN(hdop) ? "" : fixed(hdop, 1),
                Double.isNaN(mslAltM) ? "" : fixed(mslAltM, 1), "M", "", "M", "", ""));
    }

    /** An RMC for a measured fix: status A, mode A (autonomous). NaN speed / course leave the field empty. */
    static String rmc(long utcMs, double lat, double lon, double speedMps, double courseDeg) {
        return wrap(join("GPRMC", time(utcMs), "A", latField(lat), latHemi(lat), lonField(lon), lonHemi(lon),
                Double.isNaN(speedMps) ? "" : fixed(speedMps * 1.9438444924406, 1),
                Double.isNaN(courseDeg) ? "" : fixed(courseDeg, 1),
                date(utcMs), "", "", "A"));
    }

    /** "No fix": quality 0 with no position. Sent so the link stays alive while there is nothing to say. */
    static String ggaNoFix(long utcMs) {
        return wrap(join("GPGGA", time(utcMs), "", "", "", "", "0", "00", "", "", "M", "", "M", "", ""));
    }

    /** "No fix": status V, mode N. */
    static String rmcNoFix(long utcMs) {
        return wrap(join("GPRMC", time(utcMs), "V", "", "", "", "", "", "", date(utcMs), "", "", "N"));
    }

    /**
     * A stand-in HDOP for a GGA made from an Android fix, which has an accuracy in metres and no
     * DOP: accuracy / 4 m, 0.5 to 99.9. NaN when there is no accuracy.
     */
    static double hdopFromAccuracy(float accM) {
        if (!(accM > 0f) || Float.isInfinite(accM)) {
            return Double.NaN;
        }
        final double h = accM / 4.0;
        return h < 0.5 ? 0.5 : (h > 99.9 ? 99.9 : h);
    }

    private static String join(String... fields) {
        final StringBuilder b = new StringBuilder(80);
        for (int i = 0; i < fields.length; i++) {
            if (i > 0) {
                b.append(',');
            }
            b.append(fields[i]);
        }
        return b.toString();
    }

    static String pad(long v, int width) {
        final String s = Long.toString(v);
        if (s.length() >= width) {
            return s;
        }
        final StringBuilder b = new StringBuilder(width);
        for (int i = s.length(); i < width; i++) {
            b.append('0');
        }
        return b.append(s).toString();
    }

    /** v rounded to `decimals` places, without exponent or locale. */
    static String fixed(double v, int decimals) {
        long scale = 1;
        for (int i = 0; i < decimals; i++) {
            scale *= 10;
        }
        final long s = Math.round(Math.abs(v) * scale);
        final StringBuilder b = new StringBuilder(12);
        if (v < 0 && s != 0) {
            b.append('-');
        }
        b.append(s / scale);
        if (decimals > 0) {
            b.append('.').append(pad(s % scale, decimals));
        }
        return b.toString();
    }

    /** Degrees to "ddmm.mmmmm" (lat) or "dddmm.mmmmm" (lon): minutes to 5 places, about 2 cm. */
    private static String degMin(double deg, int degWidth) {
        final long scaled = Math.round(Math.abs(deg) * 6_000_000.0);   // minutes x 100000
        final long d = scaled / 6_000_000L;
        final long m = scaled % 6_000_000L;
        return pad(d, degWidth) + pad(m / 100_000L, 2) + "." + pad(m % 100_000L, 5);
    }

    static String latField(double lat) {
        return degMin(lat, 2);
    }

    static String lonField(double lon) {
        return degMin(lon, 3);
    }

    private static String latHemi(double lat) {
        return lat < 0 ? "S" : "N";
    }

    private static String lonHemi(double lon) {
        return lon < 0 ? "W" : "E";
    }

    /** UTC "hhmmss.ss". */
    static String time(long utcMs) {
        final long ms = Math.floorMod(utcMs, 86_400_000L);
        return pad(ms / 3_600_000L, 2) + pad(ms / 60_000L % 60, 2) + pad(ms / 1000 % 60, 2) + "."
                + pad(ms % 1000 / 10, 2);
    }

    /** UTC "ddmmyy" (civil-from-days, H. Hinnant). */
    static String date(long utcMs) {
        final long z = Math.floorDiv(utcMs, 86_400_000L) + 719_468L;
        final long era = Math.floorDiv(z, 146_097L);
        final long doe = z - era * 146_097L;
        final long yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
        final long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        final long mp = (5 * doy + 2) / 153;
        final long d = doy - (153 * mp + 2) / 5 + 1;
        final long m = mp < 10 ? mp + 3 : mp - 9;
        final long y = yoe + era * 400 + (m <= 2 ? 1 : 0);
        return pad(d, 2) + pad(m, 2) + pad(Math.floorMod(y, 100L), 2);
    }
}
