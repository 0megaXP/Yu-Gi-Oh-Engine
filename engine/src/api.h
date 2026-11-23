// JSON-in / JSON-out entry points. This is the surface the Android (JNI) layer uses;
// see docs/api.md for the request and response formats.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "carddb.h"
#include "deck.h"
#include "duel.h"

namespace ygoe {

struct Progress {
	std::string phase;   // "idle", "search", "verify 1", "simulate", ...
	int done = 0;
	int total = 0;
};

class Engine {
public:
	// data_dir: cards.tsv & co.  scripts_dir: CardScripts checkout or folder with scripts.pack.
	Engine(const std::string& data_dir, const std::string& scripts_dir);
	// Stops any running job and waits for it before freeing the engine.
	~Engine();
	bool ok() const { return ok_; }
	const std::string& error() const { return error_; }

	// Each call blocks until done (or cancelled) and returns a JSON document.
	// On failure the document is {"error": "..."}.
	std::string analyze(const std::string& request);
	std::string profile(const std::string& request);
	std::string side(const std::string& request);
	// {"deck": deck, "hands": 20, "iterations": 1500, "seed": n} -> reference boards (docs/api.md)
	std::string reference(const std::string& request);
	// {"deck": deck, "hand": [5 ids], "opponent": deck|null, "iterations", "branch_iterations", "seed"}
	std::string combo_tree(const std::string& request);
	// {"deck": deck, "lines": [{"id", "hand": [ids], "steps": [{"action", "code", "detail"}], "end_board": [ids]}],
	//  "beam", "tries", "seconds", "seed"} -> each line replayed and checked by the rules, plus the
	// combo pieces of the verified ones (docs/api.md, Video lines)
	std::string video_lines(const std::string& request);
	// {"decks":[{main,extra},{main,extra}], "winner":0|1, "finished":bool, "game":id, "boards":[{"turn",
	//  "turn_player", "players":[{lp, hand:[ids], grave, banished, deck:n, extra:n, mzone:[{c,p,s}], szone}]}]}
	// -> {"count", "jsonl"}: value-network training positions (tools/ml), both sides of each board.
	std::string replay_positions(const std::string& request);
	// {"messages": [base64 message], "decks": [{main, extra}|null x2], "game", "per_turn"} -> a YGO Omega
	// replay followed by the rules core (follow.h): how far it got, and value-network positions.
	std::string follow_replay(const std::string& request);
	std::string card_names(const std::string& request);  // {"codes":[...]} -> names, for debugging

	// Live duels: {"decks":[deck, deck], "seed": n}; player 0 goes first. Returns nullptr and
	// sets `error` on failure. The duel uses this engine's data, so free it before the engine.
	class LiveDuel* create_live(const std::string& request, std::string& error);

	// Safe to call from any thread while one of the calls above runs.
	void cancel() { cancel_ = true; }
	Progress progress() const;

private:
	std::string follow_replay_impl(const struct FollowInput& in);
	void set_progress(const std::string& phase, int done, int total);
	std::string missing_json(const std::vector<const Deck*>& decks);

	CardDB db_;
	std::unique_ptr<ScriptCache> scripts_;
	bool ok_ = false;
	std::string error_;
	std::atomic<bool> cancel_{false};
	std::mutex run_mu_;  // one job at a time
	mutable std::mutex progress_mu_;
	Progress progress_;
};

}  // namespace ygoe
