#ifndef MIDI_DISPLAY_H
#define MIDI_DISPLAY_H

void initialize_midi_display();
void display_text(const char* text);
void display_note(int channel, int note, int velocity);
void scan_i2c_bus();

#endif
