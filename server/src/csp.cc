#include "csp.h"

#include <drogon/drogon.h>

#include <string>
#include <string_view>

#include "httputil.h"

namespace archive {
namespace {

// Every source is 'self' or 'none'. Nothing the app does needs more, and that was
// checked against the build rather than assumed: index.html carries no inline script or
// style, the bundle has no eval or new Function, no workers, websockets or object URLs,
// the fonts are self-hosted, and the CSS references nothing external.
//
// Svelte's dynamic `style="width: {x}%"` bindings are applied through style.cssText and
// its transitions through element.animate() — both CSSOM, which style-src does not
// govern — so no 'unsafe-inline' is needed for styles either. That was verified in a
// real browser, not read off the runtime source; see the CSP section of DESIGN.md.
constexpr const char* kPolicy =
    "default-src 'self'; "
    "script-src 'self'; "
    "style-src 'self'; "
    "img-src 'self'; "      // thumbnails
    "media-src 'self'; "    // the video player's /inline
    "font-src 'self'; "     // self-hosted Inter and Instrument Serif
    "connect-src 'self'; "  // the API and tus
    "object-src 'none'; "
    // No <base> is ever used. Allowing one would let injected markup re-point every
    // relative URL on the page — including the script tag — somewhere else.
    "base-uri 'none'; "
    "form-action 'self'; "
    // The CSP form of X-Frame-Options: DENY, which Caddy also sends for older browsers.
    "frame-ancestors 'none'; "
    // report-uri rather than report-to: the only one all three engines send today.
    "report-uri /api/csp-report";

// A report arrives from anybody's browser — including anonymous recipients on a
// download page — so it is attacker-controllable text on its way into the log. Each
// field is rendered to text and then sanitised by the shared forLog.
std::string reportField(const Json::Value& value) {
    std::string text;
    if (value.isString()) {
        text = value.asString();
    } else if (value.isIntegral()) {
        text = std::to_string(value.asInt64());
    } else if (!value.isNull()) {
        Json::StreamWriterBuilder compact;
        compact["indentation"] = "";
        text = Json::writeString(compact, value);
    }
    return forLog(std::move(text));
}

}  // namespace

void registerContentSecurityPolicy() {
    auto& app = drogon::app();

    // Pre-sending advice runs on every response on its way out — handler responses,
    // static files and the SPA fallback alike — so there is no route that can forget it.
    app.registerPreSendingAdvice(
        [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
            resp->addHeader("Content-Security-Policy", kPolicy);
        });

    app.registerHandler(
        "/api/csp-report",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            const std::string_view body = req->body();
            auto done = [&callback](drogon::HttpStatusCode status) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(status);
                callback(resp);
            };

            // A real report is well under a kilobyte.
            if (body.size() > 16 * 1024) {
                done(drogon::k413RequestEntityTooLarge);
                return;
            }

            // Parsed by hand: browsers send `application/csp-report`, which Drogon's
            // JSON helper does not recognise as JSON.
            Json::Value parsed;
            std::string errors;
            Json::CharReaderBuilder builder;
            const std::unique_ptr<Json::CharReader> reader{builder.newCharReader()};
            if (!reader->parse(body.data(), body.data() + body.size(), &parsed, &errors) ||
                !parsed.isObject() || !parsed["csp-report"].isObject()) {
                done(drogon::k400BadRequest);
                return;
            }

            const Json::Value& report = parsed["csp-report"];
            // effective-directive is the specific one (style-src-attr); violated-directive
            // is what matched in the policy (style-src). The first is the useful one.
            const Json::Value& directive = report.isMember("effective-directive")
                                               ? report["effective-directive"]
                                               : report["violated-directive"];
            LOG_WARN << "csp violation: " << reportField(directive) << " blocked "
                     << reportField(report["blocked-uri"]) << " on "
                     << reportField(report["document-uri"]) << " at "
                     << reportField(report["source-file"]) << ":"
                     << reportField(report["line-number"]) << " sample=\""
                     << reportField(report["script-sample"]) << "\"";
            done(drogon::k204NoContent);
        },
        {drogon::Post});
}

}  // namespace archive
