"""Privacy-preserving diagnostics for PrintDeck."""

from __future__ import annotations

from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.redact import async_redact_data

from .const import CONF_TOKEN
from .coordinator import PrintDeckCoordinator


async def async_get_config_entry_diagnostics(
    hass: HomeAssistant, entry: ConfigEntry
) -> dict[str, Any]:
    """Return useful diagnostics without token, addresses or print names."""
    coordinator: PrintDeckCoordinator = entry.runtime_data
    data = coordinator.data
    return {
        "entry": async_redact_data(dict(entry.data), {CONF_TOKEN, "host"}),
        "device": {
            "device_id": data.info.device_id,
            "firmware_version": data.info.firmware_version,
            "hardware": data.info.hardware,
            "snapshot_supported": data.info.snapshot_supported,
        },
        "last_update_success": coordinator.last_update_success,
        "printers": [
            {
                "printer_id": printer.printer_id,
                "protocol": printer.protocol,
                "selected": printer.selected,
                "connection_state": printer.connection_state,
                "reachability": printer.reachability,
                "detail_level": printer.detail_level,
                "stale": printer.stale,
                "phase": printer.phase,
                "activity": printer.activity,
            }
            for printer in data.printers
        ],
    }
