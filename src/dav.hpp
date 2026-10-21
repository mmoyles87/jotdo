// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.hpp"
#include "http.hpp"

namespace jotdo {

// The item changed (or appeared) on the server since it was loaded; nothing was written.
class Conflict : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A calendar collection: a task list, a calendar, or both.
struct Collection {
    std::string href;   // absolute URL, ending in /
    std::string name;
    std::string color;  // "#rrggbb", or empty
    bool tasks = false;
    bool events = false;
    bool writable = true;
};

// One calendar object as the server holds it.
struct Resource {
    std::string href;  // absolute URL
    std::string etag;
    std::string data;  // the iCalendar text
};

// The account's calendars over CalDAV (RFC 4791).
class Dav {
public:
    explicit Dav(const Account& account);

    // Every calendar collection in the user's calendar home, shared ones included.
    std::vector<Collection> collections();

    // Every object with a `component` ("VTODO", "VEVENT") in the collection. For events, only
    // those between `from` and `to`, with repeating ones expanded into their occurrences.
    std::vector<Resource> query(const Collection& c, const std::string& component, std::time_t from = 0, std::time_t to = 0);

    // Writes `ics` at `href`: over the version with `etag`, or as a new object when `etag` is
    // empty. Returns the new ETag (empty if the server did not say). Throws Conflict on 412.
    std::string put(const std::string& href, const std::string& ics, const std::string& etag);

    // Removes the object, if it is still the version with `etag`. Throws Conflict on 412.
    void remove(const std::string& href, const std::string& etag);

    // The object at `href`, fresh from the server.
    Resource get(const std::string& href);

    const Account& account() const { return account_; }

private:
    Account account_;
    Http http_;
    std::string origin_;  // scheme://host[:port]
    std::string home_;    // the calendar home, once found

    std::string absolute(const std::string& href) const;
    std::string find_home();
};

}  // namespace jotdo
