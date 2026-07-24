#include "event_calendar.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <fstream>

using json = nlohmann::json;

std::vector<ScheduledEvent> loadEventCalendar(const std::string& filePath) {
    std::vector<ScheduledEvent> events;
    std::ifstream in(filePath);
    if (!in) return events; // no file yet -- fine, event multiplier just stays 1.0 everywhere
    json j;
    try {
        j = json::parse(in, nullptr, true, true);
    } catch (const std::exception&) {
        return events; // corrupt/empty file -- start empty rather than crash the bot over this
    }
    for (auto& e : j) {
        ScheduledEvent ev;
        ev.target = e.value("target", "");
        ev.date = e.value("date", "");
        ev.type = e.value("type", "");
        ev.impact = e.value("impact", 0.0);
        ev.note = e.value("note", "");
        if (!ev.target.empty() && !ev.date.empty()) events.push_back(std::move(ev));
    }
    return events;
}

// Fliegel & Van Flandern proleptic Gregorian day count -- plain integer arithmetic, no
// std::mktime/timezone/DST edge cases to worry about for a simple date difference.
static long dayNumber(int y, int m, int d) {
    int a = (14 - m) / 12, yy = y + 4800 - a, mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365L * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

static long isoDayNumber(const std::string& iso) {
    return dayNumber(std::stoi(iso.substr(0, 4)), std::stoi(iso.substr(5, 2)), std::stoi(iso.substr(8, 2)));
}

double eventMultiplier(const std::vector<ScheduledEvent>& events, const std::string& code,
                       const std::vector<std::string>& tags, const std::string& todayIso,
                       int lookaheadDays) {
    long todayNum = isoDayNumber(todayIso);
    double score = 0.0;
    for (auto& ev : events) {
        bool matches = ev.target == code || ev.target == "ALL" ||
                       std::find(tags.begin(), tags.end(), ev.target) != tags.end();
        if (!matches) continue;
        long days;
        try { days = isoDayNumber(ev.date) - todayNum; } catch (const std::exception&) { continue; }
        if (days < 0 || days > lookaheadDays) continue;
        score += ev.impact * (1.0 - (double)days / lookaheadDays);
    }
    return std::clamp(1.0 + score, 0.8, 1.3);
}
