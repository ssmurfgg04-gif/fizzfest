#!/usr/bin/env python3
"""Build the fun PR comment from results/*.json and post it via the API."""
import glob
import json
import os
import urllib.request

PR = os.environ["PR_NUMBER"]
REPO = os.environ["GITHUB_REPOSITORY"]
TOKEN = os.environ["GITHUB_TOKEN"]

rows = []
for f in sorted(glob.glob("results/*.json")):
    try:
        rows.append(json.load(open(f)))
    except Exception as e:
        print(f"bad json {f}: {e}")

if not rows:
    body = (
        "🥤 The scorer ran but produced no result json.\n\n"
        "Common causes:\n"
        "- `start.sh` did not start (it must run the server in the foreground and read `$PORT`)\n"
        "- the server never answered `GET /3` with `fizz` within 90 seconds\n"
        "- `install.sh` or `build.sh` exited non-zero\n\n"
        "Check the workflow logs above, and the [README](../blob/main/README.md)."
    )
else:
    parts = ["## 🥤 Your score card\n"]
    for r in rows:
        verdict = r.get("verdict", "?")
        name = r.get("submission", "?")
        if verdict == "QUALIFIED":
            ratio = r.get("ratio", 0)
            bar = "█" * max(1, min(int(ratio), 20))
            parts.append(f"### {name}: **{ratio:.2f}x** the baseline {bar}\n")
            parts.append("| what | number |")
            parts.append("|---|---|")
            parts.append(f"| requests per second | {r.get('rps', 0):,} |")
            parts.append(f"| baseline (same runner, same run) | {r.get('baseline_rps', 0):,} |")
            parts.append(f"| responses verified | {r.get('total', 0):,} |")
            parts.append(f"| p50 / p95 / p99 | {r.get('p50_ms')} / {r.get('p95_ms')} / {r.get('p99_ms')} ms |")
            parts.append(f"| errors | {r.get('error_pct')}% |\n")
            if ratio >= 10:
                parts.append("🦁 **10x club.** You are legally required to brag about this.\n")
            elif ratio >= 5:
                parts.append("🔥 **5x club.** Very rude to the baseline.\n")
            elif ratio >= 2:
                parts.append("⚡ Solid doubling. The baseline is crying.\n")
            parts.append("Merged entries land on the [leaderboard](../blob/main/LEADERBOARD.md).\n")
        else:
            reason = "correctness" if verdict == "FAILED_CORRECTNESS" else "load errors"
            parts.append(f"### {name}: ❌ disqualified ({reason})\n")
            parts.append(
                "The correctness gate is brutal on purpose: every response must be "
                "byte-exact. Look at `checker/checker.py` to see exactly what is "
                "tested, then try again. Nobody fails twice.\n"
            )
    body = "\n".join(parts)

# find and delete our previous auto comment so we do not pile up
api = f"https://api.github.com/repos/{REPO}/issues/{PR}/comments"
req = urllib.request.Request(api, headers={
    "Authorization": f"Bearer {TOKEN}",
    "Accept": "application/vnd.github+json",
})
with urllib.request.urlopen(req) as resp:
    comments = json.load(resp)

marker = "🥤 Your score card" if rows else "🥤 The scorer ran"
for c in comments:
    if c["user"]["login"] == "github-actions[bot]" and marker in c.get("body", ""):
        del_req = urllib.request.Request(
            f"https://api.github.com/repos/{REPO}/issues/comments/{c['id']}",
            method="DELETE",
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        urllib.request.urlopen(del_req)

post = urllib.request.Request(
    api, method="POST",
    data=json.dumps({"body": body}).encode(),
    headers={
        "Authorization": f"Bearer {TOKEN}",
        "Accept": "application/vnd.github+json",
    },
)
urllib.request.urlopen(post)
print("comment posted")
