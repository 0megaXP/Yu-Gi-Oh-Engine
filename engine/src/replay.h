// Video lines: combos read from videos, replayed and checked by the real rules.
//
// A combo read from a video is a list of steps ("activate Opening to e4", "search Problem",
// "Link Summon Castellan") with an opening hand and an end board. Here it is played in a
// duel from that hand, going first with no opponent interaction, choosing at each of the
// player's actions the option that matches the next video step. Choices inside an effect
// (costs, targets, what to search) are tried several ways, preferring the cards the video
// names, and a small beam keeps the replays that match the most steps.
//
// A line is verified when most of its steps match and the replay ends on most of the
// video's end board. A verified line is then an exact engine line: its steps are real
// options, so it can be split into combo pieces (pieces.h) and steer the search like the
// pieces from reference boards.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "analyze.h"
#include "deck.h"
#include "pieces.h"

namespace ygoe {

struct VideoStep {
	std::string action;   // "activate", "normal_summon", "link_summon", "search", "send_to_gy", ...
	uint32_t code = 0;    // official passcode, 0 when the card wasn't identified
	std::string detail;   // free text: materials, targets, what was searched
};

struct VideoLine {
	std::string id;                    // echoed back (e.g. the video URL)
	std::vector<uint32_t> hand;        // official passcodes
	std::vector<VideoStep> steps;
	std::vector<uint32_t> end_board;   // the video's field cards at the end, official passcodes
};

struct LineCheck {
	std::string id;
	bool verified = false;
	int steps_checked = 0;             // steps with a known card and an action the engine can see
	int steps_matched = 0;
	std::vector<int> step_match;       // per video step: 1 matched, 0 not matched, -1 not checked
	double board_match = 0;            // share of the video's field cards on the replayed board
	std::string problem;               // why it didn't verify ("" when verified)
	double value = 0;                  // deck-neutral board value (as reference boards)
	std::vector<std::string> moves;    // the replayed line
	std::vector<CardSummary> board;    // the replayed end board
	std::vector<ComboPiece> pieces;    // only for verified lines
};

struct LinesCheck {
	std::vector<LineCheck> lines;
	std::vector<LibraryPiece> library;  // pieces of the verified lines, merged
	int verified = 0;
	std::string policy;                 // the move policy after learning from the verified lines (JSON), if one was given
	std::string to_json(const CardDB& db) const;
};

struct ReplayConfig {
	int beam = 8;          // replays kept at each step
	int tries = 6;         // ways to play the choices inside one action
	int window = 3;        // video steps that may be skipped to find a match (misread or merged steps)
	double seconds = 60;   // time cap per line
	uint64_t seed = 1;
	const std::atomic<bool>* cancel = nullptr;
};

class Policy;

// With `policy`, the verified lines are also learned from (policy.h), counting double: they
// are a person's play, checked by the rules.
LinesCheck check_video_lines(const CardDB& db, ScriptCache& scripts, const Deck& deck, const std::vector<VideoLine>& lines,
                             const ReplayConfig& cfg, const std::function<void(int done, int total)>& progress = {},
                             Policy* policy = nullptr);

}  // namespace ygoe
