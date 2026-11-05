// ItemBook.h - the host's single book of item truth (core2, design: docs/CORE2_DESIGN.md).
//
// Pure logic: no engine, no network, no clock of its own, so it runs headless in tests.
// The engine adapter turns player actions into Request()s and applies Delta records to game
// inventories. Every item has one ItemId, one location, and changes only through this class.
//
// The DLL builds with VC++ 2010 (v100): keep to C++03 plus what v100 accepts.

#ifndef KENSHICOOP_CORE2_ITEMBOOK_H
#define KENSHICOOP_CORE2_ITEMBOOK_H

#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace core2 {

typedef unsigned int u32;
typedef u32 ItemId;       // issued by the host for one session; 0 = none
typedef u32 ContainerId;  // issued by the host; 0 = none
typedef u32 PlayerId;     // 0 = the world (no player owns it)
typedef u32 TxnId;        // unique per player, retried with the same id

struct Cell {
    short section, x, y;
    Cell() : section(0), x(0), y(0) {}
    Cell(short s, short cx, short cy) : section(s), x(cx), y(cy) {}
    bool operator==(const Cell& o) const { return section == o.section && x == o.x && y == o.y; }
};

struct ItemRec {
    ItemId id;
    std::string sid;      // template stringID, the same on every machine with the same mod set
    u32 qty;
    float quality;
    int level;
    ContainerId where;
    Cell cell;
    ItemRec() : id(0), qty(0), quality(0.f), level(0), where(0) {}
};

enum ContainerKind {
    CK_CHARACTER,  // inventory of a live character
    CK_BAG,        // backpack worn or carried; its own item sits in another container
    CK_CORPSE,     // loot: needs a reservation
    CK_STORAGE,    // chest, machine, shop: needs a reservation unless the player owns it
    CK_GROUND
};

struct ContainerRec {
    ContainerId id;
    ContainerKind kind;
    PlayerId owner;
    u32 epoch;              // bumped on every owner change; a request at another epoch is stale
    PlayerId reservedBy;    // 0 = free
    unsigned long reservedUntilMs;
    u32 rev;                // bumped on every change; Delta carries it so a view can see gaps
    ContainerRec() : id(0), kind(CK_GROUND), owner(0), epoch(1), reservedBy(0), reservedUntilMs(0), rev(0) {}
};

enum Verdict {
    V_OK = 0,
    V_NO_ITEM,      // already gone: another request took it
    V_NO_CONTAINER,
    V_BUSY,         // reserved by another player
    V_NOT_RESERVED, // this container needs a reservation first
    V_FORBIDDEN,    // another player's character or bag
    V_STALE_EPOCH,  // the owner changed after the request was sent
    V_BAD_QTY,
    V_CELL_TAKEN    // a different item is in the target cell
};
const char* verdictName(Verdict v);

enum DeltaOp { D_PUT, D_REMOVE };

// One change to one container. Applying all deltas in rev order gives the host's state.
struct Delta {
    ContainerId container;
    u32 rev;
    DeltaOp op;
    ItemRec item;   // D_REMOVE uses only item.id
    Delta() : container(0), rev(0), op(D_PUT) {}
};

struct MoveRequest {
    PlayerId player;
    TxnId txn;
    ItemId item;
    u32 qty;              // 0 = the whole stack
    ContainerId dst;
    Cell cell;
    u32 srcEpoch, dstEpoch;
    MoveRequest() : player(0), txn(0), item(0), qty(0), dst(0), srcEpoch(0), dstEpoch(0) {}
};

struct MoveResult {
    Verdict verdict;
    ItemId moved;       // the stack that landed: request.item, a new id, or the id it merged into
    bool repeated;      // the same txn again: same answer, nothing done a second time
    MoveResult() : verdict(V_NO_ITEM), moved(0), repeated(false) {}
};

class ItemBook {
public:
    ItemBook();

    // Setup, from the save at session start and for containers created in play.
    ContainerId addContainer(ContainerKind kind, PlayerId owner);
    ItemId addItem(ContainerId where, const std::string& sid, u32 qty, float quality, int level, const Cell& cell);

    // Engine-made and engine-consumed items (crafting, mining, eating) are logged, so a
    // difference in totals always has a recorded reason.
    bool destroyItem(ItemId id, u32 qty);

    Verdict setOwner(ContainerId c, PlayerId newOwner);
    Verdict reserve(PlayerId player, ContainerId c, unsigned long nowMs, unsigned long holdMs);
    Verdict release(PlayerId player, ContainerId c);
    void releaseAll(PlayerId player);      // disconnect
    void tick(unsigned long nowMs);        // expires reservations

    MoveResult move(const MoveRequest& r, unsigned long nowMs);

    // The engine already did it (shadow mode, and engine-driven moves later on): no checks, the book records.
    bool recordMove(ItemId id, ContainerId dst, const Cell& cell);
    bool recordQty(ItemId id, u32 qty);      // a difference counts as created or destroyed

    const ItemRec* item(ItemId id) const;
    const ContainerRec* container(ContainerId id) const;
    std::vector<ItemRec> contents(ContainerId c) const;

    // Order-independent, same on every machine with the same contents. The live check compares these.
    u32 hashContainer(ContainerId c) const;
    // Total quantity per template, plus the logged created and destroyed amounts.
    std::map<std::string, long long> totals() const;
    long long created(const std::string& sid) const;
    long long destroyed(const std::string& sid) const;

    size_t itemCount() const { return items_.size(); }
    size_t containerCount() const { return containers_.size(); }
    size_t rememberedTxns() const { return done_.size(); }

    // Changes since the last call, in the order they happened. The caller sends them to every view.
    void drainDeltas(std::vector<Delta>& out);
    // Everything a joining or gap-recovering view needs for one container.
    void snapshot(ContainerId c, std::vector<Delta>& out) const;

private:
    typedef std::map<ItemId, ItemRec> ItemMap;
    typedef std::map<ContainerId, ContainerRec> ContainerMap;

    Verdict checkAccess(PlayerId p, const ContainerRec& c, u32 epoch, unsigned long nowMs) const;
    ItemRec* itemInCell(ContainerId c, const Cell& cell);
    void place(ItemRec& it, ContainerId c, const Cell& cell);
    void putDelta(const ItemRec& it);
    void removeDelta(ContainerId c, ItemId id);
    void unindex(ContainerId c, ItemId id);
    MoveResult remember(const MoveRequest& q, const MoveResult& res);

    ItemMap items_;
    ContainerMap containers_;
    std::map<ContainerId, std::vector<ItemId> > byContainer_;
    std::map<std::pair<PlayerId, TxnId>, MoveResult> done_;
    std::map<PlayerId, std::deque<TxnId> > doneOrder_;   // oldest first, bounds done_
    std::map<std::string, long long> created_, destroyed_;
    std::vector<Delta> pending_;
    ItemId nextItem_;
    ContainerId nextContainer_;
};

// A client's (or the adapter's) copy, fed only by deltas.
class ItemView {
public:
    ItemView() {}
    // false = a gap: this delta came after a missed revision. Nothing is applied; ask for a snapshot.
    bool apply(const Delta& d);
    // Replaces one container with the book's snapshot; also how a detected gap is repaired.
    void applySnapshot(ContainerId c, u32 rev, const std::vector<Delta>& puts);
    u32 hashContainer(ContainerId c) const;
    u32 contentHashContainer(ContainerId c) const;
    std::vector<ItemRec> contents(ContainerId c) const;
    size_t itemCount() const { return items_.size(); }
    u32 rev(ContainerId c) const;
    const ItemRec* item(ItemId id) const { std::map<ItemId, ItemRec>::const_iterator i = items_.find(id); return i == items_.end() ? 0 : &i->second; }
private:
    std::map<ItemId, ItemRec> items_;
    std::map<ContainerId, u32> revs_;
};

// Order-independent content hash shared by the book and views.
u32 hashItem(const ItemRec& it);
// The same without the session id: comparable with what a machine reads from its own game.
u32 contentHash(const char* sid, u32 qty, const Cell& cell);

} // namespace core2

#endif
