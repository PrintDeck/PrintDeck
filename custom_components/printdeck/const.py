"""Constants for the PrintDeck integration."""

from datetime import timedelta

DOMAIN = "printdeck"
PLATFORMS = ["binary_sensor", "sensor"]

CONF_TOKEN = "token"
DEFAULT_HOST = "printdeck.local"
DEFAULT_UPDATE_INTERVAL = timedelta(seconds=10)

API_VERSION = "v1"
ZEROCONF_TYPE = "_printdeck._tcp.local."

PHASE_OPTIONS = [
    "unknown",
    "idle",
    "preparing",
    "printing",
    "paused",
    "completed",
    "failed",
    "cancelled",
]

ACTIVITY_OPTIONS = [
    "unknown",
    "standby",
    "preparing",
    "nozzle_heating",
    "bed_heating",
    "homing",
    "bed_leveling",
    "nozzle_cleaning",
    "calibrating",
    "filament_changing",
    "filament_unloading",
    "filament_loading",
    "filament_purging",
    "printing",
    "paused",
    "completed",
    "failed",
    "cancelled",
]

CONNECTION_OPTIONS = [
    "unknown",
    "stopped",
    "waiting_for_network",
    "connecting",
    "online",
    "offline",
]
REACHABILITY_OPTIONS = ["unknown", "online", "offline"]
DETAIL_LEVEL_OPTIONS = ["summary", "full"]
JOB_KIND_OPTIONS = ["print", "calibration"]
