// EngineCore2.cpp - read side of the core2 adapter: every item of an object's inventory with its
// real grid place, and a count of the engine call sites that move items. Reads only.

#include "EngineInternal.h"

#include <intrin.h>
#include <cstring>
#include <map>

#pragma intrinsic(_ReturnAddress)

namespace coop {
namespace engine {

namespace {

unsigned short sectionHash(const char* s) {
    unsigned int h = 2166136261u;
    for (; s && *s; ++s) { h ^= (unsigned char)*s; h *= 16777619u; }
    return (unsigned short)(h ^ (h >> 16));
}

bool fillRow(C2Row& r, Item* it, unsigned short section, int x, int y, int w, int h, int nest) {
    GameData* gd = it->getGameData();
    if (!gd) return false;
    memset(&r, 0, sizeof(r));
    strncpy(r.sid, gd->stringID.c_str(), sizeof(r.sid) - 1);
    int q = it->quantity;
    r.qty = q < 1 ? 1 : (unsigned int)q;
    r.quality = it->quality;
    r.section = section;
    r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
    r.nest = (short)nest;
    r.item = it;
    return true;
}

// POD only, so it can carry the SEH frame.
unsigned int walkSections(Inventory* inv, C2Row* out, unsigned int n, unsigned int maxOut, int nest) {
    if (!inv || !g_getSectionsFn) return n;
    lektor<InventorySection*>* secs = g_getSectionsFn(inv);
    unsigned int ns = secs ? secs->size() : 0;
    for (unsigned int s = 0; s < ns; ++s) {
        InventorySection* sec = (*secs)[s];
        if (!sec) continue;
        const unsigned short sh = sectionHash(sec->name.c_str());
        const Ogre::vector<InventorySection::SectionItem>::type& its = sec->items;
        const unsigned int ni = (unsigned int)its.size();
        for (unsigned int i = 0; i < ni; ++i) {
            Item* it = its[i].item;
            if (!it || n >= maxOut) continue;
            if (fillRow(out[n], it, sh, its[i].x, its[i].y, its[i].w, its[i].h, nest)) {
                ++n;
                if (nest < 2) {
                    Inventory* sub = it->getInventory();
                    if (sub && sub != inv) n = walkSections(sub, out, n, maxOut, nest + 1);
                }
            }
        }
    }
    return n;
}

} // namespace

unsigned int readC2Rows(RootObject* ro, C2Row* out, unsigned int maxOut) {
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    __try {
        return walkSections(inv, out, 0, maxOut, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- engine paths ------------------------------------------------------------------------------

namespace {
bool g_c2Paths = false;
const unsigned int C2_MAX_PATHS = 64;
C2PathCount g_paths[C2_MAX_PATHS];
unsigned int g_pathCount = 0;
}

void setC2PathCount(bool on) { g_c2Paths = on; }

void c2NotePath(int kind, void* returnAddress) {
    if (!g_c2Paths) return;
    for (unsigned int i = 0; i < g_pathCount; ++i) {
        if (g_paths[i].kind == kind && g_paths[i].ret == returnAddress) { ++g_paths[i].count; return; }
    }
    if (g_pathCount < C2_MAX_PATHS) {
        g_paths[g_pathCount].kind = kind;
        g_paths[g_pathCount].ret = returnAddress;
        g_paths[g_pathCount].count = 1;
        ++g_pathCount;
    }
}

unsigned int readC2Paths(C2PathCount* out, unsigned int maxOut) {
    unsigned int n = g_pathCount < maxOut ? g_pathCount : maxOut;
    for (unsigned int i = 0; i < n; ++i) out[i] = g_paths[i];
    return n;
}

} // namespace engine
} // namespace coop
