// ygoe-cli bench <suite.json> [--out result.json] [--baseline old.json] [--only lines,interrupts,hands,duels]
//
// A fixed set of test positions, run after a change to see whether the engine got better:
//
//   lines       a player's going-first turn from a replay (hand, steps, end board). Checks that the
//               line checker can still follow it, then lets the engine search the same hand with no
//               interaction and compares its end board with the player's (same board value scale).
//   interrupts  a hand plus the hand trap the opponent used: how much board the engine still makes.
//   hands       random opening hands of a deck (seeded): the best board the search finds.
//   duels       bot against bot, both decks going first in turn: win rate.
//
// With --baseline, every number is printed next to the old run's, with the difference.
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "api.h"
#include "deck.h"
#include "json.h"
#include "live.h"

namespace ygoe {

namespace {

std::string read_file(const std::string& path) {
	std::ifstream f(path);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

std::string num(double v) {
	char b[32];
	std::snprintf(b, sizeof b, "%.4f", v);
	return b;
}

// Re-emits a parsed value (the suite's steps and hands go into requests as they are).
std::string emit(const json::Value& v) {
	switch(v.type()) {
	case json::Value::Null: return "null";
	case json::Value::Bool: return v.boolean() ? "true" : "false";
	case json::Value::Number: {
		double d = v.num();
		if(d == std::floor(d) && std::abs(d) < 1e15)
			return std::to_string(static_cast<long long>(d));
		return num(d);
	}
	case json::Value::String: return json::quote(v.str());
	case json::Value::Array: {
		std::string o = "[";
		for(size_t i = 0; i < v.size(); ++i)
			o += (i ? "," : "") + emit(v[i]);
		return o + "]";
	}
	case json::Value::Object: {
		std::string o = "{";
		bool first = true;
		for(const auto& [k, x] : v.entries()) {
			o += (first ? "" : ",") + json::quote(k) + ":" + emit(x);
			first = false;
		}
		return o + "}";
	}
	}
	return "null";
}

// A .ydk as the API's deck object (passcodes as written).
std::string ydk_json(const std::string& path, const std::string& name, std::string& err) {
	std::ifstream f(path);
	if(!f) {
		err = "can't read " + path;
		return "";
	}
	std::vector<std::string> sec[3];
	int at = 0;
	std::string l;
	while(std::getline(f, l)) {
		while(!l.empty() && (l.back() == '\r' || l.back() == ' '))
			l.pop_back();
		if(l == "#main")
			at = 0;
		else if(l == "#extra")
			at = 1;
		else if(l == "!side")
			at = 2;
		else if(!l.empty() && std::isdigit(static_cast<unsigned char>(l[0])))
			sec[at].push_back(l);
	}
	auto arr = [](const std::vector<std::string>& v) {
		std::string o = "[";
		for(size_t i = 0; i < v.size(); ++i)
			o += (i ? "," : "") + v[i];
		return o + "]";
	};
	return "{\"name\":" + json::quote(name) + ",\"main\":" + arr(sec[0]) + ",\"extra\":" + arr(sec[1]) + ",\"side\":" + arr(sec[2]) + "}";
}

struct Budget {
	int iterations = 1500;
	double seconds = 90;
	double line_seconds = 45;
	int threads = 1;
};

// The best board the engine's search finds from `hand` going first (`opp_hand`: what the
// opponent holds; empty: no interaction). Returns board value, -1 on failure.
double search_board(Engine& engine, const std::string& deck, const std::string& opp_deck, const json::Value& hand,
                    const std::string& opp_hand, uint64_t seed, const Budget& b, int& moves, std::string& err) {
	std::ostringstream r;
	r << "{\"decks\":[" << deck << "," << opp_deck << "],\"me\":0,\"mode\":\"practice\",\"seed\":" << seed
	  << ",\"hands\":[" << (hand.is_null() ? "null" : emit(hand)) << "," << opp_hand << "]"
	  << ",\"hand_sizes\":[5," << json::Value::parse(opp_hand).size() << "]"
	  << ",\"config\":{\"iterations\":" << b.iterations << ",\"seconds\":" << b.seconds << ",\"threads\":" << b.threads
	  << ",\"samples\":1,\"lines\":3,\"verify\":false,\"horizon\":1}}";
	auto out = json::Value::parse(engine.analyze(r.str()));
	if(out.has("error") || out["lines"].size() == 0) {
		err = out.has("error") ? out["error"].str() : "no line";
		return -1;
	}
	const auto& l = out["lines"][0];
	moves = static_cast<int>(l["moves"].size());
	return l["board_value"].num(-1);
}

struct Row {
	std::string kind, id;
	std::map<std::string, double> m;  // metric -> value
	std::string note;
};

}  // namespace

int run_bench(const std::string& suite_path, const std::string& data, const std::string& scripts_dir,
              const std::map<std::string, std::string>& opt) {
	auto get = [&](const std::string& k, const std::string& def) {
		auto it = opt.find(k);
		return it == opt.end() ? def : it->second;
	};
	std::string err;
	auto suite = json::Value::parse(read_file(suite_path), &err);
	if(!suite.is_object()) {
		std::fprintf(stderr, "can't read %s: %s\n", suite_path.c_str(), err.c_str());
		return 1;
	}
	std::string base_dir = suite_path.substr(0, suite_path.find_last_of('/') + 1);
	auto path_of = [&](const std::string& p) { return p.empty() || p[0] == '/' ? p : base_dir + p; };
	Budget b;
	b.iterations = static_cast<int>(suite["budget"]["iterations"].integer(b.iterations));
	b.seconds = suite["budget"]["seconds"].num(b.seconds);
	b.line_seconds = suite["budget"]["line_seconds"].num(b.line_seconds);
	if(opt.count("iterations"))
		b.iterations = std::stoi(get("iterations", ""));
	const std::string only = get("only", "lines,interrupts,hands,duels");
	auto want = [&](const std::string& k) { return only.find(k) != std::string::npos; };

	Engine engine(data, scripts_dir);
	std::map<std::string, std::string> decks;  // path -> deck json
	auto deck = [&](const std::string& p) -> const std::string& {
		auto it = decks.find(p);
		if(it == decks.end()) {
			std::string e;
			it = decks.emplace(p, ydk_json(path_of(p), p, e)).first;
			if(!e.empty())
				std::fprintf(stderr, "%s\n", e.c_str());
		}
		return it->second;
	};
	const std::string default_opp = suite["opponent"].str();
	std::vector<Row> rows;
	std::map<std::string, double> uninterrupted;  // line id -> engine value
	auto t_start = std::chrono::steady_clock::now();

	if(want("lines"))
		for(const auto& c : suite["lines"].items()) {
			Row row{"line", c["id"].str(), {}, c["note"].str()};
			const std::string& d = deck(c["deck"].str());
			std::ostringstream r;
			r << "{\"deck\":" << d << ",\"seconds\":" << b.line_seconds << ",\"seed\":1,\"learn\":false,\"lines\":[{\"id\":"
			  << json::quote(row.id) << ",\"hand\":" << emit(c["hand"]) << ",\"steps\":" << emit(c["steps"])
			  << ",\"end_board\":" << emit(c["end_board"]) << "}]}";
			auto out = json::Value::parse(engine.video_lines(r.str()));
			const auto& l = out["lines"][0];
			const double checked = l["steps_checked"].num(0);
			row.m["followed"] = checked > 0 ? l["steps_matched"].num(0) / checked : 0;
			row.m["verified"] = l["verified"].boolean() ? 1 : 0;
			row.m["player_value"] = l["value"].num(0);
			int moves = 0;
			std::string e;
			const std::string opp = deck(c.has("opponent") ? c["opponent"].str() : default_opp.empty() ? c["deck"].str() : default_opp);
			double v = search_board(engine, d, opp, c["hand"], "[]", 1, b, moves, e);
			row.m["engine_value"] = v;
			row.m["engine_moves"] = moves;
			row.m["gap"] = v - row.m["player_value"];
			uninterrupted[row.id] = v;
			std::printf("line %-14s followed %3.0f%%%s  player %.3f  engine %.3f (%d moves)  gap %+.3f\n", row.id.c_str(),
			            row.m["followed"] * 100, row.m["verified"] ? " verified" : "", row.m["player_value"], v, moves,
			            row.m["gap"]);
			std::fflush(stdout);
			rows.push_back(row);
		}

	if(want("interrupts"))
		for(const auto& c : suite["interrupts"].items()) {
			Row row{"interrupt", c["id"].str(), {}, c["note"].str()};
			const std::string& d = deck(c["deck"].str());
			const std::string opp = deck(c.has("opponent") ? c["opponent"].str() : default_opp.empty() ? c["deck"].str() : default_opp);
			int moves = 0;
			std::string e;
			double free_v = -1;
			if(c.has("line") && uninterrupted.count(c["line"].str()))
				free_v = uninterrupted[c["line"].str()];
			else
				free_v = search_board(engine, d, opp, c["hand"], "[]", 1, b, moves, e);
			double v = search_board(engine, d, opp, c["hand"], emit(c["opponent_hand"]), 1, b, moves, e);
			row.m["engine_value"] = v;
			row.m["free_value"] = free_v;
			row.m["kept"] = free_v > 0 ? v / free_v : 0;
			std::printf("interrupt %-14s %-28s engine %.3f of %.3f free (%.0f%%)%s\n", row.id.c_str(), row.note.substr(0, 28).c_str(), v,
			            free_v, row.m["kept"] * 100, e.empty() ? "" : (" " + e).c_str());
			std::fflush(stdout);
			rows.push_back(row);
		}

	if(want("hands"))
		for(const auto& c : suite["hands"].items()) {
			const std::string& d = deck(c["deck"].str());
			const std::string opp = deck(c.has("opponent") ? c["opponent"].str() : c["deck"].str());
			const int n = static_cast<int>(c["count"].integer(4));
			double sum = 0;
			int ok = 0;
			for(int h = 0; h < n; ++h) {
				int moves = 0;
				std::string e;
				uint64_t seed = static_cast<uint64_t>(c["seed"].integer(7)) + static_cast<uint64_t>(h) * 7919;
				double v = search_board(engine, d, opp, json::Value(), "[]", seed, b, moves, e);
				Row row{"hand", c["id"].str() + "#" + std::to_string(h), {{"engine_value", v}, {"engine_moves", double(moves)}}, ""};
				rows.push_back(row);
				if(v >= 0) {
					sum += v;
					++ok;
				}
			}
			std::printf("hands %-14s %d hands, average board %.3f\n", c["id"].str().c_str(), n, ok ? sum / ok : 0.0);
			std::fflush(stdout);
		}

	if(want("duels")) {
		CardDB db;
		db.load(data, &err);
		ScriptCache scripts(scripts_dir);
		for(const auto& c : suite["duels"].items()) {
			Deck d0, d1;
			if(!load_ydk(path_of(c["deck0"].str()), d0, &err) || !load_ydk(path_of(c["deck1"].str()), d1, &err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
				continue;
			}
			const std::string lv = c["bot"].str().empty() ? "normal" : c["bot"].str();
			const BotLevel level = lv == "easy" ? BotLevel::Easy : lv == "strong" ? BotLevel::Strong : BotLevel::Normal;
			const int games = static_cast<int>(c["games"].integer(2));
			const uint32_t turns = static_cast<uint32_t>(c["turns"].integer(6));
			double score = 0;  // for deck0: win 1, unfinished 0.5
			for(int g = 0; g < games; ++g) {
				const bool swap = g % 2 == 1;  // deck1 goes first in odd games
				LiveDuel live(db, scripts, swap ? d1 : d0, swap ? d0 : d1, 1000 + static_cast<uint64_t>(g));
				if(!live.ok())
					break;
				int guard = 0;
				while(!live.duel().ended() && live.duel().turn() <= turns && guard++ < 3000)
					if(live.bot_move(level).empty())
						break;
				double r = 0.5;
				if(live.duel().ended()) {
					const int w = live.duel().winner();
					r = w == (swap ? 1 : 0) ? 1 : w == (swap ? 0 : 1) ? 0 : 0.5;
				}
				score += r;
			}
			Row row{"duel", c["id"].str(), {{"deck0_score", games ? score / games : 0}, {"games", double(games)}}, ""};
			std::printf("duels %-14s %s vs %s: %.0f%% for the first deck over %d games\n", row.id.c_str(), c["deck0"].str().c_str(),
			            c["deck1"].str().c_str(), row.m["deck0_score"] * 100, games);
			std::fflush(stdout);
			rows.push_back(row);
		}
	}

	// Totals
	std::map<std::string, std::pair<double, int>> tot;
	auto add = [&](const std::string& k, double v) {
		tot[k].first += v;
		tot[k].second += 1;
	};
	for(const auto& r : rows) {
		if(r.kind == "line") {
			add("lines.followed", r.m.at("followed"));
			add("lines.verified", r.m.at("verified"));
			add("lines.engine_value", r.m.at("engine_value"));
			add("lines.gap", r.m.at("gap"));
			add("lines.reached", r.m.at("engine_value") >= r.m.at("player_value") - 0.02 ? 1 : 0);
		} else if(r.kind == "interrupt") {
			add("interrupts.engine_value", r.m.at("engine_value"));
			add("interrupts.kept", r.m.at("kept"));
		} else if(r.kind == "hand") {
			add("hands.engine_value", r.m.at("engine_value"));
		} else if(r.kind == "duel") {
			add("duels." + r.id, r.m.at("deck0_score"));
		}
	}
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

	// Baseline
	std::map<std::string, double> base;
	if(opt.count("baseline")) {
		auto bv = json::Value::parse(read_file(get("baseline", "")));
		for(const auto& [k, v] : bv["totals"].entries())
			base[k] = v.num();
	}
	std::printf("\nTotals (%.0fs)%s\n", secs, base.empty() ? "" : "            now    before   change");
	std::ostringstream totals;
	bool first = true;
	for(const auto& [k, sv] : tot) {
		const double avg = sv.first / sv.second;
		if(base.count(k))
			std::printf("  %-24s %8.3f %8.3f %+8.3f\n", k.c_str(), avg, base[k], avg - base[k]);
		else
			std::printf("  %-24s %8.3f\n", k.c_str(), avg);
		totals << (first ? "" : ",") << json::quote(k) << ":" << num(avg);
		first = false;
	}

	if(opt.count("out")) {
		std::ofstream o(get("out", ""));
		o << "{\"suite\":" << json::quote(suite_path) << ",\"iterations\":" << b.iterations << ",\"seconds\":" << num(secs)
		  << ",\"totals\":{" << totals.str() << "},\"cases\":[";
		for(size_t i = 0; i < rows.size(); ++i) {
			const auto& r = rows[i];
			o << (i ? "," : "") << "{\"kind\":" << json::quote(r.kind) << ",\"id\":" << json::quote(r.id);
			for(const auto& [k, v] : r.m)
				o << "," << json::quote(k) << ":" << num(v);
			o << "}";
		}
		o << "]}\n";
	}
	return 0;
}

}  // namespace ygoe
