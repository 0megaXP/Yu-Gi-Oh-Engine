#include "side.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

#include "ocgapi_constants.h"

namespace ygoe {

namespace {

constexpr uint32_t ET_ACTIVATE = 0x10, ET_IGNITION = 0x40, ET_QUICK_O = 0x100, ET_TRIGGER_O = 0x80;
constexpr uint64_t CAT_DESTROY = 0x1, CAT_RELEASE = 0x2, CAT_REMOVE = 0x4, CAT_TOHAND = 0x8, CAT_TODECK = 0x10,
                   CAT_TOGRAVE = 0x20, CAT_CONTROL = 0x2000, CAT_DISABLE = 0x4000;

std::string lower(std::string s) {
	for(auto& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string pct(double v) {
	return std::to_string(static_cast<int>(v * 100 + 0.5)) + "%";
}

// Value of a card in this matchup and a short reason.
// `in_main`: a Main Deck card that is part of the engine is never offered as a side-out.
CardValue value_of(const CardDB& db, uint32_t code, const std::vector<uint16_t>& my_sets, const DeckProfile& opp,
                   const BoardShape& shape, bool going_first, bool in_main) {
	CardValue cv;
	cv.code = code;
	auto role = classify(db, code, my_sets);
	auto c = db.get(db.script_id(code));
	if(!c)
		return cv;
	std::string text = lower(db.text(db.script_id(code)));

	// 1) Interaction during the opponent's turn (hand traps, set traps, quick effects).
	double inter = 0, second = 0;
	const Interrupt* best = nullptr;
	for(const auto& in : role.interrupts) {
		if(in.kind == Interrupt::Extender)
			continue;
		double v = interrupt_value(in, opp);
		if(v > inter) {
			second = inter;
			inter = v;
			best = &in;
		} else if(v > second) {
			second = v;
		}
	}
	inter += 0.3 * second;
	const bool needs_set = best && (best->range & LOCATION_SZONE) && !(c->type & TYPE_MONSTER);
	if(!going_first && needs_set)
		inter *= 0.45;  // a trap you draw going second is a turn late

	// 2) Breaking an established board on your own turn (going second).
	double brk = 0;
	std::string brk_why;
	for(const auto& e : c->effects) {
		bool own_turn = e.type & (ET_ACTIVATE | ET_IGNITION | ET_QUICK_O | ET_TRIGGER_O);
		if((e.type & ET_QUICK_O) && (e.range & LOCATION_HAND))
			own_turn = false;  // hand traps are counted as interaction above
		if(!own_turn || !(e.category & (CAT_DESTROY | CAT_REMOVE | CAT_TODECK | CAT_TOHAND | CAT_CONTROL | CAT_TOGRAVE |
		                                CAT_RELEASE | CAT_DISABLE)))
			continue;
		bool hits_st = text.find("spell/trap") != std::string::npos || text.find("spell & trap") != std::string::npos ||
		               text.find("set card") != std::string::npos || text.find("spell card") != std::string::npos ||
		               text.find("spells/traps") != std::string::npos;
		bool hits_mon = text.find("monster") != std::string::npos;
		bool mass = text.find(" all ") != std::string::npos || text.find("as many") != std::string::npos ||
		            text.find("each card") != std::string::npos;
		double st_targets = shape.samples ? shape.st_faceup + shape.st_set : 1.0 + 3.0 * opp.spell_trap;
		double mon_targets = shape.samples ? shape.monsters : 1.0 + 2.5 * opp.monster_effects;
		double fit = 0;
		if(hits_st)
			fit = std::max(fit, std::min(1.0, st_targets / 2.0));
		if(hits_mon)
			fit = std::max(fit, std::min(1.0, mon_targets / 2.0));
		if(!hits_st && !hits_mon)
			fit = 0.5;
		double v = 0.75 * fit * (mass ? 1.5 : 1.0);
		if(v > brk) {
			brk = v;
			brk_why = std::string(hits_st && !hits_mon ? "removes Spell/Trap-zone cards" : hits_mon && !hits_st ? "removes monsters" : "removes cards") +
			          (mass ? " (several at once)" : "");
		}
	}
	if(going_first)
		brk *= 0.25;  // little to break on turn 1

	// Main Deck cards with no interaction at all are combo pieces, even when the script
	// analysis cannot tell (e.g. cards that move Pendulums to the Extra Deck).
	if(in_main && (role.engine || (inter <= 0 && brk <= 0))) {
		cv.value = 5;
		cv.why = "engine";
		return cv;
	}
	cv.value = std::max(inter, brk) + 0.25 * std::min(inter, brk);
	std::ostringstream why;
	if(inter >= brk && best) {
		why << best->name();
		switch(best->kind) {
		case Interrupt::DeckHate: why << "; " << pct(opp.searches) << " of their plays search/send from Deck"; break;
		case Interrupt::NegateEffect: why << "; " << pct(opp.monster_effects + opp.hand_effects) << " of their plays are monster effects"; break;
		case Interrupt::NegateSummon: why << "; " << pct(opp.special_summons) << " of their plays Special Summon"; break;
		case Interrupt::GyHate: why << "; " << pct(opp.gy_effects) << " of their plays use the GY"; break;
		case Interrupt::Draw: why << "; they Special Summon often (" << pct(opp.special_summons) << ")"; break;
		default: break;
		}
		if(!going_first && needs_set)
			why << "; slow going second";
	} else if(brk > 0) {
		why << brk_why;
		if(shape.samples)
			why << "; their boards average " << static_cast<int>(shape.monsters * 10 + 0.5) / 10.0 << " monsters, "
			    << static_cast<int>((shape.st_faceup + shape.st_set) * 10 + 0.5) / 10.0 << " Spell/Trap-zone cards";
		else
			why << "; " << pct(opp.spell_trap) << " of their plays use the Spell/Trap zone";
	} else {
		why << "little interaction in this matchup";
	}
	cv.why = why.str();
	return cv;
}

}  // namespace

SidePlan plan_side(const CardDB& db, const Deck& mine, const Deck& theirs, const DeckProfile& opp,
                   const BoardShape& shape, bool going_first, int max_swaps) {
	SidePlan plan;
	plan.going_first = going_first;
	auto sets = dominant_setcodes(db, mine);

	std::map<uint32_t, int> side_count, main_count;
	for(auto c : mine.side)
		++side_count[c];
	for(auto c : mine.main)
		++main_count[c];

	for(auto& [code, n] : side_count) {
		auto v = value_of(db, code, sets, opp, shape, going_first, false);
		for(int i = 0; i < n; ++i)
			plan.side_values.push_back(v);
	}
	for(auto& [code, n] : main_count) {
		auto v = value_of(db, code, sets, opp, shape, going_first, true);
		if(v.why == "engine")
			continue;
		for(int i = 0; i < n; ++i)
			plan.flex_values.push_back(v);
	}
	std::stable_sort(plan.side_values.begin(), plan.side_values.end(), [](auto& a, auto& b) { return a.value > b.value; });
	std::stable_sort(plan.flex_values.begin(), plan.flex_values.end(), [](auto& a, auto& b) { return a.value < b.value; });

	std::map<uint32_t, int> deck_now = main_count;
	size_t out_i = 0;
	for(const auto& in : plan.side_values) {
		if(static_cast<int>(plan.swaps.size()) >= max_swaps || out_i >= plan.flex_values.size())
			break;
		const auto& out = plan.flex_values[out_i];
		double gain = in.value - out.value;
		if(gain < 0.12)
			break;
		if(deck_now[in.code] >= 3)
			continue;
		++deck_now[in.code];
		--deck_now[out.code];
		plan.swaps.push_back({out.code, in.code, gain});
		++out_i;
	}
	return plan;
}

}  // namespace ygoe
