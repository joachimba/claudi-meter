#!/bin/bash
# SwiftBar plugin for Clawdmeter daemon.
# <swiftbar.title>Clawdmeter</swiftbar.title>
# <swiftbar.author>Joachim</swiftbar.author>
# <swiftbar.refreshOnOpen>true</swiftbar.refreshOnOpen>

CTL="/Users/joachimbauters/Documents/NODEJS/ClaudeBuddy/daemon/clawd-ctl.sh"
LOG_OUT="$HOME/Library/Logs/claude-usage-daemon.out.log"

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
echo "Tail logs | shell=$CTL | param0=logs | terminal=true"
echo "Open log file | shell=open | param0=$LOG_OUT | terminal=false"
echo "Status | shell=$CTL | param0=status | terminal=true"
echo "---"
echo "Refresh now | refresh=true"
