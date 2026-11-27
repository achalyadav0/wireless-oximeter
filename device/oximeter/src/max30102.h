#pragma once
/*
 * max30102.h - register-level MAX30102 driver (no external library).
 *
 * Configuration: SpO2 mode (red + IR), 400 samples/s, 4x on-chip averaging -> 100 Hz FIFO
 * output, 411 us pulses (18-bit ADC), 4096 nA full scale, FIFO roll-over enabled.
 * The FIFO (32 samples = 320 ms at 100 Hz) is drained by polling (INT pin not used);
 * overflows are counted, never silently ignored. LED current is controlled by maxAutoGain().
 *
 * v2.3.1: every initialisation step is checked and reported (MaxInitReport), the written
 * configuration is read back and verified, the bus can be scanned / its idle line levels
 * tested, and a recovery path that does NOT reset the sensor when its configuration is intact.
 */
#include <stdint.h>

#define MAX30102_ADDR        0x57
#define MAX30102_PART_ID     0x15
#define MAX_ADC_SAT_LEVEL    260000UL     // 18-bit full scale is 262143
#define LED_PA_DEFAULT       0x24         // 0.2 mA/LSB -> 7.2 mA at start
#define LED_PA_MIN           0x06
#define LED_PA_MAX           0x7F         // 25.4 mA

enum MaxInitResult : uint8_t {
  MAX_OK = 0,
  MAX_NO_ACK = 1,          // nothing acknowledges address 0x57
  MAX_WRONG_PART = 2,      // device answers, PART_ID is not 0x15
  MAX_RESET_FAIL = 3,      // reset command failed or the reset bit never cleared
  MAX_CONFIG_FAIL = 4,     // a configuration write failed or read back a different value
  MAX_READ_FAIL = 5        // ACK on address, but register read (PART_ID) failed
};

struct MaxStats {
  uint8_t  partId;
  uint8_t  ledRed, ledIr;     // current LED settings (0.2 mA per LSB)
  uint32_t i2cErrors;         // failed I2C transactions
  uint32_t fifoOverflows;     // samples lost inside the sensor (FIFO full)
};

// What maxInit() actually observed (for the startup diagnostics - nothing is assumed).
struct MaxInitReport {
  bool    ack;                // address 0x57 acknowledged
  uint8_t ackError;           // Wire.endTransmission() code of the probe (0 = ACK, 2 = address NACK, ...)
  bool    partRead;           // PART_ID register read succeeded
  uint8_t partId;
  uint8_t revId;
  bool    resetOk;            // reset written and RESET bit self-cleared
  bool    configWritten;      // all configuration writes acknowledged
  bool    configVerified;     // read-back of FIFO_CFG / SPO2_CFG / MODE / LED PA matched
  uint8_t modeReg, spo2Reg, fifoReg, ledRedReg, ledIrReg;   // values read back
  bool    fifoCleared;        // FIFO pointers zeroed and read back as zero
};

bool          maxBeginBus(int sda, int scl, uint32_t clockHz);   // false = Wire.begin() failed
MaxInitResult maxInit(MaxInitReport* rep = nullptr);   // reset + configure; keeps current LED setting
const char*   maxResultName(MaxInitResult r);
int           maxReadFifo(uint32_t* red, uint32_t* ir, int maxN);   // samples read, -1 = I2C error
int           maxFifoPending();                         // unread samples in FIFO, -1 = I2C error
void          maxSetLeds(uint8_t red, uint8_t ir);
void          maxBusRecover(int sda, int scl, uint32_t clockHz);  // frees a stuck bus, restarts Wire
const MaxStats& maxStats();

// Bus diagnostics.
uint8_t maxProbe(uint8_t addr);                         // Wire.endTransmission() code, 0 = ACK
int     maxScanBus(uint8_t* found, int maxFound);       // ACKing 7-bit addresses 0x08..0x77
// Idle line test. MUST be called while Wire is NOT running (before maxBeginBus or after Wire.end()).
// pullOk*: line reads HIGH while the ESP32 pulls it DOWN weakly (~45 kOhm) -> an external pull-up
//          to a valid logic-high level exists.
// highOk*: line reads HIGH with the internal pull-up -> the line is not shorted / held low.
struct I2cLineTest { bool sdaPullOk, sclPullOk, sdaHighOk, sclHighOk; };
I2cLineTest maxLineTest(int sda, int scl);

// Recovery without a sensor reset: true if the part answers and its configuration is still
// the one maxInit() wrote (then the FIFO is flushed and sampling can simply continue).
bool maxConfigIntact();
bool maxFlushFifo();

// LED auto-gain: keeps the DC level in the linear ADC range (call ~every 1.5 s with a finger).
// Returns true if the LED current was changed (the algorithm detects the DC step itself).
bool maxAutoGain(float dcIr, float dcRed, uint32_t irContactMin, uint32_t redContactMin,
                 uint32_t lastIr, uint32_t lastRed);
