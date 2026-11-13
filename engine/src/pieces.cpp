#include <locale>
#include "pieces.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "json.h"
#include "ocgapi_constants.h"

namespace ygoe {

namespace {

enum Zone : uint8_t { ZHand, ZField, ZGrave, ZBanished, ZNone };
const char* kZoneName[] = {"hand", "field", "gy", "banished", "?"};

Zone zone_of(uint8_t loc) {
	if(loc & LOCATION_HAND)
		return ZHand;
	if(loc & (LOCATION_MZONE | LOCATION_SZONE))
		return ZField;
	if(loc & LOCATION_GRAVE)
		return ZGrave;
	if(loc & LOCATION_REMOVED)
		return ZBanished;
	return ZNone;
}

using Key = std::pair<uint8_t, uint32_t>;  // zone, script id
using Snapshot = std::map<Key, int>;

Snapshot snapshot(const Board& b, uint8_t p) {
	Snapshot s;
	auto add = [&](const std::vector<CardView>& v, Zone z) {
		for(const auto& c : v)
			if(c.code)
				++s[{z, c.code}];
	};
	add(b.hand[p], ZHand);
	add(b.mzone[p], ZField);
	add(b.szone[p], ZField);
	add(b.grave[p], ZGrave);
	add(b.banished[p], ZBanished);
	return s;
}

bool is_pass(const std::string& l) {
	return l == "Don't respond" || l.rfind("Don't use", 0) == 0 || l.rfind("No:", 0) == 0 || l == "Done selecting" ||
	       l == "Cancel";
}

bool starts_action(const Step& s) {
	return s.player == 0 && !s.minor && !is_pass(s.label) &&
	       (s.type == DecisionType::Idle || s.type == DecisionType::Chain || s.type == DecisionType::EffectYesNo);
}

struct ActionInfo {
	std::string text;
	uint32_t actor = 0;
	Zone actor_zone = ZNone;
	bool normal_summon = false;
	struct Use {
		Key key;
		int producer;  // action index, -1 = starting hand / start of turn
	};
	std::vector<Use> uses;
	std::set<uint32_t> windows;  // hand traps that could respond
};

// Replays t's steps by label. `on_start(k, option)` runs before player 0's k-th action is
// answered; `on_window(k, decision)` for each decision of player 1 (which is passed).
// Returns the number of steps that replayed.
template <class Start, class Window>
size_t walk(Duel& d, const Trajectory& t, Start on_start, Window on_window) {
	int action = -1;
	size_t done = 0;
	for(const Step& s : t.steps) {
		int guard = 0;
		while(!d.ended() && d.decision().player != s.player && guard++ < 20) {
			const Decision& dec = d.decision();
			if(dec.player == 1)
				on_window(action, dec);
			size_t pass = 0;
			for(size_t o = 0; o < dec.options.size(); ++o)
				if(dec.options[o].kind == OptKind::Pass)
					pass = o;
			if(dec.options.empty() || !d.respond(dec.options[pass].response))
				return done;
		}
		if(d.ended())
			break;
		const Decision& dec = d.decision();
		if(dec.player == 1)
			on_window(action, dec);
		size_t pick = SIZE_MAX;
		for(size_t o = 0; o < dec.options.size(); ++o)
			if(dec.options[o].label == s.label) {
				pick = o;
				break;
			}
		if(pick == SIZE_MAX && (s.minor || s.label == "Don't respond") && !dec.options.empty())
			pick = 0;
		if(pick == SIZE_MAX)
			break;
		if(starts_action(s))
			on_start(++action, s, dec.options[pick]);
		else if(s.player == 0 && !s.minor && !is_pass(s.label) && action >= 0)
			on_start(action, s, dec.options[pick]);  // a choice inside the current action
		if(!d.respond(dec.options[pick].response))
			break;
		++done;
	}
	return done;
}

}  // namespace

int LineSplit::piece_of(int action) const {
	for(size_t i = 0; i < pieces.size(); ++i)
		if(action >= pieces[i].first && action <= pieces[i].last)
			return static_cast<int>(i);
	return -1;
}

LineSplit split_line(const CardDB& db, ScriptCache& scripts, const std::vector<uint32_t>& pool, const DuelSetup& setup,
                     const Trajectory& t, const std::vector<uint32_t>& hand_traps) {
	LineSplit out;
	std::vector<ActionInfo> acts;
	std::vector<Snapshot> snaps;  // before each action, then the end
	Snapshot end;
	{
		Duel d(db, scripts);
		d.set_declare_pool(pool);
		if(!d.replay(setup, {}))
			return out;
		walk(
		    d, t,
		    [&](int k, const Step& s, const Option& o) {
			    std::string text = s.label;
			    if(s.type == DecisionType::YesNo && text.rfind("Yes: ", 0) == 0)
				    text = text.substr(5);
			    if(k == static_cast<int>(acts.size())) {
				    snaps.push_back(snapshot(d.board(), 0));
				    ActionInfo a;
				    a.text = text;
				    a.actor = o.code;
				    a.actor_zone = zone_of(o.loc);
				    a.normal_summon = o.kind == OptKind::Summon || (o.kind == OptKind::MSet && (o.loc & LOCATION_HAND));
				    acts.push_back(std::move(a));
			    } else if(k >= 0 && k < static_cast<int>(acts.size())) {
				    acts[k].text += "  →  " + text;
			    }
		    },
		    [](int, const Decision&) {});
		end = snapshot(d.board(), 0);
	}
	const int n = static_cast<int>(acts.size());
	out.actions = n;
	if(n == 0)
		return out;
	snaps.push_back(end);

	// Where each card came from.
	std::map<Key, std::vector<int>> producer;
	for(auto& [k, c] : snaps[0])
		producer[k].assign(c, -1);
	for(int k = 0; k < n; ++k) {
		const Snapshot& before = snaps[k];
		const Snapshot& after = snaps[k + 1];
		auto& a = acts[k];
		bool actor_moved = false;
		for(auto& [key, c] : before) {
			auto it = after.find(key);
			int lost = c - (it == after.end() ? 0 : it->second);
			for(int i = 0; i < lost; ++i) {
				auto& st = producer[key];
				int p = -1;
				if(!st.empty()) {
					p = st.back();
					st.pop_back();
				}
				a.uses.push_back({key, p});
				if(key.second == a.actor && key.first == a.actor_zone)
					actor_moved = true;
			}
		}
		if(!actor_moved && a.actor && a.actor_zone != ZNone) {
			Key key{a.actor_zone, a.actor};
			auto& st = producer[key];
			a.uses.push_back({key, st.empty() ? -1 : st.back()});
		}
		for(auto& [key, c] : after) {
			auto it = before.find(key);
			int gained = c - (it == before.end() ? 0 : it->second);
			for(int i = 0; i < gained; ++i)
				producer[key].push_back(k);
		}
	}

	// Which hand traps can answer each action.
	if(!hand_traps.empty()) {
		std::set<uint32_t> ids;
		for(auto h : hand_traps)
			ids.insert(db.script_id(h));
		DuelSetup s2 = setup;
		for(auto h : hand_traps)
			s2.players[1].hand.push_back(h);
		Duel d(db, scripts);
		d.set_declare_pool(pool);
		if(d.replay(s2, {})) {
			walk(
			    d, t, [](int, const Step&, const Option&) {},
			    [&](int k, const Decision& dec) {
				    for(const auto& o : dec.options)
					    if(o.kind == OptKind::Activate && ids.count(o.code))
						    acts[std::clamp(k, 0, n - 1)].windows.insert(o.code);
			    });
		}
	}

	// Cut where the fewest cards carry over (DP over cut positions).
	auto interface_after = [&](int k) {  // cards made by actions <= k and used by actions > k
		std::set<Key> keys;
		for(int j = k + 1; j < n; ++j)
			for(auto& u : acts[j].uses)
				if(u.producer >= 0 && u.producer <= k)
					keys.insert(u.key);
		return static_cast<int>(keys.size());
	};
	auto seg_pen = [](int len) { return len < 2 ? 3.0 : (len > 5 ? 1.0 * (len - 5) : 0.0); };
	std::vector<double> best(n + 1, 1e18);
	std::vector<int> from(n + 1, 0);
	best[0] = 0;
	for(int j = 1; j <= n; ++j)
		for(int i = 0; i < j; ++i) {
			double c = best[i] + seg_pen(j - i) + (j < n ? 0.4 + interface_after(j - 1) : 0.0);
			if(n <= 3)
				c = i == 0 ? 0 : 1e18;
			if(c < best[j]) {
				best[j] = c;
				from[j] = i;
			}
		}
	std::vector<std::pair<int, int>> segs;
	for(int j = n; j > 0; j = from[j])
		segs.push_back({from[j], j - 1});
	std::reverse(segs.begin(), segs.end());

	auto add_card = [](std::vector<PieceCard>& v, const Key& k) {
		for(auto& c : v)
			if(c.code == k.second && c.where == kZoneName[k.first])
				return;
		v.push_back({k.second, kZoneName[k.first]});
	};
	for(auto [a, b] : segs) {
		ComboPiece p;
		p.first = a;
		p.last = b;
		std::map<uint32_t, int> stops;
		for(int k = a; k <= b; ++k) {
			p.steps.push_back(acts[k].text);
			p.normal_summon |= acts[k].normal_summon;
			for(auto w : acts[k].windows)
				stops.emplace(w, k - a);
			for(auto& u : acts[k].uses) {
				if(u.producer < 0 && u.key.first == ZHand) {
					// Count each starting-hand copy once.
					auto have = std::count(p.needs_hand.begin(), p.needs_hand.end(), u.key.second);
					auto in_hand = snaps[0].count(u.key) ? snaps[0].at(u.key) : 0;
					if(have < in_hand)
						p.needs_hand.push_back(u.key.second);
				} else if(u.producer >= 0 && u.producer < a) {
					add_card(p.needs_earlier, u.key);
				}
			}
		}
		for(int k = b + 1; k < n; ++k)
			for(auto& u : acts[k].uses)
				if(u.producer >= a && u.producer <= b)
					add_card(p.gives, u.key);
		for(auto& [key, st] : producer)
			if(key.first == ZField || key.first == ZHand)
				for(int pr : st)
					if(pr >= a && pr <= b)
						add_card(p.leaves, key);
		p.stopped_by.assign(stops.begin(), stops.end());
		std::stable_sort(p.stopped_by.begin(), p.stopped_by.end(), [](auto& x, auto& y) { return x.second < y.second; });
		out.pieces.push_back(std::move(p));
	}
	return out;
}

// ------------------------------------------------------------------ library

namespace {

std::string key_of(const ComboPiece& p) {
	std::vector<uint32_t> a = p.needs_hand, b;
	for(auto& x : p.needs_earlier)
		b.push_back(x.code);
	std::sort(a.begin(), a.end());
	std::sort(b.begin(), b.end());
	b.erase(std::unique(b.begin(), b.end()), b.end());
	std::string k;
	for(auto v : a)
		k += std::to_string(v) + ",";
	k += "|";
	for(auto v : b)
		k += std::to_string(v) + ",";
	k += p.normal_summon ? "|NS" : "|";
	return k;
}

}  // namespace

void add_to_library(std::vector<LibraryPiece>& lib, const LineSplit& split, double value) {
	for(const auto& p : split.pieces) {
		std::string k = key_of(p);
		auto it = std::find_if(lib.begin(), lib.end(), [&](const LibraryPiece& l) { return key_of(l.piece) == k; });
		if(it == lib.end()) {
			lib.push_back({p, 1, value, value});
		} else {
			// Keep the example from the best line, with what could stop it in any line.
			auto stops = it->piece.stopped_by;
			if(value > it->best) {
				it->piece = p;
				it->best = value;
			}
			for(auto& s : stops) {
				auto f = std::find_if(it->piece.stopped_by.begin(), it->piece.stopped_by.end(),
				                      [&](auto& x) { return x.first == s.first; });
				if(f == it->piece.stopped_by.end())
					it->piece.stopped_by.push_back(s);
			}
			it->value = (it->value * it->count + value) / (it->count + 1);
			++it->count;
		}
	}
}

void sort_library(std::vector<LibraryPiece>& lib, size_t max) {
	std::stable_sort(lib.begin(), lib.end(), [](const LibraryPiece& a, const LibraryPiece& b) {
		if(a.piece.starter() != b.piece.starter())
			return a.piece.starter();
		if(a.count != b.count)
			return a.count > b.count;
		return a.value > b.value;
	});
	if(lib.size() > max)
		lib.resize(max);
}

namespace {

std::string piece_body(const CardDB& db, const ComboPiece& p) {
	using json::quote;
	auto card = [&](uint32_t code) {
		return "{\"code\":" + std::to_string(db.official_id(code)) + ",\"name\":" + quote(db.name(code)) + "}";
	};
	auto placed = [&](const std::vector<PieceCard>& v) {
		std::string o = "[";
		for(size_t i = 0; i < v.size(); ++i)
			o += (i ? "," : "") + std::string("{\"code\":") + std::to_string(db.official_id(v[i].code)) +
			     ",\"name\":" + quote(db.name(v[i].code)) + ",\"where\":" + quote(v[i].where) + "}";
		return o + "]";
	};
	std::ostringstream o;
	o.imbue(std::locale::classic());  // C number format, whatever locale the host app set
	o << "\"first\":" << p.first << ",\"last\":" << p.last << ",\"starter\":" << (p.starter() ? "true" : "false")
	  << ",\"steps\":[";
	for(size_t i = 0; i < p.steps.size(); ++i)
		o << (i ? "," : "") << quote(p.steps[i]);
	o << "],\"needs_hand\":[";
	for(size_t i = 0; i < p.needs_hand.size(); ++i)
		o << (i ? "," : "") << card(p.needs_hand[i]);
	o << "],\"needs_earlier\":" << placed(p.needs_earlier) << ",\"normal_summon\":" << (p.normal_summon ? "true" : "false")
	  << ",\"gives\":" << placed(p.gives) << ",\"leaves\":" << placed(p.leaves) << ",\"stopped_by\":[";
	for(size_t i = 0; i < p.stopped_by.size(); ++i) {
		std::string c = card(p.stopped_by[i].first);
		c.pop_back();
		o << (i ? "," : "") << c << ",\"step\":" << p.stopped_by[i].second << "}";
	}
	o << "]";
	return o.str();
}

}  // namespace

std::string pieces_json(const CardDB& db, const std::vector<ComboPiece>& pieces) {
	std::string o = "[";
	for(size_t i = 0; i < pieces.size(); ++i)
		o += (i ? ",{" : "{") + piece_body(db, pieces[i]) + "}";
	return o + "]";
}

std::string library_json(const CardDB& db, const std::vector<LibraryPiece>& lib, int lines) {
	std::string o = "[";
	for(size_t i = 0; i < lib.size(); ++i) {
		char share[32];
		std::snprintf(share, sizeof share, "%.3f", lines ? double(lib[i].count) / lines : 0.0);
		char val[32];
		std::snprintf(val, sizeof val, "%.3f", lib[i].value);
		o += (i ? ",{" : "{") + piece_body(db, lib[i].piece) + ",\"count\":" + std::to_string(lib[i].count) +
		     ",\"share\":" + share + ",\"value\":" + val + "}";
	}
	return o + "]";
}

// ------------------------------------------------------------------ guides

std::vector<LibraryPiece> parse_library(const CardDB& db, const std::string& text) {
	return parse_library(db, json::Value::parse(text));
}

std::vector<LibraryPiece> parse_library(const CardDB& db, const json::Value& v) {
	std::vector<LibraryPiece> out;
	auto sid = [&](const json::Value& c) { return db.script_id(static_cast<uint32_t>(c["code"].integer())); };
	auto placed = [&](const json::Value& a) {
		std::vector<PieceCard> r;
		for(const auto& c : a.items())
			r.push_back({sid(c), c["where"].str()});
		return r;
	};
	for(const auto& o : v.items()) {
		LibraryPiece lp;
		ComboPiece& p = lp.piece;
		p.first = static_cast<int>(o["first"].integer());
		p.last = static_cast<int>(o["last"].integer());
		for(const auto& s : o["steps"].items())
			p.steps.push_back(s.str());
		for(const auto& c : o["needs_hand"].items())
			p.needs_hand.push_back(sid(c));
		p.needs_earlier = placed(o["needs_earlier"]);
		p.normal_summon = o["normal_summon"].boolean();
		p.gives = placed(o["gives"]);
		p.leaves = placed(o["leaves"]);
		for(const auto& c : o["stopped_by"].items())
			p.stopped_by.push_back({sid(c), static_cast<int>(c["step"].integer())});
		lp.count = static_cast<int>(o["count"].integer(1));
		lp.value = o["value"].num(0);
		lp.best = lp.value;
		if(!p.steps.empty())
			out.push_back(std::move(lp));
	}
	return out;
}

PieceResources resources_of(const Board& b, uint8_t p) {
	PieceResources r;
	for(const auto& c : b.hand[p])
		if(c.code)
			r.hand.push_back(c.code);
	for(const auto* zone : {&b.mzone[p], &b.szone[p], &b.grave[p], &b.banished[p]})
		for(const auto& c : *zone)
			if(c.code)
				r.other.push_back(c.code);
	return r;
}

namespace {

bool can_play(const ComboPiece& p, const std::vector<uint32_t>& hand, const std::vector<uint32_t>& other, bool ns_used) {
	if(p.normal_summon && ns_used)
		return false;
	std::vector<uint32_t> h = hand;
	for(auto code : p.needs_hand) {
		auto it = std::find(h.begin(), h.end(), code);
		if(it == h.end())
			return false;
		h.erase(it);
	}
	for(const auto& c : p.needs_earlier)
		if(std::find(other.begin(), other.end(), c.code) == other.end() &&
		   std::find(h.begin(), h.end(), c.code) == h.end())
			return false;
	return true;
}

std::vector<std::string> split_labels(const std::string& step) {
	std::vector<std::string> out;
	const std::string sep = "  →  ";
	size_t a = 0;
	while(true) {
		size_t b = step.find(sep, a);
		out.push_back(step.substr(a, b == std::string::npos ? std::string::npos : b - a));
		if(b == std::string::npos)
			break;
		a = b + sep.size();
	}
	return out;
}

}  // namespace

std::vector<const ComboPiece*> PieceGuide::playable(const PieceResources& r) const {
	std::vector<const ComboPiece*> out;
	for(const auto& lp : lib_)
		if(can_play(lp.piece, r.hand, r.other, r.normal_summon_used))
			out.push_back(&lp.piece);
	return out;
}

std::vector<std::string> PieceGuide::sample(const PieceResources& r, std::mt19937_64& rng, int max_pieces) const {
	std::vector<std::string> labels;
	std::vector<uint32_t> hand = r.hand, other = r.other;
	bool ns_used = r.normal_summon_used;
	std::vector<char> used(lib_.size(), 0);
	for(int n = 0; n < max_pieces; ++n) {
		std::vector<double> w(lib_.size(), 0);
		double total = 0;
		for(size_t i = 0; i < lib_.size(); ++i) {
			const auto& lp = lib_[i];
			if(used[i] || !can_play(lp.piece, hand, other, ns_used))
				continue;
			// Common pieces from strong lines first; a piece that makes nothing is a last resort.
			double x = (1.0 + lp.count) * (1.0 + std::max(0.0, lp.value) / 5.0);
			if(lp.piece.gives.empty() && lp.piece.leaves.empty())
				x *= 0.2;
			// Use what earlier pieces gave before starting something unrelated.
			if(n > 0 && !lp.piece.needs_earlier.empty())
				x *= 3;
			w[i] = x;
			total += x;
		}
		if(total <= 0)
			break;
		std::uniform_real_distribution<double> u(0, total);
		double pick = u(rng);
		size_t i = 0;
		for(; i + 1 < w.size(); ++i) {
			pick -= w[i];
			if(pick <= 0 && w[i] > 0)
				break;
		}
		if(w[i] <= 0)
			break;
		used[i] = 1;
		const ComboPiece& p = lib_[i].piece;
		for(auto code : p.needs_hand) {
			auto it = std::find(hand.begin(), hand.end(), code);
			if(it != hand.end())
				hand.erase(it);
		}
		ns_used |= p.normal_summon;
		for(const auto* list : {&p.gives, &p.leaves})
			for(const auto& c : *list)
				(c.where == "hand" ? hand : other).push_back(c.code);
		for(const auto& step : p.steps)
			for(auto& l : split_labels(step))
				labels.push_back(std::move(l));
	}
	return labels;
}

}  // namespace ygoe
