#include <locale>
#include "reference.h"
#include "deck.h"
#include <fstream>

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

#include "json.h"
#include "ocgapi_constants.h"
#include "combo.h"
#include "search.h"
#include "policy.h"

namespace ygoe {

namespace {

void make_neutral(Evaluator& e) {
	e.set_profile(0, generic_profile());
	e.set_profile(1, generic_profile());
}

}  // namespace

double board_value(const CardDB& db, const Deck& deck, const Board& b, uint8_t p) {
	Evaluator e(db, deck, deck);
	make_neutral(e);
	return e.evaluate(b, p, static_cast<uint8_t>(1 - p)).value[p];
}

ReferenceResult build_reference(const CardDB& db, ScriptCache& scripts, const Deck& deck, int hands, int iterations,
                                uint64_t seed, const std::atomic<bool>* cancel,
                                const std::function<void(int, int)>& progress, Policy* policy,
                                const std::vector<Scenario>* boards, double second_share) {
	ReferenceResult res;
	std::vector<PolicyExample> examples;
	res.deck = deck.name;
	res.iterations = iterations;
	Evaluator eval(db, deck, deck);
	make_neutral(eval);
	std::map<uint32_t, int> seen;
	const auto hand_traps = likely_hand_traps(db, nullptr, 8);
	for(int h = 0; h < hands; ++h) {
		if(cancel && cancel->load())
			break;
		if(progress)
			progress(h, hands);
		// Goldfish: the deck goes first, the other side has no hand to interact with. Or, for a
		// share of the hands, going second: against a real end board (their hand as it was seen).
		Scenario sc;
		const bool second = boards && !boards->empty() &&
		                    static_cast<double>((static_cast<uint64_t>(h) * 2654435761u) % 1000) / 1000.0 < second_share;
		if(second) {
			sc = (*boards)[(seed + static_cast<uint64_t>(h) * 40503u) % boards->size()];
			sc.placed[0].clear();
			sc.hand[0].clear();
			sc.lp[0] = 8000;
		} else {
			sc.decks[1] = deck;
			sc.hand_known[1] = true;
		}
		sc.decks[0] = deck;
		sc.hand_known[0] = false;
		sc.hand_size[0] = second ? 6 : 5;
		sc.me = 0;
		sc.seed = seed + static_cast<uint64_t>(h) * 7919;
		Searcher searcher(db, scripts, eval, declare_pool(sc));
		RootState root{make_setup(sc, 0), {}};
		if(second)
			root.setup.flags = DUEL_MODE_MR5 | DUEL_ATTACK_FIRST_TURN;
		SearchConfig cfg;
		cfg.iterations = iterations;
		cfg.seconds = 120;
		cfg.horizon_turns = 1;
		cfg.cancel = cancel;
		auto out = searcher.run({root}, 0, cfg);
		const Trajectory* best = nullptr;
		for(auto& m : out.my_moves)
			for(auto& t : m.best)
				if(!best || t.reward > best->reward)
					best = &t;
		if(!best)
			continue;
		Trajectory t = *best;
		searcher.defer_sets(root, t, 0, 1);
		searcher.defer_summons(root, t, 0, 1);
		searcher.drop_idle_summons(root, t, 0, 1);
		searcher.drop_idle_responses(root, t, 0, 1);
		Board end;
		if(!searcher.final_board(root, t, end))
			continue;
		auto ev = eval.evaluate(end, 0, 1, false, -1, true);
		if(second && best->reward >= 0.999)
			ev.value[0] = std::max(ev.value[0], 10.0);  // a win going second: the best line there is
		ReferenceSample s;
		for(auto code : root.setup.players[0].hand)
			s.hand.push_back(db.official_id(db.script_id(code)));
		s.value = ev.value[0];
		for(auto& n : ev.notes)
			if(n.player == 0)
				++s.interrupts;
		s.board = summarize_cards(db, end, 0);
		s.moves = describe(t, 0, true);
		if(policy) {
			auto ex = policy->examples_from_line(scripts, declare_pool(sc), root.setup, t.responses, 0);
			examples.insert(examples.end(), ex.begin(), ex.end());
		}
		{
			auto split = split_line(db, scripts, declare_pool(sc), root.setup, t, second ? std::vector<uint32_t>{} : hand_traps);
			for(auto& pc : split.pieces)
				pc.second = second;
			add_to_library(res.library, split, s.value);
			s.pieces = std::move(split.pieces);
		}
		for(auto& c : end.mzone[0])
			if(c.code && c.owner == 0)
				++seen[c.code];
		for(auto& c : end.szone[0])
			if(c.code)
				++seen[c.code];
		res.samples.push_back(std::move(s));
	}
	if(progress)
		progress(hands, hands);
	std::sort(res.samples.begin(), res.samples.end(), [](auto& a, auto& b) { return a.value > b.value; });
	for(auto& [code, n] : seen)
		res.common_cards.push_back({code, res.samples.empty() ? 0.0 : double(n) / res.samples.size()});
	std::sort(res.common_cards.begin(), res.common_cards.end(), [](auto& a, auto& b) { return a.second > b.second; });
	if(res.common_cards.size() > 15)
		res.common_cards.resize(15);
	sort_library(res.library, 60);
	if(policy && !examples.empty()) {
		policy->train(examples);
		res.policy = policy->to_json();
	}
	return res;
}

std::string ReferenceResult::to_json(const CardDB& db) const {
	using json::quote;
	auto num = [](double v) {
		char b[32];
		std::snprintf(b, sizeof b, "%.3f", v);
		return std::string(b);
	};
	std::ostringstream o;
	o.imbue(std::locale::classic());  // C number format, whatever locale the host app set
	o << "{\"deck\":" << quote(deck) << ",\"iterations\":" << iterations << ",\"samples\":[";
	for(size_t i = 0; i < samples.size(); ++i) {
		const auto& s = samples[i];
		o << (i ? "," : "") << "{\"hand\":[";
		for(size_t k = 0; k < s.hand.size(); ++k)
			o << (k ? "," : "") << s.hand[k];
		o << "],\"value\":" << num(s.value) << ",\"interrupts\":" << s.interrupts << ",\"board\":[";
		for(size_t k = 0; k < s.board.size(); ++k) {
			const auto& c = s.board[k];
			o << (k ? "," : "") << "{\"code\":" << c.code << ",\"name\":" << quote(c.name) << ",\"where\":" << quote(c.where)
			  << ",\"note\":" << quote(c.note) << "}";
		}
		o << "],\"moves\":[";
		for(size_t k = 0; k < s.moves.size(); ++k)
			o << (k ? "," : "") << quote(s.moves[k]);
		o << "],\"pieces\":" << pieces_json(db, s.pieces) << "}";
	}
	o << "],\"common_cards\":[";
	for(size_t i = 0; i < common_cards.size(); ++i)
		o << (i ? "," : "") << "{\"code\":" << db.official_id(common_cards[i].first) << ",\"name\":"
		  << quote(db.name(common_cards[i].first)) << ",\"share\":" << num(common_cards[i].second) << "}";
	o << "]";
	if(!policy.empty())
		o << ",\"policy\":" << policy;
	o << ",\"lines\":" << samples.size() << ",\"library\":" << library_json(db, library, static_cast<int>(samples.size()))
	  << "}";
	return o.str();
}

}  // namespace ygoe

namespace ygoe {

std::vector<Scenario> load_end_boards(const std::string& starts_path, size_t max) {
	std::vector<Scenario> out;
	std::ifstream in(starts_path);
	std::string line;
	while(std::getline(in, line) && out.size() < max) {
		// a start (both decks known) or a board (followed games: the cards nobody saw left out)
		if((line.find("\"kind\":\"start\"") == std::string::npos && line.find("\"kind\":\"board\"") == std::string::npos) ||
		   line.find("\"turn\":2,") == std::string::npos)
			continue;
		auto v = json::Value::parse(line);
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
		}
		// The player going second's deck is the library's own (build_reference). The other's: the
		// cards seen, to 40 with a stand-in Set card (Trap Hole) for the ones nobody saw, so a
		// search or draw on their side still finds a Deck.
		{
			auto& m = sc.decks[1].main;
			while(m.size() < 40)
				m.push_back(4206964);
		}
		if(sc.decks[0].main.empty())
			sc.decks[0].main = sc.decks[1].main;
		// (the player going first, at index 1, left a board)
		bool board = false;
		for(const auto& pc : sc.placed[1])
			board = board || pc.loc == LOCATION_MZONE || pc.loc == LOCATION_SZONE;
		if(ok && board)
			out.push_back(std::move(sc));
	}
	return out;
}

}  // namespace ygoe
