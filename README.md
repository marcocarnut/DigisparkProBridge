# DigisparkProBridge

A USB-to-UART bridge for the Digispark Pro (ATtiny167): plug it into a Linux
computer and it appears as a serial port (`/dev/ttyACM0`) connected to the
board's hardware UART. The UART follows the bit rate you set on the port, and
from 9600 up to 57600 bps -- the fastest standard rate -- it was tested to
lose nothing, in both directions at once. 76800 bps works too, and is the
real ceiling, but it is a nonstandard rate that most programs cannot ask for.

It uses the ATtiny167's LIN/UART peripheral in UART mode for the serial side
and [DigiCDCFast](https://github.com/marcocarnut/DigiCDCFast) for the USB
side.

## Features

- **Bit rate from the host:** `stty -F /dev/ttyACM0 38400`, or the rate in
  any terminal program, sets the UART's rate. The LIN/UART can use 8 to 63
  samples per bit, and the bridge picks the combination with the smallest
  error: 0.44% at 57600 bps, where the core's `Serial` (always 16 samples per
  bit) is 3.5% off.
- **Lossless up to 57600 bps** in both directions at once, at the full
  8000 bytes/s low-speed USB allows (details below), and up to 76800 bps if
  you can get your programs to ask for a nonstandard rate: that is the last
  one whose *line rate* fits in the 8000 bytes/s. Faster rates can still be
  set and used -- two terminals at 115200 bps talk to each other perfectly
  well -- as long as nothing sends more than 8000 bytes/s for long.
- **Hardware flow control both ways, if you want it:** an RTS output on PA2,
  so a device that obeys CTS is stopped before the bridge runs out of room,
  and a CTS input on PA3, so a device that needs a moment can stop the bridge.
  Both are off until switched on in the sketch, each on its own. With RTS,
  115200 bps carries a file losslessly one direction at a time, at the
  8000 bytes/s USB allows.
- **Reflashing without replugging:** setting the port to 134 bps
  (`stty -F /dev/ttyACM0 134`) jumps to the micronucleus bootloader.
- 8N1 only; flow control is the RTS output on PA2 and the CTS input on PA3,
  both off until switched on.

## Wiring

| Digispark Pro | Other device |
|---------------|--------------|
| PA0 (LIN/UART RX) | TX |
| PA1 (LIN/UART TX) | RX |
| PA2 (RTS out) | CTS (optional, and `RTS_OUTPUT` must be set) |
| PA3 (CTS in) | RTS (optional, and `CTS_INPUT` must be set) |
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
| 57600 | 100000 | FT232R | intact | intact (3 runs) |
| 57600 | 200000 | FT232R | intact | intact |
| 76800 | 100000 | FT232R | intact | intact |

Nothing is lost at any of these rates in either direction, with the full
8-byte USB packets; see [Collecting the byte in
time](#collecting-the-byte-in-time) for how. Earlier versions lost 0.33% of
the bytes travelling to the host at 57600 bps when both directions ran at
once, and could not do 76800 bps at all.

Sending runs a little under the line rate, in both directions at once or
alone:

| Bit rate | Line rate | The bridge sends at |
|----------|-----------|---------------------|
| 9600 | 960 | 867 bytes/s |
| 19200 | 1921 | 1714 bytes/s |
| 38400 | 3841 | 3332 bytes/s |
| 57600 | 5757 | 4998 bytes/s |
| 76800 | 7680 | 6494 bytes/s |

Nothing is lost by it, it only takes longer, and it is not the bridge running
out of data to send: the counters say the transmitter found its buffer empty
once per transfer, at the end. Each byte takes about one bit time more than
its ten, which is the LIN/UART's own spacing between bytes, plus some 10 µs
of waiting to be handed the next one.

PPP between two hosts over the bridge (`pppd` at both ends, MTU/MRU 296,
`novj`), with a 410 kB file going each way over TCP at the same time: at
57600 bps, 858 kB and 899 kB carried, **no frame errors at either end**, and
two ping floods thrown in at the end for good measure. Ping flooding drops
packets, as any link does when its queues fill, but without errors. At
38400 bps the same test is likewise clean. Before the bridge collected bytes
from inside the USB interrupt, 57600 bps lost 83 frames per 103 kB on the way
to the host, which TCP retransmitted.

### Collecting the byte in time

The LIN/UART holds one received byte besides the one arriving, so a byte must
be collected within one byte time: 174 µs at 57600 bps. V-USB handles a USB
transaction with interrupts off, and when the host runs transactions back to
back it handles the whole run without ever letting go, which reaches 200 µs.
No handler of ours can get in: the processor serves the pending USB interrupt
before any interrupt with a higher vector number, so returning between
transactions would not help either (it was tried, and the latency
distribution did not change at all).

So the byte is collected from inside the USB interrupt. DigiCDCFast calls
`usbTransactionEnd()` at the end of every transaction, and the bridge's is
twenty instructions of assembler that read `LINDAT` into the receive ring,
using only the registers the driver has already saved. The ring's head and
tail live in two general-purpose I/O registers, which one instruction
reaches. The handler still collects most bytes -- the hook only catches the
ones that would not have waited.

The same hook hands the transmitter its next byte, for the same reason in
reverse: it holds one byte, and nobody refills it while the USB interrupt has
the processor. That only shows above 38400 bps, where a byte takes less time
than a run of transactions: it is worth 11% at 38400 bps and 8% at 76800, and
nothing at all at 57600.

It is called on every transaction and not only when the next packet is
already arriving. Calling it only in that case seems the thriftier choice and
is far worse than no hook at all: the bridge's own handler then stays pending
through the whole run and fires the moment the driver returns, which is
exactly when the next packet arrives, and an ordinary handler's prologue is
long enough to make the driver miss it.

### Flow control

The bridge has 64 bytes to hold what it receives until USB takes it, and USB
takes at most 8000 bytes/s. Anything sent faster for long fills that, and the
bytes arriving next are lost: flooding it at 115200 bps with its USB port
closed lost 118649 of 118784 bytes.

PA2 prevents that, with `RTS_OUTPUT` set to 1 and the other device's CTS
wired to it. The bridge holds it low while it can take data and raises it once 44 bytes are waiting, until the
buffer is down to 16 again. In the same test, wired to an FT232R's CTS with
`crtscts` set on that side, **nothing was lost**: the adapter paused after its
own 4096 bytes were gone, and the bridge's `r` counter stayed 0.

That makes rates above 76800 bps usable, within the 8000 bytes/s:

| At 115200 bps, 60 kB | Result |
|----------------------|--------|
| adapter to bridge | intact, 7935 bytes/s |
| bridge to adapter | intact, 7937 bytes/s |
| both at once | 932 bytes lost and 100 corrupted on the way to the host |

One direction at a time runs at the USB ceiling itself. Both at once is past
what the board can keep up with -- and the losses are not the UART's, whose
overrun and ring counters stay 0, but USB transactions going wrong while the
bus is saturated in both directions and the UART interrupts 11520 times a
second. 76800 bps is the fastest rate that works in both directions at once.

Not every adapter obeys CTS: a CH340 ignored the equivalent line on the
ATtiny85 bridge, because Linux's `ch341` driver accepts `crtscts` without
implementing it. PA2 is left alone until `RTS_OUTPUT` is set to 1 at the top
of the sketch (26 bytes of flash).

### The other direction: CTS

A device that needs the bridge to pause can say so on PA3, once `CTS_INPUT`
is set to 1 at the top of the sketch (22 bytes of flash). While it is high
the bridge finishes the byte it is sending and stops; `loop()` starts the
transmitter again when it goes low. Driving an FT232R's RTS by hand, the
bridge sent **0 bytes** while told to wait and then all 20000 of them, in
order and intact, once let go (`cts_test.py`).

Nothing needs to be told to the host, and there would be no way to tell it:
CDC's `SERIAL_STATE` notification carries carrier, ring, break, framing,
parity and overrun, and has no CTS bit at all. It is not needed, because USB
provides the same back-pressure by itself: the bridge stops feeding its
transmitter, its buffer fills, it stops taking bytes from USB, and
DigiCDCFast makes the host's transactions wait. In that same test the host
got 2560 bytes into the bridge and was then simply blocked until the bridge
was let go.

Both halves are off by default and switch on independently, since either may
be wired without the other. It matters most for CTS: the pin is pulled up, so
an enabled CTS input with nothing wired to it reads "wait", and the bridge
would never send a byte. Turn each on only along with its wire.

### USB packet size

`USB_PACKET_SIZE`, at the top of `ProBridge.ino` (or `-DUSB_PACKET_SIZE=6`),
sets how many bytes a USB packet carries, 1 to 8. Each transaction keeps
V-USB's interrupts off for roughly 6 µs per byte, so smaller packets
interrupt the sketch for less time, but they also carry less: one packet per
millisecond each way, so 8 bytes gives 8000 bytes/s and 6 gives 6000, against
the 5760 that 57600 bps needs. Leave it at 8 unless something else on the
board needs the processor sooner: nothing is lost at 8 at any rate the bridge
supports.

## Limits

- **Sending runs at about 87% of the line rate** (see the table under
  [Results](#results)). Nothing is lost by it; each byte just takes about one
  bit time longer than its ten.
- **8000 bytes/s** is the most low-speed USB carries per direction, and that,
  rather than any bit rate, is the real ceiling. 76800 bps is the fastest rate
  whose line rate fits under it, and it was tested to lose nothing both ways.
  Above that the bit rate is still yours to set and use -- 115200 bps carries
  a file losslessly one direction at a time -- but only if the sender can be
  stopped, which is what the RTS line on PA2 is for (see
  [Flow control](#flow-control)). A sender that ignores it overruns the
  bridge's buffer.
- **76800 bps needs a nonstandard rate.** Linux has no `B76800` constant, so
  `stty` and Python's `termios` refuse it; `bridge_test.py` sets it through
  `TCSETS2`, and a program that wants it has to do the same. A received byte
  must also be collected within 130 µs there, less than a run of USB
  transactions takes -- which is what the hook is for.
- Interrupt handlers on the board must never keep interrupts off for long,
  or V-USB misses USB transactions: the bridge's own LIN/UART handler masks
  itself and re-enables interrupts, for the same reason as the core fix. A
  missed packet is not the worst of it -- a packet caught slightly late is
  received corrupted, and silently, since at 16 MHz V-USB has no time to
  check a CRC either.

## Ideas not tried

- **Service the UART from `loop()` and stop interrupting altogether.** What
  still fails is 115200 bps in both directions at once, and the counters say
  it is not the UART: overruns and ring drops stay 0, so the losses are USB
  transactions going wrong. The suspect is the receive handler's prologue,
  which runs 11520 times a second at that rate and needs only to start just
  before a packet does to make the driver miss it -- the same mechanism that
  made calling `usbTransactionEnd()` conditionally worse than not calling it
  at all. With no handler there would be no prologue to collide with.
  It would not replace the hook: while the USB interrupt has the processor
  `loop()` does not run either, so the hook stays the only thing that can
  reach the UART during a transaction. And it would need every path through
  `loop()` to be shorter than a byte time, 87 µs at 115200 bps, which
  `usbPoll()` and the loop that hands 64 bytes to USB would have to be
  measured against.
- **Reporting the flow control lines to the host.** There is nowhere to put
  CTS: CDC's `SERIAL_STATE` notification carries carrier, ring, break,
  framing, parity and overrun, and no CTS bit. The bridge acts on the pin
  itself instead, which is what the host would have done with it anyway.

## Diagnostics

Setting the port to 110 bps prints a line of counters and clears them, so a
test can ask the bridge what it thinks happened:

```
$ stty -F /dev/ttyACM0 110 && timeout 2 cat /dev/ttyACM0
S n0079c o0000 f0000 r0000
```

| | |
|---|---|
| `n` | bytes the handler took from the UART (the hook's are not counted, so that it costs no cycles) |
| `o` | bytes the UART lost before anyone came for them: receive overruns |
| `f` | framing errors |
| `r` | bytes dropped because the receive ring was full |
| `s` | times the transmitter stopped because there was nothing left to send |

All are 16-bit and wrap silently. `bridge_test.py --stats` prints them after
each transfer. Set `STATS` to 0 at the top of `ProBridge.ino` to leave the
counters out.

## Tests

- `bridge_test.py [--bridge PORT] [--adapter PORT] [--bytes N] [--stats] BAUD...`
  sends pseudo-random data through the bridge against a reference
  USB-serial adapter wired to the UART (TX to RX, RX to TX, GND to GND): each
  direction, then both at once. It reports throughput and bytes lost, extra
  or corrupted, with the offsets of the first mismatches, and with `--stats`
  the bridge's own counters as well. Defaults: `/dev/ttyACM0` for the bridge,
  `/dev/ttyUSB0` for the adapter, 100000 bytes.
- `cts_test.py [--baud N] [--bytes N]` drives the adapter's RTS by hand --
  rather than leaving it to the kernel's flow control, so that it tests the
  bridge and not the two drivers -- and checks that the bridge stops while
  told to wait and loses nothing when let go.
- `rts_test.py [--baud N] [--seconds N] [--no-crtscts]` floods the bridge's
  UART while its USB port stays closed, so nothing drains what it receives,
  and reports whether the adapter was stopped: it reads the bridge's RTS line
  back through the adapter's CTS input rather than taking it on trust, and
  then asks the bridge how many bytes it dropped.
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
