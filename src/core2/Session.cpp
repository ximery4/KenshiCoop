// Session.cpp - see Session.h and docs/CORE2_DESIGN.md.

#include "Session.h"

#include <cstring>

namespace core2 {

namespace {

enum MsgType {
    M_HELLO = 1, M_MOVE, M_MOVE_RES, M_RESERVE, M_RESERVE_RES, M_RELEASE, M_DELTAS, M_SNAP_REQ, M_SNAP, M_HASHES, M_META, M_WELCOME
};

const size_t MAX_MSG = 1100;            // keeps every message inside one Steam/ENet datagram
const u32 SNAP_ITEMS_PER_PART = 16;
const unsigned long RETRY_MS = 400;
const unsigned long ASK_AGAIN_MS = 1000;
const unsigned long RESERVE_HOLD_MS = 30000;
const size_t DEFAULT_HASH_PER_TICK = 64;
const unsigned long BEHIND_WAIT_MS = 1500;   // deltas in flight are not a gap
const size_t KEEP_RESULTS = 4096;             // per player; far longer than any retry

class W {
public:
    Bytes b;
    void u8(unsigned v) { b.push_back((unsigned char)v); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back((unsigned char)(v >> (8 * i))); }
    void i16(short v) { b.push_back((unsigned char)(v & 255)); b.push_back((unsigned char)((v >> 8) & 255)); }
    void f32(float v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
    void str(const std::string& s) { size_t n = s.size() > 255 ? 255 : s.size(); u8((unsigned)n); b.insert(b.end(), s.begin(), s.begin() + n); }
};

// Every read is bounds-checked: a damaged message sets bad and yields zeros.
class R {
public:
    const Bytes& b;
    size_t at;
    bool bad;
    explicit R(const Bytes& m) : b(m), at(0), bad(false) {}
    unsigned u8() { if (at + 1 > b.size()) { bad = true; return 0; } return b[at++]; }
    u32 u32v() {
        if (at + 4 > b.size()) { bad = true; return 0; }
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= (u32)b[at++] << (8 * i);
        return v;
    }
    short i16() {
        if (at + 2 > b.size()) { bad = true; return 0; }
        unsigned v = b[at] | ((unsigned)b[at + 1] << 8);
        at += 2;
        return (short)v;
    }
    float f32() { u32 x = u32v(); float v; std::memcpy(&v, &x, 4); return v; }
    std::string str() {
        unsigned n = u8();
        if (bad || at + n > b.size()) { bad = true; return std::string(); }
        std::string s(reinterpret_cast<const char*>(&b[0]) + at, n);
        at += n;
        return s;
    }
    bool done() const { return !bad && at == b.size(); }
};

void writeDelta(W& w, const Delta& d) {
    w.u32v(d.container);
    w.u32v(d.rev);
    w.u8(d.op);
    w.u32v(d.item.id);
    if (d.op == D_PUT) {
        w.str(d.item.sid);
        w.u32v(d.item.qty);
        w.f32(d.item.quality);
        w.u32v((u32)d.item.level);
        w.i16(d.item.cell.section);
        w.i16(d.item.cell.x);
        w.i16(d.item.cell.y);
    }
}

bool readDelta(R& r, Delta& d) {
    d.container = r.u32v();
    d.rev = r.u32v();
    unsigned op = r.u8();
    if (op > D_REMOVE) return false;
    d.op = (DeltaOp)op;
    d.item.id = r.u32v();
    d.item.where = d.container;
    if (d.op == D_PUT) {
        d.item.sid = r.str();
        d.item.qty = r.u32v();
        d.item.quality = r.f32();
        d.item.level = (int)r.u32v();
        d.item.cell.section = r.i16();
        d.item.cell.x = r.i16();
        d.item.cell.y = r.i16();
    }
    return !r.bad;
}

} // namespace

// ---- host ----------------------------------------------------------------------------------

HostSession::HostSession(ItemBook& book, Transport& net)
    : book_(book), net_(net), hashAt_(0), hashPerTick_(DEFAULT_HASH_PER_TICK) {}

void HostSession::sendTo(PlayerId p, const Bytes& m) {
    stats_.bytesSent += m.size();
    ++stats_.msgsSent;
    net_.send(p, m);
}

void HostSession::sendAll(const Bytes& m) {
    for (std::set<PlayerId>::const_iterator i = players_.begin(); i != players_.end(); ++i) sendTo(*i, m);
}

void HostSession::addPlayer(PlayerId p) { players_.insert(p); }

void HostSession::removePlayer(PlayerId p) {
    players_.erase(p);
    book_.releaseAll(p);
}

void HostSession::sendMeta(PlayerId to, ContainerId c) {
    const ContainerRec* r = book_.container(c);
    if (!r) return;
    W w;
    w.u8(M_META);
    w.u32v(c);
    w.u8(r->kind);
    w.u32v(r->owner);
    w.u32v(r->epoch);
    std::map<ContainerId, std::string>::const_iterator l = labels_.find(c);
    w.str(l == labels_.end() ? std::string() : l->second);
    if (to != 0) sendTo(to, w.b);
    else sendAll(w.b);
}

void HostSession::sendSnapshot(PlayerId to, ContainerId c) {
    const ContainerRec* r = book_.container(c);
    if (!r) return;
    std::vector<Delta> all;
    book_.snapshot(c, all);
    u32 parts = all.empty() ? 1 : (u32)((all.size() + SNAP_ITEMS_PER_PART - 1) / SNAP_ITEMS_PER_PART);
    for (u32 p = 0; p < parts; ++p) {
        W w;
        w.u8(M_SNAP);
        w.u32v(c);
        w.u32v(r->rev);
        w.u32v(p);
        w.u32v(parts);
        size_t from = p * SNAP_ITEMS_PER_PART, to2 = from + SNAP_ITEMS_PER_PART;
        if (to2 > all.size()) to2 = all.size();
        w.u32v((u32)(to2 - from));
        for (size_t i = from; i < to2; ++i) writeDelta(w, all[i]);
        sendTo(to, w.b);
    }
    ++stats_.snapshots;
}

void HostSession::onHello(PlayerId from) {
    W hi;
    hi.u8(M_WELCOME);
    hi.u32v(++generation_[from]);
    sendTo(from, hi.b);
    order_.clear();
    for (ContainerId c = 1; book_.container(c) != 0; ++c) {
        order_.push_back(c);
        sendMeta(from, c);
        sendSnapshot(from, c);
    }
}

void HostSession::rememberReserve(PlayerId p, TxnId txn, Verdict v) {
    results_[std::make_pair(p, txn)] = v;
    std::deque<TxnId>& q = resultOrder_[p];
    q.push_back(txn);
    if (q.size() > KEEP_RESULTS) {
        results_.erase(std::make_pair(p, q.front()));
        q.pop_front();
    }
}

void HostSession::setLabel(ContainerId c, const std::string& label) {
    labels_[c] = label;
    sendMeta(0, c);
}

void HostSession::setOwner(ContainerId c, PlayerId owner) {
    if (book_.setOwner(c, owner) == V_OK) sendMeta(0, c);
}

void HostSession::receive(PlayerId from, const Bytes& msg, unsigned long nowMs) {
    R r(msg);
    unsigned type = r.u8();
    switch (type) {
    case M_HELLO:
        if (r.done()) { players_.insert(from); onHello(from); } else ++stats_.badMsgs;
        return;
    case M_MOVE: {
        MoveRequest q;
        q.player = from;
        q.txn = r.u32v();
        q.item = r.u32v();
        q.qty = r.u32v();
        q.dst = r.u32v();
        q.cell.section = r.i16();
        q.cell.x = r.i16();
        q.cell.y = r.i16();
        q.srcEpoch = r.u32v();
        q.dstEpoch = r.u32v();
        if (!r.done()) { ++stats_.badMsgs; return; }
        MoveResult res = book_.move(q, nowMs);
        W w;
        w.u8(M_MOVE_RES);
        w.u32v(q.txn);
        w.u8(res.verdict);
        w.u32v(res.moved);
        sendTo(from, w.b);
        return;
    }
    case M_RESERVE: {
        TxnId txn = r.u32v();
        ContainerId c = r.u32v();
        if (!r.done()) { ++stats_.badMsgs; return; }
        std::pair<PlayerId, TxnId> key(from, txn);
        std::map<std::pair<PlayerId, TxnId>, Verdict>::iterator seen = results_.find(key);
        Verdict v;
        if (seen != results_.end()) {
            v = seen->second;
        } else {
            v = book_.reserve(from, c, nowMs, RESERVE_HOLD_MS);
            rememberReserve(from, txn, v);
        }
        W w;
        w.u8(M_RESERVE_RES);
        w.u32v(txn);
        w.u8(v);
        sendTo(from, w.b);
        return;
    }
    case M_RELEASE: {
        ContainerId c = r.u32v();
        if (!r.done()) { ++stats_.badMsgs; return; }
        book_.release(from, c);
        return;
    }
    case M_SNAP_REQ: {
        ContainerId c = r.u32v();
        if (!r.done()) { ++stats_.badMsgs; return; }
        if (book_.container(c) == 0) { ++stats_.badMsgs; return; }
        sendMeta(from, c);
        sendSnapshot(from, c);
        return;
    }
    default:
        ++stats_.badMsgs;
    }
}

void HostSession::tick(unsigned long nowMs) {
    book_.tick(nowMs);
    std::vector<Delta> d;
    book_.drainDeltas(d);
    W w;
    w.u8(M_DELTAS);
    for (size_t i = 0; i < d.size(); ++i) {
        W one;
        writeDelta(one, d[i]);
        if (w.b.size() + one.b.size() > MAX_MSG) {
            sendAll(w.b);
            w.b.clear();
            w.u8(M_DELTAS);
        }
        w.b.insert(w.b.end(), one.b.begin(), one.b.end());
    }
    if (w.b.size() > 1) sendAll(w.b);

    if (order_.empty()) {
        for (ContainerId c = 1; book_.container(c) != 0; ++c) order_.push_back(c);
    } else if (book_.container((ContainerId)order_.size() + 1) != 0) {
        for (ContainerId c = (ContainerId)order_.size() + 1; book_.container(c) != 0; ++c) {
            order_.push_back(c);
            sendMeta(0, c);   // a container born in play
        }
    }
    if (order_.empty() || players_.empty()) return;
    // A rotating window: each container is checked every (containers / hashPerTick_) ticks.
    W h;
    h.u8(M_HASHES);
    for (size_t n = 0; n < hashPerTick_ && n < order_.size(); ++n) {
        if (hashAt_ >= order_.size()) hashAt_ = 0;
        ContainerId c = order_[hashAt_++];
        h.u32v(c);
        h.u32v(book_.container(c)->rev);
        h.u32v(book_.hashContainer(c));
        if (h.b.size() + 12 > MAX_MSG) {
            sendAll(h.b);
            h.b.clear();
            h.u8(M_HASHES);
        }
    }
    if (h.b.size() > 1) sendAll(h.b);
}

// ---- client --------------------------------------------------------------------------------

ClientSession::ClientSession(PlayerId me, Transport& net) : me_(me), net_(net), nextTxn_(1), ready_(false) {}

void ClientSession::sendHost(const Bytes& m) {
    stats_.bytesSent += m.size();
    ++stats_.msgsSent;
    net_.send(0, m);
}

void ClientSession::connect() {
    W w;
    w.u8(M_HELLO);
    ready_ = false;
    sendHost(w.b);
}

const ContainerMeta* ClientSession::meta(ContainerId c) const {
    std::map<ContainerId, ContainerMeta>::const_iterator i = metas_.find(c);
    return i == metas_.end() ? 0 : &i->second;
}

void ClientSession::askSnapshot(ContainerId c, unsigned long nowMs) {
    std::map<ContainerId, unsigned long>::iterator a = asked_.find(c);
    if (a != asked_.end() && nowMs < a->second) return;
    asked_[c] = nowMs + ASK_AGAIN_MS;
    W w;
    w.u8(M_SNAP_REQ);
    w.u32v(c);
    sendHost(w.b);
}

TxnId ClientSession::requestMove(ItemId item, u32 qty, ContainerId dst, const Cell& cell, unsigned long nowMs) {
    const ItemRec* it = view_.item(item);
    const ContainerMeta* ms = it ? meta(it->where) : 0;
    const ContainerMeta* md = meta(dst);
    TxnId txn = nextTxn_++;
    W w;
    w.u8(M_MOVE);
    w.u32v(txn);
    w.u32v(item);
    w.u32v(qty);
    w.u32v(dst);
    w.i16(cell.section);
    w.i16(cell.x);
    w.i16(cell.y);
    w.u32v(ms ? ms->epoch : 0);
    w.u32v(md ? md->epoch : 0);
    Pending& p = pending_[txn];
    p.msg = w.b;
    p.nextMs = nowMs + RETRY_MS;
    sendHost(w.b);
    return txn;
}

TxnId ClientSession::requestReserve(ContainerId c, unsigned long nowMs) {
    TxnId txn = nextTxn_++;
    W w;
    w.u8(M_RESERVE);
    w.u32v(txn);
    w.u32v(c);
    Pending& p = pending_[txn];
    p.msg = w.b;
    p.nextMs = nowMs + RETRY_MS;
    sendHost(w.b);
    return txn;
}

void ClientSession::requestRelease(ContainerId c) {
    W w;
    w.u8(M_RELEASE);
    w.u32v(c);
    sendHost(w.b);
}

void ClientSession::drainResults(std::vector<LocalResult>& out) {
    out.insert(out.end(), results_.begin(), results_.end());
    results_.clear();
}

void ClientSession::tick(unsigned long nowMs) {
    for (std::map<TxnId, Pending>::iterator i = pending_.begin(); i != pending_.end(); ++i) {
        if (nowMs >= i->second.nextMs) {
            i->second.nextMs = nowMs + RETRY_MS;
            ++stats_.retries;
            sendHost(i->second.msg);
        }
    }
}

void ClientSession::receive(const Bytes& msg, unsigned long nowMs) {
    R r(msg);
    unsigned type = r.u8();
    switch (type) {
    case M_MOVE_RES:
    case M_RESERVE_RES: {
        LocalResult res;
        res.txn = r.u32v();
        res.verdict = (Verdict)r.u8();
        if (type == M_MOVE_RES) res.moved = r.u32v();
        if (!r.done()) { ++stats_.badMsgs; return; }
        std::map<TxnId, Pending>::iterator p = pending_.find(res.txn);
        if (p == pending_.end()) return;      // a repeated answer
        pending_.erase(p);
        results_.push_back(res);
        return;
    }
    case M_WELCOME: {
        u32 gen = r.u32v();
        if (!r.done()) { ++stats_.badMsgs; return; }
        // Answers from an earlier connect keep their ids; new requests start in the new range.
        if ((gen << 20) > nextTxn_) nextTxn_ = (gen << 20) | 1;
        return;
    }
    case M_META: {
        ContainerMeta m;
        m.id = r.u32v();
        unsigned k = r.u8();
        m.owner = r.u32v();
        m.epoch = r.u32v();
        m.label = r.str();
        if (!r.done() || k > CK_GROUND) { ++stats_.badMsgs; return; }
        m.kind = (ContainerKind)k;
        std::map<ContainerId, ContainerMeta>::iterator old = metas_.find(m.id);
        if (old == metas_.end() || m.epoch >= old->second.epoch) metas_[m.id] = m;
        return;
    }
    case M_DELTAS: {
        while (r.at < msg.size()) {
            Delta d;
            if (!readDelta(r, d)) { ++stats_.badMsgs; return; }
            if (!view_.apply(d)) {
                ++stats_.gaps;
                askSnapshot(d.container, nowMs);
            }
        }
        return;
    }
    case M_SNAP: {
        ContainerId c = r.u32v();
        u32 rev = r.u32v(), part = r.u32v(), total = r.u32v(), n = r.u32v();
        if (r.bad || total == 0 || part >= total || n > SNAP_ITEMS_PER_PART) { ++stats_.badMsgs; return; }
        std::vector<Delta> items;
        for (u32 i = 0; i < n; ++i) {
            Delta d;
            if (!readDelta(r, d)) { ++stats_.badMsgs; return; }
            items.push_back(d);
        }
        if (!r.done()) { ++stats_.badMsgs; return; }
        SnapParts& s = snaps_[c];
        if (s.rev != rev || s.total != total) { s.parts.clear(); s.rev = rev; s.total = total; }
        s.parts[part] = items;
        if ((u32)s.parts.size() == total) {
            // An old snapshot must not undo newer deltas.
            if (rev >= view_.rev(c)) {
                std::vector<Delta> all;
                for (std::map<u32, std::vector<Delta> >::iterator p = s.parts.begin(); p != s.parts.end(); ++p)
                    all.insert(all.end(), p->second.begin(), p->second.end());
                view_.applySnapshot(c, rev, all);
            }
            snaps_.erase(c);
            asked_.erase(c);
            if (!ready_) ready_ = true;
        }
        return;
    }
    case M_HASHES: {
        while (r.at < msg.size()) {
            ContainerId c = r.u32v();
            u32 rev = r.u32v(), hash = r.u32v();
            if (r.bad) { ++stats_.badMsgs; return; }
            const u32 mine = view_.rev(c);
            if (mine == rev) {
                if (view_.hashContainer(c) != hash) {
                    ++stats_.hashMismatch;
                    askSnapshot(c, nowMs);
                }
            } else if (mine < rev) {
                // Deltas may still be on the way: only ask when the same rev stays out of reach.
                std::map<ContainerId, std::pair<u32, unsigned long> >::iterator b = behind_.find(c);
                if (b == behind_.end() || mine >= b->second.first) {
                    behind_[c] = std::make_pair(rev, nowMs);
                } else if (nowMs - b->second.second >= BEHIND_WAIT_MS) {
                    askSnapshot(c, nowMs);
                    behind_.erase(b);
                }
            }
        }
        return;
    }
    default:
        ++stats_.badMsgs;
    }
}

} // namespace core2
