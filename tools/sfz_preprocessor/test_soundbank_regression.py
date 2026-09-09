#!/usr/bin/env python3
"""Regression test for the .nib codec pipeline's frame/block-independence
guarantee -- the property voice_engine.c's O(1) loop-wrap relies on (see
docs/polyphony-latency-investigation.md and firmware/notaninstrument-p4/
main/voice_engine.c's advance_voice()).

Grew out of a real bug: advance_voice()'s QOA loop-wrap reset used the
region's raw data pointer and an unadjusted (non-QOA-relative) sample
index instead of qoa_compressed_data()/qoa_raw_prefix_samples()-adjusted
ones, so every loop-wrap on a QOA-encoded voice decoded from a
essentially-arbitrary wrong byte offset. It shipped silently past code
review and only surfaced on real hardware as a "distorted, two notes at
once" symptom on held (i.e. long enough to loop) notes -- exactly the
kind of defect that's cheap to catch here and expensive to catch by ear.

Builds tiny synthetic QOA and ADPCM fixtures (no dependency on the real
Salamander library) using this project's own encoders, then hands them to
small C test drivers (test/voice_engine/*_independence_test.c) that
#include the actual production decoders unmodified -- so this test can
never silently drift out of sync with what actually ships. The property
under test: decoding sequentially from the start of a stream all the way
to frame/block N must produce bit-identical output to resetting the
stream directly to frame/block N, for every frame/block boundary. A wrong
data pointer or sample-index offset on the reset path breaks this
silently (the stream still decodes *something*, just the wrong content),
exactly like the real bug this test exists to catch.

Usage: ./test_soundbank_regression.py   (exit 0 = pass)
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import sfz_to_nib as nib

REPO_ROOT = Path(__file__).resolve().parents[2]
FIRMWARE_MAIN = REPO_ROOT / "firmware" / "notaninstrument-p4" / "main"
TEST_DIR = Path(__file__).parent.parent.parent / "firmware" / "notaninstrument-p4" / "test" / "voice_engine"
USB_MIDI_TEST_DIR = Path(__file__).parent.parent.parent / "firmware" / "notaninstrument-p4" / "test" / "usb_midi_host"
AUDIO_OUTPUT_TEST_DIR = Path(__file__).parent.parent.parent / "firmware" / "notaninstrument-p4" / "test" / "audio_output"

SAMPLE_RATE = 32000


def synth_note(seconds, freq_hz, seed):
    """A deterministic stand-in for a real sampled note: a sharp,
    noisy attack transient (the part that historically exposed both the
    LMS cold-start click and the loop-wrap addressing bug) followed by a
    decaying tone, stereo, int16. Not meant to sound like a piano --
    only needs to be structurally sharp enough to exercise the codecs
    realistically."""
    rng = np.random.default_rng(seed)
    n = int(seconds * SAMPLE_RATE)
    t = np.arange(n) / SAMPLE_RATE
    attack_n = int(0.01 * SAMPLE_RATE)
    envelope = np.ones(n)
    envelope[:attack_n] = np.linspace(0, 1, attack_n)
    envelope *= np.exp(-t * 1.5)
    tone = np.sin(2 * np.pi * freq_hz * t)
    noise_burst = rng.normal(0, 1, n) * np.exp(-t * 40)
    mono = (0.8 * tone + 0.6 * noise_burst) * envelope
    mono = np.clip(mono * 32767, -32768, 32767).astype(np.int16)
    stereo = np.stack([mono, mono], axis=1)
    return np.ascontiguousarray(stereo)


def build_qoa_fixture(tmpdir):
    samples_per_unit = nib.QOA_SLICES_PER_FRAME * 20
    raw_prefix_samples = nib.QOA_RAW_PREFIX_MIN_FRAMES * samples_per_unit
    pcm = synth_note(seconds=1.0, freq_hz=440.0, seed=1)

    raw_prefix = np.ascontiguousarray(pcm[:raw_prefix_samples])
    rest = pcm[raw_prefix_samples:]
    padded_rest = nib.pad_to_frame_multiple(rest, samples_per_unit)
    pcm_for_encoder = np.concatenate([raw_prefix, padded_rest], axis=0)
    qoa_bytes = nib.encode_qoa_stereo(pcm_for_encoder, SAMPLE_RATE, samples_per_unit, raw_prefix_samples)

    region_path = Path(tmpdir) / "qoa_region.bin"
    region_path.write_bytes(qoa_bytes)
    frame_size_bytes = nib.qoa_frame_size_bytes(samples_per_unit)
    return region_path, frame_size_bytes, samples_per_unit


def build_qoa_hybrid_loop_fixture(tmpdir):
    """A fixture purpose-built for qoa_hybrid_voice_playback_test.c: a
    noisy attack transient (raw-PCM prefix + a couple of QOA frames)
    followed by a pure tone whose period evenly divides a frame-aligned
    loop region. Because looping re-decodes the *same* stored QOA frames
    from their own header every time (that's the whole point of
    frame-independent addressing), correctly-addressed playback must
    reproduce each loop iteration's decoded content bit-for-bit -- not
    just "close enough." A wrong data pointer or sample index breaks that
    completely, which is what this fixture exists to make obvious.
    """
    samples_per_unit = nib.QOA_SLICES_PER_FRAME * 20
    raw_prefix_samples = nib.QOA_RAW_PREFIX_MIN_FRAMES * samples_per_unit
    loop_start_abs = raw_prefix_samples + samples_per_unit * 2
    loop_len = samples_per_unit * 10
    loop_end_abs = loop_start_abs + loop_len
    tail = samples_per_unit * 4
    total_len = loop_end_abs + tail

    rng = np.random.default_rng(4)
    mono = np.empty(total_len)
    noise = rng.normal(0, 1, loop_start_abs)
    mono[:loop_start_abs] = noise * np.exp(-np.arange(loop_start_abs) / (SAMPLE_RATE * 0.01)) * 0.5
    freq_hz = 500.0  # period = 64 samples @ 32kHz, divides loop_len (3200) exactly
    tail_t = np.arange(total_len - loop_start_abs) / SAMPLE_RATE
    mono[loop_start_abs:] = 0.7 * np.sin(2 * np.pi * freq_hz * tail_t)
    mono_i16 = np.clip(mono * 32767, -32768, 32767).astype(np.int16)
    pcm = np.ascontiguousarray(np.stack([mono_i16, mono_i16], axis=1))

    raw_prefix = np.ascontiguousarray(pcm[:raw_prefix_samples])
    rest = pcm[raw_prefix_samples:]
    padded_rest = nib.pad_to_frame_multiple(rest, samples_per_unit)
    pcm_for_encoder = np.concatenate([raw_prefix, padded_rest], axis=0)
    qoa_bytes = nib.encode_qoa_stereo(pcm_for_encoder, SAMPLE_RATE, samples_per_unit, raw_prefix_samples)

    region_path = Path(tmpdir) / "qoa_hybrid_region.bin"
    region_path.write_bytes(raw_prefix.tobytes() + qoa_bytes)
    frame_size_bytes = nib.qoa_frame_size_bytes(samples_per_unit)
    return {
        "region_path": region_path,
        "frame_size_bytes": frame_size_bytes,
        "samples_per_frame": samples_per_unit,
        "raw_prefix_samples": raw_prefix_samples,
        "loop_start_abs": loop_start_abs,
        "loop_end_abs": loop_end_abs,
        "loop_len": loop_len,
    }


def build_adpcm_fixture(tmpdir):
    block_size_bytes = 1 << 8  # matches sfz_to_nib.py's --adpcm-block-size-pow default
    samples_per_block = nib.adpcm_samples_per_block(block_size_bytes)
    pcm = synth_note(seconds=1.0, freq_hz=440.0, seed=2)
    left = np.ascontiguousarray(pcm[:, 0])
    block_bytes = nib.encode_adpcm_mono(left, SAMPLE_RATE, 8, 6)

    block_path = Path(tmpdir) / "adpcm_blocks.bin"
    block_path.write_bytes(block_bytes)
    return block_path, block_size_bytes, samples_per_block


def compile_test(src_name, tmpdir, test_dir=TEST_DIR):
    src = test_dir / src_name
    out = Path(tmpdir) / src.stem
    subprocess.run(["gcc", "-O2", "-o", str(out), str(src), "-lm"], check=True)
    return out


def run_test(binary, *args):
    proc = subprocess.run([str(binary), *[str(a) for a in args]],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    print(proc.stdout.strip())
    if proc.returncode != 0:
        print(proc.stderr.strip(), file=sys.stderr)
    return proc.returncode == 0


def run_hybrid_loop_test(binary, fixture):
    total_output_samples = fixture["loop_end_abs"] + 3 * fixture["loop_len"]
    proc = subprocess.run(
        [str(binary), str(fixture["region_path"]), str(fixture["frame_size_bytes"]),
         str(fixture["samples_per_frame"]), str(fixture["raw_prefix_samples"]),
         str(fixture["loop_start_abs"]), str(fixture["loop_end_abs"]), str(total_output_samples)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        print(proc.stderr.decode(errors="replace"), file=sys.stderr)
        return False

    pcm = np.frombuffer(proc.stdout, dtype=np.int16).reshape(-1, 2)
    left = pcm[:, 0]
    loop_start = fixture["loop_start_abs"]
    loop_len = fixture["loop_len"]
    first_iteration = left[loop_start:loop_start + loop_len]

    ok = True
    num_iterations = (len(left) - loop_start) // loop_len
    for i in range(1, num_iterations):
        start = loop_start + i * loop_len
        iteration = left[start:start + loop_len]
        if not np.array_equal(iteration, first_iteration):
            mismatches = np.where(iteration != first_iteration)[0]
            print(f"MISMATCH: loop iteration {i} differs from iteration 0 at "
                  f"{len(mismatches)} of {loop_len} samples "
                  f"(first at offset {mismatches[0]}: "
                  f"iteration0={first_iteration[mismatches[0]]} "
                  f"iteration{i}={iteration[mismatches[0]]})", file=sys.stderr)
            ok = False

    if ok:
        print(f"PASS: {num_iterations - 1} repeated loop iteration(s) bit-identical to the first")
    return ok


def run_level_loop_amplitude_swell_test():
    """Regression test for a real swell bug in level_loop_amplitude:
    an earlier version assumed a loop's entire natural decay between its
    two measured endpoints follows one clean exponential curve, and
    computed a single continuous compensating gain ramp to cancel it.
    Real piano decay is front-loaded (fast initial decay, a flatter
    tail) -- confirmed on real hardware that the ramp then "gives back"
    gain on a fixed schedule regardless of what the signal is actually
    doing, producing an audible rise right as a still-decaying-fast
    signal meets a ramp that assumes it's already leveled off.

    This calls the real level_loop_amplitude() directly (not a mirror)
    against a synthetic worst-case signal built to have exactly that
    shape (a sharp corner from fast decay to a flat tail, deliberately
    more adversarial than real piano decay, which is smoother) and
    checks the leveled result's windowed RMS envelope never rises more
    than a small tolerance step to step.
    """
    rate = SAMPLE_RATE
    n = int(0.5 * rate)
    t = np.arange(n) / rate
    envelope = np.concatenate([
        np.exp(-t[:n // 4] * 30),
        np.full(n - n // 4, np.exp(-(n // 4) / rate * 30)),
    ])
    tone = np.sin(2 * np.pi * 220 * t)
    mono = (envelope * tone * 20000).astype(np.int16)
    loop = np.stack([mono, mono], axis=1)

    leveled = nib.level_loop_amplitude(loop, window_frames=int(0.05 * rate))

    win = 1600  # 50ms
    left = leveled[:, 0].astype(np.float64)
    n_win = len(left) // win
    windowed_rms = np.array([np.sqrt(np.mean(left[i * win:(i + 1) * win] ** 2)) for i in range(n_win)])
    rises = np.diff(windowed_rms) / np.maximum(windowed_rms[:-1], 1.0)
    max_rise = float(rises.max())

    # The old, buggy (assumed-exponential-ramp) version overshot by far
    # more than this on the same adversarial signal; a well-behaved
    # pointwise correction should stay well under a 25% step-to-step
    # rise even on this deliberately sharp-cornered worst case (real
    # piano decay, without a hard corner, measured under 5% in practice).
    if max_rise > 0.25:
        print(f"FAIL: max step-to-step rise in leveled envelope is {max_rise*100:.1f}% "
              f"(windowed RMS: {np.round(windowed_rms).astype(int)})", file=sys.stderr)
        return False
    print(f"PASS: max step-to-step rise in leveled envelope is {max_rise*100:.1f}% (<=25% tolerance)")
    return True


def run_trim_and_loop_short_recording_guard_test():
    """Regression test for a real crash bug in trim_and_loop's "recording
    shorter than requested" fallback: for a handful of naturally short
    high-key recordings (confirmed on the real virtuosity_drums library
    and on Salamander's own shortest velocity layers), the old guard
    could compute target_loop_start >= target_loop_end after clamping to
    the available audio length, producing an inverted/empty loop window
    that crashed inside level_loop_amplitude's np.pad() with "can't
    extend empty axis 0" instead of the graceful fallback its own
    comment promised.

    Calls the real trim_and_loop() directly against synthetic audio
    shorter than attack_seconds+loop_seconds+margin would normally need,
    and asserts it returns a valid (non-empty, loop_start < loop_end)
    result instead of raising.
    """
    rate = SAMPLE_RATE
    # ~1 second of audio -- far shorter than the 3.0+0.5+0.5 = 4.0s a
    # normal Salamander attack_seconds/loop_seconds combination would
    # want, matching the real short-recording scenario (e.g. Salamander's
    # own softest A7 layer is 2.62s; some virtuosity_drums samples are
    # shorter still).
    n = int(1.0 * rate)
    t = np.arange(n) / rate
    mono = (np.sin(2 * np.pi * 220 * t) * np.exp(-t * 3) * 20000).astype(np.int16)
    audio = np.stack([mono, mono], axis=1)

    try:
        pcm, loop_start, loop_end = nib.trim_and_loop(
            audio, rate, attack_seconds=3.0, loop_seconds=0.5, crossfade_ms=100.0, samples_per_block=320)
    except Exception as e:  # noqa: BLE001 -- any exception here is exactly the regression
        print(f"FAIL: trim_and_loop raised on a short recording: {e!r}", file=sys.stderr)
        return False

    if not (0 <= loop_start < loop_end <= pcm.shape[0]):
        print(f"FAIL: trim_and_loop returned an invalid loop window "
              f"(loop_start={loop_start}, loop_end={loop_end}, pcm_len={pcm.shape[0]})", file=sys.stderr)
        return False
    print(f"PASS: short recording ({n} samples) produced a valid loop window "
          f"(loop_start={loop_start}, loop_end={loop_end}, pcm_len={pcm.shape[0]})")
    return True


def main():
    ok = True
    with tempfile.TemporaryDirectory() as tmpdir:
        print("Building QOA test fixture...")
        qoa_region_path, qoa_frame_size, qoa_samples_per_frame = build_qoa_fixture(tmpdir)
        print("Building ADPCM test fixture...")
        adpcm_block_path, adpcm_block_size, adpcm_samples_per_block = build_adpcm_fixture(tmpdir)
        print("Building QOA hybrid raw-prefix/loop-wrap test fixture...")
        hybrid_fixture = build_qoa_hybrid_loop_fixture(tmpdir)

        print("Compiling test drivers...")
        qoa_test_bin = compile_test("qoa_frame_independence_test.c", tmpdir)
        adpcm_test_bin = compile_test("adpcm_block_independence_test.c", tmpdir)
        hybrid_test_bin = compile_test("qoa_hybrid_voice_playback_test.c", tmpdir)
        mix_scale_test_bin = compile_test("mix_scale_smoothing_test.c", tmpdir)
        lowpass_test_bin = compile_test("output_lowpass_response_test.c", tmpdir)
        channel_routing_test_bin = compile_test("channel_routing_test.c", tmpdir)
        device_lifecycle_test_bin = compile_test("device_lifecycle_error_handling_test.c", tmpdir,
                                                  test_dir=USB_MIDI_TEST_DIR)
        hotpath_dispatch_test_bin = compile_test("hotpath_nonblocking_dispatch_test.c", tmpdir,
                                                  test_dir=USB_MIDI_TEST_DIR)
        immediate_resubmit_test_bin = compile_test("immediate_resubmit_test.c", tmpdir,
                                                    test_dir=USB_MIDI_TEST_DIR)
        direct_dma_render_test_bin = compile_test("direct_to_dma_render_test.c", tmpdir,
                                                   test_dir=AUDIO_OUTPUT_TEST_DIR)

        print("\n=== QOA frame independence ===")
        ok &= run_test(qoa_test_bin, qoa_region_path, qoa_frame_size, qoa_samples_per_frame)

        print("\n=== ADPCM block independence ===")
        ok &= run_test(adpcm_test_bin, adpcm_block_path, adpcm_block_size, adpcm_samples_per_block)

        print("\n=== QOA hybrid raw-prefix + loop-wrap addressing (voice_engine.c mirror) ===")
        ok &= run_hybrid_loop_test(hybrid_test_bin, hybrid_fixture)

        print("\n=== Polyphony mix-scale smoothing (voice_engine.c mirror) ===")
        ok &= run_test(mix_scale_test_bin)

        print("\n=== Output de-hiss low-pass response (voice_engine.c mirror) ===")
        ok &= run_test(lowpass_test_bin)

        print("\n=== level_loop_amplitude swell fix (real function, not a mirror) ===")
        ok &= run_level_loop_amplitude_swell_test()

        print("\n=== trim_and_loop short-recording crash guard (real function, not a mirror) ===")
        ok &= run_trim_and_loop_short_recording_guard_test()

        print("\n=== MIDI channel routing + per-channel note-off matching (voice_engine.c mirror) ===")
        ok &= run_test(channel_routing_test_bin)

        print("\n=== USB device lifecycle error handling (usb_midi_host.c mirror) ===")
        ok &= run_test(device_lifecycle_test_bin)

        print("\n=== Hot-path non-blocking display dispatch (chord latency, main.c mirror) ===")
        ok &= run_test(hotpath_dispatch_test_bin)

        print("\n=== Immediate USB transfer resubmission (chord latency, usb_midi_host.c mirror) ===")
        ok &= run_test(immediate_resubmit_test_bin)

        print("\n=== Direct-to-DMA render, no task hop (chord latency, audio_output.c mirror) ===")
        ok &= run_test(direct_dma_render_test_bin)

    print("\n" + ("ALL PASS" if ok else "FAILURES ABOVE"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
