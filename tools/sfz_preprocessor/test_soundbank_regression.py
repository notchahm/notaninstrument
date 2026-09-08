#!/usr/bin/env python3
"""Regression test for the .nib codec pipeline's frame/block-independence
guarantee -- the property voice_engine.c's O(1) loop-wrap relies on (see
docs/polyphony-latency-investigation.md and firmware/spike-usb-midi-idf/
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
FIRMWARE_MAIN = REPO_ROOT / "firmware" / "spike-usb-midi-idf" / "main"
TEST_DIR = Path(__file__).parent.parent.parent / "firmware" / "spike-usb-midi-idf" / "test" / "voice_engine"

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


def compile_test(src_name, tmpdir):
    src = TEST_DIR / src_name
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

    print("\n" + ("ALL PASS" if ok else "FAILURES ABOVE"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
