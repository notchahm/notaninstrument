/*
 * Modified from:
 * https://github.com/rppicomidi/usb_midi_host/blob/main/examples/arduino/usb_midi_host_example/usb_midi_host_example.ino
*/

#ifndef USE_TINYUSB_HOST
#error "Please Select USB Stack: Adafruit TinyUSB Host"
#else
#warning "All Serial Monitor Output is on Serial1"
#endif

#include "Adafruit_TinyUSB.h"
#define LANGUAGE_ID 0x0409  // English
#include "usb_midi_host.h"

// USB Host object
Adafruit_USBH_Host USBHost;

// holding the device address of the MIDI device
uint8_t midi_dev_addr = 0;

// prototypes for handling MIDI note events
void start_note(int channel, int key, int velocity);
void stop_note(int channel, int key, int velocity);

void initialize_midi_host()
{
	Serial1.begin(115200); // All console prints go to UART0
	USBHost.begin(0); // 0 means use native RP2040 host
}

void handle_midi_events()
{
	USBHost.task();
}

// TinyUSB Host callbacks

// Invoked when device with midi interface is mounted
void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep, uint8_t num_cables_rx, uint16_t num_cables_tx)
{
	Serial1.printf("MIDI device address = %u, IN endpoint %u has %u cables, OUT endpoint %u has %u cables\r\n", dev_addr, in_ep & 0xf, num_cables_rx, out_ep & 0xf, num_cables_tx);
	if (midi_dev_addr == 0) 
	{
		// then no MIDI device is currently connected
		midi_dev_addr = dev_addr;
	}
	else
	{
		Serial1.printf("A different USB MIDI Device is already connected.\r\nOnly one device at a time is supported in this program\r\nDevice is disabled\r\n");
	}
}

// Invoked when device with midi interface is un-mounted
void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance)
{
	if (dev_addr == midi_dev_addr) 
	{
		midi_dev_addr = 0;
		Serial1.printf("MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
	}
	else
	{
		Serial1.printf("Unused MIDI device address = %d, instance = %d is unmounted\r\n", dev_addr, instance);
	}
}

// Invoked when midi event is received
void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets)
{
	if (midi_dev_addr == dev_addr) 
	{
		if (num_packets != 0) 
		{
			uint8_t cable_num;
			uint8_t buffer[48];
			while (1) 
			{
				uint32_t bytes_read = tuh_midi_stream_read(dev_addr, &cable_num, buffer, sizeof(buffer));
				if (bytes_read == 0)
				{
					return;
				}
				if (bytes_read == 3)
				{
					uint8_t status = (buffer[0] >> 4) & 0x7;
					uint8_t channel = buffer[0] & 0xf;
					uint8_t key = buffer[1] & 0x7f;
					uint8_t velocity = buffer[2] & 0x7f;
					const char* message_type = "?";

					if (status < 7)
					{
						if (status == 1)
						{
							start_note(channel, key, velocity);
						}
						else if (status == 0)
						{
							stop_note(channel, key, velocity);
						}
					}
					//Serial1.printf("%s(%d): channel[%d], key[%s:%d], velocity[%d]", message_type, status, channel, midi_note_names[note_index], octave, velocity);

				}
				else
				{
					// An unsupported MIDI event
					for (uint32_t idx = 0; idx < bytes_read; idx++)
					{
						Serial1.printf("%02x ", buffer[idx]);
					}
				}
        			//Serial1.printf("\r\n");
			}
		}
	}
}

// Invoked when midi event is sent
void tuh_midi_tx_cb(uint8_t dev_addr)
{
	(void)dev_addr;
}


