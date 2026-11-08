// RegionMap.cpp - see RegionMap.h.

#include "RegionMap.h"

#include <algorithm>

namespace core2 {

RegionMap::RegionMap(RegionPlayer host, const RegionTuning& t) : host_(host), t_(t), nextId_(1) {}

RegionId RegionMap::defineRegion(const std::vector<RegionCell>& cells) {
    RegionId id = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (cellToRegion_.find(cells[i]) != cellToRegion_.end()) continue;
        if (id == 0) id = nextId_++;
        cellToRegion_[cells[i]] = id;
        regions_[id].cells.push_back(cells[i]);
    }
    return id;
}

RegionId RegionMap::regionOf(const RegionCell& c) {
    std::map<RegionCell, RegionId>::const_iterator it = cellToRegion_.find(c);
    if (it != cellToRegion_.end()) return it->second;
    RegionId id = nextId_++;
    cellToRegion_[c] = id;
    regions_[id].cells.push_back(c);
    return id;
}

RegionId RegionMap::findRegion(const RegionCell& c) const {
    std::map<RegionCell, RegionId>::const_iterator it = cellToRegion_.find(c);
    return it == cellToRegion_.end() ? 0 : it->second;
}

void RegionMap::pin(RegionId r, RegionPlayer p) {
    std::map<RegionId, Region>::iterator it = regions_.find(r);
    if (it != regions_.end()) it->second.pinned = p;
}

void RegionMap::push(RegionEventKind k, RegionId id, const Region& r, RegionPlayer to, int reason) {
    RegionEvent e;
    e.kind = k;
    e.region = id;
    e.owner = r.owner;
    e.to = to;
    e.epoch = r.epoch;
    e.reason = reason;
    events_.push_back(e);
}

void RegionMap::setOwner(RegionId id, Region& r, RegionPlayer p, int reason) {
    r.owner = p;
    r.handTo = REGION_NOBODY;
    r.state = RS_ACTIVE;
    ++r.epoch;
    r.togetherSinceGame = 0;
    r.ownerAwaySinceGame = 0;
    push(RE_OWNER, id, r, p, reason);
}

void RegionMap::sleep(RegionId id, Region& r, int reason) {
    r.owner = REGION_NOBODY;
    r.handTo = REGION_NOBODY;
    r.state = RS_SLEEP;
    ++r.epoch;
    r.togetherSinceGame = 0;
    r.ownerAwaySinceGame = 0;
    r.busy = false;
    push(RE_OWNER, id, r, REGION_NOBODY, reason);
}

void RegionMap::beginHandover(RegionId id, Region& r, RegionPlayer to, unsigned long nowMs) {
    r.state = RS_HANDOVER;
    r.handTo = to;
    r.handoverStartMs = nowMs;
    push(RE_HANDOVER_BEGIN, id, r, to, 0);
}

bool RegionMap::nearHost(RegionPlayer p, float x, float z) const {
    if (p == host_) return false;
    std::map<RegionPlayer, PlayerRec>::const_iterator h = players_.find(host_);
    if (h == players_.end() || !h->second.connected || !h->second.havePos) return false;
    const float dx = h->second.x - x, dz = h->second.z - z;
    return dx * dx + dz * dz <= t_.nearUnits * t_.nearUnits;
}

RegionGrant RegionMap::claim(RegionPlayer p, const RegionCell& c, float x, float z, unsigned long nowMs) {
    PlayerRec& pr = players_[p];
    pr.lastSeenMs = nowMs;
    pr.connected = true;
    pr.x = x;
    pr.z = z;
    pr.havePos = true;

    const RegionId id = regionOf(c);
    Region& r = regions_[id];
    if (r.state == RS_SLEEP) {
        if (nearHost(p, x, z)) setOwner(id, r, host_, RR_NEAR_HOST);
        else setOwner(id, r, p, RR_FIRST_VISITOR);
    }
    if (std::find(r.arrivals.begin(), r.arrivals.end(), p) == r.arrivals.end()) r.arrivals.push_back(p);

    RegionGrant g;
    g.region = id;
    g.owner = r.owner;
    g.epoch = r.epoch;
    g.mine = r.owner == p;
    return g;
}

// The next owner is whoever came first among those still here.
void RegionMap::passOn(RegionId id, Region& r, int reason, unsigned long nowMs) {
    for (size_t i = 0; i < r.arrivals.size(); ++i) {
        const RegionPlayer p = r.arrivals[i];
        if (p == r.owner || !connected(p)) continue;
        if (reason == RR_OWNER_GONE) setOwner(id, r, p, reason);   // nobody left to hand over from
        else beginHandover(id, r, p, nowMs);
        return;
    }
    if (reason == RR_OWNER_GONE || r.owner != REGION_NOBODY) sleep(id, r, reason == RR_OWNER_GONE ? RR_OWNER_GONE : RR_NOBODY_LEFT);
}

void RegionMap::updateRegion(RegionId id, Region& r, unsigned long nowMs, unsigned long gameMs) {
    if (r.state == RS_SLEEP) return;

    if (!connected(r.owner)) {
        passOn(id, r, RR_OWNER_GONE, nowMs);
        return;
    }

    if (r.state == RS_HANDOVER) {
        const bool targetLost = !connected(r.handTo) ||
            std::find(r.arrivals.begin(), r.arrivals.end(), r.handTo) == r.arrivals.end();
        if (targetLost || nowMs - r.handoverStartMs >= t_.handoverMs) {
            const RegionPlayer to = r.handTo;
            r.state = RS_ACTIVE;
            r.handTo = REGION_NOBODY;
            r.retryAfterMs = nowMs + t_.cooldownMs;
            push(RE_HANDOVER_ABORT, id, r, to, RR_NO_ANSWER);
        }
        return;
    }

    const bool ownerHere = std::find(r.arrivals.begin(), r.arrivals.end(), r.owner) != r.arrivals.end();
    if (ownerHere) {
        r.ownerAwaySinceGame = 0;
    } else {
        if (r.ownerAwaySinceGame == 0) r.ownerAwaySinceGame = gameMs ? gameMs : 1;
        if (gameMs - r.ownerAwaySinceGame >= t_.leaveGraceMs && nowMs >= r.retryAfterMs) {
            passOn(id, r, RR_OWNER_LEFT, nowMs);
            return;
        }
    }

    const bool hostHere = std::find(r.arrivals.begin(), r.arrivals.end(), host_) != r.arrivals.end();
    if (r.owner == host_ || !hostHere || !ownerHere || r.pinned == r.owner) {
        r.togetherSinceGame = 0;
        return;
    }
    if (r.togetherSinceGame == 0) r.togetherSinceGame = gameMs ? gameMs : 1;
    const unsigned long together = gameMs - r.togetherSinceGame;
    if (nowMs < r.retryAfterMs || together < t_.togetherMs) return;
    if (!r.busy || together >= t_.busyCapMs) beginHandover(id, r, host_, nowMs);
}

void RegionMap::update(const std::vector<RegionPresence>& reports, unsigned long nowMs, unsigned long gameMs) {
    // Reports first, so a region sees everyone's newest position in the same pass.
    std::set<RegionPlayer> reported;
    for (size_t i = 0; i < reports.size(); ++i) {
        PlayerRec& pr = players_[reports[i].player];
        pr.lastSeenMs = nowMs;
        pr.connected = true;
        pr.x = reports[i].x;
        pr.z = reports[i].z;
        pr.havePos = true;
        reported.insert(reports[i].player);
    }
    for (std::map<RegionPlayer, PlayerRec>::iterator p = players_.begin(); p != players_.end(); ++p)
        if (p->second.connected && nowMs - p->second.lastSeenMs >= t_.heartbeatMs) p->second.connected = false;

    // Who stands where. A silent player stays where it last was until its heartbeat runs out.
    std::map<RegionId, std::set<RegionPlayer> > here;
    for (size_t i = 0; i < reports.size(); ++i) {
        for (size_t c = 0; c < reports[i].cells.size(); ++c) {
            const RegionCell& cell = reports[i].cells[c];
            RegionId id = findRegion(cell);
            if (id == 0) id = regionOf(cell);
            here[id].insert(reports[i].player);
            Region& r = regions_[id];
            if (r.state == RS_SLEEP) claim(reports[i].player, cell, reports[i].x, reports[i].z, nowMs);
        }
    }
    for (std::map<RegionId, Region>::iterator it = regions_.begin(); it != regions_.end(); ++it) {
        Region& r = it->second;
        std::vector<RegionPlayer> kept;
        for (size_t i = 0; i < r.arrivals.size(); ++i) {
            const RegionPlayer p = r.arrivals[i];
            const bool spoke = reported.find(p) != reported.end();
            const std::set<RegionPlayer>& now = here[it->first];
            if (spoke ? now.find(p) != now.end() : connected(p)) kept.push_back(p);
        }
        const std::set<RegionPlayer>& now = here[it->first];
        for (std::set<RegionPlayer>::const_iterator n = now.begin(); n != now.end(); ++n)
            if (std::find(kept.begin(), kept.end(), *n) == kept.end()) kept.push_back(*n);
        r.arrivals.swap(kept);
    }
    for (std::map<RegionId, Region>::iterator it = regions_.begin(); it != regions_.end(); ++it)
        updateRegion(it->first, it->second, nowMs, gameMs);
}

void RegionMap::setBusy(RegionId r, bool busy) {
    std::map<RegionId, Region>::iterator it = regions_.find(r);
    if (it != regions_.end()) it->second.busy = busy;
}

bool RegionMap::ackHandover(RegionPlayer p, RegionId id, unsigned int epoch) {
    std::map<RegionId, Region>::iterator it = regions_.find(id);
    if (it == regions_.end()) return false;
    Region& r = it->second;
    if (r.state != RS_HANDOVER || r.handTo != p || r.epoch != epoch) return false;
    setOwner(id, r, p, p == host_ ? RR_TAKEOVER : RR_OWNER_LEFT);
    return true;
}

void RegionMap::playerGone(RegionPlayer p, unsigned long nowMs) {
    players_[p].connected = false;
    for (std::map<RegionId, Region>::iterator it = regions_.begin(); it != regions_.end(); ++it) {
        Region& r = it->second;
        r.arrivals.erase(std::remove(r.arrivals.begin(), r.arrivals.end(), p), r.arrivals.end());
        if (r.state == RS_SLEEP) continue;
        if (r.owner == p) {
            passOn(it->first, r, RR_OWNER_GONE, nowMs);
        } else if (r.state == RS_HANDOVER && r.handTo == p) {
            r.state = RS_ACTIVE;
            r.handTo = REGION_NOBODY;
            r.retryAfterMs = nowMs + t_.cooldownMs;
            push(RE_HANDOVER_ABORT, it->first, r, p, RR_NO_ANSWER);
        }
    }
}

RegionWrite RegionMap::checkWrite(RegionPlayer p, RegionId id, unsigned int epoch) const {
    std::map<RegionId, Region>::const_iterator it = regions_.find(id);
    if (it == regions_.end() || it->second.state == RS_SLEEP) return RW_ASLEEP;
    if (it->second.owner != p) return RW_NOT_OWNER;
    if (it->second.epoch != epoch) return RW_STALE_EPOCH;
    return RW_OK;
}

RegionPlayer RegionMap::owner(RegionId r) const {
    std::map<RegionId, Region>::const_iterator it = regions_.find(r);
    return it == regions_.end() ? REGION_NOBODY : it->second.owner;
}

unsigned int RegionMap::epoch(RegionId r) const {
    std::map<RegionId, Region>::const_iterator it = regions_.find(r);
    return it == regions_.end() ? 0 : it->second.epoch;
}

RegionState RegionMap::state(RegionId r) const {
    std::map<RegionId, Region>::const_iterator it = regions_.find(r);
    return it == regions_.end() ? RS_SLEEP : it->second.state;
}

RegionPlayer RegionMap::handTo(RegionId r) const {
    std::map<RegionId, Region>::const_iterator it = regions_.find(r);
    return it == regions_.end() ? REGION_NOBODY : it->second.handTo;
}

std::vector<RegionId> RegionMap::regions() const {
    std::vector<RegionId> out;
    for (std::map<RegionId, Region>::const_iterator it = regions_.begin(); it != regions_.end(); ++it) out.push_back(it->first);
    return out;
}

std::vector<RegionCell> RegionMap::cellsOf(RegionId r) const {
    std::map<RegionId, Region>::const_iterator it = regions_.find(r);
    return it == regions_.end() ? std::vector<RegionCell>() : it->second.cells;
}

bool RegionMap::connected(RegionPlayer p) const {
    std::map<RegionPlayer, PlayerRec>::const_iterator it = players_.find(p);
    return it != players_.end() && it->second.connected;
}

std::vector<RegionEvent> RegionMap::takeEvents() {
    std::vector<RegionEvent> out;
    out.swap(events_);
    return out;
}

}  // namespace core2
