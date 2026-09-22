#pragma once

#include "auth.h"
#include "db.h"

namespace archive {

// Rooms, invitations and messages.
//
//   GET    /api/chat/rooms                     every room, annotated with my state
//   POST   /api/chat/rooms                     {name}; creator joins automatically
//   GET    /api/chat/rooms/{id}/members        members, plus outstanding invitations
//   POST   /api/chat/rooms/{id}/invite         {username} — creator or admin
//   POST   /api/chat/rooms/{id}/respond        {action} — accept|decline|block_room|block_user
//   GET    /api/chat/rooms/{id}/messages?since=
//   POST   /api/chat/rooms/{id}/messages       {body}
//   POST   /api/chat/rooms/{id}/read           {last_id}
//   DELETE /api/chat/messages/{id}             admin only; soft delete
//   GET    /api/chat/blocks
//   DELETE /api/chat/blocks/room/{id} | /user/{id}
//
// Room *names* are visible to every signed-in user deliberately — a room nobody can see
// is a room nobody can ask to join. Everything else goes through requireRoomMember.
//
// @mentions are resolved once at send time against the room's membership and stored, so
// a mention is something the server knows rather than something each reader re-derives.
// That is what a notifier will read; see message_mentions in db.cc.
void registerChatRoutes(Database& db, Auth& auth);

}  // namespace archive
