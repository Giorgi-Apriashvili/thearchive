#pragma once

#include "auth.h"
#include "db.h"

namespace archive {

// Rooms, invitations and messages.
//
//   GET    /api/chat/rooms                     every room, annotated with my state
//   POST   /api/chat/rooms                     {name}; creator joins automatically
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
void registerChatRoutes(Database& db, Auth& auth);

}  // namespace archive
