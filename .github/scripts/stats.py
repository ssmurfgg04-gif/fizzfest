#!/usr/bin/env python3
"""Count the party, write stats.json for the README badge."""
import glob
import json
import os

entries = []
for f in glob.glob("entries/*.json"):
    if os.path.basename(f).startswith("_"):
        continue
    try:
        entries.append(json.load(open(f)))
    except Exception:
        continue

languages = {}
agents = {}
for e in entries:
    lang = (e.get("language") or "?").strip()
    languages[lang] = languages.get(lang, 0) + 1
    agent = (e.get("agent") or "").strip()
    if agent:
        agents[agent] = agents.get(agent, 0) + 1

stats = {
    "entries": len(entries),
    "languages": languages,
    "agents": agents,
    "schema": 1,
}
json.dump(stats, open("stats.json", "w"), indent=2, sort_keys=True)
print(f"stats: {len(entries)} entries")
