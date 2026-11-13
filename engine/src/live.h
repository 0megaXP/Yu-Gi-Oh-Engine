// A duel played one decision at a time: by a person (the app's duel screen) or by the bot.
#pragma once
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "deck.h"
#include "duel.h"
#include "evaluate.h"
#include "search.h"

namespace ygoe {

enum class BotLevel { Easy, Normal, Strong };

class LiveDuel {
public:
	// Player 0 goes first. Both players start with 5 random cards from their deck.
	LiveDuel(const CardDB& db, ScriptCache& scripts, const Deck& deck0, const Deck& deck1, uint64_t seed);
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
	void set_guide(int player, std::shared_ptr<const PieceGuide> g) { guides_[player & 1] = std::move(g); }
	// Learned move policy (policy.h) for both bots' searches.
	void set_policy(std::shared_ptr<const class Policy> p) { policy_ = std::move(p); }
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

	const Duel& duel() const { return *duel_; }

private:
	bool respond_internal(size_t index);
	void auto_resolve();
	int choose(BotLevel level);
	// Roots for the bot's search that hide the other player's hand: on turn 1 the cards still in
	// their hand are re-drawn from their deck (keeping what they already used), each sample
	// checked to replay to the same public position. Falls back to the true state.
	std::vector<RootState> hidden_roots(uint8_t me, int samples);
	std::string card_json(uint32_t code) const;

	const CardDB& db_;
	ScriptCache& scripts_;
	Scenario sc_;
	DuelSetup setup_;
	std::unique_ptr<Duel> duel_;
	std::unique_ptr<Evaluator> eval_;
	std::unique_ptr<Searcher> searcher_;
	std::vector<std::vector<uint8_t>> history_;
	std::vector<std::string> log_;
	int human_ = -1;
	std::vector<int8_t> log_players_;
	std::vector<Duel::Move> moves_;
	std::vector<Duel::Activation> activations_;
	std::string last_move_;
	// The bot's current plan: the expected sequence of decision labels (both players).
	std::vector<Step> plan_;
	size_t plan_pos_ = 0;
	// Whose plan it is. The plan also holds the moves that bot expected from the other side
	// (often random playout moves); those only track whether play still follows it and are
	// never played for the other side's bot.
	int plan_player_ = -1;
	std::shared_ptr<const PieceGuide> guides_[2];
	std::shared_ptr<const class Policy> policy_;
	DeckKnowledge knowledge_[2];
	std::vector<SearchConfig::Imitation> imitate_[2];
	uint32_t last_action_[2] = {0, 0};  // the last card each player activated or summoned
	std::mt19937_64 rng_;
	std::atomic<bool> cancel_{false};
	bool ok_ = false;
	std::string error_;
};

}  // namespace ygoe
