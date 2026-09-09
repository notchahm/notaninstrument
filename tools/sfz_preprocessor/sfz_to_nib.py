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

QOA is now the default codec (--codec, qoa/qoa_decode.c), not ADPCM:
better compression ratio and SNR at an equal-or-finer loop-restart
granularity, the same "each unit independently decodable" property
(QOA frames instead of ADPCM blocks) that O(1) loop-wrap relies on, and
one real capability ADPCM has no equivalent for -- a per-region adaptive
raw-PCM prefix at the start of every note (find_qoa_raw_prefix_samples)
that measures each note's own attack-transient length and stores that
span losslessly instead of ever compressing it, directly reducing (not
just filtering after the fact) the quantization noise a fast, loud
transient injects. ADPCM (above) is kept as a simpler, longer-proven
fallback via --codec adpcm, not removed.
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
# sample count -- see trim_and_loop), qoa_raw_prefix_samples (QOA only,
# 0 for ADPCM regions -- see find_qoa_raw_prefix_samples).
REGION_FMT = "<BBBBBIIIIIIII"
REGION_SIZE = struct.calcsize(REGION_FMT)

COMPRESSION_PCM = 0     # historical -- no longer produced by this tool
COMPRESSION_VORBIS = 1  # historical -- no longer produced by this tool
COMPRESSION_ADPCM = 2
COMPRESSION_QOA = 3     # experimental -- see --codec; not the default

ADPCM_XQ_DIR = Path(__file__).parent / "adpcm-xq"
ADPCM_XQ_BIN = ADPCM_XQ_DIR / "adpcm-xq"

QOA_DIR = Path(__file__).parent / "qoa"
QOA_ENCODER_BIN = QOA_DIR / "qoa-encode"
QOA_DECODER_BIN = QOA_DIR / "qoa-decode"
# Stock QOA uses 256 slices/frame (5120 samples, ~160ms @ 32kHz) -- far
# coarser than this project's loop lengths (see qoa.h's now-guarded
# QOA_SLICES_PER_FRAME). 16 slices/frame (320 samples, ~10ms @ 32kHz) is
# finer than the ADPCM path's own 256-byte/505-sample blocks, so loop
# points lose no more precision than the ADPCM path already accepts.
QOA_SLICES_PER_FRAME = 16
QOA_LMS_LEN = 4          # fixed by the QOA spec, not tunable
QOA_CHANNELS = 2         # this project always encodes true interleaved stereo QOA

# Bounds for find_qoa_raw_prefix_samples()'s per-region search (in QOA
# frames -- see QOA_SLICES_PER_FRAME). Every region stores at least
# QOA_RAW_PREFIX_MIN_FRAMES worth of raw PCM at its start regardless of
# what the search finds, both to prime the LMS predictor (see
# encode_qoa_stereo) and as a floor confirmed necessary on real hardware
# even for the softest hits. QOA_RAW_PREFIX_MAX_FRAMES caps how far the
# search will grow the window for a pathologically slow-settling
# transient, bounding worst-case storage cost per region.
QOA_RAW_PREFIX_MIN_FRAMES = 5   # ~50ms @ this bank's settings
QOA_RAW_PREFIX_MAX_FRAMES = 50  # ~500ms -- covers the hardest hit measured (~400ms) with margin


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


def ensure_qoa_encoder_built():
    """Compiles this project's own qoa-encode CLI (qoa/qoa_encode_cli.c)
    on first use -- there's no official QOA command-line tool upstream,
    just the qoa.h library, so this project wrote a minimal one. Baking
    QOA_SLICES_PER_FRAME in at compile time (rather than making it a
    runtime parameter) keeps the CLI trivial; if this project ever wants
    multiple frame sizes it'd need a runtime option instead."""
    if QOA_ENCODER_BIN.exists():
        return
    print("Building qoa-encode encoder (one-time)...")
    subprocess.run(
        ["gcc", "-O2", f"-DQOA_SLICES_PER_FRAME={QOA_SLICES_PER_FRAME}",
         "-o", str(QOA_ENCODER_BIN), "qoa_encode_cli.c", "-lm"],
        cwd=QOA_DIR, check=True)


def ensure_qoa_decoder_built():
    """Compiles qoa/qoa_decode_cli.c on first use -- a thin CLI wrapper
    around the real production decoder (firmware/notaninstrument-p4/main/
    qoa_decode.c, included directly, not reimplemented) used only by
    find_qoa_raw_prefix_samples() to check what real hardware would
    actually decode for a candidate prefix width."""
    if QOA_DECODER_BIN.exists():
        return
    print("Building qoa-decode decoder (one-time)...")
    subprocess.run(
        ["gcc", "-O2", "-o", str(QOA_DECODER_BIN), "qoa_decode_cli.c"],
        cwd=QOA_DIR, check=True)


def decode_qoa_region(region_bytes: bytes, frame_size_bytes: int, samples_per_frame: int,
                       raw_prefix_samples: int, num_samples: int) -> np.ndarray:
    """Decodes a hybrid QOA region's bytes via the real production
    decoder (qoa/qoa_decode_cli.c) -- returns interleaved stereo int16."""
    ensure_qoa_decoder_built()
    with tempfile.TemporaryDirectory() as tmpdir:
        region_path = Path(tmpdir) / "region.bin"
        region_path.write_bytes(region_bytes)
        proc = subprocess.run(
            [str(QOA_DECODER_BIN), str(region_path), str(frame_size_bytes), str(samples_per_frame),
             str(raw_prefix_samples), str(num_samples)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise RuntimeError(f"qoa-decode failed: {proc.stderr.decode(errors='replace')}")
        return np.frombuffer(proc.stdout, dtype="<i2").reshape(-1, 2)


def _band_energy(sig: np.ndarray, rate: int, lo: float, hi: float) -> float:
    spec = np.abs(np.fft.rfft(sig.astype(np.float64) * np.hanning(len(sig))))
    freqs = np.fft.rfftfreq(len(sig), 1 / rate)
    mask = (freqs >= lo) & (freqs < hi)
    return float(np.sqrt(np.mean(spec[mask] ** 2))) if mask.any() else 0.0


def find_qoa_raw_prefix_samples(pcm: np.ndarray, sample_rate: int, samples_per_frame: int):
    """Measures how wide *this specific region's* raw-PCM prefix needs to
    be to reduce QOA's attack-transient quantization noise as much as
    practical, instead of using one fixed width for every note.

    Grew out of a real, measured problem: encoding+decoding a piano
    hammer strike's fast transient through QOA injects real high-
    frequency (8-16kHz) noise into the compressed portion. The natural
    first idea -- widen the raw prefix until the compressed portion's
    noise drops to near the clean signal's own (very low) level -- turns
    out not to work: measured directly (moving a decode/FFT window to
    just past each candidate boundary, so it never overlaps the
    always-exact raw prefix itself) that this noise doesn't have a clean
    cutoff point. It tracks the note's own decaying amplitude throughout
    -- softer by 500ms than at 50ms, but never converging near the clean
    reference for a hard-hit note even that far in, because QOA's
    quantization error scales with roughly how much signal is locally
    there to quantize, not just "is this still literally the attack." A
    ratio-to-clean-signal stopping rule can chase a target that keeps
    shrinking out of reach for hard-hit notes and never terminate below
    the configured max.

    What this searches for instead: the noise-vs-width curve is locally
    noisy (non-monotonic step to step -- confirmed directly, a single
    extra frame can occasionally make the measured noise a little worse
    before it keeps trending down) but has a clear, real diminishing-
    returns shape over its full range -- most of the achievable reduction
    happens early, with a long, flat, low-value tail out to the max
    width. A greedy "stop at the first step that doesn't improve enough"
    rule is not robust to that local noise (confirmed directly: it can
    stop after just one step on a false plateau, missing a further ~50%
    reduction still available further out). Evaluating a fixed grid of
    candidate widths and comparing each against the *asymptotic* result
    (the max-width candidate, averaged over its last couple of points to
    denoise it) is robust to that local wiggle instead: pick the
    smallest width that's already within ASYMPTOTE_TOLERANCE of the
    asymptote, rather than the first width that merely beats its
    immediate predecessor.

    Returns (accepted_prefix_samples, region_bytes) -- region_bytes is
    the exact accepted candidate's raw-prefix + QOA-frames bytes, so
    build_nib doesn't need to redundantly re-encode after this returns.
    """
    ASYMPTOTE_TOLERANCE = 0.20  # accept once within 20% of the asymptotic (max-width) noise level
    GRID_STEPS = 10             # candidate widths evenly spaced from min to max
    max_prefix_samples = min(QOA_RAW_PREFIX_MAX_FRAMES * samples_per_frame,
                              (pcm.shape[0] // samples_per_frame) * samples_per_frame - samples_per_frame)
    min_prefix_samples = QOA_RAW_PREFIX_MIN_FRAMES * samples_per_frame

    # A window measured strictly *after* each candidate's boundary --
    # measuring "has the newly-compressed content's own noise leveled
    # off," never diluted by the always-exact raw prefix itself.
    window_samples = samples_per_frame * 5  # ~50ms

    def measure_noise(prefix_samples):
        raw_prefix = np.ascontiguousarray(pcm[:prefix_samples])
        rest = pcm[prefix_samples:]
        padded_rest = pad_to_frame_multiple(rest, samples_per_frame)
        pcm_for_encoder = np.concatenate([raw_prefix, padded_rest], axis=0)
        qoa_bytes = encode_qoa_stereo(pcm_for_encoder, sample_rate, samples_per_frame, prefix_samples)
        region_bytes = raw_prefix.tobytes() + qoa_bytes
        window_end = min(prefix_samples + window_samples, pcm.shape[0])
        decoded = decode_qoa_region(region_bytes, qoa_frame_size_bytes(samples_per_frame),
                                     samples_per_frame, prefix_samples, window_end)
        decoded_window = decoded[prefix_samples:window_end, 0]
        noise = max(_band_energy(decoded_window, sample_rate, 8000, 12000),
                     _band_energy(decoded_window, sample_rate, 12000, 16000))
        return noise, region_bytes

    # Grid widths, snapped to frame boundaries, min to max inclusive.
    frame_span = (max_prefix_samples - min_prefix_samples) // samples_per_frame
    step_frames = max(1, frame_span // (GRID_STEPS - 1))
    widths = []
    w = min_prefix_samples
    while w < max_prefix_samples:
        widths.append(w)
        w += step_frames * samples_per_frame
    widths.append(max_prefix_samples)

    results = [measure_noise(w) for w in widths]
    noises = [r[0] for r in results]
    asymptote = float(np.mean(noises[-2:])) if len(noises) >= 2 else noises[-1]
    threshold = asymptote * (1.0 + ASYMPTOTE_TOLERANCE)

    for width, (noise, region_bytes) in zip(widths, results):
        if noise <= threshold:
            return width, region_bytes
    return widths[-1], results[-1][1]


def qoa_frame_size_bytes(samples_per_frame: int) -> int:
    """QOA frame layout (qoa.h): an 8-byte frame header, then per-channel
    LMS state (4 history + 4 weights, 2 bytes each = 16 bytes/channel),
    then one 8-byte slice per 20 samples per channel."""
    num_slices = samples_per_frame // 20
    return 8 + QOA_LMS_LEN * 4 * QOA_CHANNELS + num_slices * 8 * QOA_CHANNELS


def encode_qoa_stereo(pcm_with_prime_prefix: np.ndarray, sample_rate: int,
                       samples_per_frame: int, prime_samples: int) -> bytes:
    """Encodes interleaved stereo int16 PCM to raw (file-header-stripped)
    QOA frames via this project's own qoa-encode CLI (qoa/qoa_encode_cli.c).

    pcm_with_prime_prefix is [prime_samples raw samples, used only to
    train the encoder's LMS predictor on this specific note before real
    encoding starts] + [the actual audio to encode, whose length must
    already be an exact multiple of samples_per_frame -- see
    trim_and_loop/build_nib -- so every encoded frame is full-size; QOA's
    last frame is shorter than the rest when the sample count isn't an
    exact multiple, which would break this project's fixed-size
    `frame_index * frame_size_bytes` addressing in qoa_decode.c]. Returns
    only the encoded bytes for the post-prime span -- the prime prefix
    itself is never written to QOA output (sfz_to_nib.py's build_nib
    stores that span separately, as raw PCM, in the hybrid layout this
    priming exists to support -- see its own comment for the full why).
    """
    ensure_qoa_encoder_built()
    to_encode_len = pcm_with_prime_prefix.shape[0] - prime_samples
    if to_encode_len % samples_per_frame != 0:
        raise ValueError("post-prime frame count must be a multiple of samples_per_frame")
    with tempfile.TemporaryDirectory() as tmpdir:
        in_path = Path(tmpdir) / "in.wav"
        out_path = Path(tmpdir) / "out.qoaframes"
        with wave.open(str(in_path), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(sample_rate)
            w.writeframes(pcm_with_prime_prefix.tobytes())
        proc = subprocess.run(
            [str(QOA_ENCODER_BIN), str(in_path), str(out_path), str(prime_samples)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise RuntimeError(f"qoa-encode failed: {proc.stderr.decode(errors='replace')}")
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


def level_loop_amplitude(loop, window_frames, extra_decay_db=0.0):
    """A real piano string never stops decaying, so a loop's end is
    always genuinely quieter than its start -- looping straight back
    means every repeat jumps back UP in level, producing an audible
    pulse/stutter once per loop period on top of (and independent of)
    the waveform-shape crossfade below, which only smooths the seam's
    shape, not its level. This applies an *additional* downward-only
    decay across the loop, pointwise, so the loop's own internal level
    trend never rises anywhere in its span and both ends land at
    approximately the same level (no jump at the wrap) -- the actual
    decay a held note is heard to have across many loop repeats still
    comes entirely from voice_engine.c's real-time envelope, unaffected
    by this.

    History: originally targeted the *geometric mean* of start/end level,
    which still pulled the end level *up* -- audible as a swell/pump once
    per loop period. Fixed by targeting the end level directly and never
    boosting -- but that version assumed the loop's *entire* natural
    decay between its two measured endpoints follows one clean
    exponential curve, and computed a single continuous compensating gain
    ramp (log-linear from a start-derived value to 1.0) to cancel it.
    Confirmed on real hardware that real piano decay doesn't actually
    follow that shape -- it's front-loaded (fast initial decay, a
    flatter tail), not a constant proportional rate throughout a half-
    second span. The assumed-exponential ramp then "gives back" gain on
    a fixed schedule regardless of what the real signal is actually
    doing -- once the real decay has already flattened out early but the
    ramp keeps restoring gain toward 1.0 on its way to the loop's end,
    the product visibly *rises*: the same swell this function exists to
    prevent, just moved from "geometric-mean overshoot" to "wrong
    assumed decay shape."

    This version makes no assumption about the decay's shape at all: it
    measures the loop's *actual* envelope via a sliding RMS window, then
    computes gain pointwise as end_rms/envelope(t). A genuinely front-
    loaded decay gets pulled down hard early (where it's still loud) and
    barely touched late (where it's already near end_rms) -- exactly the
    opposite of the previous version's behavior, and the reason this
    fixes the swell in most of the loop's span.

    One residual case the never-boost version (an earlier draft of this
    docstring/function) still couldn't fully rule out: a real decaying
    tone can briefly dip *below* its own eventual end_rms trend partway
    through (string/soundboard resonance, beating between courses) and
    then partially recover before settling back to end_rms by the loop's
    true end. Clamping gain to never exceed 1.0 leaves that recovery
    untouched (correctly not attenuating a point already below target),
    but the recovery itself is a real, if small, rise -- confirmed on
    real hardware as a lingering "slight swell," most noticeable on long
    holds of quiet (low-velocity) notes once other, larger artifacts were
    fixed and this became the next-quietest thing left to hear.
    There's no way to guarantee *both* zero rise anywhere in the loop
    *and* zero jump at the wrap seam using only attenuation: a pass that
    never rises must end no higher than it started, and matching that
    same (lower) end level at the next repeat's start requires it to
    *start* there too -- forcing a perfectly flat pass, which requires
    being willing to nudge a brief dip back up when that's what flat
    actually requires. Confirmed this is a small, local correction here
    (measured on real regions: well under 2dB, applied to roughly a
    tenth of the loop), nothing like the old geometric-mean bug's
    large-scale, whole-second-half overboost -- MAX_BOOST_DB still caps
    it defensively in case some region this hasn't been checked against
    needs more.

    extra_decay_db pulls the flat target itself down below the measured
    end_rms by that many dB, rather than trying to match it exactly.
    This doesn't change the *relative* size of the residual wobble
    above (that's governed by the sliding-RMS window's own measurement
    resolution, not this) -- but it does make that wobble quieter in
    absolute terms, and gives real headroom below the loop's natural
    peak for it to happen in without brushing back up against the level
    a listener's ear anchored to right before the loop started.
    """
    MAX_BOOST_DB = 3.0
    max_boost_linear = 10 ** (MAX_BOOST_DB / 20.0)

    window_frames = max(1, min(window_frames, loop.shape[0] // 4))
    n = loop.shape[0]
    mono = loop.astype(np.float64).mean(axis=1)

    end_rms = float(np.sqrt(np.mean(mono[-window_frames:] ** 2)) + 1e-6)
    if end_rms < 1e-6:
        return loop  # near-silent tail -- leave alone rather than risk a huge gain blowup
    target_rms = end_rms * 10 ** (-extra_decay_db / 20.0)

    # Sliding RMS envelope of the loop's actual content. Edge-padded by
    # replicating the boundary samples (not zero-padding) so the
    # window's own edges don't artificially read as quieter than they
    # are, which would otherwise show up as a spuriously inflated
    # (near-1.0) gain right at the loop's start and end.
    half = window_frames // 2
    padded = np.pad(mono, (half, window_frames - half), mode="edge")
    squared = padded ** 2
    cumsum = np.cumsum(np.insert(squared, 0, 0.0))
    mean_sq = (cumsum[window_frames:] - cumsum[:-window_frames]) / window_frames
    envelope = np.sqrt(np.maximum(mean_sq[:n], 0.0)) + 1e-6

    gain = np.minimum(max_boost_linear, target_rms / envelope).astype(np.float32).reshape(-1, 1)
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


def trim_and_loop(audio_i16, target_rate, attack_seconds, loop_seconds, crossfade_ms, samples_per_block,
                   loop_extra_decay_db=0.0):
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
        # Shorter recording than requested. Confirmed this actually
        # happens, not just theoretical: a handful of the highest piano
        # keys have naturally short recordings (Salamander's own
        # softest/v1 layer for A7 is only 2.62s) -- short enough that a
        # larger attack-seconds request leaves this guard's old version
        # no margin at all, producing an inverted or empty (loop_start
        # >= loop_end) window instead of the graceful fallback its
        # comment promised. Clamp end to what's actually available, then
        # derive start from a loop length that's the smaller of the
        # requested loop_seconds and half of what's left -- guaranteeing
        # a valid, non-empty, if short, loop window rather than trusting
        # the original request still fits.
        usable_end = max(0, audio_i16.shape[0] - search_radius - 1)
        requested_loop_len = int(loop_seconds * target_rate)
        min_loop_len = max(samples_per_block, min(requested_loop_len, usable_end // 2))
        target_loop_end = max(min_loop_len, usable_end)
        target_loop_start = max(0, target_loop_end - min_loop_len)

    loop_start, loop_end = find_best_loop_points(
        audio_i16, target_loop_start, target_loop_end, samples_per_block, search_radius)

    attack = audio_i16[:loop_start].astype(np.float32)
    loop = audio_i16[loop_start:loop_end].astype(np.float32).copy()

    loop = level_loop_amplitude(loop, window_frames=int(0.05 * target_rate), extra_decay_db=loop_extra_decay_db)

    # Crossfade the loop's tail into its head -- with loop_start/loop_end
    # now phase-matched by the search above, this only has to smooth a
    # small residual mismatch rather than paper over an arbitrary one.
    crossfade_len = int(crossfade_ms / 1000 * target_rate)
    # Widened from //4 to //3 of the loop length after real-hardware
    # feedback asking for wider cross-fading -- the old cap (~125ms at
    # this bank's 0.5s loop-seconds) left the previous 100ms default
    # crossfade-ms right up against its ceiling with no real headroom
    # to widen further.
    crossfade_len = min(crossfade_len, loop.shape[0] // 3)
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


def pad_to_frame_multiple(pcm_i16_stereo, samples_per_frame):
    """QOA's fixed-frame-size addressing (qoa_decode.c: frame_index *
    frame_size_bytes) requires every frame in a region to be full-size --
    upstream QOA instead lets the last frame be shorter when the sample
    count isn't an exact multiple. loop_start is already frame-aligned
    (find_best_loop_points only proposes frame-boundary candidates) so
    padding here only ever extends past loop_end/sample_length, which
    normal playback never reaches (the one-frame lookahead past the end
    that read_voice_frame tolerates is the only read that could touch it,
    and its result is discarded there in favor of the loop-start
    substitution -- see voice_engine.c)."""
    remainder = pcm_i16_stereo.shape[0] % samples_per_frame
    if remainder == 0:
        return pcm_i16_stereo
    pad_count = samples_per_frame - remainder
    pad = np.repeat(pcm_i16_stereo[-1:], pad_count, axis=0)
    return np.concatenate([pcm_i16_stereo, pad], axis=0)


def build_nib(sfz_path, wav_root_override, output_path, display_name,
              velocity_layers, attack_seconds, loop_seconds, crossfade_ms, target_rate,
              adpcm_block_size_pow, adpcm_lookahead, codec,
              loop_extra_decay_db_soft=0.0, loop_extra_decay_db_hard=0.0):
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

    if codec == "adpcm":
        block_size_bytes = 1 << adpcm_block_size_pow
        samples_per_unit = adpcm_samples_per_block(block_size_bytes)
        compression = COMPRESSION_ADPCM
        header_block_size_field = block_size_bytes
    elif codec == "qoa":
        samples_per_unit = QOA_SLICES_PER_FRAME * 20
        compression = COMPRESSION_QOA
        header_block_size_field = qoa_frame_size_bytes(samples_per_unit)
    else:
        raise ValueError(f"unknown codec {codec!r}")

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

        # Velocity-scaled, not a single flat value for every region: more
        # decay for soft notes (real headroom to spare, nothing else
        # about them draws attention away from the loop), less for hard
        # notes (confirmed on real hardware -- the same flat value that
        # helped a soft note's loop sounded artificially dulled/quiet on
        # a hard-hit one, which should stay prominent). Uses the
        # region's own vel_lo/vel_hi midpoint as its representative
        # velocity, same soft/hard linear interpolation style as
        # voice_engine.c's real-time HELD_DECAY_SECONDS_SOFT/HARD.
        region_velocity = (region["vel_lo"] + region["vel_hi"]) / 2.0
        velocity_frac = (region_velocity - 1) / 126.0  # 0 (softest) .. 1 (hardest)
        region_extra_decay_db = (loop_extra_decay_db_soft -
                                  velocity_frac * (loop_extra_decay_db_soft - loop_extra_decay_db_hard))

        pcm, loop_start, loop_end = trim_and_loop(
            resampled, target_rate, attack_seconds, loop_seconds, crossfade_ms, samples_per_unit,
            loop_extra_decay_db=region_extra_decay_db)
        sample_length = pcm.shape[0]
        decoded_total_frames += sample_length

        qoa_raw_prefix_samples = 0  # ADPCM regions don't use this field
        if codec == "adpcm":
            left = np.ascontiguousarray(pcm[:, 0])
            right = np.ascontiguousarray(pcm[:, 1])
            left_bytes = encode_adpcm_mono(left, target_rate, adpcm_block_size_pow, adpcm_lookahead)
            right_bytes = encode_adpcm_mono(right, target_rate, adpcm_block_size_pow, adpcm_lookahead)
            left_offset = len(sample_blob)
            sample_blob += left_bytes
            right_offset = len(sample_blob)
            sample_blob += right_bytes
            right_length = len(right_bytes)
        else:  # qoa -- one genuinely interleaved-stereo stream, not two mono ones
            # Hybrid: a per-region raw-PCM prefix (see
            # find_qoa_raw_prefix_samples), followed immediately by QOA
            # frames for the rest -- and that same raw span also *primes*
            # the QOA encoder's LMS predictor (see encode_qoa_stereo/
            # qoa_encode_cli.c) before real encoding starts. Fixes an
            # audible click at every note's attack: QOA has no ADPCM-
            # style "first sample stored directly" shortcut (every
            # sample, including a frame's first, comes from a 4-tap LMS
            # predictor that otherwise starts each region cold), and a
            # piano hammer strike's fast, loud transient produces real,
            # audible quantization error there even once primed --
            # confirmed on real hardware and measured directly (QOA's
            # per-slice-independent scale-factor search has no
            # adpcm-xq-style cross-sample noise shaping, so its error is
            # audibly jitterier there despite similar average magnitude,
            # which is what a "click" actually is). The prefix width
            # itself is measured per-region, not a shared fixed constant
            # (see find_qoa_raw_prefix_samples's own docstring for why),
            # and stored in the region record (qoa_raw_prefix_samples)
            # since voice_engine.c needs it to know where the raw/QOA
            # boundary actually is for this specific region.
            qoa_raw_prefix_samples, left_bytes = find_qoa_raw_prefix_samples(pcm, target_rate, samples_per_unit)
            left_offset = len(sample_blob)
            sample_blob += left_bytes
            right_offset = 0
            right_length = 0

        region_entries.append(struct.pack(
            REGION_FMT,
            region["lokey"], region["hikey"],
            region["vel_lo"], region["vel_hi"],
            region["pitch_keycenter"],
            left_offset, len(left_bytes),
            right_offset, right_length,
            sample_length, loop_start, loop_end,
            qoa_raw_prefix_samples,
        ))

        if (i + 1) % 20 == 0 or i + 1 == len(selected):
            print(f"  processed {i + 1}/{len(selected)} regions, "
                  f"{len(sample_blob) / 1_000_000:.1f}MB compressed ({codec}) so far", end="\r")
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
        2,   # channels (stereo -- see codec: ADPCM stores 2 independent mono streams,
             # QOA stores one genuinely interleaved-stereo stream)
        compression,
        len(region_entries),
        display_name.encode("ascii")[:32].ljust(32, b"\0"),
        region_table_offset,
        sample_data_offset,
        header_block_size_field,
    )

    output_path.write_bytes(header + region_table + padding + sample_blob)
    total_mb = output_path.stat().st_size / 1_000_000
    equivalent_pcm_mb = decoded_total_frames * 2 * 2 / 1_000_000  # stereo int16 = 4 bytes/frame
    codec_label = "IMA ADPCM" if codec == "adpcm" else "QOA"
    print(f"Wrote {output_path} -- {total_mb:.2f}MB {codec_label} on flash "
          f"(~{equivalent_pcm_mb:.1f}MB equivalent raw PCM, ~{equivalent_pcm_mb / total_mb:.1f}:1) "
          f"({len(region_entries)} regions, {target_rate}Hz/16-bit/stereo, "
          f"{header_block_size_field}B/{'block' if codec == 'adpcm' else 'frame'} -> "
          f"{samples_per_unit} samples). "
          f"Decodes live in the real-time render path -- no boot-time bulk decode step.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sfz_file", type=Path, help="Path to the plain-original Salamander .sfz file")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output .nib file path")
    parser.add_argument("--display-name", default="Salamander Piano")
    # qoa is now the default/primary path: better compression ratio and
    # SNR than ADPCM at an equal or finer loop-restart granularity, and
    # (unlike ADPCM) supports a per-region adaptive raw-PCM attack
    # prefix (find_qoa_raw_prefix_samples) that ADPCM has no equivalent
    # for. adpcm (adpcm_decode.c) is kept alongside it as a simpler,
    # longer-proven fallback, not removed.
    parser.add_argument("--codec", choices=["adpcm", "qoa"], default="qoa",
                         help="Audio compression for the sample data blob (default: qoa)")
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
    # (trim_and_loop caps it at loop_len//3 regardless -- widened from
    # //4 after real-hardware feedback asking for wider cross-fading).
    parser.add_argument("--attack-seconds", type=float, default=3.0)
    parser.add_argument("--loop-seconds", type=float, default=0.5)
    parser.add_argument("--crossfade-ms", type=float, default=150.0)
    # Extra headroom below the loop's own measured end level -- see
    # level_loop_amplitude's docstring. Doesn't change the *relative*
    # size of the small residual wobble a sliding-RMS-window level
    # correction can leave behind (that's a measurement-resolution
    # floor, not something this controls), but makes it quieter in
    # absolute terms and gives real headroom below the loop's natural
    # peak. Confirmed directly that tightening the boost cap or
    # shrinking the measurement window don't reliably help (noisy,
    # inconsistent effects across real notes) -- this does, predictably.
    #
    # Velocity-scaled (soft/hard, same interpolation style as
    # voice_engine.c's real-time HELD_DECAY_SECONDS_SOFT/HARD), not one
    # flat value for every region: confirmed on real hardware that a
    # flat value strong enough to help a soft note's loop sounded
    # artificially dulled/quiet on a hard-hit one.
    # Dialed back from 2.5/0.5dB after real-hardware feedback that even
    # the velocity-scaled version was still slightly overaggressive.
    parser.add_argument("--loop-extra-decay-db-soft", type=float, default=1.8,
                         help="Extra dB of headroom for the softest (velocity 1) regions (0 disables). Default 1.8.")
    parser.add_argument("--loop-extra-decay-db-hard", type=float, default=0.3,
                         help="Extra dB of headroom for the hardest (velocity 127) regions (0 disables). Default 0.3.")
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
              args.adpcm_block_size_pow, args.adpcm_lookahead, args.codec,
              loop_extra_decay_db_soft=args.loop_extra_decay_db_soft,
              loop_extra_decay_db_hard=args.loop_extra_decay_db_hard)


if __name__ == "__main__":
    main()
