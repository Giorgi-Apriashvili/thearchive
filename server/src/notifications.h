#pragma once

#include "auth.h"
#include "db.h"

namespace archive {

// What is waiting for you, for the browser to notify about while the site is open.
//
//   GET /api/notifications
//     {counts: {mentions, links, invitations},
//      items: [{key, kind, at, from: {username, display_name?}, …}]}  newest first
//
// Three kinds, each "unread" in the same sense its badge already uses, so a notification
// never points at something the site no longer shows as new:
//   mention     an unread message in a room you are in that @names you — not your own,
//               not removed, not in a room you have blocked;      + room, excerpt
//   link        links sent to you not yet seen in "Shared with you";  + titles, note
//   invitation  a room invitation you have not answered;           + room
//
// `key` is stable per item ("m:<message>", "l:<send>", "i:<room>") so a client polling
// repeatedly can tell what is new, and several tabs can agree on what was already shown.
// The client decides what to show and when; the server only says what is outstanding.
void registerNotificationRoutes(Database& db, Auth& auth);

}  // namespace archive
