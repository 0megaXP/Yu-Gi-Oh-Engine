#include "learn.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>

#include "carddb.h"
#include "duel.h"
#include "evaluate.h"
#include "json.h"
#include "live.h"
#include "nettrain.h"
#include "nonengine.h"
#include "pieces.h"
#include "policy.h"
#include "reference.h"
#include "search.h"
#include "selfplay.h"
#include "valuenet.h"

namespace ygoe {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string read_text(const fs::path& p) {
	std::ifstream f(p, std::ios::binary);
	if(!f)
		return "";
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// Written whole or not at all (a stopped session leaves the old file).
void write_text(const fs::path& p, const std::string& text) {
	fs::path tmp = p;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		f << text;
	}
	std::error_code ec;
	fs::rename(tmp, p, ec);
	if(ec) {
		fs::remove(p, ec);
		fs::rename(tmp, p, ec);
	}
}

// `lines` added at the end of the file, which keeps its last `keep` lines.
void append_keep(const fs::path& p, const std::vector<std::string>& lines, size_t keep) {
	std::deque<std::string> all;
	{
		std::ifstream in(p);
		std::string l;
		while(std::getline(in, l))
			if(!l.empty())
				all.push_back(std::move(l));
	}
	for(const auto& l : lines)
		all.push_back(l);
	while(all.size() > keep)
		all.pop_front();
	std::string out;
	for(const auto& l : all)
		out += l + "\n";
	write_text(p, out);
}

std::vector<std::string> read_lines(const fs::path& p) {
	std::vector<std::string> out;
	std::ifstream in(p);
	std::string l;
	while(std::getline(in, l))
		if(!l.empty())
			out.push_back(std::move(l));
	return out;
}

std::shared_ptr<const ValueNet> net_of(const std::string& text) {
	if(text.empty())
		return nullptr;
	auto n = std::make_shared<ValueNet>();
	if(!n->parse(text))
		return nullptr;
	return n;
}

// A bot's policy from a learned one (JSON), as every search uses the self-play policy.
std::shared_ptr<const Policy> bot_policy(const CardDB& db, const std::string& text) {
	if(text.empty())
		return nullptr;
	Policy q(db);
	if(!q.from_json(text) || !q.trained())
		return nullptr;
	auto p = std::make_shared<Policy>(db);
	p->add(q, Policy::kSelfPlayScale);
	return p;
}

bool parse_example(const std::string& line, PolicyExample& e, std::string& game) {
	auto v = json::Value::parse(line);
	if(!v.is_object())
		return false;
	e = PolicyExample{};
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
		return false;
	e.chosen = static_cast<int>(std::max_element(e.target.begin(), e.target.end()) - e.target.begin());
	game = v["g"].str();
	return true;
}

std::string num(double v) {
	char b[32];
	std::snprintf(b, sizeof b, "%.4f", v);
	return b;
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

// Plays games and writes each one to disk the moment it ends (so a session cut off by a closed
// app or a sleeping computer loses only the game being played): the Main Phase positions from
// both sides (self-play's format, tools/ml/train.py and nettrain.h read them) and the searches
// of the players in `example_sides` (bit per player) as move-policy examples.
class Recorder {
public:
	Recorder(const CardDB& db, ScriptCache& scripts, fs::path positions, fs::path examples)
	    : db_(db), scripts_(scripts), features_(db), pos_path_(std::move(positions)), ex_path_(std::move(examples)) {}

	Game play(const Deck& d0, const Deck& d1, const Bot& b0, const Bot& b1, uint64_t seed, const PlayConfig& pc,
	          const std::string& gid, int example_sides) {
		Evaluator base(db_, d0, d1);
		base.set_value_net(nullptr);  // the network learns a correction to the plain evaluator
		struct P {
			std::vector<float> f;
			double raw;
			uint8_t me;
		};
		std::vector<P> pos;
		std::vector<std::string> ex;
		uint32_t last_turn = 0;
		int this_turn = 0;
		LiveDuel* current = nullptr;
		auto record = [&](LiveDuel& live) {
			Board b = live.duel().board();
			if(b.turn != last_turn) {
				last_turn = b.turn;
				this_turn = 0;
			}
			if(++this_turn > 6)
				return;  // a few per turn is enough (they are much alike)
			const uint8_t next = static_cast<uint8_t>(1 - b.turn_player);
			for(uint8_t me = 0; me < 2; ++me) {
				auto ev = base.evaluate(b, me, next);
				pos.push_back({base.features(b, me, next, ev.value), ev.raw, me});
			}
		};
		LiveDuel::SearchHook hook = [&](const Decision& d, const SearchOutput& so) {
			if(!(example_sides & (1 << d.player)) || d.minor || d.options.size() < 2 || ex.size() >= 40 || !current)
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
			o.imbue(std::locale::classic());
			o << "{\"g\":" << json::quote(gid) << ",\"o\":[";
			for(size_t i = 0; i < d.options.size(); ++i) {
				std::vector<uint32_t> f;
				features_.features(d, d.options[i], c, f);
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
			o << "]}";
			ex.push_back(o.str());
		};
		Game g = play_game(db_, scripts_, d0, d1, seed, pc, b0, b1, record, hook, &current);
		if(g.stopped)
			return g;  // cut short by Stop: its result would teach the wrong thing
		{
			std::ofstream out(pos_path_, std::ios::app | std::ios::binary);
			out.imbue(std::locale::classic());
			int idx[2] = {0, 0};
			for(const auto& p : pos) {
				out << "{\"z\":" << (p.me == 0 ? g.z0 : 1.0 - g.z0) << ",\"fin\":" << (g.finished ? 1 : 0) << ",\"raw\":" << p.raw
				    << ",\"me\":" << int(p.me) << ",\"i\":" << idx[p.me]++ << ",\"game\":" << json::quote(gid) << ",\"f\":[";
				for(size_t i = 0; i < p.f.size(); ++i)
					out << (i ? "," : "") << p.f[i];
				out << "]}\n";
			}
		}
		if(!ex.empty()) {
			std::ofstream out(ex_path_, std::ios::app | std::ios::binary);
			for(const auto& l : ex)
				out << l << "\n";
		}
		positions += static_cast<int>(pos.size());
		examples += static_cast<int>(ex.size());
		return g;
	}

	int positions = 0, examples = 0;  // written by this recorder

private:
	const CardDB& db_;
	ScriptCache& scripts_;
	Policy features_;
	fs::path pos_path_, ex_path_;
};

// A best-of-3 match: game 1 by a coin flip, then the loser chooses to go first or second, games
// 2 and 3 with both decks sided for the matchup. `on_game` after each game (false: stop).
// A training game cut for time (PlayConfig::max_seconds): its last moves, kept in the folder
// (slow_games.jsonl, the latest 20) to see what the bots kept doing.
void note_slow_game(const fs::path& dir, const std::string& gid, const Game& g) {
	if(!g.timed_out && g.last_moves.empty())
		return;
	std::ostringstream o;
	o << "{\"game\":" << json::quote(gid) << ",\"at\":" << std::time(nullptr) << ",\"timed_out\":" << (g.timed_out ? "true" : "false")
	  << ",\"turns\":" << g.turns << ",\"moves\":" << g.moves << ",\"last_moves\":[";
	for(size_t i = 0; i < g.last_moves.size(); ++i)
		o << (i ? "," : "") << json::quote(g.last_moves[i]);
	o << "]}";
	append_keep(dir / "slow_games.jsonl", {o.str()}, 20);
}

// Training's play settings shared by its sessions: a time limit per game, the search threads.
double game_seconds() {
	const char* e = std::getenv("YGOE_GAME_SECONDS");  // (tests)
	return e ? std::atof(e) : 600.0;
}

template <class F>
void play_match(const CardDB& db, Siding& siding, const std::vector<Deck>& decks, size_t a, size_t b, std::mt19937_64& rng,
                F&& play_one) {
	int wins[2] = {0, 0};
	bool a_first = rng() % 2 == 0;
	for(int game = 1; game <= 3 && wins[0] < 2 && wins[1] < 2; ++game) {
		const Deck& da = game == 1 ? decks[a] : siding.sided(a, b, a_first, decks);
		const Deck& dbk = game == 1 ? decks[b] : siding.sided(b, a, !a_first, decks);
		double za = 0.5;  // a's result
		if(!play_one(da, dbk, a_first, game, za))
			return;
		const bool a_won = za > 0.5;
		if(za != 0.5)
			++wins[a_won ? 0 : 1];
		const bool a_lost = !a_won;
		const bool loser_first = choose_first(db, a_lost ? decks[a] : decks[b], a_lost ? decks[b] : decks[a]);
		a_first = a_lost ? loser_first : !loser_first;
	}
}

}  // namespace

std::string learn_status(const std::string& dir) {
	std::string s = read_text(fs::path(dir) / "learn.json");
	return s.empty() ? "{}" : s;
}

std::string learn_deck(const CardDB& db, ScriptCache& scripts, const LearnConfig& cfg) {
	const fs::path dir(cfg.dir);
	std::error_code ec;
	fs::create_directories(dir, ec);
	if(cfg.opponents.empty())
		return "{\"error\":\"no opponents to play against\"}";
	auto stopped = [&] { return cfg.cancel && cfg.cancel->load(); };
	const auto t0 = Clock::now();
	const double session = std::max(1.0, cfg.minutes) * 60.0;
	auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };
	std::string* phase_now = nullptr;  // (pc.tick's copy of the last report, set below)
	auto report = [&](const std::string& phase) {
		if(phase_now)
			*phase_now = phase;
		if(cfg.progress)
			cfg.progress(phase, static_cast<int>(std::min(elapsed(), session)), static_cast<int>(session));
	};

	// the deck's current network and policy: its own, or the format's to start from
	const fs::path net_path = dir / "value_net.json", pol_path = dir / "policy.json";
	const fs::path pos_path = dir / "positions.jsonl", ex_path = dir / "policy_examples.jsonl";
	std::string net_json = read_text(net_path), pol_json = read_text(pol_path);
	if(net_json.empty())
		net_json = cfg.base_net;
	if(pol_json.empty())
		pol_json = cfg.base_policy;

	std::shared_ptr<const PieceGuide> guide;
	if(!cfg.library.empty())
		guide = std::make_shared<PieceGuide>(cfg.library, &db);
	const Bot opp_bot{db.value_net(), bot_policy(db, cfg.base_policy), nullptr, db.nonengine_timing()};
	std::vector<Deck> decks{cfg.deck};
	for(const auto& o : cfg.opponents)
		decks.push_back(o.deck);
	Siding siding(db);
	std::mt19937_64 rng(cfg.seed ^ static_cast<uint64_t>(std::time(nullptr)));

	PlayConfig pc;
	pc.iterations = cfg.iterations;
	pc.max_turns = cfg.turns;
	pc.explore = 0.25;
	pc.explore_moves = 2;
	pc.stop = cfg.cancel;
	pc.max_seconds = game_seconds();
	pc.threads = training_threads();
	// the time shown keeps moving during a long game (the last report again, every 5 seconds)
	std::string last_phase = "Starting";
	phase_now = &last_phase;
	auto last_tick = Clock::now();
	pc.tick = [&] {
		if(std::chrono::duration<double>(Clock::now() - last_tick).count() < 5)
			return;
		last_tick = Clock::now();
		if(cfg.progress)
			cfg.progress(last_phase, static_cast<int>(std::min(elapsed(), session)), static_cast<int>(session));
	};

	// the rounds so far (one report per line), and the games played in them
	const fs::path rounds_path = dir / "rounds.jsonl";
	std::vector<std::string> rounds = read_lines(rounds_path);
	long total_games = json::Value::parse(learn_status(cfg.dir))["games"].integer(0);

	int round_no = static_cast<int>(rounds.size());
	int session_games = 0, kept_nets = 0, kept_policies = 0;
	std::string last_round = "null";
	bool first_round = true;
	while(!stopped() && (first_round || session - elapsed() > 120)) {
		first_round = false;
		++round_no;
		const double round_left = std::min(session - elapsed(), 40.0 * 60.0);
		const double play_until = elapsed() + round_left * 0.7;
		const double round_until = elapsed() + round_left;
		const Bot deck_bot{net_of(net_json), bot_policy(db, pol_json), guide, db.nonengine_timing()};

		// 1. self-play matches, each game on disk as soon as it ends
		Recorder rec(db, scripts, pos_path, ex_path);
		int games = 0, wins = 0, finished = 0;
		while(!stopped() && elapsed() < play_until) {
			const size_t opp = 1 + rng() % cfg.opponents.size();
			const std::string match = std::to_string(std::time(nullptr)) + "-" + std::to_string(rng() % 100000);
			play_match(db, siding, decks, 0, opp, rng, [&](const Deck& dd, const Deck& od, bool deck_first, int game, double& zd) {
				if(stopped())
					return false;
				const std::string gid = match + "-g" + std::to_string(game);
				Game g = deck_first ? rec.play(dd, od, deck_bot, opp_bot, rng(), pc, gid, 1)
				                    : rec.play(od, dd, opp_bot, deck_bot, rng(), pc, gid, 2);
				if(g.stopped)
					return false;
				note_slow_game(dir, gid, g);
				++games;
				finished += g.finished ? 1 : 0;
				zd = deck_first ? g.z0 : 1.0 - g.z0;
				wins += zd > 0.5 ? 1 : 0;
				report("Round " + std::to_string(round_no) + ": playing (" + std::to_string(games) + " games, deck won " +
				       std::to_string(wins) + ")");
				return !stopped() && elapsed() < play_until;
			});
		}
		session_games += games;
		total_games += games;
		append_keep(pos_path, {}, 200000);  // the latest positions only
		append_keep(ex_path, {}, 60000);
		if(stopped() && games == 0)
			break;

		// 2. training: the network...
		report("Round " + std::to_string(round_no) + ": training the network");
		std::vector<NetSample> samples;
		for(const auto& l : read_lines(pos_path)) {
			NetSample s;
			if(parse_net_sample(l, s))
				samples.push_back(std::move(s));
		}
		NetTrainConfig tc;
		tc.epochs = 40;
		tc.seed = rng();
		tc.stop = stopped;
		NetTrainResult nr = train_value_net(samples, net_json, tc, cfg.name + " round " + std::to_string(round_no));
		const bool net_passed = nr.ok && nr.net_logloss < nr.init_logloss - 0.002;
		// ...and the move policy (the deck's bot's searches)
		report("Round " + std::to_string(round_no) + ": training the move policy");
		std::vector<PolicyExample> tr, va;
		std::hash<std::string> h;
		for(const auto& l : read_lines(ex_path)) {
			PolicyExample e;
			std::string g;
			if(parse_example(l, e, g))
				(h(g) % 100 < 15 ? va : tr).push_back(std::move(e));
		}
		Policy cand(db);
		if(!pol_json.empty())
			cand.from_json(pol_json);
		double pol_before = 0, pol_after = 0;
		bool pol_passed = false;
		if(tr.size() >= 50 && va.size() >= 10 && !stopped()) {
			pol_before = cand.loss_of(va);
			cand.train(tr, 4, 0.05);
			pol_after = cand.loss_of(va);
			pol_passed = pol_after < pol_before - 0.002;
		}

		// 3. arena: what passed against what the deck has now, on the same deals
		double score = 0.5;
		int pairs = 0;
		bool kept = false;
		if((net_passed || pol_passed) && !stopped()) {
			const std::string new_net = net_passed ? nr.net_json : net_json;
			const std::string new_pol = pol_passed ? cand.to_json() : pol_json;
			const Bot nb{net_of(new_net), bot_policy(db, new_pol), guide, db.nonengine_timing()};
			double sum = 0;
			PlayConfig apc = pc;
			apc.explore = 0;
			// at least 2 pairs, however long it takes; then until the round's time is up
			while(!stopped() && (pairs < 2 || elapsed() < round_until)) {
				const size_t opp = 1 + rng() % cfg.opponents.size();
				const bool deck_first = pairs % 2 == 0;
				const uint64_t seed = rng();
				auto z = [&](const Bot& b) {
					Game g = deck_first ? play_game(db, scripts, decks[0], decks[opp], seed, apc, b, opp_bot, {})
					                    : play_game(db, scripts, decks[opp], decks[0], seed, apc, opp_bot, b, {});
					return std::make_pair(deck_first ? g.z0 : 1.0 - g.z0, g.stopped);
				};
				auto zn = z(nb);
				if(zn.second)
					break;
				auto zo = z(deck_bot);
				if(zo.second)
					break;
				sum += (zn.first - zo.first + 1) / 2;
				++pairs;
				score = sum / pairs;
				report("Round " + std::to_string(round_no) + ": testing what it learned (" + std::to_string(pairs) +
				       " deals, score " + num(score).substr(0, 4) + ")");
			}
			kept = pairs >= 2 && score >= 0.5;
			if(kept) {
				if(net_passed) {
					net_json = new_net;
					write_text(net_path, net_json);
					++kept_nets;
				}
				if(pol_passed) {
					pol_json = new_pol;
					write_text(pol_path, pol_json);
					++kept_policies;
				}
			}
		}

		std::ostringstream r;
		r.imbue(std::locale::classic());
		r << "{\"round\":" << round_no << ",\"at\":" << std::time(nullptr) << ",\"games\":" << games << ",\"deck_wins\":" << wins
		  << ",\"finished\":" << finished << ",\"positions\":" << rec.positions << ",\"policy_examples\":" << rec.examples
		  << ",\"net\":{\"trained\":" << (nr.ok ? "true" : "false") << ",\"base_logloss\":" << num(nr.base_logloss)
		  << ",\"start_logloss\":" << num(nr.init_logloss) << ",\"logloss\":" << num(nr.net_logloss)
		  << ",\"passed\":" << (net_passed ? "true" : "false") << "},\"policy\":{\"examples\":" << tr.size() + va.size()
		  << ",\"start_logloss\":" << num(pol_before) << ",\"logloss\":" << num(pol_after) << ",\"passed\":" << (pol_passed ? "true" : "false")
		  << "},\"arena\":{\"pairs\":" << pairs << ",\"score\":" << num(score) << "},\"kept\":" << (kept ? "true" : "false") << "}";
		last_round = r.str();
		rounds.push_back(last_round);
		if(rounds.size() > 100)
			rounds.erase(rounds.begin());
		append_keep(rounds_path, {last_round}, 100);
		std::ostringstream st;
		st << "{\"name\":" << json::quote(cfg.name) << ",\"games\":" << total_games << ",\"updated\":" << std::time(nullptr)
		   << ",\"has_net\":" << (fs::exists(net_path) ? "true" : "false") << ",\"has_policy\":" << (fs::exists(pol_path) ? "true" : "false")
		   << ",\"rounds\":[";
		for(size_t i = 0; i < rounds.size(); ++i)
			st << (i ? "," : "") << rounds[i];
		st << "]}";
		write_text(dir / "learn.json", st.str());
	}
	report("idle");
	std::ostringstream out;
	out << "{\"games\":" << session_games << ",\"total_games\":" << total_games << ",\"rounds\":" << round_no
	    << ",\"kept_networks\":" << kept_nets << ",\"kept_policies\":" << kept_policies << ",\"cancelled\":" << (stopped() ? "true" : "false")
	    << ",\"last_round\":" << last_round << ",\"status\":" << learn_status(cfg.dir) << "}";
	return out.str();
}

namespace {

// A counter file ({"key": n, ...}) in a session's folder, read and written whole.
long counter(const fs::path& p, const char* key) { return json::Value::parse(read_text(p))[key].integer(0); }

}  // namespace

std::string contribute_selfplay(const CardDB& db, ScriptCache& scripts, const ContributeConfig& cfg) {
	const fs::path dir(cfg.dir);
	std::error_code ec;
	fs::create_directories(dir, ec);
	if(cfg.decks.size() < 2)
		return "{\"error\":\"the format's decks are missing (update the engine)\"}";
	auto stopped = [&] { return cfg.cancel && cfg.cancel->load(); };
	const auto t0 = Clock::now();
	const double session = std::max(1.0, cfg.minutes) * 60.0;
	auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };

	std::vector<Deck> decks;
	std::vector<std::shared_ptr<const PieceGuide>> guides;
	for(const auto& d : cfg.decks) {
		decks.push_back(d.deck);
		auto lib = playable_by(db, d.deck, cfg.pool);
		sort_library(lib, 400);
		guides.push_back(lib.empty() ? nullptr : std::make_shared<PieceGuide>(std::move(lib), &db));
	}
	auto policy = bot_policy(db, cfg.base_policy);
	PlayConfig pc;
	pc.iterations = cfg.iterations;
	pc.max_turns = cfg.turns;
	pc.explore = 0.25;
	pc.explore_moves = 2;
	pc.stop = cfg.cancel;
	pc.max_seconds = game_seconds();
	pc.threads = training_threads();
	std::string last_phase = "Playing the format's games";
	auto last_tick = Clock::now();
	pc.tick = [&] {
		if(std::chrono::duration<double>(Clock::now() - last_tick).count() < 5)
			return;
		last_tick = Clock::now();
		if(cfg.progress)
			cfg.progress(last_phase, static_cast<int>(std::min(elapsed(), session)), static_cast<int>(session));
	};
	Siding siding(db);
	std::mt19937_64 rng(cfg.seed ^ static_cast<uint64_t>(std::time(nullptr)));
	Recorder rec(db, scripts, dir / "positions.jsonl", dir / "policy_examples.jsonl");
	const fs::path status = dir / "status.json";
	long total = counter(status, "games");
	int games = 0;
	while(!stopped() && elapsed() < session) {
		// the nightly run's pairings: two of the format's decks at random (a mirror now and then)
		const size_t a = rng() % decks.size(), b = rng() % decks.size();
		const std::string match = "pc-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(rng() % 100000);
		play_match(db, siding, decks, a, b, rng, [&](const Deck& da, const Deck& dbk, bool a_first, int game, double& za) {
			if(stopped())
				return false;
			const Bot ba{db.value_net(), policy, guides[a], db.nonengine_timing()}, bb{db.value_net(), policy, guides[b], db.nonengine_timing()};
			const std::string gid = match + "-g" + std::to_string(game);
			Game g = a_first ? rec.play(da, dbk, ba, bb, rng(), pc, gid, 3) : rec.play(dbk, da, bb, ba, rng(), pc, gid, 3);
			if(g.stopped)
				return false;
			note_slow_game(dir, gid, g);
			++games;
			++total;
			write_text(status, "{\"games\":" + std::to_string(total) + ",\"updated\":" + std::to_string(std::time(nullptr)) + "}");
			za = a_first ? g.z0 : 1.0 - g.z0;
			last_phase = "Playing the format's games (" + std::to_string(games) + " this session, " + std::to_string(total) + " in all)";
			if(cfg.progress)
				cfg.progress(last_phase, static_cast<int>(std::min(elapsed(), session)), static_cast<int>(session));
			return !stopped() && elapsed() < session;
		});
	}
	// the folder keeps the latest games (the app uploads them as they come)
	append_keep(dir / "positions.jsonl", {}, 200000);
	append_keep(dir / "policy_examples.jsonl", {}, 60000);
	if(cfg.progress)
		cfg.progress("idle", 0, 0);
	return "{\"games\":" + std::to_string(games) + ",\"total_games\":" + std::to_string(total) + ",\"positions\":" +
	       std::to_string(rec.positions) + ",\"policy_examples\":" + std::to_string(rec.examples) + ",\"cancelled\":" +
	       (stopped() ? "true" : "false") + "}";
}

std::string deepen_lines(const CardDB& db, ScriptCache& scripts, const DeepenConfig& cfg) {
	const fs::path dir(cfg.dir);
	std::error_code ec;
	fs::create_directories(dir / "newpieces", ec);
	if(cfg.decks.empty())
		return "{\"error\":\"no decks to search\"}";
	auto stopped = [&] { return cfg.cancel && cfg.cancel->load(); };
	const auto t0 = Clock::now();
	const double session = std::max(1.0, cfg.minutes) * 60.0;
	auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - t0).count(); };
	const fs::path status = dir / "status.json";
	long total_hands = counter(status, "hands"), total_pieces = counter(status, "pieces");
	std::mt19937_64 rng(cfg.seed ^ static_cast<uint64_t>(std::time(nullptr)));
	int batches = 0, hands = 0, pieces = 0;
	size_t next = rng() % cfg.decks.size();
	while(!stopped() && elapsed() < session) {
		// one batch: a few opening hands of one deck, searched deeply, saved when done
		const auto& d = cfg.decks[next];
		next = (next + 1) % cfg.decks.size();
		auto res = build_reference(db, scripts, d.deck, cfg.hands_per_batch, cfg.iterations, rng(), cfg.cancel, [&](int done, int total) {
			if(cfg.progress)
				cfg.progress("Searching " + d.name + ": hand " + std::to_string(done + 1) + " of " + std::to_string(total) + " (" +
				             std::to_string(hands) + " hands this session)",
				             static_cast<int>(std::min(elapsed(), session)), static_cast<int>(session));
		});
		if(stopped())
			break;  // a batch cut short: its lines may be the worse half of the hands
		const int lines = static_cast<int>(res.samples.size());
		if(!res.library.empty()) {
			const std::string name = std::to_string(std::time(nullptr)) + "-" + std::to_string(rng() % 100000) + ".json";
			write_text(dir / "newpieces" / name,
			           "{\"lines\":" + std::to_string(lines) + ",\"pieces\":" + library_json(db, res.library, lines) + "}");
		}
		++batches;
		hands += cfg.hands_per_batch;
		pieces += static_cast<int>(res.library.size());
		total_hands += cfg.hands_per_batch;
		total_pieces += static_cast<long>(res.library.size());
		write_text(status, "{\"hands\":" + std::to_string(total_hands) + ",\"pieces\":" + std::to_string(total_pieces) +
		                       ",\"updated\":" + std::to_string(std::time(nullptr)) + "}");
	}
	if(cfg.progress)
		cfg.progress("idle", 0, 0);
	return "{\"batches\":" + std::to_string(batches) + ",\"hands\":" + std::to_string(hands) + ",\"pieces\":" + std::to_string(pieces) +
	       ",\"total_hands\":" + std::to_string(total_hands) + ",\"cancelled\":" + (stopped() ? "true" : "false") + "}";
}

}  // namespace ygoe
