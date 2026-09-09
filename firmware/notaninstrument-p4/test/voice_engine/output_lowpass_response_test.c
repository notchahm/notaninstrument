// Regression test for render_locked()'s post-mix de-hiss low-pass
// (2-stage cascaded one-pole, fc=9kHz @ 48kHz). Added after direct
// offline decode + FFT measurement showed both ADPCM and QOA inject real
// reconstruction noise concentrated at 8-16kHz during a piano hammer
// strike's fast transient (11-45x / 11-33dB more energy there than the
// clean pre-codec signal has) -- confirmed present in both codecs (not a
// codec-specific defect) and confirmed *not* fixable by low-pass
// filtering the encoder's input (the noise comes from quantizing the
// transient itself, not from existing high-frequency source content).
// Loosened from an initial 3-stage/6kHz tuning once QOA (made the
// primary codec, with its own per-region adaptive raw-PCM-prefix width
// -- see sfz_to_nib.py's find_qoa_raw_prefix_samples) started sharing
// the noise-reduction burden this filter used to carry alone.
//
// This mirrors voice_engine.c's OUTPUT_LOWPASS_ALPHA_Q16/
// OUTPUT_LOWPASS_STAGES constants and one_pole_lowpass_cascade() (not an
// include of voice_engine.c, which isn't portable outside ESP-IDF) --
// keep it in sync if that arithmetic changes.
//
// What it checks: feed pure sine tones through the exact cascade and
// verify the measured attenuation at a low (piano fundamental range) and
// a high (measured noise band) frequency both land in the expected
// ballpark -- catches a badly wrong coefficient (e.g. a sign error, a
// stage count of 0, or a coefficient that barely filters anything)
// without needing bit-exact frequency-response equality.
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#define OUTPUT_LOWPASS_ALPHA_Q16 45360
#define OUTPUT_LOWPASS_STAGES 2
#define SAMPLE_RATE 48000

static int32_t one_pole_lowpass_cascade(int32_t *state, int32_t input) {
    int32_t x = input;
    for (int s = 0; s < OUTPUT_LOWPASS_STAGES; s++) {
        state[s] += (int32_t) (((int64_t) (x - state[s]) * OUTPUT_LOWPASS_ALPHA_Q16) >> 16);
        x = state[s];
    }
    return x;
}

// Measures steady-state output amplitude for a full-scale sine at
// freq_hz, after letting the filter settle past its startup transient.
static double measure_gain_db(double freq_hz) {
    int32_t state[OUTPUT_LOWPASS_STAGES] = {0};
    int settle_samples = SAMPLE_RATE / 4; // 250ms, several times this filter's time constant
    int measure_samples = SAMPLE_RATE / 10;
    double amplitude = 20000.0; // well within int16 range, matches typical mixed-output levels
    double max_out = 0.0;

    int total = settle_samples + measure_samples;
    for (int n = 0; n < total; n++) {
        double t = (double) n / SAMPLE_RATE;
        int32_t in = (int32_t) (amplitude * sin(2.0 * M_PI * freq_hz * t));
        int32_t out = one_pole_lowpass_cascade(state, in);
        if (n >= settle_samples) {
            double mag = fabs((double) out);
            if (mag > max_out) max_out = mag;
        }
    }
    return 20.0 * log10(max_out / amplitude);
}

int main(void) {
    int failures = 0;

    double gain_1k = measure_gain_db(1000.0);
    double gain_12k = measure_gain_db(12000.0);
    double gain_16k = measure_gain_db(16000.0);

    printf("gain @1kHz: %.1fdB, @12kHz: %.1fdB, @16kHz: %.1fdB\n", gain_1k, gain_12k, gain_16k);

    // Piano fundamental range should pass through nearly unaffected.
    if (gain_1k < -3.0) {
        fprintf(stderr, "FAIL: 1kHz attenuated %.1fdB -- too aggressive for piano fundamentals\n", gain_1k);
        failures++;
    }
    // The measured noise band (8-16kHz) must still be meaningfully cut --
    // a stage count of 0 or a near-unity coefficient would show ~0dB here.
    if (gain_12k > -4.0) {
        fprintf(stderr, "FAIL: 12kHz only attenuated %.1fdB -- filter isn't doing its job\n", gain_12k);
        failures++;
    }
    if (gain_16k > -6.0) {
        fprintf(stderr, "FAIL: 16kHz only attenuated %.1fdB -- filter isn't doing its job\n", gain_16k);
        failures++;
    }
    // Rolloff must be monotonic (12kHz cut more than 1kHz, 16kHz cut
    // more than 12kHz) -- catches a coefficient that's a high-pass by
    // mistake (sign flipped) or otherwise structurally wrong.
    if (!(gain_1k > gain_12k && gain_12k > gain_16k)) {
        fprintf(stderr, "FAIL: response isn't a monotonically increasing low-pass rolloff\n");
        failures++;
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: low-pass response is a sane, monotonic rolloff\n");
    return 0;
}
