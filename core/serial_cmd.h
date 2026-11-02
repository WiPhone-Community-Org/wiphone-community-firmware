/*
 * serial_cmd.h — a tiny command console on the USB serial port.
 *
 * Asked for while bringing up SMS mirroring, and the reason is worth recording because it
 * will come up again: several things on this phone can only be reached by TAPPING THE GLASS
 * — the WiFi uploader is the obvious one — and that makes them unavailable to anyone working
 * on the phone remotely, or holding a cable and a terminal instead of the phone. Bringing
 * the uploader up to install a config file, then taking it down again so the app that reads
 * that file is allowed to run, is exactly that shape.
 *
 * ⚠ THE USB PORT IS FREE FOR THIS. `userSerial` is UART2 on its own pins (WiPhone.ino), so
 * the USB console has only ever been an output. Nothing else reads it.
 *
 * Deliberately small: a 48-byte line buffer, one comparison chain, no parser and no
 * framework. It is a debug aid, and it must not become another thing that owns memory the
 * SIP stack and the emulator are already fighting over.
 *
 * ⚠ NO AUTHENTICATION, BY DESIGN — physical possession of the USB port IS the credential.
 * Do not add a command here that a stranger with a cable should not be able to run, and in
 * particular do not add one that PRINTS A SECRET. The mirror token is deliberately absent
 * from `mirror` for that reason.
 *
 *   Commands (newline-terminated, case-insensitive):
 *     ?          this help
 *     up on      start the WiFi uploader (files land in /roms)
 *     up off     stop it
 *     up         where to point a browser
 *     sync       poll COVEY for mirrored texts right now, ignoring the interval
 *     mirror     the mirror poller's state
 */
#ifndef SERIAL_CMD_H
#define SERIAL_CMD_H

/* Consume whatever has arrived on USB serial and run any complete line.
 * Call once per main-loop pass; returns immediately when nothing is waiting. */
void serialCmdLoop();

#endif // SERIAL_CMD_H
