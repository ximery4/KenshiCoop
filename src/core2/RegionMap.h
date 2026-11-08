// RegionMap.h - who simulates which part of the world (core2, design: docs/HYBRID_ZONES_20261104.md).
//
// The host's registry of regions: one or more engine zone cells with one owner. The owner simulates the region
// and writes to the host's book. Pure logic: no engine, no network, no clock of its own, so it runs headless.
//
// Rules: the first visitor owns the region; the host decides, so every machine gets the same answer; a region
// nobody stands in sleeps; a write carries the region epoch, and a stale one is refused; the host takes a region
// over from a client only after being there together for a while and only when nothing is going on there.
//
// The DLL builds with VC++ 2010 (v100): keep to C++03 plus what v100 accepts.

#ifndef KENSHICOOP_CORE2_REGIONMAP_H
#define KENSHICOOP_CORE2_REGIONMAP_H

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace core2 {

typedef unsigned int RegionId;
typedef unsigned int RegionPlayer;  // player ids as on the wire: 0 is the host
const RegionPlayer REGION_NOBODY = 0xFFFFFFFFu;

struct RegionCell {
    int x, z;
    RegionCell() : x(0), z(0) {}
    RegionCell(int cx, int cz) : x(cx), z(cz) {}
    bool operator<(const RegionCell& o) const { return x != o.x ? x < o.x : z < o.z; }
    bool operator==(const RegionCell& o) const { return x == o.x && z == o.z; }
};

enum RegionState {
    RS_SLEEP,     // nobody simulates it; its truth is in the book and the region journal
    RS_ACTIVE,    // the owner simulates it
    RS_HANDOVER   // the owner still simulates it, handTo was told to take it and has not answered yet
};

enum RegionWrite {
    RW_OK,
    RW_NOT_OWNER,
    RW_STALE_EPOCH,
    RW_ASLEEP
};

struct RegionTuning {
    float nearUnits;              // a client this close to the host walks with it, the host owns what they enter
    unsigned long togetherMs;     // game time host and owner must be together before a takeover
    unsigned long busyCapMs;      // game time the takeover waits for a busy region, then goes ahead
    unsigned long leaveGraceMs;   // game time the owner may be away before the region is passed on
    unsigned long handoverMs;     // real time the new owner has to answer
    unsigned long cooldownMs;     // real time after an aborted handover before the next try
    unsigned long heartbeatMs;    // real time of silence after which a player counts as gone
    RegionTuning()
        : nearUnits(1500.f), togetherMs(10000), busyCapMs(60000), leaveGraceMs(8000),
          handoverMs(5000), cooldownMs(30000), heartbeatMs(5000) {}
};

// One player's report for one tick.
struct RegionPresence {
    RegionPlayer player;
    float x, z;                       // where the leader stands, used for nearUnits
    std::vector<RegionCell> cells;    // cells this player's tabs stand in
    RegionPresence() : player(REGION_NOBODY), x(0.f), z(0.f) {}
};

struct RegionGrant {
    RegionId region;
    RegionPlayer owner;
    unsigned int epoch;
    bool mine;                        // the asking player is the owner now
    RegionGrant() : region(0), owner(REGION_NOBODY), epoch(0), mine(false) {}
};

// What the registry tells the machines to do. Applied in order.
enum RegionEventKind {
    RE_OWNER,          // region has a new owner, or none: owner == REGION_NOBODY means it fell asleep
    RE_HANDOVER_BEGIN, // to must take the region: it answers with ackHandover
    RE_HANDOVER_ABORT  // to did not answer, owner stays
};

struct RegionEvent {
    RegionEventKind kind;
    RegionId region;
    RegionPlayer owner, to;
    unsigned int epoch;
    int reason;
};

enum RegionReason {
    RR_FIRST_VISITOR = 1,
    RR_NEAR_HOST,
    RR_TAKEOVER,
    RR_OWNER_LEFT,
    RR_OWNER_GONE,
    RR_NOBODY_LEFT,
    RR_NO_ANSWER,
    RR_PIN
};

class RegionMap {
public:
    explicit RegionMap(RegionPlayer host = 0, const RegionTuning& t = RegionTuning());

    // A base or a town: these cells have one owner. Cells that belong to a region already stay where they are.
    // Returns the region id, 0 when no cell could be taken.
    RegionId defineRegion(const std::vector<RegionCell>& cells);
    RegionId regionOf(const RegionCell& c);      // creates a one-cell region for a cell not seen before
    RegionId findRegion(const RegionCell& c) const;  // 0 when the cell was never seen

    // A base stays with this player: no takeover while the pinned player owns it. REGION_NOBODY removes the pin.
    void pin(RegionId r, RegionPlayer p);

    // The asking player approaches or enters a cell.
    RegionGrant claim(RegionPlayer p, const RegionCell& c, float x, float z, unsigned long nowMs);

    // Called about once a second with every connected player's report. A silent player stays present until
    // the heartbeat runs out. gameMs runs with the game speed, nowMs is real time.
    void update(const std::vector<RegionPresence>& reports, unsigned long nowMs, unsigned long gameMs);

    // Something is going on in the region: combat, an open trade, a dialogue. Holds a takeover, up to busyCapMs.
    void setBusy(RegionId r, bool busy);

    // The new owner has taken the region.
    bool ackHandover(RegionPlayer p, RegionId r, unsigned int epoch);

    // A player's network connection ended: no waiting for the heartbeat.
    void playerGone(RegionPlayer p, unsigned long nowMs);

    // May this player write to this region with this epoch?
    RegionWrite checkWrite(RegionPlayer p, RegionId r, unsigned int epoch) const;

    RegionPlayer owner(RegionId r) const;
    unsigned int epoch(RegionId r) const;
    RegionState state(RegionId r) const;
    RegionPlayer handTo(RegionId r) const;
    std::vector<RegionId> regions() const;
    std::vector<RegionCell> cellsOf(RegionId r) const;
    bool connected(RegionPlayer p) const;

    std::vector<RegionEvent> takeEvents();

private:
    struct Region {
        RegionPlayer owner, handTo, pinned;
        unsigned int epoch;
        RegionState state;
        bool busy;
        std::vector<RegionCell> cells;
        std::vector<RegionPlayer> arrivals;   // who stands here now, in order of arrival
        unsigned long togetherSinceGame;      // 0 = host and owner are not together
        unsigned long ownerAwaySinceGame;     // 0 = the owner is here
        unsigned long handoverStartMs;
        unsigned long retryAfterMs;
        Region() : owner(REGION_NOBODY), handTo(REGION_NOBODY), pinned(REGION_NOBODY), epoch(0), state(RS_SLEEP),
                   busy(false), togetherSinceGame(0), ownerAwaySinceGame(0), handoverStartMs(0), retryAfterMs(0) {}
    };
    struct PlayerRec {
        unsigned long lastSeenMs;
        bool connected;
        float x, z;
        bool havePos;
        PlayerRec() : lastSeenMs(0), connected(false), x(0.f), z(0.f), havePos(false) {}
    };

    void setOwner(RegionId id, Region& r, RegionPlayer p, int reason);
    void sleep(RegionId id, Region& r, int reason);
    void beginHandover(RegionId id, Region& r, RegionPlayer to, unsigned long nowMs);
    bool nearHost(RegionPlayer p, float x, float z) const;
    void passOn(RegionId id, Region& r, int reason, unsigned long nowMs);
    void updateRegion(RegionId id, Region& r, unsigned long nowMs, unsigned long gameMs);
    void push(RegionEventKind k, RegionId id, const Region& r, RegionPlayer to, int reason);

    RegionPlayer host_;
    RegionTuning t_;
    RegionId nextId_;
    std::map<RegionCell, RegionId> cellToRegion_;
    std::map<RegionId, Region> regions_;
    std::map<RegionPlayer, PlayerRec> players_;
    std::vector<RegionEvent> events_;
};

}  // namespace core2

#endif  // KENSHICOOP_CORE2_REGIONMAP_H
