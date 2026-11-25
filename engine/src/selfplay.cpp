#include "selfplay.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <algorithm>

#include "carddb.h"
#include "duel.h"
#include "evaluate.h"
#include "ocgapi_constants.h"
#include "profile.h"
#include "side.h"

namespace ygoe {

const Deck& Siding::sided(size_t mine, size_t theirs, bool first, const std::vector<Deck>& decks) {
	const auto key = std::make_tuple(mine, theirs, first);
	auto it = cache_.find(key);
	if(it != cache_.end())
		return it->second;
	Deck d = decks[mine];
	if(!d.side.empty()) {
		auto plan = plan_side(db_, decks[mine], decks[theirs], profile_deck(db_, decks[theirs]), BoardShape{}, first);
		for(const auto& s : plan.swaps) {
			auto o = std::find(d.main.begin(), d.main.end(), s.out);
			auto i = std::find(d.side.begin(), d.side.end(), s.in);
			if(o == d.main.end() || i == d.side.end())
				continue;
			*o = s.in;
			*i = s.out;
		}
	}
	return cache_.emplace(key, std::move(d)).first->second;
}

Game play_game(const CardDB& db, ScriptCache& scripts, const Deck& d0, const Deck& d1, uint64_t seed, const PlayConfig& pc,
               const Bot& b0, const Bot& b1, const std::function<void(LiveDuel&)>& on_position, const LiveDuel::SearchHook& hook,
               LiveDuel** expose) {
	Game g;
	std::unique_ptr<LiveDuel> lp;
	if(pc.start)
		lp = std::make_unique<LiveDuel>(db, scripts, *pc.start, DUEL_MODE_MR5 | DUEL_ATTACK_FIRST_TURN);
	else
		lp = std::make_unique<LiveDuel>(db, scripts, d0, d1, seed);
	LiveDuel& live = *lp;
	if(!live.ok())
		return g;
	if(expose)
		*expose = &live;
	live.set_iterations(pc.iterations);
	if(pc.threads > 0)
		live.set_threads(pc.threads);
	live.set_value_net(0, b0.net);
	live.set_value_net(1, b1.net);
	live.set_policy(0, b0.policy);
	live.set_policy(1, b1.policy);
	live.set_guide(0, b0.guide);
	live.set_guide(1, b1.guide);
	live.set_nonengine_timing(0, b0.timing);
	live.set_nonengine_timing(1, b1.timing);
	if(pc.ne_hook)
		live.set_nonengine_exploration(pc.ne_explore, pc.ne_hook);
	if(pc.explore > 0)
		live.set_exploration(pc.explore, pc.explore_moves);
	if(hook)
		live.set_search_hook(hook);
	const int last_turn = std::max(pc.max_turns - pc.turn_offset, 4);
	int guard = 0;
	const auto t0 = std::chrono::steady_clock::now();
	std::vector<std::string> recent;  // the last moves, kept for a game that runs out of time
	while(!live.duel().ended() && static_cast<int>(live.duel().turn()) <= last_turn && guard++ < 4000) {
		if(pc.stop && pc.stop->load()) {
			g.stopped = true;
			break;
		}
		if(pc.max_seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > pc.max_seconds) {
			g.timed_out = true;
			break;
		}
		if(pc.tick)
			pc.tick();
		const Decision& d = live.duel().decision();
		if(d.type == DecisionType::Idle && on_position)
			on_position(live);
		const uint8_t who = d.player;
		const char* type = decision_name(d.type);
		const uint32_t turn = live.duel().turn();
		const std::string label = live.bot_move(BotLevel::Normal);
		if(label.empty())
			break;
		++g.moves;
		recent.push_back("turn " + std::to_string(turn) + " P" + std::to_string(who) + " " + type + ": " + label);
		if(recent.size() > 80)
			recent.erase(recent.begin());
	}
	if(g.timed_out || guard >= 4000)
		g.last_moves = std::move(recent);
	const Board b = live.duel().board();
	g.lp[0] = b.lp[0];
	g.lp[1] = b.lp[1];
	g.turns = static_cast<int>(live.duel().turn()) + pc.turn_offset;
	if(live.duel().ended()) {
		g.finished = true;
		g.winner = live.duel().winner();
		g.z0 = g.winner == 0 ? 1.0 : g.winner == 1 ? 0.0 : 0.5;
	} else {
		// cut: the current evaluator's (and network's) win chance for player 0 there
		Evaluator ev(db, pc.start ? pc.start->decks[0] : d0, pc.start ? pc.start->decks[1] : d1);
		ev.set_value_net(db.value_net());
		Board bb = b;
		bb.turn += static_cast<uint32_t>(pc.turn_offset);
		g.z0 = ev.evaluate(bb, 0, static_cast<uint8_t>(1 - b.turn_player)).score;
		if(g.z0 > 0.5)
			g.winner = 0;
		else if(g.z0 < 0.5)
			g.winner = 1;
	}
	if(expose)
		*expose = nullptr;
	return g;
}

int training_threads() {
	const int n = static_cast<int>(std::thread::hardware_concurrency());
	return std::clamp(n - 1, 1, 8);
}

}  // namespace ygoe
