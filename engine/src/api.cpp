#include <cctype>
#include "api.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

#include "analyze.h"
#include "json.h"
#include "live.h"
#include "pieces.h"
#include "reference.h"
#include "replay.h"
#include "combo.h"
#include "ocgapi_constants.h"
#include "profile.h"
#include "side.h"

namespace ygoe {

namespace {

using json::quote;
using json::Value;

std::string error_json(const std::string& msg) { return "{\"error\":" + quote(msg) + "}"; }

std::string num(double v, int prec = 3) {
	char b[32];
	std::snprintf(b, sizeof b, "%.*f", prec, v);
	return b;
}

std::vector<uint32_t> codes(const Value& v) {
	std::vector<uint32_t> out;
	for(const auto& x : v.items())
		out.push_back(static_cast<uint32_t>(x.integer()));
	return out;
}

bool read_deck(const Value& v, Deck& d, std::string& err, const CardDB& db) {
	if(!v.is_object()) {
		err = "deck must be an object with main/extra/side arrays";
		return false;
	}
	d.name = v["name"].str();
	d.main = codes(v["main"]);
	d.extra = codes(v["extra"]);
	d.side = codes(v["side"]);
	// Optional {"passcode": "name"}: lets the engine correct passcodes it maps to the wrong card.
	if(v["names"].is_object())
		for(const auto& [k, n] : v["names"].entries())
			if(!k.empty() && std::isdigit(static_cast<unsigned char>(k[0])))
				db.learn_name(static_cast<uint32_t>(std::stoul(k)), n.str());
	if(d.main.empty()) {
		err = "deck '" + d.name + "' has an empty main deck";
		return false;
	}
	return true;
}

uint32_t zone_of(const std::string& z) {
	if(z == "mzone" || z == "monster")
		return LOCATION_MZONE;
	if(z == "szone" || z == "spelltrap")
		return LOCATION_SZONE;
	if(z == "grave" || z == "gy")
		return LOCATION_GRAVE;
	if(z == "banished")
		return LOCATION_REMOVED;
	return 0;
}

uint32_t pos_of(const std::string& p) {
	if(p == "atk")
		return POS_FACEUP_ATTACK;
	if(p == "def")
		return POS_FACEUP_DEFENSE;
	if(p == "set")
		return POS_FACEDOWN_DEFENSE;
	if(p == "up")
		return POS_FACEUP;
	return 0;
}

std::string card_json(const CardDB& db, uint32_t code) {
	uint32_t sid = db.script_id(code);
	return "{\"code\":" + std::to_string(db.official_id(sid)) + ",\"name\":" + quote(db.name(sid)) + "}";
}

const char* where_name(uint32_t range) {
	if(range & LOCATION_HAND)
		return "hand";
	if(range & LOCATION_GRAVE)
		return "gy";
	if(range & LOCATION_SZONE)
		return "spell_trap_zone";
	return "field";
}

std::string profile_json(const CardDB& db, const Deck& d) {
	auto p = profile_deck(db, d);
	auto sets = dominant_setcodes(db, d);
	std::ostringstream o;
	o << "{\"name\":" << quote(d.name) << ",\"monster_effects\":" << num(p.monster_effects)
	  << ",\"hand_effects\":" << num(p.hand_effects) << ",\"gy_effects\":" << num(p.gy_effects)
	  << ",\"spell_trap\":" << num(p.spell_trap) << ",\"searches\":" << num(p.searches)
	  << ",\"special_summons\":" << num(p.special_summons) << ",\"engine_cards\":" << p.engine_cards
	  << ",\"summary\":" << quote(p.summary()) << ",\"key_cards\":[";
	for(size_t i = 0; i < p.key_cards.size(); ++i)
		o << (i ? "," : "") << card_json(db, p.key_cards[i].first);
	o << "],\"interrupts\":[";
	std::map<uint32_t, int> counts;
	for(auto c : d.main)
		++counts[c];
	bool first = true;
	for(auto& [code, n] : counts) {
		auto r = classify(db, code, sets);
		const Interrupt* best = nullptr;
		for(auto& in : r.interrupts)
			if(in.kind != Interrupt::Extender && (!best || in.base > best->base))
				best = &in;
		if(!best)
			continue;
		o << (first ? "" : ",") << "{\"card\":" << card_json(db, code) << ",\"copies\":" << n << ",\"what\":"
		  << quote(best->name()) << ",\"from\":" << quote(where_name(best->range)) << ",\"base_value\":"
		  << num(best->base, 2) << ",\"engine\":" << (r.engine ? "true" : "false") << "}";
		first = false;
	}
	o << "]}";
	return o.str();
}

}  // namespace

std::string Engine::missing_json(const std::vector<const Deck*>& decks) {
	// Cards the rules engine can't play (no card data or no script yet): they are
	// still in the deck but do nothing, so the app should warn about them.
	std::vector<uint32_t> missing;
	for(const Deck* d : decks)
		for(auto* list : {&d->main, &d->extra, &d->side})
			for(auto code : *list) {
				uint32_t sid = db_.script_id(code);
				auto c = db_.get(sid);
				bool ok = c && (scripts_->has_card_script(sid) || (c->alias && scripts_->has_card_script(c->alias)) ||
				                (c->type & TYPE_NORMAL) || (c->type & TYPE_TOKEN));
				if(!ok && std::find(missing.begin(), missing.end(), code) == missing.end())
					missing.push_back(code);
			}
	std::string o = "[";
	for(size_t i = 0; i < missing.size(); ++i)
		o += (i ? "," : "") + std::to_string(missing[i]);
	return o + "]";
}

Engine::Engine(const std::string& data_dir, const std::string& scripts_dir) {
	ok_ = db_.load(data_dir, &error_);
	scripts_ = std::make_unique<ScriptCache>(scripts_dir);
	if(ok_ && !scripts_->get("utility.lua")) {
		ok_ = false;
		error_ = "no card scripts found in " + scripts_dir;
	}
}

Engine::~Engine() {
	cancel_ = true;
	std::lock_guard<std::mutex> wait(run_mu_);
}

void Engine::set_progress(const std::string& phase, int done, int total) {
	std::lock_guard<std::mutex> lock(progress_mu_);
	progress_.phase = phase;
	progress_.done = done;
	progress_.total = total;
}

Progress Engine::progress() const {
	std::lock_guard<std::mutex> lock(progress_mu_);
	return progress_;
}

std::string Engine::analyze(const std::string& request) {
	if(!ok_)
		return error_json(error_);
	std::lock_guard<std::mutex> run(run_mu_);
	cancel_ = false;
	std::string err;
	Value req = Value::parse(request, &err);
	if(!req.is_object())
		return error_json(err.empty() ? "request must be a JSON object" : err);

	Scenario sc;
	for(int p = 0; p < 2; ++p)
		if(!read_deck(req["decks"][p], sc.decks[p], err, db_))
			return error_json(err);
	sc.me = static_cast<uint8_t>(req["me"].integer(0));
	sc.knowledge = req["mode"].str() == "real" ? Knowledge::Real : Knowledge::Practice;
	sc.seed = static_cast<uint64_t>(req["seed"].integer(1));
	for(int p = 0; p < 2; ++p) {
		const Value& hand = req["hands"][p];
		if(hand.is_array()) {
			sc.hand[p] = codes(hand);
		} else {
			sc.hand_known[p] = false;
			sc.hand_size[p] = static_cast<size_t>(req["hand_sizes"][p].integer(5));
		}
		sc.lp[p] = static_cast<uint32_t>(req["lp"][p].integer(8000));
		for(const auto& c : req["field"][p].items()) {
			PlacedCard pc;
			pc.code = static_cast<uint32_t>(c["code"].integer());
			pc.loc = zone_of(c["zone"].str());
			pc.seq = static_cast<uint32_t>(c["seq"].integer(0));
			pc.pos = pos_of(c["pos"].str());
			if(!pc.code || !pc.loc)
				return error_json("bad field entry for player " + std::to_string(p));
			sc.placed[p].push_back(pc);
		}
		for(auto code : codes(req["grave"][p]))
			sc.placed[p].push_back({code, LOCATION_GRAVE, 0, POS_FACEUP});
		for(auto code : codes(req["banished"][p]))
			sc.placed[p].push_back({code, LOCATION_REMOVED, 0, POS_FACEUP});
	}
	if(sc.me > 1)
		return error_json("me must be 0 or 1");
	if(sc.knowledge == Knowledge::Real && !sc.hand_known[sc.me])
		return error_json("real mode needs your own hand");

	const Value& c = req["config"];
	AnalysisConfig cfg;
	cfg.search.iterations = static_cast<int>(c["iterations"].integer(2000));
	cfg.search.seconds = c["seconds"].num(45);
	cfg.search.threads = static_cast<int>(c["threads"].integer(0));
	cfg.search.horizon_turns = static_cast<int>(c["horizon"].integer(0));
	cfg.search.cancel = &cancel_;
	if(req["library"].is_array())
		cfg.search.guide = std::make_shared<PieceGuide>(parse_library(db_, req["library"]));
	cfg.determinizations = static_cast<int>(c["samples"].integer(12));
	cfg.lines = static_cast<int>(c["lines"].integer(3));
	cfg.verify = c["verify"].boolean(true);
	cfg.verify_iterations = static_cast<int>(c["verify_iterations"].integer(300));
	cfg.on_progress = [this](const std::string& phase, int done, int total) { set_progress(phase, done, total); };

	set_progress("search", 0, cfg.search.iterations);
	auto res = ygoe::analyze(db_, *scripts_, sc, cfg);
	set_progress("idle", 0, 0);
	std::string out = res.to_json();
	out.pop_back();  // reopen the object to add the flag
	out += std::string(",\"cancelled\":") + (cancel_ ? "true" : "false") +
	       ",\"missing_cards\":" + missing_json({&sc.decks[0], &sc.decks[1]}) + "}";
	return out;
}

std::string Engine::profile(const std::string& request) {
	if(!ok_)
		return error_json(error_);
	std::string err;
	Value req = Value::parse(request, &err);
	Deck d;
	if(!read_deck(req["deck"], d, err, db_))
		return error_json(err);
	return profile_json(db_, d);
}

std::string Engine::side(const std::string& request) {
	if(!ok_)
		return error_json(error_);
	std::lock_guard<std::mutex> run(run_mu_);
	cancel_ = false;
	std::string err;
	Value req = Value::parse(request, &err);
	Deck mine, theirs;
	if(!read_deck(req["mine"], mine, err, db_) || !read_deck(req["theirs"], theirs, err, db_))
		return error_json(err);
	auto opp = profile_deck(db_, theirs);
	BoardShape shape;
	int sim = static_cast<int>(req["simulate"].integer(0));
	if(sim > 0) {
		set_progress("simulate", 0, sim);
		shape = simulate_board_shape(db_, *scripts_, theirs, mine, sim, static_cast<int>(req["iterations"].integer(400)),
		                             &cancel_, [&](int s, const BoardShape&, int, int, int) { set_progress("simulate", s, sim); });
		set_progress("idle", 0, 0);
	}
	std::ostringstream o;
	o << "{\"opponent\":" << profile_json(db_, theirs) << ",\"board_shape\":{\"samples\":" << shape.samples
	  << ",\"monsters\":" << num(shape.monsters, 2) << ",\"st_faceup\":" << num(shape.st_faceup, 2)
	  << ",\"st_set\":" << num(shape.st_set, 2) << ",\"hand_traps\":" << num(shape.hand_traps, 2) << "}";
	for(bool first : {true, false}) {
		auto plan = plan_side(db_, mine, theirs, opp, shape, first);
		std::map<uint32_t, int> ins, outs;
		for(auto& s : plan.swaps) {
			++ins[s.in];
			++outs[s.out];
		}
		auto list = [&](const std::map<uint32_t, int>& m, const std::vector<CardValue>& values) {
			std::string r = "[";
			bool f = true;
			for(auto& [code, n] : m) {
				const CardValue* v = nullptr;
				for(auto& x : values)
					if(x.code == code)
						v = &x;
				r += std::string(f ? "" : ",") + "{\"card\":" + card_json(db_, code) + ",\"count\":" + std::to_string(n) +
				     ",\"value\":" + num(v ? v->value : 0, 2) + ",\"why\":" + quote(v ? v->why : "") + "}";
				f = false;
			}
			return r + "]";
		};
		o << ",\"" << (first ? "going_first" : "going_second") << "\":{\"side_in\":" << list(ins, plan.side_values)
		  << ",\"side_out\":" << list(outs, plan.flex_values) << "}";
	}
	o << ",\"cancelled\":" << (cancel_ ? "true" : "false") << ",\"missing_cards\":" << missing_json({&mine, &theirs})
	  << "}";
	return o.str();
}

LiveDuel* Engine::create_live(const std::string& request, std::string& error) {
	if(!ok_) {
		error = error_;
		return nullptr;
	}
	Value req = Value::parse(request, &error);
	Deck d[2];
	for(int p = 0; p < 2; ++p)
		if(!read_deck(req["decks"][p], d[p], error, db_))
			return nullptr;
	auto* live = new LiveDuel(db_, *scripts_, d[0], d[1], static_cast<uint64_t>(req["seed"].integer(1)));
	if(!live->ok()) {
		error = live->error();
		delete live;
		return nullptr;
	}
	live->set_human(static_cast<int>(req["human"].integer(-1)));
	for(int p = 0; p < 2; ++p)
		if(req["libraries"][p].is_array())
			live->set_guide(p, std::make_shared<PieceGuide>(parse_library(db_, req["libraries"][p])));
	return live;
}

std::string Engine::reference(const std::string& request) {
	if(!ok_)
		return error_json(error_);
	std::lock_guard<std::mutex> run(run_mu_);
	cancel_ = false;
	std::string err;
	Value req = Value::parse(request, &err);
	Deck d;
	if(!read_deck(req["deck"], d, err, db_))
		return error_json(err);
	int hands = static_cast<int>(req["hands"].integer(20));
	int iterations = static_cast<int>(req["iterations"].integer(1500));
	set_progress("reference", 0, hands);
	auto res = build_reference(db_, *scripts_, d, hands, iterations, static_cast<uint64_t>(req["seed"].integer(1)), &cancel_,
	                           [&](int done, int total) { set_progress("reference", done, total); });
	set_progress("idle", 0, 0);
	std::string out = res.to_json(db_);
	out.pop_back();
	out += std::string(",\"cancelled\":") + (cancel_ ? "true" : "false") + ",\"missing_cards\":" + missing_json({&d}) + "}";
	return out;
}

std::string Engine::video_lines(const std::string& request) {
	if(!ok_)
		return error_json(error_);
	std::lock_guard<std::mutex> run(run_mu_);
	cancel_ = false;
	std::string err;
	Value req = Value::parse(request, &err);
	Deck d;
	if(!read_deck(req["deck"], d, err, db_))
		return error_json(err);
	std::vector<VideoLine> lines;
	for(const auto& l : req["lines"].items()) {
		VideoLine v;
		v.id = l["id"].str();
		for(const auto& c : l["hand"].items())
			v.hand.push_back(static_cast<uint32_t>(c.integer()));
		for(const auto& s : l["steps"].items())
			v.steps.push_back({s["action"].str(), static_cast<uint32_t>(s["code"].integer(0)), s["detail"].str()});
		for(const auto& c : l["end_board"].items())
			v.end_board.push_back(static_cast<uint32_t>(c.integer()));
		lines.push_back(std::move(v));
	}
	if(lines.empty())
		return error_json("no lines to check");
	ReplayConfig cfg;
	cfg.beam = static_cast<int>(req["beam"].integer(cfg.beam));
	cfg.tries = static_cast<int>(req["tries"].integer(cfg.tries));
	cfg.seconds = req["seconds"].num(cfg.seconds);
	cfg.seed = static_cast<uint64_t>(req["seed"].integer(1));
	cfg.cancel = &cancel_;
	set_progress("video lines", 0, static_cast<int>(lines.size()));
	auto res = check_video_lines(db_, *scripts_, d, lines, cfg,
	                             [&](int done, int total) { set_progress("video lines", done, total); });
	set_progress("idle", 0, 0);
	std::string out = res.to_json(db_);
	out.pop_back();
	out += std::string(",\"cancelled\":") + (cancel_ ? "true" : "false") + "}";
	return out;
}

std::string Engine::combo_tree(const std::string& request) {
	if(!ok_)
		return error_json(error_);
	std::lock_guard<std::mutex> run(run_mu_);
	cancel_ = false;
	std::string err;
	Value req = Value::parse(request, &err);
	Deck mine, theirs;
	if(!read_deck(req["deck"], mine, err, db_))
		return error_json(err);
	bool known = req["opponent"].is_object();
	if(known && !read_deck(req["opponent"], theirs, err, db_))
		return error_json(err);
	std::vector<uint32_t> hand = codes(req["hand"]);
	if(hand.empty())
		return error_json("the combo tree needs your hand");
	ComboTreeConfig cfg;
	if(req["library"].is_array())
		cfg.guide = std::make_shared<PieceGuide>(parse_library(db_, req["library"]));
	cfg.iterations = static_cast<int>(req["iterations"].integer(1500));
	cfg.branch_iterations = static_cast<int>(req["branch_iterations"].integer(300));
	cfg.seed = static_cast<uint64_t>(req["seed"].integer(1));
	cfg.cancel = &cancel_;
	cfg.progress = [this](const std::string& phase, int done, int total) { set_progress("tree: " + phase, done, total); };
	auto tree = build_combo_tree(db_, *scripts_, mine, hand, known ? &theirs : nullptr, cfg);
	set_progress("idle", 0, 0);
	std::string out = tree.to_json(db_);
	out.pop_back();
	std::vector<const Deck*> decks{&mine};
	if(known)
		decks.push_back(&theirs);
	out += ",\"missing_cards\":" + missing_json(decks) + "}";
	return out;
}

std::string Engine::card_names(const std::string& request) {
	Value req = Value::parse(request);
	std::string o = "[";
	bool first = true;
	for(auto code : codes(req["codes"])) {
		o += std::string(first ? "" : ",") + card_json(db_, code);
		first = false;
	}
	return o + "]";
}

}  // namespace ygoe
