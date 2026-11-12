#include "combo.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

#include "json.h"
#include "ocgapi_constants.h"
#include "profile.h"
#include "reference.h"
#include "search.h"

namespace ygoe {

namespace {

// Most played hand traps (TCG), used when the opponent's deck is unknown.
const uint32_t kDefaultHandTraps[] = {
	14558127,  // Ash Blossom & Joyous Spring
	97268402,  // Effect Veiler
	10045474,  // Infinite Impermanence
	59438930,  // Ghost Ogre & Snow Rabbit
	94145021,  // Droll & Lock Bird
	27204311,  // Nibiru, the Primal Being
	42141493,  // Mulcharmy Fuwalos
	73642296,  // Ghost Belle & Haunted Mansion
};

bool is_pass(const std::string& l) { return l == "Don't respond"; }

// Best continuation value from a search root: you maximise; if the opponent decides first
// (e.g. picks the target of their hand trap), they pick what is worst for you.
const Trajectory* robust_best(const SearchOutput& out, uint8_t me, double& value) {
	const Trajectory* pick = nullptr;
	value = -1;
	if(out.moves.empty())
		return nullptr;
	auto best_of = [](const MoveStats& m) -> const Trajectory* {
		const Trajectory* b = nullptr;
		for(auto& t : m.best)
			if(!b || t.reward > b->reward)
				b = &t;
		return b;
	};
	if(out.root_player == me) {
		for(auto& m : out.moves)
			if(auto t = best_of(m); t && t->reward > value) {
				value = t->reward;
				pick = t;
			}
	} else {
		int most = out.moves.front().visits;
		double worst = 2;
		for(auto& m : out.moves) {
			if(m.visits < std::max(3, most / 5))
				continue;
			if(auto t = best_of(m); t && t->reward < worst) {
				worst = t->reward;
				pick = t;
			}
		}
		value = pick ? worst : -1;
	}
	return pick;
}

}  // namespace

std::vector<uint32_t> likely_hand_traps(const CardDB& db, const Deck* opp, int max) {
	std::vector<uint32_t> out;
	if(opp) {
		// Their non-engine cards that interrupt from the hand. Main Deck copies count fully,
		// Side Deck copies half (they are only in some games).
		auto sets = dominant_setcodes(db, *opp);
		std::map<uint32_t, double> copies;
		for(auto* list : {&opp->main, &opp->side})
			for(auto code : *list) {
				auto c = db.get(db.script_id(code));
				if(!c)
					continue;
				auto role = classify(db, code, sets);
				if(role.engine)
					continue;
				bool interrupts = false;
				for(auto& in : role.interrupts)
					if((in.range & LOCATION_HAND) && in.kind != Interrupt::Extender && in.kind != Interrupt::Other)
						interrupts = true;
				// Traps that can be activated from the hand (Infinite Impermanence, Dominus...).
				bool trap_from_hand = (c->type & TYPE_TRAP) && db.text(c->code).find("activate this card from your hand") != std::string::npos;
				if(interrupts || trap_from_hand)
					copies[code] += list == &opp->main ? 1.0 : 0.5;
			}
		std::vector<std::pair<double, uint32_t>> ranked;
		for(auto& [code, n] : copies)
			ranked.push_back({n, code});
		std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
		for(auto& [n, code] : ranked)
			if(static_cast<int>(out.size()) < max)
				out.push_back(code);
		if(!out.empty())
			return out;
	}
	for(auto code : kDefaultHandTraps)
		if(db.get(code) && static_cast<int>(out.size()) < max)
			out.push_back(code);
	return out;
}

ComboTree build_combo_tree(const CardDB& db, ScriptCache& scripts, const Deck& mine, const std::vector<uint32_t>& hand,
                           const Deck* opponent, const ComboTreeConfig& cfg) {
	ComboTree tree;
	tree.opponent_known = opponent != nullptr;
	const Deck& theirs = opponent ? *opponent : mine;
	Evaluator eval(db, mine, theirs);
	if(!opponent)
		eval.set_profile(1, generic_profile());
	auto hts = likely_hand_traps(db, opponent, cfg.max_hand_traps);
	auto cancelled = [&] { return cfg.cancel && cfg.cancel->load(); };
	const int total_steps = 1 + static_cast<int>(hts.size());
	int step = 0;
	auto report = [&](const std::string& phase) {
		if(cfg.progress)
			cfg.progress(phase, step, total_steps);
	};

	// Main line: going first, no interaction.
	Scenario sc;
	sc.decks[0] = mine;
	sc.decks[1] = theirs;
	sc.hand[0] = hand;
	sc.hand_known[0] = sc.hand_known[1] = true;
	sc.me = 0;
	sc.seed = cfg.seed;
	auto pool = declare_pool(sc);
	Searcher searcher(db, scripts, eval, pool);
	RootState root{make_setup(sc, 0), {}};
	report("main line");
	SearchConfig mc;
	mc.iterations = cfg.iterations;
	mc.seconds = 120;
	mc.horizon_turns = 1;
	mc.cancel = cfg.cancel;
	mc.guide = cfg.guide;
	mc.policy = cfg.policy;
	auto out = searcher.run({root}, 0, mc);
	const Trajectory* best = nullptr;
	for(auto& m : out.my_moves)
		for(auto& t : m.best)
			if(!best || t.reward > best->reward)
				best = &t;
	if(!best) {
		tree.cancelled = cancelled();
		return tree;
	}
	Trajectory main = *best;
	searcher.defer_sets(root, main, 0, 1);
	searcher.drop_idle_summons(root, main, 0, 1);
	searcher.drop_idle_responses(root, main, 0, 1);
	tree.main_score = main.reward;
	tree.main_moves = describe(main, 0, true);
	Board end;
	if(searcher.final_board(root, main, end)) {
		tree.main_board = summarize_cards(db, end, 0);
		tree.main_value = board_value(db, mine, end, 0);
	}
	LineSplit main_split = split_line(db, scripts, pool, root.setup, main, hts);
	tree.main_parts = main_split.pieces;
	++step;

	// Index of each of your non-trivial steps in describe()'s numbering, to name choke points.
	for(uint32_t ht : hts) {
		if(cancelled())
			break;
		ComboBranch br;
		br.hand_trap = db.official_id(db.script_id(ht));
		br.hand_trap_name = db.name(db.script_id(ht));
		report("vs " + br.hand_trap_name);

		Scenario sh = sc;
		sh.hand[1] = {ht};
		RootState hroot{make_setup(sh, 0), {}};
		const uint32_t ht_id = db.script_id(ht);

		// Replay the main line with the hand trap available; record every window to use it.
		struct Choke {
			std::vector<std::vector<uint8_t>> history;  // responses up to the window
			std::vector<uint8_t> use;                   // the response that activates it
			std::string after;                          // your last action before the window
			int move_index;
		};
		std::vector<Choke> chokes;
		{
			Duel d(db, scripts);
			d.set_declare_pool(pool);
			if(!d.replay(hroot.setup, hroot.history))
				continue;
			std::vector<std::vector<uint8_t>> hist;
			std::string last_action;
			int move_index = -1;
			for(const Step& s : main.steps) {
				// Extra windows can appear now that the opponent holds a card: pass them.
				int guard = 0;
				while(!d.ended() && d.decision().player != s.player && guard++ < 20) {
					const Decision& dec = d.decision();
					if(dec.player == 1)
						for(const auto& o : dec.options)
							if(o.kind == OptKind::Activate && o.code == ht_id)
								chokes.push_back({hist, o.response, last_action, move_index});
					size_t pass = 0;
					for(size_t o = 0; o < dec.options.size(); ++o)
						if(dec.options[o].kind == OptKind::Pass)
							pass = o;
					hist.push_back(dec.options[pass].response);
					if(!d.respond(dec.options[pass].response))
						break;
				}
				if(d.ended())
					break;
				const Decision& dec = d.decision();
				if(dec.player == 1) {
					for(const auto& o : dec.options)
						if(o.kind == OptKind::Activate && o.code == ht_id)
							chokes.push_back({hist, o.response, last_action, move_index});
				}
				size_t pick = SIZE_MAX;
				for(size_t o = 0; o < dec.options.size(); ++o)
					if(dec.options[o].label == s.label) {
						pick = o;
						break;
					}
				if(pick == SIZE_MAX && (s.minor || is_pass(s.label)))
					pick = 0;
				if(pick == SIZE_MAX)
					break;  // the line no longer replays from here (the card changed something)
				if(s.player == 0 && !s.minor && !is_pass(s.label) &&
				   (s.type == DecisionType::Idle || s.type == DecisionType::Chain || s.type == DecisionType::EffectYesNo)) {
					last_action = s.label;
					++move_index;
				}
				hist.push_back(dec.options[pick].response);
				if(!d.respond(dec.options[pick].response))
					break;
			}
		}
		br.choke_points = static_cast<int>(chokes.size());
		if(chokes.empty()) {
			br.stops_nothing = true;
			br.score = tree.main_score;
			br.value = tree.main_value;
			tree.branches.push_back(std::move(br));
			++step;
			continue;
		}
		// Try up to max_choke_points windows, spread over the line (one per action it follows).
		std::vector<Choke> tried;
		{
			std::vector<Choke> distinct;
			for(auto& c : chokes)
				if(distinct.empty() || distinct.back().move_index != c.move_index)
					distinct.push_back(c);
			if(static_cast<int>(distinct.size()) <= cfg.max_choke_points) {
				tried = distinct;
			} else {
				for(int k = 0; k < cfg.max_choke_points; ++k)
					tried.push_back(distinct[k * (distinct.size() - 1) / (cfg.max_choke_points - 1)]);
			}
		}
		double worst = 2;
		for(const Choke& c : tried) {
			if(cancelled())
				break;
			RootState r{hroot.setup, c.history};
			r.history.push_back(c.use);
			SearchConfig bc;
			bc.iterations = cfg.branch_iterations;
			bc.seconds = 30;
			bc.horizon_turns = 1;
			bc.cancel = cfg.cancel;
			bc.guide = cfg.guide;
			bc.policy = cfg.policy;
			auto bo = searcher.run({r}, 0, bc);
			double v = -1;
			const Trajectory* t = robust_best(bo, 0, v);
			if(!t)
				continue;
			if(v < worst) {
				worst = v;
				Trajectory cont = *t;
				searcher.defer_sets(r, cont, 0, 1);
				br.used_on = c.after.empty() ? "the start of your turn" : c.after;
				br.used_at_step = c.move_index;
				br.hits_part = main_split.piece_of(std::max(0, c.move_index));
				br.continuation = describe(cont, 0, true);
				br.score = v;
				Board b;
				if(searcher.final_board(r, cont, b)) {
					br.board = summarize_cards(db, b, 0);
					br.value = board_value(db, mine, b, 0);
				}
			}
		}
		if(worst <= 1.5)
			tree.branches.push_back(std::move(br));
		++step;
	}
	std::stable_sort(tree.branches.begin(), tree.branches.end(), [](auto& a, auto& b) {
		if(a.stops_nothing != b.stops_nothing)
			return !a.stops_nothing;
		return a.score < b.score;
	});
	tree.cancelled = cancelled();
	step = total_steps;
	report("done");
	return tree;
}

std::string ComboTree::to_json(const CardDB& db) const {
	using json::quote;
	auto num = [](double v) {
		char b[32];
		std::snprintf(b, sizeof b, "%.3f", v);
		return std::string(b);
	};
	auto cards = [&](const std::vector<CardSummary>& v) {
		std::string o = "[";
		for(size_t i = 0; i < v.size(); ++i)
			o += (i ? "," : "") + std::string("{\"code\":") + std::to_string(v[i].code) + ",\"name\":" + quote(v[i].name) +
			     ",\"where\":" + quote(v[i].where) + ",\"note\":" + quote(v[i].note) + "}";
		return o + "]";
	};
	auto strings = [&](const std::vector<std::string>& v) {
		std::string o = "[";
		for(size_t i = 0; i < v.size(); ++i)
			o += (i ? "," : "") + quote(v[i]);
		return o + "]";
	};
	std::ostringstream o;
	o << "{\"opponent_known\":" << (opponent_known ? "true" : "false") << ",\"main\":{\"moves\":" << strings(main_moves)
	  << ",\"board\":" << cards(main_board) << ",\"value\":" << num(main_value) << ",\"score\":" << num(main_score)
	  << ",\"parts\":" << pieces_json(db, main_parts) << "},\"branches\":[";
	for(size_t i = 0; i < branches.size(); ++i) {
		const auto& b = branches[i];
		o << (i ? "," : "") << "{\"hand_trap\":{\"code\":" << b.hand_trap << ",\"name\":" << quote(b.hand_trap_name)
		  << "},\"stops_nothing\":" << (b.stops_nothing ? "true" : "false") << ",\"used_on\":" << quote(b.used_on)
		  << ",\"used_at_step\":" << b.used_at_step << ",\"choke_points\":" << b.choke_points << ",\"hits_part\":" << b.hits_part
		  << ",\"continuation\":" << strings(b.continuation) << ",\"board\":" << cards(b.board)
		  << ",\"value\":" << num(b.value) << ",\"score\":" << num(b.score) << "}";
	}
	o << "],\"cancelled\":" << (cancelled ? "true" : "false") << "}";
	return o.str();
}

}  // namespace ygoe
