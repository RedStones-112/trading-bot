#pragma once
#include <string>

// Fliegel & Van Flandern proleptic Gregorian day count -- plain integer arithmetic, no
// std::mktime/timezone/DST edge cases for a simple "how many days apart are these two ISO
// dates" question. Shared by event_calendar.cpp (event lookahead window) and ml_model.cpp
// (retrain-cadence check).
inline long isoDayNumber(const std::string& iso) {
    int y = std::stoi(iso.substr(0, 4)), m = std::stoi(iso.substr(5, 2)), d = std::stoi(iso.substr(8, 2));
    int a = (14 - m) / 12, yy = y + 4800 - a, mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365L * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}
