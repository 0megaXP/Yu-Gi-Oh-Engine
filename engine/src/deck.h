// Decklists (.ydk), proxy hands and the text "state file" the CLI reads.
#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "carddb.h"
#include "duel.h"

namespace ygoe {

struct Deck {
	std::string name;
	std::vector<uint32_t> main, extra, side;
};

bool load_ydk(const std::string& path, Deck& out, std::string* error = nullptr);

// Resolves "Ash Blossom & Joyous Spring", a unique name prefix, or a passcode.
// Cards in `prefer` (e.g. the deck being described) win ties.
uint32_t resolve_card(const CardDB& db, const std::string& text, const std::vector<uint32_t>& prefer = {});

// Draws n cards from what remains of the Main Deck after removing `known`.
std::vector<uint32_t> sample_hand(const std::vector<uint32_t>& main, size_t n, std::mt19937_64& rng,
                                  const std::vector<uint32_t>& known = {});
// Removes one copy of each code in `cards` from `pool` (cards that are already in hand / on field).
std::vector<uint32_t> without(std::vector<uint32_t> pool, const std::vector<uint32_t>& cards);

enum class Knowledge { Practice, Real };

// A position to analyse.
struct Scenario {
	Deck decks[2];
	std::vector<uint32_t> hand[2];
	bool hand_known[2] = {true, true};  // false: hidden, sampled from the deck each determinization
	size_t hand_size[2] = {5, 5};       // used when the hand is hidden
	std::vector<PlacedCard> placed[2];
	uint32_t lp[2] = {8000, 8000};
	uint8_t me = 0;                     // side the engine plays for
	Knowledge knowledge = Knowledge::Practice;
	uint64_t seed = 1;
};

// Parses a state file (see docs/state-format.md). Paths inside are relative to the file.
bool load_scenario(const CardDB& db, const std::string& path, Scenario& out, std::string* error = nullptr);

// Builds one concrete duel from the scenario. Hidden information (the opponent's hand in
// Real mode, any hand marked hidden) is filled by sampling with the given seed.
DuelSetup make_setup(const Scenario& sc, uint64_t determinization_seed);

std::vector<uint32_t> declare_pool(const Scenario& sc);

}  // namespace ygoe
