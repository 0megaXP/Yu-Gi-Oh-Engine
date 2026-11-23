#include <locale>
#include "policy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "json.h"
#include "deck.h"
#include "evaluate.h"
#include "profile.h"
#include "search.h"

namespace ygoe {

namespace {

// Integer mixing (murmur3 finalizer): feature id = mix(decision part, detail) folded into kDim.
inline uint32_t mix(uint32_t a, uint32_t b) {
	uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u + (a << 6) + (a >> 2));
	h ^= h >> 16;
	h *= 0x85ebca6bu;
	h ^= h >> 13;
	h *= 0xc2b2ae35u;
	h ^= h >> 16;
	return h;
}

inline uint32_t tok(char kind, uint32_t value) { return mix(static_cast<uint32_t>(kind), value); }

int bucket_steps(int s) { return s == 0 ? 0 : s <= 3 ? 1 : s <= 8 ? 2 : s <= 16 ? 3 : 4; }

}  // namespace

Policy::Policy(const CardDB& db) : db_(db), w_(kDim, 0.0f) {
	tokens_.reserve(db.all().size());
	for(const auto& [code, c] : db.all()) {
		std::vector<uint32_t> t;
		for(int b = 0; b < 32; ++b)
			if(c.type & (1u << b))
				t.push_back(tok('T', static_cast<uint32_t>(b)));
		if(c.type & 0x1)  // monster
			t.push_back(tok('V', std::min<uint32_t>(c.lv(), 13)));
		uint32_t cats = 0;
		for(const auto& e : c.effects)
			cats |= e.category;
		for(int b = 0; b < 32; ++b)
			if(cats & (1u << b))
				t.push_back(tok('C', static_cast<uint32_t>(b)));
		t.push_back(tok('N', std::min<uint32_t>(static_cast<uint32_t>(c.effects.size()), 6)));
		t.push_back(tok('I', code));  // the card itself: a deck's own patterns
		tokens_.emplace(code, std::move(t));
	}
}

void Policy::features(const Decision& d, const Option& o, const PolicyContext& c, std::vector<uint32_t>& out) const {
	out.clear();
	const uint32_t base = mix(static_cast<uint32_t>(d.type) + 1, static_cast<uint32_t>(o.kind) + 101);
	const uint32_t own = (d.player == c.turn_player) ? 1u : 2u;
	const uint32_t steps = static_cast<uint32_t>(bucket_steps(c.steps_this_turn));
	const uint32_t chain = c.chain_depth > 0 ? 1u : 0u;
	const uint32_t hint = static_cast<uint32_t>(d.hint ^ (d.hint >> 32));
	auto put = [&](uint32_t h) { out.push_back(h & (kDim - 1)); };
	put(base);
	put(mix(base, tok('o', own)));
	put(mix(base, tok('s', steps * 4 + own)));
	put(mix(base, tok('c', chain * 4 + own)));
	if(o.loc)
		put(mix(base, tok('l', o.loc)));
	if(d.hint)
		put(mix(base, tok('h', hint)));
	if(!o.code)
		return;
	auto it = tokens_.find(o.code);
	if(it == tokens_.end())
		return;
	for(uint32_t t : it->second) {
		put(mix(base, t));
		// the same card property in a different light: where it is played from, and on whose turn
		if(o.loc)
			put(mix(mix(base, t), tok('l', o.loc)));
		put(mix(mix(base, t), tok('o', own)));
		if(d.hint)
			put(mix(mix(base, t), tok('h', hint)));
	}
}

double Policy::score(const std::vector<uint32_t>& f) const {
	double s = 0;
	for(uint32_t i : f)
		s += w_[i];
	return s;
}

double Policy::factor(const Decision& d, const Option& o, const PolicyContext& c) const {
	if(!trained_)
		return 1.0;
	// A plain local, not thread_local: on Windows (MinGW) thread_locals with a destructor
	// corrupted the heap when the bots' search threads ended.
	std::vector<uint32_t> f;
	features(d, o, c, f);
	double s = std::clamp(score(f), -2.08, 2.08);  // e^2.08 ≈ 8
	return std::exp(s);
}

double Policy::loss(const std::vector<PolicyExample>& ex) const {
	double total = 0, weight = 0;
	for(const auto& e : ex) {
		std::vector<double> z(e.options.size());
		double mx = -1e300;
		for(size_t i = 0; i < z.size(); ++i) {
			z[i] = std::log(std::max(1e-6f, e.prior[i])) + score(e.options[i]);
			mx = std::max(mx, z[i]);
		}
		double sum = 0;
		for(double v : z)
			sum += std::exp(v - mx);
		total += e.weight * -(z[static_cast<size_t>(e.chosen)] - mx - std::log(sum));
		weight += e.weight;
	}
	return weight > 0 ? total / weight : 0;
}

std::pair<double, double> Policy::train(const std::vector<PolicyExample>& examples, int epochs, double rate, double l2) {
	if(examples.empty())
		return {0, 0};
	const double before = loss(examples);
	std::vector<size_t> order(examples.size());
	for(size_t i = 0; i < order.size(); ++i)
		order[i] = i;
	uint64_t r = 0x2545F4914F6CDD1DULL;
	for(int ep = 0; ep < epochs; ++ep) {
		for(size_t i = order.size(); i > 1; --i) {  // shuffle
			r ^= r << 13; r ^= r >> 7; r ^= r << 17;
			std::swap(order[i - 1], order[r % i]);
		}
		const double lr = rate / (1 + ep);
		for(size_t idx : order) {
			const auto& e = examples[idx];
			std::vector<double> z(e.options.size());
			double mx = -1e300;
			for(size_t i = 0; i < z.size(); ++i) {
				z[i] = std::log(std::max(1e-6f, e.prior[i])) + score(e.options[i]);
				mx = std::max(mx, z[i]);
			}
			double sum = 0;
			for(auto& v : z) {
				v = std::exp(v - mx);
				sum += v;
			}
			for(size_t i = 0; i < z.size(); ++i) {
				double g = z[i] / sum - (static_cast<int>(i) == e.chosen ? 1.0 : 0.0);
				if(std::fabs(g) < 1e-4)
					continue;
				for(uint32_t f : e.options[i]) {
					float& w = w_[f];
					w -= static_cast<float>(lr * e.weight * (g + l2 * w));
				}
			}
		}
	}
	trained_ = true;
	seen_ += static_cast<int>(examples.size());
	return {before, loss(examples)};
}

std::vector<PolicyExample> Policy::examples_from_line(ScriptCache& scripts, const std::vector<uint32_t>& declare_pool,
                                                      const DuelSetup& setup,
                                                      const std::vector<std::vector<uint8_t>>& responses, uint8_t player,
                                                      float weight) const {
	std::vector<PolicyExample> out;
	Duel d(db_, scripts);
	d.set_declare_pool(declare_pool);
	if(!d.start(setup))
		return out;
	uint32_t turn = d.turn();
	int steps = 0;
	for(const auto& resp : responses) {
		if(d.ended())
			break;
		if(d.turn() != turn) {
			turn = d.turn();
			steps = 0;
		}
		const Decision& dec = d.decision();
		if(dec.player == player && !dec.minor && dec.options.size() > 1) {
			int chosen = -1;
			for(size_t i = 0; i < dec.options.size(); ++i)
				if(dec.options[i].response == resp) {
					chosen = static_cast<int>(i);
					break;
				}
			if(chosen >= 0) {
				PolicyExample e;
				e.chosen = chosen;
				e.weight = weight;
				PolicyContext c{d.turn_player(), steps, d.chain_depth()};
				for(const auto& o : dec.options) {
					std::vector<uint32_t> f;
					features(dec, o, c, f);
					e.options.push_back(std::move(f));
					e.prior.push_back(static_cast<float>(playout_weight(dec, o, c.turn_player, steps)));
				}
				out.push_back(std::move(e));
			}
		}
		if(!d.respond(resp))
			break;
		++steps;
	}
	return out;
}

std::vector<PolicyExample> self_play_examples(const CardDB& db, ScriptCache& scripts, const Deck& deck, const Policy& policy,
                                              int hands, int iterations, uint64_t seed, const std::atomic<bool>* cancel) {
	std::vector<PolicyExample> out;
	Evaluator eval(db, deck, deck);
	eval.set_profile(0, generic_profile());
	eval.set_profile(1, generic_profile());
	for(int h = 0; h < hands; ++h) {
		if(cancel && cancel->load())
			break;
		Scenario sc;
		sc.decks[0] = sc.decks[1] = deck;
		sc.hand_known[0] = false;
		sc.hand_size[0] = 5;
		sc.hand_known[1] = true;
		sc.seed = seed + static_cast<uint64_t>(h) * 7919;
		Searcher searcher(db, scripts, eval, declare_pool(sc));
		RootState root{make_setup(sc, 0), {}};
		SearchConfig cfg;
		cfg.iterations = iterations;
		cfg.seconds = 300;
		cfg.horizon_turns = 1;
		cfg.cancel = cancel;
		auto res = searcher.run({root}, 0, cfg);
		const Trajectory* best = nullptr;
		for(auto& m : res.my_moves)
			for(auto& t : m.best)
				if(!best || t.reward > best->reward)
					best = &t;
		if(!best)
			continue;
		Trajectory t = *best;
		searcher.drop_idle_summons(root, t, 0, 1);
		searcher.drop_idle_responses(root, t, 0, 1);
		auto ex = policy.examples_from_line(scripts, declare_pool(sc), root.setup, t.responses, 0);
		out.insert(out.end(), std::make_move_iterator(ex.begin()), std::make_move_iterator(ex.end()));
	}
	return out;
}

std::string Policy::to_json() const {
	std::ostringstream o;
	o.imbue(std::locale::classic());  // C number format, whatever locale the host app set
	o << "{\"version\":1,\"dim\":" << kDim << ",\"seen\":" << seen_ << ",\"w\":[";
	bool first = true;
	char buf[48];
	for(uint32_t i = 0; i < kDim; ++i) {
		if(std::fabs(w_[i]) < 1e-5f)
			continue;
		std::snprintf(buf, sizeof buf, "%s[%u,%.5g]", first ? "" : ",", i, static_cast<double>(w_[i]));
		o << buf;
		first = false;
	}
	o << "]}";
	return o.str();
}

bool Policy::from_json(const std::string& text) {
	std::string err;
	json::Value v = json::Value::parse(text, &err);
	return err.empty() && from_value(v);
}

bool Policy::from_value(const json::Value& v) {
	if(!v.is_object() || v["dim"].integer() != kDim)
		return false;
	std::fill(w_.begin(), w_.end(), 0.0f);
	for(const auto& p : v["w"].items()) {
		auto i = static_cast<uint32_t>(p[0].integer());
		if(i < kDim)
			w_[i] = static_cast<float>(p[1].num());
	}
	seen_ = static_cast<int>(v["seen"].integer());
	trained_ = seen_ > 0;
	return true;
}

}  // namespace ygoe
