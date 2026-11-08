// ScenarioFunc.cpp - narrow tests of single game functions, so a question does not need a whole session.
//
// The scenarios only measure and write "FT ..." lines; scripts/ft_oracle.py reads them and decides.
// A scenario passes when it measured. Whether the numbers are good is the oracle's business.
//
//   func_rows   every character the engine has, every N seconds, in a list of places. Answers: does the same zone
//               get the same population on two machines, do copies follow their owner.
//   func_perf   how long single engine calls take with the world this save gives. One window is enough.
//   func_moves  moves one item between two characters of the local squad at a set rate. Checks nothing is lost.
//
// Settings come from the environment: KENSHICOOP_FT_*. See scripts/functests.psd1.

#include "ScenarioSupport.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace coop {
namespace {

int envInt(const char* name, int def) {
    const char* e = getenv(name);
    return (e && e[0]) ? atoi(e) : def;
}

void ftLine(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(b, sizeof(b) - 1, fmt, a);
    va_end(a);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

const float CELL_U = 4608.0f, ORIGIN_U = -147456.0f;
const unsigned int ROWS_MAX = NPC_CENSUS_MAX;

// One row per character, the same on both windows.
unsigned int dumpRows(GameWorld* gw, int stop, int sample, float radius) {
    static Character* chars[ROWS_MAX];    // main-thread only
    static EntityState states[ROWS_MAX];
    bool trunc = false;
    const unsigned int n = engine::listNpcsWide(gw, radius, chars, states, ROWS_MAX, &trunc);
    for (unsigned int i = 0; i < n; ++i) {
        char sid[64], fac[64];
        float x = 0, y = 0, z = 0, heading = 0, age = 0;
        bool dead = false;
        sid[0] = fac[0] = '\0';
        engine::describeCharacter(chars[i], sid, sizeof(sid), fac, sizeof(fac), &x, &y, &z, &heading, &dead, &age);
        int cx = 0, cz = 0;
        engine::cellAt(gw, states[i].x, states[i].z, &cx, &cz);
        ftLine("FT ROW stop=%d sample=%d hand=%u,%u,%u,%u,%u cell=%d,%d pos=%.0f,%.0f,%.0f moving=%d speed=%.0f task=%u dead=%d sid=%s fac=%s",
               stop, sample, states[i].hIndex, states[i].hSerial, states[i].hType, states[i].hContainer, states[i].hContainerSerial,
               cx, cz, states[i].x, states[i].y, states[i].z, (int)states[i].cMoving, states[i].cSpeed, (unsigned)states[i].task,
               dead ? 1 : 0, sid, fac);
    }
    ftLine("FT ROWS stop=%d sample=%d n=%u trunc=%d", stop, sample, n, trunc ? 1 : 0);
    return n;
}

// Frame times over one measuring window: the only cost a player can feel.
class FrameClock {
public:
    FrameClock() : last_(0) { QueryPerformanceFrequency(&freq_); }
    void tick() {
        LARGE_INTEGER n;
        QueryPerformanceCounter(&n);
        if (last_ != 0) ms_.push_back((float)(1000.0 * (double)(n.QuadPart - last_) / (double)freq_.QuadPart));
        last_ = n.QuadPart;
    }
    void report(const char* tag) {
        if (ms_.size() < 2) return;
        std::sort(ms_.begin(), ms_.end());
        ftLine("FT FRAME tag=%s n=%u p50=%.1f p95=%.1f p99=%.1f max=%.1f", tag, (unsigned)ms_.size(),
               ms_[ms_.size() / 2], ms_[ms_.size() * 95 / 100], ms_[ms_.size() * 99 / 100], ms_[ms_.size() - 1]);
        ms_.clear();
    }
private:
    LARGE_INTEGER freq_;
    long long last_;
    std::vector<float> ms_;
};

// ---- func_rows ------------------------------------------------------------------------------------------------
// KENSHICOOP_FT_CELLS: "x,z;x,z" places to visit in order, empty = stay where the save starts.
// KENSHICOOP_FT_SETTLE_S: seconds to wait in a place before the first dump.
// KENSHICOOP_FT_EVERY_S, KENSHICOOP_FT_SAMPLES: dump cadence and count per place.
// Both windows run the same timetable from their own arming, so samples line up to within a few seconds.
class FuncRowsScenario : public TimedScenario {
public:
    FuncRowsScenario() : TimedScenario("func_rows", 0), stop_(-1), sample_(0), arrivedMs_(0), lastDumpMs_(0), rows_(0), done_(false) {
        settleMs_ = (unsigned long)envInt("KENSHICOOP_FT_SETTLE_S", 30) * 1000ul;
        everyMs_ = (unsigned long)envInt("KENSHICOOP_FT_EVERY_S", 10) * 1000ul;
        samples_ = envInt("KENSHICOOP_FT_SAMPLES", 3);
        radius_ = (float)envInt("KENSHICOOP_FT_RADIUS", 3000);
        const char* e = getenv("KENSHICOOP_FT_CELLS");
        std::string s = e ? e : "";
        size_t p = 0;
        while (p < s.size()) {
            size_t q = s.find(';', p);
            if (q == std::string::npos) q = s.size();
            int cx = 0, cz = 0;
            if (sscanf(s.substr(p, q - p).c_str(), "%d,%d", &cx, &cz) == 2) cells_.push_back(std::make_pair(cx, cz));
            p = q + 1;
        }
    }
    virtual void onStart(const ScenarioContext& ctx) {
        ftLine("FT START name=func_rows role=%s stops=%u settleMs=%lu everyMs=%lu samples=%d radius=%.0f",
               ctx.isHost ? "host" : "join", (unsigned)cells_.size(), settleMs_, everyMs_, samples_, radius_);
        next(ctx);
    }
    virtual bool onTick(const ScenarioContext& ctx) {
        frames_.tick();
        if (done_) return true;
        const unsigned long inPlace = ctx.elapsedMs - arrivedMs_;
        if (inPlace < settleMs_) return false;
        if (sample_ >= samples_) {
            frames_.report("stop");
            if (!next(ctx)) {
                setPassed(rows_ > 0);
                ftLine("FT END name=func_rows rows=%u", rows_);
                done_ = true;
                return true;
            }
            return false;
        }
        if (lastDumpMs_ == 0 || ctx.elapsedMs - lastDumpMs_ >= everyMs_) {
            lastDumpMs_ = ctx.elapsedMs;
            rows_ += dumpRows(ctx.gw, stop_, sample_, radius_);
            ++sample_;
        }
        return false;
    }
private:
    // Goes to the next place. Returns false when the list ran out. An empty list is one stop right here.
    bool next(const ScenarioContext& ctx) {
        ++stop_;
        sample_ = 0;
        lastDumpMs_ = 0;
        arrivedMs_ = ctx.elapsedMs;
        if (cells_.empty()) return stop_ == 0;
        if ((size_t)stop_ >= cells_.size()) return false;
        const float x = ORIGIN_U + ((float)cells_[stop_].first + 0.5f) * CELL_U;
        const float z = ORIGIN_U + ((float)cells_[stop_].second + 0.5f) * CELL_U;
        float y = 0.f;
        const bool haveY = engine::terrainHeightAt(x, z, &y);
        Character* lead = engine::leader(ctx.gw);
        float ly = 0, lx = 0, lz = 0;
        if (lead) engine::readPos(lead, &lx, &ly, &lz);
        const bool parked = lead && engine::park(lead, x, haveY ? y : ly, z, 0.0f);
        if (lead) engine::cameraFocusOn(ctx.gw, lead);
        ftLine("FT STOP stop=%d cell=%d,%d pos=%.0f,%.0f haveY=%d parked=%d", stop_, cells_[stop_].first, cells_[stop_].second, x, z, haveY ? 1 : 0, parked ? 1 : 0);
        return true;
    }
    std::vector<std::pair<int, int> > cells_;
    int stop_, sample_, samples_;
    unsigned long arrivedMs_, lastDumpMs_, settleMs_, everyMs_;
    float radius_;
    unsigned int rows_;
    bool done_;
    FrameClock frames_;
};

// ---- func_perf ------------------------------------------------------------------------------------------------
// One call of the engine at a time, timed over the world this save gives. The call runs once per measuring tick
// with the game otherwise left alone, so a slow call shows up as itself and not as a frame hitch of something else.
class FuncPerfScenario : public TimedScenario {
public:
    FuncPerfScenario() : TimedScenario("func_perf", 0), round_(0), lastMs_(0) {
        QueryPerformanceFrequency(&freq_);
        rounds_ = envInt("KENSHICOOP_FT_ROUNDS", 60);
        everyMs_ = (unsigned long)envInt("KENSHICOOP_FT_EVERY_MS", 1000);
    }
    virtual void onStart(const ScenarioContext& ctx) {
        ftLine("FT START name=func_perf role=%s rounds=%d everyMs=%lu", ctx.isHost ? "host" : "join", rounds_, everyMs_);
    }
    virtual bool onTick(const ScenarioContext& ctx) {
        frames_.tick();
        if (ctx.elapsedMs - lastMs_ < everyMs_ && lastMs_ != 0) return false;
        lastMs_ = ctx.elapsedMs;
        if (round_ >= rounds_) {
            report();
            frames_.report("perf");
            setPassed(true);
            return true;
        }
        measure(ctx.gw);
        ++round_;
        return false;
    }
private:
    long long now() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n.QuadPart; }
    void add(const char* name, long long t0, unsigned int items) {
        Stat& s = stats_[name];
        s.us.push_back((float)(1000000.0 * (double)(now() - t0) / (double)freq_.QuadPart));
        s.items += items;
    }
    void measure(GameWorld* gw) {
        static Character* chars[ROWS_MAX];
        static EntityState states[ROWS_MAX];
        static EntityState squad[96];
        static InvItemEntry inv[256];
        static engine::C2Row rows[1024];

        long long t = now();
        unsigned int nSquad = engine::captureSquad(gw, false, squad, 96);
        add("captureSquad", t, nSquad);

        t = now();
        unsigned int nNear = engine::listNpcs(gw, chars, states, 256);
        add("listNpcs", t, nNear);

        t = now();
        unsigned int nWide = engine::listNpcsWide(gw, 2000.f, chars, states, ROWS_MAX);
        add("listNpcsWide2000", t, nWide);

        t = now();
        unsigned int nFar = engine::listNpcsWide(gw, 5000.f, chars, states, ROWS_MAX);
        add("listNpcsWide5000", t, nFar);

        t = now();
        for (unsigned int i = 0; i < nFar; ++i) {
            float x, y, z;
            engine::readPos(chars[i], &x, &y, &z);
        }
        add("readPos_all", t, nFar);

        t = now();
        for (unsigned int i = 0; i < nFar; ++i) {
            int cx, cz;
            engine::cellAt(gw, states[i].x, states[i].z, &cx, &cz);
        }
        add("cellAt_all", t, nFar);

        t = now();
        for (unsigned int i = 0; i < nFar; ++i) engine::resolveCharByHand(states[i].hIndex, states[i].hSerial, states[i].hType, states[i].hContainer, states[i].hContainerSerial);
        add("resolveHand_all", t, nFar);

        t = now();
        for (unsigned int i = 0; i < nFar && i < 64; ++i) {
            char sid[64], fac[64];
            float x, y, z, h, a;
            bool d;
            engine::describeCharacter(chars[i], sid, sizeof(sid), fac, sizeof(fac), &x, &y, &z, &h, &d, &a);
        }
        add("describeCharacter_64", t, nFar < 64 ? nFar : 64);

        t = now();
        unsigned int nZone = 0;
        for (unsigned int i = 0; i < nSquad; ++i) if (engine::isZoneLoadedAt(gw, squad[i].x, squad[i].y, squad[i].z)) ++nZone;
        add("isZoneLoaded_squad", t, nSquad);

        t = now();
        unsigned int invItems = 0;
        for (unsigned int i = 0; i < nSquad; ++i) {
            unsigned int h[5];
            handFromEntity(squad[i], h);
            const int n = engine::invList(gw, h, inv, 256, 0);
            if (n > 0) invItems += (unsigned int)n;
        }
        add("invList_squad", t, invItems);

        t = now();
        unsigned int c2Items = 0;
        for (unsigned int i = 0; i < nFar; ++i) c2Items += engine::readC2Rows((RootObject*)chars[i], rows, 1024);
        add("readC2Rows_near", t, c2Items);
        (void)nZone;
    }
    void report() {
        for (std::map<std::string, Stat>::iterator it = stats_.begin(); it != stats_.end(); ++it) {
            std::vector<float>& v = it->second.us;
            if (v.empty()) continue;
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (size_t i = 0; i < v.size(); ++i) sum += v[i];
            ftLine("FT PERF call=%s rounds=%u items=%.0f mean_us=%.0f p50_us=%.0f p95_us=%.0f max_us=%.0f", it->first.c_str(), (unsigned)v.size(),
                   (double)it->second.items / (double)v.size(), sum / (double)v.size(), v[v.size() / 2], v[v.size() * 95 / 100], v[v.size() - 1]);
        }
    }
    struct Stat { std::vector<float> us; unsigned long long items; Stat() : items(0) {} };
    std::map<std::string, Stat> stats_;
    LARGE_INTEGER freq_;
    int round_, rounds_;
    unsigned long lastMs_, everyMs_;
    FrameClock frames_;
};

// ---- func_moves -----------------------------------------------------------------------------------------------
// The host moves one carried item back and forth between two characters of its own squad.
// KENSHICOOP_FT_RATE: moves per second. KENSHICOOP_FT_MOVES: how many. KENSHICOOP_FT_SETTLE_S: wait before the end.
// Both windows log the two inventories at the end, the oracle compares them. A solo run shows the cost of the engine
// call alone, a duo run adds the sync. Conservation is checked on the side doing the moves.
class FuncMovesScenario : public TimedScenario {
public:
    FuncMovesScenario() : TimedScenario("func_moves", 0), picked_(false), moved_(0), refused_(0), tried_(0), lastMs_(0), doneMs_(0), start_(0) {
        QueryPerformanceFrequency(&freq_);
        const int rate = envInt("KENSHICOOP_FT_RATE", 5);
        gapMs_ = rate > 0 ? (unsigned long)(1000 / rate) : 200ul;
        moves_ = envInt("KENSHICOOP_FT_MOVES", 300);
        settleMs_ = (unsigned long)envInt("KENSHICOOP_FT_SETTLE_S", 20) * 1000ul;
        toB_ = true;
        memset(a_, 0, sizeof(a_));
        memset(b_, 0, sizeof(b_));
        sid_[0] = '\0';
        type_ = 0;
    }
    virtual void onStart(const ScenarioContext& ctx) {
        pick(ctx);
        ftLine("FT START name=func_moves role=%s picked=%d sid=%s a=%u,%u b=%u,%u gapMs=%lu moves=%d", ctx.isHost ? "host" : "join", picked_ ? 1 : 0, sid_,
               a_[0], a_[1], b_[0], b_[1], gapMs_, moves_);
        if (picked_) sum("begin");
        start_ = ctx.elapsedMs;
    }
    virtual bool onTick(const ScenarioContext& ctx) {
        frames_.tick();
        if (!picked_) { setPassed(false); return ctx.elapsedMs > 5000; }
        // Only the host moves: a join copy of the same characters is what the sync must keep equal.
        if (ctx.isHost && tried_ < moves_ && ctx.elapsedMs - lastMs_ >= gapMs_) {
            lastMs_ = ctx.elapsedMs;
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            const int got = toB_ ? engine::moveItemBetweenContainers(ctx.gw, a_, b_, sid_, type_, 1)
                                 : engine::moveItemBetweenContainers(ctx.gw, b_, a_, sid_, type_, 1);
            QueryPerformanceCounter(&t1);
            us_.push_back((float)(1000000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq_.QuadPart));
            if (got > 0) { ++moved_; toB_ = !toB_; } else { ++refused_; toB_ = !toB_; }
            ++tried_;
        }
        const bool finishedMoving = !ctx.isHost || tried_ >= moves_;
        if (!finishedMoving) return false;
        if (doneMs_ == 0) {
            doneMs_ = ctx.elapsedMs;
            if (!ctx.isHost) doneMs_ += (unsigned long)moves_ * gapMs_ + 5000ul;  // a join reads after the host
            frames_.report("moves");
        }
        if (ctx.elapsedMs < doneMs_ + settleMs_) return false;
        sum("end");
        if (!us_.empty()) {
            std::sort(us_.begin(), us_.end());
            ftLine("FT MOVES tried=%d moved=%d refused=%d p50_us=%.0f p95_us=%.0f max_us=%.0f", tried_, moved_, refused_, us_[us_.size() / 2], us_[us_.size() * 95 / 100], us_[us_.size() - 1]);
        }
        setPassed(true);
        return true;
    }
private:
    void pick(const ScenarioContext& ctx) {
        static EntityState squad[96];
        static InvItemEntry inv[256];
        const unsigned int n = engine::captureSquad(ctx.gw, false, squad, 96);
        gw_ = ctx.gw;
        for (unsigned int i = 0; i < n && !picked_; ++i) {
            unsigned int h[5];
            handFromEntity(squad[i], h);
            const int c = engine::invList(ctx.gw, h, inv, 256, 0);
            for (int k = 0; k < c && !picked_; ++k) {
                if (inv[k].equipped || inv[k].quantity < 1) continue;
                for (unsigned int j = 0; j < n; ++j) {
                    if (j == i || squad[j].hContainer != squad[i].hContainer || squad[j].hContainerSerial != squad[i].hContainerSerial) continue;
                    memcpy(a_, h, sizeof(a_));
                    handFromEntity(squad[j], b_);
                    strncpy(sid_, inv[k].stringID, sizeof(sid_) - 1);
                    sid_[sizeof(sid_) - 1] = '\0';
                    type_ = inv[k].itemType;
                    picked_ = true;
                    break;
                }
            }
        }
    }
    unsigned int count(const unsigned int h[5]) {
        static InvItemEntry inv[256];
        const int c = engine::invList(gw_, h, inv, 256, 0);
        unsigned int q = 0;
        for (int k = 0; k < c; ++k) if (strcmp(inv[k].stringID, sid_) == 0) q += inv[k].quantity;
        return q;
    }
    void sum(const char* when) {
        const unsigned int qa = count(a_), qb = count(b_);
        ftLine("FT QTY when=%s a=%u b=%u total=%u", when, qa, qb, qa + qb);
    }
    GameWorld* gw_;
    bool picked_, toB_;
    unsigned int a_[5], b_[5];
    char sid_[48];
    unsigned int type_;
    int moved_, refused_, tried_, moves_;
    unsigned long lastMs_, gapMs_, settleMs_, doneMs_, start_;
    LARGE_INTEGER freq_;
    std::vector<float> us_;
    FrameClock frames_;
};

}  // namespace

Scenario* makeFuncScenario(const std::string& name) {
    if (name == "func_rows") return new FuncRowsScenario();
    if (name == "func_perf") return new FuncPerfScenario();
    if (name == "func_moves") return new FuncMovesScenario();
    return 0;
}

}  // namespace coop
