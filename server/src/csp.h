#pragma once

namespace archive {

// Content-Security-Policy on every response, and the endpoint browsers report
// violations to.
//
// The policy lives in the app rather than in Caddy with the other security headers.
// HSTS, nosniff and frame options are deployment policy and hold whatever the app does;
// a CSP describes what *this frontend* is allowed to do, so it changes when the frontend
// does and is tested with it.
//
// Violations are reported to POST /api/csp-report and logged. A CSP's failure mode is
// silence — a blocked script or style just does not happen, and the symptom is a
// feature that quietly stops working in somebody else's browser. The report turns that
// into a log line.
void registerContentSecurityPolicy();

}  // namespace archive
