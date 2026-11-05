// ItemBook.cpp - see ItemBook.h and docs/CORE2_DESIGN.md.

#include "ItemBook.h"

#include <algorithm>
#include <cstring>

namespace core2 {

const char* verdictName(Verdict v) {
    switch (v) {
    case V_OK: return "OK";
    case V_NO_ITEM: return "NO_ITEM";
    case V_NO_CONTAINER: return "NO_CONTAINER";
    case V_BUSY: return "BUSY";
    case V_NOT_RESERVED: return "NOT_RESERVED";
    case V_FORBIDDEN: return "FORBIDDEN";
    case V_STALE_EPOCH: return "STALE_EPOCH";
    case V_BAD_QTY: return "BAD_QTY";
    case V_CELL_TAKEN: return "CELL_TAKEN";
    }
    return "?";
}

static u32 fnv(u32 h, const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}

u32 hashItem(const ItemRec& it) {
    u32 h = 2166136261u;
    h = fnv(h, &it.id, sizeof(it.id));
    h = fnv(h, it.sid.data(), it.sid.size());
    h = fnv(h, &it.qty, sizeof(it.qty));
    h = fnv(h, &it.quality, sizeof(it.quality));
    h = fnv(h, &it.level, sizeof(it.level));
    h = fnv(h, &it.cell.section, sizeof(short));
    h = fnv(h, &it.cell.x, sizeof(short));
    h = fnv(h, &it.cell.y, sizeof(short));
    return h;
}

u32 contentHash(const char* sid, u32 qty, const Cell& cell) {
    u32 h = 2166136261u;
    h = fnv(h, sid, std::strlen(sid));
    h = fnv(h, &qty, sizeof(qty));
    h = fnv(h, &cell.section, sizeof(short));
    h = fnv(h, &cell.x, sizeof(short));
    h = fnv(h, &cell.y, sizeof(short));
    return h;
}

ItemBook::ItemBook() : nextItem_(1), nextContainer_(1) {}

ContainerId ItemBook::addContainer(ContainerKind kind, PlayerId owner) {
    ContainerRec c;
    c.id = nextContainer_++;
    c.kind = kind;
    c.owner = owner;
    containers_[c.id] = c;
    return c.id;
}

ItemId ItemBook::addItem(ContainerId where, const std::string& sid, u32 qty, float quality, int level, const Cell& cell) {
    if (containers_.find(where) == containers_.end() || qty == 0) return 0;
    ItemRec it;
    it.id = nextItem_++;
    it.sid = sid;
    it.qty = qty;
    it.quality = quality;
    it.level = level;
    it.where = where;
    it.cell = cell;
    items_[it.id] = it;
    byContainer_[where].push_back(it.id);
    created_[sid] += qty;
    putDelta(it);
    return it.id;
}

bool ItemBook::destroyItem(ItemId id, u32 qty) {
    ItemMap::iterator i = items_.find(id);
    if (i == items_.end() || qty == 0 || qty > i->second.qty) return false;
    destroyed_[i->second.sid] += qty;
    if (qty == i->second.qty) {
        ContainerId c = i->second.where;
        unindex(c, id);
        items_.erase(i);
        removeDelta(c, id);
    } else {
        i->second.qty -= qty;
        putDelta(i->second);
    }
    return true;
}

Verdict ItemBook::setOwner(ContainerId c, PlayerId newOwner) {
    ContainerMap::iterator i = containers_.find(c);
    if (i == containers_.end()) return V_NO_CONTAINER;
    i->second.owner = newOwner;
    ++i->second.epoch;
    i->second.reservedBy = 0;
    return V_OK;
}

Verdict ItemBook::reserve(PlayerId player, ContainerId c, unsigned long nowMs, unsigned long holdMs) {
    ContainerMap::iterator i = containers_.find(c);
    if (i == containers_.end()) return V_NO_CONTAINER;
    ContainerRec& r = i->second;
    if (r.reservedBy != 0 && r.reservedBy != player && nowMs < r.reservedUntilMs) return V_BUSY;
    r.reservedBy = player;
    r.reservedUntilMs = nowMs + holdMs;
    return V_OK;
}

Verdict ItemBook::release(PlayerId player, ContainerId c) {
    ContainerMap::iterator i = containers_.find(c);
    if (i == containers_.end()) return V_NO_CONTAINER;
    if (i->second.reservedBy != player) return V_BUSY;
    i->second.reservedBy = 0;
    return V_OK;
}

void ItemBook::releaseAll(PlayerId player) {
    for (ContainerMap::iterator i = containers_.begin(); i != containers_.end(); ++i)
        if (i->second.reservedBy == player) i->second.reservedBy = 0;
}

void ItemBook::tick(unsigned long nowMs) {
    for (ContainerMap::iterator i = containers_.begin(); i != containers_.end(); ++i)
        if (i->second.reservedBy != 0 && nowMs >= i->second.reservedUntilMs) i->second.reservedBy = 0;
}

Verdict ItemBook::checkAccess(PlayerId p, const ContainerRec& c, u32 epoch, unsigned long nowMs) const {
    if (epoch != c.epoch) return V_STALE_EPOCH;
    if (c.reservedBy != 0 && c.reservedBy != p && nowMs < c.reservedUntilMs) return V_BUSY;
    switch (c.kind) {
    case CK_CHARACTER:
    case CK_BAG:
        // A player's own squad and bags. World-owned ones are free to act on (an NPC's copy).
        if (c.owner != 0 && c.owner != p) return V_FORBIDDEN;
        return V_OK;
    case CK_CORPSE:
        if (c.reservedBy != p || nowMs >= c.reservedUntilMs) return V_NOT_RESERVED;
        return V_OK;
    case CK_STORAGE:
        if (c.owner == p) return V_OK;
        if (c.reservedBy != p || nowMs >= c.reservedUntilMs) return V_NOT_RESERVED;
        return V_OK;
    case CK_GROUND:
        return V_OK;
    }
    return V_FORBIDDEN;
}

ItemRec* ItemBook::itemInCell(ContainerId c, const Cell& cell) {
    std::map<ContainerId, std::vector<ItemId> >::iterator v = byContainer_.find(c);
    if (v == byContainer_.end()) return 0;
    for (size_t i = 0; i < v->second.size(); ++i) {
        ItemRec& r = items_[v->second[i]];
        if (r.cell == cell) return &r;
    }
    return 0;
}

void ItemBook::unindex(ContainerId c, ItemId id) {
    std::vector<ItemId>& v = byContainer_[c];
    std::vector<ItemId>::iterator f = std::find(v.begin(), v.end(), id);
    if (f != v.end()) { *f = v.back(); v.pop_back(); }
}

void ItemBook::putDelta(const ItemRec& it) {
    ContainerRec& c = containers_[it.where];
    Delta d;
    d.container = it.where;
    d.rev = ++c.rev;
    d.op = D_PUT;
    d.item = it;
    pending_.push_back(d);
}

void ItemBook::removeDelta(ContainerId c, ItemId id) {
    ContainerRec& r = containers_[c];
    Delta d;
    d.container = c;
    d.rev = ++r.rev;
    d.op = D_REMOVE;
    d.item.id = id;
    pending_.push_back(d);
}

MoveResult ItemBook::remember(const MoveRequest& q, const MoveResult& res) {
    done_[std::make_pair(q.player, q.txn)] = res;
    std::deque<TxnId>& order = doneOrder_[q.player];
    order.push_back(q.txn);
    if (order.size() > 4096) {     // far longer than any retry window
        done_.erase(std::make_pair(q.player, order.front()));
        order.pop_front();
    }
    return res;
}

MoveResult ItemBook::move(const MoveRequest& q, unsigned long nowMs) {
    std::pair<PlayerId, TxnId> key(q.player, q.txn);
    std::map<std::pair<PlayerId, TxnId>, MoveResult>::iterator seen = done_.find(key);
    if (seen != done_.end()) {
        MoveResult again = seen->second;
        again.repeated = true;
        return again;
    }

    MoveResult res;
    ItemMap::iterator ii = items_.find(q.item);
    ContainerMap::iterator di = containers_.find(q.dst);
    if (ii == items_.end()) { res.verdict = V_NO_ITEM; return remember(q, res); }
    if (di == containers_.end()) { res.verdict = V_NO_CONTAINER; return remember(q, res); }

    ItemRec& it = ii->second;
    u32 qty = q.qty == 0 ? it.qty : q.qty;
    if (qty > it.qty) { res.verdict = V_BAD_QTY; return remember(q, res); }

    const ContainerId srcId = it.where;
    const ContainerRec& src = containers_[srcId];
    Verdict v = checkAccess(q.player, src, q.srcEpoch, nowMs);
    if (v == V_OK) v = checkAccess(q.player, di->second, q.dstEpoch, nowMs);
    if (v != V_OK) { res.verdict = v; return remember(q, res); }

    ItemRec* occupant = itemInCell(q.dst, q.cell);
    if (occupant == &it) occupant = 0;
    const bool merge = occupant != 0 && occupant->sid == it.sid
        && occupant->quality == it.quality && occupant->level == it.level;
    if (occupant != 0 && !merge) { res.verdict = V_CELL_TAKEN; return remember(q, res); }

    if (merge) {
        ItemId into = occupant->id;
        occupant->qty += qty;
        putDelta(*occupant);
        if (qty == it.qty) {
            ItemId gone = it.id;
            unindex(srcId, gone);
            items_.erase(ii);
            removeDelta(srcId, gone);
        } else {
            it.qty -= qty;
            putDelta(it);
        }
        res.moved = into;
    } else if (qty == it.qty) {
        if (srcId == q.dst) {
            it.cell = q.cell;
            putDelta(it);
        } else {
            unindex(srcId, it.id);
            removeDelta(srcId, it.id);
            it.where = q.dst;
            it.cell = q.cell;
            byContainer_[q.dst].push_back(it.id);
            putDelta(it);
        }
        res.moved = it.id;
    } else {
        it.qty -= qty;
        putDelta(it);
        ItemRec part = it;
        part.id = nextItem_++;
        part.qty = qty;
        part.where = q.dst;
        part.cell = q.cell;
        items_[part.id] = part;
        byContainer_[q.dst].push_back(part.id);
        putDelta(part);
        res.moved = part.id;
    }
    res.verdict = V_OK;
    return remember(q, res);
}

bool ItemBook::recordMove(ItemId id, ContainerId dst, const Cell& cell) {
    ItemMap::iterator i = items_.find(id);
    if (i == items_.end() || containers_.find(dst) == containers_.end()) return false;
    ItemRec& it = i->second;
    if (it.where != dst) {
        unindex(it.where, id);
        removeDelta(it.where, id);
        it.where = dst;
        byContainer_[dst].push_back(id);
    }
    it.cell = cell;
    putDelta(it);
    return true;
}

bool ItemBook::recordQty(ItemId id, u32 qty) {
    ItemMap::iterator i = items_.find(id);
    if (i == items_.end() || qty == 0) return false;
    ItemRec& it = i->second;
    if (qty > it.qty) created_[it.sid] += qty - it.qty;
    else destroyed_[it.sid] += it.qty - qty;
    it.qty = qty;
    putDelta(it);
    return true;
}

const ItemRec* ItemBook::item(ItemId id) const {
    ItemMap::const_iterator i = items_.find(id);
    return i == items_.end() ? 0 : &i->second;
}

const ContainerRec* ItemBook::container(ContainerId id) const {
    ContainerMap::const_iterator i = containers_.find(id);
    return i == containers_.end() ? 0 : &i->second;
}

std::vector<ItemRec> ItemBook::contents(ContainerId c) const {
    std::vector<ItemRec> out;
    std::map<ContainerId, std::vector<ItemId> >::const_iterator v = byContainer_.find(c);
    if (v == byContainer_.end()) return out;
    for (size_t i = 0; i < v->second.size(); ++i) out.push_back(items_.find(v->second[i])->second);
    return out;
}

u32 ItemBook::hashContainer(ContainerId c) const {
    u32 sum = 0;
    std::map<ContainerId, std::vector<ItemId> >::const_iterator v = byContainer_.find(c);
    if (v == byContainer_.end()) return 0;
    for (size_t i = 0; i < v->second.size(); ++i) sum += hashItem(items_.find(v->second[i])->second);
    return sum;
}

std::map<std::string, long long> ItemBook::totals() const {
    std::map<std::string, long long> t;
    for (ItemMap::const_iterator i = items_.begin(); i != items_.end(); ++i) t[i->second.sid] += i->second.qty;
    return t;
}

long long ItemBook::created(const std::string& sid) const {
    std::map<std::string, long long>::const_iterator i = created_.find(sid);
    return i == created_.end() ? 0 : i->second;
}

long long ItemBook::destroyed(const std::string& sid) const {
    std::map<std::string, long long>::const_iterator i = destroyed_.find(sid);
    return i == destroyed_.end() ? 0 : i->second;
}

void ItemBook::drainDeltas(std::vector<Delta>& out) {
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

void ItemBook::snapshot(ContainerId c, std::vector<Delta>& out) const {
    std::map<ContainerId, std::vector<ItemId> >::const_iterator v = byContainer_.find(c);
    ContainerMap::const_iterator ci = containers_.find(c);
    if (ci == containers_.end()) return;
    if (v == byContainer_.end()) return;
    for (size_t i = 0; i < v->second.size(); ++i) {
        Delta d;
        d.container = c;
        d.rev = ci->second.rev;
        d.op = D_PUT;
        d.item = items_.find(v->second[i])->second;
        out.push_back(d);
    }
}

bool ItemView::apply(const Delta& d) {
    u32& r = revs_[d.container];
    if (d.rev <= r) return true;          // already have it (a repeat)
    if (d.rev != r + 1) return false;     // a gap
    r = d.rev;
    if (d.op == D_PUT) {
        items_[d.item.id] = d.item;
    } else {
        // Only if still here: after a snapshot of the new container this item may already sit there.
        std::map<ItemId, ItemRec>::iterator i = items_.find(d.item.id);
        if (i != items_.end() && i->second.where == d.container) items_.erase(i);
    }
    return true;
}

void ItemView::applySnapshot(ContainerId c, u32 rev, const std::vector<Delta>& puts) {
    for (std::map<ItemId, ItemRec>::iterator i = items_.begin(); i != items_.end();) {
        if (i->second.where == c) items_.erase(i++);
        else ++i;
    }
    for (size_t i = 0; i < puts.size(); ++i) items_[puts[i].item.id] = puts[i].item;
    revs_[c] = rev;
}

u32 ItemView::hashContainer(ContainerId c) const {
    u32 sum = 0;
    for (std::map<ItemId, ItemRec>::const_iterator i = items_.begin(); i != items_.end(); ++i)
        if (i->second.where == c) sum += hashItem(i->second);
    return sum;
}

u32 ItemView::contentHashContainer(ContainerId c) const {
    u32 sum = 0;
    for (std::map<ItemId, ItemRec>::const_iterator i = items_.begin(); i != items_.end(); ++i)
        if (i->second.where == c) sum += contentHash(i->second.sid.c_str(), i->second.qty, i->second.cell);
    return sum;
}

std::vector<ItemRec> ItemView::contents(ContainerId c) const {
    std::vector<ItemRec> out;
    for (std::map<ItemId, ItemRec>::const_iterator i = items_.begin(); i != items_.end(); ++i)
        if (i->second.where == c) out.push_back(i->second);
    return out;
}

u32 ItemView::rev(ContainerId c) const {
    std::map<ContainerId, u32>::const_iterator i = revs_.find(c);
    return i == revs_.end() ? 0 : i->second;
}

} // namespace core2
