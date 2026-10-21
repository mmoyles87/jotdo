# jotdo

Reminders from your CalDAV server, in a terminal.

A CalDAV server keeps task lists alongside calendars: Nextcloud, Radicale, Baïkal, Fastmail
and most others do. Apple Reminders on iPhone, iPad and Mac shows the lists of a CalDAV account
as ordinary reminder lists, and Tasks.org or jtx Board do the same on Android through DAVx⁵.
jotdo reads and writes those lists directly: no sync service, nothing to run on the server, and
a phone sees an edit made in jotdo as an edit.

- Browse lists, Today, Scheduled and an agenda of the coming days in a terminal UI; tick
  reminders off with the space bar or a click.
- Due dates and times, repeats, alerts, priority and notes, typed the way you would say them:
  `tomorrow 9am`, `every mon`, `1h before`.
- A repeating reminder ticked off moves on to its next date; times stay put across daylight
  saving changes.
- If a reminder changes elsewhere while you edit it, the save is refused and your text is kept.
- Scriptable: `list`, `today`, `new`, `done`, `edit`, `rm` and `agenda` work without the UI.

## Build

Needs a C++20 compiler, CMake 3.24+, libcurl and libical 4; pugixml is used when
installed and fetched otherwise. FTXUI and toml++ are fetched at configure time.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run the tests with `ctest --test-dir build`.

## Configure

`~/.config/jotdo/config.toml`:

```toml
default = "home"
agenda_days = 7                                      # how far ahead the agenda looks
mouse = true                                         # false: keep the terminal's own text selection

[accounts.home]
url = "https://cloud.example.com"                    # the server, or its DAV address
user = "me"
password_command = "pass show cloud/app-password"    # first line of output is the password
default_list = "Personal"                            # optional: where new reminders go
# lists = ["Personal", "Family"]                     # optional: which lists, in this order
# calendars = ["Family"]                             # optional: which calendars feed the agenda
# ca_file = "~/certs/my-ca.pem"                      # optional: also trust this CA
```

jotdo finds the calendars itself, starting from `url` and falling back to
`/.well-known/caldav`. With Nextcloud, use an app password (Settings → Security) rather than
the account's own.

`password_command` runs through `/bin/sh`, so `pass`, `secret-tool`,
`security find-internet-password -w` or `op read` all work.

## Use

```sh
jotdo                                  # browse
jotdo list                             # open reminders, numbered, by due date
jotdo list Family                      # one list
jotdo today                            # due today, and overdue
jotdo new Call the dentist             # add to the default list
jotdo new -d "fri 5pm" -A "1h before" -l Family Pick up the cake
jotdo new -d "mon 8:00" -r "every mon" Put the bins out
jotdo new                              # write it in $EDITOR
jotdo done 3                           # tick off; a repeating one moves to its next date
jotdo edit 3                           # edit in $EDITOR (or: jotdo edit 3 < reminder.txt)
jotdo rm 3
jotdo agenda 3                         # events and reminders for the next 3 days
```

Numbers stay the same across `list`, `today` and `all` until the reminders change, so
`jotdo done 3` means the third line of `jotdo all`. The start of a reminder's UID works too.

In the editor a reminder looks like this; lines left out stay as they were:

```
Call the dentist
due: 2026-10-22 09:00
repeat: every Monday
alert: at due, 1h before
priority: high
list: Personal

Notes, after the first blank line.
```

| Key | |
|---|---|
| `j` `k` / arrows | move |
| `tab`, `h` `l` | switch between the lists and the reminders |
| `1`–`9` | open the n-th entry in the sidebar |
| `space` / `x` | tick off, or back on |
| `a` | add: a title, and a due date after ` @` (`Call Mum @ fri 6pm`) |
| `n` | new reminder in `$EDITOR` |
| `enter` / `e` | edit in `$VISUAL` or `$EDITOR` |
| `d` `d` | delete |
| `c` | show finished reminders in a list |
| `/` | search titles and notes; `esc` clears |
| `r` | reload (it also reloads every two minutes) |
| `q` | quit |

With the mouse: click a list or a reminder to select it, click a reminder's circle to tick it
off, double-click to edit, scroll with the wheel, and click the hints along the bottom.

## Dates, repeats and alerts

| Field | Examples |
|---|---|
| due | `today`, `tomorrow 9am`, `fri`, `next tue 14:30`, `oct 24`, `24 oct 7pm`, `10/30`, `2026-10-24 18:00`, `in 2h`, `in 3 days`, `noon`, `tonight`, `17:00` |
| repeat | `daily`, `weekdays`, `weekends`, `weekly`, `every 2 weeks`, `every mon, wed`, `monthly`, `yearly`, or an RRULE such as `FREQ=MONTHLY;BYDAY=1MO` |
| alert | `at due`, `15m before`, `1h before`, `1h30m before`, `1d before`; several with commas |
| priority | `high`, `medium`, `low`, `none` (or `!!!`, `!!`, `!`) |

A time alone is the next time it comes round: `9am` typed in the afternoon is tomorrow
morning. A weekday is the next one after today.

## Theme

```toml
[theme]
preset = "forest"        # forest, gruvbox, nord or terminal
accent = "#fabd2f"       # and any colour below, to change just that one
```

| Colour | Used for |
|---|---|
| `background` | the detail pane |
| `panel` | the sidebar, status bar and footer |
| `text` | text |
| `muted` | finished reminders, hints, labels |
| `status` | the status bar's text |
| `accent` | the selected reminder, headings, circles |
| `link` | due dates, list names |
| `highlight` | the open prompt, dividers, the selection while the sidebar has focus |
| `search` | the prompt |
| `selected-text` | text on the selected reminder |

A colour is `#rrggbb`, `default` (the terminal's own), an ANSI name (`red`, `bright-black`, …)
or a 256-colour number.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
