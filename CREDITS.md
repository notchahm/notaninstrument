# Third-party content credits

The two built-in instrument soundbanks (`firmware/notaninstrument-p4`'s
`drumkit`/`soundbank` flash partitions, built by `tools/sfz_preprocessor/`)
are derived from third-party sample libraries. Neither is bundled in this
repository in its original form — only the offline-converted `.nib`
binaries built from them are flashed to the device — but both require
attribution under their licenses.

## Salamander Grand Piano V3

By Alexander Holm. Licensed
[CC-BY 3.0](https://creativecommons.org/licenses/by/3.0/).
[archive.org/details/SalamanderGrandPianoV3](https://archive.org/details/SalamanderGrandPianoV3).
Converted to this project's `.nib` format by
`tools/sfz_preprocessor/sfz_to_nib.py`.

## MuldjordKit

By Lars Muldjord ([muldjord.com](https://muldjord.com),
[drumgizmo.org](https://drumgizmo.org)). Licensed
[CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/). SFZ port by
kinwie:
[sfzinstruments/DrumGizmo.MuldjordKit](https://github.com/sfzinstruments/DrumGizmo.MuldjordKit).
A real Tama Superstar drum kit, recorded 2010. Converted to this
project's `.nib` format by `tools/sfz_preprocessor/muldjordkit_to_nib.py`,
using only the overhead stereo mic pair, core kit pieces, reduced
velocity/round-robin depth, and one-shot playback — not the library's
full multi-mic mixing-console feature set. Replaced
[sfzinstruments/virtuosity_drums](https://github.com/sfzinstruments/virtuosity_drums)
(CC0, no attribution required) as the built-in channel-10 drum kit.
