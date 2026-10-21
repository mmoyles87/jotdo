// SPDX-License-Identifier: GPL-3.0-or-later
#include "dav.hpp"

#include <cctype>
#include <cstring>
#include <functional>

#include <pugixml.hpp>

namespace jotdo {

namespace {

// pugixml does not resolve namespaces; servers pick their own prefixes, so match local names.
const char* local(const pugi::xml_node& n) {
    const char* name = n.name();
    const char* colon = std::strrchr(name, ':');
    return colon ? colon + 1 : name;
}

pugi::xml_node child(const pugi::xml_node& n, const char* name) {
    for (auto c : n.children())
        if (std::strcmp(local(c), name) == 0)
            return c;
    return {};
}

std::vector<pugi::xml_node> children(const pugi::xml_node& n, const char* name) {
    std::vector<pugi::xml_node> out;
    for (auto c : n.children())
        if (std::strcmp(local(c), name) == 0)
            out.push_back(c);
    return out;
}

bool has_descendant(const pugi::xml_node& n, const char* name) {
    for (auto c : n.children())
        if (std::strcmp(local(c), name) == 0 || has_descendant(c, name))
            return true;
    return false;
}

// All the element's direct text, CDATA included, in order (a server may split it).
std::string text_of(const pugi::xml_node& n) {
    std::string s;
    for (auto c : n.children())
        if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata)
            s += c.value();
    return s;
}

std::string xml_escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// The <prop> elements of a <response> whose propstat status is 200.
std::vector<pugi::xml_node> ok_props(const pugi::xml_node& response) {
    std::vector<pugi::xml_node> out;
    for (auto ps : children(response, "propstat")) {
        std::string status = text_of(child(ps, "status"));
        if (status.find(" 200") != std::string::npos)
            out.push_back(child(ps, "prop"));
    }
    return out;
}

pugi::xml_node find_prop(const pugi::xml_node& response, const char* name) {
    for (auto p : ok_props(response))
        if (auto n = child(p, name))
            return n;
    return {};
}

struct Multistatus {
    pugi::xml_document doc;
    std::vector<pugi::xml_node> responses;
};

void parse_multistatus(const Response& r, const std::string& what, Multistatus& out) {
    if (r.status != 207)
        throw std::runtime_error(what + ": the server answered " + std::to_string(r.status));
    auto res = out.doc.load_buffer(r.body.data(), r.body.size());
    if (!res)
        throw std::runtime_error(what + ": unreadable reply (" + std::string(res.description()) + ")");
    auto root = out.doc.first_child();
    out.responses = children(root, "response");
}

std::string percent_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string utc_stamp(std::time_t t) {
    char buf[32];
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

const char* PROPFIND_HEADERS[] = {"Content-Type: application/xml; charset=utf-8"};

}  // namespace

Dav::Dav(const Account& account)
    : account_(account), http_(account.user, run_password_command(account.password_command), account.ca_file) {
    auto scheme = account_.url.find("://");
    auto slash = account_.url.find('/', scheme + 3);
    origin_ = slash == std::string::npos ? account_.url : account_.url.substr(0, slash);
}

// Every request carries the account's password, so an href the server hands back must stay on
// the account's own origin.
std::string Dav::absolute(const std::string& href) const {
    if (href.starts_with("http://") || href.starts_with("https://")) {
        if (href != origin_ && !href.starts_with(origin_ + "/"))
            throw std::runtime_error("the server pointed at another site (" + href + "); not following it");
        return href;
    }
    return origin_ + (href.starts_with("/") ? "" : "/") + href;
}

std::string Dav::find_home() {
    if (!home_.empty())
        return home_;
    const std::string principal_body =
        R"(<?xml version="1.0" encoding="utf-8"?><d:propfind xmlns:d="DAV:"><d:prop><d:current-user-principal/></d:prop></d:propfind>)";
    std::vector<std::string> headers{PROPFIND_HEADERS[0], "Depth: 0"};

    std::string principal;
    for (const std::string& start : {account_.url, origin_ + "/.well-known/caldav"}) {
        Response r = http_.request("PROPFIND", start, principal_body, headers);
        if (r.status != 207)
            continue;
        Multistatus ms;
        parse_multistatus(r, "finding the principal", ms);
        for (auto& resp : ms.responses)
            if (auto p = find_prop(resp, "current-user-principal"))
                if (auto h = child(p, "href"))
                    principal = text_of(h);
        if (!principal.empty())
            break;
    }
    if (principal.empty())
        throw std::runtime_error(account_.url + " does not look like a CalDAV server (no current-user-principal)");

    const std::string home_body =
        R"(<?xml version="1.0" encoding="utf-8"?><d:propfind xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav"><d:prop><c:calendar-home-set/></d:prop></d:propfind>)";
    Response r = http_.request("PROPFIND", absolute(principal), home_body, headers);
    Multistatus ms;
    parse_multistatus(r, "finding the calendar home", ms);
    for (auto& resp : ms.responses)
        if (auto p = find_prop(resp, "calendar-home-set"))
            if (auto h = child(p, "href"))
                home_ = absolute(text_of(h));
    if (home_.empty())
        throw std::runtime_error("the server named no calendar home for " + account_.user);
    if (!home_.ends_with("/"))
        home_ += "/";
    return home_;
}

std::vector<Collection> Dav::collections() {
    const std::string body = R"(<?xml version="1.0" encoding="utf-8"?>
<d:propfind xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav" xmlns:a="http://apple.com/ns/ical/">
  <d:prop><d:resourcetype/><d:displayname/><c:supported-calendar-component-set/><a:calendar-color/><d:current-user-privilege-set/></d:prop>
</d:propfind>)";
    Response r = http_.request("PROPFIND", find_home(), body, {PROPFIND_HEADERS[0], "Depth: 1"});
    Multistatus ms;
    parse_multistatus(r, "listing calendars", ms);

    std::vector<Collection> out;
    for (auto& resp : ms.responses) {
        auto type = find_prop(resp, "resourcetype");
        if (!type || !child(type, "calendar"))
            continue;
        Collection c;
        c.href = absolute(text_of(child(resp, "href")));
        if (!c.href.ends_with("/"))
            c.href += "/";
        c.name = text_of(find_prop(resp, "displayname"));
        if (c.name.empty()) {  // the last path segment
            std::string h = c.href.substr(0, c.href.size() - 1);
            c.name = percent_decode(h.substr(h.rfind('/') + 1));
        }
        if (auto comps = find_prop(resp, "supported-calendar-component-set")) {
            for (auto comp : children(comps, "comp")) {
                std::string n = comp.attribute("name").value();
                c.tasks |= n == "VTODO";
                c.events |= n == "VEVENT";
            }
        } else {
            c.tasks = c.events = true;
        }
        std::string color = text_of(find_prop(resp, "calendar-color"));
        if (color.size() >= 7 && color[0] == '#')
            c.color = color.substr(0, 7);  // #rrggbbaa → #rrggbb
        if (auto privs = find_prop(resp, "current-user-privilege-set"))
            c.writable = has_descendant(privs, "write") || has_descendant(privs, "write-content") || has_descendant(privs, "all");
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Resource> Dav::query(const Collection& c, const std::string& component, std::time_t from, std::time_t to) {
    std::string data = "<c:calendar-data/>";
    std::string filter = "<c:comp-filter name=\"" + component + "\"/>";
    if (from && to) {
        std::string range = "start=\"" + utc_stamp(from) + "\" end=\"" + utc_stamp(to) + "\"";
        data = "<c:calendar-data><c:expand " + range + "/></c:calendar-data>";
        filter = "<c:comp-filter name=\"" + component + "\"><c:time-range " + range + "/></c:comp-filter>";
    }
    std::string body = R"(<?xml version="1.0" encoding="utf-8"?>
<c:calendar-query xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <d:prop><d:getetag/>)" + data + R"(</d:prop>
  <c:filter><c:comp-filter name="VCALENDAR">)" + filter + R"(</c:comp-filter></c:filter>
</c:calendar-query>)";
    Response r = http_.request("REPORT", c.href, body, {PROPFIND_HEADERS[0], "Depth: 1"});
    Multistatus ms;
    parse_multistatus(r, "reading " + c.name, ms);

    std::vector<Resource> out;
    for (auto& resp : ms.responses) {
        Resource res;
        res.href = absolute(text_of(child(resp, "href")));
        res.etag = text_of(find_prop(resp, "getetag"));
        res.data = text_of(find_prop(resp, "calendar-data"));
        if (!res.data.empty())
            out.push_back(std::move(res));
    }
    return out;
}

std::vector<Resource> Dav::find_uid(const Collection& c, const std::string& component, const std::string& uid) {
    if (uid.empty())
        return {};
    std::string safe = xml_escape(uid);
    std::string body = R"(<?xml version="1.0" encoding="utf-8"?>
<c:calendar-query xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <d:prop><d:getetag/><c:calendar-data/></d:prop>
  <c:filter><c:comp-filter name="VCALENDAR"><c:comp-filter name=")" + component + R"("><c:prop-filter name="UID"><c:text-match collation="i;ascii-casemap">)" + safe + R"(</c:text-match></c:prop-filter></c:comp-filter></c:comp-filter></c:filter>
</c:calendar-query>)";
    Response r = http_.request("REPORT", c.href, body, {PROPFIND_HEADERS[0], "Depth: 1"});
    Multistatus ms;
    parse_multistatus(r, "searching " + c.name, ms);
    std::vector<Resource> out;
    for (auto& resp : ms.responses) {
        Resource res;
        res.href = absolute(text_of(child(resp, "href")));
        res.etag = text_of(find_prop(resp, "getetag"));
        res.data = text_of(find_prop(resp, "calendar-data"));
        if (!res.data.empty())
            out.push_back(std::move(res));
    }
    return out;
}

std::string Dav::put(const std::string& href, const std::string& ics, const std::string& etag) {
    std::vector<std::string> headers{"Content-Type: text/calendar; charset=utf-8"};
    headers.push_back(etag.empty() ? "If-None-Match: *" : "If-Match: " + etag);
    Response r = http_.request("PUT", href, ics, headers);
    if (r.status == 412)
        throw Conflict("changed on the server");
    if (r.status != 200 && r.status != 201 && r.status != 204)
        throw std::runtime_error("saving: the server answered " + std::to_string(r.status) +
                                 (r.body.empty() ? "" : " (" + r.body.substr(0, 200) + ")"));
    return r.header("etag");
}

void Dav::remove(const std::string& href, const std::string& etag) {
    if (etag.empty())  // without a version to check against, a newer edit could be deleted unseen
        throw std::runtime_error("the server gave no version for " + href + "; not deleting it blind");
    std::vector<std::string> headers{"If-Match: " + etag};
    Response r = http_.request("DELETE", href, "", headers);
    if (r.status == 412)
        throw Conflict("changed on the server");
    if (r.status != 200 && r.status != 204 && r.status != 404)
        throw std::runtime_error("deleting: the server answered " + std::to_string(r.status));
}

Resource Dav::get(const std::string& href) {
    Response r = http_.request("GET", href);
    if (r.status != 200)
        throw std::runtime_error("reading " + href + ": the server answered " + std::to_string(r.status));
    return {href, r.header("etag"), r.body};
}

}  // namespace jotdo
