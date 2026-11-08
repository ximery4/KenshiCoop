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
//   machines <radius>          mines / refiners / benches / farms / research stands near the selected member
//   machine <hand>             one machine's buffers and what its container holds
//   work <hand> [task]         send the selected member to work that machine or mine
//   workstate                  the selected member's current task, working=1 while it operates a machine
//   collect <hand> [sid-part]  move the machine container's contents (ore, crafted goods) into the selected member
//   feed <hand> <sid> <type> <qty>  move raw material from the selected member into the machine
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
        static EntityState sq[128];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, 128);
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
            static EntityState sq[128];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, 128);
            rowf(id, "t=%lu host=%d peer=%d sel=%u n=%u", ctx.elapsedMs, ctx.isHost ? 1 : 0,
                 ctx.peerReady ? 1 : 0, sel_, n);
            for (unsigned int i = 0; i < n; ++i) {
                unsigned int h[5]; handFromEntity(sq[i], h);
                char nm[48]; nm[0] = 0;
                { Character* ch = engine::resolve(sq[i]); if (ch) engine::charName(ch, nm, sizeof(nm)); }
                rowf(id, "m=%u h=%u,%u,%u,%u,%u pos=%.1f,%.1f,%.1f task=%u bs=%u moving=%d tab=%d own=%d name=%s", i,
                     h[0], h[1], h[2], h[3], h[4], sq[i].x, sq[i].y, sq[i].z,
                     (unsigned int)sq[i].task, (unsigned int)sq[i].bodyState, (int)sq[i].cMoving, tabRankOf(sq, n, i), (tabRankOf(sq, n, i) == (int)ctx.localId) ? 1 : 0, nm);
            }
            reply(id, "END");
        } else if (!strcmp(cmd, "npcs")) {
            float r = a1 ? (float)atof(a1) : 100.0f;
            static EntityState npc[256];
            unsigned int n = engine::captureNpcs(ctx.gw, npc, 256);
            unsigned int shown = 0;
            for (unsigned int i = 0; i < n; ++i) {
                float dx = haveMe ? npc[i].x - e.x : 0, dz = haveMe ? npc[i].z - e.z : 0;
                float d = sqrtf(dx * dx + dz * dz);
                if (haveMe && d > r) continue;
                unsigned int h[5]; handFromEntity(npc[i], h);
                engine::CombatRead cr; memset(&cr, 0, sizeof(cr));
                engine::readCombatByHand(h, &cr);
                char nm[48]; nm[0] = 0;
                { Character* ch = engine::resolve(npc[i]); if (ch) engine::charName(ch, nm, sizeof(nm)); }
                rowf(id, "h=%u,%u,%u,%u,%u d=%.1f pos=%.1f,%.1f,%.1f task=%u bs=%u ic=%d rg=%d tgt=%u,%u,%u,%u,%u name=%s",
                     h[0], h[1], h[2], h[3], h[4], d, npc[i].x, npc[i].y, npc[i].z,
                     (unsigned int)npc[i].task, (unsigned int)npc[i].bodyState,
                     cr.valid && cr.modeActive ? 1 : 0, cr.ranged ? 1 : 0,
                     cr.target[0], cr.target[1], cr.target[2], cr.target[3], cr.target[4], nm);
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
        } else if (!strcmp(cmd, "split")) {
            // eject the selected member into a NEW squad tab (the engine's own separateIntoMyOwnSquad)
            if (!haveMe) { reply(id, "ERR no member"); return; }
            unsigned int before[5] = {0, 0, 0, 0, 0}, after[5] = {0, 0, 0, 0, 0};
            int r = engine::probeMoveSquadMember(ctx.gw, mh, mh, 0, before, after);
            char b[160]; _snprintf(b, sizeof(b) - 1, "%s res=%d after=%u,%u,%u,%u,%u", r == 1 ? "OK" : "ERR", r,
                                   after[0], after[1], after[2], after[3], after[4]);
            b[sizeof(b) - 1] = 0; reply(id, b);
        } else if (!strcmp(cmd, "savegame")) {
            if (!a1) { reply(id, "ERR usage: savegame <name>"); return; }
            reply(id, engine::saveGameAs(a1) ? "OK" : "ERR save failed");
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
            if (a1 && !strcmp(a1, "deep")) {
                // inv deep: also what worn containers (backpacks) hold; parent=N means nested inside entry N-1
                unsigned int nd = engine::captureContainerContents(ctx.gw, mh, items, 96, 0, 0, true);
                for (unsigned int i = 0; i < nd; ++i)
                    rowf(id, "#%u sid=%s qty=%u type=%u q=%u eq=%d parent=%u", i, items[i].stringID,
                         (unsigned int)items[i].quantity, items[i].itemType, (unsigned int)items[i].quality,
                         (int)items[i].equipped, (unsigned int)items[i].parentIdx);
                reply(id, "END");
                return;
            }
            int n = engine::invList(ctx.gw, mh, items, 96, names);
            for (int i = 0; i < n; ++i)
                rowf(id, "sid=%s qty=%u type=%u q=%u eq=%d name=%s", items[i].stringID, (unsigned int)items[i].quantity,
                     items[i].itemType, (unsigned int)items[i].quality, (int)items[i].equipped, names + i * 64);
            reply(id, "END");
        } else if (!strcmp(cmd, "modes")) {
            // the orders-panel toggles of the selected member: block hold passive taunt ranged stealth
            if (!me) { reply(id, "ERR no member"); return; }
            int m = engine::readCombatModes(me);
            if (m < 0) { reply(id, "ERR unreadable"); return; }
            rowf(id, "block=%d hold=%d passive=%d taunt=%d ranged=%d stealth=%d", (m & 1) ? 1 : 0, (m & 2) ? 1 : 0,
                 (m & 4) ? 1 : 0, (m & 8) ? 1 : 0, (m & 16) ? 1 : 0, engine::readStealthMode(me));
            reply(id, "END");
        } else if (!strcmp(cmd, "mode")) {
            // mode block|hold|passive|taunt|ranged 0|1
            if (!me || !a2) { reply(id, "ERR usage: mode block|hold|passive|taunt|ranged 0|1"); return; }
            int bit = !strcmp(a1, "block") ? 1 : !strcmp(a1, "hold") ? 2 : !strcmp(a1, "passive") ? 4
                    : !strcmp(a1, "taunt") ? 8 : !strcmp(a1, "ranged") ? 16 : 0;
            if (!bit) { reply(id, "ERR unknown mode"); return; }
            reply(id, engine::applyCombatModes(me, bit, atoi(a2) ? bit : 0) ? "OK" : "ERR mode failed");
        } else if (!strcmp(cmd, "loot")) {
            // loot [radius]: move everything from the nearest dead body in reach into the selected member
            // (the same container move a UI drag makes, so the transfer detector sees it like a real take)
            if (!haveMe) { reply(id, "ERR no member"); return; }
            float rad = a1 ? (float)atof(a1) : 12.0f;
            static engine::CorpseRead cr[16];
            unsigned int nc = engine::enumNpcBodiesNear(ctx.gw, rad + 60.0f, cr, 16, true);
            int best = -1; float bd = 1e30f;
            for (unsigned int i = 0; i < nc; ++i) {
                // a2 = ";"-separated hands the panel already looted (a host-rejected take rolls back, so the same body keeps looking full)
                char hs[64]; _snprintf(hs, sizeof(hs) - 1, "%u,%u,%u,%u,%u", cr[i].hand[0], cr[i].hand[1], cr[i].hand[2], cr[i].hand[3], cr[i].hand[4]);
                hs[sizeof(hs) - 1] = '\0';
                if (a2 && strstr(a2, hs)) continue;
                float dx = cr[i].x - e.x, dz = cr[i].z - e.z, d = dx * dx + dz * dz;
                if (d < bd) { bd = d; best = (int)i; }
            }
            if (best < 0 || bd > rad * rad) { reply(id, "ERR no corpse in reach"); return; }
            InvItemEntry items[64];
            unsigned int n = engine::captureContainerContents(ctx.gw, cr[best].hand, items, 64, 0);
            int moved = 0;
            for (unsigned int i = 0; i < n; ++i)
                moved += engine::moveItemBetweenContainers(ctx.gw, cr[best].hand, mh, items[i].stringID,
                                                           items[i].itemType, items[i].quantity < 1 ? 1 : (int)items[i].quantity);
            char b[160]; _snprintf(b, sizeof(b) - 1, "%s corpse=%u,%u,%u,%u,%u items=%u moved=%d d=%.1f",
                                   moved > 0 ? "OK" : "ERR", cr[best].hand[0], cr[best].hand[1], cr[best].hand[2],
                                   cr[best].hand[3], cr[best].hand[4], n, moved, sqrt(bd));
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "machines")) {
            // machines [radius]: mines, refiners, benches, farms, research stands near the selected member.
            // cls: BuildingClassType number (the script tells a mine by out= + mine>0, a bench by in0/in1)
            if (!haveMe) { reply(id, "ERR no member"); return; }
            float rad = a1 ? (float)atof(a1) : 150.0f;
            static engine::ProdRead pr[96];
            unsigned int n = engine::enumMachinesAround(ctx.gw, e.x, e.y, e.z, rad, pr, 96);   // around the member, not the squad leader
            unsigned int shown = 0;
            for (unsigned int i = 0; i < n; ++i) {
                float dx = pr[i].x - e.x, dz = pr[i].z - e.z, d = sqrtf(dx * dx + dz * dz);
                if (d > rad) continue;
                rowf(id, "h=%u,%u,%u,%u,%u d=%.1f pos=%.1f,%.1f,%.1f cls=%d done=%d pow=%d ps=%d mine=%.2f out=%s:%.2f/%d "
                         "in0=%s:%.2f in1=%s:%.2f sid=%s name=%s",
                     pr[i].hand[0], pr[i].hand[1], pr[i].hand[2], pr[i].hand[3], pr[i].hand[4], d,
                     pr[i].x, pr[i].y, pr[i].z, pr[i].classType, pr[i].complete, pr[i].powerOn, pr[i].productionState,
                     pr[i].miningLevel, pr[i].outSid[0] ? pr[i].outSid : "-", pr[i].outAmount, pr[i].outCap,
                     pr[i].nInputs > 0 && pr[i].inSid[0][0] ? pr[i].inSid[0] : "-", pr[i].nInputs > 0 ? pr[i].inAmount[0] : -1.0f,
                     pr[i].nInputs > 1 && pr[i].inSid[1][0] ? pr[i].inSid[1] : "-", pr[i].nInputs > 1 ? pr[i].inAmount[1] : -1.0f,
                     pr[i].sid, pr[i].name);
                ++shown;
            }
            rowf(id, "total=%u shown=%u", n, shown);
            reply(id, "END");
        } else if (!strcmp(cmd, "machine")) {
            // machine t,c,cs,i,s: one machine's state plus what its container holds (its stock = mined ore / crafted goods)
            unsigned int mc[5];
            engine::ProdRead pr;
            if (!a1 || !parseHand(a1, mc)) { reply(id, "ERR usage: machine t,c,cs,i,s"); return; }
            memset(&pr, 0, sizeof(pr));
            if (!engine::readMachineByHand(mc, &pr)) { reply(id, "ERR not a machine here"); return; }
            rowf(id, "cls=%d done=%d pow=%d ps=%d mine=%.2f out=%s:%.2f/%d in0=%s:%.2f in1=%s:%.2f name=%s",
                 pr.classType, pr.complete, pr.powerOn, pr.productionState, pr.miningLevel,
                 pr.outSid[0] ? pr.outSid : "-", pr.outAmount, pr.outCap,
                 pr.nInputs > 0 && pr.inSid[0][0] ? pr.inSid[0] : "-", pr.nInputs > 0 ? pr.inAmount[0] : -1.0f,
                 pr.nInputs > 1 && pr.inSid[1][0] ? pr.inSid[1] : "-", pr.nInputs > 1 ? pr.inAmount[1] : -1.0f, pr.name);
            InvItemEntry mi[64];
            unsigned int nm = engine::captureContainerContents(ctx.gw, mc, mi, 64, 0);
            for (unsigned int i = 0; i < nm; ++i)
                rowf(id, "stock sid=%s qty=%u type=%u", mi[i].stringID, (unsigned int)mi[i].quantity, mi[i].itemType);
            reply(id, "END");
        } else if (!strcmp(cmd, "cont")) {
            // cont t,c,cs,i,s: what ANY container holds (chest, storage, furniture, machine, character) - block5g
            // container_multi_intent check across windows.
            unsigned int mc[5];
            if (!a1 || !parseHand(a1, mc)) { reply(id, "ERR usage: cont t,c,cs,i,s"); return; }
            if (engine::resolveObjectByHand(mc) == 0) { reply(id, "ERR unresolved"); return; }
            InvItemEntry mi[96];
            unsigned int nm = engine::captureContainerContents(ctx.gw, mc, mi, 96, 0);
            unsigned int units = 0;
            for (unsigned int i = 0; i < nm; ++i) {
                units += mi[i].quantity < 1 ? 1u : (unsigned int)mi[i].quantity;
                rowf(id, "stock sid=%s qty=%u type=%u", mi[i].stringID, (unsigned int)mi[i].quantity, mi[i].itemType);
            }
            rowf(id, "entries=%u units=%u", nm, units);
            reply(id, "END");
        } else if (!strcmp(cmd, "chests")) {
            // chests [radius]: containers (storage, furniture with an inventory, machines) around the selected member,
            // the same census the host uses - block5g
            if (!haveMe) { reply(id, "ERR no member"); return; }
            float rad = a1 ? (float)atof(a1) : 150.0f;
            static engine::ContRead cr2[96];
            unsigned int n = engine::enumContainersNear(ctx.gw, rad, cr2, 96, false);
            unsigned int shown = 0;
            for (unsigned int i = 0; i < n; ++i) {
                float dx = cr2[i].x - e.x, dz = cr2[i].z - e.z, d = sqrtf(dx * dx + dz * dz);
                if (d > rad) continue;
                rowf(id, "h=%u,%u,%u,%u,%u d=%.1f cls=%d inv=%d n=%d qty=%d sid=%s name=%s", cr2[i].hand[0], cr2[i].hand[1],
                     cr2[i].hand[2], cr2[i].hand[3], cr2[i].hand[4], d, cr2[i].classType, cr2[i].hasInv, cr2[i].nEntries,
                     cr2[i].qtyTotal, cr2[i].sid, cr2[i].name);
                ++shown;
            }
            rowf(id, "total=%u shown=%u", n, shown);
            reply(id, "END");
        } else if (!strcmp(cmd, "work")) {
            // work t,c,cs,i,s [task]: send the selected member to work that mine/machine (player order,
            // default task OPERATE_MACHINERY). Re-sending while it already works is harmless but resets the walk.
            unsigned int mc[5];
            if (!me || !a1 || !parseHand(a1, mc)) { reply(id, "ERR usage: work t,c,cs,i,s [task]"); return; }
            int used = 0;
            int r = engine::orderWorkByHand(me, mc, a2 ? atoi(a2) : 0, &used);
            char b[96]; _snprintf(b, sizeof(b) - 1, "%s res=%d task=%d", r == 1 ? "OK" : "ERR", r, used);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "workstate")) {
            // workstate: what the selected member does right now. working=1 while its task is a machine-work task.
            if (!me) { reply(id, "ERR no member"); return; }
            int tk = engine::readTaskKey(me);
            rowf(id, "task=%d working=%d moving=%d bs=%u pos=%.1f,%.1f,%.1f", tk, engine::isMachineWorkTask(tk) ? 1 : 0,
                 (int)e.cMoving, (unsigned int)e.bodyState, e.x, e.y, e.z);
            reply(id, "END");
        } else if (!strcmp(cmd, "collect")) {
            // collect t,c,cs,i,s [sid-part]: move what the machine's container holds (mined ore, crafted goods) into the
            // selected member - a container-to-container move like the UI drag. The member must stand near the machine.
            unsigned int mc[5];
            if (!haveMe || !a1 || !parseHand(a1, mc)) { reply(id, "ERR usage: collect t,c,cs,i,s [sid-part]"); return; }
            InvItemEntry mi[64];
            unsigned int nm = engine::captureContainerContents(ctx.gw, mc, mi, 64, 0);
            int moved = 0, kinds = 0;
            for (unsigned int i = 0; i < nm; ++i) {
                if (a2 && !strstr(mi[i].stringID, a2)) continue;
                int q = mi[i].quantity < 1 ? 1 : (int)mi[i].quantity;
                int mv = engine::moveItemBetweenContainers(ctx.gw, mc, mh, mi[i].stringID, mi[i].itemType, q);
                if (mv > 0) { moved += mv; ++kinds; }
            }
            char b[96]; _snprintf(b, sizeof(b) - 1, "%s stacks=%u moved=%d kinds=%d", moved > 0 ? "OK" : "ERR", nm, moved, kinds);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "feed")) {
            // feed t,c,cs,i,s <sid> <type> <qty>: move qty of sid from the selected member into the machine's container
            // (raw material for a refiner or bench). Moves what is there, up to qty.
            char sidS[64]; unsigned int ty = 0; int qn = 0, mc5[5];
            unsigned int mc[5];
            if (!haveMe || sscanf(tail, "%d,%d,%d,%d,%d %63s %u %d", &mc5[0], &mc5[1], &mc5[2], &mc5[3], &mc5[4], sidS, &ty, &qn) != 8) {
                reply(id, "ERR usage: feed t,c,cs,i,s <sid> <type> <qty>"); return; }
            for (int k = 0; k < 5; ++k) mc[k] = (unsigned int)mc5[k];
            int mv = engine::moveItemBetweenContainers(ctx.gw, mh, mc, sidS, ty, qn);
            if (mv < qn) mv += engine::moveItemOutOfNestedContainer(ctx.gw, mh, mc, sidS, ty, qn - mv);
            char b[64]; _snprintf(b, sizeof(b) - 1, "%s moved=%d", mv > 0 ? "OK" : "ERR", mv);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "givebag")) {
            // givebag <sid> <type> <qty>: mint qty of sid INSIDE the selected member's backpack (test fixture)
            if (!haveMe || !a3) { reply(id, "ERR usage: givebag <sid> <type> <qty>"); return; }
            int r = engine::addItemToNestedContainer(ctx.gw, mh, a1, (unsigned int)atoi(a2), atoi(a3), 0);
            char b[64]; _snprintf(b, sizeof(b) - 1, "%s added=%d", r > 0 ? "OK" : "ERR", r);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "setq")) {
            // setq <sid> <type> <q>: set the condition (hundredths, 10000 = pristine) of the selected member's (sid,type) items, bags included
            if (!haveMe || !a3) { reply(id, "ERR usage: setq <sid> <type> <q>"); return; }
            int r = engine::setItemQualityBySid(ctx.gw, mh, a1, (unsigned int)atoi(a2), atoi(a3));
            char b[64]; _snprintf(b, sizeof(b) - 1, "%s set=%d", r > 0 ? "OK" : "ERR", r);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "bagmove")) {
            // bagmove <dst t,c,cs,i,s> <sid> <type> <qty>: move from the selected member (top level first, then its
            // backpack) into dst's inventory, as a player's drag between two characters would
            char dstS[64], sidS[64]; unsigned int ty = 0; int qn = 0;
            unsigned int dh[5];
            if (!haveMe || sscanf(tail, "%63s %63s %u %d", dstS, sidS, &ty, &qn) != 4 || !parseHand(dstS, dh)) {
                reply(id, "ERR usage: bagmove t,c,cs,i,s <sid> <type> <qty>"); return; }
            int mv = engine::moveItemBetweenContainers(ctx.gw, mh, dh, sidS, ty, qn);
            int top = mv;
            if (mv < qn) mv += engine::moveItemOutOfNestedContainer(ctx.gw, mh, dh, sidS, ty, qn - mv);
            char b[80]; _snprintf(b, sizeof(b) - 1, "%s moved=%d top=%d", mv > 0 ? "OK" : "ERR", mv, top);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "heal")) {
            // heal [all|t,c,cs,i,s]: bandage every damaged part (the medic_order scaffold; the flesh then
            // self-heals up to the bandage level). Default = selected member, "all" = the whole local squad.
            int parts = 0, who = 0;
            if (a1 && !strcmp(a1, "all")) {
                static EntityState sq[128];
                unsigned int n = engine::captureSquad(ctx.gw, false, sq, 128);
                for (unsigned int i = 0; i < n; ++i) {
                    if (tabRankOf(sq, n, i) != (int)ctx.localId) continue; // own squad only (copies of others are not ours to treat)
                    unsigned int h[5]; handFromEntity(sq[i], h);
                    parts += engine::healSubjectBandage(ctx.gw, h); ++who;
                }
            } else if (a1) {
                unsigned int t[5];
                if (!parseHand(a1, t)) { reply(id, "ERR usage: heal [all|t,c,cs,i,s]"); return; }
                parts = engine::healSubjectBandage(ctx.gw, t); who = 1;
            } else {
                if (!haveMe) { reply(id, "ERR no member"); return; }
                parts = engine::healSubjectBandage(ctx.gw, mh); who = 1;
            }
            char b[64]; _snprintf(b, sizeof(b) - 1, "OK who=%d parts=%d", who, parts);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
        } else if (!strcmp(cmd, "give")) {
            // give <sid> <type> <qty> : mint qty of sid into the selected member's inventory (repair/restore tool)
            if (!haveMe || !a3) { reply(id, "ERR usage: give <sid> <type> <qty>"); return; }
            int r = engine::addItemsToContainerBySid(ctx.gw, mh, a1, (unsigned int)atoi(a2), atoi(a3), 0, "", "");
            char b[64]; _snprintf(b, sizeof(b) - 1, "%s added=%d", r > 0 ? "OK" : "ERR", r);
            b[sizeof(b) - 1] = '\0'; reply(id, b);
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
