/*
  ProBridge -- USB <-> UART bridge for the Digispark Pro (ATtiny167)

  USB side: DigiCDCFast. UART side: the ATtiny167's LIN/UART in UART mode,
  RX on PA0, TX on PA1, 8N1. The UART bit rate follows the rate the host sets
  for the USB port (stty, terminal programs). The LIN/UART can use 8 to 63
  samples per bit; the combination with the smallest error is chosen, e.g.
  0.44% at 57600 bps, where the core's Serial (always 16 samples) is 3.5% off.

  Setting the port to 134 bps (stty -F /dev/ttyACM0 134) jumps to the
  micronucleus bootloader, for reflashing without replugging.

  Low-speed USB carries at most 8000 bytes/s, so UART input faster than that
  (above about 76800 bps, sent continuously) overflows the receive buffer.
*/

#include <DigiCDCFast.h>

#define BOOTLOADER_BAUD 134
#define RX_SIZE         64  // powers of 2
#define TX_SIZE         64

static uint8_t rxBuf[RX_SIZE], txBuf[TX_SIZE];
static volatile uint8_t rxHead, txTail;  // advanced by the interrupt handler
static uint8_t rxTail, txHead;           // advanced by loop()
// LINENIR as it should be; the register itself is 0 while the handler runs
static uint8_t linEnable;

static void receive()  // interrupts must be off, or this must be the handler
{
  uint8_t c = LINDAT;  // reading clears LRXOK
  uint8_t next = (rxHead + 1) & (RX_SIZE - 1);
  if (next != rxTail) {
    rxBuf[rxHead] = c;
    rxHead = next;
  }
}

static void transmit(uint8_t c)
{
  LINDAT = c;  // writing clears LTXOK
}

// V-USB must never wait for this handler (a delayed USB interrupt makes the
// host's transaction fail), so it masks its own interrupts and lets others in.
ISR(LIN_TC_vect)
{
  LINENIR = 0;
  sei();
  if (LINSIR & _BV(LRXOK)) {
    cli();
    receive();
    sei();
  }
  if ((linEnable & _BV(LENTXOK)) && (LINSIR & _BV(LTXOK))) {
    if (txTail == txHead) {
      linEnable &= ~_BV(LENTXOK);  // nothing left to send
    } else {
      transmit(txBuf[txTail]);
      txTail = (txTail + 1) & (TX_SIZE - 1);
    }
  }
  cli();
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

static void uartWrite(uint8_t c)  // the caller checks there is room
{
  txBuf[txHead] = c;
  uint8_t sreg = SREG;
  cli();
  txHead = (txHead + 1) & (TX_SIZE - 1);
  if (!(linEnable & _BV(LENTXOK))) {  // transmitter idle: start it
    transmit(txBuf[txTail]);
    txTail = (txTail + 1) & (TX_SIZE - 1);
    linEnable |= _BV(LENTXOK);
    LINENIR = linEnable;
  }
  SREG = sreg;
}

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
  SerialUSB.begin();
}

void loop()
{
  static unsigned long currentBaud;
  unsigned long baud = SerialUSB.baud();
  if (baud != currentBaud) {
    if (baud == BOOTLOADER_BAUD)
      enterBootloader();
    uartBegin(baud);
    currentBaud = baud;
  }

  for (int room = SerialUSB.availableForWrite(); room > 0 && rxTail != rxHead; room--) {
    SerialUSB.write(rxBuf[rxTail]);
    rxTail = (rxTail + 1) & (RX_SIZE - 1);
  }

  while (((txHead + 1) & (TX_SIZE - 1)) != txTail && SerialUSB.available())
    uartWrite(SerialUSB.read());

  SerialUSB.refresh();
}
