// Monte Carlo tree search over real duel states.
//
// Each iteration rebuilds a duel (Duel::replay), walks the tree choosing actions with UCB,
// plays the rest of the turn with a cheap policy, and scores the position with the
// Evaluator. Decisions are keyed by what they mean ("Activate X", "Select Y") rather than
// by index, so one tree is shared across determinizations of hidden information
// (single-observer information-set MCTS).
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "deck.h"
#include "duel.h"
#include "evaluate.h"

namespace ygoe {

class PieceGuide;
class Policy;

// One concrete state to search from: a duel setup plus the responses already played.
struct RootState {
	DuelSetup setup;
	std::vector<std::vector<uint8_t>> history;
};

struct Step {
	uint8_t player = 0;
	DecisionType type = DecisionType::None;
	std::string label;
	bool minor = false;
	// An activation answering the other player's chain link: interaction, not part of the
	// player's own combo (describe() marks it "Answer: ").
	bool answer = false;
};

struct Trajectory {
	size_t root = 0;                          // index into the RootState list
	std::vector<std::vector<uint8_t>> responses;  // after the root's history
	std::vector<Step> steps;
	double reward = 0;
};

// What deck guides say about playing a deck (from the app's guide videos): its starters and
// extenders, the cards to search first, and the cards to keep (not to spend as cost or
// material). It biases the searching player's playout prior; the search can still go elsewhere.
struct CardPriorities {
	std::vector<uint32_t> starters;      // script ids
	std::vector<uint32_t> extenders;
	std::vector<uint32_t> search_first;  // best first
	std::vector<uint32_t> protect;
	// Bridge cards (reach.h): they take the deck into another of its archetypes. Their plays are
	// tried first, they are searched before others and kept rather than spent.
	std::vector<uint32_t> bridges;       // strongest first
	bool empty() const {
		return starters.empty() && extenders.empty() && search_first.empty() && protect.empty() && bridges.empty();
	}
	// The playout prior's multiplier for one of the player's own options.
	double factor(const Decision& d, const Option& o, bool own_turn) const;
};

struct SearchConfig {
	int iterations = 4000;
	double seconds = 30;
	int threads = 0;              // 0 = hardware concurrency
	int horizon_turns = 1;        // stop when this many new turns have started
	double exploration = 0.6;
	double prior_bias = 0.25;     // weight of the playout policy as a fading bonus in UCB
	double max_blend = 0.5;       // share of the best-line value (vs the average) in UCB
	int max_rollout_steps = 600;
	int keep_trajectories = 6;    // best trajectories kept per first move
	const std::atomic<bool>* cancel = nullptr;  // set to true to stop early (results so far are kept)
	// Combo pieces to try (pieces.h): in `guided_share` of the iterations the searching player
	// is steered along a plan chained from these pieces, which the search then improves on.
	std::shared_ptr<const PieceGuide> guide;
	// Choices players make in replays that the engine doesn't understand yet (explain.h): inside
	// the effect of `action` (script id), choose `choose`. Strongly preferred for the searching player.
	struct Imitation {
		uint32_t action = 0, choose = 0;
	};
	std::vector<Imitation> imitate;
	double guided_share = 0.7;
	// Learned move policy (policy.h): multiplies the playout prior of every option.
	std::shared_ptr<const Policy> policy;
	// Deck-guide advice for the searching player (see CardPriorities).
	std::shared_ptr<const CardPriorities> priorities;
};

struct MoveStats {
	std::string label;
	int visits = 0;
	double mean = 0;
	std::vector<Trajectory> best;  // best complete trajectories through this move
};

struct SearchOutput {
	std::vector<MoveStats> moves;     // answers to the root decision, most visited first
	std::vector<MoveStats> my_moves;  // grouped by the searching player's first real action, most played first
	int iterations = 0;
	double seconds = 0;
	int failures = 0;
	uint8_t root_player = 0;
	DecisionType root_type = DecisionType::None;
};

class Searcher {
public:
	Searcher(const CardDB& db, ScriptCache& scripts, const Evaluator& eval, std::vector<uint32_t> declare_pool);
	// `me` maximises; the other side minimises (adversarial on open information).
	SearchOutput run(const std::vector<RootState>& roots, uint8_t me, const SearchConfig& cfg,
	                 const std::function<void(int, double)>& progress = {});
	// Moves `player`'s Set actions in `t` to the end of their Main Phase, if the reordered line
	// replays and scores at least as well. Returns true when `t` was changed.
	// Also drops Battle Phases that pass straight to Main Phase 2.
	bool defer_sets(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns);
	// Removes Normal Summons / monster Sets that add nothing (the line scores as well without
	// them), e.g. summoning a hand trap with no use for the body.
	bool drop_idle_summons(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns);
	// Moves each Normal Summon (and monster Set) to the latest point of its Main Phase where the
	// line still works as well: just before the first action that needs that monster.
	bool defer_summons(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns);
	// Replays t.steps by their labels from `root`, filling in responses and reward. Fails if a
	// step is no longer available or the result scores below `min_reward`.
	// With `lenient`, chain windows the steps don't answer are passed, and a "__decline__" step
	// takes the decision's pass / "no" answer.
	bool replay_labels(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns, double min_reward,
	                   bool lenient = false);
	// Removes `player`'s optional responses (chain activations, "use this effect?" yes) that add
	// nothing: the line scores as well holding the card, e.g. a hand trap on your own card or
	// on a monster that has already done its job. Holding the card keeps it for a better time.
	bool drop_idle_responses(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns);
	// The turn planner. Improves a whole line of `me`'s turn: at each of `me`'s choices in it, every
	// other answer is tried, the rest of the turn played (first along the line's own later steps,
	// then also freely), and a line that ends better replaces it. The other player passes where it
	// can (the bot plans again when they interrupt). Stops after `seconds`. True if `t` changed.
	bool improve_line(const RootState& root, Trajectory& t, uint8_t me, const SearchConfig& cfg, double seconds);
	// Takes out `player`'s Main Phase plays that do nothing for the end of the turn (the line scores
	// as well without them): an effect activated just because it can be, a Link Summon that leads
	// nowhere, a Normal Spell set for no reason. True if `t` changed.
	bool drop_idle_plays(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns);
	// Replays a trajectory and returns the board where it stopped.
	bool final_board(const RootState& root, const Trajectory& t, Board& out, Duel** keep = nullptr);

private:
	const CardDB& db_;
	ScriptCache& scripts_;
	const Evaluator& eval_;
	std::vector<uint32_t> pool_;
};

// Prior weight of an option in the playout policy (also the Easy bot's move choice).
double playout_weight(const Decision& d, const Option& o, uint8_t turn_player, int steps_this_turn);

// Labels of the non-trivial steps a player took, merged into readable lines.
// Steps by `player` are written plainly, the other side's with `other_prefix`.
std::vector<std::string> describe(const Trajectory& t, uint8_t player, bool include_other = false,
                                  const std::string& other_prefix = "Opp: ");
// The first non-trivial action `player` takes in a trajectory ("Pass" if none).
std::string first_action(const Trajectory& t, uint8_t player);

}  // namespace ygoe
