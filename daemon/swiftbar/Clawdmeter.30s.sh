#!/bin/bash
# SwiftBar plugin for Clawdmeter daemon.
# <swiftbar.title>Clawdmeter</swiftbar.title>
# <swiftbar.author>Joachim</swiftbar.author>
# <swiftbar.refreshOnOpen>true</swiftbar.refreshOnOpen>

CTL="/Users/joachimbauters/Documents/NODEJS/ClaudeBuddy/daemon/clawd-ctl.sh"
VAC="/Users/joachimbauters/Documents/NODEJS/ClaudeBuddy/daemon/clawd-vacation.sh"
LOG_OUT="$HOME/Library/Logs/claude-usage-daemon.out.log"
VAC_JSON="$HOME/.config/claude-usage-monitor/vacation.json"

if pgrep -fq claude_usage_daemon 2>/dev/null; then
    STATE="running"
    ICON="🟢"
else
    STATE="stopped"
    ICON="🔴"
fi

echo "$ICON Clawd"
echo "---"
echo "Clawdmeter daemon: $STATE | size=14"
echo "---"

if [[ "$STATE" == "running" ]]; then
    last=$(tail -n 1 "$LOG_OUT" 2>/dev/null | cut -c1-80)
    [[ -n "$last" ]] && echo "$last | color=gray font=Menlo size=11"
    echo "---"
    echo "Stop | shell=$CTL | param0=stop | terminal=false | refresh=true"
    echo "Restart | shell=$CTL | param0=restart | terminal=false | refresh=true"
else
    echo "Start | shell=$CTL | param0=start | terminal=false | refresh=true"
fi

echo "---"
# Vacation countdown
if [[ -f "$VAC_JSON" ]]; then
    trip=$(python3 -c 'import json,sys,datetime as d
try:
 c=json.load(open(sys.argv[1])); t=d.date.fromisoformat(c["date"]); n=(t-d.date.today()).days
 print(f"✈️ {c[\"destination\"]} — {n}d")
except Exception: print("")' "$VAC_JSON" 2>/dev/null)
    [[ -n "$trip" ]] && echo "$trip | size=13"
    echo "Change trip… | shell=$VAC | param0=set | terminal=false | refresh=true"
    echo "Clear trip | shell=$VAC | param0=clear | terminal=false | refresh=true"
else
    echo "✈️ Set vacation… | shell=$VAC | param0=set | terminal=false | refresh=true"
fi
echo "Set Unsplash key… | shell=$VAC | param0=key | terminal=false"
echo "---"
echo "Tail logs | shell=$CTL | param0=logs | terminal=true"
echo "Open log file | shell=open | param0=$LOG_OUT | terminal=false"
echo "Status | shell=$CTL | param0=status | terminal=true"
echo "---"
echo "Refresh now | refresh=true"
