#include "explain.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

#include "evaluate.h"
#include "json.h"
#include "profile.h"

namespace ygoe {

namespace {

bool pass_like(const std::string& l) {
	return l == "Don't respond" || l.rfind("Don't use", 0) == 0 || l.rfind("No:", 0) == 0 || l == "Done selecting" ||
	       l == "Cancel" || l == "Finish";
}

bool choice_type(DecisionType t) {
	return t == DecisionType::Card || t == DecisionType::Option || t == DecisionType::EffectYesNo ||
	       t == DecisionType::Tribute || t == DecisionType::Sum;
}

bool action_type(DecisionType t) {
	return t == DecisionType::Idle || t == DecisionType::Chain || t == DecisionType::EffectYesNo;
}

// Up to four names; the main differences, not every card.
std::string names(const std::vector<CardSummary>& v) {
	std::string o;
	const size_t n = std::min<size_t>(v.size(), 4);
	for(size_t i = 0; i < n; ++i)
		o += (i ? (i + 1 == n && n == v.size() ? " and " : ", ") : "") + v[i].name;
	if(v.size() > n)
		o += " and " + std::to_string(v.size() - n) + " more";
	return o;
}

// Cards of `a` that `b` doesn't have (by name, counting copies).
std::vector<CardSummary> minus(const std::vector<CardSummary>& a, const std::vector<CardSummary>& b) {
	std::vector<CardSummary> out;
	std::vector<char> used(b.size(), 0);
	for(const auto& c : a) {
		bool found = false;
		for(size_t i = 0; i < b.size(); ++i)
			if(!used[i] && b[i].name == c.name) {
				used[i] = 1;
				found = true;
				break;
			}
		if(!found)
			out.push_back(c);
	}
	return out;
}

struct Point {
	size_t step;
	std::string action;
	uint32_t action_code = 0;
	std::vector<Option> options;  // the choices, without pass-like ones (one per label)
	size_t chosen = 0;
};

}  // namespace

double net_value(const CardDB& db, const Deck& deck, const Board& b) {
	Evaluator e(db, deck, deck);
	e.set_profile(0, generic_profile());
	e.set_profile(1, generic_profile());
	auto ev = e.evaluate(b, 0, 1);
	return ev.value[0] - ev.value[1];
}

std::vector<ChoiceExplanation> explain_line(const CardDB& db, ScriptCache& scripts, const Deck& deck, const DuelSetup& setup,
                                            const std::vector<uint32_t>& pool, const Trajectory& line, const ExplainConfig& cfg) {
	std::vector<ChoiceExplanation> out;
	// Walk the line once, noting its choice points.
	std::vector<Point> points;
	{
		Duel d(db, scripts);
		d.set_declare_pool(pool);
		if(!d.replay(setup, {}))
			return out;
		std::string action;
		uint32_t action_code = 0;
		for(size_t k = 0; k < line.responses.size() && k < line.steps.size(); ++k) {
			if(d.ended())
				break;
			const Decision& dec = d.decision();
			const Step& s = line.steps[k];
			size_t picked = SIZE_MAX;
			for(size_t o = 0; o < dec.options.size(); ++o)
				if(dec.options[o].response == line.responses[k]) {
					picked = o;
					break;
				}
			if(s.player == 0 && !s.minor && picked != SIZE_MAX) {
				if(action_type(s.type) && !pass_like(s.label) && s.type != DecisionType::EffectYesNo) {
					action = s.label;
					action_code = dec.options[picked].code;
				} else if(choice_type(s.type) && !pass_like(s.label) && static_cast<int>(points.size()) < cfg.max_choices) {
					Point p;
					p.step = k;
					p.action = action;
					p.action_code = action_code;
					std::set<std::string> seen;
					for(size_t o = 0; o < dec.options.size(); ++o) {
						const Option& op = dec.options[o];
						if(pass_like(op.label) && o != picked)
							continue;
						if(!seen.insert(op.label).second && o != picked)
							continue;
						if(o == picked)
							p.chosen = p.options.size();
						p.options.push_back(op);
					}
					if(p.options.size() >= 2 && static_cast<int>(p.options.size()) <= cfg.max_options)
						points.push_back(std::move(p));
				}
			}
			if(!d.respond(line.responses[k]))
				break;
		}
	}
	if(points.empty())
		return out;

	Evaluator eval(db, deck, deck);
	eval.set_profile(0, generic_profile());
	eval.set_profile(1, generic_profile());
	Searcher searcher(db, scripts, eval, pool);
	// The line's own end, for its choices.
	double line_net = -1e9;
	std::vector<CardSummary> line_mine, line_theirs;
	{
		Duel d(db, scripts);
		d.set_declare_pool(pool);
		if(d.replay(setup, line.responses)) {
			line_net = net_value(db, deck, d.board());
			line_mine = summarize_cards(db, d.board(), 0);
			line_theirs = summarize_cards(db, d.board(), 1, false);
		}
	}

	for(const auto& p : points) {
		if(cfg.cancel && cfg.cancel->load())
			break;
		ChoiceExplanation ex;
		ex.step = static_cast<int>(p.step);
		ex.action = p.action;
		ex.action_code = p.action_code;
		ex.chosen = p.options[p.chosen].label;
		ex.chosen_code = p.options[p.chosen].code;
		for(size_t o = 0; o < p.options.size(); ++o) {
			ChoiceOption co;
			co.label = p.options[o].label;
			co.code = p.options[o].code;
			co.chosen = o == p.chosen;
			RootState root{setup, {}};
			root.history.assign(line.responses.begin(), line.responses.begin() + static_cast<long>(p.step));
			root.history.push_back(p.options[o].response);
			Duel d(db, scripts);
			d.set_declare_pool(pool);
			if(!d.replay(root.setup, root.history))
				continue;
			Board end = d.board();
			bool have = false;
			if(!d.ended()) {
				SearchConfig sc;
				sc.iterations = cfg.iterations;
				sc.seconds = 20;
				sc.horizon_turns = 1;
				sc.cancel = cfg.cancel;
				auto res = searcher.run({root}, 0, sc);
				const Trajectory* best = nullptr;
				for(const auto& m : res.my_moves)
					for(const auto& t : m.best)
						if(!best || t.reward > best->reward)
							best = &t;
				if(best && searcher.final_board(root, *best, end))
					have = true;
			}
			(void)have;
			co.value = net_value(db, deck, end);
			co.mine = summarize_cards(db, end, 0);
			co.theirs = summarize_cards(db, end, 1, false);
			// The line itself continues from its choice: count its own end too.
			if(co.chosen && line_net > co.value) {
				co.value = line_net;
				co.mine = line_mine;
				co.theirs = line_theirs;
			}
			ex.options.push_back(std::move(co));
		}
		if(ex.options.size() < 2)
			continue;
		std::stable_sort(ex.options.begin(), ex.options.end(), [](const ChoiceOption& a, const ChoiceOption& b) { return a.value > b.value; });
		const ChoiceOption* chosen = nullptr;
		const ChoiceOption* other = nullptr;
		for(const auto& o : ex.options) {
			if(o.chosen && !chosen)
				chosen = &o;
			else if(!o.chosen && !other)
				other = &o;
		}
		if(!chosen || !other)
			continue;
		ex.margin = chosen->value - other->value;
		ex.understood = ex.margin >= -cfg.tolerance;

		// In words: what the line's choice ends with that the best other option doesn't, and
		// what the chosen card goes on to do in the line.
		auto gain = minus(chosen->mine, other->mine);
		auto lose = minus(other->mine, chosen->mine);
		auto opp_more = minus(other->theirs, chosen->theirs);
		auto opp_less = minus(chosen->theirs, other->theirs);
		std::ostringstream w;
		char num[64];
		std::snprintf(num, sizeof num, "%.1f vs %.1f", chosen->value, other->value);
		const std::string cname = ex.chosen;
		if(std::abs(ex.margin) < 0.1)
			w << "About as good as " << other->label << " (" << num << ")";
		else if(ex.understood && ex.margin > 0)
			w << "Better than " << other->label << " (" << num << ")";
		else if(ex.understood)
			w << "Close to " << other->label << " (" << num << ")";
		else
			w << "The engine prefers " << other->label << " (" << num << "): it doesn't see yet what this choice adds";
		std::vector<std::string> parts;
		if(!gain.empty())
			parts.push_back("you end with " + names(gain));
		if(!lose.empty())
			parts.push_back((gain.empty() ? "you'd end with " : "instead of ") + names(lose) + (gain.empty() ? " with the other option" : ""));
		if(!opp_more.empty())
			parts.push_back("the other option leaves the opponent " + names(opp_more));
		if(!opp_less.empty())
			parts.push_back("the opponent gets " + names(opp_less));
		if(!parts.empty()) {
			w << ": ";
			for(size_t i = 0; i < parts.size(); ++i)
				w << (i ? "; " : "") << parts[i];
		}
		// What the chosen card does next in the line
		if(ex.chosen_code) {
			const std::string nm = db.name(ex.chosen_code);
			for(size_t k = p.step + 1; k < line.steps.size(); ++k) {
				const Step& s = line.steps[k];
				if(s.player != 0 || s.minor)
					continue;
				const std::string& l = s.label;
				if(l.find(nm) == std::string::npos ||
				   !(l.rfind("Activate " + nm, 0) == 0 || l.rfind("Chain " + nm, 0) == 0 || l.rfind("Use " + nm, 0) == 0))
					continue;
				std::string what = l;
				if(auto dash = what.find(" — "); dash != std::string::npos)
					what = what.substr(dash + 5);
				if(what.size() > 120)
					what = what.substr(0, 117) + "...";
				// the choices made inside that effect
				std::string inside;
				for(size_t j = k + 1; j < line.steps.size() && j < k + 4; ++j) {
					const Step& n = line.steps[j];
					if(n.player != 0)
						continue;
					if(action_type(n.type) && n.type != DecisionType::EffectYesNo)
						break;
					if(!n.minor && !pass_like(n.label) && n.label.find('?') == std::string::npos)
						inside += (inside.empty() ? "" : ", ") + n.label;
				}
				w << ". Then " << nm << "'s effect: " << what << (inside.empty() ? "" : " (" + inside + ")");
				break;
			}
		}
		ex.why = w.str();
		out.push_back(std::move(ex));
	}
	return out;
}

std::string choices_json(const CardDB& db, const std::vector<ChoiceExplanation>& c) {
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
			     ",\"where\":" + quote(v[i].where) + "}";
		return o + "]";
	};
	std::ostringstream o;
	o.imbue(std::locale::classic());
	o << "[";
	for(size_t i = 0; i < c.size(); ++i) {
		const auto& e = c[i];
		o << (i ? "," : "") << "{\"step\":" << e.step << ",\"action\":" << quote(e.action)
		  << ",\"action_code\":" << db.official_id(e.action_code) << ",\"chosen\":" << quote(e.chosen)
		  << ",\"chosen_code\":" << db.official_id(e.chosen_code) << ",\"margin\":" << num(e.margin)
		  << ",\"understood\":" << (e.understood ? "true" : "false") << ",\"why\":" << quote(e.why) << ",\"options\":[";
		for(size_t k = 0; k < e.options.size(); ++k) {
			const auto& op = e.options[k];
			o << (k ? "," : "") << "{\"label\":" << quote(op.label) << ",\"code\":" << db.official_id(op.code)
			  << ",\"value\":" << num(op.value) << ",\"chosen\":" << (op.chosen ? "true" : "false") << ",\"mine\":" << cards(op.mine)
			  << ",\"theirs\":" << cards(op.theirs) << "}";
		}
		o << "]}";
	}
	o << "]";
	return o.str();
}

}  // namespace ygoe
