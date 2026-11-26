// A deck's own learning, on the person's computer (the app's Learn screen, Windows).
//
// The nightly pipeline (tools/ml) learns one network and one move policy for the whole format.
// Here one deck learns its own, starting from those: in rounds until the time is up,
//  1. Self-play: the deck plays best-of-3 matches against the opponents (the format's decks by
//     default), sided from game 2, the loser choosing to go first or second. The deck's bot uses
//     the deck's network and policy, the opponents' the format's. Every Main Phase position is
//     kept from both sides, and the deck's bot's searches as policy examples.
//  2. Training: a network on the deck's positions (this round's and the earlier rounds', the
//     latest 200k), continuing from the deck's network, the C++ port of tools/ml/train.py
//     (nettrain.h); the policy on its examples. Each must predict the held-out games better than
//     the one it started from (log-loss lower by 0.002).
//  3. Arena: what passed plays the same deals as the current one, the deck played by each in
//     turn against the same opponent's bot; it is kept when it scores at least as well.
// What is kept goes in the deck's folder (value_net.json, policy.json), which duels and
// analyses of that deck use (api "learned"); learn.json there has every round's report.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "deck.h"
#include "pieces.h"

namespace ygoe {

class CardDB;
class ScriptCache;

struct LearnOpponent {
	std::string name;
	Deck deck;
};

struct LearnConfig {
	Deck deck;
	std::string name;
	std::vector<LearnOpponent> opponents;
	std::string dir;               // the deck's folder (created)
	double minutes = 30;           // the whole session
	int iterations = 200;          // search iterations of each bot decision
	int turns = 12;                // games cut there (scored by the evaluator)
	uint64_t seed = 1;
	std::string base_net;          // the format's network and policy (JSON), where the deck starts
	std::string base_policy;
	std::vector<LibraryPiece> library;  // the deck's combo pieces (pieces.h), optional
	const std::atomic<bool>* cancel = nullptr;
	std::function<void(const std::string& phase, int done, int total)> progress;
};

// Playing the format's games for the nightly learning (the app's "Play the format's games"): the
// nightly run's own self-play on the person's computer, usually with a deeper search. Two of the
// format's decks at random, best of 3, the format's network and policy, each deck with the pieces
// of the shared pool it can play. Each game goes to dir/positions.jsonl and
// policy_examples.jsonl as it ends (both players' searches), for the app to upload.
struct ContributeConfig {
	std::vector<LearnOpponent> decks;
	std::string dir;
	double minutes = 60;
	int iterations = 800;
	int turns = 12;
	uint64_t seed = 1;
	std::string base_policy;            // the format's policy (JSON)
	std::vector<LibraryPiece> pool;     // the shared pool of combo pieces (combo-pool.json)
	const std::atomic<bool>* cancel = nullptr;
	std::function<void(const std::string& phase, int done, int total)> progress;
};
std::string contribute_selfplay(const CardDB& db, ScriptCache& scripts, const ContributeConfig& cfg);

// Deepening the combo lines (the app's "Deepen combo lines"): the decks' opening hands searched
// deeply in batches (reference.h), each batch's new pieces saved as soon as it is done, as
// dir/newpieces/*.json in the shape the nightly pool merge reads (ygoe-cli library --new-out).
struct DeepenConfig {
	std::vector<LearnOpponent> decks;
	std::string dir;
	double minutes = 60;
	int hands_per_batch = 10;
	int iterations = 3000;
	uint64_t seed = 1;
	// Real end boards (data/end_boards.jsonl: real games at the start of turn 2): 40% of the hands
	// are played going second against one of them. Empty: all going first.
	std::string end_boards;
	const std::atomic<bool>* cancel = nullptr;
	std::function<void(const std::string& phase, int done, int total)> progress;
};
std::string deepen_lines(const CardDB& db, ScriptCache& scripts, const DeepenConfig& cfg);

// Runs the session; returns its report (JSON): the rounds, games, what was kept.
std::string learn_deck(const CardDB& db, ScriptCache& scripts, const LearnConfig& cfg);

// The deck's learning so far (its folder's learn.json), "{}" when none.
std::string learn_status(const std::string& dir);

}  // namespace ygoe
