// Learned move policy: which options strong play picks, learned from good lines.
//
// The search's playout prior (policy_weight in search.cpp) is a few hand-written rules. This
// adds a learned correction on top: every option is described by general features (what kind
// of decision and option it is, where the card is, the card's type and what its effects do,
// what the selection is for, and the card itself), and a log-linear model trained on the
// choices made in good lines (deep searches, verified video lines) scores them. The playout
// weight becomes  rule weight × exp(score), so options strong lines prefer get tried first.
//
// Most features are not tied to a card, so what is learned on one deck carries over to
// others ("a search effect from the hand early in the turn"); the card-specific features let
// a deck's own patterns be learned too.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "carddb.h"
#include "duel.h"

namespace ygoe {

class ScriptCache;
struct DuelSetup;
namespace json { class Value; }
struct Trajectory;

// What the policy sees at a decision besides the options.
struct PolicyContext {
	uint8_t turn_player = 0;
	int steps_this_turn = 0;   // responses since the turn began (as the search counts them)
	int chain_depth = 0;
};

// One decision from a good line: every option's features and the one that was chosen.
struct PolicyExample {
	std::vector<std::vector<uint32_t>> options;  // feature ids per option
	std::vector<float> prior;                    // the rule weight of each option
	int chosen = 0;
	float weight = 1;                            // how much this example counts
	// Instead of one chosen option: how much each option should get (sums to 1), e.g. the share
	// of a deep search's visits it got. Empty: `chosen` is the target.
	std::vector<float> target;
};

class Policy {
public:
	static constexpr uint32_t kDim = 1u << 16;

	explicit Policy(const CardDB& db);

	// Feature ids of one option (cached per card; cheap enough for playouts).
	void features(const Decision& d, const Option& o, const PolicyContext& c, std::vector<uint32_t>& out) const;
	// exp(score), clamped to [1/8, 8]; 1 for an untrained policy.
	double factor(const Decision& d, const Option& o, const PolicyContext& c) const;

	// Trains on `examples` (added to what was learned before). Returns the average log-loss
	// before and after, so callers can see whether it learned anything.
	std::pair<double, double> train(const std::vector<PolicyExample>& examples, int epochs = 8, double rate = 0.1,
	                                double l2 = 1e-4);

	// Adds another policy's weights to this one's (two learned corrections in one), times `scale`.
	void add(const Policy& other, double scale = 1.0);
	// How much of the policy learned on GitHub from self-play searches (data/policy.json) the
	// bots use: half its weights, so it nudges the search rather than steering it while it is
	// still learning from the bots' own searches.
	static constexpr double kSelfPlayScale = 0.5;
	// The average log-loss on `examples` (lower is better).
	double loss_of(const std::vector<PolicyExample>& examples) const { return loss(examples); }

	bool trained() const { return trained_; }
	int examples_seen() const { return seen_; }

	std::string to_json() const;
	bool from_json(const std::string& text);
	bool from_value(const class json::Value& v);

	// The decisions `player` made in a line, as examples. The line is replayed from `setup`
	// (with `history` before it); minor and single-option decisions are skipped.
	std::vector<PolicyExample> examples_from_line(ScriptCache& scripts, const std::vector<uint32_t>& declare_pool,
	                                              const DuelSetup& setup, const std::vector<std::vector<uint8_t>>& responses,
	                                              uint8_t player, float weight = 1) const;

private:
	double score(const std::vector<uint32_t>& f) const;
	double loss(const std::vector<PolicyExample>& ex) const;

	const CardDB& db_;
	std::vector<float> w_;
	bool trained_ = false;
	int seen_ = 0;
	// What each card is (type, level, what its effects do, which card): computed once, read-only
	// afterwards, so searching threads share it without locks.
	std::unordered_map<uint32_t, std::vector<uint32_t>> tokens_;
};

struct Deck;

// Good lines from the engine itself: random opening hands of `deck` going first (goldfish),
// each searched deeply; the best line's decisions become examples.
std::vector<PolicyExample> self_play_examples(const CardDB& db, ScriptCache& scripts, const Deck& deck, const Policy& policy,
                                              int hands, int iterations, uint64_t seed,
                                              const std::atomic<bool>* cancel = nullptr);

// The rule-based playout weight (search.cpp), for training against it.
double playout_weight(const Decision& d, const Option& o, uint8_t turn_player, int steps_this_turn);

}  // namespace ygoe
