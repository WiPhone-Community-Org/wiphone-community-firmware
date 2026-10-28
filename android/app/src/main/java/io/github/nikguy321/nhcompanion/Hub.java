package io.github.nikguy321.nhcompanion;

import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.LinkedBlockingDeque;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The TCP port the WiPhone and the e-reader read from. Plain java.net, so it is tested on the Mac.
 *
 * <p>Each device gets its own small queue and writer thread. When a device stops reading, its
 * queue drops the OLDEST lines, so whatever it reads next is current (gpsdRelay keeps the oldest,
 * which hands a new client lines that can be hours old). A write stuck for {@link #STUCK_MS}
 * means the device has gone without saying so; it is disconnected.
 */
final class Hub {
    interface Events {
        /** Null to let this device read, or why not (the socket is then closed). Called on the accept thread. */
        String admit(Socket s);

        /** A device connected or left. Called on the accept or a writer thread. */
        void changed();
    }

    static final int QUEUE_LINES = 64;
    static final long STUCK_MS = 30_000L;

    private final int port;
    private final int maxClients;
    private final Events events;
    private final List<Client> clients = new CopyOnWriteArrayList<>();
    private volatile ServerSocket server;
    private volatile boolean stopped = true;

    Hub(int port, int maxClients, Events events) {
        this.port = port;
        this.maxClients = maxClients;
        this.events = events;
    }

    /** Opens the port. Throws when another app holds it. */
    synchronized void start() throws IOException {
        if (!stopped) {
            return;
        }
        final ServerSocket ss = new ServerSocket();
        ss.setReuseAddress(true);
        try {
            ss.bind(new InetSocketAddress(port), 8);
        } catch (IOException e) {
            closeQuietly(ss);
            throw e;
        }
        server = ss;
        stopped = false;
        final Thread a = new Thread(this::acceptLoop, "hub-accept");
        a.setDaemon(true);
        a.start();
        final Thread w = new Thread(this::watchdog, "hub-watchdog");
        w.setDaemon(true);
        w.start();
    }

    synchronized void stop() {
        stopped = true;
        closeQuietly(server);
        for (Client c : clients) {
            c.close();
        }
    }

    boolean isOpen() {
        return !stopped;
    }

    /** The port actually bound (tests bind port 0). */
    int boundPort() {
        final ServerSocket s = server;
        return s == null ? -1 : s.getLocalPort();
    }

    int clientCount() {
        return clients.size();
    }

    /** Queue text for every connected device. */
    void broadcast(String text) {
        for (Client c : clients) {
            c.offer(text);
        }
    }

    static final class Info {
        final String address;
        final long sinceMs;
        final long bytes;
        final long dropped;

        Info(String address, long sinceMs, long bytes, long dropped) {
            this.address = address;
            this.sinceMs = sinceMs;
            this.bytes = bytes;
            this.dropped = dropped;
        }
    }

    List<Info> snapshot() {
        final List<Info> out = new ArrayList<>();
        for (Client c : clients) {
            out.add(new Info(c.address, c.since, c.bytes, c.dropped));
        }
        return out;
    }

    private void acceptLoop() {
        final ServerSocket ss = server;
        while (!stopped) {
            final Socket s;
            try {
                s = ss.accept();
            } catch (IOException e) {
                if (stopped) {
                    return;
                }
                sleep(500);
                continue;
            }
            String why;
            try {
                why = events.admit(s);
            } catch (RuntimeException e) {
                why = "could not check its network";
            }
            if (why == null && clients.size() >= maxClients) {
                why = "too many devices";
            }
            if (why != null) {
                closeQuietly(s);
                continue;
            }
            final Client c;
            try {
                s.setTcpNoDelay(true);
                s.setKeepAlive(true);
                c = new Client(s);
            } catch (IOException e) {
                closeQuietly(s);
                continue;
            }
            // Under the lock stop() holds: a device is either added before stop() (and closed by
            // it) or not at all.
            synchronized (this) {
                if (stopped) {
                    closeQuietly(s);
                    return;
                }
                clients.add(c);
            }
            c.start();
            events.changed();
        }
    }

    private void watchdog() {
        while (!stopped) {
            sleep(5_000);
            final long now = System.currentTimeMillis();
            for (Client c : clients) {
                final long w = c.writingSince;
                if (w != 0 && now - w > STUCK_MS) {
                    c.close();
                }
            }
        }
    }

    private final class Client implements Runnable {
        final Socket socket;
        final String address;
        final long since = System.currentTimeMillis();
        final LinkedBlockingDeque<String> queue = new LinkedBlockingDeque<>(QUEUE_LINES);
        final AtomicBoolean closed = new AtomicBoolean();
        final OutputStream out;
        volatile long bytes;
        volatile long dropped;
        volatile long writingSince;

        Client(Socket s) throws IOException {
            socket = s;
            address = s.getInetAddress() == null ? "?" : s.getInetAddress().getHostAddress();
            out = s.getOutputStream();
        }

        void start() {
            final Thread t = new Thread(this, "hub-client-" + address);
            t.setDaemon(true);
            t.start();
        }

        void offer(String text) {
            while (!queue.offerLast(text)) {
                if (queue.pollFirst() != null) {
                    dropped++;
                }
            }
        }

        @Override
        public void run() {
            try {
                while (!closed.get()) {
                    final String t = queue.pollFirst(1, TimeUnit.SECONDS);
                    if (t == null) {
                        continue;
                    }
                    final byte[] b = t.getBytes(StandardCharsets.US_ASCII);
                    writingSince = System.currentTimeMillis();
                    out.write(b);
                    out.flush();
                    writingSince = 0;
                    bytes += b.length;
                }
            } catch (IOException | InterruptedException e) {
                // the device left; close() below says so
            }
            close();
        }

        void close() {
            if (closed.compareAndSet(false, true)) {
                closeQuietly(socket);
                clients.remove(this);
                events.changed();
            }
        }
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static void closeQuietly(java.io.Closeable c) {
        if (c == null) {
            return;
        }
        try {
            c.close();
        } catch (IOException e) {
            // nothing to do
        }
    }
}
