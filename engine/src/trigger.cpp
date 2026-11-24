#include "trigger.h"

#include <algorithm>

#include "carddb.h"
#include "duel.h"
#include "ocgapi_constants.h"

namespace ygoe {

namespace {

// Values from CardScripts/constant.lua.
constexpr uint32_t ET_SINGLE = 0x1, ET_FIELD = 0x2, ET_FLIP = 0x20, ET_IGNITION = 0x40, ET_TRIGGER_O = 0x80,
                   ET_QUICK_O = 0x100, ET_TRIGGER_F = 0x200, ET_QUICK_F = 0x400;
constexpr uint32_t EF_DELAY = 0x10000;
constexpr uint32_t EV_FLIP = 1001, EV_FREE_CHAIN = 1002;
constexpr uint32_t CAT_DESTROY = 0x1, CAT_REMOVE = 0x4, CAT_TOHAND = 0x8, CAT_TODECK = 0x10, CAT_TOGRAVE = 0x20,
                   CAT_DECKDES = 0x40, CAT_SPECIAL_SUMMON = 0x200, CAT_CONTROL = 0x2000, CAT_DRAW = 0x10000,
                   CAT_SEARCH = 0x20000, CAT_NEGATE = 0x10000000, CAT_DISABLE = 0x4000;
constexpr uint64_t HM_RELEASE = 500, HM_DISCARD = 501, HM_DESTROY = 502, HM_REMOVE = 503, HM_TOGRAVE = 504,
                   HM_RTOHAND = 505, HM_ATOHAND = 506, HM_TODECK = 507, HM_FMATERIAL = 511, HM_SMATERIAL = 512,
                   HM_XMATERIAL = 513, HM_LMATERIAL = 533;

// The finer zone bits the scripts use (STZONE, FZONE, PZONE, MMZONE, EMZONE) folded into the
// Monster and Spell & Trap Zones.
uint32_t fold(uint32_t loc) {
	uint32_t out = loc & (LOCATION_DECK | LOCATION_HAND | LOCATION_GRAVE | LOCATION_REMOVED | LOCATION_EXTRA | LOCATION_OVERLAY);
	if(loc & (LOCATION_MZONE | 0x800 | 0x1000))
		out |= LOCATION_MZONE;
	if(loc & (LOCATION_SZONE | 0x100 | 0x200 | 0x400))
		out |= LOCATION_SZONE;
	return out;
}

TrigEvent event_of(uint32_t code, uint32_t type, uint32_t now, uint32_t prev) {
	if(type & ET_FLIP)
		return TrigEvent::Flipped;
	if(code >= 0x10000000)
		return TrigEvent::Custom;
	if(code & 0x30000)  // counters added / removed
		return TrigEvent::Other;
	if(code & 0x3000)
		return TrigEvent::Phase;
	switch(code) {
	case 1014: return TrigEvent::ToGrave;
	case 1011: return TrigEvent::Banished;
	case 1015: case 1019: return TrigEvent::LeftField;
	case 1010: case 1029: return TrigEvent::Destroyed;
	case 1140: return TrigEvent::BattleDestroyed;
	case 1139: return TrigEvent::BattleDestroying;
	case 1017: return TrigEvent::Tributed;
	case 1108: case 1109: return TrigEvent::Material;
	case 1202: return TrigEvent::Detached;
	case 1018: return TrigEvent::Discarded;
	case 1012: return TrigEvent::ToHand;
	case 1013: return TrigEvent::ToDeck;
	case 1031: return TrigEvent::LeftGrave;
	case 1030:
		// "placed": it has to end up in a Spell & Trap / Field / Pendulum Zone, coming from
		// outside them (else it's a move between zones: a column effect)
		if((fold(now) & LOCATION_SZONE) && !(fold(prev) & LOCATION_SZONE))
			return TrigEvent::Placed;
		return TrigEvent::Moved;
	case 1100: case 1103: return TrigEvent::Summoned;
	case 1102: case 1105: return TrigEvent::SpSummoned;
	case 1101: case 1104: return TrigEvent::FlipSummoned;
	case EV_FLIP: return TrigEvent::Flipped;
	case 1027: case 1020: case 1021: case 1022: case 1024: case 1025: case 1026: return TrigEvent::Chaining;
	case 1110: case 1113: return TrigEvent::Drawn;
	case 1111: case 1112: case 1201: return TrigEvent::Damage;
	case 1028: return TrigEvent::Targeted;
	default:
		if(code >= 1130 && code <= 1143)
			return TrigEvent::Battle;
		return TrigEvent::Other;
	}
}

double worth(uint32_t category) {
	if(category & (CAT_SEARCH | CAT_TOHAND | CAT_SPECIAL_SUMMON | CAT_DRAW))
		return 1.0;
	if(category & (CAT_NEGATE | CAT_DISABLE | CAT_DESTROY | CAT_REMOVE | CAT_TODECK | CAT_CONTROL))
		return 0.8;
	if(category & (CAT_TOGRAVE | CAT_DECKDES))
		return 0.6;
	return 0.4;
}

std::string locs(uint32_t loc) {
	loc = fold(loc);
	std::string s;
	auto add = [&](uint32_t bit, const char* name) {
		if(loc & bit)
			s += (s.empty() ? "" : "/") + std::string(name);
	};
	if((loc & LOCATION_ONFIELD) == LOCATION_ONFIELD) {
		s = "the field";
		loc &= ~static_cast<uint32_t>(LOCATION_ONFIELD);
	}
	add(LOCATION_MZONE, "the Monster Zone");
	add(LOCATION_SZONE, "the Spell & Trap Zone");
	add(LOCATION_HAND, "the hand");
	add(LOCATION_DECK, "the Deck");
	add(LOCATION_EXTRA, "the Extra Deck");
	add(LOCATION_GRAVE, "the GY");
	add(LOCATION_REMOVED, "the banishment");
	add(LOCATION_OVERLAY, "Xyz material");
	return s;
}

std::string reasons_text(uint32_t r) {
	std::string s;
	auto add = [&](uint32_t bit, const char* name) {
		if(r & bit)
			s += (s.empty() ? "" : " or ") + std::string(name);
	};
	add(REASON_EFFECT, "a card effect");
	add(REASON_BATTLE, "battle");
	add(REASON_COST, "a cost");
	add(REASON_DISCARD, "discarding");
	add(REASON_FUSION, "Fusion material");
	add(REASON_SYNCHRO, "Synchro material");
	add(REASON_XYZ, "Xyz material");
	add(REASON_LINK, "Link material");
	add(REASON_RITUAL, "Ritual material");
	add(REASON_SUMMON, "a Tribute Summon");
	add(REASON_LOST_TARGET, "losing its target");
	add(REASON_RULE, "the rules");
	return s;
}

}  // namespace

const char* trig_event_name(TrigEvent e) {
	switch(e) {
	case TrigEvent::ToGrave: return "sent to the GY";
	case TrigEvent::Banished: return "banished";
	case TrigEvent::LeftField: return "leaves the field";
	case TrigEvent::Destroyed: return "destroyed";
	case TrigEvent::BattleDestroyed: return "destroyed by battle";
	case TrigEvent::BattleDestroying: return "destroys a monster by battle";
	case TrigEvent::Tributed: return "Tributed";
	case TrigEvent::Material: return "used as material";
	case TrigEvent::Detached: return "detached";
	case TrigEvent::Discarded: return "discarded";
	case TrigEvent::ToHand: return "added to the hand";
	case TrigEvent::ToDeck: return "returned to the Deck";
	case TrigEvent::LeftGrave: return "leaves the GY";
	case TrigEvent::Moved: return "moves to another zone";
	case TrigEvent::Placed: return "placed in a zone";
	case TrigEvent::Summoned: return "Normal Summoned";
	case TrigEvent::SpSummoned: return "Special Summoned";
	case TrigEvent::FlipSummoned: return "Flip Summoned";
	case TrigEvent::Flipped: return "flipped face-up";
	case TrigEvent::Chaining: return "a card or effect is activated";
	case TrigEvent::Phase: return "a phase";
	case TrigEvent::Battle: return "battle";
	case TrigEvent::Drawn: return "drawn";
	case TrigEvent::Damage: return "damage / Life Points";
	case TrigEvent::Targeted: return "targeted";
	case TrigEvent::Custom: return "a script's own event";
	default: return "other";
	}
}

std::string describe(const Trigger& t) {
	std::string s;
	const bool move = t.event >= TrigEvent::ToGrave && t.event <= TrigEvent::Flipped;
	if(t.event == TrigEvent::Phase)
		s = t.delayed ? "Delayed: in a later phase (\"during the End Phase of the turn this card was ...\")" : "In a phase";
	else if(move)
		s = std::string(t.self ? "If this card is " : "If a card is ") + trig_event_name(t.event);
	else
		s = std::string("On: ") + trig_event_name(t.event);
	if(t.from)
		s += " from " + locs(t.from);
	if(t.now && (t.event == TrigEvent::Placed || t.event == TrigEvent::Moved))
		s += " to " + locs(t.now);
	if(t.reasons)
		s += " by " + reasons_text(t.reasons);
	if(t.not_reasons)
		s += " (not by " + reasons_text(t.not_reasons) + ")";
	if(t.by == 1)
		s += ", by its controller";
	else if(t.by == 2)
		s += ", by the opponent";
	if(!t.self && t.watch)
		s += ", while this card is in " + locs(t.watch);
	switch(t.kind) {
	case Trigger::OptionalIf: s += " (optional, doesn't miss timing)"; break;
	case Trigger::OptionalWhen: s += " (optional, can miss timing)"; break;
	case Trigger::Mandatory: s += " (mandatory)"; break;
	case Trigger::Quick: s += " (quick effect)"; break;
	}
	return s;
}

std::vector<Trigger> triggers_of(const CardInfo& c) {
	std::vector<Trigger> out;
	for(size_t i = 0; i < c.effects.size(); ++i) {
		const EffectInfo& e = c.effects[i];
		const bool trig = e.type & (ET_TRIGGER_O | ET_TRIGGER_F);
		const bool quick = (e.type & (ET_QUICK_O | ET_QUICK_F)) && e.code && e.code != EV_FREE_CHAIN;
		const bool flip = (e.type & ET_FLIP) != 0;
		if(!trig && !quick && !flip)
			continue;
		Trigger t;
		t.code = e.code;
		t.event = event_of(e.code, e.type, e.now, e.prev);
		t.self = (e.type & ET_SINGLE) || flip;
		t.watch = t.self ? 0 : e.range;
		t.from = e.prev;
		t.now = e.now;
		t.reasons = e.reasons;
		t.not_reasons = e.not_reasons;
		t.by = e.by;
		t.kind = quick ? Trigger::Quick : (e.type & ET_TRIGGER_F) ? Trigger::Mandatory
		       : (e.property & EF_DELAY) ? Trigger::OptionalIf : Trigger::OptionalWhen;
		t.delayed = t.event == TrigEvent::Phase && !t.self && (e.range & (LOCATION_GRAVE | LOCATION_REMOVED));
		t.category = e.category;
		t.effect = static_cast<int>(i);
		out.push_back(t);
	}
	return out;
}

bool fires(const Trigger& t, const Move& m) {
	if(!t.self)
		return false;
	const uint32_t from = fold(m.from), to = fold(m.to);
	const uint32_t r = m.reason;
	bool ok = false;
	switch(t.event) {
	case TrigEvent::ToGrave: ok = to & LOCATION_GRAVE; break;
	case TrigEvent::Banished: ok = to & LOCATION_REMOVED; break;
	case TrigEvent::LeftField: ok = (from & LOCATION_ONFIELD) && !(to & LOCATION_ONFIELD); break;
	case TrigEvent::Destroyed: ok = r & REASON_DESTROY; break;
	case TrigEvent::BattleDestroyed: ok = (r & REASON_DESTROY) && (r & REASON_BATTLE); break;
	case TrigEvent::Tributed: ok = r & REASON_RELEASE; break;
	case TrigEvent::Material: ok = r & REASON_MATERIAL; break;
	case TrigEvent::Detached: ok = from & LOCATION_OVERLAY; break;
	case TrigEvent::Discarded: ok = r & REASON_DISCARD; break;
	case TrigEvent::ToHand: ok = to & LOCATION_HAND; break;
	case TrigEvent::ToDeck: ok = to & (LOCATION_DECK | LOCATION_EXTRA); break;
	case TrigEvent::LeftGrave: ok = from & LOCATION_GRAVE; break;
	case TrigEvent::Placed: ok = (to & LOCATION_SZONE) && !(from & LOCATION_SZONE) && !(r & REASON_SUMMON); break;
	case TrigEvent::Moved: ok = (from & LOCATION_ONFIELD) && (to & LOCATION_ONFIELD); break;
	default: return false;
	}
	if(!ok)
		return false;
	if(t.from && !(fold(t.from) & from))
		return false;
	if(t.now && !(fold(t.now) & to))
		return false;
	// Destroyed / Tributed / material are what the event says; the rest of the reasons narrow it
	constexpr uint32_t kWhy = ~static_cast<uint32_t>(REASON_TEMPORARY | REASON_REPLACE | REASON_REDIRECT);
	if((t.reasons & kWhy) && !((r | m.maybe) & t.reasons))
		return false;
	if(t.not_reasons & r)
		return false;
	if((t.by == 1 && m.by_opponent) || (t.by == 2 && !m.by_opponent))
		return false;
	return true;
}

double move_value(const CardInfo& c, const Move& m) {
	double v = 0;
	for(const Trigger& t : triggers_of(c))
		if(fires(t, m))
			v += worth(t.category) * (t.kind == Trigger::OptionalWhen ? 0.8 : 1.0);
	return v;
}

bool has_grave_effects(const CardInfo& c) {
	for(const auto& e : c.effects)
		if((e.type & (ET_IGNITION | ET_QUICK_O | ET_QUICK_F)) && (e.range & LOCATION_GRAVE))
			return true;
	return false;
}

double floats(const CardInfo& c) {
	const uint32_t home = (c.type & TYPE_MONSTER) ? LOCATION_MZONE : LOCATION_SZONE;
	double best = 0;
	for(const Move& m : {Move{home, LOCATION_GRAVE, REASON_DESTROY | REASON_EFFECT, 0, true},
	                     Move{home, LOCATION_GRAVE, REASON_DESTROY | REASON_BATTLE, 0, true},
	                     Move{home, LOCATION_GRAVE, REASON_EFFECT, 0, true},
	                     Move{home, LOCATION_REMOVED, REASON_EFFECT, 0, true}})
		best = std::max(best, move_value(c, m));
	return 0.25 * std::min(best, 1.5);
}

bool selection_move(const Decision& d, uint32_t& to, uint32_t& reason, uint32_t& maybe) {
	maybe = 0;
	if(d.type == DecisionType::Tribute) {
		to = LOCATION_GRAVE, reason = REASON_RELEASE, maybe = REASON_SUMMON | REASON_EFFECT | REASON_COST;
		return true;
	}
	if(d.type != DecisionType::Card && d.type != DecisionType::UnselectCard)
		return false;
	switch(d.hint) {
	case HM_RELEASE: to = LOCATION_GRAVE, reason = REASON_RELEASE, maybe = REASON_EFFECT | REASON_COST | REASON_SUMMON; return true;
	case HM_DISCARD: to = LOCATION_GRAVE, reason = REASON_DISCARD, maybe = REASON_EFFECT | REASON_COST; return true;
	case HM_DESTROY: to = LOCATION_GRAVE, reason = REASON_DESTROY | REASON_EFFECT; return true;
	case HM_REMOVE: to = LOCATION_REMOVED, reason = 0, maybe = REASON_EFFECT | REASON_COST; return true;
	case HM_TOGRAVE: to = LOCATION_GRAVE, reason = 0, maybe = REASON_EFFECT | REASON_COST; return true;
	case HM_RTOHAND:
	case HM_ATOHAND: to = LOCATION_HAND, reason = REASON_EFFECT; return true;
	case HM_TODECK: to = LOCATION_DECK, reason = REASON_EFFECT; return true;
	case HM_FMATERIAL: to = LOCATION_GRAVE, reason = REASON_MATERIAL | REASON_FUSION; return true;
	case HM_SMATERIAL: to = LOCATION_GRAVE, reason = REASON_MATERIAL | REASON_SYNCHRO; return true;
	case HM_XMATERIAL: to = LOCATION_OVERLAY, reason = REASON_MATERIAL | REASON_XYZ; return true;
	case HM_LMATERIAL: to = LOCATION_GRAVE, reason = REASON_MATERIAL | REASON_LINK; return true;
	default: return false;
	}
}

double selection_factor(const CardDB& db, const Decision& d, const Option& o) {
	if(!o.code || o.kind != OptKind::Select)
		return 1.0;
	uint32_t to = 0, reason = 0, maybe = 0;
	if(!selection_move(d, to, reason, maybe))
		return 1.0;
	const CardInfo* c = db.get(o.code);
	if(!c)
		return 1.0;
	const bool own = o.con == d.player;
	const double v = std::min(1.5, move_value(*c, Move{o.loc, to, reason, maybe, !own}));
	double f = own ? 1.0 + 0.8 * v : 1.0 / (1.0 + 0.8 * v);
	if(has_grave_effects(*c)) {
		if((to & LOCATION_REMOVED) && (o.loc & LOCATION_GRAVE))
			f *= own ? 0.6 : 1.5;           // banished from the GY: its GY effects are gone
		else if((to & LOCATION_GRAVE) && !(o.loc & LOCATION_GRAVE))
			f *= own ? 1.25 : 0.8;          // to the GY: they can be used from there
	}
	return f;
}

}  // namespace ygoe
