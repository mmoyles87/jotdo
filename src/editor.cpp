// SPDX-License-Identifier: GPL-3.0-or-later
#include "editor.hpp"

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <sys/stat.h>
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
    std::ifstream f(tmpl, std::ios::binary);
    if (!f) {  // the editor removed or replaced the file: nothing to read, nothing to save
        unlink(tmpl.c_str());
        throw std::runtime_error("the edited text could not be read back; nothing was saved");
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string edited = buf.str();
    unlink(tmpl.c_str());
    if (status == -1 || !WIFEXITED(status))
        throw std::runtime_error(std::string("could not run ") + editor);
    if (WEXITSTATUS(status) != 0) {  // vim's :cq and the like: an abandoned edit
        if (edited != original.value_or(text))
            throw std::runtime_error(std::string(editor) + " exited with an error; not saved, your text is in " +
                                     keep_unsaved(edited).string());
        return std::nullopt;
    }
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
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    // A unique name, private to the user, so two failures in the same second keep both texts.
    std::string path = (dir / (std::string("unsaved-") + stamp + "-XXXXXX.txt")).string();
    int fd = mkstemps(path.data(), 4);
    if (fd < 0)
        throw std::runtime_error("could not keep the unsaved text in " + dir.string());
    fchmod(fd, 0600);
    bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ok = close(fd) == 0 && ok;
    if (!ok)
        throw std::runtime_error("could not keep the unsaved text in " + path);
    return path;
}

}  // namespace jotdo
