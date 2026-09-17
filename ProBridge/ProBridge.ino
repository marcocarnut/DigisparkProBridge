/*
  ProBridge -- USB <-> UART bridge for the Digispark Pro (ATtiny167)

  USB side: DigiCDCFast. UART side: the ATtiny167's LIN/UART in UART mode,
  RX on PA0, TX on PA1, 8N1. The UART bit rate follows the rate the host sets
  for the USB port (stty, terminal programs). The LIN/UART can use 8 to 63
  samples per bit; the combination with the smallest error is chosen, e.g.
  0.44% at 57600 bps, where the core's Serial (always 16 samples) is 3.5% off.

  Setting the port to 134 bps (stty -F /dev/ttyACM0 134) jumps to the
  micronucleus bootloader, for reflashing without replugging.

  PA2 can be an RTS output, low while the bridge can take data, and PA3 a CTS
  input, which stops the bridge while it is high. Both are off until you set
  RTS_OUTPUT or CTS_INPUT below.

  Rates from 9600 to 76800 bps were tested to lose nothing in both directions
  at once. 76800 bps is the last that fits: it needs 7680 of the 8000 bytes/s
  low-speed USB carries. Faster rates work one direction at a time if the
  other device obeys the RTS output on PA2, which goes high when the bridge
  is running out of room; without it, anything sustained above 8000 bytes/s
  overflows the receive buffer.
*/

#include <DigiCDCFast.h>

// USB packet size, 1 to 8 bytes. Smaller packets keep V-USB's interrupts off
// for less time, which the LIN/UART's single spare byte needs at high bit
// rates, but carry less: one packet per millisecond each way, so 8 bytes
// gives 8000 bytes/s, 6 gives 6000 (57600 bps needs 5760). Measured with an
// FT232R, 100 kB in both directions at once, nothing is lost at any rate up
// to 76800 bps with 8-byte packets, which usbTransactionEnd() below is what
// makes possible; smaller packets are for sketches whose own timing cannot
// wait ~110 us for the processor.
#ifndef USB_PACKET_SIZE
#define USB_PACKET_SIZE 8
#endif
#if USB_PACKET_SIZE != 8
#include <DigiCDCDescriptor.h>
const uchar digiCdcConfigDescriptor[DIGICDC_DESCRIPTOR_SIZE] PROGMEM =
    DIGICDC_CONFIG_DESCRIPTOR(USB_PACKET_SIZE, USB_PACKET_SIZE);
#endif

// Hardware flow control, off unless you wire it, and each half on its own:
#ifndef RTS_OUTPUT
#define RTS_OUTPUT      0    // 1: PA2 is an RTS output, low while the bridge can
                             //    take data, high once its buffer is filling up
#endif
#ifndef CTS_INPUT
#define CTS_INPUT       0    // 1: PA3 is a CTS input, and the bridge only sends
                             //    while it is low. The pin is pulled up, so with
                             //    nothing wired to it the bridge would never send.
#endif
#ifndef STATS
#define STATS           1    // 1: 110 bps prints and clears diagnostic counters
#endif
#define BOOTLOADER_BAUD 134
#define STATS_BAUD      110
#define RX_SIZE         64  // powers of 2
#define TX_SIZE         64
#define RTS_HIGH  (RX_SIZE - 20)  // bytes waiting at which the far end is asked to stop
#define RTS_LOW   16              // ... and at which it may resume

#if CTS_INPUT
#define maySend() (!(PINA & _BV(PA3)))
#else
#define maySend() true  // the compiler then drops every test of it
#endif

// The receive ring is shared with usbTransactionEnd() below, which is written
// in assembler, so its head and tail live in two of the general-purpose I/O
// registers, which one instruction can read and another write.
static uint8_t rxBuf[RX_SIZE];
#define rxHead GPIOR0  // advanced by the interrupt handler and by the hook
#define rxTail GPIOR1  // advanced by loop()
static uint8_t txBuf[TX_SIZE];
static volatile uint8_t txTail;  // advanced by the interrupt handler
static uint8_t txHead;           // advanced by loop()
// LINENIR as it should be; the register itself is 0 while the handler runs
static uint8_t linEnable;

#if STATS
// Diagnostic counters, printed at STATS_BAUD. All 16-bit, and they wrap
// silently. The hook counts nothing: every cycle there costs USB packets.
static struct {
  uint16_t received;  // n: bytes taken from the UART by the handler
  uint16_t overruns;  // o: bytes the UART lost before anyone came for them
  uint16_t framing;   // f: framing errors
  uint16_t dropped;   // r: bytes dropped because the ring was full
  uint16_t starved;   // s: times the transmitter stopped, with nothing to send
} stats;
#define COUNT(counter) (stats.counter++)
#else
#define COUNT(counter) ((void)0)
#endif

static void receive()  // interrupts must be off, or this must be the handler
{
  uint8_t c = LINDAT;  // reading clears LRXOK
  uint8_t next = (rxHead + 1) & (RX_SIZE - 1);
  if (next != rxTail) {
    rxBuf[rxHead] = c;
    rxHead = next;
    COUNT(received);
  } else {
    COUNT(dropped);
  }
}

// The LIN/UART holds one received byte besides the one arriving, so a byte
// must be collected within a byte time: 174 us at 57600 bps. V-USB handles a
// transaction with interrupts off, and a run of them without ever letting go,
// which reaches 200 us -- and no ordinary handler can get in, since the
// processor serves the pending USB interrupt first. So DigiCDCFast calls this
// at the end of every transaction, and here we take the byte. It is also what
// keeps the handler below from being pending when the driver returns, which
// is when the next packet comes and no time to enter a handler first.
//
// The transmitter is handed its next byte here too, for the same reason in
// reverse: it holds one byte, and while the USB interrupt has the processor
// nobody refills it, so the line goes idle between bytes. That costs no data,
// only speed, but at 57600 bps it was a sixth of the line rate.
//
// Assembler, and only the registers V-USB has saved: r0, r16 to r22, Y and
// the flags. Short, too: a packet may be arriving as it runs.
#if !USB_CFG_TRANSACTION_END_HOOK
#error "the bridge needs USB_CFG_TRANSACTION_END_HOOK in DigiCDCFast's usbconfig.h"
#endif
extern "C" void usbTransactionEnd() __attribute__((naked, used));
void usbTransactionEnd()
{
  asm volatile(
      "        lds  r19, %[linsir]         \n"  // both flags, in one read
      "        sbrs r19, %[lrxok]          \n"
      "        rjmp 1f                     \n"  // nothing received
      "        lds  r16, %[lindat]         \n"  // the byte; this clears LRXOK
      "        in   r17, %[head]           \n"
      "        mov  r28, r17               \n"
      "        ldi  r29, 0                 \n"
      "        subi r28, lo8(-(%[rxbuf]))  \n"  // Y = rxBuf + rxHead
      "        sbci r29, hi8(-(%[rxbuf]))  \n"
      "        st   Y, r16                 \n"
      "        inc  r17                    \n"
      "        andi r17, %[rxmask]         \n"
      "        in   r16, %[rxtail]         \n"
      "        cp   r17, r16               \n"
      "        breq 1f                     \n"  // no room: drop it, as receive() does
      "        out  %[head], r17           \n"
      "1:      sbrs r19, %[ltxok]          \n"  // transmitter wants the next byte?
      "        ret                         \n"
      "        lds  r16, %[enable]         \n"  // ... and is it running at all?
      "        sbrs r16, %[lentxok]        \n"
      "        ret                         \n"
      "        lds  r16, %[txtail]         \n"
      "        lds  r17, %[txhead]         \n"
      "        cp   r16, r17               \n"
      "        breq 2f                     \n"  // nothing to send: the handler stops it
      "        mov  r28, r16               \n"
      "        ldi  r29, 0                 \n"
      "        subi r28, lo8(-(%[txbuf]))  \n"  // Y = txBuf + txTail
      "        sbci r29, hi8(-(%[txbuf]))  \n"
      "        ld   r18, Y                 \n"
      "        sts  %[lindat], r18         \n"  // writing LINDAT clears LTXOK
      "        inc  r16                    \n"
      "        andi r16, %[txmask]         \n"
      "        sts  %[txtail], r16         \n"
      "2:      ret                         \n"
      :
      : [linsir] "i"(_SFR_MEM_ADDR(LINSIR)), [lindat] "i"(_SFR_MEM_ADDR(LINDAT)),
        [lrxok] "I"(LRXOK), [ltxok] "I"(LTXOK), [lentxok] "I"(LENTXOK),
        [head] "I"(_SFR_IO_ADDR(rxHead)), [rxtail] "I"(_SFR_IO_ADDR(rxTail)),
        [rxbuf] "i"(rxBuf), [rxmask] "M"(RX_SIZE - 1), [enable] "i"(&linEnable),
        [txbuf] "i"(txBuf), [txtail] "i"(&txTail), [txhead] "i"(&txHead),
        [txmask] "M"(TX_SIZE - 1));
}

static void transmit(uint8_t c)
{
  LINDAT = c;  // writing clears LTXOK
}

// Hand the transmitter its next byte, or stop it: when there is nothing to
// send, and when the other device has asked us to wait. Either way loop()
// starts it again. Interrupts must be off.
static void transmitNext()
{
  if (txTail == txHead || !maySend()) {
    linEnable &= ~_BV(LENTXOK);
  } else {
    transmit(txBuf[txTail]);
    txTail = (txTail + 1) & (TX_SIZE - 1);
  }
}

// V-USB must never wait for this handler (a delayed USB interrupt makes the
// host's transaction fail), so it masks its own interrupts and lets others in.
ISR(LIN_TC_vect)
{
  LINENIR = 0;
  sei();  // a USB interrupt waiting for us runs here
  // usbTransactionEnd() takes received bytes too, so test and read together
  cli();
  if (LINSIR & _BV(LRXOK))
    receive();
  sei();  // and again here
  // usbTransactionEnd() hands the transmitter bytes too, so test and write
  // together, and leave interrupts off through to the end of the handler
  cli();
  if ((linEnable & _BV(LENTXOK)) && (LINSIR & _BV(LTXOK))) {
    if (txTail == txHead)
      COUNT(starved);
    transmitNext();
  }
  LINENIR = linEnable;
}

static void uartBegin(unsigned long baud)
{
  // bit rate = F_CPU / (samples per bit * (LINBRR + 1))
  uint8_t bestLbt = 16;
  uint16_t bestBrr = 0;
  unsigned long bestError = ~0UL;
  for (uint8_t lbt = 8; lbt <= 63; lbt++) {
    unsigned long divisor = lbt * baud;
    unsigned long brr1 = (F_CPU + divisor / 2) / divisor;
    if (brr1 < 1 || brr1 > 4096)
      continue;
    unsigned long actual = F_CPU / (lbt * brr1);
    unsigned long error = actual > baud ? actual - baud : baud - actual;
    if (error <= bestError) {  // on ties, prefer more samples per bit
      bestError = error;
      bestLbt = lbt;
      bestBrr = brr1 - 1;
    }
  }

  cli();
  LINCR = _BV(LSWRES);
  LINBTR = _BV(LDISR) | bestLbt;
  LINBRR = bestBrr;
  LINCR = _BV(LENA) | _BV(LCMD2) | _BV(LCMD1) | _BV(LCMD0);  // UART, RX and TX, 8N1
  linEnable = _BV(LENRXOK);
  LINENIR = linEnable;
  rxHead = rxTail = txHead = txTail = 0;
  sei();
}

static void startTransmitter()  // interrupts must be off
{
  if (!(linEnable & _BV(LENTXOK)) && txTail != txHead && maySend()) {
    transmit(txBuf[txTail]);
    txTail = (txTail + 1) & (TX_SIZE - 1);
    linEnable |= _BV(LENTXOK);
    LINENIR = linEnable;
  }
}

static void uartWrite(uint8_t c)  // the caller checks there is room
{
  txBuf[txHead] = c;
  uint8_t sreg = SREG;
  cli();
  txHead = (txHead + 1) & (TX_SIZE - 1);
  startTransmitter();
  SREG = sreg;
}

#if STATS
static void writeHex(char tag, uint16_t v)
{
  SerialUSB.write(' ');
  SerialUSB.write(tag);
  for (int8_t shift = 12; shift >= 0; shift -= 4) {
    uint8_t d = (v >> shift) & 0x0F;
    SerialUSB.write(d < 10 ? '0' + d : 'a' + d - 10);
  }
}

static void printStats()
{
  SerialUSB.write('S');
  writeHex('n', stats.received);
  writeHex('o', stats.overruns);
  writeHex('f', stats.framing);
  writeHex('r', stats.dropped);
  writeHex('s', stats.starved);
  SerialUSB.write('\r');
  SerialUSB.write('\n');
  memset(&stats, 0, sizeof stats);
}

// The UART's own account of what it lost. Polled rather than handled: an
// error costs a byte either way, and the counters only have to say so.
static void collectErrors()
{
  if (LINSIR & _BV(LERR)) {
    uint8_t e = LINERR;
    if (e & _BV(LOVERR))
      COUNT(overruns);
    if (e & _BV(LFERR))
      COUNT(framing);
    LINSIR = _BV(LERR);  // clears LINERR with it
  }
}
#endif

static void enterBootloader()
{
  SerialUSB.delay(100);  // let the host's SET_LINE_CODING request complete
  cli();
  LINCR = _BV(LSWRES);   // no sketch interrupts may fire once the bootloader runs
  linEnable = 0;
  LINENIR = 0;
  TIMSK0 = 0;
  TIMSK1 = 0;
  TCCR0B = 0;
  TCCR1B = 0;
  ((void (*)())0)();     // micronucleus points the reset vector at itself
}

void setup()
{
#if RTS_OUTPUT
  PORTA &= ~_BV(PA2);  // low: the far end may send
  DDRA |= _BV(PA2);
#endif
#if CTS_INPUT
  PORTA |= _BV(PA3);   // input with its pull-up: unconnected reads "wait"
#endif
  SerialUSB.begin();
}

void loop()
{
  static unsigned long currentBaud;
  unsigned long baud = SerialUSB.baud();
  if (baud != currentBaud) {
    if (baud == BOOTLOADER_BAUD)
      enterBootloader();
#if STATS
    if (baud == STATS_BAUD)
      printStats();
    else
#endif
      uartBegin(baud);
    currentBaud = baud;
  }

#if STATS
  collectErrors();
#endif

  for (int room = SerialUSB.availableForWrite(); room > 0 && rxTail != rxHead; room--) {
    SerialUSB.write(rxBuf[rxTail]);
    rxTail = (rxTail + 1) & (RX_SIZE - 1);
  }

  while (((txHead + 1) & (TX_SIZE - 1)) != txTail && SerialUSB.available())
    uartWrite(SerialUSB.read());

#if RTS_OUTPUT
  // Ask the far end to pause before the ring fills, and only let it resume
  // once there is real room again, so it isn't switched on every byte.
  uint8_t waiting = (rxHead - rxTail) & (RX_SIZE - 1);
  if (waiting >= RTS_HIGH)
    PORTA |= _BV(PA2);
  else if (waiting <= RTS_LOW)
    PORTA &= ~_BV(PA2);
#endif

#if CTS_INPUT
  cli();  // pick the transmitter up again once the other device is ready
  startTransmitter();
  sei();
#endif

  SerialUSB.refresh();
}
