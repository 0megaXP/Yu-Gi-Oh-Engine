#include "carddb.h"

#include <fstream>
#include <sstream>

#include "ocgapi_constants.h"

namespace ygoe {

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

static std::vector<std::string> split_tabs(const std::string& line) {
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
	std::ifstream cards(dir + "/cards.tsv");
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
		c.code = static_cast<uint32_t>(std::stoul(f[0]));
		c.alias = static_cast<uint32_t>(std::stoul(f[1]));
		c.setcode = static_cast<uint64_t>(std::stoll(f[2]));
		c.type = static_cast<uint32_t>(std::stoul(f[3]));
		c.atk = std::stoi(f[4]);
		c.def = std::stoi(f[5]);
		c.level = static_cast<uint32_t>(std::stoll(f[6]));
		c.race = static_cast<uint64_t>(std::stoll(f[7]));
		c.attribute = static_cast<uint32_t>(std::stoul(f[8]));
		c.category = static_cast<uint32_t>(std::stoll(f[9]));
		c.name = f[10];
		cards_[c.code] = std::move(c);
	}

	std::ifstream effects(dir + "/effects.tsv");
	while(std::getline(effects, line)) {
		auto f = split_tabs(line);
		if(f.size() < 8)
			continue;
		auto it = cards_.find(static_cast<uint32_t>(std::stoul(f[0])));
		if(it == cards_.end())
			continue;
		EffectInfo e;
		e.type = static_cast<uint32_t>(std::stoul(f[2]));
		e.code = static_cast<uint32_t>(std::stoul(f[3]));
		e.range = static_cast<uint32_t>(std::stoul(f[4]));
		e.category = static_cast<uint32_t>(std::stoul(f[5]));
		e.property = static_cast<uint32_t>(std::stoul(f[6]));
		e.count_limit = std::stoi(f[7]);
		it->second.effects.push_back(e);
	}

	std::ifstream strings(dir + "/strings.tsv");
	while(std::getline(strings, line)) {
		auto f = split_tabs(line);
		if(f.size() < 3)
			continue;
		uint64_t idx = std::stoull(f[1]);
		if(idx == 0)
			texts_[static_cast<uint32_t>(std::stoul(f[0]))] = f[2];
		else
			strings_[std::stoull(f[0]) * 16 + idx - 1] = f[2];
	}

	std::ifstream pmap(dir + "/passcode_map.tsv");
	while(std::getline(pmap, line)) {
		if(line.empty() || line[0] == '#')
			continue;
		auto f = split_tabs(line);
		if(f.size() < 2)
			continue;
		uint32_t official = static_cast<uint32_t>(std::stoul(f[0]));
		uint32_t sid = static_cast<uint32_t>(std::stoul(f[1]));
		passcode_map_[official] = sid;
		official_map_[sid] = official;
	}
	return true;
}

const CardInfo* CardDB::get(uint32_t code) const {
	auto it = cards_.find(code);
	return it == cards_.end() ? nullptr : &it->second;
}

uint32_t CardDB::script_id(uint32_t passcode) const {
	auto it = passcode_map_.find(passcode);
	return it == passcode_map_.end() ? passcode : it->second;
}

uint32_t CardDB::official_id(uint32_t sid) const {
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
