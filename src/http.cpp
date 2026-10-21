// SPDX-License-Identifier: GPL-3.0-or-later
#include "http.hpp"

#include <cctype>
#include <mutex>
#include <stdexcept>

#include <curl/curl.h>

namespace jotdo {

namespace {

size_t on_body(char* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

size_t on_header(char* data, size_t size, size_t n, void* out) {
    std::string line(data, size * n);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        for (auto& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(0, 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        (*static_cast<std::map<std::string, std::string>*>(out))[name] = value;
    } else if (line.starts_with("HTTP/")) {
        static_cast<std::map<std::string, std::string>*>(out)->clear();  // a new response, after a redirect
    }
    return size * n;
}

}  // namespace

struct Http::Impl {
    CURL* curl = nullptr;
    std::string user, password, ca_file;
};

Http::Http(std::string user, std::string password, std::string ca_file) : impl_(std::make_unique<Impl>()) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    impl_->curl = curl_easy_init();
    if (!impl_->curl)
        throw std::runtime_error("could not start libcurl");
    impl_->user = std::move(user);
    impl_->password = std::move(password);
    impl_->ca_file = std::move(ca_file);
}

Http::~Http() {
    if (impl_->curl)
        curl_easy_cleanup(impl_->curl);
}

Response Http::request(const std::string& method, const std::string& url, const std::string& body,
                       const std::vector<std::string>& headers) {
    CURL* c = impl_->curl;
    curl_easy_reset(c);
    Response r;
    curl_slist* list = nullptr;
    for (auto& h : headers)
        list = curl_slist_append(list, h.c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(c, CURLOPT_USERNAME, impl_->user.c_str());
    curl_easy_setopt(c, CURLOPT_PASSWORD, impl_->password.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_UNRESTRICTED_AUTH, 0L);  // never send the password to another host
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, url.starts_with("https://") ? "https" : "http,https");  // no downgrade
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "jotdo/0.1");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (!impl_->ca_file.empty())
        curl_easy_setopt(c, CURLOPT_CAINFO, impl_->ca_file.c_str());
    if (!body.empty() || method == "PUT" || method == "REPORT" || method == "PROPFIND") {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);
    // A redirect keeps the method (PROPFIND, REPORT) instead of turning into a GET.
    curl_easy_setopt(c, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(list);
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string(curl_easy_strerror(rc)) + " (" + url + ")");
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    char* effective = nullptr;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &effective);
    r.url = effective ? effective : url;
    if (r.status == 401)
        throw std::runtime_error("the server refused the user name or password (" + url + ")");
    return r;
}

}  // namespace jotdo
