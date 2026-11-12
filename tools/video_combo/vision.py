"""Read the frames with Claude and turn them into a combo.

Pass 1 reads frames in small batches, in order, and logs what happened on screen.
Pass 2 reads that log (plus captions and the video description) and writes the combo.
Both passes force a tool call so the answer always comes back as structured JSON.
"""
import base64
import json
import os
import time

DEFAULT_MODEL = "claude-sonnet-5-5"

ACTIONS = ["normal_summon", "special_summon", "link_summon", "xyz_summon", "synchro_summon",
           "fusion_summon", "ritual_summon", "pendulum_summon", "set", "activate", "chain", "resolve",
           "search", "draw", "send_to_gy", "banish", "attach", "return", "negate", "attack",
           "phase", "hand_shown", "board_shown", "other"]

EVENTS_TOOL = {
    "name": "log_events",
    "description": "Log what happens on screen in these frames, in order.",
    "input_schema": {
        "type": "object",
        "properties": {
            "events": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "t": {"type": "number", "description": "seconds, from the frame label"},
                        "action": {"type": "string", "enum": ACTIONS},
                        "card": {"type": "string", "description": "exact card name as shown; '' if none"},
                        "player": {"type": "string", "enum": ["combo", "opponent", "unknown"]},
                        "from": {"type": "string", "description": "hand, deck, extra, gy, banished, field, ..."},
                        "to": {"type": "string"},
                        "detail": {"type": "string", "description": "materials, cost, targets, which effect, chain link"},
                        "confidence": {"type": "string", "enum": ["high", "medium", "low"]},
                    },
                    "required": ["t", "action", "card", "confidence"],
                },
            },
            "hand": {"type": "array", "items": {"type": "string"},
                     "description": "the combo player's hand if fully readable in these frames, else []"},
            "board": {"type": "array", "items": {"type": "string"},
                      "description": "combo player's face-up/set field cards visible at the last frame"},
            "overlay_text": {"type": "string", "description": "any explanatory text the creator put on screen"},
        },
        "required": ["events"],
    },
}

COMBO_TOOL = {
    "name": "write_combo",
    "description": "Write the combo shown in the video.",
    "input_schema": {
        "type": "object",
        "properties": {
            "deck": {"type": "string"},
            "going": {"type": "string", "enum": ["first", "second", "unknown"]},
            "starting_hand": {"type": "array", "items": {"type": "string"}},
            "steps": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "t": {"type": "number"},
                        "action": {"type": "string", "enum": ACTIONS},
                        "card": {"type": "string"},
                        "detail": {"type": "string"},
                    },
                    "required": ["action", "card", "detail"],
                },
            },
            "end_board": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "card": {"type": "string"},
                        "where": {"type": "string", "description": "monster zone, spell/trap zone, field zone, hand, gy"},
                        "note": {"type": "string", "description": "e.g. 'set', 'xyz with 2 materials'"},
                    },
                    "required": ["card", "where"],
                },
            },
            "interruptions": {"type": "array", "items": {"type": "string"},
                              "description": "interruptions the creator says the end board has"},
            "choke_points": {"type": "array", "items": {"type": "string"},
                             "description": "where the creator says hand traps hit, or that you can see"},
            "uncertain": {"type": "array", "items": {"type": "string"},
                          "description": "anything you could not read or had to infer"},
        },
        "required": ["starting_hand", "steps", "end_board", "uncertain"],
    },
}


def _client():
    try:
        import anthropic
    except ImportError:
        raise SystemExit("pip install -r tools/video_combo/requirements.txt")
    if not os.environ.get("ANTHROPIC_API_KEY"):
        raise SystemExit("Set ANTHROPIC_API_KEY (an API key from console.anthropic.com).")
    return anthropic.Anthropic(max_retries=4)


def _call(client, model, system, content, tool, max_tokens):
    for attempt in range(3):
        msg = client.messages.create(
            model=model, max_tokens=max_tokens, system=system,
            tools=[tool], tool_choice={"type": "tool", "name": tool["name"]},
            messages=[{"role": "user", "content": content}],
        )
        for block in msg.content:
            if block.type == "tool_use":
                return block.input, msg.usage
        time.sleep(2 * (attempt + 1))
    raise RuntimeError("model returned no structured answer")


def _img(path):
    data = base64.standard_b64encode(open(path, "rb").read()).decode()
    return {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": data}}


def _captions_window(captions, t0, t1, pad=8):
    out = []
    for line in captions.splitlines():
        if line.startswith("[") and len(line) > 7:
            try:
                mm, ss = line[1:6].split(":")
                t = int(mm) * 60 + int(ss)
            except ValueError:
                continue
            if t0 - pad <= t <= t1 + pad:
                out.append(line)
    return "\n".join(out)


def read_frames(frames, card_names, info, captions, work, model=DEFAULT_MODEL, batch=6, log=print):
    """Pass 1. Returns the list of per-batch results. Cached in work/events.jsonl, so a
    re-run (after a crash or Ctrl-C) only pays for the batches it has not done yet."""
    cache_path = os.path.join(work, "events.jsonl")
    done = {}
    if os.path.exists(cache_path):
        for line in open(cache_path, encoding="utf-8"):
            r = json.loads(line)
            done[r["batch"]] = r
    client = None
    system = (
        "You are reading frames from a Yu-Gi-Oh! combo video, usually recorded in EDOPro, "
        "Master Duel, Dueling Nexus or Duelingbook, sometimes a real table. Log every game action "
        "you can see: activations, summons (with materials), searches, cards sent to the GY, "
        "banished, attached, and the chain. Read card names from the card art panel, the card "
        "text box, the chain/activation banner and the duel log. Use exact card names; when a name "
        "is partly hidden, pick the matching card from the candidate list. Frames are consecutive "
        "screens from one video, so do not log the same action twice when it stays on screen. "
        "If nothing happens in these frames, return an empty events list."
    )
    cards = "Candidate cards (the deck shown, plus common hand traps):\n" + "; ".join(card_names)
    header = f"Video: {info.get('title', '')}\nBy: {info.get('uploader', '')}\n"
    results, recent = [], []
    batches = [frames[i:i + batch] for i in range(0, len(frames), batch)]
    for bi, chunk in enumerate(batches):
        if bi in done:
            r = done[bi]
        else:
            client = client or _client()
            content = [{"type": "text", "text": header + cards}]
            if recent:
                content.append({"type": "text", "text": "Already logged just before these frames:\n" +
                                "\n".join(recent[-12:])})
            cap = _captions_window(captions, chunk[0][0], chunk[-1][0])
            if cap:
                content.append({"type": "text", "text": "Captions around these frames:\n" + cap})
            for t, path in chunk:
                content.append({"type": "text", "text": f"Frame t={t:.1f}s"})
                content.append(_img(path))
            out, usage = _call(client, model, system, content, EVENTS_TOOL, 4000)
            r = {"batch": bi, "t0": chunk[0][0], "t1": chunk[-1][0], **out}
            with open(cache_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
            log(f"  frames {bi + 1}/{len(batches)}  t={chunk[0][0]:.0f}-{chunk[-1][0]:.0f}s  "
                f"{len(out.get('events', []))} events  ({usage.input_tokens} in / {usage.output_tokens} out)")
        for e in r.get("events", []):
            recent.append(f"{e.get('t', 0):.0f}s {e.get('action')} {e.get('card', '')} {e.get('detail', '')}".strip())
        results.append(r)
    return results


def write_combo(batches, card_names, info, captions, model=DEFAULT_MODEL):
    """Pass 2: turn the frame log into one clean combo."""
    client = _client()
    log_lines, hands, overlays = [], [], []
    for b in batches:
        for e in b.get("events", []):
            log_lines.append(
                f"{e.get('t', 0):.1f}s | {e.get('player', '?')} | {e.get('action')} | {e.get('card', '')} | "
                f"{e.get('from', '')} -> {e.get('to', '')} | {e.get('detail', '')} | {e.get('confidence', '')}")
        if b.get("hand"):
            hands.append(f"{b['t0']:.0f}s: " + "; ".join(b["hand"]))
        if b.get("overlay_text"):
            overlays.append(f"{b['t0']:.0f}s: {b['overlay_text']}")
        if b.get("board"):
            last_board = b["board"]
    parts = [
        f"Video: {info.get('title', '')}\nBy: {info.get('uploader', '')}\n"
        f"Description:\n{(info.get('description') or '')[:3000]}",
        "Candidate cards:\n" + "; ".join(card_names),
        "Frame log (time | player | action | card | from -> to | detail | confidence):\n" + "\n".join(log_lines),
    ]
    if hands:
        parts.append("Hands seen on screen:\n" + "\n".join(hands))
    if overlays:
        parts.append("On-screen text:\n" + "\n".join(overlays))
    if captions:
        parts.append("Captions:\n" + captions[:15000])
    if any(b.get("board") for b in batches):
        parts.append("Board at the last frames: " + "; ".join(last_board))
    parts.append(
        "Write the combo as one clean line of play: the starting hand, each step in order, and the end "
        "board. Merge duplicate log entries, drop menu noise, and fix the order where the log is clearly "
        "out of order (a cost is paid before the effect resolves). If the video shows several combos or "
        "hands, write the first full one and mention the others in 'uncertain'. Use exact card names.")
    out, _ = _call(client, model, "You turn a Yu-Gi-Oh! video log into a precise combo write-up.",
                   [{"type": "text", "text": "\n\n".join(parts)}], COMBO_TOOL, 8000)
    return out
