#include "search.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "ocgapi_constants.h"

namespace ygoe {

namespace {

struct Node;

struct Child {
	std::string key;
	std::string label;
	Node* node = nullptr;
	double W = 0;
	int N = 0;
	int avail = 0;
	bool bad = false;  // the core rejected this answer
};

struct Node {
	uint8_t player = 0;
	int N = 0;
	std::vector<Child> children;
	Child* find(const std::string& key) {
		for(auto& c : children)
			if(c.key == key)
				return &c;
		return nullptr;
	}
};

std::string key_of(const Option& o) {
	return o.label + "|" + std::to_string(static_cast<int>(o.kind)) + "|" + std::to_string(o.loc);
}

// Prior weights for the playout policy: favour developing plays on your own turn and
// holding responses unless they are clearly interrupts.
double policy_weight(const Decision& d, const Option& o, uint8_t turn_player, int steps_this_turn) {
	const bool own_turn = d.player == turn_player;
	switch(d.type) {
	case DecisionType::Idle:
		switch(o.kind) {
		case OptKind::Activate: return 5.0;
		case OptKind::SpSummon: return 4.0;
		case OptKind::Summon: return 3.0;
		case OptKind::SSet: return 2.0;
		case OptKind::MSet: return 0.3;
		case OptKind::Reposition: return 0.05;
		case OptKind::ToBattle: return 1.0;
		case OptKind::ToEnd: return 0.4 + 0.03 * steps_this_turn;
		default: return 1.0;
		}
	case DecisionType::Battle:
		switch(o.kind) {
		case OptKind::Attack: return 3.0;
		case OptKind::Activate: return 1.0;
		default: return 0.6;
		}
	case DecisionType::Chain:
		if(o.kind == OptKind::Pass)
			return own_turn ? 1.0 : 3.0;
		return own_turn ? 2.0 : 1.0;
	case DecisionType::EffectYesNo:
	case DecisionType::YesNo:
		return o.kind == OptKind::Yes ? 3.0 : 1.0;
	default:
		return 1.0;
	}
}

size_t weighted_pick(const std::vector<double>& w, std::mt19937_64& rng) {
	double total = 0;
	for(double x : w)
		total += x;
	if(total <= 0)
		return 0;
	std::uniform_real_distribution<double> u(0, total);
	double r = u(rng);
	for(size_t i = 0; i < w.size(); ++i) {
		r -= w[i];
		if(r <= 0)
			return i;
	}
	return w.size() - 1;
}

}  // namespace

Searcher::Searcher(const CardDB& db, ScriptCache& scripts, const Evaluator& eval, std::vector<uint32_t> pool)
    : db_(db), scripts_(scripts), eval_(eval), pool_(std::move(pool)) {}

bool Searcher::final_board(const RootState& root, const Trajectory& t, Board& out, Duel** keep) {
	auto* d = new Duel(db_, scripts_);
	d->set_declare_pool(pool_);
	bool ok = d->replay(root.setup, root.history);
	for(size_t i = 0; ok && i < t.responses.size(); ++i)
		ok = d->respond(t.responses[i]);
	if(ok)
		out = d->board();
	if(keep && ok)
		*keep = d;
	else
		delete d;
	return ok;
}

SearchOutput Searcher::run(const std::vector<RootState>& roots, uint8_t me, const SearchConfig& cfg,
                           const std::function<void(int, double)>& progress) {
	SearchOutput out;
	if(roots.empty())
		return out;
	std::deque<Node> nodes;
	std::mutex mu;
	nodes.emplace_back();
	Node* root = &nodes.back();
	std::vector<std::vector<Trajectory>> best_by_move;  // indexed like root->children
	struct Group {
		int count = 0;
		double sum = 0;
		std::vector<Trajectory> best;
	};
	std::map<std::string, Group> groups;  // by the searching player's first action
	auto keep_best = [&](std::vector<Trajectory>& list, const Trajectory& t) {
		if(static_cast<int>(list.size()) >= cfg.keep_trajectories && list.back().reward >= t.reward)
			return;
		list.push_back(t);
		std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.reward > b.reward; });
		if(list.size() > static_cast<size_t>(cfg.keep_trajectories))
			list.resize(cfg.keep_trajectories);
	};

	std::atomic<int> next_iteration{0};
	std::atomic<int> done{0};
	std::atomic<int> failures{0};
	const auto t0 = std::chrono::steady_clock::now();
	auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
	bool root_info_set = false;

	auto worker = [&](unsigned tid) {
		std::mt19937_64 rng(0xC0FFEEull * (tid + 1) + roots.size());
		Duel duel(db_, scripts_);
		duel.set_declare_pool(pool_);
		while(true) {
			int it = next_iteration.fetch_add(1);
			if(it >= cfg.iterations || elapsed() > cfg.seconds || (cfg.cancel && cfg.cancel->load()))
				break;
			size_t ri = static_cast<size_t>(it) % roots.size();
			if(!duel.replay(roots[ri].setup, roots[ri].history)) {
				++failures;
				continue;
			}
			const uint32_t root_turn = duel.turn();
			uint32_t turn_seen = duel.turn();
			int steps_this_turn = 0;
			Trajectory traj;
			traj.root = ri;
			std::vector<std::pair<Node*, size_t>> path;
			Node* node = root;
			bool in_tree = true;
			int steps = 0;
			{
				std::lock_guard<std::mutex> lock(mu);
				if(!root_info_set) {
					out.root_player = duel.decision().player;
					out.root_type = duel.decision().type;
					root_info_set = true;
				}
			}
			while(!duel.ended() && duel.turn() < root_turn + static_cast<uint32_t>(cfg.horizon_turns) &&
			      steps < cfg.max_rollout_steps) {
				if(duel.turn() != turn_seen) {
					turn_seen = duel.turn();
					steps_this_turn = 0;
				}
				const Decision dec = duel.decision();
				if(dec.options.empty())
					break;
				std::vector<char> tried(dec.options.size(), 0);
				bool responded = false;
				while(!responded) {
					size_t idx = 0;
					Node* parent = nullptr;
					size_t chosen_index = 0;
					if(dec.minor || dec.options.size() == 1) {
						idx = 0;
						while(idx < tried.size() && tried[idx])
							++idx;
					} else if(in_tree) {
						std::lock_guard<std::mutex> lock(mu);
						node->player = dec.player;
						std::vector<size_t> present;  // child index for each option, or npos
						present.reserve(dec.options.size());
						for(size_t i = 0; i < dec.options.size(); ++i) {
							auto key = key_of(dec.options[i]);
							Child* c = node->find(key);
							if(!c) {
								node->children.push_back({key, dec.options[i].label});
								c = &node->children.back();
							}
							present.push_back(static_cast<size_t>(c - node->children.data()));
						}
						// Availability counts (ISMCTS): once per visit to this decision.
						if(std::all_of(tried.begin(), tried.end(), [](char t) { return !t; }))
							for(size_t i = 0; i < present.size(); ++i)
								if(std::find(present.begin(), present.begin() + i, present[i]) == present.begin() + i)
									++node->children[present[i]].avail;
						std::vector<size_t> unvisited;
						std::vector<double> uw;
						double best = -std::numeric_limits<double>::infinity();
						size_t best_i = SIZE_MAX;
						for(size_t i = 0; i < present.size(); ++i) {
							Child& c = node->children[present[i]];
							if(tried[i] || c.bad)
								continue;
							if(c.N == 0) {
								unvisited.push_back(i);
								uw.push_back(policy_weight(dec, dec.options[i], duel.turn_player(), steps_this_turn));
								continue;
							}
							double q = c.W / c.N;
							if(dec.player != me)
								q = 1.0 - q;
							double u = q + cfg.exploration * std::sqrt(std::log(std::max(1, c.avail)) / c.N);
							if(u > best) {
								best = u;
								best_i = i;
							}
						}
						if(!unvisited.empty()) {
							idx = unvisited[weighted_pick(uw, rng)];
						} else if(best_i != SIZE_MAX) {
							idx = best_i;
						} else {
							idx = SIZE_MAX;
						}
						if(idx != SIZE_MAX) {
							chosen_index = present[idx];
							parent = node;
						}
					} else {
						std::vector<double> w(dec.options.size());
						for(size_t i = 0; i < w.size(); ++i)
							w[i] = tried[i] ? 0 : policy_weight(dec, dec.options[i], duel.turn_player(), steps_this_turn);
						idx = weighted_pick(w, rng);
						if(tried[idx])
							idx = SIZE_MAX;
					}
					if(idx == SIZE_MAX || idx >= dec.options.size())
						break;  // nothing legal left
					tried[idx] = 1;
					const Option& opt = dec.options[idx];
					if(duel.respond(opt.response)) {
						responded = true;
						traj.responses.push_back(opt.response);
						traj.steps.push_back({dec.player, dec.type, opt.label, dec.minor});
						if(parent) {
							std::lock_guard<std::mutex> lock(mu);
							Child& c = parent->children[chosen_index];
							path.emplace_back(parent, chosen_index);
							bool expand = c.N == 0;
							if(!c.node) {
								nodes.emplace_back();
								c.node = &nodes.back();
							}
							node = c.node;
							if(expand)
								in_tree = false;
						}
					} else if(parent) {
						std::lock_guard<std::mutex> lock(mu);
						parent->children[chosen_index].bad = true;
					}
				}
				if(!responded)
					break;
				++steps;
				++steps_this_turn;
			}
			// Who acts next decides whose interruptions count.
			uint8_t next = duel.turn() >= root_turn + static_cast<uint32_t>(cfg.horizon_turns)
			                   ? duel.turn_player()
			                   : static_cast<uint8_t>(1 - duel.turn_player());
			Evaluation ev = eval_.evaluate(duel.board(), me, next, duel.ended(), duel.winner());
			traj.reward = ev.score;
			{
				std::lock_guard<std::mutex> lock(mu);
				root->N++;
				for(auto& [n, ci] : path) {
					n->N++;
					n->children[ci].N++;
					n->children[ci].W += ev.score;
				}
				Group& g = groups[first_action(traj, me)];
				++g.count;
				g.sum += ev.score;
				keep_best(g.best, traj);
				if(!path.empty() && path[0].first == root) {
					size_t m = path[0].second;
					if(best_by_move.size() < root->children.size())
						best_by_move.resize(root->children.size());
					keep_best(best_by_move[m], traj);
				}
			}
			int d = ++done;
			if(progress && tid == 0 && d % 25 == 0)
				progress(d, elapsed());
		}
	};

	unsigned n = cfg.threads > 0 ? static_cast<unsigned>(cfg.threads) : std::max(1u, std::thread::hardware_concurrency());
	// Warm the script cache on one thread first so the others hit memory.
	{
		Duel warm(db_, scripts_);
		warm.replay(roots[0].setup, roots[0].history);
	}
	std::vector<std::thread> pool;
	for(unsigned t = 0; t < n; ++t)
		pool.emplace_back(worker, t);
	for(auto& t : pool)
		t.join();

	out.iterations = done.load();
	out.failures = failures.load();
	out.seconds = elapsed();
	best_by_move.resize(root->children.size());
	for(size_t i = 0; i < root->children.size(); ++i) {
		const Child& c = root->children[i];
		if(c.N == 0)
			continue;
		MoveStats ms;
		ms.label = c.label;
		ms.visits = c.N;
		ms.mean = c.W / c.N;
		ms.best = best_by_move[i];
		out.moves.push_back(std::move(ms));
	}
	std::sort(out.moves.begin(), out.moves.end(), [](auto& a, auto& b) { return a.visits > b.visits; });
	for(auto& [label, g] : groups) {
		MoveStats ms;
		ms.label = label;
		ms.visits = g.count;
		ms.mean = g.sum / g.count;
		ms.best = std::move(g.best);
		out.my_moves.push_back(std::move(ms));
	}
	std::sort(out.my_moves.begin(), out.my_moves.end(), [](auto& a, auto& b) { return a.visits > b.visits; });
	return out;
}

namespace {
bool is_pass_label(const std::string& l) {
	return l == "Don't respond" || l.rfind("Don't use", 0) == 0 || l.rfind("No:", 0) == 0 || l == "Done selecting" ||
	       l == "Cancel";
}
}  // namespace

std::string first_action(const Trajectory& t, uint8_t player) {
	for(const auto& s : t.steps) {
		if(s.player != player || s.minor || is_pass_label(s.label))
			continue;
		if(s.type == DecisionType::Idle || s.type == DecisionType::Chain || s.type == DecisionType::Battle ||
		   s.type == DecisionType::EffectYesNo)
			return s.label;
	}
	return "Pass";
}

std::vector<std::string> describe(const Trajectory& t, uint8_t player, bool include_other, const std::string& other_prefix) {
	std::vector<std::string> lines;
	bool last_mine = false;
	for(const auto& s : t.steps) {
		if(s.minor)
			continue;
		bool mine = s.player == player;
		if(!mine && !include_other)
			continue;
		if(is_pass_label(s.label))
			continue;
		bool starts = s.type == DecisionType::Idle || s.type == DecisionType::Chain || s.type == DecisionType::Battle ||
		              s.type == DecisionType::EffectYesNo;
		std::string text = s.label;
		if(s.type == DecisionType::YesNo && text.rfind("Yes: ", 0) == 0)
			text = text.substr(5);
		if(starts || lines.empty() || mine != last_mine) {
			lines.push_back((mine ? "" : other_prefix) + text);
		} else {
			lines.back() += "  →  " + text;
		}
		last_mine = mine;
	}
	return lines;
}

}  // namespace ygoe
