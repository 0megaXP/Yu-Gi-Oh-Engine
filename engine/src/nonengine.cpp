#include "nonengine.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "json.h"
#include "ocgapi_constants.h"
#include "profile.h"

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

uint32_t stop_bit(Interrupt::Kind k) {
	switch(k) {
	case Interrupt::NegateActivation: return kStopsActivation;
	case Interrupt::NegateEffect: return kStopsEffect;
	case Interrupt::NegateSummon: return kStopsSummon;
	case Interrupt::Removal: return kRemoves;
	case Interrupt::GyHate: return kStopsGy;
	case Interrupt::DeckHate: return kStopsDeck;
	case Interrupt::Draw: return kDraws;
	case Interrupt::Extender: return 0;
	default: return kOther;
	}
}

// An effect used on its owner's own turn that removes the other player's cards (Evenly
// Matched, Dark Ruler No More, Lightning Storm...): a board breaker, as side.cpp judges them.
bool breaks_boards(const CardDB& db, const CardInfo& c, uint32_t sid) {
	const std::string text = lower(db.text(sid));
	const bool mentions_theirs = text.find("your opponent") != std::string::npos || text.find("opponent controls") != std::string::npos ||
	                             text.find("on the field") != std::string::npos;
	if(!mentions_theirs)
		return false;
	for(const auto& e : c.effects) {
		bool own_turn = e.type & (ET_ACTIVATE | ET_IGNITION | ET_QUICK_O | ET_TRIGGER_O);
		if((e.type & ET_QUICK_O) && (e.range & LOCATION_HAND))
			own_turn = false;  // hand traps
		if(own_turn && (e.category & (CAT_DESTROY | CAT_REMOVE | CAT_TODECK | CAT_TOHAND | CAT_CONTROL | CAT_TOGRAVE | CAT_RELEASE |
		                              CAT_DISABLE)))
			return true;
	}
	return false;
}

std::vector<float> floats(const json::Value& v) {
	std::vector<float> out;
	for(size_t i = 0; i < v.size(); ++i)
		out.push_back(static_cast<float>(v[i].num(0)));
	return out;
}

double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }

}  // namespace

const char* ne_role_name(NeRole r) {
	switch(r) {
	case NeRole::Engine: return "engine";
	case NeRole::HandTrap: return "hand trap";
	case NeRole::Interrupt: return "interruption";
	case NeRole::BoardBreaker: return "board breaker";
	case NeRole::Floodgate: return "floodgate";
	case NeRole::Staple: return "staple";
	}
	return "?";
}

std::vector<std::string> stops_names(uint32_t bits) {
	static const char* names[] = {"activations", "effects", "summons", "removes cards", "GY effects", "Deck plays", "draws", "other"};
	std::vector<std::string> out;
	for(int i = 0; i < 8; ++i)
		if(bits & (1u << i))
			out.push_back(names[i]);
	return out;
}

uint32_t ne_stops(const CardRole& r) {
	uint32_t bits = 0;
	for(const auto& in : r.interrupts)
		bits |= stop_bit(in.kind);
	return bits;
}

uint16_t archetype_key(const CardDB& db, const Deck& deck) {
	std::map<uint16_t, int> count;
	for(auto* list : {&deck.main, &deck.extra})
		for(auto code : *list)
			if(auto c = db.get(db.script_id(code)); c && !classify(db, code).hand_trap)
				for(int i = 0; i < 4; ++i)
					if(uint16_t s = static_cast<uint16_t>(c->setcode >> (16 * i)))
						++count[s];
	uint16_t best = 0;
	int n = 3;  // at least 4 cards
	for(auto& [s, k] : count)
		if(k > n) {
			n = k;
			best = s;
		}
	return best;
}

NeCard classify_card(const CardDB& db, uint32_t code, const std::vector<uint16_t>& deck_sets) {
	NeCard out;
	out.code = code;
	const uint32_t sid = db.script_id(code);
	const CardInfo* c = db.get(sid);
	if(!c)
		return out;
	const CardRole role = classify(db, code, deck_sets);
	if(role.engine)
		return out;  // the archetype's own cards, its hand traps too (Elfnote Regina in Elfnote)
	for(const auto& in : role.interrupts)
		out.stops |= stop_bit(in.kind);
	if(role.hand_trap) {
		out.role = NeRole::HandTrap;
	} else if(out.stops) {
		out.role = NeRole::Interrupt;
	} else if(breaks_boards(db, *c, sid)) {
		out.role = NeRole::BoardBreaker;
		out.stops |= kRemoves;
	} else if(role.passive_mzone + role.passive_szone >= 0.3 && !(c->type & TYPE_MONSTER)) {
		out.role = NeRole::Floodgate;
	} else {
		return out;  // no interaction at all: a combo piece, even when not of the archetype
	}
	out.going_second = out.role == NeRole::BoardBreaker || role.needs_empty_field;
	return out;
}

std::vector<NeCard> classify_nonengine(const CardDB& db, const Deck& deck) {
	const auto sets = dominant_setcodes(db, deck);
	std::map<uint32_t, NeCard> by;
	for(auto code : deck.main) {
		auto it = by.find(code);
		if(it == by.end())
			it = by.emplace(code, classify_card(db, code, sets)).first;
		++it->second.copies;
	}
	for(auto code : deck.side) {
		auto it = by.find(code);
		if(it == by.end())
			it = by.emplace(code, classify_card(db, code, sets)).first;
		++it->second.side_copies;
	}
	std::vector<NeCard> out;
	for(auto& [code, c] : by)
		out.push_back(c);
	return out;
}

std::vector<float> ne_features(const NeSituation& s) {
	std::vector<float> f;
	f.reserve(kNeFeatures);
	f.push_back(1.0f);
	f.push_back(static_cast<float>(std::clamp(s.search_gain * 10.0, -3.0, 3.0)));
	f.push_back(static_cast<float>(std::min(s.their_plays, 10)) / 5.0f);
	f.push_back(s.their_turn ? 1.0f : 0.0f);
	f.push_back(s.deck_play ? 1.0f : 0.0f);
	f.push_back(s.choke ? 1.0f : 0.0f);
	f.push_back(static_cast<float>(std::min(s.my_interruptions, 6)) / 3.0f);
	f.push_back(static_cast<float>(std::min(s.turn, 12)) / 12.0f);
	f.push_back(static_cast<float>(std::min(s.their_hand, 10)) / 6.0f);
	f.push_back(static_cast<float>(std::min(s.their_field, 12)) / 6.0f);
	f.push_back(static_cast<float>(std::min(s.my_field, 12)) / 6.0f);
	f.push_back(static_cast<float>(std::min(s.chain_depth, 6)) / 3.0f);
	f.push_back(s.hand_trap ? 1.0f : 0.0f);
	for(int b = 0; b < 7; ++b)
		f.push_back((s.stops & (1u << b)) ? 1.0f : 0.0f);
	return f;
}

bool NeTiming::parse(const std::string& text, std::string* err) {
	std::string e;
	auto v = json::Value::parse(text, &e);
	use_ = floats(v["use"]);
	hold_ = floats(v["hold"]);
	if(use_.size() != static_cast<size_t>(kNeFeatures) || hold_.size() != use_.size()) {
		if(err)
			*err = "non-engine timing: " + (e.empty() ? std::string("expected ") + std::to_string(kNeFeatures) + " weights per model" : e);
		use_.clear();
		hold_.clear();
		return false;
	}
	return true;
}

double NeTiming::gain(const NeSituation& s) const {
	if(empty())
		return 0;
	const auto f = ne_features(s);
	double u = 0, h = 0;
	for(size_t i = 0; i < f.size(); ++i) {
		u += use_[i] * f[i];
		h += hold_[i] * f[i];
	}
	const double g = sigmoid(u) - sigmoid(h);
	return std::isfinite(g) ? g : 0;
}

bool NeValues::parse(const std::string& text, std::string* err) {
	std::string e;
	auto v = json::Value::parse(text, &e);
	if(!v.is_object()) {
		if(err)
			*err = "non-engine values: " + (e.empty() ? std::string("not a JSON object") : e);
		return false;
	}
	table_.clear();
	for(const auto& x : v["entries"].items()) {
		Entry en;
		en.delta = x["delta"].num(0);
		en.games = static_cast<int>(x["games"].integer(0));
		table_[{static_cast<uint16_t>(x["me"].integer(0)), static_cast<uint16_t>(x["opp"].integer(0)), x["first"].integer(0) != 0,
		        static_cast<uint32_t>(x["code"].integer(0))}] = en;
	}
	return true;
}

const NeValues::Entry* NeValues::get(uint16_t me, uint16_t opp, bool first, uint32_t sid) const {
	auto it = table_.find({me, opp, first, sid});
	return it == table_.end() ? nullptr : &it->second;
}

std::vector<NeChoice> rank_nonengine(const CardDB& db, const Deck& deck, const std::vector<Deck>& opponents) {
	std::vector<NeChoice> out;
	const uint16_t me = archetype_key(db, deck);
	const NeValues* values = db.nonengine_values().get();
	for(const auto& c : classify_nonengine(db, deck)) {
		if(c.engine())
			continue;
		NeChoice ch;
		ch.card = c;
		const uint32_t sid = db.script_id(c.code);
		double sum[2] = {0, 0};
		int n[2] = {0, 0};
		auto add = [&](uint16_t opp, bool first, double weight) {
			const NeValues::Entry* e = values ? values->get(me, opp, first, sid) : nullptr;
			if(!e || e->games < 10)
				return false;
			sum[first] += weight * e->delta;
			n[first] += 1;
			ch.games += e->games;
			return true;
		};
		for(bool first : {true, false}) {
			for(const auto& o : opponents)
				add(archetype_key(db, o), first, 1.0);
			if(!n[first])
				add(0, first, 1.0);  // the whole format
		}
		ch.measured = n[0] + n[1] > 0;
		ch.measured_first = n[1] > 0;
		ch.measured_second = n[0] > 0;
		ch.value_first = n[1] ? sum[1] / n[1] : 0;
		ch.value_second = n[0] ? sum[0] / n[0] : 0;
		out.push_back(ch);
	}
	std::sort(out.begin(), out.end(), [](const NeChoice& a, const NeChoice& b) {
		if(a.measured != b.measured)
			return a.measured;
		return a.value_first + a.value_second > b.value_first + b.value_second;
	});
	return out;
}

}  // namespace ygoe
