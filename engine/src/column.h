// Columns: what a card's effects reach, by where it stands (from the card text, any card worded
// this way): "this card's column", "adjacent column", "N columns away", "another column",
// "the center Main Monster Zone", a Set Infinite Impermanence's "Spell/Trap effects in this column".
#pragma once
#include <cstdint>
#include <string>

namespace ygoe {

struct Board;
struct CardView;

enum : uint8_t {
	kColSame = 1,      // in this card's column
	kColAdjacent = 2,  // in an adjacent column
	kColTwoAway = 4,   // 2 columns away
	kColOther = 8,     // in another column
};

struct ColumnRule {
	uint8_t mon = 0;          // which columns of opponent's monsters it reaches (kCol* bits)
	uint8_t st = 0;           // of opponent's Spell & Trap Zone cards
	bool center = false;      // an effect only from / when summoned to the center Main Monster Zone
	bool st_column = false;   // Set, it also negates Spell/Trap effects in its column (Imperm)
	bool any() const { return mon || st || center || st_column; }
};

// From the card's text (lower case).
ColumnRule column_rule(const std::string& lower_text);
// Column 0..4 as seen from player 0's left (Extra Monster Zones: 1 and 3), -1 if none (Field,
// Pendulum Zones, off the field).
int column_of(uint8_t controller, uint8_t location, uint32_t sequence);
// Cards of player `opp` within `r`'s reach of column `col`.
int column_reach(const ColumnRule& r, int col, const Board& b, uint8_t opp);

}  // namespace ygoe
