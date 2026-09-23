#pragma once

#include "auth.h"
#include "db.h"

namespace archive {

// Sending your links to one member, and their inbox of links sent to them.
//
//   POST   /api/users/{username}/links {links: [token…], note?} — up to ten, your own
//                                      working links only
//   POST   /api/shares/{token}/send    {username, note?} — the same, for a single link
//   GET    /api/inbox                  what was sent to you, newest first
//   GET    /api/inbox/unread           {count}, for the badge
//   POST   /api/inbox/seen             marks everything in your inbox seen
//   DELETE /api/inbox/items/{id}       dismisses one item
//
// Links sent together are one inbox item: one note, one entry in the unread count, one
// dismissal, with a card per link. A send is all or nothing — every link is checked
// before any is delivered, so one that expired while the picker was open refuses the
// send with a reason rather than delivering the rest without it. Sending a link that is
// already in their inbox moves it into the new item, unread again, rather than showing
// it twice.
//
// A link keeps its own rules when sent: a members-only link opens for any member as it
// always did, and a password-protected one still needs its password — sending grants
// nothing. What the recipient gets is the link and who it came from.
//
// Someone who has blocked you in chat does not receive your links either, and sending to
// them reports success exactly as inviting them does: telling the sender they are blocked
// would hand them what the blocker chose not to share.
void registerInboxRoutes(Database& db, Auth& auth);

}  // namespace archive
