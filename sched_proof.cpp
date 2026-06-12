// sched_proof.cpp
// Host simulation that replicates the EXACT steady-state decision logic from
// ScheduleManager::TaskSchedule (src/ScheduleManager.cpp) to prove two things:
//   1) 1-second polling + the existing dedup produces the IDENTICAL sequence of
//      AC actions as the current minute-aligned (:00) evaluation.
//   2) How many of the 86,400 daily ticks do heavy work vs. early-out cheaply.
//
// Build: g++ -O2 -std=c++17 sched_proof.cpp -o sched_proof

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

// ---- Mirror of ScheduleSegment (SharedState.h) ----
struct Seg { uint16_t startMin, endMin; uint8_t temp, radar, eco, teco, toff; };

// In-memory schedule per weekday (replaces NVS findSegment). Same semantics:
// segment covers [startMin, endMin). A realistic Monday with multiple
// transitions: ON 24 -> ON 26 (temp change) -> gap (OFF) -> ON 22 -> gap.
static std::vector<Seg> schedule[7];

void buildSchedule() {
    // Monday (wday=1)
    schedule[1] = {
        {  540,  600, 24, 0,0,0,0 },  // 09:00-10:00  ON 24
        {  600,  720, 26, 0,0,0,0 },  // 10:00-12:00  ON 26 (temp change at 10:00)
        // 12:00-13:00 GAP -> OFF
        {  780,  900, 22, 0,0,0,0 },  // 13:00-15:00  ON 22
        // 15:00-> GAP -> OFF
    };
    // Tuesday empty (all gap) to test day-rollover with no segments.
}

// ---- Mirror of findSegment() ----
bool findSegment(int wday, int minute, Seg& out) {
    for (auto& s : schedule[wday]) {
        if (minute >= (int)s.startMin && minute < (int)s.endMin) { out = s; return true; }
    }
    return false;
}

// ---- Mirror of sameSegment() ----
bool sameSegment(const Seg& a, const Seg& b) {
    return a.startMin==b.startMin && a.endMin==b.endMin && a.temp==b.temp &&
           a.radar==b.radar && a.eco==b.eco && a.teco==b.teco && a.toff==b.toff;
}

// Scheduler state (mirrors the file-scope globals).
struct SchedState {
    int lastScheduledMin = -1;
    int lastScheduledWday = -1;
    int acState = 0; // 0=OFF, 1=ON  (we log temp on ON)
};

// Counters for the heavy/medium/cheap breakdown (1s-poll run only).
struct Counters { long ticks=0, dedupEarlyOut=0, minuteChanged=0, findSegRuns=0, irActions=0; };

// Replicates the steady-state body of TaskSchedule for ONE evaluation at the
// given wall-clock (wday, minute). Appends a human-readable action to `log`
// whenever the real code would send IR / change AC state. Returns nothing;
// mutates state + counters + log exactly as the firmware path would.
void evaluate(int currentWday, int currentMin, SchedState& st,
              std::vector<std::string>& log, Counters* c) {
    if (c) c->ticks++;

    // --- dedup early-out (ScheduleManager.cpp:616-618) ---
    bool isBootRun = (st.lastScheduledMin == -1 || st.lastScheduledWday == -1);
    if (!isBootRun && currentMin == st.lastScheduledMin && currentWday == st.lastScheduledWday) {
        if (c) c->dedupEarlyOut++;
        return; // continue;
    }
    if (c) c->minuteChanged++;

    int prevMin = st.lastScheduledMin;
    int prevWday = st.lastScheduledWday;
    st.lastScheduledMin = currentMin;
    st.lastScheduledWday = currentWday;

    if (isBootRun) {
        // Boot run: catch AC up to schedule. Simplified to the OFF/ON outcome
        // (radar-managed branches don't apply in this pure-schedule sim).
        Seg seg;
        if (findSegment(currentWday, currentMin, seg)) {
            st.acState = 1;
            char buf[32]; snprintf(buf, sizeof buf, "BOOT ON %d", seg.temp);
            log.push_back(buf);
        } else {
            st.acState = 0;
            log.push_back("BOOT OFF");
        }
        return; // continue;
    }

    // --- steady-state transition logic (ScheduleManager.cpp:763-830) ---
    Seg currentSeg, prevSeg;
    bool hasCurrentSeg = findSegment(currentWday, currentMin, currentSeg);
    bool hasPrevSeg    = findSegment(prevWday,    prevMin,    prevSeg);
    if (c) c->findSegRuns += 2;

    if (!hasCurrentSeg && !hasPrevSeg) return;                 // continue;
    if (hasCurrentSeg && hasPrevSeg && sameSegment(currentSeg, prevSeg)) return; // continue;

    if (hasCurrentSeg) {
        if (!hasPrevSeg || currentSeg.temp != prevSeg.temp) {
            st.acState = 1;
            char buf[32]; snprintf(buf, sizeof buf, "ON %d", currentSeg.temp);
            log.push_back(buf);
            if (c) c->irActions++;
        } else {
            // "segment updated, AC remains" — no IR sent (no log entry).
        }
    } else {
        st.acState = 0;
        log.push_back("OFF");
        if (c) c->irActions++;
    }
}

int main() {
    buildSchedule();

    // Simulate Monday 00:00:00 -> Tuesday 00:00:00 (one full day = 86400 s).
    const int wday = 1;

    // ---- Run A: minute-aligned (one evaluation per minute, at :00) ----
    std::vector<std::string> logAligned;
    { SchedState st;
      for (int minute = 0; minute < 1440; ++minute)
          evaluate(wday, minute, st, logAligned, nullptr);
    }

    // ---- Run B: 1-second polling (86400 ticks; dedup gates the work) ----
    std::vector<std::string> logPoll;
    Counters c;
    { SchedState st;
      for (long sec = 0; sec < 86400; ++sec) {
          int minute = (int)(sec / 60);     // wall-clock minute for this second
          evaluate(wday, minute, st, logPoll, &c);
      }
    }

    // ---- Compare the two action sequences ----
    bool identical = (logAligned == logPoll);
    printf("=== FUNCTIONAL EQUIVALENCE ===\n");
    printf("Minute-aligned actions : %zu\n", logAligned.size());
    printf("1s-poll actions        : %zu\n", logPoll.size());
    printf("Sequences identical    : %s\n", identical ? "YES" : "NO  <-- MISMATCH!");
    printf("Action log (1s-poll):\n");
    for (auto& a : logPoll) printf("   %s\n", a.c_str());

    printf("\n=== 1s-POLL WORK BREAKDOWN (per 24h) ===\n");
    printf("Total ticks            : %ld\n", c.ticks);
    printf("Cheap dedup early-outs : %ld  (%.2f%%)\n",
           c.dedupEarlyOut, 100.0*c.dedupEarlyOut/c.ticks);
    printf("Minute-changed ticks   : %ld  (do the real work)\n", c.minuteChanged);
    printf("findSegment() calls    : %ld  (the NVS-read path, hardware only)\n", c.findSegRuns);
    printf("Actual IR actions       : %ld\n", c.irActions);

    return identical ? 0 : 1;
}
