// Machine-learning commands: self-play data, combo libraries, the move policy, and comparing
// learned networks and policies. See tools/ml/README.md.
//
//   ygoe-cli selfplay <a.ydk> [<b.ydk> ...] --games N --out positions.jsonl [--seed S]
//            [--iterations 300] [--turns 14] [--net value_net.json|none]
//            [--libraries DIR] [--pieces phone_pieces.json] [--policy policy.json|none] [--phone-policy p.json]
//            [--explore 0.25] [--explore-moves 2] [--bo3 1] [--starts starts.jsonl] [--start-share 0.25]
//            [--policy-out examples.jsonl] [--choice-explore 0.2]
//     Bots play each other (random deck pairs from the list). With --bo3 a pairing is a match:
//     game 1 with a coin flip for who goes first, then the loser chooses first or second (by the
//     measured rates, --choice-explore 0.2 of the choices turned around), games 2 and 3 with
//     both decks sided for the matchup (side.h) until someone has won two. With --starts, a
//     share of the games starts from a real game's position instead (the engine's follow_replay writes
//     them when the app follows a YGO Omega replay, and the app uploads them). The bots get the decks' combo pieces (--libraries: DIR/<deck
//     name>.json, from `ygoe-cli library`) and the move policy, and explore a little early on.
//     At each Main Phase decision (a few per turn) the position is written twice, once from each
//     player's side: the value network's features, the hand-written raw value, and the result z:
//     1 / 0 for a finished game, and for a game cut at --turns the current evaluator's win chance
//     there (cards in hand, board and GY count, not only LP). tools/ml/train.py learns from them.
//     With --policy-out, the bots' searches are written as move-policy examples: every option's
//     features and the share of the search's visits it got (`ygoe-cli policy-train` learns them).
//
//   ygoe-cli arena <a.ydk> [<b.ydk> ...] --games N --net new.json [--vs old.json|none]
//            [--policy new_policy.json] [--vs-policy old_policy.json|none] [--libraries DIR] ...
//            [--iterations 300] [--turns 14] [--seed S] [--out result.json]
//     The same bots, one with the new network and policy, one with the old: every deal is played
//     twice, the same decks and shuffles, the two bots swapping seats (that cancels most of the
//     luck of the draw). Prints the share of points the new bot scored (a win 1, a draw 0.5, a
//     game cut at --turns by the evaluator's win chance) and writes each pair's score.
//
//   ygoe-cli library <deck.ydk> --out lib.json [--hands 40] [--iterations 1500] [--seed S] [--pieces p.json]
//                    [--carry earlier.json --carry-out next.json | --carry-dir DIR] [--max-pieces 400]
//     Combo pieces for a deck's bot: the best lines of many opening hands found by deep search
//     (reference.h), added to those of earlier runs (--carry, written by --carry-out: the
//     library grows run after run, the most played pieces kept), plus the pieces of --pieces
//     (the app's, from replays and videos) that the deck can play.
//
//   ygoe-cli policy-train <examples.jsonl> [...] --out policy.json [--init policy.json] [--epochs 4] [--report r.json]
//     Trains the move policy on self-play examples; 15% of the games are held out, and the report
//     gives their log-loss with the rule prior alone, the starting policy and the trained one.
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "carddb.h"
#include "deck.h"
#include "duel.h"
#include "evaluate.h"
#include "json.h"
#include "live.h"
#include "nonengine.h"
#include "ocgapi_constants.h"
#include "pieces.h"
#include "policy.h"
#include "profile.h"
#include "reference.h"
#include "side.h"
#include "valuenet.h"

namespace ygoe {

namespace {

using Opts = std::map<std::string, std::string>;

std::string opt_get(const Opts& opt, const std::string& k, const std::string& def) {
	auto it = opt.find(k);
	return it == opt.end() ? def : it->second;
}

std::string read_file(const std::string& path) {
	std::ifstream f(path);
	if(!f)
		return "";
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

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

// The policy a bot uses: `path` ("" the data folder's, "none" none) plus the app's (`phone`).
std::shared_ptr<const Policy> policy_from(const std::string& path, const std::string& phone, const CardDB& db) {
	std::shared_ptr<Policy> p;
	// the self-play policy at Policy::kSelfPlayScale, as every search in the engine uses it
	if(path.empty()) {
		if(db.policy()) {
			p = std::make_shared<Policy>(db);
			p->add(*db.policy(), Policy::kSelfPlayScale);
		}
	} else if(path != "none") {
		Policy q(db);
		if(q.from_json(read_file(path)) && q.trained()) {
			p = std::make_shared<Policy>(db);
			p->add(q, Policy::kSelfPlayScale);
		}
	}
	if(!phone.empty()) {
		Policy ph(db);
		if(ph.from_json(read_file(phone)) && ph.trained()) {
			if(!p)
				p = std::make_shared<Policy>(db);
			p->add(ph);
		}
	}
	return p;
}

std::string base_name(const std::string& path) {
	std::string b = path.substr(path.find_last_of("/\\") + 1);
	auto dot = b.rfind('.');
	return dot == std::string::npos ? b : b.substr(0, dot);
}

// Pieces of `lib` the deck can play: every starting-hand card is in it.
std::vector<LibraryPiece> playable_by(const CardDB& db, const Deck& deck, const std::vector<LibraryPiece>& lib) {
	std::vector<uint32_t> cards;
	for(auto c : deck.main)
		cards.push_back(db.script_id(c));
	for(auto c : deck.extra)
		cards.push_back(db.script_id(c));
	std::sort(cards.begin(), cards.end());
	std::vector<LibraryPiece> out;
	for(const auto& p : lib) {
		bool ok = !p.piece.needs_hand.empty();
		for(auto c : p.piece.needs_hand)
			ok = ok && std::binary_search(cards.begin(), cards.end(), db.script_id(c));
		if(ok)
			out.push_back(p);
	}
	return out;
}

struct Decks {
	std::vector<Deck> decks;
	std::vector<std::shared_ptr<const PieceGuide>> guides;
};

bool load_decks(const std::vector<std::string>& paths, const CardDB& db, const Opts& opt, Decks& out) {
	std::string err;
	const std::string dir = opt_get(opt, "libraries", "");
	std::vector<LibraryPiece> phone;
	if(opt.count("pieces"))
		phone = parse_library(db, read_file(opt.at("pieces")));
	for(const auto& p : paths) {
		Deck d;
		if(!load_ydk(p, d, &err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return false;
		}
		std::vector<LibraryPiece> lib;
		if(!dir.empty()) {
			std::string text = read_file(dir + "/" + base_name(p) + ".json");
			if(!text.empty())
				lib = parse_library(db, text);
		}
		auto more = playable_by(db, d, phone);
		lib.insert(lib.end(), more.begin(), more.end());
		out.guides.push_back(lib.empty() ? nullptr : std::make_shared<PieceGuide>(std::move(lib), &db));
		out.decks.push_back(std::move(d));
	}
	return true;
}

// A deck sided for one matchup (side.h): the plan's swaps applied. Cached per matchup.
class Siding {
public:
	explicit Siding(const CardDB& db) : db_(db) {}
	const Deck& sided(size_t mine, size_t theirs, bool first, const std::vector<Deck>& decks) {
		const auto key = std::make_tuple(mine, theirs, first);
		auto it = cache_.find(key);
		if(it != cache_.end())
			return it->second;
		Deck d = decks[mine];
		if(!d.side.empty()) {
			auto plan = plan_side(db_, decks[mine], decks[theirs], profile_deck(db_, decks[theirs]), BoardShape{}, first);
			for(const auto& s : plan.swaps) {
				auto o = std::find(d.main.begin(), d.main.end(), s.out);
				auto i = std::find(d.side.begin(), d.side.end(), s.in);
				if(o == d.main.end() || i == d.side.end())
					continue;
				*o = s.in;
				*i = s.out;
			}
		}
		return cache_.emplace(key, std::move(d)).first->second;
	}

private:
	const CardDB& db_;
	std::map<std::tuple<size_t, size_t, bool>, Deck> cache_;
};

// One bot's equipment.
struct Bot {
	std::shared_ptr<const ValueNet> net;
	std::shared_ptr<const Policy> policy;
	std::shared_ptr<const PieceGuide> guide;
	std::shared_ptr<const NeTiming> timing;  // non-engine timing (nonengine.h)
};

std::shared_ptr<const NeTiming> timing_from(const std::string& path, const CardDB& db) {
	if(path.empty())
		return db.nonengine_timing();
	if(path == "none")
		return nullptr;
	auto t = std::make_shared<NeTiming>();
	if(!t->parse(read_file(path)))
		return nullptr;
	return t;
}

struct PlayConfig {
	int iterations = 300;
	int max_turns = 14;
	double explore = 0;
	int explore_moves = 0;
	const Scenario* start = nullptr;  // a real game's position, player 0 the turn player
	int turn_offset = 0;              // that position's turn number minus 1
	double ne_explore = 0;            // non-engine choices turned around (self-play)
	LiveDuel::NonEngineHook ne_hook;
};

struct Game {
	int winner = -1;     // 0 / 1, -1 draw or cut
	bool finished = false;
	double z0 = 0.5;     // result for player 0: 1, 0, 0.5 for a draw; the win chance when cut
	int lp[2] = {8000, 8000};
	int turns = 0;
};

// Plays one game; `on_position` is called at each Main Phase decision of either player.
template <class F>
Game play(const CardDB& db, ScriptCache& scripts, const Deck& d0, const Deck& d1, uint64_t seed, const PlayConfig& pc,
          const Bot& b0, const Bot& b1, F&& on_position, const LiveDuel::SearchHook& hook = {}, LiveDuel** expose = nullptr) {
	Game g;
	std::unique_ptr<LiveDuel> lp;
	if(pc.start)
		lp = std::make_unique<LiveDuel>(db, scripts, *pc.start, DUEL_MODE_MR5 | DUEL_ATTACK_FIRST_TURN);
	else
		lp = std::make_unique<LiveDuel>(db, scripts, d0, d1, seed);
	LiveDuel& live = *lp;
	if(!live.ok())
		return g;
	if(expose)
		*expose = &live;
	live.set_iterations(pc.iterations);
	live.set_value_net(0, b0.net);
	live.set_value_net(1, b1.net);
	live.set_policy(0, b0.policy);
	live.set_policy(1, b1.policy);
	live.set_guide(0, b0.guide);
	live.set_guide(1, b1.guide);
	live.set_nonengine_timing(0, b0.timing);
	live.set_nonengine_timing(1, b1.timing);
	if(pc.ne_hook)
		live.set_nonengine_exploration(pc.ne_explore, pc.ne_hook);
	if(pc.explore > 0)
		live.set_exploration(pc.explore, pc.explore_moves);
	if(hook)
		live.set_search_hook(hook);
	const int last_turn = std::max(pc.max_turns - pc.turn_offset, 4);
	int guard = 0;
	while(!live.duel().ended() && static_cast<int>(live.duel().turn()) <= last_turn && guard++ < 4000) {
		const Decision& d = live.duel().decision();
		if(d.type == DecisionType::Idle)
			on_position(live);
		if(live.bot_move(BotLevel::Normal).empty())
			break;
	}
	const Board b = live.duel().board();
	g.lp[0] = b.lp[0];
	g.lp[1] = b.lp[1];
	g.turns = static_cast<int>(live.duel().turn()) + pc.turn_offset;
	if(live.duel().ended()) {
		g.finished = true;
		g.winner = live.duel().winner();
		g.z0 = g.winner == 0 ? 1.0 : g.winner == 1 ? 0.0 : 0.5;
	} else {
		// cut: the current evaluator's (and network's) win chance for player 0 there
		Evaluator ev(db, pc.start ? pc.start->decks[0] : d0, pc.start ? pc.start->decks[1] : d1);
		ev.set_value_net(db.value_net());
		Board bb = b;
		bb.turn += static_cast<uint32_t>(pc.turn_offset);
		g.z0 = ev.evaluate(bb, 0, static_cast<uint8_t>(1 - b.turn_player)).score;
		if(g.z0 > 0.5)
			g.winner = 0;
		else if(g.z0 < 0.5)
			g.winner = 1;
	}
	if(expose)
		*expose = nullptr;
	return g;
}

// A real game's position to start from (follow_replay writes them as "kind":"start").
struct Start {
	Scenario sc;
	int turn = 1;
};

std::vector<Start> load_starts(const std::string& path) {
	std::vector<Start> out;
	std::ifstream in(path);
	std::string line;
	while(std::getline(in, line)) {
		if(line.find("\"kind\":\"start\"") == std::string::npos)
			continue;
		std::string err;
		auto v = json::Value::parse(line, &err);
		if(!v.is_object())
			continue;
		Scenario sc;
		bool ok = true;
		for(int p = 0; p < 2; ++p) {
			const auto& d = v["decks"][static_cast<size_t>(p)];
			for(const auto& c : d["main"].items())
				sc.decks[p].main.push_back(static_cast<uint32_t>(c.integer()));
			for(const auto& c : d["extra"].items())
				sc.decks[p].extra.push_back(static_cast<uint32_t>(c.integer()));
			for(const auto& c : v["hand"][static_cast<size_t>(p)].items())
				sc.hand[p].push_back(static_cast<uint32_t>(c.integer()));
			sc.hand_size[p] = static_cast<size_t>(v["hand_size"][static_cast<size_t>(p)].integer(0));
			sc.hand_known[p] = sc.hand[p].size() == sc.hand_size[p];
			sc.lp[p] = static_cast<uint32_t>(std::max<int64_t>(1, v["lp"][static_cast<size_t>(p)].integer(8000)));
			for(const auto& c : v["placed"][static_cast<size_t>(p)].items()) {
				PlacedCard pc;
				pc.code = static_cast<uint32_t>(c["c"].integer());
				pc.loc = static_cast<uint32_t>(c["l"].integer());
				pc.seq = static_cast<uint32_t>(c["s"].integer());
				pc.pos = static_cast<uint32_t>(c["p"].integer());
				sc.placed[p].push_back(pc);
			}
			ok = ok && !sc.decks[p].main.empty();
		}
		if(ok)
			out.push_back({std::move(sc), static_cast<int>(v["turn"].integer(2))});
	}
	return out;
}

// One self-play position, from one side.
struct Pos {
	std::vector<float> f;
	double raw;
	uint8_t me;
	uint32_t turn;
};

// Writes a game's positions; z from player 0's result.
int write_positions(std::ofstream& out, const std::vector<Pos>& pos, const Game& g, const std::string& game, const char* src) {
	int n = 0;
	int idx[2] = {0, 0};
	for(const auto& p : pos) {
		const double z = p.me == 0 ? g.z0 : 1.0 - g.z0;
		out << "{\"z\":" << z << ",\"fin\":" << (g.finished ? 1 : 0) << ",\"raw\":" << p.raw << ",\"turn\":" << p.turn
		    << ",\"me\":" << int(p.me) << ",\"i\":" << idx[p.me]++ << ",\"game\":" << json::quote(game);
		if(src)
			out << ",\"src\":\"" << src << "\"";
		out << ",\"f\":[";
		for(size_t i = 0; i < p.f.size(); ++i)
			out << (i ? "," : "") << p.f[i];
		out << "]}\n";
		++n;
	}
	out.flush();
	return n;
}

}  // namespace

int run_selfplay(const std::vector<std::string>& decks_paths, const CardDB& db, ScriptCache& scripts, const Opts& opt) {
	auto get = [&](const std::string& k, const std::string& def) { return opt_get(opt, k, def); };
	std::string err;
	Decks dk;
	if(!load_decks(decks_paths, db, opt, dk))
		return 1;
	const auto& decks = dk.decks;
	const int games = std::stoi(get("games", "4"));
	PlayConfig pc;
	pc.iterations = std::stoi(get("iterations", "300"));
	pc.max_turns = std::stoi(get("turns", "14"));
	pc.explore = std::stod(get("explore", "0"));
	pc.explore_moves = std::stoi(get("explore-moves", "2"));
	const bool bo3 = get("bo3", "0") != "0";
	// the loser of a match's game chooses to go first or second (nonengine.h choose_first); this
	// share of the choices is turned around, so both keep being measured
	const double choice_explore = std::stod(get("choice-explore", "0.2"));
	const uint64_t seed = std::stoull(get("seed", "1"));
	auto net = net_from(get("net", ""), db, err);
	if(!err.empty()) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	auto policy = policy_from(get("policy", ""), get("phone-policy", ""), db);
	auto timing = timing_from(get("ne-timing", ""), db);
	pc.ne_explore = std::stod(get("ne-explore", "0"));
	std::unique_ptr<std::ofstream> ne_out, games_out;
	if(opt.count("ne-out"))
		ne_out = std::make_unique<std::ofstream>(opt.at("ne-out"), std::ios::app);
	if(opt.count("games-out"))
		games_out = std::make_unique<std::ofstream>(opt.at("games-out"), std::ios::app);
	// each deck's non-engine cards (nonengine.h), by its address
	std::map<const Deck*, std::vector<NeCard>> ne_of;
	auto nonengine = [&](const Deck& d) -> const std::vector<NeCard>& {
		auto it = ne_of.find(&d);
		if(it == ne_of.end())
			it = ne_of.emplace(&d, classify_nonengine(db, d)).first;
		return it->second;
	};
	std::vector<Start> starts;
	if(opt.count("starts"))
		starts = load_starts(opt.at("starts"));
	const double start_share = starts.empty() ? 0.0 : std::stod(get("start-share", "0.25"));
	std::ofstream out(get("out", "positions.jsonl"), std::ios::app);
	std::unique_ptr<std::ofstream> pol_out;
	if(opt.count("policy-out"))
		pol_out = std::make_unique<std::ofstream>(opt.at("policy-out"), std::ios::app);
	Policy features_of(db);  // only its features() (untrained)
	std::mt19937_64 rng(seed);
	Siding siding(db);
	std::printf("%zu decks, %d with combo pieces; policy %s; %zu start positions\n", decks.size(),
	            static_cast<int>(std::count_if(dk.guides.begin(), dk.guides.end(), [](auto& g) { return g != nullptr; })),
	            policy ? "yes" : "no", starts.size());
	int written = 0, examples = 0;
	auto t0 = std::chrono::steady_clock::now();
	for(int gi = 0; gi < games; ++gi) {
		std::uniform_real_distribution<double> unit(0, 1);
		const std::string match_id = std::to_string(seed) + "-" + std::to_string(gi);
		// the search's visits at the bots' decisions, as policy examples
		int game_examples = 0;
		std::string game_id;
		LiveDuel::SearchHook hook;
		LiveDuel* current = nullptr;
		if(pol_out)
			hook = [&](const Decision& d, const SearchOutput& so) {
				if(d.minor || d.options.size() < 2 || game_examples >= 40 || !current)
					return;
				int total = 0;
				for(const auto& m : so.moves)
					total += m.visits;
				if(total < 40 || so.moves.front().visits > total * 0.97)
					return;  // too few, or the search had no doubt: little to learn
				std::vector<float> target(d.options.size(), 0.0f);
				for(const auto& m : so.moves)
					for(size_t i = 0; i < d.options.size(); ++i)
						if(d.options[i].label == m.label) {
							target[i] += static_cast<float>(m.visits) / static_cast<float>(total);
							break;
						}
				const PolicyContext c{current->duel().turn_player(), current->steps_this_turn(), current->duel().chain_depth()};
				std::ostringstream o;
				o << "{\"g\":" << json::quote(game_id) << ",\"o\":[";
				for(size_t i = 0; i < d.options.size(); ++i) {
					std::vector<uint32_t> f;
					features_of.features(d, d.options[i], c, f);
					o << (i ? "," : "") << "[";
					for(size_t k = 0; k < f.size(); ++k)
						o << (k ? "," : "") << f[k];
					o << "]";
				}
				o << "],\"p\":[";
				for(size_t i = 0; i < d.options.size(); ++i)
					o << (i ? "," : "") << playout_weight(d, d.options[i], c.turn_player, c.steps_this_turn);
				o << "],\"t\":[";
				for(size_t i = 0; i < target.size(); ++i)
					o << (i ? "," : "") << target[i];
				o << "]}\n";
				*pol_out << o.str();
				++game_examples;
				++examples;
			};

		// one game: positions recorded and written; returns the result
		auto one = [&](const Deck& d0, const Deck& d1, const Bot& b0, const Bot& b1, uint64_t gseed, const PlayConfig& gpc,
		               const std::string& gid, const char* src) {
			Evaluator base(db, d0, d1);
			base.set_value_net(nullptr);  // base values without the network (it learns a correction)
			std::vector<Pos> pos;
			uint32_t last_turn = 0;
			int this_turn = 0;
			game_id = gid;
			game_examples = 0;
			// non-engine cards each player had in hand early (turns 1-4), and the chances to use one
			std::set<uint32_t> seen[2];
			struct NeChance {
				uint8_t player;
				std::vector<float> f;
				bool used;
			};
			std::vector<NeChance> chances;
			PlayConfig gpc2 = gpc;
			if(ne_out)
				gpc2.ne_hook = [&](uint8_t player, const std::vector<float>& f, bool used) { chances.push_back({player, f, used}); };
			auto record = [&](LiveDuel& live) {
				Board b = live.duel().board();
				if(!gpc.start && b.turn <= 4)
					for(uint8_t p = 0; p < 2; ++p)
						for(const auto& c : b.hand[p])
							if(c.code)
								seen[p].insert(c.code);
				b.turn += static_cast<uint32_t>(gpc.turn_offset);
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
			Game g = play(db, scripts, d0, d1, gseed, gpc2, b0, b1, record, hook, &current);
			written += write_positions(out, pos, g, gid, src);
			if(ne_out) {
				for(const auto& c : chances) {
					*ne_out << "{\"g\":" << json::quote(gid) << ",\"z\":" << (c.player == 0 ? g.z0 : 1.0 - g.z0) << ",\"u\":" << (c.used ? 1 : 0)
					        << ",\"f\":[";
					for(size_t i = 0; i < c.f.size(); ++i)
						*ne_out << (i ? "," : "") << c.f[i];
					*ne_out << "]}\n";
				}
				ne_out->flush();
			}
			if(games_out && !gpc.start) {
				// which non-engine cards each side had early, for their values (tools/ml/nonengine.py)
				*games_out << "{\"g\":" << json::quote(gid) << ",\"z0\":" << g.z0 << ",\"fin\":" << (g.finished ? 1 : 0) << ",\"arch\":["
				           << archetype_key(db, d0) << "," << archetype_key(db, d1) << "],\"ne\":[";
				for(int p = 0; p < 2; ++p) {
					*games_out << (p ? "," : "") << "[";
					bool f = true;
					for(const auto& c : nonengine(p ? d1 : d0)) {
						if(c.engine() || !c.copies)
							continue;
						const uint32_t sid = db.script_id(c.code);
						*games_out << (f ? "" : ",") << "{\"c\":" << sid << ",\"n\":" << c.copies << ",\"seen\":" << (seen[p].count(sid) ? 1 : 0)
						           << "}";
						f = false;
					}
					*games_out << "]";
				}
				*games_out << "]}\n";
				games_out->flush();
			}
			const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			std::printf("game %s: %s, %d turns, winner %d (z %.2f, LP %d/%d), %zu positions, %.0fs\n", gid.c_str(),
			            g.finished ? "finished" : "cut", g.turns, g.winner, g.z0, g.lp[0], g.lp[1], pos.size(), secs);
			std::fflush(stdout);
			return g;
		};

		if(!starts.empty() && unit(rng) < start_share) {
			// a real game's position, played on by the bots (with that game's decks)
			const Start& st = starts[rng() % starts.size()];
			Scenario sc = st.sc;
			PlayConfig spc = pc;
			spc.turn_offset = st.turn - 1;
			sc.seed = rng();
			spc.start = &sc;
			Bot b{net, policy, nullptr, timing};
			one(sc.decks[0], sc.decks[1], b, b, sc.seed, spc, match_id, "start");
			continue;
		}
		const size_t a = rng() % decks.size(), bdx = rng() % decks.size();
		const size_t idx[2] = {a, bdx};
		if(!bo3) {
			const bool swap = gi % 2 == 1;
			const size_t p0 = swap ? bdx : a, p1 = swap ? a : bdx;
			one(decks[p0], decks[p1], Bot{net, policy, dk.guides[p0], timing}, Bot{net, policy, dk.guides[p1], timing},
			    seed * 7919 + static_cast<uint64_t>(gi), pc, match_id, nullptr);
			continue;
		}
		// a match: game 1 by a coin flip, then the loser chooses to go first or second; games 2-3 sided
		int wins[2] = {0, 0};
		int first = static_cast<int>(rng() % 2);  // index into idx: who goes first
		for(int game = 1; game <= 3 && wins[0] < 2 && wins[1] < 2; ++game) {
			const size_t f = idx[first], s = idx[1 - first];
			const Deck& df = game == 1 ? decks[f] : siding.sided(f, s, true, decks);
			const Deck& ds = game == 1 ? decks[s] : siding.sided(s, f, false, decks);
			Game g = one(df, ds, Bot{net, policy, dk.guides[f], timing}, Bot{net, policy, dk.guides[s], timing},
			             seed * 7919 + static_cast<uint64_t>(gi) * 3 + static_cast<uint64_t>(game), pc,
			             match_id + "-g" + std::to_string(game), nullptr);
			int loser;
			if(g.winner == 0) {
				++wins[first];
				loser = 1 - first;
			} else if(g.winner == 1) {
				++wins[1 - first];
				loser = first;
			} else {
				loser = static_cast<int>(rng() % 2);
			}
			if(wins[0] >= 2 || wins[1] >= 2)
				break;
			double edge = 0;
			bool goes_first = choose_first(db, decks[idx[loser]], decks[idx[1 - loser]], &edge);
			const bool turned = unit(rng) < choice_explore;
			if(turned)
				goes_first = !goes_first;
			std::printf("match %s: the loser (deck %zu) goes %s (first minus second %+.2f%s)\n", match_id.c_str(), idx[loser],
			            goes_first ? "first" : "second", edge, turned ? ", turned around" : "");
			first = goes_first ? loser : 1 - loser;
		}
	}
	std::printf("%d positions written", written);
	if(pol_out)
		std::printf(", %d policy examples", examples);
	std::printf("\n");
	return 0;
}

int run_arena(const std::vector<std::string>& decks_paths, const CardDB& db, ScriptCache& scripts, const Opts& opt) {
	auto get = [&](const std::string& k, const std::string& def) { return opt_get(opt, k, def); };
	std::string err;
	Decks dk;
	if(!load_decks(decks_paths, db, opt, dk))
		return 1;
	const auto& decks = dk.decks;
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
	const std::string phone = get("phone-policy", "");
	auto fresh_pol = policy_from(get("policy", ""), phone, db);
	auto old_pol = policy_from(get("vs-policy", ""), phone, db);
	auto fresh_t = timing_from(get("ne-timing", ""), db);
	auto old_t = timing_from(get("vs-ne-timing", ""), db);
	const int games = std::stoi(get("games", "8"));
	PlayConfig pc;
	pc.iterations = std::stoi(get("iterations", "300"));
	pc.max_turns = std::stoi(get("turns", "14"));
	const uint64_t seed = std::stoull(get("seed", "1"));
	double points = 0;
	int played = 0;
	std::vector<double> pairs;
	double pair_points = 0;
	for(int gi = 0; gi < games; ++gi) {
		// deck pairs in turn (each --seed starts further along, so parallel arenas cover different
		// pairs); the new bot sits first, then second, with the same decks and shuffles
		const size_t pair = static_cast<size_t>(gi / 2) + static_cast<size_t>((seed - 1) * static_cast<uint64_t>((games + 1) / 2));
		const size_t ia = pair % decks.size();
		const size_t ib = (pair / decks.size() + 1 + pair) % decks.size();
		const bool new_first = gi % 2 == 0;
		const Bot nb0{fresh, fresh_pol, dk.guides[ia], fresh_t}, ob0{old, old_pol, dk.guides[ia], old_t};
		const Bot nb1{fresh, fresh_pol, dk.guides[ib], fresh_t}, ob1{old, old_pol, dk.guides[ib], old_t};
		Game g = play(db, scripts, decks[ia], decks[ib], seed * 104729 + static_cast<uint64_t>(pair), pc, new_first ? nb0 : ob0,
		              new_first ? ob1 : nb1, [](LiveDuel&) {});
		const double r = new_first ? g.z0 : 1.0 - g.z0;
		points += r;
		pair_points += r;
		++played;
		if(gi % 2 == 1 || gi + 1 == games) {
			pairs.push_back(pair_points / (gi % 2 == 1 ? 2 : 1));
			pair_points = 0;
		}
		std::printf("game %d/%d: new bot %s, %s %d turns -> %.2f (total %.2f/%d)\n", gi + 1, games, new_first ? "first" : "second",
		            g.finished ? "finished" : "cut", g.turns, r, points, played);
		std::fflush(stdout);
	}
	const double share = played ? points / played : 0.5;
	std::printf("new bot score: %.3f over %d games\n", share, played);
	if(opt.count("out")) {
		std::ofstream o(get("out", ""));
		o << "{\"score\":" << share << ",\"games\":" << played << ",\"pairs\":[";
		for(size_t i = 0; i < pairs.size(); ++i)
			o << (i ? "," : "") << pairs[i];
		o << "]}\n";
	}
	return 0;
}

int run_library(const std::string& deck_path, const CardDB& db, ScriptCache& scripts, const Opts& opt) {
	auto get = [&](const std::string& k, const std::string& def) { return opt_get(opt, k, def); };
	Deck d;
	std::string err;
	if(!load_ydk(deck_path, d, &err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	const int hands = std::stoi(get("hands", "40"));
	const int iterations = std::stoi(get("iterations", "1500"));
	auto t0 = std::chrono::steady_clock::now();
	auto res = build_reference(db, scripts, d, hands, iterations, std::stoull(get("seed", "1")), nullptr, [&](int done, int total) {
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("hand %d/%d, %.0fs\n", done, total, secs);
		std::fflush(stdout);
	});
	std::vector<LibraryPiece> lib = res.library;
	// the lines found by earlier runs for this deck (--carry: their own pieces, without the
	// app's): this run's are added to them, so the library grows run after run
	size_t carried = 0;
	int lines = static_cast<int>(res.samples.size());
	// --carry-dir: the deck's archetype's file there (any deck: an archetype keeps its lines when
	// a newer list of it is played; one without an archetype goes by its file name)
	std::string carry = opt.count("carry") ? opt.at("carry") : "", carry_out = opt.count("carry-out") ? opt.at("carry-out") : "";
	if(opt.count("carry-dir")) {
		const uint16_t arch = archetype_key(db, d);
		std::error_code ec;
		std::filesystem::create_directories(opt.at("carry-dir"), ec);
		carry = carry_out = opt.at("carry-dir") + "/library-" + (arch ? std::to_string(arch) : base_name(deck_path)) + ".json";
		std::printf("library carried in %s\n", carry.c_str());
	}
	if(!carry.empty() && std::ifstream(carry).good()) {
		const std::string text = read_file(carry);
		const json::Value v = json::Value::parse(text);
		auto earlier = playable_by(db, d, parse_library(db, v.is_object() ? v["pieces"] : v));
		carried = earlier.size();
		if(v.is_object())
			lines += static_cast<int>(v["lines"].integer(0));
		merge_library(lib, earlier);
	}
	sort_library(lib, static_cast<size_t>(std::stoi(get("max-pieces", "400"))));
	if(!carry_out.empty())
		std::ofstream(carry_out) << "{\"lines\":" << lines << ",\"pieces\":" << library_json(db, lib, lines) << "}";
	const size_t own = lib.size();
	if(opt.count("pieces")) {
		auto more = playable_by(db, d, parse_library(db, read_file(opt.at("pieces"))));
		lib.insert(lib.end(), more.begin(), more.end());
	}
	std::ofstream(get("out", base_name(deck_path) + ".json")) << library_json(db, lib, static_cast<int>(res.samples.size()));
	std::printf("%zu pieces from %d lines (%zu this run, %zu pieces from earlier runs), %zu from the app's replays and videos\n", own,
	            lines, res.samples.size(), carried, lib.size() - own);
	return 0;
}

int run_policy_train(const std::vector<std::string>& files, const CardDB& db, const Opts& opt) {
	auto get = [&](const std::string& k, const std::string& def) { return opt_get(opt, k, def); };
	std::vector<PolicyExample> train, val;
	std::hash<std::string> h;
	for(const auto& path : files) {
		std::ifstream in(path);
		std::string line;
		while(std::getline(in, line)) {
			auto v = json::Value::parse(line);
			if(!v.is_object())
				continue;  // a line cut by a stopped job
			PolicyExample e;
			for(const auto& o : v["o"].items()) {
				std::vector<uint32_t> f;
				for(const auto& x : o.items())
					f.push_back(static_cast<uint32_t>(x.integer()) % Policy::kDim);
				e.options.push_back(std::move(f));
			}
			for(const auto& x : v["p"].items())
				e.prior.push_back(static_cast<float>(x.num(1)));
			for(const auto& x : v["t"].items())
				e.target.push_back(static_cast<float>(x.num(0)));
			if(e.options.size() < 2 || e.prior.size() != e.options.size() || e.target.size() != e.options.size())
				continue;
			e.chosen = static_cast<int>(std::max_element(e.target.begin(), e.target.end()) - e.target.begin());
			(h(v["g"].str()) % 100 < 15 ? val : train).push_back(std::move(e));
		}
	}
	if(val.empty())  // few games: every 7th example instead
		for(size_t i = train.size(); i-- > 0;)
			if(i % 7 == 3) {
				val.push_back(std::move(train[i]));
				train.erase(train.begin() + static_cast<long>(i));
			}
	if(train.empty() || val.empty()) {
		std::fprintf(stderr, "not enough policy examples (%zu training, %zu held out)\n", train.size(), val.size());
		return 1;
	}
	Policy rules(db);
	Policy p(db);
	if(opt.count("init"))
		p.from_json(read_file(opt.at("init")));
	const double rule_loss = rules.loss_of(val), before = p.loss_of(val);
	auto tl = p.train(train, std::stoi(get("epochs", "4")), std::stod(get("rate", "0.05")));
	const double after = p.loss_of(val);
	std::ofstream(get("out", "policy.json")) << p.to_json();
	std::ostringstream r;
	r << "{\"examples\":" << train.size() << ",\"validation_examples\":" << val.size() << ",\"rules_logloss\":" << rule_loss
	  << ",\"init_logloss\":" << before << ",\"policy_logloss\":" << after << ",\"train_logloss_before\":" << tl.first
	  << ",\"train_logloss_after\":" << tl.second << "}";
	std::printf("%s\n", r.str().c_str());
	if(opt.count("report"))
		std::ofstream(opt.at("report")) << r.str() << "\n";
	return 0;
}

// ygoe-cli nonengine <deck.ydk> [<opponent.ydk> ...]: the deck's non-engine cards (Main and Side
// Deck) with their role, what they stop and their measured value against the opponents (the
// format when none are given: data/nonengine_values.json), best first.
int run_nonengine(const std::vector<std::string>& paths, const CardDB& db) {
	std::vector<Deck> decks;
	for(const auto& p : paths) {
		Deck d;
		std::string err;
		if(!load_ydk(p, d, &err)) {
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
		decks.push_back(std::move(d));
	}
	const Deck& mine = decks.front();
	std::vector<Deck> opp(decks.begin() + 1, decks.end());
	std::printf("archetype %u; measured values: %s\n", archetype_key(db, mine), db.nonengine_values() ? "yes" : "none yet");
	std::printf("%-44s %-14s %5s %5s %8s %8s %6s  stops\n", "card", "role", "main", "side", "1st", "2nd", "games");
	for(const auto& ch : rank_nonengine(db, mine, opp)) {
		std::string stops;
		for(const auto& s : stops_names(ch.card.stops))
			stops += (stops.empty() ? "" : ", ") + s;
		auto pct = [&](double v, bool m) {
			char b[16];
			std::snprintf(b, sizeof b, m ? "%+.1f%%" : "-", v * 100);
			return std::string(b);
		};
		std::printf("%-44.44s %-14s %5d %5d %8s %8s %6d  %s\n", db.name(db.script_id(ch.card.code)).c_str(), ne_role_name(ch.card.role),
		            ch.card.copies, ch.card.side_copies, pct(ch.value_first, ch.measured_first).c_str(), pct(ch.value_second, ch.measured_second).c_str(), ch.games, stops.c_str());
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
