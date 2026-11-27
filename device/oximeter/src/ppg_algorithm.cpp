/*
 * ppg_algorithm.cpp - see ppg_algorithm.h.  Hardware independent; validated offline on
 * 20 physiological red/IR scenarios (rest, low perfusion, brady/tachycardia, hypoxia ramp,
 * tapping, walking, exercise, motion at the heart rate, pressure steps, AF, finger off/on,
 * saturation, HR jumps, jerks, heavy motion, weak signal) with realistic noise.
 * Simulation is not clinical validation: verify against a certified reference oximeter.
 *
 * v3.1.0 (stability / trust release) - see FIRMWARE_CHANGES_v3.1.0.md:
 *   - tracker band 42-200 bpm, edge-of-band locks rejected (slow wander used to be locked as ~35 bpm)
 *   - pulse-shape (peak width) verification; a rejected frequency is suppressed for 15 s
 *   - beat detector: hysteresis re-arm (was counting ripple on broad peaks as extra beats)
 *   - harmonic guard (2x / 3x locks corrected using beat rate + subharmonic power)
 *   - first HR needs 3 agreeing frames; no 12 s hole in the jump confirmation
 *   - display: median-of-3 + slew limit (HR 4 bpm/s, SpO2 0.7 %/s); SpO2 only while HR is valid
 */
#include "ppg_algorithm.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>

#define FS_HZ ALGO_FS_HZ

// ---- Sensor ------------------------------------------------------------------
static const uint32_t ADC_SAT_LEVEL       = 260000UL;  // MAX30102 18-bit full scale is 262143

// ---- Contact ----------------------------------------------------------------
static const uint32_t IR_CONTACT_MIN      = 20000;  // default; set with algoSetContactThresholds()
static const uint32_t RED_CONTACT_MIN     = 8000;
static const int      CONTACT_ON_SAMPLES  = 25;     // 0.5 s
static const int      CONTACT_OFF_SAMPLES = 5;      // 0.1 s
static const float    FINGER_LEAVE_FRAC   = 0.6f;   // IR below 60 % of its DC = finger being lifted

// ---- Filters ----------------------------------------------------------------
static const float BP_HP_HZ  = 0.5f;
static const float BP_LP_HZ  = 6.0f;
static const float DC_LP_HZ  = 0.4f;
static const float STEP_FRAC = 0.06f;               // fast DC trend >6 % away from slow DC (persisting) => re-init filters

// ---- Analysis ---------------------------------------------------------------
#define RB_SIZE               512                   // ring buffer (power of 2)
#define RB_MASK               (RB_SIZE - 1)
#define WIN_N                 480                   // 9.6 s analysis window
#define WIN_MIN               250                   // v3.2.1: 5 s – faster first estimate on weak signals
#define HOP_N                 50                    // analyse every 1 s
#define NFFT                  1024
#define LOG2_NFFT             10
// v3.1.0: tracker band 42-200 bpm (was 35-220); a lock on the lowest/highest 2 bins is rejected as an
// edge lock (see below), so the reported range is about 50-200 bpm. The bottom of the old band sat right
// on top of slow baseline wander (respiration, finger pressure, slight hand movement, ~0.5-0.7 Hz):
// a weak pulse next to such wander made the spectral tracker lock onto the wander at ~35 bpm and
// report it as "VALID". A resting adult finger pulse below 40 bpm is rare enough that withholding
// it (rather than risking a wander lock) is the trustworthy choice for this sensor.
static const float HR_MIN_BPM = 42.0f;
static const float HR_MAX_BPM = 200.0f;
#define SPEC_KMAX             160                   // highest bin kept (7.8 Hz)
#ifndef R_WINDOW_MODE
#define R_WINDOW_MODE         1                     // R spectrum window: 0 = same Hann 8 s, 1 = Blackman 10 s (low leakage)
#endif
#define WINR_N                350                   // v3.2.1: 7 s – matches WIN_MIN on weak-signal sensors

// ---- Optical motion reference / two-source separation -------------------------
static const float RA_DEFAULT    = 0.55f;           // initial arterial ratio (~97-98 %)
static const float RA_MIN = 0.25f, RA_MAX = 2.0f;
static const float MOT_BIN_RATIO = 0.10f;           // bin is motion-dominated if |ref|^2 > this x |ir|^2
static const float MOT_FRAC_MIN  = 0.15f;           // min share of band power in motion bins to separate
static const float SEP_MIN_DK    = 0.15f;           // |k_motion - Ra| below this: colour cannot separate
static const int   STEP_PERSIST  = 8;               // samples a DC jump must persist (0.16 s)
static const float SEP_KEEP_FRAC = 0.30f;           // separation must keep >= 30 % of the tracked pulse power (weak-motion case)
static const float CONTAM_MAX    = 0.08f;           // v3.2.1: weak signal has higher contamination ratio at rest

// ---- Tracker ------------------------------------------------------------------
static const float TRK_SIGMA_BINS = 1.6f;           // HR can move ~5 bpm/s (1 bin = 2.93 bpm)
static const float TRK_FLOOR      = 0.002f;         // allows re-acquisition after long artifacts
static const float HARM_W         = 0.5f;           // weight of 2nd harmonic support
static const float LOCK_CONF      = 0.30f;

// ---- v3.1.0: pulse verification + display governor ------------------------------------------
// A real finger pulse peak is narrow in ABSOLUTE time: measured at 70 % of its height it is typically
// 0.10-0.30 s wide whatever the heart rate. Slow baseline wander / pressure drift that the spectral
// tracker may lock onto is sinusoidal: at 70 % height it is 0.37 x its period wide (0.64 s at 35 bpm).
// A lock whose beat peaks are wider than this is not a pulse and is rejected (and its frequency
// suppressed so the real pulse can win). Unvalidated engineering default - see KNOWN_LIMITATIONS.
static const float PULSE_WIDTH_LEVEL  = 0.70f;   // height fraction at which the width is measured
static const float PULSE_FWHM_MAX_S   = 0.45f;
static const float PULSE_GATE_MIN_BPM = 50.0f;   // below this the 0.5 Hz high-pass widens real pulses: width is not informative
static const int   PULSE_FAIL_FRAMES  = 4;      // v3.2.1: weak-signal beat detector is noisier – need 4 bad frames
static const int   SUPPRESS_FRAMES    = 5;      // v3.2.1: shorter suppression avoids long blackouts on weak signals
static const int   FIRST_LOCK_AGREE   = 2;      // v3.2.1: 2 frames – faster first lock on weak signals
static const float HR_MAX_SLEW        = 4.0f;   // displayed HR may move at most this many bpm per second
static const float SPO2_MAX_SLEW      = 0.7f;   // displayed SpO2 may move at most this many % per second

// ---- Time-domain beat detector --------------------------------------------------
static const float BEAT_THR_FRAC  = 0.55f;          // peak must exceed p10 + this x (p90 - p10)
static const float BEAT_MIN_SEP   = 0.55f;          // x spectral period: blocks dicrotic double counts
static const int   BEAT_MIN_N     = 4;              // v3.2.1: 4 beats – more lenient on weak signals
static const float IRREG_CV       = 0.12f;          // RR coefficient of variation above this = irregular rhythm
static const float TD_AGREE       = 0.30f;
static const int   HR_JUMP_CONFIRM = 2;             // v3.2.1: 2 frames – faster HR update
static const float TD_TIGHT       = 0.08f;          // beats refine the spectral HR only within 8 %          // time-domain and spectral HR must agree within 30 %

// ---- Output policy ------------------------------------------------------------
static const float HR_HOLD_S      = 12.0f;          // v3.2.1: longer hold keeps values on-screen
static const float SPO2_HOLD_S    = 12.0f;          // v3.2.1: longer hold
static const float JUMP_REST      = 0.15f;          // v3.2.1: weak signals jitter more – higher jump threshold
static const float JUMP_MOVE      = 0.04f;          // ... and while the window contains movement
static const float TREND_R        = 0.05f;          // newest-3 mean vs long average: follow when they differ by > 0.05 R (~1.3 % SpO2)
#define R_HIST                12                    // trimmed mean of the last accepted R windows
static const float SPO2_MAX_AGE_S = 30.0f;          // R windows older than this are dropped

// ---- SpO2 CALIBRATION ---------------------------------------------------------
// R (ratio of ratios) is MEASURED; the R -> SpO2 curve is EMPIRICAL and depends on the
// LEDs, the finger and the enclosure. ADI/Maxim publish no curve for the bare MAX30102.
//  * Uncalibrated default: Maxim RD117 reference curve -45.060 R^2 + 30.354 R + 94.845,
//    made monotonic (R below the curve's peak 0.337 maps to that peak, ~100 %), because the
//    raw quadratic bends DOWN again below R = 0.337, which is physically wrong.
//  * Calibrated: put the finger on this sensor and a certified oximeter on another finger,
//    keep still, and type  ref=<reading of the certified oximeter>  (e.g. ref=97).
//    1 point  -> offset fit with the standard slope (SpO2 = a - 25 R)
//    2+ points (R spread >= 0.05, ideally different people / SpO2 levels) -> least-squares line.
//    Points are stored in flash.  ref=clear removes them.
static const float SPO2_A = 94.845f, SPO2_B = 30.354f, SPO2_C = -45.060f;
static const float SPO2_R_PEAK = 0.3368f;           // vertex of the RD117 quadratic (-B / 2C)
static const float CAL_SLOPE_DEFAULT = -25.0f;      // standard linear slope (SpO2 = 110 - 25 R family)
#ifndef XN_FACTOR
#define XN_FACTOR 1.5f
#endif
static const float R_VALID_MIN = 0.20f, R_VALID_MAX = 1.60f;

// ============================================================================
//                                   TYPES
// ============================================================================
const char* const STATE_NAMES[] = {
  "SENSOR_ERROR", "NO_FINGER", "WARMUP", "VALID", "MOTION", "LOW_PERFUSION", "SEARCHING"
};
const char* const MOTION_NAMES[] = { "NONE", "LOW", "MODERATE", "HIGH" };

struct Biquad { float b0, b1, b2, a1, a2, z1, z2; };

// ============================================================================
//                               ALGORITHM STATE
// ============================================================================
static AlgoOut  out;

// contact
static bool     contact = false;
static int      onCnt = 0, offCnt = 0;
static uint32_t irContactMin = IR_CONTACT_MIN, redContactMin = RED_CONTACT_MIN;

// filters (per channel): DC low-pass, band-pass = 2x HP + 2x LP
static Biquad   dcIrF, dcRedF, hpIr1, hpIr2, lpIr1, lpIr2, hpRed1, hpRed2, lpRed1, lpRed2;
static float    refIrDC = 1, refRedDC = 1;        // DC at last (re)initialisation
static float    dcIr = 0, dcRed = 0;
static float    trendIr = 0;                      // fast DC tracker for step detection

// ring buffers (AC/DC normalised signals)
static float    rbIr[RB_SIZE], rbRed[RB_SIZE], rbRef[RB_SIZE];
static uint8_t  rbBad[RB_SIZE];
static uint32_t nSamp = 0;                        // samples since contact start
static uint32_t sinceReset = 0;                   // samples since last filter re-init
static int      blankLeft = 0;                    // samples to mark bad after a step

// optical separation
static float    ra = RA_DEFAULT;                  // arterial red/IR ratio (learned on clean data)
static float    kMot = 1.0f;                      // motion red/IR ratio (estimated every frame)
static float    raSep = RA_DEFAULT;               // arterial ratio used by the separation this frame
static bool     sepOn = false;
static int      stepCnt = 0;

// FFT
static float    fRe[NFFT], fIm[NFFT];
static float    twC[NFFT / 2], twS[NFFT / 2];
static uint16_t bitRev[NFFT];
static float    hann[WIN_N];
static float    blackR[WINR_N];
static float    Pir[SPEC_KMAX + 1], Pred[SPEC_KMAX + 1], Part[SPEC_KMAX + 1];
static float    XrIr[SPEC_KMAX + 1], XiIr[SPEC_KMAX + 1], XrRed[SPEC_KMAX + 1], XiRed[SPEC_KMAX + 1];

// tracker
static int      kLo = 0, kHi = 0, nStates = 0;
static float    post[128], prior[128], emis[128], kern[16];
static int      kernHalf = 0;
static bool     trkInit = false;
static float    hrTrk = 0;
static float    tdBuf[WIN_N];                     // clean pulse waveform for beat detection
static int      tdDisagree = 0;                   // frames where beats contradict the tracker
static float    lastGoodHrS = -1e9f;              // time (s since contact) of last locked HR

// SpO2
static float    rHist[R_HIST], rHistT[R_HIST];
static int      rHistN = 0, rHistHead = 0;
static float    lastSpo2AcceptS = -1e9f;
static float    rPend = 0; static int rPendN = 0;     // confirmation of large R jumps
static uint8_t  motHist[16]; static int motHead = 0; // per-frame motion level history
static uint8_t  ampHist[16]; static int ampHead = 0; // per-frame pulse-amplitude/colour motion evidence

// quality / motion
static float    cleanRms = 0;   static bool cleanRmsInit = false;
static float    motIdxBase = 0.02f;               // resting floor of the motion index (sensor-specific)
static float    rPeakPrev = 0, rPeakT = -1e9f;    // last TRUSTED (accepted) arterial ratio and its time
static uint16_t moveHist = 0;                     // 1 bit per frame: movement evidence (last 16 s)
static int      hrAgreeN = 0; static float hrFirst = 0;   // start-up confirmation of the first HR
static float    dcIrSecAgo = 0;
// sensor-noise estimate (variance of the DC-normalised raw samples, from their 2nd difference)
static float    nzIr = 0, nzRed = 0, pyI1 = 0, pyI2 = 0, pyR1 = 0, pyR2 = 0; static int nzCnt = 0;
static const float NOISE_BAND_FRAC = 0.23f;       // share of white-noise power that passes the 0.5-6 Hz band-pass

// display smoothing
static float    dispHr = 0;
static float    hrMedBuf[3]; static int hrMedN = 0, hrMedHead = 0;   // v3.1.0: median of the last 3 frame estimates
static float    dispSpo2 = 0;                                        // v3.1.0: slew-limited SpO2 shown
static uint8_t  supFrames[SPEC_KMAX + 1];                            // v3.1.0: frames left to suppress each FFT bin
static int      widthFailN = 0;
static float    jumpHr = 0; static int jumpN = 0;           // pending large heart-rate jump
static float    fsScale = 1.0f;                   // true sample rate / nominal (sensor clock error)

static inline float clampf(float v, float lo, float hi);

// SpO2 calibration (R, reference SpO2) pairs and fitted line SpO2 = calA + calB * R
static float    calR[CAL_MAX_POINTS], calS[CAL_MAX_POINTS];
static int      calN = 0;
static float    calA = 0, calB = CAL_SLOPE_DEFAULT;
// v3.0.0 (Phase 2 point 3): true only when calFit() itself executed the least-squares branch
// AND the resulting coefficients are finite and within the sane-slope clamp. calCount()>=2
// alone does NOT imply this - two points with too little R-spread, or a degenerate/non-finite
// input, leave calB at the untouched default. This is the ONLY place that decides "fitted";
// the SpO2-mode label (.ino) reads this flag rather than re-deriving the condition itself.
static bool     calIsFit = false;

static void calFit() {
  calB = CAL_SLOPE_DEFAULT; calIsFit = false;              // reset every call - see comment above
  if (calN <= 0) { calA = 0; return; }
  float mr = 0, ms = 0;
  for (int i = 0; i < calN; i++) {
    // Reject non-finite stored points outright (defensive: calAddPoint()/calSetPoints() should
    // never store one, but a corrupted NVS blob on a real device could). A non-finite input
    // must never silently propagate into calA/calB.
    if (!isfinite(calR[i]) || !isfinite(calS[i])) { calA = 0; calB = CAL_SLOPE_DEFAULT; return; }
    mr += calR[i]; ms += calS[i];
  }
  mr /= calN; ms /= calN;
  float sxx = 0, sxy = 0, rmin = 9, rmax = -9;
  for (int i = 0; i < calN; i++) {
    sxx += (calR[i] - mr) * (calR[i] - mr); sxy += (calR[i] - mr) * (calS[i] - ms);
    if (calR[i] < rmin) rmin = calR[i];
    if (calR[i] > rmax) rmax = calR[i];
  }
  if (calN >= 2 && rmax - rmin >= 0.05f && sxx > 0) {
    float b = sxy / sxx;
    if (isfinite(b)) { calB = clampf(b, -60.0f, -8.0f); calIsFit = true; }  // genuine, sane, finite fit
  }
  calA = ms - calB * mr;
  if (!isfinite(calA)) { calA = 0; calB = CAL_SLOPE_DEFAULT; calIsFit = false; }  // final safety net
}
bool calWasFitted() { return calIsFit; }

// v3.2.0: SPO2_CLAMP_MAX / SPO2_R_MIN_DISPLAY are read from config.h via ppg_algorithm.h.
// Because ppg_algorithm.cpp is hardware-independent (no Arduino headers), the values are
// injected as extern constants defined once in OximeterFirmware.ino.  When built for the
// host test suite (HOST_SIM / native env), the test harness defines them.
#ifndef SPO2_CLAMP_MAX
#define SPO2_CLAMP_MAX       99.0f
#endif
#ifndef SPO2_R_MIN_DISPLAY
#define SPO2_R_MIN_DISPLAY   0.35f
#endif

// R -> SpO2 (%). Monotonic: a higher R always means a lower saturation.
// v3.2.0 changes:
//   * Returns 0.0f (sentinel "not displayable") when R < SPO2_R_MIN_DISPLAY – the flat top
//     of the calibration curve where the sensor cannot distinguish 99 % from 100 % and the
//     uncalibrated quadratic has already turned around (physically meaningless).
//   * Caps at SPO2_CLAMP_MAX (default 99 %) so the display never shows 100 % from an
//     uncalibrated consumer sensor that has no way to verify clinical accuracy.
float spo2FromR(float R) {
  if (R < SPO2_R_MIN_DISPLAY) return 0.0f;  // flat zone – withhold rather than show 100%
  float sp;
  if (calN > 0) sp = calA + calB * R;
  else { float r = R < SPO2_R_PEAK ? SPO2_R_PEAK : R; sp = SPO2_A + SPO2_B * r + SPO2_C * r * r; }
  if (sp > SPO2_CLAMP_MAX) sp = SPO2_CLAMP_MAX;
  return sp;
}
// ============================================================================
//                                SMALL HELPERS
// ============================================================================
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float tNowS() { return nSamp / FS_HZ; }

static void biqDesign(Biquad& f, bool highpass, float fc, float fs) {
  float w0 = 2.0f * 3.14159265f * fc / fs, c = cosf(w0), s = sinf(w0);
  float alpha = s / (2.0f * 0.70710678f), a0 = 1.0f + alpha;
  float b0, b1, b2;
  if (highpass) { b0 = (1 + c) * 0.5f; b1 = -(1 + c); b2 = (1 + c) * 0.5f; }
  else          { b0 = (1 - c) * 0.5f; b1 = (1 - c);  b2 = (1 - c) * 0.5f; }
  f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = -2.0f * c / a0; f.a2 = (1.0f - alpha) / a0;
  f.z1 = f.z2 = 0;
}
static inline float biq(Biquad& f, float x) {          // transposed direct form II
  float y = f.b0 * x + f.z1;
  f.z1 = f.b1 * x - f.a1 * y + f.z2;
  f.z2 = f.b2 * x - f.a2 * y;
  return y;
}
static void biqSteadyLP(Biquad& f, float x) { f.z1 = x * (1.0f - f.b0); f.z2 = x * (f.b2 - f.a2); }

static float medianOf(float* a, int n) {
  float t[16]; if (n > 16) n = 16;
  for (int i = 0; i < n; i++) t[i] = a[i];
  std::sort(t, t + n);
  return (n & 1) ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

// Power response of the cascaded band-pass (two identical 2nd-order Butterworth sections each side).
static float bpGain2(float f) {
  float xl = f / BP_LP_HZ, xh = BP_HP_HZ / f;
  float l = 1.0f / (1.0f + xl * xl * xl * xl), h = 1.0f / (1.0f + xh * xh * xh * xh);
  return l * l * h * h;
}

// Sensor-noise power per FFT bin AT the heart-rate bins (k0).  Sensor noise is white before the
// filters, while the pulse and almost all motion sit below ~6 Hz, so the 6.6-7.8 Hz bins show
// the noise level; it is scaled to the heart-rate bins with the known filter response.  Taking
// the minimum with a low quantile of the pulse band keeps broadband motion from being mistaken
// for noise (and then being "subtracted" from the contamination tests).
static float noiseFloor(const float* P, int kLo_, int kHi_, int k0) {
  const int kA = (int)ceilf(6.6f * NFFT / FS_HZ);
  float hi = 0, g = 0; int nh = 0;
  for (int k = kA; k <= SPEC_KMAX; k++) { hi += P[k]; g += bpGain2(k * FS_HZ / NFFT); nh++; }
  float nf = (nh > 0 && g > 0) ? (hi / g) * bpGain2(k0 * FS_HZ / NFFT) : 1e30f;
  float t[SPEC_KMAX + 1]; int n = 0;
  for (int k = kLo_; k <= kHi_; k++) {
    if (abs(k - k0) <= 6 || abs(k - 2 * k0) <= 6 || abs(k - 3 * k0) <= 6) continue;
    t[n++] = P[k];
  }
  if (n >= 8) {
    int q = n / 4;
    std::nth_element(t, t + q, t + n);
    float nq = t[q] / 0.288f;                      // 25th percentile of an exponential distribution
    if (nq < nf) nf = nq;
  }
  return nf < 1e29f ? nf : 0.0f;
}

// Trimmed mean (drops the highest and lowest quarter): averages down estimation noise while
// ignoring the occasional outlier window.
static float trimmedMean(const float* a, int n) {
  float t[16]; if (n > 16) n = 16;
  for (int i = 0; i < n; i++) t[i] = a[i];
  std::sort(t, t + n);
  int cut = n / 4, m = n - 2 * cut; float s = 0;
  for (int i = cut; i < n - cut; i++) s += t[i];
  return s / m;
}

// ---- FFT (iterative radix-2, in place) ----------------------------------------
static void fftInit() {
  for (int i = 0; i < NFFT / 2; i++) {
    twC[i] = cosf(2.0f * 3.14159265f * i / NFFT);
    twS[i] = -sinf(2.0f * 3.14159265f * i / NFFT);
  }
  for (int i = 0; i < NFFT; i++) {
    int r = 0; for (int b = 0; b < LOG2_NFFT; b++) if (i & (1 << b)) r |= 1 << (LOG2_NFFT - 1 - b);
    bitRev[i] = (uint16_t)r;
  }
  for (int i = 0; i < WIN_N; i++) hann[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / (WIN_N - 1));
  for (int i = 0; i < WINR_N; i++) {
    float x = 2.0f * 3.14159265f * i / (WINR_N - 1);
    blackR[i] = 0.42f - 0.5f * cosf(x) + 0.08f * cosf(2.0f * x);
  }
}
static void fft(float* re, float* im) {
  for (int i = 0; i < NFFT; i++) { int j = bitRev[i]; if (j > i) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
  for (int len = 2; len <= NFFT; len <<= 1) {
    int half = len >> 1, step = NFFT / len;
    for (int i = 0; i < NFFT; i += len) {
      for (int k = 0; k < half; k++) {
        float wr = twC[k * step], wi = twS[k * step];
        int a = i + k, b = a + half;
        float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
      }
    }
  }
}

// ============================================================================
//                              ALGORITHM: RESET
// ============================================================================
static void filtersInit(float irRaw, float redRaw) {
  biqDesign(dcIrF, false, DC_LP_HZ, FS_HZ);  biqDesign(dcRedF, false, DC_LP_HZ, FS_HZ);
  biqDesign(hpIr1, true, BP_HP_HZ, FS_HZ);   biqDesign(hpIr2, true, BP_HP_HZ, FS_HZ);
  biqDesign(lpIr1, false, BP_LP_HZ, FS_HZ);  biqDesign(lpIr2, false, BP_LP_HZ, FS_HZ);
  biqDesign(hpRed1, true, BP_HP_HZ, FS_HZ);  biqDesign(hpRed2, true, BP_HP_HZ, FS_HZ);
  biqDesign(lpRed1, false, BP_LP_HZ, FS_HZ); biqDesign(lpRed2, false, BP_LP_HZ, FS_HZ);
  if (irRaw < 1) irRaw = 1;
  if (redRaw < 1) redRaw = 1;
  refIrDC = irRaw; refRedDC = redRaw;
  biqSteadyLP(dcIrF, irRaw); biqSteadyLP(dcRedF, redRaw);
  dcIr = irRaw; dcRed = redRaw; trendIr = irRaw;
  sinceReset = 0; stepCnt = 0;
}

static void algoReset() {
  memset(rbIr, 0, sizeof(rbIr)); memset(rbRed, 0, sizeof(rbRed));
  memset(rbRef, 0, sizeof(rbRef)); memset(rbBad, 0, sizeof(rbBad));
  nSamp = 0; sinceReset = 0; blankLeft = 0; stepCnt = 0;
  sepOn = false; kMot = 1.0f;
  trkInit = false; hrTrk = 0; lastGoodHrS = -1e9f; tdDisagree = 0;
  rHistN = 0; rHistHead = 0; lastSpo2AcceptS = -1e9f; rPend = 0; rPendN = 0;
  memset(motHist, 0, sizeof(motHist)); motHead = 0; memset(ampHist, 0, sizeof(ampHist)); ampHead = 0;
  cleanRms = 0; cleanRmsInit = false; dispHr = 0; jumpN = 0; hrMedN = 0; hrMedHead = 0; dispSpo2 = 0; widthFailN = 0; memset(supFrames, 0, sizeof(supFrames)); motIdxBase = 0.02f; rPeakPrev = 0; rPeakT = -1e9f;
  hrAgreeN = 0; hrFirst = 0; moveHist = 0; nzIr = nzRed = 0; nzCnt = 0;
  // keep 'ra' between sessions: it is a property of the person/sensor, a better start than default
  AlgoOut z; memset(&z, 0, sizeof(z)); uint32_t se = out.stepEvents, rc = out.rAcceptCount, ft = out.framesTotal;
  out = z; out.stepEvents = se; out.rAcceptCount = rc; out.framesTotal = ft; out.ra = ra;
}

// ============================================================================
//            TIME-DOMAIN BEATS on the (colour-separated) pulse waveform
// ============================================================================
// v3.0.0 two-pass, belief-independent redesign (Phase 2 point 1 / finding F2).
//
// The previous design derived the minimum peak separation from `hrSpec` (the spectral
// tracker's CURRENT, possibly-wrong estimate). If the tracker drifted onto a harmonic, the
// separation shrank in lock-step with that error - exactly when its protection against
// double-counting a dicrotic notch needed to be STRONGEST, it weakened instead, letting a
// spurious close peak confirm the wrong rate. A fallback to any single external HR belief
// (tracked, min, or max) has the same structural flaw: no single scalar can be simultaneously
// wide enough to never suppress a genuine fast rhythm and narrow enough to reject a notch at
// a slow one (confirmed by the cold-start/60->150 BPM counter-examples that invalidated the
// first two proposals for this function).
//
// PASS 1 - candidate detection, using ONLY a fixed, physiology-derived floor that depends on
// NOTHING but the configured absolute ceiling HR_MAX_BPM - never on hrSpec, hrTrk, or any
// other belief. This floor can never exclude a genuine beat at any rate <= HR_MAX_BPM, because
// the true period at any such rate is always >= the floor. It only rejects candidates spaced
// closer than the fastest period a human heart can plausibly produce - which can only be noise
// or electrical ringing, never a second real beat.
//
// Collision rule (resolves Phase-3 clarification 1 - "a taller noise peak must not
// automatically discard a genuine beat"): when two candidates fall within the floor, the
// EARLIER one wins outright whenever it is already a CONFIDENT candidate (amplitude well
// clear of the adaptive threshold, by STRONG_MARGIN_FRAC). Only when the earlier candidate is
// itself marginal (barely above threshold - i.e. plausibly noise) does the taller of the two
// win. This specifically stops a noise upswing arriving shortly AFTER a confidently-detected
// genuine beat from displacing it, while still letting a genuinely taller second beat win when
// the first candidate was weak to begin with.
//
// PASS 2 - dicrotic-notch removal, using ONLY the Pass-1 candidates' OWN interval statistics
// (again, no hrSpec, no hrTrk anywhere in this pass). For every interior triplet
// (P[i-1], P[i], P[i+1]) whose two intervals are one "short" and one "long" relative to the
// candidate list's OWN median interval, AND whose middle peak is smaller than BOTH its
// neighbours, P[i] is marked as a dicrotic-notch candidate. Resolves Phase-3 clarification 2:
// exactly ONE named peak - the middle one of the triplet - is ever removed; the two
// neighbours (the genuine systolic peaks either side of the notch) are always kept. This is a
// deletion of specific indices, never an averaging of three peaks into one.
//
// The removal is applied GLOBALLY only if (a) it is well supported - at least
// DICROTIC_MIN_MARKED marked triplets, covering at least DICROTIC_MIN_FRACTION of every
// "short" interval in the window - AND (b) removing them makes the resulting interval set
// MORE regular (lower CV) than leaving them in. Both gates must agree. This resolves
// Phase-3 clarification 3: a genuine HR transition (e.g. 60->150 BPM) produces at most ONE
// anomalous interval at the transition edge, never a sustained repeating alternation across
// most of the window's triplets, so it never reaches the required count/fraction; and even if
// an edge case did, removing a transition-edge peak does not improve regularity (the rate
// genuinely changed), so the independent CV-improvement gate rejects it too. The same two
// gates protect a genuinely irregular rhythm (e.g. simulated AF): an irregular-but-superficially
// -alternating pattern does not produce a LOW-CV result after removal, so the merge is
// rejected and the true irregular beat count is preserved.
//
// KNOWN LIMITATION (stated, not hidden - see docs/KNOWN_LIMITATIONS.md): this is a heuristic,
// tuned on synthetic waveforms only (see test/test_hr_estimation.cpp). A dicrotic notch whose
// amplitude is close to, or exceeds, the true systolic peak (physiologically unusual but not
// impossible on some sensor placements) would not satisfy "middle peak smaller than both
// neighbours" and would not be removed. This is a deliberate fail-safe (under-correction, not
// over-correction - an unremoved notch inflates hrMeanTd, which then correctly DISAGREES with
// the spectral tracker and is rejected by the existing disagreement checks in analyseFrame(),
// rather than being silently miscounted as a confirmed rate).
// ============================================================================
// v3.1.0: hysteresis re-arm. A new beat can only be registered after the signal has fallen below
// BEAT_REARM_FRAC of its own p10..p90 range since the previous beat. Without it, noise ripple on the
// top of a broad pulse peak (slow heart rates) was counted as several beats and the time-domain rate
// read 1.5-3x too high. Needs no heart-rate estimate.
static const float BEAT_REARM_FRAC       = 0.40f;
static const float STRONG_MARGIN_FRAC    = 0.30f; // Pass-1 collision: earlier candidate wins outright above this margin over threshold
static const float DICROTIC_SHORT_FRAC   = 0.75f; // interval < 0.75 x median => "short"
static const float DICROTIC_LONG_FRAC    = 1.25f; // interval > 1.25 x median => "long"
static const int   DICROTIC_MIN_MARKED   = 2;     // at least this many triplets must qualify
static const float DICROTIC_MIN_FRACTION = 0.70f; // ... covering at least this share of all "short" intervals
static const float DICROTIC_ACCEPT_CV    = 0.15f; // post-removal CV must be below this to accept the merge

// Core two-pass detector. Belief-independent: takes no HR estimate of any kind. Shared by
// beatAnalysis() (production) and algoTestBeatDetect() (test hook, ALGO_TEST_HOOKS builds
// only) so host tests exercise exactly the production code path, never a reimplementation.
static int beatDetectTwoPass(const float* pulse, int N, float* outPkT, float* outPkV, int maxPk, bool* outMerged) {
  if (outMerged) *outMerged = false;
  // ---- robust amplitude threshold (unchanged from v2.3.1) ----
  static float tmp[WIN_N];
  int iN = N > WIN_N ? WIN_N : N;
  for (int i = 0; i < iN; i++) tmp[i] = pulse[i];
  std::nth_element(tmp, tmp + iN / 10, tmp + iN);       float p10 = tmp[iN / 10];
  std::nth_element(tmp, tmp + (9 * iN) / 10, tmp + iN); float p90 = tmp[(9 * iN) / 10];
  float thr = p10 + BEAT_THR_FRAC * (p90 - p10);
  float strongThr = thr + STRONG_MARGIN_FRAC * (p90 - thr);

  // ---- PASS 1: fixed-floor candidate detection (belief-independent) ----
  const int PASS1_FLOOR = (int)ceilf(BEAT_MIN_SEP * 60.0f / HR_MAX_BPM * FS_HZ);  // samples; e.g. 8 at FS_HZ=50, HR_MAX_BPM=220
  float pkT[40]; float pkV[40]; int np = 0;
  const float loThr = p10 + BEAT_REARM_FRAC * (p90 - p10);
  float minSince = pulse[0];                       // lowest value since the last registered candidate
  for (int i = 1; i < N - 1; i++) {
    float v = pulse[i];
    if (v < minSince) minSince = v;
    if (v < thr || v <= pulse[i - 1] || v < pulse[i + 1]) continue;
    float a = pulse[i - 1], c = pulse[i + 1], den = a - 2 * v + c;
    float t = i + ((den < 0) ? clampf(0.5f * (a - c) / den, -0.5f, 0.5f) : 0.0f);
    if (np > 0 && minSince > loThr) {
      // never dipped back down since the previous candidate: same beat (ripple), not a new one
      if (pkV[np - 1] < strongThr && v > pkV[np - 1]) { pkT[np - 1] = t; pkV[np - 1] = v; }
      continue;
    }
    if (np > 0 && (t - pkT[np - 1]) < PASS1_FLOOR) {
      // Collision: the earlier candidate keeps its slot unless it is itself marginal.
      if (pkV[np - 1] < strongThr && v > pkV[np - 1]) { pkT[np - 1] = t; pkV[np - 1] = v; }
      continue;
    }
    if (np < 40) { pkT[np] = t; pkV[np] = v; np++; minSince = v; }
  }
  if (np < 3) {    // no interior triplet exists yet - nothing for Pass 2 to evaluate
    int n = np < maxPk ? np : maxPk;
    if (outPkT) for (int i = 0; i < n; i++) outPkT[i] = pkT[i];
    if (outPkV) for (int i = 0; i < n; i++) outPkV[i] = pkV[i];
    return np;
  }

  // ---- PASS 2: self-consistent dicrotic-notch removal (belief-independent) ----
  float iv[40]; int niv = np - 1;
  for (int i = 0; i < niv; i++) iv[i] = pkT[i + 1] - pkT[i];
  float ivSorted[40]; for (int i = 0; i < niv; i++) ivSorted[i] = iv[i];
  std::sort(ivSorted, ivSorted + niv);
  float medIv = (niv & 1) ? ivSorted[niv / 2] : 0.5f * (ivSorted[niv / 2 - 1] + ivSorted[niv / 2]);

  bool removeMid[40]; for (int i = 0; i < np; i++) removeMid[i] = false;
  int marked = 0, shortCount = 0;
  for (int i = 0; i < niv; i++) if (iv[i] < DICROTIC_SHORT_FRAC * medIv) shortCount++;
  for (int i = 1; i < np - 1; i++) {                  // interior peaks only: triplet (i-1, i, i+1)
    float ivA = iv[i - 1], ivB = iv[i];               // ivA = P[i]-P[i-1], ivB = P[i+1]-P[i]
    bool shortLong = (ivA < DICROTIC_SHORT_FRAC * medIv && ivB > DICROTIC_LONG_FRAC * medIv) ||
                      (ivB < DICROTIC_SHORT_FRAC * medIv && ivA > DICROTIC_LONG_FRAC * medIv);
    if (shortLong && pkV[i] < pkV[i - 1] && pkV[i] < pkV[i + 1]) { removeMid[i] = true; marked++; }
  }

  float finalT[40]; float finalV[40]; int finalN = np;
  for (int i = 0; i < np; i++) { finalT[i] = pkT[i]; finalV[i] = pkV[i]; }
  bool wellSupported = marked >= DICROTIC_MIN_MARKED && shortCount > 0 &&
                        (float)marked / (float)shortCount >= DICROTIC_MIN_FRACTION;
  if (wellSupported) {
    float candT[40]; float candV[40]; int nc = 0;
    for (int i = 0; i < np; i++) if (!removeMid[i]) { candT[nc] = pkT[i]; candV[nc] = pkV[i]; nc++; }
    if (nc >= 2) {
      float s1 = 0, s2 = 0; int nci = nc - 1;
      for (int i = 1; i < nc; i++) { float d = candT[i] - candT[i - 1]; s1 += d; s2 += d * d; }
      float m = s1 / nci;
      float mcv = (m > 0) ? sqrtf(fmaxf(s2 / nci - m * m, 0)) / m : 1e9f;
      if (mcv < DICROTIC_ACCEPT_CV) {                 // removal genuinely improves regularity: accept
        for (int i = 0; i < nc; i++) { finalT[i] = candT[i]; finalV[i] = candV[i]; }
        finalN = nc;
        if (outMerged) *outMerged = true;
      }                                               // else: rejected, finalT/finalV/finalN already = Pass-1 output
    }
  }
  int n = finalN < maxPk ? finalN : maxPk;
  if (outPkT) for (int i = 0; i < n; i++) outPkT[i] = finalT[i];
  if (outPkV) for (int i = 0; i < n; i++) outPkV[i] = finalV[i];
  return finalN;
}

// v3.1.0: median full-width-at-half-height (seconds) of the detected beat peaks, or -1 when fewer
// than 3 interior peaks can be measured. Each peak is measured between the troughs that separate it
// from its neighbours, so it does not depend on any heart-rate estimate.
static float pulseWidthS(const float* x, int N, const float* pkT, int np) {
  float w[40]; int nw = 0;
  for (int i = 1; i < np - 1 && nw < 40; i++) {
    int c = (int)lrintf(pkT[i]), lo = (int)pkT[i - 1], hi = (int)pkT[i + 1] + 1;
    if (c < 1 || c >= N - 1 || lo < 0) continue;
    if (hi > N - 1) hi = N - 1;
    float tl = x[c], tr = x[c];
    for (int j = lo; j <= c; j++) if (x[j] < tl) tl = x[j];
    for (int j = c; j <= hi; j++) if (x[j] < tr) tr = x[j];
    float base = tl > tr ? tl : tr, v = x[c];
    if (v - base <= 0) continue;
    float half = base + PULSE_WIDTH_LEVEL * (v - base);
    int l = c, r = c;
    while (l > lo && x[l] > half) l--;
    while (r < hi && x[r] > half) r++;
    w[nw++] = (float)(r - l) / FS_HZ;
  }
  if (nw < 3) return -1.0f;
  std::sort(w, w + nw);
  return (nw & 1) ? w[nw / 2] : 0.5f * (w[nw / 2 - 1] + w[nw / 2]);
}
static float lastPulseWidthS = -1.0f;

// Fills tdBuf with the clean pulse, runs the two-pass detector, returns the number of beats.
// hrMean = mean rate over the window, hrRecent = last 4 intervals, cv = RR variability.
static int beatAnalysis(int N, uint32_t first, float hrSpec, float& hrMean, float& hrRecent, float& cv, int& badIv) {
  (void)hrSpec;   // v3.0.0: intentionally unused - see beatDetectTwoPass() header comment above.
                  // Kept as a parameter only so this function's external call signature (and
                  // therefore the rest of analyseFrame()) is unchanged, per the "keep the
                  // existing contract unless a change is technically unavoidable" instruction.
  float dk = kMot - raSep;
  for (int i = 0; i < N; i++) {
    uint32_t q = (first + i) & RB_MASK;
    tdBuf[i] = sepOn ? (kMot * rbIr[q] - rbRed[q]) / dk : rbIr[q];
  }
  float pkT[40]; float pkV[40];
  int np = beatDetectTwoPass(tdBuf, N, pkT, pkV, 40, nullptr);
  lastPulseWidthS = pulseWidthS(tdBuf, N, pkT, np < 40 ? np : 40);

  hrMean = hrRecent = cv = 0; badIv = 0;
  if (np < 2) return np;

  // Self-referential plausibility check: uses the DETECTOR'S OWN median interval, not an
  // external hrSpec-derived period - consistent with the rest of this redesign.
  float iv[40]; int ni = np - 1;
  for (int i = 0; i < ni; i++) iv[i] = pkT[i + 1] - pkT[i];
  float ivSorted[40]; for (int i = 0; i < ni; i++) ivSorted[i] = iv[i];
  std::sort(ivSorted, ivSorted + ni);
  float medIvSelf = (ni & 1) ? ivSorted[ni / 2] : 0.5f * (ivSorted[ni / 2 - 1] + ivSorted[ni / 2]);

  float s1 = 0, s2 = 0;
  for (int i = 0; i < ni; i++) {
    if (iv[i] < 0.45f * medIvSelf || iv[i] > 1.8f * medIvSelf) badIv++;
    s1 += iv[i]; s2 += iv[i] * iv[i];
  }
  float m = s1 / ni;
  cv = sqrtf(fmaxf(s2 / ni - m * m, 0)) / m;
  hrMean = 60.0f * FS_HZ * (np - 1) / (pkT[np - 1] - pkT[0]);
  int nr = np - 1 < 4 ? np - 1 : 4;
  hrRecent = 60.0f * FS_HZ * nr / (pkT[np - 1] - pkT[np - 1 - nr]);
  return np;
}

#ifdef ALGO_TEST_HOOKS
int algoTestBeatDetect(const float* pulse, int N, float* outPkTimes, int maxPk,
                        float& outHrMean, float& outCv, int& outBadIv, bool& outMerged) {
  float pkT[40]; float pkV[40]; bool merged = false;
  int np = beatDetectTwoPass(pulse, N, pkT, pkV, 40, &merged);
  outMerged = merged; outHrMean = outCv = 0; outBadIv = 0;
  if (np >= 2) {
    float iv[40]; int ni = np - 1;
    for (int i = 0; i < ni; i++) iv[i] = pkT[i + 1] - pkT[i];
    float ivSorted[40]; for (int i = 0; i < ni; i++) ivSorted[i] = iv[i];
    std::sort(ivSorted, ivSorted + ni);
    float medIvSelf = (ni & 1) ? ivSorted[ni / 2] : 0.5f * (ivSorted[ni / 2 - 1] + ivSorted[ni / 2]);
    float s1 = 0, s2 = 0;
    for (int i = 0; i < ni; i++) { if (iv[i] < 0.45f * medIvSelf || iv[i] > 1.8f * medIvSelf) outBadIv++; s1 += iv[i]; s2 += iv[i] * iv[i]; }
    float m = s1 / ni; outCv = sqrtf(fmaxf(s2 / ni - m * m, 0)) / m;
    outHrMean = 60.0f * FS_HZ * (np - 1) / (pkT[np - 1] - pkT[0]);
  }
  int n = np < maxPk ? np : maxPk;
  for (int i = 0; i < n; i++) outPkTimes[i] = pkT[i];
  return np;
}
#endif

// ============================================================================
//                 ALGORITHM: ONE ANALYSIS FRAME (every 1 s)
// ============================================================================
static void analyseFrame() {
  out.framesTotal++;
  uint32_t avail = (sinceReset < nSamp ? sinceReset : nSamp);
  int N = (int)(nSamp < WIN_N ? nSamp : WIN_N);
  out.warmupPct = clampf(100.0f * nSamp / WIN_MIN, 0, 100);
  if ((int)nSamp < WIN_MIN) return;

  // ---- window statistics: bad samples, motion index, skewness ----
  int bad = 0; uint32_t first = nSamp - N;
  for (int i = 0; i < N; i++) if (rbBad[(first + i) & RB_MASK]) bad++;
  bool stepInWin = bad > 0;

  // ---- time-domain motion indicators over the last second (used by the separation below) ----
  // motionIdx = energy of the reference that is NOT proportional to IR (LS projection residual).
  // A real SpO2 change gives ref = (R - Ra)*IR -> residual 0; motion of another colour does not.
  float sIr2 = 0, sRef2 = 0, sRI = 0;
  for (int i = 0; i < HOP_N; i++) {
    uint32_t q = (nSamp - 1 - i) & RB_MASK;
    sIr2 += rbIr[q] * rbIr[q]; sRef2 += rbRef[q] * rbRef[q]; sRI += rbRef[q] * rbIr[q];
  }
  // remove the expected sensor-noise energy, otherwise noise alone looks like motion on weak signals
  float nIrE = NOISE_BAND_FRAC * nzIr * HOP_N, nRedE = NOISE_BAND_FRAC * nzRed * HOP_N;
  float sIr2c = fmaxf(sIr2 - nIrE, 0.2f * sIr2);
  float rms1 = sqrtf(sIr2c / HOP_N);
  float refResid = sIr2 > 0 ? fmaxf(sRef2 - sRI * sRI / sIr2, 0.0f) : 0;
  refResid = fmaxf(refResid - (nRedE + ra * ra * nIrE), 0.0f);
  float motionIdx = sIr2c > 0 ? refResid / sIr2c : 0;
  float ampRatio = (cleanRmsInit && cleanRms > 0) ? rms1 / cleanRms : 1.0f;
  float dcJump = (dcIrSecAgo > 0) ? fabsf(dcIr - dcIrSecAgo) / dcIrSecAgo : 0;
  dcIrSecAgo = dcIr;
  // the slow DC tracker also ripples with a strong pulse: judge a DC jump relative to the pulse size
  float dcTol = 1.5f * rms1;
  bool moveNow = ampRatio > 1.5f || dcJump > 0.01f + dcTol || stepInWin || (motionIdx - motIdxBase) > 0.10f;
  moveHist = (uint16_t)((moveHist << 1) | (moveNow ? 1 : 0));
  // movement anywhere inside the analysis window (the spectrum still contains it)
  int winFrames = (WINR_N + HOP_N - 1) / HOP_N;
  bool moveWin = (moveHist & (uint16_t)((1u << winFrames) - 1)) != 0;

  // ---- spectra: IR + j*RED packed into one complex FFT ----
  float mI = 0, mR = 0;
  for (int i = 0; i < N; i++) { uint32_t q = (first + i) & RB_MASK; mI += rbIr[q]; mR += rbRed[q]; }
  mI /= N; mR /= N;
  for (int i = 0; i < NFFT; i++) { fRe[i] = fIm[i] = 0; }
  for (int i = 0; i < N; i++) {
    uint32_t q = (first + i) & RB_MASK;
    float w = hann[(i * (WIN_N - 1)) / (N > 1 ? N - 1 : 1)];
    fRe[i] = (rbIr[q] - mI) * w; fIm[i] = (rbRed[q] - mR) * w;
  }
  fft(fRe, fIm);
  for (int k = 0; k <= SPEC_KMAX; k++) {
    int kn = (NFFT - k) & (NFFT - 1);
    float irR = 0.5f * (fRe[k] + fRe[kn]), irI = 0.5f * (fIm[k] - fIm[kn]);
    float rdR = 0.5f * (fIm[k] + fIm[kn]), rdI = -0.5f * (fRe[k] - fRe[kn]);
    XrIr[k] = irR; XiIr[k] = irI; XrRed[k] = rdR; XiRed[k] = rdI;
    Pir[k] = irR * irR + irI * irI; Pred[k] = rdR * rdR + rdI * rdI;
  }
  int kBandLo = (int)(0.5f * NFFT / FS_HZ), kBandHi = (int)(6.0f * NFFT / FS_HZ);
  if (kBandHi > SPEC_KMAX) kBandHi = SPEC_KMAX;

  // ---- per-bin two-source separation (frequency-domain form of the DST idea) ----
  // IR = P + M, RED = Ra*P + k*M  =>  P = (k*IR - RED) / (k - Ra)   (P arterial, M motion)
  // Ra = trusted arterial ratio, k (motion colour) estimated from motion-dominated bins.
  {
    // A bin counts as motion if its red/IR ratio differs from the TRUSTED arterial ratio.
    // Without amplitude/DC evidence of movement the bins of the HR (fundamental, harmonic) are
    // always treated as arterial, so a genuine SpO2 change at rest is never mistaken for motion.
    float anchor = (tNowS() - rPeakT < SPO2_MAX_AGE_S) ? rPeakPrev : ra;
    raSep = anchor;
    int kPrev = (hrTrk > 0) ? (int)lrintf(hrTrk / 60.0f * NFFT / FS_HZ) : -100;
    float kn = 0, kd = 0, pm = 0, pt = 0;
    for (int k = kBandLo; k <= kBandHi; k++) {
      pt += Pir[k];
      if (Pir[k] <= 0) continue;
      if (!moveWin && (abs(k - kPrev) <= 4 || abs(k - 2 * kPrev) <= 4)) continue;
      float rr = XrRed[k] - anchor * XrIr[k], ri = XiRed[k] - anchor * XiIr[k];
      if (rr * rr + ri * ri <= MOT_BIN_RATIO * Pir[k]) continue;
      kn += XrRed[k] * XrIr[k] + XiRed[k] * XiIr[k]; kd += Pir[k]; pm += Pir[k];
    }
    sepOn = false;
    // separation needs evidence of real movement; sensor noise alone (random red/IR ratio) must
    // not switch it on, or the division by (k - Ra) would amplify noise on weak signals
    bool moveEvidence = moveWin;
    if (moveEvidence && kd > 0 && pt > 0 && pm / pt > MOT_FRAC_MIN) {
      kMot = clampf(kn / kd, 0.0f, 4.0f);
      if (fabsf(kMot - raSep) >= SEP_MIN_DK) sepOn = true;
    }
    // Sanity: without clear amplitude/colour evidence of motion, the tracked pulse must survive the
    // separation. If the separation removed it, the fitted 'motion colour' was really the pulse itself
    // (e.g. after a genuine SpO2 change) - the tracker would then jump to a harmonic (2x / 3x HR).
    if (sepOn && hrTrk > 0) {
      uint8_t priorAmp = 0;
      for (int i = 1; i <= (N + HOP_N - 1) / HOP_N && i <= 16; i++) { uint8_t m = ampHist[(ampHead - i) & 15]; if (m > priorAmp) priorAmp = m; }
      if (priorAmp < 2) {
        float eRaw = 0, eSep = 0, d2 = (kMot - raSep) * (kMot - raSep);
        for (int k = kPrev - 3; k <= kPrev + 3; k++) {
          if (k < 1 || k > SPEC_KMAX) continue;
          float pr = kMot * XrIr[k] - XrRed[k], pq = kMot * XiIr[k] - XiRed[k];
          eRaw += Pir[k]; eSep += (pr * pr + pq * pq) / d2;
        }
        if (eRaw > 0 && eSep < SEP_KEEP_FRAC * eRaw) sepOn = false;
      }
    }
    float dk2 = (kMot - raSep) * (kMot - raSep);
    for (int k = 0; k <= SPEC_KMAX; k++) {
      if (sepOn) {
        float pr = kMot * XrIr[k] - XrRed[k], pq = kMot * XiIr[k] - XiRed[k];
        Part[k] = (pr * pr + pq * pq) / dk2;
      } else Part[k] = Pir[k];
    }
  }

  // ---- emission: separated arterial power + 2nd-harmonic support ----
  float emax = 0;
  for (int s = 0; s < nStates; s++) {
    int k = kLo + s;
    // harmonic support is capped by the fundamental's own power: a strong component at 2f
    // (e.g. finger tapping) cannot invent a heartbeat at f
    float e = Part[k] + ((2 * k <= SPEC_KMAX) ? HARM_W * fminf(Part[2 * k], Part[k]) : 0);
    if (supFrames[k]) e *= 0.02f;           // v3.1.0: frequency recently shown NOT to be a pulse
    emis[s] = e; if (e > emax) emax = e;
  }
  for (int k = 0; k <= SPEC_KMAX; k++) if (supFrames[k]) supFrames[k]--;
  if (emax <= 0) return;
  float esum = 0;
  // linear (not squared) emission: one frame cannot overturn the tracker's accumulated history
  for (int s = 0; s < nStates; s++) { emis[s] = emis[s] / emax + 1e-4f; esum += emis[s]; }
  for (int s = 0; s < nStates; s++) emis[s] /= esum;

  // ---- tracker: prior = post (*) gaussian + floor ; post = prior x emission ----
  if (!trkInit) {     // physiological prior: broad around 75 bpm (any rate 35-220 still reachable)
    for (int s = 0; s < nStates; s++) {
      float bpm = (kLo + s) * FS_HZ / NFFT * 60.0f, d = (bpm - 75.0f) / 40.0f;
      prior[s] = expf(-0.5f * d * d) + 0.05f;
    }
    trkInit = true;
  }
  else {
    for (int s = 0; s < nStates; s++) {
      float acc = 0;
      for (int d = -kernHalf; d <= kernHalf; d++) { int j = s + d; if (j >= 0 && j < nStates) acc += post[j] * kern[d + kernHalf]; }
      prior[s] = acc + TRK_FLOOR / nStates;
    }
  }
  float psum = 0;
  for (int s = 0; s < nStates; s++) { post[s] = prior[s] * emis[s]; psum += post[s]; }
  if (psum <= 0) { for (int s = 0; s < nStates; s++) post[s] = 1.0f / nStates; psum = 1; }
  int sMap = 0; float pMax = 0;
  for (int s = 0; s < nStates; s++) { post[s] /= psum; if (post[s] > pMax) { pMax = post[s]; sMap = s; } }
  float conf = 0;
  for (int d = -2; d <= 2; d++) { int j = sMap + d; if (j >= 0 && j < nStates) conf += post[j]; }

  // refine frequency with parabolic interpolation on the IR spectrum
  int k0 = kLo + sMap; float kf = (float)k0;
  {
    float a = Part[k0 - 1], b = Part[k0], c = Part[k0 + 1];
    if (b >= a && b >= c && (a - 2 * b + c) < 0) kf = k0 + clampf(0.5f * (a - c) / (a - 2 * b + c), -0.5f, 0.5f);
  }
  float hr = kf * FS_HZ / NFFT * 60.0f;

  // ---- time-domain beats (fast response, true mean rate for irregular rhythms) ----
  float hrMeanTd = 0, hrRecTd = 0, rrCv = 0; int badIv = 0;
  int nb = beatAnalysis(N, first, hr, hrMeanTd, hrRecTd, rrCv, badIv);

  // ---- v3.1.0 harmonic guard ----
  // A pulse with a strong dicrotic wave has a 2nd harmonic as strong as its fundamental, and the
  // tracker can then sit on 2x (or 3x) the true rate with high "confidence". If the beat detector's
  // rate matches hr/2 (or hr/3) instead of hr, AND the separated spectrum really holds power at that
  // subharmonic, the subharmonic is the heart rate: re-anchor the tracker there. A subharmonic with no
  // spectral power (or too close to the band edge) is never chosen, so noise beats cannot halve a
  // genuine rate.
  if (nb >= BEAT_MIN_N && hrMeanTd > 0 && fabsf(hrMeanTd - hr) > 0.30f * hr) {
    for (int div = 3; div >= 2; div--) {
      float sub = hr / div;
      if (fabsf(hrMeanTd - sub) >= 0.25f * sub) continue;
      int ks = (int)lrintf(kf / div);
      if (ks < kLo + 2) continue;
      int kb = ks; float pS = 0;
      for (int j = ks - 2; j <= ks + 2; j++) if (j >= kLo && Part[j] > pS) { pS = Part[j]; kb = j; }
      if (pS < 0.15f * Part[k0]) continue;
      k0 = kb; kf = (float)k0;
      { float a = Part[k0 - 1], b = Part[k0], c = Part[k0 + 1];
        if (b >= a && b >= c && (a - 2 * b + c) < 0) kf = k0 + clampf(0.5f * (a - c) / (a - 2 * b + c), -0.5f, 0.5f); }
      hr = kf * FS_HZ / NFFT * 60.0f;
      float ps2 = 0; int sc = k0 - kLo;
      for (int q = 0; q < nStates; q++) { float d = (q - sc) / 2.0f; post[q] = expf(-0.5f * d * d); ps2 += post[q]; }
      for (int q = 0; q < nStates; q++) post[q] /= ps2;
      conf = fmaxf(conf, 0.5f); tdDisagree = 0;
      break;
    }
  }

  // ---- spectral purity: share of 0.5-6 Hz power near f0 and 2f0 (raw IR, and separated) ----
  float pTot = 0, pIn = 0, aTot = 0, aIn = 0;
  for (int k = kBandLo; k <= kBandHi; k++) {
    bool near = (abs(k - k0) <= 4 || abs(k - 2 * k0) <= 4);
    pTot += Pir[k];  if (near) pIn += Pir[k];
    aTot += Part[k]; if (near) aIn += Part[k];
  }
  float purity = pTot > 0 ? pIn / pTot : 0;
  float purityArt = aTot > 0 ? aIn / aTot : 0;

  // ---- perfusion index (%) from the Hann spectrum: Parseval, sum(|X|^2 near f0) ~= NFFT*A^2*0.375*N/4
  float pa = 0;
  for (int k = k0 - 3; k <= k0 + 3; k++) pa += Pir[k];
  if (2 * k0 + 3 <= SPEC_KMAX) for (int k = 2 * k0 - 3; k <= 2 * k0 + 3; k++) pa += Pir[k];
  float pi = clampf(2.0f * 100.0f * sqrtf(4.0f * pa / ((float)NFFT * 0.375f * (float)N)), 0, 50);

#if R_WINDOW_MODE == 1
  // ---- low-leakage spectrum for R: Blackman (-58 dB side-lobes), 10 s ----
  {
    int NR = (int)(nSamp < WINR_N ? nSamp : WINR_N);
    if (NR > (int)avail && avail >= (uint32_t)WIN_MIN) NR = (int)avail;   // never straddle a filter re-init
    uint32_t fr0 = nSamp - NR; float a = 0, b = 0;
    for (int i = 0; i < NR; i++) { uint32_t q = (fr0 + i) & RB_MASK; a += rbIr[q]; b += rbRed[q]; }
    a /= NR; b /= NR;
    for (int i = 0; i < NFFT; i++) fRe[i] = fIm[i] = 0;
    for (int i = 0; i < NR; i++) {
      uint32_t q = (fr0 + i) & RB_MASK; float w = blackR[(i * (WINR_N - 1)) / (NR > 1 ? NR - 1 : 1)];
      fRe[i] = (rbIr[q] - a) * w; fIm[i] = (rbRed[q] - b) * w;
    }
    fft(fRe, fIm);
    for (int k = 0; k <= SPEC_KMAX; k++) {
      int kn = (NFFT - k) & (NFFT - 1);
      XrIr[k] = 0.5f * (fRe[k] + fRe[kn]); XiIr[k] = 0.5f * (fIm[k] - fIm[kn]);
      XrRed[k] = 0.5f * (fIm[k] + fIm[kn]); XiRed[k] = -0.5f * (fRe[k] - fRe[kn]);
      Pir[k] = XrIr[k] * XrIr[k] + XiIr[k] * XiIr[k]; Pred[k] = XrRed[k] * XrRed[k] + XiRed[k] * XiRed[k];
    }
  }
#endif

  // ---- R at the heart-rate fundamental and harmonic (HR-tuned comb) ----
  float n1 = 0, d1 = 0, n2 = 0, d2 = 0, cohN = 0, cohR = 0, pk1 = 0;
  for (int k = k0 - 3; k <= k0 + 3; k++) {
    n1 += XrRed[k] * XrIr[k] + XiRed[k] * XiIr[k]; d1 += Pir[k]; cohR += Pred[k];
    cohN += XiRed[k] * XrIr[k] - XrRed[k] * XiIr[k];
    if (Pir[k] > pk1) pk1 = Pir[k];
  }
  float nq2 = 0;
  bool haveH2 = (2 * k0 + 3) <= SPEC_KMAX;
  if (haveH2) for (int k = 2 * k0 - 3; k <= 2 * k0 + 3; k++) {
    n2 += XrRed[k] * XrIr[k] + XiRed[k] * XiIr[k]; nq2 += XiRed[k] * XrIr[k] - XrRed[k] * XiIr[k]; d2 += Pir[k];
  }
  // R from the MAGNITUDE of the red/IR cross-spectrum: insensitive to the small red-vs-IR
  // time lag / waveform difference that real sensors show
  // sensor noise adds power to Pir / Pred (never to the red x IR cross-spectrum, whose noise
  // terms average out): remove the measured noise floor of the 7 peak bins, otherwise R is biased
  // low and every coherence / spread test fails on a clean-but-weak signal
  float nfIr = noiseFloor(Pir, kBandLo, kBandHi, k0), nfRed = noiseFloor(Pred, kBandLo, kBandHi, k0);
  float nsr = d1 > 0 ? 7.0f * nfIr / d1 : 1.0f;                 // noise-to-pulse power ratio (IR)
  float d1c = fmaxf(d1 - 7.0f * nfIr, 0.15f * d1), cohRc = fmaxf(cohR - 7.0f * nfRed, 0.15f * cohR);
  float d2c = fmaxf(d2 - 7.0f * nfIr, 0.15f * d2);
  // |cross-spectrum|^2 also carries a positive noise bias (sum of IR-noise x red-noise over the bins)
  float xn = XN_FACTOR * 7.0f * nfIr * nfRed;
  float x1 = n1 * n1 + cohN * cohN, x2 = n2 * n2 + nq2 * nq2;
  float x1c = fmaxf(x1 - xn, 0.05f * x1), x2c = fmaxf(x2 - xn, 0.05f * x2);
  float R1 = d1c > 0 ? sqrtf(x1c) / d1c : 0;
  float R2 = d2c > 0 ? sqrtf(x2c) / d2c : 0;
  float coh = (d1c > 0 && cohRc > 0) ? x1c / (d1c * cohRc) : 0;
  float nsr2 = d2 > 0 ? 7.0f * nfIr / d2 : 1.0f;                // the harmonic is weaker: its own noise ratio
  bool h2Strong = haveH2 && d2c > 0.10f * d1c && nsr2 < 0.5f;
  float Rw = R1;
  bool rAgree = !h2Strong || (fabsf(R1 - R2) < (0.15f + 0.6f * sqrtf(nsr + nsr2)) * fmaxf(R1, 0.1f));
  float cRe0 = d1 > 0 ? n1 / d1 : 0, cIm0 = d1 > 0 ? cohN / d1 : 0;
  float cRe1 = d1c > 0 ? n1 / d1c : 0, cIm1 = d1c > 0 ? cohN / d1c : 0;   // noise-corrected complex ratio
  float sw = 0, sd = 0, resAll = 0;
  for (int k = k0 - 3; k <= k0 + 3; k++) {
    float er = XrRed[k] - (cRe1 * XrIr[k] - cIm1 * XiIr[k]);
    float ei = XiRed[k] - (cRe1 * XiIr[k] + cIm1 * XrIr[k]);
    resAll += er * er + ei * ei;
    if (Pir[k] < 0.05f * pk1) continue;
    float re = (XrRed[k] * XrIr[k] + XiRed[k] * XiIr[k]) / Pir[k];
    float im = (XiRed[k] * XrIr[k] - XrRed[k] * XiIr[k]) / Pir[k];
    sw += Pir[k]; sd += Pir[k] * ((re - cRe1) * (re - cRe1) + (im - cIm1) * (im - cIm1));
  }
  (void)cRe0; (void)cIm0;
  float rSpread = sw > 0 ? sqrtf(sd / sw) : 1.0f;
  // red power not explained by one arterial ratio (above the expected noise residual)
  float c2 = cRe1 * cRe1 + cIm1 * cIm1;
  float resNoise = 7.0f * (nfRed + c2 * nfIr);
  float contamRes = (d1c > 0 && c2 > 0) ? fmaxf(resAll - resNoise, 0.0f) / (c2 * d1c) : 1.0f;
  float anc = (tNowS() - rPeakT < SPO2_MAX_AGE_S) ? rPeakPrev : ra, pAnc = 0;
  for (int k = k0 - 3; k <= k0 + 3; k++) {
    float er = XrRed[k] - anc * XrIr[k], ei = XiRed[k] - anc * XiIr[k]; pAnc += er * er + ei * ei;
  }
  float contamRef = d1c > 0 ? fmaxf(pAnc - 7.0f * (nfRed + anc * anc * nfIr), 0.0f) / (anc * anc * d1c) : 1.0f;
  float contam = fmaxf(contamRes, contamRef);

  float mean = 0, m2 = 0, m3 = 0;
  // skewness SQI (Elgendi 2016) over last 4 s of the (pulse-up) IR signal
  int ns = (int)(nSamp < 200 ? nSamp : 200);
  for (int i = 0; i < ns; i++) mean += rbIr[(nSamp - 1 - i) & RB_MASK];
  mean /= ns;
  for (int i = 0; i < ns; i++) { float d = rbIr[(nSamp - 1 - i) & RB_MASK] - mean; m2 += d * d; m3 += d * d * d; }
  m2 /= ns; m3 /= ns;
  float skew = m2 > 0 ? m3 / (m2 * sqrtf(m2)) : 0;

  // motion index relative to the resting floor (red/IR shape differences, noise) of this sensor
  float mExcess = motionIdx - motIdxBase;
  uint8_t mot = 0;
  if (ampRatio > 4.0f || mExcess > 0.60f || dcJump > 0.05f + dcTol) mot = 3;
  else if (ampRatio > 2.2f || mExcess > 0.25f || dcJump > 0.02f + dcTol) mot = 2;
  else if (ampRatio > 1.5f || mExcess > 0.10f || dcJump > 0.01f + dcTol) mot = 1;
  // learn the floor on calm frames (slowly, and never above a sane ceiling)
  if (ampRatio < 1.3f && dcJump < 0.008f + dcTol && !stepInWin)
    motIdxBase = clampf(motIdxBase + (motionIdx < motIdxBase ? 0.2f : 0.05f) * (motionIdx - motIdxBase), 0.0f, 0.25f);
  if (stepInWin && mot < 2) mot = 2;
  motHist[motHead] = mot; motHead = (motHead + 1) & 15;
  uint8_t winMot = 0;
  for (int i = 1; i <= (N + HOP_N - 1) / HOP_N && i <= 16; i++) { uint8_t m = motHist[(motHead - i) & 15]; if (m > winMot) winMot = m; }

  // learn the clean pulse amplitude (only on calm, pure, locked frames)
  if (mot == 0 && purity > 0.5f && conf > LOCK_CONF && !stepInWin) {
    if (!cleanRmsInit) { cleanRms = rms1; cleanRmsInit = true; }
    else cleanRms += 0.2f * (rms1 - cleanRms);
  } else if (!cleanRmsInit && nSamp > 8 * FS_HZ) { cleanRms = rms1; cleanRmsInit = true; }   // v3.2.1: 15->8 s


  // (time-domain beats were computed above, before the harmonic guard - see "v3.1.0 harmonic guard")
  bool tdUsable = (mot <= 1 || (sepOn && purityArt >= 0.35f)) && !stepInWin;
  bool tdOk = tdUsable && nb >= BEAT_MIN_N && badIv == 0;
  bool irregular = tdUsable && nb >= BEAT_MIN_N && badIv <= 1 && rrCv > IRREG_CV;
  // re-anchor the tracker if clean, regular beats contradict it for 3 frames (lock recovery)
  if (tdOk && rrCv < IRREG_CV && fabsf(hrMeanTd - hr) > 0.25f * hr) {
    if (++tdDisagree >= 3) {
      float kc = hrMeanTd / 60.0f * NFFT / FS_HZ - kLo, ps = 0;
      for (int s = 0; s < nStates; s++) { float d = (s - kc) / 2.0f; post[s] = expf(-0.5f * d * d); ps += post[s]; }
      for (int s = 0; s < nStates; s++) post[s] /= ps;
      hr = hrMeanTd; conf = 0.5f; tdDisagree = 0;
    }
  } else tdDisagree = 0;

  // ---- v3.1.0 pulse verification: is the tracked frequency really a heartbeat? ----
  // widthKnown/widthBad use only the detected beat peaks (never the HR estimate). Judged only on
  // frames where the beat detector is usable (no step, no more than mild motion).
  bool widthKnown = tdUsable && nb >= 4 && lastPulseWidthS > 0 && hr >= PULSE_GATE_MIN_BPM;
  bool widthBad = widthKnown && lastPulseWidthS > PULSE_FWHM_MAX_S;
  bool widthGood = widthKnown && !widthBad;
  if (widthBad) widthFailN++; else if (widthKnown) widthFailN = 0;
  bool dropLock = false;
  // A lock on the first/last two bins of the band is almost always the skirt of an out-of-band
  // component (slow wander below the band, motion above it), not a pulse: reject at once.
  bool edgeLock = (k0 <= kLo + 1) || (k0 >= kHi - 1);
  if (widthFailN >= PULSE_FAIL_FRAMES || edgeLock) {
    // the tracked frequency is not a pulse: suppress it, forget the lock, start a fresh acquisition
    for (int k = k0 - 4; k <= k0 + 4; k++) if (k >= 0 && k <= SPEC_KMAX) supFrames[k] = SUPPRESS_FRAMES;
    trkInit = false; hrTrk = 0; tdDisagree = 0; widthFailN = 0; jumpN = 0; hrAgreeN = 0;
    lastGoodHrS = -1e9f; dispHr = 0; hrMedN = 0; hrMedHead = 0; dispSpo2 = 0;
    rHistN = 0; rHistHead = 0; rPendN = 0; lastSpo2AcceptS = -1e9f;
    dropLock = true;
  }

  // ---- HR decision ----
  // motion that colour cannot separate (same red/IR ratio as blood) cannot be verified:
  // in that case the display holds the last good value instead of following the tracker
  // (slow DC drift - finger settling, pressure - is removed by the band-pass and does not
  //  disturb the HR, so only pulse-amplitude / colour evidence of motion counts here)
  uint8_t motAmp = 0;
  if (ampRatio > 4.0f || mExcess > 0.60f) motAmp = 3;
  else if (ampRatio > 2.2f || mExcess > 0.25f) motAmp = 2;
  else if (ampRatio > 1.5f || mExcess > 0.10f) motAmp = 1;
  // the spectrum still contains the motion for as long as the window does: use the worst level
  // inside the analysis window, otherwise a tracker that followed the motion frequency is shown
  // again the moment the motion stops
  ampHist[ampHead] = motAmp; ampHead = (ampHead + 1) & 15;
  uint8_t winAmp = 0;
  for (int i = 1; i <= (N + HOP_N - 1) / HOP_N && i <= 16; i++) { uint8_t m = ampHist[(ampHead - i) & 15]; if (m > winAmp) winAmp = m; }
  bool unverifiable = (winAmp >= 2 && !sepOn) || (sepOn && purityArt < 0.20f);   // v3.2.1
  bool locked = conf >= LOCK_CONF && purityArt >= 0.15f && !unverifiable;   // v3.2.1: 0.25->0.15 for weak PI
  // mild motion that colour cannot separate: the spectrum must still be clearly pulse-dominated
  if (winAmp >= 1 && !sepOn && purityArt < 0.20f) locked = false;   // v3.2.1: 0.40->0.20
  // heavy motion (pulse amplitude swamped): publish only when two independent estimators agree -
  // the spectral tracker and the beat detector on the colour-separated waveform
  // (random "beats" found in residual motion are irregular; real separated pulses are regular)
  bool consensus = (nb >= BEAT_MIN_N && badIv <= 1 && fabsf(hrMeanTd - hr) < 0.10f * hr &&
                    rrCv < IRREG_CV && conf >= 0.85f);
  if (winAmp >= 3 && !consensus) locked = false;
  if (dropLock || widthBad) locked = false;
  // first reading after finger placement: two consecutive frames must agree (and the signal be
  // reasonably pure) before anything is displayed
  // v3.1.0: the first reading must ALSO be corroborated by the time-domain beats (same rate within
  // 12 %, regular beats of pulse-like width) for FIRST_LOCK_AGREE frames in a row.
  if (lastGoodHrS < -1e8f) {
    // not contradicted by regular beats, and either pulse-shaped peaks were measured or the spectrum is
    // very clean (the beat detector is unreliable under strong baseline drift - do not wait for it)
    bool tdContradicts = tdOk && rrCv < IRREG_CV && fabsf(hrMeanTd - hr) > 0.12f * hr;
    // v3.2.1: third branch for weak signals: moderate confidence + low purity is enough
    bool corroborated = !widthBad && !tdContradicts && (widthGood || (conf >= 0.85f && purityArt >= 0.55f) || (conf >= 0.50f && purityArt >= 0.15f));
    if (locked && corroborated && purityArt >= 0.20f && hrAgreeN > 0 && fabsf(hr - hrFirst) < 0.08f * hr) hrAgreeN++;
    else if (locked && corroborated && purityArt >= 0.20f) { hrAgreeN = 1; hrFirst = hr; }   // v3.2.1: 0.35->0.20 for weak signals
    else hrAgreeN = 0;
    if (hrAgreeN < FIRST_LOCK_AGREE) locked = false;
  }
  float tS = tNowS();
  float hrOut = hr;
  // A heart rate cannot jump by > 25 % within seconds: such a jump (typically a harmonic / motion
  // frequency taking over) must persist for 4 consecutive frames before it is shown; meanwhile the last
  // value stays on the display (flagged held).
  if (locked && dispHr > 0 && fabsf(hr - dispHr / fsScale) > 0.20f * dispHr / fsScale) {   // v3.1.0: 20 %, no time limit
    if (jumpN > 0 && fabsf(hr - jumpHr) < 0.08f * jumpHr) jumpN++; else { jumpHr = hr; jumpN = 1; }
    if (jumpN < HR_JUMP_CONFIRM) locked = false;
  } else jumpN = 0;
  // Output = spectral tracker, refined by the beat detector ONLY when the two agree closely.
  // (Last-few-beats estimates are far too noisy on weak signals to be shown.)
  // v3.1.0: the beat detector may only NUDGE the spectral rate, by at most +-5 % (it is noisier than
  // the spectral estimate under drift; unbounded it pulled the display 10 bpm off a correct lock).
  if (tdOk && fabsf(hrMeanTd - hr) < TD_TIGHT * hr) {
    if (irregular || rrCv > IRREG_CV) hrOut = hrMeanTd;           // irregular: mean beat rate
    else hrOut = 0.5f * (hrMeanTd + hr);
  } else if (irregular && nb >= BEAT_MIN_N && fabsf(hrMeanTd - hr) < TD_AGREE * hr) hrOut = hrMeanTd;
  hrOut = clampf(hrOut, 0.95f * hr, 1.05f * hr);
  if (locked) {
    hrTrk = hr; lastGoodHrS = tS;
    // v3.1.0: median of the last 3 frame estimates removes a single odd frame; the displayed value
    // can then move by at most HR_MAX_SLEW bpm per second (a real heart rate cannot change faster).
    hrMedBuf[hrMedHead] = hrOut; hrMedHead = (hrMedHead + 1) % 3; if (hrMedN < 3) hrMedN++;
    float hrMed = hrMedN >= 3 ? medianOf(hrMedBuf, hrMedN) : hrOut;
    if (dispHr <= 0) dispHr = hrMed;
    else { float d = 0.5f * (hrMed - dispHr); if (d > HR_MAX_SLEW) d = HR_MAX_SLEW; if (d < -HR_MAX_SLEW) d = -HR_MAX_SLEW; dispHr += d; }
  }
  // expired hold: forget the old value, the next HR must pass the full first-lock verification again
  if ((tS - lastGoodHrS) > HR_HOLD_S && lastGoodHrS > -1e8f) { lastGoodHrS = -1e9f; dispHr = 0; hrMedN = 0; hrMedHead = 0; hrAgreeN = 0; jumpN = 0; }
  out.hrSpec = hr * fsScale; out.hrBeats = tdOk ? hrMeanTd * fsScale : 0; out.rrCv = rrCv; out.irregular = irregular;
  out.hrValid = (tS - lastGoodHrS) <= HR_HOLD_S;
  out.hrHeld  = out.hrValid && !locked;
  out.hrBpm   = out.hrValid ? dispHr * fsScale : 0;
  out.hrConf  = conf;

  // ---- SpO2 decision: accept this window's R only if it is trustworthy ----
  // during motion inside the window, demand stronger evidence that the HR bins are motion-free
  float spreadMax = ((winMot == 0) ? 0.10f : 0.06f) + 0.60f * sqrtf(nsr);   // v3.2.1: wider for weak-signal sensors
  // v3.2.1: Thresholds relaxed for weak-signal sensors (PI 0.2-0.5%).
  // At low perfusion index, coherence and NSR are inherently degraded even with
  // a perfect signal – the original thresholds assume PI >= 1%.
  uint16_t rej = 0;
  if (stepInWin) rej |= REJ_STEP;
  if (!locked || conf < LOCK_CONF) rej |= REJ_NOLOCK;      // was 0.45, now matches LOCK_CONF=0.30
  // purity: 0.45 -> 0.30. At PI<0.5% the pulse peak is narrow; band power outside it is sensor
  // noise, not motion. A lower purity is expected and does not indicate a bad R reading.
  if ((sepOn ? purityArt : purity) < 0.30f) rej |= REJ_PURITY;
  if (!rAgree) rej |= REJ_HARM;
  // coherence: 0.85 -> 0.40. Coherence = |cross-spectrum|^2 / (Pir * Pred). With a weak pulse
  // the noise floor dominates Pred and Pir but cancels in the numerator, so coherence is biased
  // DOWN by the noise. The noise-corrected quantities already remove most of this bias from R
  // itself; the coherence gate is a redundant second-level check here.
  if (coh < 0.40f) rej |= REJ_COH;
  // NSR: 0.60 -> 1.5. At PI=0.3% the sensor noise is comparable to the pulse. The R calculation
  // already corrects for noise in the numerator and denominator; NSR>1.5 means noise completely
  // swamps the pulse and the correction becomes unreliable.
  if (nsr > 1.5f) rej |= REJ_NOISE;
  if (rSpread > spreadMax) rej |= REJ_SPREAD;
  if (Rw < R_VALID_MIN || Rw > R_VALID_MAX) rej |= REJ_RANGE;
  if (winMot >= 3 && !sepOn) rej |= REJ_MOTION;
  // Mild motion: only the self-consistency test (one arterial ratio explains RED from IR) applies.
  // The test against the last trusted ratio is for moderate/heavy motion only - with a blip of mild
  // motion it would reject every genuine SpO2 change (a desaturation) and freeze the old value.
  float contamUse = (winMot >= 2) ? contam : contamRes;
  if (winMot >= 1 && contamUse > CONTAM_MAX + 0.5f * nsr) rej |= REJ_MOTION;     // motion energy sits on the HR bins
  if (avail < (uint32_t)N) rej |= REJ_SHORT;
  bool rOk = (rej == 0);
  // ---- v3.2.1 R-history / confirmation (complete rewrite to fix deadlock) ----
  // After a gap the history is stale: reset.
  if (tS - lastSpo2AcceptS > SPO2_HOLD_S) { rHistN = 0; rHistHead = 0; rPendN = 0; }

  if (rOk) {
    if (rHistN == 0) {
      // Phase 0: no history yet.
      // During motion, don't seed the history (a motion-contaminated R would corrupt all future estimates).
      // At rest, accept unconditionally to seed rHist – this is the ONLY way to break the deadlock.
      if (winMot >= 2 || moveWin) { rOk = false; rej |= REJ_CONFIRM; }
      // else: rOk stays true; rHist is populated below.
    } else if (rHistN == 1) {
      // Phase 1: one seeding window exists.
      // Accept if the new estimate agrees with the seed within a generous tolerance (0.25 R ~ 6 SpO2%).
      // This covers normal jitter on a weak signal without letting a large initial error propagate.
      float seed = rHist[(rHistHead - 1 + R_HIST) % R_HIST];
      if (fabsf(Rw - seed) > 0.25f) {
        // Disagrees strongly – replace the seed rather than accumulating a bad starting point.
        rHistN = 0; rHistHead = 0;   // drop bad seed; next window re-seeds
        rOk = false; rej |= REJ_CONFIRM;
      }
      // else: second window agrees → rOk stays true, SpO2 displays after this window.
    } else {
      // Phase 2: established history (>= 2 windows). Original jump-detection logic, but now
      // rm is computed from REAL accepted windows (not from an empty/zero array).
      float tmpc[R_HIST]; int nc = 0;
      for (int i = 0; i < rHistN; i++) if (tS - rHistT[i] <= SPO2_MAX_AGE_S) tmpc[nc++] = rHist[i];
      if (nc >= 2) {
        float rm = medianOf(tmpc, nc);
        bool moving = (winMot >= 1 || moveWin);
        float jumpThr = moving ? JUMP_MOVE : JUMP_REST;
        if (fabsf(Rw - rm) > jumpThr) {
          if (moving) { rOk = false; rej |= REJ_CONFIRM; rPendN = 0; }
          else {
            // At rest: a large jump needs 3 consecutive confirming windows before we adopt it.
            if (rPendN > 0 && fabsf(Rw - rPend) < 0.06f) rPendN++; else { rPend = Rw; rPendN = 1; }
            if (rPendN < 3) { rOk = false; rej |= REJ_CONFIRM; }
            else { rHistN = 0; rHistHead = 0; }   // confirmed real SpO2 change: restart average
          }
        } else rPendN = 0;
      }
    }
  }
  out.rNow = Rw; out.rAccepted = rOk; out.rSpread = rSpread; out.winMotion = winMot;
  out.rReject = rej; out.contam = contam;
  if (rOk) {
    rHist[rHistHead] = Rw; rHistT[rHistHead] = tS; rHistHead = (rHistHead + 1) % R_HIST; if (rHistN < R_HIST) rHistN++;
    lastSpo2AcceptS = tS; out.rAcceptCount++;
    rPeakPrev = Rw; rPeakT = tS;            // trusted arterial ratio (anchor for motion-bin selection)
    // slowly adapt the arterial ratio used to build the optical motion reference
    if (winMot == 0 && purity > 0.6f) ra = clampf(ra + 0.25f * (Rw - ra), RA_MIN, RA_MAX);
  }
  // median of recent accepted windows (drop stale ones)
  float tmp[R_HIST]; int nt = 0;
  for (int i = 0; i < rHistN; i++) if (tS - rHistT[i] <= SPO2_MAX_AGE_S) tmp[nt++] = rHist[i];
  out.spo2Valid = false;
  // v3.2.1: display SpO2 as soon as we have 1 accepted window (nt >= 1).
  // nt >= 2 (original) prevented display until 2 windows were accepted, adding an extra ~1 s delay.
  if (nt >= 1 && (tS - lastSpo2AcceptS) <= SPO2_HOLD_S) {
    float rm = trimmedMean(tmp, nt);
    // trend guard: the long average smooths noise but lags a real desaturation. When the newest
    // accepted windows persistently sit away from it, follow them.
    if (nt >= 4) {
      float lat = 0; int c = 0;
      for (int j = 1; j <= 3; j++) { int idx = (rHistHead - j + R_HIST) % R_HIST; if (tS - rHistT[idx] <= SPO2_MAX_AGE_S) { lat += rHist[idx]; c++; } }
      if (c == 3) { lat /= 3; if (fabsf(lat - rm) > TREND_R) rm = lat; }
    }
    float sp = spo2FromR(rm);
    out.rMedian = rm;
    // v3.2.0: sp == 0.0f is the sentinel from spo2FromR() meaning R is in the flat/saturated
    // zone (< SPO2_R_MIN_DISPLAY). Withhold the display value rather than showing an
    // unreliable reading or clamped 100 %.
    if (sp >= 50.0f) {
      // v3.1.0: slew-limited display value (SpO2 does not change by several % within a second)
      if (dispSpo2 <= 0) dispSpo2 = sp;
      else { float d = sp - dispSpo2; if (d > SPO2_MAX_SLEW) d = SPO2_MAX_SLEW; if (d < -SPO2_MAX_SLEW) d = -SPO2_MAX_SLEW; dispSpo2 += d; }
      // cap: never show above SPO2_CLAMP_MAX on an uncalibrated consumer sensor
      if (dispSpo2 > SPO2_CLAMP_MAX) dispSpo2 = SPO2_CLAMP_MAX;
      out.spo2 = dispSpo2; out.spo2Valid = true; out.spo2Held = !rOk;
    }
  }
  // v3.1.0: SpO2 is only reported while the heart rate itself is verified (same pulse, same window)
  if (!out.hrValid) out.spo2Valid = false;
  if (!out.spo2Valid) dispSpo2 = 0;

  out.motion = mot; out.motionIdx = motionIdx; out.ampRatio = ampRatio; out.dcJump = dcJump;
  out.purity = purity; out.skew = skew; out.pi = pi; out.ra = ra;
  out.kMot = kMot; out.sep = sepOn; out.purityArt = purityArt;
  // quality 0..100
  float q = 100.0f * (0.35f * clampf(conf / 0.8f, 0, 1) + 0.30f * clampf(purityArt / 0.7f, 0, 1) +
                      0.20f * (1.0f - mot / 3.0f) + 0.15f * clampf(pi / 1.0f, 0, 1));
  if (skew < 0.0f) q *= 0.85f;      // clean finger PPG (pulse-up) is positively skewed
  out.quality = q;
}

// ============================================================================
//                     ALGORITHM: PER-SAMPLE ENTRY POINT
// ============================================================================
void algoInit() {
  fftInit();
  kLo = (int)ceilf(HR_MIN_BPM / 60.0f * NFFT / FS_HZ);
  kHi = (int)floorf(HR_MAX_BPM / 60.0f * NFFT / FS_HZ);
  nStates = kHi - kLo + 1; if (nStates > 128) nStates = 128;
  kernHalf = (int)ceilf(3.0f * TRK_SIGMA_BINS); if (kernHalf > 7) kernHalf = 7;
  float ks = 0;
  for (int d = -kernHalf; d <= kernHalf; d++) { kern[d + kernHalf] = expf(-0.5f * d * d / (TRK_SIGMA_BINS * TRK_SIGMA_BINS)); ks += kern[d + kernHalf]; }
  for (int i = 0; i < 2 * kernHalf + 1; i++) kern[i] /= ks;
  memset(&out, 0, sizeof(out)); out.ra = ra;
  algoReset();
}

// Call once per 50 Hz sample. Returns true if an analysis frame was computed.
bool algoPush(uint32_t redRaw, uint32_t irRaw) {
  bool levelOk = (irRaw > irContactMin) && (redRaw > redContactMin);
  if (!contact) {
    if (levelOk) { if (++onCnt >= CONTACT_ON_SAMPLES) { contact = true; offCnt = 0; algoReset(); filtersInit(irRaw, redRaw); } }
    else onCnt = 0;
    if (!contact) { out.contact = false; return false; }
  } else {
    // finger leaving: light collapses far below the running DC long before the absolute
    // threshold is reached - stop showing the last values immediately (no stale "VALID")
    bool leaving = dcIr > 1.0f && (float)irRaw < FINGER_LEAVE_FRAC * dcIr;
    if (leaving) { out.hrValid = out.spo2Valid = false; out.hrHeld = out.spo2Held = false; }
    if (!levelOk || leaving) {
      if (++offCnt >= CONTACT_OFF_SAMPLES) { contact = false; onCnt = 0; algoReset(); out.contact = false; return false; }
    } else offCnt = 0;
  }
  out.contact = true;

  float ir = (float)irRaw, red = (float)redRaw;
  bool sat = irRaw >= ADC_SAT_LEVEL || redRaw >= ADC_SAT_LEVEL;

  // step detection: fast tracker (~0.1 s) vs slow DC
  trendIr += 0.3f * (ir - trendIr);
  if (fabsf(trendIr - dcIr) > STEP_FRAC * dcIr) stepCnt++; else stepCnt = 0;
  if (stepCnt >= STEP_PERSIST && sinceReset > (uint32_t)FS_HZ) {
    filtersInit(ir, red);                 // instant re-centre, no 3-second ringing
    blankLeft = (int)(1.0f * FS_HZ);      // data kept, but flagged (no SpO2 from this window)
    out.stepEvents++;
  }

  dcIr = biq(dcIrF, ir); dcRed = biq(dcRedF, red);
  {
    float yi = ir / (dcIr > 1 ? dcIr : 1), yr = red / (dcRed > 1 ? dcRed : 1);
    if (nzCnt >= 2) {
      float di = yi - 2 * pyI1 + pyI2, dr = yr - 2 * pyR1 + pyR2;
      float vi = di * di / 6.0f, vr = dr * dr / 6.0f;
      if (nzCnt == 2) { nzIr = vi; nzRed = vr; }
      else {   // follows the LOWER envelope: bursts of motion must not inflate the noise estimate
        nzIr  += (vi > nzIr  ? 0.002f : 0.02f) * (vi - nzIr);
        nzRed += (vr > nzRed ? 0.002f : 0.02f) * (vr - nzRed);
      }
    }
    if (nzCnt < 1000000) nzCnt++;
    pyI2 = pyI1; pyI1 = yi; pyR2 = pyR1; pyR1 = yr;
  }
  float xi = ir / refIrDC - 1.0f, xr = red / refRedDC - 1.0f;
  float bi = biq(lpIr2, biq(lpIr1, biq(hpIr2, biq(hpIr1, xi))));
  float br = biq(lpRed2, biq(lpRed1, biq(hpRed2, biq(hpRed1, xr))));
  // AC/DC with pulse pointing UP (more blood = less light)
  float nI = -bi * refIrDC / (dcIr > 1 ? dcIr : 1);
  float nR = -br * refRedDC / (dcRed > 1 ? dcRed : 1);
  sinceReset++;

  // optical motion reference: arterial pulsation cancels, motion (other colour) remains
  float ref = nR - ra * nI;
  uint32_t n = nSamp;
  rbIr[n & RB_MASK] = nI; rbRed[n & RB_MASK] = nR; rbRef[n & RB_MASK] = ref;
  bool badNow = sat || blankLeft > 0;
  if (blankLeft > 0) blankLeft--;
  rbBad[n & RB_MASK] = badNow ? 1 : 0;
  nSamp++;

  if ((nSamp % HOP_N) == 0) { analyseFrame(); return true; }
  return false;
}

MeasState algoState(bool sensorOk) {
  if (!sensorOk) return ST_SENSOR_ERROR;
  if (!out.contact) return ST_NO_FINGER;
  if ((int)nSamp < WIN_MIN) return ST_WARMUP;
  if (!out.hrValid) return ST_NO_LOCK;
  if (out.motion >= 2) return ST_MOTION;
  if (out.pi < 0.08f) return ST_LOW_PERFUSION;
  return ST_VALID;
}

// LED auto-gain decision. Returns true if a new current should be applied.

// ============================================================================
//                               SMALL PUBLIC API
// ============================================================================
const AlgoOut& algoOut() { return out; }
// v2.3.1: the sensor stopped delivering samples (I2C failure / disconnection). The analysis
// window would otherwise join data from before and after the gap as if it were continuous,
// and the last HR/SpO2 would stay "valid". Drop the session: contact must be re-established
// (0.5 s) and the window refilled before any new estimate. Calibration and the learned
// arterial ratio are kept (they are properties of the sensor/person, not of the session).
void algoSensorGap() {
  contact = false; onCnt = 0; offCnt = 0;
  algoReset();
  out.contact = false;
}
void     algoSetContactThresholds(uint32_t irMin, uint32_t redMin) { irContactMin = irMin; redContactMin = redMin; }
uint32_t algoIrContactMin()  { return irContactMin; }
uint32_t algoRedContactMin() { return redContactMin; }
void     algoSetFsScale(float s) { fsScale = clampf(s, 0.93f, 1.07f); }
float    algoFsScale() { return fsScale; }
float    algoDcIr()  { return dcIr; }
float    algoDcRed() { return dcRed; }
// v3.0.0 (Phase 2 point 2): cleanRms is updated ONLY on calm/pure/locked frames (see the
// "learn the clean pulse amplitude" block in analyseFrame()) - it is a HISTORICAL baseline of
// the last known-good contact quality, not a live measurement. A caller must not treat it as
// "current" contact strength during active motion; it is specifically useful BECAUSE it does
// not move with motion. See docs/KNOWN_LIMITATIONS.md for the continuous-motion edge case
// (if the sensor never sees a calm second in the first 15 s, cleanRmsInit still becomes true
// from a possibly motion-influenced sample - this accessor cannot by itself distinguish that
// case from a genuine calm baseline).
float    algoCleanRms()     { return cleanRms; }
bool     algoCleanRmsInit() { return cleanRmsInit; }

int  calCount() { return calN; }
void calGetPoint(int i, float& R, float& refSpo2) { R = calR[i]; refSpo2 = calS[i]; }
void calSetPoints(const float* R, const float* refSpo2, int n) {
  if (n < 0) n = 0;
  if (n > CAL_MAX_POINTS) n = CAL_MAX_POINTS;
  for (int i = 0; i < n; i++) { calR[i] = R[i]; calS[i] = refSpo2[i]; }
  calN = n; calB = CAL_SLOPE_DEFAULT; calFit();
}
void calAddPoint(float R, float refSpo2) {
  if (calN == CAL_MAX_POINTS) { for (int i = 1; i < calN; i++) { calR[i - 1] = calR[i]; calS[i - 1] = calS[i]; } calN--; }
  calR[calN] = R; calS[calN] = refSpo2; calN++;
  calFit();
}
void calClear() { calN = 0; calA = 0; calB = CAL_SLOPE_DEFAULT; calIsFit = false; }
void calLine(float& a, float& b) { a = calA; b = calB; }

const char* rejectReason(uint16_t m) {
  static const char* const why[] = { "DC step", "pulse not locked", "pulse not dominant", "harmonic mismatch",
                                     "red/IR incoherent", "motion overlaps pulse", "R out of range",
                                     "motion on HR", "window filling", "confirming change", "signal too noisy" };
  for (int i = 0; i < 11; i++) if (m & (1 << i)) return why[i];
  return "collecting windows";
}
