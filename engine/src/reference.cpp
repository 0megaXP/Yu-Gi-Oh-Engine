#include "reference.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

#include "json.h"
#include "ocgapi_constants.h"
#include "combo.h"
#include "search.h"

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
                                const std::function<void(int, int)>& progress) {
	ReferenceResult res;
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
		// Goldfish: the deck goes first, the other side has no hand to interact with.
		Scenario sc;
		sc.decks[0] = deck;
		sc.decks[1] = deck;
		sc.hand_known[0] = false;
		sc.hand_size[0] = 5;
		sc.hand_known[1] = true;
		sc.me = 0;
		sc.seed = seed + static_cast<uint64_t>(h) * 7919;
		Searcher searcher(db, scripts, eval, declare_pool(sc));
		RootState root{make_setup(sc, 0), {}};
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
		searcher.drop_idle_summons(root, t, 0, 1);
		searcher.drop_idle_responses(root, t, 0, 1);
		Board end;
		if(!searcher.final_board(root, t, end))
			continue;
		auto ev = eval.evaluate(end, 0, 1, false, -1, true);
		ReferenceSample s;
		for(auto code : root.setup.players[0].hand)
			s.hand.push_back(db.official_id(db.script_id(code)));
		s.value = ev.value[0];
		for(auto& n : ev.notes)
			if(n.player == 0)
				++s.interrupts;
		s.board = summarize_cards(db, end, 0);
		s.moves = describe(t, 0, true);
		{
			auto split = split_line(db, scripts, declare_pool(sc), root.setup, t, hand_traps);
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
	sort_library(res.library, 30);
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
	o << "],\"lines\":" << samples.size() << ",\"library\":" << library_json(db, library, static_cast<int>(samples.size()))
	  << "}";
	return o.str();
}

}  // namespace ygoe
