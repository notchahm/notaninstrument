#ifndef MIDI_DISPLAY_H
#define MIDI_DISPLAY_H

void initialize_midi_display();
void display_text(const char* text);
void display_note(char* channel, char* key, int velocity);

#endif
