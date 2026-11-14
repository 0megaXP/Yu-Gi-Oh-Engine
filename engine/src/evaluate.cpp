#include "evaluate.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "ocgapi_constants.h"

namespace ygoe {

namespace {
constexpr uint32_t kStatusDisabled = 0x1;  // STATUS_DISABLED
}

Evaluator::Evaluator(const CardDB& db, const Deck& d0, const Deck& d1) : db_(db) {
	profile_[0] = profile_deck(db, d0);
	profile_[1] = profile_deck(db, d1);
	sets_[0] = dominant_setcodes(db, d0);
	sets_[1] = dominant_setcodes(db, d1);
	const Deck* decks[2] = {&d0, &d1};
	for(int p = 0; p < 2; ++p)
		for(const auto* list : {&decks[p]->main, &decks[p]->extra})
			for(uint32_t code : *list) {
				const CardInfo* c = db.get(db.script_id(code));
				if(!c || !(c->type & TYPE_MONSTER) || !(c->type & TYPE_EFFECT))
					continue;
				++effect_monsters_[p];
				for(int bit = 0; bit < 8; ++bit)
					if(c->attribute & (1u << bit))
						++by_attribute_[p][bit];
			}
	// Interruptions that hit a whole column (Infinite Impermanence: negate a monster, and the
	// Spell/Trap Zone cards in its column can't be activated that turn): how likely each deck
	// holds one in its opening six cards.
	for(int p = 0; p < 2; ++p) {
		int k = 0;
		for(uint32_t code : decks[p]->main) {
			const uint32_t sid = db.script_id(code);
			if(!db.cares_about_columns(sid))
				continue;
			const CardRole& r = role(static_cast<uint8_t>(p), sid);
			bool hits = false;
			for(const auto& in : r.interrupts)
				hits = hits || in.kind == Interrupt::NegateEffect;
			k += hits;
		}
		const double n = std::max<size_t>(decks[p]->main.size(), 40);
		column_threat_[p] = k ? 1.0 - std::pow(1.0 - k / n, 6.0) : 0.0;
	}
}

uint32_t Evaluator::locked_attributes(uint64_t hint) const {
	{
		std::lock_guard<std::mutex> lock(mu_);
		auto it = lock_cache_.find(hint);
		if(it != lock_cache_.end())
			return it->second;
	}
	std::string t = db_.description(hint);
	for(auto& ch : t)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	uint32_t mask = 0;
	if(t.find("cannot activate") != std::string::npos && t.find("monster") != std::string::npos) {
		static const std::pair<const char*, uint32_t> names[] = {
			{"earth", ATTRIBUTE_EARTH}, {"water", ATTRIBUTE_WATER}, {"fire", ATTRIBUTE_FIRE}, {"wind", ATTRIBUTE_WIND},
			{"light", ATTRIBUTE_LIGHT}, {"dark", ATTRIBUTE_DARK}, {"divine", ATTRIBUTE_DIVINE}};
		for(auto& [n, bit] : names)
			if(t.find(n) != std::string::npos)
				mask |= bit;
	}
	std::lock_guard<std::mutex> lock(mu_);
	lock_cache_[hint] = mask;
	return mask;
}

const CardRole& Evaluator::role(uint8_t owner, uint32_t code) const {
	std::lock_guard<std::mutex> lock(mu_);
	auto it = roles_[owner].find(code);
	if(it == roles_[owner].end())
		it = roles_[owner].emplace(code, classify(db_, code, sets_[owner])).first;
	return it->second;
}

Evaluation Evaluator::evaluate(const Board& b, uint8_t me, uint8_t next, bool ended, int winner, bool want_notes) const {
	Evaluation ev;
	if(ended) {
		ev.raw = winner == me ? 50 : (winner == 1 - me ? -50 : 0);
		ev.score = winner == me ? 1.0 : (winner == 1 - me ? 0.0 : 0.5);
		return ev;
	}
	double held_off[2] = {0, 0};  // interruptions the defending side holds
	for(uint8_t p = 0; p < 2; ++p) {
		const bool defending = p != next;
		const DeckProfile& opp = profile_[1 - p];
		double v = 0;
		// Lasting locks on this player ("cannot activate the effects of EARTH... monsters for the
		// rest of the Duel", from activating a Dominus trap from the hand): those monsters'
		// interruptions are gone now, and every later turn loses that share of the deck.
		uint32_t locked = 0;
		for(uint64_t h : b.hints[p])
			locked |= locked_attributes(h);
		if(locked && effect_monsters_[p] > 0) {
			int hit = 0;
			// Share of the deck's effect monsters that lock covers (a monster with several
			// attribute bits counts once per matching bit; attributes are single in practice).
			for(int bit = 0; bit < 8; ++bit)
				if(locked & (1u << bit))
					hit += by_attribute_[p][bit];
			v -= 3.0 * std::min(1.0, double(hit) / effect_monsters_[p]);
		}
		auto locked_out = [&](const CardView& cv) {
			if(!locked)
				return false;
			const CardInfo* c = db_.get(cv.code);
			return c && (c->type & TYPE_MONSTER) && (c->attribute & locked) && !(cv.type & (TYPE_SPELL | TYPE_TRAP));
		};
		// Best interrupt a card offers from where it currently is.
		auto interrupts_from = [&](const CardView& cv, uint32_t loc, const char* where) {
			if(locked_out(cv))
				return 0.0;
			const CardRole& r = role(p, cv.code);
			double best = 0, second = 0;
			const Interrupt* best_in = nullptr;
			for(const auto& in : r.interrupts) {
				if(!(in.range & loc))
					continue;
				double val = interrupt_value(in, opp);
				if(val > best) {
					second = best;
					best = val;
					best_in = &in;
				} else if(val > second) {
					second = val;
				}
			}
			double total = best + 0.35 * second;
			if(!defending)
				total *= 0.35;  // on your own turn they mostly protect the combo
			if(want_notes && best_in && defending)
				ev.notes.push_back({p, cv.code, where, best_in->name(), total});
			if(defending)
				held_off[p] += total;
			return total;
		};
		// At low LP, while the other player is about to attack, any monster that can block one
		// attack is worth more (a spare hand trap set or summoned as a wall).
		const double wall = defending && b.lp[p] < 4000 ? 0.45 * (1.0 - std::max(0, b.lp[p]) / 4000.0) : 0.0;
		std::map<uint32_t, int> copies;  // hand traps already counted in this hand
		bool controls = false;  // any card on this player's field
		for(const auto* z : {&b.mzone[p], &b.szone[p]})
			for(const auto& c : *z)
				controls |= c.code != 0;
		for(const auto& cv : b.hand[p]) {
			// "If you control no cards": with a board it can't be used from the hand. A dead card,
			// worth little (the one to discard or shuffle away for a cost).
			if(controls && role(p, cv.code).needs_empty_field) {
				v += defending ? 0.12 : 0.3;
				continue;
			}
			double iv = interrupts_from(cv, LOCATION_HAND, "hand");
			if(iv > 0 && role(p, cv.code).hand_trap) {
				// A hand trap is worth most where it is: hidden in the hand, used when it counts.
				// A second copy of the same one adds less.
				int n = copies[cv.code]++;
				iv = (iv + 0.15) * (n == 0 ? 1.0 : 0.55);
			}
			v += std::max(iv, defending ? 0.3 : 0.55);
		}
		for(const auto& cv : b.mzone[p]) {
			if(!cv.code)
				continue;
			// A monster the other player owns (handed over by an effect) is mostly a body that
			// will go back or be used up; it is not that player's engine.
			if(cv.owner != p) {
				v += 0.1;
				// ...unless it gives them a way to remove one of the owner's cards
				if((cv.pos & POS_FACEUP) && !(cv.status & kStatusDisabled))
					v += role(cv.owner, cv.code).field_removal * (defending ? 0.4 : 1.0);
				continue;
			}
			const bool live = (cv.pos & POS_FACEUP) && !(cv.status & kStatusDisabled);
			double body = (cv.pos & POS_FACEUP) ? 0.35 + 0.35 * std::min(cv.atk, 4000) / 4000.0 : 0.3;
			double iv = (cv.status & kStatusDisabled) ? 0 : interrupts_from(cv, LOCATION_MZONE, "field");
			if(role(p, cv.code).hand_trap) {
				// On the field a hand trap is seen and can be dealt with before it is used, and its
				// body adds next to nothing: its ATK doesn't matter before the owner's next turn, and
				// a card left there is one less in the hand. (At low LP the wall below still counts.)
				body = 0.05;
				iv *= 0.5;
			}
			body += wall;
			double extra = 0;
			if(live) {
				const CardRole& r = role(p, cv.code);
				extra = r.passive(LOCATION_MZONE) * (defending ? 1.0 : 0.6) + (r.reusable(LOCATION_MZONE) ? 0.08 : 0.0);
			}
			v += body + iv + extra;
		}
		for(const auto& cv : b.szone[p]) {
			if(!cv.code)
				continue;
			const bool facedown = cv.pos & POS_FACEDOWN;
			double iv = (cv.status & kStatusDisabled) ? 0 : interrupts_from(cv, LOCATION_SZONE, facedown ? "set" : "S/T zone");
			if(facedown) {
				v += std::max(iv, 0.1);
				continue;
			}
			// Face-up Spell/Trap Zone cards are worth what they do from there: static effects,
			// effects you can use again, interruptions. A card with nothing to do there (e.g. a
			// monster placed as a Continuous Spell that has no Spell-Zone effect) is nearly dead.
			const CardRole& r = role(p, cv.code);
			double worth = (cv.status & kStatusDisabled) ? 0 : r.passive(LOCATION_SZONE) + (r.reusable(LOCATION_SZONE) ? 0.15 : 0.0);
			v += std::max(0.05, iv + worth);
		}
		// Facing a deck with a column interruption (Infinite Impermanence): a Set or face-up
		// Spell/Trap that can interrupt, in the same column as a monster that can, is lost with it
		// to one card. Spread them out.
		if(defending && column_threat_[1 - p] > 0) {
			auto col = [](const CardView& c, bool monster) -> int {
				if(monster)
					return c.seq == 5 ? 1 : c.seq == 6 ? 3 : static_cast<int>(c.seq);
				return c.seq < 5 ? static_cast<int>(c.seq) : -1;
			};
			bool key_monster[5] = {false, false, false, false, false};
			for(const auto& cv : b.mzone[p])
				if(cv.code && cv.owner == p && (cv.pos & POS_FACEUP) && !role(p, cv.code).interrupts.empty()) {
					int c = col(cv, true);
					if(c >= 0 && c < 5)
						key_monster[c] = true;
				}
			for(const auto& cv : b.szone[p]) {
				int c = cv.code ? col(cv, false) : -1;
				if(c >= 0 && key_monster[c] && !role(p, cv.code).interrupts.empty())
					v -= 0.15 * column_threat_[1 - p];
			}
		}
		for(const auto& cv : b.grave[p]) {
			// A card with an effect to use from the GY is a resource there (Angelechy Verdict and
			// Disturbance: what Angelechy Problem should discard); most others are spent.
			double iv = interrupts_from(cv, LOCATION_GRAVE, "GY");
			double later = locked_out(cv) ? 0.0 : role(p, cv.code).gy_effect * (defending ? 0.8 : 1.0);
			v += 0.03 + std::max(iv, later);
		}
		v += 0.02 * b.extra[p].size();
		// Life points: worth more the lower they get, so bots push damage and protect themselves.
		if(b.lp[p] <= 0)
			v -= 50;
		else
			v += 2.5 * std::min(b.lp[p], 8000) / 8000.0 - (b.lp[p] <= 2000 ? 0.6 : 0.0);
		ev.value[p] = v;
	}
	// A free turn: cards in the hand of the player about to take their turn are worth more when
	// nothing on the other side can stop them. Without this, passing with a full hand looks
	// almost even, and giving the opponent cards (e.g. through Mulcharmy Fuwalos draws) looks
	// as costly as leaving them an open field.
	if(next < 2) {
		const uint8_t def = static_cast<uint8_t>(1 - next);
		ev.value[next] += 0.45 * b.hand[next].size() * std::exp(-held_off[def] / 1.5);
	}
	ev.raw = ev.value[me] - ev.value[1 - me];
	ev.score = 1.0 / (1.0 + std::exp(-ev.raw / 2.5));
	if(want_notes)
		std::sort(ev.notes.begin(), ev.notes.end(), [](auto& a, auto& c) { return a.value > c.value; });
	return ev;
}

}  // namespace ygoe
