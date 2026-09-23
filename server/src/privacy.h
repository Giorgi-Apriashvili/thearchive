#pragma once

namespace archive {

// GET /api/privacy — what the privacy notice needs that only the deployment knows.
//
// Who runs the site comes from the environment (ARCHIVE_OPERATOR_NAME,
// ARCHIVE_OPERATOR_CONTACT, optionally ARCHIVE_HOSTING_LOCATION), never from the source:
// the repository is public, and a name and email address committed there would be in
// its history for good. Every deployment states its own.
//
// Retention periods are the values this server actually enforces — the same constants
// and settings the code acts on — so the notice cannot promise one thing while the
// system does another.
//
// Public: the notice has to be readable by someone with no account, such as a person who
// was sent a link.
void registerPrivacyRoutes();

}  // namespace archive
