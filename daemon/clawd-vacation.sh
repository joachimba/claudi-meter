#!/bin/bash
# Vacation countdown config for Clawdmeter. Writes ~/.config/claude-usage-monitor/
# vacation.json (destination + date), which the daemon reads to drive the
# device's countdown screen. Also stores the Unsplash access key.
#
# Usage: clawd-vacation.sh set | clear | key
#   set    — prompt for destination + date (GUI dialogs), write vacation.json
#   clear  — remove vacation.json (device shows "No trip set")
#   key    — prompt for and store the Unsplash access key
set -e

CFG_DIR="$HOME/.config/claude-usage-monitor"
VAC="$CFG_DIR/vacation.json"
KEY="$CFG_DIR/unsplash-key"
mkdir -p "$CFG_DIR"

ask() {  # ask "prompt" "default" -> stdout (empty on cancel)
    osascript -e "text returned of (display dialog \"$1\" default answer \"$2\" with title \"Clawd Vacation\")" 2>/dev/null || true
}

case "${1:-set}" in
    set)
        dest=$(ask "Where are you going? (e.g. Tokyo, Japan)" "")
        [ -z "$dest" ] && exit 0
        default_date=$(date -v+30d +%Y-%m-%d 2>/dev/null || date +%Y-%m-%d)
        date_in=$(ask "Departure date (YYYY-MM-DD):" "$default_date")
        [ -z "$date_in" ] && exit 0
        if ! date -j -f "%Y-%m-%d" "$date_in" >/dev/null 2>&1; then
            osascript -e 'display alert "Invalid date" message "Use YYYY-MM-DD, e.g. 2026-09-01."' >/dev/null 2>&1 || true
            exit 1
        fi
        # Write JSON safely via python (handles quoting/unicode in the name).
        python3 -c 'import json,sys; json.dump({"destination":sys.argv[1],"date":sys.argv[2]}, open(sys.argv[3],"w"))' \
            "$dest" "$date_in" "$VAC"
        osascript -e "display notification \"$dest — $date_in\" with title \"Trip saved\"" >/dev/null 2>&1 || true
        ;;
    clear)
        rm -f "$VAC"
        osascript -e 'display notification "Countdown cleared" with title "Clawd Vacation"' >/dev/null 2>&1 || true
        ;;
    key)
        k=$(ask "Unsplash access key (unsplash.com/developers):" "")
        [ -z "$k" ] && exit 0
        printf '%s\n' "$k" > "$KEY"
        chmod 600 "$KEY"
        osascript -e 'display notification "Unsplash key saved" with title "Clawd Vacation"' >/dev/null 2>&1 || true
        ;;
    *)
        echo "Usage: $0 {set|clear|key}"; exit 1 ;;
esac
