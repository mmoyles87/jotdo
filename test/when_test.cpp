// SPDX-License-Identifier: GPL-3.0-or-later
// Run with TZ=America/New_York (ctest sets it).
#include <gtest/gtest.h>

#include "when.hpp"

using namespace jotdo;

namespace {

std::time_t at(int y, int mo, int d, int h = 0, int mi = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900, tm.tm_mon = mo - 1, tm.tm_mday = d, tm.tm_hour = h, tm.tm_min = mi, tm.tm_isdst = -1;
    return std::mktime(&tm);
}

// Wednesday 2026-10-21 10:30 local.
const std::time_t NOW = at(2026, 10, 21, 10, 30);

Due day(int y, int mo, int d) { return {at(y, mo, d), false}; }
Due moment(int y, int mo, int d, int h, int mi = 0) { return {at(y, mo, d, h, mi), true}; }

}  // namespace

TEST(ParseDue, Words) {
    EXPECT_EQ(parse_due("today", NOW), day(2026, 10, 21));
    EXPECT_EQ(parse_due("tomorrow", NOW), day(2026, 10, 22));
    EXPECT_EQ(parse_due("tomorrow 9am", NOW), moment(2026, 10, 22, 9));
    EXPECT_EQ(parse_due("Tomorrow at 9:30pm", NOW), moment(2026, 10, 22, 21, 30));
    EXPECT_EQ(parse_due("tonight", NOW), moment(2026, 10, 21, 20));
    EXPECT_EQ(parse_due("noon", NOW), moment(2026, 10, 21, 12));
    EXPECT_FALSE(parse_due("", NOW));
    EXPECT_FALSE(parse_due("none", NOW));
}

TEST(ParseDue, Weekdays) {
    EXPECT_EQ(parse_due("fri", NOW), day(2026, 10, 23));
    EXPECT_EQ(parse_due("mon 8:00", NOW), moment(2026, 10, 26, 8));
    EXPECT_EQ(parse_due("wed", NOW), day(2026, 10, 28));  // today is Wednesday: next week's
    EXPECT_EQ(parse_due("next tue 14:30", NOW), moment(2026, 10, 27, 14, 30));
}

TEST(ParseDue, Dates) {
    EXPECT_EQ(parse_due("2026-10-24 18:00", NOW), moment(2026, 10, 24, 18));
    EXPECT_EQ(parse_due("2026-12-01", NOW), day(2026, 12, 1));
    EXPECT_EQ(parse_due("oct 24", NOW), day(2026, 10, 24));
    EXPECT_EQ(parse_due("24 oct 7pm", NOW), moment(2026, 10, 24, 19));
    EXPECT_EQ(parse_due("jan 5", NOW), day(2027, 1, 5));  // past this year: next year
    EXPECT_EQ(parse_due("10/30", NOW), day(2026, 10, 30));
    EXPECT_EQ(parse_due("12/25/2027", NOW), day(2027, 12, 25));
}

TEST(ParseDue, TimesAndOffsets) {
    EXPECT_EQ(parse_due("17:00", NOW), moment(2026, 10, 21, 17));
    EXPECT_EQ(parse_due("9am", NOW), moment(2026, 10, 22, 9));  // already past today
    EXPECT_EQ(parse_due("in 2h", NOW), (Due{NOW + 7200, true}));
    EXPECT_EQ(parse_due("in 30 minutes", NOW), (Due{NOW + 1800, true}));
    EXPECT_EQ(parse_due("in 3 days", NOW), day(2026, 10, 24));
}

TEST(ParseDue, Rejects) {
    EXPECT_THROW(parse_due("someday", NOW), std::invalid_argument);
    EXPECT_THROW(parse_due("25:00", NOW), std::invalid_argument);
    EXPECT_THROW(parse_due("tomorrow fri", NOW), std::invalid_argument);
}

TEST(FormatDue, Relative) {
    set_clock(false);
    EXPECT_EQ(format_due(moment(2026, 10, 21, 9), NOW), "Today 09:00");
    EXPECT_EQ(format_due(day(2026, 10, 22), NOW), "Tomorrow");
    EXPECT_EQ(format_due(moment(2026, 10, 26, 8), NOW), "Mon 08:00");
    EXPECT_EQ(format_due(day(2026, 11, 14), NOW), "Nov 14");
    EXPECT_EQ(format_due(day(2027, 2, 1), NOW), "Feb 1 2027");
    EXPECT_EQ(format_due_exact(moment(2026, 10, 24, 18)), "2026-10-24 18:00");
    EXPECT_TRUE(is_overdue(moment(2026, 10, 21, 9), NOW));
    EXPECT_FALSE(is_overdue(day(2026, 10, 21), NOW));  // a day is not late until it is over
}

TEST(FormatDue, TwelveHour) {
    set_clock(true);
    EXPECT_EQ(format_due(moment(2026, 10, 21, 9), NOW), "Today 9:00 AM");
    EXPECT_EQ(format_due(moment(2026, 10, 26, 20, 5), NOW), "Mon 8:05 PM");
    EXPECT_EQ(format_time(at(2026, 10, 21, 0, 30)), "12:30 AM");
    EXPECT_EQ(format_time(at(2026, 10, 21, 12)), "12:00 PM");
    EXPECT_EQ(format_span(at(2026, 10, 21, 9), at(2026, 10, 21, 10, 30)), "9:00–10:30 AM");
    EXPECT_EQ(format_span(at(2026, 10, 21, 11, 30), at(2026, 10, 21, 13)), "11:30 AM–1:00 PM");
    EXPECT_EQ(format_due_exact(moment(2026, 10, 24, 18)), "2026-10-24 6:00 PM");
    EXPECT_EQ(parse_due(format_due_exact(moment(2026, 10, 24, 18)), NOW), moment(2026, 10, 24, 18));  // reads its own output
    set_clock(false);
    EXPECT_EQ(format_span(at(2026, 10, 21, 9), at(2026, 10, 21, 10, 30)), "09:00–10:30");
}

TEST(Repeat, RoundTrip) {
    EXPECT_EQ(parse_repeat("daily"), "FREQ=DAILY");
    EXPECT_EQ(parse_repeat("weekdays"), "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR");
    EXPECT_EQ(parse_repeat("every 2 weeks"), "FREQ=WEEKLY;INTERVAL=2");
    EXPECT_EQ(parse_repeat("every mon, wed"), "FREQ=WEEKLY;BYDAY=MO,WE");
    EXPECT_EQ(parse_repeat("weekly on friday"), "FREQ=WEEKLY;BYDAY=FR");
    EXPECT_EQ(parse_repeat("freq=monthly;byday=1mo"), "FREQ=MONTHLY;BYDAY=1MO");
    EXPECT_EQ(parse_repeat("none"), "");
    EXPECT_THROW(parse_repeat("now and then"), std::invalid_argument);

    for (std::string text : {"daily", "weekly", "weekdays", "every 3 days", "every Monday", "every Mon, Wed", "monthly", "yearly"})
        EXPECT_EQ(describe_repeat(parse_repeat(text)), text == "every Mon, Wed" ? "every Mon, Wed" : text) << text;
    EXPECT_EQ(describe_repeat("FREQ=MONTHLY;BYDAY=1MO"), "FREQ=MONTHLY;BYDAY=1MO");
    EXPECT_EQ(describe_repeat("FREQ=DAILY;COUNT=5"), "FREQ=DAILY;COUNT=5");
}

TEST(Alerts, RoundTrip) {
    EXPECT_EQ(parse_alerts("at due"), (std::vector<int>{0}));
    EXPECT_EQ(parse_alerts("1h before, 15m before"), (std::vector<int>{15, 60}));
    EXPECT_EQ(parse_alerts("1 day before"), (std::vector<int>{1440}));
    EXPECT_EQ(parse_alerts("1h 30m before"), (std::vector<int>{90}));
    EXPECT_EQ(describe_alerts({0, 90, 1440}), "at due, 1h30m before, 1d before");
    EXPECT_EQ(parse_alerts(describe_alerts({0, 90, 1440})), (std::vector<int>{0, 90, 1440}));
    EXPECT_THROW(parse_alerts("soonish"), std::invalid_argument);
}

TEST(Priority, Levels) {
    EXPECT_EQ(parse_priority("high"), 1);
    EXPECT_EQ(parse_priority("!!"), 5);
    EXPECT_EQ(parse_priority("low"), 9);
    EXPECT_EQ(parse_priority(""), 0);
    EXPECT_EQ(describe_priority(1), "high");
    EXPECT_EQ(describe_priority(0), "");
}
