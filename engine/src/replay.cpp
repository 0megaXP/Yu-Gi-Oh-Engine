#include "replay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <sstream>

#include "combo.h"
#include "json.h"
#include "reference.h"
#include "search.h"
#include "policy.h"

namespace ygoe {

namespace {

// How a video step can be found in a replay.
enum class Kind {
	Action,    // must be chosen: activate, Normal Summon, Set
	Summon,    // chosen (a summon from the Extra Deck by its own procedure) or seen (a card summoned by an effect)
	Seen,      // only seen: a card added to the hand, sent to the GY, banished, attached
	Ignored,   // nothing to check (phases, attacks, "resolve", unknown cards)
};

enum class Zone { Hand, Field, Grave, Banished, Overlay, SetZone, None };

struct StepInfo {
	Kind kind = Kind::Ignored;
	Zone zone = Zone::None;
	uint32_t sid = 0;  // script id
};

StepInfo classify(const CardDB& db, const VideoStep& s) {
	StepInfo i;
	if(!s.code)
		return i;
	i.sid = db.script_id(s.code);
	const std::string& a = s.action;
	if(a == "activate" || a == "chain" || a == "normal_summon") {
		i.kind = Kind::Action;
	} else if(a == "set") {
		i.kind = Kind::Action;
		i.zone = Zone::SetZone;
	} else if(a.size() > 7 && a.compare(a.size() - 7, 7, "_summon") == 0) {
		i.kind = Kind::Summon;
		i.zone = Zone::Field;
	} else if(a == "search" || a == "draw" || a == "return") {
		i.kind = Kind::Seen;
		i.zone = Zone::Hand;
	} else if(a == "send_to_gy") {
		i.kind = Kind::Seen;
		i.zone = Zone::Grave;
	} else if(a == "banish") {
		i.kind = Kind::Seen;
		i.zone = Zone::Banished;
	} else if(a == "attach") {
		i.kind = Kind::Seen;
		i.zone = Zone::Overlay;
	}
	return i;
}

int count_in(const Board& b, uint32_t sid, Zone z) {
	auto n = [&](const std::vector<CardView>& v) {
		return static_cast<int>(std::count_if(v.begin(), v.end(), [&](const CardView& c) { return c.code == sid; }));
	};
	switch(z) {
	case Zone::Hand: return n(b.hand[0]);
	case Zone::Field: return n(b.mzone[0]) + n(b.szone[0]);
	case Zone::Grave: return n(b.grave[0]);
	case Zone::Banished: return n(b.banished[0]);
	case Zone::SetZone: return n(b.szone[0]) + n(b.mzone[0]);
	case Zone::Overlay: {
		int k = 0;
		for(auto& c : b.mzone[0])
			k += static_cast<int>(std::count(c.overlay.begin(), c.overlay.end(), sid));
		return k;
	}
	default: return 0;
	}
}

bool option_matches(const Option& o, const VideoStep& s, const StepInfo& i) {
	if(!i.sid || o.code != i.sid)
		return false;
	switch(o.kind) {
	case OptKind::Activate: return s.action == "activate" || s.action == "chain" || i.kind == Kind::Summon;
	case OptKind::Summon: return s.action == "normal_summon";
	case OptKind::MSet:
	case OptKind::SSet: return s.action == "set";
	case OptKind::SpSummon: return i.kind == Kind::Summon;
	default: return false;
	}
}

struct Node {
	std::vector<std::vector<uint8_t>> responses;
	std::vector<Step> steps;
	size_t next = 0;               // next video step to find
	std::vector<int> match;        // per video step: 1 matched, 0 missed, -1 ignored
	Board base;                    // board before the action that may have caused the pending "seen" steps
	bool done = false;
	bool stuck = false;            // no option matched the next steps: the rest of the turn was skipped
	double score = 0;
	std::string sig;
};

class Replayer {
public:
	Replayer(const CardDB& db, ScriptCache& scripts, const Deck& deck, const VideoLine& line, const ReplayConfig& cfg)
	    : db_(db), scripts_(scripts), line_(line), cfg_(cfg), rng_(cfg.seed) {
		sc_.decks[0] = deck;
		sc_.decks[1] = deck;
		sc_.hand[0] = line.hand;
		sc_.hand_known[0] = true;
		sc_.hand_known[1] = true;  // empty hand: goldfish, nothing interacts
		sc_.me = 0;
		sc_.seed = cfg.seed;
		setup_ = make_setup(sc_, 0);
		pool_ = declare_pool(sc_);
		for(const auto& s : line.steps)
			info_.push_back(classify(db, s));
		for(auto c : line.end_board)
			end_sids_.push_back(db.script_id(c));
	}

	const DuelSetup& setup() const { return setup_; }
	const std::vector<uint32_t>& pool() const { return pool_; }
	const std::vector<StepInfo>& info() const { return info_; }

	// Runs the beam; returns the best finished replay.
	Node run() {
		auto t0 = std::chrono::steady_clock::now();
		auto out_of_time = [&] {
			return (cfg_.cancel && cfg_.cancel->load()) ||
			       std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > cfg_.seconds;
		};
		Node root;
		root.match.assign(line_.steps.size(), -1);
		for(size_t k = 0; k < info_.size(); ++k)
			if(info_[k].kind != Kind::Ignored)
				root.match[k] = 0;
		{
			Duel d(db_, scripts_);
			d.set_declare_pool(pool_);
			if(!d.start(setup_))
				return root;
			skip_ignored(root);
			root.base = d.board();
			advance(d, root, false);
			finish_node(d, root);
		}
		std::vector<Node> beam{root};
		for(int depth = 0; depth < static_cast<int>(line_.steps.size()) + 8; ++depth) {
			if(std::all_of(beam.begin(), beam.end(), [](const Node& n) { return n.done; }))
				break;
			std::vector<Node> next;
			for(const Node& n : beam) {
				if(n.done || out_of_time()) {
					Node m = n;
					if(!m.done)
						close(m);
					next.push_back(std::move(m));
					continue;
				}
				expand(n, next);
			}
			// best first, one node per distinct state
			std::sort(next.begin(), next.end(), [](const Node& a, const Node& b) { return a.score > b.score; });
			beam.clear();
			std::set<std::string> seen;
			for(auto& n : next) {
				if(!seen.insert(n.sig).second)
					continue;
				beam.push_back(std::move(n));
				if(static_cast<int>(beam.size()) >= cfg_.beam)
					break;
			}
			if(beam.empty())
				break;
		}
		for(auto& n : beam)
			if(!n.done)
				close(n);
		return *std::max_element(beam.begin(), beam.end(), [](const Node& a, const Node& b) { return a.score < b.score; });
	}

	bool board_of(const Node& n, Board& out) {
		Duel d(db_, scripts_);
		d.set_declare_pool(pool_);
		if(!d.replay(setup_, n.responses))
			return false;
		out = d.board();
		return true;
	}

private:
	// Steps that need nothing checked are skipped over.
	void skip_ignored(Node& n) const {
		while(n.next < info_.size() && info_[n.next].kind == Kind::Ignored)
			++n.next;
	}

	// Pending "seen" steps that happened since the last chosen action count as matched.
	void absorb_seen(Node& n, const Board& now) const {
		for(;;) {
			skipped_ahead(n);
			if(n.next >= info_.size())
				return;
			const StepInfo& i = info_[n.next];
			if(i.kind == Kind::Action)
				return;
			if(count_in(now, i.sid, i.zone) > count_in(n.base, i.sid, i.zone)) {
				n.match[n.next] = 1;
				++n.next;
				continue;
			}
			// A summon or "seen" step not visible yet: it may come from a later action, or the
			// video logged it out of order. Look one step past it before giving up on it.
			if(n.next + 1 < info_.size()) {
				const StepInfo& j = info_[n.next + 1];
				if(j.kind != Kind::Action && j.kind != Kind::Ignored &&
				   count_in(now, j.sid, j.zone) > count_in(n.base, j.sid, j.zone)) {
					// keep n.next on the unseen step; the later one is skipped once reached
					n.match[n.next + 1] = 1;
				}
			}
			return;
		}
	}

	// After a later step was matched out of order, move past steps already matched.
	void skipped_ahead(Node& n) const {
		while(n.next < info_.size() && (info_[n.next].kind == Kind::Ignored || n.match[n.next] == 1))
			++n.next;
	}

	// Is this decision one where the player picks their next action?
	bool is_action_point(const Decision& dec, const Node& n) const {
		if(dec.player != 0)
			return false;
		if(dec.type == DecisionType::Idle || dec.type == DecisionType::Battle)
			return true;
		if(dec.type == DecisionType::Chain)
			return !candidates(dec, n, 0).empty();
		return false;
	}

	// Options matching one of the next video steps: (option index, video step index).
	// Chain windows open all the time, so there only the very next step counts (window 0);
	// at the player's own action points a few misread or merged steps may be skipped.
	std::vector<std::pair<size_t, size_t>> candidates(const Decision& dec, const Node& n, int window = -1) const {
		std::vector<std::pair<size_t, size_t>> out;
		if(window < 0)
			window = dec.type == DecisionType::Chain ? 0 : cfg_.window;
		size_t k = n.next;
		int skipped = 0;
		while(k < info_.size() && skipped <= window) {
			const StepInfo& i = info_[k];
			if(n.match[k] == 1) {  // already found (a trigger or an out-of-order step)
				++k;
				continue;
			}
			if(i.kind == Kind::Action || i.kind == Kind::Summon) {
				for(size_t o = 0; o < dec.options.size(); ++o)
					if(option_matches(dec.options[o], line_.steps[k], i))
						out.emplace_back(o, k);
				if(!out.empty())
					break;
			}
			if(i.kind != Kind::Ignored && n.match[k] != 1)
				++skipped;
			++k;
		}
		return out;
	}

	// The next unmatched "activate" step (within the skip window) for card `sid`, or SIZE_MAX.
	size_t pending_activation(uint32_t sid, const Node& n) const {
		int skipped = 0;
		for(size_t k = n.next; k < info_.size() && skipped <= cfg_.window; ++k) {
			const StepInfo& i = info_[k];
			if(n.match[k] == 1 || i.kind == Kind::Ignored)
				continue;
			if(i.kind == Kind::Action && i.sid == sid &&
			   (line_.steps[k].action == "activate" || line_.steps[k].action == "chain"))
				return k;
			++skipped;
		}
		return SIZE_MAX;
	}

	// Names the video mentions around the next step, with how strongly the choices inside an
	// action should favour them: the action's own detail and the next few steps count most.
	std::vector<std::pair<std::string, double>> hints(const Node& n) const {
		std::vector<std::pair<std::string, double>> h;
		auto add = [&](const std::string& t, double w) {
			if(!t.empty())
				h.emplace_back(t, w);
		};
		// n.next already points past the action being played; its detail is the step before
		if(n.next > 0 && n.next - 1 < line_.steps.size())
			add(line_.steps[n.next - 1].detail, 6);
		for(size_t k = n.next; k < line_.steps.size() && k < n.next + 6; ++k) {
			if(line_.steps[k].code)
				add(db_.name(db_.script_id(line_.steps[k].code)), k < n.next + 3 ? 6 : 2);
			add(line_.steps[k].detail, k < n.next + 2 ? 3 : 1);
		}
		for(auto c : line_.end_board)
			add(db_.name(db_.script_id(c)), 1);
		return h;
	}

	static bool mentions(const std::string& text, const std::string& name) {
		return !name.empty() && name.size() > 3 && text.find(name) != std::string::npos;
	}

	// Plays every decision that isn't one of the player's actions. Returns true if any choice
	// was sampled (so trying again could give a different result).
	bool advance(Duel& d, Node& n, bool sample) {
		bool sampled = false;
		auto h = hints(n);
		for(int guard = 0; guard < 400; ++guard) {
			if(d.ended() || d.turn() > 1)
				return sampled;
			const Decision& dec = d.decision();
			if(dec.options.empty())
				return sampled;
			if(is_action_point(dec, n))
				return sampled;
			size_t pick = 0;
			// An optional trigger ("Use Angelechy Destrier") the video shows as an activation.
			if(dec.player == 0 && dec.type == DecisionType::EffectYesNo) {
				size_t yes = SIZE_MAX, step = SIZE_MAX;
				for(size_t o = 0; o < dec.options.size() && yes == SIZE_MAX; ++o)
					if(dec.options[o].kind == OptKind::Yes && (step = pending_activation(dec.options[o].code, n)) != SIZE_MAX)
						yes = o;
				if(yes != SIZE_MAX) {
					if(!take(d, n, yes))
						return sampled;
					n.match[step] = 1;
					if(step == n.next) {
						n.next = step + 1;
						skipped_ahead(n);
					}
					continue;
				}
			}
			if(dec.player != 0) {
				// goldfish: the opponent passes whenever it can
				pick = pass_or(dec, best_policy(dec));
			} else if(dec.type == DecisionType::Chain) {
				pick = pass_or(dec, 0);
			} else if(dec.minor || dec.options.size() == 1) {
				pick = best_policy(dec);
			} else {
				std::vector<double> w(dec.options.size());
				for(size_t o = 0; o < w.size(); ++o) {
					// yes/no answers get a real chance either way: the policy's guess is often "no"
					// for "then you can add 1 ..." options a combo depends on
					OptKind k = dec.options[o].kind;
					double floor = (k == OptKind::Yes || k == OptKind::No) ? 0.3 : 0.05;
					double x = std::max(floor, playout_weight(dec, dec.options[o], 0, static_cast<int>(n.steps.size())));
					double m = 0;
					const std::string& label = dec.options[o].label;
					// the card names in the option ("Dominus Purge (hand)", "A + B") against the
					// names and details the video gives around this action
					std::string bare = label.substr(0, label.find(" ("));
					for(auto& [text, weight] : h) {
						if(mentions(label, text) || (bare.size() > 3 && text.find(bare) != std::string::npos))
							m += weight;
					}
					w[o] = x * (1 + m);
				}
				if(sample) {
					std::discrete_distribution<size_t> dist(w.begin(), w.end());
					pick = dist(rng_);
					sampled = true;
				} else {
					pick = static_cast<size_t>(std::max_element(w.begin(), w.end()) - w.begin());
				}
			}
			if(!take(d, n, pick))
				return sampled;
		}
		return sampled;
	}

	static size_t pass_or(const Decision& dec, size_t fallback) {
		for(size_t o = 0; o < dec.options.size(); ++o)
			if(dec.options[o].kind == OptKind::Pass || dec.options[o].kind == OptKind::No)
				return o;
		return fallback;
	}

	size_t best_policy(const Decision& dec) const {
		size_t best = 0;
		double bw = -1;
		for(size_t o = 0; o < dec.options.size(); ++o) {
			double w = playout_weight(dec, dec.options[o], 0, 0);
			if(w > bw) { bw = w; best = o; }
		}
		return best;
	}

	bool take(Duel& d, Node& n, size_t pick) {
		const Decision& dec = d.decision();
		auto resp = dec.options[pick].response;
		Step s{dec.player, dec.type, dec.options[pick].label, dec.minor};
		if(!d.respond(resp))
			return false;
		n.responses.push_back(std::move(resp));
		n.steps.push_back(std::move(s));
		return true;
	}

	// After an advance: count what the video said would happen, score, sign.
	void finish_node(Duel& d, Node& n) {
		Board now = d.board();
		absorb_seen(n, now);
		if(d.ended() || d.turn() > 1)
			n.done = true;
		score(n, now);
		// a replay where the video's next action can be played right away is on the right track
		if(!n.done && n.next < info_.size() && !d.decision().options.empty() && !candidates(d.decision(), n).empty())
			n.score += 6;
	}

	void score(Node& n, const Board& now) {
		int matched = 0, missed = 0;
		for(size_t k = 0; k < n.match.size(); ++k) {
			if(n.match[k] == 1)
				++matched;
			else if(n.match[k] == 0 && k < n.next)
				++missed;
		}
		int on_board = 0;
		for(auto sid : end_sids_)
			if(count_in(now, sid, Zone::Field) > 0)
				++on_board;
		n.score = matched * 10.0 - missed * 3.0 + on_board * 2.0 - (n.stuck ? 20 : 0);
		std::ostringstream sig;
		sig << n.next << '|';
		auto zone = [&](const std::vector<CardView>& v, char tag) {
			std::vector<uint32_t> c;
			for(auto& x : v)
				c.push_back(x.code);
			std::sort(c.begin(), c.end());
			sig << tag;
			for(auto x : c)
				sig << x << ',';
		};
		zone(now.hand[0], 'h');
		zone(now.mzone[0], 'm');
		zone(now.szone[0], 's');
		zone(now.grave[0], 'g');
		zone(now.banished[0], 'b');
		n.sig = sig.str();
	}

	void expand(const Node& n, std::vector<Node>& out) {
		Duel d(db_, scripts_);
		d.set_declare_pool(pool_);
		if(!d.replay(setup_, n.responses))
			return;
		const Decision& dec = d.decision();
		auto cands = n.next < info_.size() ? candidates(dec, n) : std::vector<std::pair<size_t, size_t>>{};
		if(cands.empty()) {
			// all steps found, or the next ones can't be played here: end the turn
			Node m = n;
			if(m.next < info_.size())
				m.stuck = true;
			close(m);
			out.push_back(std::move(m));
			return;
		}
		if(cands.size() > 4)
			cands.resize(4);
		for(auto [opt, step] : cands) {
			for(int t = 0; t < cfg_.tries; ++t) {
				Duel e(db_, scripts_);
				e.set_declare_pool(pool_);
				if(!e.replay(setup_, n.responses))
					return;
				Node m = n;
				m.base = e.board();
				m.match[step] = 1;  // steps skipped to get here stay unmatched
				m.next = step + 1;
				skipped_ahead(m);
				if(!take(e, m, opt))
					break;
				bool sampled = advance(e, m, t > 0);
				finish_node(e, m);
				out.push_back(std::move(m));
				if(!sampled && t > 0)
					break;  // nothing random inside this action: one try is enough
			}
		}
	}

	// Plays the rest of the turn without new actions.
	void close(Node& n) {
		Duel d(db_, scripts_);
		d.set_declare_pool(pool_);
		if(!d.replay(setup_, n.responses)) {
			n.done = true;
			return;
		}
		for(int guard = 0; guard < 300 && !d.ended() && d.turn() <= 1; ++guard) {
			const Decision& dec = d.decision();
			if(dec.options.empty())
				break;
			size_t pick;
			if(dec.player == 0 && (dec.type == DecisionType::Idle || dec.type == DecisionType::Battle)) {
				pick = SIZE_MAX;
				for(size_t o = 0; o < dec.options.size(); ++o)
					if(dec.options[o].kind == OptKind::ToEnd)
						pick = o;
				if(pick == SIZE_MAX)
					pick = pass_or(dec, best_policy(dec));
			} else if(dec.type == DecisionType::Chain) {
				pick = pass_or(dec, 0);
			} else {
				pick = dec.player == 0 ? best_policy(dec) : pass_or(dec, best_policy(dec));
			}
			if(!take(d, n, pick))
				break;
		}
		n.done = true;
		Board b = d.board();
		score(n, b);
	}

	const CardDB& db_;
	ScriptCache& scripts_;
	const VideoLine& line_;
	ReplayConfig cfg_;
	std::mt19937_64 rng_;
	Scenario sc_;
	DuelSetup setup_;
	std::vector<uint32_t> pool_;
	std::vector<StepInfo> info_;
	std::vector<uint32_t> end_sids_;
};

}  // namespace

LinesCheck check_video_lines(const CardDB& db, ScriptCache& scripts, const Deck& deck, const std::vector<VideoLine>& lines,
                             const ReplayConfig& cfg, const std::function<void(int, int)>& progress, Policy* policy) {
	LinesCheck res;
	std::vector<PolicyExample> examples;
	const auto hand_traps = likely_hand_traps(db, nullptr, 8);
	int verified_lines = 0;
	for(size_t li = 0; li < lines.size(); ++li) {
		if(cfg.cancel && cfg.cancel->load())
			break;
		if(progress)
			progress(static_cast<int>(li), static_cast<int>(lines.size()));
		const VideoLine& line = lines[li];
		LineCheck lc;
		lc.id = line.id;
		if(line.hand.empty()) {
			lc.problem = "the video's opening hand wasn't read";
			res.lines.push_back(std::move(lc));
			continue;
		}
		Replayer r(db, scripts, deck, line, cfg);
		Node best = r.run();
		for(size_t k = 0; k < best.match.size(); ++k) {
			lc.step_match.push_back(best.match[k]);
			if(best.match[k] >= 0)
				++lc.steps_checked;
			if(best.match[k] == 1)
				++lc.steps_matched;
		}
		Board end;
		if(!r.board_of(best, end)) {
			lc.problem = "the replay failed";
			res.lines.push_back(std::move(lc));
			continue;
		}
		int want = 0, have = 0;
		for(auto c : line.end_board) {
			uint32_t sid = db.script_id(c);
			++want;
			if(count_in(end, sid, Zone::Field) > 0)
				++have;
		}
		lc.board_match = want ? double(have) / want : 0;
		lc.value = board_value(db, deck, end, 0);
		lc.board = summarize_cards(db, end, 0);
		Trajectory t;
		t.responses = best.responses;
		t.steps = best.steps;
		lc.moves = describe(t, 0, false);
		double step_share = lc.steps_checked ? double(lc.steps_matched) / lc.steps_checked : 0;
		if(lc.steps_checked < 3)
			lc.problem = "too few steps could be checked";
		else if(want == 0)
			lc.problem = "the video's end board wasn't read";
		else if(step_share < 0.75)
			lc.problem = "only " + std::to_string(lc.steps_matched) + " of " + std::to_string(lc.steps_checked) +
			             " steps could be played by the rules";
		else if(lc.board_match < 0.75)
			lc.problem = "the replay ends on " + std::to_string(have) + " of the video's " + std::to_string(want) + " end-board cards";
		lc.verified = lc.problem.empty();
		if(lc.verified) {
			++verified_lines;
			auto split = split_line(db, scripts, r.pool(), r.setup(), t, hand_traps);
			add_to_library(res.library, split, lc.value);
			if(policy) {
				auto ex = policy->examples_from_line(scripts, r.pool(), r.setup(), best.responses, 0, 2.0f);
				examples.insert(examples.end(), ex.begin(), ex.end());
			}
			lc.pieces = std::move(split.pieces);
		}
		res.lines.push_back(std::move(lc));
	}
	if(progress)
		progress(static_cast<int>(lines.size()), static_cast<int>(lines.size()));
	res.verified = verified_lines;
	sort_library(res.library, 60);
	if(policy && !examples.empty()) {
		policy->train(examples);
		res.policy = policy->to_json();
	}
	return res;
}

std::string LinesCheck::to_json(const CardDB& db) const {
	using json::quote;
	auto num = [](double v) {
		char b[32];
		std::snprintf(b, sizeof b, "%.3f", v);
		return std::string(b);
	};
	std::ostringstream o;
	o << "{\"verified\":" << verified << ",\"lines\":[";
	for(size_t i = 0; i < lines.size(); ++i) {
		const auto& l = lines[i];
		o << (i ? "," : "") << "{\"id\":" << quote(l.id) << ",\"verified\":" << (l.verified ? "true" : "false")
		  << ",\"steps_checked\":" << l.steps_checked << ",\"steps_matched\":" << l.steps_matched << ",\"step_match\":[";
		for(size_t k = 0; k < l.step_match.size(); ++k)
			o << (k ? "," : "") << l.step_match[k];
		o << "],\"board_match\":" << num(l.board_match) << ",\"problem\":" << quote(l.problem) << ",\"value\":" << num(l.value)
		  << ",\"moves\":[";
		for(size_t k = 0; k < l.moves.size(); ++k)
			o << (k ? "," : "") << quote(l.moves[k]);
		o << "],\"board\":[";
		for(size_t k = 0; k < l.board.size(); ++k) {
			const auto& c = l.board[k];
			o << (k ? "," : "") << "{\"code\":" << c.code << ",\"name\":" << quote(c.name) << ",\"where\":" << quote(c.where)
			  << ",\"note\":" << quote(c.note) << "}";
		}
		o << "],\"pieces\":" << pieces_json(db, l.pieces) << "}";
	}
	o << "],\"library\":" << library_json(db, library, std::max(1, verified));
	if(!policy.empty())
		o << ",\"policy\":" << policy;
	o << "}";
	return o.str();
}

}  // namespace ygoe
