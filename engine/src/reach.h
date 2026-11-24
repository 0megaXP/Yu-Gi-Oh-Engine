// Bridges: the cards that take a deck from one archetype to another.
//
// Found by trying each card's effects in the rules core, not by reading its text:
//  1. Reach (probe_card): a card is used in a test duel (from the hand, on the field, in the GY)
//     whose Deck and Extra Deck hold a large pool of cards; every card its effects can pick from
//     the Deck, the Extra Deck, the GY or the banishment is noted, with what happened to the one
//     picked (added to the hand, Special Summoned, sent to the GY). Run over the whole card pool
//     once (`ygoe-cli reachscan`, the Reach workflow) into data/reach.tsv.
//  2. Materials (probe_materials, per deck): pairs of the deck's monsters on the field, and the
//     Extra Deck monsters they can be used to summon.
//  3. Bridges (find_bridges, per deck): the deck's cards grouped by archetype; a bridge is a card
//     whose reach (or whose materials' summons) crosses from one group to another, or into a
//     card of another archetype that a card of the deck's list could add. Bots value them
//     (search.h CardPriorities): searched first, kept, and their plays tried first.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "deck.h"

namespace ygoe {

class CardDB;
class ScriptCache;

enum ReachHow : uint8_t {
	kReachSearch = 1,       // added to the hand from the Deck / GY / banishment
	kReachSummon = 2,       // Special Summoned from the Deck / GY / banishment / hand
	kReachExtraSummon = 3,  // Special Summoned from the Extra Deck
	kReachSend = 4,         // sent from the Deck or Extra Deck to the GY (or banished from it)
	kReachMaterial = 5,     // used as material to summon it (an Extra Deck monster)
	kReachOther = 6,        // picked by its effect for something else
};
const char* reach_how_name(uint8_t how);

struct ReachEdge {
	uint32_t from = 0;  // official passcodes
	uint32_t to = 0;
	uint8_t how = 0;
};

// What `code` can reach, tried in the rules core against `main_pool` / `extra_pool` (passcodes).
std::vector<ReachEdge> probe_card(const CardDB& db, ScriptCache& scripts, uint32_t code,
                                  const std::vector<uint32_t>& main_pool, const std::vector<uint32_t>& extra_pool);

// Extra Deck monsters of `deck` that pairs of its Main Deck monsters can summon (materials).
std::vector<ReachEdge> probe_materials(const CardDB& db, ScriptCache& scripts, const Deck& deck, int max_pairs = 300);

// The scanned reach of the card pool (data/reach.tsv: from, to, how per line).
class ReachTable {
public:
	bool load(const std::string& path);
	const std::vector<ReachEdge>* from(uint32_t code) const;
	size_t size() const { return n_; }

private:
	std::unordered_map<uint32_t, std::vector<ReachEdge>> by_from_;
	size_t n_ = 0;
};

struct Bridge {
	uint32_t code = 0;            // the bridge card (in the deck)
	uint16_t from_set = 0;        // the archetype it is reached from / belongs to (0: generic)
	uint16_t to_set = 0;          // the archetype it leads into
	std::vector<ReachEdge> edges; // how: what it reaches across, or the materials it joins
	double weight = 0;            // how much it connects (cards reached in the other group)
};

struct BridgeReport {
	std::vector<uint16_t> groups;  // the deck's archetypes, most cards first
	std::vector<Bridge> bridges;   // inside the list, strongest first
	// Outside the list: archetypes in the card pool a card of the deck reaches into.
	std::vector<Bridge> outside;
	std::string to_json(const CardDB& db) const;
};

// Bridges of a deck (passcodes), from the scanned reach (may be null: only the deck's own
// cards are probed then, against the deck itself) and the materials probe.
BridgeReport find_bridges(const CardDB& db, ScriptCache& scripts, const Deck& deck, const ReachTable* table);

}  // namespace ygoe
