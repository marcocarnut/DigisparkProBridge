# DigisparkProBridge

A USB-to-UART bridge for the Digispark Pro (ATtiny167): plug it into a Linux
computer and it appears as a serial port (`/dev/ttyACM0`) connected to the
board's hardware UART. The UART follows the bit rate you set on the port;
it was tested from 19200 to 57600 bps.

It uses the ATtiny167's LIN/UART peripheral in UART mode for the serial side
and [DigiCDCFast](https://github.com/marcocarnut/DigiCDCFast) for the USB
side.

## Features

- **Bit rate from the host:** `stty -F /dev/ttyACM0 38400`, or the rate in
  any terminal program, sets the UART's rate. The LIN/UART can use 8 to 63
  samples per bit, and the bridge picks the combination with the smallest
  error: 0.44% at 57600 bps, where the core's `Serial` (always 16 samples per
  bit) is 3.5% off.
- **Lossless up to 38400 bps** in both directions at once, and at 57600 bps
  one way at a time (details below).
- **Reflashing without replugging:** setting the port to 134 bps
  (`stty -F /dev/ttyACM0 134`) jumps to the micronucleus bootloader.
- 8N1 only; no hardware flow control lines.

## Wiring

| Digispark Pro | Other device |
|---------------|--------------|
| PA0 (LIN/UART RX) | TX |
| PA1 (LIN/UART TX) | RX |
| GND | GND |

The Pro's I/O is at 5 V. Use a level shifter for 3.3 V devices, and an
RS-232 transceiver (such as a MAX232) for RS-232 ports.

## Requirements

- **Linux host.** Windows refuses low-speed USB serial devices; see
  [DigiCDCFast's README](https://github.com/marcocarnut/DigiCDCFast#windows-does-not-work).
- **Digistump AVR core 1.7.5**, from the board manager URL
  `https://raw.githubusercontent.com/ArminJo/DigistumpArduino/master/package_digistump_index.json`.
- **A one-line fix to that core.** Its `millis()` timer interrupt on the Pro
  is blocking, `SIGNAL(TIMER0_OVF_vect)` in `cores/pro/wiring.c`. Timer0
  overflows at 976.56 Hz, beating against the host's 1000 Hz USB frames, so
  every 42.7 ms the handler delays V-USB just as the host polls, and data sent
  to the host is lost. Change the line to:

  ```c
  ISR(TIMER0_OVF_vect, ISR_NOBLOCK)
  ```

  The file is in `~/.arduino15/packages/digistump/hardware/avr/1.7.5/cores/pro/`
  on Linux. (The ATtiny85 core already does this.)
- **DigiCDCFast** 1.0.0 or later, from the Arduino Library Manager.

## Building and flashing

With the Arduino IDE: open `ProBridge/ProBridge.ino`, select the board
*Digispark Pro (16 MHz)* and upload; plug the board in when asked.

With arduino-cli:

```sh
arduino-cli compile --fqbn digistump:avr:digispark-pro --output-dir build ProBridge
stty -F /dev/ttyACM0 134        # only if the bridge is already running
~/.arduino15/packages/digistump/tools/micronucleus/2.6/micronucleus --run build/ProBridge.ino.hex
```

## Results

Against two USB-serial adapters wired to the UART, a Linux PC with an xHCI
host controller, each direction measured separately and both at once
(`bridge_test.py`):

| Bit rate | Bytes per test | Adapter | One direction | Both directions at once |
|----------|----------------|---------|---------------|-------------------------|
| 19200 | 50000 | FT232R | intact | intact |
| 19200 | 50000 | CH340 | intact | intact |
| 38400 | 100000 | FT232R | intact | intact |
| 38400 | 100000 | CH340 | intact | intact |
| 57600 | 100000 | FT232R | intact | to the adapter intact; to the host 0.33% lost (3 runs) |
| 57600 | 100000 | CH340 | intact | intact (2 runs) |

PPP between two hosts over the bridge (`pppd` at both ends, MTU/MRU 296,
`novj`), with a 62 kB file going each way over TCP at the same time: at
38400 bps, no frame errors at either end, ~86 kB carried each way at the
line rate. Ping flooding both ways drops packets, as any link does when its
queues fill, without errors.

### USB packet size

The losses at 57600 bps in both directions are UART receive overruns (see
[Limits](#limits)), and they shrink with the USB packet size: each USB
transaction keeps V-USB's interrupts off for roughly 6 us per byte it
carries, and a byte must be collected within one byte time, 174 us at
57600 bps. Smaller packets also carry less, one per millisecond each way,
so the bit rate sets a floor: 57600 bps needs 5760 bytes/s.

| `USB_PACKET_SIZE` | Bytes/s available | Lost to the host, 100 kB both ways at 57600 |
|-------------------|-------------------|---------------------------------------------|
| 8 (default) | 8000 | 0.33% |
| 7 | 7000 | 0.16% |
| 6 | 6000 | 0.11% |
| 5 | 5000 | too slow for 57600 bps |

Set it at the top of `ProBridge.ino` (or with `-DUSB_PACKET_SIZE=6`). It
makes no difference at 38400 bps and below, where nothing is lost either
way, and 6 bytes still carries 38400 bps at the line rate.

## Limits

- **57600 bps in both directions** depends on the other end. The losses with
  the FT232R are UART receive overruns: the LIN/UART holds one received byte
  besides the one being shifted in, so each byte must be read within one byte
  time (174 µs at 57600 bps). V-USB handles every USB transaction with
  interrupts off, and with traffic in both directions it kept other
  interrupts waiting for up to 198 µs. Why the CH340 doesn't trigger overruns
  hasn't been examined; perhaps it leaves short gaps between bytes. At
  38400 bps a byte takes 260 µs, and nothing was lost with either adapter.
- **8000 bytes/s** is the most low-speed USB carries per direction, so UART
  input faster than that (above about 76800 bps, sent continuously) overflows
  the bridge's buffer.
- Interrupt handlers on the board must never keep interrupts off for long,
  or V-USB misses USB transactions: the bridge's own LIN/UART handler masks
  itself and re-enables interrupts, for the same reason as the core fix.

## Tests

- `bridge_test.py [--bridge PORT] [--adapter PORT] [--bytes N] BAUD...`
  sends pseudo-random data through the bridge against a reference
  USB-serial adapter wired to the UART (TX to RX, RX to TX, GND to GND): each
  direction, then both at once. It reports throughput and bytes lost, extra
  or corrupted, with the offsets of the first mismatches. Defaults:
  `/dev/ttyACM0` for the bridge, `/dev/ttyUSB0` for the adapter,
  100000 bytes.
- `LinDuplex/` with `lin_duplex_test.py [BAUD] [SECONDS]` checks the
  LIN/UART on its own, without USB carrying the data: the board transmits a
  counting sequence while checking one it receives. It showed the LIN/UART
  itself is fine in full duplex (57600 bps both ways, no errors).

## License and credits

GPL version 2 or version 3, at your choice; see [LICENSE](LICENSE).

- [DigiCDCFast](https://github.com/marcocarnut/DigiCDCFast), and through it
  [V-USB](https://www.obdev.at/vusb/) by OBJECTIVE DEVELOPMENT Software GmbH.
- The Digistump AVR core, by Digistump LLC, as maintained by ArminJo.
- DigisparkProBridge by Marco Carnut.

See also [DigisparkBridge](https://github.com/marcocarnut/DigisparkBridge),
the experimental bridge for the original Digispark (ATtiny85).
