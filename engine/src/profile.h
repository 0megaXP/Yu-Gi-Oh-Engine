// What a card can do on the opponent's turn, and what a whole deck depends on.
//
// Built from the static script analysis (data/effects.tsv): no card is special-cased,
// so a new archetype is profiled as soon as ProjectIgnis scripts it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "carddb.h"
#include "deck.h"

namespace ygoe {

// How an opponent's deck plays, as shares in 0..1 of its "engine" effects.
struct DeckProfile {
	std::string name;
	double monster_effects = 0;  // activated by monsters on the field
	double hand_effects = 0;     // activated from the hand (non-hand-trap)
	double gy_effects = 0;       // activated from the GY
	double spell_trap = 0;       // spell/trap activations and spell/trap-zone effects
	double searches = 0;         // add from Deck / send from Deck / draw
	double special_summons = 0;  // effects that Special Summon
	double extra_deck = 0;       // Extra Deck monsters per 15
	double normal_summon = 0;    // monsters that want to be Normal Summoned
	int engine_cards = 0;        // main-deck cards counted as the deck's engine
	std::vector<std::pair<uint32_t, double>> key_cards;  // most-played engine cards, by weight
	std::string summary() const;
};

// One way a card can interfere on the opponent's turn.
struct Interrupt {
	enum Kind : uint8_t { NegateActivation, NegateEffect, NegateSummon, Removal, GyHate, DeckHate, Extender, Draw, Other };
	Kind kind = Other;
	uint32_t range = 0;   // where it is used from
	bool once = false;    // has a once-per-turn limit
	double base = 0;      // value against a generic deck
	const char* name() const;
};

struct CardRole {
	std::vector<Interrupt> interrupts;  // quick effects / traps usable on the opponent's turn
	bool hand_trap = false;
	bool engine = false;               // part of the deck's own combo (by archetype or role)
};

// Card-level classification from scripts. `deck_sets` are the archetype codes that dominate
// the deck the card is in; cards sharing them count as engine.
CardRole classify(const CardDB& db, uint32_t code, const std::vector<uint16_t>& deck_sets = {});
std::vector<uint16_t> dominant_setcodes(const CardDB& db, const Deck& deck);
DeckProfile profile_deck(const CardDB& db, const Deck& deck);

// How much an interrupt is worth against this specific opponent (≈0..1.2).
double interrupt_value(const Interrupt& in, const DeckProfile& opp);

}  // namespace ygoe
