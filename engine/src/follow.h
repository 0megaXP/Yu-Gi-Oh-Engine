// Following a recorded duel (a YGO Omega replay) in the real rules core.
//
// A replay keeps what the rules core announced to the player who saved it (classic YGOPro
// messages: every move, summon, activation, draw, attack and Life Point change), and the
// decks, but not the players' answers or the shuffles. To rebuild the game here:
//  - the Decks are stacked so the recorded draws come in their order (the core is told not to
//    shuffle), and cards that leave a Deck from its top (draws, excavations) are kept in that
//    order; the opponent's hidden cards are known in hindsight (a card drawn face-down is the
//    one later played from that hand), the rest are cards of their list nobody saw;
//  - at each decision of either player the option is chosen whose result matches what the
//    replay shows next (the next summon, activation, move...), backing up a few decisions when a
//    choice only shows its effect later;
//  - when a card returns to a Deck, the cards drawn next are put back on top.
// Where the game can't be followed any more (a card that works differently in YGO Omega, a coin
// flip, a card never revealed that mattered) it stops, and what was followed is kept.
//
// Along the way the positions are written for the value network (tools/ml), in the form
// `ygoe-cli selfplay` writes them: at the turn player's Main and Battle Phase decisions and where
// a player answers with an activation, a few per turn, with the exact board of the rules core.
#pragma once
#include <string>
#include <vector>

#include "deck.h"

namespace ygoe {

class CardDB;
class ScriptCache;

struct FollowInput {
	// The replay's messages, in order (classic YGOPro format, the saving player's view).
	std::vector<std::vector<uint8_t>> messages;
	// The players' lists (official passcodes) when the replay keeps them, as recorded players 0/1.
	bool has_deck[2] = {false, false};
	Deck decks[2];
	std::string game;      // written with the positions (keeps a game's positions together)
	int per_turn = 6;      // positions kept per turn at most
	int max_trials = 3000; // options tried in all (a bound on the time a game takes)
	int max_ms = 60000;    // time a game may take; past it, what was followed is kept
	int max_idle = 300;    // moves in a row that match nothing the replay shows: it stops there
};

// Raised whenever the follower follows more of the games (or fixes what it writes): the app sends
// its games again when the engine's version is newer than the one they were sent with.
//   2: stand-in Extra Deck cards, Deck reordering optional (Pot of Prosperity), the positions
//      lost by version 1's start positions back
constexpr int kFollowVersion = 2;

struct FollowResult {
	std::string jsonl;          // training positions, one JSON object per line
	int positions = 0;
	int events = 0;             // what the replay shows (moves, summons, activations, ...)
	int followed = 0;           // of those, reproduced by the rules core
	int turns = 0, turns_followed = 0;
	bool complete = false;      // followed to the end
	int winner = -1;            // recorded player 0/1, -1 when the replay doesn't say
	std::string stopped;        // why it stopped ("" when complete)
	int stop_turn = 0;          // the turn it stopped in (1 = the first)
	std::string stop_next;      // the recorded events it couldn't reproduce from there (card ids)
	std::string to_json() const;
};

FollowResult follow_replay(const CardDB& db, ScriptCache& scripts, const FollowInput& in);

// A check of the follower without YGO Omega: a game of random choices between two decks
// (passcodes) is recorded as player 0 would have seen it (classic messages, the opponent's
// hidden cards without codes), then followed. Returns the follower's result as JSON.
std::string follow_selftest(const CardDB& db, ScriptCache& scripts, const Deck& d0, const Deck& d1, uint64_t seed, int turns);

}  // namespace ygoe
