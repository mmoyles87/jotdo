// SPDX-License-Identifier: GPL-3.0-or-later
// Run with TZ=America/New_York (ctest sets it).
#include <gtest/gtest.h>

#include "ical.hpp"

using namespace jotdo;

namespace {

std::time_t at(int y, int mo, int d, int h = 0, int mi = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900, tm.tm_mon = mo - 1, tm.tm_mday = d, tm.tm_hour = h, tm.tm_min = mi, tm.tm_isdst = -1;
    return std::mktime(&tm);
}

const std::time_t NOW = at(2026, 10, 21, 10, 30);

// A reminder in the shape a phone writes it, with properties jotdo does not know about.
const char* PHONE = "BEGIN:VCALENDAR\r\n"
                    "VERSION:2.0\r\n"
                    "PRODID:-//Apple Inc.//iOS 26.0//EN\r\n"
                    "BEGIN:VTIMEZONE\r\n"
                    "TZID:America/New_York\r\n"
                    "BEGIN:DAYLIGHT\r\n"
                    "TZOFFSETFROM:-0500\r\n"
                    "RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=2SU\r\n"
                    "DTSTART:20070311T020000\r\n"
                    "TZNAME:EDT\r\n"
                    "TZOFFSETTO:-0400\r\n"
                    "END:DAYLIGHT\r\n"
                    "BEGIN:STANDARD\r\n"
                    "TZOFFSETFROM:-0400\r\n"
                    "RRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=1SU\r\n"
                    "DTSTART:20071104T020000\r\n"
                    "TZNAME:EST\r\n"
                    "TZOFFSETTO:-0500\r\n"
                    "END:STANDARD\r\n"
                    "END:VTIMEZONE\r\n"
                    "BEGIN:VTODO\r\n"
                    "UID:2C3E7B8A-1111-4222-8333-444455556666\r\n"
                    "CREATED:20261020T120000Z\r\n"
                    "DTSTAMP:20261020T120000Z\r\n"
                    "LAST-MODIFIED:20261020T120000Z\r\n"
                    "SUMMARY:Call the dentist\\, about Friday\r\n"
                    "DESCRIPTION:Number is on the card\\nsecond line\r\n"
                    "DTSTART;TZID=America/New_York:20261022T090000\r\n"
                    "DUE;TZID=America/New_York:20261022T090000\r\n"
                    "PRIORITY:1\r\n"
                    "STATUS:NEEDS-ACTION\r\n"
                    "X-APPLE-SORT-ORDER:12345\r\n"
                    "BEGIN:VALARM\r\n"
                    "UID:ALARM-1\r\n"
                    "X-WR-ALARMUID:ALARM-1\r\n"
                    "TRIGGER;VALUE=DATE-TIME:20261022T130000Z\r\n"
                    "ACTION:DISPLAY\r\n"
                    "DESCRIPTION:Reminder\r\n"
                    "END:VALARM\r\n"
                    "END:VTODO\r\n"
                    "END:VCALENDAR\r\n";

}  // namespace

TEST(Task, ParsesAPhoneReminder) {
    auto t = parse_task(PHONE);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->uid, "2C3E7B8A-1111-4222-8333-444455556666");
    EXPECT_EQ(t->title, "Call the dentist, about Friday");
    EXPECT_EQ(t->notes, "Number is on the card\nsecond line");
    ASSERT_TRUE(t->due);
    EXPECT_EQ(t->due->t, at(2026, 10, 22, 9));
    EXPECT_TRUE(t->due->has_time);
    EXPECT_EQ(t->priority, 1);
    EXPECT_EQ(t->alerts, (std::vector<int>{0}));  // 13:00Z is 09:00 EDT
    EXPECT_FALSE(t->done);
}

TEST(Task, EditKeepsWhatItDoesNotKnow) {
    auto t = parse_task(PHONE);
    t->title = "Call the dentist";
    std::string out = write_task(*t, NOW);
    EXPECT_NE(out.find("X-APPLE-SORT-ORDER:12345"), std::string::npos);
    EXPECT_NE(out.find("X-WR-ALARMUID:ALARM-1"), std::string::npos);  // alarms unchanged, kept as they were
    EXPECT_NE(out.find("SUMMARY:Call the dentist\r\n"), std::string::npos);
    EXPECT_NE(out.find("SEQUENCE:1"), std::string::npos);
    auto back = parse_task(out);
    EXPECT_EQ(back->due, t->due);
    EXPECT_EQ(back->notes, t->notes);
}

TEST(Task, CompleteAndReopen) {
    auto t = parse_task(PHONE);
    t->done = true;
    t->completed = NOW;
    auto done = parse_task(write_task(*t, NOW));
    EXPECT_TRUE(done->done);
    EXPECT_EQ(done->completed, NOW);
    done->done = false;
    std::string reopened = write_task(*done, NOW);
    EXPECT_EQ(reopened.find("COMPLETED:"), std::string::npos);
    EXPECT_NE(reopened.find("STATUS:NEEDS-ACTION"), std::string::npos);
}

TEST(Task, NewReminderWithDateAndAlerts) {
    Task t;
    t.title = "Pay the water bill";
    t.due = Due{at(2026, 11, 2, 18), true};
    t.alerts = {60};
    t.repeat = "FREQ=MONTHLY";
    std::string out = write_task(t, NOW);
    EXPECT_NE(out.find("DUE;TZID=America/New_York:20261102T180000"), std::string::npos) << out;
    EXPECT_NE(out.find("DTSTART;TZID=America/New_York:20261102T180000"), std::string::npos);
    EXPECT_NE(out.find("BEGIN:VTIMEZONE"), std::string::npos);
    EXPECT_NE(out.find("RRULE:FREQ=MONTHLY"), std::string::npos);
    EXPECT_NE(out.find("TRIGGER;RELATED=END:-PT1H"), std::string::npos) << out;
    auto back = parse_task(out);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->due, t.due);
    EXPECT_EQ(back->alerts, (std::vector<int>{60}));
    EXPECT_FALSE(back->uid.empty());
}

TEST(Task, AllDay) {
    Task t;
    t.title = "Bins out";
    t.due = Due{at(2026, 10, 23), false};
    std::string out = write_task(t, NOW);
    EXPECT_NE(out.find("DUE;VALUE=DATE:20261023"), std::string::npos) << out;
    EXPECT_EQ(parse_task(out)->due, t.due);
}

TEST(Advance, WeeklyKeepsTheWallClockAcrossDst) {
    Task t;
    t.title = "Standup";
    t.due = Due{at(2026, 10, 26, 8), true};  // Monday, EDT
    t.repeat = "FREQ=WEEKLY;BYDAY=MO";
    t.raw = write_task(t, NOW);
    ASSERT_TRUE(advance(t));
    EXPECT_EQ(t.due->t, at(2026, 11, 2, 8));  // Monday, EST: still 08:00
}

TEST(Advance, EndsWithCount) {
    Task t;
    t.title = "Twice";
    t.due = Due{at(2026, 10, 21), false};
    t.repeat = "FREQ=DAILY;COUNT=2";
    t.raw = write_task(t, NOW);
    ASSERT_TRUE(advance(t));
    EXPECT_EQ(t.due->t, at(2026, 10, 22));
    EXPECT_FALSE(advance(t));
}

TEST(Events, AllDayAndTimed) {
    const char* ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
                      "BEGIN:VEVENT\r\nUID:a\r\nSUMMARY:Raclette date\r\nDTSTART:20261021T203000Z\r\nDTEND:20261021T213000Z\r\nEND:VEVENT\r\n"
                      "BEGIN:VEVENT\r\nUID:b\r\nSUMMARY:Holiday\r\nDTSTART;VALUE=DATE:20261023\r\nDTEND;VALUE=DATE:20261024\r\nEND:VEVENT\r\n"
                      "END:VCALENDAR\r\n";
    auto ev = parse_events(ics);
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].start, at(2026, 10, 21, 16, 30));
    EXPECT_FALSE(ev[0].all_day);
    EXPECT_TRUE(ev[1].all_day);
    EXPECT_EQ(ev[1].start, at(2026, 10, 23));
}

TEST(Events, WriteNewAndEdit) {
    Event e;
    e.title = "Dinner";
    e.start = at(2026, 10, 24, 19);
    e.end = at(2026, 10, 24, 20, 30);
    e.location = "Home";
    e.alerts = {60};
    std::string out = write_event(e, NOW);
    EXPECT_NE(out.find("DTSTART;TZID=America/New_York:20261024T190000"), std::string::npos) << out;
    EXPECT_NE(out.find("DTEND;TZID=America/New_York:20261024T203000"), std::string::npos);
    EXPECT_NE(out.find("TRIGGER:-PT1H"), std::string::npos);
    auto back = parse_events(out);
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].start, e.start);
    EXPECT_EQ(back[0].end, e.end);
    EXPECT_EQ(back[0].alerts, (std::vector<int>{60}));
    EXPECT_EQ(back[0].location, "Home");

    Event edited = back[0];
    edited.raw = out + "";  // an attendee the phone added stays through an edit
    edited.raw.replace(edited.raw.find("END:VEVENT"), 10, "ATTENDEE;CN=Bridget:mailto:bridget@example.com\r\nEND:VEVENT");
    edited.title = "Dinner with B";
    std::string again = write_event(edited, NOW);
    EXPECT_NE(again.find("ATTENDEE;CN=Bridget"), std::string::npos);
    EXPECT_NE(again.find("SUMMARY:Dinner with B"), std::string::npos);
    EXPECT_NE(again.find("SEQUENCE:1"), std::string::npos);
}

TEST(Events, AllDayEndIsExclusive) {
    Event e;
    e.title = "Trip";
    e.all_day = true;
    e.start = at(2026, 10, 30);
    e.end = at(2026, 11, 1);  // through Oct 31
    std::string out = write_event(e, NOW);
    EXPECT_NE(out.find("DTSTART;VALUE=DATE:20261030"), std::string::npos) << out;
    EXPECT_NE(out.find("DTEND;VALUE=DATE:20261101"), std::string::npos);
    EXPECT_THROW(write_event(Event{}, NOW), std::invalid_argument);
}

TEST(Advance, CountSurvivesSaving) {
    Task t;
    t.title = "Twice";
    t.due = Due{at(2026, 10, 21), false};
    t.repeat = "FREQ=DAILY;COUNT=2";
    t.raw = write_task(t, NOW);
    ASSERT_TRUE(advance(t));
    EXPECT_EQ(t.repeat, "FREQ=DAILY;COUNT=1");
    t.raw = write_task(t, NOW);  // saved and loaded again, as after ticking it off on the server
    t = *parse_task(t.raw);
    EXPECT_FALSE(advance(t)) << "the second occurrence was the last";
}

TEST(Task, MovingTheDueDateKeepsTheStartsLead) {
    const char* ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VTODO\r\nUID:lead-1\r\nSUMMARY:Report\r\n"
                      "DTSTART;TZID=America/New_York:20261022T080000\r\nDUE;TZID=America/New_York:20261022T170000\r\n"
                      "END:VTODO\r\nEND:VCALENDAR\r\n";
    auto t = parse_task(ics);
    t->due = Due{at(2026, 10, 23, 17), true};
    std::string out = write_task(*t, NOW);
    EXPECT_NE(out.find("DTSTART;TZID=America/New_York:20261023T080000"), std::string::npos) << out;
    EXPECT_NE(out.find("DUE;TZID=America/New_York:20261023T170000"), std::string::npos);
}

TEST(Advance, RepeatsInTheReminderOwnZone) {
    // Weekly at 09:00 London. The UK leaves summer time on Oct 25, a week before New York.
    const char* ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VTODO\r\nUID:london-1\r\nSUMMARY:Call\r\n"
                      "DTSTART;TZID=Europe/London:20261019T090000\r\nDUE;TZID=Europe/London:20261019T090000\r\n"
                      "RRULE:FREQ=WEEKLY;BYDAY=MO\r\nEND:VTODO\r\nEND:VCALENDAR\r\n";
    auto t = parse_task(ics);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->due->t, 1792396800);  // 2026-10-19 08:00 UTC (09:00 BST)
    ASSERT_TRUE(advance(*t));
    EXPECT_EQ(t->due->t, 1793005200);  // 2026-10-26 09:00 UTC (09:00 GMT), not 08:00
    std::string out = write_task(*t, NOW);
    EXPECT_NE(out.find("DUE;TZID=Europe/London:20261026T090000"), std::string::npos) << out;
}

TEST(Task, AlarmsAfterTheDueTimeSurviveEdits) {
    const char* ics = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VTODO\r\nUID:after-1\r\nSUMMARY:Bins\r\n"
                      "DUE;TZID=America/New_York:20261022T190000\r\n"
                      "BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:Reminder\r\nTRIGGER;RELATED=END:PT15M\r\nEND:VALARM\r\n"
                      "END:VTODO\r\nEND:VCALENDAR\r\n";
    auto t = parse_task(ics);
    EXPECT_EQ(t->alerts, (std::vector<int>{-15}));
    EXPECT_EQ(describe_alerts(t->alerts), "");  // not offered for editing
    t->title = "Bins out";
    t->alerts = {};  // what the form gives back
    std::string out = write_task(*t, NOW);
    EXPECT_NE(out.find("TRIGGER;RELATED=END:PT15M"), std::string::npos) << out;
    t = parse_task(out);
    t->due = Due{at(2026, 10, 23, 19), true};  // a due change rewrites the alarms: the after-due one stays
    t->alerts = {60};
    out = write_task(*t, NOW);
    EXPECT_NE(out.find("PT15M"), std::string::npos) << out;
    EXPECT_NE(out.find("-PT1H"), std::string::npos);
}
