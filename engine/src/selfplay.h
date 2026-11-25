// Self-play: bots playing each other, for learning (tools/ml, `ygoe-cli selfplay` / `arena`,
// and a deck's own learning in the app, learn.h).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "deck.h"
#include "live.h"

namespace ygoe {

class CardDB;
class ScriptCache;
class ValueNet;
class Policy;
class PieceGuide;
class NeTiming;

// One bot's equipment.
struct Bot {
	std::shared_ptr<const ValueNet> net;
	std::shared_ptr<const Policy> policy;
	std::shared_ptr<const PieceGuide> guide;
	std::shared_ptr<const NeTiming> timing;  // non-engine timing (nonengine.h)
};

struct PlayConfig {
	int iterations = 300;
	int max_turns = 14;
	double explore = 0;
	int explore_moves = 0;
	const Scenario* start = nullptr;  // a real game's position, player 0 the turn player
	int turn_offset = 0;              // that position's turn number minus 1
	double ne_explore = 0;            // non-engine choices turned around (self-play)
	LiveDuel::NonEngineHook ne_hook;
	const std::atomic<bool>* stop = nullptr;  // set: the game ends at the next move (cut)
};

struct Game {
	int winner = -1;     // 0 / 1, -1 draw or cut
	bool finished = false;
	double z0 = 0.5;     // result for player 0: 1, 0, 0.5 for a draw; the win chance when cut
	int lp[2] = {8000, 8000};
	int turns = 0;
	bool stopped = false;  // ended by PlayConfig::stop
};

// Plays one game; `on_position` is called at each Main Phase decision of either player. A game
// cut at the turn limit (or stopped) is scored by the evaluator's win chance there.
Game play_game(const CardDB& db, ScriptCache& scripts, const Deck& d0, const Deck& d1, uint64_t seed, const PlayConfig& pc,
               const Bot& b0, const Bot& b1, const std::function<void(LiveDuel&)>& on_position,
               const LiveDuel::SearchHook& hook = {}, LiveDuel** expose = nullptr);

// A deck sided for one matchup (side.h): the plan's swaps applied. Cached per matchup.
class Siding {
public:
	explicit Siding(const CardDB& db) : db_(db) {}
	const Deck& sided(size_t mine, size_t theirs, bool first, const std::vector<Deck>& decks);

private:
	const CardDB& db_;
	std::map<std::tuple<size_t, size_t, bool>, Deck> cache_;
};

}  // namespace ygoe
