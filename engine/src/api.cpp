#include <locale>
#include <cctype>
#include "api.h"
#include <fstream>
#include "valuenet.h"
#include "learn.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

#include "analyze.h"
#include "evaluate.h"
#include "follow.h"
#include "json.h"
#include "live.h"
#include "pieces.h"
#include "reference.h"
#include "replay.h"
#include "policy.h"
#include "combo.h"
#include "ocgapi_constants.h"
#include "profile.h"
#include "side.h"

namespace ygoe {

namespace {
// [{"action": passcode, "choose": passcode}, ...] -> script ids
std::vector<SearchConfig::Imitation> read_imitate(const json::Value& v, const CardDB& db) {
	std::vector<SearchConfig::Imitation> out;
	for(const auto& e : v.items()) {
		SearchConfig::Imitation im;
		im.action = db.script_id(static_cast<uint32_t>(e["action"].integer()));
		im.choose = db.script_id(static_cast<uint32_t>(e["choose"].integer()));
		if(im.action && im.choose)
			out.push_back(im);
	}
	return out;
}
}  // namespace

namespace {

using json::quote;
using json::Value;

std::string error_json(const std::string& msg) { return "{\"error\":" + quote(msg) + "}"; }

// The policy a request passes (the app's own, learned on the phone), plus the one learned on
// GitHub (data/policy.json): the weights of both, added. Either alone when the other is missing.
std::shared_ptr<const Policy> read_policy(const Value& v, const CardDB& db) {
	std::shared_ptr<Policy> p;
	if(v.is_object()) {
		auto q = std::make_shared<Policy>(db);
		if(q->from_value(v) && q->trained())
			p = q;
	}
	if(auto base = db.policy()) {
		if(!p)
			p = std::make_shared<Policy>(db);
		p->add(*base, Policy::kSelfPlayScale);
	}
	return p;
}

// A deck's own learning (learn.h), from its folder: its value network (null when none) and its
// policy with the app's (`app`, as read_policy) instead of the format's.
struct Learned {
	std::shared_ptr<const ValueNet> net;
	bool has_net = false;
	std::shared_ptr<const Policy> policy;
	bool has_policy = false;
};

std::string file_text(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if(!f)
		return "";
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

Learned read_learned(const Value& dir, const Value& app, const CardDB& db) {
	Learned l;
	if(dir.str().empty())
		return l;
	auto net = std::make_shared<ValueNet>();
	if(net->load_file(dir.str() + "/value_net.json")) {
		l.net = net;
		l.has_net = true;
	}
	Policy q(db);
	if(q.from_json(file_text(dir.str() + "/policy.json")) && q.trained()) {
		std::shared_ptr<Policy> p;
		if(app.is_object()) {
			auto a = std::make_shared<Policy>(db);
			if(a->from_value(app) && a->trained())
				p = a;
		}
		if(!p)
			p = std::make_shared<Policy>(db);
		p->add(q, Policy::kSelfPlayScale);
		l.policy = p;
		l.has_policy = true;
	}
	return l;
}

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
				db.learn_name(static_cast<uint32_t>(std::stoull(k)), n.str());
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
	o.imbue(std::locale::classic());  // C number format, whatever locale the host app set
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

Engine::Engine(const std::string& data_dir, const std::string& scripts_dir) : data_dir_(data_dir) {
	ok_ = db_.load(data_dir, &error_);
	scripts_ = std::make_unique<ScriptCache>(scripts_dir);
	reach_ = std::make_unique<ReachTable>();
	if(!reach_->load(data_dir + "/reach.tsv"))
		reach_.reset();
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
	if(req["task"].str() == "learn")
		return learn(req);

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
		cfg.search.guide = std::make_shared<PieceGuide>(parse_library(db_, req["library"]), &db_);
	cfg.search.imitate = read_imitate(req["imitate"], db_);
	cfg.search.policy = read_policy(req["policy"], db_);
	// "learned": the analysed deck's folder (learn.h): its own network and policy
	Learned l = read_learned(req["learned"], req["policy"], db_);
	{
		if(l.has_net) {
			cfg.value_net = l.net;
			cfg.value_net_set = true;
		}
		if(l.has_policy)
			cfg.search.policy = l.policy;
	}
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
	out += std::string(",\"cancelled\":") + (cancel_ ? "true" : "false") + ",\"learned\":{\"net\":" + (l.has_net ? "true" : "false") +
	       ",\"policy\":" + (l.has_policy ? "true" : "false") + "},\"missing_cards\":" + missing_json({&sc.decks[0], &sc.decks[1]}) + "}";
	return out;
}

std::string Engine::learn(const Value& req) {
	LearnConfig cfg;
	std::string err;
	if(!read_deck(req["deck"], cfg.deck, err, db_))
		return error_json(err);
	cfg.name = req["name"].str();
	cfg.dir = req["dir"].str();
	if(cfg.dir.empty())
		return error_json("learn: no folder");
	if(req["opponents"].is_array() && req["opponents"].size() > 0) {
		for(const auto& o : req["opponents"].items()) {
			LearnOpponent lo;
			lo.name = o["name"].str();
			if(!read_deck(o, lo.deck, err, db_))
				return error_json(err);
			cfg.opponents.push_back(std::move(lo));
		}
	} else {
		// the format's decks the nightly runs play (format_decks.json in the data folder)
		Value fd = Value::parse(file_text(data_dir_ + "/format_decks.json"));
		for(const auto& o : fd["decks"].items()) {
			LearnOpponent lo;
			lo.name = o["name"].str();
			if(read_deck(o, lo.deck, err, db_))
				cfg.opponents.push_back(std::move(lo));
		}
	}
	if(req["status_only"].boolean(false))
		return learn_status(cfg.dir);
	cfg.minutes = req["minutes"].num(30);
	cfg.iterations = static_cast<int>(req["iterations"].integer(200));
	cfg.turns = static_cast<int>(req["turns"].integer(12));
	cfg.seed = static_cast<uint64_t>(req["seed"].integer(1));
	cfg.base_net = file_text(data_dir_ + "/value_net.json");
	cfg.base_policy = file_text(data_dir_ + "/policy.json");
	if(req["library"].is_array())
		cfg.library = parse_library(db_, req["library"]);
	cfg.cancel = &cancel_;
	cfg.progress = [this](const std::string& phase, int done, int total) { set_progress(phase, done, total); };
	set_progress("Starting", 0, 1);
	std::string out = learn_deck(db_, *scripts_, cfg);
	set_progress("idle", 0, 0);
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
	o.imbue(std::locale::classic());  // C number format, whatever locale the host app set
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

std::shared_ptr<const CardPriorities> Engine::bridge_priorities(const Deck& deck) {
	std::vector<uint32_t> all(deck.main);
	all.insert(all.end(), deck.extra.begin(), deck.extra.end());
	std::sort(all.begin(), all.end());
	std::string key;
	for(uint32_t c : all)
		key += std::to_string(c) + ",";
	{
		std::lock_guard<std::mutex> lock(bridges_mu_);
		auto it = bridges_.find(key);
		if(it != bridges_.end())
			return it->second;
	}
	std::shared_ptr<CardPriorities> pr;
	try {
		BridgeReport rep = find_bridges(db_, *scripts_, deck, reach_.get());
		pr = std::make_shared<CardPriorities>();
		for(const auto& b : rep.bridges) {
			const uint32_t sid = db_.script_id(b.code);
			if(std::find(pr->bridges.begin(), pr->bridges.end(), sid) == pr->bridges.end())
				pr->bridges.push_back(sid);
		}
	} catch(...) {
		pr.reset();
	}
	std::lock_guard<std::mutex> lock(bridges_mu_);
	bridges_[key] = pr;
	return pr;
}

std::string Engine::bridges(const std::string& request) {
	if(!ok_)
		return error_json(error_);
	Value req = Value::parse(request);
	Deck d;
	std::string error;
	if(!read_deck(req["deck"], d, error, db_))
		return error_json(error);
	std::lock_guard<std::mutex> run(run_mu_);
	return find_bridges(db_, *scripts_, d, reach_.get()).to_json(db_);
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
			live->set_guide(p, std::make_shared<PieceGuide>(parse_library(db_, req["libraries"][p]), &db_));
	live->set_policy(read_policy(req["policy"], db_));
	// "learned": [folder or null, ...]: a player's deck learned on this device (learn.h)
	for(int p = 0; p < 2; ++p) {
		Learned l = read_learned(req["learned"][p], req["policy"], db_);
		if(l.has_net)
			live->set_value_net(p, l.net);
		if(l.has_policy)
			live->set_policy(p, l.policy);
	}
	// {"knowledge": [{"chokes": [passcodes], "plays": 6.5}, null]}: per player, about their deck
	for(int p = 0; p < 2; ++p) {
		const Value& k = req["knowledge"][p];
		if(!k.is_object())
			continue;
		LiveDuel::DeckKnowledge dk;
		for(const auto& c : k["chokes"].items())
			dk.chokes.push_back(db_.script_id(static_cast<uint32_t>(c.integer())));
		dk.plays = k["plays"].num(0);
		live->set_knowledge(p, std::move(dk));
	}
	for(int p = 0; p < 2; ++p)
		live->set_imitate(p, read_imitate(req["imitate"][p], db_));
	// {"priorities": [{"starters": [...], "extenders": [...], "search_first": [...], "protect": [...]}, null]}:
	// per player, what deck guides say about playing their deck (passcodes); with the deck's
	// bridge cards (reach.h) unless "bridges": false
	const bool use_bridges = req["bridges"].boolean(true);
	for(int p = 0; p < 2; ++p) {
		const Value& k = req["priorities"][p];
		auto pr = std::make_shared<CardPriorities>();
		if(use_bridges)
			if(auto b = bridge_priorities(d[p]))
				pr->bridges = b->bridges;
		if(!k.is_object()) {
			if(!pr->empty())
				live->set_priorities(p, std::move(pr));
			continue;
		}
		auto read = [&](const char* key, std::vector<uint32_t>& out) {
			for(const auto& c : k[key].items())
				out.push_back(db_.script_id(static_cast<uint32_t>(c.integer())));
		};
		read("starters", pr->starters);
		read("extenders", pr->extenders);
		read("search_first", pr->search_first);
		read("protect", pr->protect);
		if(!pr->empty())
			live->set_priorities(p, std::move(pr));
	}
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
	// The deck's best lines also train the move policy (the one passed in, or a new one).
	Policy policy(db_);
	if(req["policy"].is_object())
		policy.from_value(req["policy"]);
	auto res = build_reference(db_, *scripts_, d, hands, iterations, static_cast<uint64_t>(req["seed"].integer(1)), &cancel_,
	                           [&](int done, int total) { set_progress("reference", done, total); },
	                           req["learn"].boolean(true) ? &policy : nullptr);
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
		for(const auto& s : l["steps"].items()) {
			v.steps.push_back({s["action"].str(), static_cast<uint32_t>(s["code"].integer(0)), s["detail"].str(),
			                   s["optional"].boolean(false), static_cast<uint8_t>(s["by"].integer(0) == 1 ? 1 : 0)});
			if(s.has("zone")) {
				const auto& z = s["zone"];
				const std::string loc = z["loc"].str();
				v.steps.back().zone_loc = loc == "mzone" ? 0x04 : loc == "szone" ? 0x08 : 0;
				v.steps.back().zone_seq = static_cast<uint8_t>(z["seq"].integer(0));
				v.steps.back().zone_other = z["other"].boolean(false);
			}
		}
		for(const auto& c : l["end_board"].items())
			v.end_board.push_back(static_cast<uint32_t>(c.integer()));
		{
			std::vector<std::string> u;
			for(const auto& s : l["under"].items())
				u.push_back(s.str());
			v.under = under_bits(u);
		}
		lines.push_back(std::move(v));
	}
	if(lines.empty())
		return error_json("no lines to check");
	ReplayConfig cfg;
	cfg.beam = static_cast<int>(req["beam"].integer(cfg.beam));
	cfg.tries = static_cast<int>(req["tries"].integer(cfg.tries));
	cfg.seconds = req["seconds"].num(cfg.seconds);
	cfg.seed = static_cast<uint64_t>(req["seed"].integer(1));
	cfg.explain = static_cast<int>(req["explain"].integer(0));
	cfg.cancel = &cancel_;
	set_progress("video lines", 0, static_cast<int>(lines.size()));
	// Verified lines also train the move policy (the one passed in, or a new one).
	Policy policy(db_);
	if(req["policy"].is_object())
		policy.from_value(req["policy"]);
	auto res = check_video_lines(db_, *scripts_, d, lines, cfg,
	                             [&](int done, int total) { set_progress("video lines", done, total); },
	                             req["learn"].boolean(true) ? &policy : nullptr);
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
		cfg.guide = std::make_shared<PieceGuide>(parse_library(db_, req["library"]), &db_);
	cfg.imitate = read_imitate(req["imitate"], db_);
	cfg.policy = read_policy(req["policy"], db_);
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

// Training positions from a recorded game (a YGO Omega replay rebuilt by the app), in the same
// form as `ygoe-cli selfplay` writes them, so the value network also learns from real games.
std::string Engine::replay_positions(const std::string& request) {
	if(!ok_)
		return error_json(error_);
	Value req = Value::parse(request);
	const int winner = static_cast<int>(req["winner"].integer(-1));
	if(winner != 0 && winner != 1)
		return error_json("the game has no winner");
	auto sid = [&](int64_t c) { return c > 0 ? db_.script_id(static_cast<uint32_t>(c)) : 0u; };
	Deck decks[2];
	for(int p = 0; p < 2; ++p) {
		const Value& d = req["decks"][static_cast<size_t>(p)];
		for(const auto& x : d["main"].items())
			decks[p].main.push_back(sid(x.integer()));
		for(const auto& x : d["extra"].items())
			decks[p].extra.push_back(sid(x.integer()));
	}
	Evaluator base(db_, decks[0], decks[1]);
	base.set_value_net(nullptr);  // the network learns a correction to the hand-written value
	auto view = [&](int64_t code, uint32_t pos, uint8_t p, uint8_t loc, uint32_t seq) {
		CardView v;
		v.code = sid(code);
		v.pos = pos;
		v.con = v.owner = p;
		v.loc = loc;
		v.seq = seq;
		if(const CardInfo* ci = v.code ? db_.get(v.code) : nullptr) {
			v.type = ci->type;
			v.atk = ci->atk;
			v.def = ci->def;
			v.level = ci->lv();
		}
		return v;
	};
	const std::string game = req["game"].str();
	const bool finished = req["finished"].boolean(true);
	std::ostringstream o;
	int n = 0;
	for(const auto& bj : req["boards"].items()) {
		Board b;
		b.turn = static_cast<uint32_t>(bj["turn"].integer());
		b.turn_player = static_cast<uint8_t>(bj["turn_player"].integer() & 1);
		for(uint8_t p = 0; p < 2; ++p) {
			const Value& s = bj["players"][p];
			b.lp[p] = static_cast<int32_t>(s["lp"].integer(8000));
			for(const auto& c : s["hand"].items())
				b.hand[p].push_back(view(c.integer(), POS_FACEUP, p, LOCATION_HAND, 0));
			for(const auto& c : s["grave"].items())
				b.grave[p].push_back(view(c.integer(), POS_FACEUP, p, LOCATION_GRAVE, 0));
			for(const auto& c : s["banished"].items())
				b.banished[p].push_back(view(c.integer(), POS_FACEUP, p, LOCATION_REMOVED, 0));
			b.deck[p].resize(static_cast<size_t>(std::max<int64_t>(0, s["deck"].integer())));
			b.extra[p].resize(static_cast<size_t>(std::max<int64_t>(0, s["extra"].integer())));
			for(const auto& c : s["mzone"].items())
				b.mzone[p].push_back(view(c["c"].integer(), static_cast<uint32_t>(c["p"].integer(POS_FACEUP_ATTACK)), p,
				                          LOCATION_MZONE, static_cast<uint32_t>(c["s"].integer())));
			for(const auto& c : s["szone"].items())
				b.szone[p].push_back(view(c["c"].integer(), static_cast<uint32_t>(c["p"].integer(POS_FACEUP)), p,
				                          LOCATION_SZONE, static_cast<uint32_t>(c["s"].integer())));
		}
		const uint8_t next = static_cast<uint8_t>(1 - b.turn_player);
		for(uint8_t me = 0; me < 2; ++me) {
			auto ev = base.evaluate(b, me, next);
			auto f = base.features(b, me, next, ev.value);
			o << "{\"z\":" << (winner == me ? 1 : 0) << ",\"fin\":" << (finished ? 1 : 0) << ",\"raw\":" << ev.raw
			  << ",\"turn\":" << b.turn << ",\"game\":" << quote(game) << ",\"src\":\"replay\",\"f\":[";
			for(size_t i = 0; i < f.size(); ++i)
				o << (i ? "," : "") << f[i];
			o << "]}\n";
			++n;
		}
	}
	return "{\"count\":" + std::to_string(n) + ",\"jsonl\":" + quote(o.str()) + "}";
}

namespace {
std::vector<uint8_t> from_base64(const std::string& in) {
	static int8_t t[256];
	static bool init = false;
	if(!init) {
		std::fill(std::begin(t), std::end(t), static_cast<int8_t>(-1));
		const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		for(int i = 0; i < 64; ++i)
			t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
		init = true;
	}
	std::vector<uint8_t> out;
	uint32_t acc = 0;
	int bits = 0;
	for(unsigned char c : in) {
		if(t[c] < 0)
			continue;
		acc = (acc << 6) | static_cast<uint32_t>(t[c]);
		bits += 6;
		if(bits >= 8) {
			bits -= 8;
			out.push_back(static_cast<uint8_t>(acc >> bits));
		}
	}
	return out;
}
}  // namespace

// {"messages": [base64 of each replay message], "decks": [{main, extra}|null, ...], "game", "per_turn"}
// -> the replay followed by the rules (follow.h): how far, and value-network positions.
std::string Engine::follow_replay(const std::string& request) {
	if(!ok_)
		return error_json(error_);
	Value req = Value::parse(request);
	FollowInput in;
	for(const auto& m : req["messages"].items())
		in.messages.push_back(from_base64(m.str()));
	for(int p = 0; p < 2; ++p) {
		const Value& d = req["decks"][static_cast<size_t>(p)];
		if(!d.is_object())
			continue;
		in.has_deck[p] = true;
		in.decks[p].main = codes(d["main"]);
		in.decks[p].extra = codes(d["extra"]);
	}
	in.game = req["game"].str();
	in.per_turn = static_cast<int>(req["per_turn"].integer(6));
	in.max_trials = static_cast<int>(req["max_trials"].integer(3000));
	in.max_ms = static_cast<int>(req["max_ms"].integer(60000));
	return follow_replay_impl(in);
}

std::string Engine::follow_replay_impl(const FollowInput& in) {
	std::lock_guard<std::mutex> run(run_mu_);
	return ygoe::follow_replay(db_, *scripts_, in).to_json();
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
