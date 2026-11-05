// session_test - the core2 protocol over a hostile fake network: loss, repeats, reordering,
// reconnects, damaged messages. After the network settles every view must equal the book and
// the book must conserve every template.
//   session_test [items.tsv]   the file adds a base-scale traffic measure on real save contents

#include "../Session.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace core2;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Lcg { unsigned long s; Lcg(unsigned long x) : s(x) {} u32 next(u32 n) { s = s * 1103515245UL + 12345UL; return (u32)((s >> 8) % n); } };

struct Packet { unsigned long due; PlayerId from, to; Bytes data; };

// The whole network. Sender 0 is the host.
class FakeNet {
public:
    FakeNet(Lcg& r, u32 lossPct, u32 dupPct, u32 maxDelay) : r_(r), loss(lossPct), dup(dupPct), delay(maxDelay), now(0), bytes(0), biggest(0) {}
    void enqueue(PlayerId from, PlayerId to, const Bytes& m) {
        bytes += m.size();
        if (m.size() > biggest) biggest = m.size();
        if (r_.next(100) < loss) return;
        Packet p; p.from = from; p.to = to; p.data = m; p.due = now + 5 + r_.next(delay);
        q.push_back(p);
        if (r_.next(100) < dup) { p.due = now + 5 + r_.next(delay); q.push_back(p); }
    }
    std::vector<Packet> q;
    Lcg& r_;
    u32 loss, dup, delay;
    unsigned long now;
    unsigned long long bytes;
    size_t biggest;
};

class HostLink : public Transport {
public:
    explicit HostLink(FakeNet& n) : net(n) {}
    void send(PlayerId to, const Bytes& m) { net.enqueue(0, to, m); }
    FakeNet& net;
};

class ClientLink : public Transport {
public:
    ClientLink(FakeNet& n, PlayerId me) : net(n), self(me) {}
    void send(PlayerId, const Bytes& m) { net.enqueue(self, 0, m); }
    FakeNet& net;
    PlayerId self;
};

struct World {
    ItemBook book;
    FakeNet net;
    HostLink hostLink;
    HostSession host;
    std::vector<ClientLink*> links;
    std::vector<ClientSession*> clients;   // index 0 unused
    std::vector<bool> online;
    World(Lcg& r, int players, u32 loss, u32 dup, u32 delay)
        : net(r, loss, dup, delay), hostLink(net), host(book, hostLink), links(players + 1), clients(players + 1), online(players + 1, false) {
        for (int p = 1; p <= players; ++p) {
            links[p] = new ClientLink(net, p);
            clients[p] = new ClientSession(p, *links[p]);
        }
    }
    ~World() {
        for (size_t p = 1; p < clients.size(); ++p) { delete clients[p]; delete links[p]; }
    }
    void join(int p) { online[p] = true; host.addPlayer(p); clients[p]->connect(); }
    void leave(int p) { online[p] = false; host.removePlayer(p); }
    void pump() {
        for (size_t i = 0; i < net.q.size();) {
            if (net.q[i].due > net.now) { ++i; continue; }
            Packet p = net.q[i];
            net.q[i] = net.q.back();
            net.q.pop_back();
            if (p.to == 0) {
                if (p.from != 0 && online[p.from]) host.receive(p.from, p.data, net.now);
            } else if (online[p.to]) {
                clients[p.to]->receive(p.data, net.now);
            }
        }
    }
    void step(unsigned long ms) {
        net.now += ms;
        pump();
        host.tick(net.now);
        for (size_t p = 1; p < clients.size(); ++p) if (online[p]) clients[p]->tick(net.now);
    }
};

static bool conserved(const ItemBook& b) {
    std::map<std::string, long long> t = b.totals();
    for (std::map<std::string, long long>::const_iterator i = t.begin(); i != t.end(); ++i)
        if (i->second != b.created(i->first) - b.destroyed(i->first)) return false;
    return true;
}

static int mismatches(World& w, const std::vector<ContainerId>& all, int players) {
    int bad = 0;
    for (int p = 1; p <= players; ++p) {
        if (!w.online[p]) continue;
        for (size_t c = 0; c < all.size(); ++c)
            if (w.clients[p]->view().hashContainer(all[c]) != w.book.hashContainer(all[c])) ++bad;
    }
    return bad;
}

static void hostile(unsigned long seed, int players, u32 loss, u32 dup, u32 delay, int steps, bool churn) {
    Lcg r(seed);
    World w(r, players, loss, dup, delay);
    std::vector<ContainerId> all, corpses;
    std::vector<std::vector<ContainerId> > owned(players + 1);
    for (int p = 1; p <= players; ++p)
        for (int c = 0; c < 3; ++c) { ContainerId id = w.book.addContainer(CK_CHARACTER, p); all.push_back(id); owned[p].push_back(id); }
    for (int c = 0; c < players * 2; ++c) { ContainerId id = w.book.addContainer(CK_CORPSE, 0); all.push_back(id); corpses.push_back(id); }
    for (int c = 0; c < players; ++c) { ContainerId id = w.book.addContainer(CK_STORAGE, 1 + c % players); all.push_back(id); }
    const char* sids[] = { "hide", "meat", "sword", "boots", "iron", "plate" };
    for (int i = 0; i < 40 * players; ++i)
        w.book.addItem(all[r.next((u32)all.size())], sids[r.next(6)], 1 + r.next(20), 100.f, 0, Cell(0, (short)r.next(30), (short)r.next(30)));
    for (int p = 1; p <= players; ++p) w.join(p);
    for (int s = 0; s < 400; ++s) w.step(20);        // connect through the bad network

    std::map<Verdict, int> verdicts;
    int rejoins = 0, garbage = 0;
    for (int s = 0; s < steps; ++s) {
        w.step(20);
        int p = 1 + (int)r.next(players);
        if (!w.online[p]) { if (r.next(100) < 3) { w.join(p); ++rejoins; } continue; }
        if (churn && r.next(1000) < 2) { w.leave(p); continue; }
        ClientSession& c = *w.clients[p];
        u32 what = r.next(100);
        if (what < 10) {
            c.requestReserve(corpses[r.next((u32)corpses.size())], w.net.now);
        } else if (what < 13) {
            c.requestRelease(corpses[r.next((u32)corpses.size())]);
        } else if (what < 15) {
            w.host.setOwner(owned[p][r.next((u32)owned[p].size())], 1 + r.next(players));
        } else if (what < 16) {                      // damaged traffic must be refused, never crash
            Bytes junk(1 + r.next(40));
            for (size_t i = 0; i < junk.size(); ++i) junk[i] = (unsigned char)r.next(256);
            w.net.enqueue((PlayerId)p, 0, junk);
            w.net.enqueue(0, (PlayerId)p, junk);
            ++garbage;
        } else if (what < 18) {
            ItemId id = 1 + r.next((u32)w.book.itemCount() + 50);
            if (w.book.item(id)) w.book.destroyItem(id, 1);
        } else {
            std::vector<ItemRec> seen = w.book.contents(all[r.next((u32)all.size())]);
            if (seen.empty()) continue;
            const ItemRec& it = seen[r.next((u32)seen.size())];
            ContainerId dst = r.next(100) < 70 ? owned[p][r.next((u32)owned[p].size())] : all[r.next((u32)all.size())];
            c.requestMove(it.id, r.next(3) == 0 ? 0 : 1 + r.next(it.qty), dst, Cell(0, (short)r.next(30), (short)r.next(30)), w.net.now);
        }
        std::vector<LocalResult> res;
        c.drainResults(res);
        for (size_t i = 0; i < res.size(); ++i) verdicts[res[i].verdict]++;
    }
    for (int p = 1; p <= players; ++p) if (!w.online[p]) w.join(p);
    // The network settles: the same loss goes on, no new actions. Everyone must converge by itself.
    unsigned long settleMs = 0;
    int bad = -1;
    w.host.setHashWindow(512);
    while (settleMs < 120000) {
        w.step(20);
        settleMs += 20;
        if (settleMs % 1000 == 0) {
            bad = mismatches(w, all, players);
            size_t pend = 0;
            for (int p = 1; p <= players; ++p) pend += w.clients[p]->pending();
            if (bad == 0 && pend == 0) break;
        }
    }
    size_t pend = 0;
    unsigned long long gaps = 0, snaps = w.host.stats().snapshots, retries = 0, hm = 0, badc = 0;
    for (int p = 1; p <= players; ++p) {
        pend += w.clients[p]->pending();
        gaps += w.clients[p]->stats().gaps;
        retries += w.clients[p]->stats().retries;
        hm += w.clients[p]->stats().hashMismatch;
        badc += w.clients[p]->stats().badMsgs;
    }
    CHECK(bad == 0);
    CHECK(pend == 0);
    CHECK(conserved(w.book));
    CHECK(verdicts[V_OK] > 0);
    printf("  hostile seed=%lu players=%d loss=%u%% dup=%u%% delay<=%ums churn=%d: settle=%lums mismatch=%d pending=%u OK=%d BUSY=%d FORBID=%d STALE=%d NOT_RES=%d CELL=%d NOITEM=%d gaps=%llu snaps=%llu retries=%llu hashFix=%llu rejoins=%d junk=%d refused=%llu/%llu\n",
        seed, players, loss, dup, delay, churn ? 1 : 0, settleMs, bad, (unsigned)pend, verdicts[V_OK], verdicts[V_BUSY], verdicts[V_FORBIDDEN], verdicts[V_STALE_EPOCH], verdicts[V_NOT_RESERVED], verdicts[V_CELL_TAKEN], verdicts[V_NO_ITEM],
        gaps, snaps, retries, hm, rejoins, garbage, w.host.stats().badMsgs, badc);
}

// A worker-heavy base on real save contents: how much traffic does the book cost?
static void baseTraffic(const char* path, int players, int movesPerSecond, int seconds) {
    std::ifstream f(path);
    if (!f) { printf("cannot open %s\n", path); ++g_fail; return; }
    Lcg r(7);
    World w(r, players, 0, 0, 40);
    std::map<std::string, ContainerId> cs;
    std::vector<ContainerId> all;
    std::vector<ItemId> ids;
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> p;
        size_t at = 0;
        for (;;) { size_t t = line.find('\t', at); if (t == std::string::npos) { p.push_back(line.substr(at)); break; } p.push_back(line.substr(at, t - at)); at = t + 1; }
        if (p.size() < 8) continue;
        ContainerId& c = cs[p[0]];
        if (c == 0) { c = w.book.addContainer(CK_GROUND, 0); all.push_back(c); }
        int q = atoi(p[2].c_str());
        ids.push_back(w.book.addItem(c, p[1], q > 0 ? (u32)q : 1, (float)atof(p[3].c_str()), atoi(p[4].c_str()), Cell((short)atoi(p[5].c_str()), (short)atoi(p[6].c_str()), (short)atoi(p[7].c_str()))));
    }
    for (int p = 1; p <= players; ++p) w.join(p);
    unsigned long long before = w.net.bytes;
    int guard = 0;
    bool ready = false;
    while (!ready && guard++ < 5000) {
        w.step(20);
        ready = true;
        for (int p = 1; p <= players; ++p) if (!w.clients[p]->ready()) ready = false;
    }
    for (int s = 0; s < 200; ++s) w.step(20);
    unsigned long long joinBytes = w.net.bytes - before;
    CHECK(mismatches(w, all, players) == 0);

    before = w.net.bytes;
    w.host.setHashWindow(16);
    const int tickMs = 50;
    int total = seconds * 1000 / tickMs, moved = 0;
    for (int s = 0; s < total; ++s) {
        w.step(tickMs);
        for (int m = 0; m < movesPerSecond * tickMs / 1000 + (s % (1000 / tickMs) < movesPerSecond % (1000 / tickMs) ? 1 : 0); ++m) {
            ItemId id = ids[r.next((u32)ids.size())];
            const ItemRec* it = w.book.item(id);
            if (!it) continue;
            w.clients[1 + r.next(players)]->requestMove(id, 0, all[r.next((u32)all.size())], Cell(1, (short)r.next(30000), (short)r.next(30000)), w.net.now);
            ++moved;
        }
    }
    unsigned long long runBytes = w.net.bytes - before;
    for (int s = 0; s < 300; ++s) w.step(20);
    CHECK(mismatches(w, all, players) == 0);
    CHECK(conserved(w.book));
    printf("  base %s players=%d: items=%u containers=%u join=%.0f KB; %d moves/s for %ds (%d sent): %.1f KB/s total all links, biggest message %u B\n",
        path, players, (unsigned)ids.size(), (unsigned)all.size(), joinBytes / 1024.0 / players, movesPerSecond, seconds, moved, runBytes / 1024.0 / seconds, (unsigned)w.net.biggest);
}

// A player restarts the game: a brand-new client must not collide with its old request ids.
static void testRestartedClient() {
    Lcg r(5);
    World w(r, 1, 0, 0, 10);
    ContainerId a = w.book.addContainer(CK_CHARACTER, 1), c = w.book.addContainer(CK_CHARACTER, 1);
    ItemId i = w.book.addItem(a, "meat", 5, 100.f, 0, Cell());
    w.join(1);
    for (int s = 0; s < 50; ++s) w.step(20);
    TxnId t1 = w.clients[1]->requestMove(i, 1, c, Cell(0, 1, 1), w.net.now);
    for (int s = 0; s < 50; ++s) w.step(20);
    CHECK(w.book.item(i)->qty == 4);
    w.leave(1);
    delete w.clients[1];
    w.clients[1] = new ClientSession(1, *w.links[1]);   // the old process is gone
    w.join(1);
    for (int s = 0; s < 50; ++s) w.step(20);
    TxnId t2 = w.clients[1]->requestMove(i, 1, c, Cell(0, 2, 2), w.net.now);
    for (int s = 0; s < 50; ++s) w.step(20);
    CHECK(t2 != t1);
    CHECK(w.book.item(i)->qty == 3);
    CHECK(conserved(w.book));
}

// The hash window finds a view damaged without any gap (a dropped apply, a bad patch).
static void testDamagedViewHeals() {
    Lcg r(6);
    World w(r, 1, 0, 0, 10);
    ContainerId a = w.book.addContainer(CK_CHARACTER, 1);
    w.book.addItem(a, "meat", 5, 100.f, 0, Cell());
    w.join(1);
    for (int s = 0; s < 50; ++s) w.step(20);
    CHECK(w.clients[1]->view().hashContainer(a) == w.book.hashContainer(a));
    std::vector<Delta> none;
    const_cast<ItemView&>(w.clients[1]->view()).applySnapshot(a, w.book.container(a)->rev, none);
    CHECK(w.clients[1]->view().hashContainer(a) != w.book.hashContainer(a));
    for (int s = 0; s < 100; ++s) w.step(20);
    CHECK(w.clients[1]->view().hashContainer(a) == w.book.hashContainer(a));
    CHECK(w.clients[1]->stats().hashMismatch >= 1);
}

// Containers born during play reach views with their label, and labels change hands correctly.
static void testNewContainerMeta() {
    Lcg r(8);
    World w(r, 1, 0, 0, 10);
    ContainerId a = w.book.addContainer(CK_CHARACTER, 1);
    w.host.setLabel(a, "c.100");
    w.join(1);
    for (int s = 0; s < 50; ++s) w.step(20);
    CHECK(w.clients[1]->meta(a) && w.clients[1]->meta(a)->label == "c.100");
    ContainerId b = w.book.addContainer(CK_STORAGE, 1);
    w.host.setLabel(b, "b.200");
    ItemId i = w.book.addItem(b, "iron", 2, 100.f, 0, Cell(0, 1, 1));
    for (int s = 0; s < 50; ++s) w.step(20);
    CHECK(w.clients[1]->meta(b) && w.clients[1]->meta(b)->label == "b.200" && w.clients[1]->item(i) != 0);
    CHECK(w.clients[1]->view().contentHashContainer(b) == contentHash("iron", 2, Cell(0, 1, 1)));
}

int main(int argc, char** argv) {
    testNewContainerMeta();
    testRestartedClient();
    testDamagedViewHeals();
    for (unsigned long s = 1; s <= 4; ++s) hostile(s, 2, 0, 0, 50, 4000, false);
    for (unsigned long s = 11; s <= 14; ++s) hostile(s, 2, 5, 2, 300, 4000, false);
    for (unsigned long s = 21; s <= 24; ++s) hostile(s, 4, 10, 5, 500, 4000, true);
    for (unsigned long s = 31; s <= 33; ++s) hostile(s, 8, 20, 5, 800, 4000, true);
    for (int i = 1; i < argc; ++i) { baseTraffic(argv[i], 2, 65, 60); baseTraffic(argv[i], 8, 65, 60); }
    printf("%s pass=%d fail=%d\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
