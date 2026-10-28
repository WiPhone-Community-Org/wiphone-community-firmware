package io.github.nikguy321.nhcompanion;

import java.util.Set;

/**
 * Who may read the position: devices on this phone's own hotspot (or USB / Bluetooth tethering),
 * nobody else.
 *
 * <p>The port listens on every interface, so the check is made per connection:
 * <ul>
 *   <li>Connections from the phone itself are refused, because any app could then read the
 *       position without location permission. That includes an app connecting to the phone's
 *       own hotspot address: Linux delivers it locally, and neither end is loopback.</li>
 *   <li>An interface Android lists as a network the phone USES (its Wi-Fi, mobile data, a VPN)
 *       is upstream, so a connection that arrived on one comes from someone else's network. Wi-Fi
 *       Direct and Wi-Fi Aware links are refused too: they aren't Android networks either, but
 *       they aren't the hotspot. Tethering interfaces are not in Android's list, so their devices
 *       are let in.</li>
 *   <li>The device must be on the subnet of the interface it connected on. That turns away a
 *       packet aimed at the hotspot address from another network.</li>
 * </ul>
 */
final class Policy {
    private Policy() {}

    /** Interface names that are always upstream: mobile data, 464xlat, VPNs, loopback. */
    private static final String[] UPSTREAM_PREFIXES = {
        "rmnet", "ccmni", "seth_", "pdp", "v4-", "tun", "ipsec", "ppp", "dummy", "lo"
    };

    /** Wi-Fi Direct and Wi-Fi Aware: peers (printers, screen mirroring, file sharing), not the hotspot. */
    private static final String[] PEER_PREFIXES = {"p2p", "aware", "nan"};

    static final String FROM_THIS_PHONE = "from an app on this phone";

    /**
     * Null when the connection may read, else why not, in a few plain words.
     *
     * @param local the address the device connected TO (this phone's address on that network)
     * @param localIf the interface that address is on, or null when unknown
     * @param localPrefix that address's subnet prefix length, or -1 when unknown
     * @param remote the device's address
     * @param remoteIsOwn the device's address is one of this phone's own addresses
     * @param upstreamIfs interface names of every network the phone uses
     * @param upstreamAddrs {@link #key} of every address on those networks
     * @param allowLoopback debug builds only: lets "adb forward" in
     */
    static String refuse(byte[] local, String localIf, int localPrefix, byte[] remote, boolean remoteIsOwn,
                         Set<String> upstreamIfs, Set<String> upstreamAddrs, boolean allowLoopback) {
        local = unmap(local);
        remote = unmap(remote);
        if (local == null || remote == null) {
            return "unknown address";
        }
        if (isLoopback(local) || isLoopback(remote)) {
            return allowLoopback ? null : FROM_THIS_PHONE;
        }
        final String remoteKey = key(remote);
        if (remoteIsOwn || remoteKey.equals(key(local)) || upstreamAddrs.contains(remoteKey)) {
            return FROM_THIS_PHONE;
        }
        if (upstreamAddrs.contains(key(local))) {
            return "from the network this phone is on, not its hotspot";
        }
        if (localIf != null) {
            if (upstreamIfs.contains(localIf)) {
                return "from the network this phone is on, not its hotspot";
            }
            for (String p : UPSTREAM_PREFIXES) {
                if (localIf.startsWith(p)) {
                    return "from mobile data or a VPN, not the hotspot";
                }
            }
            for (String p : PEER_PREFIXES) {
                if (localIf.startsWith(p)) {
                    return "from a Wi-Fi Direct peer, not the hotspot";
                }
            }
        }
        if (!isPrivate(remote)) {
            return "from a public address";
        }
        if (localPrefix >= 0 && !samePrefix(local, remote, localPrefix)) {
            return "not on the hotspot's own subnet";
        }
        return null;
    }

    /** a and b agree in their first `bits` bits (false when one is IPv4 and the other IPv6). */
    static boolean samePrefix(byte[] a, byte[] b, int bits) {
        a = unmap(a);
        b = unmap(b);
        if (a == null || b == null || a.length != b.length || bits > a.length * 8) {
            return false;
        }
        for (int i = 0; i < bits; i++) {
            final int mask = 0x80 >> (i & 7);
            if ((a[i >> 3] & mask) != (b[i >> 3] & mask)) {
                return false;
            }
        }
        return true;
    }
    /** Hex of the address bytes, IPv4-mapped IPv6 written as the IPv4 address. */
    static String key(byte[] a) {
        a = unmap(a);
        if (a == null) {
            return "";
        }
        final StringBuilder b = new StringBuilder(a.length * 2);
        for (byte x : a) {
            b.append(Character.forDigit((x >> 4) & 15, 16)).append(Character.forDigit(x & 15, 16));
        }
        return b.toString();
    }

    /** ::ffff:a.b.c.d becomes a.b.c.d; anything else is returned as it is. */
    static byte[] unmap(byte[] a) {
        if (a == null || a.length != 16) {
            return a;
        }
        for (int i = 0; i < 10; i++) {
            if (a[i] != 0) {
                return a;
            }
        }
        if (a[10] != (byte) 0xff || a[11] != (byte) 0xff) {
            return a;
        }
        return new byte[] {a[12], a[13], a[14], a[15]};
    }

    static boolean isLoopback(byte[] a) {
        if (a.length == 4) {
            return (a[0] & 0xff) == 127;
        }
        for (int i = 0; i < 15; i++) {
            if (a[i] != 0) {
                return false;
            }
        }
        return a[15] == 1;
    }

    /** RFC 1918, IPv4 link-local, IPv6 link-local and unique-local: what a hotspot hands out. */
    static boolean isPrivate(byte[] a) {
        if (a.length == 4) {
            final int b0 = a[0] & 0xff;
            final int b1 = a[1] & 0xff;
            return b0 == 10 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168)
                    || (b0 == 169 && b1 == 254);
        }
        final int b0 = a[0] & 0xff;
        final int b1 = a[1] & 0xff;
        return (b0 == 0xfe && (b1 & 0xc0) == 0x80) || (b0 & 0xfe) == 0xfc;
    }
}
