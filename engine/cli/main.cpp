// Command-line front end for the engine.
//
//   ygoe-cli analyze <state.txt> [options]     best plays + end boards for a position
//   ygoe-cli profile <deck.ydk>                what a deck relies on, its interruptions
//   ygoe-cli side <mine.ydk> <theirs.ydk>      side-deck plan, going first and second
//   ygoe-cli proxy <deck.ydk> [count]          sample opening hands
//
// Common options: --data DIR (default ./data)  --scripts DIR (default ./third_party/CardScripts)
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <map>
#include <random>
#include <sstream>
#include <string>

#include "analyze.h"
#include "json.h"
#include "api.h"
#include "follow.h"
#include "reach.h"
#include "live.h"
#include "deck.h"
#include "ocgapi_constants.h"
#include "profile.h"
#include "side.h"
#include "pieces.h"
#include "reference.h"
#include "policy.h"
#include "search.h"

using namespace ygoe;

namespace ygoe {
int run_bench(const std::string& suite, const std::string& data, const std::string& scripts,
              const std::map<std::string, std::string>& opt);
int run_selfplay(const std::vector<std::string>& decks, const CardDB& db, ScriptCache& scripts,
                 const std::map<std::string, std::string>& opt);
int run_arena(const std::vector<std::string>& decks, const CardDB& db, ScriptCache& scripts,
              const std::map<std::string, std::string>& opt);
int run_netcheck(const std::string& net, const std::string& positions);
int run_library(const std::string& deck, const CardDB& db, ScriptCache& scripts, const std::map<std::string, std::string>& opt);
int run_policy_train(const std::vector<std::string>& files, const CardDB& db, const std::map<std::string, std::string>& opt);
int run_nonengine(const std::vector<std::string>& decks, const CardDB& db);
}

namespace {

struct Args {
	std::vector<std::string> pos;
	std::map<std::string, std::string> opt;
	std::string get(const std::string& k, const std::string& def = "") const {
		auto it = opt.find(k);
		return it == opt.end() ? def : it->second;
	}
	bool has(const std::string& k) const { return opt.count(k) > 0; }
};

Args parse(int argc, char** argv) {
	Args a;
	for(int i = 1; i < argc; ++i) {
		std::string s = argv[i];
		if(s.rfind("--", 0) == 0) {
			std::string key = s.substr(2);
			if(i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0 &&
			   key != "no-verify" && key != "json" && key != "quiet")
				a.opt[key] = argv[++i];
			else
				a.opt[key] = "1";
		} else {
			a.pos.push_back(s);
		}
	}
	return a;
}

int usage() {
	std::fprintf(stderr,
	             "usage:\n"
	             "  ygoe-cli analyze <state.txt> [--iterations N] [--seconds S] [--threads T] [--samples D]\n"
	             "                   [--lines K] [--verify-iterations N] [--no-verify] [--json] [--out FILE]\n"
	             "  ygoe-cli profile <deck.ydk>\n"
	             "  ygoe-cli side <mine.ydk> <theirs.ydk> [--simulate N] [--iterations N]\n"
	             "  ygoe-cli proxy <deck.ydk> [count]\n"
	             "  ygoe-cli json <analyze|profile|side> <request.json>   (the app API, see docs/api.md)\n"
	             "  ygoe-cli bench <suite.json> [--out result.json] [--baseline old.json] [--only lines,interrupts,hands,duels]\n"
	             "  ygoe-cli selfplay <a.ydk> [b.ydk ...] --games N --out positions.jsonl [--seed S --iterations 300 --turns 14 --net f|none]\n"
	             "  ygoe-cli arena <a.ydk> [b.ydk ...] --games N --net new.json [--vs old.json|none] [--policy p.json --vs-policy q.json] [--out result.json]\n"
	             "  ygoe-cli nonengine <deck.ydk> [opponent.ydk ...]   (non-engine cards: role, measured value)\n"
	             "  ygoe-cli library <deck.ydk> --out lib.json [--hands 40 --iterations 1500 --pieces app_pieces.json]\n"
	             "  ygoe-cli policy-train <examples.jsonl> [...] --out policy.json [--init policy.json --epochs 4 --report r.json]\n"
	             "  ygoe-cli duel <deck0.ydk> <deck1.ydk> [--bot0 easy|normal|strong] [--bot1 ...] [--turns N] [--seed S]\n"
	             "  ygoe-cli followall <inputs.jsonl> --out positions.jsonl --summary follows.jsonl [--shard i/n]\n"
	             "                   (follows the app's uploaded games: one follow request per line)\n"
	             "options: --data DIR  --scripts DIR\n");
	return 2;
}

std::string pct(double v) { return std::to_string(static_cast<int>(v * 100 + 0.5)) + "%"; }

int cmd_profile(const CardDB& db, const Args& a) {
	if(a.pos.size() < 2)
		return usage();
	Deck d;
	std::string err;
	if(!load_ydk(a.pos[1], d, &err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	auto p = profile_deck(db, d);
	auto sets = dominant_setcodes(db, d);
	std::printf("%s\n  %s\n  engine cards in Main Deck: %d\n  key engine cards:\n", d.name.c_str(), p.summary().c_str(),
	            p.engine_cards);
	for(auto& [code, w] : p.key_cards)
		std::printf("    %s\n", db.name(db.script_id(code)).c_str());
	std::printf("  interruptions in the 40 (value vs a generic deck):\n");
	DeckProfile generic;
	generic.monster_effects = 0.5;
	generic.hand_effects = 0.15;
	generic.gy_effects = 0.15;
	generic.spell_trap = 0.2;
	generic.searches = 0.4;
	generic.special_summons = 0.5;
	std::map<uint32_t, int> counts;
	for(auto c : d.main)
		++counts[c];
	for(auto& [code, n] : counts) {
		auto r = classify(db, code, sets);
		double best = 0;
		const Interrupt* bi = nullptr;
		for(auto& in : r.interrupts)
			if(in.kind != Interrupt::Extender && interrupt_value(in, generic) > best)
				best = interrupt_value(in, generic), bi = &in;
		if(bi)
			std::printf("    %dx %-45s %s from %s (%.2f)%s\n", n, db.name(db.script_id(code)).c_str(), bi->name(),
			            (bi->range & LOCATION_HAND) ? "hand" : (bi->range & LOCATION_GRAVE) ? "GY" : (bi->range & LOCATION_SZONE) ? "S/T zone" : "field",
			            best, r.engine ? " [engine]" : "");
	}
	return 0;
}

int cmd_proxy(const CardDB& db, const Args& a) {
	if(a.pos.size() < 2)
		return usage();
	Deck d;
	std::string err;
	if(!load_ydk(a.pos[1], d, &err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	int n = a.pos.size() > 2 ? std::stoi(a.pos[2]) : 5;
	std::mt19937_64 rng(std::stoull(a.get("seed", std::to_string(std::random_device{}()))));
	for(int i = 0; i < n; ++i) {
		auto h = sample_hand(d.main, 5, rng);
		std::string line;
		for(auto c : h)
			line += (line.empty() ? "" : "; ") + db.name(db.script_id(c));
		std::printf("hand0: %s\n", line.c_str());
	}
	return 0;
}

SearchConfig search_config(const Args& a) {
	SearchConfig s;
	s.iterations = std::stoi(a.get("iterations", "3000"));
	s.seconds = std::stod(a.get("seconds", "60"));
	s.threads = std::stoi(a.get("threads", "0"));
	s.horizon_turns = std::stoi(a.get("horizon", "0"));
	return s;
}

int cmd_analyze(const CardDB& db, ScriptCache& scripts, const Args& a) {
	if(a.pos.size() < 2)
		return usage();
	Scenario sc;
	std::string err;
	if(!load_scenario(db, a.pos[1], sc, &err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	AnalysisConfig cfg;
	cfg.search = search_config(a);
	cfg.determinizations = std::stoi(a.get("samples", "12"));
	cfg.lines = std::stoi(a.get("lines", "3"));
	cfg.verify = !a.has("no-verify");
	cfg.verify_iterations = std::stoi(a.get("verify-iterations", "400"));
	bool quiet = a.has("quiet") || a.has("json");
	auto res = analyze(db, scripts, sc, cfg, [&](const std::string& s) {
		if(!quiet)
			std::fprintf(stderr, "%s\n", s.c_str());
	});
	std::string text = a.has("json") ? res.to_json() : res.to_text();
	if(a.has("out")) {
		std::ofstream(a.get("out")) << text << "\n";
		std::fprintf(stderr, "wrote %s\n", a.get("out").c_str());
	} else {
		std::printf("%s\n", text.c_str());
	}
	return 0;
}

int cmd_side(const CardDB& db, ScriptCache& scripts, const Args& a) {
	if(a.pos.size() < 3)
		return usage();
	Deck mine, theirs;
	std::string err;
	if(!load_ydk(a.pos[1], mine, &err) || !load_ydk(a.pos[2], theirs, &err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	auto opp = profile_deck(db, theirs);
	BoardShape shape;
	int sim = std::stoi(a.get("simulate", "0"));
	if(sim > 0) {
		std::fprintf(stderr, "simulating %d opening turns for %s...\n", sim, theirs.name.c_str());
		shape = simulate_board_shape(db, scripts, theirs, mine, sim, std::stoi(a.get("iterations", "400")), nullptr,
		                             [](int s, const BoardShape&, int monsters, int st_up, int st_set) {
			                             std::fprintf(stderr, "  sample %d: %d monsters, %d S/T face-up, %d set\n", s,
			                                          monsters, st_up, st_set);
		                             });
	}
	std::printf("%s vs %s\n  they rely on: %s\n", mine.name.c_str(), theirs.name.c_str(), opp.summary().c_str());
	if(shape.samples)
		std::printf("  their simulated first-turn boards: %.1f monsters, %.1f face-up S/T-zone cards, %.1f set cards, %.1f hand traps kept\n",
		            shape.monsters, shape.st_faceup, shape.st_set, shape.hand_traps);
	for(bool first : {true, false}) {
		auto plan = plan_side(db, mine, theirs, opp, shape, first);
		std::printf("\nGoing %s:\n", first ? "first" : "second");
		if(plan.swaps.empty())
			std::printf("  no swaps clearly improve the deck\n");
		std::map<uint32_t, int> outs, ins;
		for(auto& s : plan.swaps) {
			++outs[s.out];
			++ins[s.in];
		}
		auto why = [&](const std::vector<CardValue>& list, uint32_t code) {
			for(auto& v : list)
				if(v.code == code)
					return v.why + " (" + std::to_string(v.value).substr(0, 4) + ")";
			return std::string();
		};
		for(auto& [code, n] : ins)
			std::printf("  + %dx %-42s %s\n", n, db.name(db.script_id(code)).c_str(), why(plan.side_values, code).c_str());
		for(auto& [code, n] : outs)
			std::printf("  - %dx %-42s %s\n", n, db.name(db.script_id(code)).c_str(), why(plan.flex_values, code).c_str());
	}
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	Args a = parse(argc, argv);
	if(a.pos.empty())
		return usage();
	CardDB db;
	std::string err;
	if(!db.load(a.get("data", "data"), &err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	const std::string& cmd = a.pos[0];
	if(cmd == "bench") {
		if(a.pos.size() < 2)
			return usage();
		return run_bench(a.pos[1], a.get("data", "data"), a.get("scripts", "third_party/CardScripts"), a.opt);
	}
	if(cmd == "followall") {
		// Each line: a game the app uploaded (its YGO Omega replay's moves and decks, the follow
		// request of docs/api.md). Writes the training positions and one summary line per game.
		if(a.pos.size() < 2 || a.get("out", "").empty())
			return usage();
		int shard = 1, shards = 1;
		std::sscanf(a.get("shard", "1/1").c_str(), "%d/%d", &shard, &shards);
		Engine engine(a.get("data", "data"), a.get("scripts", "third_party/CardScripts"));
		std::ifstream in(a.pos[1]);
		std::ofstream out(a.get("out", "")), sum(a.get("summary", "/dev/null"));
		std::string line;
		int n = 0, done = 0, complete = 0, positions = 0;
		while(std::getline(in, line)) {
			if(line.empty())
				continue;
			const json::Value req = json::Value::parse(line);
			const std::string game = req["game"].str();
			if(game.empty() || static_cast<int>(std::hash<std::string>{}(game) % static_cast<size_t>(shards)) != shard - 1)
				continue;
			++n;
			const json::Value r = json::Value::parse(engine.follow_replay(line));
			if(!r.is_object() || !r["error"].is_null())
				continue;
			++done;
			out << r["jsonl"].str();
			positions += static_cast<int>(r["positions"].integer(0));
			complete += r["complete"].boolean() ? 1 : 0;
			sum << "{\"kind\":\"follow\",\"game\":" << json::quote(game) << ",\"turns\":" << r["turns"].integer(0)
			    << ",\"turns_followed\":" << r["turns_followed"].integer(0) << ",\"events\":" << r["events"].integer(0)
			    << ",\"followed\":" << r["followed"].integer(0) << ",\"complete\":" << (r["complete"].boolean() ? "true" : "false")
			    << ",\"winner\":" << r["winner"].integer(-1) << ",\"stopped\":" << json::quote(r["stopped"].str())
			    << ",\"stop_turn\":" << r["stop_turn"].integer(0) << ",\"stop_next\":" << json::quote(r["stop_next"].str())
			    << ",\"follow_version\":" << r["follow_version"].integer(0) << "}\n";
		}
		std::printf("shard %d/%d: %d games, %d followed, %d to the end, %d positions\n", shard, shards, n, done, complete, positions);
		return 0;
	}
	if(cmd == "json") {
		if(a.pos.size() < 3)
			return usage();
		std::ifstream f(a.pos[2]);
		std::stringstream ss;
		ss << f.rdbuf();
		Engine engine(a.get("data", "data"), a.get("scripts", "third_party/CardScripts"));
		const std::string& what = a.pos[1];
		std::string out = what == "analyze" ? engine.analyze(ss.str())
		                  : what == "profile" ? engine.profile(ss.str())
		                  : what == "side" ? engine.side(ss.str())
		                  : what == "reference" ? engine.reference(ss.str())
		                  : what == "tree" ? engine.combo_tree(ss.str())
		                  : what == "lines" ? engine.video_lines(ss.str())
		                  : what == "positions" ? engine.replay_positions(ss.str())
		                  : what == "follow" ? engine.follow_replay(ss.str())
		                  : std::string("{\"error\":\"unknown call\"}");
		std::printf("%s\n", out.c_str());
		return out.rfind("{\"error\"", 0) == 0 ? 1 : 0;
	}
	ScriptCache scripts(a.get("scripts", "third_party/CardScripts"));
	if(cmd == "netcheck") {
		if(a.pos.size() < 3)
			return usage();
		return run_netcheck(a.pos[1], a.pos[2]);
	}
	if(cmd == "selfplay" || cmd == "arena") {
		if(a.pos.size() < 2)
			return usage();
		std::vector<std::string> decks(a.pos.begin() + 1, a.pos.end());
		return cmd == "selfplay" ? run_selfplay(decks, db, scripts, a.opt) : run_arena(decks, db, scripts, a.opt);
	}
	if(cmd == "library") {
		if(a.pos.size() < 2)
			return usage();
		return run_library(a.pos[1], db, scripts, a.opt);
	}
	if(cmd == "nonengine") {
		if(a.pos.size() < 2)
			return usage();
		return run_nonengine(std::vector<std::string>(a.pos.begin() + 1, a.pos.end()), db);
	}
	if(cmd == "policy-train") {
		if(a.pos.size() < 2)
			return usage();
		return run_policy_train(std::vector<std::string>(a.pos.begin() + 1, a.pos.end()), db, a.opt);
	}
	if(cmd == "bridges" || cmd == "reachscan") {
		// ygoe-cli bridges deck.ydk [--reach data/reach.tsv]: the deck's bridge cards (reach.h)
		// ygoe-cli reachscan a.ydk b.ydk ... [--shard I/N] [--out reach.tsv]: what each card of
		// these lists can reach, tried in the rules core against all of them (the card pool)
		if(a.pos.size() < 2)
			return usage();
		std::vector<Deck> decks;
		for(size_t i = 1; i < a.pos.size(); ++i) {
			Deck d;
			std::string err;
			if(!load_ydk(a.pos[i], d, &err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
				return 1;
			}
			decks.push_back(std::move(d));
		}
		if(cmd == "bridges") {
			ReachTable table;
			const bool have = table.load(a.get("reach", "data/reach.tsv"));
			std::printf("%s\n", find_bridges(db, scripts, decks[0], have ? &table : nullptr).to_json(db).c_str());
			return 0;
		}
		std::vector<uint32_t> main_pool, extra_pool, all;
		for(const auto& d : decks)
			for(const auto* v : {&d.main, &d.extra, &d.side})
				for(uint32_t c : *v) {
					if(std::find(all.begin(), all.end(), c) != all.end())
						continue;
					all.push_back(c);
					const CardInfo* ci = db.get(db.script_id(c));
					if(!ci)
						continue;
					(ci->is_extra_deck() ? extra_pool : main_pool).push_back(c);
				}
		std::sort(all.begin(), all.end());
		int shard = 0, shards = 1;
		std::sscanf(a.get("shard", "0/1").c_str(), "%d/%d", &shard, &shards);
		std::FILE* out = std::fopen(a.get("out", "/dev/stdout").c_str(), "w");
		if(!out)
			return 1;
		size_t done = 0;
		for(size_t i = 0; i < all.size(); ++i) {
			if(static_cast<int>(i % static_cast<size_t>(std::max(1, shards))) != shard)
				continue;
			for(const auto& e : probe_card(db, scripts, all[i], main_pool, extra_pool))
				std::fprintf(out, "%u\t%u\t%u\n", e.from, e.to, e.how);
			std::fflush(out);
			if(++done % 25 == 0)
				std::fprintf(stderr, "%zu cards\n", done);
		}
		// what pairs of each list's monsters can be materials for
		if(shard == 0)
			for(const auto& d : decks)
				for(const auto& e : probe_materials(db, scripts, d))
					std::fprintf(out, "%u\t%u\t%u\n", e.from, e.to, e.how);
		std::fclose(out);
		return 0;
	}
	if(cmd == "followtest") {
		// ygoe-cli followtest a.ydk b.ydk [--seed N] [--turns T] [--games G]: random games, recorded
		// as YGO Omega would have, followed back (follow.h)
		if(a.pos.size() < 3)
			return usage();
		Deck d0, d1;
		std::string err;
		if(!load_ydk(a.pos[1], d0, &err) || !load_ydk(a.pos[2], d1, &err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		const uint64_t seed = std::stoull(a.get("seed", "1"));
		const int games = std::stoi(a.get("games", "1"));
		for(int g = 0; g < games; ++g)
			std::printf("%s\n", follow_selftest(db, scripts, d0, d1, seed + static_cast<uint64_t>(g), std::stoi(a.get("turns", "6"))).c_str());
		return 0;
	}
	if(cmd == "analyze")
		return cmd_analyze(db, scripts, a);
	if(cmd == "profile")
		return cmd_profile(db, a);
	if(cmd == "side")
		return cmd_side(db, scripts, a);
	if(cmd == "policy-bench") {
		// policy-bench train.ydk[,more.ydk] test.ydk [--train-hands N --train-iterations K --hands H
		//   --iterations I --seed S --save policy.json --load policy.json]
		// Learns a move policy from deep searches of the training decks, then searches the test
		// deck's hands at a small budget with and without it.
		if(a.pos.size() < 3)
			return usage();
		Policy policy(db);
		if(a.has("load")) {
			std::ifstream f(a.get("load", ""));
			std::stringstream ss;
			ss << f.rdbuf();
			if(!policy.from_json(ss.str())) {
				std::fprintf(stderr, "can't read %s\n", a.get("load", "").c_str());
				return 1;
			}
		}
		int train_hands = std::stoi(a.get("train-hands", "16"));
		int train_iters = std::stoi(a.get("train-iterations", "1500"));
		std::vector<PolicyExample> examples;
		std::string list = a.pos[1];
		for(size_t at = 0; at <= list.size();) {
			size_t comma = list.find(',', at);
			std::string path = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
			at = comma == std::string::npos ? list.size() + 1 : comma + 1;
			if(path.empty() || train_hands <= 0)
				continue;
			Deck d;
			if(!load_ydk(path, d, &err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
				return 1;
			}
			auto ex = self_play_examples(db, scripts, d, policy, train_hands, train_iters, std::stoull(a.get("seed", "900")) + 17);
			std::printf("%s: %zu decisions from %d hands\n", path.c_str(), ex.size(), train_hands);
			examples.insert(examples.end(), ex.begin(), ex.end());
		}
		if(!examples.empty()) {
			auto [before, after] = policy.train(examples);
			std::printf("trained on %zu decisions: log-loss %.3f -> %.3f\n", examples.size(), before, after);
		}
		if(a.has("save")) {
			std::ofstream o(a.get("save", ""));
			o << policy.to_json();
		}
		Deck deck;
		if(!load_ydk(a.pos[2], deck, &err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		auto shared = std::make_shared<Policy>(policy);
		int hands = std::stoi(a.get("hands", "10"));
		int iters = std::stoi(a.get("iterations", "300"));
		uint64_t seed = std::stoull(a.get("seed", "900"));
		Evaluator eval(db, deck, deck);
		eval.set_profile(0, generic_profile());
		eval.set_profile(1, generic_profile());
		double sum[2] = {0, 0};
		int wins = 0, ties = 0;
		for(int h = 0; h < hands; ++h) {
			Scenario sc;
			sc.decks[0] = sc.decks[1] = deck;
			sc.hand_known[0] = false;
			sc.hand_size[0] = 5;
			sc.hand_known[1] = true;
			sc.seed = seed + 100003 + h * 7919;  // not the training hands
			Searcher searcher(db, scripts, eval, declare_pool(sc));
			RootState root{make_setup(sc, 0), {}};
			double r[2];
			for(int g = 0; g < 2; ++g) {
				SearchConfig cfg;
				cfg.iterations = iters;
				cfg.seconds = 120;
				cfg.horizon_turns = 1;
				if(g)
					cfg.policy = shared;
				auto out = searcher.run({root}, 0, cfg);
				r[g] = 0;
				for(auto& m : out.my_moves)
					for(auto& t : m.best)
						r[g] = std::max(r[g], t.reward);
				sum[g] += r[g];
			}
			std::printf("hand %2d: rules %.3f  learned %.3f\n", h, r[0], r[1]);
			wins += r[1] > r[0] + 0.005;
			ties += std::abs(r[1] - r[0]) <= 0.005;
		}
		std::printf("average: rules %.3f  learned %.3f; learned better on %d, equal %d, worse %d of %d\n", sum[0] / hands,
		            sum[1] / hands, wins, ties, hands - wins - ties, hands);
		return 0;
	}
	if(cmd == "pieces-bench") {
		// pieces-bench deck.ydk reference.json [--hands N --iterations K --seed S]
		// Searches random hands going first with and without the reference's combo pieces.
		if(a.pos.size() < 3)
			return usage();
		Deck deck;
		if(!load_ydk(a.pos[1], deck, &err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		std::ifstream f(a.pos[2]);
		std::stringstream ss;
		ss << f.rdbuf();
		auto ref = json::Value::parse(ss.str());
		auto lib = parse_library(db, ref["library"]);
		auto guide = std::make_shared<PieceGuide>(lib, &db);
		std::printf("%zu pieces\n", guide->size());
		int hands = std::stoi(a.get("hands", "10"));
		int iters = std::stoi(a.get("iterations", "400"));
		uint64_t seed = std::stoull(a.get("seed", "500"));
		Evaluator eval(db, deck, deck);
		eval.set_profile(0, generic_profile());
		eval.set_profile(1, generic_profile());
		double sum[2] = {0, 0};
		int wins = 0, ties = 0;
		for(int h = 0; h < hands; ++h) {
			Scenario sc;
			sc.decks[0] = sc.decks[1] = deck;
			sc.hand_known[0] = false;
			sc.hand_size[0] = 5;
			sc.hand_known[1] = true;
			sc.seed = seed + h * 7919;
			Searcher searcher(db, scripts, eval, declare_pool(sc));
			RootState root{make_setup(sc, 0), {}};
			double r[2];
			for(int g = 0; g < 2; ++g) {
				SearchConfig cfg;
				cfg.iterations = iters;
				cfg.seconds = 120;
				cfg.horizon_turns = 1;
				if(g) {
					cfg.guide = guide;
					cfg.guided_share = std::stod(a.get("share", "0.7"));
				}
				auto out = searcher.run({root}, 0, cfg);
				r[g] = 0;
				for(auto& m : out.my_moves)
					for(auto& t : m.best)
						r[g] = std::max(r[g], t.reward);
				sum[g] += r[g];
			}
			Duel d(db, scripts);
			d.replay(root.setup, {});
			auto res = resources_of(db, d.board(), 0);
			std::printf("hand %2d: plain %.3f  pieces %.3f  (%zu playable)\n", h, r[0], r[1], guide->playable(res).size());
			wins += r[1] > r[0] + 0.005;
			ties += std::abs(r[1] - r[0]) <= 0.005;
		}
		std::printf("average: plain %.3f  pieces %.3f; pieces better on %d, equal %d, worse %d of %d\n", sum[0] / hands,
		            sum[1] / hands, wins, ties, hands - wins - ties, hands);
		return 0;
	}
	if(cmd == "proxy")
		return cmd_proxy(db, a);
	if(cmd == "duel") {
		if(a.pos.size() < 3)
			return usage();
		Deck d0, d1;
		if(!load_ydk(a.pos[1], d0, &err) || !load_ydk(a.pos[2], d1, &err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		auto level = [&](const std::string& k) {
			std::string v = a.get(k, "normal");
			return v == "easy" ? BotLevel::Easy : v == "strong" ? BotLevel::Strong : BotLevel::Normal;
		};
		BotLevel lv[2] = {level("bot0"), level("bot1")};
		LiveDuel live(db, scripts, d0, d1, std::stoull(a.get("seed", "1")));
		if(!live.ok()) {
			std::fprintf(stderr, "%s\n", live.error().c_str());
			return 1;
		}
		for(int p = 0; p < 2; ++p) {
			std::string path = a.get("lib" + std::to_string(p), "");
			if(path.empty())
				continue;
			std::ifstream lf(path);
			std::stringstream ls;
			ls << lf.rdbuf();
			auto v = json::Value::parse(ls.str());
			live.set_guide(p, std::make_shared<PieceGuide>(parse_library(db, v.is_array() ? v : v["library"]), &db));
		}
		uint32_t max_turns = static_cast<uint32_t>(std::stoi(a.get("turns", "4")));
		auto print_log = [&] {
			std::string st = live.state_json();
			auto v = json::Value::parse(st);
			for(auto& line : v["log"].items())
				std::printf("%s\n", line.str().c_str());
		};
		print_log();
		int moves = 0;
		auto t0 = std::chrono::steady_clock::now();
		while(!live.duel().ended() && live.duel().turn() <= max_turns) {
			uint8_t p = live.duel().decision().player;
			std::string label = live.bot_move(lv[p]);
			if(label.empty())
				break;
			++moves;
			std::printf("  > P%d: %s\n", p, label.c_str());
			print_log();
		}
		double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		auto b = live.duel().board();
		std::printf("\n%d bot decisions in %.1fs; LP %d / %d; %s\n", moves, secs, b.lp[0], b.lp[1],
		            live.duel().ended() ? ("winner P" + std::to_string(live.duel().winner())).c_str() : "not finished");
		return 0;
	}
	return usage();
}
