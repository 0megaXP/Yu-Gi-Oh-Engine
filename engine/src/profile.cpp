#include <locale>
#include "profile.h"

#include <cctype>

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

// The scripts use finer zone constants (LOCATION_STZONE, FZONE, PZONE, MMZONE, EMZONE);
// fold them into the two the evaluator looks at.
uint32_t zones(uint32_t range) {
	uint32_t out = range & (LOCATION_HAND | LOCATION_GRAVE | LOCATION_REMOVED | LOCATION_DECK | LOCATION_EXTRA);
	if(range & (LOCATION_MZONE | 0x800 | 0x1000))
		out |= LOCATION_MZONE;
	if(range & (LOCATION_SZONE | 0x100 | 0x200 | 0x400))
		out |= LOCATION_SZONE;
	return out;
}

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
	case DeckHate: return "stops a search, summon or send from the Deck";
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
	{
		std::string t = db.text(db.script_id(code));
		for(auto& ch : t)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		role.needs_empty_field = t.find("if you control no cards") != std::string::npos;
	}
	const bool is_trap = c->type & TYPE_TRAP;
	const bool is_quickplay = (c->type & TYPE_SPELL) && (c->type & TYPE_QUICKPLAY);
	const bool is_monster = c->type & TYPE_MONSTER;
	bool engine_effect = false;
	const uint32_t home = is_monster ? (uint32_t)LOCATION_MZONE : (uint32_t)LOCATION_SZONE;
	for(const auto& e : c->effects) {
		// Static effects: FIELD effects with a range, and protection on the card itself.
		constexpr uint32_t ET_SINGLE = 0x1, ET_FIELD_T = 0x2, ET_CONT = 0x800;
		constexpr uint32_t EF_IMMUNE = 1, EF_INDES_EFFECT = 41, EF_INDES_BATTLE = 42, EF_CANNOT_BE_TARGET = 71,
		                   EF_CANNOT_ACTIVATE = 6, EF_DISABLE = 2;
		if(!activated(e)) {
			uint32_t where = zones(e.range);
			double v = 0;
			if((e.type & ET_FIELD_T) && where)
				v = (e.code == EF_CANNOT_ACTIVATE || e.code == EF_DISABLE) ? 0.15 : 0.08;  // locks weigh more
			else if((e.type & ET_SINGLE) && (e.code == EF_IMMUNE || e.code == EF_INDES_EFFECT ||
			                                  e.code == EF_INDES_BATTLE || e.code == EF_CANNOT_BE_TARGET))
				v = 0.15, where = where ? where : home;
			else if((e.type & ET_CONT) && (e.type & ET_FIELD_T) && where)
				v = 0.1;
			if(where & LOCATION_MZONE)
				role.passive_mzone = std::min(0.6, role.passive_mzone + v);
			if(where & LOCATION_SZONE)
				role.passive_szone = std::min(0.6, role.passive_szone + v);
			continue;
		}
		{
			uint32_t where = zones(e.range ? e.range : home);
			if(e.type & (ET_IGNITION | ET_TRIGGER_O | ET_TRIGGER_F | ET_QUICK_O | ET_QUICK_F)) {
				if(where & LOCATION_MZONE)
					role.reusable_mzone = true;
				if(where & LOCATION_SZONE)
					role.reusable_szone = true;
			}
			if((e.type & (ET_IGNITION | ET_QUICK_O)) && (zones(e.range) & LOCATION_MZONE) &&
			   (e.category & (CAT_REMOVE | CAT_DESTROY | CAT_TOGRAVE | CAT_TOHAND | CAT_TODECK | CAT_CONTROL)))
				role.field_removal = std::max(role.field_removal, (e.category & (CAT_REMOVE | CAT_DESTROY)) ? 0.7 : 0.5);
			// Effects used from the GY later, by choice (ignition / quick). "If this card is sent to
			// the GY" triggers have already happened by the time a position is scored.
			if((e.type & (ET_IGNITION | ET_QUICK_O)) && (zones(e.range) & LOCATION_GRAVE)) {
				double v = 0.2;
				if(e.category & (CAT_SEARCH | CAT_SPECIAL_SUMMON | CAT_TOHAND | CAT_DRAW))
					v = 0.4;  // brings a card back into play: an extender
				else if(e.category & (CAT_TOGRAVE | CAT_DECKDES))
					v = 0.3;
				role.gy_effect = std::max(role.gy_effect, v);
			}
		}
		bool quick = e.type & (ET_QUICK_O | ET_QUICK_F);
		bool set_card = (e.type & ET_ACTIVATE) && (is_trap || is_quickplay);
		if(!quick && !set_card) {
			if(e.category & (CAT_SEARCH | CAT_SPECIAL_SUMMON | CAT_DECKDES | CAT_DRAW | CAT_TOHAND | CAT_TOGRAVE))
				engine_effect = true;
			continue;
		}
		Interrupt in;
		in.range = set_card ? (uint32_t)LOCATION_SZONE : zones(e.range ? e.range : home);
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
	// Some quick effects are registered from inside other functions, so the static pass can't
	// see them; the card text still says "(Quick Effect)".
	if(role.interrupts.empty() && !is_trap) {
		std::string text = db.text(c->code);
		auto has = [&](const char* w) { return text.find(w) != std::string::npos; };
		if(has("(Quick Effect)")) {
			Interrupt in;
			in.range = is_monster ? (has("discard this card") || has("from your hand") ? (uint32_t)LOCATION_HAND : (uint32_t)LOCATION_MZONE)
			                      : (uint32_t)LOCATION_SZONE;
			if(has("negate the activation") || has("negate that activation"))
				in.kind = Interrupt::NegateActivation, in.base = 1.0;
			else if(has("negate"))
				in.kind = Interrupt::NegateEffect, in.base = 0.8;
			else if(has("banish") || has("destroy") || has("return") || has("send") || has("shuffle"))
				in.kind = Interrupt::Removal, in.base = 0.65;
			else
				in.kind = Interrupt::Other, in.base = 0.2;
			if(is_monster && (in.range & LOCATION_HAND))
				role.hand_trap = true;
			role.interrupts.push_back(in);
		}
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
		// Its summons come from the Deck ("Special Summon ... from your Deck", "from your hand or Deck")
		std::string text = db.text(db.script_id(code));
		const bool summons_from_deck = text.find("from your Deck") != std::string::npos ||
		                               text.find("from your hand or Deck") != std::string::npos ||
		                               text.find("from your hand, Deck") != std::string::npos;
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
			if((e.category & (CAT_SEARCH | CAT_DECKDES)) || ((e.category & CAT_SPECIAL_SUMMON) && summons_from_deck))
				p.deck_plays += w;
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
		for(double* v : {&p.monster_effects, &p.hand_effects, &p.gy_effects, &p.spell_trap, &p.searches, &p.deck_plays,
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
	o.imbue(std::locale::classic());  // C number format, whatever locale the host app set
	o.precision(0);
	o << std::fixed;
	auto pct = [](double v) { return v * 100; };
	o << "monster effects " << pct(monster_effects) << "%, hand " << pct(hand_effects) << "%, GY "
	  << pct(gy_effects) << "%, spells/traps " << pct(spell_trap) << "%; searches/sends " << pct(searches)
	  << "%, from the Deck (search/summon/send) " << pct(deck_plays)
	  << "%, special summons " << pct(special_summons) << "%";
	return o.str();
}

DeckProfile generic_profile() {
	DeckProfile g;
	g.name = "generic";
	g.monster_effects = 0.5;
	g.hand_effects = 0.12;
	g.gy_effects = 0.15;
	g.spell_trap = 0.23;
	g.searches = 0.35;
	g.deck_plays = 0.4;
	g.special_summons = 0.45;
	return g;
}

double interrupt_value(const Interrupt& in, const DeckProfile& o) {
	double f = 1.0;
	switch(in.kind) {
	case Interrupt::NegateActivation: f = 1.0; break;
	case Interrupt::NegateEffect: f = 0.45 + 0.8 * (o.monster_effects + 0.5 * o.gy_effects + 0.5 * o.hand_effects); break;
	case Interrupt::NegateSummon: f = 0.4 + 1.0 * o.special_summons; break;
	case Interrupt::Removal: f = 0.75 + 0.3 * (o.monster_effects + o.spell_trap); break;
	case Interrupt::DeckHate: f = 0.35 + 1.1 * o.deck_plays; break;
	case Interrupt::GyHate: f = 0.3 + 1.2 * o.gy_effects; break;
	case Interrupt::Draw: f = 0.5 + 0.8 * o.special_summons; break;
	default: break;
	}
	return clamp01(in.base * f, 1.3);
}

}  // namespace ygoe
