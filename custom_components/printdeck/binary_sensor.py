"""Binary sensor entities exposed by PrintDeck."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
    BinarySensorEntityDescription,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .api import PrintDeckPrinter
from .coordinator import PrintDeckCoordinator
from .entity import PrintDeckEntity


@dataclass(frozen=True, kw_only=True)
class PrintDeckBinarySensorEntityDescription(BinarySensorEntityDescription):
    """Describe a PrintDeck binary sensor."""

    value_fn: Callable[[PrintDeckPrinter], bool]
    available_when_stale: bool = True


BINARY_SENSORS: tuple[PrintDeckBinarySensorEntityDescription, ...] = (
    PrintDeckBinarySensorEntityDescription(
        key="online",
        translation_key="online",
        device_class=BinarySensorDeviceClass.CONNECTIVITY,
        value_fn=lambda printer: printer.reachability == "online",
    ),
    PrintDeckBinarySensorEntityDescription(
        key="selected",
        translation_key="selected",
        icon="mdi:star-check",
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda printer: printer.selected,
    ),
    PrintDeckBinarySensorEntityDescription(
        key="data_stale",
        translation_key="data_stale",
        icon="mdi:database-clock-outline",
        device_class=BinarySensorDeviceClass.PROBLEM,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda printer: printer.stale,
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Set up PrintDeck binary sensors and newly added printer profiles."""
    coordinator: PrintDeckCoordinator = entry.runtime_data
    known_printers: set[str] = set()

    @callback
    def async_add_new_printers() -> None:
        entities: list[PrintDeckBinarySensor] = []
        for printer in coordinator.data.printers:
            if printer.printer_id in known_printers:
                continue
            known_printers.add(printer.printer_id)
            entities.extend(
                PrintDeckBinarySensor(coordinator, printer, description)
                for description in BINARY_SENSORS
            )
        if entities:
            async_add_entities(entities)

    async_add_new_printers()
    entry.async_on_unload(coordinator.async_add_listener(async_add_new_printers))


class PrintDeckBinarySensor(PrintDeckEntity, BinarySensorEntity):
    """One normalized PrintDeck binary sensor."""

    entity_description: PrintDeckBinarySensorEntityDescription

    @property
    def is_on(self) -> bool | None:
        """Return the current boolean state without performing I/O."""
        printer = self.printer
        return None if printer is None else self.entity_description.value_fn(printer)
