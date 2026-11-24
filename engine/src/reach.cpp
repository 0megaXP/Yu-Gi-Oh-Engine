#include "reach.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "carddb.h"
#include "duel.h"
#include "json.h"
#include "ocgapi_constants.h"

namespace ygoe {

const char* reach_how_name(uint8_t how) {
	switch(how) {
	case kReachSearch: return "adds";
	case kReachSummon: return "summons";
	case kReachExtraSummon: return "summons from the Extra Deck";
	case kReachSend: return "sends";
	case kReachMaterial: return "material for";
	default: return "uses";
	}
}

namespace {

constexpr uint32_t kTake = LOCATION_DECK | LOCATION_EXTRA | LOCATION_GRAVE | LOCATION_REMOVED;
constexpr uint32_t kFiller = 89631139;  // Blue-Eyes White Dragon: the other player's Deck

struct Reader {
	const uint8_t* p;
	const uint8_t* end;
	template <class T> T get() {
		T v{};
		if(p + sizeof(T) <= end)
			std::memcpy(&v, p, sizeof(T));
		p += sizeof(T);
		return v;
	}
};

// Watches the rules core's messages while a card's effect is used: the lists of cards it offers
// to pick from (Deck, Extra Deck, GY, banished) and what happens to the cards picked.
class Watch {
public:
	explicit Watch(uint32_t self) : self_(self) {}
	struct Pick {
		std::vector<uint32_t> codes;
		uint8_t how = 0;
	};
	std::vector<Pick> picks;
	std::vector<std::pair<uint32_t, uint8_t>> direct;  // moves with no list before them

	void read(const uint8_t* data, uint32_t len) {
		Reader all{data, data + len};
		while(all.p + 4 <= all.end) {
			uint32_t size = all.get<uint32_t>();
			const uint8_t* msg = all.p;
			all.p += size;
			if(size == 0 || all.p > all.end)
				break;
			one(msg, msg + size);
		}
	}

private:
	uint32_t self_;
	void list(Reader& r, uint32_t n, bool tribute) {
		Pick pk;
		for(uint32_t i = 0; i < n && r.p < r.end; ++i) {
			uint32_t code = r.get<uint32_t>();
			uint8_t con = r.get<uint8_t>(), loc = r.get<uint8_t>();
			r.get<uint32_t>();
			if(tribute)
				r.get<uint8_t>();
			else
				r.get<uint32_t>();
			if(con == 0 && (loc & kTake) && code && code != self_)
				pk.codes.push_back(code);
		}
		if(!pk.codes.empty())
			picks.push_back(std::move(pk));
	}
	void one(const uint8_t* msg, const uint8_t* end) {
		Reader r{msg + 1, end};
		switch(msg[0]) {
		case MSG_SELECT_CARD: {
			r.get<uint8_t>();  // player
			r.get<uint8_t>();  // cancelable
			r.get<uint32_t>();
			r.get<uint32_t>();
			list(r, r.get<uint32_t>(), false);
			break;
		}
		case MSG_SELECT_TRIBUTE: {
			r.get<uint8_t>();
			r.get<uint8_t>();
			r.get<uint32_t>();
			r.get<uint32_t>();
			list(r, r.get<uint32_t>(), true);
			break;
		}
		case MSG_SELECT_UNSELECT_CARD: {
			r.get<uint8_t>();
			r.get<uint8_t>();
			r.get<uint8_t>();
			r.get<uint32_t>();
			r.get<uint32_t>();
			list(r, r.get<uint32_t>(), false);
			break;
		}
		case MSG_MOVE: {
			uint32_t code = r.get<uint32_t>();
			uint8_t fc = r.get<uint8_t>(), fl = r.get<uint8_t>();
			r.get<uint32_t>();
			r.get<uint32_t>();
			uint8_t tc = r.get<uint8_t>(), tl = r.get<uint8_t>();
			(void)tc;
			if(fc != 0 || !(fl & kTake) || !code || code == self_ || (fl & LOCATION_OVERLAY))
				break;
			uint8_t how = 0;
			if(tl & LOCATION_HAND)
				how = (fl & (LOCATION_DECK | LOCATION_GRAVE | LOCATION_REMOVED)) ? kReachSearch : 0;
			else if(tl & LOCATION_MZONE)
				how = (fl & LOCATION_EXTRA) ? kReachExtraSummon : kReachSummon;
			else if(tl & (LOCATION_GRAVE | LOCATION_REMOVED))
				how = (fl & (LOCATION_DECK | LOCATION_EXTRA)) ? kReachSend : 0;  // GY to banished: a cost
			else if(tl & LOCATION_SZONE)
				how = kReachOther;  // placed in the Spell & Trap Zone
			if(!how)
				break;
			// the list it was picked from: every card in it could have been
			for(auto it = picks.rbegin(); it != picks.rend(); ++it)
				if(!it->how && std::find(it->codes.begin(), it->codes.end(), code) != it->codes.end()) {
					it->how = how;
					return;
				}
			direct.push_back({code, how});
			break;
		}
		default:
			break;
		}
	}
};

// Plays out one use of the card: its own options and triggers taken, the first choice elsewhere.
void drive(Duel& d, uint32_t sid, int max_steps = 60) {
	for(int step = 0; step < max_steps && !d.ended(); ++step) {
		const Decision& dec = d.decision();
		if(dec.type == DecisionType::None || dec.options.empty())
			return;
		if(dec.type == DecisionType::Idle || dec.type == DecisionType::Battle)
			return;  // back in the open state: the effect has resolved
		const Option* pick = &dec.options.front();
		switch(dec.type) {
		case DecisionType::Chain:
		case DecisionType::EffectYesNo:
		case DecisionType::YesNo: {
			// its own effects yes, others no
			const Option* yes = nullptr;
			const Option* no = nullptr;
			for(const auto& o : dec.options) {
				if((o.kind == OptKind::Activate || o.kind == OptKind::Yes) && (o.code == sid || dec.type != DecisionType::Chain))
					yes = yes ? yes : &o;
				if(o.kind == OptKind::Pass || o.kind == OptKind::No)
					no = no ? no : &o;
			}
			pick = yes ? yes : no ? no : pick;
			break;
		}
		case DecisionType::Card:
		case DecisionType::UnselectCard:
		case DecisionType::Tribute:
		case DecisionType::Sum:
			// the first card offered (finishing a selection when it can)
			for(const auto& o : dec.options)
				if(o.kind == OptKind::Select) {
					pick = &o;
					break;
				}
			break;
		default:
			break;
		}
		if(!d.respond(pick->response) && !d.respond(dec.options.front().response))
			return;
	}
}

// To the open state: what the starting cards set off is let go (no responses, the first choice).
void settle(Duel& d) {
	for(int step = 0; step < 30 && !d.ended(); ++step) {
		const Decision& dec = d.decision();
		if(dec.type == DecisionType::None || dec.options.empty() || dec.type == DecisionType::Idle)
			return;
		const Option* pick = &dec.options.front();
		for(const auto& o : dec.options)
			if(o.kind == OptKind::Pass || o.kind == OptKind::No) {
				pick = &o;
				break;
			}
		if(!d.respond(pick->response) && !d.respond(dec.options.front().response))
			return;
	}
}

uint32_t pick_normal(const CardDB& db, const std::vector<uint32_t>& pool, uint32_t attr_not) {
	for(uint32_t c : pool) {
		const CardInfo* ci = db.get(db.script_id(c));
		if(ci && (ci->type & TYPE_NORMAL) && (ci->type & TYPE_MONSTER) && ci->lv() == 4 && ci->attribute != attr_not)
			return c;
	}
	return 0;
}

}  // namespace

std::vector<ReachEdge> probe_card(const CardDB& db, ScriptCache& scripts, uint32_t code,
                                  const std::vector<uint32_t>& main_pool, const std::vector<uint32_t>& extra_pool) {
	std::vector<ReachEdge> out;
	const uint32_t sid = db.script_id(code);
	const CardInfo* ci = db.get(sid);
	if(!ci || (ci->type & TYPE_TOKEN))
		return out;
	const bool extra = ci->is_extra_deck();
	const bool monster = (ci->type & TYPE_MONSTER) != 0;
	const bool spell = (ci->type & TYPE_SPELL) != 0;

	// two monsters to pay costs and be materials, and some cards in the GY to revive or add
	std::vector<uint32_t> fodder;
	uint32_t a = pick_normal(db, main_pool, 0);
	if(!a)
		a = 89631139;
	uint32_t b = pick_normal(db, main_pool, db.get(db.script_id(a)) ? db.get(db.script_id(a))->attribute : 0);
	if(!b)
		b = a;
	std::vector<uint32_t> gy;
	for(size_t i = 0; i < main_pool.size() && gy.size() < 12; i += std::max<size_t>(1, main_pool.size() / 12))
		gy.push_back(main_pool[i]);

	enum Where { kHand, kField, kGrave };
	std::vector<Where> wheres;
	if(!extra)
		wheres.push_back(kHand);
	if(monster || (spell && (ci->type & (TYPE_CONTINUOUS | TYPE_FIELD))) || (ci->type & TYPE_TRAP))
		wheres.push_back(kField);
	wheres.push_back(kGrave);

	std::map<std::pair<uint32_t, uint8_t>, bool> seen;
	auto add = [&](uint32_t to, uint8_t how) {
		const uint32_t off = db.official_id(to);
		if(!off || off == code || to == sid)
			return;
		if(seen.emplace(std::make_pair(off, how), true).second)
			out.push_back({code, off, how});
	};

	for(Where w : wheres) {
		DuelSetup s;
		s.seed = {code, 7, 11, 13};
		auto& p0 = s.players[0];
		p0.main = main_pool;
		p0.extra = extra_pool;
		if(extra)
			p0.extra.push_back(code);
		PlacedCard f1{a, LOCATION_MZONE, 0, POS_FACEUP_ATTACK}, f2{b, LOCATION_MZONE, 1, POS_FACEUP_ATTACK};
		p0.placed = {f1, f2};
		for(size_t i = 0; i < gy.size(); ++i)
			p0.placed.push_back({gy[i], LOCATION_GRAVE, static_cast<uint32_t>(i), POS_FACEUP});
		p0.hand = {a, b};
		if(w == kHand)
			p0.hand.push_back(code);
		else if(w == kField)
			p0.placed.push_back(monster ? PlacedCard{code, LOCATION_MZONE, 2, POS_FACEUP_ATTACK}
			                            : PlacedCard{code, LOCATION_SZONE, (ci->type & TYPE_FIELD) ? 5u : 2u,
			                                         static_cast<uint32_t>((ci->type & TYPE_TRAP) ? POS_FACEDOWN : POS_FACEUP)});
		else
			p0.placed.push_back({code, LOCATION_GRAVE, static_cast<uint32_t>(gy.size()), POS_FACEUP});
		s.players[1].main.assign(40, kFiller);

		// what the card can do from there: its options at the open state, each tried once
		Duel probe(db, scripts);
		std::string err;
		const bool started = probe.start(s, &err);
		if(started)
			settle(probe);
		if(std::getenv("YGOE_REACH_DEBUG")) {
			std::fprintf(stderr, "probe %u where %d: start %d (%s) decision %s, options:", code, int(w), started, err.c_str(),
			             decision_name(probe.decision().type));
			for(const auto& o : probe.decision().options)
				std::fprintf(stderr, " {%s}", o.label.c_str());
			std::fprintf(stderr, "\n");
		}
		if(!started || probe.decision().type != DecisionType::Idle)
			continue;
		std::vector<size_t> mine;
		for(size_t i = 0; i < probe.decision().options.size() && mine.size() < 6; ++i) {
			const Option& o = probe.decision().options[i];
			if(o.code == sid && (o.kind == OptKind::Activate || o.kind == OptKind::Summon || o.kind == OptKind::SpSummon))
				mine.push_back(i);
		}
		for(size_t k = 0; k < mine.size(); ++k) {
			Duel d(db, scripts);
			Watch watch(sid);
			d.set_tap([&](const uint8_t* buf, uint32_t n) { watch.read(buf, n); });
			if(!d.start(s))
				continue;
			settle(d);
			watch.picks.clear();
			watch.direct.clear();
			if(d.decision().options.size() <= mine[k])
				continue;
			if(!d.respond(d.decision().options[mine[k]].response))
				continue;
			drive(d, sid);
			for(const auto& pk : watch.picks)
				if(pk.how)
					for(uint32_t c : pk.codes)
						add(c, pk.how);
			// moves its effect didn't choose (the top cards, at random) aren't reach
		}
	}
	return out;
}

std::vector<ReachEdge> probe_materials(const CardDB& db, ScriptCache& scripts, const Deck& deck, int max_pairs) {
	std::vector<ReachEdge> out;
	if(deck.extra.empty())
		return out;
	std::vector<uint32_t> monsters;
	for(uint32_t c : deck.main) {
		const CardInfo* ci = db.get(db.script_id(c));
		if(ci && (ci->type & TYPE_MONSTER) && std::find(monsters.begin(), monsters.end(), c) == monsters.end())
			monsters.push_back(c);
	}
	std::set<std::tuple<uint32_t, uint32_t>> seen;
	int pairs = 0;
	for(size_t i = 0; i < monsters.size() && pairs < max_pairs; ++i)
		for(size_t j = i; j < monsters.size() && pairs < max_pairs; ++j, ++pairs) {
			DuelSetup s;
			auto& p0 = s.players[0];
			p0.main.assign(20, kFiller);
			p0.extra = deck.extra;
			p0.placed = {{monsters[i], LOCATION_MZONE, 0, POS_FACEUP_ATTACK}, {monsters[j], LOCATION_MZONE, 1, POS_FACEUP_ATTACK}};
			s.players[1].main.assign(40, kFiller);
			Duel d(db, scripts);
			if(!d.start(s))
				continue;
			settle(d);
			if(d.decision().type != DecisionType::Idle)
				continue;
			for(const auto& o : d.decision().options)
				if(o.kind == OptKind::SpSummon && (o.loc & LOCATION_EXTRA)) {
					const uint32_t e = db.official_id(o.code);
					for(uint32_t m : {monsters[i], monsters[j]})
						if(seen.insert({m, e}).second)
							out.push_back({m, e, kReachMaterial});
				}
		}
	return out;
}

bool ReachTable::load(const std::string& path) {
	std::ifstream in(path);
	if(!in)
		return false;
	std::string line;
	while(std::getline(in, line)) {
		if(line.empty() || line[0] == '#')
			continue;
		std::istringstream ls(line);
		ReachEdge e;
		unsigned how = 0;
		if(ls >> e.from >> e.to >> how) {
			e.how = static_cast<uint8_t>(how);
			by_from_[e.from].push_back(e);
			++n_;
		}
	}
	return n_ > 0;
}

const std::vector<ReachEdge>* ReachTable::from(uint32_t code) const {
	auto it = by_from_.find(code);
	return it == by_from_.end() ? nullptr : &it->second;
}

namespace {

// A card's archetypes (base codes, without the sub-archetype bits).
std::vector<uint16_t> sets_of(const CardDB& db, uint32_t code) {
	std::vector<uint16_t> out;
	const CardInfo* ci = db.get(db.script_id(code));
	if(!ci)
		return out;
	for(int k = 0; k < 4; ++k) {
		uint16_t s = static_cast<uint16_t>((ci->setcode >> (16 * k)) & 0xffff);
		if(s)
			out.push_back(static_cast<uint16_t>(s & 0x0fff));
	}
	return out;
}

}  // namespace

BridgeReport find_bridges(const CardDB& db, ScriptCache& scripts, const Deck& deck, const ReachTable* table) {
	BridgeReport rep;
	std::vector<uint32_t> cards;  // distinct, Main + Extra + Side
	for(const auto* v : {&deck.main, &deck.extra, &deck.side})
		for(uint32_t c : *v)
			if(std::find(cards.begin(), cards.end(), c) == cards.end())
				cards.push_back(c);
	std::set<uint32_t> in_deck(cards.begin(), cards.end());

	// the deck's archetypes: those with 3 or more of its cards
	std::map<uint16_t, int> count;
	for(uint32_t c : cards)
		for(uint16_t s : sets_of(db, c))
			++count[s];
	for(auto& [s, n] : count)
		if(n >= 3)
			rep.groups.push_back(s);
	std::sort(rep.groups.begin(), rep.groups.end(), [&](uint16_t x, uint16_t y) { return count[x] > count[y]; });
	auto groups_of = [&](uint32_t c) {
		std::vector<uint16_t> g;
		for(uint16_t s : sets_of(db, c))
			if(std::find(rep.groups.begin(), rep.groups.end(), s) != rep.groups.end())
				g.push_back(s);
		return g;
	};

	// what each card reaches: the scanned table, else tried here against the deck itself
	std::vector<uint32_t> main_pool, extra_pool;
	for(uint32_t c : cards) {
		const CardInfo* ci = db.get(db.script_id(c));
		if(ci && ci->is_extra_deck())
			extra_pool.push_back(c);
		else if(ci)
			main_pool.push_back(c);
	}
	std::vector<ReachEdge> edges;
	for(uint32_t c : cards) {
		const auto* t = table ? table->from(c) : nullptr;
		if(t) {
			edges.insert(edges.end(), t->begin(), t->end());
		} else {
			auto p = probe_card(db, scripts, c, main_pool, extra_pool);
			edges.insert(edges.end(), p.begin(), p.end());
		}
	}
	auto mat = probe_materials(db, scripts, deck);
	edges.insert(edges.end(), mat.begin(), mat.end());

	// a bridge: a card of the list whose reach crosses into another of the deck's archetypes
	std::map<std::pair<uint32_t, uint16_t>, Bridge> inside, outside;
	for(const auto& e : edges) {
		if(!in_deck.count(e.from))
			continue;
		const auto gf = groups_of(e.from);
		if(in_deck.count(e.to)) {
			const auto gt = groups_of(e.to);
			for(uint16_t t : gt) {
				if(std::find(gf.begin(), gf.end(), t) != gf.end())
					continue;
				Bridge& b = inside[{e.from, t}];
				b.code = e.from;
				b.from_set = gf.empty() ? 0 : gf.front();
				b.to_set = t;
				b.edges.push_back(e);
				b.weight += e.how == kReachMaterial ? 0.5 : 1.0;
			}
		} else {
			// outside the list: into an archetype the deck doesn't play (what it adds, summons or
			// sends; being generic material for another deck's monsters doesn't count)
			if(e.how == kReachMaterial)
				continue;
			for(uint16_t t : sets_of(db, e.to)) {
				if(std::find(rep.groups.begin(), rep.groups.end(), t) != rep.groups.end())
					continue;
				Bridge& b = outside[{e.from, t}];
				b.code = e.from;
				b.from_set = gf.empty() ? 0 : gf.front();
				b.to_set = t;
				b.edges.push_back(e);
				b.weight += 1.0;
			}
		}
	}
	// a card of no archetype of the deck is a bridge only when it joins two of them (it reaches
	// both, or is reached... as material for both)
	std::map<uint32_t, std::set<uint16_t>> generic_reach;
	for(auto& [k, b] : inside)
		if(!b.from_set)
			generic_reach[b.code].insert(b.to_set);
	for(auto& [k, b] : inside)
		if(b.from_set || generic_reach[b.code].size() >= 2)
			rep.bridges.push_back(b);
	for(auto& [k, b] : outside)
		if(b.weight >= 2)
			rep.outside.push_back(b);
	auto by_weight = [](const Bridge& x, const Bridge& y) { return x.weight > y.weight; };
	std::sort(rep.bridges.begin(), rep.bridges.end(), by_weight);
	std::sort(rep.outside.begin(), rep.outside.end(), by_weight);
	if(rep.outside.size() > 30)
		rep.outside.resize(30);
	return rep;
}

std::string BridgeReport::to_json(const CardDB& db) const {
	auto bridge = [&](std::ostringstream& o, const Bridge& b) {
		o << "{\"code\":" << b.code << ",\"name\":" << json::quote(db.name(db.script_id(b.code))) << ",\"from_set\":" << b.from_set
		  << ",\"to_set\":" << b.to_set << ",\"weight\":" << b.weight << ",\"reaches\":[";
		for(size_t i = 0; i < b.edges.size() && i < 12; ++i)
			o << (i ? "," : "") << "{\"code\":" << b.edges[i].to << ",\"name\":" << json::quote(db.name(db.script_id(b.edges[i].to)))
			  << ",\"how\":" << json::quote(reach_how_name(b.edges[i].how)) << "}";
		o << "]}";
	};
	std::ostringstream o;
	o << "{\"groups\":[";
	for(size_t i = 0; i < groups.size(); ++i)
		o << (i ? "," : "") << groups[i];
	o << "],\"bridges\":[";
	for(size_t i = 0; i < bridges.size(); ++i) {
		if(i)
			o << ",";
		bridge(o, bridges[i]);
	}
	o << "],\"outside\":[";
	for(size_t i = 0; i < outside.size(); ++i) {
		if(i)
			o << ",";
		bridge(o, outside[i]);
	}
	o << "]}";
	return o.str();
}

}  // namespace ygoe
