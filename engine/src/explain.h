// Why a line makes its choices.
//
// A line from a replay or a video is a person's play. At each choice inside it (what a card
// searches, discards, summons or places; which option of an effect), the engine replays the
// position with every other option and searches the best continuation after each, then
// compares the end positions with the line's own choice. Where the line's choice comes out on
// top the engine knows why (what it ends with that the next best option doesn't); where an
// alternative scores higher, the engine doesn't understand that choice yet: either the player
// erred or the engine misjudges something, which is worth a look.
#pragma once
#include <atomic>
#include <string>
#include <vector>

#include "analyze.h"
#include "carddb.h"
#include "deck.h"
#include "duel.h"
#include "search.h"

namespace ygoe {

struct ChoiceOption {
	std::string label;
	uint32_t code = 0;     // script id of the option's card (0 if none)
	double value = 0;      // net value (yours minus the opponent's) after the best continuation found
	bool chosen = false;   // the line's choice
	std::vector<CardSummary> mine, theirs;  // field + hand at the end
};

struct ChoiceExplanation {
	int step = 0;                    // index in the line's steps
	std::string action;              // the action the choice belongs to (the last action before it)
	uint32_t action_code = 0;        // script id of that action's card
	std::string chosen;              // the line's choice
	uint32_t chosen_code = 0;
	std::vector<ChoiceOption> options;  // best first
	double margin = 0;               // chosen value minus the best other option (negative: an option scores higher)
	bool understood = false;
	std::string why;                 // plain words
};

struct ExplainConfig {
	int iterations = 250;            // search per option
	int max_choices = 8;             // choice points looked at per line
	int max_options = 6;             // choices with more options are skipped
	double tolerance = 0.25;         // an alternative must beat the line's choice by this to count as better
	const std::atomic<bool>* cancel = nullptr;
};

// `responses` is the line from `setup` (player 0 going first); `line_net` the net value of its
// own end position. The searching player is 0.
std::vector<ChoiceExplanation> explain_line(const CardDB& db, ScriptCache& scripts, const Deck& deck, const DuelSetup& setup,
                                            const std::vector<uint32_t>& pool, const Trajectory& line, const ExplainConfig& cfg);

// Net value (player 0 minus player 1), deck-neutral.
double net_value(const CardDB& db, const Deck& deck, const Board& b);

std::string choices_json(const CardDB& db, const std::vector<ChoiceExplanation>& c);

}  // namespace ygoe
