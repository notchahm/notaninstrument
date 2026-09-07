cmake_minimum_required(VERSION 3.16)

# tinyusb_host (components/tinyusb_host/) is kept vendored in-tree as a
# reference/fallback (see docs/polyphony-latency-investigation.md and
# CLAUDE.md architecture decision #4) but is no longer used by this app --
# main/ now uses ESP-IDF's native USB Host Library (usb_midi_host.c)
# instead. ESP-IDF compiles+links every component under components/
# regardless of whether anything REQUIRES it, so it has to be excluded
# explicitly here or it silently ends up dead code in the final binary.
set(EXCLUDE_COMPONENTS "tinyusb_host")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spike_usb_midi_idf)
