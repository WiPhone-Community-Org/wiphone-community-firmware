package io.github.nikguy321.nhcompanion;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.util.Collections;
import java.util.HashSet;
import java.util.Set;
import org.junit.Test;


public class PolicyTest {
    private static byte[] v4(int a, int b, int c, int d) {
        return new byte[] {(byte) a, (byte) b, (byte) c, (byte) d};
    }

    private static byte[] mapped(byte[] v4) {
        final byte[] a = new byte[16];
        a[10] = (byte) 0xff;
        a[11] = (byte) 0xff;
        System.arraycopy(v4, 0, a, 12, 4);
        return a;
    }

    // The phone: on a café's Wi-Fi as 10.0.0.23 (wlan0), mobile data on rmnet_data1, hotspot on swlan0.
    private final Set<String> ifs = new HashSet<>();
    private final Set<String> addrs = new HashSet<>();

    {
        ifs.add("wlan0");
        ifs.add("rmnet_data1");
        addrs.add(Policy.key(v4(10, 0, 0, 23)));
        addrs.add(Policy.key(v4(100, 81, 4, 7)));
    }

    /** The old shape: no prefix known, remote not one of ours. */
    private static String refuse(byte[] local, String localIf, Set<String> ifs, Set<String> addrs, byte[] remote,
                                 boolean allowLoopback) {
        return Policy.refuse(local, localIf, -1, remote, false, ifs, addrs, allowLoopback);
    }

    @Test
    public void hotspotDeviceMayRead() {
        assertNull(refuse(v4(192, 168, 43, 1), "swlan0", ifs, addrs, v4(192, 168, 43, 120), false));
        assertNull(refuse(mapped(v4(192, 168, 43, 1)), "swlan0", ifs, addrs, mapped(v4(192, 168, 43, 120)), false));
        // An unknown interface name still passes on the address checks alone.
        assertNull(refuse(v4(10, 112, 5, 1), null, ifs, addrs, v4(10, 112, 5, 77), false));
        // iPhone-style and USB tethering subnets.
        assertNull(refuse(v4(172, 20, 10, 1), "rndis0", ifs, addrs, v4(172, 20, 10, 2), false));
    }

    @Test
    public void theCafesNetworkMayNot() {
        assertNotNull(refuse(v4(10, 0, 0, 23), "wlan0", ifs, addrs, v4(10, 0, 0, 99), false));
        // Address matches even when the interface name is unknown.
        assertNotNull(refuse(v4(10, 0, 0, 23), null, ifs, addrs, v4(10, 0, 0, 99), false));
        // Interface matches even if the address list missed it.
        assertNotNull(refuse(v4(10, 0, 0, 24), "wlan0", ifs, Collections.emptySet(), v4(10, 0, 0, 99), false));
    }

    @Test
    public void mobileDataAndVpnMayNot() {
        assertNotNull(refuse(v4(100, 81, 4, 7), "rmnet_data1", ifs, addrs, v4(8, 8, 8, 8), false));
        assertNotNull(refuse(v4(192, 0, 0, 4), "v4-rmnet_data1", Collections.emptySet(), Collections.emptySet(),
                v4(192, 168, 1, 5), false));
        assertNotNull(refuse(v4(10, 8, 0, 2), "tun0", Collections.emptySet(), Collections.emptySet(),
                v4(10, 8, 0, 1), false));
    }

    @Test
    public void appsOnThisPhoneMayNotUnlessDebug() {
        assertNotNull(refuse(v4(127, 0, 0, 1), "lo", ifs, addrs, v4(127, 0, 0, 1), false));
        final byte[] six = new byte[16];
        six[15] = 1;
        assertNotNull(refuse(six, null, ifs, addrs, six, false));
        assertNull(refuse(v4(127, 0, 0, 1), "lo", ifs, addrs, v4(127, 0, 0, 1), true));
    }

    /**
     * An app on the phone connecting to the phone's own hotspot address: Linux delivers it locally,
     * source = destination, and neither end is loopback (review finding, 2026-10-11).
     */
    @Test
    public void appsUsingTheHotspotAddressMayNot() {
        final byte[] hs = v4(192, 168, 43, 1);
        assertEquals(Policy.FROM_THIS_PHONE, Policy.refuse(hs, "swlan0", 24, hs, true, ifs, addrs, false));
        // Even when the interface lookup could not say the remote is ours.
        assertEquals(Policy.FROM_THIS_PHONE, Policy.refuse(hs, "swlan0", 24, hs, false, ifs, addrs, false));
        // Source bound to the phone's Wi-Fi address instead.
        assertEquals(Policy.FROM_THIS_PHONE,
                Policy.refuse(hs, "swlan0", 24, v4(10, 0, 0, 23), false, ifs, addrs, false));
        // Or to another of its own addresses the networks list missed.
        assertEquals(Policy.FROM_THIS_PHONE,
                Policy.refuse(hs, "swlan0", 24, v4(192, 168, 7, 1), true, ifs, addrs, false));
        // The debug loopback allowance does not open this.
        assertEquals(Policy.FROM_THIS_PHONE, Policy.refuse(hs, "swlan0", 24, hs, true, ifs, addrs, true));
    }

    @Test
    public void onlyTheHotspotsOwnSubnet() {
        final byte[] hs = v4(192, 168, 43, 1);
        assertNull(Policy.refuse(hs, "swlan0", 24, v4(192, 168, 43, 200), false, ifs, addrs, false));
        // A packet aimed at the hotspot address from another private network.
        assertNotNull(Policy.refuse(hs, "swlan0", 24, v4(192, 168, 44, 2), false, ifs, addrs, false));
        assertNotNull(Policy.refuse(hs, "swlan0", 24, v4(10, 0, 0, 99), false, ifs, addrs, false));
        // Unknown prefix: the other checks decide.
        assertNull(Policy.refuse(hs, "swlan0", -1, v4(192, 168, 44, 2), false, ifs, addrs, false));
    }

    @Test
    public void wifiDirectAndAwarePeersMayNot() {
        assertNotNull(refuse(v4(192, 168, 49, 1), "p2p-wlan0-0", ifs, addrs, v4(192, 168, 49, 22), false));
        assertNotNull(refuse(v4(192, 168, 49, 1), "p2p0", ifs, addrs, v4(192, 168, 49, 22), false));
        assertNotNull(refuse(v4(169, 254, 1, 1), "aware_data0", ifs, addrs, v4(169, 254, 1, 2), false));
    }

    @Test
    public void samePrefix() {
        assertTrue(Policy.samePrefix(v4(192, 168, 43, 1), v4(192, 168, 43, 254), 24));
        assertFalse(Policy.samePrefix(v4(192, 168, 43, 1), v4(192, 168, 42, 1), 24));
        assertTrue(Policy.samePrefix(v4(172, 20, 10, 1), v4(172, 20, 10, 14), 28));
        assertFalse(Policy.samePrefix(v4(172, 20, 10, 1), v4(172, 20, 10, 17), 28));
        assertTrue(Policy.samePrefix(v4(10, 1, 2, 3), v4(99, 9, 9, 9), 0));
        assertFalse(Policy.samePrefix(v4(10, 1, 2, 3), new byte[16], 8));
        assertFalse(Policy.samePrefix(v4(10, 1, 2, 3), v4(10, 1, 2, 3), 33));
    }

    @Test
    public void publicAddressesMayNot() {
        assertNotNull(refuse(v4(192, 168, 43, 1), "swlan0", ifs, addrs, v4(203, 0, 113, 9), false));
    }

    @Test
    public void privateRanges() {
        assertTrue(Policy.isPrivate(v4(10, 1, 2, 3)));
        assertTrue(Policy.isPrivate(v4(172, 16, 0, 1)));
        assertTrue(Policy.isPrivate(v4(172, 31, 255, 1)));
        assertFalse(Policy.isPrivate(v4(172, 32, 0, 1)));
        assertTrue(Policy.isPrivate(v4(192, 168, 0, 1)));
        assertTrue(Policy.isPrivate(v4(169, 254, 3, 4)));
        assertFalse(Policy.isPrivate(v4(8, 8, 8, 8)));
        final byte[] ll = new byte[16];
        ll[0] = (byte) 0xfe;
        ll[1] = (byte) 0x80;
        assertTrue(Policy.isPrivate(ll));
        final byte[] ula = new byte[16];
        ula[0] = (byte) 0xfd;
        assertTrue(Policy.isPrivate(ula));
        final byte[] global = new byte[16];
        global[0] = 0x20;
        global[1] = 0x01;
        assertFalse(Policy.isPrivate(global));
    }

    @Test
    public void keyUnmapsV4() {
        assertEquals(Policy.key(v4(192, 168, 1, 2)), Policy.key(mapped(v4(192, 168, 1, 2))));
        assertEquals("c0a80102", Policy.key(v4(192, 168, 1, 2)));
    }
}
