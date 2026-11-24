// Triggers: effects that start when something happens to a card.
//
// Every trigger (and quick effect answering an event) a card's script registers is classified
// along the same axes (data/effects.tsv, tools/analyze_scripts.py):
//  - whose card it is about: this card ("If this card is sent to the GY") or another card,
//    watched from a zone ("If a monster is sent to the GY while this card is on the field");
//  - what happened: sent to the GY, banished, left the field, destroyed (by battle, by an effect),
//    Tributed, used as material, detached, discarded, added to the hand, returned to the Deck,
//    left the GY, placed in a zone (not activated: "placed" is the core's "moved" event with
//    the zone it ended in), moved, summoned...;
//  - where it came from (the field, the hand, the Deck, Xyz material);
//  - why: by a card effect, by battle, as a cost, discarded, as material (Fusion, Synchro...);
//  - who did it: anyone, the card's controller, the opponent;
//  - how it activates: optional ("If": doesn't miss timing; "When": can), mandatory, a quick
//    effect, or delayed (in the End Phase of the turn it happened).
//
// The engine uses this wherever a card moves: the bridge probe (reach.h) makes each trigger
// happen and sees what it reaches; the search picks costs, discards, materials and the cards
// it sends or banishes with it (own cards whose triggers it sets off, the opponent's it doesn't);
// the evaluation counts a monster that leaves something behind when removed ("floats").
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ygoe {

class CardDB;
struct CardInfo;
struct Decision;
struct Option;

enum class TrigEvent : uint8_t {
	Other = 0,
	ToGrave,           // sent to the GY
	Banished,
	LeftField,
	Destroyed,
	BattleDestroyed,   // destroyed by battle
	BattleDestroying,  // destroyed a monster by battle
	Tributed,
	Material,          // used as material (Fusion, Synchro, Xyz, Link, Ritual)
	Detached,          // Xyz material detached
	Discarded,
	ToHand,
	ToDeck,
	LeftGrave,
	Moved,             // changed zones (column effects, control changes)
	Placed,            // placed in the Spell & Trap / Field / Pendulum Zone (not activated)
	Summoned,          // Normal / Tribute Summoned
	SpSummoned,
	FlipSummoned,
	Flipped,
	Chaining,          // a card or effect is activated
	Phase,             // a phase began / ended (End Phase effects, delayed triggers)
	Battle,            // attack declared, battle steps, battle damage
	Drawn,
	Damage,            // damage or LP gained
	Targeted,
	Custom,            // events a script defines itself (often "one or more cards ... at once")
};

struct Trigger {
	TrigEvent event = TrigEvent::Other;
	uint32_t code = 0;          // EVENT_* as the script registers it
	bool self = false;          // about this card (else: another card, watched from `watch`)
	uint32_t watch = 0;         // LOCATION_* it watches from (another card's move)
	uint32_t from = 0;          // LOCATION_* the moved card had to come from (0: anywhere)
	uint32_t now = 0;           // LOCATION_* it has to be in afterwards (0: not checked)
	uint32_t reasons = 0;       // REASON_* it asks for (any of them; 0: any reason)
	uint32_t not_reasons = 0;   // REASON_* it rules out
	uint8_t by = 0;             // 0 anyone, 1 the card's controller, 2 the opponent
	enum Kind : uint8_t { OptionalIf, OptionalWhen, Mandatory, Quick } kind = OptionalIf;
	bool delayed = false;       // happens in a later phase (End Phase of the turn it moved)
	uint32_t category = 0;      // CATEGORY_* of what it does
	int effect = 0;             // index among the card's effects
};

const char* trig_event_name(TrigEvent e);
// "If this card is sent to the GY from the field by a card effect (optional, doesn't miss timing)"
std::string describe(const Trigger& t);

// The card's triggers (and quick effects answering an event), in script order.
std::vector<Trigger> triggers_of(const CardInfo& c);

// A card moving (or being used): what the rules core would see.
struct Move {
	uint32_t from = 0;     // LOCATION_*
	uint32_t to = 0;       // LOCATION_* (LOCATION_OVERLAY for Xyz material)
	uint32_t reason = 0;   // REASON_*
	uint32_t maybe = 0;    // REASON_* that may or may not be part of it (an effect or a cost)
	bool by_opponent = false;  // done by the other player than the card's controller
};

// Does this card's own trigger `t` happen when it moves like that?
bool fires(const Trigger& t, const Move& m);
// What a card's own triggers give when it moves like that (0: nothing): about 1 for a trigger
// that adds or summons a card, less for others.
double move_value(const CardInfo& c, const Move& m);
// The card's effects that it uses from the GY by choice (ignition / quick), lost if it's banished.
bool has_grave_effects(const CardInfo& c);
// Worth of what the card leaves behind when the opponent removes it from the field (its own
// triggers on being destroyed, sent, banished, leaving the field): 0 when nothing.
double floats(const CardInfo& c);

// The move a card selection makes (from its HINT_SELECTMSG, or a Tribute), when it is one:
// to, reason (and the reasons it may also have); false when the selection doesn't move the card picked (targets, summons...).
bool selection_move(const Decision& d, uint32_t& to, uint32_t& reason, uint32_t& maybe);
// The search's prior for picking `o` in that selection, from the triggers it sets off: own cards
// whose triggers happen first, the opponent's whose triggers would happen last; own cards with
// GY effects kept out of the banishment, the opponent's banished first. 1: no difference.
double selection_factor(const CardDB& db, const Decision& d, const Option& o);

}  // namespace ygoe
