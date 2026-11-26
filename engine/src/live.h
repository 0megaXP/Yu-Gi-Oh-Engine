// A duel played one decision at a time: by a person (the app's duel screen) or by the bot.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "deck.h"
#include "duel.h"
#include "evaluate.h"
#include "guess.h"
#include "search.h"

namespace ygoe {

enum class BotLevel { Easy, Normal, Strong };

namespace json { class Value; }
// A saved simulator duel (the app's replay file, or LiveDuel::setup_json()): its setup.
bool setup_from_json(const json::Value& v, DuelSetup& out, std::string* error = nullptr);
std::string hex(const std::vector<uint8_t>& bytes);
bool unhex(const std::string& s, std::vector<uint8_t>& out);

class LiveDuel {
public:
	// The other player's Main Deck cards `me` has seen: on the field face-up, in the GY or
	// banished, and the hand cards it knows (script id -> copies).
	std::map<uint32_t, int> seen_cards(uint8_t me) const;
	// What `me` makes of the other player's deck from those (guess.h); empty without a library.
	DeckGuess deck_guess(uint8_t me) const;
	// The decks the bots guess from (default: the CardDB's). Null: they draw the other player's
	// unseen cards from their real list.
	void set_deck_library(std::shared_ptr<const DeckLibrary> lib) { library_ = std::move(lib); library_set_ = true; }
	// Player 0 goes first. Both players start with 5 random cards from their deck.
	LiveDuel(const CardDB& db, ScriptCache& scripts, const Deck& deck0, const Deck& deck1, uint64_t seed);
	// A duel from a given position (a board taken from a real game): `sc` holds both decks,
	// hands, the cards on the field / in the GY / banished and LP, player 0 being the one whose
	// turn it is. `flags` are the rules core's duel flags (0: Master Rule 5); a position from
	// later in a game needs DUEL_ATTACK_FIRST_TURN so the turn player can attack.
	LiveDuel(const CardDB& db, ScriptCache& scripts, const Scenario& sc, uint64_t flags);
	// A saved simulator duel (setup_from_json) played up to its first `answers.size()` answers,
	// to ask the bot what it would do there (tests, simreplay --bot). ok() is false if an
	// answer isn't legal any more.
	LiveDuel(const CardDB& db, ScriptCache& scripts, const DuelSetup& setup, const std::vector<std::vector<uint8_t>>& answers);
	// The player a person controls (-1 = none). Their zone choices are asked, not answered automatically.
	void set_human(int player) { human_ = player; }
	bool ok() const { return ok_; }
	const std::string& error() const { return error_; }

	// The whole visible state as JSON (both hands included; the app hides what it should).
	// Log lines produced since the previous call are included once.
	std::string state_json();
	// Answers the current decision with option `index`. False if it is not a legal answer.
	bool respond(size_t index);
	// Lets the bot answer the current decision. Returns the chosen label ("" if nothing to do).
	std::string bot_move(BotLevel level);
	void cancel() { cancel_ = true; }
	// Combo pieces for a player's bot (from that deck's reference boards), see pieces.h.
	// The deck's plan read from them (PieceGuide::plan): a grind deck values the cards it keeps.
	void set_guide(int player, std::shared_ptr<const PieceGuide> g);
	// Learned move policy (policy.h) for both bots' searches.
	void set_policy(std::shared_ptr<const class Policy> p) { policies_[0] = policies_[1] = std::move(p); }
	// One player's bot only (two bots with different policies, to compare them).
	void set_policy(int player, std::shared_ptr<const class Policy> p) { policies_[player & 1] = std::move(p); }
	// Exploration for self-play: in each player's first `moves` Main / Battle Phase decisions
	// the bot plays, with probability `p`, another well-explored move instead of its best one
	// (so the games don't all follow the same lines).
	void set_exploration(double p, int moves) {
		explore_p_ = p;
		explore_moves_ = moves;
	}
	// Called after every search a bot makes, with the decision and what the search found (the
	// visits of each answer), before the move is chosen. Self-play uses it to train the policy.
	using SearchHook = std::function<void(const Decision&, const SearchOutput&)>;
	void set_search_hook(SearchHook h) { search_hook_ = std::move(h); }
	// Non-engine timing (nonengine.h) for a player's bot: unset, the card database's. Null: none.
	void set_nonengine_timing(int player, std::shared_ptr<const class NeTiming> t) {
		ne_timing_[player & 1] = std::move(t);
		ne_timing_set_[player & 1] = true;
	}
	// Self-play: at each chance to use a non-engine card on the opponent's play, the bot's choice
	// is turned around with probability `p` (so both choices are seen), and the hook gets the
	// situation's features (nonengine.h) and whether the card was used.
	using NonEngineHook = std::function<void(uint8_t player, const std::vector<float>& features, bool used)>;
	void set_nonengine_exploration(double p, NonEngineHook h) {
		ne_explore_ = p;
		ne_hook_ = std::move(h);
	}
	// What the move policy sees at the current decision (policy.h).
	int steps_this_turn() const { return steps_this_turn_; }
	// A player's learned value network (valuenet.h): their bot's evaluation uses it. Unset: the
	// card database's (data/value_net.json), if any. Set null explicitly for none.
	void set_value_net(int player, std::shared_ptr<const class ValueNet> n) {
		nets_[player & 1] = std::move(n);
		net_set_[player & 1] = true;
	}
	// The search budget of a bot decision (0: the level's own). Self-play uses less to play more games.
	void set_iterations(int n) { iterations_ = n; }
	// Search threads (0: the default, every core up to 8).
	void set_threads(int n) { threads_ = n; }
	const class Evaluator& evaluator() const { return *eval_; }
	// What is known about a player's deck from past games (replays): the cards where it is
	// usually stopped, and how many plays it makes in a turn. The other bot uses it to time its
	// interruptions: on a choke point, or straight away when it holds as many interruptions as
	// that deck has plays left.
	struct DeckKnowledge {
		std::vector<uint32_t> chokes;  // script ids
		double plays = 0;              // average activations in one of its turns; 0 = unknown
	};
	void set_knowledge(int player, DeckKnowledge k) { knowledge_[player & 1] = std::move(k); }
	// Choices players make in replays that the engine doesn't understand yet, for a player's bot
	// (SearchConfig::imitate): followed unless its search finds something clearly better.
	void set_imitate(int player, std::vector<SearchConfig::Imitation> v) { imitate_[player & 1] = std::move(v); }
	// Deck-guide advice for a player's bot (SearchConfig::priorities), or null.
	void set_priorities(int player, std::shared_ptr<const CardPriorities> p) { priorities_[player & 1] = std::move(p); }

	const Duel& duel() const { return *duel_; }
	// For saving the duel as a replay: how it was set up (decks in order, hands, seed) and the
	// answers given so far, hex. state_json() carries the setup once and each new answer once.
	std::string setup_json() const;
	const std::vector<std::vector<uint8_t>>& history() const { return history_; }

private:
	void begin(const Deck& d0, const Deck& d1);
	bool respond_internal(size_t index);
	void auto_resolve();
	int choose(BotLevel level);
	// The plan's next non-Set, non-Normal-Summon play available now, when plan_[at] is one of those (-1: none).
	int later_play(const Decision& d, size_t at) const;
	// Moves the plan's next Main Phase play `label` (after `at`) to `at`. False if not in the plan.
	bool bring_forward(const std::string& label, size_t at);
	// At the end of Main Phase 1: the Battle Phase option when a monster can attack through (-1: none).
	int battle_instead(const Decision& d) const;
	// Roots for the bot's search that hide what it can't see: in each, the other player's hand
	// cards it doesn't know (drawn, never shown) are swapped with random cards of their Deck,
	// and both Decks are shuffled (DuelSetup::Hidden, applied after the history's replay).
	std::vector<RootState> hidden_roots(uint8_t me, int samples);
	// Updates known_ after a response (hand cards drawn are hidden, the rest was seen).
	void track_knowledge();
	// `label` Normal Summons a hand trap that no later step of `line` (from `from`) uses.
	bool unused_summon(const Decision& d, const std::string& label, const std::vector<Step>& line, size_t from) const;
	// `p` has a face-up card that can still do something this turn (worth negating).
	bool live_target(uint8_t p) const;
	// `p` has a face-up card with a lasting effect (a lock, a continuous effect): nothing to wait for,
	// it can only be negated while it is there.
	bool lasting_target(uint8_t p) const;
	// Per player: their deck can take its own monster off the field as a Quick Effect (Elfnote
	// Regina sending a face-up Elfnote as cost), so a negate that waits for the activation can be
	// dodged: then it is used before.
	bool dodges_[2] = {false, false};
	std::string card_json(uint32_t code) const;

	const CardDB& db_;
	ScriptCache& scripts_;
	Scenario sc_;
	DuelSetup setup_;
	std::unique_ptr<Duel> duel_;
	std::unique_ptr<Evaluator> eval_;
	std::shared_ptr<const class ValueNet> nets_[2];
	bool net_set_[2] = {false, false};
	int iterations_ = 0;
	int threads_ = 0;
	std::unique_ptr<Searcher> searcher_;
	std::vector<std::vector<uint8_t>> history_;
	bool setup_sent_ = false;
	size_t history_sent_ = 0;
	std::vector<std::string> log_;
	int human_ = -1;
	std::vector<int8_t> log_players_;
	std::vector<Duel::Move> moves_;
	std::vector<Duel::Activation> activations_;
	std::vector<Duel::Pick> picks_;
	std::string last_move_;
	// The bot's current plan: the expected sequence of decision labels (both players).
	std::vector<Step> plan_;
	size_t plan_pos_ = 0;
	// Whose plan it is. The plan also holds the moves that bot expected from the other side
	// (often random playout moves); those only track whether play still follows it and are
	// never played for the other side's bot.
	int plan_player_ = -1;
	// The other bot's plan while this one decides (two bots, one each): swapped in by choose().
	std::vector<Step> other_plan_;
	size_t other_pos_ = 0;
	int other_player_ = -1;
	std::shared_ptr<const PieceGuide> guides_[2];
	std::shared_ptr<const class Policy> policies_[2];
	double explore_p_ = 0;
	int explore_moves_ = 0;
	int explored_[2] = {0, 0};  // Main / Battle Phase searches made so far, per player
	SearchHook search_hook_;
	std::shared_ptr<const class NeTiming> ne_timing_[2];
	bool ne_timing_set_[2] = {false, false};
	double ne_explore_ = 0;
	NonEngineHook ne_hook_;
	int steps_this_turn_ = 0;
	uint32_t steps_turn_ = 0;
	DeckKnowledge knowledge_[2];
	std::shared_ptr<const CardPriorities> priorities_[2];
	std::vector<SearchConfig::Imitation> imitate_[2];
	uint32_t last_action_[2] = {0, 0};  // the last card each player activated or summoned
	// Cards in each player's hand that the other player knows (script id -> copies), and the
	// hand as it was at the last update.
	std::map<uint32_t, int> known_[2];
	std::shared_ptr<const DeckLibrary> library_;
	bool library_set_ = false;
	const DeckLibrary* library() const { return library_set_ ? library_.get() : db_.deck_library().get(); }
	std::map<uint32_t, int> last_hand_[2];
	std::mt19937_64 rng_;
	std::atomic<bool> cancel_{false};
	bool ok_ = false;
	bool replaying_ = false;
	// What was left of the bot's plan when play left it (a draw, a response it didn't expect):
	// tried again at the next search of that turn.
	std::vector<Step> stale_plan_;
	uint32_t stale_turn_ = 0;
	uint8_t stale_player_ = 0;
	std::string error_;
};

}  // namespace ygoe
