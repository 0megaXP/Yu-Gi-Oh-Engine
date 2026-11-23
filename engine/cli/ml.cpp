// Machine-learning commands: self-play data and comparing value networks.
//
//   ygoe-cli selfplay <a.ydk> [<b.ydk> ...] --games N --out positions.jsonl [--seed S]
//            [--iterations 300] [--turns 14] [--net value_net.json|none]
//     Bots play each other (random deck pairs from the list, each going first in turn). At each
//     of a player's Main Phase decisions the position is written twice, once from each player's
//     side: the value network's features, the hand-written raw value, and later who won (z: 1, 0,
//     0.5 for a draw; games cut at --turns end by LP). tools/ml/train.py learns from these.
//
//   ygoe-cli arena <a.ydk> [<b.ydk> ...] --games N --net new.json [--vs old.json|none]
//            [--iterations 300] [--turns 14] [--seed S] [--out result.json]
//     The same bots, one with each network, both decks and both seats in turn. Prints the share
//     of points the new network scored (a win 1, a draw or unfinished game 0.5).
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "carddb.h"
#include "deck.h"
#include "duel.h"
#include "evaluate.h"
#include "live.h"
#include "json.h"
#include "valuenet.h"

namespace ygoe {

namespace {

std::shared_ptr<const ValueNet> net_from(const std::string& path, const CardDB& db, std::string& err) {
	if(path.empty())
		return db.value_net();
	if(path == "none")
		return nullptr;
	auto n = std::make_shared<ValueNet>();
	if(!n->load_file(path, &err))
		return nullptr;
	return n;
}

struct Game {
	int winner = -1;     // 0 / 1, -1 draw or cut
	bool finished = false;
	int lp[2] = {8000, 8000};
	int turns = 0;
};

// Plays one game; `on_position` is called at each Main Phase decision of either player.
template <class F>
Game play(const CardDB& db, ScriptCache& scripts, const Deck& d0, const Deck& d1, uint64_t seed, int iterations, int max_turns,
          const std::shared_ptr<const ValueNet>& n0, const std::shared_ptr<const ValueNet>& n1, F&& on_position) {
	Game g;
	LiveDuel live(db, scripts, d0, d1, seed);
	if(!live.ok())
		return g;
	live.set_iterations(iterations);
	live.set_value_net(0, n0);
	live.set_value_net(1, n1);
	int guard = 0;
	while(!live.duel().ended() && static_cast<int>(live.duel().turn()) <= max_turns && guard++ < 4000) {
		const Decision& d = live.duel().decision();
		if(d.type == DecisionType::Idle)
			on_position(live);
		if(live.bot_move(BotLevel::Normal).empty())
			break;
	}
	const Board b = live.duel().board();
	g.lp[0] = b.lp[0];
	g.lp[1] = b.lp[1];
	g.turns = static_cast<int>(live.duel().turn());
	if(live.duel().ended()) {
		g.finished = true;
		g.winner = live.duel().winner();
	} else if(b.lp[0] != b.lp[1]) {
		g.winner = b.lp[0] > b.lp[1] ? 0 : 1;  // cut at the turn limit: ahead on LP
	}
	return g;
}

}  // namespace

int run_selfplay(const std::vector<std::string>& decks_paths, const CardDB& db, ScriptCache& scripts,
                 const std::map<std::string, std::string>& opt) {
	auto get = [&](const std::string& k, const std::string& def) {
		auto it = opt.find(k);
		return it == opt.end() ? def : it->second;
	};
	std::string err;
	std::vector<Deck> decks;
	for(const auto& p : decks_paths) {
		Deck d;
		if(!load_ydk(p, d, &err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		decks.push_back(d);
	}
	const int games = std::stoi(get("games", "4"));
	const int iterations = std::stoi(get("iterations", "300"));
	const int max_turns = std::stoi(get("turns", "14"));
	const uint64_t seed = std::stoull(get("seed", "1"));
	auto net = net_from(get("net", ""), db, err);
	if(!err.empty()) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	std::ofstream out(get("out", "positions.jsonl"), std::ios::app);
	std::mt19937_64 rng(seed);
	int written = 0;
	auto t0 = std::chrono::steady_clock::now();
	for(int gi = 0; gi < games; ++gi) {
		const size_t a = rng() % decks.size(), bdx = rng() % decks.size();
		const bool swap = gi % 2 == 1;
		const Deck& d0 = swap ? decks[bdx] : decks[a];
		const Deck& d1 = swap ? decks[a] : decks[bdx];
		// Base values come from an evaluator without the network (the network learns a correction).
		Evaluator base(db, d0, d1);
		base.set_value_net(nullptr);
		struct Pos {
			std::vector<float> f;
			double raw;
			uint8_t me;
			uint32_t turn;
		};
		std::vector<Pos> pos;
		uint32_t last_turn = 0;
		int this_turn = 0;
		auto record = [&](LiveDuel& live) {
			const Board b = live.duel().board();
			if(b.turn != last_turn) {
				last_turn = b.turn;
				this_turn = 0;
			}
			if(++this_turn > 6)
				return;  // a few per turn is enough (they are much alike)
			const uint8_t next = static_cast<uint8_t>(1 - b.turn_player);
			for(uint8_t me = 0; me < 2; ++me) {
				auto ev = base.evaluate(b, me, next);
				pos.push_back({base.features(b, me, next, ev.value), ev.raw, me, b.turn});
			}
		};
		Game g = play(db, scripts, d0, d1, seed * 7919 + static_cast<uint64_t>(gi), iterations, max_turns, net, net, record);
		for(const auto& p : pos) {
			const double z = g.winner < 0 ? 0.5 : (g.winner == p.me ? 1.0 : 0.0);
			out << "{\"z\":" << z << ",\"fin\":" << (g.finished ? 1 : 0) << ",\"raw\":" << p.raw << ",\"turn\":" << p.turn
			    << ",\"game\":" << json::quote(std::to_string(seed) + "-" + std::to_string(gi)) << ",\"f\":[";
			for(size_t i = 0; i < p.f.size(); ++i)
				out << (i ? "," : "") << p.f[i];
			out << "]}\n";
			++written;
		}
		out.flush();
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("game %d/%d: %s, %d turns, winner %d (LP %d/%d), %zu positions, %.0fs\n", gi + 1, games,
		            g.finished ? "finished" : "cut", g.turns, g.winner, g.lp[0], g.lp[1], pos.size(), secs);
		std::fflush(stdout);
	}
	std::printf("%d positions written\n", written);
	return 0;
}

int run_arena(const std::vector<std::string>& decks_paths, const CardDB& db, ScriptCache& scripts,
              const std::map<std::string, std::string>& opt) {
	auto get = [&](const std::string& k, const std::string& def) {
		auto it = opt.find(k);
		return it == opt.end() ? def : it->second;
	};
	std::string err;
	std::vector<Deck> decks;
	for(const auto& p : decks_paths) {
		Deck d;
		if(!load_ydk(p, d, &err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		decks.push_back(d);
	}
	auto fresh = net_from(get("net", "none"), db, err);
	if(!err.empty()) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	auto old = net_from(get("vs", "none"), db, err);
	if(!err.empty()) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	const int games = std::stoi(get("games", "8"));
	const int iterations = std::stoi(get("iterations", "300"));
	const int max_turns = std::stoi(get("turns", "14"));
	const uint64_t seed = std::stoull(get("seed", "1"));
	double points = 0;
	int played = 0;
	for(int gi = 0; gi < games; ++gi) {
		// deck pairs in turn; the new network sits first, then second, with the same deck pair
		const size_t pair = static_cast<size_t>(gi / 2);
		const Deck& da = decks[pair % decks.size()];
		const Deck& db2 = decks[(pair / decks.size() + 1 + pair) % decks.size()];
		const bool new_first = gi % 2 == 0;
		Game g = play(db, scripts, da, db2, seed * 104729 + static_cast<uint64_t>(pair), iterations, max_turns,
		              new_first ? fresh : old, new_first ? old : fresh, [](LiveDuel&) {});
		const int new_seat = new_first ? 0 : 1;
		const double r = g.winner < 0 ? 0.5 : (g.winner == new_seat ? 1.0 : 0.0);
		points += r;
		++played;
		std::printf("game %d/%d: new network %s, %s %d turns -> %.1f (total %.1f/%d)\n", gi + 1, games, new_first ? "first" : "second",
		            g.finished ? "finished" : "cut", g.turns, r, points, played);
		std::fflush(stdout);
	}
	const double share = played ? points / played : 0.5;
	std::printf("new network score: %.3f over %d games\n", share, played);
	if(opt.count("out")) {
		std::ofstream o(get("out", ""));
		o << "{\"score\":" << share << ",\"games\":" << played << "}\n";
	}
	return 0;
}

// ygoe-cli netcheck net.json positions.jsonl: the network's correction for each position (to
// compare with tools/ml, which must compute the same).
int run_netcheck(const std::string& net_path, const std::string& positions) {
	ValueNet net;
	std::string err;
	if(!net.load_file(net_path, &err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	std::ifstream in(positions);
	std::string line;
	while(std::getline(in, line)) {
		auto v = json::Value::parse(line);
		std::vector<float> f;
		for(size_t i = 0; i < v["f"].size(); ++i)
			f.push_back(static_cast<float>(v["f"][i].num(0)));
		std::printf("%.6f\n", net.correction(f));
	}
	return 0;
}

}  // namespace ygoe
