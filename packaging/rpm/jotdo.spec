%global ftxui_version 7.0.3
%global toml_version 3.4.0

Name:           jotdo
Version:        0.1.0
Release:        1%{?dist}
Summary:        Reminders from your CalDAV server, in a terminal
License:        GPL-3.0-or-later AND MIT
URL:            https://github.com/mmoyles87/jotdo
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz
Source1:        https://github.com/ArthurSonzogni/FTXUI/archive/v%{ftxui_version}/FTXUI-%{ftxui_version}.tar.gz
Source2:        https://github.com/marzer/tomlplusplus/archive/v%{toml_version}/tomlplusplus-%{toml_version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake >= 3.28
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(libical)
BuildRequires:  pugixml-devel
BuildRequires:  gtest-devel

Provides:       bundled(ftxui) = %{ftxui_version}
Provides:       bundled(tomlplusplus) = %{toml_version}

%description
jotdo reads and writes the task lists on a CalDAV server (Nextcloud, Radicale,
Baikal, Fastmail and others): the lists Apple Reminders, Tasks.org and jtx
Board show. A phone sees an edit made in jotdo as an edit.

It has a terminal browser for lists, Today, Scheduled and an agenda, handles
due dates, repeats and alerts typed as you would say them, and has a command
line with JSON output for scripts.

%prep
%autosetup
# %%autosetup takes only one -a
tar xf %{SOURCE1}
tar xf %{SOURCE2}

%build
%cmake -G Ninja \
    -DBUILD_SHARED_LIBS=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_FTXUI=$PWD/FTXUI-%{ftxui_version} \
    -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$PWD/tomlplusplus-%{toml_version}
%cmake_build

%check
%ctest

%install
%cmake_install

%files
%license LICENSE
%doc README.md
%{_bindir}/jotdo
%{_mandir}/man1/jotdo.1*

%changelog
* Mon Oct 26 2026 Matt Moyles <matt@mattmoyles.com> - 0.1.0-1
- Initial release
