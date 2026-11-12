#include "profile.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "ocgapi_constants.h"

namespace ygoe {

namespace {
// Values from CardScripts/constant.lua.
constexpr uint32_t ET_ACTIVATE = 0x10, ET_FLIP = 0x20, ET_IGNITION = 0x40, ET_TRIGGER_O = 0x80,
                   ET_QUICK_O = 0x100, ET_TRIGGER_F = 0x200, ET_QUICK_F = 0x400;
constexpr uint32_t EV_CHAINING = 1027, EV_SPSUMMON = 1105, EV_SUMMON = 1103, EV_SUMMON_SUCCESS = 1100;
constexpr uint64_t CAT_DESTROY = 0x1, CAT_RELEASE = 0x2, CAT_REMOVE = 0x4, CAT_TOHAND = 0x8, CAT_TODECK = 0x10,
                   CAT_TOGRAVE = 0x20, CAT_DECKDES = 0x40, CAT_SPECIAL_SUMMON = 0x200, CAT_POSITION = 0x1000,
                   CAT_CONTROL = 0x2000, CAT_DISABLE = 0x4000, CAT_DISABLE_SUMMON = 0x8000, CAT_DRAW = 0x10000,
                   CAT_SEARCH = 0x20000, CAT_NEGATE = 0x10000000;

bool activated(const EffectInfo& e) {
	return e.type & (ET_ACTIVATE | ET_IGNITION | ET_TRIGGER_O | ET_TRIGGER_F | ET_QUICK_O | ET_QUICK_F | ET_FLIP);
}

double clamp01(double v, double hi = 1.0) { return std::max(0.0, std::min(hi, v)); }
}  // namespace

const char* Interrupt::name() const {
	switch(kind) {
	case NegateActivation: return "negates an activation";
	case NegateEffect: return "negates an effect";
	case NegateSummon: return "negates a summon";
	case Removal: return "removes a card";
	case GyHate: return "stops the GY";
	case DeckHate: return "stops a search/send from Deck";
	case Extender: return "summons as a quick effect";
	case Draw: return "draws off their plays";
	default: return "other quick effect";
	}
}

CardRole classify(const CardDB& db, uint32_t code, const std::vector<uint16_t>& deck_sets) {
	CardRole role;
	auto c = db.get(db.script_id(code));
	if(!c)
		return role;
	const bool is_trap = c->type & TYPE_TRAP;
	const bool is_quickplay = (c->type & TYPE_SPELL) && (c->type & TYPE_QUICKPLAY);
	const bool is_monster = c->type & TYPE_MONSTER;
	bool engine_effect = false;
	for(const auto& e : c->effects) {
		if(!activated(e))
			continue;
		bool quick = e.type & (ET_QUICK_O | ET_QUICK_F);
		bool set_card = (e.type & ET_ACTIVATE) && (is_trap || is_quickplay);
		if(!quick && !set_card) {
			if(e.category & (CAT_SEARCH | CAT_SPECIAL_SUMMON | CAT_DECKDES | CAT_DRAW | CAT_TOHAND | CAT_TOGRAVE))
				engine_effect = true;
			continue;
		}
		Interrupt in;
		in.range = set_card ? (uint32_t)LOCATION_SZONE : (e.range ? e.range : (uint32_t)LOCATION_MZONE);
		in.once = e.count_limit > 0;
		uint64_t cat = e.category;
		if(cat & CAT_NEGATE)
			in.kind = Interrupt::NegateActivation, in.base = 1.0;
		else if(cat & CAT_DISABLE_SUMMON)
			in.kind = Interrupt::NegateSummon, in.base = 0.9;
		else if((cat & CAT_DISABLE) && e.code == EV_CHAINING && (in.range & LOCATION_HAND))
			in.kind = Interrupt::DeckHate, in.base = 0.7;
		else if(cat & CAT_DISABLE)
			in.kind = Interrupt::NegateEffect, in.base = 0.8;
		else if((cat & CAT_DRAW) && (in.range & LOCATION_HAND))
			in.kind = Interrupt::Draw, in.base = 0.45;  // "draw for each card they add/summon" style
		else if(cat & (CAT_REMOVE | CAT_TODECK | CAT_TOHAND | CAT_CONTROL | CAT_DESTROY | CAT_TOGRAVE | CAT_RELEASE))
			in.kind = Interrupt::Removal, in.base = (cat & (CAT_REMOVE | CAT_TODECK | CAT_CONTROL)) ? 0.75 : 0.65;
		else if(cat & CAT_POSITION)
			in.kind = Interrupt::Removal, in.base = 0.4;
		else if(cat & CAT_DRAW)
			in.kind = Interrupt::Draw, in.base = 0.35;
		else if(cat & CAT_SPECIAL_SUMMON)
			in.kind = Interrupt::Extender, in.base = 0.3;
		else
			in.kind = Interrupt::Other, in.base = 0.15;
		if(e.code == EV_SPSUMMON || e.code == EV_SUMMON) {
			if(in.kind == Interrupt::Removal || in.kind == Interrupt::NegateEffect)
				in.kind = Interrupt::NegateSummon, in.base = std::max(in.base, 0.8);
		}
		if(is_monster && (in.range & LOCATION_HAND))
			role.hand_trap = true;
		role.interrupts.push_back(in);
	}
	bool archetype = false;
	for(auto s : deck_sets)
		if(c->has_setcode(s))
			archetype = true;
	role.engine = archetype || (engine_effect && !role.hand_trap);
	return role;
}

std::vector<uint16_t> dominant_setcodes(const CardDB& db, const Deck& deck) {
	// Archetypes the deck is built around. Hand-trap families (e.g. a playset of one
	// archetype's hand traps) are not the deck's engine, so they are not counted.
	std::map<uint16_t, int> count;
	for(auto* list : {&deck.main, &deck.extra})
		for(auto code : *list)
			if(auto c = db.get(db.script_id(code)); c && !classify(db, code).hand_trap)
				for(int i = 0; i < 4; ++i)
					if(uint16_t s = static_cast<uint16_t>(c->setcode >> (16 * i)))
						++count[s];
	int best = 0;
	for(auto& [s, n] : count)
		best = std::max(best, n);
	std::vector<uint16_t> out;
	for(auto& [s, n] : count)
		if(n >= 4 && n * 4 >= best)
			out.push_back(s);
	return out;
}

DeckProfile profile_deck(const CardDB& db, const Deck& deck) {
	DeckProfile p;
	p.name = deck.name;
	auto sets = dominant_setcodes(db, deck);
	double total = 0;
	std::map<uint32_t, double> weight;
	auto visit = [&](uint32_t code, double w) {
		auto role = classify(db, code, sets);
		auto c = db.get(db.script_id(code));
		if(!c || !role.engine || role.hand_trap)
			return;
		bool st = c->type & (TYPE_SPELL | TYPE_TRAP);
		for(const auto& e : c->effects) {
			if(!activated(e))
				continue;
			total += w;
			weight[code] += w;
			if(st || (e.range & LOCATION_SZONE) || (e.range & 0x100 /* LOCATION_FZONE */))
				p.spell_trap += w;
			else if(e.range & LOCATION_GRAVE)
				p.gy_effects += w;
			else if(e.range & LOCATION_HAND)
				p.hand_effects += w;
			else
				p.monster_effects += w;
			if(e.category & (CAT_SEARCH | CAT_DECKDES | CAT_DRAW))
				p.searches += w;
			if(e.category & CAT_SPECIAL_SUMMON)
				p.special_summons += w;
			if(e.code == EV_SUMMON_SUCCESS)
				p.normal_summon += w;
		}
	};
	for(auto code : deck.main) {
		visit(code, 1.0);
		if(auto role = classify(db, code, sets); role.engine && !role.hand_trap)
			++p.engine_cards;
	}
	for(auto code : deck.extra)
		visit(code, 0.5);
	if(total > 0) {
		for(double* v : {&p.monster_effects, &p.hand_effects, &p.gy_effects, &p.spell_trap, &p.searches,
		                 &p.special_summons, &p.normal_summon})
			*v /= total;
	}
	p.extra_deck = deck.extra.size() / 15.0;
	std::vector<std::pair<uint32_t, double>> ranked(weight.begin(), weight.end());
	std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.second > b.second; });
	if(ranked.size() > 8)
		ranked.resize(8);
	p.key_cards = ranked;
	return p;
}

std::string DeckProfile::summary() const {
	std::ostringstream o;
	o.precision(0);
	o << std::fixed;
	auto pct = [](double v) { return v * 100; };
	o << "monster effects " << pct(monster_effects) << "%, hand " << pct(hand_effects) << "%, GY "
	  << pct(gy_effects) << "%, spells/traps " << pct(spell_trap) << "%; searches/sends " << pct(searches)
	  << "%, special summons " << pct(special_summons) << "%";
	return o.str();
}

double interrupt_value(const Interrupt& in, const DeckProfile& o) {
	double f = 1.0;
	switch(in.kind) {
	case Interrupt::NegateActivation: f = 1.0; break;
	case Interrupt::NegateEffect: f = 0.45 + 0.8 * (o.monster_effects + 0.5 * o.gy_effects + 0.5 * o.hand_effects); break;
	case Interrupt::NegateSummon: f = 0.4 + 1.0 * o.special_summons; break;
	case Interrupt::Removal: f = 0.75 + 0.3 * (o.monster_effects + o.spell_trap); break;
	case Interrupt::DeckHate: f = 0.35 + 1.1 * o.searches; break;
	case Interrupt::GyHate: f = 0.3 + 1.2 * o.gy_effects; break;
	case Interrupt::Draw: f = 0.5 + 0.8 * o.special_summons; break;
	default: break;
	}
	return clamp01(in.base * f, 1.3);
}

}  // namespace ygoe
