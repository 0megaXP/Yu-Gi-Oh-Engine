"""Get a combo video onto disk and cut it into the frames worth reading.

Combo videos change the screen once per action, so sampling at a fixed rate and
dropping frames that look like the one before keeps roughly one frame per step.
"""
import glob
import json
import os
import re
import shutil
import subprocess

from PIL import Image


def need(tool):
    if not shutil.which(tool):
        raise SystemExit(f"'{tool}' not found on PATH. Install it first (see tools/video_combo/README.md).")


def download(url, work):
    """Download with yt-dlp. Returns (video path, info dict, captions text or '')."""
    import yt_dlp

    os.makedirs(work, exist_ok=True)
    opts = {
        "outtmpl": os.path.join(work, "video.%(ext)s"),
        # 720p is enough to read card names and keeps the download small
        "format": "bv*[height<=720][ext=mp4]+ba[ext=m4a]/b[height<=720]/bv*[height<=720]/b",
        "merge_output_format": "mp4",
        "writesubtitles": True,
        "writeautomaticsub": True,
        "subtitleslangs": ["en", "en-orig", "en.*"],
        "subtitlesformat": "vtt",
        "quiet": True,
        "no_warnings": True,
        "noplaylist": True,
    }
    with yt_dlp.YoutubeDL(opts) as ydl:
        info = ydl.extract_info(url, download=True)
    videos = [p for p in glob.glob(os.path.join(work, "video.*")) if not p.endswith(".vtt")]
    if not videos:
        raise SystemExit("yt-dlp finished but no video file was written.")
    subs = sorted(glob.glob(os.path.join(work, "video.*.vtt")))
    captions = parse_vtt(open(subs[0], encoding="utf-8").read()) if subs else ""
    keep = {k: info.get(k) for k in ("id", "title", "uploader", "upload_date", "duration", "description", "webpage_url")}
    json.dump(keep, open(os.path.join(work, "info.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    return videos[0], keep, captions


def parse_vtt(text):
    """VTT -> '[mm:ss] line' text, with the rolling duplicates of auto-captions removed."""
    out, last = [], ""
    stamp = None
    for line in text.splitlines():
        m = re.match(r"(\d+):(\d\d):(\d\d)\.\d+\s+-->", line) or re.match(r"(\d\d):(\d\d)\.\d+\s+-->", line)
        if m:
            g = [int(x) for x in m.groups()]
            stamp = g[0] * 3600 + g[1] * 60 + g[2] if len(g) == 3 else g[0] * 60 + g[1]
            continue
        line = re.sub(r"<[^>]+>", "", line).strip()
        if not line or line.startswith(("WEBVTT", "Kind:", "Language:", "NOTE")) or line == last or stamp is None:
            continue
        last = line
        out.append(f"[{stamp // 60:02d}:{stamp % 60:02d}] {line}")
    return "\n".join(out)


def _dhash(img, size=16):
    g = img.convert("L").resize((size + 1, size), Image.BILINEAR)
    px = list(g.getdata())
    bits = 0
    for r in range(size):
        row = px[r * (size + 1):(r + 1) * (size + 1)]
        for c in range(size):
            bits = (bits << 1) | (row[c] > row[c + 1])
    return bits


def extract(video, work, fps=1.0, width=1280, threshold=10, max_frames=400, start=None, end=None):
    """Sample frames with ffmpeg, then keep only frames that differ from the last kept one.

    threshold: how many of the 256 hash bits must change to count as a new screen.
    Returns [(seconds, path)].
    """
    need("ffmpeg")
    raw = os.path.join(work, "raw")
    shutil.rmtree(raw, ignore_errors=True)
    os.makedirs(raw)
    cmd = ["ffmpeg", "-loglevel", "error", "-y"]
    if start is not None:
        cmd += ["-ss", str(start)]
    if end is not None:
        cmd += ["-to", str(end)]
    cmd += ["-i", video, "-vf", f"fps={fps},scale='min({width},iw)':-2", "-q:v", "3",
            os.path.join(raw, "f%05d.jpg")]
    subprocess.run(cmd, check=True)
    files = sorted(glob.glob(os.path.join(raw, "f*.jpg")))
    if not files:
        raise SystemExit("ffmpeg produced no frames. Is the file a video?")

    out_dir = os.path.join(work, "frames")
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir)
    kept, prev, pending = [], None, None
    offset = float(start or 0)
    for i, path in enumerate(files):
        t = offset + i / fps
        with Image.open(path) as img:
            h = _dhash(img)
        if prev is None or bin(h ^ prev).count("1") >= threshold:
            # the screen changed: keep the last frame of the previous screen too if it
            # moved on a little since we kept it (an animation finishing, a counter
            # updating), then the new one
            if pending and pending[2]:
                kept.append(pending[:2])
            kept.append((t, path))
            prev = h
            pending = None
        else:
            pending = (t, path, bin(h ^ prev).count("1") >= max(2, threshold // 3))
    if pending and pending[2]:
        kept.append(pending[:2])

    # drop the "settled" frames first if we are over budget, then thin evenly
    if len(kept) > max_frames:
        step = len(kept) / max_frames
        kept = [kept[int(i * step)] for i in range(max_frames)]
    result = []
    for t, path in kept:
        dst = os.path.join(out_dir, f"t{int(round(t * 10)):06d}.jpg")
        if not os.path.exists(dst):
            shutil.copy(path, dst)
            result.append((t, dst))
    shutil.rmtree(raw, ignore_errors=True)
    return result


def from_dir(frames_dir):
    """Reuse frames from an earlier run: files named t<tenths of a second>.jpg."""
    out = []
    for p in sorted(glob.glob(os.path.join(frames_dir, "t*.jpg"))):
        m = re.search(r"t(\d+)\.jpg$", p)
        if m:
            out.append((int(m.group(1)) / 10, p))
    return out
