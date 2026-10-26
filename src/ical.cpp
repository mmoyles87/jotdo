// SPDX-License-Identifier: GPL-3.0-or-later
#include "ical.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>

#include <libical/ical.h>

namespace jotdo {

namespace {

void init() {
    static std::once_flag once;
    std::call_once(once, [] { icaltimezone_set_tzid_prefix(""); });  // TZID=America/New_York, as phones write
}

struct CompFree {
    void operator()(icalcomponent* c) const { icalcomponent_free(c); }
};
using Comp = std::unique_ptr<icalcomponent, CompFree>;

#if ICAL_MAJOR_VERSION < 4  // libical 3, as Debian, Ubuntu and Fedora ship it
icalcomponent* icalcomponent_clone(icalcomponent* c) { return icalcomponent_new_clone(c); }
int icaldurationtype_as_seconds(icaldurationtype d) { return icaldurationtype_as_int(d); }
#endif

// A parsed RRULE, false when it does not parse. libical 4 reference-counts rules; libical 3
// passes them by value.
class Rule {
public:
    explicit Rule(const std::string& s) {
#if ICAL_MAJOR_VERSION >= 4
        r_ = icalrecurrencetype_new_from_string(s.c_str());
        ok_ = r_ != nullptr;
#else
        r_ = icalrecurrencetype_from_string(s.c_str());
        ok_ = r_.freq != ICAL_NO_RECURRENCE;
#endif
    }
    ~Rule() {
#if ICAL_MAJOR_VERSION >= 4
        if (r_)
            icalrecurrencetype_unref(r_);
#endif
    }
    Rule(const Rule&) = delete;
    Rule& operator=(const Rule&) = delete;

    explicit operator bool() const { return ok_; }
    icalproperty* property() const { return icalproperty_new_rrule(r_); }
    icalrecur_iterator* iterate(icaltimetype start) const { return icalrecur_iterator_new(r_, start); }

private:
#if ICAL_MAJOR_VERSION >= 4
    icalrecurrencetype* r_ = nullptr;
#else
    icalrecurrencetype r_{};
#endif
    bool ok_ = false;
};

Comp parse(const std::string& ics) {
    init();
    Comp c(icalparser_parse_string(ics.c_str()));
    if (!c)
        throw std::runtime_error("unreadable calendar data");
    return c;
}

std::string text(icalcomponent* c) {
    char* s = icalcomponent_as_ical_string_r(c);
    std::string out = s ? s : "";
    free(s);
    return out;
}

icaltimezone* local_zone() {
    init();
    static icaltimezone* zone = [] {
        icaltimezone* z = icaltimezone_get_builtin_timezone(local_tzid().c_str());
        return z ? z : icaltimezone_get_utc_timezone();
    }();
    return zone;
}

std::time_t make_local(int year, int mon0, int mday) {
    std::tm tm{};
    tm.tm_year = year - 1900, tm.tm_mon = mon0, tm.tm_mday = mday, tm.tm_isdst = -1;
    return std::mktime(&tm);
}

// A property's date or date-time as a Due, resolving its TZID against the object's VTIMEZONEs
// or, failing that, the built-in zone database. Floating times are taken as local time.
std::optional<Due> due_of(icalproperty* p, icalcomponent* comp) {
    if (!p)
        return std::nullopt;
    icaltimetype tt = icalproperty_get_datetime_with_component(p, comp);
    if (icaltime_is_null_time(tt))
        return std::nullopt;
    if (icaltime_is_date(tt))
        return Due{make_local(tt.year, tt.month - 1, tt.day), false};
    const icaltimezone* zone = icaltime_is_utc(tt) ? icaltimezone_get_utc_timezone() : tt.zone;
    if (!zone)
        if (icalparameter* tz = icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER))
            zone = icaltimezone_get_builtin_timezone(icalparameter_get_tzid(tz));
    if (!zone)
        zone = local_zone();
    return Due{static_cast<std::time_t>(icaltime_as_timet_with_zone(tt, zone)), true};
}

std::time_t time_of(icalproperty* p, icalcomponent* comp) {
    auto d = due_of(p, comp);
    return d ? d->t : 0;
}

// The VCALENDAR around `comp`.
icalcomponent* calendar_of(icalcomponent* comp) {
    icalcomponent* top = comp;
    while (icalcomponent_get_parent(top))
        top = icalcomponent_get_parent(top);
    return top;
}

void remove_all(icalcomponent* c, icalproperty_kind kind) {
    while (icalproperty* p = icalcomponent_get_first_property(c, kind)) {
        icalcomponent_remove_property(c, p);
        icalproperty_free(p);
    }
}

// A DUE or DTSTART property for `d`: VALUE=DATE for a day, local wall time with TZID otherwise.
// The VTIMEZONE it names is added to the calendar if it is not there yet.
icalproperty* new_time(icalproperty_kind kind, icaltimetype tt) {
    switch (kind) {
    case ICAL_DUE_PROPERTY:
        return icalproperty_new_due(tt);
    case ICAL_DTEND_PROPERTY:
        return icalproperty_new_dtend(tt);
    default:
        return icalproperty_new_dtstart(tt);
    }
}

// A DUE, DTSTART or DTEND property for `d`: VALUE=DATE for a day, local wall time with TZID
// otherwise. The VTIMEZONE it names is added to the calendar if it is not there yet.
icalproperty* time_prop(icalproperty_kind kind, const Due& d, icalcomponent* comp, icaltimezone* in_zone = nullptr) {
    if (!d.has_time) {
        std::tm tm{};
        localtime_r(&d.t, &tm);
        icaltimetype tt = icaltime_null_date();
        tt.year = tm.tm_year + 1900, tt.month = tm.tm_mon + 1, tt.day = tm.tm_mday;
        return new_time(kind, tt);
    }
    icaltimezone* zone = in_zone ? in_zone : local_zone();
    if (zone == icaltimezone_get_utc_timezone())
        return new_time(kind, icaltime_from_timet_with_zone(d.t, 0, zone));
    icaltimetype tt = icaltime_from_timet_with_zone(d.t, 0, zone);
    tt.zone = nullptr;  // written as wall time plus the TZID parameter
    icalproperty* p = new_time(kind, tt);
    // The zone's location, America/New_York, as phones write it: libical 3 names zones from the
    // system database /freeassociation.sourceforge.net/America/New_York whatever the prefix.
    const char* location = icaltimezone_get_location(zone);
    std::string tzid = location && *location ? location : icaltimezone_get_tzid(zone);
    icalproperty_add_parameter(p, icalparameter_new_tzid(tzid.c_str()));
    icalcomponent* cal = calendar_of(comp);
    if (!icalcomponent_get_timezone(cal, tzid.c_str()) && icaltimezone_get_builtin_timezone(tzid.c_str()) == zone)
        if (icalcomponent* vtz = icaltimezone_get_component(zone)) {
            icalcomponent* copy = icalcomponent_clone(vtz);
            if (icalproperty* id = icalcomponent_get_first_property(copy, ICAL_TZID_PROPERTY))
                icalproperty_set_tzid(id, tzid.c_str());
            icalcomponent_add_component(cal, copy);
        }
    return p;
}

// -PT15M, -PT1H30M, -P1D: the units phones write, rather than libical's -PT900S.
// m minutes before the due time; a negative m is an alarm after it.
icaldurationtype minutes_before(int m) {
    icaldurationtype d = icaldurationtype_null_duration();
    d.is_neg = m > 0;
    m = std::abs(m);
    if (m % 10080 == 0 && m > 0) {
        d.weeks = static_cast<unsigned>(m / 10080);
        return d;
    }
    d.days = static_cast<unsigned>(m / 1440);
    d.hours = static_cast<unsigned>(m % 1440 / 60);
    d.minutes = static_cast<unsigned>(m % 60);
    return d;
}

// The zone a date-time property is written in, if it names one jotdo can use: the built-in
// zone of that TZID (so it can be written again with its VTIMEZONE). Null for dates, UTC and
// floating times.
icaltimezone* zone_of(icalproperty* p) {
    if (!p)
        return nullptr;
    icalparameter* tz = icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER);
    if (!tz)
        return nullptr;
    return icaltimezone_get_builtin_timezone(icalparameter_get_tzid(tz));
}

icaltimetype utc_now(std::time_t now) {
    return icaltime_from_timet_with_zone(now, 0, icaltimezone_get_utc_timezone());
}

// Minutes before `due` at which each VALARM of the VTODO fires.
std::vector<int> alerts_of(icalcomponent* vtodo, const std::optional<Due>& due) {
    std::vector<int> out;
    std::optional<Due> start = due_of(icalcomponent_get_first_property(vtodo, ICAL_DTSTART_PROPERTY), vtodo);
    for (icalcomponent* a = icalcomponent_get_first_component(vtodo, ICAL_VALARM_COMPONENT); a;
         a = icalcomponent_get_next_component(vtodo, ICAL_VALARM_COMPONENT)) {
        icalproperty* tp = icalcomponent_get_first_property(a, ICAL_TRIGGER_PROPERTY);
        if (!tp)
            continue;
        icaltriggertype trig = icalproperty_get_trigger(tp);
        if (!icaltime_is_null_time(trig.time)) {  // an absolute time
            if (!due)
                continue;
            const icaltimezone* zone = icaltime_is_utc(trig.time) ? icaltimezone_get_utc_timezone() : local_zone();
            std::time_t at = icaltime_as_timet_with_zone(trig.time, zone);
            out.push_back(static_cast<int>((due->t - at) / 60));
            continue;
        }
        int secs = icaldurationtype_as_seconds(trig.duration);
        icalparameter* rel = icalproperty_get_first_parameter(tp, ICAL_RELATED_PARAMETER);
        bool to_end = rel && icalparameter_get_related(rel) == ICAL_RELATED_END;
        std::time_t base_shift = 0;  // how far the trigger's base is before the due time
        if (!to_end && start && due)
            base_shift = due->t - start->t;
        out.push_back(static_cast<int>((-secs + base_shift) / 60));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The property's value: unescaped for TEXT ("a, b" rather than "a\, b"), as written otherwise.
std::string prop_text(icalcomponent* c, icalproperty_kind kind) {
    icalproperty* p = icalcomponent_get_first_property(c, kind);
    if (!p)
        return "";
    icalvalue* val = icalproperty_get_value(p);
    const char* v = val && icalvalue_isa(val) == ICAL_TEXT_VALUE ? icalvalue_get_text(val) : icalproperty_get_value_as_string(p);
    return v ? v : "";
}

void set_text(icalcomponent* c, icalproperty_kind kind, const std::string& value) {
    remove_all(c, kind);
    if (value.empty())
        return;
    icalproperty* p = icalproperty_new(kind);
    icalproperty_set_value(p, icalvalue_new_text(value.c_str()));
    icalcomponent_add_property(c, p);
}

}  // namespace

std::string local_tzid() {
    if (const char* tz = std::getenv("TZ"); tz && *tz) {
        std::string s = tz[0] == ':' ? tz + 1 : tz;
        if (auto pos = s.find("zoneinfo/"); pos != std::string::npos)
            s = s.substr(pos + 9);
        if (!s.empty() && s.find('/') != std::string::npos)
            return s;
        if (s == "UTC" || s == "GMT")
            return "UTC";
    }
    std::error_code ec;
    auto target = std::filesystem::read_symlink("/etc/localtime", ec).string();
    if (auto pos = target.find("zoneinfo/"); !ec && pos != std::string::npos)
        return target.substr(pos + 9);
    return "UTC";
}

std::string new_uid() {
    std::random_device rd;
    std::mt19937_64 g(rd());
    const char* hex = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 32; ++i) {
        s += hex[g() % 16];
        if (i == 7 || i == 11 || i == 15 || i == 19)
            s += '-';
    }
    s[14] = '4';  // a version-4 UUID, upper case like Apple's
    s[19] = "89AB"[g() % 4];
    return s;
}

std::optional<Task> parse_task(const std::string& ics) {
    Comp cal = parse(ics);
    icalcomponent* v = icalcomponent_isa(cal.get()) == ICAL_VTODO_COMPONENT
                           ? cal.get()
                           : icalcomponent_get_first_component(cal.get(), ICAL_VTODO_COMPONENT);
    if (!v)
        return std::nullopt;
    Task t;
    t.raw = ics;
    t.uid = prop_text(v, ICAL_UID_PROPERTY);
    t.title = prop_text(v, ICAL_SUMMARY_PROPERTY);
    t.notes = prop_text(v, ICAL_DESCRIPTION_PROPERTY);
    t.url = prop_text(v, ICAL_URL_PROPERTY);
    t.due = due_of(icalcomponent_get_first_property(v, ICAL_DUE_PROPERTY), v);
    if (!t.due)  // some clients set only a start
        t.due = due_of(icalcomponent_get_first_property(v, ICAL_DTSTART_PROPERTY), v);
    t.repeat = prop_text(v, ICAL_RRULE_PROPERTY);
    t.alerts = alerts_of(v, t.due);
    if (icalproperty* p = icalcomponent_get_first_property(v, ICAL_PRIORITY_PROPERTY))
        t.priority = icalproperty_get_priority(p);
    icalproperty* completed = icalcomponent_get_first_property(v, ICAL_COMPLETED_PROPERTY);
    t.done = icalcomponent_get_status(v) == ICAL_STATUS_COMPLETED || completed;
    t.completed = time_of(completed, v);
    t.created = time_of(icalcomponent_get_first_property(v, ICAL_CREATED_PROPERTY), v);
    t.modified = time_of(icalcomponent_get_first_property(v, ICAL_LASTMODIFIED_PROPERTY), v);
    return t;
}

std::string write_task(const Task& t, std::time_t now) {
    init();
    Comp cal;
    icalcomponent* v = nullptr;
    std::optional<Task> before;
    if (!t.raw.empty()) {
        cal = parse(t.raw);
        v = icalcomponent_get_first_component(cal.get(), ICAL_VTODO_COMPONENT);
        before = parse_task(t.raw);
    }
    if (!v) {
        cal.reset(icalcomponent_new(ICAL_VCALENDAR_COMPONENT));
        icalcomponent_add_property(cal.get(), icalproperty_new_version("2.0"));
        icalcomponent_add_property(cal.get(), icalproperty_new_prodid("-//jotdo//jotdo 0.1//EN"));
        v = icalcomponent_new(ICAL_VTODO_COMPONENT);
        icalcomponent_add_component(cal.get(), v);
        icalcomponent_add_property(v, icalproperty_new_uid(t.uid.empty() ? new_uid().c_str() : t.uid.c_str()));
        icalcomponent_add_property(v, icalproperty_new_created(utc_now(now)));
    }

    set_text(v, ICAL_SUMMARY_PROPERTY, t.title);
    set_text(v, ICAL_DESCRIPTION_PROPERTY, t.notes);
    if (!before || before->url != t.url) {
        remove_all(v, ICAL_URL_PROPERTY);
        if (!t.url.empty())
            icalcomponent_add_property(v, icalproperty_new_url(t.url.c_str()));
    }

    // Dates. DTSTART follows DUE when it was there before or a repeat needs it, keeping its
    // distance (a task that starts at 08:00 and is due at 17:00 still starts 9 hours earlier),
    // and both stay in the zone they were written in.
    icalproperty* old_start_prop = icalcomponent_get_first_property(v, ICAL_DTSTART_PROPERTY);
    bool had_start = old_start_prop;
    std::optional<Due> old_start = due_of(old_start_prop, v);
    icaltimezone* zone = zone_of(icalcomponent_get_first_property(v, ICAL_DUE_PROPERTY));
    if (!zone)
        zone = zone_of(old_start_prop);
    bool due_changed = !before || before->due != t.due;
    if (due_changed || (!t.repeat.empty() && !had_start)) {
        std::time_t lead = 0;
        if (before && before->due && old_start && old_start->has_time == before->due->has_time && old_start->t <= before->due->t)
            lead = before->due->t - old_start->t;
        remove_all(v, ICAL_DUE_PROPERTY);
        remove_all(v, ICAL_DTSTART_PROPERTY);
        if (t.due) {
            icalcomponent_add_property(v, time_prop(ICAL_DUE_PROPERTY, *t.due, v, zone));
            if (had_start || !t.repeat.empty()) {
                Due start = *t.due;
                if (lead)
                    start.t = start.has_time ? start.t - lead : add_days(start.t, -static_cast<int>(std::lround(lead / 86400.0)));
                icalcomponent_add_property(v, time_prop(ICAL_DTSTART_PROPERTY, start, v, zone));
            }
        }
    }
    if (!before || before->repeat != t.repeat) {
        remove_all(v, ICAL_RRULE_PROPERTY);
        if (!t.repeat.empty()) {
            if (!t.due)
                throw std::invalid_argument("a repeating reminder needs a due date");
            Rule r(t.repeat);
            if (!r)
                throw std::invalid_argument("not a repeat rule: " + t.repeat);
            icalcomponent_add_property(v, r.property());
        }
    }

    remove_all(v, ICAL_PRIORITY_PROPERTY);
    if (t.priority > 0)
        icalcomponent_add_property(v, icalproperty_new_priority(t.priority));

    // Completion.
    bool was_done = before && before->done;
    if (t.done != was_done || !before) {
        remove_all(v, ICAL_STATUS_PROPERTY);
        remove_all(v, ICAL_COMPLETED_PROPERTY);
        remove_all(v, ICAL_PERCENTCOMPLETE_PROPERTY);
        if (t.done) {
            icalcomponent_add_property(v, icalproperty_new_status(ICAL_STATUS_COMPLETED));
            icalcomponent_add_property(v, icalproperty_new_completed(utc_now(t.completed ? t.completed : now)));
            icalcomponent_add_property(v, icalproperty_new_percentcomplete(100));
        } else {
            icalcomponent_add_property(v, icalproperty_new_status(ICAL_STATUS_NEEDSACTION));
        }
    }

    // Alarms are only rewritten when they changed, so a phone's own alarm metadata survives.
    auto before_due = [](const std::vector<int>& a) {
        std::vector<int> out;
        for (int m : a)
            if (m >= 0)
                out.push_back(m);
        return out;
    };
    std::vector<int> wanted = t.alerts;
    if (before)  // alarms after the due time are not edited in jotdo: carry them over
        for (int m : before->alerts)
            if (m < 0 && std::find(wanted.begin(), wanted.end(), m) == wanted.end())
                wanted.push_back(m);
    bool alarms_changed = !before ? !t.alerts.empty()
                                  : before_due(before->alerts) != before_due(t.alerts) || (due_changed && !wanted.empty());
    if (alarms_changed) {
        std::vector<icalcomponent*> old;
        for (icalcomponent* a = icalcomponent_get_first_component(v, ICAL_VALARM_COMPONENT); a;
             a = icalcomponent_get_next_component(v, ICAL_VALARM_COMPONENT))
            old.push_back(a);
        for (auto* a : old) {
            icalcomponent_remove_component(v, a);
            icalcomponent_free(a);
        }
        if (t.due)
            for (int m : wanted) {
                icalcomponent* a = icalcomponent_new(ICAL_VALARM_COMPONENT);
                icalcomponent_add_property(a, icalproperty_new_action(ICAL_ACTION_DISPLAY));
                icalcomponent_add_property(a, icalproperty_new_description("Reminder"));
                icaltriggertype trig{};
                trig.time = icaltime_null_time();
                trig.duration = minutes_before(m);
                icalproperty* tp = icalproperty_new_trigger(trig);
                icalproperty_add_parameter(tp, icalparameter_new_related(ICAL_RELATED_END));
                icalcomponent_add_property(a, tp);
                icalcomponent_add_component(v, a);
            }
    }

    remove_all(v, ICAL_DTSTAMP_PROPERTY);
    icalcomponent_add_property(v, icalproperty_new_dtstamp(utc_now(now)));
    remove_all(v, ICAL_LASTMODIFIED_PROPERTY);
    icalcomponent_add_property(v, icalproperty_new_lastmodified(utc_now(now)));
    int seq = 1;
    if (icalproperty* p = icalcomponent_get_first_property(v, ICAL_SEQUENCE_PROPERTY))
        seq = icalproperty_get_sequence(p) + 1;
    remove_all(v, ICAL_SEQUENCE_PROPERTY);
    if (before)
        icalcomponent_add_property(v, icalproperty_new_sequence(seq));
    return text(cal.get());
}

bool advance(Task& t) {
    if (t.repeat.empty() || !t.due)
        return false;
    init();
    Rule rule(t.repeat);
    if (!rule)
        return false;
    // Iterate the series from its start (DTSTART, else the due date) in the zone it is written
    // in, so "every Monday 08:00" stays at 08:00 there across daylight-saving changes.
    Due start = *t.due;
    icaltimezone* zone = nullptr;
    if (!t.raw.empty()) {
        Comp cal = parse(t.raw);
        if (icalcomponent* v = icalcomponent_get_first_component(cal.get(), ICAL_VTODO_COMPONENT)) {
            icalproperty* sp = icalcomponent_get_first_property(v, ICAL_DTSTART_PROPERTY);
            if (auto s = due_of(sp, v))
                start = *s;
            zone = zone_of(sp);
            if (!zone)
                zone = zone_of(icalcomponent_get_first_property(v, ICAL_DUE_PROPERTY));
        }
    }
    if (!zone)
        zone = local_zone();
    std::time_t lead = t.due->t - start.t;  // the due date's distance after the start
    icaltimetype dtstart;
    if (start.has_time) {
        dtstart = icaltime_from_timet_with_zone(start.t, 0, zone);
    } else {
        std::tm tm{};
        localtime_r(&start.t, &tm);
        // not icaltime_null_date(): libical 3 repeats the first occurrence of a series whose
        // start has its time fields at -1
        dtstart = icaltime_null_time();
        dtstart.is_date = 1;
        dtstart.year = tm.tm_year + 1900, dtstart.month = tm.tm_mon + 1, dtstart.day = tm.tm_mday;
    }
    icalrecur_iterator* it = rule.iterate(dtstart);
    if (!it)
        return false;
    std::optional<Due> next;
    int used = 0;  // occurrences up to and including the current one
    for (int guard = 0; guard < 100000; ++guard) {
        icaltimetype n = icalrecur_iterator_next(it);
        if (icaltime_is_null_time(n))
            break;
        Due d;
        if (start.has_time)
            d = {static_cast<std::time_t>(icaltime_as_timet_with_zone(n, zone)), true};
        else
            d = {make_local(n.year, n.month - 1, n.day), false};
        std::time_t due_at = d.has_time ? d.t + lead : add_days(d.t, static_cast<int>(std::lround(lead / 86400.0)));
        if (due_at > t.due->t) {
            next = Due{due_at, t.due->has_time};
            break;
        }
        ++used;
    }
    icalrecur_iterator_free(it);
    if (!next)
        return false;
    // The series is saved as starting at the new date, so a COUNT has to shrink by the
    // occurrences already used, or it would start over each time.
    if (auto pos = t.repeat.find("COUNT="); pos != std::string::npos) {
        size_t end = t.repeat.find(';', pos);
        int count = std::stoi(t.repeat.substr(pos + 6, end == std::string::npos ? std::string::npos : end - pos - 6));
        t.repeat.replace(pos + 6, (end == std::string::npos ? t.repeat.size() : end) - pos - 6,
                         std::to_string(std::max(1, count - used)));
    }
    t.due = next;
    return true;
}

namespace {

// Minutes before the start at which each VALARM of an event fires.
std::vector<int> event_alerts(icalcomponent* v, std::time_t start, std::time_t end) {
    std::vector<int> out;
    for (icalcomponent* a = icalcomponent_get_first_component(v, ICAL_VALARM_COMPONENT); a;
         a = icalcomponent_get_next_component(v, ICAL_VALARM_COMPONENT)) {
        icalproperty* tp = icalcomponent_get_first_property(a, ICAL_TRIGGER_PROPERTY);
        if (!tp)
            continue;
        icaltriggertype trig = icalproperty_get_trigger(tp);
        if (!icaltime_is_null_time(trig.time)) {
            const icaltimezone* zone = icaltime_is_utc(trig.time) ? icaltimezone_get_utc_timezone() : local_zone();
            out.push_back(static_cast<int>((start - icaltime_as_timet_with_zone(trig.time, zone)) / 60));
            continue;
        }
        int secs = icaldurationtype_as_seconds(trig.duration);
        icalparameter* rel = icalproperty_get_first_parameter(tp, ICAL_RELATED_PARAMETER);
        std::time_t base = rel && icalparameter_get_related(rel) == ICAL_RELATED_END ? end : start;
        out.push_back(static_cast<int>((start - (base + secs)) / 60));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Event read_event(icalcomponent* v) {
    Event e;
    e.uid = prop_text(v, ICAL_UID_PROPERTY);
    e.title = prop_text(v, ICAL_SUMMARY_PROPERTY);
    e.location = prop_text(v, ICAL_LOCATION_PROPERTY);
    e.notes = prop_text(v, ICAL_DESCRIPTION_PROPERTY);
    e.repeat = prop_text(v, ICAL_RRULE_PROPERTY);
    e.recurring = !e.repeat.empty() || icalcomponent_get_first_property(v, ICAL_RECURRENCEID_PROPERTY);
    auto start = due_of(icalcomponent_get_first_property(v, ICAL_DTSTART_PROPERTY), v);
    if (!start)
        return e;
    e.start = start->t;
    e.all_day = !start->has_time;
    if (auto end = due_of(icalcomponent_get_first_property(v, ICAL_DTEND_PROPERTY), v)) {
        e.end = end->t;
    } else if (icalproperty* d = icalcomponent_get_first_property(v, ICAL_DURATION_PROPERTY)) {
        e.end = e.start + icaldurationtype_as_seconds(icalproperty_get_duration(d));
    } else {
        e.end = e.all_day ? add_days(e.start, 1) : e.start;
    }
    e.alerts = event_alerts(v, e.start, e.end);
    return e;
}

// The series' main VEVENT: the first one without a RECURRENCE-ID.
icalcomponent* main_event(icalcomponent* cal) {
    if (icalcomponent_isa(cal) == ICAL_VEVENT_COMPONENT)
        return cal;
    icalcomponent* first = nullptr;
    for (icalcomponent* v = icalcomponent_get_first_component(cal, ICAL_VEVENT_COMPONENT); v;
         v = icalcomponent_get_next_component(cal, ICAL_VEVENT_COMPONENT)) {
        if (!first)
            first = v;
        if (!icalcomponent_get_first_property(v, ICAL_RECURRENCEID_PROPERTY))
            return v;
    }
    return first;
}

}  // namespace

std::vector<Event> parse_events(const std::string& ics) {
    std::vector<Event> out;
    Comp cal = parse(ics);
    if (icalcomponent_isa(cal.get()) == ICAL_VEVENT_COMPONENT) {
        Event e = read_event(cal.get());
        if (e.start)
            out.push_back(std::move(e));
        return out;
    }
    for (icalcomponent* v = icalcomponent_get_first_component(cal.get(), ICAL_VEVENT_COMPONENT); v;
         v = icalcomponent_get_next_component(cal.get(), ICAL_VEVENT_COMPONENT)) {
        Event e = read_event(v);
        if (e.start)
            out.push_back(std::move(e));
    }
    return out;
}

std::string write_event(const Event& e, std::time_t now) {
    init();
    if (e.title.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::invalid_argument("an event needs a title");
    if (!e.start)
        throw std::invalid_argument("an event needs a start");
    if (e.end < e.start)
        throw std::invalid_argument("the event ends before it starts");
    Comp cal;
    icalcomponent* v = nullptr;
    std::optional<Event> before;
    if (!e.raw.empty()) {
        cal = parse(e.raw);
        v = main_event(cal.get());
        if (v)
            before = read_event(v);
    }
    if (!v) {
        cal.reset(icalcomponent_new(ICAL_VCALENDAR_COMPONENT));
        icalcomponent_add_property(cal.get(), icalproperty_new_version("2.0"));
        icalcomponent_add_property(cal.get(), icalproperty_new_prodid("-//jotdo//jotdo 0.1//EN"));
        v = icalcomponent_new(ICAL_VEVENT_COMPONENT);
        icalcomponent_add_component(cal.get(), v);
        icalcomponent_add_property(v, icalproperty_new_uid(e.uid.empty() ? new_uid().c_str() : e.uid.c_str()));
        icalcomponent_add_property(v, icalproperty_new_created(utc_now(now)));
    }

    set_text(v, ICAL_SUMMARY_PROPERTY, e.title);
    set_text(v, ICAL_LOCATION_PROPERTY, e.location);
    set_text(v, ICAL_DESCRIPTION_PROPERTY, e.notes);

    bool times_changed = !before || before->start != e.start || before->end != e.end || before->all_day != e.all_day;
    if (times_changed) {
        remove_all(v, ICAL_DTSTART_PROPERTY);
        remove_all(v, ICAL_DTEND_PROPERTY);
        remove_all(v, ICAL_DURATION_PROPERTY);
        icalcomponent_add_property(v, time_prop(ICAL_DTSTART_PROPERTY, {e.start, !e.all_day}, v));
        std::time_t end = e.end > e.start ? e.end : e.all_day ? add_days(e.start, 1) : e.start;
        icalcomponent_add_property(v, time_prop(ICAL_DTEND_PROPERTY, {end, !e.all_day}, v));
    }
    if (!before || before->repeat != e.repeat) {
        remove_all(v, ICAL_RRULE_PROPERTY);
        if (!e.repeat.empty()) {
            Rule r(e.repeat);
            if (!r)
                throw std::invalid_argument("not a repeat rule: " + e.repeat);
            icalcomponent_add_property(v, r.property());
        }
    }
    if (!before ? !e.alerts.empty() : before->alerts != e.alerts) {
        std::vector<icalcomponent*> old;
        for (icalcomponent* a = icalcomponent_get_first_component(v, ICAL_VALARM_COMPONENT); a;
             a = icalcomponent_get_next_component(v, ICAL_VALARM_COMPONENT))
            old.push_back(a);
        for (auto* a : old) {
            icalcomponent_remove_component(v, a);
            icalcomponent_free(a);
        }
        for (int m : e.alerts) {
            icalcomponent* a = icalcomponent_new(ICAL_VALARM_COMPONENT);
            icalcomponent_add_property(a, icalproperty_new_action(ICAL_ACTION_DISPLAY));
            icalcomponent_add_property(a, icalproperty_new_description("Reminder"));
            icaltriggertype trig{};
            trig.time = icaltime_null_time();
            trig.duration = minutes_before(m);
            icalcomponent_add_property(a, icalproperty_new_trigger(trig));  // relative to the start
            icalcomponent_add_component(v, a);
        }
    }

    remove_all(v, ICAL_DTSTAMP_PROPERTY);
    icalcomponent_add_property(v, icalproperty_new_dtstamp(utc_now(now)));
    remove_all(v, ICAL_LASTMODIFIED_PROPERTY);
    icalcomponent_add_property(v, icalproperty_new_lastmodified(utc_now(now)));
    if (before) {
        int seq = 1;
        if (icalproperty* p = icalcomponent_get_first_property(v, ICAL_SEQUENCE_PROPERTY))
            seq = icalproperty_get_sequence(p) + 1;
        remove_all(v, ICAL_SEQUENCE_PROPERTY);
        icalcomponent_add_property(v, icalproperty_new_sequence(seq));
    }
    return text(cal.get());
}

}  // namespace jotdo
