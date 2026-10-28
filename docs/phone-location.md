# Phone location: your phone's GPS on a WiPhone, over the phone's hotspot

A WiPhone without the GPS backplate can still know where it is. Join it to your phone's
WiFi hotspot and run a small app on the phone that shares the phone's GPS. The WiPhone finds
the app by itself and uses that position:

- for **"me (phone)" on the map**, distances and bearings, the Almanac and the weather;
- for **position reports to the mesh**, if you turned those on (My node → Position) and the fix
  is good enough. See [what gets sent](#what-gets-sent-to-the-mesh);
- for the **time zone** when Settings → Time zone is on Auto (0.9.82): the zone where the phone's
  fix is, after two looks two minutes apart, never near a zone line. A phone's fix counts only
  when no other clock is within 10 km, whatever app sends it: even a fix with good satellite
  numbers can be kilometres off (gpsdRelay with NMEA generation on, GPS 2 IP on another port).
  The backplate GPS only has to be clear of the line itself.

No Bluetooth is involved. The WiPhone only reads: it never sends anything to the app, and it
never uses the phone's data to set its clock's time (UTC) - only, on Auto, which zone to show
it in.

**Which source wins:**
1. The WiPhone's own GPS backplate, when it has a fix.
2. The phone over the hotspot.
3. A [Wi-Fi location](#wi-fi-location) from the access points around it, only when it is
   genuinely local and accurate. It is off unless you turn it on.
4. Nothing. A wrong location is worse than none.

The WiPhone never guesses a location from your internet address. On a phone hotspot that
guess lands wherever your carrier's network leaves the internet, often hundreds of kilometres
away.

**Supported apps:**

| Phone | App | Where | Cost | Port | Sent to the mesh? |
|---|---|---|---|---|---|
| Android | **NH Devices Companion** (recommended) | this repo ([download](#3a-android-nh-devices-companion-recommended)) | free, open source | 10110 | yes |
| Android | gpsdRelay | F-Droid | free, open source | 10110 | yes |
| Android | Share GPS | Google Play | free (ads) | 10110 (set it) | yes |
| iPhone | GPS 2 IP | App Store | $7.99 | 11123 (its usual port) | **no**, see below |
| iPhone | GPS Metrics, NMEAconnect | App Store | free / $3.99 | set it to 11123 | **untested**, see [3e](#3e-iphone-other-apps-untested) |

The WiPhone asks the hotspot's gateway, which is your phone, on port 10110 and then on 11123.
You can add one more port under My node, or with `phonepos port <n>` on the serial console.

---

## 1. On the WiPhone (once)

1. Firmware **0.9.82-NH or newer**.
2. **Meshtastic → My node → Phone location: auto**. This is the default. Tapping the row
   switches it between auto and off.
3. Optional, to report your position to the mesh:
   - **My node → Position: every 5 / 15 / 30 min**;
   - **Send to:** a *private* channel your group uses. A public channel (readable by any radio
     in range) has to be confirmed on the WiPhone's screen. The serial console can never choose
     one.
4. Join the phone's hotspot like any WiFi network (Settings → WiFi). Save it so the WiPhone
   rejoins by itself.

The small line under **Phone location** tells you what it found. See
[Troubleshooting](#troubleshooting).

## 2. The phone's hotspot

The WiPhone's radio is 2.4 GHz only and older than WPA3.

**Android (Samsung shown; others are similar).** Settings → Connections → Mobile Hotspot and
Tethering → Mobile Hotspot → Configure:
- **Band:** 2.4 GHz
- **Security:** WPA2-Personal (not WPA3, not "WPA2/WPA3")
- **Advanced:**
  - turn **Protected Management Frames** off;
  - set **Turn off hotspot when no devices are connected** to *Never*, so it does not vanish
    while the WiPhone sleeps.

**iPhone.** Settings → Personal Hotspot → **Allow Others to Join**, and turn on **Maximize
Compatibility** (this makes it 2.4 GHz). Keep the Personal Hotspot screen in mind: iOS can
turn the hotspot off when nothing has been connected for a while.

---

## 3a. Android: NH Devices Companion (recommended)

A small app made for this: it shares the phone's GPS with the WiPhone and the X4 e-reader
on its hotspot, and sets itself up. Its source is in [`android/`](../android/) in this repo.
It shows as **NH Companion** on the home screen. It will grow into the companion app for the
other devices on NH firmware (file transfer is planned); today it does the GPS.

- **Only your hotspot's devices can read the position.** The port answers devices on the
  phone's hotspot (or USB/Bluetooth tethering) and refuses everything else: a café's Wi-Fi the
  phone joined, mobile data, a VPN, and other apps on the phone itself.
- **It makes no internet connections** and keeps no location history.
- **The GPS is on only while a device is connected,** and for 5 minutes after the last one
  leaves. While no device is connected it just waits.
- It relays the GPS chip's own NMEA sentences, which carry real satellite counts. Only on a
  phone whose chip gives Android no NMEA text does it build GGA/RMC from Android's GPS fix.
- It shows a notification while sharing and asks Android to let it run with the screen off.
  gpsdRelay never asks for notification permission, so on Android 13+ its notification can't
  show, and on a Samsung it stops when the screen locks (see [3b](#3b-android-gpsdrelay)).
- On Samsung phones it checks one more setting, described under **Samsung phones** below.

**Status: 1.2, 2026-10-11.**
- Tested on a Galaxy Note 20 Ultra (Android 13) with a WiPhone on its hotspot. With the phone
  locked, unplugged and lying still by a window, the WiPhone had a live fix the whole time.
- Also tested on the Android emulator (Android 15): setup, sharing in deep Doze, restart after a
  reboot or a crash, and refusing other networks and other apps on the phone. Every sentence was
  checked with the WiPhone's own code (`tools/phone_stream_check.cpp`).
- The code was reviewed independently before release.

**Install**
1. On the phone, download
   **[nh-companion.apk](https://github.com/Nikguy321/wiphone-meshtastic/raw/main/android/dist/nh-companion.apk)**
   (32 KB, version 1.2). Its SHA-256 is in [`android/dist/SHA256SUMS`](../android/dist/SHA256SUMS).
   - If Chrome sits at "Downloading…" with the full size shown, it is still checking the file:
     open **My Files → Downloads** and tap the APK there.
   - To update, install the new APK over the old one. Your settings stay.
2. Open the downloaded file. Android asks once whether your browser (or Files) may install
   apps: allow it, go back, and tap **Install**.
3. Google Play Protect may say it doesn't recognise the developer. The app isn't from the Play
   Store, so this is expected. Tap **More details → Install anyway**. You can instead build it
   yourself from [`android/`](../android/README.md).

**Set it up (one button)**
1. **If gpsdRelay is installed, stop it first**: tap its stop (X) button, or switch its server
   off. Only one app can hold port 10110. NH Companion says **Port 10110 is in use** until it is
   free.
2. Open **NH Companion** and tap **Set up everything**. Answer Android's questions in turn:
   - Location: **While using the app**, with **Precise** on;
   - Notifications: **Allow**;
   - Let app always run in background: **Allow**;
   - Start after a restart: **Continue**, then choose **Allow all the time** and go back. This
     step is optional. Without it, after the phone restarts, a notification asks for one tap.
3. It turns sharing on and opens the hotspot settings if the hotspot is off. Set up the hotspot
   as in [section 2](#2-the-phones-hotspot) and turn it on.
4. Join the WiPhone (or the e-reader) to the hotspot. The app's screen and its notification say
   **Sharing with 1 device**, then **GPS fix: 9 satellites, ±4 m** once it has a fix. On the
   WiPhone, Phone location says `this network: fix, 9 sats`.

Every row on the setup list has a tick when it is done. A row without a tick has its own button.
Turning the **Share location on the hotspot** switch off closes the port and turns the GPS off.
The notification's **Stop sharing** button does the same.

**Samsung phones: turn on "Force full GNSS measurements".** Without it, Samsung switches the
GPS off for *every* app a few minutes after the phone is put down: when the screen is off, it
isn't charging, and its motion sensor says the phone is lying still. The WiPhone then shows the
last position as too old, and stops sending it to the mesh. The position comes back the moment
the phone moves.

Samsung's location service skips that rule only when this developer setting is on. Found and
proven on 2026-10-11 from Samsung's own log on a Galaxy Note 20 Ultra (`OldStopController_FLP:
onCheckGpsStopBySensor ... isStationary = TRUE ... isPlugged = false, isMeasurementsMode = false
... so stop gps`):
- without the setting, the GPS stopped about 3 minutes after the phone was put down;
- with it, it ran for 16 minutes without a gap.

NH Companion's setup list has a **Samsung** row for this, and **Set up everything** walks you to
it:
1. If Developer options are hidden: Settings → About phone → Software information → tap
   **Build number** 7 times and enter your PIN.
2. Settings → **Developer options** → turn on **Force full GNSS measurements**.
3. **Leave Developer options on.** Turning them off resets this switch. USB debugging, if you
   used it, can be turned off on its own.

It makes the GPS chip track continuously instead of in short bursts, so it uses a little more
battery, but only while something is using the GPS. NH Companion uses it only while a device is
connected.

Also on Samsung: keep **Power saving** off while you need the position. Power saving can turn
the GPS off while the screen is off, and the app warns you when it is on.

**If something still stops:** the bottom of the app's screen has a **Diagnostics** timeline.
It records screen on/off, Doze, power saving, the GPS engine starting and stopping, and "GPS
data stopped / back after N s". It never records a position. A screenshot of it after a locked
test shows what Android did.

**Battery:** the GPS adds a few percent an hour while a device is connected, on top of the
hotspot itself. With no device connected, the app costs next to nothing.

**The X4 e-reader** (CrossPoint NH fork) reads it the same way with **Locate Me**. From fork
build 1011 on, it waits up to 30 s while the phone's GPS warms up.

## 3b. Android: gpsdRelay

The alternative if you would rather use an app from a store. It is not on Google Play; it is on
**F-Droid**, a store for free open-source apps. On a Samsung phone it stops sending when the
screen locks (see the known limit below), so [NH Companion](#3a-android-nh-devices-companion-recommended)
is the better choice there.

**Install**
1. On the phone, open **f-droid.org** in Chrome and tap **Download F-Droid**. Open the
   downloaded file. Android asks once whether Chrome may install apps: allow it.
2. Open F-Droid, search **gpsdRelay** and install it (version 2.2 or newer).
   - You can instead install just the APK from the
     [gpsdRelay page on F-Droid](https://f-droid.org/packages/io.github.project_kaat.gpsdrelay/)
     or its [GitHub releases](https://github.com/project-kaat/gpsdRelay/releases/latest).
     You then won't get updates automatically.

**Set it up**
1. Open gpsdRelay. When it asks for location, choose **Allow all the time** and keep
   **Precise** on. Allow notifications.
2. Tap the **gear** (Settings):
   - turn **Monitor default network state** **off**. Left on, gpsdRelay stops sharing
     whenever the phone loses cell signal, which is exactly when you are out in the woods;
   - optional: turn on **Start service when Android boots**;
   - leave **NMEA generation interval** at 1000 ms;
   - tap **Apply**.
3. Tap **+** → **TCP Server**:
   - **IPv4 address:** `0.0.0.0` (the default)
   - **Port:** `10110`
   - tick **NMEA relaying**: the phone's real GPS sentences, with real satellite counts;
   - untick **NMEA generation**. It is ticked by default. Its sentences carry no satellite
     count, and gpsdRelay can misprint a generated coordinate. The WiPhone checks for both, but
     relaying is better.
   - Tap **Add**. The list shows `T:0.0.0.0:10110`.
4. **Turn on the switch to the right of `T:0.0.0.0:10110`.** gpsdRelay adds every new server switched
   *off*, and an off server never listens: the WiPhone then says **no app found on this network**.
5. Tap the **play** button at the top. A notification says **NMEA relay is running**. gpsdRelay reads
   its server list only when it starts, so after adding or switching a server, tap **stop (X)**, then
   **play** again.
6. Keep Android from stopping it: Settings → Apps → gpsdRelay → **Battery → Unrestricted**.
   On Samsung, an Unrestricted app may not appear in Background usage limits → Never sleeping
   apps. That is fine: Unrestricted is the setting that matters, and the running relay keeps its
   notification up.

**Known limit, tested 2026-10-11 on a Samsung Galaxy Note 20 (Android 13):** with the phone's screen
locked, gpsdRelay 2.2 stops sending within 1-3 minutes. This happened even with Battery set to
Unrestricted, "Keep open" in Recents, location allowed all the time and NMEA generation on. It
comes back within seconds of waking the phone. gpsdRelay never asks Android for permission to show
notifications, so its "relay is running" notification can never appear, and Samsung then treats it
as an ordinary background app. In practice:
- the WiPhone gets your position whenever your phone is awake;
- two fixes a couple of minutes apart are enough for its automatic time zone;
- the map and Almanac keep the last phone position, marked with its age.

A steady position for the mesh needs the phone awake. This may differ on other phones.

gpsdRelay reads the phone's real GPS only. Expect a fix outdoors or by a window, not deep
indoors. Without a fix the WiPhone says **no fix (indoors?)** and reports nothing.

When you are done, tap the stop button. Anything on a network the phone has joined can read
the position while gpsdRelay runs, including a hotel's WiFi.

## 3c. Android: Share GPS (Google Play)

Share GPS is older (last updated around 2016). It did **not** appear in the Play Store on a Galaxy
Note 20 (Android 13) in October 2026. On older phones that still list it, it works the same way.

1. Install **Share GPS** from Google Play. Allow location (**Allow all the time**, Precise).
2. Open **Connections** and add a new connection: type **NMEA**, method **TCP/IP**.
3. Tick **TCP/IP server** (the phone listens) and set the port to **10110**.
4. Start the connection. Its status says **Listening**.
5. Settings → Apps → Share GPS → Battery → **Unrestricted**. Share GPS's own help says
   battery optimisation makes the WiFi sleep in the background.

## 3d. iPhone: GPS 2 IP

1. Install **GPS 2 IP** from the App Store ($7.99). The free **GPS2IP Lite** won't work: it
   sends RMC only, with no GGA, and stops after 4 minutes. The WiPhone then says
   **the app sends no GGA**.
2. Settings (in the app):
   - **Connection Method: Socket**. The iPhone listens and the WiPhone connects.
   - **Network → port 11123**. Every one of GPS 2 IP's own examples uses it; if yours shows another
     number, set 11123.
   - **Protocol Type: NMEA**, with **GGA** and **RMC** turned on.
   - **Time between sending positions:** 1 s (or *No delay*).
   - **Disable when Static: off**. When it is on, GPS 2 IP goes quiet while you stand still,
     and the WiPhone takes the silence for a lost link.
   - **GPS Accuracy: The very best**.
   - **Operate in background mode: on**.
3. iPhone Settings → Privacy & Security → Location Services → GPS 2 IP → **Always** and
   **Precise Location: on**. With Precise off, iOS moves the position by kilometres on purpose.
4. Turn GPS 2 IP **on** on its main screen.

**What the WiPhone does with an iPhone's position:** it is shown on the WiPhone's map, used
for distances, the Almanac and the weather, and **not sent to the mesh**. iOS gives apps no
satellite count and no accuracy. GPS 2 IP's documentation says it always writes 8 satellites
and HDOP 0.9, so an indoor WiFi-based position looks exactly like a good GPS fix. The WiPhone
recognises those stand-in numbers and won't broadcast a position it can't grade. The status
line says **iPhone app gives no accuracy: shown, not sent**.

## 3e. iPhone: other apps (untested)

These apps say they serve the iPhone's GPS as NMEA over TCP. **None has been tested with a
WiPhone or the e-reader.** If you try one, please report how it went.

| App | Price (US, Oct 2026) | What its listing says |
|---|---|---|
| **GPS Metrics** (Berend Hartman) | free | TCP server or UDP; sends RMC, GGA, GSA, VTG, GLL; can run in the background. |
| **NMEAconnect** (zapfware) | $3.99 | serves the internal GPS as NMEA over TCP/UDP, with background updates. |

Set the app to a **TCP server** on port **11123** (or 10110), sending **GGA and RMC** every
second, with location **Always** and **Precise** on. Then join the WiPhone to the iPhone's hotspot.

⚠ **Turn position reports off, or use a private channel, while you try one.** iOS gives apps no
satellite count or accuracy, so every iPhone app writes stand-in numbers. The WiPhone knows GPS 2
IP's stand-ins and never sends those fixes to the mesh. Another app's stand-ins can look like a
good fix, and the WiPhone would send it.

These won't work:
- **GPS2IP Lite**: RMC only, no GGA.
- **NMEA Send Location**: sends UDP only, and the WiPhone connects over TCP.
- **GPS Server**: not NMEA.

---

## What gets sent to the mesh

A position goes out only when all of these hold:
- you turned reporting on (Position + a Send to channel chosen on the screen);
- it comes from your phone over the hotspot (or the WiPhone's own GPS);
- it is fresh (under 30 s old);
- it has a real rating: at least 4 satellites and HDOP 10 or better.

It is marked **External** (from a phone) and full precision, the same way the official
Meshtastic apps mark a phone's GPS. The WiPhone's own backplate GPS is marked **Internal**.

Never sent:
- an iPhone fix;
- a weak fix;
- anything older than 30 s;
- anything from the serial bench tools.

**A Wi-Fi location** is sent only with **Wi-Fi location: map + mesh**, and only when:
- two scans at least 60 s apart have confirmed it (a fix taken while moving never goes out);
- a scan finished within the 60 s before the send confirmed it again. When a report is due
  and this is all that is missing, the WiPhone asks for a scan and sends on its confirmation;
- no GPS or phone report went out in the last hour within the Wi-Fi fix's accuracy + 50 m of it
  (a receiver keeps the last position it heard: a coarse one must not replace a precise one).

It goes out as a ~182 m cell (Meshtastic `precision_bits` 17, the point moved to the middle of the
cell), with no `location_source` (it is not a measurement), and its time is the time of the scan
that confirmed it. The WiPhone's own map and COVEY draw a ring of that size. While a new fix lies
within the accuracy of the one that produced the last cell, the same cell is sent again, so a
fix near a cell edge does not flip between neighbouring cells.

**Who it trusts.** Whatever answers on the joined network's gateway. On your own hotspot
that is your phone. On someone else's WiFi (a hotel, a campground, a boat's NMEA gateway) a
device serving NMEA on those ports could set where the WiPhone thinks it is. Turn Phone
location **off** on networks you don't control if that matters to you (or pick a named zone in
Settings → Time zone, so the clock's zone cannot follow it). The backplate GPS always wins, and
the clock's time is never touched.

## Wi-Fi location

With no GPS fix and no phone over the hotspot, the WiPhone can work out roughly where it is from
the WiFi access points it hears. It asks **beaconDB** (a free, open, volunteer-run database of
where access points are) and believes the answer only when two separate questions agree.
**Meshtastic → My node → Wi-Fi location** has three settings; tapping the row moves to the next:

| Setting | What happens |
|---|---|
| **off** (the default) | Nothing is scanned for it and nothing is sent anywhere. |
| **map only** | The map shows **me ±60 m Wi-Fi** (written `me +/-60 m Wi-Fi`) with a dotted ring that size; distances, the Almanac and its weather use it. It is never sent to the mesh. |
| **map + mesh** | The same, and position reports may send it, under the stricter rules [below](#what-gets-sent-to-the-mesh). |

The serial console can choose only `netloc off` or `netloc map`. **map + mesh** puts your
position on other people's maps, so it is chosen on the WiPhone's screen, and it starts only when
you **leave the My node screen** with it chosen (the line under the row says
`mesh starts when you leave here`). The row goes off → map only → map + mesh → off, so the way
from map only to off passes through map + mesh; that tap never sends anything.

Switching it **off** stops a lookup that is still waiting or half done: nothing more is sent, and
nothing from it is kept.

**What it sends, and to whom.** When a position is wanted, the WiPhone sends beaconDB
(`https://api.beacondb.net/v1/geolocate`) the hardware addresses (BSSIDs) and signal strengths of
up to 20 nearby access points in each of two requests. Nothing else is sent: no network names, no
details of this phone, a generic `WiPhone-NH` User-Agent with no version. The request turns
beaconDB's own fallback to your internet address off twice (`"considerIp":false` and
`"fallbacks":{"ipf":false}`), and any answer that still says it came from a fallback is thrown
away. Nothing is ever submitted back. beaconDB's privacy page says it keeps its web server logs
(your internet address, the time, the URL and the User-Agent) for 28 days and does not record the
request bodies. The line under the row says this while it is off:
`if on: sends Wi-Fi IDs to beaconDB`.

These access points are never sent:
- hidden networks, and any network whose name ends in `_nomap` or `_optout` (the owner asked);
- addresses that travel with someone: randomised (locally administered) addresses, which most
  phone hotspots use, group addresses, and VRRP virtual routers;
- the network the WiPhone has joined, when it is a phone's hotspot (a location app answered on
  it under Phone location at any time since joining, even if the app has stopped since) or its
  address is a locally administered one. This is checked again when a look starts, and a look
  waits 15 seconds after joining a network so the phone's app can answer first;
- anything weaker than -85 dBm.

**When it asks.** All of these must hold:
- the setting is not off, and the WiPhone is on someone else's WiFi (not its own hotspot);
- there is no live GPS or phone fix (a measured position always wins);
- a position is wanted: Maps or the Almanac was on screen in the last 10 minutes, position
  reports are armed with **map + mesh**, or `netloc now` on the console;
- something changed: it has not looked yet on this network, the access points it heard last
  time are mostly gone, or the last fix ran out;
- at least 10 minutes since the last look (2 for `netloc now`), and at most 3 looks an hour (a look
  is up to two requests);
- no map download, uploader, book sync, call, game or Files job is running, and no music is
  playing (`netloc now` pauses the music, as the weather's Refresh does).

It needs a WiFi scan of this network from the last 2 minutes. When there is none, it asks the
WiFi auto-switcher for one, at most every 2 minutes. That scan obeys every rule the
auto-switcher's own scans obey (enough free memory, not during a call or a game), and waits while
an AI question, a weather fetch or a lookup is connecting (a scan and a secure connection at the
same moment can run the phone out of memory). It is only a scan: it never hops to another network,
never disconnects, and never reloads the saved-network list (with auto-switch on, the
auto-switcher's own 10-minute scan still does all of that on time). While a phone's GPS stream is
live on the hotspot, auto-switch does not hop away from that hotspot either. A lookup never starts
while a scan is running.

**The agreement rule.** beaconDB's answer is an average of whichever access points it knows, and
its accuracy figure says how far each one is usually heard, not whether they agree. One access
point that has moved (a household that moved, a travel router) can drag the answer kilometres away
while it still reads "±50 m". So:
1. At least 4 separate devices must be heard at -85 dBm or stronger. Several addresses of one
   router (2.4 GHz, 5 GHz, guest) count as one device.
2. The devices are dealt into two halves by signal strength (1st, 3rd, 5th... and 2nd, 4th,
   6th...), and each half is asked on its own.
3. Both answers must be HTTP 200 with no fallback, each must claim 1 to 100 m, and they must lie
   within the larger of the two claims of each other.
4. The WiPhone uses the midpoint, at an accuracy of the largest of 50 m, either claim, and the
   distance between them. It never claims better than 50 m.

Two halves cannot catch access points that moved **together** (a household's router, extender
and travel router, all mapped at the old address). So two more checks:
- **The jump check.** If this phone had a GPS or phone fix in the last 6 hours, the Wi-Fi answer
  must be reachable from it at 300 m/s or less (beyond the answer's own accuracy and 50 m).
  Otherwise it is refused: `far from your last fix: not used`.
- **A scan during the lookup.** A lookup can wait up to a minute behind a weather fetch. If a WiFi
  scan finishes meanwhile and no longer hears 2/3 of the devices asked about, the answer is
  refused (`moved during the lookup`) and it looks again later.

**How long it lasts.** A Wi-Fi location is where some access points are, so it stays true only
while the WiPhone still hears them. Every later scan is checked: if at least 2/3 of those devices
are still heard, the fix is kept; if not, it is dropped **at once**, before any new look. While
Maps or the Almanac is open, a fix is re-checked by a scan every 2 minutes, so a fix carried off
on a phone hotspot that stays connected is dropped within about 2 minutes. When the WiFi drops or
the WiPhone joins another network, the fix comes off the map at once and comes back only if the
next scan still hears its access points. Shown for 12 minutes after the last scan that confirmed
it, then gone. There is no grey "last Wi-Fi" marker.

Everything measured from a Wi-Fi fix carries its size: distances read "from Wi-Fi +/-60 m", and the
map's height line reads "35m below Wi-Fi +/-60 m" rather than "below you".

**Why it often says "not enough known Wi-Fi here".** beaconDB knows only access points that
volunteers have passed with its app. A home, a cabin or a campground has often never been seen,
and the halves must both be known and agree. That is by design: it says nothing far more often
than it says something wrong. The time zone never uses a Wi-Fi location either.

The line under **Wi-Fi location**:

| It says | What it means |
|---|---|
| `if on: sends Wi-Fi IDs to beaconDB` | Off. |
| `+/-60 m, 3 min ago, map only` | Working: the size, and when a scan last confirmed it. |
| `used while Maps or Almanac is open` | On, waiting until a position is wanted. |
| `waiting for a Wi-Fi scan` | It asked the auto-switcher for a scan. |
| `too few Wi-Fi networks (2 of 4)` | Not enough separate devices at -85 dBm or stronger. Nothing was sent. |
| `not enough known Wi-Fi here` | beaconDB did not know enough of them: a half got HTTP 404, or an answer that was not a Wi-Fi fix. |
| `too vague (+/-350 m): not used` | A half claimed more than 100 m. |
| `lookups disagree: not used` | The two halves were further apart than they claimed. One access point has probably moved. |
| `far from your last fix: not used` | The halves agreed, but on a place this phone could not have reached since its last GPS or phone fix. The access points have probably moved together. |
| `moved during the lookup` | A scan that finished while the lookup was out no longer heard the access points. It looks again later. |
| `WiFi changed: waiting for a scan` | The WiFi dropped or changed. The fix is off the map until a scan shows the access points are still there. |
| `mesh starts when you leave here` | map + mesh is chosen. Nothing goes to the mesh until you leave this screen. |
| `no fix now (moved or expired)` | The access points behind the fix are gone, or it ran out. It looks again when allowed. |
| `beaconDB unreachable: will retry` / `low memory: will retry` | Tries again after 10 minutes. |
| `not needed: GPS/phone fix in hand` / `waits while music plays` / `not on WiFi` | Self-explanatory. |

`netloc` on the serial console prints the setting, the last scan (how many access points, how
many usable, how many devices), the last look's verdict and accuracy, the fix's state and
whether it may be sent, and each half's HTTP code and handshake time. It never prints an address
or a coordinate. `netloc now` asks for a look; `netloc answer <A> <B>` feeds two canned answers
through the same agreement rule for the bench (each `{...}` as beaconDB's JSON, a 3-digit HTTP
status, or `-` for "not asked"). The result is a bench fix: shown, never sent by the schedule.

## Troubleshooting

The line under **Meshtastic → My node → Phone location**:

| It says | What it means / what to do |
|---|---|
| `this network: fix, 9 sats` | Working. |
| `no app found on this network` | Nothing answered on 10110 / 11123. Is the app running, on the right port, and is the WiPhone on *this phone's* hotspot? NH Companion: is **Share location on the hotspot** on, and does it say **Ready** (not *Port 10110 is in use*)? gpsdRelay: is the server's **switch** on, and was the service restarted (X, then play) after adding it? It asks again every 30 s on a phone hotspot. |
| `looking for a location app` | Asking now. |
| `connected, waiting for data` | The app accepted the connection but has sent nothing yet (gpsdRelay: is the service started?). NH Companion sends within 4 s, so seeing this for long means another app is on the port. |
| `old data dropped, waiting` | gpsdRelay replays old lines when the WiPhone connects. They are skipped on purpose. Wait a few seconds. |
| `fix found, checking it` | A fix needs a second, agreeing fix before it is used. One more second. |
| `no fix (indoors?)` | The phone has no GPS fix. Go outside or near a window. NH Companion turns the GPS on only when the WiPhone connects, so a first fix can take up to a minute. |
| `weak fix: not sent` | Fewer than 4 satellites or HDOP above 10. Shown, not reported. |
| `iPhone app gives no accuracy: shown, not sent` | GPS 2 IP. See [3d](#3d-iphone-gps-2-ip). |
| `fix, accuracy unknown: not sent` | The app sends no satellite count or HDOP. |
| `estimate only, not a GPS fix` | The app marked the position as an estimate (dead reckoning). Not used. |
| `fixes jump about: not used` | Two fixes in a row disagree. Often gpsdRelay with **NMEA generation** on: untick it, tick **NMEA relaying**. |
| `the app sends no GGA` | gpsdRelay: tick **NMEA relaying**. GPS 2 IP: turn on **GGA**. |
| `unreadable data from the app` | Not NMEA on that port, or the app sends something else (GPS 2 IP: **Protocol Type: NMEA**). |
| `not on WiFi` / `off` | Join the hotspot / tap the row to turn Phone location on. |

The serial console's `phonepos` command prints the same state with counters: bytes,
sentences, bad checksums, stale lines dropped, reconnects. It never prints a coordinate or an
address.

## For developers: testing without a phone

**The Android app** has its own notes in [`android/README.md`](../android/README.md): unit tests,
the emulator recipe, and `tools/phone_stream_check.cpp`. That tool reads any app's stream
through the WiPhone's own gate (`self_pos.cpp`, unchanged) on the Mac, and says what the
WiPhone would keep.

**The WiPhone side:**

`tools/nmea_phone_stub.py` is a stand-in for the phone apps (standard library only):

```bash
python3 tools/nmea_phone_stub.py --port 10111 --bind <the Mac's address>
```

Then on the WiPhone's serial console: `phonepos at <the Mac's address>:10111`. This is RAM
only, and its fixes are bench fixes that are never broadcast. Flags reproduce each app's
quirks:
- `--burst 30`: gpsdRelay's replayed backlog;
- `--generated --acc 8`: gpsdRelay's generated sentences;
- `--ios`: no sats/HDOP;
- `--sats 8 --hdop 0.9` on port 11123: GPS 2 IP's stand-in numbers;
- `--sats 3`, `--hdop 12`, `--status V --quality 0`, `--stall 15`, `--no-crlf`.

The rules themselves live in `WiPhone/self_pos.{h,cpp}` (host-tested by
`tests/test_selfpos.cpp`). The socket is in `WiPhone/phone_pos.cpp`.

The X4 Pro e-reader fork ([crosspoint-reader](https://github.com/Nikguy321/crosspoint-reader))
reads the same apps the same way for its **Locate Me**.
