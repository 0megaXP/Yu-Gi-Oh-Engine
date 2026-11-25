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
	auto report = [&](const std::string& phase) {
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
	Policy features_of(db);
	std::mt19937_64 rng(cfg.seed ^ static_cast<uint64_t>(std::time(nullptr)));
	std::uniform_real_distribution<double> unit(0, 1);

	PlayConfig pc;
	pc.iterations = cfg.iterations;
	pc.max_turns = cfg.turns;
	pc.explore = 0.25;
	pc.explore_moves = 2;
	pc.stop = cfg.cancel;

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

		// 1. self-play matches
		std::vector<std::string> pos_lines, ex_lines;
		int games = 0, wins = 0, finished = 0;
		std::string game_id;
		int game_examples = 0;
		uint8_t deck_side = 0;
		LiveDuel* current = nullptr;
		LiveDuel::SearchHook hook = [&](const Decision& d, const SearchOutput& so) {
			if(d.player != deck_side || d.minor || d.options.size() < 2 || game_examples >= 40 || !current)
				return;
			int total = 0;
			for(const auto& m : so.moves)
				total += m.visits;
			if(total < 40 || so.moves.front().visits > total * 0.97)
				return;
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
			o << "]}";
			ex_lines.push_back(o.str());
			++game_examples;
		};
		auto one = [&](const Deck& d0, const Deck& d1, const Bot& b0, const Bot& b1, uint64_t seed, const std::string& gid) {
			Evaluator base(db, d0, d1);
			base.set_value_net(nullptr);  // the network learns a correction to the plain evaluator
			struct P {
				std::vector<float> f;
				double raw;
				uint8_t me;
			};
			std::vector<P> pos;
			uint32_t last_turn = 0;
			int this_turn = 0;
			game_id = gid;
			game_examples = 0;
			auto record = [&](LiveDuel& live) {
				Board b = live.duel().board();
				if(b.turn != last_turn) {
					last_turn = b.turn;
					this_turn = 0;
				}
				if(++this_turn > 6)
					return;
				const uint8_t next = static_cast<uint8_t>(1 - b.turn_player);
				for(uint8_t me = 0; me < 2; ++me) {
					auto ev = base.evaluate(b, me, next);
					pos.push_back({base.features(b, me, next, ev.value), ev.raw, me});
				}
			};
			Game g = play_game(db, scripts, d0, d1, seed, pc, b0, b1, record, hook, &current);
			if(g.stopped)
				return g;
			int idx[2] = {0, 0};
			for(const auto& p : pos) {
				std::ostringstream o;
				o.imbue(std::locale::classic());
				o << "{\"z\":" << (p.me == 0 ? g.z0 : 1.0 - g.z0) << ",\"fin\":" << (g.finished ? 1 : 0) << ",\"raw\":" << p.raw
				  << ",\"me\":" << int(p.me) << ",\"i\":" << idx[p.me]++ << ",\"game\":" << json::quote(gid) << ",\"f\":[";
				for(size_t i = 0; i < p.f.size(); ++i)
					o << (i ? "," : "") << p.f[i];
				o << "]}";
				pos_lines.push_back(o.str());
			}
			return g;
		};
		while(!stopped() && elapsed() < play_until) {
			const size_t opp = 1 + rng() % cfg.opponents.size();
			const std::string match = std::to_string(std::time(nullptr)) + "-" + std::to_string(rng() % 100000);
			int mw[2] = {0, 0};  // match wins: deck, opponent
			bool deck_first = rng() % 2 == 0;
			for(int game = 1; game <= 3 && mw[0] < 2 && mw[1] < 2 && !stopped(); ++game) {
				const Deck& dd = game == 1 ? decks[0] : siding.sided(0, opp, deck_first, decks);
				const Deck& od = game == 1 ? decks[opp] : siding.sided(opp, 0, !deck_first, decks);
				deck_side = deck_first ? 0 : 1;
				const uint64_t seed = rng();
				Game g = deck_first ? one(dd, od, deck_bot, opp_bot, seed, match + "-g" + std::to_string(game))
				                    : one(od, dd, opp_bot, deck_bot, seed, match + "-g" + std::to_string(game));
				if(g.stopped)
					break;
				++games;
				finished += g.finished ? 1 : 0;
				const double zd = deck_first ? g.z0 : 1.0 - g.z0;
				const bool deck_won = zd > 0.5;
				if(zd != 0.5)
					++mw[deck_won ? 0 : 1];
				wins += deck_won ? 1 : 0;
				// the loser chooses to go first or second
				const bool deck_lost = !deck_won;
				const bool loser_first = choose_first(db, deck_lost ? decks[0] : decks[opp], deck_lost ? decks[opp] : decks[0]);
				deck_first = deck_lost ? loser_first : !loser_first;
				report("Round " + std::to_string(round_no) + ": playing (" + std::to_string(games) + " games, deck won " +
				       std::to_string(wins) + ")");
			}
		}
		session_games += games;
		total_games += games;
		if(!pos_lines.empty())
			append_keep(pos_path, pos_lines, 200000);
		if(!ex_lines.empty())
			append_keep(ex_path, ex_lines, 60000);
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
		  << ",\"finished\":" << finished << ",\"positions\":" << pos_lines.size() << ",\"policy_examples\":" << ex_lines.size()
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

}  // namespace ygoe
