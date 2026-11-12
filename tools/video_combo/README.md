# Video combo extractor

Turns a Yu-Gi-Oh! combo video (YouTube or a local file) into a written combo, then lets the engine try the same opening hand and reports whether it reaches the same end board.

```
YouTube link ──yt-dlp──▶ video + captions ──ffmpeg──▶ one frame per screen change
      ──Claude (vision)──▶ action log ──Claude──▶ combo.json / combo.md / state.txt
state.txt ──ygoe-cli analyze──▶ engine.json ──compare──▶ "the engine found 4/5 of the video's board"
```

## Setup

Run this on your own computer. YouTube often blocks downloads from cloud servers, so a CI runner or hosted session usually can't fetch videos.

```sh
tools/bootstrap.sh                                   # once: card data in data/cards.tsv
pip install -r tools/video_combo/requirements.txt    # anthropic, yt-dlp, pillow
# ffmpeg on PATH: winget install ffmpeg / brew install ffmpeg / apt install ffmpeg
export ANTHROPIC_API_KEY=sk-ant-...                  # from console.anthropic.com
```

## Use

```sh
# 1. video -> combo
python3 tools/video_combo/video_combo.py extract "https://www.youtube.com/watch?v=..." \
    --deck decks/angelechy_luna_mardelplata_2026-11.ydk \
    --opponent decks/elfnote_koller_wien_2026-11.ydk \
    --out combos/angelechy_e4_line

# 2. engine from the same hand
build/ygoe-cli analyze combos/angelechy_e4_line/state.txt --json --out combos/angelechy_e4_line/engine.json

# 3. does the engine reach the video's board?
python3 tools/video_combo/video_combo.py compare combos/angelechy_e4_line
```

The output folder contains:

| File | What it is |
|---|---|
| `combo.md` | The combo to read: hand, numbered steps with timestamps, end board, interruptions, choke points, and anything uncertain |
| `combo.json` | The same data, with every card resolved to its official passcode (the ids `.ydk` files and the engine use) |
| `state.txt` | A [state file](../../docs/state-format.md) with the video's opening hand |
| `work/` | The download, the kept frames, the captions and `events.jsonl` (the per-frame log) |

## Options worth knowing

| Option | Use it when |
|---|---|
| `--deck` | Always, if you have the list. Names read off the screen snap to cards in the deck, which fixes most misreads. |
| `--start 45 --end 300` | The video has an intro, a deck profile or several combos. Only that part is read. |
| `--fps 2` | Fast combos where several actions pass within one second. |
| `--threshold 6` | Small changes (a chain link, an LP counter) are being skipped. Raise it if near-identical frames pile up. |
| `--frames-only` | Check what frames get kept before paying for any model calls. |
| `--frames-from combos/x/work/frames` | Re-run the reading with another `--model` without downloading again. |
| `--model` | Default `claude-sonnet-5-5`. Can also be set with the `VIDEO_COMBO_MODEL` environment variable. |

An interrupted run resumes: the frame batches already read are cached in `work/events.jsonl`. Delete that file to read the frames again.

## Cost and accuracy

- **Cost.** A 10-minute video is typically 150–300 kept frames, roughly 1,500 input tokens each. Run `--frames-only` first to see the count, and use `--start`/`--end` to skip parts you don't need.
- **Works best on** clean EDOPro, Master Duel, Duelingbook or Dueling Nexus recordings, where card names show in the activation banner, the card text panel or the log.
- **Weaker on** table footage, heavy editing, and videos that cut between several hands. Those cards often end up under "Uncertain".
- **Spoken explanations.** Narration is used through YouTube captions when they exist. The tool doesn't transcribe audio itself.
- **Check `combo.md` before you trust it.** It is a reading of the video, not the engine. Hand traps or alternate lines the creator only mentions can blur the main line.
- **`compare` checks the field only.** It compares field cards and leaves out hand and GY. Going-second videos need the opponent's board added to `state.txt` by hand.
