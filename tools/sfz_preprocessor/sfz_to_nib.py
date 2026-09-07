#!/usr/bin/env python3
"""SFZ -> .nib converter (docs/multi-instrument-soundbanks.md).

Takes a plain SFZ instrument (region list: sample/lokey/hikey/lovel/hivel/
pitch_keycenter -- the format Salamander Grand Piano V3's *original*
distribution uses, not the ARIA-extension repackaging with keyswitches/
macros/CC-triggered noise layers, which this parser deliberately does not
support) and produces a single .nib file: a fixed binary layout the device
reads directly at runtime with no string parsing, per
docs/multi-instrument-soundbanks.md.

Why trim + loop at all: Salamander's raw note recordings run 16-21 seconds
each (full natural decay); at 530 regions that's far more audio than is
worth keeping per note. Trimming to attack + a loop region, reducing
velocity layers, bounds the amount of audio per region -- with the loop
long enough (and level-corrected, see level_loop_amplitude) to not sound
obviously repetitive.

Why IMA ADPCM (via the vendored adpcm-xq encoder, adpcm-xq/): this went
through two other designs first, both abandoned for reasons worth knowing
if this ever needs revisiting:

1. Raw PCM on flash (the original design). Flash's ~14MB partition budget
   (partitions.csv) forced the loop short enough to sound audibly
   repetitive (docs/polyphony-latency-investigation.md's sibling decay-fix
   investigation traced exactly this symptom).
2. Ogg Vorbis on flash, decoded entirely to raw PCM in PSRAM once at boot.
   Fixed the loop-length problem (moved the budget from flash to PSRAM),
   but boot decode took ~14 seconds -- a real problem for a project whose
   whole premise is "instant-on" (CLAUDE.md) -- and Vorbis's frame-based,
   non-randomly-seekable structure means it can't be decoded on demand in
   the real-time render path either, so that bulk decode step was
   unavoidable as long as Vorbis was the format.

IMA ADPCM solves both: its ~4:1 ratio is worse than Vorbis's ~9:1, but
still leaves flash comfortably under budget, and -- the actual point --
it's block-structured (each block is independently decodable from a
stored predictor/step-index header) and each sample depends only on a
simple integer running state, not a frequency-domain transform. That
means the firmware (adpcm_decode.c) can decode it live, per-sample, in
voice_engine.c's real-time render path, straight out of the mmap'd flash
partition -- exactly like the original raw-PCM design's zero-copy access,
except each sample now costs a handful of integer ops instead of being
free. No boot-time bulk decode step at all. Loop points are snapped to
block boundaries at encode time (see trim_and_loop) so looping back to
loop_start is also O(1) at runtime: reset to that block's own stored
header, no replay-from-track-start needed.

adpcm-xq specifically (not a naive/reference IMA ADPCM encoder) buys
meaningfully better quality at the same 4:1 ratio via lookahead + dynamic
noise shaping -- the *decoder* is completely standard IMA ADPCM either
way, so this is a pure quality win with no runtime cost.
"""

import argparse
import re
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly

NIB_MAGIC = b"NIB1"
# '<' = little-endian, no padding -- must match the C struct the firmware
# reads byte-for-byte (see docs/multi-instrument-soundbanks.md).
# Adds adpcm_block_size (bytes/block, same for every region) versus the
# pre-ADPCM header -- the firmware needs it to compute block boundaries.
HEADER_FMT = "<4sIBBBH32sIIH"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
# key_lo,key_hi,vel_lo,vel_hi,root_key,
# left_offset,left_length,right_offset,right_length (all compressed-ADPCM
# byte offsets/lengths into the sample data blob -- left/right are two
# independently-encoded mono ADPCM streams, not one interleaved-stereo
# stream, so this project's decoder doesn't need to replicate Microsoft's
# stereo nibble-interleaving layout),
# sample_length (decoded frames), loop_start, loop_end (decoded-frame
# indices; loop_start is always an exact multiple of the ADPCM block's
# sample count -- see trim_and_loop).
REGION_FMT = "<BBBBBIIIIIII"
REGION_SIZE = struct.calcsize(REGION_FMT)

COMPRESSION_PCM = 0     # historical -- no longer produced by this tool
COMPRESSION_VORBIS = 1  # historical -- no longer produced by this tool
COMPRESSION_ADPCM = 2

ADPCM_XQ_DIR = Path(__file__).parent / "adpcm-xq"
ADPCM_XQ_BIN = ADPCM_XQ_DIR / "adpcm-xq"


def ensure_adpcm_xq_built():
    """Compiles the vendored adpcm-xq encoder on first use -- it's a
    build-machine tool, never shipped to the device, so there's no reason
    to require a separate manual build step over just doing it here."""
    if ADPCM_XQ_BIN.exists():
        return
    print("Building adpcm-xq encoder (one-time)...")
    subprocess.run(
        ["gcc", "-O2", "-o", str(ADPCM_XQ_BIN), "adpcm-xq.c", "adpcm-lib.c", "adpcm-dns.c", "-lm"],
        cwd=ADPCM_XQ_DIR, check=True)


def adpcm_samples_per_block(block_size_bytes: int) -> int:
    """Standard IMA ADPCM, mono: a 4-byte header (int16 initial predictor,
    uint8 initial step index, uint8 reserved) stores the first sample
    directly, then every remaining byte packs 2 nibble-encoded samples."""
    return 1 + (block_size_bytes - 4) * 2


def encode_adpcm_mono(pcm_i16_mono: np.ndarray, sample_rate: int, block_size_pow: int, lookahead: int) -> bytes:
    """Encodes mono int16 PCM to raw (headerless) IMA ADPCM via adpcm-xq.
    -r asks for raw little-endian output (just the block stream, no WAV
    container) -- exactly the bytes this project's own adpcm_decode.c
    parses directly out of the .nib file, no container parsing needed."""
    ensure_adpcm_xq_built()
    with tempfile.TemporaryDirectory() as tmpdir:
        in_path = Path(tmpdir) / "in.wav"
        out_path = Path(tmpdir) / "out.raw"
        with wave.open(str(in_path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(sample_rate)
            w.writeframes(pcm_i16_mono.tobytes())
        proc = subprocess.run(
            [str(ADPCM_XQ_BIN), "-q", "-y", "-r", f"-b{block_size_pow}", f"-{lookahead}",
             str(in_path), str(out_path)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise RuntimeError(f"adpcm-xq encode failed: {proc.stderr.decode(errors='replace')}")
        return out_path.read_bytes()


def parse_sfz(sfz_path: Path):
    """Returns a list of dicts, one per playable note region. Regions
    without pitch_keycenter (release-noise, pedal-noise, other non-note
    extras present even in the plain original library) are skipped --
    they're not referenced by lokey/hivel note ranges a MIDI key press
    would ever hit the same way, and this project doesn't model pedal/
    damper noise."""
    regions = []
    sfz_dir = sfz_path.parent
    with open(sfz_path, encoding="latin-1") as f:
        for line in f:
            if "<region>" not in line:
                continue
            opcodes = dict(re.findall(r"(\w+)=(\S+)", line))
            if "pitch_keycenter" not in opcodes or "sample" not in opcodes or "lovel" not in opcodes:
                continue
            sample_rel = opcodes["sample"].replace("\\", "/")
            if Path(sample_rel).name.lower().startswith("harm"):
                continue  # sympathetic-resonance layers, not a primary dynamic layer -- not modeled
            regions.append({
                "sample_path": sfz_dir / sample_rel,
                "lokey": int(opcodes["lokey"]),
                "hikey": int(opcodes["hikey"]),
                "lovel": int(opcodes["lovel"]),
                "hivel": int(opcodes.get("hivel", 127)),
                "pitch_keycenter": int(opcodes["pitch_keycenter"]),
            })
    return regions


def reduce_velocity_layers(regions_for_pitch, target_layer_count):
    """Picks target_layer_count regions spread across the original
    (typically 16-19) velocity layers, then re-maps their vel_lo/vel_hi so
    the reduced set still covers the full 1-127 range with no gaps --
    losing velocity *resolution*, not velocity *range*."""
    regions_for_pitch = sorted(regions_for_pitch, key=lambda r: r["lovel"])
    n = len(regions_for_pitch)
    if target_layer_count >= n:
        chosen = regions_for_pitch
    else:
        indices = [round(i * (n - 1) / (target_layer_count - 1)) for i in range(target_layer_count)]
        chosen = [regions_for_pitch[i] for i in sorted(set(indices))]

    layer_count = len(chosen)
    remapped = []
    for i, region in enumerate(chosen):
        vel_lo = 1 if i == 0 else round(i * 127 / layer_count) + 1
        vel_hi = 127 if i == layer_count - 1 else round((i + 1) * 127 / layer_count)
        remapped.append({**region, "vel_lo": vel_lo, "vel_hi": vel_hi})
    return remapped


def load_wav_stereo_float(path: Path):
    with wave.open(str(path), "rb") as w:
        channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        framerate = w.getframerate()
        frames = w.readframes(w.getnframes())
    if sampwidth != 2:
        raise ValueError(f"{path}: expected 16-bit samples, got {sampwidth * 8}-bit")
    data = np.frombuffer(frames, dtype="<i2").astype(np.float32).reshape(-1, channels)
    if channels == 1:
        data = np.repeat(data, 2, axis=1)  # mono source -> duplicate to stereo
    elif channels != 2:
        raise ValueError(f"{path}: expected mono or stereo, got {channels} channels")
    return data, framerate


def level_loop_amplitude(loop, window_frames):
    """A real piano string never stops decaying, so a loop's end is
    always genuinely quieter than its start -- looping straight back
    means every repeat jumps back UP in level, producing an audible
    pulse/stutter once per loop period on top of (and independent of)
    the waveform-shape crossfade below, which only smooths the seam's
    shape, not its level. This applies an *additional* downward-only
    decay across the loop, from its start down to its own already-
    decayed end level, so the loop's own internal level trend is flat
    (monotonic, never rising) by the time it reaches its end -- the
    actual decay a held note is heard to have across many loop repeats
    still comes entirely from voice_engine.c's real-time envelope,
    unaffected by this.

    Previously targeted the *geometric mean* of start/end level instead
    of the end outright, to avoid ever amplifying the tail -- but
    confirmed on real hardware that a geometric-mean target still pulls
    the end level *up* (as well as the start down), and a decaying
    signal whose level rises within every loop repeat is audible as a
    swell/pump once per loop period. Targeting the end level directly
    and never boosting it (see the min() below) fixes that: this only
    ever attenuates, matching what "additional decay" should mean.
    """
    window_frames = max(1, min(window_frames, loop.shape[0] // 4))
    start_rms = float(np.sqrt(np.mean(loop[:window_frames].astype(np.float64) ** 2)) + 1e-6)
    end_rms = float(np.sqrt(np.mean(loop[-window_frames:].astype(np.float64) ** 2)) + 1e-6)
    if end_rms < 1e-6:
        return loop  # near-silent tail -- leave alone rather than risk a huge gain blowup

    start_gain = min(1.0, end_rms / start_rms)  # never boost -- only ever an *additional* decay
    end_gain = 1.0

    # Ramp in the log domain -- a smooth exponential/linear-dB fade in
    # gain, matching how the ear perceives loudness change, rather than a
    # linear-amplitude ramp that would over-correct early and under-
    # correct late in the loop.
    n = loop.shape[0]
    log_ramp = np.linspace(np.log(start_gain), np.log(end_gain), n, dtype=np.float64)
    gain = np.exp(log_ramp).astype(np.float32).reshape(-1, 1)
    return loop * gain


def find_best_loop_points(audio_i16, target_loop_start, target_loop_end,
                           samples_per_block, search_radius_frames):
    """Fixed-time-offset loop points (the previous approach) means the
    crossfade in trim_and_loop blends two essentially arbitrary points of
    a decaying sinusoid -- if they don't happen to match in phase, the
    result is an audible click or "phasiness" at the seam no matter how
    long the crossfade window is (confirmed on real hardware: widening
    the loop and the crossfade both independently, neither fixed it).
    This searches nearby candidate points for a pair that actually
    matches in local level *and* slope direction, which is what the
    crossfade actually needs to have a chance of sounding seamless.

    loop_start candidates are restricted to exact ADPCM block boundaries
    (never post-hoc snapped afterward the way the previous design did --
    that snap could shift loop_start by up to half a block after this
    search already picked a well-phase-matched point, undoing it,
    especially for high notes where half a block can be many full
    periods of the fundamental). loop_end has no such constraint.
    """
    center_block = round(target_loop_start / samples_per_block)
    block_radius = max(1, search_radius_frames // samples_per_block)
    start_candidates = [
        (center_block + delta) * samples_per_block
        for delta in range(-block_radius, block_radius + 1)
        if (center_block + delta) >= 0
    ]

    end_lo = max(1, target_loop_end - search_radius_frames)
    end_hi = min(audio_i16.shape[0] - 2, target_loop_end + search_radius_frames)
    end_candidates = np.arange(end_lo, end_hi + 1)

    # Left channel as the phase reference -- close enough for a stereo
    # pair recorded from the same physical string via a spaced mic pair.
    mono = audio_i16[:, 0].astype(np.float64)
    slope = np.diff(mono)  # slope[i] = mono[i+1] - mono[i]

    e_vals = mono[end_candidates]
    e_slopes = slope[end_candidates]

    best_start, best_end, best_score = start_candidates[0], int(end_candidates[0]), None
    for loop_start in start_candidates:
        s_val = mono[loop_start]
        s_slope = slope[loop_start]
        # Slope-direction mismatch weighted well above amplitude mismatch --
        # a matching level with an inverted or mismatched slope is what
        # actually produces an audible click (the waveform visibly kinks at
        # the seam); level_loop_amplitude already corrects overall level
        # separately, so this search mainly needs to find matching phase.
        scores = np.abs(e_vals - s_val) + np.abs(e_slopes - s_slope) * 8.0
        idx = int(np.argmin(scores))
        if best_score is None or scores[idx] < best_score:
            best_score = scores[idx]
            best_start = loop_start
            best_end = int(end_candidates[idx])

    return best_start, best_end


def trim_and_loop(audio_i16, target_rate, attack_seconds, loop_seconds, crossfade_ms, samples_per_block):
    """audio_i16 is already resampled to target_rate (see build_nib) --
    the loop-point search below only makes sense running against the
    exact samples that will actually be encoded and played; searching at
    the original recording's sample rate and converting the result
    afterward reintroduces the same kind of post-hoc imprecision this
    whole approach exists to avoid."""
    target_loop_start = int(attack_seconds * target_rate)
    target_loop_end = target_loop_start + int(loop_seconds * target_rate)
    search_radius = max(samples_per_block, int(0.02 * target_rate))  # +-20ms

    if audio_i16.shape[0] < target_loop_end + search_radius:
        # Shorter recording than requested (shouldn't happen with
        # Salamander's 16-21s notes at these trim lengths, but guard
        # rather than crash).
        target_loop_end = min(target_loop_end, audio_i16.shape[0] - search_radius - 1)
        target_loop_start = min(target_loop_start, target_loop_end - samples_per_block)

    loop_start, loop_end = find_best_loop_points(
        audio_i16, target_loop_start, target_loop_end, samples_per_block, search_radius)

    attack = audio_i16[:loop_start].astype(np.float32)
    loop = audio_i16[loop_start:loop_end].astype(np.float32).copy()

    loop = level_loop_amplitude(loop, window_frames=int(0.05 * target_rate))

    # Crossfade the loop's tail into its head -- with loop_start/loop_end
    # now phase-matched by the search above, this only has to smooth a
    # small residual mismatch rather than paper over an arbitrary one.
    crossfade_len = int(crossfade_ms / 1000 * target_rate)
    crossfade_len = min(crossfade_len, loop.shape[0] // 4)
    if crossfade_len > 0:
        # Smoothstep (3t^2 - 2t^3), not a plain linear ramp -- a linear
        # fade's weighting has a slope *discontinuity* right at the
        # window edges (zero slope just outside the window, a constant
        # nonzero slope just inside it), which can itself add a subtle
        # audible artifact even once the blended values match at the
        # endpoints. Smoothstep has zero derivative at both t=0 and t=1,
        # so the fade eases in and out instead of switching on sharply.
        t = np.linspace(0.0, 1.0, crossfade_len, dtype=np.float64)
        fade = (3.0 * t ** 2 - 2.0 * t ** 3).astype(np.float32).reshape(-1, 1)
        tail_start = loop.shape[0] - crossfade_len
        loop[tail_start:] = loop[tail_start:] * (1.0 - fade) + loop[:crossfade_len] * fade

    combined = np.concatenate([attack, loop], axis=0)
    combined_i16 = np.clip(np.round(combined), -32768, 32767).astype("<i2")
    return combined_i16, loop_start, loop_end


def resample_and_quantize(audio, source_rate, target_rate):
    resampled = resample_poly(audio, target_rate, source_rate, axis=0)
    clipped = np.clip(resampled, -32768, 32767)
    return np.round(clipped).astype("<i2")


def build_nib(sfz_path, wav_root_override, output_path, display_name,
              velocity_layers, attack_seconds, loop_seconds, crossfade_ms, target_rate,
              adpcm_block_size_pow, adpcm_lookahead):
    regions = parse_sfz(sfz_path)
    if not regions:
        sys.exit(f"No note regions found in {sfz_path} -- wrong file, or an SFZ dialect this parser doesn't handle?")

    by_pitch = {}
    for r in regions:
        by_pitch.setdefault(r["pitch_keycenter"], []).append(r)

    # Recompute key ranges from actual recorded-pitch spacing instead of
    # trusting the source SFZ's own lokey/hikey -- confirmed on real
    # hardware that Salamander's original library has at least one real
    # gap (A3=57 covers 56-58, D#4=63 covers 62-64, leaving MIDI 59-61 /
    # B3-C4-C#4 with no region at all -- those keys were silent). Splitting
    # each pitch's range at the midpoint to its neighbors guarantees full,
    # gapless, non-overlapping coverage regardless of where the source
    # spacing is uneven.
    sorted_pitches = sorted(by_pitch.keys())
    key_range_for_pitch = {}
    for i, pitch in enumerate(sorted_pitches):
        lokey = 0 if i == 0 else (sorted_pitches[i - 1] + pitch) // 2 + 1
        hikey = 127 if i == len(sorted_pitches) - 1 else (pitch + sorted_pitches[i + 1]) // 2
        key_range_for_pitch[pitch] = (lokey, hikey)

    selected = []
    for pitch, group in sorted(by_pitch.items()):
        lokey, hikey = key_range_for_pitch[pitch]
        group = [{**r, "lokey": lokey, "hikey": hikey} for r in group]
        selected.extend(reduce_velocity_layers(group, velocity_layers))

    print(f"Parsed {len(regions)} note regions across {len(by_pitch)} recorded pitches; "
          f"selected {len(selected)} after reducing to {velocity_layers} velocity layers/pitch. "
          f"Key ranges recomputed for gapless coverage.")

    block_size_bytes = 1 << adpcm_block_size_pow
    samples_per_block = adpcm_samples_per_block(block_size_bytes)

    sample_blob = bytearray()
    region_entries = []
    decoded_total_frames = 0

    for i, region in enumerate(selected):
        wav_path = region["sample_path"]
        if wav_root_override:
            wav_path = wav_root_override / wav_path.name
        audio, source_rate = load_wav_stereo_float(wav_path)

        # Resample only as much of the source as trim_and_loop could
        # possibly need (attack + loop + its search radius, plus a
        # safety margin) rather than the full 16-21s recording -- the
        # loop-point search needs real resampled-at-target-rate audio to
        # search over (see trim_and_loop), but there's no reason to pay
        # for resampling the other 10+ seconds of every recording that
        # gets discarded immediately after.
        needed_seconds = attack_seconds + loop_seconds + 0.5
        needed_source_frames = int(needed_seconds * source_rate)
        audio_prefix = audio[:needed_source_frames]
        resampled = resample_and_quantize(audio_prefix, source_rate, target_rate)

        pcm, loop_start, loop_end = trim_and_loop(
            resampled, target_rate, attack_seconds, loop_seconds, crossfade_ms, samples_per_block)
        sample_length = pcm.shape[0]
        decoded_total_frames += sample_length

        left = np.ascontiguousarray(pcm[:, 0])
        right = np.ascontiguousarray(pcm[:, 1])
        left_bytes = encode_adpcm_mono(left, target_rate, adpcm_block_size_pow, adpcm_lookahead)
        right_bytes = encode_adpcm_mono(right, target_rate, adpcm_block_size_pow, adpcm_lookahead)

        left_offset = len(sample_blob)
        sample_blob += left_bytes
        right_offset = len(sample_blob)
        sample_blob += right_bytes

        region_entries.append(struct.pack(
            REGION_FMT,
            region["lokey"], region["hikey"],
            region["vel_lo"], region["vel_hi"],
            region["pitch_keycenter"],
            left_offset, len(left_bytes),
            right_offset, len(right_bytes),
            sample_length, loop_start, loop_end,
        ))

        if (i + 1) % 20 == 0 or i + 1 == len(selected):
            print(f"  processed {i + 1}/{len(selected)} regions, "
                  f"{len(sample_blob) / 1_000_000:.1f}MB compressed (ADPCM) so far", end="\r")
    print()

    region_table = b"".join(region_entries)
    region_table_offset = HEADER_SIZE
    unaligned_end = region_table_offset + len(region_table)
    sample_data_offset = (unaligned_end + 3) & ~3
    padding = b"\0" * (sample_data_offset - unaligned_end)

    header = struct.pack(
        HEADER_FMT,
        NIB_MAGIC,
        target_rate,
        16,  # bit_depth
        2,   # channels (stereo -- stored as 2 independent mono ADPCM streams per region)
        COMPRESSION_ADPCM,
        len(region_entries),
        display_name.encode("ascii")[:32].ljust(32, b"\0"),
        region_table_offset,
        sample_data_offset,
        block_size_bytes,
    )

    output_path.write_bytes(header + region_table + padding + sample_blob)
    total_mb = output_path.stat().st_size / 1_000_000
    equivalent_pcm_mb = decoded_total_frames * 2 * 2 / 1_000_000  # stereo int16 = 4 bytes/frame
    print(f"Wrote {output_path} -- {total_mb:.2f}MB IMA ADPCM on flash "
          f"(~{equivalent_pcm_mb:.1f}MB equivalent raw PCM, ~{equivalent_pcm_mb / total_mb:.1f}:1) "
          f"({len(region_entries)} regions, {target_rate}Hz/16-bit/stereo, "
          f"{block_size_bytes}B/block -> {samples_per_block} samples/block). "
          f"Decodes live in the real-time render path -- no boot-time bulk decode step.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sfz_file", type=Path, help="Path to the plain-original Salamander .sfz file")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output .nib file path")
    parser.add_argument("--display-name", default="Salamander Piano")
    parser.add_argument("--velocity-layers", type=int, default=3,
                         help="Velocity layers to keep per recorded pitch (source has 16-19)")
    # Rebalanced now that ADPCM (~4:1) makes flash cheap (~7MB for the
    # whole 88-key/3-velocity-layer bank, vs. a ~14MB budget): most of a
    # region's stored audio is the *real*, non-repeating decay
    # (attack-seconds, a couple of seconds -- despite the name, this is
    # everything before the loop starts, not just the transient).
    # loop-seconds started at 0.1 (100ms) on the theory that by 2s in, a
    # struck note's timbre has settled enough that a short loop wouldn't
    # be noticeable -- confirmed wrong on real hardware: ~100ms sits in
    # the worst spot for a loop, long enough to be heard as a distinct
    # repeating chunk (an audible ~10Hz flutter/stutter) but too short to
    # blur into a smooth drone the way a true few-ms micro-loop would.
    # 0.5s moves well clear of that zone (~2Hz repetition, not perceived
    # as a distinct event). crossfade-ms scaled back up to match
    # (trim_and_loop still caps it at loop_len//4 regardless).
    parser.add_argument("--attack-seconds", type=float, default=3.0)
    parser.add_argument("--loop-seconds", type=float, default=0.5)
    parser.add_argument("--crossfade-ms", type=float, default=100.0)
    parser.add_argument("--sample-rate", type=int, default=32000)
    # 8 is adpcm-xq's own hard minimum (n=8..15) -- the smallest block
    # available, and worth using now that loop-seconds is short (~100ms):
    # loop_start snaps to the nearest block boundary, so a smaller block
    # means less of that ~100ms loop gets eaten by snap error (256B ->
    # ~16ms/block; the old default of 512B -> ~32ms/block would be a
    # third of the whole loop).
    parser.add_argument("--adpcm-block-size-pow", type=int, default=8,
                         help="ADPCM block size = 2^n bytes (n=8..15, 8 is adpcm-xq's minimum). Smaller "
                              "blocks = finer loop-point snapping but slightly more header overhead. "
                              "Default 8 = 256 bytes/block.")
    parser.add_argument("--adpcm-lookahead", type=int, default=6,
                         help="adpcm-xq encoder lookahead depth (0-16, default 3 upstream) -- higher is "
                              "better quality at the same 4:1 ratio, slower to encode (build-machine only, "
                              "doesn't affect the device). 6 is a solid quality/build-time balance.")
    parser.add_argument("--wav-root", type=Path, default=None,
                         help="Override the directory samples are read from (defaults to paths relative to the SFZ file)")
    args = parser.parse_args()

    build_nib(args.sfz_file, args.wav_root, args.output, args.display_name,
              args.velocity_layers, args.attack_seconds, args.loop_seconds,
              args.crossfade_ms, args.sample_rate,
              args.adpcm_block_size_pow, args.adpcm_lookahead)


if __name__ == "__main__":
    main()
