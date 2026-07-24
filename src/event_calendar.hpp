#pragma once
#include <string>
#include <vector>

// One manually-curated entry in the event calendar: something with a known date expected
// to move a stock's (or a whole sector's) price -- a dividend date, a bill's scheduled
// vote, an election date for a lawmaker pushing a specific law. No free API reliably
// covers this mix (KIS has no calendar endpoint; DART/election data would need separate
// keys or scraping), so this is hand-maintained input, same spirit as stock_tags.json's
// heuristic tagging -- see events.json.example for the format.
struct ScheduledEvent {
    std::string target; // stock code, a stock_tags.json tag (sector/theme), or "ALL"
    std::string date;   // "YYYY-MM-DD"
    std::string type;   // free-form label ("dividend"/"legal"/"political"/...), logging only
    double impact = 0.0; // signed EV multiplier contribution at zero days-to-event, e.g. 0.15 or -0.2
    std::string note;
};

// Reads `filePath` (a JSON array of the fields above). Missing/corrupt file -> empty
// list, not a startup failure -- same tolerance as stock_tags.json.
std::vector<ScheduledEvent> loadEventCalendar(const std::string& filePath);

// Sums `impact` from every event whose target is `code`, "ALL", or one of `tags`, and
// whose date falls within [today, today+lookaheadDays] -- weighted linearly by closeness
// (right at the date: full impact; lookaheadDays out: ~0; past or beyond the window:
// excluded). Folds into the same [0.8, 1.3] clamp as the other EV multipliers so one
// large event can't dominate the ranking outright.
double eventMultiplier(const std::vector<ScheduledEvent>& events, const std::string& code,
                       const std::vector<std::string>& tags, const std::string& todayIso,
                       int lookaheadDays);
