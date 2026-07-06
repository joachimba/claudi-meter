#!/bin/bash
# Clawdmeter daemon control. Single-command on/off/status/logs.
# Add the alias to your shell rc:
#   alias clawd="/Users/joachimbauters/Documents/NODEJS/ClaudeBuddy/daemon/clawd-ctl.sh"
# Then: clawd start | clawd stop | clawd restart | clawd status | clawd logs
set -e

PLIST="$HOME/Library/LaunchAgents/com.user.claude-usage-daemon.plist"
LABEL="com.user.claude-usage-daemon"
LOG_OUT="$HOME/Library/Logs/claude-usage-daemon.out.log"
LOG_ERR="$HOME/Library/Logs/claude-usage-daemon.err.log"

uid=$(id -u)

ensure_plist() {
    if [[ ! -f "$PLIST" ]]; then
        echo "LaunchAgent missing at $PLIST — re-run ./install-mac.sh."
        exit 1
    fi
}

# Block until launchd reports the label fully unloaded. bootout returns before
# teardown finishes; bootstrapping into a half-torn-down label yields
# "I/O error 5" or a stale "Already loaded". Poll up to ~10s.
wait_unloaded() {
    for _ in $(seq 1 50); do
        launchctl print "gui/$uid/$LABEL" >/dev/null 2>&1 || return 0
        sleep 0.2
    done
    return 1
}

case "${1:-status}" in
    start)
        ensure_plist
        if launchctl print "gui/$uid/$LABEL" >/dev/null 2>&1; then
            echo "Already loaded."
        else
            # Retry: launchd can still be releasing the label right after a stop.
            started=0
            for attempt in 1 2 3 4 5; do
                if launchctl bootstrap "gui/$uid" "$PLIST" 2>/dev/null; then
                    echo "Started."
                    started=1
                    break
                fi
                sleep 1
            done
            if [[ "$started" -ne 1 ]]; then
                echo "Bootstrap failed after retries — check: launchctl bootstrap gui/$uid \"$PLIST\""
                exit 1
            fi
        fi
        ;;
    stop)
        if launchctl print "gui/$uid/$LABEL" >/dev/null 2>&1; then
            launchctl bootout "gui/$uid/$LABEL" || true
        fi
        # Belt + braces: KeepAlive can race the bootout.
        pkill -f claude_usage_daemon 2>/dev/null || true
        # Don't return until launchd has actually released the label, else a
        # following start races the teardown.
        wait_unloaded || echo "Warning: label still loaded after 10s."
        echo "Stopped."
        ;;
    restart)
        "$0" stop
        "$0" start
        ;;
    status)
        if pgrep -fl claude_usage_daemon >/dev/null; then
            echo "Running:"
            pgrep -fl claude_usage_daemon
        else
            echo "Not running."
        fi
        if launchctl print "gui/$uid/$LABEL" >/dev/null 2>&1; then
            echo "LaunchAgent: loaded"
        else
            echo "LaunchAgent: unloaded"
        fi
        ;;
    logs)
        tail -F "$LOG_OUT" "$LOG_ERR"
        ;;
    tail)
        tail -n "${2:-50}" "$LOG_OUT"
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status|logs|tail [N]}"
        exit 1
        ;;
esac
