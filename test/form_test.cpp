// SPDX-License-Identifier: GPL-3.0-or-later
// Run with TZ=America/New_York (ctest sets it).
#include <gtest/gtest.h>

#include "form.hpp"

using namespace jotdo;

namespace {

std::time_t at(int y, int mo, int d, int h = 0, int mi = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900, tm.tm_mon = mo - 1, tm.tm_mday = d, tm.tm_hour = h, tm.tm_min = mi, tm.tm_isdst = -1;
    return std::mktime(&tm);
}

const std::time_t NOW = at(2026, 10, 21, 10, 30);

}  // namespace

TEST(Form, RoundTrip) {
    Task t;
    t.title = "Call the dentist";
    t.due = Due{at(2026, 10, 22, 9), true};
    t.repeat = "FREQ=WEEKLY;BYDAY=MO";
    t.alerts = {0, 60};
    t.priority = 1;
    t.notes = "Number is on the card\n\nAsk about Friday";
    std::string list = "Personal";
    std::string text = form_text(t, list);
    EXPECT_EQ(text,
              "Call the dentist\ndue: 2026-10-22 09:00\nrepeat: every Monday\nalert: at due, 1h before\npriority: high\n"
              "list: Personal\n\nNumber is on the card\n\nAsk about Friday\n");
    Task back;
    std::string back_list = "Family";
    apply_form(text, back, back_list, NOW);
    EXPECT_EQ(back.title, t.title);
    EXPECT_EQ(back.due, t.due);
    EXPECT_EQ(back.repeat, t.repeat);
    EXPECT_EQ(back.alerts, t.alerts);
    EXPECT_EQ(back.priority, 1);
    EXPECT_EQ(back.notes, t.notes);
    EXPECT_EQ(back_list, "Personal");
}

TEST(Form, NaturalValuesAndBlanks) {
    Task t;
    std::string list = "Personal";
    apply_form("Bins out\ndue: fri\nrepeat:\nalert:\npriority:\nlist: Family\n", t, list, NOW);
    EXPECT_EQ(t.title, "Bins out");
    EXPECT_EQ(t.due, (Due{at(2026, 10, 23), false}));
    EXPECT_TRUE(t.repeat.empty());
    EXPECT_EQ(list, "Family");
    EXPECT_TRUE(t.notes.empty());
}

TEST(Form, FieldsMayBeLeftOut) {
    Task t;
    std::string list = "Personal";
    apply_form("Just a title\n\nand some notes", t, list, NOW);
    EXPECT_EQ(t.title, "Just a title");
    EXPECT_EQ(t.notes, "and some notes");
    EXPECT_FALSE(t.due);
}

TEST(Form, BadValueNamesTheField) {
    Task t;
    std::string list = "Personal";
    try {
        apply_form("X\ndue: someday\n", t, list, NOW);
        FAIL();
    } catch (const std::invalid_argument& e) {
        EXPECT_EQ(std::string(e.what()).rfind("due: ", 0), 0u) << e.what();
    }
    EXPECT_THROW(apply_form("X\nrepeat: weekly\n", t, list, NOW), std::invalid_argument);  // no due date
    EXPECT_THROW(apply_form("\n\n", t, list, NOW), std::invalid_argument);
}
