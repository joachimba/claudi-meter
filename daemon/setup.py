"""py2app setup for Clawdmeter daemon.

Builds a self-contained ClawdmeterDaemon.app bundle whose Info.plist carries
the macOS Bluetooth / Calendar usage-description keys. Without those keys
macOS 26+ TCC kills the process before it can even prompt the user.

Build:
    /usr/bin/python3 -m pip install --user py2app bleak httpx pillow
    cd daemon && /usr/bin/python3 setup.py py2app
    cp -R dist/claude_usage_daemon.app ./ClawdmeterDaemon.app
"""
from setuptools import setup

APP = ["claude_usage_daemon.py"]

OPTIONS = {
    "argv_emulation": False,
    "plist": {
        "CFBundleName": "Clawdmeter Daemon",
        "CFBundleDisplayName": "Clawdmeter Daemon",
        "CFBundleIdentifier": "com.clawdmeter.daemon",
        "CFBundleVersion": "1.0",
        "CFBundleShortVersionString": "1.0",
        "LSUIElement": True,
        "LSBackgroundOnly": True,
        "NSBluetoothAlwaysUsageDescription":
            "Clawdmeter sends Claude Code usage stats to the desk dashboard over BLE.",
        "NSBluetoothPeripheralUsageDescription":
            "Clawdmeter sends Claude Code usage stats to the desk dashboard over BLE.",
        "NSCalendarsFullAccessUsageDescription":
            "Clawdmeter reads your next calendar event so it can be shown on the dashboard.",
        "NSCalendarsUsageDescription":
            "Clawdmeter reads your next calendar event so it can be shown on the dashboard.",
    },
    "packages": ["bleak", "httpx", "PIL"],
    "includes": ["asyncio", "json", "subprocess", "struct", "zlib", "datetime"],
}

setup(
    app=APP,
    options={"py2app": OPTIONS},
    setup_requires=["py2app"],
)
