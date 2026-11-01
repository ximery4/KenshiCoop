// ScenarioBot.cpp - bot_remote: a file-driven remote control for a LOCAL character,
// so an outside agent (an AI assistant, a script) can play one side of a two-copy
// lab run without a mouse or a screen. FORK (ximery4/KenshiCoop), harness only
// (the whole test\ tree is ExcludedFromBuild in Release).
//
// Protocol (plain text, one command per line; the controller APPENDS to the cmd file):
//   <dir>\bot_cmd.txt   "<id> <command> [args]"          written by the controller
//   <dir>\bot_out.txt   "<id> OK|ERR|ROW|END ..."        written here, append-only
// <dir> is KENSHICOOP_BOT_DIR (default ".\KenshiCoop_bot", relative to the game dir).
// Both files are deleted when the scenario arms, then "0 READY ..." is written.
//
// Hands in this protocol are the readObjectHand layout "t,c,cs,i,s" - the same order
// every engine::* hand array takes - so a hand copied from a ROW can be pasted back.
//
// Commands (allow-list; anything else answers ERR):
//   state                      my squad: ROW sel= h= pos= task= bs= moving=
//   npcs <radius>              non-squad characters near the leader
//   items <radius>             free ground items near the leader
//   traders <radius>           trader characters near the leader
//   sel <n>                    choose which squad member the next commands act on
//   move <x> <y> <z>           walk the selected member there (engine locomotion)
//   halt                       stop the selected member
//   attack <hand>              selected member attacks the character at <hand>
//   pickup <sid> <type> <r>    pick a free ground item of sid/type into the selected member
//   trade_open <hand>          open the trade window with the trader at <hand>
//   trade_buy <hand>           buy the first loose one-unit stack of that trader's stock
//   trade_close                close the trade window
//   dump                       log the selected member's full inventory (devtools log)
//   log <text>                 put "BOT <text>" into the coop log (markers for oracles)
//   wait <ms>                  hold the command queue
//   quit                       end the scenario (RESULT PASS)
//
// Must NOT: change any other scenario's log strings; read or write anything outside
// <dir>; run in a Release build.

#include "ScenarioSupport.h"

#include <windows.h>
#include <vector>
#include <string>

namespace coop {
namespace {

class BotRemoteScenario : public TimedScenario {
public:
    BotRemoteScenario()
        : TimedScenario("bot_remote", 0),
          readOff_(0), head_(0), waitUntil_(0), lastPoll_(0), sel_(0), quit_(false) {
        const char* d = getenv("KENSHICOOP_BOT_DIR");
        dir_ = (d && d[0]) ? d : ".\\KenshiCoop_bot";
        cmdPath_ = dir_ + "\\bot_cmd.txt";
        outPath_ = dir_ + "\\bot_out.txt";
    }

    virtual void onStart(const ScenarioContext& ctx) {
        CreateDirectoryA(dir_.c_str(), 0);
        DeleteFileA(cmdPath_.c_str());
        DeleteFileA(outPath_.c_str());
        char b[160];
        _snprintf(b, sizeof(b) - 1, "READY host=%d dir=%s", ctx.isHost ? 1 : 0, dir_.c_str());
        b[sizeof(b) - 1] = '\0';
        reply("0", b);
        coop::logLine("BOT armed (bot_remote)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Poll the command file ~10x/s; run at most one command per tick.
        if (ctx.elapsedMs - lastPoll_ >= 100) {
            lastPoll_ = ctx.elapsedMs;
            pollFile();
        }
        if (waitUntil_ != 0 && ctx.elapsedMs < waitUntil_) return false;
        waitUntil_ = 0;
        if (head_ < queue_.size()) {
            std::string line = queue_[head_++];
            run(ctx, line);
        }
        if (quit_) { setPassed(true); return true; }
        return false;
    }

private:
    // ---- output ----
    void reply(const char* id, const char* text) {
        FILE* f = fopen(outPath_.c_str(), "ab");
        if (!f) return;
        fprintf(f, "%s %s\n", id, text);
        fclose(f);
    }
    void rowf(const char* id, const char* fmt, ...) {
        char b[512];
        va_list ap; va_start(ap, fmt);
        _vsnprintf(b, sizeof(b) - 1, fmt, ap);
        va_end(ap);
        b[sizeof(b) - 1] = '\0';
        std::string s = "ROW "; s += b;
        reply(id, s.c_str());
    }

    // ---- input ----
    void pollFile() {
        FILE* f = fopen(cmdPath_.c_str(), "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        if (size > readOff_) {
            fseek(f, readOff_, SEEK_SET);
            char buf[4096];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = '\0';
            // consume complete lines only; a partial last line waits for its newline
            size_t used = 0, start = 0;
            for (size_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    std::string ln(buf + start, i - start);
                    while (!ln.empty() && (ln[ln.size() - 1] == '\r' || ln[ln.size() - 1] == ' '))
                        ln.erase(ln.size() - 1);
                    if (!ln.empty()) queue_.push_back(ln);
                    start = i + 1;
                    used = i + 1;
                }
            }
            readOff_ += (long)used;
        }
        fclose(f);
    }

    static bool parseHand(const char* s, unsigned int h[5]) {
        return sscanf(s, "%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &h[4]) == 5;
    }

    // The selected squad member: its EntityState (hand + pos) and live Character*.
    bool selected(const ScenarioContext& ctx, EntityState* e, Character** c, unsigned int h[5]) {
        EntityState sq[32];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, 32);
        if (n == 0) return false;
        unsigned int i = (sel_ < n) ? sel_ : 0;
        *e = sq[i];
        handFromEntity(sq[i], h);
        *c = engine::resolve(sq[i]);
        return true;
    }

    void run(const ScenarioContext& ctx, const std::string& line) {
        char buf[512];
        strncpy(buf, line.c_str(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
        char* id = strtok(buf, " ");
        char* cmd = strtok(0, " ");
        if (!id || !cmd) return;
        char* a1 = strtok(0, " ");
        char* a2 = strtok(0, " ");
        char* a3 = strtok(0, " ");
        char tail[256]; tail[0] = '\0';
        { // the rest of the line verbatim, for "log"
            size_t skip = strlen(id) + 1 + strlen(cmd) + 1;
            if (skip < line.size()) { strncpy(tail, line.c_str() + skip, sizeof(tail) - 1); tail[sizeof(tail) - 1] = '\0'; }
        }
        char lg[200];
        _snprintf(lg, sizeof(lg) - 1, "BOT cmd id=%s %s", id, cmd);
        lg[sizeof(lg) - 1] = '\0';
        coop::logLine(lg);

        EntityState e; Character* me = 0; unsigned int mh[5] = {0, 0, 0, 0, 0};
        const bool haveMe = selected(ctx, &e, &me, mh);

        if (!strcmp(cmd, "state")) {
            EntityState sq[32];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, 32);
            rowf(id, "t=%lu host=%d peer=%d sel=%u n=%u", ctx.elapsedMs, ctx.isHost ? 1 : 0,
                 ctx.peerReady ? 1 : 0, sel_, n);
            for (unsigned int i = 0; i < n; ++i) {
                unsigned int h[5]; handFromEntity(sq[i], h);
                rowf(id, "m=%u h=%u,%u,%u,%u,%u pos=%.1f,%.1f,%.1f task=%u bs=%u moving=%d", i,
                     h[0], h[1], h[2], h[3], h[4], sq[i].x, sq[i].y, sq[i].z,
                     (unsigned int)sq[i].task, (unsigned int)sq[i].bodyState, (int)sq[i].cMoving);
            }
            reply(id, "END");
        } else if (!strcmp(cmd, "npcs")) {
            float r = a1 ? (float)atof(a1) : 100.0f;
            EntityState npc[64];
            unsigned int n = engine::captureNpcs(ctx.gw, npc, 64);
            unsigned int shown = 0;
            for (unsigned int i = 0; i < n; ++i) {
                float dx = haveMe ? npc[i].x - e.x : 0, dz = haveMe ? npc[i].z - e.z : 0;
                float d = sqrtf(dx * dx + dz * dz);
                if (haveMe && d > r) continue;
                unsigned int h[5]; handFromEntity(npc[i], h);
                engine::CombatRead cr; memset(&cr, 0, sizeof(cr));
                engine::readCombatByHand(h, &cr);
                rowf(id, "h=%u,%u,%u,%u,%u d=%.1f pos=%.1f,%.1f,%.1f task=%u bs=%u ic=%d rg=%d tgt=%u,%u,%u,%u,%u",
                     h[0], h[1], h[2], h[3], h[4], d, npc[i].x, npc[i].y, npc[i].z,
                     (unsigned int)npc[i].task, (unsigned int)npc[i].bodyState,
                     cr.valid && cr.modeActive ? 1 : 0, cr.ranged ? 1 : 0,
                     cr.target[0], cr.target[1], cr.target[2], cr.target[3], cr.target[4]);
                ++shown;
            }
            rowf(id, "total=%u shown=%u", n, shown);
            reply(id, "END");
        } else if (!strcmp(cmd, "items")) {
            float r = a1 ? (float)atof(a1) : 50.0f;
            engine::WorldItemRaw raw[64];
            unsigned int n = engine::captureWorldItems(ctx.gw, raw, 64, r);
            for (unsigned int i = 0; i < n; ++i)
                rowf(id, "sid=%s type=%u qty=%u pos=%.1f,%.1f,%.1f", raw[i].stringID, raw[i].itemType,
                     (unsigned int)raw[i].quantity, raw[i].x, raw[i].y, raw[i].z);
            reply(id, "END");
        } else if (!strcmp(cmd, "traders")) {
            float r = a1 ? (float)atof(a1) : 100.0f;
            engine::TraderRead trs[16];
            unsigned int n = engine::listTradersNear(ctx.gw, r, trs, 16);
            for (unsigned int i = 0; i < n; ++i)
                rowf(id, "h=%u,%u,%u,%u,%u name=%s pos=%.1f,%.1f,%.1f stockEntries=%d",
                     trs[i].hand[0], trs[i].hand[1], trs[i].hand[2], trs[i].hand[3], trs[i].hand[4],
                     trs[i].name, trs[i].x, trs[i].y, trs[i].z, trs[i].nEntries);
            reply(id, "END");
        } else if (!strcmp(cmd, "sel")) {
            if (!a1) { reply(id, "ERR usage: sel <n>"); return; }
            sel_ = (unsigned int)atoi(a1);
            reply(id, "OK");
        } else if (!strcmp(cmd, "move")) {
            if (!me || !a3) { reply(id, "ERR no member or usage: move x y z"); return; }
            bool ok = engine::orderMoveTo(me, (float)atof(a1), (float)atof(a2), (float)atof(a3));
            reply(id, ok ? "OK" : "ERR orderMoveTo failed");
        } else if (!strcmp(cmd, "sneak")) {
            if (!me || !a1) { reply(id, "ERR usage: sneak 0|1"); return; }
            reply(id, engine::applyStealth(me, atoi(a1) != 0) ? "OK" : "ERR sneak failed");
        } else if (!strcmp(cmd, "combat")) {
            if (!haveMe) { reply(id, "ERR no member"); return; }
            engine::CombatRead cr; memset(&cr, 0, sizeof(cr));
            engine::readCombatByHand(mh, &cr);
            rowf(id, "valid=%d inCombat=%d mode=%d ranged=%d fleeing=%d hasTarget=%d tgt=%u,%u,%u,%u,%u sneak=%d",
                 cr.valid ? 1 : 0, cr.inCombat ? 1 : 0, cr.modeActive ? 1 : 0, cr.ranged ? 1 : 0,
                 cr.fleeing ? 1 : 0, cr.hasTarget ? 1 : 0, cr.target[0], cr.target[1], cr.target[2],
                 cr.target[3], cr.target[4], engine::readStealthMode(me));
            reply(id, "END");
        } else if (!strcmp(cmd, "halt")) {
            if (!me) { reply(id, "ERR no member"); return; }
            reply(id, engine::haltMovement(me) ? "OK" : "ERR halt failed");
        } else if (!strcmp(cmd, "attack")) {
            unsigned int v[5];
            if (!haveMe || !a1 || !parseHand(a1, v)) { reply(id, "ERR usage: attack t,c,cs,i,s"); return; }
            reply(id, engine::orderAttackByHand(ctx.gw, mh, v) ? "OK" : "ERR attack not issued");
        } else if (!strcmp(cmd, "pickup")) {
            if (!haveMe || !a3) { reply(id, "ERR usage: pickup sid type radius"); return; }
            int r = engine::pickupWorldItemIntoInventory(ctx.gw, mh, a1, (unsigned int)atoi(a2),
                                                         (float)atof(a3));
            char b[48]; _snprintf(b, sizeof(b) - 1, "%s res=%d", r > 0 ? "OK" : "ERR", r);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "trade_open")) {
            unsigned int t[5];
            if (!haveMe || !a1 || !parseHand(a1, t)) { reply(id, "ERR usage: trade_open t,c,cs,i,s"); return; }
            int r = engine::openTradeWindow(mh, t);
            char b[48]; _snprintf(b, sizeof(b) - 1, "%s res=%d", r == 1 ? "OK" : "ERR", r);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "trade_buy")) {
            unsigned int t[5];
            if (!haveMe || !a1 || !parseHand(a1, t)) { reply(id, "ERR usage: trade_buy t,c,cs,i,s"); return; }
            char sid[48]; sid[0] = '\0'; int qty = 0;
            int r = engine::traderBuyOne(ctx.gw, t, mh, sid, sizeof(sid), &qty);
            char b[128]; _snprintf(b, sizeof(b) - 1, "%s res=%d sid=%s qty=%d", r == 1 ? "OK" : "ERR", r, sid, qty);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "stock")) {
            unsigned int t[5];
            if (!a1 || !parseHand(a1, t)) { reply(id, "ERR usage: stock t,c,cs,i,s"); return; }
            InvItemEntry items[96]; static char names[96 * 64];
            int n = engine::traderStockList(ctx.gw, t, items, 96, names);
            if (n < 0) { reply(id, "ERR no shop stock (open the trade window first)"); return; }
            for (int i = 0; i < n; ++i)
                rowf(id, "sid=%s qty=%u type=%u eq=%d name=%s", items[i].stringID, (unsigned int)items[i].quantity, items[i].itemType, (int)items[i].equipped, names + i * 64);
            reply(id, "END");
        } else if (!strcmp(cmd, "inv")) {
            if (!haveMe) { reply(id, "ERR no member"); return; }
            InvItemEntry items[96]; static char names[96 * 64];
            int n = engine::invList(ctx.gw, mh, items, 96, names);
            for (int i = 0; i < n; ++i)
                rowf(id, "sid=%s qty=%u type=%u eq=%d name=%s", items[i].stringID, (unsigned int)items[i].quantity,
                     items[i].itemType, (int)items[i].equipped, names + i * 64);
            reply(id, "END");
        } else if (!strcmp(cmd, "buy")) {
            unsigned int t[5];
            if (!haveMe || !a2 || !parseHand(a1, t)) { reply(id, "ERR usage: buy t,c,cs,i,s <sid-part>"); return; }
            char sid[48]; sid[0] = '\0'; int qty = 0;
            int r = engine::traderBuySid(ctx.gw, t, mh, a2, sid, sizeof(sid), &qty);
            char b[128]; _snprintf(b, sizeof(b) - 1, "%s res=%d sid=%s qty=%d", r == 1 ? "OK" : "ERR", r, sid, qty);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "sell")) {
            unsigned int t[5];
            if (!haveMe || !a2 || !parseHand(a1, t)) { reply(id, "ERR usage: sell t,c,cs,i,s <sid-part>"); return; }
            // the shop counter nearest to that trader
            engine::TraderRead trs[16];
            unsigned int nt = engine::listTradersNear(ctx.gw, 400.0f, trs, 16);
            float tx = 0, tz = 0; bool found = false;
            for (unsigned int i = 0; i < nt; ++i)
                if (!memcmp(trs[i].hand, t, sizeof(t))) { tx = trs[i].x; tz = trs[i].z; found = true; break; }
            engine::ShopCounterRead rows[32];
            unsigned int nc = found ? engine::enumShopCountersNear(ctx.gw, 400.0f, rows, 32) : 0;
            int best = -1; float bd = 1e30f;
            for (unsigned int i = 0; i < nc; ++i) {
                float dx = rows[i].x - tx, dz = rows[i].z - tz, d = dx * dx + dz * dz;
                if (d < bd) { bd = d; best = (int)i; }
            }
            if (best < 0) { reply(id, "ERR trader or counter not found"); return; }
            char sid[48]; sid[0] = '\0'; int qty = 0;
            int r = engine::vendorSellSid(ctx.gw, mh, rows[best].hand, a2, sid, sizeof(sid), &qty);
            char b[128]; _snprintf(b, sizeof(b) - 1, "%s res=%d sid=%s qty=%d", r == 1 ? "OK" : "ERR", r, sid, qty);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "trade_close")) {
            engine::closeTradeWindow();
            reply(id, "OK");
        } else if (!strcmp(cmd, "dump")) {
            if (!haveMe) { reply(id, "ERR no member"); return; }
            engine::dumpInventory(ctx.gw, mh);
            reply(id, "OK see coop log");
        } else if (!strcmp(cmd, "log")) {
            char b[300]; _snprintf(b, sizeof(b) - 1, "BOT %s", tail);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            reply(id, "OK");
        } else if (!strcmp(cmd, "wait")) {
            unsigned long ms = a1 ? (unsigned long)atol(a1) : 1000;
            if (ms > 600000) ms = 600000;
            waitUntil_ = ctx.elapsedMs + ms;
            reply(id, "OK");
        } else if (!strcmp(cmd, "quit")) {
            quit_ = true;
            reply(id, "OK");
        } else {
            reply(id, "ERR unknown command");
        }
    }

    std::string              dir_, cmdPath_, outPath_;
    long                     readOff_;
    std::vector<std::string> queue_;
    size_t                   head_;
    unsigned long            waitUntil_, lastPoll_;
    unsigned int             sel_;
    bool                     quit_;
};

} // namespace

Scenario* makeBotScenario(const std::string& name) {
    if (name == "bot_remote") return new BotRemoteScenario();
    return 0;
}

} // namespace coop
