#pragma once

// Bring-up step 4 (docs/bring-up-plan.md): PCM5102A over I2S. Proves I2S
// wiring/clocking/DMA in isolation, before MIDI or SD touch the audio path.

void initialize_audio_output();
void start_audio_output_task();
// Re-prints init results + a live write counter -- the one-shot boot log
// keeps getting missed by a serial reader attaching after reset, so this
// is called periodically from the main loop instead (same fix already
// used for the chip/PSRAM status banner).
void print_audio_status();
