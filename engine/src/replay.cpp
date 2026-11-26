#include <locale>
#include "replay.h"

#include <algorithm>
#include <cstdlib>
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
	Use,       // "played somehow" (text recognition sees the name, not the action): chosen, or seen anywhere
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
	if(!s.code || s.by != 0)
		return i;
	i.sid = db.script_id(s.code);
	const std::string& a = s.action;
	if(a == "use") {
		i.kind = Kind::Use;
	} else if(a == "activate" || a == "chain" || a == "normal_summon") {
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
	case Zone::Field: return n(b.mzone[0]) + n(b.szone[0]) + n(b.mzone[1]);
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
	if(s.action == "use")
		return o.kind == OptKind::Activate || o.kind == OptKind::Summon || o.kind == OptKind::SpSummon ||
		       o.kind == OptKind::MSet || o.kind == OptKind::SSet;
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
	std::vector<size_t> settled;   // per video step: responses up to where its action had played out
	Board base;                    // board before the action that may have caused the pending "seen" steps
	bool done = false;
	bool stuck = false;            // no option matched the next steps: the rest of the turn was skipped
	double score = 0;
	std::string sig;
	size_t events_seen = 0;           // duel events already looked at for activations
	std::vector<char> zone_used;      // steps whose logged zone was already used
	std::vector<uint32_t> chosen;     // activations chosen by a decision, not yet seen starting
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
		// the same card logged twice in a row: often one action declared twice (Duelingbook)
		for(size_t k = 0; k < line.steps.size(); ++k)
			repeat_.push_back(k > 0 && line.steps[k].code && line.steps[k].code == line.steps[k - 1].code &&
			                  line.steps[k].action == line.steps[k - 1].action);
	}

	// A logged repeat that couldn't be played isn't counted as a miss.
	bool repeat(size_t k) const { return k < repeat_.size() && repeat_[k]; }
	// A step that may be left out: not a miss, and skipping it doesn't use up the skip window.
	bool optional(size_t k) const { return k < line_.steps.size() && line_.steps[k].optional; }

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
			if(i.kind == Kind::Use) {
				// summoned, searched or sent by another card's effect: it turned up somewhere new
				bool moved = false;
				for(Zone z : {Zone::Field, Zone::Hand, Zone::Grave, Zone::Banished, Zone::Overlay})
					moved = moved || count_in(now, i.sid, z) > count_in(n.base, i.sid, z);
				if(!moved)
					return;
				n.match[n.next] = 1;
				++n.next;
				continue;
			}
			if(count_in(now, i.sid, i.zone) > count_in(n.base, i.sid, i.zone)) {
				n.match[n.next] = 1;
				++n.next;
				continue;
			}
			// A summon or "seen" step not visible yet: it may come from a later action, or the
			// video logged it out of order. Look one step past it before giving up on it.
			if(n.next + 1 < info_.size()) {
				const StepInfo& j = info_[n.next + 1];
				if(j.kind != Kind::Action && j.kind != Kind::Ignored && j.kind != Kind::Use &&
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
		if(dec.type == DecisionType::Chain) {
			bool hit = !candidates(dec, n, 0).empty();
			if(std::getenv("YGOE_REPLAY_TRACE") && dec.options.size() > 1) {
				std::fprintf(stderr, "chain window p0 at step %zu (%s): %s; options:", n.next,
				             n.next < info_.size() ? db_.name(info_[n.next].sid).c_str() : "-", hit ? "match" : "no match");
				for(auto& o : dec.options)
					std::fprintf(stderr, " [%s]", o.label.substr(0, 60).c_str());
				std::fprintf(stderr, "\n");
			}
			return hit;
		}
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
			if(i.kind == Kind::Action || i.kind == Kind::Summon || i.kind == Kind::Use) {
				for(size_t o = 0; o < dec.options.size(); ++o)
					if(option_matches(dec.options[o], line_.steps[k], i))
						out.emplace_back(o, k);
				if(!out.empty())
					break;
			}
			if(i.kind != Kind::Ignored && n.match[k] != 1 && !optional(k))
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
			if((i.kind == Kind::Action || i.kind == Kind::Use) && i.sid == sid &&
			   (line_.steps[k].action == "activate" || line_.steps[k].action == "chain" || line_.steps[k].action == "use"))
				return k;
			if(!optional(k))
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

	// Activations the rules start without a decision (a mandatory trigger: Angelechy Seneschal
	// summoned to the opponent's field by the combo) count as the logged step they are.
	void absorb_activations(const Duel& d, Node& n) const {
		const auto& ev = d.events();
		for(; n.events_seen < ev.size(); ++n.events_seen) {
			const Event& e = ev[n.events_seen];
			if(e.kind != Event::Chaining || !e.code)
				continue;
			auto c = std::find(n.chosen.begin(), n.chosen.end(), e.code);
			if(c != n.chosen.end()) {
				n.chosen.erase(c);  // chosen by a decision: matched (or not) where it was chosen
				continue;
			}
			size_t step = pending_activation(e.code, n);
			if(step == SIZE_MAX)
				continue;
			n.match[step] = 1;
			if(step == n.next) {
				n.next = step + 1;
				skipped_ahead(n);
			}
		}
	}

	// Plays every decision that isn't one of the player's actions. Returns true if any choice
	// was sampled (so trying again could give a different result).
	bool advance(Duel& d, Node& n, bool sample) {
		bool sampled = false;
		// set while an opponent-side activation from the log resolves: its choices (what it
		// summons, targets) follow the video's hints like the player's own
		bool opponent_acting = false;
		auto h = hints(n);
		for(int guard = 0; guard < 400; ++guard) {
			absorb_activations(d, n);
			if(d.ended() || d.turn() > 1)
				return sampled;
			const Decision& dec = d.decision();
			if(dec.options.empty())
				return sampled;
			// a chain window inside the action: what it has summoned, searched or sent so far
			// counts already, so the trigger the video logs next can be chained
			if(dec.player == 0 && dec.type == DecisionType::Chain && dec.options.size() > 1 && n.next < info_.size() &&
			   info_[n.next].kind != Kind::Action)
				absorb_seen(n, d.board());
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
			// The log names this card's activation, but it's the opponent's to make: a card the combo
			// gave them (Seneschal summoned to their field), or one a manual simulator lets either
			// player declare. Play it as logged.
			if(dec.player != 0) {
				size_t take_o = SIZE_MAX, step = SIZE_MAX;
				for(size_t o = 0; o < dec.options.size() && take_o == SIZE_MAX; ++o) {
					const Option& op = dec.options[o];
					if(op.kind != OptKind::Activate && op.kind != OptKind::Yes)
						continue;
					// only the very next logged step: the opponent acting early derails the line
					if((step = pending_activation(op.code, n)) != SIZE_MAX && step == n.next)
						take_o = o;
				}
				if(take_o != SIZE_MAX) {
					if(!take(d, n, take_o))
						return sampled;
					n.match[step] = 1;
					if(step == n.next) {
						n.next = step + 1;
						skipped_ahead(n);
					}
					opponent_acting = true;
					continue;
				}
			}
			bool chooser = dec.player == 0 || (opponent_acting && pass_index(dec) == SIZE_MAX);
			// The opponent choosing inside an effect of the combo's own card on their field (Angelechy
			// Seneschal: "each player can Special Summon ..."): what the log shows them doing, else pass.
			if(std::getenv("YGOE_REPLAY_TRACE") && dec.player != 0 && dec.type != DecisionType::Chain)
				std::fprintf(stderr, "opp decision type %d depth %d owner %d chooser %d: %s\n", static_cast<int>(dec.type), d.chain_depth(),
				             d.chain_last_owner(), chooser ? 1 : 0, dec.options.empty() ? "" : dec.options[0].label.substr(0, 60).c_str());
			// A zone to pick: the one the log shows for the card being placed.
			if(dec.type == DecisionType::Place && zone_pick(dec, n, pick)) {
				if(!take(d, n, pick))
					return sampled;
				continue;
			}
			if(dec.player != 0 && !chooser && dec.type != DecisionType::Chain && d.chain_depth() > 0 &&
			   d.chain_last_owner() == 0 && opponent_hint_pick(dec, n, pick)) {
				if(!take(d, n, pick))
					return sampled;
				continue;
			}
			if(dec.player != 0 && !chooser) {
				// goldfish: the opponent passes whenever it can
				opponent_acting = false;
				pick = pass_or(dec, best_policy(dec));
			} else if(dec.player != 0 && (dec.minor || dec.options.size() == 1)) {
				pick = best_policy(dec);
			} else if(dec.player == 0 && dec.type == DecisionType::Chain) {
				pick = pass_or(dec, 0);
			} else if(dec.player == 0 && (dec.minor || dec.options.size() == 1)) {
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
				if(std::getenv("YGOE_REPLAY_TRACE_CHOICES")) {
					std::fprintf(stderr, "choice p%d at step %zu:", dec.player, n.next);
					for(size_t o = 0; o < w.size(); ++o)
						std::fprintf(stderr, " [%s %.1f]", dec.options[o].label.substr(0, 40).c_str(), w[o]);
					std::fprintf(stderr, "\n");
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

	// The zone the log gives for a step around here (the action just taken, or the next few),
	// if it is offered. Each step's zone is used once.
	bool zone_pick(const Decision& dec, Node& n, size_t& pick) const {
		if(n.zone_used.size() < line_.steps.size())
			n.zone_used.resize(line_.steps.size(), 0);
		const size_t from = n.next > 2 ? n.next - 2 : 0;
		for(size_t k = from; k < line_.steps.size() && k < n.next + 5; ++k) {
			const VideoStep& s = line_.steps[k];
			if(!s.zone_loc || n.zone_used[k])
				continue;
			const uint8_t pl = static_cast<uint8_t>((s.by == 1 ? 1 : 0) ^ (s.zone_other ? 1 : 0));
			for(size_t o = 0; o < dec.options.size(); ++o) {
				const auto& r = dec.options[o].response;
				if(r.size() == 3 && r[0] == pl && r[1] == s.zone_loc && r[2] == s.zone_seq) {
					n.zone_used[k] = 1;
					pick = o;
					return true;
				}
			}
		}
		return false;
	}

	// The option naming the opponent's logged step closest to the next step, if any.
	bool opponent_hint_pick(const Decision& dec, const Node& n, size_t& pick) const {
		const size_t from = n.next > 3 ? n.next - 3 : 0;
		for(size_t k = from; k < line_.steps.size() && k < n.next + 8; ++k) {
			const VideoStep& s = line_.steps[k];
			if(s.by != 1 || !s.code)
				continue;
			const std::string nm = db_.name(db_.script_id(s.code));
			for(size_t o = 0; o < dec.options.size(); ++o)
				if(dec.options[o].code == db_.script_id(s.code) || mentions(dec.options[o].label, nm)) {
					pick = o;
					return true;
				}
			// a yes/no ("Special Summon ...?") before the card itself is picked
			if(dec.type == DecisionType::EffectYesNo || dec.type == DecisionType::YesNo)
				for(size_t o = 0; o < dec.options.size(); ++o)
					if(dec.options[o].kind == OptKind::Yes) {
						pick = o;
						return true;
					}
		}
		return false;
	}

	static size_t pass_index(const Decision& dec) {
		for(size_t o = 0; o < dec.options.size(); ++o)
			if(dec.options[o].kind == OptKind::Pass || dec.options[o].kind == OptKind::No)
				return o;
		return SIZE_MAX;
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
		const Option& op = dec.options[pick];
		const bool activation = op.code && (op.kind == OptKind::Activate || (op.kind == OptKind::Yes && dec.type == DecisionType::EffectYesNo));
		const uint32_t code = op.code;
		if(!d.respond(resp))
			return false;
		if(activation)
			n.chosen.push_back(code);
		n.responses.push_back(std::move(resp));
		n.steps.push_back(std::move(s));
		return true;
	}

	// After an advance: count what the video said would happen, score, sign.
	void finish_node(Duel& d, Node& n) {
		absorb_activations(d, n);
		Board now = d.board();
		absorb_seen(n, now);
		// steps found since the last point: the line is consistent up to here
		n.settled.resize(n.match.size(), 0);
		for(size_t k = 0; k < n.match.size(); ++k)
			if(n.match[k] == 1 && n.settled[k] == 0)
				n.settled[k] = n.responses.size();
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
			else if(n.match[k] == 0 && k < n.next && !repeat(k))
				++missed;
		}
		int on_board = 0;
		for(auto sid : end_sids_)
			if(count_in(now, sid, Zone::Field) > 0)
				++on_board;
		n.score = matched * 10.0 - missed * 3.0 + on_board * 2.0 - (n.stuck ? 20 : 0);
		std::ostringstream sig;
		sig.imbue(std::locale::classic());  // C number format, whatever locale the host app set
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
		if(cands.empty() && n.next < info_.size() && std::getenv("YGOE_REPLAY_TRACE")) {
			// where a replay got stuck: the step it waits for and what the engine offers instead
			std::fprintf(stderr, "stuck before step %zu (%s %s), score %.0f, after:\n", n.next,
			             line_.steps[n.next].action.c_str(), db_.name(info_[n.next].sid).c_str(), n.score);
			for(size_t k = n.steps.size() > 6 ? n.steps.size() - 6 : 0; k < n.steps.size(); ++k)
				std::fprintf(stderr, "    p%d %s\n", n.steps[k].player, n.steps[k].label.substr(0, 110).c_str());
			std::fprintf(stderr, "  offered to p%d (%d):\n", dec.player, static_cast<int>(dec.type));
			for(auto& o : dec.options)
				std::fprintf(stderr, "    - %s\n", o.label.substr(0, 110).c_str());
		}
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
				size_t before = m.steps.size();
				bool sampled = advance(e, m, t > 0);
				finish_node(e, m);
				if(std::getenv("YGOE_REPLAY_TRACE") && step >= 3 && step <= 4) {
					std::fprintf(stderr, "after step %zu try %d: next=%zu score=%.0f done=%d\n", step, t, m.next, m.score, m.done ? 1 : 0);
					for(size_t k = before > 0 ? before - 1 : 0; k < m.steps.size(); ++k)
						std::fprintf(stderr, "    p%d %s\n", m.steps[k].player, m.steps[k].label.substr(0, 100).c_str());
				}
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
	std::vector<bool> repeat_;
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
			if(best.match[k] == 0 && (r.repeat(k) || r.optional(k)))
				best.match[k] = -1;
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
		// Without the video's end board (text recognition can't see it), the steps alone decide,
		// with a higher bar; the replayed board becomes the line's end board.
		if(lc.steps_checked < 3)
			lc.problem = "too few steps could be checked";
		else if(want == 0 && (lc.steps_checked < 4 || step_share < 0.8))
			lc.problem = "only " + std::to_string(lc.steps_matched) + " of " + std::to_string(lc.steps_checked) +
			             " steps could be played by the rules";
		else if(step_share < 0.75)
			lc.problem = "only " + std::to_string(lc.steps_matched) + " of " + std::to_string(lc.steps_checked) +
			             " steps could be played by the rules";
		else if(want > 0 && lc.board_match < 0.75)
			lc.problem = "the replay ends on " + std::to_string(have) + " of the video's " + std::to_string(want) + " end-board cards";
		lc.verified = lc.problem.empty();
		// Not all of it, but the start played by the rules: that part is a real line too. A log from
		// a manual simulator (Duelingbook) can follow its player's clicks rather than the chain
		// rules, so a replay may agree only up to a point.
		if(!lc.verified) {
			size_t last = SIZE_MAX;
			int run = 0;
			for(size_t k = 0; k < best.match.size(); ++k) {
				if(best.match[k] < 0)
					continue;
				if(best.match[k] != 1)
					break;
				last = k;
				++run;
			}
			if(run >= 4 && last < best.settled.size() && best.settled[last] > 0) {
				lc.verified_steps = run;
				Trajectory part;
				part.responses.assign(best.responses.begin(), best.responses.begin() + static_cast<long>(best.settled[last]));
				part.steps.assign(best.steps.begin(), best.steps.begin() + static_cast<long>(best.settled[last]));
				Board pb;
				Duel pd(db, scripts);
				pd.set_declare_pool(r.pool());
				if(pd.replay(r.setup(), part.responses)) {
					pb = pd.board();
					auto split = split_line(db, scripts, r.pool(), r.setup(), part, hand_traps);
					for(auto& p : split.pieces)
						p.under = line.under;
					add_to_library(res.library, split, board_value(db, deck, pb, 0));
					lc.pieces = std::move(split.pieces);
					if(cfg.explain > 0) {
						ExplainConfig ec;
						ec.iterations = cfg.explain;
						ec.cancel = cfg.cancel;
						lc.choices = explain_line(db, scripts, deck, r.setup(), r.pool(), part, ec);
					}
					if(policy) {
						auto ex = policy->examples_from_line(scripts, r.pool(), r.setup(), part.responses, 0, 1.0f);
						examples.insert(examples.end(), ex.begin(), ex.end());
					}
				}
			}
		}
		if(lc.verified) {
			++verified_lines;
			lc.verified_steps = lc.steps_matched;
			auto split = split_line(db, scripts, r.pool(), r.setup(), t, hand_traps);
			for(auto& p : split.pieces)
				p.under = line.under;
			add_to_library(res.library, split, lc.value);
			if(policy) {
				auto ex = policy->examples_from_line(scripts, r.pool(), r.setup(), best.responses, 0, 2.0f);
				examples.insert(examples.end(), ex.begin(), ex.end());
			}
			lc.pieces = std::move(split.pieces);
			if(cfg.explain > 0) {
				ExplainConfig ec;
				ec.iterations = cfg.explain;
				ec.cancel = cfg.cancel;
				lc.choices = explain_line(db, scripts, deck, r.setup(), r.pool(), t, ec);
			}
		}
		res.lines.push_back(std::move(lc));
	}
	if(progress)
		progress(static_cast<int>(lines.size()), static_cast<int>(lines.size()));
	res.verified = verified_lines;
	sort_library(res.library, 200);
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
	o.imbue(std::locale::classic());  // C number format, whatever locale the host app set
	o << "{\"verified\":" << verified << ",\"lines\":[";
	for(size_t i = 0; i < lines.size(); ++i) {
		const auto& l = lines[i];
		o << (i ? "," : "") << "{\"id\":" << quote(l.id) << ",\"verified\":" << (l.verified ? "true" : "false")
		  << ",\"steps_checked\":" << l.steps_checked << ",\"steps_matched\":" << l.steps_matched << ",\"step_match\":[";
		for(size_t k = 0; k < l.step_match.size(); ++k)
			o << (k ? "," : "") << l.step_match[k];
		o << "],\"verified_steps\":" << l.verified_steps << ",\"board_match\":" << num(l.board_match) << ",\"problem\":" << quote(l.problem) << ",\"value\":" << num(l.value)
		  << ",\"moves\":[";
		for(size_t k = 0; k < l.moves.size(); ++k)
			o << (k ? "," : "") << quote(l.moves[k]);
		o << "],\"board\":[";
		for(size_t k = 0; k < l.board.size(); ++k) {
			const auto& c = l.board[k];
			o << (k ? "," : "") << "{\"code\":" << c.code << ",\"name\":" << quote(c.name) << ",\"where\":" << quote(c.where)
			  << ",\"note\":" << quote(c.note) << "}";
		}
		o << "],\"pieces\":" << pieces_json(db, l.pieces) << ",\"choices\":" << choices_json(db, l.choices) << "}";
	}
	o << "],\"library\":" << library_json(db, library, std::max(1, verified));
	if(!policy.empty())
		o << ",\"policy\":" << policy;
	o << "}";
	return o.str();
}

}  // namespace ygoe
