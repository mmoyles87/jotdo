// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jotdo {

struct Response {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;  // names in lower case
    std::string url;                             // where the request ended up, after redirects

    std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it == headers.end() ? "" : it->second;
    }
};

// One HTTPS connection, kept alive between requests, with Basic authentication.
class Http {
public:
    Http(std::string user, std::string password, std::string ca_file);
    ~Http();
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    // Throws on a network or TLS failure; any HTTP status is returned.
    Response request(const std::string& method, const std::string& url, const std::string& body = "",
                     const std::vector<std::string>& headers = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace jotdo
