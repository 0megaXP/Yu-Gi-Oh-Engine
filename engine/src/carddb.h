// Card data, effect metadata and effect strings, loaded from the files tools/bootstrap.sh exports.
#pragma once
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ygoe {

struct EffectInfo {
	uint32_t type = 0;      // EFFECT_TYPE_* bits
	uint32_t code = 0;      // EVENT_* / EFFECT_* code
	uint32_t range = 0;     // LOCATION_* bits the effect works from
	uint32_t category = 0;  // CATEGORY_* bits
	uint32_t property = 0;  // EFFECT_FLAG_* bits
	int count_limit = 0;    // 0 = unlimited
};

struct CardInfo {
	uint32_t code = 0;
	uint32_t alias = 0;
	uint64_t setcode = 0;   // up to 4 packed 16-bit archetype codes
	uint32_t type = 0;
	int32_t atk = 0, def = 0;
	uint32_t level = 0;     // raw: low byte level, bytes 2/3 pendulum scales
	uint64_t race = 0;
	uint32_t attribute = 0;
	uint32_t category = 0;
	std::string name;
	std::vector<EffectInfo> effects;

	uint32_t lv() const { return level & 0xff; }
	uint32_t lscale() const { return (level >> 24) & 0xff; }
	uint32_t rscale() const { return (level >> 16) & 0xff; }
	bool is_extra_deck() const;
	bool has_setcode(uint16_t set) const;
};

class CardDB {
public:
	// data_dir must contain cards.tsv; effects.tsv, strings.tsv and passcode_map.tsv are optional.
	bool load(const std::string& data_dir, std::string* error = nullptr);

	const CardInfo* get(uint32_t code) const;
	// Official passcode -> id used by the scripts (identity when not remapped).
	uint32_t script_id(uint32_t passcode) const;
	// Inverse of script_id: the official passcode (what YGOPRODeck and card images use).
	uint32_t official_id(uint32_t script_id) const;
	// The app knows each passcode's real name. If the card this passcode resolves to has another
	// name, use the card with that name instead (fixes wrong or missing passcode_map rows).
	// Returns true when it changed what the passcode means.
	bool learn_name(uint32_t passcode, const std::string& name) const;
	std::string name(uint32_t code) const;
	// Text for an effect description value (aux.Stringid(code, n) == (code << 20) | n).
	std::string description(uint64_t desc) const;
	std::string text(uint32_t code) const;  // full card text
	const std::unordered_map<uint32_t, CardInfo>& all() const { return cards_; }

private:
	std::unordered_map<uint32_t, CardInfo> cards_;
	std::unordered_map<uint32_t, uint32_t> passcode_map_, official_map_;
	std::unordered_map<std::string, uint32_t> by_name_;  // lowercase name -> id
	// Corrections learned from names at run time (see learn_name).
	mutable std::unordered_map<uint32_t, uint32_t> learned_, learned_official_;
	mutable std::shared_mutex learn_mu_;
	mutable std::atomic<bool> has_learned_{false};
	std::unordered_map<uint64_t, std::string> strings_;  // key: code*16+idx
	std::unordered_map<uint32_t, std::string> texts_;
};

}  // namespace ygoe
