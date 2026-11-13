#include <locale>
#include "deck.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "ocgapi_constants.h"

namespace ygoe {

namespace {

std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if(a == std::string::npos)
		return {};
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
	for(auto& ch : s)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return s;
}

std::string dir_of(const std::string& path) {
	auto pos = path.find_last_of('/');
	return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::vector<std::string> split(const std::string& s, char sep) {
	std::vector<std::string> out;
	std::stringstream ss(s);
	ss.imbue(std::locale::classic());  // C number format, whatever locale the host app set
	std::string item;
	while(std::getline(ss, item, sep)) {
		item = trim(item);
		if(!item.empty())
			out.push_back(item);
	}
	return out;
}

uint32_t parse_loc(const std::string& s) {
	std::string l = lower(s);
	if(l == "mzone" || l == "monster")
		return LOCATION_MZONE;
	if(l == "szone" || l == "spelltrap" || l == "st")
		return LOCATION_SZONE;
	if(l == "grave" || l == "gy")
		return LOCATION_GRAVE;
	if(l == "banished" || l == "removed")
		return LOCATION_REMOVED;
	return 0;
}

uint32_t parse_pos(const std::string& s) {
	std::string l = lower(s);
	if(l == "atk" || l == "attack")
		return POS_FACEUP_ATTACK;
	if(l == "def" || l == "defense")
		return POS_FACEUP_DEFENSE;
	if(l == "set" || l == "facedown")
		return POS_FACEDOWN_DEFENSE;
	if(l == "up" || l == "faceup")
		return POS_FACEUP;
	return 0;
}

}  // namespace

bool load_ydk(const std::string& path, Deck& out, std::string* error) {
	std::ifstream f(path);
	if(!f) {
		if(error)
			*error = "cannot open " + path;
		return false;
	}
	out = {};
	auto slash = path.find_last_of('/');
	out.name = path.substr(slash == std::string::npos ? 0 : slash + 1);
	std::vector<uint32_t>* section = &out.main;
	std::string line;
	while(std::getline(f, line)) {
		line = trim(line);
		if(line.empty())
			continue;
		if(line == "#main") {
			section = &out.main;
			continue;
		}
		if(line == "#extra") {
			section = &out.extra;
			continue;
		}
		if(line == "!side") {
			section = &out.side;
			continue;
		}
		if(line[0] == '#' || line[0] == '!')
			continue;
		section->push_back(static_cast<uint32_t>(std::stoull(line)));
	}
	return true;
}

uint32_t resolve_card(const CardDB& db, const std::string& raw, const std::vector<uint32_t>& prefer) {
	std::string text = trim(raw);
	if(text.empty())
		return 0;
	if(std::all_of(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
		return static_cast<uint32_t>(std::stoull(text));
	std::string want = lower(text);
	auto match = [&](uint32_t code, bool exact) {
		auto c = db.get(db.script_id(code));
		if(!c)
			return false;
		std::string n = lower(c->name);
		return exact ? n == want : n.rfind(want, 0) == 0;
	};
	for(bool exact : {true, false}) {
		for(auto code : prefer)
			if(match(code, exact))
				return code;
	}
	for(bool exact : {true, false}) {
		uint32_t found = 0;
		for(auto& [code, c] : db.all()) {
			if(c.alias)
				continue;
			std::string n = lower(c.name);
			if(exact ? n == want : n.rfind(want, 0) == 0) {
				if(found && !exact)
					return 0;  // ambiguous prefix
				found = code;
				if(exact)
					break;
			}
		}
		if(found)
			return found;
	}
	return 0;
}

std::vector<uint32_t> without(std::vector<uint32_t> pool, const std::vector<uint32_t>& cards) {
	for(auto c : cards) {
		auto it = std::find(pool.begin(), pool.end(), c);
		if(it != pool.end())
			pool.erase(it);
	}
	return pool;
}

std::vector<uint32_t> sample_hand(const std::vector<uint32_t>& main, size_t n, std::mt19937_64& rng,
                                  const std::vector<uint32_t>& known) {
	auto pool = without(main, known);
	std::shuffle(pool.begin(), pool.end(), rng);
	if(pool.size() > n)
		pool.resize(n);
	return pool;
}

bool load_scenario(const CardDB& db, const std::string& path, Scenario& sc, std::string* error) {
	std::ifstream f(path);
	if(!f) {
		if(error)
			*error = "cannot open " + path;
		return false;
	}
	sc = {};
	std::string base = dir_of(path);
	std::string line;
	int lineno = 0;
	auto fail = [&](const std::string& why) {
		if(error)
			*error = path + ":" + std::to_string(lineno) + ": " + why;
		return false;
	};
	// Deck lines must come first so names resolve against the right lists.
	while(std::getline(f, line)) {
		++lineno;
		line = trim(line);
		if(line.empty() || line[0] == '#')
			continue;
		auto colon = line.find(':');
		if(colon == std::string::npos)
			return fail("expected 'key: value'");
		std::string key = lower(trim(line.substr(0, colon)));
		std::string value = trim(line.substr(colon + 1));
		int p = -1;
		if(!key.empty() && (key.back() == '0' || key.back() == '1')) {
			p = key.back() - '0';
			key.pop_back();
		}
		auto deck_cards = [&](int who) {
			std::vector<uint32_t> all = sc.decks[who].main;
			all.insert(all.end(), sc.decks[who].extra.begin(), sc.decks[who].extra.end());
			all.insert(all.end(), sc.decks[who].side.begin(), sc.decks[who].side.end());
			return all;
		};
		auto cards = [&](int who, std::vector<uint32_t>& out) {
			for(auto& item : split(value, ';')) {
				uint32_t code = resolve_card(db, item, deck_cards(who));
				if(!code)
					return fail("unknown or ambiguous card '" + item + "'");
				out.push_back(code);
			}
			return true;
		};
		if(key == "deck" && p >= 0) {
			std::string dp = value[0] == '/' ? value : base + "/" + value;
			if(!load_ydk(dp, sc.decks[p], error))
				return false;
		} else if(key == "me") {
			sc.me = static_cast<uint8_t>(std::stoi(value));
		} else if(key == "mode") {
			sc.knowledge = lower(value) == "real" ? Knowledge::Real : Knowledge::Practice;
		} else if(key == "seed") {
			sc.seed = std::stoull(value);
		} else if(key == "lp" && p >= 0) {
			sc.lp[p] = static_cast<uint32_t>(std::stoull(value));
		} else if(key == "hand" && p >= 0) {
			std::string l = lower(value);
			if(l.rfind("proxy", 0) == 0 || l.rfind("hidden", 0) == 0) {
				sc.hand_known[p] = false;
				auto parts = split(value, ' ');
				if(parts.size() > 1)
					sc.hand_size[p] = std::stoull(parts[1]);
			} else if(!cards(p, sc.hand[p])) {
				return false;
			}
		} else if((key == "grave" || key == "banished") && p >= 0) {
			std::vector<uint32_t> list;
			if(!cards(p, list))
				return false;
			for(auto c : list)
				sc.placed[p].push_back({c, key == "grave" ? (uint32_t)LOCATION_GRAVE : (uint32_t)LOCATION_REMOVED, 0, POS_FACEUP});
		} else if(key == "field" && p >= 0) {
			// field0: mzone 2 atk; Card Name
			auto parts = split(value, ';');
			if(parts.size() != 2)
				return fail("field needs 'zone seq [pos]; card'");
			auto where = split(parts[0], ' ');
			if(where.size() < 2)
				return fail("field needs 'zone seq [pos]; card'");
			PlacedCard pc;
			pc.loc = parse_loc(where[0]);
			pc.seq = static_cast<uint32_t>(std::stoull(where[1]));
			pc.pos = where.size() > 2 ? parse_pos(where[2]) : 0;
			pc.code = resolve_card(db, parts[1], deck_cards(p));
			if(!pc.loc || !pc.code)
				return fail("bad field entry");
			sc.placed[p].push_back(pc);
		} else {
			return fail("unknown key '" + key + "'");
		}
	}
	for(int p = 0; p < 2; ++p)
		if(sc.decks[p].main.empty())
			return fail("deck" + std::to_string(p) + " missing");
	return true;
}

DuelSetup make_setup(const Scenario& sc, uint64_t det_seed) {
	DuelSetup s;
	std::mt19937_64 rng(sc.seed * 0x9E3779B97F4A7C15ull + det_seed);
	s.seed = {rng(), rng(), rng(), rng()};
	for(int p = 0; p < 2; ++p) {
		auto& ps = s.players[p];
		ps.lp = sc.lp[p];
		ps.extra = sc.decks[p].extra;
		std::vector<uint32_t> used = sc.hand[p];
		std::vector<uint32_t> placed_main;
		for(auto& pc : sc.placed[p]) {
			ps.placed.push_back(pc);
			placed_main.push_back(pc.code);
		}
		// Placed extra-deck monsters leave the Extra Deck.
		ps.extra = without(ps.extra, placed_main);
		bool hidden = !sc.hand_known[p] || (sc.knowledge == Knowledge::Real && p != sc.me);
		if(hidden) {
			auto base = without(sc.decks[p].main, placed_main);
			ps.hand = sample_hand(base, sc.hand_known[p] ? sc.hand[p].size() : sc.hand_size[p], rng);
		} else {
			ps.hand = sc.hand[p];
		}
		ps.main = without(without(sc.decks[p].main, ps.hand), placed_main);
	}
	return s;
}

std::vector<uint32_t> declare_pool(const Scenario& sc) {
	std::vector<uint32_t> pool;
	for(int p : {1 - sc.me, (int)sc.me})
		for(auto* list : {&sc.decks[p].main, &sc.decks[p].extra, &sc.decks[p].side})
			pool.insert(pool.end(), list->begin(), list->end());
	return pool;
}

}  // namespace ygoe
