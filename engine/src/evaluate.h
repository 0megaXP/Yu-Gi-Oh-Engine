// Static evaluation of a position, tuned to the opponent's deck profile.
#pragma once
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "duel.h"
#include "profile.h"

namespace ygoe {

struct BoardNote {
	uint8_t player = 0;
	uint32_t code = 0;
	std::string where;
	std::string what;
	double value = 0;
};

struct Evaluation {
	double score = 0;            // reward for `me`, 0..1
	double raw = 0;              // value(me) - value(opp)
	double value[2] = {0, 0};
	std::vector<BoardNote> notes;  // interruptions each side holds, best first
};

class Evaluator {
public:
	Evaluator(const CardDB& db, const Deck& deck0, const Deck& deck1);
	// `next_player` is who takes the next turn; the other side's interruptions are what count.
	Evaluation evaluate(const Board& b, uint8_t me, uint8_t next_player, bool ended = false, int winner = -1,
	                    bool want_notes = false) const;
	const DeckProfile& profile(uint8_t p) const { return profile_[p]; }
	// Replaces what a side is assumed to rely on (e.g. generic_profile() for deck-neutral scores).
	void set_profile(uint8_t p, const DeckProfile& prof) { profile_[p] = prof; }
	const CardRole& role(uint8_t owner, uint32_t code) const;

private:
	const CardDB& db_;
	DeckProfile profile_[2];
	std::vector<uint16_t> sets_[2];
	mutable std::mutex mu_;
	mutable std::unordered_map<uint32_t, CardRole> roles_[2];
};

}  // namespace ygoe
