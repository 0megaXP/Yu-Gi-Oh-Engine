#include <cctype>
#include <locale>
#include "live.h"
#include "pieces.h"
#include <random>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "json.h"
#include "nonengine.h"
#include "profile.h"
#include "ocgapi_constants.h"

namespace ygoe {

namespace {

using json::quote;

const char* phase_text(uint16_t ph) {
	switch(ph) {
	case PHASE_DRAW: return "Draw Phase";
	case PHASE_STANDBY: return "Standby Phase";
	case PHASE_MAIN1: return "Main Phase 1";
	case PHASE_BATTLE_START:
	case PHASE_BATTLE_STEP:
	case PHASE_DAMAGE:
	case PHASE_DAMAGE_CAL:
	case PHASE_BATTLE: return "Battle Phase";
	case PHASE_MAIN2: return "Main Phase 2";
	case PHASE_END: return "End Phase";
	default: return "";
	}
}

const char* pos_text(uint32_t pos) {
	if(pos & POS_FACEDOWN)
		return "set";
	if(pos & POS_DEFENSE)
		return "def";
	return "atk";
}

const char* kind_text(OptKind k) {
	switch(k) {
	case OptKind::Pass: return "pass";
	case OptKind::Summon: return "summon";
	case OptKind::SpSummon: return "spsummon";
	case OptKind::Reposition: return "reposition";
	case OptKind::MSet: return "mset";
	case OptKind::SSet: return "sset";
	case OptKind::Activate: return "activate";
	case OptKind::ToBattle: return "battle";
	case OptKind::ToMain2: return "main2";
	case OptKind::ToEnd: return "end";
	case OptKind::Attack: return "attack";
	case OptKind::Select: return "select";
	case OptKind::Yes: return "yes";
	case OptKind::No: return "no";
	case OptKind::Shuffle: return "shuffle";
	default: return "choose";
	}
}

// Decisions a person never needs to make by hand: the first answer is always fine.
bool trivial(const Decision& d, int human) {
	if(d.options.size() == 1)
		return true;
	switch(d.type) {
	case DecisionType::Place:
		// A person picks their own zones. Bots take the first free one, unless a card in the duel
		// cares about columns (then the zone is a real choice and the bot plans it).
		return d.player != human && d.minor;
	case DecisionType::Sort:
	case DecisionType::Counter:
	case DecisionType::RockPaperScissors: return true;
	default: return false;
	}
}

}  // namespace

LiveDuel::LiveDuel(const CardDB& db, ScriptCache& scripts, const Deck& d0, const Deck& d1, uint64_t seed)
    : db_(db), scripts_(scripts), rng_(seed * 7919 + 17) {
	sc_.decks[0] = d0;
	sc_.decks[1] = d1;
	sc_.hand_known[0] = sc_.hand_known[1] = false;
	sc_.hand_size[0] = sc_.hand_size[1] = 5;
	sc_.seed = seed;
	if(d0.main.empty() || d1.main.empty()) {
		error_ = "both decks need a Main Deck";
		return;
	}
	setup_ = make_setup(sc_, 0);
	begin(d0, d1);
}

LiveDuel::LiveDuel(const CardDB& db, ScriptCache& scripts, const Scenario& sc, uint64_t flags)
    : db_(db), scripts_(scripts), sc_(sc), rng_(sc.seed * 7919 + 17) {
	if(sc_.decks[0].main.empty() || sc_.decks[1].main.empty()) {
		error_ = "both decks need a Main Deck";
		return;
	}
	setup_ = make_setup(sc_, 0);
	setup_.flags = flags;
	begin(sc_.decks[0], sc_.decks[1]);
}

LiveDuel::LiveDuel(const CardDB& db, ScriptCache& scripts, const DuelSetup& setup,
                   const std::vector<std::vector<uint8_t>>& answers)
    : db_(db), scripts_(scripts), rng_(setup.seed[0] * 7919 + 17) {
	for(int p = 0; p < 2; ++p) {
		sc_.decks[p].main = setup.players[p].main;
		sc_.decks[p].main.insert(sc_.decks[p].main.end(), setup.players[p].hand.begin(), setup.players[p].hand.end());
		sc_.decks[p].extra = setup.players[p].extra;
		sc_.hand_known[p] = false;
	}
	sc_.seed = setup.seed[0];
	setup_ = setup;
	replaying_ = true;
	begin(sc_.decks[0], sc_.decks[1]);
	replaying_ = false;
	if(!ok_)
		return;
	for(size_t i = 0; i < answers.size(); ++i) {
		const Decision& d = duel_->decision();
		size_t at = d.options.size();
		for(size_t k = 0; k < d.options.size() && at == d.options.size(); ++k)
			if(d.options[k].response == answers[i])
				at = k;
		bool done = at < d.options.size() ? respond_internal(at) : false;
		if(!done && duel_->respond(answers[i])) {  // an answer not among the options (a free-form one)
			history_.push_back(answers[i]);
			track_knowledge();
			plan_.clear(), plan_pos_ = 0;
			done = true;
		}
		if(!done) {
			ok_ = false;
			error_ = "answer " + std::to_string(i) + " is not legal any more";
			return;
		}
	}
	steps_turn_ = duel_->turn();
}

void LiveDuel::set_guide(int player, std::shared_ptr<const PieceGuide> g) {
	const uint8_t p = static_cast<uint8_t>(player & 1);
	// A deck whose going-second lines break the board but rarely won (PieceGuide::Plan::grind): it wins over several turns,
	// by having more cards left, so from turn 2 on each card it has (in hand or on the field) is worth
	// a little more: lines that spend cards for nothing lose against ones that keep them.
	double grind = 0;
	if(g) {
		const auto pl = g->plan();
		if(pl.grind())
			grind = 0.1;
	}
	if(eval_)
		eval_->set_grind(p, grind);
	guides_[p] = std::move(g);
}

void LiveDuel::begin(const Deck& d0, const Deck& d1) {
	duel_ = std::make_unique<Duel>(db_, scripts_);
	duel_->enable_log(true);
	duel_->set_declare_pool(declare_pool(sc_));
	if(!duel_->start(setup_, &error_))
		return;
	log_ = duel_->take_log();
	log_players_ = duel_->take_log_players();
	moves_ = duel_->take_moves();
	activations_ = duel_->take_activations();
	picks_ = duel_->take_picks();
	eval_ = std::make_unique<Evaluator>(db_, d0, d1);
	for(int p = 0; p < 2; ++p)
		if(guides_[p])
			set_guide(p, guides_[p]);
	for(int pl = 0; pl < 2; ++pl) {
		dodges_[pl] = false;
		const Deck& dk = pl == 0 ? d0 : d1;
		for(const auto* v : {&dk.main, &dk.extra})
			for(uint32_t c : *v) {
				std::string t = db_.text(db_.script_id(c));
				for(auto& ch : t)
					ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				size_t q = t.find("(quick effect)");
				while(q != std::string::npos && !dodges_[pl]) {
					const size_t end = t.find('.', q);
					const std::string sent = t.substr(q, end == std::string::npos ? std::string::npos : end - q);
					for(const char* k : {"face-up field to the gy", "from your field to the gy", "monster you control to the hand",
					                     "return this card to the hand", "return this card to the extra deck", "banish this card",
					                     "tribute this card", "tribute 1 monster"})
						dodges_[pl] = dodges_[pl] || sent.find(k) != std::string::npos;
					q = t.find("(quick effect)", q + 1);
				}
			}
	}
	searcher_ = std::make_unique<Searcher>(db_, scripts_, *eval_, declare_pool(sc_));
	steps_turn_ = duel_->turn();
	// the starting hands are hidden from the other player (a replay's start position too)
	duel_->take_hand_news();
	for(uint8_t p = 0; p < 2; ++p)
		for(const auto& c : duel_->board().hand[p])
			if(c.code)
				++last_hand_[p][c.code];
	ok_ = true;
	if(!replaying_)
		auto_resolve();
}

bool LiveDuel::respond_internal(size_t index) {
	const Decision& d = duel_->decision();
	if(duel_->ended() || index >= d.options.size())
		return false;
	Option opt = d.options[index];
	const uint8_t who = d.player;
	const DecisionType type = d.type;
	const bool minor = d.minor;
	if(!duel_->respond(opt.response))
		return false;
	if(who < 2 && opt.code && opt.kind != OptKind::Pass &&
	   (type == DecisionType::Idle || type == DecisionType::Chain || type == DecisionType::EffectYesNo))
		last_action_[who] = opt.code;
	history_.push_back(opt.response);
	track_knowledge();
	if(duel_->turn() != steps_turn_) {
		steps_turn_ = duel_->turn();
		steps_this_turn_ = 0;
	} else {
		++steps_this_turn_;
	}
	last_move_ = opt.label;
	// The plan put aside (the other bot's): its owner's opponent doing anything but passing ends it.
	if(other_player_ >= 0 && who != other_player_ && !minor && opt.kind != OptKind::Pass)
		other_plan_.clear(), other_pos_ = 0, other_player_ = -1;
	// Keep the bot's plan while play follows it. The other player passing a window the plan didn't
	// imagine changes nothing; anything else that differs ends the plan (what is left of it stays a
	// candidate for the next search, this turn).
	if(plan_pos_ < plan_.size() && plan_[plan_pos_].label == opt.label) {
		++plan_pos_;
	} else if(plan_pos_ < plan_.size() && (minor || type == DecisionType::Place) && plan_[plan_pos_].type == type &&
	          plan_[plan_pos_].player == who) {
		++plan_pos_;  // a zone / position / order taken by default: another free zone than the plan's, same plan
	} else if(plan_pos_ < plan_.size() && (minor || (who != plan_player_ && opt.kind == OptKind::Pass))) {
	} else {
		if(plan_pos_ < plan_.size() && std::getenv("YGOE_PLAN_DEBUG"))
			std::fprintf(stderr, "plan left at step %zu: P%d '%s', the plan had P%d '%s'\n", plan_pos_, who, opt.label.substr(0, 60).c_str(),
			             plan_[plan_pos_].player, plan_[plan_pos_].label.substr(0, 60).c_str());
		if(plan_pos_ + 1 < plan_.size()) {
			stale_plan_.assign(plan_.begin() + static_cast<long>(plan_pos_) + 1, plan_.end());
			stale_turn_ = duel_->turn();
			stale_player_ = plan_player_;
		}
		plan_.clear(), plan_pos_ = 0;
	}
	auto lines = duel_->take_log();
	log_.insert(log_.end(), lines.begin(), lines.end());
	auto players = duel_->take_log_players();
	log_players_.insert(log_players_.end(), players.begin(), players.end());
	// Activations count the moves before them within this step; shift them past the moves already queued.
	auto acts = duel_->take_activations();
	for(auto& a : acts)
		a.after += static_cast<uint32_t>(moves_.size());
	activations_.insert(activations_.end(), acts.begin(), acts.end());
	auto picks = duel_->take_picks();
	for(auto& k : picks)
		k.after += static_cast<uint32_t>(moves_.size());
	picks_.insert(picks_.end(), picks.begin(), picks.end());
	auto moves = duel_->take_moves();
	moves_.insert(moves_.end(), moves.begin(), moves.end());
	return true;
}

void LiveDuel::auto_resolve() {
	int guard = 0;
	while(!duel_->ended() && trivial(duel_->decision(), human_) && guard++ < 200) {
		// For a single "Don't respond" chain window, or a zone/sort choice, take the first answer.
		if(!respond_internal(0))
			break;
	}
}

bool LiveDuel::respond(size_t index) {
	if(!ok_)
		return false;
	if(!respond_internal(index))
		return false;
	auto_resolve();
	return true;
}

std::vector<RootState> LiveDuel::hidden_roots(uint8_t me, int samples) {
	const uint8_t opp = static_cast<uint8_t>(1 - me);
	const Board truth = duel_->board(true);
	// their hand cards `me` doesn't know, and the cards they could be: those plus their Deck
	std::map<uint32_t, int> known = known_[opp];
	std::vector<size_t> hidden_idx;
	std::vector<uint32_t> pool;
	for(size_t i = 0; i < truth.hand[opp].size(); ++i) {
		const CardView& c = truth.hand[opp][i];
		auto k = known.find(c.code);
		if(c.is_public || (k != known.end() && k->second > 0)) {
			if(!c.is_public)
				--k->second;
			continue;
		}
		hidden_idx.push_back(i);
		pool.push_back(c.code);
	}
	for(const auto& c : truth.deck[opp])
		if(c.code)
			pool.push_back(c.code);
	// their deck isn't known either: each guess draws from a deck picked by how likely it is
	const DeckLibrary* lib = library();
	DeckGuess guess;
	std::vector<double> chance;
	std::map<uint32_t, int> seen, rogue;
	if(lib && !lib->empty()) {
		seen = seen_cards(me);
		guess = guess_deck(*lib, seen);
		chance = guess.p;
		chance.push_back(guess.rogue);
		rogue = rogue_deck(db_, *lib, seen);
	}
	const size_t deck_size = truth.deck[opp].size();
	std::vector<RootState> roots;
	for(int k = 0; k < samples; ++k) {
		RootState r{setup_, history_};
		r.setup.hidden_at = history_.size();
		DuelSetup::Hidden h;
		h.player = opp;
		h.hand.assign(truth.hand[opp].size(), 0);
		if(!chance.empty()) {
			std::discrete_distribution<size_t> pick(chance.begin(), chance.end());
			const size_t di = pick(rng_);
			const auto& list = di < lib->decks().size() ? lib->decks()[di].main : rogue;
			std::vector<uint32_t> all, rest;
			for(const auto& [sid, n] : list) {
				const int s_n = seen.count(sid) ? seen.at(sid) : 0;
				for(int c = 0; c < n; ++c) {
					all.push_back(sid);
					if(c >= s_n)
						rest.push_back(sid);  // the copies not seen yet
				}
			}
			std::shuffle(rest.begin(), rest.end(), rng_);
			auto next = [&](size_t& at) {
				if(at < rest.size())
					return rest[at++];
				return all.empty() ? pool.front() : all[rng_() % all.size()];
			};
			size_t at = 0;
			for(size_t j = 0; j < hidden_idx.size(); ++j)
				h.hand[hidden_idx[j]] = next(at);
			h.guessed = true;
			for(size_t j = 0; j < deck_size; ++j)
				h.deck.push_back(next(at));
		} else {
			std::shuffle(pool.begin(), pool.end(), rng_);
			for(size_t j = 0; j < hidden_idx.size(); ++j)
				h.hand[hidden_idx[j]] = pool[j];
		}
		h.seed = rng_() | 1;
		DuelSetup::Hidden mine;  // `me` doesn't know their own Deck's order either
		mine.player = me;
		mine.seed = rng_() | 1;
		r.setup.hidden = {h, mine};
		roots.push_back(std::move(r));
	}
	static const bool dbg = std::getenv("YGOE_HIDDEN_DEBUG") != nullptr;
	if(dbg && !roots.empty()) {
		// each guess must keep everything `me` sees and the decision, and change only their hidden cards
		Duel check(db_, scripts_);
		check.set_declare_pool(declare_pool(sc_));
		int changed = 0, bad = 0;
		for(const auto& r : roots) {
			if(!check.replay(r.setup, r.history)) {
				++bad;
				continue;
			}
			const Board b = check.board(true);
			auto codes = [](const std::vector<CardView>& v) {
				std::vector<uint32_t> o;
				for(const auto& c : v)
					o.push_back(c.code);
				return o;
			};
			auto sorted = [&](std::vector<CardView> v) {
				auto o = codes(v);
				std::sort(o.begin(), o.end());
				return o;
			};
			std::vector<CardView> all_t = truth.hand[opp], all_b = b.hand[opp];
			all_t.insert(all_t.end(), truth.deck[opp].begin(), truth.deck[opp].end());
			all_b.insert(all_b.end(), b.deck[opp].begin(), b.deck[opp].end());
			const bool same_public = codes(b.hand[me]) == codes(truth.hand[me]) && codes(b.mzone[opp]) == codes(truth.mzone[opp]) &&
			                         codes(b.grave[opp]) == codes(truth.grave[opp]) &&
			                         (r.setup.hidden[0].guessed ? b.deck[opp].size() == truth.deck[opp].size() : sorted(all_b) == sorted(all_t)) &&
			                         b.hand[opp].size() == truth.hand[opp].size() &&
			                         check.decision().options.size() == duel_->decision().options.size();
			if(!same_public)
				++bad;
			if(codes(b.hand[opp]) != codes(truth.hand[opp]))
				++changed;
		}
		std::string g;
		if(!chance.empty()) {
			int shown = 0;
			for(const auto& [i, pr] : guess.ranked()) {
				if(shown++ >= 3 || pr < 0.01)
					break;
				char buf[96];
				std::snprintf(buf, sizeof buf, "%s %.0f%%, ", i < 0 ? "rogue" : lib->decks()[static_cast<size_t>(i)].name.c_str(), 100 * pr);
				g += buf;
			}
		}
		int seen_n = 0;
		for(const auto& [c, n] : seen)
			seen_n += n;
		std::fprintf(stderr, "hidden: turn %u, player %d, their hand %zu (%zu hidden), %zu guesses: %d with another hand, %d bad; %d seen; %s\n",
		             truth.turn, me, truth.hand[opp].size(), hidden_idx.size(), roots.size(), changed, bad, seen_n, g.c_str());
	}
	return roots;
}

std::map<uint32_t, int> LiveDuel::seen_cards(uint8_t me) const {
	const uint8_t opp = static_cast<uint8_t>(1 - me);
	std::map<uint32_t, int> out;
	const Board b = duel_->board();
	for(uint8_t p = 0; p < 2; ++p) {
		for(const auto* z : {&b.mzone[p], &b.szone[p]})
			for(const auto& c : *z)
				if(c.code && c.owner == opp && (c.pos & POS_FACEUP) && main_deck_card(db_, c.code))
					++out[c.code];
		for(const auto* z : {&b.grave[p], &b.banished[p]})
			for(const auto& c : *z)
				if(c.code && c.owner == opp && main_deck_card(db_, c.code))
					++out[c.code];
	}
	for(const auto& [code, n] : known_[opp])
		if(main_deck_card(db_, code))
			out[code] += n;
	return out;
}

DeckGuess LiveDuel::deck_guess(uint8_t me) const {
	const DeckLibrary* lib = library();
	if(!lib || lib->empty())
		return {};
	return guess_deck(*lib, seen_cards(me));
}

void LiveDuel::track_knowledge() {
	const Board b = duel_->board();
	std::map<uint32_t, int> drawn[2], shown[2];
	for(const auto& n : duel_->take_hand_news())
		++(n.drawn ? drawn : shown)[n.player][n.code];
	for(uint8_t p = 0; p < 2; ++p) {
		std::map<uint32_t, int> hand;
		for(const auto& c : b.hand[p])
			if(c.code)
				++hand[c.code];
		auto& known = known_[p];
		for(const auto& [code, n] : hand) {
			const int before = last_hand_[p].count(code) ? last_hand_[p][code] : 0;
			const int added = std::max(0, n - before);
			// new cards not drawn came from a public place or were searched (shown)
			const int d = drawn[p].count(code) ? drawn[p][code] : 0;
			known[code] += std::max(0, added - d);
			if(shown[p].count(code))
				known[code] += shown[p][code];
		}
		for(auto it = known.begin(); it != known.end();) {
			const int n = hand.count(it->first) ? hand[it->first] : 0;
			it->second = std::min(it->second, n);
			it = it->second > 0 ? std::next(it) : known.erase(it);
		}
		last_hand_[p] = std::move(hand);
	}
}

int LiveDuel::battle_instead(const Decision& d) const {
	// Ending Main Phase 1 with a monster that can attack a field it gets through (nothing there, or
	// something weaker): the Battle Phase option, -1 otherwise.
	if(d.type != DecisionType::Idle || d.player != duel_->turn_player() || duel_->phase() != PHASE_MAIN1)
		return -1;
	int battle = -1;
	for(size_t i = 0; i < d.options.size(); ++i)
		if(d.options[i].kind == OptKind::ToBattle)
			battle = static_cast<int>(i);
	if(battle < 0)
		return -1;
	const Board b = duel_->board();
	int strongest = 0, weakest = INT32_MAX;
	bool open = true;
	for(const auto& c : b.mzone[d.player])
		if(c.code && (c.pos & POS_FACEUP_ATTACK))
			strongest = std::max(strongest, c.atk);
	for(const auto& c : b.mzone[1 - d.player])
		if(c.code) {
			open = false;
			weakest = std::min(weakest, (c.pos & POS_FACEDOWN) ? INT32_MAX : (c.pos & POS_ATTACK) ? c.atk : c.def);
		}
	return strongest > 0 && (open || strongest > weakest) ? battle : -1;
}

bool LiveDuel::bring_forward(const std::string& label, size_t at) {
	// The plan keeps going with that play (and its choices) moved to `at`, the Set or Normal
	// Summon after it: no new search, the plan stays the plan.
	for(size_t k = at + 1; k < plan_.size(); ++k) {
		if(plan_[k].type != DecisionType::Idle || plan_[k].label != label)
			continue;
		size_t j = k + 1;
		while(j < plan_.size() && plan_[j].type != DecisionType::Idle)
			++j;
		std::vector<Step> block(plan_.begin() + static_cast<long>(k), plan_.begin() + static_cast<long>(j));
		plan_.erase(plan_.begin() + static_cast<long>(k), plan_.begin() + static_cast<long>(j));
		plan_.insert(plan_.begin() + static_cast<long>(at), block.begin(), block.end());
		return true;
	}
	return false;
}

namespace {
bool holds_back(const std::string& l) { return l.rfind("Set ", 0) == 0 || l.rfind("Normal Summon ", 0) == 0; }
}  // namespace

int LiveDuel::later_play(const Decision& d, size_t at) const {
	// When plan_[at] is a Set or a Normal Summon: the option of the plan's next other play of this
	// Main Phase, if it can be made now; -1 otherwise (or when plan_[at] is neither). A summon the
	// next play needs (its own effect, a Synchro using it) isn't skipped: that play isn't offered yet.
	if(at >= plan_.size() || !holds_back(plan_[at].label))
		return -1;
	for(size_t k = at + 1; k < plan_.size(); ++k) {
		const Step& s = plan_[k];
		if(s.player != d.player || s.type != DecisionType::Idle)
			continue;
		if(s.label.rfind("Go to ", 0) == 0)
			return -1;
		if(holds_back(s.label))
			continue;
		for(size_t o = 0; o < d.options.size(); ++o)
			if(d.options[o].label == s.label)
				return static_cast<int>(o);
		return -1;
	}
	return -1;
}

int LiveDuel::choose(BotLevel level) {
	const Decision& d = duel_->decision();
	if(d.options.empty())
		return -1;
	if(d.options.size() == 1)
		return 0;
	// Each bot keeps its own plan (bot against bot): the deciding player's is the one in use.
	if(plan_player_ != d.player && other_player_ == d.player) {
		std::swap(plan_, other_plan_);
		std::swap(plan_pos_, other_pos_);
		std::swap(plan_player_, other_player_);
	} else if(plan_player_ != d.player && plan_player_ >= 0) {
		other_plan_ = std::move(plan_);
		other_pos_ = plan_pos_;
		other_player_ = plan_player_;
		plan_.clear(), plan_pos_ = 0, plan_player_ = -1;
	}
	// The other player's passes the plan imagined but that didn't come (no window opened for them):
	// skipped, the plan goes on.
	// (Its own too: a chain window after its own play that the rules core didn't ask about here.)
	// And the other player's plays the plan's line had them make but they didn't (the search's line
	// is one imagined game): the plan goes on without them.
	while(plan_pos_ < plan_.size() && plan_player_ == d.player &&
	      ((plan_[plan_pos_].player != d.player) ||
	       (plan_[plan_pos_].type != d.type && (plan_[plan_pos_].label == "Don't respond" || plan_[plan_pos_].minor))))
		++plan_pos_;
	// A zone the plan's line chose that isn't offered now (other zones are free): the default one.
	if(plan_pos_ < plan_.size() && plan_player_ == d.player && d.type == DecisionType::Place &&
	   plan_[plan_pos_].type == DecisionType::Place && plan_[plan_pos_].player == d.player) {
		bool offered = false;
		for(const auto& o : d.options)
			offered = offered || o.label == plan_[plan_pos_].label;
		if(!offered)
			return 0;
	}
	// Follow the plan while it still matches (but not into a hand trap Normal Summoned that
	// nothing in the rest of the plan uses: decide again).
	// A chain window isn't followed into using a card either: the plan is one imagined line, and its
	// answers there are that line's random play (it once had a Mulcharmy Fuwalos chained in its
	// owner's own Standby Phase); using a card is decided with the checks below.
	bool plan_uses_card = (d.type == DecisionType::Chain || d.type == DecisionType::EffectYesNo) &&
	                      plan_pos_ < plan_.size() && plan_[plan_pos_].label != "Don't respond" &&
	                      plan_[plan_pos_].label.rfind("Don't use", 0) != 0;
	// (Its own engine cards during its own combo, not answering the opponent, are the plan itself:
	// followed. Searching again there replaced the planned turn with a fresh, shorter-sighted one.)
	if(plan_uses_card && d.player == duel_->turn_player() && duel_->chain_last_player() != 1 - d.player)
		for(const auto& o : d.options)
			if(o.label == plan_[plan_pos_].label && o.code && eval_->role(d.player, o.code).engine &&
			   !eval_->role(d.player, o.code).hand_trap)
				plan_uses_card = false;
	if(level != BotLevel::Easy && plan_pos_ < plan_.size() && plan_[plan_pos_].player == d.player &&
	   plan_player_ == d.player && !plan_uses_card && !unused_summon(d, plan_[plan_pos_].label, plan_, plan_pos_ + 1)) {
		for(size_t i = 0; i < d.options.size(); ++i)
			if(d.options[i].label == plan_[plan_pos_].label) {
				// A Set or a Normal Summon waits while another play of this Main Phase in the plan
				// can be made now (it is only played once the next play needs it).
				if(d.type == DecisionType::Idle) {
					int later = later_play(d, plan_pos_);
					if(later >= 0) {
						if(!bring_forward(d.options[static_cast<size_t>(later)].label, plan_pos_))
							plan_.clear(), plan_pos_ = 0;
						return later;
					}
				}
				if(d.options[i].label == "Go to End Phase") {
					const int b = battle_instead(d);
					if(b >= 0) {
						plan_.clear(), plan_pos_ = 0;
						return b;
					}
				}
				return static_cast<int>(i);
			}
	}
	if(std::getenv("YGOE_PLAN_DEBUG") && level != BotLevel::Easy) {
		if(plan_pos_ < plan_.size())
			std::fprintf(stderr, "plan not followed at step %zu/%zu (P%d %s): the plan has P%d '%s'%s\n", plan_pos_, plan_.size(), d.player,
			             decision_name(d.type), plan_[plan_pos_].player, plan_[plan_pos_].label.substr(0, 60).c_str(),
			             plan_uses_card ? " (a card use: decided again)" : "");
		else
			std::fprintf(stderr, "no plan for P%d (%s), %zu steps held%s\n", d.player, decision_name(d.type), plan_.size(),
			             plan_player_ == d.player ? "" : " by the other bot");
	}
	if(level == BotLevel::Easy) {
		std::vector<double> w(d.options.size());
		for(size_t i = 0; i < w.size(); ++i)
			w[i] = playout_weight(d, d.options[i], duel_->turn_player(), 0);
		std::discrete_distribution<size_t> pick(w.begin(), w.end());
		return static_cast<int>(pick(rng_));
	}
	// The Battle Phase on an open field: every monster that can attack directly does, the strongest
	// first. (The search weighs attacks by random playouts and can rate ending the Battle Phase as
	// well as the damage; with nothing to block there is no reason to hold an attack back.)
	if(d.type == DecisionType::Battle && d.player == duel_->turn_player()) {
		const Board b = duel_->board();
		bool open = true;
		for(const auto& c : b.mzone[1 - d.player])
			open = open && !c.code;
		if(open) {
			int best = -1, best_atk = -1;
			for(size_t i = 0; i < d.options.size(); ++i) {
				const Option& o = d.options[i];
				if(o.kind != OptKind::Attack)
					continue;
				int atk = 0;
				for(const auto& c : b.mzone[d.player])
					if(c.code == o.code && (c.seq == o.seq || atk == 0))
						atk = c.atk;
				if(atk > best_atk)
					best = static_cast<int>(i), best_atk = atk;
			}
			if(best >= 0 && best_atk > 0) {
				plan_.clear(), plan_pos_ = 0;
				return best;
			}
		}
	}
	// Plan the rest of this turn from the exact current state.
	SearchConfig cfg;
	cfg.threads = threads_;
	cfg.iterations = level == BotLevel::Strong ? 2500 : 800;
	cfg.seconds = level == BotLevel::Strong ? 25 : 10;
	// Its own turn, a Main or Battle Phase play: the search's best line is then planned through to
	// the end of the turn (improve_line), and that plan is played. The time is shared between them.
	const bool plans_turn = d.player == duel_->turn_player() && d.type != DecisionType::Chain && d.type != DecisionType::EffectYesNo &&
	                        d.type != DecisionType::Battle;
	// (a smaller search budget, as self-play sets: the planner's time shrinks with it)
	const double plan_seconds = !plans_turn ? 0
	                            : (level == BotLevel::Strong ? 9.0 : 3.0) *
	                                  (iterations_ > 0 ? std::min(1.0, iterations_ / (level == BotLevel::Strong ? 2500.0 : 800.0)) : 1.0);
	cfg.seconds -= plan_seconds;
	if(std::getenv("YGOE_NO_PLANNER"))
		cfg.seconds += plan_seconds;
	if(iterations_ > 0)
		cfg.iterations = iterations_;
	// The opponent's interruption on the chain against the bot's own turn: a window where the
	// right answer (protecting the combo, a card that dodges the hand trap) matters most, and the
	// options and hidden cards split the search: search it twice as long.
	if((d.type == DecisionType::Chain || d.type == DecisionType::EffectYesNo) && duel_->turn_player() == d.player &&
	   duel_->chain_last_player() == 1 - d.player)
		cfg.iterations *= 2;
	// This bot's value network (players can have different ones, to compare them).
	eval_->set_value_net(net_set_[d.player] ? nets_[d.player] : db_.value_net());
	cfg.horizon_turns = 1;
	cfg.cancel = &cancel_;
	cfg.guide = guides_[d.player];
	cfg.imitate = imitate_[d.player];
	cfg.policy = policies_[d.player];
	cfg.priorities = priorities_[d.player];
	// The bot does not see the other player's hidden cards: its search samples them.
	std::vector<RootState> roots = hidden_roots(d.player, 8);
	auto out = searcher_->run(roots, d.player, cfg);
	if(out.moves.empty())
		return 0;
	if(search_hook_)
		search_hook_(d, out);
	// Combo turns are long, and random playouts after a strong first card are noisy, so the
	// average alone undersells good starters. Rank well-explored moves by a blend of their
	// average and the best complete line found through them.
	const MoveStats* pick = &out.moves.front();
	const bool response = d.type == DecisionType::Chain || d.type == DecisionType::EffectYesNo;
	if(response) {
		// Using a card in a chain window spends it; the search's horizon cannot see the better
		// moment it is being saved for. So rank by the average only (the best single line is
		// noisy here, it depends on random opponent play) and use it only for a clear gain.
		const MoveStats* pass = nullptr;
		for(const auto& m : out.moves)
			if(m.label == "Don't respond" || m.label.rfind("Don't use", 0) == 0)
				pass = &m;
		int most = out.moves.front().visits;
		const MoveStats* use = nullptr;
		for(const auto& m : out.moves)
			if(&m != pass && m.visits >= std::max(8, most / 6) && (!use || m.mean > use->mean))
				use = &m;
		// Spending a card from the hand, or acting on the opponent's turn, needs a clear gain;
		// an effect of a card already on your field during your own turn only a small one.
		// (The deck's own engine cards from the hand during its own turn are part of the combo, not a
		// hand trap being spent: the small margin, so answers like Verdict placing Bastion against
		// the opponent's Ghost Ogre get played.)
		double margin = 0.005;
		uint32_t use_code = 0;
		if(use) {
			for(const auto& o : d.options)
				if(o.label == use->label) {
					use_code = o.code;
					const bool own_engine = d.player == duel_->turn_player() && o.code && eval_->role(d.player, o.code).engine &&
					                        !eval_->role(d.player, o.code).hand_trap;
					if(((o.loc & LOCATION_HAND) || d.player != duel_->turn_player()) && !own_engine)
						margin = 0.03;
				}
		}
		// An optional trigger ("Use X?": Endgame Problem's draw in the opponent's Standby Phase) is
		// now or never: there is no better moment to save it for, so no margin. (Not a negate: one
		// that waits for a better target keeps it.)
		if(use_code && d.type == DecisionType::EffectYesNo && !eval_->role(d.player, use_code).hand_trap) {
			bool negates = false;
			for(const auto& in : eval_->role(d.player, use_code).interrupts)
				negates |= in.kind == Interrupt::NegateActivation || in.kind == Interrupt::NegateEffect ||
				           in.kind == Interrupt::NegateSummon;
			if(!negates)
				margin = 0.0;
		}
		// A hand trap on its owner's own turn answers the opponent's link (Ash on their Ash...);
		// with none on top, using it (Mulcharmy Fuwalos in its own Standby Phase) mostly throws it
		// away: only for a very clear gain.
		if(use_code && eval_->role(d.player, use_code).hand_trap && d.player == duel_->turn_player() &&
		   duel_->chain_last_player() != 1 - d.player)
			margin = std::max(margin, 0.10);
		// Does the card stop the link on top of the chain? The shortcuts below that make the bot
		// answer more readily (a choke point, a starter from the Deck...) only count when it does:
		// an Infinite Impermanence can't stop a Spell or a GY effect, and would land on any monster.
		const uint32_t top_code = duel_->chain_last_code();
		bool stops_top = true;
		if(use_code && top_code) {
			const CardRole& ur = eval_->role(d.player, use_code);
			if(!ur.interrupts.empty()) {
				std::string tt = db_.text(db_.script_id(top_code));
				for(auto& ch : tt)
					ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				const uint8_t tl = duel_->chain_last_loc();
				stops_top = false;
				for(const auto& in : ur.interrupts) {
					switch(in.kind) {
					case Interrupt::NegateActivation: stops_top = true; break;
					case Interrupt::NegateEffect: stops_top |= (tl & (LOCATION_MZONE | LOCATION_SZONE)) != 0; break;
					case Interrupt::GyHate:
						stops_top |= tt.find("from your deck") != std::string::npos || tt.find("graveyard") != std::string::npos ||
						             tt.find("gy") != std::string::npos;
						break;
					case Interrupt::Removal: stops_top |= (tl & (LOCATION_MZONE | LOCATION_SZONE)) != 0; break;
					case Interrupt::Other: stops_top = true; break;
					default: break;
					}
				}
			}
		}
		// Answering the turn player's combo with what was learned about their deck: stop a known
		// choke point now, and stop the first play when the interruptions in hand / set are as
		// many as the plays that deck usually has left (they won't run out of plays first).
		const uint8_t opp = static_cast<uint8_t>(1 - d.player);
		const DeckKnowledge& know = knowledge_[opp];
		const uint32_t top = duel_->chain_last_code();
		if(use && stops_top && d.type == DecisionType::Chain && duel_->turn_player() == opp && top &&
		   duel_->chain_last_player() == opp) {
			if(std::find(know.chokes.begin(), know.chokes.end(), top) != know.chokes.end()) {
				margin = -0.01;
			} else if(know.plays > 0) {
				int theirs = 0;  // their activations this turn so far
				const auto& ev = duel_->events();
				for(size_t i = ev.size(); i-- > 0;) {
					if(ev[i].kind == Event::NewTurn)
						break;
					if(ev[i].kind == Event::Chaining && ev[i].player == opp)
						++theirs;
				}
				Board b = duel_->board();
				int mine = 0;
				// (not counting one whose once-per-turn use is spent this turn: a second Ash after the first)
				for(const auto& c : b.hand[d.player])
					if(c.code && !c.spent() && eval_->role(d.player, c.code).hand_trap)
						++mine;
				for(const auto& c : b.szone[d.player])
					if(c.code && !c.spent() && (c.pos & POS_FACEDOWN) && !eval_->role(d.player, c.code).interrupts.empty())
						++mine;
				if(mine >= know.plays - theirs)
					margin = 0.0;
			}
			// One of their first plays this turn that searches or summons from the Deck (a
			// starter): stopping it can stop the whole turn, which the search undersells (the hands
			// it imagines for them hold more starters than a real one usually does). Use the card
			// whenever the search rates it at least as well as passing.
			int theirs = 0;
			for(size_t i = duel_->events().size(); i-- > 0;) {
				const Event& e = duel_->events()[i];
				if(e.kind == Event::NewTurn)
					break;
				if(e.kind == Event::Chaining && e.player == opp)
					++theirs;
			}
			std::string text = db_.text(db_.script_id(top));
			for(auto& ch : text)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			const bool from_deck = text.find("from your deck") != std::string::npos;
			if(theirs <= 2 && from_deck)
				margin = std::min(margin, 0.0);
		}
		// A non-engine card (hand trap, interrupting Set): what the learned timing says about using
		// it now rather than holding it, added to the search's view (nonengine.h).
		bool ne_card = false;
		NeSituation ns;
		if(use && pass) {
			uint32_t code = 0;
			for(const auto& o : d.options)
				if(o.label == use->label)
					code = o.code;
			const CardRole& r = code ? eval_->role(d.player, code) : CardRole{};
			ns.stops = code ? ne_stops(r) : 0;
			ne_card = code && (r.hand_trap || (!r.engine && ns.stops));
			if(ne_card) {
				const uint8_t o2 = static_cast<uint8_t>(1 - d.player);
				ns.search_gain = use->mean - pass->mean;
				ns.their_turn = duel_->turn_player() == o2;
				ns.hand_trap = r.hand_trap;
				ns.chain_depth = duel_->chain_depth();
				ns.turn = static_cast<int>(duel_->turn());
				for(size_t i = duel_->events().size(); i-- > 0;) {
					const Event& e = duel_->events()[i];
					if(e.kind == Event::NewTurn)
						break;
					if(e.kind == Event::Chaining && e.player == o2)
						++ns.their_plays;
				}
				const uint32_t top2 = duel_->chain_last_code();
				if(top2 && duel_->chain_last_player() == o2) {
					std::string text = db_.text(db_.script_id(top2));
					for(auto& ch : text)
						ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
					ns.deck_play = text.find("from your deck") != std::string::npos;
					const auto& ck = knowledge_[o2].chokes;
					ns.choke = std::find(ck.begin(), ck.end(), top2) != ck.end();
				}
				Board b = duel_->board();
				for(const auto& c : b.hand[d.player])
					if(c.code && !c.spent() && eval_->role(d.player, c.code).hand_trap)
						++ns.my_interruptions;
				for(const auto& c : b.szone[d.player])
					if(c.code && !c.spent() && (c.pos & POS_FACEDOWN) && !eval_->role(d.player, c.code).interrupts.empty())
						++ns.my_interruptions;
				ns.their_hand = static_cast<int>(b.hand[o2].size());
				ns.their_field = b.count_field(o2);
				ns.my_field = b.count_field(d.player);
				auto timing = ne_timing_set_[d.player & 1] ? ne_timing_[d.player & 1] : db_.nonengine_timing();
				if(timing)
					margin -= std::clamp(timing->gain(ns), -0.05, 0.05);
			}
		}
		// A negation that targets a face-up card, used on the opponent's turn: worth it only if one of
		// their face-up cards can still do something this turn (an effect it can activate now or
		// later, or a lasting one), or the link it answers is one of those cards. Otherwise held.
		if(use && use_code && d.player != duel_->turn_player()) {
			const CardRole& ur = eval_->role(d.player, use_code);
			bool only_negate = !ur.interrupts.empty() && !ur.hand_trap;
			for(const auto& in : ur.interrupts)
				only_negate &= in.kind == Interrupt::NegateEffect;
			if(only_negate && !(top_code && (duel_->chain_last_loc() & (LOCATION_MZONE | LOCATION_SZONE)))) {
				const uint8_t o2 = static_cast<uint8_t>(1 - d.player);
				if(!live_target(o2))
					margin = std::max(margin, 1.0);
			}
		}
		// A card that clears the opponent's whole field (Medicurius: "banish ... all cards your
		// opponent controls"), used on their turn while they control little: most of what it can hit
		// is still to come this turn. Held unless the gain is clear (it stops a choke point, it wins).
		if(use && use_code && d.player != duel_->turn_player()) {
			std::string ut = db_.text(db_.script_id(use_code));
			for(auto& ch : ut)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			const bool wipe = ut.find("all cards your opponent controls") != std::string::npos ||
			                  ut.find("all monsters your opponent controls") != std::string::npos ||
			                  ut.find("all cards on the field") != std::string::npos ||
			                  ut.find("all other cards on the field") != std::string::npos;
			if(wipe && duel_->board().count_field(static_cast<uint8_t>(1 - d.player)) < 3)
				margin = std::max(margin, 0.08);
		}
		// A negate on the opponent's turn waits for their activation: it answers the effect activated,
		// on the link it stops. In an open window (a Summon, a phase change) it needs a very clear gain
		// (a lethal board coming), a clear one where waiting doesn't work: a card with a lasting effect
		// (no activation to wait for), a deck that can take its monster away in response to the negate
		// (Regina sending Lucina, so Veiler goes on Lucina before her effect), or the turn's End Phase
		// (no later window). Removal isn't held: answering a Summon or an attacker is its normal use.
		if(use && use_code && d.player != duel_->turn_player()) {
			const CardRole& ur = eval_->role(d.player, use_code);
			bool stopper = false, on_summon = false;
			for(const auto& in : ur.interrupts) {
				stopper |= in.kind == Interrupt::NegateActivation || in.kind == Interrupt::NegateEffect;
				on_summon |= in.kind == Interrupt::NegateSummon;
			}
			const uint8_t o2 = static_cast<uint8_t>(1 - d.player);
			const bool answers = duel_->chain_depth() > 0 && duel_->chain_last_player() == o2;
			if(stopper && !on_summon && !answers) {
				const bool early_ok = lasting_target(o2) || (dodges_[o2] && live_target(o2)) || duel_->phase() == PHASE_END;
				margin = std::max(margin, early_ok ? 0.06 : 0.10);
				if(std::getenv("YGOE_BOT_DEBUG"))
					std::fprintf(stderr, "   %s: no activation to answer, %s\n", db_.name(use_code).c_str(),
					             early_ok ? "used early only for a clear gain" : "used only for a very clear gain");
			}
		}
		bool used = !pass || (use && use->mean > pass->mean + margin);
		if(ne_card && ne_explore_ > 0 && std::uniform_real_distribution<double>(0, 1)(rng_) < ne_explore_)
			used = !used;  // self-play: both choices get seen
		if(ne_card && ne_hook_)
			ne_hook_(d.player, ne_features(ns), used);
		pick = pass;
		if(used)
			pick = use ? use : &out.moves.front();
	} else {
		int most = out.moves.front().visits;
		double best_score = -1;
		for(const auto& m : out.moves) {
			if(m.visits < std::max(8, most / 6) || m.best.empty())
				continue;
			double score = 0.4 * m.mean + 0.6 * m.best.front().reward;
			if(score > best_score) {
				best_score = score;
				pick = &m;
			}
		}
		// A choice players make that the engine doesn't understand yet: take it unless the search
		// found something clearly better.
		if(!imitate_[d.player].empty() && d.type != DecisionType::Idle && d.type != DecisionType::Battle) {
			const uint32_t ctx1 = last_action_[d.player], ctx2 = duel_->chain_last_code();
			for(const auto& m : out.moves) {
				uint32_t code = 0;
				for(const auto& o : d.options)
					if(o.label == m.label)
						code = o.code;
				bool wanted = false;
				for(const auto& im : imitate_[d.player])
					wanted |= code && im.choose == code && (im.action == ctx1 || im.action == ctx2);
				if(wanted && m.visits > 0 && m.mean >= pick->mean - 0.05) {
					pick = &m;
					break;
				}
			}
		}
	}
	// Strong, at the start of its own Main Phase: play the opponent's next turn against the
	// end board of each of the best few candidates and keep the one that holds up best.
	std::vector<std::pair<const MoveStats*, Trajectory>> repaired;
	auto repair = [&](const MoveStats& m) {
		Trajectory t = m.best.front();
		const RootState& r = roots[std::min(t.root, roots.size() - 1)];
		searcher_->defer_sets(r, t, d.player, cfg.horizon_turns);
		searcher_->defer_summons(r, t, d.player, cfg.horizon_turns);
		searcher_->drop_idle_summons(r, t, d.player, cfg.horizon_turns);
		searcher_->drop_idle_responses(r, t, d.player, cfg.horizon_turns);
		return t;
	};
	const bool check_boards = level == BotLevel::Strong && d.type == DecisionType::Idle &&
	                          d.player == duel_->turn_player() && duel_->phase() == PHASE_MAIN1;
	if(check_boards && !cancel_) {
		std::vector<std::pair<double, const MoveStats*>> ranked;
		int most = out.moves.front().visits;
		for(const auto& m : out.moves)
			if(!m.best.empty() && m.visits >= std::min(2, most))
				ranked.push_back({0.4 * m.mean + 0.6 * m.best.front().reward, &m});
		std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
		if(ranked.size() > 3)
			ranked.resize(3);
		double best_total = -1;
		for(auto& [own, m] : ranked) {
			Trajectory t = repair(*m);
			RootState after{roots[std::min(t.root, roots.size() - 1)].setup, history_};
			after.history.insert(after.history.end(), t.responses.begin(), t.responses.end());
			SearchConfig vc;
			vc.iterations = 250;
			vc.seconds = 6;
			vc.horizon_turns = 1;
			vc.cancel = &cancel_;
			auto vo = searcher_->run({after}, d.player, vc);
			if(vo.moves.empty())
				continue;
			const MoveStats* reply = &vo.moves.front();
			if(vo.root_player != d.player)
				for(const auto& r : vo.moves)
					if(r.visits >= std::max(10, vo.moves.front().visits / 4) && r.mean < reply->mean)
						reply = &r;
			double total = 0.35 * t.reward + 0.65 * reply->mean;
			if(std::getenv("YGOE_BOT_DEBUG"))
				fprintf(stderr, "   board check: own %.3f, after their turn %.3f  (%s)\n", t.reward, reply->mean,
				        m->label.substr(0, 60).c_str());
			if(total > best_total) {
				best_total = total;
				pick = m;
				repaired.clear();
				repaired.push_back({m, std::move(t)});
			}
		}
	}
	bool explored = false;
	// Self-play exploration: early in the game, sometimes another well-explored move.
	if(explore_p_ > 0 && !response && (d.type == DecisionType::Idle || d.type == DecisionType::Battle) &&
	   explored_[d.player & 1]++ < explore_moves_ && std::uniform_real_distribution<double>(0, 1)(rng_) < explore_p_) {
		const int most = out.moves.front().visits;
		std::vector<const MoveStats*> other;
		for(const auto& m : out.moves)
			if(&m != pick && m.visits >= std::max(8, most / 6))
				other.push_back(&m);
		if(!other.empty()) {
			pick = other[std::uniform_int_distribution<size_t>(0, other.size() - 1)(rng_)];
			repaired.clear();
			explored = true;
		}
	}
	const MoveStats& best = *pick;
	std::string want = best.label;
	if(std::getenv("YGOE_BOT_DEBUG")) {
		fprintf(stderr, "bot P%d turn %u, %d iterations:\n", d.player, duel_->turn(), out.iterations);
		for(size_t i = 0; i < out.moves.size() && i < 8; ++i)
			fprintf(stderr, "   %5d  %.3f  %s\n", out.moves[i].visits, out.moves[i].mean, out.moves[i].label.substr(0, 90).c_str());
		for(size_t i = 0; i < out.moves.size() && i < 3; ++i) {
			if(out.moves[i].best.empty())
				continue;
			const Trajectory& t = out.moves[i].best.front();
			fprintf(stderr, "   best for #%zu: reward %.3f\n", i + 1, t.reward);
			for(auto& l : describe(t, d.player, true))
				fprintf(stderr, "      %s\n", l.substr(0, 140).c_str());
			Board fb;
			if(searcher_->final_board(roots[std::min(t.root, roots.size() - 1)], t, fb)) {
				auto ev = eval_->evaluate(fb, d.player, 1 - d.player, false, -1, true);
				fprintf(stderr, "      values me %.2f opp %.2f\n", ev.value[d.player], ev.value[1 - d.player]);
				for(int pl = 0; pl < 2; ++pl) {
					std::string f;
					for(auto& c : fb.mzone[pl]) f += db_.name(c.code) + (c.owner != pl ? "(*)" : "") + ", ";
					for(auto& c : fb.szone[pl]) f += "[" + db_.name(c.code) + "], ";
					fprintf(stderr, "      P%d field: %s hand %zu\n", pl, f.c_str(), fb.hand[pl].size());
				}
			}
		}
		auto b = duel_->board();
		auto opt = [](const CardView& c) {
			return c.opt_total ? " [once per turn: " + std::to_string(c.opt_left) + "/" + std::to_string(c.opt_total) + " left]" : std::string();
		};
		std::string hand, field;
		for(auto& c : b.hand[d.player])
			hand += db_.name(c.code) + opt(c) + "; ";
		for(int pl = 0; pl < 2; ++pl)
			for(const auto* z : {&b.mzone[pl], &b.szone[pl]})
				for(auto& c : *z)
					if(c.code)
						field += "P" + std::to_string(pl) + " " + db_.name(c.code) + opt(c) + "; ";
		fprintf(stderr, "   hand: %s\n   field: %s\n", hand.c_str(), field.c_str());
	}
	plan_.clear();
	plan_pos_ = 0;
	if(!best.best.empty()) {
		// Develop first, set last: the same end board with fewer chances for the opponent
		// to hit set cards, and no Spell/Trap Zone blocked mid-combo.
		Trajectory t = !repaired.empty() && repaired.front().first == &best ? repaired.front().second : repair(best);
		// The turn planner: the whole line looked at again, choice by choice, then what does nothing
		// for the end of the turn taken out. (Not while exploring for self-play: that move is the point.)
		static const bool no_planner = std::getenv("YGOE_NO_PLANNER") != nullptr;
		if(plans_turn && !explored && !no_planner && !cancel_) {
			const RootState& r = roots[std::min(t.root, roots.size() - 1)];
			if(!stale_plan_.empty() && stale_turn_ == duel_->turn() && stale_player_ == d.player) {
				Trajectory old;
				old.root = t.root;
				old.steps = stale_plan_;
				if(searcher_->replay_labels(r, old, d.player, cfg.horizon_turns, -1e9, true) && old.reward > t.reward) {
					if(std::getenv("YGOE_PLAN_DEBUG"))
						std::fprintf(stderr, "plan: the rest of the earlier plan is better (%.3f > %.3f)\n", old.reward, t.reward);
					t = std::move(old);
				}
			}
			stale_plan_.clear();
			const Trajectory searched = t;  // (the search's line, before planning: a candidate for the turn goal)
			// The puzzle (Main Phase 1): the deck's combo pieces put together in different orders; the
			// planner plays each and starts from the best if it ends better than the search's line.
			std::vector<std::vector<std::string>> arrangements;
			if(d.type == DecisionType::Idle && duel_->phase() == PHASE_MAIN1 && guides_[d.player]) {
				std::mt19937_64 prng(duel_->turn() * 7919u + d.player);
				arrangements = guides_[d.player]->arrangements(resources_of(db_, duel_->board(), d.player), prng,
				                                               level == BotLevel::Strong ? 48 : 20);
			}
			if(searcher_->improve_line(r, t, d.player, cfg, plan_seconds, &arrangements, plan_seconds * 0.4) | searcher_->drop_idle_plays(r, t, d.player, cfg.horizon_turns)) {
				searcher_->defer_sets(r, t, d.player, cfg.horizon_turns);
				searcher_->defer_summons(r, t, d.player, cfg.horizon_turns);
			}
			// The turn goal (turn 2 on, Main Phase 1): a line that wins this turn, if one still wins
			// when the opponent answers; else the planned turn (take their resources, leave a board).
			// Each candidate is scored by what it is worth when the opponent answers (win_rate's
			// value): the planned line (planned with the opponent passing), a winning line, the
			// search's own line (it already weighed their answers).
			// (Turn 1 too: going first, the opponent's hand traps are their answers.)
			if(d.type == DecisionType::Idle && duel_->phase() == PHASE_MAIN1 && !cancel_) {
				const double scale = plan_seconds / (level == BotLevel::Strong ? 9.0 : 3.0);
				const int samples = level == BotLevel::Strong ? 3 : 2;
				const bool dbg = std::getenv("YGOE_PLAN_DEBUG") != nullptr;
				struct Cand {
					const char* what;
					Trajectory line;
					double wins = 0, value = 0;
				};
				std::vector<Cand> cands;
				cands.push_back({"the planned turn", t});
				// The deck's plan (its pieces): a deck whose going-second lines win looks harder for
				// the win; one whose lines mostly only break the board and build (a grind deck) less.
				double reach_scale = 1.0;
				if(guides_[d.player]) {
					const auto pl = guides_[d.player]->plan();
					if(pl.breakers)
						reach_scale = pl.grind() ? 0.5 : 1.0 + pl.lethal_reach();
					if(dbg && duel_->turn() >= 2)
						std::fprintf(stderr, "deck plan: %d end-board pieces, %d going-second pieces, %d of them winning%s\n",
						             pl.end_board, pl.breakers, pl.wins, pl.grind() ? " (a grind deck)" : "");
				}
				Trajectory lethal;
				if(duel_->turn() >= 2 && t.reward < 0.999 &&
				   searcher_->seek_lethal(roots, d.player, cfg, (level == BotLevel::Strong ? 4.0 : 1.5) * scale * reach_scale, lethal))
					cands.push_back({"a winning line", lethal});
				// (not when it just ends the turn: a line that does nothing has nothing for the
				// opponent to answer, so it scores as if it lost nothing)
				if(searched.responses != t.responses && first_action(searched, d.player).rfind("Go to End Phase", 0) != 0)
					cands.push_back({"the search's line", searched});
				size_t pick_c = 0;
				for(size_t i = 0; i < cands.size(); ++i) {
					cands[i].wins = searcher_->win_rate(roots, cands[i].line, d.player, cfg, samples, &cands[i].value);
					if(dbg)
						std::fprintf(stderr, "turn goal: %s wins %.0f%% when the opponent answers, worth %.3f\n", cands[i].what,
						             100 * cands[i].wins, cands[i].value);
					if(cands[i].value > cands[pick_c].value + 0.02)
						pick_c = i;
				}
				if(pick_c != 0) {
					t = std::move(cands[pick_c].line);
					t.root = std::min(t.root, roots.size() - 1);
				}
			}
			if(std::getenv("YGOE_BOT_DEBUG")) {
				fprintf(stderr, "   plan (%.3f):\n", t.reward);
				for(auto& l : describe(t, d.player, true))
					fprintf(stderr, "      %s\n", l.substr(0, 140).c_str());
			}
		}
		plan_ = t.steps;
		plan_player_ = d.player;
		if(!plan_.empty() && plan_.front().player == d.player)
			want = plan_.front().label;
		// A response the bot decided to make stays made: the clean-up judges "not needed" on one
		// imagined line, which must not overrule the decision itself. Search again afterwards.
		if(response && want != best.label) {
			want = best.label;
			plan_.clear();
			plan_pos_ = 0;
		}
		// A Set or a Normal Summon goes as late as it can: if the plan has another play of this
		// Main Phase available now, make that one first and search again afterwards. The card
		// stays in the hand until it is really the next thing to do, and if the opponent
		// interrupts in between, the bot decides again with the new position. (A summon the next
		// play needs, like a starter whose effect comes next, isn't skipped: that play isn't
		// available yet.)
		if(d.type == DecisionType::Idle && !plan_.empty() && plan_.front().label == want) {
			int later = later_play(d, 0);
			if(later >= 0) {
				want = d.options[static_cast<size_t>(later)].label;
				if(!bring_forward(want, 0))
					plan_.clear(), plan_pos_ = 0;
			}
		}
	}
	// A hand trap Normal Summoned with nothing later in the line that uses it (as material, for an
	// effect) only leaves it on the field, worth next to nothing there and gone from the hand: the
	// best other move instead.
	if(d.type == DecisionType::Idle && unused_summon(d, want, plan_, plan_.empty() ? 0 : 1)) {
		int most = out.moves.front().visits;
		double best_score = -1;
		std::string other;
		for(const auto& m : out.moves) {
			if(m.visits < std::max(8, most / 6) || m.best.empty() || unused_summon(d, m.label, m.best.front().steps, 1))
				continue;
			double score = 0.4 * m.mean + 0.6 * m.best.front().reward;
			if(score > best_score) {
				best_score = score;
				other = m.label;
			}
		}
		if(!other.empty()) {
			want = other;
			plan_.clear();
			plan_pos_ = 0;
		}
	}
	// Phases. Ending Main Phase 1 with a monster that can attack: to the Battle Phase first (see
	// battle_instead). Going to battle with nothing that can attack only skips the rest of Main
	// Phase 1: the best other play instead.
	if(d.type == DecisionType::Idle && want == "Go to End Phase") {
		const int b = battle_instead(d);
		if(b >= 0) {
			want = d.options[static_cast<size_t>(b)].label;
			plan_.clear(), plan_pos_ = 0;
		}
	} else if(d.type == DecisionType::Idle && d.player == duel_->turn_player() && duel_->phase() == PHASE_MAIN1 &&
	          want == "Go to Battle Phase") {
		bool attacker = false;
		for(const auto& c : duel_->board().mzone[d.player])
			attacker = attacker || (c.code && (c.pos & POS_FACEUP_ATTACK));
		const int most = out.moves.front().visits;
		const MoveStats* other = nullptr;
		for(const auto& m : out.moves) {
			if(attacker || m.visits < std::max(4, most / 10) || m.label.rfind("Go to ", 0) == 0)
				continue;
			if(!other || m.mean > other->mean)
				other = &m;
		}
		if(other) {
			want = other->label;
			plan_.clear(), plan_pos_ = 0;
		}
	}
	int index = 0;
	for(size_t i = 0; i < d.options.size(); ++i)
		if(d.options[i].label == want)
			index = static_cast<int>(i);
	return index;
}

bool LiveDuel::unused_summon(const Decision& d, const std::string& label, const std::vector<Step>& line, size_t from) const {
	if(label.rfind("Normal Summon ", 0) != 0)
		return false;
	uint32_t code = 0;
	for(const auto& o : d.options)
		if(o.label == label)
			code = o.code;
	if(!code || !eval_->role(d.player, code).hand_trap)
		return false;
	const std::string name = db_.name(code);
	for(size_t k = from; k < line.size(); ++k) {
		const Step& s = line[k];
		if(s.player != d.player)
			continue;
		if(s.label.find(name) != std::string::npos)
			return false;  // used later in the line (material, its own effect...)
	}
	return true;
}

bool LiveDuel::lasting_target(uint8_t p) const {
	constexpr uint32_t kField = 0x2, kContinuous = 0x800;
	const Board b = duel_->board();
	for(const auto* z : {&b.mzone[p], &b.szone[p]})
		for(const auto& c : *z) {
			if(!c.code || !(c.pos & POS_FACEUP) || (c.status & 0x1))
				continue;
			const CardInfo* ci = db_.get(db_.script_id(c.code));
			if(!ci)
				continue;
			for(const auto& e : ci->effects)
				if((e.range & (LOCATION_MZONE | LOCATION_SZONE)) && (e.type & (kField | kContinuous)))
					return true;
		}
	return false;
}

bool LiveDuel::live_target(uint8_t p) const {
	// A face-up card of `p` with an effect it can still activate this turn (an ignition one on its
	// own turn, a quick or trigger one, not a once-per-turn one it has used), or a lasting one while
	// it is on the field.
	// (EFFECT_TYPE_*; an effect without a range, like a summon trigger, has already happened by now)
	constexpr uint32_t kIgnition = 0x40, kTriggerO = 0x80, kQuickO = 0x100, kTriggerF = 0x200, kQuickF = 0x400, kField = 0x2,
	                   kContinuous = 0x800;
	const Board b = duel_->board();
	for(const auto* z : {&b.mzone[p], &b.szone[p]})
		for(const auto& c : *z) {
			if(!c.code || !(c.pos & POS_FACEUP) || (c.status & 0x1))  // STATUS_DISABLED: already negated
				continue;
			const CardInfo* ci = db_.get(db_.script_id(c.code));
			if(!ci)
				continue;
			// what of its once-per-turn effects is left this turn (the rules core's count)
			const bool limited_left = c.opt_left > 0;
			for(const auto& e : ci->effects) {
				if(!(e.range & (LOCATION_MZONE | LOCATION_SZONE)))
					continue;
				if(e.count_limit > 0 && !limited_left && (e.type & (kIgnition | kQuickO | kQuickF | kTriggerO | kTriggerF)))
					continue;  // a once-per-turn effect already used this turn
				if((e.type & (kQuickO | kQuickF | kTriggerO | kTriggerF | kField | kContinuous)) ||
				   ((e.type & kIgnition) && duel_->turn_player() == p))
					return true;
			}
		}
	return false;
}

std::string LiveDuel::bot_move(BotLevel level) {
	if(!ok_ || duel_->ended())
		return {};
	cancel_ = false;
	int i = choose(level);
	if(i < 0)
		return {};
	std::string label = duel_->decision().options[i].label;
	if(!respond_internal(static_cast<size_t>(i))) {
		// Shouldn't happen; fall back to any answer the core accepts.
		plan_.clear();
		bool done = false;
		for(size_t k = 0; k < duel_->decision().options.size() && !done; ++k)
			if(respond_internal(k)) {
				label = last_move_;
				done = true;
			}
		if(!done)
			return {};
	}
	auto_resolve();
	return label;
}

std::string LiveDuel::card_json(uint32_t code) const {
	return "{\"code\":" + std::to_string(db_.official_id(code)) + ",\"name\":" + quote(db_.name(code)) + "}";
}

std::string LiveDuel::state_json() {
	std::ostringstream o;
	// Always the C locale: inside a desktop JVM app the process-wide C++ locale can lack the
	// number facets (writing a number then crashed), and JSON needs plain digits anyway.
	o.imbue(std::locale::classic());
	if(!ok_) {
		o << "{\"error\":" << quote(error_) << "}";
		return o.str();
	}
	Board b = duel_->board(true);
	o << "{\"turn\":" << duel_->turn() << ",\"turn_player\":" << int(duel_->turn_player()) << ",\"phase\":"
	  << quote(phase_text(duel_->phase())) << ",\"ended\":" << (duel_->ended() ? "true" : "false")
	  << ",\"winner\":" << duel_->winner() << ",\"last_move\":" << quote(last_move_) << ",\"players\":[";
	for(int p = 0; p < 2; ++p) {
		auto list = [&](const std::vector<CardView>& v) {
			std::string s = "[";
			for(size_t i = 0; i < v.size(); ++i)
				s += (i ? "," : "") + card_json(v[i].code);
			return s + "]";
		};
		auto zones = [&](const std::vector<CardView>& v, int slots) {
			std::vector<const CardView*> at(slots, nullptr);
			for(auto& c : v)
				if(c.code && c.seq < static_cast<uint32_t>(slots))
					at[c.seq] = &c;
			std::string s = "[";
			for(int i = 0; i < slots; ++i) {
				if(i)
					s += ",";
				const CardView* c = at[i];
				if(!c) {
					s += "null";
					continue;
				}
				s += "{\"code\":" + std::to_string(db_.official_id(c->code)) + ",\"name\":" + quote(db_.name(c->code)) +
				     ",\"pos\":\"" + pos_text(c->pos) + "\",\"atk\":" + std::to_string(c->atk) +
				     ",\"def\":" + std::to_string(c->def) + ",\"materials\":" + std::to_string(c->overlay.size()) +
				     ",\"monster\":" + ((c->type & TYPE_MONSTER) ? "true" : "false") + "}";
			}
			return s + "]";
		};
		o << (p ? "," : "") << "{\"lp\":" << b.lp[p] << ",\"hand\":" << list(b.hand[p])
		  << ",\"deck\":" << b.deck[p].size() << ",\"extra\":" << list(b.extra[p]) << ",\"grave\":" << list(b.grave[p])
		  << ",\"banished\":" << list(b.banished[p]) << ",\"monsters\":" << zones(b.mzone[p], 7)
		  << ",\"spells\":" << zones(b.szone[p], 8) << "}";
	}
	o << "],\"decision\":";
	const Decision& d = duel_->decision();
	if(duel_->ended() || d.type == DecisionType::None) {
		o << "null";
	} else {
		std::string prompt;
		std::string hint = d.hint > 0xfffff ? db_.description(d.hint) : std::string();
		switch(d.type) {
		case DecisionType::Idle: prompt = std::string(phase_text(duel_->phase())) + ": choose an action"; break;
		case DecisionType::Battle: prompt = "Battle Phase: attack or move on"; break;
		case DecisionType::Chain: prompt = "Respond with a card or effect?"; break;
		case DecisionType::EffectYesNo: prompt = "Use this effect?"; break;
		case DecisionType::YesNo: prompt = "Choose"; break;
		case DecisionType::Option: prompt = "Choose an effect"; break;
		case DecisionType::Position: prompt = "Choose a battle position"; break;
		case DecisionType::Tribute: prompt = "Choose monsters to Tribute"; break;
		case DecisionType::Sum: prompt = hint.empty() ? "Choose materials" : hint; break;
		case DecisionType::AnnounceCard: prompt = "Declare a card name"; break;
		case DecisionType::AnnounceRace: prompt = "Declare a monster Type"; break;
		case DecisionType::AnnounceAttrib: prompt = "Declare an Attribute"; break;
		case DecisionType::AnnounceNumber: prompt = "Declare a number"; break;
		default: prompt = hint.empty() ? "Select card(s)" : hint; break;
		}
		o << "{\"player\":" << int(d.player) << ",\"type\":" << quote(decision_name(d.type)) << ",\"prompt\":"
		  << quote(prompt) << ",\"options\":[";
		for(size_t i = 0; i < d.options.size(); ++i) {
			const Option& opt = d.options[i];
			o << (i ? "," : "") << "{\"label\":" << quote(opt.label) << ",\"code\":" << db_.official_id(opt.code)
			  << ",\"kind\":\"" << kind_text(opt.kind) << "\"}";
		}
		o << "]}";
	}
	o << ",\"log\":[";
	for(size_t i = 0; i < log_.size(); ++i)
		o << (i ? "," : "") << quote(log_[i]);
	o << "],\"log_players\":[";
	for(size_t i = 0; i < log_.size(); ++i)
		o << (i ? "," : "") << int(i < log_players_.size() ? log_players_[i] : -1);
	o << "],\"moves\":[";
	for(size_t i = 0; i < moves_.size(); ++i) {
		const Duel::Move& mv = moves_[i];
		o << (i ? "," : "") << "{\"code\":" << db_.official_id(mv.code) << ",\"from\":[" << int(mv.from_con) << ","
		  << int(mv.from_loc) << "," << mv.from_seq << "," << mv.from_pos << "],\"to\":[" << int(mv.to_con) << ","
		  << int(mv.to_loc) << "," << mv.to_seq << "," << mv.to_pos << "]}";
	}
	o << "],\"activations\":[";
	for(size_t i = 0; i < activations_.size(); ++i) {
		const Duel::Activation& a = activations_[i];
		o << (i ? "," : "") << "{\"code\":" << db_.official_id(a.code) << ",\"place\":[" << int(a.con) << "," << int(a.loc)
		  << "," << a.seq << "," << a.pos << "],\"after\":" << a.after << "}";
	}
	auto place = [](uint8_t con, uint8_t loc, uint32_t seq, uint32_t pos) {
		return "[" + std::to_string(con) + "," + std::to_string(loc) + "," + std::to_string(seq) + "," + std::to_string(pos) + "]";
	};
	auto pick_json = [&](const Duel::Pick& k) {
		return "{\"code\":" + std::to_string(db_.official_id(k.code)) + ",\"name\":" + quote(k.code ? db_.name(k.code) : "") +
		       ",\"place\":" + place(k.con, k.loc, k.seq, k.pos) + ",\"target\":" + (k.target ? "true" : "false") +
		       ",\"link\":" + std::to_string(k.link) + ",\"source\":" + std::to_string(db_.official_id(k.source)) +
		       ",\"after\":" + std::to_string(k.after) + "}";
	};
	o << "],\"picks\":[";
	for(size_t i = 0; i < picks_.size(); ++i)
		o << (i ? "," : "") << pick_json(picks_[i]);
	// The chain as it stands (being built, or resolving): first link first, with what each targets.
	o << "],\"chain\":[";
	const auto& chain = duel_->chain();
	for(size_t i = 0; i < chain.size(); ++i) {
		const Duel::ChainLink& c = chain[i];
		std::string desc = c.desc > 0xfffff ? db_.description(c.desc) : std::string();
		o << (i ? "," : "") << "{\"code\":" << db_.official_id(c.code) << ",\"name\":" << quote(db_.name(c.code))
		  << ",\"player\":" << int(c.con) << ",\"place\":" << place(c.con, c.loc, c.seq, c.pos) << ",\"effect\":" << quote(desc)
		  << ",\"negated\":" << (c.negated ? "true" : "false") << ",\"state\":\""
		  << (c.resolved ? "resolved" : c.resolving ? "resolving" : "waiting") << "\",\"picks\":[";
		for(size_t j = 0; j < c.picks.size(); ++j)
			o << (j ? "," : "") << pick_json(c.picks[j]);
		o << "]}";
	}
	o << "],\"step\":" << history_.size();
	if(!setup_sent_) {
		o << ",\"setup\":" << setup_json();
		setup_sent_ = true;
	}
	o << ",\"responses\":[";
	for(size_t i = history_sent_; i < history_.size(); ++i)
		o << (i > history_sent_ ? "," : "") << "\"" << hex(history_[i]) << "\"";
	history_sent_ = history_.size();
	log_.clear();
	log_players_.clear();
	moves_.clear();
	activations_.clear();
	picks_.clear();
	o << "]}";
	return o.str();
}

std::string hex(const std::vector<uint8_t>& bytes) {
	static const char* digits = "0123456789abcdef";
	std::string s;
	for(uint8_t b : bytes) {
		s += digits[b >> 4];
		s += digits[b & 15];
	}
	return s;
}

bool unhex(const std::string& s, std::vector<uint8_t>& out) {
	auto val = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
	if(s.size() % 2)
		return false;
	out.clear();
	for(size_t i = 0; i < s.size(); i += 2) {
		int h = val(s[i]), l = val(s[i + 1]);
		if(h < 0 || l < 0)
			return false;
		out.push_back(static_cast<uint8_t>(h << 4 | l));
	}
	return true;
}

std::string LiveDuel::setup_json() const {
	std::ostringstream o;
	o.imbue(std::locale::classic());
	auto list = [&](const std::vector<uint32_t>& v) {
		std::string s = "[";
		for(size_t i = 0; i < v.size(); ++i)
			s += (i ? "," : "") + std::to_string(v[i]);
		return s + "]";
	};
	o << "{\"seed\":[";
	for(int i = 0; i < 4; ++i)
		o << (i ? "," : "") << "\"" << setup_.seed[i] << "\"";  // strings: 64-bit numbers don't fit a JSON double
	o << "],\"flags\":\"" << setup_.flags << "\",\"no_shuffle\":" << (setup_.no_shuffle ? "true" : "false") << ",\"players\":[";
	for(int p = 0; p < 2; ++p) {
		const PlayerSetup& ps = setup_.players[p];
		o << (p ? "," : "") << "{\"lp\":" << ps.lp << ",\"main\":" << list(ps.main) << ",\"extra\":" << list(ps.extra)
		  << ",\"hand\":" << list(ps.hand) << ",\"placed\":[";
		for(size_t i = 0; i < ps.placed.size(); ++i) {
			const PlacedCard& c = ps.placed[i];
			o << (i ? "," : "") << "[" << c.code << "," << c.loc << "," << c.seq << "," << c.pos << "]";
		}
		o << "]}";
	}
	o << "]}";
	return o.str();
}

bool setup_from_json(const json::Value& v, DuelSetup& out, std::string* error) {
	auto fail = [&](const char* why) {
		if(error)
			*error = why;
		return false;
	};
	if(!v.is_object() || !v["players"].is_array() || v["players"].size() != 2 || v["seed"].size() != 4)
		return fail("not a duel setup");
	out = DuelSetup{};
	for(int i = 0; i < 4; ++i)
		out.seed[i] = std::stoull(v["seed"][i].str().empty() ? "0" : v["seed"][i].str());
	out.flags = v["flags"].str().empty() ? 0 : std::stoull(v["flags"].str());
	out.no_shuffle = v["no_shuffle"].boolean();
	for(int p = 0; p < 2; ++p) {
		const json::Value& j = v["players"][p];
		PlayerSetup& ps = out.players[p];
		ps.lp = static_cast<uint32_t>(j["lp"].integer(8000));
		auto list = [&](const json::Value& a, std::vector<uint32_t>& to) {
			for(const auto& x : a.items())
				to.push_back(static_cast<uint32_t>(x.integer()));
		};
		list(j["main"], ps.main);
		list(j["extra"], ps.extra);
		list(j["hand"], ps.hand);
		for(const auto& c : j["placed"].items())
			ps.placed.push_back({static_cast<uint32_t>(c[0].integer()), static_cast<uint32_t>(c[1].integer()),
			                     static_cast<uint32_t>(c[2].integer()), static_cast<uint32_t>(c[3].integer())});
	}
	return true;
}

}  // namespace ygoe
