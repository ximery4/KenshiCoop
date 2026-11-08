// Core2Shadow.cpp - see Core2Shadow.h.
//
// Every scan reads all loaded characters and owned buildings, compares the items with the book by Item*,
// and records each difference in the book: moved, split or merged, created, destroyed. The log answers
// the stage 1 and stage 3 questions on the bench:
//   [c2] HASH     per object content hash, to compare host and join at the same moment
//   [c2] MOVE/NEW/GONE/QTY   what the game did by itself, with the template
//   [c2] SCAN     cost of a scan and running totals; the book must stay equal to what was seen
//   [c2] PATH     call sites that went through the hooked item primitives
//   [c2net] CHECK with KENSHICOOP_CORE2_NET=1: the host's book reaches the join over the real link, and
//                 the join compares that copy with its own game. Still nothing in the game changes.

#define _CRT_SECURE_NO_WARNINGS 1

#include "Core2Shadow.h"

#include "../../core2/ItemBook.h"
#include "../../core2/Session.h"
#include "../CoopLog.h"
#include "../game/Engine.h"
#include "../net/NetLink.h"
#include "../../netproto/Wire.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace coop {
namespace core2shadow {

namespace {

using core2::ItemBook;
using core2::ItemId;
using core2::ContainerId;
using core2::Cell;
using engine::C2Row;

const unsigned int MAX_CHARS = 1024;
const unsigned int MAX_BUILDINGS = 512;
const unsigned int MAX_ROWS = 512;
const float BUILD_RADIUS = 3000.f;
const unsigned long CHECK_MS = 10000;

struct Seen {
    ItemId id;
    ContainerId where;
    unsigned int qty;
    Cell cell;
};

// A new Item* this scan. Paired with a vanished one of the same template and quantity: the engine
// often moves an item by making a copy, and that is a move, not a birth and a death.
struct Fresh {
    void* item;
    ContainerId where;
    char sid[48];
    unsigned int qty;
    float quality;
    Cell cell;
    int nest;
};

class Link : public core2::Transport {
public:
    Link() : net(0), isHost(false) {}
    void send(core2::PlayerId to, const core2::Bytes& msg) {
        if (!net) return;
        net->queueCore2(msg, (isHost && to != 0) ? peerBit(to) : PEER_MASK_ALL);
    }
    NetLink* net;
    bool isHost;
};

struct State {
    ItemBook book;
    std::map<std::string, ContainerId> containers;
    std::map<void*, Seen> items;
    std::map<std::string, unsigned int> content;   // key -> content hash in the last scan
    std::map<std::string, std::vector<std::pair<std::string, core2::Cell> > > rows;   // key -> sid:qty and place, join only
    unsigned long nextScanMs, nextHashMs, nextPathMs, nextCheckMs, nextHelloMs;
    unsigned long scans, moves, created, destroyed, qtyChanges, cellChanges, logged, pairedCopies;
    unsigned long intervalMs;
    unsigned long collisionsLogged;
    bool started;
    Link link;
    core2::HostSession* host;
    core2::ClientSession* client;
    State() : nextScanMs(0), nextHashMs(0), nextPathMs(0), nextCheckMs(0), nextHelloMs(0), scans(0), moves(0),
              created(0), destroyed(0), qtyChanges(0), cellChanges(0), logged(0), pairedCopies(0), intervalMs(2000),
              collisionsLogged(0), started(false), host(0), client(0) {}
};

State* g_s = 0;
Character* g_chars[MAX_CHARS];
EntityState g_states[MAX_CHARS];
engine::DeedRead g_builds[MAX_BUILDINGS];
C2Row g_rows[MAX_ROWS];

bool netEnabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("KENSHICOOP_CORE2_NET");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v == 1;
}

void logf(const char* fmt, const char* a, unsigned int b, unsigned int c, unsigned int d) {
    char line[320];
    _snprintf(line, sizeof(line) - 1, fmt, a, b, c, d);
    line[sizeof(line) - 1] = '\0';
    coop::logLine(line);
}

// Differences can come in thousands on a load; a budget keeps the log readable, totals stay exact.
bool budget(State& s) { return ++s.logged <= 4000; }

void scanObject(State& s, RootObject* ro, const std::string& key, bool hashNow, std::map<void*, bool>& alive,
                std::vector<Fresh>& fresh) {
    const unsigned int n = engine::readC2Rows(ro, g_rows, MAX_ROWS);
    if (n == 0 && s.containers.find(key) == s.containers.end()) return;
    ContainerId& cref = s.containers[key];
    if (cref == 0) {
        cref = s.book.addContainer(core2::CK_GROUND, 0);
        if (s.host) s.host->setLabel(cref, key);
    }
    const ContainerId c = cref;
    unsigned int sum = 0;
    for (unsigned int i = 0; i < n; ++i) {
        const C2Row& r = g_rows[i];
        const Cell cell(r.nest == 0 ? (short)r.section : (short)(r.section ^ (r.nest << 12)), r.x, r.y);
        sum += core2::contentHash(r.sid, r.qty, cell);
        if (s.client) {
            char one[64];
            _snprintf(one, sizeof(one) - 1, "%s:%u", r.sid, r.qty);
            one[sizeof(one) - 1] = '\0';
            s.rows[key].push_back(std::make_pair(std::string(one), cell));
        }
        alive[r.item] = true;
        std::map<void*, Seen>::iterator f = s.items.find(r.item);
        if (f == s.items.end()) {
            Fresh nw;
            nw.item = r.item; nw.where = c; nw.qty = r.qty; nw.quality = r.quality; nw.cell = cell; nw.nest = r.nest;
            strncpy(nw.sid, r.sid, sizeof(nw.sid) - 1);
            nw.sid[sizeof(nw.sid) - 1] = '\0';
            fresh.push_back(nw);
            continue;
        }
        Seen& v = f->second;
        if (v.where != c) {
            s.book.recordMove(v.id, c, cell);
            if (budget(s)) {
                char line[220];
                _snprintf(line, sizeof(line) - 1, "[c2] MOVE sid='%s' qty=%u from=%u to=%u", r.sid, r.qty, v.where, c);
                line[sizeof(line) - 1] = '\0';
                coop::logLine(line);
            }
            ++s.moves;
            v.where = c; v.cell = cell;
        } else if (!(v.cell == cell)) {
            s.book.recordMove(v.id, c, cell);
            ++s.cellChanges;
            v.cell = cell;
        }
        if (v.qty != r.qty) {
            s.book.recordQty(v.id, r.qty);
            ++s.qtyChanges;
            if (budget(s)) logf("[c2] QTY sid='%s' was=%u now=%u in=%u", r.sid, v.qty, r.qty, c);
            v.qty = r.qty;
        }
    }
    s.content[key] = sum;
    if (hashNow && n > 0) {
        char line[200];
        _snprintf(line, sizeof(line) - 1, "[c2] HASH key=%s n=%u hash=%u", key.c_str(), n, sum);
        line[sizeof(line) - 1] = '\0';
        coop::logLine(line);
    }
}

// Settles this scan's new and vanished items after every object was read.
void settle(State& s, std::map<void*, bool>& alive, std::vector<Fresh>& fresh) {
    std::vector<void*> gone;
    for (std::map<void*, Seen>::iterator i = s.items.begin(); i != s.items.end(); ++i)
        if (alive.find(i->first) == alive.end()) gone.push_back(i->first);

    std::vector<bool> used(fresh.size(), false);
    for (size_t g = 0; g < gone.size(); ++g) {
        Seen v = s.items[gone[g]];
        const core2::ItemRec* rec = s.book.item(v.id);
        s.items.erase(gone[g]);
        if (!rec) continue;
        size_t match = fresh.size();
        for (size_t k = 0; k < fresh.size(); ++k) {
            if (!used[k] && fresh[k].qty == rec->qty && rec->sid == fresh[k].sid) { match = k; break; }
        }
        if (match < fresh.size()) {
            used[match] = true;
            const Fresh& nw = fresh[match];
            if (nw.where != v.where) {
                ++s.moves;
                if (budget(s)) {
                    char line[220];
                    _snprintf(line, sizeof(line) - 1, "[c2] MOVE sid='%s' qty=%u from=%u to=%u (copy)", nw.sid, nw.qty, v.where, nw.where);
                    line[sizeof(line) - 1] = '\0';
                    coop::logLine(line);
                }
            }
            ++s.pairedCopies;
            s.book.recordMove(v.id, nw.where, nw.cell);
            Seen again;
            again.id = v.id; again.where = nw.where; again.qty = nw.qty; again.cell = nw.cell;
            s.items[nw.item] = again;
        } else {
            if (budget(s)) logf("[c2] GONE sid='%s' qty=%u in=%u %u", rec->sid.c_str(), rec->qty, rec->where, 0);
            s.book.destroyItem(v.id, rec->qty);
            ++s.destroyed;
        }
    }
    for (size_t k = 0; k < fresh.size(); ++k) {
        if (used[k]) continue;
        const Fresh& nw = fresh[k];
        Seen v;
        v.id = s.book.addItem(nw.where, nw.sid, nw.qty, nw.quality, 0, nw.cell);
        v.where = nw.where; v.qty = nw.qty; v.cell = nw.cell;
        s.items[nw.item] = v;
        if (s.started) {
            ++s.created;
            if (budget(s)) logf("[c2] NEW sid='%s' qty=%u in=%u nest=%u", nw.sid, nw.qty, nw.where, (unsigned)nw.nest);
        }
    }
}

void scanAll(State& s, GameWorld* gw, unsigned long nowMs) {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    const bool hashNow = nowMs >= s.nextHashMs;
    if (hashNow) s.nextHashMs = nowMs + 30000;
    std::map<void*, bool> alive;
    std::vector<Fresh> fresh;
    s.content.clear();
    s.rows.clear();

    unsigned int objects = 0;
    unsigned int hd[5];
    const unsigned int np = engine::listPlayerChars(gw, g_chars, MAX_CHARS);
    bool trunc = false;
    const unsigned int nn = engine::listNpcsWide(gw, BUILD_RADIUS, g_chars + np, g_states, MAX_CHARS - np, &trunc);
    // FORK 2026-11-07 (review D1): the serial alone is not unique - a nomad
    // (hand 1,576171904) and a pet goat (hand 9,576171904) shared "c.576171904",
    // so two inventories merged into one book container. The plain serial key
    // stays (a squad move changes the index, not the serial), but a second
    // object hitting an already-used key this scan gets ".<index>" appended.
    // Player characters are listed first, so they always keep the plain key.
    std::map<std::string, void*> usedKey;
    for (unsigned int i = 0; i < np + nn; ++i) {
        RootObject* ro = reinterpret_cast<RootObject*>(g_chars[i]);
        if (!ro || !engine::readObjectHand(ro, hd)) continue;
        char key[48];
        _snprintf(key, sizeof(key) - 1, "c.%u", hd[4]);
        key[sizeof(key) - 1] = '\0';
        std::map<std::string, void*>::iterator uk = usedKey.find(key);
        if (uk != usedKey.end() && uk->second != (void*)ro) {
            _snprintf(key, sizeof(key) - 1, "c.%u.%u", hd[4], hd[3]);
            key[sizeof(key) - 1] = '\0';
            // block5g (PC B review 2.8): the same key was logged 20 times every 2 s; once per key now.
            static std::set<std::string> collidedKeys;
            if (s.collisionsLogged < 20 && collidedKeys.insert(key).second) {
                ++s.collisionsLogged;
                logf("[c2] KEY-COLLISION key=%s serial=%u index=%u type=%u", key, hd[4], hd[3], hd[0]);
            }
        }
        usedKey[key] = (void*)ro;
        scanObject(s, ro, key, hashNow, alive, fresh);
        ++objects;
    }
    const unsigned int nb = engine::enumBuildingsNear(gw, BUILD_RADIUS, g_builds, MAX_BUILDINGS);
    for (unsigned int i = 0; i < nb; ++i) {
        if (!g_builds[i].resolved) continue;
        RootObject* ro = engine::resolveObjectByHand(g_builds[i].hand);
        if (!ro) continue;
        char key[48];
        _snprintf(key, sizeof(key) - 1, "b.%u", g_builds[i].hand[4]);
        key[sizeof(key) - 1] = '\0';
        scanObject(s, ro, key, hashNow, alive, fresh);
        ++objects;
    }
    settle(s, alive, fresh);

    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    const double us = (double)(t1.QuadPart - t0.QuadPart) * 1000000.0 / (double)fq.QuadPart;
    ++s.scans;
    if (s.scans == 1 || s.scans % 15 == 0) {
        char line[280];
        _snprintf(line, sizeof(line) - 1,
            "[c2] SCAN n=%lu objects=%u items=%u containers=%u us=%.0f trunc=%d moved=%lu copies=%lu new=%lu gone=%lu qty=%lu cell=%lu",
            s.scans, objects, (unsigned)s.book.itemCount(), (unsigned)s.book.containerCount(), us, trunc ? 1 : 0,
            s.moves, s.pairedCopies, s.created, s.destroyed, s.qtyChanges, s.cellChanges);
        line[sizeof(line) - 1] = '\0';
        coop::logLine(line);
    }
    s.started = true;
}

// 04.11.2026: the two machines place the same things into stacks differently (1+3 here, 4 there), which the
// per-stack comparison counted as a content difference: 342 of 1181 "content" lines in the Kat session were only that.
// Totals per template tell the two apart.
static bool sameTotals(const std::multiset<std::string>& a, const std::multiset<std::string>& b) {
    std::map<std::string, long> sum;
    for (std::multiset<std::string>::const_iterator i = a.begin(); i != a.end(); ++i) {
        const size_t c = i->rfind(':');
        sum[i->substr(0, c)] += (c == std::string::npos) ? 1 : atol(i->c_str() + c + 1);
    }
    for (std::multiset<std::string>::const_iterator i = b.begin(); i != b.end(); ++i) {
        const size_t c = i->rfind(':');
        sum[i->substr(0, c)] -= (c == std::string::npos) ? 1 : atol(i->c_str() + c + 1);
    }
    for (std::map<std::string, long>::const_iterator i = sum.begin(); i != sum.end(); ++i)
        if (i->second != 0) return false;
    return true;
}

// Join: is the host's book, as received, the same as what this game shows?
void checkAgainstHost(State& s, unsigned long nowMs) {
    if (nowMs < s.nextCheckMs) return;
    s.nextCheckMs = nowMs + CHECK_MS;
    unsigned int same = 0, differ = 0, unknownHere = 0, shown = 0, layoutOnly = 0, stacksOnly = 0;
    const std::map<ContainerId, core2::ContainerMeta>& metas = s.client->metas();
    std::map<std::string, ContainerId> byLabel;
    for (std::map<ContainerId, core2::ContainerMeta>::const_iterator m = metas.begin(); m != metas.end(); ++m)
        byLabel[m->second.label] = m->first;
    for (std::map<std::string, unsigned int>::const_iterator c = s.content.begin(); c != s.content.end(); ++c) {
        std::map<std::string, ContainerId>::const_iterator f = byLabel.find(c->first);
        if (f == byLabel.end()) { ++unknownHere; continue; }
        const unsigned int hostSum = s.client->view().contentHashContainer(f->second);
        if (hostSum == c->second) ++same;
        else {
            ++differ;
            // Same things in different places, or different things?
            std::multiset<std::string> here, there;
            const std::vector<std::pair<std::string, core2::Cell> >& mine = s.rows[c->first];
            for (size_t i = 0; i < mine.size(); ++i) here.insert(mine[i].first);
            const std::vector<core2::ItemRec> theirs = s.client->view().contents(f->second);
            for (size_t i = 0; i < theirs.size(); ++i) {
                char one[64];
                _snprintf(one, sizeof(one) - 1, "%s:%u", theirs[i].sid.c_str(), theirs[i].qty);
                one[sizeof(one) - 1] = '\0';
                there.insert(one);
            }
            const bool sameThings = here == there;
            const bool sameTotal = !sameThings && sameTotals(here, there);
            if (sameThings) ++layoutOnly;
            else if (sameTotal) ++stacksOnly;
            if (shown < 8) {
                ++shown;
                std::string onlyHere, onlyHost;
                std::multiset<std::string> rest = there;
                for (std::multiset<std::string>::const_iterator h = here.begin(); h != here.end(); ++h) {
                    std::multiset<std::string>::iterator t = rest.find(*h);
                    if (t != rest.end()) rest.erase(t);
                    else if (onlyHere.size() < 160) { onlyHere += *h; onlyHere += ' '; }
                }
                for (std::multiset<std::string>::const_iterator h = rest.begin(); h != rest.end(); ++h)
                    if (onlyHost.size() < 160) { onlyHost += *h; onlyHost += ' '; }
                char line[520];
                _snprintf(line, sizeof(line) - 1, "[c2net] DIFF key=%s kind=%s n=%u/%u onlyHere=[%s] onlyHost=[%s]",
                    c->first.c_str(), sameThings ? "layout" : (sameTotal ? "stacks" : "content"), (unsigned)here.size(), (unsigned)there.size(), onlyHere.c_str(), onlyHost.c_str());
                line[sizeof(line) - 1] = '\0';
                coop::logLine(line);
            }
        }
    }
    char line[260];
    const core2::SessionStats& st = s.client->stats();
    _snprintf(line, sizeof(line) - 1,
        "[c2net] CHECK same=%u differ=%u layoutOnly=%u notInBook=%u ready=%d viewItems=%u gaps=%llu snapshots=%llu hashFix=%llu pending=%u stacksOnly=%u",
        same, differ, layoutOnly, unknownHere, s.client->ready() ? 1 : 0, (unsigned)s.client->view().itemCount(),
        st.gaps, st.snapshots, st.hashMismatch, (unsigned)s.client->pending(), stacksOnly);
    line[sizeof(line) - 1] = '\0';
    coop::logLine(line);
}

void netTick(State& s, NetLink& net, bool isHost, unsigned long nowMs) {
    if (!netEnabled()) return;
    if (!s.link.net) {
        s.link.net = &net;
        s.link.isHost = isHost;
        if (isHost) {
            s.host = new core2::HostSession(s.book, s.link);
            for (std::map<std::string, ContainerId>::iterator c = s.containers.begin(); c != s.containers.end(); ++c)
                s.host->setLabel(c->second, c->first);
        } else {
            s.client = new core2::ClientSession(net.localId(), s.link);
        }
        coop::logLine(isHost ? "[c2net] host session on" : "[c2net] client session on");
    }
    if (s.host) {
        s.host->tick(nowMs);
        if (s.scans % 30 == 1) {
            char line[200];
            const core2::SessionStats& st = s.host->stats();
            _snprintf(line, sizeof(line) - 1, "[c2net] HOST bytes=%llu msgs=%llu snapshots=%llu bad=%llu items=%u",
                st.bytesSent, st.msgsSent, st.snapshots, st.badMsgs, (unsigned)s.book.itemCount());
            line[sizeof(line) - 1] = '\0';
            coop::logLine(line);
        }
    }
    if (s.client) {
        if (!s.client->ready() && nowMs >= s.nextHelloMs) {
            s.nextHelloMs = nowMs + 15000;
            s.client->connect();
        }
        s.client->tick(nowMs);
        checkAgainstHost(s, nowMs);
    }
}

void logPaths(State& s, unsigned long nowMs) {
    if (nowMs < s.nextPathMs) return;
    s.nextPathMs = nowMs + 60000;
    engine::C2PathCount p[64];
    const unsigned int n = engine::readC2Paths(p, 64);
    HMODULE exe = GetModuleHandleA(0);
    for (unsigned int i = 0; i < n; ++i) {
        char line[120];
        _snprintf(line, sizeof(line) - 1, "[c2] PATH kind=%s rva=0x%llx count=%lu",
            p[i].kind == 0 ? "remove" : "add",
            (unsigned long long)((char*)p[i].ret - (char*)exe), p[i].count);
        line[sizeof(line) - 1] = '\0';
        coop::logLine(line);
    }
}

} // namespace

bool enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("KENSHICOOP_CORE2_SHADOW");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v == 1;
}

void tick(GameWorld* gw, unsigned long nowMs, NetLink& net, bool isHost) {
    if (!enabled() || !gw) return;
    if (!g_s) {
        g_s = new State();
        const char* e = getenv("KENSHICOOP_CORE2_SHADOW_MS");
        if (e && atoi(e) >= 200) g_s->intervalMs = (unsigned long)atoi(e);
        coop::logLine("[c2] shadow on: the book follows the game, nothing in the game is changed");
    }
    if (nowMs < g_s->nextScanMs) return;
    g_s->nextScanMs = nowMs + g_s->intervalMs;
    scanAll(*g_s, gw, nowMs);
    netTick(*g_s, net, isHost, nowMs);
    logPaths(*g_s, nowMs);
}

void receive(unsigned int from, const std::vector<unsigned char>& bytes, unsigned long nowMs) {
    if (!g_s) return;
    if (g_s->host) g_s->host->receive(from, bytes, nowMs);
    else if (g_s->client) g_s->client->receive(bytes, nowMs);
}

} // namespace core2shadow
} // namespace coop
