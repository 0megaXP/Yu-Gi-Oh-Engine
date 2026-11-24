#include <locale>
#include "duel.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include "lua.h"
#include "lauxlib.h"
#include "ocgapi.h"
#include "ocgapi_constants.h"

namespace ygoe {

// ---------------------------------------------------------------- helpers

namespace {

struct Reader {
	const uint8_t* p;
	const uint8_t* end;
	bool ok() const { return p <= end; }
	template<typename T>
	T get() {
		T v{};
		if(p + sizeof(T) <= end)
			std::memcpy(&v, p, sizeof(T));
		p += sizeof(T);
		return v;
	}
	void skip(size_t n) { p += n; }
};

struct LocInfo {
	uint8_t con, loc;
	uint32_t seq, pos;
};

LocInfo read_loc(Reader& r) {
	LocInfo l;
	l.con = r.get<uint8_t>();
	l.loc = r.get<uint8_t>();
	l.seq = r.get<uint32_t>();
	l.pos = r.get<uint32_t>();
	return l;
}

std::vector<uint8_t> select_indices(const std::vector<uint32_t>& idx) {
	std::vector<uint8_t> out(8 + 4 * idx.size());
	int32_t type = 0;
	uint32_t n = static_cast<uint32_t>(idx.size());
	std::memcpy(out.data(), &type, 4);
	std::memcpy(out.data() + 4, &n, 4);
	for(size_t i = 0; i < idx.size(); ++i)
		std::memcpy(out.data() + 8 + 4 * i, &idx[i], 4);
	return out;
}

// All index subsets with min..max elements, largest first, capped.
void subsets(uint32_t n, uint32_t min, uint32_t max, size_t cap, std::vector<std::vector<uint32_t>>& out) {
	if(max > n)
		max = n;
	for(int32_t k = static_cast<int32_t>(max); k >= static_cast<int32_t>(min) && out.size() < cap; --k) {
		std::vector<uint32_t> c(k);
		for(int32_t i = 0; i < k; ++i)
			c[i] = i;
		while(out.size() < cap) {
			out.push_back(c);
			int32_t i = k - 1;
			while(i >= 0 && c[i] == n - k + i)
				--i;
			if(i < 0)
				break;
			++c[i];
			for(int32_t j = i + 1; j < k; ++j)
				c[j] = c[j - 1] + 1;
		}
		if(k == 0)
			break;
	}
}

bool sum_check_exact(const std::vector<uint32_t>& params, size_t idx, int32_t acc) {
	// Mirrors select_sum_check1 in the core.
	if(acc == 0 || idx == params.size())
		return false;
	int32_t o1 = params[idx] & 0xffff;
	int32_t o2 = params[idx] >> 16;
	if(idx == params.size() - 1)
		return acc == o1 || acc == o2;
	return (acc > o1 && sum_check_exact(params, idx + 1, acc - o1)) ||
	       (o2 > 0 && acc > o2 && sum_check_exact(params, idx + 1, acc - o2));
}

bool sum_check_atleast(const std::vector<uint32_t>& params, int32_t acc) {
	int32_t sum = 0, mx = 0, mn = 0x7fffffff;
	for(auto op : params) {
		int32_t o1 = op & 0xffff, o2 = op >> 16;
		int32_t ms = (o2 && o2 < o1) ? o2 : o1;
		sum += ms;
		mx += (o2 > o1) ? o2 : o1;
		mn = std::min(mn, ms);
	}
	return !(mx < acc || sum - mn >= acc);
}

const char* pos_name(uint32_t pos) {
	switch(pos) {
	case POS_FACEUP_ATTACK: return "ATK";
	case POS_FACEDOWN_ATTACK: return "face-down ATK";
	case POS_FACEUP_DEFENSE: return "DEF";
	case POS_FACEDOWN_DEFENSE: return "set";
	default: return "?";
	}
}

constexpr size_t kMaxCombos = 40;

}  // namespace

std::vector<uint8_t> resp_i32(int32_t v) {
	std::vector<uint8_t> out(4);
	std::memcpy(out.data(), &v, 4);
	return out;
}

std::string loc_name(uint8_t loc) {
	switch(loc) {
	case LOCATION_DECK: return "Deck";
	case LOCATION_HAND: return "hand";
	case LOCATION_MZONE: return "Monster Zone";
	case LOCATION_SZONE: return "S/T Zone";
	case LOCATION_GRAVE: return "GY";
	case LOCATION_REMOVED: return "banished";
	case LOCATION_EXTRA: return "Extra Deck";
	case LOCATION_OVERLAY: return "material";
	default: return "?";
	}
}

const char* decision_name(DecisionType t) {
	static const char* names[] = {"none", "idle", "battle", "effect yes/no", "yes/no", "option", "select card",
	                              "chain", "place", "position", "tribute", "counter", "sum", "sort",
	                              "select/unselect", "announce race", "announce attribute", "announce card",
	                              "announce number", "rock-paper-scissors"};
	return names[static_cast<int>(t)];
}

int Board::count_field(uint8_t p) const {
	return static_cast<int>(mzone[p].size() + szone[p].size());
}

// ---------------------------------------------------------------- scripts

ScriptCache::ScriptCache(std::string dir, bool bytecode) : dir_(std::move(dir)), bytecode_(bytecode) {
	// scripts.pack: "YGOEPK1\n", u32 count, then per entry u16 name length, name, u64 offset, u32 size;
	// offsets are from the start of the file.
	std::ifstream f(dir_ + "/scripts.pack", std::ios::binary);
	if(!f)
		return;
	char magic[8];
	f.read(magic, 8);
	if(std::string(magic, 8) != "YGOEPK1\n")
		return;
	uint32_t count = 0;
	f.read(reinterpret_cast<char*>(&count), 4);
	for(uint32_t i = 0; i < count && f; ++i) {
		uint16_t len = 0;
		f.read(reinterpret_cast<char*>(&len), 2);
		std::string name(len, '\0');
		f.read(name.data(), len);
		PackEntry e;
		f.read(reinterpret_cast<char*>(&e.offset), 8);
		f.read(reinterpret_cast<char*>(&e.size), 4);
		pack_[name] = e;
	}
	if(f)
		pack_path_ = dir_ + "/scripts.pack";
	else
		pack_.clear();
}

bool ScriptCache::read_source(const std::string& name, std::string& out) {
	if(!pack_path_.empty()) {
		auto it = pack_.find(name);
		if(it == pack_.end())
			return false;
		std::ifstream f(pack_path_, std::ios::binary);
		f.seekg(static_cast<std::streamoff>(it->second.offset));
		out.resize(it->second.size);
		f.read(out.data(), it->second.size);
		return static_cast<bool>(f);
	}
	std::ifstream f(dir_ + "/" + name, std::ios::binary);
	if(!f)
		return false;
	std::stringstream ss;
	ss.imbue(std::locale::classic());  // C number format, whatever locale the host app set
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

void ScriptCache::put(const std::string& name, std::string text) {
	std::lock_guard<std::mutex> lock(mu_);
	// kept if there is one already: a duel may be loading it (its text must stay where it is)
	cache_.emplace(name, std::make_unique<std::string>(std::move(text)));
}

const std::string* ScriptCache::get(const std::string& name) {
	std::lock_guard<std::mutex> lock(mu_);
	auto it = cache_.find(name);
	if(it != cache_.end())
		return it->second.get();
	static const char* subdirs[] = {"official/", "pre-release/", "", "unofficial/"};
	std::unique_ptr<std::string> content;
	for(auto sub : subdirs) {
		std::string text;
		if(read_source(std::string(sub) + name, text)) {
			content = std::make_unique<std::string>(std::move(text));
			break;
		}
	}
	// Store precompiled Lua bytecode instead of source: every replayed duel loads the same
	// ~100 scripts, and skipping the parser makes duel creation several times faster.
	if(content && bytecode_) {
		lua_State* L = luaL_newstate();
		if(luaL_loadbuffer(L, content->data(), content->size(), name.c_str()) == LUA_OK) {
			std::string out;
			lua_dump(L, [](lua_State*, const void* p, size_t sz, void* ud) {
				static_cast<std::string*>(ud)->append(static_cast<const char*>(p), sz);
				return 0;
			}, &out, 0);
			*content = std::move(out);
		}
		lua_close(L);
	}
	auto* raw = content.get();
	cache_.emplace(name, std::move(content));
	return raw;
}

// ---------------------------------------------------------------- core callbacks

namespace {

using Payload = DuelReaders;

void card_reader(void* payload, uint32_t code, OCG_CardData* data) {
	auto* pl = static_cast<Payload*>(payload);
	std::memset(data, 0, sizeof(*data));
	data->code = code;
	auto* c = pl->db->get(code);
	auto* sets = new uint16_t[5]{};
	data->setcodes = sets;
	if(code == kProbeCardCode) {
		data->type = TYPE_SPELL | TYPE_CONTINUOUS;
		return;
	}
	if(!c)
		return;
	data->alias = c->alias;
	int n = 0;
	for(int i = 0; i < 4; ++i) {
		uint16_t s = static_cast<uint16_t>(c->setcode >> (i * 16));
		if(s)
			sets[n++] = s;
	}
	data->type = c->type;
	data->level = c->lv();
	data->attribute = c->attribute;
	data->race = c->race;
	data->attack = c->atk;
	data->defense = c->def;
	data->lscale = c->lscale();
	data->rscale = c->rscale();
	if(c->type & TYPE_LINK) {
		data->link_marker = static_cast<uint32_t>(c->def);
		data->defense = 0;
	}
}

void card_reader_done(void*, OCG_CardData* data) {
	delete[] data->setcodes;
}

int script_reader(void* payload, OCG_Duel duel, const char* name) {
	auto* pl = static_cast<Payload*>(payload);
	const std::string* s = pl->scripts->get(name);
	if(!s)
		return 0;
	return OCG_LoadScript(duel, s->data(), static_cast<uint32_t>(s->size()), name);
}

void log_handler(void*, const char* msg, int type) {
	if(type == OCG_LOG_TYPE_ERROR && std::getenv("YGOE_LOG"))
		fprintf(stderr, "[core] %s\n", msg);
}

}  // namespace

// ---------------------------------------------------------------- Duel

Duel::Duel(const CardDB& db, ScriptCache& scripts) : db_(db), scripts_(scripts) {}

Duel::~Duel() {
	if(duel_)
		OCG_DestroyDuel(duel_);
}

bool Duel::start(const DuelSetup& setup, std::string* error) {
	if(duel_)
		OCG_DestroyDuel(duel_);
	duel_ = nullptr;
	decision_ = {};
	ended_ = false;
	winner_ = -1;
	player_hints_[0].clear();
	player_hints_[1].clear();
	chain_depth_ = 0;
	given_.clear();
	zone_choices_ = false;
	for(const auto& ps : setup.players) {
		for(const auto* v : {&ps.main, &ps.extra, &ps.hand})
			for(uint32_t c : *v)
				zone_choices_ = zone_choices_ || db_.cares_about_columns(c);
		for(const auto& pc : ps.placed)
			zone_choices_ = zone_choices_ || db_.cares_about_columns(pc.code);
	}
	turn_ = 0;
	turn_player_ = 0;
	phase_ = 0;
	steps_ = 0;
	events_.clear();
	hand_news_.clear();
	log_.clear();
	log_players_.clear();
	moves_.clear();
	activations_.clear();
	chain_players_.clear();
	chain_codes_.clear();

	// The readers' payload is the duel's own, not one per thread: a live duel is created on one
	// app thread and continued on others, and a thread's data is gone once that thread ends.
	Payload& payload = readers_;

	OCG_DuelOptions opt{};
	for(int i = 0; i < 4; ++i)
		opt.seed[i] = setup.seed[i];
	opt.flags = setup.flags ? setup.flags : DUEL_MODE_MR5;
	if(setup.no_shuffle)
		opt.flags |= DUEL_PSEUDO_SHUFFLE;
	opt.team1 = {setup.players[0].lp, 0, 1};
	opt.team2 = {setup.players[1].lp, 0, 1};
	opt.cardReader = card_reader;
	opt.payload1 = &payload;
	opt.scriptReader = script_reader;
	opt.payload2 = &payload;
	opt.logHandler = log_handler;
	opt.cardReaderDone = card_reader_done;
	opt.enableUnsafeLibraries = 0;
	if(OCG_CreateDuel(&duel_, &opt) != OCG_DUEL_CREATION_SUCCESS) {
		if(error)
			*error = "OCG_CreateDuel failed";
		duel_ = nullptr;
		return false;
	}
	for(const char* base : {"constant.lua", "utility.lua"}) {
		if(!script_reader(&payload, duel_, base)) {
			if(error)
				*error = std::string("failed to load ") + base;
			return false;
		}
	}

	auto resolve = [&](uint32_t passcode) {
		uint32_t id = db_.script_id(passcode);
		if(!scripts_.has_card_script(id)) {
			if(auto c = db_.get(id); c && c->alias && scripts_.has_card_script(c->alias))
				return c->alias;
		}
		return id;
	};
	auto add = [&](uint8_t team, uint32_t code, uint32_t loc, uint32_t seq, uint32_t pos) {
		OCG_NewCardInfo info{};
		info.team = team;
		info.duelist = 0;
		info.code = resolve(code);
		info.con = team;
		info.loc = loc;
		info.seq = seq;
		info.pos = pos;
		OCG_DuelNewCard(duel_, &info);
	};
	for(uint8_t p = 0; p < 2; ++p) {
		const auto& ps = setup.players[p];
		for(auto code : ps.main)
			add(p, code, LOCATION_DECK, 0, POS_FACEDOWN_DEFENSE);
		for(auto code : ps.extra)
			add(p, code, LOCATION_EXTRA, 0, POS_FACEDOWN_DEFENSE);
		for(auto code : ps.hand)
			add(p, code, LOCATION_HAND, 0, POS_FACEDOWN_DEFENSE);
		for(const auto& pc : ps.placed) {
			uint32_t pos = pc.pos;
			if(!pos)
				pos = (pc.loc == LOCATION_MZONE) ? POS_FACEUP_ATTACK
				      : (pc.loc == LOCATION_SZONE) ? POS_FACEDOWN_DEFENSE : POS_FACEUP;
			add(p, pc.code, pc.loc, pc.seq, pos);
		}
	}
	OCG_StartDuel(duel_);
	process();
	return true;
}

bool Duel::run_lua(const std::string& code) {
	if(!duel_)
		return false;
	return OCG_LoadScript(duel_, code.data(), static_cast<uint32_t>(code.size()), "ygoe_probe") != 0;
}

bool Duel::respond(const std::vector<uint8_t>& response) {
	if(!duel_ || ended_)
		return false;
	Decision saved = decision_;
	OCG_DuelSetResponse(duel_, response.data(), static_cast<uint32_t>(response.size()));
	retry_ = false;
	++steps_;
	process();
	if(retry_) {
		// The core keeps waiting on the same question; restore it so another answer can be tried.
		decision_ = std::move(saved);
		return false;
	}
	return true;
}

bool Duel::replay(const DuelSetup& setup, const std::vector<std::vector<uint8_t>>& responses, std::string* error) {
	if(!start(setup, error))
		return false;
	auto hide = [&](size_t done) {
		if(done == setup.hidden_at)
			for(const auto& h : setup.hidden)
				set_hidden(h);
	};
	hide(0);
	for(size_t i = 0; i < responses.size(); ++i) {
		if(!respond(responses[i])) {
			if(error)
				*error = "response " + std::to_string(i) + " rejected during replay";
			return false;
		}
		hide(i + 1);
	}
	return true;
}

void Duel::set_hidden(const DuelSetup::Hidden& h) {
	if(duel_ && h.player < 2)
		OCG_DuelSetHidden(duel_, h.player, h.hand.data(), static_cast<uint32_t>(h.hand.size()), h.guessed ? h.deck.data() : nullptr,
		                  static_cast<uint32_t>(h.deck.size()), h.seed);
}

void Duel::set_deck_top(uint8_t player, const std::vector<uint32_t>& codes) {
	if(duel_ && player < 2 && !codes.empty())
		OCG_DuelSetDeckTop(duel_, player, codes.data(), static_cast<uint32_t>(codes.size()));
}

void Duel::process() {
	decision_ = {};
	// the rules core going round without ever asking or ending (a loop in a card script): ended
	constexpr long kMaxSteps = 2000000;
	for(long steps = 0;; ++steps) {
		if(steps > kMaxSteps) {
			ended_ = true;
			winner_ = 2;
			return;
		}
		int status = OCG_DuelProcess(duel_);
		uint32_t len = 0;
		auto* buf = static_cast<const uint8_t*>(OCG_DuelGetMessage(duel_, &len));
		if(tap_)
			tap_(buf, len);
		parse_messages(buf, len);
		if(retry_)
			return;
		if(status == OCG_DUEL_STATUS_END) {
			ended_ = true;
			if(winner_ < 0)
				winner_ = 2;
			return;
		}
		if(ended_)
			return;
		if(status == OCG_DUEL_STATUS_AWAITING && decision_.type != DecisionType::None)
			return;
		if(status == OCG_DUEL_STATUS_AWAITING) {
			// Awaiting without a decision we understand: should not happen.
			ended_ = true;
			winner_ = 2;
			return;
		}
	}
}

void Duel::parse_messages(const uint8_t* data, uint32_t len) {
	Reader r{data, data + len};
	while(r.p + 4 <= r.end) {
		uint32_t size = r.get<uint32_t>();
		const uint8_t* msg = r.p;
		r.skip(size);
		if(size == 0 || r.p > r.end)
			break;
		Reader m{msg + 1, msg + size};
		uint8_t type = msg[0];
		if(log_enabled_)
			log_message(type, msg + 1, msg + size);
		switch(type) {
		case MSG_RETRY:
			retry_ = true;
			break;
		case MSG_HINT: {
			uint8_t htype = m.get<uint8_t>();
			uint8_t player = m.get<uint8_t>();
			uint64_t value = m.get<uint64_t>();
			if(htype == HINT_SELECTMSG && player < 2)
				last_hint_[player] = value;
			break;
		}
		case MSG_MOVE: {
			m.get<uint32_t>();
			auto from = read_loc(m);
			auto to = read_loc(m);
			auto field = [](const LocInfo& l) { return !(l.loc & LOCATION_OVERLAY) && (l.loc & (LOCATION_MZONE | LOCATION_SZONE)); };
			auto key = [](const LocInfo& l) { return static_cast<uint32_t>(l.con) << 16 | static_cast<uint32_t>(l.loc) << 8 | (l.seq & 0xff); };
			uint8_t giver = to.con;
			if(field(from)) {
				auto g = given_.find(key(from));
				if(g != given_.end()) {
					giver = g->second;
					given_.erase(g);
				}
			} else if(from.loc) {
				giver = from.con;  // from a hand, Deck, Extra Deck, GY or banishment
			}
			// from one's own piles onto the other field it stays the putting player's card; moved
			// between fields (a control change) the taker plays it
			if(field(to) && giver != to.con && (!field(from) || from.con == to.con))
				given_[key(to)] = giver;
			break;
		}
		case MSG_CHAIN_END:
			chain_depth_ = 0;
			break;
		case MSG_PLAYER_HINT: {
			uint8_t player = m.get<uint8_t>();
			uint8_t htype = m.get<uint8_t>();
			uint64_t value = m.get<uint64_t>();
			if(player < 2) {
				auto& v = player_hints_[player];
				if(htype == PHINT_DESC_ADD) {
					v.push_back(value);
				} else if(htype == PHINT_DESC_REMOVE) {
					auto it = std::find(v.begin(), v.end(), value);
					if(it != v.end())
						v.erase(it);
				}
			}
			break;
		}
		case MSG_WIN: {
			uint8_t player = m.get<uint8_t>();
			ended_ = true;
			winner_ = player;
			events_.push_back({Event::Win, player, 0, m.get<uint8_t>()});
			break;
		}
		case MSG_NEW_TURN:
			turn_player_ = m.get<uint8_t>();
			++turn_;
			events_.push_back({Event::NewTurn, turn_player_, 0, turn_});
			break;
		case MSG_NEW_PHASE:
			phase_ = m.get<uint16_t>();
			events_.push_back({Event::NewPhase, turn_player_, 0, phase_});
			break;
		case MSG_CHAINING: {
			uint32_t code = m.get<uint32_t>();
			auto li = read_loc(m);
			events_.push_back({Event::Chaining, li.con, code, li.loc});
			++chain_depth_;
			chain_last_player_ = li.con;
			{
				auto g = given_.find(static_cast<uint32_t>(li.con) << 16 | static_cast<uint32_t>(li.loc) << 8 | (li.seq & 0xff));
				chain_last_owner_ = g != given_.end() ? g->second : li.con;
			}
			chain_last_code_ = code;
			chain_last_loc_ = li.loc;
			break;
		}
		case MSG_CHAIN_NEGATED:
		case MSG_CHAIN_DISABLED:
			events_.push_back({Event::Negated, 0, 0, m.get<uint8_t>()});
			break;
		case MSG_SPSUMMONING: {
			uint32_t code = m.get<uint32_t>();
			auto li = read_loc(m);
			events_.push_back({Event::SpSummoned, li.con, code, 0});
			break;
		}
		case MSG_SUMMONING: {
			uint32_t code = m.get<uint32_t>();
			auto li = read_loc(m);
			events_.push_back({Event::Summoned, li.con, code, 0});
			break;
		}
		case MSG_DRAW: {
			uint8_t player = m.get<uint8_t>();
			const uint32_t n = m.get<uint32_t>();
			events_.push_back({Event::Draw, player, 0, n});
			for(uint32_t k = 0; k < n && m.p + 8 <= m.end; ++k) {
				const uint32_t code = m.get<uint32_t>();
				m.get<uint32_t>();  // position
				if(player < 2)
					hand_news_.push_back({player, code, true});
			}
			break;
		}
		case MSG_CONFIRM_CARDS: {
			m.get<uint8_t>();  // shown to
			const uint32_t n = m.get<uint32_t>();
			for(uint32_t k = 0; k < n && m.p + 10 <= m.end; ++k) {
				const uint32_t code = m.get<uint32_t>();
				const uint8_t con = m.get<uint8_t>();
				const uint8_t loc = m.get<uint8_t>();
				m.get<uint32_t>();  // sequence
				if(con < 2 && (loc & LOCATION_HAND))
					hand_news_.push_back({con, code, false});
			}
			break;
		}
		case MSG_SELECT_BATTLECMD:
		case MSG_SELECT_IDLECMD:
		case MSG_SELECT_EFFECTYN:
		case MSG_SELECT_YESNO:
		case MSG_SELECT_OPTION:
		case MSG_SELECT_CARD:
		case MSG_SELECT_CHAIN:
		case MSG_SELECT_PLACE:
		case MSG_SELECT_DISFIELD:
		case MSG_SELECT_POSITION:
		case MSG_SELECT_TRIBUTE:
		case MSG_SELECT_COUNTER:
		case MSG_SELECT_SUM:
		case MSG_SORT_CARD:
		case MSG_SORT_CHAIN:
		case MSG_SELECT_UNSELECT_CARD:
		case MSG_ANNOUNCE_RACE:
		case MSG_ANNOUNCE_ATTRIB:
		case MSG_ANNOUNCE_CARD:
		case MSG_ANNOUNCE_NUMBER:
		case MSG_ROCK_PAPER_SCISSORS:
			build_decision(type, msg + 1, msg + size);
			break;
		default:
			break;
		}
	}
}

namespace {
std::string who(uint8_t p) { return p < 2 ? "[P" + std::to_string(p) + "]" : "?"; }
const char* phase_name(uint16_t ph) {
	switch(ph) {
	case PHASE_DRAW: return "Draw Phase";
	case PHASE_STANDBY: return "Standby Phase";
	case PHASE_MAIN1: return "Main Phase 1";
	case PHASE_BATTLE_START: return "Battle Phase";
	case PHASE_MAIN2: return "Main Phase 2";
	case PHASE_END: return "End Phase";
	default: return nullptr;
	}
}
}  // namespace

void Duel::log_message(uint8_t type, const uint8_t* begin, const uint8_t* end) {
	Reader m{begin, end};
	auto name = [&](uint32_t code) { return code ? db_.name(code) : std::string("a face-down card"); };
	switch(type) {
	case MSG_NEW_TURN: {
		uint8_t p = m.get<uint8_t>();
		push_log(p, "— Turn " + std::to_string(turn_ + 1) + ": " + who(p) + " —");
		break;
	}
	case MSG_NEW_PHASE: {
		if(const char* n = phase_name(m.get<uint16_t>()))
			push_log(turn_player_, n);
		break;
	}
	case MSG_DRAW: {
		uint8_t p = m.get<uint8_t>();
		uint32_t n = m.get<uint32_t>();
		push_log(p, who(p) + " draws " + std::to_string(n) + " card" + (n == 1 ? "" : "s"));
		break;
	}
	case MSG_SUMMONING: {
		uint32_t code = m.get<uint32_t>();
		auto li = read_loc(m);
		push_log(li.con, who(li.con) + " Normal Summons " + name(code));
		break;
	}
	case MSG_SPSUMMONING: {
		uint32_t code = m.get<uint32_t>();
		auto li = read_loc(m);
		push_log(li.con, who(li.con) + " Special Summons " + name(code));
		break;
	}
	case MSG_FLIPSUMMONING: {
		uint32_t code = m.get<uint32_t>();
		auto li = read_loc(m);
		push_log(li.con, who(li.con) + " Flip Summons " + name(code));
		break;
	}
	case MSG_SET: {
		m.get<uint32_t>();
		auto li = read_loc(m);
		push_log(li.con, who(li.con) + " sets a card");
		break;
	}
	case MSG_CHAINING: {
		uint32_t code = m.get<uint32_t>();
		auto li = read_loc(m);
		chain_codes_.push_back(code);
		chain_players_.push_back(li.con);
		activations_.push_back({code, li.con, li.loc, li.seq, li.pos, static_cast<uint32_t>(moves_.size())});
		push_log(li.con, who(li.con) + " activates " + name(code) +
		               (chain_codes_.size() > 1 ? " (chain link " + std::to_string(chain_codes_.size()) + ")" : ""));
		break;
	}
	case MSG_CHAIN_NEGATED:
	case MSG_CHAIN_DISABLED: {
		uint8_t link = m.get<uint8_t>();
		std::string what = link >= 1 && link <= chain_codes_.size() ? name(chain_codes_[link - 1]) : "the effect";
		int owner = link >= 1 && link <= chain_players_.size() ? chain_players_[link - 1] : -1;
		push_log(owner, "   " + what + (type == MSG_CHAIN_NEGATED ? " is negated" : "'s effect is negated"));
		break;
	}
	case MSG_CHAIN_END:
		chain_codes_.clear();
		chain_players_.clear();
		break;
	case MSG_ATTACK: {
		auto a = read_loc(m);
		auto t = read_loc(m);
		(void)t;
		push_log(a.con, who(a.con) + " attacks" + (t.loc ? "" : " directly"));
		break;
	}
	case MSG_DAMAGE:
	case MSG_PAY_LPCOST: {
		uint8_t p = m.get<uint8_t>();
		uint32_t amount = m.get<uint32_t>();
		push_log(p, who(p) + (type == MSG_DAMAGE ? " takes " : " pays ") + std::to_string(amount) + " LP");
		break;
	}
	case MSG_RECOVER: {
		uint8_t p = m.get<uint8_t>();
		push_log(p, who(p) + " gains " + std::to_string(m.get<uint32_t>()) + " LP");
		break;
	}
	case MSG_MOVE: {
		uint32_t code = m.get<uint32_t>();
		auto from = read_loc(m);
		auto to = read_loc(m);
		// Every real change of place goes to the animation list (not position changes in the same zone).
		if(from.loc != to.loc || from.con != to.con || from.seq != to.seq)
			moves_.push_back({code, from.con, from.loc, to.con, to.loc, from.seq, from.pos, to.seq, to.pos});
		// Only the moves a player would notice: cards leaving the field or hand, or being banished.
		bool from_visible = from.loc & (LOCATION_MZONE | LOCATION_SZONE | LOCATION_HAND | LOCATION_GRAVE);
		bool to_pile = to.loc & (LOCATION_GRAVE | LOCATION_REMOVED | LOCATION_HAND | LOCATION_DECK | LOCATION_EXTRA);
		if(from.loc && to.loc && from.loc != to.loc && from_visible && to_pile && !(from.loc & LOCATION_OVERLAY)) {
			if(from.loc == LOCATION_HAND && to.loc == LOCATION_DECK)
				break;
			push_log(from.con, "   " + name(code) + ": " + loc_name(from.loc) + " → " + loc_name(to.loc));
		} else if((from.loc & LOCATION_DECK) && (to.loc & (LOCATION_HAND | LOCATION_GRAVE | LOCATION_REMOVED)) && code) {
			push_log(from.con, "   " + name(code) + ": Deck → " + loc_name(to.loc));
		}
		break;
	}
	case MSG_WIN: {
		uint8_t p = m.get<uint8_t>();
		push_log(p, p < 2 ? who(p) + " wins" : "The duel is a draw");
		break;
	}
	default:
		break;
	}
}

std::string Duel::card_label(uint32_t code, uint64_t desc) const {
	std::string s = db_.name(code);
	if(desc) {
		std::string d = db_.description(desc);
		if(!d.empty())
			s += " — " + d;
	}
	return s;
}

void Duel::build_decision(uint8_t msg, const uint8_t* begin, const uint8_t* end) {
	Reader r{begin, end};
	Decision d;
	d.player = r.get<uint8_t>();
	if(d.player < 2)
		d.hint = last_hint_[d.player];
	auto add = [&](OptKind kind, std::vector<uint8_t> resp, std::string label, uint32_t code = 0,
	               uint8_t con = 0, uint8_t loc = 0, uint32_t seq = 0, uint64_t desc = 0) {
		Option o;
		o.kind = kind;
		o.response = std::move(resp);
		o.label = std::move(label);
		o.code = code;
		o.con = con;
		o.loc = loc;
		o.seq = seq;
		o.desc = desc;
		d.options.push_back(std::move(o));
	};

	switch(msg) {
	case MSG_SELECT_IDLECMD: {
		d.type = DecisionType::Idle;
		const char* verbs[] = {"Normal Summon", "Special Summon", "Change position of", "Set", "Set"};
		const OptKind kinds[] = {OptKind::Summon, OptKind::SpSummon, OptKind::Reposition, OptKind::MSet, OptKind::SSet};
		for(int t = 0; t < 5; ++t) {
			uint32_t n = r.get<uint32_t>();
			for(uint32_t i = 0; i < n; ++i) {
				uint32_t code = r.get<uint32_t>();
				uint8_t con = r.get<uint8_t>();
				uint8_t loc = r.get<uint8_t>();
				uint32_t seq = (t == 2) ? r.get<uint8_t>() : r.get<uint32_t>();
				std::string label = std::string(verbs[t]) + " " + db_.name(code);
				if(t == 1)
					label += " from " + loc_name(loc);
				add(kinds[t], resp_i32(static_cast<int32_t>((i << 16) | t)), label, code, con, loc, seq);
			}
		}
		uint32_t n = r.get<uint32_t>();
		for(uint32_t i = 0; i < n; ++i) {
			uint32_t code = r.get<uint32_t>();
			uint8_t con = r.get<uint8_t>();
			uint8_t loc = r.get<uint8_t>();
			uint32_t seq = r.get<uint32_t>();
			uint64_t desc = r.get<uint64_t>();
			r.get<uint8_t>();
			add(OptKind::Activate, resp_i32(static_cast<int32_t>((i << 16) | 5)),
			    "Activate " + card_label(code, desc) + " (" + loc_name(loc) + ")", code, con, loc, seq, desc);
		}
		if(r.get<uint8_t>())
			add(OptKind::ToBattle, resp_i32(6), "Go to Battle Phase");
		if(r.get<uint8_t>())
			add(OptKind::ToEnd, resp_i32(7), "Go to End Phase");
		// Hand shuffle (t == 8) never matters; not offered.
		break;
	}
	case MSG_SELECT_BATTLECMD: {
		d.type = DecisionType::Battle;
		uint32_t n = r.get<uint32_t>();
		for(uint32_t i = 0; i < n; ++i) {
			uint32_t code = r.get<uint32_t>();
			uint8_t con = r.get<uint8_t>();
			uint8_t loc = r.get<uint8_t>();
			uint32_t seq = r.get<uint32_t>();
			uint64_t desc = r.get<uint64_t>();
			r.get<uint8_t>();
			add(OptKind::Activate, resp_i32(static_cast<int32_t>(i << 16)),
			    "Activate " + card_label(code, desc), code, con, loc, seq, desc);
		}
		n = r.get<uint32_t>();
		for(uint32_t i = 0; i < n; ++i) {
			uint32_t code = r.get<uint32_t>();
			uint8_t con = r.get<uint8_t>();
			uint8_t loc = r.get<uint8_t>();
			uint8_t seq = r.get<uint8_t>();
			uint8_t direct = r.get<uint8_t>();
			add(OptKind::Attack, resp_i32(static_cast<int32_t>((i << 16) | 1)),
			    "Attack with " + db_.name(code) + (direct ? " (can attack directly)" : ""), code, con, loc, seq);
		}
		if(r.get<uint8_t>())
			add(OptKind::ToMain2, resp_i32(2), "Go to Main Phase 2");
		if(r.get<uint8_t>())
			add(OptKind::ToEnd, resp_i32(3), "Go to End Phase");
		break;
	}
	case MSG_SELECT_EFFECTYN: {
		d.type = DecisionType::EffectYesNo;
		uint32_t code = r.get<uint32_t>();
		auto li = read_loc(r);
		uint64_t desc = r.get<uint64_t>();
		add(OptKind::Yes, resp_i32(1), "Use " + card_label(code, desc), code, li.con, li.loc, li.seq, desc);
		add(OptKind::No, resp_i32(0), "Don't use " + db_.name(code), code, li.con, li.loc, li.seq, desc);
		break;
	}
	case MSG_SELECT_YESNO: {
		d.type = DecisionType::YesNo;
		uint64_t desc = r.get<uint64_t>();
		std::string text = db_.description(desc);
		if(text.empty())
			text = "#" + std::to_string(desc);
		add(OptKind::Yes, resp_i32(1), "Yes: " + text, static_cast<uint32_t>(desc >> 20), 0, 0, 0, desc);
		add(OptKind::No, resp_i32(0), "No: " + text, static_cast<uint32_t>(desc >> 20), 0, 0, 0, desc);
		break;
	}
	case MSG_SELECT_OPTION: {
		d.type = DecisionType::Option;
		uint8_t n = r.get<uint8_t>();
		for(uint8_t i = 0; i < n; ++i) {
			uint64_t desc = r.get<uint64_t>();
			std::string text = db_.description(desc);
			if(text.empty())
				text = "option " + std::to_string(i + 1);
			add(OptKind::Choose, resp_i32(i), text, static_cast<uint32_t>(desc >> 20), 0, 0, 0, desc);
		}
		break;
	}
	case MSG_SELECT_CARD:
	case MSG_SELECT_TRIBUTE: {
		d.type = msg == MSG_SELECT_CARD ? DecisionType::Card : DecisionType::Tribute;
		uint8_t cancelable = r.get<uint8_t>();
		uint32_t min = r.get<uint32_t>();
		uint32_t max = r.get<uint32_t>();
		uint32_t n = r.get<uint32_t>();
		std::vector<uint32_t> codes(n), params(n, 1);
		std::vector<LocInfo> locs(n);
		for(uint32_t i = 0; i < n; ++i) {
			codes[i] = r.get<uint32_t>();
			if(msg == MSG_SELECT_CARD) {
				locs[i] = read_loc(r);
			} else {
				locs[i].con = r.get<uint8_t>();
				locs[i].loc = r.get<uint8_t>();
				locs[i].seq = r.get<uint32_t>();
				params[i] = r.get<uint8_t>();
			}
		}
		std::vector<std::vector<uint32_t>> combos;
		if(msg == MSG_SELECT_TRIBUTE) {
			std::vector<std::vector<uint32_t>> all;
			subsets(n, 1, max, 400, all);
			for(auto& c : all) {
				uint32_t t = 0;
				for(auto i : c)
					t += params[i];
				if(t >= min && combos.size() < kMaxCombos)
					combos.push_back(c);
			}
		} else {
			subsets(n, min, max, kMaxCombos, combos);
		}
		for(auto& c : combos) {
			std::string label;
			uint32_t first = 0;
			for(auto i : c) {
				if(!label.empty())
					label += " + ";
				label += db_.name(codes[i]);
				if(locs[i].loc)
					label += " (" + loc_name(locs[i].loc) + (locs[i].con != d.player ? ", opp." : "") + ")";
				if(!first)
					first = codes[i];
			}
			if(c.empty())
				label = "(none)";
			Option o;
			o.kind = OptKind::Select;
			o.code = first;
			if(!c.empty()) {
				o.con = locs[c[0]].con;
				o.loc = locs[c[0]].loc;
			}
			o.response = select_indices(c);
			o.label = label;
			for(auto i : c) {
				o.codes.push_back(codes[i]);
				o.locs.push_back(locs[i].loc);
				o.seqs.push_back(locs[i].seq);
			}
			d.options.push_back(std::move(o));
		}
		if(d.options.empty() && cancelable)
			add(OptKind::Pass, resp_i32(-1), "Cancel");
		break;
	}
	case MSG_SELECT_UNSELECT_CARD: {
		d.type = DecisionType::UnselectCard;
		uint8_t finishable = r.get<uint8_t>();
		uint8_t cancelable = r.get<uint8_t>();
		r.get<uint32_t>();
		r.get<uint32_t>();
		uint32_t n1 = r.get<uint32_t>();
		uint32_t idx = 0;
		for(uint32_t i = 0; i < n1; ++i, ++idx) {
			uint32_t code = r.get<uint32_t>();
			auto li = read_loc(r);
			std::vector<uint8_t> resp(8);
			int32_t one = 1;
			std::memcpy(resp.data(), &one, 4);
			std::memcpy(resp.data() + 4, &idx, 4);
			add(OptKind::Select, resp, db_.name(code) + " (" + loc_name(li.loc) + (li.con != d.player ? ", opp." : "") + ")",
			    code, li.con, li.loc, li.seq);
		}
		uint32_t n2 = r.get<uint32_t>();
		for(uint32_t i = 0; i < n2; ++i, ++idx) {
			uint32_t code = r.get<uint32_t>();
			auto li = read_loc(r);
			(void)code;
			(void)li;  // unselecting is never needed to reach a legal selection
		}
		if(finishable || (d.options.empty() && cancelable))
			add(OptKind::Pass, resp_i32(-1), finishable ? "Done selecting" : "Cancel");
		break;
	}
	case MSG_SELECT_CHAIN: {
		d.type = DecisionType::Chain;
		r.get<uint8_t>();
		uint8_t forced = r.get<uint8_t>();
		r.get<uint32_t>();
		r.get<uint32_t>();
		uint32_t n = r.get<uint32_t>();
		if(!forced)
			add(OptKind::Pass, resp_i32(-1), "Don't respond");
		for(uint32_t i = 0; i < n; ++i) {
			uint32_t code = r.get<uint32_t>();
			auto li = read_loc(r);
			uint64_t desc = r.get<uint64_t>();
			r.get<uint8_t>();
			add(OptKind::Activate, resp_i32(static_cast<int32_t>(i)),
			    "Chain " + card_label(code, desc) + " (" + loc_name(li.loc) + ")", code, li.con, li.loc, li.seq, desc);
		}
		break;
	}
	case MSG_SELECT_PLACE:
	case MSG_SELECT_DISFIELD: {
		d.type = DecisionType::Place;
		d.minor = true;
		uint8_t count = r.get<uint8_t>();
		uint32_t flag = r.get<uint32_t>();
		// Zones whose bit is 0 are available. Own side first, main monster zones left to right.
		std::vector<std::array<uint8_t, 3>> zones;
		for(int side = 0; side < 2; ++side) {
			uint8_t pl = side == 0 ? d.player : 1 - d.player;
			uint32_t f = side == 0 ? flag : flag >> 16;
			for(uint8_t s = 0; s < 7; ++s)
				if(!(f & (1u << s)))
					zones.push_back({pl, LOCATION_MZONE, s});
			for(uint8_t s = 0; s < 8; ++s)
				if(!(f & (1u << (s + 8))))
					zones.push_back({pl, LOCATION_SZONE, s});
		}
		if(msg == MSG_SELECT_DISFIELD)
			std::stable_sort(zones.begin(), zones.end(), [&](auto& a, auto& b) { return (a[0] != d.player) > (b[0] != d.player); });
		// Zones named as the deciding player sees the table: columns 1-5 from their left.
		auto zone_label = [&](const std::array<uint8_t, 3>& z) {
			bool mine = z[0] == d.player;
			uint8_t s = z[2];
			std::string what;
			if(z[1] == LOCATION_MZONE && s >= 5) {
				bool left = (s == 5) == mine;
				what = std::string("Extra Monster Zone (") + (left ? "left" : "right") + ")";
			} else if(z[1] == LOCATION_SZONE && s == 5) {
				what = "Field Zone";
			} else if(z[1] == LOCATION_SZONE && s >= 6) {
				what = std::string("Pendulum Zone (") + ((s == 6) == mine ? "left" : "right") + ")";
			} else {
				int column = mine ? s + 1 : 5 - s;
				what = std::string(z[1] == LOCATION_MZONE ? "Monster Zone" : "Spell & Trap Zone") + ", column " +
				       std::to_string(column) + (column == 1 ? " (left)" : column == 5 ? " (right)" : "");
			}
			return (mine ? "Your " : "Opponent's ") + what;
		};
		if(zones.size() >= count) {
			std::vector<uint8_t> resp;
			std::string label;
			for(uint8_t i = 0; i < count; ++i) {
				resp.insert(resp.end(), zones[i].begin(), zones[i].end());
				label += (i ? " + " : "") + zone_label(zones[i]);
			}
			add(OptKind::Choose, resp, label);
			// One zone to pick: offer every free zone, because columns and Link arrows matter.
			if(count == 1)
				for(size_t z = 1; z < zones.size(); ++z)
					add(OptKind::Choose, {zones[z][0], zones[z][1], zones[z][2]}, zone_label(zones[z]));
			// Where a card in the duel cares about columns, the zone is a real decision. Only zones
			// that differ in what shares their column are offered (the first free zone first): one
			// per mix of "a card of yours / of the opponent in this column / next to it".
			if(count == 1 && zone_choices_ && d.options.size() > 1) {
				d.minor = false;
				auto column = [](uint8_t pl, uint8_t loc, uint8_t seq) -> int {
					// absolute column 0..4 seen from player 0 (Extra Monster Zones: 1 and 3)
					int c = loc == LOCATION_MZONE && seq == 5 ? 1 : loc == LOCATION_MZONE && seq == 6 ? 3 : seq;
					if(loc == LOCATION_SZONE && seq >= 5)
						return -1;
					return pl == 0 ? c : 4 - c;
				};
				// What sits in each column now.
				int own[5] = {0, 0, 0, 0, 0}, opp[5] = {0, 0, 0, 0, 0};
				const Board b = board();
				for(uint8_t pl = 0; pl < 2; ++pl) {
					for(const auto& c : b.mzone[pl])
						if(c.code && c.seq < 7)
							++(pl == d.player ? own : opp)[column(pl, LOCATION_MZONE, c.seq)];
					for(const auto& c : b.szone[pl])
						if(c.code && c.seq < 5)
							++(pl == d.player ? own : opp)[column(pl, LOCATION_SZONE, c.seq)];
				}
				std::vector<Option> kept;
				std::vector<int> seen;
				for(const auto& o : d.options) {
					if(o.response.size() != 3)
						continue;
					int c = column(o.response[0], o.response[1], o.response[2]);
					int sig = o.response[1] * 1000 + (o.response[0] == d.player ? 0 : 500);
					if(c >= 0)
						sig += (own[c] > 0) + 2 * (opp[c] > 0) + 4 * ((c > 0 && opp[c - 1]) || (c < 4 && opp[c + 1])) +
						       8 * ((c > 0 && own[c - 1]) || (c < 4 && own[c + 1]));
					else
						sig += 100 + o.response[2];
					if(std::find(seen.begin(), seen.end(), sig) != seen.end())
						continue;
					seen.push_back(sig);
					kept.push_back(o);
					if(kept.size() >= 4)
						break;
				}
				// The default (first) zone keeps cards apart from one's own cards in the same column: a
				// Spell/Trap away from your monsters, a monster away from your Spell/Trap cards. One
				// column interruption (Infinite Impermanence) then can't stop both. The search still
				// weighs the other zones where a column effect wants them.
				auto apart = [&](const Option& o) {
					const uint8_t pl = o.response[0], loc = o.response[1], seq = o.response[2];
					if(pl != d.player)
						return 0;
					int mine_other = 0;  // own cards of the other row in this column
					const int c = column(pl, loc, seq);
					if(c < 0)
						return 0;
					for(const auto& x : loc == LOCATION_MZONE ? b.szone[pl] : b.mzone[pl])
						if(x.code && column(pl, loc == LOCATION_MZONE ? LOCATION_SZONE : LOCATION_MZONE, static_cast<uint8_t>(x.seq)) == c)
							++mine_other;
					return mine_other > 0 ? 1 : 0;
				};
				std::stable_sort(kept.begin(), kept.end(), [&](const Option& a, const Option& b2) { return apart(a) < apart(b2); });
				// A real choice only while a column card is in play (on the field, or in the deciding
				// player's hand); otherwise the safe default is taken without branching the search.
				bool live = false;
				for(uint8_t pl = 0; pl < 2 && !live; ++pl)
					for(const auto* z : {&b.mzone[pl], &b.szone[pl]})
						for(const auto& x : *z)
							live = live || (x.code && db_.cares_about_columns(x.code));
				for(const auto& x : b.hand[d.player])
					live = live || db_.cares_about_columns(x.code);
				const size_t offered = kept.size();
				if(offered > 1)
					d.options = std::move(kept);
				if(offered <= 1 || !live)
					d.minor = true;
			}
		}
		break;
	}
	case MSG_SELECT_POSITION: {
		d.type = DecisionType::Position;
		d.minor = true;
		uint32_t code = r.get<uint32_t>();
		uint8_t positions = r.get<uint8_t>();
		auto c = db_.get(code);
		uint32_t order[4] = {POS_FACEUP_ATTACK, POS_FACEUP_DEFENSE, POS_FACEDOWN_DEFENSE, POS_FACEDOWN_ATTACK};
		if(c && c->def > c->atk)
			std::swap(order[0], order[1]);
		for(auto p : order)
			if(positions & p)
				add(OptKind::Choose, resp_i32(static_cast<int32_t>(p)), db_.name(code) + " in " + pos_name(p), code);
		break;
	}
	case MSG_SELECT_COUNTER: {
		d.type = DecisionType::Counter;
		d.minor = true;
		r.get<uint16_t>();
		uint16_t count = r.get<uint16_t>();
		uint32_t n = r.get<uint32_t>();
		std::vector<uint8_t> resp;
		for(uint32_t i = 0; i < n; ++i) {
			r.get<uint32_t>();
			r.get<uint8_t>();
			r.get<uint8_t>();
			r.get<uint8_t>();
			uint16_t have = r.get<uint16_t>();
			uint16_t take = std::min(have, count);
			count = static_cast<uint16_t>(count - take);
			resp.push_back(static_cast<uint8_t>(take & 0xff));
			resp.push_back(static_cast<uint8_t>(take >> 8));
		}
		add(OptKind::Choose, resp, "remove counters");
		break;
	}
	case MSG_SELECT_SUM: {
		d.type = DecisionType::Sum;
		uint8_t at_least = r.get<uint8_t>();
		int32_t acc = static_cast<int32_t>(r.get<uint32_t>());
		uint32_t min = r.get<uint32_t>();
		uint32_t max = r.get<uint32_t>();
		uint32_t nm = r.get<uint32_t>();
		std::vector<uint32_t> must;
		for(uint32_t i = 0; i < nm; ++i) {
			r.get<uint32_t>();
			read_loc(r);
			must.push_back(r.get<uint32_t>());
		}
		uint32_t n = r.get<uint32_t>();
		std::vector<uint32_t> codes(n), params(n);
		std::vector<LocInfo> locs(n);
		for(uint32_t i = 0; i < n; ++i) {
			codes[i] = r.get<uint32_t>();
			locs[i] = read_loc(r);
			params[i] = r.get<uint32_t>();
		}
		std::vector<std::vector<uint32_t>> all;
		subsets(n, at_least ? 1 : std::max<uint32_t>(min, 1), at_least ? n : max, 2000, all);
		std::reverse(all.begin(), all.end());  // fewest cards first
		for(auto& c : all) {
			std::vector<uint32_t> ps = must;
			for(auto i : c)
				ps.push_back(params[i]);
			bool ok = at_least ? sum_check_atleast(ps, acc) : sum_check_exact(ps, 0, acc);
			if(!ok)
				continue;
			std::string label;
			for(auto i : c)
				label += (label.empty() ? "" : " + ") + db_.name(codes[i]);
			Option o;
			o.kind = OptKind::Select;
			o.code = c.empty() ? 0 : codes[c[0]];
			o.response = select_indices(c);
			o.label = label;
			for(auto i : c) {
				o.codes.push_back(codes[i]);
				o.locs.push_back(locs[i].loc);
				o.seqs.push_back(locs[i].seq);
			}
			d.options.push_back(std::move(o));
			if(d.options.size() >= kMaxCombos)
				break;
		}
		break;
	}
	case MSG_SORT_CARD:
	case MSG_SORT_CHAIN: {
		d.type = DecisionType::Sort;
		d.minor = true;
		add(OptKind::Choose, {0xff}, "default order");
		break;
	}
	case MSG_ANNOUNCE_RACE: {
		d.type = DecisionType::AnnounceRace;
		uint8_t count = r.get<uint8_t>();
		uint64_t avail = r.get<uint64_t>();
		std::vector<uint64_t> bits;
		for(int b = 0; b < 64; ++b)
			if(avail & (1ull << b))
				bits.push_back(1ull << b);
		std::vector<std::vector<uint32_t>> combos;
		subsets(static_cast<uint32_t>(bits.size()), count, count, kMaxCombos, combos);
		for(auto& c : combos) {
			uint64_t v = 0;
			for(auto i : c)
				v |= bits[i];
			std::vector<uint8_t> resp(8);
			std::memcpy(resp.data(), &v, 8);
			std::string label;
			for(auto i : c) {
				static const char* races[] = {"Warrior", "Spellcaster", "Fairy", "Fiend", "Zombie", "Machine", "Aqua",
				                              "Pyro", "Rock", "Winged Beast", "Plant", "Insect", "Thunder", "Dragon",
				                              "Beast", "Beast-Warrior", "Dinosaur", "Fish", "Sea Serpent", "Reptile",
				                              "Psychic", "Divine-Beast", "Creator God", "Wyrm", "Cyberse", "Illusion"};
				int b = 0;
				while(b < 64 && !(bits[i] & (1ull << b)))
					++b;
				label += (label.empty() ? "" : " + ") + std::string(b < 26 ? races[b] : "Type " + std::to_string(b));
			}
			add(OptKind::Choose, resp, "Declare " + label);
		}
		break;
	}
	case MSG_ANNOUNCE_ATTRIB: {
		d.type = DecisionType::AnnounceAttrib;
		uint8_t count = r.get<uint8_t>();
		uint32_t avail = r.get<uint32_t>();
		static const char* names[] = {"EARTH", "WATER", "FIRE", "WIND", "LIGHT", "DARK", "DIVINE"};
		std::vector<uint32_t> bits;
		for(int b = 0; b < 7; ++b)
			if(avail & (1u << b))
				bits.push_back(b);
		std::vector<std::vector<uint32_t>> combos;
		subsets(static_cast<uint32_t>(bits.size()), count, count, kMaxCombos, combos);
		for(auto& c : combos) {
			uint32_t v = 0;
			std::string label;
			for(auto i : c) {
				v |= 1u << bits[i];
				label += (label.empty() ? "" : "+") + std::string(names[bits[i]]);
			}
			add(OptKind::Choose, resp_i32(static_cast<int32_t>(v)), label);
		}
		break;
	}
	case MSG_ANNOUNCE_CARD: {
		d.type = DecisionType::AnnounceCard;
		// Declare from the cards actually in play in this matchup; the core rejects
		// illegal names with MSG_RETRY, which the search treats as a dead option.
		std::vector<uint32_t> pool = declare_pool_;
		std::sort(pool.begin(), pool.end());
		pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
		for(auto code : pool) {
			uint32_t id = db_.script_id(code);
			add(OptKind::Choose, resp_i32(static_cast<int32_t>(id)), "Declare " + db_.name(id), id);
		}
		break;
	}
	case MSG_ANNOUNCE_NUMBER: {
		d.type = DecisionType::AnnounceNumber;
		uint8_t n = r.get<uint8_t>();
		for(uint8_t i = 0; i < n; ++i)
			add(OptKind::Choose, resp_i32(i), "Declare " + std::to_string(r.get<uint64_t>()));
		break;
	}
	case MSG_ROCK_PAPER_SCISSORS: {
		d.type = DecisionType::RockPaperScissors;
		d.minor = true;
		add(OptKind::Choose, resp_i32(1), "rock-paper-scissors");
		break;
	}
	default:
		return;
	}
	decision_ = std::move(d);
}

Board Duel::board(bool include_decks) const {
	Board b;
	b.hints = player_hints_;
	b.turn = turn_;
	b.turn_player = turn_player_;
	b.phase = phase_;
	uint32_t flags = QUERY_CODE | QUERY_POSITION | QUERY_TYPE | QUERY_LEVEL | QUERY_RANK | QUERY_ATTACK |
	                 QUERY_DEFENSE | QUERY_OVERLAY_CARD | QUERY_STATUS | QUERY_LINK | QUERY_OWNER;
	auto query = [&](uint8_t con, uint32_t loc, std::vector<CardView>& out) {
		OCG_QueryInfo qi{};
		qi.flags = flags;
		qi.con = con;
		qi.loc = loc;
		uint32_t len = 0;
		auto* buf = static_cast<const uint8_t*>(OCG_DuelQueryLocation(duel_, &len, &qi));
		Reader r{buf, buf + len};
		r.get<uint32_t>();
		uint32_t seq = 0;
		while(r.p + 2 <= r.end) {
			uint16_t sz = r.get<uint16_t>();
			if(sz == 0) {
				++seq;
				continue;
			}
			CardView cv;
			cv.con = con;
			cv.owner = con;
			cv.loc = static_cast<uint8_t>(loc);
			cv.seq = seq++;
			bool done = false;
			while(!done && r.p + 4 <= r.end) {
				const uint8_t* start = r.p;
				uint32_t q = r.get<uint32_t>();
				switch(q) {
				case QUERY_CODE: cv.code = r.get<uint32_t>(); break;
				case QUERY_POSITION: cv.pos = r.get<uint32_t>(); break;
				case QUERY_TYPE: cv.type = r.get<uint32_t>(); break;
				case QUERY_LEVEL: cv.level = r.get<uint32_t>(); break;
				case QUERY_RANK: cv.rank = r.get<uint32_t>(); break;
				case QUERY_ATTACK: cv.atk = r.get<int32_t>(); break;
				case QUERY_DEFENSE: cv.def = r.get<int32_t>(); break;
				case QUERY_STATUS: cv.status = r.get<uint32_t>(); break;
				case QUERY_OWNER: cv.owner = r.get<uint8_t>(); break;
				case QUERY_IS_PUBLIC: cv.is_public = r.get<uint8_t>() != 0; break;
				case QUERY_LINK: cv.link = r.get<uint32_t>(); r.get<uint32_t>(); break;
				case QUERY_OVERLAY_CARD: {
					uint32_t n = r.get<uint32_t>();
					for(uint32_t i = 0; i < n; ++i)
						cv.overlay.push_back(r.get<uint32_t>());
					break;
				}
				case QUERY_END: done = true; break;
				default: break;
				}
				r.p = start + sz;
				if(!done && r.p + 2 <= r.end)
					sz = r.get<uint16_t>();
			}
			out.push_back(std::move(cv));
		}
	};
	// what is left of each card's once-per-turn effects this turn
	auto limits = [&](uint8_t p, uint8_t loc, std::vector<CardView>& v) {
		for(auto& cv : v) {
			const uint32_t x = OCG_DuelEffectsLeft(duel_, p, loc, cv.seq);
			cv.opt_total = static_cast<uint8_t>(std::min<uint32_t>(x & 0xffff, 255));
			cv.opt_left = static_cast<uint8_t>(std::min<uint32_t>(x >> 16, 255));
		}
	};
	for(uint8_t p = 0; p < 2; ++p) {
		query(p, LOCATION_HAND, b.hand[p]);
		query(p, LOCATION_MZONE, b.mzone[p]);
		query(p, LOCATION_SZONE, b.szone[p]);
		query(p, LOCATION_GRAVE, b.grave[p]);
		limits(p, LOCATION_HAND, b.hand[p]);
		limits(p, LOCATION_MZONE, b.mzone[p]);
		limits(p, LOCATION_SZONE, b.szone[p]);
		limits(p, LOCATION_GRAVE, b.grave[p]);
		query(p, LOCATION_REMOVED, b.banished[p]);
		query(p, LOCATION_EXTRA, b.extra[p]);
		if(include_decks)
			query(p, LOCATION_DECK, b.deck[p]);
	}
	// LP: OCG_DuelQueryField starts with options then LP per player.
	uint32_t len = 0;
	auto* f = static_cast<const uint8_t*>(OCG_DuelQueryField(duel_, &len));
	if(len >= 8) {
		Reader r{f, f + len};
		r.get<uint32_t>();
		b.lp[0] = static_cast<int32_t>(r.get<uint32_t>());
		// skip player 0's zones to reach player 1's LP
		for(int z = 0; z < 7 + 8; ++z) {
			if(r.get<uint8_t>()) {
				r.get<uint8_t>();
				r.get<uint32_t>();
			}
		}
		for(int i = 0; i < 6; ++i)
			r.get<uint32_t>();
		b.lp[1] = static_cast<int32_t>(r.get<uint32_t>());
	}
	return b;
}

}  // namespace ygoe
