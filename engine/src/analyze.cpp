#include "analyze.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "ocgapi_constants.h"

namespace ygoe {

namespace {

std::string fmt(double v, int prec = 2) {
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.*f", prec, v);
	return buf;
}

std::string json_str(const std::string& s) {
	std::string o = "\"";
	for(char c : s) {
		switch(c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		default:
			if(static_cast<unsigned char>(c) < 0x20) {
				char b[8];
				std::snprintf(b, sizeof b, "\\u%04x", c);
				o += b;
			} else {
				o += c;
			}
		}
	}
	return o + "\"";
}

std::string json_cards(const std::vector<CardSummary>& v) {
	std::string o = "[";
	for(size_t i = 0; i < v.size(); ++i) {
		if(i)
			o += ",";
		o += "{\"code\":" + std::to_string(v[i].code) + ",\"name\":" + json_str(v[i].name) +
		     ",\"where\":" + json_str(v[i].where) + ",\"note\":" + json_str(v[i].note) + "}";
	}
	return o + "]";
}

std::string json_strings(const std::vector<std::string>& v) {
	std::string o = "[";
	for(size_t i = 0; i < v.size(); ++i)
		o += (i ? "," : "") + json_str(v[i]);
	return o + "]";
}

std::string json_profile(const DeckProfile& p, const CardDB* db = nullptr) {
	return "{\"name\":" + json_str(p.name) + ",\"monster_effects\":" + fmt(p.monster_effects, 3) +
	       ",\"hand_effects\":" + fmt(p.hand_effects, 3) + ",\"gy_effects\":" + fmt(p.gy_effects, 3) +
	       ",\"spell_trap\":" + fmt(p.spell_trap, 3) + ",\"searches\":" + fmt(p.searches, 3) +
	       ",\"special_summons\":" + fmt(p.special_summons, 3) + "}";
}

std::vector<CardSummary> notes_to_cards(const CardDB& db, const std::vector<BoardNote>& notes, uint8_t player) {
	std::vector<CardSummary> out;
	for(const auto& n : notes) {
		if(n.player != player)
			continue;
		out.push_back({n.code, db.name(n.code), n.where, std::string(n.what) + " (" + fmt(n.value) + ")"});
	}
	return out;
}

}  // namespace

std::vector<CardSummary> summarize_cards(const CardDB& db, const Board& b, uint8_t p, bool include_hand) {
	std::vector<CardSummary> out;
	for(const auto& c : b.mzone[p])
		if(c.code)
			out.push_back({c.code, db.name(c.code), "Monster Zone",
			               (c.pos & POS_FACEDOWN) ? "face-down" : (std::to_string(c.atk) + " ATK") +
			                   (c.overlay.empty() ? "" : ", " + std::to_string(c.overlay.size()) + " material")});
	for(const auto& c : b.szone[p])
		if(c.code)
			out.push_back({c.code, db.name(c.code), c.seq == 5 ? "Field Zone" : "S/T Zone", (c.pos & POS_FACEDOWN) ? "set" : "face-up"});
	if(include_hand)
		for(const auto& c : b.hand[p])
			out.push_back({c.code, db.name(c.code), "hand", ""});
	return out;
}

AnalysisResult analyze(const CardDB& db, ScriptCache& scripts, const Scenario& sc, const AnalysisConfig& cfg,
                       const std::function<void(const std::string&)>& log) {
	AnalysisResult res;
	res.me = sc.me;
	res.mode = sc.knowledge == Knowledge::Real ? "real" : "practice";
	Evaluator eval(db, sc.decks[0], sc.decks[1]);
	res.my_profile = eval.profile(sc.me);
	res.opp_profile = eval.profile(1 - sc.me);
	Searcher searcher(db, scripts, eval, declare_pool(sc));

	const bool hidden = sc.knowledge == Knowledge::Real || !sc.hand_known[0] || !sc.hand_known[1];
	const int dets = hidden ? std::max(1, cfg.determinizations) : std::max(1, std::min(cfg.determinizations, 4));
	std::vector<RootState> roots;
	for(int d = 0; d < dets; ++d)
		roots.push_back({make_setup(sc, static_cast<uint64_t>(d)), {}});

	// Current position, from the first sample.
	uint8_t first_turn_player = 0;
	{
		Duel duel(db, scripts);
		duel.set_declare_pool(declare_pool(sc));
		if(duel.replay(roots[0].setup, roots[0].history)) {
			first_turn_player = duel.turn_player();
			auto ev = eval.evaluate(duel.board(), sc.me, duel.turn_player(), duel.ended(), duel.winner(), true);
			res.current_score = ev.score;
			res.current_interrupts = notes_to_cards(db, ev.notes, sc.me);
		}
	}

	// Phase A: search through the end of the player's next turn.
	SearchConfig a = cfg.search;
	if(a.horizon_turns <= 0)
		a.horizon_turns = first_turn_player == sc.me ? 1 : 2;
	if(log)
		log("searching " + std::to_string(a.iterations) + " iterations over " + std::to_string(roots.size()) +
		    " sample(s), horizon " + std::to_string(a.horizon_turns) + " turn(s)");
	auto out = searcher.run(roots, sc.me, a, [&](int n, double s) {
		if(log && n % 500 == 0)
			log("  " + std::to_string(n) + " iterations, " + fmt(s, 1) + "s");
	});
	res.iterations = out.iterations;
	res.seconds = out.seconds;

	for(const auto& mv : out.my_moves) {
		if(static_cast<int>(res.lines.size()) >= cfg.lines)
			break;
		if(mv.best.empty())
			continue;
		LineReport line;
		line.first_move = mv.label;
		line.visits = mv.visits;
		line.own_turn_score = mv.mean;
		const Trajectory& top = mv.best.front();
		line.moves = describe(top, sc.me, true);
		Board end;
		if(searcher.final_board(roots[top.root], top, end)) {
			uint8_t next = 1 - sc.me;
			auto ev = eval.evaluate(end, sc.me, next, false, -1, true);
			line.own_turn_score = 0.5 * mv.mean + 0.5 * ev.score;
			line.end_board = summarize_cards(db, end, sc.me);
			line.interrupts = notes_to_cards(db, ev.notes, sc.me);
		}
		line.combined = line.own_turn_score;
		res.lines.push_back(std::move(line));
	}

	// Phase B: let the opponent play their turn against each candidate board.
	if(cfg.verify) {
		size_t li = 0;
		for(const auto& mv : out.my_moves) {
			if(li >= res.lines.size())
				break;
			if(mv.best.empty() || mv.label != res.lines[li].first_move)
				continue;
			LineReport& line = res.lines[li++];
			std::vector<RootState> vroots;
			std::vector<size_t> used_roots;
			for(const auto& t : mv.best) {
				if(static_cast<int>(vroots.size()) >= cfg.verify_trajectories)
					break;
				if(std::find(used_roots.begin(), used_roots.end(), t.root) != used_roots.end() && hidden)
					continue;
				used_roots.push_back(t.root);
				RootState r = roots[t.root];
				r.history.insert(r.history.end(), t.responses.begin(), t.responses.end());
				vroots.push_back(std::move(r));
			}
			if(vroots.empty())
				continue;
			SearchConfig b = cfg.search;
			b.horizon_turns = 1;
			b.iterations = cfg.verify_iterations * static_cast<int>(vroots.size());
			b.seconds = cfg.search.seconds / 2;
			if(log)
				log("checking line " + std::to_string(li) + " against the opponent's turn");
			auto vo = searcher.run(vroots, sc.me, b);
			if(vo.moves.empty())
				continue;
			// Value for the side to move at the verification root: the opponent picks the move
			// that is worst for us among its well-explored moves.
			const MoveStats* pick = &vo.moves.front();
			if(vo.root_player != sc.me) {
				for(const auto& m : vo.moves)
					if(m.visits >= std::max(10, vo.moves.front().visits / 4) && m.mean < pick->mean)
						pick = &m;
			}
			line.after_opponent = pick->mean;
			if(!pick->best.empty()) {
				// Their strongest reply: the lowest-scoring trajectory for us.
				// Prefer replies against the exact line shown above (verification root 0).
				const Trajectory* worst = nullptr;
				for(const auto& t : pick->best)
					if(t.root == 0 && (!worst || t.reward < worst->reward))
						worst = &t;
				if(!worst)
					for(const auto& t : pick->best)
						if(!worst || t.reward < worst->reward)
							worst = &t;
				line.opponent_reply = describe(*worst, static_cast<uint8_t>(1 - sc.me), true, "You: ");
				Board after;
				if(searcher.final_board(vroots[worst->root], *worst, after))
					line.board_after = summarize_cards(db, after, sc.me);
			}
			line.combined = 0.35 * line.own_turn_score + 0.65 * line.after_opponent;
		}
		std::stable_sort(res.lines.begin(), res.lines.end(), [](auto& x, auto& y) { return x.combined > y.combined; });
	}
	return res;
}

std::string AnalysisResult::to_json() const {
	std::ostringstream o;
	o << "{\"me\":" << int(me) << ",\"mode\":" << json_str(mode) << ",\"current_score\":" << fmt(current_score, 3)
	  << ",\"current_interrupts\":" << json_cards(current_interrupts) << ",\"my_profile\":" << json_profile(my_profile)
	  << ",\"opponent_profile\":" << json_profile(opp_profile) << ",\"iterations\":" << iterations
	  << ",\"seconds\":" << fmt(seconds, 1) << ",\"lines\":[";
	for(size_t i = 0; i < lines.size(); ++i) {
		const auto& l = lines[i];
		o << (i ? "," : "") << "{\"first_move\":" << json_str(l.first_move) << ",\"moves\":" << json_strings(l.moves)
		  << ",\"own_turn_score\":" << fmt(l.own_turn_score, 3) << ",\"after_opponent\":" << fmt(l.after_opponent, 3)
		  << ",\"combined\":" << fmt(l.combined, 3) << ",\"visits\":" << l.visits
		  << ",\"end_board\":" << json_cards(l.end_board) << ",\"interrupts\":" << json_cards(l.interrupts)
		  << ",\"opponent_reply\":" << json_strings(l.opponent_reply) << ",\"board_after\":" << json_cards(l.board_after)
		  << "}";
	}
	o << "]}";
	return o.str();
}

std::string AnalysisResult::to_text() const {
	std::ostringstream o;
	o << "Mode: " << mode << " (" << (mode == "real" ? "own hand + public info only; opponent cards shown in lines are one sampled hand" : "both hands known") << ")\n";
	o << "Opponent deck: " << opp_profile.name << "\n  " << opp_profile.summary() << "\n";
	o << "Current position: " << fmt(current_score * 100, 0) << "/100 for you\n";
	for(const auto& c : current_interrupts)
		o << "  holding " << c.name << " (" << c.where << "): " << c.note << "\n";
	o << "Searched " << iterations << " iterations in " << fmt(seconds, 1) << "s\n";
	for(size_t i = 0; i < lines.size(); ++i) {
		const auto& l = lines[i];
		o << "\n#" << (i + 1) << "  " << l.first_move << "\n";
		o << "   score " << fmt(l.combined * 100, 0) << "/100  (end of your turn " << fmt(l.own_turn_score * 100, 0);
		if(l.after_opponent >= 0)
			o << ", after their best turn " << fmt(l.after_opponent * 100, 0);
		o << "; " << l.visits << " visits)\n";
		o << "   Line:\n";
		for(const auto& m : l.moves)
			o << "     - " << m << "\n";
		o << "   End board:\n";
		for(const auto& c : l.end_board)
			o << "     " << c.name << " [" << c.where << (c.note.empty() ? "" : ", " + c.note) << "]\n";
		if(!l.interrupts.empty()) {
			o << "   Interruptions vs this deck:\n";
			for(const auto& c : l.interrupts)
				o << "     " << c.name << " (" << c.where << "): " << c.note << "\n";
		}
		if(!l.opponent_reply.empty()) {
			o << "   Their strongest reply found:\n";
			for(size_t k = 0; k < l.opponent_reply.size() && k < 14; ++k)
				o << "     - " << l.opponent_reply[k] << "\n";
			if(l.opponent_reply.size() > 14)
				o << "     ... (" << l.opponent_reply.size() - 14 << " more steps)\n";
		}
	}
	return o.str();
}

}  // namespace ygoe
