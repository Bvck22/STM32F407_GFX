/*
 * usb_cmd.h - ASCII command parser for the CDC link (docs/protocol.md v2).
 *
 * Everything runs in the main loop; the USB interrupt only fills a ring buffer.
 * Long replies (ALGOS, STATE, ...) are streamed line by line as TX space frees
 * up, and no further command is parsed until the stream has finished.
 */
#ifndef USB_CMD_H
#define USB_CMD_H

#include <stdbool.h>

/* Parses received bytes, executes complete lines, streams long replies,
 * emits events and pumps the TX queue. Call continuously from the main loop. */
void usb_cmd_poll(void);

/* Queues the unsolicited "EVT PLAY <0/1>" line. */
void usb_cmd_notify_play(bool playing);

/* Button action: sends REC to the first looper slot and reports the new state
 * ("EVT LOOP"). Returns false if no slot holds a looper. */
bool usb_cmd_looper_rec(void);

/* True while any looper slot is recording (state sampled every 50 ms). */
bool usb_cmd_looper_recording(void);

#endif /* USB_CMD_H */
