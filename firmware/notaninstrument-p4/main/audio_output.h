#pragma once

// I2S/DAC plumbing only -- the actual voice mixing lives in
// voice_engine.h/.c. Bring-up step 4 (docs/bring-up-plan.md) proved this
// half in isolation (a fixed test tone); this now drains
// voice_engine_render() into the DMA buffer instead.

void init_audio_output(void);
