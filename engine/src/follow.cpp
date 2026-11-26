#include "follow.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>

#include "carddb.h"
#include "duel.h"
#include "evaluate.h"
#include "json.h"
#include "ocgapi_constants.h"

namespace ygoe {

namespace {

// ---------------------------------------------------------------- events

// What both streams are compared on: the recorded one (classic messages) and the rules core's.
enum Kind : uint8_t { TURN, PHASE, DRAW, MOVE, SUMMON, SPSUMMON, FLIP, CHAIN, ATTACK, LP, POS, WIN };

struct Ev {
	uint8_t k = 0;
	uint8_t p = 0;        // the player it is about (MOVE: the controller it leaves)
	uint8_t fl = 0;       // MOVE: location it leaves; CHAIN: where the card is
	uint8_t tp = 0, tl = 0;  // MOVE: controller and location it goes to; ATTACK: the target's
	uint32_t code = 0;    // 0: not known (a hidden card)
	uint32_t ts = 0;      // MOVE: zone it goes to; ATTACK, POS: the card's zone
	uint32_t tpos = 0;    // MOVE, POS: position after
	int32_t v = 0;        // TURN: -; PHASE: phase; DRAW: count; LP: Life Points after
	uint64_t desc = 0;    // CHAIN: the effect (its description id)
	uint32_t hint = 0;    // recorded stream: the code known in hindsight (the replay showed it later)
	uint8_t need = 0;     // recorded stream: a hidden card later Set as a monster (1) or a Spell/Trap (2)
	int inst = -1;        // recorded stream: the card instance
	bool top = false;     // recorded stream: a card taken from the top of a Deck
};

// Events either stream may have or not: phases, LP, positions, and a card moving within its own
// Deck (YGO Omega's scripts and the rules core's reorder excavated cards differently, e.g. Pot
// of Prosperity putting the rest on the bottom; nobody sees the difference).
bool soft(const Ev& e) {
	return e.k == PHASE || e.k == LP || e.k == POS ||
	       (e.k == MOVE && e.fl == LOCATION_DECK && e.tl == LOCATION_DECK && e.p == e.tp);
}

uint8_t norm_loc(uint32_t loc) { return (loc & LOCATION_OVERLAY) ? LOCATION_OVERLAY : static_cast<uint8_t>(loc & 0x7f); }
bool on_field(uint8_t loc) { return loc == LOCATION_MZONE || loc == LOCATION_SZONE; }

struct Bytes {
	const uint8_t* p;
	const uint8_t* end;
	template <class T> T get() {
		T v{};
		if(p + sizeof(T) <= end)
			std::memcpy(&v, p, sizeof(T));
		p += sizeof(T);
		return v;
	}
	bool ok() const { return p <= end; }
};

// ---------------------------------------------------------------- the recorded stream

// Reads a classic-format replay into events, following each card so that hidden cards get
// their codes in hindsight (the face-down card drawn on turn 3 is the Ash Blossom played later).
class Recorded {
public:
	int start_extra[2] = {0, 0};  // Extra Deck sizes at the start (MSG_START)
	std::vector<Ev> ev;
	std::vector<int> tops[2];  // instances taken from each Deck's top, in order
	int first = -1;            // who took the first turn
	int winner = -1;
	std::vector<uint32_t> code;  // per instance
	std::vector<uint8_t> need;   // per instance: 1 a monster, 2 a Spell/Trap (Set face-down, never shown)
	uint8_t need_of(int inst) const { return inst >= 0 && static_cast<size_t>(inst) < need.size() ? need[static_cast<size_t>(inst)] : 0; }

	void read(const std::vector<std::vector<uint8_t>>& msgs) {
		for(auto& s : side_)
			std::fill(std::begin(s.mz), std::end(s.mz), -1), std::fill(std::begin(s.sz), std::end(s.sz), -1);
		for(const auto& m : msgs)
			if(!m.empty())
				one(m);
		for(auto& e : ev) {
			e.hint = e.code ? e.code : e.inst >= 0 ? code[static_cast<size_t>(e.inst)] : 0;
			e.need = need_of(e.inst);
		}
	}
	uint32_t code_of(int inst) const { return inst >= 0 ? code[static_cast<size_t>(inst)] : 0; }

private:
	struct Side {
		int lp = 8000;
		int deck = 0;
		std::vector<int> hand, grave, removed, extra;
		int mz[7];
		int sz[8];
		std::vector<int> ov[7];
	} side_[2];

	int fresh(uint32_t c) {
		code.push_back(c);
		need.push_back(0);
		return static_cast<int>(code.size()) - 1;
	}
	void reveal(int i, uint32_t c) {
		if(i >= 0 && c && !code[static_cast<size_t>(i)])
			code[static_cast<size_t>(i)] = c;
	}
	// A card leaving a hand: the one with that code, else the hidden card held longest.
	int from_hand(Side& s, uint32_t c) {
		if(s.hand.empty())
			return fresh(c);
		size_t at = s.hand.size();
		if(c)
			for(size_t i = 0; i < s.hand.size() && at == s.hand.size(); ++i)
				if(code[static_cast<size_t>(s.hand[i])] == c)
					at = i;
		for(size_t i = 0; i < s.hand.size() && at == s.hand.size(); ++i)
			if(!code[static_cast<size_t>(s.hand[i])])
				at = i;
		if(at == s.hand.size())
			at = 0;
		int x = s.hand[at];
		s.hand.erase(s.hand.begin() + static_cast<std::ptrdiff_t>(at));
		reveal(x, c);
		return x;
	}
	int from_list(std::vector<int>& l, uint32_t c, uint32_t seq) {
		if(l.empty())
			return fresh(c);
		size_t at = l.size();
		if(c)
			for(size_t i = l.size(); i-- > 0 && at == l.size();)
				if(code[static_cast<size_t>(l[i])] == c)
					at = i;
		if(at == l.size())
			at = seq < l.size() ? seq : l.size() - 1;
		int x = l[at];
		l.erase(l.begin() + static_cast<std::ptrdiff_t>(at));
		reveal(x, c);
		return x;
	}
	int take(uint8_t p, uint8_t loc, uint32_t seq, uint32_t c, bool& top) {
		Side& s = side_[p & 1];
		top = false;
		if(loc & LOCATION_OVERLAY) {
			auto& o = s.ov[seq < 7 ? seq : 0];
			return from_list(o, c, 0);
		}
		switch(loc) {
		case LOCATION_MZONE:
		case LOCATION_SZONE: {
			int* z = loc == LOCATION_MZONE ? (seq < 7 ? &s.mz[seq] : nullptr) : (seq < 8 ? &s.sz[seq] : nullptr);
			if(!z)
				return fresh(c);
			int x = *z >= 0 ? *z : fresh(c);
			*z = -1;
			reveal(x, c);
			return x;
		}
		case LOCATION_HAND:
			return from_hand(s, c);
		case LOCATION_DECK: {
			top = static_cast<int>(seq) >= s.deck - 1;
			if(s.deck > 0)
				--s.deck;
			int x = fresh(c);
			if(top)
				tops[p & 1].push_back(x);
			return x;
		}
		case LOCATION_GRAVE: return from_list(s.grave, c, seq);
		case LOCATION_REMOVED: return from_list(s.removed, c, seq);
		case LOCATION_EXTRA: return from_list(s.extra, c, seq);
		default: return fresh(c);
		}
	}
	void put(uint8_t p, uint8_t loc, uint32_t seq, int x) {
		Side& s = side_[p & 1];
		if(loc & LOCATION_OVERLAY) {
			s.ov[seq < 7 ? seq : 0].push_back(x);
			return;
		}
		switch(loc) {
		case LOCATION_MZONE: if(seq < 7) s.mz[seq] = x; break;
		case LOCATION_SZONE: if(seq < 8) s.sz[seq] = x; break;
		case LOCATION_HAND: s.hand.push_back(x); break;
		case LOCATION_DECK: ++s.deck; break;  // shuffled in: who it is is lost
		case LOCATION_GRAVE: s.grave.push_back(x); break;
		case LOCATION_REMOVED: s.removed.push_back(x); break;
		case LOCATION_EXTRA: s.extra.push_back(x); break;
		default: break;
		}
	}
	int at_zone(uint8_t p, uint8_t loc, uint32_t seq) {
		Side& s = side_[p & 1];
		if(loc == LOCATION_MZONE && seq < 7)
			return s.mz[seq];
		if(loc == LOCATION_SZONE && seq < 8)
			return s.sz[seq];
		return -1;
	}
	void lp(uint8_t p, int v) {
		Side& s = side_[p & 1];
		if(v == s.lp)
			return;
		s.lp = v;
		Ev e;
		e.k = LP;
		e.p = p & 1;
		e.v = v;
		ev.push_back(e);
	}

	void one(const std::vector<uint8_t>& m) {
		Bytes r{m.data() + 1, m.data() + m.size()};
		auto loc4 = [&](uint8_t& con, uint8_t& loc, uint32_t& seq, uint32_t& pos) {
			con = r.get<uint8_t>() & 1;
			loc = r.get<uint8_t>();
			seq = r.get<uint8_t>();
			pos = r.get<uint8_t>();
		};
		switch(m[0]) {
		case MSG_START: {
			// [type u8][lp0 u32][lp1 u32][deck0 u16][extra0 u16][deck1 u16][extra1 u16]
			r.get<uint8_t>();
			int lp0 = static_cast<int>(r.get<uint32_t>()), lp1 = static_cast<int>(r.get<uint32_t>());
			if(lp0 > 0 && lp0 < 1000000)
				side_[0].lp = lp0;
			if(lp1 > 0 && lp1 < 1000000)
				side_[1].lp = lp1;
			for(int p = 0; p < 2; ++p) {
				side_[p].deck = r.get<uint16_t>();
				int ex = r.get<uint16_t>();
				start_extra[p] = ex;
				for(int i = 0; i < ex; ++i)
					side_[p].extra.push_back(fresh(0));
			}
			break;
		}
		case MSG_NEW_TURN: {
			Ev e;
			e.k = TURN;
			e.p = r.get<uint8_t>() & 1;
			if(first < 0)
				first = e.p;
			ev.push_back(e);
			break;
		}
		case MSG_NEW_PHASE: {
			Ev e;
			e.k = PHASE;
			e.v = r.get<uint16_t>();
			ev.push_back(e);
			break;
		}
		case MSG_DRAW: {
			uint8_t p = r.get<uint8_t>() & 1;
			uint8_t n = r.get<uint8_t>();
			for(uint8_t i = 0; i < n; ++i) {
				uint32_t c = r.get<uint32_t>() & 0x7fffffff;
				int x = fresh(c);
				if(side_[p].deck > 0)
					--side_[p].deck;
				side_[p].hand.push_back(x);
				tops[p].push_back(x);
			}
			Ev e;
			e.k = DRAW;
			e.p = p;
			e.v = n;
			e.top = true;
			ev.push_back(e);
			break;
		}
		case MSG_MOVE: {
			uint32_t c = r.get<uint32_t>() & 0x7fffffff;
			uint8_t fc, fl, tc, tl;
			uint32_t fs, fp, ts, tpos;
			loc4(fc, fl, fs, fp);
			loc4(tc, tl, ts, tpos);
			if(!r.ok())
				break;
			bool top = false;
			int x = (fl == 0) ? fresh(c) : take(fc, fl, fs, c, top);
			if(c)
				reveal(x, c);
			if(tl)
				put(tc, tl, ts, x);
			if((tpos & POS_FACEDOWN) && norm_loc(fl) == LOCATION_HAND)
				need[static_cast<size_t>(x)] = norm_loc(tl) == LOCATION_MZONE ? 1 : norm_loc(tl) == LOCATION_SZONE ? 2 : need[static_cast<size_t>(x)];
			Ev e;
			e.k = MOVE;
			e.code = c;
			e.p = fc;
			e.fl = norm_loc(fl);
			e.tp = tc;
			e.tl = norm_loc(tl);
			e.ts = ts;
			e.tpos = tpos;
			e.inst = x;
			e.top = top;
			ev.push_back(e);
			break;
		}
		case MSG_SWAP: {
			uint32_t c1 = r.get<uint32_t>() & 0x7fffffff;
			uint8_t c1c, c1l, c2c, c2l;
			uint32_t c1s, c1p, c2s, c2p;
			loc4(c1c, c1l, c1s, c1p);
			uint32_t c2 = r.get<uint32_t>() & 0x7fffffff;
			loc4(c2c, c2l, c2s, c2p);
			int a = at_zone(c1c, c1l, c1s), b = at_zone(c2c, c2l, c2s);
			reveal(a, c1);
			reveal(b, c2);
			if(c1l == LOCATION_MZONE && c2l == LOCATION_MZONE && c1s < 7 && c2s < 7) {
				side_[c1c].mz[c1s] = b;
				side_[c2c].mz[c2s] = a;
			}
			for(int k = 0; k < 2; ++k) {
				Ev e;
				e.k = MOVE;
				e.code = k ? c2 : c1;
				e.p = k ? c2c : c1c;
				e.fl = k ? c2l : c1l;
				e.tp = k ? c1c : c2c;
				e.tl = k ? c1l : c2l;
				e.ts = k ? c1s : c2s;
				e.tpos = k ? c2p : c1p;
				e.inst = k ? b : a;
				ev.push_back(e);
			}
			break;
		}
		case MSG_POS_CHANGE: {
			uint32_t c = r.get<uint32_t>() & 0x7fffffff;
			uint8_t con = r.get<uint8_t>() & 1, loc = r.get<uint8_t>(), seq = r.get<uint8_t>();
			r.get<uint8_t>();
			uint8_t cur = r.get<uint8_t>();
			int x = at_zone(con, loc, seq);
			reveal(x, c);
			Ev e;
			e.k = POS;
			e.p = con;
			e.ts = seq;
			e.tpos = cur;
			e.code = c;
			e.inst = x;
			ev.push_back(e);
			break;
		}
		case MSG_SUMMONING:
		case MSG_SPSUMMONING:
		case MSG_FLIPSUMMONING: {
			uint32_t c = r.get<uint32_t>() & 0x7fffffff;
			uint8_t con, loc;
			uint32_t seq, pos;
			loc4(con, loc, seq, pos);
			int x = at_zone(con, loc, seq);
			reveal(x, c);
			Ev e;
			e.k = m[0] == MSG_SUMMONING ? SUMMON : m[0] == MSG_SPSUMMONING ? SPSUMMON : FLIP;
			e.p = con;
			e.code = c;
			e.inst = x;
			ev.push_back(e);
			break;
		}
		case MSG_CHAINING: {
			uint32_t c = r.get<uint32_t>() & 0x7fffffff;
			uint8_t con, loc;
			uint32_t seq, pos;
			loc4(con, loc, seq, pos);
			r.get<uint8_t>();
			r.get<uint8_t>();
			r.get<uint8_t>();
			const uint32_t desc = r.get<uint32_t>();
			int x = -1;
			if(on_field(loc)) {
				x = at_zone(con, loc, seq);
				reveal(x, c);
			} else if(loc == LOCATION_HAND && c) {
				// the card is still in the hand: name the hidden card held longest, unless one has that code
				Side& s = side_[con];
				bool known = false;
				for(int h : s.hand)
					known = known || code[static_cast<size_t>(h)] == c;
				if(!known)
					for(int h : s.hand)
						if(!code[static_cast<size_t>(h)]) {
							code[static_cast<size_t>(h)] = c;
							break;
						}
			}
			Ev e;
			e.k = CHAIN;
			e.p = con;
			e.fl = norm_loc(loc);
			e.code = c;
			e.inst = x;
			e.desc = r.ok() ? desc : 0;
			ev.push_back(e);
			break;
		}
		case MSG_CONFIRM_CARDS: {
			r.get<uint8_t>();
			uint8_t n = r.get<uint8_t>();
			for(uint8_t i = 0; i < n && r.ok(); ++i) {
				uint32_t c = r.get<uint32_t>() & 0x7fffffff;
				uint8_t con = r.get<uint8_t>() & 1, loc = r.get<uint8_t>(), seq = r.get<uint8_t>();
				if(on_field(loc)) {
					reveal(at_zone(con, loc, seq), c);
				} else if(loc == LOCATION_HAND && c) {
					// shown after it was added (a search): the hidden card added last
					Side& s = side_[con];
					bool known = false;
					for(int h : s.hand)
						known = known || code[static_cast<size_t>(h)] == c;
					if(!known)
						for(size_t k = s.hand.size(); k-- > 0;)
							if(!code[static_cast<size_t>(s.hand[k])]) {
								code[static_cast<size_t>(s.hand[k])] = c;
								break;
							}
				}
			}
			break;
		}
		case MSG_ATTACK: {
			uint8_t ac, al, tc, tl;
			uint32_t as, ap, ts, tpos;
			loc4(ac, al, as, ap);
			loc4(tc, tl, ts, tpos);
			Ev e;
			e.k = ATTACK;
			e.p = ac;
			e.ts = as;
			e.tp = tc;
			e.tl = norm_loc(tl);
			ev.push_back(e);
			break;
		}
		case MSG_DAMAGE:
		case MSG_PAY_LPCOST: {
			uint8_t p = r.get<uint8_t>() & 1;
			int v = static_cast<int>(r.get<uint32_t>());
			lp(p, std::max(0, side_[p].lp - v));
			break;
		}
		case MSG_RECOVER: {
			uint8_t p = r.get<uint8_t>() & 1;
			int v = static_cast<int>(r.get<uint32_t>());
			lp(p, side_[p].lp + v);
			break;
		}
		case MSG_LPUPDATE: {
			uint8_t p = r.get<uint8_t>() & 1;
			lp(p, static_cast<int>(r.get<uint32_t>()));
			break;
		}
		case MSG_WIN: {
			uint8_t p = r.get<uint8_t>();
			Ev e;
			e.k = WIN;
			e.p = p & 1;
			e.v = p;
			if(p <= 1)
				winner = p;
			ev.push_back(e);
			break;
		}
		default:
			break;
		}
	}
};

// ---------------------------------------------------------------- the rules core's stream

// Turns the core's messages (EDOPro format) into the same events.
class Live {
public:
	int lp[2] = {8000, 8000};
	std::vector<Ev> out;
	bool to_deck[2] = {false, false};  // a card went into a Deck (the draws after it need fixing)

	void read(const uint8_t* data, uint32_t len) {
		Bytes all{data, data + len};
		while(all.p + 4 <= all.end) {
			uint32_t size = all.get<uint32_t>();
			const uint8_t* msg = all.p;
			all.p += size;
			if(size == 0 || all.p > all.end)
				break;
			one(msg, msg + size);
		}
	}

private:
	struct Loc {
		uint8_t con = 0, loc = 0;
		uint32_t seq = 0, pos = 0;
	};
	static Loc loc10(Bytes& r) {
		Loc l;
		l.con = r.get<uint8_t>() & 1;
		l.loc = r.get<uint8_t>();
		l.seq = r.get<uint32_t>();
		l.pos = r.get<uint32_t>();
		return l;
	}
	void set_lp(uint8_t p, int v) {
		if(v == lp[p])
			return;
		lp[p] = v;
		Ev e;
		e.k = LP;
		e.p = p;
		e.v = v;
		out.push_back(e);
	}
	void one(const uint8_t* msg, const uint8_t* end) {
		Bytes r{msg + 1, end};
		switch(msg[0]) {
		case MSG_NEW_TURN: {
			Ev e;
			e.k = TURN;
			e.p = r.get<uint8_t>() & 1;
			out.push_back(e);
			break;
		}
		case MSG_NEW_PHASE: {
			Ev e;
			e.k = PHASE;
			e.v = r.get<uint16_t>();
			out.push_back(e);
			break;
		}
		case MSG_DRAW: {
			Ev e;
			e.k = DRAW;
			e.p = r.get<uint8_t>() & 1;
			e.v = static_cast<int32_t>(r.get<uint32_t>());
			e.code = r.get<uint32_t>();  // the first card drawn (for debugging; draws are compared by count)
			out.push_back(e);
			break;
		}
		case MSG_MOVE: {
			uint32_t c = r.get<uint32_t>();
			Loc f = loc10(r), t = loc10(r);
			if(norm_loc(t.loc) == LOCATION_DECK)
				to_deck[t.con] = true;
			Ev e;
			e.k = MOVE;
			e.code = c;
			e.p = f.con;
			e.fl = norm_loc(f.loc);
			e.tp = t.con;
			e.tl = norm_loc(t.loc);
			e.ts = t.seq;
			e.tpos = t.pos;
			out.push_back(e);
			break;
		}
		case MSG_SWAP: {
			uint32_t c1 = r.get<uint32_t>();
			Loc a = loc10(r);
			uint32_t c2 = r.get<uint32_t>();
			Loc b = loc10(r);
			for(int k = 0; k < 2; ++k) {
				Ev e;
				e.k = MOVE;
				e.code = k ? c2 : c1;
				e.p = k ? b.con : a.con;
				e.fl = norm_loc(k ? b.loc : a.loc);
				e.tp = k ? a.con : b.con;
				e.tl = norm_loc(k ? a.loc : b.loc);
				e.ts = k ? a.seq : b.seq;
				e.tpos = k ? b.pos : a.pos;
				out.push_back(e);
			}
			break;
		}
		case MSG_POS_CHANGE: {
			Ev e;
			e.k = POS;
			e.code = r.get<uint32_t>();
			e.p = r.get<uint8_t>() & 1;
			r.get<uint8_t>();
			e.ts = r.get<uint8_t>();
			r.get<uint8_t>();
			e.tpos = r.get<uint8_t>();
			out.push_back(e);
			break;
		}
		case MSG_SUMMONING:
		case MSG_SPSUMMONING:
		case MSG_FLIPSUMMONING: {
			Ev e;
			e.k = msg[0] == MSG_SUMMONING ? SUMMON : msg[0] == MSG_SPSUMMONING ? SPSUMMON : FLIP;
			e.code = r.get<uint32_t>();
			e.p = loc10(r).con;
			out.push_back(e);
			break;
		}
		case MSG_CHAINING: {
			Ev e;
			e.k = CHAIN;
			e.code = r.get<uint32_t>();
			Loc l = loc10(r);
			e.p = l.con;
			e.fl = norm_loc(l.loc);
			r.get<uint8_t>();
			r.get<uint8_t>();
			r.get<uint32_t>();
			e.desc = r.get<uint64_t>();
			out.push_back(e);
			break;
		}
		case MSG_ATTACK: {
			Loc a = loc10(r), t = loc10(r);
			Ev e;
			e.k = ATTACK;
			e.p = a.con;
			e.ts = a.seq;
			e.tp = t.con;
			e.tl = norm_loc(t.loc);
			out.push_back(e);
			break;
		}
		case MSG_DAMAGE:
		case MSG_PAY_LPCOST: {
			uint8_t p = r.get<uint8_t>() & 1;
			set_lp(p, std::max(0, lp[p] - static_cast<int>(r.get<uint32_t>())));
			break;
		}
		case MSG_RECOVER: {
			uint8_t p = r.get<uint8_t>() & 1;
			set_lp(p, lp[p] + static_cast<int>(r.get<uint32_t>()));
			break;
		}
		case MSG_LPUPDATE: {
			uint8_t p = r.get<uint8_t>() & 1;
			set_lp(p, static_cast<int>(r.get<uint32_t>()));
			break;
		}
		case MSG_WIN: {
			uint8_t p = r.get<uint8_t>();
			Ev e;
			e.k = WIN;
			e.p = p & 1;
			e.v = p;
			out.push_back(e);
			break;
		}
		default:
			break;
		}
	}
};

// ---------------------------------------------------------------- following

constexpr uint32_t kFiller = 46986414;  // Dark Magician: stands for an opponent's card nobody saw
// Trap Hole: stands for one nobody saw that is Set in the Spell & Trap Zone (a monster stand-in
// can't be: the game stopped there)
constexpr uint32_t kFillerST = 4206964;
bool is_filler(uint32_t c) { return c == kFiller || c == kFillerST; }
// Flame Swordsman (a Fusion with no effect): stands for an Extra Deck card nobody saw, so costs
// like Pot of Prosperity's (banish face-down cards from the Extra Deck) can be paid
constexpr uint32_t kExtraFiller = 45231177;

struct Step {
	bool deck_top = false;
	uint8_t player = 0;
	std::vector<uint32_t> codes;       // deck_top: the cards put on top
	std::vector<uint8_t> response;     // otherwise: the answer to the decision
};

struct Pos {
	std::vector<float> f[2];
	double raw[2];
	uint32_t turn;
	size_t steps;  // history length when taken (dropped when the search backs up before it)
	bool action;   // at a Main or Battle Phase decision (kept first when thinning a turn)
	// At the turn player's first Main Phase 1 decision of a turn (turn 2 on): the position as a
	// start for self-play (see start_json), "" otherwise.
	std::string start;
	// The board the player going first left (turn 2, Main Phase 1), for the combo lines going
	// second (reference.cpp load_end_boards): written even when cards nobody saw are in a deck.
	std::string board;
	uint8_t turn_player = 0;
};

std::string ev_str(const Ev& e) {
	static const char* names[] = {"TURN", "PHASE", "DRAW", "MOVE", "SUMMON", "SPSUMMON", "FLIP", "CHAIN", "ATTACK", "LP", "POS", "WIN"};
	std::ostringstream o;
	o << names[e.k] << " p" << int(e.p) << " c" << e.code;
	if(e.k == MOVE)
		o << " " << int(e.fl) << "->" << int(e.tp) << ":" << int(e.tl) << "/" << e.ts << " pos" << e.tpos;
	if(e.k == CHAIN)
		o << " from " << int(e.fl);
	if(e.k == ATTACK || e.k == POS)
		o << " s" << e.ts << " t" << int(e.tp) << ":" << int(e.tl);
	if(e.k == PHASE || e.k == DRAW || e.k == LP)
		o << " v" << e.v;
	return o.str();
}
const bool kDebug = std::getenv("YGOE_FOLLOW_DEBUG") != nullptr;

class Follower {
public:
	Follower(const CardDB& db, ScriptCache& scripts, const FollowInput& in) : db_(db), scripts_(scripts), in_(in) {}

	FollowResult run();

private:
	const CardDB& db_;
	ScriptCache& scripts_;
	const FollowInput& in_;
	std::vector<Ev> rec_;           // recorded events, players as the engine numbers them
	std::vector<uint32_t> tops_[2];  // cards to come off each Deck's top, in order (engine players)
	std::vector<size_t> top_count_[2];  // per recorded event index: top cards taken before it
	DuelSetup setup_;
	std::unique_ptr<Duel> duel_;
	Live live_;
	std::vector<Step> hist_;
	std::vector<Pos> pos_;
	std::unique_ptr<Evaluator> eval_;
	Deck decks_[2];  // engine players, script ids (for the evaluator)

	bool same_code(uint32_t rec, uint32_t live) const {
		if(!rec || !live)
			return true;
		uint32_t a = db_.script_id(rec);
		if(a == live || rec == live)
			return true;
		const CardInfo* l = db_.get(live);
		const CardInfo* r = db_.get(a);
		if((l && l->alias && (l->alias == a || l->alias == rec)) || (r && r->alias && r->alias == live))
			return true;
		// two tokens: YGO Omega's scripts and the core's may make the same token under different
		// ids (Girsu's World Legacy Token: 69811711 there, 46647145 here); a token is a token
		return l && r && (l->type & TYPE_TOKEN) && (r->type & TYPE_TOKEN);
	}
	bool eq(const Ev& r, const Ev& l) const {
		if(r.k != l.k)
			return false;
		switch(r.k) {
		case TURN: return r.p == l.p;
		case PHASE: return r.v == l.v;
		case DRAW: return r.p == l.p && r.v == l.v;
		case MOVE:
			if(!same_code(r.code, l.code) || r.p != l.p || r.fl != l.fl || r.tp != l.tp || r.tl != l.tl)
				return false;
			if(on_field(r.tl))
				return r.ts == l.ts && ((r.tpos & POS_FACEDOWN) != 0) == ((l.tpos & POS_FACEDOWN) != 0) &&
				       ((r.tpos & POS_DEFENSE) != 0) == ((l.tpos & POS_DEFENSE) != 0);
			return true;
		case SUMMON:
		case SPSUMMON:
		case FLIP: return r.p == l.p && same_code(r.code, l.code);
		case CHAIN: return r.p == l.p && r.fl == l.fl && same_code(r.code, l.code);
		case ATTACK: return r.p == l.p && r.ts == l.ts && r.tl == l.tl && (!r.tl || r.tp == l.tp);
		case LP: return r.p == l.p && r.v == l.v;
		case POS:
			return r.p == l.p && r.ts == l.ts && ((r.tpos & POS_FACEDOWN) != 0) == ((l.tpos & POS_FACEDOWN) != 0) &&
			       ((r.tpos & POS_DEFENSE) != 0) == ((l.tpos & POS_DEFENSE) != 0);
		case WIN: return r.p == l.p;
		}
		return false;
	}
	// The recorded events the core's [batch] stands for, from [cur]: the new position, or -1.
	long match(const std::vector<Ev>& batch, size_t cur) const {
		size_t i = cur;
		for(const Ev& e : batch) {
			if(soft(e)) {
				size_t j = i;
				while(j < rec_.size() && soft(rec_[j]) && !eq(rec_[j], e))
					++j;
				if(j < rec_.size() && soft(rec_[j]) && eq(rec_[j], e))
					i = j + 1;
				continue;
			}
			size_t j = i;
			while(j < rec_.size() && soft(rec_[j]))
				++j;
			if(j >= rec_.size())
				return static_cast<long>(rec_.size());  // the replay ends here (the core goes on)
			if(!eq(rec_[j], e))
				return -1;
			i = j + 1;
		}
		return static_cast<long>(i);
	}
	// The next recorded events that aren't soft, from [cur].
	std::vector<const Ev*> ahead(size_t cur, size_t n) const {
		std::vector<const Ev*> out;
		for(size_t j = cur; j < rec_.size() && out.size() < n; ++j)
			if(!soft(rec_[j]))
				out.push_back(&rec_[j]);
		return out;
	}
	std::vector<int> rank(const Decision& d, size_t cur) const;
	// The player shows this card later in the replay (activates, summons or moves it from a hand or Deck).
	bool used_later(uint32_t code, uint8_t player, size_t cur) const {
		for(size_t j = cur; j < rec_.size() && j < cur + 400; ++j) {
			const Ev& e = rec_[j];
			const uint32_t c = e.code ? e.code : e.hint;
			if(!c || !same_code(c, code))
				continue;
			if((e.k == CHAIN && e.p == player && e.fl == LOCATION_HAND) || (e.k == SUMMON && e.p == player) ||
			   (e.k == MOVE && e.p == player && (e.fl == LOCATION_HAND || e.fl == LOCATION_DECK)))
				return true;
		}
		return false;
	}
	bool rebuild(size_t steps);
	bool apply(const Step& s);
	void fix_decks(size_t cur);
	void take_position(size_t cur);
	std::string start_json(const Board& b, bool board_only = false) const;
};

std::vector<int> Follower::rank(const Decision& d, size_t cur) const {
	auto next = ahead(cur, 14);
	// a recorded card's code, or the one known in hindsight
	auto kc = [](const Ev* e) { return e->code ? e->code : e->hint; };
	const Ev* f = next.empty() ? nullptr : next.front();
	// the next soft event too (a phase change, a position change)
	const Ev* sf = nullptr;
	for(size_t j = cur; j < rec_.size() && !sf; ++j)
		if(rec_[j].k == PHASE || rec_[j].k == POS)
			sf = &rec_[j];
	auto in_next = [&](uint32_t code, size_t within) {
		for(size_t i = 0; i < next.size() && i < within; ++i)
			if(kc(next[i]) && same_code(kc(next[i]), code))
				return true;
		return false;
	};
	std::vector<std::pair<double, int>> scored;
	for(size_t i = 0; i < d.options.size(); ++i) {
		const Option& o = d.options[i];
		double s = 0;
		switch(o.kind) {
		case OptKind::Summon:
			if(f && f->k == SUMMON && same_code(kc(f), o.code))
				s = 100;
			else if(f && f->k == MOVE && f->fl == LOCATION_HAND && f->tl == LOCATION_MZONE && same_code(kc(f), o.code))
				s = 60;
			break;
		case OptKind::SpSummon:
			for(size_t k = 0; k < next.size() && k < 10; ++k)
				if(next[k]->k == SPSUMMON && same_code(kc(next[k]), o.code)) {
					s = 90 - static_cast<double>(k);
					break;
				}
			break;
		case OptKind::Activate:
			if(f && f->k == CHAIN && same_code(kc(f), o.code))
				s = (f->fl == norm_loc(o.loc) ? 100 : 70) + (f->desc && f->desc == o.desc ? 10 : 0);
			else if(in_next(o.code, 3))
				s = 20;
			break;
		case OptKind::MSet:
			if(f && f->k == MOVE && f->fl == LOCATION_HAND && f->tl == LOCATION_MZONE && (f->tpos & POS_FACEDOWN))
				s = same_code(kc(f), o.code) ? (kc(f) ? 95 : 60) : f->code ? 0 : 40;
			break;
		case OptKind::SSet:
			if(f && f->k == MOVE && f->fl == LOCATION_HAND && f->tl == LOCATION_SZONE && (f->tpos & POS_FACEDOWN))
				s = same_code(kc(f), o.code) ? (kc(f) ? 95 : 60) : f->code ? 0 : 40;
			break;
		case OptKind::Reposition:
			if(sf && sf->k == POS && sf->ts == o.seq)
				s = 80;
			break;
		case OptKind::ToBattle:
			if(sf && sf->k == PHASE && sf->v == PHASE_BATTLE_START)
				s = 90;
			break;
		case OptKind::ToMain2:
			if(sf && sf->k == PHASE && sf->v == PHASE_MAIN2)
				s = 90;
			break;
		case OptKind::ToEnd:
			if(sf && sf->k == PHASE && (sf->v == PHASE_END || sf->v == PHASE_MAIN2))
				s = sf->v == PHASE_END ? 90 : 0;
			else if(f && f->k == TURN)
				s = 80;
			break;
		case OptKind::Attack:
			if(f && f->k == ATTACK && f->p == d.player && f->ts == o.seq)
				s = 100;
			break;
		case OptKind::Pass:
			s = (f && f->k == CHAIN && f->p == d.player) ? 0 : 50;
			if(d.type == DecisionType::Card || d.type == DecisionType::UnselectCard)
				s = 5;  // finishing a selection: after the cards it names
			break;
		case OptKind::Yes:
			s = (f && (f->k == CHAIN || f->k == MOVE || f->k == SPSUMMON) && (f->p == d.player || same_code(kc(f), o.code))) ? 50 : 20;
			break;
		case OptKind::No:
			s = 30;
			break;
		case OptKind::Select: {
			const auto& codes = o.codes.empty() ? std::vector<uint32_t>{o.code} : o.codes;
			std::vector<bool> used(next.size(), false);
			int hit = 0, miss = 0;
			double deck_depth = 0;
			for(size_t c = 0; c < codes.size(); ++c) {
				bool found = false;
				for(size_t k = 0; k < next.size() && !found; ++k)
					if(!used[k] && kc(next[k]) && same_code(kc(next[k]), codes[c])) {
						used[k] = true;
						found = true;
					}
				if(!found) {
					++miss;
					// a card the replay doesn't name, Set later as a monster or a Spell/Trap: one that can be
					for(const Ev* e : next)
						if(e->k == MOVE && !kc(e) && e->need) {
							const CardInfo* ci = db_.get(codes[c]);
							if(ci && ((e->need == 1) == ((ci->type & TYPE_MONSTER) != 0)))
								s += 3;
							break;
						}
					// a hidden card (the replay doesn't say which): rather not one the player uses later
					if(c < o.locs.size() && (o.locs[c] == LOCATION_HAND || o.locs[c] == LOCATION_DECK || o.locs[c] == LOCATION_EXTRA) &&
					   used_later(codes[c], d.player, cur))
						s -= 4;
				} else {
					++hit;
				}
				if(c < o.locs.size() && o.locs[c] == LOCATION_DECK && c < o.seqs.size())
					deck_depth += o.seqs[c];  // the bottom copy first: the top ones are the draws to come
			}
			s += 10.0 * hit - 6.0 * miss - 0.001 * deck_depth;
			break;
		}
		case OptKind::Choose:
			if(d.type == DecisionType::Place && o.response.size() >= 3) {
				for(size_t k = 0; k < next.size() && k < 4; ++k)
					if(next[k]->k == MOVE && on_field(next[k]->tl) && next[k]->tp == o.response[0] &&
					   next[k]->tl == o.response[1] && next[k]->ts == o.response[2]) {
						s = 50;
						break;
					}
			} else if(d.type == DecisionType::Position && o.response.size() >= 4) {
				uint32_t p = 0;
				std::memcpy(&p, o.response.data(), 4);
				for(size_t k = 0; k < next.size() && k < 4; ++k)
					if(next[k]->k == MOVE && next[k]->tl == LOCATION_MZONE &&
					   ((next[k]->tpos & POS_FACEDOWN) != 0) == ((p & POS_FACEDOWN) != 0) &&
					   ((next[k]->tpos & POS_DEFENSE) != 0) == ((p & POS_DEFENSE) != 0)) {
						s = 50;
						break;
					}
			}
			break;
		default:
			break;
		}
		scored.push_back({s, static_cast<int>(i)});
	}
	std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
	// selections of the same cards from the same hidden piles (the hand, the Deck, the Extra Deck)
	// lead to the same game: try one of them (many stand-in cards make many such choices)
	std::set<std::vector<uint32_t>> same;
	std::vector<int> order;
	for(auto& p : scored) {
		const Option& o = d.options[static_cast<size_t>(p.second)];
		if(o.kind == OptKind::Select && !o.codes.empty() && o.locs.size() == o.codes.size()) {
			bool hidden = true;
			std::vector<uint32_t> key;
			for(size_t c = 0; c < o.codes.size(); ++c) {
				hidden = hidden && (o.locs[c] == LOCATION_HAND || o.locs[c] == LOCATION_DECK || o.locs[c] == LOCATION_EXTRA);
				key.push_back(o.codes[c] << 8 | o.locs[c]);
			}
			std::sort(key.begin(), key.end());
			if(hidden && !same.insert(key).second)
				continue;
		}
		order.push_back(p.second);
	}
	return order;
}

bool Follower::apply(const Step& s) {
	if(s.deck_top) {
		duel_->set_deck_top(s.player, s.codes);
		return true;
	}
	return duel_->respond(s.response);
}

bool Follower::rebuild(size_t steps) {
	duel_ = std::make_unique<Duel>(db_, scripts_);
	duel_->set_every_zone(true);
	duel_->set_tap(nullptr);
	if(!duel_->start(setup_))
		return false;
	for(size_t i = 0; i < steps && i < hist_.size(); ++i)
		if(!apply(hist_[i]))
			return false;
	hist_.resize(std::min(steps, hist_.size()));
	duel_->set_tap([this](const uint8_t* b, uint32_t n) { live_.read(b, n); });
	return true;
}

// After a card went into a Deck: the cards the replay draws next are put back on top.
void Follower::fix_decks(size_t cur) {
	for(uint8_t p = 0; p < 2; ++p) {
		if(!live_.to_deck[p])
			continue;
		live_.to_deck[p] = false;
		size_t taken = cur < top_count_[p].size() ? top_count_[p][cur] : tops_[p].size();
		std::vector<uint32_t> next;
		for(size_t i = taken; i < tops_[p].size() && next.size() < 8; ++i)
			next.push_back(db_.script_id(tops_[p][i]));
		if(next.empty())
			continue;
		Step s;
		s.deck_top = true;
		s.player = p;
		s.codes = next;
		apply(s);
		hist_.push_back(std::move(s));
	}
}

void Follower::take_position(size_t cur) {
	const Decision& d = duel_->decision();
	const Board b = duel_->board();
	bool action = (d.type == DecisionType::Idle || d.type == DecisionType::Battle) && d.player == b.turn_player;
	if(!action) {
		// a player answering with an activation (a hand trap, a quick effect)
		if(d.type != DecisionType::Chain)
			return;
		auto next = ahead(cur, 1);
		if(next.empty() || next[0]->k != CHAIN || next[0]->p != d.player)
			return;
	}
	Pos p;
	const uint8_t nxt = static_cast<uint8_t>(1 - b.turn_player);
	for(uint8_t me = 0; me < 2; ++me) {
		auto ev = eval_->evaluate(b, me, nxt);
		p.f[me] = eval_->features(b, me, nxt, ev.value);
		p.raw[me] = ev.raw;
	}
	p.turn = b.turn;
	p.steps = hist_.size();
	p.action = action;
	p.turn_player = b.turn_player;
	if(d.type == DecisionType::Idle && d.player == b.turn_player && b.phase == PHASE_MAIN1 && b.turn >= 2) {
		bool first = true;
		for(const auto& q : pos_)
			first = first && !(q.turn == b.turn && !q.start.empty());
		if(first)
			p.start = start_json(b);
		bool first_board = b.turn == 2;
		for(const auto& q : pos_)
			first_board = first_board && q.board.empty();
		if(first_board)
			p.board = start_json(b, true);
	}
	pos_.push_back(std::move(p));
}

// A real game's position for self-play to play on from (ygoe-cli selfplay --starts): both
// decks, hands, the cards on the field / in the GY / banished, LP, the turn player first.
// Script ids. A card nobody saw in a hand is only counted; positions where one would be on
// the field, in a GY or in a Deck list are not used ("" back). Xyz materials are not kept.
//
// With `board_only` ("kind":"board"): the cards nobody saw are left out of the decks and hands, a
// face-down Spell/Trap nobody saw stays as the stand-in Set card (kFillerST), a face-down monster
// nobody saw is left out; "" if the player going first left nothing on the field.
std::string Follower::start_json(const Board& b, bool board_only) const {
	const uint8_t order[2] = {b.turn_player, static_cast<uint8_t>(1 - b.turn_player)};
	if(!board_only)
		for(uint8_t e : order)
			for(auto c : decks_[e].main)
				if(is_filler(c))
					return "";
	if(board_only) {
		bool any = false;
		for(const auto* zone : {&b.mzone[order[1]], &b.szone[order[1]]})
			for(const auto& c : *zone)
				any = any || (c.code && !(c.type & TYPE_TOKEN));
		if(!any)
			return "";
	}
	std::ostringstream o;
	o << "{\"kind\":\"" << (board_only ? "board" : "start") << "\",\"turn\":" << b.turn << ",\"lp\":[" << b.lp[order[0]] << "," << b.lp[order[1]] << "],\"decks\":[";
	auto list = [&](const std::vector<uint32_t>& v) {
		std::string r = "[";
		bool f = true;
		for(auto c : v) {
			if(is_filler(c))
				continue;
			r += (f ? "" : ",") + std::to_string(c);
			f = false;
		}
		return r + "]";
	};
	for(int k = 0; k < 2; ++k)
		o << (k ? "," : "") << "{\"main\":" << list(decks_[order[k]].main) << ",\"extra\":" << list(decks_[order[k]].extra) << "}";
	o << "],\"hand\":[";
	for(int k = 0; k < 2; ++k) {
		std::vector<uint32_t> known;
		for(const auto& c : b.hand[order[k]])
			if(c.code && !is_filler(c.code))
				known.push_back(c.code);
		o << (k ? "," : "") << list(known);
	}
	o << "],\"hand_size\":[" << b.hand[order[0]].size() << "," << b.hand[order[1]].size() << "],\"placed\":[";
	for(int k = 0; k < 2; ++k) {
		o << (k ? "," : "") << "[";
		bool f = true;
		for(const auto* zone : {&b.mzone[order[k]], &b.szone[order[k]], &b.grave[order[k]], &b.banished[order[k]]})
			for(const auto& c : *zone) {
				if(!c.code || (c.type & TYPE_TOKEN))
					continue;
				if(is_filler(c.code)) {
					if(!board_only)
						return "";
					if(c.loc != LOCATION_SZONE)
						continue;
				}
				o << (f ? "" : ",") << "{\"c\":" << c.code << ",\"l\":" << int(c.loc) << ",\"s\":" << c.seq << ",\"p\":" << c.pos << "}";
				f = false;
			}
		o << "]";
	}
	o << "]";
	return o.str();
}

FollowResult Follower::run() {
	FollowResult res;
	Recorded rec;
	rec.read(in_.messages);
	if(rec.first < 0) {
		res.stopped = "the replay has no turns";
		return res;
	}
	res.winner = rec.winner;
	const uint8_t flip = static_cast<uint8_t>(rec.first);  // engine player = recorded player ^ flip
	auto eng = [&](uint8_t p) { return static_cast<uint8_t>((p ^ flip) & 1); };

	// The opening hands: the draws before the first turn. They start in the hands here (the
	// core deals none), so those draws aren't compared.
	size_t opening[2] = {0, 0};
	rec_.clear();
	{
		bool started = false;
		for(const auto& e : rec.ev) {
			started = started || e.k == TURN;
			if(!started && e.k == DRAW) {
				opening[e.p & 1] += static_cast<size_t>(e.v);
				continue;
			}
			rec_.push_back(e);
		}
	}
	// Recorded events with the engine's player numbers.
	for(auto& e : rec_) {
		e.p = eng(e.p);
		if(e.k == MOVE || e.k == ATTACK)
			e.tp = eng(e.tp);
	}
	// YGOE_FOLLOW_TURNS=N: only the first N turns (collecting end boards: N = 2)
	if(const char* ft = std::getenv("YGOE_FOLLOW_TURNS")) {
		const int n = std::atoi(ft);
		int seen = 0;
		for(size_t j = 0; j < rec_.size(); ++j)
			if(rec_[j].k == TURN && ++seen > n) {
				rec_.resize(j);
				break;
			}
	}
	for(const auto& e : rec_) {
		if(!soft(e))
			++res.events;
		if(e.k == TURN)
			++res.turns;
	}

	// Each player's Deck: the cards that come off its top, in the order the replay takes them,
	// then the rest of their list. Hidden ones: cards of their list nobody saw.
	for(uint8_t r = 0; r < 2; ++r) {
		const uint8_t e = eng(r);
		std::vector<uint32_t> top;
		std::vector<uint8_t> top_need;
		for(int inst : rec.tops[r]) {
			top.push_back(rec.code_of(inst));
			top_need.push_back(rec.need_of(inst));
		}
		auto fits = [&](uint32_t c, uint8_t nd) {
			if(!nd)
				return true;
			const CardInfo* ci = db_.get(db_.script_id(c));
			if(!ci)
				return false;
			return nd == 1 ? (ci->type & TYPE_MONSTER) != 0 : (ci->type & (TYPE_SPELL | TYPE_TRAP)) != 0;
		};
		std::multiset<uint32_t> seen;  // codes the replay shows of this player's cards
		for(const auto& ev : rec.ev)
			if(ev.hint && ((ev.k == MOVE && (ev.p == r || ev.tp == r)) || ((ev.k == CHAIN || ev.k == SUMMON || ev.k == SPSUMMON) && ev.p == r)))
				seen.insert(ev.hint);
		std::multiset<uint32_t> pool;
		std::vector<uint32_t> extra;
		if(in_.has_deck[r]) {
			extra = in_.decks[r].extra;
			for(auto c : in_.decks[r].main) {
				// an Extra Deck card the sender couldn't tell apart (an id unknown to it, such as
				// YGO Omega's own artworks) goes to the Extra Deck
				const CardInfo* ci = db_.get(db_.script_id(c));
				if(ci && ci->is_extra_deck())
					extra.push_back(c);
				else
					pool.insert(c);
			}
		} else {
			// no list: the cards seen of it (one copy of each beyond the draws), filled up to 40
			std::set<uint32_t> once;
			for(auto c : seen) {
				const CardInfo* ci = db_.get(db_.script_id(c));
				if(ci && ci->is_extra_deck()) {
					if(std::count(extra.begin(), extra.end(), c) < 3)
						extra.push_back(c);
				} else if(once.insert(c).second) {
					pool.insert(c);
				}
			}
		}
		for(auto c : top)
			if(c)
				if(auto it = pool.find(c); it != pool.end())
					pool.erase(it);
		for(size_t ti = 0; ti < top.size(); ++ti) {
			auto& c = top[ti];
			if(c)
				continue;
			// a hidden card: one of the list the replay never shows (a monster when it was Set in a
			// Monster Zone, a Spell/Trap when Set in the Spell & Trap Zone), else any, else a stand-in
			const uint8_t nd = top_need[ti];
			auto pick = pool.end();
			for(auto it = pool.begin(); it != pool.end() && pick == pool.end(); ++it)
				if(!seen.count(*it) && fits(*it, nd))
					pick = it;
			for(auto it = pool.begin(); it != pool.end() && pick == pool.end(); ++it)
				if(fits(*it, nd))
					pick = it;
			if(pick == pool.end() && !pool.empty() && !nd)
				pick = pool.begin();
			if(pick != pool.end()) {
				c = *pick;
				pool.erase(pick);
			} else {
				c = nd == 2 ? kFillerST : kFiller;
			}
		}
		const size_t nopen = std::min(opening[r], top.size());
		setup_.players[e].hand.assign(top.begin(), top.begin() + static_cast<std::ptrdiff_t>(nopen));
		top.erase(top.begin(), top.begin() + static_cast<std::ptrdiff_t>(nopen));
		std::vector<uint32_t> main(pool.begin(), pool.end());
		while(main.size() + top.size() + nopen < 40 && !in_.has_deck[r])
			main.push_back(kFiller);
		for(size_t i = top.size(); i-- > 0;)
			main.push_back(top[i]);  // the last element is the top
		setup_.players[e].main = main;
		if(!in_.has_deck[r])
			while(static_cast<int>(extra.size()) < std::min(rec.start_extra[r], 15))
				extra.push_back(kExtraFiller);
		setup_.players[e].extra = extra;
		tops_[e] = top;
		for(auto c : main)
			decks_[e].main.push_back(db_.script_id(c));
		for(auto c : setup_.players[e].hand)
			decks_[e].main.push_back(db_.script_id(c));
		for(auto c : extra)
			decks_[e].extra.push_back(db_.script_id(c));
		if(decks_[e].main.empty())
			decks_[e].main.push_back(kFiller);
	}
	// how many top cards each player had taken before each recorded event
	for(uint8_t e = 0; e < 2; ++e) {
		top_count_[e].assign(rec_.size() + 1, 0);
		size_t n = 0;
		for(size_t i = 0; i < rec_.size(); ++i) {
			top_count_[e][i] = n;
			if(rec_[i].top && rec_[i].p == e)
				n += rec_[i].k == DRAW ? static_cast<size_t>(rec_[i].v) : 1;
		}
		top_count_[e][rec_.size()] = n;
	}
	if(kDebug)
		for(int e = 0; e < 2; ++e) {
			std::fprintf(stderr, "setup P%d hand:", e);
			for(auto c : setup_.players[e].hand)
				std::fprintf(stderr, " %u", c);
			std::fprintf(stderr, " | tops:");
			for(auto c : tops_[e])
				std::fprintf(stderr, " %u", c);
			std::fprintf(stderr, " | deck %zu\n", setup_.players[e].main.size());
		}
	setup_.no_shuffle = true;
	eval_ = std::make_unique<Evaluator>(db_, decks_[0], decks_[1]);
	eval_->set_value_net(nullptr);

	live_ = Live{};
	duel_ = std::make_unique<Duel>(db_, scripts_);
	duel_->set_every_zone(true);  // the replay names the exact zone
	duel_->set_tap([this](const uint8_t* b, uint32_t n) { live_.read(b, n); });
	if(!duel_->start(setup_)) {
		res.stopped = "the rules core couldn't start the duel";
		return res;
	}
	size_t cur = 0;
	{
		long m = match(live_.out, 0);
		live_.out.clear();
		if(m < 0) {
			res.stopped = "the opening doesn't match";
			return res;
		}
		cur = static_cast<size_t>(m);
	}

	struct Frame {
		std::vector<int> order;
		size_t next = 0;
		size_t steps;
		size_t cur;
		size_t positions;
		int lp[2];
	};
	std::vector<Frame> stack;
	constexpr size_t kWindow = 48;  // decisions that can still be changed
	int trials = 0;
	bool dirty = false;
	size_t best_dbg = 0;
	// a game must not run on: a time limit, and a limit on moves that reproduce nothing
	const auto started = std::chrono::steady_clock::now();
	auto out_of_time = [&] {
		return in_.max_ms > 0 && std::chrono::steady_clock::now() - started > std::chrono::milliseconds(in_.max_ms);
	};
	int idle = 0;
	bool timed_out = false;
	// the deepest point reached: what is kept when the game can't be followed to its end
	size_t best = cur;
	std::vector<Pos> best_pos;
	auto battle_locked = [&](size_t from) {
		const Decision& d = duel_->decision();
		if(d.type != DecisionType::Idle || d.player != duel_->turn_player() || duel_->phase() != PHASE_MAIN1)
			return false;
		for(const auto& o : d.options)
			if(o.kind == OptKind::ToBattle)
				return false;
		for(size_t j = from; j < rec_.size() && rec_[j].k != TURN; ++j)
			if(rec_[j].k == ATTACK || (rec_[j].k == PHASE && rec_[j].v == PHASE_BATTLE_START))
				return true;
		return false;
	};
	auto rest_is_end = [&](size_t from) {
		// only the result is left (a surrender, a time-out: the core doesn't announce those)
		for(size_t j = from; j < rec_.size(); ++j)
			if(!soft(rec_[j]) && rec_[j].k != WIN)
				return false;
		return true;
	};
	while(true) {
		if(cur > best || best_pos.empty()) {
			best = cur;
			best_pos = pos_;
		}
		if(duel_->ended() || cur >= rec_.size() || rest_is_end(cur)) {
			res.complete = true;
			best = cur;
			best_pos = pos_;
			break;
		}
		if(duel_->decision().type == DecisionType::None) {
			res.stopped = "the rules core stopped without a decision";
			break;
		}
		fix_decks(cur);
		take_position(cur);
		Frame fr;
		fr.order = rank(duel_->decision(), cur);
		fr.steps = hist_.size();
		fr.cur = cur;
		fr.positions = pos_.size();
		fr.lp[0] = live_.lp[0];
		fr.lp[1] = live_.lp[1];
		stack.push_back(std::move(fr));
		if(stack.size() > kWindow)
			stack.erase(stack.begin());
		bool advanced = false;
		while(!stack.empty() && trials < in_.max_trials) {
			if(out_of_time()) {
				timed_out = true;
				break;
			}
			Frame& f = stack.back();
			if(f.next >= f.order.size()) {
				stack.pop_back();
				dirty = true;
				continue;
			}
			if(dirty) {
				live_.out.clear();
				if(!rebuild(f.steps)) {
					stack.clear();
					break;
				}
				live_.lp[0] = f.lp[0];
				live_.lp[1] = f.lp[1];
				live_.to_deck[0] = live_.to_deck[1] = false;
				pos_.resize(f.positions);
				dirty = false;
			}
			const int oi = f.order[f.next++];
			if(oi < 0 || static_cast<size_t>(oi) >= duel_->decision().options.size())
				continue;
			const std::vector<uint8_t> resp = duel_->decision().options[static_cast<size_t>(oi)].response;
			if(kDebug)
				std::fprintf(stderr, "d%zu %s: %s\n", stack.size(), decision_name(duel_->decision().type),
				             duel_->decision().options[static_cast<size_t>(oi)].label.c_str());
			if(kDebug && f.next == 1 && (duel_->decision().type == DecisionType::Card || duel_->decision().type == DecisionType::Idle || duel_->decision().type == DecisionType::Battle)) {
				std::fprintf(stderr, "   options:");
				for(const auto& o : duel_->decision().options)
					std::fprintf(stderr, " {%s}", o.label.c_str());
				std::fprintf(stderr, "\n");
			}
			const int lp0 = live_.lp[0], lp1 = live_.lp[1];
			live_.out.clear();
			++trials;
			if(!duel_->respond(resp)) {
				live_.out.clear();
				live_.lp[0] = lp0;
				live_.lp[1] = lp1;
				continue;  // the core rejected it and still asks the same question
			}
			long m = match(live_.out, f.cur);
			if(kDebug && m < 0 && f.cur >= best_dbg) {
				best_dbg = f.cur;
				std::fprintf(stderr, "FRONTIER at %zu, P%d %s: %s; replay next:", f.cur, duel_->decision().player,
				             decision_name(duel_->decision().type), "(after)");
				for(size_t j = f.cur; j < rec_.size() && j < f.cur + 6; ++j)
					std::fprintf(stderr, " [%s]", ev_str(rec_[j]).c_str());
				std::fprintf(stderr, "\n");
			}
			if(kDebug && m < 0) {
				std::fprintf(stderr, "  tried %s ->", duel_->decision().options.empty() ? "?" : "option");
				for(const auto& e : live_.out)
					std::fprintf(stderr, " [%s]", ev_str(e).c_str());
				std::fprintf(stderr, "\n");
			}
			// A dead end the replay already shows: Main Phase 1 with no Battle Phase on offer (an effect
			// used that locks it, a lock the player didn't take) while the replay attacks this turn.
			// Backing up now instead of after many more tries (it was most of the "hidden attack" stops).
			if(m >= 0 && battle_locked(static_cast<size_t>(m))) {
				if(kDebug)
					std::fprintf(stderr, "  dead end: no Battle Phase, the replay attacks this turn\n");
				m = -1;
			}
			if(m < 0) {
				live_.out.clear();
				dirty = true;
				continue;
			}
			if(kDebug)
				for(const auto& e : live_.out)
					if(e.k == DRAW || (e.k == MOVE && e.fl == LOCATION_DECK))
						std::fprintf(stderr, "    ok %s\n", ev_str(e).c_str());
			live_.out.clear();
			Step s;
			s.response = resp;
			hist_.push_back(std::move(s));
			idle = static_cast<size_t>(m) == cur ? idle + 1 : 0;
			cur = static_cast<size_t>(m);
			advanced = true;
			break;
		}
		if(advanced && in_.max_idle > 0 && idle > in_.max_idle) {
			res.stopped = "many moves in a row that the replay doesn't show";
			break;
		}
		if(!advanced && kDebug) {
			rebuild(stack.empty() ? hist_.size() : hist_.size());
			const Board b = duel_->board(true);
			for(int p = 0; p < 2; ++p) {
				std::fprintf(stderr, "P%d hand:", p);
				for(auto& c : b.hand[p])
					std::fprintf(stderr, " %u", c.code);
				std::fprintf(stderr, " | deck top:");
				for(size_t i = b.deck[p].size(), k = 0; i-- > 0 && k < 6; ++k)
					std::fprintf(stderr, " %u", b.deck[p][i].code);
				size_t taken = cur < top_count_[p].size() ? top_count_[p][cur] : 0;
				std::fprintf(stderr, " | replay tops next (%zu taken):", taken);
				for(size_t i = taken; i < tops_[p].size() && i < taken + 6; ++i)
					std::fprintf(stderr, " %u", tops_[p][i]);
				std::fprintf(stderr, "\n");
			}
			std::fprintf(stderr, "decision %s of P%d:", decision_name(duel_->decision().type), duel_->decision().player);
			for(const auto& o : duel_->decision().options)
				std::fprintf(stderr, " {%s}", o.label.c_str());
			std::fprintf(stderr, "\n");
			std::fprintf(stderr, "stopped at recorded event %zu; next:", cur);
			for(size_t j = cur; j < rec_.size() && j < cur + 8; ++j)
				std::fprintf(stderr, " [%s]", ev_str(rec_[j]).c_str());
			std::fprintf(stderr, "\n");
		}
		if(!advanced) {
			res.stopped = timed_out ? "took too long" : trials >= in_.max_trials ? "too many tries" : "the game went differently from here";
			break;
		}
	}
	cur = std::max(cur, best);
	pos_ = std::move(best_pos);
	if(!res.complete && cur < rec_.size()) {
		// where it stopped: the turn, and the recorded events it couldn't reproduce (card ids only)
		for(size_t j = 0; j < cur; ++j)
			if(rec_[j].k == TURN)
				++res.stop_turn;
		for(size_t j = cur, n = 0; j < rec_.size() && n < 6; ++j)
			if(!soft(rec_[j])) {
				res.stop_next += (n++ ? " " : "") + ("[" + ev_str(rec_[j]) + "]");
			}
	}
	res.followed = 0;
	for(size_t j = 0; j < cur && j < rec_.size(); ++j)
		if(!soft(rec_[j]))
			++res.followed;
	if(res.complete)
		res.followed = res.events;
	for(size_t j = 0; j < cur && j < rec_.size(); ++j)
		if(rec_[j].k == TURN)
			++res.turns_followed;

	// The positions, a few per turn spread over it (the Main and Battle Phase ones first).
	if(rec.winner == 0 || rec.winner == 1) {
		const uint8_t w = eng(static_cast<uint8_t>(rec.winner));
		std::map<uint32_t, std::vector<const Pos*>> by_turn;
		for(const auto& p : pos_)
			by_turn[p.turn].push_back(&p);
		std::ostringstream o;
		for(auto& [turn, list] : by_turn) {
			std::vector<const Pos*> keep;
			const size_t n = list.size(), k = static_cast<size_t>(std::max(1, in_.per_turn));
			if(n <= k)
				keep = list;
			else
				for(size_t i = 0; i < k; ++i)
					keep.push_back(list[(n - 1) * (i + 1) / k]);
			for(const Pos* p : keep)
				for(uint8_t me = 0; me < 2; ++me) {
					o << "{\"z\":" << (w == me ? 1 : 0) << ",\"fin\":1,\"raw\":" << p->raw[me] << ",\"turn\":" << p->turn
					  << ",\"game\":" << json::quote(in_.game) << ",\"src\":\"replay\",\"f\":[";
					for(size_t i = 0; i < p->f[me].size(); ++i)
						o << (i ? "," : "") << p->f[me][i];
					o << "]}\n";
					++res.positions;
				}
		}
		res.jsonl = o.str();
		// the positions to start self-play games from, with who won (from the turn player's side)
		for(const auto& p : pos_)
			if(!p.start.empty()) {
				const uint8_t tp = p.turn_player;
				o.str("");
				o << p.start << ",\"z\":" << (w == tp ? 1 : 0) << ",\"game\":" << json::quote(in_.game) << ",\"src\":\"replay\"}\n";
				res.jsonl += o.str();
			}
		for(const auto& p : pos_)
			if(!p.board.empty()) {
				o.str("");
				o << p.board << ",\"game\":" << json::quote(in_.game) << ",\"src\":\"replay\"}\n";
				res.jsonl += o.str();
			}
	}
	return res;
}

}  // namespace

std::string FollowResult::to_json() const {
	std::ostringstream o;
	o << "{\"positions\":" << positions << ",\"events\":" << events << ",\"followed\":" << followed << ",\"turns\":" << turns
	  << ",\"turns_followed\":" << turns_followed << ",\"complete\":" << (complete ? "true" : "false") << ",\"winner\":" << winner
	  << ",\"stopped\":" << json::quote(stopped) << ",\"follow_version\":" << kFollowVersion << ",\"stop_turn\":" << stop_turn << ",\"stop_next\":" << json::quote(stop_next)
	  << ",\"jsonl\":" << json::quote(jsonl) << "}";
	return o.str();
}

namespace {

// The core's messages (EDOPro format) as YGO Omega records them (classic format), seen by [viewer].
class Classic {
public:
	explicit Classic(uint8_t viewer) : viewer_(viewer) {}
	std::vector<std::vector<uint8_t>> out;

	void read(const uint8_t* data, uint32_t len) {
		Bytes all{data, data + len};
		while(all.p + 4 <= all.end) {
			uint32_t size = all.get<uint32_t>();
			const uint8_t* msg = all.p;
			all.p += size;
			if(size == 0 || all.p > all.end)
				break;
			one(msg, msg + size);
		}
	}
	void msg(uint8_t type, std::initializer_list<uint32_t> u32s) {
		std::vector<uint8_t> m{type};
		for(auto v : u32s)
			put32(m, v);
		out.push_back(std::move(m));
	}
	static void put32(std::vector<uint8_t>& m, uint32_t v) {
		for(int i = 0; i < 4; ++i)
			m.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}

private:
	uint8_t viewer_;
	struct Loc {
		uint8_t con = 0, loc = 0;
		uint32_t seq = 0, pos = 0;
	};
	static Loc loc10(Bytes& r) {
		Loc l;
		l.con = r.get<uint8_t>();
		l.loc = r.get<uint8_t>();
		l.seq = r.get<uint32_t>();
		l.pos = r.get<uint32_t>();
		return l;
	}
	static void loc4(std::vector<uint8_t>& m, const Loc& l) {
		m.push_back(l.con);
		m.push_back(l.loc);
		m.push_back(static_cast<uint8_t>(l.seq));
		m.push_back(static_cast<uint8_t>(l.pos));
	}
	bool hidden_place(const Loc& l) const {
		const uint8_t loc = norm_loc(l.loc);
		if(loc == LOCATION_DECK)
			return true;
		if(l.con == viewer_)
			return false;
		if(loc == LOCATION_HAND)
			return true;
		return (on_field(loc) || loc == LOCATION_EXTRA) && (l.pos & POS_FACEDOWN);
	}
	void one(const uint8_t* msg, const uint8_t* end) {
		Bytes r{msg + 1, end};
		std::vector<uint8_t> m{msg[0]};
		switch(msg[0]) {
		case MSG_NEW_TURN:
			m.push_back(r.get<uint8_t>());
			break;
		case MSG_NEW_PHASE: {
			uint16_t ph = r.get<uint16_t>();
			m.push_back(static_cast<uint8_t>(ph));
			m.push_back(static_cast<uint8_t>(ph >> 8));
			break;
		}
		case MSG_DRAW: {
			uint8_t p = r.get<uint8_t>();
			uint32_t n = r.get<uint32_t>();
			m.push_back(p);
			m.push_back(static_cast<uint8_t>(n));
			for(uint32_t i = 0; i < n; ++i) {
				uint32_t c = r.get<uint32_t>();
				r.get<uint32_t>();
				put32(m, p == viewer_ ? c : 0);
			}
			break;
		}
		case MSG_MOVE: {
			uint32_t c = r.get<uint32_t>();
			Loc f = loc10(r), t = loc10(r);
			uint32_t reason = r.get<uint32_t>();
			put32(m, (hidden_place(f) && hidden_place(t)) || (norm_loc(f.loc) == LOCATION_DECK && t.con != viewer_ && hidden_place(t)) ? 0 : c);
			loc4(m, f);
			loc4(m, t);
			put32(m, reason);
			break;
		}
		case MSG_SWAP: {
			for(int k = 0; k < 2; ++k) {
				put32(m, r.get<uint32_t>());
				loc4(m, loc10(r));
			}
			break;
		}
		case MSG_POS_CHANGE: {
			uint32_t c = r.get<uint32_t>();
			uint8_t con = r.get<uint8_t>(), loc = r.get<uint8_t>(), seq = r.get<uint8_t>(), prev = r.get<uint8_t>(), cur = r.get<uint8_t>();
			put32(m, con != viewer_ && (cur & POS_FACEDOWN) ? 0 : c);
			for(uint8_t b : {con, loc, seq, prev, cur})
				m.push_back(b);
			break;
		}
		case MSG_SUMMONING:
		case MSG_SPSUMMONING:
		case MSG_FLIPSUMMONING: {
			put32(m, r.get<uint32_t>());
			loc4(m, loc10(r));
			break;
		}
		case MSG_CHAINING: {
			put32(m, r.get<uint32_t>());
			loc4(m, loc10(r));
			m.push_back(r.get<uint8_t>());
			m.push_back(r.get<uint8_t>());
			m.push_back(static_cast<uint8_t>(r.get<uint32_t>()));
			put32(m, static_cast<uint32_t>(r.get<uint64_t>()));
			m.push_back(static_cast<uint8_t>(r.get<uint32_t>()));
			break;
		}
		case MSG_ATTACK: {
			loc4(m, loc10(r));
			loc4(m, loc10(r));
			break;
		}
		case MSG_DAMAGE:
		case MSG_RECOVER:
		case MSG_PAY_LPCOST:
		case MSG_LPUPDATE:
			m.push_back(r.get<uint8_t>());
			put32(m, r.get<uint32_t>());
			break;
		case MSG_WIN:
			m.push_back(r.get<uint8_t>());
			m.push_back(r.get<uint8_t>());
			break;
		case MSG_CONFIRM_CARDS: {
			m.push_back(r.get<uint8_t>());
			uint32_t n = r.get<uint32_t>();
			m.push_back(static_cast<uint8_t>(n));
			for(uint32_t i = 0; i < n; ++i) {
				put32(m, r.get<uint32_t>());
				m.push_back(r.get<uint8_t>());
				m.push_back(r.get<uint8_t>());
				m.push_back(static_cast<uint8_t>(r.get<uint32_t>()));
			}
			break;
		}
		default:
			return;
		}
		out.push_back(std::move(m));
	}
};

}  // namespace

std::string follow_selftest(const CardDB& db, ScriptCache& scripts, const Deck& d0, const Deck& d1, uint64_t seed, int turns) {
	std::mt19937_64 rng(seed);
	DuelSetup setup;
	setup.no_shuffle = true;
	setup.seed = {seed, seed * 3 + 1, seed * 7 + 2, seed * 11 + 3};
	const Deck* dk[2] = {&d0, &d1};
	for(int p = 0; p < 2; ++p) {
		auto main = dk[p]->main;
		std::shuffle(main.begin(), main.end(), rng);
		setup.players[p].hand.assign(main.end() - 5, main.end());
		main.resize(main.size() - 5);
		setup.players[p].main = main;
		setup.players[p].extra = dk[p]->extra;
	}
	Classic rec(0);
	// what Omega writes first: the start, then the opening hands as draws
	{
		std::vector<uint8_t> m{MSG_START, 0};
		Classic::put32(m, 8000);
		Classic::put32(m, 8000);
		for(int p = 0; p < 2; ++p) {
			uint16_t a = static_cast<uint16_t>(setup.players[p].main.size() + 5), b = static_cast<uint16_t>(setup.players[p].extra.size());
			m.push_back(static_cast<uint8_t>(a));
			m.push_back(static_cast<uint8_t>(a >> 8));
			m.push_back(static_cast<uint8_t>(b));
			m.push_back(static_cast<uint8_t>(b >> 8));
		}
		rec.out.push_back(m);
		for(uint8_t p = 0; p < 2; ++p) {
			std::vector<uint8_t> d{MSG_DRAW, p, 5};
			for(auto c : setup.players[p].hand)
				Classic::put32(d, p == 0 ? c : 0);
			rec.out.push_back(d);
		}
	}
	Duel duel(db, scripts);
	duel.set_tap([&](const uint8_t* b, uint32_t n) { rec.read(b, n); });
	if(!duel.start(setup))
		return "{\"error\":\"start failed\"}";
	int guard = 0;
	while(!duel.ended() && static_cast<int>(duel.turn()) <= turns && guard++ < 3000) {
		const Decision& d = duel.decision();
		if(d.type == DecisionType::None || d.options.empty())
			break;
		// mostly the first options (they play cards), sometimes any
		size_t n = d.options.size();
		size_t pick = (rng() % 3 == 0) ? rng() % n : std::min<size_t>(n - 1, rng() % 2);
		if(!duel.respond(d.options[pick].response))
			duel.respond(d.options[0].response);
	}
	// a game cut at the turn limit: won by who is ahead (so the positions get an outcome)
	if(!duel.ended()) {
		const Board b = duel.board();
		rec.out.push_back({MSG_WIN, static_cast<uint8_t>(b.lp[0] >= b.lp[1] ? 0 : 1), 0});
	}
	FollowInput in;
	in.messages = rec.out;
	for(int p = 0; p < 2; ++p) {
		in.has_deck[p] = true;
		in.decks[p] = *dk[p];
	}
	in.game = "selftest";
	FollowResult r = follow_replay(db, scripts, in);
	if(!std::getenv("YGOE_FOLLOW_JSONL"))
		r.jsonl.clear();
	return r.to_json();
}

FollowResult follow_replay(const CardDB& db, ScriptCache& scripts, const FollowInput& in) {
	Follower f(db, scripts, in);
	return f.run();
}

}  // namespace ygoe
