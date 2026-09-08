#!/usr/bin/env python3
"""virtuosity_to_nib.py -- converts a single-mic slice of sfzinstruments/
virtuosity_drums (github.com/sfzinstruments/virtuosity_drums, CC0 1.0
Universal / public domain) into a .nib soundbank for this project's
built-in drum kit (channel 10 -- see voice_engine.c's DRUM_MIDI_CHANNEL).

Why a separate script from sfz_to_nib.py: almost none of that script's
pipeline applies to one-shot percussion (no loop-point search, no
level_loop_amplitude, no QOA raw-prefix adaptive search -- drum hits are
short and never loop) while this library's SFZ dialect needs real new
parsing sfz_to_nib.py's flat-Salamander-only parser never had to do:
nested #include, #define macro substitution, and <global>/<master>/
<group>/<region> opcode inheritance. This script imports and reuses
sfz_to_nib.py's low-level, codec-agnostic primitives (encode functions,
resampling, the .nib header/region struct formats, reduce_velocity_layers)
rather than duplicating them -- the .nib format itself is unchanged;
percussion regions just carry loop_start == loop_end == 0 (the format's
existing "no loop" convention, already handled correctly by
voice_engine.c's non-loop playback path with zero firmware changes).

Scope (see docs/second-instrument-drums-todo.md and this session's
planning for the full rationale) -- deliberately NOT the full library:
- Single mic: the overhead ("oh") mic, which alone has full coverage of
  the whole core kit and has its own dedicated entry point
  (Programs/05-oh-mic.sfz) -- not the library's full 6-mic simultaneous
  layering.
- Core kit only, the 18 standard-GM keys defined in
  Programs/keymaps/keymap_basic.sfz (kick/snare/toms/hihat/cymbals) --
  not the auxiliary percussion (tambourine, cowbell, etc., sourced from
  VSCO 2 Pro/Karoryfer) or the extended technique variants (stickshot,
  buzz, flam, roll, hi-hat half/3-4/splash) crammed into keys 85-96.
  Enforced structurally, not via an explicit key allowlist: macros for
  everything out of scope (e.g. $SNARE_BUZZ_KEY) simply aren't defined
  in keymap_basic.sfz, so substitution leaves them unresolved and those
  regions get dropped for failing to produce a valid integer key --
  see resolve_region_key().
- One round-robin variant per (key, velocity layer) -- seq_position == 1
  (or the lowest present) -- and a reduced velocity-layer count via the
  existing reduce_velocity_layers(), same as piano. No round-robin
  cycling state added to voice_engine.c/the .nib format for this first
  pass; repeated hits sound identical instead of cycling.
- One-shot playback only: loop_start = loop_end = 0. Regions requiring
  looping (snare rolls, trigger=release_key roll-ends) are out of scope
  and get dropped (their key macros -- e.g. $SNARE_ROLL_KEY -- aren't in
  keymap_basic.sfz either, so this happens automatically via the same
  unresolved-macro mechanism, with an explicit trigger=release_key/
  loop_mode check as a defensive backstop).
- A CC21 (snares on/off) gating quirk in the library itself needed a
  real, non-obvious filter: several in-scope pieces (e.g.
  $SNARE_CENTER_KEY) are mapped *twice* in oh_all.sfz -- once gated
  hicc21=100 ("snares on", the default state with no CC21 sent) and
  once locc21=101 ("snares off", unreachable since this project never
  sends CC21). Any resolved locc<N> opcode with a positive integer
  value marks a region as CC-gated and unreachable by default, and it's
  dropped -- see is_cc_gated().
"""

import re
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import sfz_to_nib as nib  # noqa: E402  (reuses low-level primitives, see module docstring)

TARGET_SAMPLE_RATE = 32000
# Tightened from an initial 2.5s: real cymbal/tom decays in this library
# run long enough that several regions hit that cap exactly, and 63
# regions averaging ~2.3s each blew well past the drum kit's flash
# budget (4.63MB against a 2MB partition). 1.0s keeps the punch/character
# of every piece (most of a drum hit's energy and identity is in the
# first second) while fitting comfortably -- FADEOUT_SECONDS smooths the
# truncation point so a still-ringing cymbal doesn't end in an audible
# click.
MAX_SAMPLE_SECONDS = 1.0
FADEOUT_SECONDS = 0.05

INCLUDE_RE = re.compile(r'#include\s+"([^"]+)"')
DEFINE_RE = re.compile(r'#define\s+(\$\w+)\s+(\S+)')
HEADER_RE = re.compile(r"^<(\w+)>\s*(.*)$")
OPCODE_RE = re.compile(r"(\w+)=(\S+)")
MACRO_TOKEN_RE = re.compile(r"\$\w+")


def resolve_includes(path: Path, programs_root: Path) -> list:
    """Recursively inlines #include lines, resolved relative to
    programs_root (confirmed on the real repo: paths are written relative
    to Programs/, not to the including file's own directory) -- not
    relevant which of the two include styles (self-contained header+
    region includes, or bare-opcode-only includes) produced a given
    line, since both just become ordinary lines in the flattened output
    the caller walks linearly."""
    lines = []
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = INCLUDE_RE.search(raw_line)
        if m:
            inc_path = programs_root / m.group(1)
            lines.extend(resolve_includes(inc_path, programs_root))
        else:
            lines.append(raw_line)
    return lines


def load_macros(keymap_path: Path) -> dict:
    """Pure $NAME -> value token substitution (confirmed no conditionals/
    nesting/arithmetic anywhere in the library). Deliberately loads only
    keymap_basic.sfz's macro set, never keymap.sfz's (the CC4-hi-hat-
    pedal variant real single-mic patches like 05-oh-mic.sfz actually
    #include) -- keymap_basic.sfz's smaller macro set is what makes the
    scope filtering above work at all."""
    macros = {}
    for raw_line in keymap_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("//")[0]
        m = DEFINE_RE.search(line)
        if m:
            macros[m.group(1)] = m.group(2)
    return macros


def substitute_macros(lines: list, macros: dict) -> list:
    def repl(m):
        return macros.get(m.group(0), m.group(0))  # leave unresolved if unknown -- see module docstring
    return [MACRO_TOKEN_RE.sub(repl, line) for line in lines]


def parse_flattened_sfz(lines: list) -> list:
    """Walks <global>/<master>/<group>/<region> inheritance top-to-bottom
    (confirmed real cascading behavior against the actual repo -- see
    module docstring). Each more-specific header resets its own and all
    more-specific levels' accumulated opcodes but keeps less-specific
    ones; bare opcode lines (not starting a new header, including ones
    that arrived via a bare-opcode #include like kick_dampen.sfz) merge
    into whichever level was most recently opened. Any header type other
    than global/master/group/region (e.g. <curve>, <control>) closes out
    accumulation entirely until the next recognized header, so stray
    opcodes under it are dropped rather than misattributed."""
    regions = []
    global_ops, master_ops, group_ops = {}, {}, {}
    region_ops = None
    level = None

    def flush_region():
        nonlocal region_ops
        if region_ops is not None:
            merged = {}
            merged.update(global_ops)
            merged.update(master_ops)
            merged.update(group_ops)
            merged.update(region_ops)
            regions.append(merged)
        region_ops = None

    for raw_line in lines:
        line = raw_line.split("//")[0].strip()
        if not line or line.startswith("#"):
            continue
        m = HEADER_RE.match(line)
        if m:
            tag, rest = m.group(1), m.group(2)
            rest_ops = dict(OPCODE_RE.findall(rest))
            if tag == "region":
                flush_region()
                region_ops = dict(rest_ops)
                level = "region"
            elif tag == "group":
                flush_region()
                group_ops = dict(rest_ops)
                level = "group"
            elif tag == "master":
                flush_region()
                master_ops = dict(rest_ops)
                group_ops = {}
                level = "master"
            elif tag == "global":
                flush_region()
                global_ops = dict(rest_ops)
                master_ops = {}
                group_ops = {}
                level = "global"
            else:
                flush_region()
                level = None
            continue
        found = dict(OPCODE_RE.findall(line))
        if not found:
            continue
        if level == "region" and region_ops is not None:
            region_ops.update(found)
        elif level == "group":
            group_ops.update(found)
        elif level == "master":
            master_ops.update(found)
        elif level == "global":
            global_ops.update(found)
        # level is None (inside <curve>/<control>/etc.) -- drop stray opcodes
    flush_region()
    return regions


def resolve_region_key(opcodes: dict):
    """Returns the region's key as an int, or None if out of scope (its
    controlling macro wasn't defined in keymap_basic.sfz and so never
    resolved to a plain integer -- see module docstring). Handles the
    key=N shorthand (equivalent to lokey=hikey=pitch_keycenter=N) used
    throughout this library instead of Salamander's separate opcodes."""
    key_str = opcodes.get("key") or opcodes.get("pitch_keycenter") or opcodes.get("lokey")
    if key_str is None:
        return None
    try:
        return int(key_str)
    except ValueError:
        return None  # still contains an unresolved $MACRO -- out of scope


def is_cc_gated(opcodes: dict) -> bool:
    """True if any locc<N> opcode resolved to a positive integer -- see
    module docstring's CC21 snares-on/off finding. Opcodes that never
    resolved (e.g. hi-hat's locc4=$HH_CLOSED_LOCC, since keymap_basic.sfz
    doesn't define CC4-pedal macros at all) fail the int() conversion and
    are correctly treated as "not a real gate," not as a match."""
    for opcode, value in opcodes.items():
        if re.match(r"^locc\d+$", opcode):
            try:
                if int(value) > 0:
                    return True
            except ValueError:
                pass
    return False


def load_sample_flac_or_wav(path: Path):
    """Loads a stereo (or mono, duplicated to stereo like
    load_wav_stereo_float already does for mono Salamander files) sample
    at its native rate as float32, decoding FLAC via ffmpeg first (no
    other FLAC dependency exists yet in this tool) since this library's
    core-kit samples are FLAC, unlike Salamander's WAV."""
    if path.suffix.lower() == ".wav":
        return nib.load_wav_stereo_float(path)
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = Path(tmpdir) / "decoded.wav"
        proc = subprocess.run(
            ["ffmpeg", "-y", "-i", str(path), "-c:a", "pcm_s16le", "-v", "error", str(wav_path)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise RuntimeError(f"ffmpeg failed decoding {path}: {proc.stderr.decode(errors='replace')}")
        return nib.load_wav_stereo_float(wav_path)


def build_drumkit_nib(virtuosity_root: Path, output_path: Path, display_name: str,
                       velocity_layers: int, codec: str,
                       adpcm_block_size_pow: int, adpcm_lookahead: int):
    programs_root = virtuosity_root / "Programs"
    macros = load_macros(programs_root / "keymaps" / "keymap_basic.sfz")

    flattened = resolve_includes(programs_root / "mappings" / "oh_all.sfz", programs_root)
    flattened = substitute_macros(flattened, macros)
    raw_regions = parse_flattened_sfz(flattened)

    by_key = {}
    dropped_out_of_scope = 0
    dropped_cc_gated = 0
    dropped_no_sample = 0
    for opcodes in raw_regions:
        key = resolve_region_key(opcodes)
        if key is None:
            dropped_out_of_scope += 1
            continue
        if opcodes.get("trigger") == "release_key" or "loop" in opcodes.get("loop_mode", ""):
            dropped_out_of_scope += 1
            continue
        if is_cc_gated(opcodes):
            dropped_cc_gated += 1
            continue
        sample_rel = opcodes.get("sample")
        if not sample_rel or sample_rel == "*silence":
            dropped_no_sample += 1
            continue
        sample_path = (programs_root / sample_rel.replace("\\", "/")).resolve()
        if not sample_path.exists():
            dropped_no_sample += 1
            continue
        try:
            lovel = int(opcodes.get("lovel", 1))
            hivel = int(opcodes.get("hivel", 127))
        except ValueError:
            dropped_out_of_scope += 1
            continue
        seq_position = int(opcodes.get("seq_position", 1))
        by_key.setdefault(key, []).append({
            "sample_path": sample_path,
            "lokey": key, "hikey": key,
            "lovel": lovel, "hivel": hivel,
            "pitch_keycenter": key,
            "seq_position": seq_position,
        })

    print(f"Parsed {len(raw_regions)} regions from oh_all.sfz; "
          f"dropped {dropped_out_of_scope} out-of-scope (unresolved key/roll/release-trigger), "
          f"{dropped_cc_gated} CC-gated (unreachable snares-off duplicates), "
          f"{dropped_no_sample} missing/silence sample -- "
          f"{len(by_key)} in-scope keys remain.")

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
        # Collapse round-robin variants (seq_position) to the lowest one
        # present per distinct (lovel, hivel) velocity tier -- see module
        # docstring -- before reducing the tier count itself, so
        # reduce_velocity_layers (which assumes one region per tier)
        # doesn't see duplicate RR entries for the same tier.
        by_tier = {}
        for region in by_key[key]:
            tier = (region["lovel"], region["hivel"])
            if tier not in by_tier or region["seq_position"] < by_tier[tier]["seq_position"]:
                by_tier[tier] = region
        tier_regions = sorted(by_tier.values(), key=lambda r: r["lovel"])
        selected = nib.reduce_velocity_layers(tier_regions, min(velocity_layers, len(tier_regions)))

        for region in selected:
            audio, source_rate = load_sample_flac_or_wav(region["sample_path"])
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
    parser.add_argument("virtuosity_root", type=Path,
                         help="Path to a local clone of sfzinstruments/virtuosity_drums")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output .nib file path")
    parser.add_argument("--display-name", default="Virtuosity Drums")
    # 2, not piano's 3: with 21 keys the total region count (and flash
    # footprint) scales directly with this, and the 2MB drumkit
    # partition is a much tighter budget than piano's -- 2 layers
    # (soft/hard) still gives real velocity response for a first,
    # "basic" pass.
    parser.add_argument("--velocity-layers", type=int, default=2,
                         help="Velocity layers to keep per key (source has up to 36)")
    # Defaults to adpcm here specifically, unlike sfz_to_nib.py's qoa
    # default for piano: QOA's advantage there comes largely from the
    # per-region adaptive raw-PCM prefix that keeps its LMS predictor's
    # cold-start out of the compressed portion -- not implemented here,
    # since one-shot percussion has no sustain/loop to make that
    # investment worthwhile the way piano's did. Without it, QOA's
    # cold-start would apply to essentially the *entire* sample (a drum
    # hit is nothing but transient), where ADPCM's block-header-stores-
    # the-predictor-directly design has no equivalent weak spot.
    parser.add_argument("--codec", choices=["adpcm", "qoa"], default="adpcm")
    parser.add_argument("--adpcm-block-size-pow", type=int, default=8)
    parser.add_argument("--adpcm-lookahead", type=int, default=6)
    args = parser.parse_args()

    build_drumkit_nib(args.virtuosity_root, args.output, args.display_name,
                       args.velocity_layers, args.codec,
                       args.adpcm_block_size_pow, args.adpcm_lookahead)


if __name__ == "__main__":
    main()
