#!/usr/bin/env python3
"""muldjordkit_to_nib.py -- converts a single-mic (overhead pair) slice of
sfzinstruments/DrumGizmo.MuldjordKit
(github.com/sfzinstruments/DrumGizmo.MuldjordKit) into a .nib soundbank
for this project's built-in drum kit (channel 10 -- see voice_engine.c's
DRUM_MIDI_CHANNEL). Replaces virtuosity_to_nib.py's output as the shipped
drumkit content (see docs/second-instrument-drums-todo.md) -- a real Tama
Superstar rock kit rather than virtuosity_drums' jazz kit.

License: CC-BY 4.0 (Creative Commons Attribution), NOT public domain like
virtuosity_drums -- see CREDITS.md at the repo root for the required
attribution (MuldjordKit by Lars Muldjord, muldjord.com / drumgizmo.org;
SFZ port by kinwie).

Why a separate script from virtuosity_to_nib.py, despite both targeting
one-shot percussion: this library's own SFZ dialect is a different (and in
some ways simpler) shape. It has no <master>/<group> opcode-inheritance
cascade to track and no CC-gated duplicate regions to filter -- but it DOES
wrap its real region data (Data/region/<Piece>.txt) in an elaborate
ARIA/DrumGizmo live-mixing-console dialect (Data/stereo/*.txt,
Data/mic/*.txt: label_cc/set_hdcc/amplitude_oncc/pan_oncc opcodes driving a
whole cross-mic "bleed" simulation) that's entirely irrelevant here -- this
device has no runtime CC mixing, so this script reads Data/region/*.txt
directly rather than parsing the Stereo/Multi wrapper SFZ files that exist
to drive that mixing console. Reuses sfz_to_nib.py's low-level,
codec-agnostic primitives (encode functions, resampling, the .nib
header/region struct formats, reduce_velocity_layers) and
virtuosity_to_nib.py's load_sample_flac_or_wav() (FLAC decode via ffmpeg)
rather than duplicating them.

Scope (see docs/second-instrument-drums-todo.md and this session's
planning for the full rationale) -- deliberately NOT the full library:
- Single mic position: the Overhead stereo pair (OHL/OHR), the only mic
  position with full-kit coverage (every piece has its own dedicated close
  mic instead, e.g. KdrumL/Snare_top/Tom1 -- no single one covers every
  piece the way Overhead does). Assembled as a genuine stereo pair: OHL's
  recording of a hit becomes the left channel, OHR's the right -- not
  derived from any pan/CC logic, since the library already recorded these
  as two separate, spatially-real mic feeds.
- Core kit only: the 16 keys defined in Data/keymap.txt (2 kick zones,
  snare, 4 toms, hi-hat open/closed, 2 ride-cymbal zones each with a tip
  and bell key, 2 crash cymbals, china) -- not SnareRest (snares-off
  resonance layer) or the cymbal/hi-hat choke *trigger* keys (separate
  notes whose only job is muting a still-ringing cymbal). Enforced by a
  fixed piece list (PIECES below) rather than dynamic filtering, since
  this script hand-picks which Data/region/*.txt files to read instead of
  parsing a "whole program" SFZ that might pull in extras.
- One round-robin variant per (key, velocity layer) -- the lowest
  seq_position present -- and a reduced velocity-layer count via the
  existing reduce_velocity_layers(), same as virtuosity and piano.
- One-shot playback only: loop_start = loop_end = 0, matching
  Data/global.txt's loop_mode=one_shot default for the whole library.
- Choke groups (off_by= opcodes on ride/crash/hi-hat regions, for
  realistic cymbal muting) are out of scope -- this project's voice engine
  has no choke-group mechanism, so a ringing cymbal simply isn't cut off
  by a later hit, the same accepted trade-off virtuosity's hi-hat choke
  already made.
"""

import re
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import sfz_to_nib as nib  # noqa: E402
from virtuosity_to_nib import load_sample_flac_or_wav  # noqa: E402  (FLAC decode via ffmpeg, reused as-is)

TARGET_SAMPLE_RATE = 32000
MAX_SAMPLE_SECONDS = 1.0
FADEOUT_SECONDS = 0.05

DEFINE_RE = re.compile(r"#define\s+(\$\w+)\s+(\S+)")
REGION_LINE_RE = re.compile(r"^<region>\s+(.*)$", re.MULTILINE)
OPCODE_RE = re.compile(r"(\S+)=(\S+)")
MACRO_TOKEN_RE = re.compile(r"\$\w+")

# (region file stem, key macro name OR a literal int key). Keys normally
# come straight from Data/keymap.txt -- see module docstring; no
# hand-typed key numbers for those, they're looked up from the parsed
# macros in main().
#
# Notes 37-43 are a deliberate exception, overriding this kit's own
# GM-ish keymap: the user's physical pad controller sends a contiguous
# run of notes in that range, one per pad, so a scattered GM-style layout
# (kick 35/36, snare 38, hi-hat 42/46, ...) left several of those pads
# silent even though every *piece* they'd want was already mapped
# somewhere else. Requested (2026-09-08) layout for that contiguous run:
#   37 snare, 38 closed hi-hat, 39 open hi-hat, 40 a hanging tom
#   (Tom1, the highest-pitched of this kit's three hanging toms -- not
#   specified further), 41 floor tom, 42 ride (RideR's tip -- the "main"
#   ride per this kit's own RideR/RideL split, see module docstring),
#   43 crash (CrashL -- either would do, this kit has two).
# Snare and HihatClosed's *natural* keys (38 and 42 respectively) are
# superseded by this -- their entries below use the literal 37/38 instead
# of their own $key_sn_hit/$key_hh_cl macros, so those old note numbers
# now play what's listed above instead of colliding with it. Every other
# piece keeps its natural GM-ish key IN ADDITION to picking up a second,
# duplicate key in 37-43 where requested (e.g. Tom1 plays at both 40 and
# its natural 48; HihatOpen at both 39 and its natural 46) -- duplicating
# a piece onto a second key is harmless (same sample, two ways to trigger
# it), so nothing already working elsewhere on the keyboard is lost.
PIECES = [
    ("KdrumL", "$key_kl"),
    ("KdrumR", "$key_kr"),
    ("Snare", 37),
    ("HihatClosed", 38),
    ("HihatOpen", 39),  # duplicate; also at its natural $key_hh_op (46)
    ("Tom1", 40),  # duplicate; also at its natural $key_t1 (48) -- "a hanging tom"
    ("Tom1", "$key_t1"),
    ("Tom2", "$key_t2"),
    ("Tom3", "$key_t3"),
    ("Tom4", "$key_t4"),  # 41 -- already "floor tom", no change needed here
    ("RideR", 42),  # duplicate; also at its natural $key_rr_tip (51)
    ("CrashL", 43),  # duplicate; also at its natural $key_cl (49)
    ("HihatOpen", "$key_hh_op"),
    ("RideL", "$key_rl_tip"),
    ("RideLBell", "$key_rl_bel"),
    ("RideR", "$key_rr_tip"),
    ("RideRBell", "$key_rr_bel"),
    ("CrashL", "$key_cl"),
    ("CrashR", "$key_cr"),
    ("China", "$key_ch"),
]


def load_macros(*paths: Path) -> dict:
    macros = {}
    for path in paths:
        macros.update(DEFINE_RE.findall(path.read_text(errors="replace")))
    return macros


def substitute(text: str, macros: dict) -> str:
    return MACRO_TOKEN_RE.sub(lambda m: macros.get(m.group(0), m.group(0)), text)


def parse_region_file(path: Path, macros: dict) -> list:
    """Returns the <region> opcode dicts in path, with every $macro token
    (including the file's own local #define $instr, folded into macros
    before calling) already substituted."""
    text = path.read_text(errors="replace")
    regions = []
    for line in REGION_LINE_RE.findall(text):
        opcodes = dict(OPCODE_RE.findall(substitute(line, macros)))
        regions.append(opcodes)
    return regions


def normalize_peak(audio: np.ndarray, target_peak: float = 8000.0) -> np.ndarray:
    """Scales audio so its peak absolute sample reaches target_peak.

    Necessary because of a real, measured problem with using the overhead
    mic alone: overhead-mic levels vary wildly by how close each piece
    physically sits to the overhead mics, completely independent of how
    hard it was actually hit -- confirmed directly against this library's
    raw source files, e.g. a hard kick hit peaks at ~562 (of 32768) while
    a hard crash hit peaks at ~4160, roughly 7x louder, at similar
    striking intensity. In the full DrumGizmo instrument this is exactly
    what the CC-driven per-piece volume faders (out of scope here, see
    module docstring) exist to correct; without them, playing this kit
    back at raw relative levels would produce a cymbal-dominated mix with
    a buried kick/snare -- the opposite of the "solid" kit this is
    supposed to replace virtuosity_drums with. Normalizing each region's
    peak independently gives every piece consistent, punchy impact
    instead, at the cost of the (otherwise uncorrectable) natural
    mic-distance balance a real mixed recording would have.

    target_peak of 8000 (~24% of int16 full scale), not something close to
    full scale: confirmed on real hardware that an initial 28000 target
    caused audible oversaturation -- voice_engine.c's mixer only reaches
    its 1/sqrt(active-voices) headroom compensation via a ~5ms one-pole
    smoother (mix_scale_smoothing, tuned for piano chords ramping in), so
    a real drum hit's transient -- which peaks in its very first output
    sample, before the smoother has reacted at all -- can sum against
    other simultaneously-struck pieces (kick+snare+hi-hat is an entirely
    normal single hit) essentially uncompensated. 8000 keeps even 4 pieces
    peaking in perfect alignment (32000, worst case, in practice far less
    likely since independent transients rarely align to the same exact
    sample) under int16 range with no compensation at all, while still
    being ~14x louder than the kick's original raw level -- enough to fix
    the buried-kick balance problem without reintroducing clipping."""
    peak = np.abs(audio).max()
    if peak > 0:
        audio = audio * (target_peak / peak)
    return audio


def load_oh_stereo_pair(samples_root: Path, sample_pattern: str, macros: dict):
    """Resolves sample_pattern (e.g. "$instr/3-$instr-$mic.$ext") once
    with $mic=OHL and once with $mic=OHR, decodes both mono FLACs, and
    stacks them into one (N, 2) float array -- left channel from OHL,
    right from OHR, per the user's explicit "CH1/CH2 as left and right"
    instruction. Trims to the shorter of the two if their lengths ever
    differ (they're independent recordings of the same physical hit, so
    a stray sample or two of difference is possible)."""
    left_path = samples_root / substitute(sample_pattern, {**macros, "$mic": "OHL"})
    right_path = samples_root / substitute(sample_pattern, {**macros, "$mic": "OHR"})
    left_audio, left_rate = load_sample_flac_or_wav(left_path)
    right_audio, right_rate = load_sample_flac_or_wav(right_path)
    assert left_rate == right_rate, f"sample rate mismatch: {left_path} ({left_rate}) vs {right_path} ({right_rate})"
    n = min(left_audio.shape[0], right_audio.shape[0])
    stereo = np.empty((n, 2), dtype=left_audio.dtype)
    stereo[:, 0] = left_audio[:n, 0]
    stereo[:, 1] = right_audio[:n, 0]
    return stereo, left_rate


def build_drumkit_nib(kit_root: Path, output_path: Path, display_name: str,
                       velocity_layers: int, codec: str,
                       adpcm_block_size_pow: int, adpcm_lookahead: int):
    data_root = kit_root / "Data"
    samples_root = kit_root / "Samples"
    region_root = data_root / "region"

    global_macros = load_macros(data_root / "keymap.txt", data_root / "macro.txt")

    by_key = {}
    for piece_name, key_macro in PIECES:
        key = key_macro if isinstance(key_macro, int) else int(global_macros[key_macro])
        instr_line = (region_root / f"{piece_name}.txt").read_text(errors="replace").splitlines()[0]
        instr_match = DEFINE_RE.search(instr_line)
        if not instr_match:
            raise RuntimeError(f"{piece_name}.txt: expected a leading '#define $instr ...' line, got {instr_line!r}")
        macros = {**global_macros, instr_match.group(1): instr_match.group(2)}

        raw_regions = parse_region_file(region_root / f"{piece_name}.txt", macros)
        for opcodes in raw_regions:
            lovel = int(opcodes["lovel"])
            hivel = int(opcodes["hivel"])
            seq_position = int(opcodes.get("seq_position", 1))
            by_key.setdefault(key, {}).setdefault((lovel, hivel), []).append({
                "sample_pattern": opcodes["sample"],
                "macros": macros,
                "lokey": key, "hikey": key,
                "lovel": lovel, "hivel": hivel,
                "pitch_keycenter": key,
                "seq_position": seq_position,
            })

    print(f"Parsed {len(PIECES)} pieces -> {len(by_key)} keys "
          f"({sum(len(tiers) for tiers in by_key.values())} velocity tiers total before reduction).")

    if codec == "adpcm":
        block_size_bytes = 1 << adpcm_block_size_pow
        samples_per_unit = nib.adpcm_samples_per_block(block_size_bytes)
        compression = nib.COMPRESSION_ADPCM
        header_block_size_field = block_size_bytes
    elif codec == "qoa":
        samples_per_unit = nib.QOA_SLICES_PER_FRAME * 20
        compression = nib.COMPRESSION_QOA
        header_block_size_field = nib.qoa_frame_size_bytes(samples_per_unit)
    else:
        raise ValueError(f"unknown codec {codec!r}")

    sample_blob = bytearray()
    region_entries = []
    max_samples = int(MAX_SAMPLE_SECONDS * TARGET_SAMPLE_RATE)

    for key in sorted(by_key):
        # One region per velocity tier: collapse round-robin to the
        # lowest seq_position present in that tier.
        tier_regions = []
        for tier, variants in by_key[key].items():
            tier_regions.append(min(variants, key=lambda r: r["seq_position"]))
        tier_regions.sort(key=lambda r: r["lovel"])
        selected = nib.reduce_velocity_layers(tier_regions, min(velocity_layers, len(tier_regions)))

        for region in selected:
            audio, source_rate = load_oh_stereo_pair(samples_root, region["sample_pattern"], region["macros"])
            audio = normalize_peak(audio)
            truncated = audio.shape[0] > int(MAX_SAMPLE_SECONDS * source_rate)
            audio = audio[:int(MAX_SAMPLE_SECONDS * source_rate)]
            pcm = nib.resample_and_quantize(audio, source_rate, TARGET_SAMPLE_RATE)
            pcm = pcm[:max_samples]
            if truncated:
                fade_len = min(int(FADEOUT_SECONDS * TARGET_SAMPLE_RATE), pcm.shape[0])
                fade = np.linspace(1.0, 0.0, fade_len, dtype=np.float64).reshape(-1, 1)
                pcm = pcm.astype(np.float64)
                pcm[-fade_len:] *= fade
                pcm = np.clip(np.round(pcm), -32768, 32767).astype("<i2")
            sample_length = pcm.shape[0]

            qoa_raw_prefix_samples = 0
            if codec == "adpcm":
                left = np.ascontiguousarray(pcm[:, 0])
                right = np.ascontiguousarray(pcm[:, 1])
                left_bytes = nib.encode_adpcm_mono(left, TARGET_SAMPLE_RATE, adpcm_block_size_pow, adpcm_lookahead)
                right_bytes = nib.encode_adpcm_mono(right, TARGET_SAMPLE_RATE, adpcm_block_size_pow, adpcm_lookahead)
                left_offset = len(sample_blob)
                sample_blob += left_bytes
                right_offset = len(sample_blob)
                sample_blob += right_bytes
                right_length = len(right_bytes)
            else:
                padded = nib.pad_to_frame_multiple(pcm, samples_per_unit)
                qoa_bytes = nib.encode_qoa_stereo(padded, TARGET_SAMPLE_RATE, samples_per_unit, 0)
                left_bytes = qoa_bytes
                left_offset = len(sample_blob)
                sample_blob += left_bytes
                right_offset = 0
                right_length = 0

            region_entries.append(struct.pack(
                nib.REGION_FMT,
                region["lokey"], region["hikey"],
                region["vel_lo"], region["vel_hi"],
                region["pitch_keycenter"],
                left_offset, len(left_bytes),
                right_offset, right_length,
                sample_length, 0, 0,  # one-shot: loop_start = loop_end = 0
                qoa_raw_prefix_samples,
            ))

    region_table = b"".join(region_entries)
    region_table_offset = nib.HEADER_SIZE
    unaligned_end = region_table_offset + len(region_table)
    sample_data_offset = (unaligned_end + 3) & ~3
    padding = b"\0" * (sample_data_offset - unaligned_end)

    header = struct.pack(
        nib.HEADER_FMT,
        nib.NIB_MAGIC,
        TARGET_SAMPLE_RATE,
        16,  # bit_depth
        2,   # channels
        compression,
        len(region_entries),
        display_name.encode("ascii")[:32].ljust(32, b"\0"),
        region_table_offset,
        sample_data_offset,
        header_block_size_field,
    )

    output_path.write_bytes(header + region_table + padding + sample_blob)
    total_mb = output_path.stat().st_size / 1_000_000
    codec_label = "IMA ADPCM" if codec == "adpcm" else "QOA"
    print(f"Wrote {output_path} -- {total_mb:.2f}MB {codec_label} "
          f"({len(region_entries)} regions across {len(by_key)} keys, "
          f"{TARGET_SAMPLE_RATE}Hz/16-bit/stereo, one-shot, no loop).")


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("kit_root", type=Path,
                         help="Path to a local clone of sfzinstruments/DrumGizmo.MuldjordKit "
                              "(this script reads <kit_root>/DrumGizmo/MuldjordKit/{Data,Samples})")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output .nib file path")
    parser.add_argument("--display-name", default="MuldjordKit Drums")
    parser.add_argument("--velocity-layers", type=int, default=3,
                         help="Velocity layers to keep per key (source has up to 5)")
    parser.add_argument("--codec", choices=["adpcm", "qoa"], default="adpcm")
    parser.add_argument("--adpcm-block-size-pow", type=int, default=8)
    parser.add_argument("--adpcm-lookahead", type=int, default=6)
    args = parser.parse_args()

    kit_dir = args.kit_root / "DrumGizmo" / "MuldjordKit"
    if not kit_dir.is_dir():
        kit_dir = args.kit_root  # allow pointing directly at the DrumGizmo/MuldjordKit subdir too

    build_drumkit_nib(kit_dir, args.output, args.display_name,
                       args.velocity_layers, args.codec,
                       args.adpcm_block_size_pow, args.adpcm_lookahead)


if __name__ == "__main__":
    main()
