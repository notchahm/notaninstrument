#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

void initialize_audio_output();
void play_sample_bytes(int32_t* output_samples, int num_channels, int num_samples);

#endif
