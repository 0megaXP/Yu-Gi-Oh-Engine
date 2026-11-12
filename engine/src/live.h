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

	const Duel& duel() const { return *duel_; }

private:
	bool respond_internal(size_t index);
	void auto_resolve();
	int choose(BotLevel level);
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
	std::string last_move_;
	// The bot's current plan: the expected sequence of decision labels (both players).
	std::vector<Step> plan_;
	size_t plan_pos_ = 0;
	std::mt19937_64 rng_;
	std::atomic<bool> cancel_{false};
	bool ok_ = false;
	std::string error_;
};

}  // namespace ygoe
