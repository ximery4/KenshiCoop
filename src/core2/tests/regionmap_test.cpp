// regionmap_test.cpp - headless tests of the region registry: every rule alone, then random play with invariants.
// Usage: regionmap_test [claims.tsv]  A TSV replays a real session: ms<TAB>player<TAB>cellX<TAB>cellZ.

#include "../RegionMap.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace core2;

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

namespace {

const RegionPlayer HOST = 0, KAT = 1, THIRD = 2;

RegionPresence at(RegionPlayer p, int cx, int cz) {
    RegionPresence r;
    r.player = p;
    r.x = (float)cx * 4608.f;
    r.z = (float)cz * 4608.f;
    r.cells.push_back(RegionCell(cx, cz));
    return r;
}

RegionPresence atPos(RegionPlayer p, int cx, int cz, float x, float z) {
    RegionPresence r = at(p, cx, cz);
    r.x = x;
    r.z = z;
    return r;
}

// Runs the world for seconds with fixed reports, once a second. The same clock for real and game time.
struct World {
    RegionMap m;
    unsigned long now;
    explicit World(const RegionTuning& t = RegionTuning()) : m(HOST, t), now(1000) {}
    void run(const std::vector<RegionPresence>& who, int seconds) {
        for (int i = 0; i < seconds; ++i) {
            now += 1000;
            m.update(who, now, now);
        }
    }
    void run1(const RegionPresence& a, int seconds) { std::vector<RegionPresence> v(1, a); run(v, seconds); }
    void run2(const RegionPresence& a, const RegionPresence& b, int seconds) {
        std::vector<RegionPresence> v;
        v.push_back(a);
        v.push_back(b);
        run(v, seconds);
    }
    // Answers a handover if one is open.
    bool answer(RegionId r) {
        if (m.state(r) != RS_HANDOVER) return false;
        return m.ackHandover(m.handTo(r), r, m.epoch(r));
    }
};

void testFirstVisitor() {
    World w;
    RegionGrant g = w.m.claim(KAT, RegionCell(20, 32), 0.f, 0.f, w.now);
    CHECK(g.mine);
    CHECK(g.owner == KAT);
    CHECK(g.epoch == 1);
    RegionGrant h = w.m.claim(THIRD, RegionCell(20, 32), 10.f, 0.f, w.now);
    CHECK(!h.mine);
    CHECK(h.owner == KAT);
    CHECK(h.epoch == 1);
    CHECK(w.m.state(g.region) == RS_ACTIVE);
}

void testEveryMachineGetsOneAnswer() {
    World w;
    const RegionGrant a = w.m.claim(KAT, RegionCell(1, 1), 0.f, 0.f, w.now);
    const RegionGrant b = w.m.claim(HOST, RegionCell(1, 1), 5000.f, 0.f, w.now);
    CHECK(a.owner == b.owner);
    CHECK(a.region == b.region);
    CHECK(a.epoch == b.epoch);
    CHECK(a.mine != b.mine);
}

void testHostDoesNotStealOnArrival() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 5);
    CHECK(w.m.owner(r) == KAT);
    CHECK(w.m.state(r) == RS_ACTIVE);
}

void testTakeoverAfterTogether() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 11);
    CHECK(w.m.state(r) == RS_HANDOVER);
    CHECK(w.m.handTo(r) == HOST);
    CHECK(w.m.owner(r) == KAT);          // the old owner keeps simulating until the answer
    const unsigned int e = w.m.epoch(r);
    CHECK(w.m.checkWrite(KAT, r, e) == RW_OK);
    CHECK(w.answer(r));
    CHECK(w.m.owner(r) == HOST);
    CHECK(w.m.epoch(r) == e + 1);
    CHECK(w.m.checkWrite(KAT, r, e) == RW_NOT_OWNER);
    CHECK(w.m.checkWrite(HOST, r, e) == RW_STALE_EPOCH);
    CHECK(w.m.checkWrite(HOST, r, e + 1) == RW_OK);
}

void testTogetherUsesGameTime() {
    RegionMap m(HOST);
    const RegionId r = m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, 1000).region;
    std::vector<RegionPresence> both;
    both.push_back(at(KAT, 5, 5));
    both.push_back(at(HOST, 5, 5));
    // 5x: ten real seconds are fifty game seconds, the timer must follow the game.
    for (int i = 0; i < 3; ++i) m.update(both, 2000 + i * 1000, 5000 + i * 5000);
    CHECK(m.state(r) == RS_HANDOVER);
    // And a paused game must not run it out.
    RegionMap p(HOST);
    const RegionId q = p.claim(KAT, RegionCell(5, 5), 0.f, 0.f, 1000).region;
    for (int i = 0; i < 30; ++i) p.update(both, 2000 + i * 1000, 5000);
    CHECK(p.state(q) == RS_ACTIVE);
}

void testBusyHoldsTakeover() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    w.m.setBusy(r, true);
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 30);
    CHECK(w.m.state(r) == RS_ACTIVE);
    CHECK(w.m.owner(r) == KAT);
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 31);     // past the cap
    CHECK(w.m.state(r) == RS_HANDOVER);

    World x;
    const RegionId s = x.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, x.now).region;
    x.m.setBusy(s, true);
    x.run2(at(KAT, 5, 5), at(HOST, 5, 5), 20);
    x.m.setBusy(s, false);
    x.run2(at(KAT, 5, 5), at(HOST, 5, 5), 1);      // calm again: goes at once
    CHECK(x.m.state(s) == RS_HANDOVER);
}

void testPinnedBaseStays() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    w.m.pin(r, KAT);
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 120);
    CHECK(w.m.owner(r) == KAT);
    CHECK(w.m.state(r) == RS_ACTIVE);
    w.m.pin(r, REGION_NOBODY);
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 11);
    CHECK(w.m.state(r) == RS_HANDOVER);
}

void testRegionOfSeveralCells() {
    World w;
    std::vector<RegionCell> base;
    base.push_back(RegionCell(29, 36));
    base.push_back(RegionCell(30, 36));
    const RegionId r = w.m.defineRegion(base);
    CHECK(r != 0);
    CHECK(w.m.regionOf(RegionCell(30, 36)) == r);
    CHECK(w.m.cellsOf(r).size() == 2);
    RegionGrant a = w.m.claim(KAT, RegionCell(29, 36), 0.f, 0.f, w.now);
    RegionGrant b = w.m.claim(THIRD, RegionCell(30, 36), 9000.f, 0.f, w.now);
    CHECK(a.region == b.region);
    CHECK(b.owner == KAT);
    // Walking inside the region is not leaving it.
    w.run1(at(KAT, 30, 36), 30);
    CHECK(w.m.owner(r) == KAT);
    // A cell taken already stays where it is.
    std::vector<RegionCell> other;
    other.push_back(RegionCell(30, 36));
    CHECK(w.m.defineRegion(other) == 0);
    CHECK(w.m.regionOf(RegionCell(30, 36)) == r);
}

void testNearHostGoesToHost() {
    World w;
    w.run1(atPos(HOST, 20, 32, 1000.f, 1000.f), 1);
    // The client crosses the border first, a step ahead of the host: not a split.
    RegionGrant g = w.m.claim(KAT, RegionCell(21, 32), 1400.f, 1000.f, w.now);
    CHECK(g.owner == HOST);
    CHECK(!g.mine);
    // Far from the host the same claim is the client's.
    RegionGrant f = w.m.claim(KAT, RegionCell(25, 32), 90000.f, 0.f, w.now);
    CHECK(f.owner == KAT);
    CHECK(f.mine);
    // Adjacent cells are not near by themselves: the split_far2 lesson.
    World x;
    x.run1(atPos(HOST, 21, 31, 0.f, 4000.f), 1);
    RegionGrant a = x.m.claim(KAT, RegionCell(21, 32), 0.f, 9000.f, x.now);
    CHECK(a.owner == KAT);
}

void testOwnerLeaves() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    w.run1(at(KAT, 5, 5), 2);
    // The owner walks off, nobody else: asleep after the grace, not before.
    w.run1(at(KAT, 6, 5), 5);
    CHECK(w.m.owner(r) == KAT);
    w.run1(at(KAT, 6, 5), 5);
    CHECK(w.m.state(r) == RS_SLEEP);
    CHECK(w.m.owner(r) == REGION_NOBODY);
    CHECK(w.m.checkWrite(KAT, r, w.m.epoch(r)) == RW_ASLEEP);

    // With someone else here: handed to them, in order of arrival.
    World x;
    const RegionId s = x.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, x.now).region;
    x.m.claim(THIRD, RegionCell(5, 5), 0.f, 0.f, x.now);
    x.run2(at(KAT, 5, 5), at(THIRD, 5, 5), 1);
    x.run2(at(KAT, 6, 5), at(THIRD, 5, 5), 9);
    CHECK(x.m.state(s) == RS_HANDOVER);
    CHECK(x.m.handTo(s) == THIRD);
    CHECK(x.answer(s));
    CHECK(x.m.owner(s) == THIRD);
}

void testOwnerBackWithinGrace() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    for (int i = 0; i < 20; ++i) {
        w.run1(at(KAT, 6, 5), 3);
        w.run1(at(KAT, 5, 5), 3);
    }
    CHECK(w.m.owner(r) == KAT);
    CHECK(w.m.epoch(r) == 1);  // pacing the border moves nothing: the 13-claims-in-3-minutes case
}

void testOwnerCrash() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    w.m.claim(THIRD, RegionCell(5, 5), 0.f, 0.f, w.now);
    const unsigned int e = w.m.epoch(r);
    w.m.playerGone(KAT, w.now);
    CHECK(w.m.owner(r) == THIRD);
    CHECK(w.m.state(r) == RS_ACTIVE);
    CHECK(w.m.epoch(r) == e + 1);
    CHECK(w.m.checkWrite(KAT, r, e) == RW_NOT_OWNER);

    World x;
    const RegionId s = x.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, x.now).region;
    x.m.playerGone(KAT, x.now);
    CHECK(x.m.state(s) == RS_SLEEP);

    // Silence, no disconnect event: the heartbeat does it.
    World y;
    const RegionId q = y.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, y.now).region;
    y.run1(at(THIRD, 5, 5), 3);
    CHECK(y.m.owner(q) == KAT);
    y.run1(at(THIRD, 5, 5), 4);
    CHECK(y.m.owner(q) == THIRD);
    CHECK(!y.m.connected(KAT));
}

void testNoAnswerKeepsOwner() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 11);
    CHECK(w.m.state(r) == RS_HANDOVER);
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 6);
    CHECK(w.m.state(r) == RS_ACTIVE);
    CHECK(w.m.owner(r) == KAT);
    // No new attempt during the cooldown, one after it.
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 20);
    CHECK(w.m.state(r) == RS_ACTIVE);
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 10);
    CHECK(w.m.state(r) == RS_HANDOVER);
}

void testHandoverTargetLeaves() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 11);
    CHECK(w.m.state(r) == RS_HANDOVER);
    w.m.playerGone(HOST, w.now);
    CHECK(w.m.state(r) == RS_ACTIVE);
    CHECK(w.m.owner(r) == KAT);
    CHECK(!w.m.ackHandover(HOST, r, w.m.epoch(r)));
}

void testWrongAck() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    CHECK(!w.m.ackHandover(HOST, r, w.m.epoch(r)));     // no handover open
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 11);
    CHECK(!w.m.ackHandover(THIRD, r, w.m.epoch(r)));    // not the target
    CHECK(!w.m.ackHandover(HOST, r, w.m.epoch(r) + 7)); // a late answer to something else
    CHECK(w.m.owner(r) == KAT);
}

void testWakeUp() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    w.run1(at(KAT, 6, 5), 10);
    CHECK(w.m.state(r) == RS_SLEEP);
    const unsigned int e = w.m.epoch(r);
    w.run1(at(KAT, 5, 5), 1);
    CHECK(w.m.owner(r) == KAT);
    CHECK(w.m.epoch(r) > e);
}

void testEvents() {
    World w;
    const RegionId r = w.m.claim(KAT, RegionCell(5, 5), 0.f, 0.f, w.now).region;
    std::vector<RegionEvent> ev = w.m.takeEvents();
    CHECK(ev.size() == 1);
    CHECK(ev[0].kind == RE_OWNER);
    CHECK(ev[0].reason == RR_FIRST_VISITOR);
    CHECK(ev[0].owner == KAT);
    w.run2(at(KAT, 5, 5), at(HOST, 5, 5), 11);
    ev = w.m.takeEvents();
    CHECK(ev.size() == 1);
    CHECK(ev[0].kind == RE_HANDOVER_BEGIN);
    CHECK(ev[0].to == HOST);
    CHECK(w.answer(r));
    ev = w.m.takeEvents();
    CHECK(ev.size() == 1);
    CHECK(ev[0].reason == RR_TAKEOVER);
    CHECK(w.m.takeEvents().empty());
}

// Random play: 8 players walk on a small map, connect and drop, answer late or never. Invariants after every step.
struct Rng {
    unsigned int s;
    explicit Rng(unsigned int seed) : s(seed) {}
    unsigned int next(unsigned int n) { s = s * 1664525u + 1013904223u; return (s >> 8) % n; }
};

std::string fuzz(unsigned int seed, int steps) {
    RegionMap m(HOST);
    Rng rng(seed);
    const int PLAYERS = 8;
    int cx[PLAYERS], cz[PLAYERS];
    bool online[PLAYERS];
    for (int i = 0; i < PLAYERS; ++i) { cx[i] = rng.next(6); cz[i] = rng.next(6); online[i] = i < 4; }
    std::vector<RegionCell> base;
    base.push_back(RegionCell(2, 2));
    base.push_back(RegionCell(3, 2));
    m.defineRegion(base);
    std::map<RegionId, unsigned int> lastEpoch;
    unsigned long now = 1000, game = 1000;
    std::ostringstream trace;
    for (int step = 0; step < steps; ++step) {
        now += 1000;
        game += 1000 * (1 + rng.next(5));
        std::vector<RegionPresence> reports;
        for (int p = 0; p < PLAYERS; ++p) {
            if (!online[p]) { if (rng.next(40) == 0) online[p] = true; continue; }
            if (p != HOST && rng.next(200) == 0) { online[p] = false; m.playerGone(p, now); continue; }
            if (rng.next(5) == 0) {
                cx[p] += (int)rng.next(3) - 1;
                cz[p] += (int)rng.next(3) - 1;
                if (cx[p] < 0) cx[p] = 0; if (cx[p] > 5) cx[p] = 5;
                if (cz[p] < 0) cz[p] = 0; if (cz[p] > 5) cz[p] = 5;
            }
            if (rng.next(30) == 0) continue;  // a silent second
            reports.push_back(at(p, cx[p], cz[p]));
        }
        m.update(reports, now, game);
        const std::vector<RegionId> rs = m.regions();
        for (size_t i = 0; i < rs.size(); ++i) {
            const RegionId r = rs[i];
            if (rng.next(3) == 0) m.setBusy(r, rng.next(2) == 0);
            if (m.state(r) == RS_HANDOVER && rng.next(3) == 0) m.ackHandover(m.handTo(r), r, m.epoch(r));
            const unsigned int e = m.epoch(r);
            CHECK(e >= lastEpoch[r]);
            lastEpoch[r] = e;
            const RegionState st = m.state(r);
            const RegionPlayer o = m.owner(r);
            CHECK((st == RS_SLEEP) == (o == REGION_NOBODY));
            if (st == RS_HANDOVER) CHECK(m.handTo(r) != REGION_NOBODY && m.handTo(r) != o);
            else CHECK(m.handTo(r) == REGION_NOBODY);
            // An owner is always someone the registry counts as connected.
            if (st != RS_SLEEP) CHECK(m.connected(o));
            // Exactly the owner may write, with exactly the current epoch.
            int writers = 0;
            for (int p = 0; p < PLAYERS; ++p)
                if (m.checkWrite(p, r, e) == RW_OK) ++writers;
            CHECK(writers == (st == RS_SLEEP ? 0 : 1));
            if (st != RS_SLEEP && e > 0) CHECK(m.checkWrite(o, r, e - 1) == RW_STALE_EPOCH);
            trace << r << ':' << o << ':' << e << ':' << (int)st << ' ';
        }
        std::vector<RegionEvent> ev = m.takeEvents();
        for (size_t i = 0; i < ev.size(); ++i) trace << 'E' << ev[i].kind << ev[i].region << ev[i].reason;
        trace << '\n';
    }
    return trace.str();
}

void testFuzz() {
    for (unsigned int seed = 1; seed <= 40; ++seed) {
        const std::string a = fuzz(seed, 400);
        if (seed <= 5) CHECK(a == fuzz(seed, 400));   // the same input gives the same answer
    }
}

// Replays claims from a real session, one line per cell change, and counts what the registry would have done.
void replay(const char* path) {
    std::ifstream f(path);
    if (!f) { printf("replay: cannot read %s\n", path); ++g_fail; return; }
    struct Ev { unsigned long ms; RegionPlayer p; int x, z; };
    std::vector<Ev> evs;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream s(line);
        Ev e;
        if (s >> e.ms >> e.p >> e.x >> e.z) evs.push_back(e);
    }
    if (evs.empty()) { printf("replay: no events\n"); ++g_fail; return; }
    RegionMap m(HOST);
    std::map<RegionPlayer, RegionCell> where;
    size_t next = 0;
    int owners = 0, handovers = 0, takeovers = 0, sleeps = 0, clientOwned = 0, seconds = 0;
    const unsigned long start = evs[0].ms, end = evs[evs.size() - 1].ms + 60000;
    for (unsigned long now = start; now <= end; now += 1000, ++seconds) {
        while (next < evs.size() && evs[next].ms <= now) {
            where[evs[next].p] = RegionCell(evs[next].x, evs[next].z);
            ++next;
        }
        std::vector<RegionPresence> reports;
        for (std::map<RegionPlayer, RegionCell>::const_iterator w = where.begin(); w != where.end(); ++w)
            reports.push_back(at(w->first, w->second.x, w->second.z));
        m.update(reports, now, now);
        const std::vector<RegionId> rs = m.regions();
        for (size_t i = 0; i < rs.size(); ++i) {
            if (m.state(rs[i]) == RS_HANDOVER) m.ackHandover(m.handTo(rs[i]), rs[i], m.epoch(rs[i]));
            if (m.state(rs[i]) != RS_SLEEP && m.owner(rs[i]) != HOST) ++clientOwned;
        }
        std::vector<RegionEvent> ev = m.takeEvents();
        for (size_t i = 0; i < ev.size(); ++i) {
            if (ev[i].kind == RE_HANDOVER_BEGIN) ++handovers;
            if (ev[i].kind == RE_OWNER) {
                if (ev[i].owner == REGION_NOBODY) ++sleeps; else ++owners;
                if (ev[i].reason == RR_TAKEOVER) ++takeovers;
            }
        }
    }
    printf("REPLAY %s claims=%u seconds=%d owners=%d handovers=%d takeovers=%d sleeps=%d clientOwnedRegionSeconds=%d\n",
           path, (unsigned)evs.size(), seconds, owners, handovers, takeovers, sleeps, clientOwned);
}

}  // namespace

int main(int argc, char** argv) {
    testFirstVisitor();
    testEveryMachineGetsOneAnswer();
    testHostDoesNotStealOnArrival();
    testTakeoverAfterTogether();
    testTogetherUsesGameTime();
    testBusyHoldsTakeover();
    testPinnedBaseStays();
    testRegionOfSeveralCells();
    testNearHostGoesToHost();
    testOwnerLeaves();
    testOwnerBackWithinGrace();
    testOwnerCrash();
    testNoAnswerKeepsOwner();
    testHandoverTargetLeaves();
    testWrongAck();
    testWakeUp();
    testEvents();
    testFuzz();
    for (int i = 1; i < argc; ++i) replay(argv[i]);
    printf("%s pass=%d fail=%d\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
