#pragma once

#include <stdbool.h>

#include "telegram.h"

// Inline mode: "@thisbot ..." typed into any chat, by anyone, without the bot
// being a member of it.
//
// Two kinds of answer, chosen by whether anything was typed after the name:
//
//   empty query     -> the device reports on itself. Two cards, the same
//                      /status and /ping text the chat commands produce, so
//                      there is one report in one format however it was asked
//                      for. Picking one posts it into the chat and nothing
//                      reaches the pager -- these are questions about the
//                      device, not messages for it.
//   anything typed  -> one card that pages the device. Picking it posts the
//                      text into the chat and, through the chosen_inline_
//                      result update, hands the same text to the pager.
//
// There is no allow-list, for the same reason there is none on messages or
// commands: the bot token is the access control. Inline mode does widen who
// can reach the device in practice, though -- an inline query needs no chat
// with the bot at all -- so a bot whose token has leaked is now also a pager
// anyone can page from any group they share with the leaker.
//
// Answering is not something the firmware can enable on its own: inline mode
// is a BotFather setting (/setinline), and /setinlinefeedback must be Enabled
// or a picked page is posted in the chat and never arrives here.
//
// Runs on the calling task and does a TLS handshake, exactly like commands.c,
// so call it from the pager task. True when the answer went out.
bool inline_answer_query(const telegram_inline_query_t *query);
