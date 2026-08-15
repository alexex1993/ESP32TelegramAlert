#pragma once

#include "msg_queue.h"

// Messages that address the firmware rather than the person carrying it.
//
// A command is answered in the chat it came from and never reaches the screen:
// it asks about the device, so it must not take a queue slot, light the
// backlight or wait for a button press to be cleared.
//
// "/last N" is the closest thing to an exception on the reading side: it
// answers with the N newest pages still in the queue, but only reads them --
// they stay queued, still unacknowledged, because only the BOOT key means "I
// have seen this".
//
// "/pager <text>" is the deliberate exception. It is a command in form only --
// it carries a page rather than a question, so it strips its own prefix and
// goes on to the screen as ordinary text, reported as COMMAND_NONE. Which is
// why `msg` is not const here.
//
// Only the names listed in commands.c are treated this way. Anything else
// starting with '/' is an ordinary message and is paged as usual -- someone
// typing "/dev/ttyUSB0" is sending a page, not driving the pager.
//
// There is no allow-list here either, for the same reason there is none on
// messages: the bot token is the access control. Whoever can page the device
// can also ask it how it is doing.
typedef enum {
    COMMAND_NONE = 0,   // Not a command; page it.
    COMMAND_ANSWERED,   // Handled, and the reply went out.
    COMMAND_FAILED,     // Handled, but the reply did not reach the chat.
} command_result_t;

// Answers msg if it is a known command. Sends on the calling task, so call it
// from the pager task -- it does a TLS handshake like every other API call.
command_result_t commands_try_handle(pager_msg_t *msg);

// A dozen short lines, with room for a 32-byte SSID and the widest number
// every field can hold. Anything longer is truncated rather than split.
#define COMMANDS_STATUS_MAX 640

// The /last listing: one line of heading per page plus its clipped text, for
// as many pages as APP_LAST_MAX_PAGES allows. The 96 covers the "[MM-DD HH:MM]
// Sender:" line, the ellipsis a clipped page ends on and the blank line
// between entries; the trailing one covers the title. Derived from the two
// knobs rather than written out, so raising either cannot silently start
// truncating the last page in the reply.
#define COMMANDS_LAST_MAX (APP_LAST_MAX_PAGES * (APP_LAST_TEXT_MAX + 96) + 96)

// Writes the /status text into buf. Public because inline mode offers the
// same report as a card (see inline_mode.c) -- one report, one format,
// whichever way it was asked for.
void commands_build_status(char *buf, size_t size);
