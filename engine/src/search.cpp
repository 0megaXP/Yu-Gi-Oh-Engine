#include "search.h"
#include "policy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
#include "pieces.h"

namespace ygoe {

namespace {

struct Node;

struct Child {
	std::string key;
	std::string label;
	Node* node = nullptr;
	double W = 0;
	double hi = 0, lo = 1;  // best and worst reward seen through this child
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
//
// Setting cards is held back while anything else can still be played: a card set early can
// be destroyed or banished during your own combo, and it takes a Spell/Trap Zone that a
// later effect may need (Angelechy places Extra Deck monsters there, Pendulum decks need the
// scales, Field Spells, Equip Spells...). Once only sets are left, they are played normally.
double policy_weight(const Decision& d, const Option& o, uint8_t turn_player, int steps_this_turn) {
	const bool own_turn = d.player == turn_player;
	switch(d.type) {
	case DecisionType::Idle: {
		bool developing = false, can_set = false;
		for(const auto& other : d.options) {
			if(other.kind == OptKind::Activate || other.kind == OptKind::SpSummon || other.kind == OptKind::Summon)
				developing = true;
			if(other.kind == OptKind::SSet)
				can_set = true;
		}
		switch(o.kind) {
		case OptKind::Activate: return 5.0;
		case OptKind::SpSummon: return 4.0;
		case OptKind::Summon: return 3.0;
		case OptKind::SSet: return developing ? 0.05 : 2.0;
		case OptKind::MSet: return developing ? 0.03 : 0.3;
		case OptKind::Reposition: return 0.05;
		case OptKind::ToBattle: return developing ? 0.15 : 1.0;  // combo in Main Phase 1, then attack
		// Before ending the turn, set what can be set (ending is still possible, just tried later).
		case OptKind::ToEnd: return can_set ? 0.02 : developing ? 0.3 + 0.03 * steps_this_turn : 1.0;
		default: return 1.0;
		}
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

double playout_weight(const Decision& d, const Option& o, uint8_t turn_player, int steps_this_turn) {
	return policy_weight(d, o, turn_player, steps_this_turn);
}

Searcher::Searcher(const CardDB& db, ScriptCache& scripts, const Evaluator& eval, std::vector<uint32_t> pool)
    : db_(db), scripts_(scripts), eval_(eval), pool_(std::move(pool)) {}

bool Searcher::defer_sets(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns) {
	auto is_set = [&](size_t i) {
		const Step& s = t.steps[i];
		return s.player == player && s.type == DecisionType::Idle && s.label.rfind("Set ", 0) == 0;
	};
	auto is_phase_end = [&](size_t i) {
		const Step& s = t.steps[i];
		return s.player == player && s.type == DecisionType::Idle &&
		       (s.label == "Go to End Phase" || s.label == "Go to Battle Phase");
	};
	// A Battle Phase that only passes straight to Main Phase 2 (no attack, no effect) is a
	// detour: drop it so the turn reads Main Phase 1 → plays → end.
	std::vector<char> drop(t.steps.size(), 0);
	bool dropped = false;
	for(size_t i = 0; i < t.steps.size(); ++i) {
		const Step& s = t.steps[i];
		if(s.player != player || s.type != DecisionType::Idle || s.label != "Go to Battle Phase")
			continue;
		size_t j = i + 1;
		bool idle = true;
		while(j < t.steps.size() && !(t.steps[j].player == player && t.steps[j].type == DecisionType::Battle)) {
			if(t.steps[j].type == DecisionType::Idle || (t.steps[j].label != "Don't respond" && !t.steps[j].minor))
				idle = false;
			++j;
		}
		if(!idle || j >= t.steps.size() || t.steps[j].label != "Go to Main Phase 2")
			continue;
		// Whatever followed in Main Phase 2 now happens in Main Phase 1; anything after
		// that (End Phase) is unchanged.
		for(size_t k = i; k <= j; ++k)
			drop[k] = 1;
		dropped = true;
	}
	if(dropped) {
		Trajectory shorter = t;
		shorter.steps.clear();
		for(size_t k = 0; k < t.steps.size(); ++k)
			if(!drop[k])
				shorter.steps.push_back(t.steps[k]);
		Trajectory saved = t;
		t = std::move(shorter);
		if(!defer_sets(root, t, player, horizon_turns) && !replay_labels(root, t, player, horizon_turns, saved.reward))
			t = std::move(saved);
		return t.steps.size() != saved.steps.size() || t.responses != saved.responses;
	}

	// Build the new order: each early Set, together with everything up to the player's next
	// Main Phase action (zone choice, the chain windows the Set opened), is moved to just
	// before the player leaves that Main Phase.
	std::vector<size_t> order;
	std::vector<size_t> pending;
	bool moved = false;
	for(size_t i = 0; i < t.steps.size();) {
		if(is_set(i)) {
			size_t j = i + 1;
			while(j < t.steps.size() && t.steps[j].type != DecisionType::Idle)
				++j;
			// Only worth moving if the player still makes other Main Phase plays afterwards.
			bool later_play = false;
			for(size_t k = j; k < t.steps.size(); ++k) {
				if(t.steps[k].type == DecisionType::Idle && t.steps[k].player != player)
					break;  // the turn changed
				if(is_phase_end(k))
					break;
				if(t.steps[k].player == player && t.steps[k].type == DecisionType::Idle && !is_set(k))
					later_play = true;
			}
			if(later_play) {
				for(size_t k = i; k < j; ++k)
					pending.push_back(k);
				moved = true;
			} else {
				for(size_t k = i; k < j; ++k)
					order.push_back(k);
			}
			i = j;
			continue;
		}
		bool new_turn = !pending.empty() && t.steps[i].type == DecisionType::Idle && t.steps[i].player != player;
		if(!pending.empty() && (is_phase_end(i) || new_turn)) {
			order.insert(order.end(), pending.begin(), pending.end());
			pending.clear();
		}
		order.push_back(i);
		++i;
	}
	static const bool dbg = std::getenv("YGOE_DEFER_DEBUG") != nullptr;
	if(!moved)
		return false;
	if(!pending.empty()) {
		if(dbg) fprintf(stderr, "defer: line cut before phase end\n");
		return false;  // the line was cut before the phase ended: leave it as it is
	}
	Trajectory reordered = t;
	reordered.steps.clear();
	for(size_t k : order)
		reordered.steps.push_back(t.steps[k]);
	double before = t.reward;
	Trajectory saved = t;
	t = std::move(reordered);
	if(!replay_labels(root, t, player, horizon_turns, before)) {
		t = std::move(saved);
		return false;
	}
	return true;
}

namespace {
// Is picking `o` at `d` an activation answering the other player's chain link?
bool answering(const Duel& d, const Decision& dec, const Option& o) {
	return dec.type == DecisionType::Chain && o.kind != OptKind::Pass && d.chain_depth() > 0 &&
	       d.chain_last_owner() >= 0 && d.chain_last_owner() != dec.player;
}

bool is_pass_label(const std::string& l) {
	return l == "Don't respond" || l.rfind("Don't use", 0) == 0 || l.rfind("No:", 0) == 0 || l == "Done selecting" ||
	       l == "Cancel";
}
}  // namespace

bool Searcher::drop_idle_summons(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns) {
	bool changed = false;
	for(size_t i = 0; i < t.steps.size(); ++i) {
		const Step& s = t.steps[i];
		if(s.player != player || s.type != DecisionType::Idle)
			continue;
		bool summon = s.label.rfind("Normal Summon ", 0) == 0;
		bool mset = s.label.rfind("Set ", 0) == 0 && i + 1 < t.steps.size() && t.steps[i + 1].type == DecisionType::Position;
		if(!summon && !mset)
			continue;
		// The step plus everything up to the player's next Main Phase action.
		size_t j = i + 1;
		bool quiet = true;  // nothing but zone/position choices and passes follow
		while(j < t.steps.size() && t.steps[j].type != DecisionType::Idle) {
			if(!t.steps[j].minor && t.steps[j].type != DecisionType::Position && t.steps[j].label != "Don't respond")
				quiet = false;
			++j;
		}
		// If the summon drew a response (a hand trap, a trigger), removing it would also remove
		// that response and the comparison would be unfair: keep it.
		if(!quiet)
			continue;
		Trajectory candidate = t;
		candidate.steps.erase(candidate.steps.begin() + static_cast<long>(i), candidate.steps.begin() + static_cast<long>(j));
		if(replay_labels(root, candidate, player, horizon_turns, t.reward - 0.002)) {
			t = std::move(candidate);
			changed = true;
			--i;  // the next step now sits at index i
		}
	}
	return changed;
}

bool Searcher::drop_idle_responses(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns) {
	bool changed = false;
	int tries = 0;
	for(size_t i = 0; i < t.steps.size() && tries < 8; ++i) {
		const Step& s = t.steps[i];
		if(s.player != player || s.minor || is_pass_label(s.label))
			continue;
		if(s.type != DecisionType::Chain && !(s.type == DecisionType::EffectYesNo && s.label.rfind("Use ", 0) == 0))
			continue;
		// The activation and its choices, up to this player's next action.
		size_t j = i + 1;
		while(j < t.steps.size()) {
			const Step& n = t.steps[j];
			if(n.player == player && !n.minor && !is_pass_label(n.label) &&
			   (n.type == DecisionType::Idle || n.type == DecisionType::Chain || n.type == DecisionType::Battle ||
			    n.type == DecisionType::EffectYesNo))
				break;
			if(n.type == DecisionType::Idle || n.type == DecisionType::Battle)
				break;  // the other player's action: not part of this response
			++j;
		}
		++tries;
		Trajectory candidate = t;
		Step decline{player, s.type, "__decline__", false};
		candidate.steps.erase(candidate.steps.begin() + static_cast<long>(i), candidate.steps.begin() + static_cast<long>(j));
		candidate.steps.insert(candidate.steps.begin() + static_cast<long>(i), decline);
		if(replay_labels(root, candidate, player, horizon_turns, t.reward - 0.005, true)) {
			t = std::move(candidate);
			changed = true;
		}
	}
	return changed;
}

bool Searcher::replay_labels(const RootState& root, Trajectory& t, uint8_t player, int horizon_turns, double min_reward,
                             bool lenient) {
	static const bool dbg = std::getenv("YGOE_DEFER_DEBUG") != nullptr;
	// Replay the steps by labels; every step must still be available.
	Duel d(db_, scripts_);
	d.set_declare_pool(pool_);
	if(!d.replay(root.setup, root.history))
		return false;
	const uint32_t root_turn = d.turn();
	Trajectory out;
	out.root = t.root;
	auto pass_index = [](const Decision& dec) {
		for(size_t o = 0; o < dec.options.size(); ++o)
			if(dec.options[o].kind == OptKind::Pass || dec.options[o].kind == OptKind::No)
				return o;
		return SIZE_MAX;
	};
	auto take = [&](size_t pick) {
		const Decision& dec = d.decision();
		auto resp = dec.options[pick].response;
		Step done{dec.player, dec.type, dec.options[pick].label, dec.minor, answering(d, dec, dec.options[pick])};
		if(!d.respond(resp))
			return false;
		out.responses.push_back(std::move(resp));
		out.steps.push_back(std::move(done));
		return true;
	};
	for(const Step& s : t.steps) {
		if(lenient) {
			// Pass windows the line does not answer (they appear or move when a response is dropped).
			int guard = 0;
			while(!d.ended() && guard++ < 30) {
				const Decision& dec = d.decision();
				size_t p = pass_index(dec);
				bool matches = dec.player == s.player;
				if(matches && s.label != "__decline__") {
					matches = false;
					for(const auto& o : dec.options)
						if(o.label == s.label)
							matches = true;
					matches = matches || s.minor;
				}
				if(matches)
					break;
				if(dec.type == DecisionType::Chain && p != SIZE_MAX) {
					if(!take(p))
						return false;
					continue;
				}
				// A choice the line never met (an effect that now resolves): take the first answer.
				if(dec.type != DecisionType::Idle && dec.type != DecisionType::Battle && !dec.options.empty() &&
				   s.label != "__decline__") {
					if(!take(0))
						return false;
					continue;
				}
				break;
			}
			if(d.ended())
				break;
			const Decision& dec = d.decision();
			if(s.label == "__decline__") {
				size_t p = dec.player == s.player ? pass_index(dec) : SIZE_MAX;
				if(p == SIZE_MAX || !take(p))
					return false;
				continue;
			}
			// A pass the line no longer meets (the window it answered is gone): skip it.
			if(is_pass_label(s.label) && (dec.player != s.player || pass_index(dec) == SIZE_MAX))
				continue;
		}
		const Decision& dec = d.decision();
		if(d.ended() || dec.options.empty() || dec.player != s.player)
			return false;
		size_t pick = SIZE_MAX;
		for(size_t o = 0; o < dec.options.size(); ++o)
			if(dec.options[o].label == s.label) {
				pick = o;
				break;
			}
		if(pick == SIZE_MAX && s.minor)
			pick = 0;
		if(pick == SIZE_MAX) {
			if(dbg) fprintf(stderr, "defer: step '%s' not available\n", s.label.c_str());
			return false;
		}
		auto resp = dec.options[pick].response;
		Step done{dec.player, dec.type, dec.options[pick].label, dec.minor, answering(d, dec, dec.options[pick])};
		if(!d.respond(resp))
			return false;
		out.responses.push_back(std::move(resp));
		out.steps.push_back(std::move(done));
	}
	uint8_t next = d.turn() >= root_turn + static_cast<uint32_t>(horizon_turns) ? d.turn_player()
	                                                                             : static_cast<uint8_t>(1 - d.turn_player());
	auto evr = eval_.evaluate(d.board(), player, next, d.ended(), d.winner(), dbg);
	out.reward = evr.score;
	if(dbg) {
		fprintf(stderr, "replay: me %.2f opp %.2f next P%d\n", evr.value[player], evr.value[1 - player], next);
		auto b = d.board();
		for(int pl = 0; pl < 2; ++pl) {
			std::string f;
			for(auto& c : b.hand[pl]) f += db_.name(c.code) + "; ";
			fprintf(stderr, "   P%d hand: %s\n", pl, f.c_str());
		}
	}
	if(out.reward + 0.01 < min_reward) {
		if(dbg) fprintf(stderr, "defer: reward %.3f < %.3f\n", out.reward, min_reward);
		return false;
	}
	if(dbg) fprintf(stderr, "defer: ok\n");
	t = std::move(out);
	return true;
}

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
	std::atomic<long> guide_steps{0}, guide_done{0}, guide_runs{0};
	const auto t0 = std::chrono::steady_clock::now();
	auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
	bool root_info_set = false;

	// What the searching player has, for chaining pieces (their own cards are the same in
	// every determinization).
	PieceResources resources;
	const bool guided = cfg.guide && !cfg.guide->empty() && cfg.guided_share > 0;
	if(guided) {
		Duel d(db_, scripts_);
		d.set_declare_pool(pool_);
		if(d.replay(roots[0].setup, roots[0].history))
			resources = resources_of(db_, d.board(), me);
	}

	auto worker = [&](unsigned tid) {
		std::mt19937_64 rng(0xC0FFEEull * (tid + 1) + roots.size());
		Duel duel(db_, scripts_);
		duel.set_declare_pool(pool_);
		int steps_this_turn = 0;
		std::vector<std::string> plan;  // labels of a chained-pieces plan, for this iteration
		size_t plan_pos = 0;
		auto plan_match = [&](const Option& o, size_t at) {
			if(at >= plan.size())
				return false;
			const std::string& want = plan[at];
			return o.label == want || (o.label.size() > 5 && o.label.compare(0, 5, "Yes: ") == 0 && o.label.compare(5, std::string::npos, want) == 0);
		};
		// How far ahead in the plan `o` is (0 = the next label), -1 if not within reach. A few
		// labels may be skipped: a step the line already did another way, a choice that
		// differs this time.
		auto plan_ahead = [&](const Option& o) {
			for(size_t k = 0; k < 4 && plan_pos + k < plan.size(); ++k)
				if(plan_match(o, plan_pos + k))
					return static_cast<int>(k);
			return -1;
		};
		std::uniform_real_distribution<double> unit(0, 1);
		// The card whose effect this decision belongs to: the last one the searching player
		// activated or summoned (or the chain's top card while it resolves).
		uint32_t last_action = 0;
		auto imitates = [&](const Decision& dd, const Option& o) {
			if(dd.type == DecisionType::Idle || dd.type == DecisionType::Chain || dd.type == DecisionType::Battle)
				return false;
			const uint32_t ctx1 = last_action, ctx2 = duel.chain_last_code();
			for(const auto& im : cfg.imitate)
				if(im.choose == o.code && (im.action == ctx1 || im.action == ctx2))
					return true;
			return false;
		};
		// Playout prior. Summoning or setting a hand trap usually wastes it, so those are
		// tried last (still possible: some lines need the body, e.g. as Synchro material).
		auto weight = [&](const Decision& dd, const Option& o) {
			double w = policy_weight(dd, o, duel.turn_player(), steps_this_turn);
			// learned from this deck's own good lines, so only for the searching player's options
			if(cfg.policy && dd.player == me)
				w *= cfg.policy->factor(dd, o, PolicyContext{duel.turn_player(), steps_this_turn, duel.chain_depth()});
			if((o.kind == OptKind::Summon || o.kind == OptKind::MSet) && eval_.role(dd.player, o.code).hand_trap)
				w *= 0.1;
			// The other player answering an open window (a phase change, nothing on the chain)
			// mostly throws a card away; a random opponent doing it often makes passing the turn
			// look good ("they'll waste their hand trap and I continue").
			if(dd.type == DecisionType::Chain && dd.player != duel.turn_player() && duel.chain_depth() == 0 &&
			   o.kind == OptKind::Pass)
				w *= 6;
			// Nor does a hand trap answer that player's own effect.
			if(dd.type == DecisionType::Chain && o.kind == OptKind::Activate && duel.chain_last_player() == dd.player &&
			   eval_.role(dd.player, o.code).hand_trap)
				w *= 0.05;
			if(dd.player == me && plan_ahead(o) >= 0)
				w = std::max(w, 1.0) * 25;
			if(dd.player == me && !cfg.imitate.empty() && o.code && imitates(dd, o))
				w = std::max(w, 1.0) * 12;
			return w;
		};
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
			steps_this_turn = 0;
			plan.clear();
			plan_pos = 0;
			last_action = 0;
			if(guided && unit(rng) < cfg.guided_share)
				plan = cfg.guide->sample(resources, rng);
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
				// Setting a hand trap face-down throws it away; don't consider it while
				// anything else can be done.
				if(dec.type == DecisionType::Idle && dec.options.size() > 1) {
					size_t blocked = 0;
					for(size_t i = 0; i < dec.options.size(); ++i) {
						const Option& o = dec.options[i];
						bool block = o.kind == OptKind::MSet && eval_.role(dec.player, o.code).hand_trap;
						// Setting a Spell you could activate from the hand right now only delays it
						// (Quick-Play Spells are the exception: they can wait for the opponent's turn).
						if(o.kind == OptKind::SSet) {
							auto c = db_.get(o.code);
							bool spell = c && (c->type & TYPE_SPELL) && !(c->type & TYPE_QUICKPLAY);
							if(spell)
								for(const auto& other : dec.options)
									if(other.kind == OptKind::Activate && other.code == o.code && other.loc == LOCATION_HAND)
										block = true;
						}
						if(block) {
							tried[i] = 1;
							++blocked;
						}
					}
					if(blocked == dec.options.size())
						std::fill(tried.begin(), tried.end(), 0);
				}
				bool responded = false;
				bool counted = false;  // availability counted for this visit
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
						// Availability counts (ISMCTS): once per visit to this decision. (Options held
						// back up front, like setting a hand trap, must not stop the count: with avail
						// at 0 the exploration term vanishes and one unlucky playout buries a move.)
						if(!counted) {
							counted = true;
							for(size_t i = 0; i < present.size(); ++i)
								if(std::find(present.begin(), present.begin() + i, present[i]) == present.begin() + i)
									++node->children[present[i]].avail;
						}
						std::vector<size_t> unvisited;
						std::vector<double> uw;
						double best = -std::numeric_limits<double>::infinity();
						size_t best_i = SIZE_MAX;
						// Prior of each option relative to the best one: a progressive bias that
						// fades as the option gets visits, so the search tries natural orderings
						// (develop first, set last) before equivalent but worse ones.
						double max_prior = 0;
						for(size_t i = 0; i < present.size(); ++i)
							max_prior = std::max(max_prior, weight(dec, dec.options[i]));
						for(size_t i = 0; i < present.size(); ++i) {
							Child& c = node->children[present[i]];
							if(tried[i] || c.bad)
								continue;
							if(c.N == 0) {
								unvisited.push_back(i);
								uw.push_back(weight(dec, dec.options[i]));
								continue;
							}
							// Blend the average with the best line found (worst, for the opponent):
							// a turn is mostly our own choices, and random playouts after a strong
							// starter undersell it.
							double q = (1 - cfg.max_blend) * (c.W / c.N) + cfg.max_blend * (dec.player == me ? c.hi : c.lo);
							if(dec.player != me)
								q = 1.0 - q;
							double prior = max_prior > 0
							                   ? weight(dec, dec.options[i]) / max_prior
							                   : 1.0;
							double u = q + cfg.exploration * std::sqrt(std::log(std::max(1, c.avail)) / c.N) +
							           cfg.prior_bias * prior / (1.0 + c.N);
							if(u > best) {
								best = u;
								best_i = i;
							}
						}
						// A guided iteration follows its plan through the tree too.
						size_t follow = SIZE_MAX;
						if(dec.player == me && plan_pos < plan.size()) {
							int best_k = 99;
							for(size_t i = 0; i < present.size(); ++i) {
								if(tried[i] || node->children[present[i]].bad)
									continue;
								int k = plan_ahead(dec.options[i]);
								if(k >= 0 && k < best_k) {
									best_k = k;
									follow = i;
								}
							}
						}
						if(follow != SIZE_MAX) {
							idx = follow;
						} else if(!unvisited.empty()) {
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
							w[i] = tried[i] ? 0 : weight(dec, dec.options[i]);
						idx = weighted_pick(w, rng);
						if(tried[idx])
							idx = SIZE_MAX;
					}
					if(idx == SIZE_MAX || idx >= dec.options.size())
						break;  // nothing legal left
					tried[idx] = 1;
					const Option& opt = dec.options[idx];
					const bool answer = answering(duel, dec, opt);
					if(duel.respond(opt.response)) {
						responded = true;
						traj.responses.push_back(opt.response);
						traj.steps.push_back({dec.player, dec.type, opt.label, dec.minor, answer});
						if(dec.player == me && opt.code &&
						   (dec.type == DecisionType::Idle || dec.type == DecisionType::Chain || dec.type == DecisionType::EffectYesNo) &&
						   opt.kind != OptKind::Pass)
							last_action = opt.code;
						if(dec.player == me && plan_pos < plan.size()) {
							int k = plan_ahead(opt);
							if(k >= 0)
								plan_pos += static_cast<size_t>(k) + 1;
						}
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
			if(!plan.empty()) {
				guide_steps += static_cast<long>(plan.size());
				guide_done += static_cast<long>(std::min(plan_pos, plan.size()));
				++guide_runs;
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
					Child& ch = n->children[ci];
					if(ch.N == 0)
						ch.hi = ch.lo = ev.score;
					ch.N++;
					ch.W += ev.score;
					ch.hi = std::max(ch.hi, ev.score);
					ch.lo = std::min(ch.lo, ev.score);
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
	if(std::getenv("YGOE_SEARCH_DEBUG"))
		for(const auto& c : root->children)
			std::fprintf(stderr, "  root child N=%d avail=%d bad=%d mean=%.3f hi=%.3f %s\n", c.N, c.avail, c.bad ? 1 : 0,
			             c.N ? c.W / c.N : 0.0, c.hi, c.label.substr(0, 60).c_str());
	if(guide_runs > 0 && std::getenv("YGOE_GUIDE_DEBUG"))
		std::fprintf(stderr, "guide: %ld plans, %.0f%% of their steps followed (avg plan %.1f labels)\n", guide_runs.load(),
		             100.0 * guide_done / std::max(1L, guide_steps.load()), double(guide_steps) / guide_runs);
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
		if(s.answer && mine)
			text = "Answer: " + text;  // interaction with the other player, not the combo
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
