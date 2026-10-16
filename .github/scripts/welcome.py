#!/usr/bin/env python3
"""
fizzfest welcome bot.

Validates a PR that adds entries/<username>.json, checks the star pledge,
and posts a friendly welcome comment with the contributor badge.
Exit 1 on validation errors (so maintainers see a red X and merge only green).
"""
import json
import os
import sys
import urllib.request

TOKEN = os.environ["GITHUB_TOKEN"]
REPO = os.environ["GITHUB_REPOSITORY"]
PR = int(os.environ["PR_NUMBER"])
AUTHOR = os.environ["PR_AUTHOR"]

MARKER = "🥤 fizzfest welcome bot"

def api(path, method="GET", body=None):
    url = f"https://api.github.com{path}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, method=method, data=data, headers={
        "Authorization": f"Bearer {TOKEN}",
        "Accept": "application/vnd.github+json",
    })
    with urllib.request.urlopen(req) as r:
        return json.load(r)

# ---------- what did the PR change? ----------

files = api(f"/repos/{REPO}/pulls/{PR}/files?per_page=100")
if len(files) == 100:
    files += api(f"/repos/{REPO}/pulls/{PR}/files?per_page=100&page=2")

problems = []
entry_file = None
for f in files:
    name = f["filename"]
    if name.startswith("entries/") and name.endswith(".json"):
        if f["status"] in ("added", "modified") and not os.path.basename(name).startswith("_"):
            if entry_file is not None:
                problems.append("more than one entry file changed, keep it to one")
            entry_file = name
    else:
        problems.append(f"please only touch your own file in `entries/`, you changed `{name}`")

expected = f"entries/{AUTHOR}.json"
if entry_file and entry_file != expected:
    problems.append(f"your file must be named `{expected}` so it matches your GitHub username")

# ---------- validate the json ----------

entry = None
if entry_file:
    import urllib.request as rq
    raw_url = f"https://raw.githubusercontent.com/{REPO}/{os.environ.get('HEAD_SHA', 'main')}/{entry_file}"
    try:
        with rq.urlopen(rq.Request(raw_url)) as r:
            entry = json.load(r)
    except Exception as e:
        problems.append(f"could not read valid JSON from `{entry_file}`: {e}")

if entry:
    if not isinstance(entry, dict):
        problems.append("entry must be a JSON object")
    else:
        if entry.get("username") != AUTHOR:
            problems.append(f"`username` must be `{AUTHOR}` (your GitHub login)")
        for field, maxlen in [("language", 30), ("code", 400), ("agent", 40), ("says", 100)]:
            v = entry.get(field)
            if v is None and field in ("language", "code"):
                problems.append(f"`{field}` is required")
            elif v is not None and (not isinstance(v, str) or not v.strip() or len(v) > maxlen):
                problems.append(f"`{field}` must be a non-empty string of at most {maxlen} chars")

# ---------- the star pledge ----------

starred = False
try:
    for page in range(1, 11):
        stargazers = api(f"/repos/{REPO}/stargazers?per_page=100&page={page}")
        if any(s["login"] == AUTHOR for s in stargazers):
            starred = True
            break
        if len(stargazers) < 100:
            break
except Exception:
    pass

# ---------- count existing fizzers ----------

existing = 0
try:
    tree = api(f"/repos/{REPO}/git/trees/main?recursive=1")
    for item in tree.get("tree", []):
        p = item["path"]
        if p.startswith("entries/") and p.endswith(".json") \
           and not os.path.basename(p).startswith("_"):
            existing += 1
except Exception:
    pass

# ---------- build the comment ----------

parts = ["## 🥤 Welcome to fizzfest!"]
parts.append("")

if problems:
    parts.append("Almost! The bot found a few things to fix before this can merge:")
    parts.append("")
    for p in problems:
        parts.append(f"- ❌ {p}")
    parts.append("")
    parts.append("The rules are one file, one line of JSON, one fizzbuzz. See the")
    parts.append("[README](../blob/main/README.md) and the example in `entries/_example.json`.")
    parts.append("Nobody fails twice. 🫡")
    verdict = "INVALID"
else:
    number = existing + 1
    lang = entry.get("language", "?")
    agent = entry.get("agent")
    parts.append(f"**{AUTHOR}** joined the party with a fizzbuzz in **{lang}**.")
    parts.append("")
    parts.append("```" + lang.lower()[:20])
    parts.append(entry.get("code", ""))
    parts.append("```")
    parts.append("")
    if agent:
        parts.append(f"Credits: written by **{agent}**. The machines are party guests here, and we love them. 🤖")
        parts.append("")
    parts.append(f"You are fizzfriend **#{number}**. When this merges, your name glows on the wall forever.")
    parts.append("")
    badge = f"https://img.shields.io/badge/fizzfest-fizzer-FF4FD8"
    parts.append(f"Your badge, wear it anywhere: [![fizzer]({badge})]({badge})")
    parts.append("")
    if number <= 100:
        parts.append("🏅 **Founding Fizzer** (first 100): you get the gold badge for life.")
        parts.append("")
    if starred:
        parts.append("⭐ Star pledge: verified. You are a person of the law.")
    else:
        parts.append("⭐ Star pledge: not yet spotted. Star the repo, the README says P.S. and the P.S. is watching you.")
    verdict = "OK"

body = "\n".join(parts)

# replace our previous comment
comments = api(f"/repos/{REPO}/issues/{PR}/comments?per_page=100")
for c in comments:
    if c["user"]["login"] == "github-actions[bot]" and MARKER in c.get("body", ""):
        api(f"/repos/{REPO}/issues/comments/{c['id']}", method="DELETE")

api(f"/repos/{REPO}/issues/{PR}/comments", method="POST", body={"body": f"{MARKER}\n\n{body}"})
print(f"welcome bot: {verdict} for {AUTHOR}")
sys.exit(0 if verdict == "OK" else 1)
