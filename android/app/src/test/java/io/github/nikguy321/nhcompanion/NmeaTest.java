package io.github.nikguy321.nhcompanion;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.time.Instant;
import java.time.ZoneOffset;
import java.time.ZonedDateTime;
import java.util.List;
import java.util.Locale;
import org.junit.Test;

public class NmeaTest {
    // 2026-03-01 12:34:56.789 UTC
    private static final long T = Instant.parse("2026-03-01T12:34:56.789Z").toEpochMilli();

    private static String body(String line) {
        assertTrue(line, line.startsWith("$") && line.endsWith("\r\n"));
        final String s = line.substring(1, line.length() - 2);
        final int star = s.lastIndexOf('*');
        assertEquals(String.format(Locale.US, "%02X", Nmea.checksum(s.substring(0, star))), s.substring(star + 1));
        return s.substring(0, star);
    }

    @Test
    public void checksumOfAKnownSentence() {
        // The classic example from NMEA references.
        assertEquals(0x47, Nmea.checksum("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"));
    }

    @Test
    public void ggaFieldsAndCount() {
        // Royal Observatory, Greenwich: a public landmark, west of the meridian.
        final String b = body(Nmea.gga(T, 51.477928, -0.001545, 9, 0.8, 46.2));
        final String[] f = b.split(",", -1);
        assertEquals(15, f.length);
        assertEquals("GPGGA", f[0]);
        assertEquals("123456.78", f[1]);
        assertEquals("5128.67568", f[2]);
        assertEquals("N", f[3]);
        assertEquals("00000.09270", f[4]);
        assertEquals("W", f[5]);
        assertEquals("1", f[6]);
        assertEquals("09", f[7]);
        assertEquals("0.8", f[8]);
        assertEquals("46.2", f[9]);
        assertEquals("M", f[10]);
    }

    @Test
    public void ggaLeavesUnknownsEmpty() {
        final String[] f = body(Nmea.gga(T, -33.8568, 151.2153, -1, Double.NaN, Double.NaN)).split(",", -1);
        assertEquals(15, f.length);
        assertEquals("3351.40800", f[2]);
        assertEquals("S", f[3]);
        assertEquals("15112.91800", f[4]);
        assertEquals("E", f[5]);
        assertEquals("", f[7]);
        assertEquals("", f[8]);
        assertEquals("", f[9]);
    }

    @Test
    public void rmcFieldsAndDate() {
        final String[] f = body(Nmea.rmc(T, 51.477928, -0.001545, 10.0, 271.04)).split(",", -1);
        assertEquals(13, f.length);
        assertEquals("GPRMC", f[0]);
        assertEquals("A", f[2]);
        assertEquals("19.4", f[7]);           // 10 m/s in knots
        assertEquals("271.0", f[8]);
        assertEquals("010326", f[9]);
        assertEquals("A", f[12]);
    }

    @Test
    public void noFixSentences() {
        final String[] g = body(Nmea.ggaNoFix(T)).split(",", -1);
        assertEquals(15, g.length);
        assertEquals("0", g[6]);
        assertEquals("", g[2]);
        final String[] r = body(Nmea.rmcNoFix(T)).split(",", -1);
        assertEquals(13, r.length);
        assertEquals("V", r[2]);
        assertEquals("010326", r[9]);
        assertEquals("N", r[12]);
    }

    /** The bug that made one app's fixes 2-17 km off: minutes just above a whole minute. */
    @Test
    public void tinyMinutesNeverUseExponents() {
        assertEquals("4000.00006", Nmea.latField(40.000001));
        assertEquals("4000.00000", Nmea.latField(40.0000000001));
        assertEquals("10000.00006", Nmea.lonField(-100.000001));
    }

    @Test
    public void minutesThatRoundUpCarryIntoDegrees() {
        assertEquals("4100.00000", Nmea.latField(40.99999999999));
        assertEquals("18000.00000", Nmea.lonField(179.9999999999));
    }

    @Test
    public void coordinatesRoundTripEverywhere() {
        for (double lat = -89.9; lat < 90; lat += 7.3) {
            for (double lon = -179.9; lon < 180; lon += 11.7) {
                final String la = Nmea.latField(lat);
                final String lo = Nmea.lonField(lon);
                final double backLat = Integer.parseInt(la.substring(0, 2)) + Double.parseDouble(la.substring(2)) / 60.0;
                final double backLon = Integer.parseInt(lo.substring(0, 3)) + Double.parseDouble(lo.substring(3)) / 60.0;
                assertEquals(Math.abs(lat), backLat, 1e-6);
                assertEquals(Math.abs(lon), backLon, 1e-6);
            }
        }
    }

    @Test
    public void numbersIgnoreThePhonesLanguage() {
        final Locale was = Locale.getDefault();
        try {
            Locale.setDefault(Locale.GERMANY);
            assertTrue(Nmea.gga(T, 51.5, 7.5, 8, 1.2, 100.5).contains("1.2,100.5"));
            assertEquals("0.5", Nmea.fixed(0.45, 1));
            assertEquals("-12.3", Nmea.fixed(-12.34, 1));
            assertEquals("0.0", Nmea.fixed(-0.04, 1));
        } finally {
            Locale.setDefault(was);
        }
    }

    @Test
    public void timeAndDateMatchJavaTimeAcrossYears() {
        for (long t = Instant.parse("1999-12-31T23:59:59.999Z").toEpochMilli();
             t < Instant.parse("2101-01-01T00:00:00Z").toEpochMilli(); t += 86_400_000L * 37 + 3_723_456L) {
            final ZonedDateTime z = Instant.ofEpochMilli(t).atZone(ZoneOffset.UTC);
            assertEquals(String.format(Locale.US, "%02d%02d%02d", z.getDayOfMonth(), z.getMonthValue(), z.getYear() % 100),
                    Nmea.date(t));
            assertEquals(String.format(Locale.US, "%02d%02d%02d.%02d", z.getHour(), z.getMinute(), z.getSecond(),
                    z.getNano() / 10_000_000), Nmea.time(t));
        }
    }

    @Test
    public void hdopStandIn() {
        assertTrue(Double.isNaN(Nmea.hdopFromAccuracy(0f)));
        assertTrue(Double.isNaN(Nmea.hdopFromAccuracy(-1f)));
        assertEquals(0.5, Nmea.hdopFromAccuracy(1f), 1e-9);
        assertEquals(2.5, Nmea.hdopFromAccuracy(10f), 1e-9);
        assertEquals(99.9, Nmea.hdopFromAccuracy(5000f), 1e-9);
    }

    @Test
    public void relayKeepsGoodLinesAndAddsCrlf() {
        final String gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
        final List<String> one = Nmea.relayable(gga);
        assertEquals(1, one.size());
        assertEquals(gga + "\r\n", one.get(0));
        assertEquals("GGA", Nmea.typeOf(one.get(0)));
        // Already terminated, lower-case checksum, trailing junk spaces.
        assertEquals(1, Nmea.relayable(gga.replace("*47", "*47") + "\r\n").size());
    }

    @Test
    public void relaySplitsGluedSentences() {
        final String a = Nmea.rmc(T, 10, 20, 0, 0).trim();
        final String b = Nmea.gga(T, 10, 20, 7, 1.0, 5).trim();
        final List<String> out = Nmea.relayable(a + b);
        assertEquals(2, out.size());
        assertEquals("RMC", Nmea.typeOf(out.get(0)));
        assertEquals("GGA", Nmea.typeOf(out.get(1)));
    }

    @Test
    public void relayDropsWhatTheDevicesDoNotNeed() {
        assertTrue(Nmea.relayable(Nmea.wrap("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00")).isEmpty());
        assertTrue(Nmea.relayable(Nmea.wrap("PQGSA,blah")).isEmpty());
        assertTrue(Nmea.relayable("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48").isEmpty());
        assertTrue(Nmea.relayable("$GPGGA,123519,4807.038,N").isEmpty());
        assertTrue(Nmea.relayable("").isEmpty());
        assertTrue(Nmea.relayable(null).isEmpty());
        final StringBuilder longBody = new StringBuilder("GNGSA,A,3");
        while (longBody.length() < 100) {
            longBody.append(",01");
        }
        assertTrue(Nmea.relayable(Nmea.wrap(longBody.toString())).isEmpty());
        assertEquals(1, Nmea.relayable(Nmea.wrap("GNGSA,A,3,05,07,13,,,,,,,,,,1.6,0.9,1.3,1")).size());
        assertEquals(1, Nmea.relayable(Nmea.wrap("GPGST,123519.00,1.2,2.1,1.5,45.0,1.6,1.9,2.8")).size());
    }

    @Test
    public void typeOf() {
        assertEquals("RMC", Nmea.typeOf("$GNRMC,1"));
        assertNull(Nmea.typeOf("GPRMC,1"));
        assertNull(Nmea.typeOf("$GPRMC"));
    }
}
