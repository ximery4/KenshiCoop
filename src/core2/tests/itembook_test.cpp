// itembook_test - headless tests of the item book. No game, no network, runs in seconds.
//   itembook_test              unit cases and the concurrency fuzz
//   itembook_test <items.tsv>  also loads real save contents and measures scale

#include "../ItemBook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace core2;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static MoveRequest req(ItemBook& b, PlayerId p, TxnId t, ItemId it, u32 q, ContainerId d, Cell cell = Cell()) {
    MoveRequest r;
    r.player = p; r.txn = t; r.item = it; r.qty = q; r.dst = d; r.cell = cell;
    const ItemRec* i = b.item(it);
    r.srcEpoch = (i && b.container(i->where)) ? b.container(i->where)->epoch : 1;
    r.dstEpoch = b.container(d) ? b.container(d)->epoch : 1;
    return r;
}

// The whole-book invariant: quantity per template = created - destroyed.
static bool conserved(const ItemBook& b) {
    std::map<std::string, long long> t = b.totals();
    for (std::map<std::string, long long>::const_iterator i = t.begin(); i != t.end(); ++i)
        if (i->second != b.created(i->first) - b.destroyed(i->first)) {
            printf("  not conserved: %s have=%lld created=%lld destroyed=%lld\n", i->first.c_str(), i->second, b.created(i->first), b.destroyed(i->first));
            return false;
        }
    return true;
}

// What the live check orders on a hash mismatch.
static void repair(ItemBook& b, ItemView& v, ContainerId c) {
    std::vector<Delta> s;
    b.snapshot(c, s);
    v.applySnapshot(c, b.container(c)->rev, s);
}

static void testBasics() {
    ItemBook b;
    ContainerId a = b.addContainer(CK_CHARACTER, 1), c = b.addContainer(CK_CHARACTER, 1);
    ItemId i = b.addItem(a, "meat", 10, 100.f, 0, Cell(0, 0, 0));
    MoveResult r = b.move(req(b, 1, 1, i, 3, c, Cell(0, 2, 2)), 0);      // split
    CHECK(r.verdict == V_OK && r.moved != i);
    CHECK(b.item(i)->qty == 7 && b.item(r.moved)->qty == 3 && b.item(r.moved)->cell == Cell(0, 2, 2));
    MoveResult m = b.move(req(b, 1, 2, i, 0, c, Cell(0, 2, 2)), 0);      // merge the rest into it
    CHECK(m.verdict == V_OK && m.moved == r.moved && b.item(r.moved)->qty == 10 && b.item(i) == 0);
    MoveResult n = b.move(req(b, 1, 3, r.moved, 0, c, Cell(0, 5, 5)), 0); // reposition inside a container
    CHECK(n.verdict == V_OK && b.item(r.moved)->cell == Cell(0, 5, 5));
    CHECK(conserved(b));
}

// A move between containers is a remove from one and a put into the other, each with its own rev.
static void testMoveDeltas() {
    ItemBook b;
    ContainerId a = b.addContainer(CK_CHARACTER, 1), c = b.addContainer(CK_CHARACTER, 1);
    ItemId i = b.addItem(a, "sword", 1, 100.f, 0, Cell());
    std::vector<Delta> d;
    b.drainDeltas(d);
    d.clear();
    CHECK(b.move(req(b, 1, 1, i, 0, c), 0).verdict == V_OK);
    b.drainDeltas(d);
    CHECK(d.size() == 2);
    CHECK(d[0].container == a && d[0].op == D_REMOVE && d[0].item.id == i && d[0].rev == 2);
    CHECK(d[1].container == c && d[1].op == D_PUT && d[1].item.id == i && d[1].rev == 1);
}

static void testCellTaken() {
    ItemBook b;
    ContainerId a = b.addContainer(CK_CHARACTER, 1);
    ItemId x = b.addItem(a, "sword", 1, 100.f, 0, Cell(0, 0, 0));
    b.addItem(a, "boots", 1, 100.f, 0, Cell(0, 1, 0));
    CHECK(b.move(req(b, 1, 1, x, 0, a, Cell(0, 1, 0)), 0).verdict == V_CELL_TAKEN);
    CHECK(b.item(x)->cell == Cell(0, 0, 0));
}

// Kat's real case: two players take from one corpse in the same moment. One wins, no copy, no loss.
static void testCorpseRace() {
    ItemBook b;
    ContainerId corpse = b.addContainer(CK_CORPSE, 0), p1 = b.addContainer(CK_CHARACTER, 1), p2 = b.addContainer(CK_CHARACTER, 2);
    ItemId hide = b.addItem(corpse, "hide", 3, 100.f, 0, Cell());
    CHECK(b.move(req(b, 1, 1, hide, 0, p1), 0).verdict == V_NOT_RESERVED);
    CHECK(b.reserve(1, corpse, 0, 5000) == V_OK);
    CHECK(b.reserve(2, corpse, 10, 5000) == V_BUSY);
    CHECK(b.move(req(b, 2, 1, hide, 0, p2), 10).verdict == V_BUSY);
    CHECK(b.move(req(b, 1, 2, hide, 0, p1), 20).verdict == V_OK);
    CHECK(b.item(hide)->where == p1 && b.contents(corpse).empty() && conserved(b));
    CHECK(b.release(1, corpse) == V_OK);
    CHECK(b.reserve(2, corpse, 30, 5000) == V_OK);
    CHECK(b.move(req(b, 2, 2, hide, 0, p2), 40).verdict == V_FORBIDDEN);  // it is in player 1's inventory now
}

static void testReservationExpiry() {
    ItemBook b;
    ContainerId corpse = b.addContainer(CK_CORPSE, 0), p1 = b.addContainer(CK_CHARACTER, 1), p2 = b.addContainer(CK_CHARACTER, 2);
    ItemId hide = b.addItem(corpse, "hide", 1, 100.f, 0, Cell());
    CHECK(b.reserve(1, corpse, 0, 1000) == V_OK);
    CHECK(b.reserve(2, corpse, 1000, 1000) == V_OK);       // the first player's hold ran out
    CHECK(b.move(req(b, 1, 1, hide, 0, p1), 1001).verdict == V_BUSY);
    CHECK(b.reserve(1, corpse, 1500, 1000) == V_BUSY);
    b.releaseAll(2);                                       // player 2 disconnected
    CHECK(b.reserve(1, corpse, 1600, 1000) == V_OK);
    (void)p2;
}

// A request sent before the squad changed hands must not land after it.
static void testStaleEpoch() {
    ItemBook b;
    ContainerId a = b.addContainer(CK_CHARACTER, 1), c = b.addContainer(CK_CHARACTER, 1);
    ItemId i = b.addItem(a, "sword", 1, 100.f, 0, Cell());
    MoveRequest r = req(b, 1, 1, i, 0, c);
    CHECK(b.setOwner(a, 2) == V_OK);
    CHECK(b.move(r, 0).verdict == V_STALE_EPOCH);
    CHECK(b.item(i)->where == a);
    CHECK(b.move(req(b, 1, 2, i, 0, c), 0).verdict == V_FORBIDDEN);
    CHECK(b.move(req(b, 2, 1, i, 0, a, Cell(0, 1, 1)), 0).verdict == V_OK);
}

// A resent request does the work once and repeats the first answer, whatever happened since.
static void testIdempotent() {
    ItemBook b;
    ContainerId a = b.addContainer(CK_CHARACTER, 1), c = b.addContainer(CK_CHARACTER, 1);
    ItemId i = b.addItem(a, "meat", 5, 100.f, 0, Cell());
    MoveRequest r = req(b, 1, 7, i, 2, c);
    MoveResult f = b.move(r, 0);
    MoveResult g = b.move(r, 0);
    CHECK(f.verdict == V_OK && g.verdict == V_OK && g.repeated && g.moved == f.moved);
    CHECK(b.item(i)->qty == 3 && b.item(f.moved)->qty == 2 && conserved(b));
}

// The retry memory stays bounded and still answers recent retries.
static void testRememberBound() {
    ItemBook b;
    ContainerId a = b.addContainer(CK_CHARACTER, 1);
    ItemId i = b.addItem(a, "meat", 5, 100.f, 0, Cell());
    for (u32 t = 1; t <= 5000; ++t) b.move(req(b, 1, t, i, 0, a, Cell(0, (short)(t % 100), 0)), 0);
    CHECK(b.rememberedTxns() == 4096);
    MoveResult r = b.move(req(b, 1, 5000, i, 0, a, Cell(0, 99, 99)), 0);
    CHECK(r.repeated && b.item(i)->cell.y == 0);
}

// Shadow mode: what the engine already did is recorded without checks and stays conserved.
static void testRecord() {
    ItemBook b;
    ContainerId a = b.addContainer(CK_CORPSE, 0), c = b.addContainer(CK_CHARACTER, 2);
    ItemId i = b.addItem(a, "hide", 3, 100.f, 0, Cell());
    CHECK(b.recordMove(i, c, Cell(0, 4, 4)));
    CHECK(b.item(i)->where == c && b.item(i)->cell == Cell(0, 4, 4) && b.contents(a).empty());
    CHECK(b.recordQty(i, 5) && b.created("hide") == 5 && conserved(b));
    CHECK(b.recordQty(i, 2) && b.destroyed("hide") == 3 && conserved(b));
    CHECK(!b.recordQty(i, 0));
    CHECK(!b.recordMove(999, c, Cell()));
    std::vector<Delta> d;
    b.drainDeltas(d);
    ItemView v;
    for (size_t k = 0; k < d.size(); ++k) CHECK(v.apply(d[k]));
    CHECK(v.hashContainer(c) == b.hashContainer(c) && v.hashContainer(a) == b.hashContainer(a));
}

static void testDestroyLogged() {
    ItemBook b;
    ContainerId a = b.addContainer(CK_CHARACTER, 1);
    ItemId i = b.addItem(a, "meat", 5, 100.f, 0, Cell());
    CHECK(b.destroyItem(i, 2) && b.item(i)->qty == 3 && b.destroyed("meat") == 2 && conserved(b));
    CHECK(!b.destroyItem(i, 9));
    CHECK(b.destroyItem(i, 3) && b.item(i) == 0 && conserved(b));
}

// Views follow the book, a lost delta is detected, a snapshot repairs it.
static void testGapRecovery() {
    ItemBook b;
    ItemView v;
    ContainerId a = b.addContainer(CK_CHARACTER, 1), c = b.addContainer(CK_CHARACTER, 1);
    ItemId i = b.addItem(a, "meat", 5, 100.f, 0, Cell());
    std::vector<Delta> d;
    b.drainDeltas(d);
    CHECK(v.apply(d[0]));
    CHECK(v.hashContainer(a) == b.hashContainer(a));
    d.clear();
    b.move(req(b, 1, 1, i, 2, c), 0);
    b.move(req(b, 1, 2, i, 1, c, Cell(0, 3, 3)), 0);
    b.drainDeltas(d);
    size_t gaps = 0;
    for (size_t k = 1; k < d.size(); ++k) if (!v.apply(d[k])) ++gaps;   // d[0] lost
    CHECK(gaps > 0);
    CHECK(v.hashContainer(a) != b.hashContainer(a));
    repair(b, v, a);
    repair(b, v, c);
    CHECK(v.hashContainer(a) == b.hashContainer(a) && v.hashContainer(c) == b.hashContainer(c));
    (void)i;
}

// N players, random moves, reservations, owner changes, repeated and lost packets.
// After repair every view equals the book and nothing appeared or vanished.
struct Lcg { unsigned long s; Lcg(unsigned long x) : s(x) {} u32 next(u32 n) { s = s * 1103515245UL + 12345UL; return (u32)((s >> 8) % n); } };

struct Late { MoveRequest q; int due; };

static void fuzz(unsigned long seed, int players, int containersEach, int items, int steps, double lossRate) {
    Lcg r(seed);
    ItemBook b;
    std::vector<ContainerId> all;
    std::vector<std::vector<ContainerId> > owned(players + 1);
    for (int p = 1; p <= players; ++p)
        for (int c = 0; c < containersEach; ++c) { ContainerId id = b.addContainer(CK_CHARACTER, p); all.push_back(id); owned[p].push_back(id); }
    std::vector<ContainerId> corpses, stores;
    for (int c = 0; c < players * 3; ++c) { ContainerId id = b.addContainer(CK_CORPSE, 0); all.push_back(id); corpses.push_back(id); }
    for (int c = 0; c < players * 2; ++c) { ContainerId id = b.addContainer(CK_STORAGE, 1 + c % players); all.push_back(id); stores.push_back(id); }
    const char* sids[] = { "hide", "meat", "sword", "boots", "iron", "plate" };
    std::vector<ItemId> ids;
    for (int i = 0; i < items; ++i)
        ids.push_back(b.addItem(all[r.next((u32)all.size())], sids[r.next(6)], 1 + r.next(20), 100.f, 0, Cell(0, (short)r.next(40), (short)r.next(40))));

    std::vector<ItemView> views(players + 1);
    std::vector<Delta> d;
    b.drainDeltas(d);
    for (int p = 1; p <= players; ++p) for (size_t k = 0; k < d.size(); ++k) views[p].apply(d[k]);
    d.clear();

    std::vector<Late> late;
    std::vector<u32> nextTxn(players + 1, 1);
    std::map<Verdict, int> verdicts;
    long long gaps = 0, lost = 0, repairs = 0, sent = 0;
    unsigned long now = 0;
    for (int s = 0; s < steps; ++s) {
        now += 5;
        const PlayerId p = 1 + r.next(players);
        const u32 what = r.next(100);
        if (what < 8) {
            b.reserve(p, corpses[r.next((u32)corpses.size())], now, 2000);
        } else if (what < 12) {
            b.reserve(p, stores[r.next((u32)stores.size())], now, 2000);
        } else if (what < 14) {
            b.releaseAll(p);
        } else if (what < 15) {
            b.setOwner(owned[p][r.next((u32)owned[p].size())], 1 + r.next(players));
        } else if (what < 16) {
            b.tick(now);
        } else if (what < 18) {
            ItemId id = ids[r.next((u32)ids.size())];
            const ItemRec* i = b.item(id);
            if (i) b.destroyItem(id, 1);
        } else {
            // A real player acts on what they can see: mostly their own items or a corpse they hold.
            ItemId id = ids[r.next((u32)ids.size())];
            const ItemRec* i = b.item(id);
            if (i) {
                const ContainerRec* src = b.container(i->where);
                PlayerId actor = p;
                if (r.next(100) < 75 && src->owner != 0) actor = src->owner;
                if (src->kind == CK_CORPSE && r.next(100) < 70) b.reserve(actor, i->where, now, 2000);
                ContainerId dst = r.next(100) < 70 ? owned[actor][r.next((u32)owned[actor].size())] : all[r.next((u32)all.size())];
                const ContainerRec* dc = b.container(dst);
                if (dc->owner != 0 && dc->owner != actor && r.next(100) < 80) dst = owned[actor][0];
                u32 q = r.next(3) == 0 ? 0 : 1 + r.next(i->qty);
                Late l;
                l.q = req(b, actor, nextTxn[actor]++, id, q, dst, Cell(0, (short)r.next(40), (short)r.next(40)));
                l.due = s + (int)r.next(40);     // network delay: the world can change before it lands
                late.push_back(l);
            }
        }
        for (size_t k = 0; k < late.size();) {
            if (late[k].due > s) { ++k; continue; }
            MoveResult first = b.move(late[k].q, now);
            verdicts[first.verdict]++;
            if (first.verdict == V_OK && first.moved != late[k].q.item) ids.push_back(first.moved);
            if (r.next(10) == 0) {                     // the client resent it
                MoveResult again = b.move(late[k].q, now);
                CHECK(again.repeated && again.verdict == first.verdict && again.moved == first.moved);
            }
            late[k] = late.back();
            late.pop_back();
        }
        b.drainDeltas(d);
        for (size_t k = 0; k < d.size(); ++k) {
            ++sent;
            for (int v = 1; v <= players; ++v) {
                if (lossRate > 0 && (double)r.next(100000) / 100000.0 < lossRate) { ++lost; continue; }
                if (!views[v].apply(d[k])) ++gaps;
            }
        }
        d.clear();
        if (s % 500 == 499) {                              // the live hash check and repair
            for (int v = 1; v <= players; ++v)
                for (size_t c = 0; c < all.size(); ++c)
                    if (views[v].hashContainer(all[c]) != b.hashContainer(all[c])) {
                        repair(b, views[v], all[c]);
                        ++repairs;
                    }
        }
    }
    // Final repair pass, then everything must agree.
    for (int v = 1; v <= players; ++v)
        for (size_t c = 0; c < all.size(); ++c)
            if (views[v].hashContainer(all[c]) != b.hashContainer(all[c])) { repair(b, views[v], all[c]); ++repairs; }
    int bad = 0;
    for (int v = 1; v <= players; ++v)
        for (size_t c = 0; c < all.size(); ++c)
            if (views[v].hashContainer(all[c]) != b.hashContainer(all[c])) ++bad;
    CHECK(bad == 0);
    CHECK(conserved(b));
    printf("  fuzz seed=%lu players=%d steps=%d loss=%.3f: items=%u deltas=%lld lost=%lld gaps=%lld repairs=%lld mismatch_after_repair=%d OK=%d BUSY=%d NOT_RES=%d FORBID=%d STALE=%d CELL=%d NOITEM=%d QTY=%d\n",
        seed, players, steps, lossRate, (unsigned)b.itemCount(), sent, lost, gaps, repairs, bad,
        verdicts[V_OK], verdicts[V_BUSY], verdicts[V_NOT_RESERVED], verdicts[V_FORBIDDEN], verdicts[V_STALE_EPOCH], verdicts[V_CELL_TAKEN], verdicts[V_NO_ITEM], verdicts[V_BAD_QTY]);
}

// Real save contents: exported by scripts/core2_export_items.py.
static void realData(const char* path) {
    std::ifstream f(path);
    if (!f) { printf("cannot open %s\n", path); ++g_fail; return; }
    ItemBook b;
    std::map<std::string, ContainerId> containers;
    std::string line;
    long lines = 0;
    size_t sidBytes = 0;
    clock_t t0 = clock();
    while (std::getline(f, line)) {
        // key \t sid \t qty \t quality \t level \t section \t x \t y
        std::vector<std::string> p;
        size_t at = 0;
        for (;;) { size_t tab = line.find('\t', at); if (tab == std::string::npos) { p.push_back(line.substr(at)); break; } p.push_back(line.substr(at, tab - at)); at = tab + 1; }
        if (p.size() < 8) continue;
        ContainerId& c = containers[p[0]];
        if (c == 0) c = b.addContainer(p[0][0] == 'C' ? CK_CHARACTER : CK_STORAGE, 0);
        b.addItem(c, p[1], (u32)atoi(p[2].c_str()) > 0 ? (u32)atoi(p[2].c_str()) : 1, (float)atof(p[3].c_str()), atoi(p[4].c_str()), Cell((short)atoi(p[5].c_str()), (short)atoi(p[6].c_str()), (short)atoi(p[7].c_str())));
        sidBytes += p[1].size();
        ++lines;
    }
    double load = (double)(clock() - t0) / CLOCKS_PER_SEC;
    std::vector<Delta> d;
    b.drainDeltas(d);
    // Wire estimate: id, container, rev, qty, quality, level, cell (28 bytes) + sid with a length byte.
    double baseline = (double)d.size() * 29 + (double)sidBytes;
    t0 = clock();
    u32 sum = 0;
    for (std::map<std::string, ContainerId>::iterator i = containers.begin(); i != containers.end(); ++i) sum += b.hashContainer(i->second);
    double hashAll = (double)(clock() - t0) / CLOCKS_PER_SEC;
    // Moves through the whole book (a worst-case busy base).
    std::vector<ContainerId> ids;
    for (std::map<std::string, ContainerId>::iterator i = containers.begin(); i != containers.end(); ++i) ids.push_back(i->second);
    Lcg r(99);
    t0 = clock();
    int ok = 0, n = 200000;
    for (int k = 0; k < n; ++k) {
        ItemId id = 1 + r.next((u32)b.itemCount());
        const ItemRec* it = b.item(id);
        if (!it) continue;
        MoveRequest q = req(b, 1, (TxnId)(k + 1), id, 0, ids[r.next((u32)ids.size())], Cell(1, (short)r.next(30000), (short)r.next(30000)));
        if (b.move(q, 0).verdict == V_OK) ++ok;
    }
    double moves = (double)(clock() - t0) / CLOCKS_PER_SEC;
    CHECK(conserved(b));
    printf("  real %s: items=%ld containers=%u load=%.3fs hashAll=%.4fs baseline~%.0f KB %d moves ok=%d in %.3fs (%.0f per s) sum=%u\n",
        path, lines, (unsigned)containers.size(), load, hashAll, baseline / 1024.0, n, ok, moves, moves > 0 ? ok / moves : 0.0, sum);
}

int main(int argc, char** argv) {
    testBasics();
    testMoveDeltas();
    testCellTaken();
    testCorpseRace();
    testReservationExpiry();
    testStaleEpoch();
    testIdempotent();
    testRecord();
    testRememberBound();
    testDestroyLogged();
    testGapRecovery();
    for (unsigned long seed = 1; seed <= 5; ++seed) fuzz(seed, 2, 4, 300, 6000, 0.0);
    for (unsigned long seed = 11; seed <= 15; ++seed) fuzz(seed, 8, 8, 2000, 12000, 0.02);
    for (int i = 1; i < argc; ++i) realData(argv[i]);
    printf("%s pass=%d fail=%d\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
