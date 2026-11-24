// Guessing the opponent's deck from the cards they have shown.
//
// The library is the decks a player can meet: the format's top-cut lists (decks/) and the
// decks of real games (the app's replays), each weighted by how often it is met
// (data/deck_library.json, built by tools/ml/decklib.py in the Learn pipeline). Each card the
// opponent shows makes the lists that play it more likely: a list's likelihood is the product,
// over the cards seen, of (copies in the list + a little) / its size, so the archetype's own
// cards tell lists apart quickly and the staples everyone plays hardly matter. A "rogue" deck
// (none of the lists) is kept at a small prior: the cards seen, the archetype's other cards
// and the format's staples.
//
// The bots' searches draw the opponent's unseen cards from decks picked by these chances
// (live.h hidden_roots), so they play against what the opponent could have, not their real list.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "carddb.h"
#include "deck.h"

namespace ygoe {

class DeckLibrary {
public:
	struct Entry {
		std::string name;
		std::map<uint32_t, int> main;  // script id -> copies
		int size = 0;
		double weight = 1;  // how often it is met (copies of the list in the sources)
	};
	// {"decks": [{"name", "main": [passcodes], "weight"}]}
	bool parse(const CardDB& db, const std::string& json_text, std::string* err = nullptr);
	void add(const CardDB& db, const Deck& d, double weight = 1);
	bool empty() const { return decks_.empty(); }
	const std::vector<Entry>& decks() const { return decks_; }
	// Non-engine cards in at least half of the lists' weight, with their usual copies.
	const std::map<uint32_t, int>& staples() const { return staples_; }

private:
	void finish(const CardDB& db);
	std::vector<Entry> decks_;
	std::map<uint32_t, int> staples_;
};

struct DeckGuess {
	std::vector<double> p;  // per library deck
	double rogue = 0;       // none of them
	// The likeliest first: (library index, chance); index -1 is the rogue deck.
	std::vector<std::pair<int, double>> ranked() const;
};

// `seen`: the opponent's Main Deck cards seen so far (script ids -> copies).
DeckGuess guess_deck(const DeckLibrary& lib, const std::map<uint32_t, int>& seen);
// A rogue deck for these cards: what was seen, the archetype's other cards, the staples (~40).
std::map<uint32_t, int> rogue_deck(const CardDB& db, const DeckLibrary& lib, const std::map<uint32_t, int>& seen);
// Cards that don't go in the Main Deck (Extra Deck monsters, tokens).
bool main_deck_card(const CardDB& db, uint32_t script_id);

}  // namespace ygoe
