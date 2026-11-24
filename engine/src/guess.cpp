#include "guess.h"

#include <algorithm>
#include <cmath>

#include "json.h"
#include "ocgapi_constants.h"

namespace ygoe {

namespace {
constexpr double kAbsent = 0.03;     // "copies" of a card a list doesn't play (side decks, tech, a different list)
constexpr double kRoguePrior = 0.05;
constexpr double kRogueRate = 1.5;   // copies of each seen card a rogue deck plays
}  // namespace

bool main_deck_card(const CardDB& db, uint32_t sid) {
	const CardInfo* c = db.get(sid);
	return c && !c->is_extra_deck() && !(c->type & TYPE_TOKEN);
}

void DeckLibrary::add(const CardDB& db, const Deck& d, double weight) {
	Entry e;
	e.name = d.name;
	e.weight = weight;
	for(auto code : d.main) {
		const uint32_t sid = db.script_id(code);
		if(!db.get(sid))
			continue;
		++e.main[sid];
		++e.size;
	}
	if(e.size < 20)
		return;  // not a whole list
	decks_.push_back(std::move(e));
	finish(db);
}

bool DeckLibrary::parse(const CardDB& db, const std::string& text, std::string* err) {
	std::string e;
	auto v = json::Value::parse(text, &e);
	if(!v.is_object()) {
		if(err)
			*err = "deck library: " + (e.empty() ? std::string("not a JSON object") : e);
		return false;
	}
	decks_.clear();
	for(const auto& x : v["decks"].items()) {
		Deck d;
		d.name = x["name"].str();
		for(const auto& c : x["main"].items())
			d.main.push_back(static_cast<uint32_t>(c.integer(0)));
		Entry en;
		en.name = d.name;
		en.weight = std::max(0.01, x["weight"].num(1));
		for(auto code : d.main) {
			const uint32_t sid = db.script_id(code);
			if(db.get(sid)) {
				++en.main[sid];
				++en.size;
			}
		}
		if(en.size >= 20)
			decks_.push_back(std::move(en));
	}
	finish(db);
	return true;
}

void DeckLibrary::finish(const CardDB&) {
	staples_.clear();
	double total = 0;
	std::map<uint32_t, double> share;
	std::map<uint32_t, std::map<int, double>> copies;
	for(const auto& d : decks_) {
		total += d.weight;
		for(const auto& [sid, n] : d.main) {
			share[sid] += d.weight;
			copies[sid][n] += d.weight;
		}
	}
	for(const auto& [sid, w] : share) {
		if(w < 0.5 * total)
			continue;
		int best = 1;
		double bw = -1;
		for(const auto& [n, cw] : copies[sid])
			if(cw > bw) {
				bw = cw;
				best = n;
			}
		staples_[sid] = best;
	}
}

std::vector<std::pair<int, double>> DeckGuess::ranked() const {
	std::vector<std::pair<int, double>> out;
	for(size_t i = 0; i < p.size(); ++i)
		out.push_back({static_cast<int>(i), p[i]});
	out.push_back({-1, rogue});
	std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second > b.second; });
	return out;
}

DeckGuess guess_deck(const DeckLibrary& lib, const std::map<uint32_t, int>& seen) {
	DeckGuess g;
	const auto& decks = lib.decks();
	double total_w = 0;
	for(const auto& d : decks)
		total_w += d.weight;
	std::vector<double> logp;
	for(const auto& d : decks) {
		double lp = std::log((1.0 - kRoguePrior) * d.weight / std::max(total_w, 1e-9));
		for(const auto& [sid, n] : seen) {
			auto it = d.main.find(sid);
			const double copies = it == d.main.end() ? 0.0 : it->second;
			// shown more copies than the list plays: the extra ones count as absent
			const double have = std::min<double>(n, copies);
			lp += have * std::log((copies + kAbsent) / d.size) + (n - have) * std::log(kAbsent / d.size);
		}
		logp.push_back(lp);
	}
	double rogue_lp = std::log(decks.empty() ? 1.0 : kRoguePrior);
	for(const auto& [sid, n] : seen)
		rogue_lp += n * std::log(kRogueRate / 40.0);
	double top = rogue_lp;
	for(double x : logp)
		top = std::max(top, x);
	double sum = std::exp(rogue_lp - top);
	for(double x : logp)
		sum += std::exp(x - top);
	for(double x : logp)
		g.p.push_back(std::exp(x - top) / sum);
	g.rogue = std::exp(rogue_lp - top) / sum;
	return g;
}

std::map<uint32_t, int> rogue_deck(const CardDB& db, const DeckLibrary& lib, const std::map<uint32_t, int>& seen) {
	std::map<uint32_t, int> out;
	int n = 0;
	for(const auto& [sid, k] : seen) {
		out[sid] = std::min(3, std::max(k, 2));
		n += out[sid];
	}
	// the archetype most seen cards share: its other Main Deck cards, 2 each
	std::map<uint16_t, int> sets;
	for(const auto& [sid, k] : seen)
		if(const CardInfo* c = db.get(sid))
			for(int i = 0; i < 4; ++i)
				if(uint16_t s = static_cast<uint16_t>(c->setcode >> (16 * i)))
					sets[s] += k;
	uint16_t arch = 0;
	int most = 1;
	for(const auto& [s, k] : sets)
		if(k > most) {
			most = k;
			arch = s;
		}
	if(arch) {
		std::vector<uint32_t> cards;
		for(const auto& [sid, c] : db.all())
			if(!c.alias && c.has_setcode(arch) && main_deck_card(db, sid) && !out.count(sid))
				cards.push_back(sid);
		std::sort(cards.begin(), cards.end());
		for(size_t i = 0; i < cards.size() && n < 28; ++i) {
			out[cards[i]] = 2;
			n += 2;
		}
	}
	for(const auto& [sid, k] : lib.staples()) {
		if(n >= 40)
			break;
		if(!out.count(sid)) {
			out[sid] = k;
			n += k;
		}
	}
	return out;
}

}  // namespace ygoe
