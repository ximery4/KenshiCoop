// Core2Shadow.h - stage 3 of docs/CORE2_DESIGN.md: the book follows the game and only writes a log.
// Off unless KENSHICOOP_CORE2_SHADOW=1. It never changes the game. KENSHICOOP_CORE2_NET=1 also sends the book to joins.

#ifndef KENSHICOOP_CORE2_SHADOW_H
#define KENSHICOOP_CORE2_SHADOW_H

#include <vector>

class GameWorld;

namespace coop {
class NetLink;
namespace core2shadow {

bool enabled();                                      // reads the setting once
void tick(GameWorld* gw, unsigned long nowMs, NetLink& net, bool isHost);   // main thread, world live; scans at its own pace
void receive(unsigned int from, const std::vector<unsigned char>& bytes, unsigned long nowMs);   // main thread

} // namespace core2shadow
} // namespace coop

#endif
