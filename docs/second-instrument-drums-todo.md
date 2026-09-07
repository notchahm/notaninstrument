# Second instrument: drum kit — investigation + todo

Status: investigated, not started. A candidate for exercising the
multi-instrument path (`docs/multi-instrument-soundbanks.md`) once the
Salamander piano's single-instrument polyphony (bring-up steps 6-7) is
fully solid — matches that doc's own suggested phasing, not a change to
it.

## Candidate considered: sfzinstruments/virtuosity_drums

[github.com/sfzinstruments/virtuosity_drums](https://github.com/sfzinstruments/virtuosity_drums)
— a real, well-recorded jazz drum kit (six mixable mic positions, deep
per-drum articulation coverage: multiple snare hit types, tom
articulations, hi-hat states, cymbals, plus auxiliary percussion pulled in
from VSCO 2 Pro / Karoryfer). Good content, but **significantly more SFZ
dialect than `tools/sfz_preprocessor/sfz_to_nib.py` currently parses** —
that parser was written against Salamander's plain original distribution
(flat `<region>` list, ~6 opcodes total, no macros or includes) and does
not handle any of the following, all present in this library:

- **Nested `#include`** — the top-level program file
  (`Programs/01-basic-kit.sfz`) pulls in mapping files under
  `Programs/mappings/`, which themselves `#include` further files under
  `Programs/mappings/kickmic/` — at least 3 levels deep.
- **`#define` macros for key constants** (`$KICK_SNWRONG_KEY`,
  `$SNARE_STICKSHOT1_KEY`, etc.), resolved from a separate keymap file.
- **`<master>`/`<group>`/`<region>` opcode inheritance** — settings
  cascade down through multiple header levels; the current parser only
  understands flat `<region>` opcodes with no inheritance at all.
- **Deliberate simultaneous multi-mic layering** — a single kick hit is
  kick-mic + snare-mic-bleed + overhead-mic regions all triggering
  together *by design* (that's the point of "six mixable mic positions"),
  not alternate choices where you'd pick one. Needs an explicit decision
  on how many mic layers this project keeps, since keeping all of them
  multiplies both storage and per-hit voice count.
- **Some auxiliary percussion samples are sourced from other libraries
  entirely** (VSCO 2 Pro, Karoryfer) not included in this repo — those
  regions would need their own sample sourcing or should be dropped.

None of this is a fundamental blocker, just real, scoped parser and
design work beyond what exists today.

## What's needed to actually support it

1. **`#include` resolution** in `sfz_to_nib.py` — recursively inline
   included files relative to the including file's directory, before the
   existing line-by-line opcode parsing runs.
2. **`#define` macro substitution** — a preprocessing pass replacing
   `$NAME` tokens with their defined values, same as a C preprocessor's
   `#define` (no function-like macros needed here, just simple token
   substitution).
3. **Opcode inheritance** across `<master>` → `<group>` → `<region>** —
   track the "current" opcode set as the parser walks the file
   top-to-bottom, with each more-specific header overriding/adding to
   what came before it, per the SFZ spec's own scoping rules.
4. **A mic-layer decision** — likely start with a single mic position
   (probably the overhead or a blended "room" mic if one exists) rather
   than all six, to keep storage and per-hit voice cost bounded; revisit
   if the reduced version sounds too thin.
5. **Handle or drop externally-sourced regions** — either source VSCO
   2 Pro / Karoryfer separately for the auxiliary percussion, or skip
   those regions (core kit pieces don't depend on them).

## Alternative worth considering first

Look for a simpler, flat-format drum SFZ (single mic, one sample per
articulation, no `#include`/macros/multi-mic layering) as the actual
second instrument — would work with the *existing* parser today, no new
work, and still fully exercises the multi-instrument hot-swap path
(`docs/multi-instrument-soundbanks.md`) that's the real point of adding a
second instrument in the first place. Worth deciding whether the goal
right now is "prove multi-instrument swapping works" (simpler kit is
enough) vs. "ship a great-sounding drum kit" (worth the virtuosity_drums
parser investment) before picking either path.
