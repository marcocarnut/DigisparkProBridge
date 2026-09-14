/*
  LinDuplex -- does the ATtiny167 LIN/UART receive correctly while it is
  transmitting? No USB data path involved: the board transmits its own
  counting sequence and checks a counting sequence it receives.

  UART: RX on PA0, TX on PA1, bit rate from the host's setting for the USB
  port (like ProBridge), 8N1. Once a second it reports over USB:
    rx <bytes> breaks <sequence breaks> tx <bytes sent> <on|off>
  Commands over USB: 't' toggles transmitting, Ctrl-C x3 (or 134 bps) jumps
  to the bootloader. Driven by lin_duplex_test.py.
*/

#include <DigiCDCFast.h>

static volatile uint16_t rxBytes, rxBreaks, txBytes;
static volatile bool txOn;
static uint8_t lastRx, nextTx;
static bool haveRx;

ISR(LIN_TC_vect)
{
  if (LINSIR & _BV(LRXOK)) {
    uint8_t c = LINDAT;
    if (haveRx && c != (uint8_t)(lastRx + 1))
      rxBreaks++;
    lastRx = c;
    haveRx = true;
    rxBytes++;
  }
  if ((LINENIR & _BV(LENTXOK)) && (LINSIR & _BV(LTXOK))) {
    if (txOn) {
      LINDAT = nextTx++;
      txBytes++;
    } else {
      LINENIR &= ~_BV(LENTXOK);
    }
  }
}

static void uartBegin(unsigned long baud)
{
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
    if (error <= bestError) {
      bestError = error;
      bestLbt = lbt;
      bestBrr = brr1 - 1;
    }
  }
  cli();
  LINCR = _BV(LSWRES);
  LINBTR = _BV(LDISR) | bestLbt;
  LINBRR = bestBrr;
  LINCR = _BV(LENA) | _BV(LCMD2) | _BV(LCMD1) | _BV(LCMD0);
  LINENIR = _BV(LENRXOK);
  sei();
}

static void startTx()
{
  cli();
  txOn = true;
  if (!(LINENIR & _BV(LENTXOK))) {
    LINDAT = nextTx++;
    txBytes++;
    LINENIR |= _BV(LENTXOK);
  }
  sei();
}

static void enterBootloader()
{
  SerialUSB.delay(100);
  cli();
  LINCR = _BV(LSWRES);
  LINENIR = 0;
  TIMSK0 = 0;
  TIMSK1 = 0;
  TCCR0B = 0;
  TCCR1B = 0;
  ((void (*)())0)();
}

void setup()
{
  SerialUSB.begin();
}

void loop()
{
  static unsigned long currentBaud, lastReport;
  static uint8_t ctrlCs;

  unsigned long baud = SerialUSB.baud();
  if (baud != currentBaud) {
    if (baud == 134)
      enterBootloader();
    uartBegin(baud);
    currentBaud = baud;
  }

  int c = SerialUSB.read();
  if (c >= 0) {
    ctrlCs = c == 3 ? ctrlCs + 1 : 0;
    if (ctrlCs == 3)
      enterBootloader();
    if (c == 't') {
      if (txOn)
        txOn = false;
      else
        startTx();
    }
  }

  if (millis() - lastReport >= 1000) {
    lastReport = millis();
    cli();
    uint16_t rx = rxBytes, breaks = rxBreaks, tx = txBytes;
    rxBytes = rxBreaks = txBytes = 0;
    sei();
    SerialUSB.print("rx ");
    SerialUSB.print(rx);
    SerialUSB.print(" breaks ");
    SerialUSB.print(breaks);
    SerialUSB.print(" tx ");
    SerialUSB.print(tx);
    SerialUSB.println(txOn ? " on" : " off");
  }
}
