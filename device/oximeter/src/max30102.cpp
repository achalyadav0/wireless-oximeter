#include "max30102.h"
#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#define REG_INTEN1     0x02
#define REG_INTEN2     0x03
#define REG_FIFO_WR    0x04
#define REG_FIFO_OVF   0x05
#define REG_FIFO_RD    0x06
#define REG_FIFO_DATA  0x07
#define REG_FIFO_CFG   0x08
#define REG_MODE       0x09
#define REG_SPO2_CFG   0x0A
#define REG_LED1_PA    0x0C   // red
#define REG_LED2_PA    0x0D   // IR
#define REG_REV_ID     0xFE
#define REG_PART_ID    0xFF
#define REG_INTSTAT1   0x00
#define MODE_SPO2      0x03   // MODE[2:0] = 011: SpO2 mode, red (LED1) + IR (LED2)

// FIFO_CFG: SMP_AVE = 4 (0b010), FIFO_ROLLOVER_EN = 1, FIFO_A_FULL = 15
#define FIFO_CFG_VALUE ((0x02 << 5) | (1 << 4) | 0x0F)
// SPO2_CFG: ADC range 4096 nA (01), sample rate 400 sps (011), pulse width 411 us / 18 bit (11)
#define SPO2_CFG_VALUE ((0x01 << 5) | (0x03 << 2) | 0x03)

// v3.2.1: auto-gain targets – higher target = more LED current = better PI
// These can be overridden in config.h via LED_AGC_TARGET_HI / _LO.
#ifndef LED_AGC_TARGET_LO
#define LED_AGC_TARGET_LO  120000UL
#endif
#ifndef LED_AGC_TARGET_HI
#define LED_AGC_TARGET_HI  210000UL
#endif
static const uint32_t AGC_HIGH   = LED_AGC_TARGET_HI;
static const uint32_t AGC_LOW    = LED_AGC_TARGET_LO;
static const uint32_t AGC_TARGET = (LED_AGC_TARGET_LO + LED_AGC_TARGET_HI) / 2;

static MaxStats st = { 0, LED_PA_DEFAULT, LED_PA_DEFAULT, 0, 0 };

static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MAX30102_ADDR); Wire.write(reg); Wire.write(val);
  if (Wire.endTransmission() != 0) { st.i2cErrors++; return false; }
  return true;
}
static bool rd(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(MAX30102_ADDR); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) { st.i2cErrors++; return false; }
  if (Wire.requestFrom((uint8_t)MAX30102_ADDR, len) != len) { while (Wire.available()) Wire.read(); st.i2cErrors++; return false; }
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool maxBeginBus(int sda, int scl, uint32_t clockHz) {
  bool ok = Wire.begin(sda, scl, clockHz);        // pins + clock in one call (ESP32 core 3.x API)
  Wire.setClock(clockHz);
  Wire.setTimeOut(50);                            // ms per transaction: a missing sensor never blocks long
  return ok;
}

uint8_t maxProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission();                  // 0 ACK, 2 address NACK, 3 data NACK, 4/5 bus error/timeout
}

int maxScanBus(uint8_t* found, int maxFound) {
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    if (maxProbe(a) == 0) { if (n < maxFound) found[n] = a; n++; }
  }
  return n;
}

I2cLineTest maxLineTest(int sda, int scl) {
  I2cLineTest t;
  pinMode(sda, INPUT_PULLDOWN); pinMode(scl, INPUT_PULLDOWN); delayMicroseconds(200);
  t.sdaPullOk = digitalRead(sda) == HIGH; t.sclPullOk = digitalRead(scl) == HIGH;
  pinMode(sda, INPUT_PULLUP);   pinMode(scl, INPUT_PULLUP);   delayMicroseconds(200);
  t.sdaHighOk = digitalRead(sda) == HIGH; t.sclHighOk = digitalRead(scl) == HIGH;
  return t;
}

const char* maxResultName(MaxInitResult r) {
  switch (r) {
    case MAX_OK:          return "OK";
    case MAX_NO_ACK:      return "NO ACK at 0x57";
    case MAX_WRONG_PART:  return "WRONG PART_ID";
    case MAX_RESET_FAIL:  return "RESET FAILED";
    case MAX_CONFIG_FAIL: return "CONFIGURATION FAILED";
    case MAX_READ_FAIL:   return "REGISTER READ FAILED";
  }
  return "?";
}

MaxInitResult maxInit(MaxInitReport* rep) {
  MaxInitReport R; memset(&R, 0, sizeof(R));
  MaxInitResult res = MAX_OK;
  do {
    R.ackError = maxProbe(MAX30102_ADDR);
    R.ack = (R.ackError == 0);
    if (!R.ack) { res = MAX_NO_ACK; break; }

    uint8_t id = 0, rev = 0;
    R.partRead = rd(REG_PART_ID, &id, 1);
    if (!R.partRead) { res = MAX_READ_FAIL; break; }
    rd(REG_REV_ID, &rev, 1);
    R.partId = id; R.revId = rev; st.partId = id;
    if (id != MAX30102_PART_ID) { res = MAX_WRONG_PART; break; }

    // ---- reset: the RESET bit (MODE bit 6) clears itself when the reset is complete ----
    bool rw = wr(REG_MODE, 0x40);
    if (!rw) { delay(5); rw = wr(REG_MODE, 0x40); }                 // one retry (part may still be powering up)
    if (!rw) { res = MAX_RESET_FAIL; break; }
    uint32_t t0 = millis(); uint8_t m = 0x40;
    while (millis() - t0 < 100) { delay(2); if (rd(REG_MODE, &m, 1) && !(m & 0x40)) break; }
    R.resetOk = !(m & 0x40);
    if (!R.resetOk) { res = MAX_RESET_FAIL; break; }
    uint8_t dummy; rd(REG_INTSTAT1, &dummy, 1);                    // clear PWR_RDY / pending flags

    // ---- configuration ----
    bool ok = true;
    ok &= wr(REG_INTEN1, 0x00);
    ok &= wr(REG_INTEN2, 0x00);
    ok &= wr(REG_FIFO_CFG, FIFO_CFG_VALUE);
    ok &= wr(REG_SPO2_CFG, SPO2_CFG_VALUE);
    ok &= wr(REG_LED1_PA, st.ledRed);
    ok &= wr(REG_LED2_PA, st.ledIr);
    ok &= wr(REG_FIFO_WR, 0); ok &= wr(REG_FIFO_OVF, 0); ok &= wr(REG_FIFO_RD, 0);
    ok &= wr(REG_MODE, MODE_SPO2);                                   // SpO2 mode = red + IR LEDs active
    R.configWritten = ok;
    if (!ok) { res = MAX_CONFIG_FAIL; break; }

    // ---- read back: report what the sensor actually holds, not what we hoped to write ----
    bool rb = rd(REG_FIFO_CFG, &R.fifoReg, 1) && rd(REG_MODE, &R.modeReg, 1) && rd(REG_SPO2_CFG, &R.spo2Reg, 1) &&
              rd(REG_LED1_PA, &R.ledRedReg, 1) && rd(REG_LED2_PA, &R.ledIrReg, 1);
    // SPO2_CFG bit 7 is reserved: compare bits 6..0 only
    R.configVerified = rb && R.fifoReg == FIFO_CFG_VALUE && (R.modeReg & 0x07) == MODE_SPO2 &&
                       (R.spo2Reg & 0x7F) == SPO2_CFG_VALUE && R.ledRedReg == st.ledRed && R.ledIrReg == st.ledIr;
    if (!R.configVerified) { res = MAX_CONFIG_FAIL; break; }

    uint8_t p[3] = { 0xFF, 0xFF, 0xFF };
    // The sensor starts converting immediately, so WR may already be 1-2; only RD and OVF must be 0.
    R.fifoCleared = rd(REG_FIFO_WR, p, 3) && (p[1] & 0x1F) == 0 && (p[2] & 0x1F) == 0;
    if (!R.fifoCleared) { res = MAX_CONFIG_FAIL; break; }
  } while (0);
  if (rep) *rep = R;
  return res;
}

bool maxConfigIntact() {
  if (maxProbe(MAX30102_ADDR) != 0) return false;
  uint8_t id = 0, f = 0, m = 0, s = 0, lr = 0, li = 0;
  if (!rd(REG_PART_ID, &id, 1) || id != MAX30102_PART_ID) return false;
  if (!rd(REG_FIFO_CFG, &f, 1) || !rd(REG_MODE, &m, 1) || !rd(REG_SPO2_CFG, &s, 1) ||
      !rd(REG_LED1_PA, &lr, 1) || !rd(REG_LED2_PA, &li, 1)) return false;
  return f == FIFO_CFG_VALUE && (m & 0x07) == MODE_SPO2 && (s & 0x7F) == SPO2_CFG_VALUE && lr == st.ledRed && li == st.ledIr;
}

bool maxFlushFifo() {
  return wr(REG_FIFO_WR, 0) && wr(REG_FIFO_OVF, 0) && wr(REG_FIFO_RD, 0);
}

int maxFifoPending() {
  uint8_t p[3];
  if (!rd(REG_FIFO_WR, p, 3)) return -1;
  if (p[1] & 0x1F) return 32;
  return ((p[0] & 0x1F) - (p[2] & 0x1F)) & 0x1F;
}

int maxReadFifo(uint32_t* red, uint32_t* ir, int maxN) {
  uint8_t p[3];
  if (!rd(REG_FIFO_WR, p, 3)) return -1;
  uint8_t w = p[0] & 0x1F, ovf = p[1] & 0x1F, r = p[2] & 0x1F;
  int n = (w - r) & 0x1F;
  if (ovf) { n = 32; st.fifoOverflows += ovf; }
  if (n > maxN) n = maxN;
  int got = 0;
  while (got < n) {
    int c = n - got; if (c > 5) c = 5;                              // 30 bytes per transfer
    Wire.beginTransmission(MAX30102_ADDR); Wire.write(REG_FIFO_DATA);
    if (Wire.endTransmission(false) != 0) { st.i2cErrors++; return -1; }
    uint8_t want = (uint8_t)(c * 6);
    if (Wire.requestFrom((uint8_t)MAX30102_ADDR, want) != want) { while (Wire.available()) Wire.read(); st.i2cErrors++; return -1; }
    for (int i = 0; i < c; i++) {
      uint32_t rv = 0, xv = 0;
      for (int k = 0; k < 3; k++) rv = (rv << 8) | (uint8_t)Wire.read();
      for (int k = 0; k < 3; k++) xv = (xv << 8) | (uint8_t)Wire.read();
      red[got + i] = rv & 0x3FFFF; ir[got + i] = xv & 0x3FFFF;
    }
    got += c;
  }
  return got;
}

void maxSetLeds(uint8_t red, uint8_t ir) {
  st.ledRed = red; st.ledIr = ir;
  wr(REG_LED1_PA, red); wr(REG_LED2_PA, ir);
}

void maxBusRecover(int sda, int scl, uint32_t clockHz) {
  Wire.end();
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, OUTPUT_OPEN_DRAIN); digitalWrite(scl, HIGH); delayMicroseconds(6);
  for (int i = 0; i < 9 && digitalRead(sda) == LOW; i++) {         // clock out a slave stuck mid-byte
    digitalWrite(scl, LOW); delayMicroseconds(6); digitalWrite(scl, HIGH); delayMicroseconds(6);
  }
  // STOP condition (SDA low -> high while SCL is high) so every slave returns to idle
  pinMode(sda, OUTPUT_OPEN_DRAIN);
  digitalWrite(scl, LOW);  delayMicroseconds(6);
  digitalWrite(sda, LOW);  delayMicroseconds(6);
  digitalWrite(scl, HIGH); delayMicroseconds(6);
  digitalWrite(sda, HIGH); delayMicroseconds(6);
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  maxBeginBus(sda, scl, clockHz);
}

const MaxStats& maxStats() { return st; }

static bool agcDecide(uint32_t dc, uint32_t contactMin, uint8_t cur, uint8_t& next) {
  next = cur;
  if (dc > AGC_HIGH && cur > LED_PA_MIN) {
    float f = (float)AGC_TARGET / (float)dc; int v = (int)(cur * f); if (v >= cur) v = cur - 1;
    next = (uint8_t)(v < LED_PA_MIN ? LED_PA_MIN : v);
  } else if (dc < AGC_LOW && dc > contactMin && cur < LED_PA_MAX) {
    float f = (float)AGC_TARGET / (float)(dc > 1 ? dc : 1); if (f > 2.0f) f = 2.0f;
    int v = (int)(cur * f); if (v <= cur) v = cur + 1;
    next = (uint8_t)(v > LED_PA_MAX ? LED_PA_MAX : v);
  }
  return next != cur;
}

bool maxAutoGain(float dcIr, float dcRed, uint32_t irContactMin, uint32_t redContactMin,
                 uint32_t lastIr, uint32_t lastRed) {
  uint8_t ni = st.ledIr, nr = st.ledRed;
  bool ch = agcDecide((uint32_t)dcIr, irContactMin, st.ledIr, ni);
  ch |= agcDecide((uint32_t)dcRed, redContactMin, st.ledRed, nr);
  if (!ch && (lastIr >= MAX_ADC_SAT_LEVEL || lastRed >= MAX_ADC_SAT_LEVEL)) {    // saturated
    if (lastIr >= MAX_ADC_SAT_LEVEL)  ni = st.ledIr  > LED_PA_MIN + 8 ? st.ledIr  / 2 : LED_PA_MIN;
    if (lastRed >= MAX_ADC_SAT_LEVEL) nr = st.ledRed > LED_PA_MIN + 8 ? st.ledRed / 2 : LED_PA_MIN;
    ch = (ni != st.ledIr) || (nr != st.ledRed);
  }
  if (ch) maxSetLeds(nr, ni);
  return ch;
}
