#include "live.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "json.h"
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
bool trivial(const Decision& d) {
	if(d.options.size() == 1)
		return true;
	switch(d.type) {
	case DecisionType::Place:
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
	duel_ = std::make_unique<Duel>(db_, scripts_);
	duel_->enable_log(true);
	duel_->set_declare_pool(declare_pool(sc_));
	if(!duel_->start(setup_, &error_))
		return;
	log_ = duel_->take_log();
	eval_ = std::make_unique<Evaluator>(db_, d0, d1);
	searcher_ = std::make_unique<Searcher>(db_, scripts_, *eval_, declare_pool(sc_));
	ok_ = true;
	auto_resolve();
}

bool LiveDuel::respond_internal(size_t index) {
	const Decision& d = duel_->decision();
	if(duel_->ended() || index >= d.options.size())
		return false;
	Option opt = d.options[index];
	if(!duel_->respond(opt.response))
		return false;
	history_.push_back(opt.response);
	last_move_ = opt.label;
	// Keep the bot's plan while play follows it.
	if(plan_pos_ < plan_.size() && plan_[plan_pos_].label == opt.label)
		++plan_pos_;
	else
		plan_.clear(), plan_pos_ = 0;
	auto lines = duel_->take_log();
	log_.insert(log_.end(), lines.begin(), lines.end());
	return true;
}

void LiveDuel::auto_resolve() {
	int guard = 0;
	while(!duel_->ended() && trivial(duel_->decision()) && guard++ < 200) {
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

int LiveDuel::choose(BotLevel level) {
	const Decision& d = duel_->decision();
	if(d.options.empty())
		return -1;
	if(d.options.size() == 1)
		return 0;
	// Follow the plan while it still matches.
	if(level != BotLevel::Easy && plan_pos_ < plan_.size() && plan_[plan_pos_].player == d.player) {
		for(size_t i = 0; i < d.options.size(); ++i)
			if(d.options[i].label == plan_[plan_pos_].label)
				return static_cast<int>(i);
	}
	if(level == BotLevel::Easy) {
		std::vector<double> w(d.options.size());
		for(size_t i = 0; i < w.size(); ++i)
			w[i] = playout_weight(d, d.options[i], duel_->turn_player(), 0);
		std::discrete_distribution<size_t> pick(w.begin(), w.end());
		return static_cast<int>(pick(rng_));
	}
	// Plan the rest of this turn from the exact current state.
	SearchConfig cfg;
	cfg.iterations = level == BotLevel::Strong ? 1200 : 350;
	cfg.seconds = level == BotLevel::Strong ? 20 : 8;
	cfg.horizon_turns = 1;
	cfg.cancel = &cancel_;
	std::vector<RootState> roots{{setup_, history_}};
	auto out = searcher_->run(roots, d.player, cfg);
	if(out.moves.empty())
		return 0;
	const MoveStats& best = out.moves.front();
	int index = 0;
	for(size_t i = 0; i < d.options.size(); ++i)
		if(d.options[i].label == best.label)
			index = static_cast<int>(i);
	plan_.clear();
	plan_pos_ = 0;
	if(!best.best.empty()) {
		plan_ = best.best.front().steps;
	}
	return index;
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
	log_.clear();
	o << "]}";
	return o.str();
}

}  // namespace ygoe
