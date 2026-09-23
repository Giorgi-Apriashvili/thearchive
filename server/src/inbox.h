#pragma once

#include "auth.h"
#include "db.h"

namespace archive {

// Sending one of your links to one member, and their inbox of links sent to them.
//
//   POST   /api/shares/{token}/send    {username, note?} — your own working link only
//   GET    /api/inbox                  links sent to you, newest first
//   GET    /api/inbox/unread           {count}, for the badge
//   POST   /api/inbox/seen             marks everything in your inbox seen
//   DELETE /api/inbox/items/{id}       dismisses one item
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
