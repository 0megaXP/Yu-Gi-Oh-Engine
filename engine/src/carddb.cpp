#include <cctype>
#include <mutex>
#include "carddb.h"

#include <fstream>
#include <sstream>

#include "ocgapi_constants.h"

namespace ygoe {

namespace {
// Lowercase, keeping only letters and digits, so small punctuation differences don't matter.
std::string lower_name(const std::string& s) {
	std::string out;
	for(unsigned char ch : s)
		if(std::isalnum(ch))
			out += static_cast<char>(std::tolower(ch));
	return out;
}
}  // namespace


bool CardInfo::is_extra_deck() const {
	return (type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK)) != 0;
}

bool CardInfo::has_setcode(uint16_t set) const {
	for(int i = 0; i < 4; ++i) {
		uint16_t sc = static_cast<uint16_t>(setcode >> (i * 16));
		if(sc && (sc & 0xfff) == (set & 0xfff) && (sc & set & 0xf000) == (set & 0xf000))
			return true;
	}
	return false;
}

static std::vector<std::string> split_tabs(const std::string& raw) {
	// Files are read in binary mode (the same bytes on every system); drop a Windows line end.
	const std::string line = !raw.empty() && raw.back() == '\r' ? raw.substr(0, raw.size() - 1) : raw;
	std::vector<std::string> out;
	size_t start = 0;
	while(true) {
		size_t tab = line.find('\t', start);
		out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
		if(tab == std::string::npos)
			break;
		start = tab + 1;
	}
	return out;
}

bool CardDB::load(const std::string& dir, std::string* error) {
	std::ifstream cards(dir + "/cards.tsv", std::ios::binary);
	if(!cards) {
		if(error)
			*error = "cannot open " + dir + "/cards.tsv (run tools/bootstrap.sh)";
		return false;
	}
	std::string line;
	while(std::getline(cards, line)) {
		auto f = split_tabs(line);
		if(f.size() < 11)
			continue;
		CardInfo c;
		c.code = static_cast<uint32_t>(std::stoull(f[0]));
		c.alias = static_cast<uint32_t>(std::stoull(f[1]));
		c.setcode = static_cast<uint64_t>(std::stoll(f[2]));
		c.type = static_cast<uint32_t>(std::stoull(f[3]));
		c.atk = std::stoi(f[4]);
		c.def = std::stoi(f[5]);
		c.level = static_cast<uint32_t>(std::stoll(f[6]));
		c.race = static_cast<uint64_t>(std::stoll(f[7]));
		c.attribute = static_cast<uint32_t>(std::stoull(f[8]));
		c.category = static_cast<uint32_t>(std::stoll(f[9]));
		c.name = f[10];
		cards_[c.code] = std::move(c);
	}

	std::ifstream effects(dir + "/effects.tsv", std::ios::binary);
	while(std::getline(effects, line)) {
		auto f = split_tabs(line);
		if(f.size() < 8)
			continue;
		auto it = cards_.find(static_cast<uint32_t>(std::stoull(f[0])));
		if(it == cards_.end())
			continue;
		EffectInfo e;
		e.type = static_cast<uint32_t>(std::stoull(f[2]));
		e.code = static_cast<uint32_t>(std::stoull(f[3]));
		e.range = static_cast<uint32_t>(std::stoull(f[4]));
		e.category = static_cast<uint32_t>(std::stoull(f[5]));
		e.property = static_cast<uint32_t>(std::stoull(f[6]));
		e.count_limit = std::stoi(f[7]);
		it->second.effects.push_back(e);
	}

	std::ifstream strings(dir + "/strings.tsv", std::ios::binary);
	while(std::getline(strings, line)) {
		auto f = split_tabs(line);
		if(f.size() < 3)
			continue;
		uint64_t idx = std::stoull(f[1]);
		if(idx == 0)
			texts_[static_cast<uint32_t>(std::stoull(f[0]))] = f[2];
		else
			strings_[std::stoull(f[0]) * 16 + idx - 1] = f[2];
	}

	std::ifstream pmap(dir + "/passcode_map.tsv", std::ios::binary);
	while(std::getline(pmap, line)) {
		if(line.empty() || line[0] == '#')
			continue;
		auto f = split_tabs(line);
		if(f.size() < 2)
			continue;
		uint32_t official = static_cast<uint32_t>(std::stoull(f[0]));
		uint32_t sid = static_cast<uint32_t>(std::stoull(f[1]));
		passcode_map_[official] = sid;
		official_map_[sid] = official;
	}
	// Name index: for names shared by several ids (official and pre-release copies), keep the lowest id.
	for(const auto& [id, c] : cards_) {
		// Only real cards (8-digit passcodes) and pre-release copies (1xx xxx xxx), not anime or custom cards.
		if(c.alias || c.name.empty() || id >= 110000000u)
			continue;
		std::string key = lower_name(c.name);
		auto it = by_name_.find(key);
		if(it == by_name_.end() || id < it->second)
			by_name_[key] = id;
	}
	// Cards whose effects care where cards are: "in this card's column", "adjacent", "in that
	// column" (Angelechy, Infinite Impermanence...). Where these are in a duel, zones are chosen.
	for(const auto& [id, t] : texts_) {
		std::string l = t;
		for(auto& ch : l)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		if(l.find("column") != std::string::npos)
			columns_.insert(id);
	}
	return true;
}

bool CardDB::learn_name(uint32_t passcode, const std::string& name) const {
	std::string want = lower_name(name);
	if(want.empty())
		return false;
	// A passcode the database lists as itself is trusted: name differences there are only
	// translations (OCG vs TCG names). Only mapped or unknown passcodes are corrected.
	if(get(passcode) && !passcode_map_.count(passcode))
		return false;
	uint32_t sid = script_id(passcode);
	if(auto c = get(sid); c && lower_name(c->name) == want)
		return false;
	auto it = by_name_.find(want);
	if(it == by_name_.end() || it->second == sid)
		return false;
	std::unique_lock lock(learn_mu_);
	learned_[passcode] = it->second;
	learned_official_[it->second] = passcode;
	has_learned_.store(true, std::memory_order_release);
	return true;
}

const CardInfo* CardDB::get(uint32_t code) const {
	auto it = cards_.find(code);
	return it == cards_.end() ? nullptr : &it->second;
}

uint32_t CardDB::script_id(uint32_t passcode) const {
	if(has_learned_.load(std::memory_order_acquire)) {
		std::shared_lock lock(learn_mu_);
		if(auto l = learned_.find(passcode); l != learned_.end())
			return l->second;
	}
	auto it = passcode_map_.find(passcode);
	return it == passcode_map_.end() ? passcode : it->second;
}

uint32_t CardDB::official_id(uint32_t sid) const {
	if(has_learned_.load(std::memory_order_acquire)) {
		std::shared_lock lock(learn_mu_);
		if(auto l = learned_official_.find(sid); l != learned_official_.end())
			return l->second;
	}
	auto it = official_map_.find(sid);
	return it == official_map_.end() ? sid : it->second;
}

std::string CardDB::name(uint32_t code) const {
	if(auto c = get(code))
		return c->name;
	return "#" + std::to_string(code);
}

std::string CardDB::text(uint32_t code) const {
	auto it = texts_.find(code);
	return it == texts_.end() ? std::string() : it->second;
}

std::string CardDB::description(uint64_t desc) const {
	if(desc == 0)
		return {};
	// aux.Stringid(code, n) == (code << 20) | n
	auto it = strings_.find((desc >> 20) * 16 + (desc & 0xfffff));
	if(it != strings_.end())
		return it->second;
	return {};
}

}  // namespace ygoe
