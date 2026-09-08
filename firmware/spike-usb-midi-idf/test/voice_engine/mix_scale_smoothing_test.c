// Regression test for a real audible-click bug in render_locked()'s
// polyphony gain compensation: INV_SQRT_TABLE[active] used to be applied
// as a direct, instantaneous per-sample multiplier. Since `active` is a
// discrete voice count, the moment a second voice starts next to an
// already-playing one (even a second voice still silent in its own
// attack ramp), the scale applied to *all* voices -- including the
// already-audible first one -- snaps from 65536 (unity) to 46341
// (~0.707) in a single sample: a real ~3dB step on sound that's already
// playing. Confirmed on real hardware as a click specifically on
// retriggering a note before its previous voice finished releasing (a
// second voice appearing next to a loud first one), and confirmed
// absent on a solo note from silence (0->1 active has nothing audible
// to step). The fix chases the table value with a one-pole smoother
// instead of snapping to it.
//
// This mirrors render_locked()'s mix-scale smoothing arithmetic (not an
// include of voice_engine.c, which isn't portable outside ESP-IDF) --
// keep it in sync if that arithmetic changes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const uint32_t INV_SQRT_TABLE[9] = {
    0, 65536, 46341, 37837, 32768, 29309, 26755, 24777, 23170,
};

int main(void) {
    uint32_t s_mix_scale = 65536;
    int failures = 0;

    // Step 1->2 active voices for many samples: the applied scale must
    // never jump by more than a small fraction per sample (i.e. must be
    // smoothed, not snapped), while still eventually converging on the
    // target.
    uint32_t max_single_step_delta = 0;
    for (int n = 0; n < 2000; n++) {
        int active = (n == 0) ? 1 : 2; // step happens between sample 0 and 1
        uint32_t target = (active > 0) ? INV_SQRT_TABLE[active] : 65536u;
        uint32_t prev = s_mix_scale;
        s_mix_scale = (uint32_t) ((int32_t) s_mix_scale +
                                   (((int32_t) target - (int32_t) s_mix_scale) >> 8));
        uint32_t delta = (s_mix_scale > prev) ? (s_mix_scale - prev) : (prev - s_mix_scale);
        if (delta > max_single_step_delta) {
            max_single_step_delta = delta;
        }
    }
    // The old, buggy behavior's step was 65536-46341=19195 in one
    // sample. A properly smoothed transition's largest single-sample
    // step should be a small fraction of that.
    if (max_single_step_delta > 19195 / 4) {
        fprintf(stderr, "FAIL: max single-sample scale delta %u -- looks unsmoothed (old bug's step was 19195)\n",
                max_single_step_delta);
        failures++;
    }
    if (s_mix_scale < 46000 || s_mix_scale > 46341) {
        fprintf(stderr, "FAIL: scale didn't converge near INV_SQRT_TABLE[2]=46341 after 2000 samples (got %u)\n",
                s_mix_scale);
        failures++;
    }

    // Silence (active==0) must hold at unity, not decay toward
    // INV_SQRT_TABLE[0]==0 -- a fresh solo note must start at full
    // scale with no added fade-in.
    s_mix_scale = 37837; // pretend we just came from 3 active voices
    for (int n = 0; n < 5000; n++) {
        uint32_t target = 65536u; // active == 0
        s_mix_scale = (uint32_t) ((int32_t) s_mix_scale +
                                   (((int32_t) target - (int32_t) s_mix_scale) >> 8));
    }
    // An integer one-pole smoother's error term truncates to 0 (no
    // further progress) once the remaining gap is smaller than 2^shift,
    // so it settles *near* the target, not bit-exactly on it -- 65536 -
    // 256 is the worst case here, and that's inaudible (<0.4%).
    if (s_mix_scale < 65536 - 256 || s_mix_scale > 65536) {
        fprintf(stderr, "FAIL: scale didn't settle near unity (65536) during silence (got %u)\n", s_mix_scale);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: mix-scale transitions are smoothed and silence holds at unity gain\n");
    return 0;
}
