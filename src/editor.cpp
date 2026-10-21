// SPDX-License-Identifier: GPL-3.0-or-later
#include "editor.hpp"

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <sys/wait.h>
#include <unistd.h>

namespace jotdo {

namespace {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

}  // namespace

std::optional<std::string> edit_text(const std::string& text, const std::optional<std::string>& original) {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    std::string tmpl = std::string(dir && *dir ? dir : "/tmp") + "/jotdo-XXXXXX.txt";
    int fd = mkstemps(tmpl.data(), 4);
    if (fd < 0)
        throw std::runtime_error("could not create a temporary file");
    if (write(fd, text.data(), text.size()) != static_cast<ssize_t>(text.size())) {
        close(fd);
        unlink(tmpl.c_str());
        throw std::runtime_error("could not write the temporary file");
    }
    close(fd);

    const char* editor = std::getenv("VISUAL");
    if (!editor || !*editor)
        editor = std::getenv("EDITOR");
    if (!editor || !*editor)
        editor = "vi";
    std::string cmd = std::string(editor) + " " + shell_quote(tmpl);
    int status = std::system(cmd.c_str());
    std::string edited = read_file(tmpl);
    unlink(tmpl.c_str());
    if (status == -1 || !WIFEXITED(status))
        throw std::runtime_error(std::string("could not run ") + editor);
    if (edited == original.value_or(text))
        return std::nullopt;
    return edited;
}

std::filesystem::path keep_unsaved(const std::string& text) {
    std::filesystem::path dir;
    if (const char* s = std::getenv("XDG_STATE_HOME"); s && *s)
        dir = s;
    else
        dir = std::filesystem::path(std::getenv("HOME")) / ".local/state";
    dir /= "jotdo";
    std::filesystem::create_directories(dir);
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    auto path = dir / (std::string("unsaved-") + stamp + ".txt");
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

}  // namespace jotdo
