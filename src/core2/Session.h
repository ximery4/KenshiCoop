// Session.h - the core2 item protocol between the host's book and client views.
// Pure logic over an abstract transport: every message may be lost, repeated or reordered,
// and the session must still converge. Requests are retried with the same TxnId; a view that
// sees a gap or a wrong hash asks for a snapshot of that container.

#ifndef KENSHICOOP_CORE2_SESSION_H
#define KENSHICOOP_CORE2_SESSION_H

#include "ItemBook.h"

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace core2 {

typedef std::vector<unsigned char> Bytes;

class Transport {
public:
    virtual ~Transport() {}
    virtual void send(PlayerId to, const Bytes& msg) = 0;   // to = 0 means the host
};

struct ContainerMeta {
    ContainerId id;
    ContainerKind kind;
    PlayerId owner;
    u32 epoch;
    std::string label;   // the machine-independent name of the object, set by the host
    ContainerMeta() : id(0), kind(CK_GROUND), owner(0), epoch(0) {}
};

struct SessionStats {
    unsigned long long bytesSent, msgsSent, badMsgs, gaps, snapshots, hashMismatch, retries;
    SessionStats() : bytesSent(0), msgsSent(0), badMsgs(0), gaps(0), snapshots(0), hashMismatch(0), retries(0) {}
};

class HostSession {
public:
    HostSession(ItemBook& book, Transport& net);

    void addPlayer(PlayerId p);
    void removePlayer(PlayerId p);          // releases the player's reservations
    void receive(PlayerId from, const Bytes& msg, unsigned long nowMs);
    void tick(unsigned long nowMs);         // send deltas, expire reservations, rotate hash checks
    void setOwner(ContainerId c, PlayerId owner);   // the only way an owner changes during play
    void setLabel(ContainerId c, const std::string& label);

    void setHashWindow(size_t perTick) { hashPerTick_ = perTick; }
    const SessionStats& stats() const { return stats_; }
    size_t resultCacheSize() const { return results_.size(); }

private:
    void sendTo(PlayerId p, const Bytes& m);
    void sendAll(const Bytes& m);
    void sendMeta(PlayerId to, ContainerId c);
    void sendSnapshot(PlayerId to, ContainerId c);
    void onHello(PlayerId from);
    void rememberReserve(PlayerId p, TxnId txn, Verdict v);

    ItemBook& book_;
    Transport& net_;
    std::set<PlayerId> players_;
    std::vector<ContainerId> order_;    // containers in hash rotation
    std::map<ContainerId, std::string> labels_;
    size_t hashAt_, hashPerTick_;
    SessionStats stats_;
    std::map<std::pair<PlayerId, TxnId>, Verdict> results_;   // for retried reservation answers
    std::map<PlayerId, std::deque<TxnId> > resultOrder_;      // oldest first, bounds results_
    std::map<PlayerId, u32> generation_;                       // connects so far; high bits of the player's TxnIds
};

struct LocalResult {
    TxnId txn;
    Verdict verdict;
    ItemId moved;
    LocalResult() : txn(0), verdict(V_OK), moved(0) {}
};

class ClientSession {
public:
    ClientSession(PlayerId me, Transport& net);

    void connect();                         // sends HELLO; also after a reconnect
    void receive(const Bytes& msg, unsigned long nowMs);
    void tick(unsigned long nowMs);         // retries unanswered requests

    TxnId requestMove(ItemId item, u32 qty, ContainerId dst, const Cell& cell, unsigned long nowMs);
    TxnId requestReserve(ContainerId c, unsigned long nowMs);
    void requestRelease(ContainerId c);

    void drainResults(std::vector<LocalResult>& out);

    const ItemView& view() const { return view_; }
    const ContainerMeta* meta(ContainerId c) const;
    const std::map<ContainerId, ContainerMeta>& metas() const { return metas_; }
    size_t pending() const { return pending_.size(); }
    bool ready() const { return ready_; }
    const SessionStats& stats() const { return stats_; }
    const ItemRec* item(ItemId id) const { return view_.item(id); }

private:
    struct Pending {
        Bytes msg;
        unsigned long nextMs;
        Pending() : nextMs(0) {}
    };
    struct SnapParts {
        u32 rev, total;
        std::map<u32, std::vector<Delta> > parts;
        SnapParts() : rev(0), total(0) {}
    };
    void sendHost(const Bytes& m);
    void askSnapshot(ContainerId c, unsigned long nowMs);

    PlayerId me_;
    Transport& net_;
    ItemView view_;
    std::map<ContainerId, ContainerMeta> metas_;
    std::map<TxnId, Pending> pending_;
    std::map<ContainerId, unsigned long> asked_;
    std::map<ContainerId, std::pair<u32, unsigned long> > behind_;   // rev the hash list reported, when first seen ahead of us
    std::map<ContainerId, SnapParts> snaps_;
    std::vector<LocalResult> results_;
    TxnId nextTxn_;     // generation << 20 | counter: a restarted game cannot reuse an old id
    bool ready_;
    SessionStats stats_;
};

} // namespace core2

#endif
