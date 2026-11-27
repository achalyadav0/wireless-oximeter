#pragma once
/*
 * ppg_algorithm.h - motion-tolerant heart-rate / SpO2 estimation for a red + IR PPG
 * sensor (MAX30102) WITHOUT an accelerometer.
 *
 * Pure C++ (no Arduino, no hardware access): it can be unit-tested on a PC
 * (see test/test_algorithm and the [env:native] section in platformio.ini).
 *
 * Input : one red + one IR raw ADC sample at ALGO_FS_HZ (50 Hz) via algoPush().
 *         The firmware samples at 100 Hz and feeds the mean of each sample pair,
 *         which is exactly a 400 sps / 8x-averaged stream.
 * Output: algoOut() - HR, SpO2, R, perfusion index, quality, motion, state ...
 *         updated once per second (algoPush() returns true on those samples).
 *
 * Method (details in docs/hardware/firmware-algorithm.md):
 *   band-pass + DC normalisation -> per-FFT-bin two-source (arterial/motion) separation
 *   from the red/IR colour difference (DST principle, dual-wavelength motion reference)
 *   -> Bayesian spectral HR tracker fused with a time-domain beat detector
 *   -> ratio-of-ratios R measured only at the tracked HR, accepted only after 8
 *   artifact tests -> median -> calibration curve.  Values that cannot be verified are
 *   withheld (null), never invented.
 */
#include <stdint.h>

#define ALGO_FS_HZ 50.0f                 // algorithm sample rate (Hz)

// ---- why a 10-s window was NOT used for SpO2 (AlgoOut::rReject bit mask) ----
#define REJ_STEP     0x001   // DC step / saturation inside window
#define REJ_NOLOCK   0x002   // HR tracker not confident
#define REJ_PURITY   0x004   // pulse does not dominate the spectrum
#define REJ_HARM     0x008   // fundamental and 2nd harmonic give different R
#define REJ_COH      0x010   // red and IR not coherent at the HR
#define REJ_SPREAD   0x020   // ratio varies across the HR peak (motion overlap)
#define REJ_RANGE    0x040   // R outside plausible range
#define REJ_MOTION   0x080   // motion energy on the HR bins
#define REJ_SHORT    0x100   // window not full yet
#define REJ_CONFIRM  0x200   // large change waiting for confirmation
#define REJ_NOISE    0x400   // sensor noise too high relative to the pulse

enum MeasState : uint8_t {
  ST_SENSOR_ERROR = 0, ST_NO_FINGER, ST_WARMUP, ST_VALID, ST_MOTION, ST_LOW_PERFUSION, ST_NO_LOCK
};
extern const char* const STATE_NAMES[];   // "SENSOR_ERROR", "NO_FINGER", "WARMUP", "VALID", ...
extern const char* const MOTION_NAMES[];  // "NONE", "LOW", "MODERATE", "HIGH"

// Everything the algorithm publishes.
struct AlgoOut {
  bool    contact;
  float   hrBpm;        bool hrValid;   bool hrHeld;   float hrConf;
  float   spo2;         bool spo2Valid; bool spo2Held;
  float   rNow;         float rMedian;  bool rAccepted;
  float   pi;           // perfusion index (IR, %)
  uint8_t motion;       // 0..3 (NONE..HIGH)
  float   motionIdx, ampRatio, dcJump, purity, skew, quality;
  float   ra;           // arterial ratio used for the motion reference
  float   kMot;         // estimated red/IR ratio of the motion component
  bool    sep;          // two-source separation active this frame
  float   purityArt;    // spectral purity after separation
  float   rSpread;      // per-bin ratio spread around the HR (contamination test)
  uint8_t winMotion;    // worst motion level inside the analysis window
  uint16_t rReject;     // why the last window's R was rejected (REJ_* bits)
  float   contam;       // motion power on the HR bins relative to the pulse
  float   hrSpec;       // spectral (tracker) HR
  float   hrBeats;      // time-domain HR (mean over window), 0 if unreliable
  float   rrCv;         // beat interval variability (irregular rhythm indicator)
  bool    irregular;    // rhythm looks irregular (AF / ectopics) - HR is the mean rate
  float   warmupPct;
  uint32_t stepEvents, rAcceptCount, framesTotal;
};

// ---- core API ----
void           algoInit();
bool           algoPush(uint32_t redRaw, uint32_t irRaw);   // true = new 1-s estimate
MeasState      algoState(bool sensorOk);
const AlgoOut& algoOut();
void           algoSensorGap();                           // sensor data gap: restart the session (no stale values)
const char*    rejectReason(uint16_t rejectMask);           // human-readable first reason

// ---- v3.0.0: motion-INDEPENDENT contact-quality basis (Phase 2 point 2) ----
// cleanRms is updated ONLY on calm, pure, locked frames (see ppg_algorithm.cpp) and therefore
// reflects the pulse amplitude the LAST time the signal was motion-free - it does not rise or
// fall just because the patient is currently moving, unlike the real-time quality/pi values.
// algoCleanRmsInit() is false until at least one calm second has been observed (or the 15 s
// forced-init fallback has fired - see docs/KNOWN_LIMITATIONS.md "continuous-motion edge case").
float algoCleanRms();
bool  algoCleanRmsInit();

// ---- finger detection thresholds (raw counts) ----
void     algoSetContactThresholds(uint32_t irMin, uint32_t redMin);
uint32_t algoIrContactMin();
uint32_t algoRedContactMin();
// The MAX30102 time base is not exact (typically +-1-2 %): main.cpp measures the true sample
// rate against millis() and passes true/nominal here; every heart rate is multiplied by it.
void     algoSetFsScale(float scale);
float    algoFsScale();
float    algoDcIr();                                        // slow DC level (for LED auto-gain)
float    algoDcRed();

// ---- SpO2 calibration: SpO2 = a + b*R fitted to (R, reference SpO2) points ----
#define CAL_MAX_POINTS 8
float spo2FromR(float R);            // monotonic: higher R -> lower SpO2, capped at 100
int   calCount();
void  calGetPoint(int i, float& R, float& refSpo2);
void  calSetPoints(const float* R, const float* refSpo2, int n);
void  calAddPoint(float R, float refSpo2);
void  calClear();
void  calLine(float& a, float& b);
// v3.0.0 (Phase 2 point 3): calLine() alone cannot tell a caller whether calB is a genuine
// least-squares fit or the untouched default slope (CAL_SLOPE_DEFAULT) with only an offset
// shift applied. calWasFitted() exposes calFit()'s OWN internal decision directly, so the
// SpO2-mode label is never re-derived independently of what calFit() actually did.
bool  calWasFitted();

#ifdef ALGO_TEST_HOOKS
// ======================================================================================
// TEST-ONLY white-box entry point. Not part of the production API; compiled out unless
// ALGO_TEST_HOOKS is defined (never defined in the Arduino/firmware build). Exposes the
// two-pass beat detector in isolation so host tests can verify exact candidate-removal
// behaviour (which peak is dropped, in what order) without needing a full 7+ second
// algoPush() stream for every case. See test/test_hr_estimation.cpp.
// ----------------------------------------------------------------------------------------
// pulse:      a pre-built, already colour-corrected pulse waveform (as beatAnalysis() would
//             receive from tdBuf[]) - caller supplies it directly for precise test control.
// N:          number of samples in `pulse`, at ALGO_FS_HZ.
// outPkTimes: Pass-2 (post dicrotic-removal) accepted peak sample indices, float (parabolic-
//             interpolated), written up to maxPk entries.
// returns:    number of Pass-2 peaks written to outPkTimes.
// outHrMean/outCv/outBadIv: same meaning as beatAnalysis()'s existing outputs, computed from
//             the Pass-2 (post-removal) peak set.
// outMerged:  true if Pass 2's dicrotic-removal was ACCEPTED (regularity improved); false if
//             Pass 1's raw candidate list was kept unchanged (removal rejected or not attempted).
// ======================================================================================
int algoTestBeatDetect(const float* pulse, int N, float* outPkTimes, int maxPk,
                        float& outHrMean, float& outCv, int& outBadIv, bool& outMerged);
#endif
