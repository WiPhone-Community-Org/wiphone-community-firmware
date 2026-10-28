package io.github.nikguy321.nhcompanion;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.After;
import org.junit.Test;

public class HubTest {
    private Hub hub;
    private final AtomicInteger changes = new AtomicInteger();
    private volatile String verdict;

    private final Hub.Events events = new Hub.Events() {
        @Override
        public String admit(Socket s) {
            return verdict;
        }

        @Override
        public void changed() {
            changes.incrementAndGet();
        }
    };

    @After
    public void tearDown() {
        if (hub != null) {
            hub.stop();
        }
    }

    private Socket connect() throws IOException {
        final Socket s = new Socket();
        s.connect(new InetSocketAddress("127.0.0.1", hub.boundPort()), 2000);
        s.setSoTimeout(3000);
        return s;
    }

    private static void waitFor(java.util.function.BooleanSupplier ok) throws InterruptedException {
        for (int i = 0; i < 100 && !ok.getAsBoolean(); i++) {
            Thread.sleep(20);
        }
        assertTrue(ok.getAsBoolean());
    }

    private static String readLine(InputStream in) throws IOException {
        final StringBuilder b = new StringBuilder();
        int c;
        while ((c = in.read()) >= 0) {
            b.append((char) c);
            if (c == '\n') {
                break;
            }
        }
        return b.toString();
    }

    @Test
    public void broadcastsToEveryDevice() throws Exception {
        hub = new Hub(0, 8, events);
        hub.start();
        try (Socket a = connect(); Socket b = connect()) {
            waitFor(() -> hub.clientCount() == 2);
            hub.broadcast("$X*00\r\n");
            assertEquals("$X*00\r\n", readLine(a.getInputStream()));
            assertEquals("$X*00\r\n", readLine(b.getInputStream()));
        }
        waitFor(() -> {
            hub.broadcast("tick\r\n");
            return hub.clientCount() == 0;
        });
    }

    @Test
    public void refusedDevicesAreClosedAtOnce() throws Exception {
        hub = new Hub(0, 8, events);
        hub.start();
        verdict = "not on the hotspot";
        try (Socket a = connect()) {
            assertEquals(-1, a.getInputStream().read());
        }
        assertEquals(0, hub.clientCount());
    }

    @Test
    public void tooManyDevices() throws Exception {
        hub = new Hub(0, 1, events);
        hub.start();
        try (Socket a = connect()) {
            waitFor(() -> hub.clientCount() == 1);
            try (Socket b = connect()) {
                assertEquals(-1, b.getInputStream().read());
            }
            assertEquals(1, hub.clientCount());
        }
    }

    @Test
    public void aSlowReaderGetsTheNewestLines() throws Exception {
        hub = new Hub(0, 8, events);
        hub.start();
        try (Socket a = connect()) {
            waitFor(() -> hub.clientCount() == 1);
            // Fill far past the queue and the socket buffers without reading.
            final StringBuilder big = new StringBuilder();
            for (int i = 0; i < 2000; i++) {
                big.append('x');
            }
            for (int i = 0; i < 2000; i++) {
                hub.broadcast("old " + big + "\r\n");
            }
            hub.broadcast("NEWEST\r\n");
            final InputStream in = a.getInputStream();
            String line;
            long n = 0;
            do {
                line = readLine(in);
                n++;
            } while (!line.isEmpty() && !line.startsWith("NEWEST"));
            assertEquals("NEWEST\r\n", line);
            assertTrue("old lines must have been dropped, read " + n, n < 2000);
            assertTrue(hub.snapshot().get(0).dropped > 0);
        }
    }

    @Test
    public void aBusyPortSaysSo() throws Exception {
        try (ServerSocket taken = new ServerSocket(0)) {
            hub = new Hub(taken.getLocalPort(), 8, events);
            try {
                hub.start();
                fail("bound a port already in use");
            } catch (IOException expected) {
                // gpsdRelay holding 10110 looks like this
            }
            assertTrue(!hub.isOpen());
        }
    }

    @Test
    public void stopClosesEveryone() throws Exception {
        hub = new Hub(0, 8, events);
        hub.start();
        try (Socket a = connect()) {
            waitFor(() -> hub.clientCount() == 1);
            hub.stop();
            assertEquals(-1, a.getInputStream().read());
        }
        assertEquals(0, hub.clientCount());
    }
}
