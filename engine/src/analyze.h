// End-to-end analysis of a position: current evaluation, best lines for the player,
// and a check of each candidate end board against the opponent's actual turn.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "deck.h"
#include "evaluate.h"
#include "search.h"

namespace ygoe {

struct AnalysisConfig {
	SearchConfig search;          // phase A: the player's own turn(s)
	int determinizations = 12;    // sampled hidden hands / deck orders
	int lines = 3;                // candidate lines to report
	bool verify = true;           // phase B: play the opponent's next turn against each end board
	int verify_iterations = 400;  // per candidate trajectory
	int verify_trajectories = 2;  // trajectories (different samples) per line
	// Called from a worker thread: phase ("search" / "verify N"), iterations done, iterations planned.
	std::function<void(const std::string&, int, int)> on_progress;
};

struct CardSummary {
	uint32_t code = 0;
	std::string name;
	std::string where;
	std::string note;  // e.g. "negates an effect (0.82)"
};

struct LineReport {
	std::string first_move;
	std::vector<std::string> moves;      // the player's steps, in order
	double own_turn_score = 0;           // phase A: static score of the end board
	double after_opponent = -1;          // phase B: score after the opponent's best turn (-1 = not run)
	double combined = 0;
	double board_value = 0;              // deck-neutral value, comparable with reference boards
	int visits = 0;
	std::vector<CardSummary> end_board;  // the player's cards on field / set / in hand
	std::vector<CardSummary> interrupts; // what the board can stop, valued against this opponent
	std::vector<std::string> opponent_reply;  // the opponent's strongest turn found, with our responses
	std::vector<CardSummary> board_after;     // the player's cards after the opponent's turn
};

struct AnalysisResult {
	uint8_t me = 0;
	std::string mode;
	double current_score = 0;
	std::vector<CardSummary> current_interrupts;
	DeckProfile my_profile, opp_profile;
	std::vector<LineReport> lines;
	int iterations = 0;
	double seconds = 0;
	std::string to_json() const;
	std::string to_text() const;
};

AnalysisResult analyze(const CardDB& db, ScriptCache& scripts, const Scenario& sc, const AnalysisConfig& cfg,
                       const std::function<void(const std::string&)>& log = {});

std::vector<CardSummary> summarize_cards(const CardDB& db, const Board& b, uint8_t player, bool include_hand = true);

}  // namespace ygoe
