#include "column.h"

#include <cstdlib>
#include <vector>

#include "duel.h"
#include "ocgapi_constants.h"

namespace ygoe {

namespace {
uint8_t scope_of(const std::string& part) {
	uint8_t s = 0;
	if(part.find("this card's column") != std::string::npos || part.find("its column") != std::string::npos ||
	   part.find("same column") != std::string::npos)
		s |= kColSame;
	if(part.find("adjacent column") != std::string::npos)
		s |= kColAdjacent;
	if(part.find("2 columns away") != std::string::npos || part.find("two columns away") != std::string::npos)
		s |= kColTwoAway;
	if(part.find("another column") != std::string::npos || part.find("different column") != std::string::npos)
		s |= kColOther;
	return s;
}
}  // namespace

ColumnRule column_rule(const std::string& t) {
	ColumnRule r;
	r.center = t.find("center main monster zone") != std::string::npos;
	r.st_column = t.find("spell/trap effects in this column") != std::string::npos;
	// clause by clause, and "X, or Y" apart (Angelechy Castellan: a monster in the adjacent column,
	// or a Spell & Trap Zone card 2 columns away)
	size_t start = 0;
	while(start < t.size()) {
		size_t end = t.find_first_of(".;", start);
		if(end == std::string::npos)
			end = t.size();
		const std::string clause = t.substr(start, end - start);
		start = end + 1;
		if(clause.find("column") == std::string::npos || r.st_column && clause.find("spell/trap effects in this column") != std::string::npos)
			continue;
		size_t p = 0;
		while(p < clause.size()) {
			size_t q = clause.find(", or ", p);
			if(q == std::string::npos)
				q = clause.size();
			const std::string part = clause.substr(p, q - p);
			p = q + 5;
			const uint8_t s = scope_of(part);
			if(!s)
				continue;
			const bool st = part.find("spell & trap zone") != std::string::npos || part.find("spell/trap") != std::string::npos;
			const bool mon = part.find("monster") != std::string::npos;
			if(mon || !st)
				r.mon |= s;
			if(st || !mon)
				r.st |= s;
		}
	}
	return r;
}

int column_of(uint8_t pl, uint8_t loc, uint32_t seq) {
	int c;
	if(loc == LOCATION_MZONE)
		c = seq == 5 ? 1 : seq == 6 ? 3 : static_cast<int>(seq);
	else if(loc == LOCATION_SZONE && seq < 5)
		c = static_cast<int>(seq);
	else
		return -1;
	if(c < 0 || c > 4)
		return -1;
	return pl == 0 ? c : 4 - c;
}

int column_reach(const ColumnRule& r, int col, const Board& b, uint8_t opp) {
	if(col < 0)
		return 0;
	auto hits = [&](uint8_t scope, int c) {
		if(c < 0)
			return false;
		const int d = std::abs(c - col);
		return ((scope & kColSame) && d == 0) || ((scope & kColAdjacent) && d == 1) || ((scope & kColTwoAway) && d == 2) ||
		       ((scope & kColOther) && d != 0);
	};
	int n = 0;
	if(r.mon)
		for(const auto& c : b.mzone[opp])
			if(c.code && hits(r.mon, column_of(opp, LOCATION_MZONE, c.seq)))
				++n;
	if(r.st)
		for(const auto& c : b.szone[opp])
			if(c.code && hits(r.st, column_of(opp, LOCATION_SZONE, c.seq)))
				++n;
	return n;
}

}  // namespace ygoe
