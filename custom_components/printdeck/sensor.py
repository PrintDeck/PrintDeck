"""Sensor entities exposed by PrintDeck."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import (
    PERCENTAGE,
    EntityCategory,
    UnitOfTemperature,
    UnitOfTime,
)
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .api import PrintDeckPrinter
from .const import (
    ACTIVITY_OPTIONS,
    CONNECTION_OPTIONS,
    DETAIL_LEVEL_OPTIONS,
    JOB_KIND_OPTIONS,
    PHASE_OPTIONS,
    REACHABILITY_OPTIONS,
)
from .coordinator import PrintDeckCoordinator
from .entity import PrintDeckEntity


@dataclass(frozen=True, kw_only=True)
class PrintDeckSensorEntityDescription(SensorEntityDescription):
    """Describe a PrintDeck sensor."""

    value_fn: Callable[[PrintDeckPrinter], Any]
    available_when_stale: bool = False
    requires_full_detail: bool = False
    none_is_unavailable: bool = False


SENSORS: tuple[PrintDeckSensorEntityDescription, ...] = (
    PrintDeckSensorEntityDescription(
        key="phase",
        translation_key="phase",
        icon="mdi:printer-3d",
        device_class=SensorDeviceClass.ENUM,
        options=PHASE_OPTIONS,
        value_fn=lambda printer: printer.phase,
    ),
    PrintDeckSensorEntityDescription(
        key="activity",
        translation_key="activity",
        icon="mdi:progress-wrench",
        device_class=SensorDeviceClass.ENUM,
        options=ACTIVITY_OPTIONS,
        requires_full_detail=True,
        value_fn=lambda printer: printer.activity,
    ),
    PrintDeckSensorEntityDescription(
        key="progress",
        translation_key="progress",
        icon="mdi:progress-clock",
        native_unit_of_measurement=PERCENTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=1,
        requires_full_detail=True,
        value_fn=lambda printer: printer.progress_percent,
    ),
    PrintDeckSensorEntityDescription(
        key="job_name",
        translation_key="job_name",
        icon="mdi:file-outline",
        value_fn=lambda printer: printer.job_name,
    ),
    PrintDeckSensorEntityDescription(
        key="job_kind",
        translation_key="job_kind",
        icon="mdi:shape-outline",
        device_class=SensorDeviceClass.ENUM,
        options=JOB_KIND_OPTIONS,
        value_fn=lambda printer: printer.job_kind,
    ),
    PrintDeckSensorEntityDescription(
        key="remaining_time",
        translation_key="remaining_time",
        icon="mdi:timer-sand",
        device_class=SensorDeviceClass.DURATION,
        native_unit_of_measurement=UnitOfTime.SECONDS,
        value_fn=lambda printer: printer.remaining_seconds,
    ),
    PrintDeckSensorEntityDescription(
        key="elapsed_time",
        translation_key="elapsed_time",
        icon="mdi:timer-outline",
        device_class=SensorDeviceClass.DURATION,
        native_unit_of_measurement=UnitOfTime.SECONDS,
        requires_full_detail=True,
        value_fn=lambda printer: printer.elapsed_seconds,
    ),
    PrintDeckSensorEntityDescription(
        key="current_layer",
        translation_key="current_layer",
        icon="mdi:layers-outline",
        requires_full_detail=True,
        value_fn=lambda printer: printer.current_layer,
    ),
    PrintDeckSensorEntityDescription(
        key="total_layers",
        translation_key="total_layers",
        icon="mdi:layers-triple-outline",
        requires_full_detail=True,
        value_fn=lambda printer: printer.total_layers,
    ),
    PrintDeckSensorEntityDescription(
        key="nozzle_temperature",
        translation_key="nozzle_temperature",
        icon="mdi:printer-3d-nozzle-heat",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        requires_full_detail=True,
        none_is_unavailable=True,
        value_fn=lambda printer: printer.nozzle_current_c,
    ),
    PrintDeckSensorEntityDescription(
        key="nozzle_target_temperature",
        translation_key="nozzle_target_temperature",
        icon="mdi:printer-3d-nozzle-heat-outline",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        requires_full_detail=True,
        none_is_unavailable=True,
        value_fn=lambda printer: printer.nozzle_target_c,
    ),
    PrintDeckSensorEntityDescription(
        key="bed_temperature",
        translation_key="bed_temperature",
        icon="mdi:radiator",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        requires_full_detail=True,
        none_is_unavailable=True,
        value_fn=lambda printer: printer.bed_current_c,
    ),
    PrintDeckSensorEntityDescription(
        key="bed_target_temperature",
        translation_key="bed_target_temperature",
        icon="mdi:radiator-disabled",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        requires_full_detail=True,
        none_is_unavailable=True,
        value_fn=lambda printer: printer.bed_target_c,
    ),
    PrintDeckSensorEntityDescription(
        key="chamber_temperature",
        translation_key="chamber_temperature",
        icon="mdi:thermometer",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        requires_full_detail=True,
        none_is_unavailable=True,
        value_fn=lambda printer: printer.chamber_current_c,
    ),
    PrintDeckSensorEntityDescription(
        key="connection_state",
        translation_key="connection_state",
        icon="mdi:lan-connect",
        device_class=SensorDeviceClass.ENUM,
        options=CONNECTION_OPTIONS,
        entity_category=EntityCategory.DIAGNOSTIC,
        available_when_stale=True,
        value_fn=lambda printer: printer.connection_state,
    ),
    PrintDeckSensorEntityDescription(
        key="reachability",
        translation_key="reachability",
        icon="mdi:access-point-network",
        device_class=SensorDeviceClass.ENUM,
        options=REACHABILITY_OPTIONS,
        entity_category=EntityCategory.DIAGNOSTIC,
        available_when_stale=True,
        value_fn=lambda printer: printer.reachability,
    ),
    PrintDeckSensorEntityDescription(
        key="detail_level",
        translation_key="detail_level",
        icon="mdi:database-eye-outline",
        device_class=SensorDeviceClass.ENUM,
        options=DETAIL_LEVEL_OPTIONS,
        entity_category=EntityCategory.DIAGNOSTIC,
        available_when_stale=True,
        value_fn=lambda printer: printer.detail_level,
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Set up PrintDeck sensors and discover newly added printer profiles."""
    coordinator: PrintDeckCoordinator = entry.runtime_data
    known_printers: set[str] = set()

    @callback
    def async_add_new_printers() -> None:
        entities: list[PrintDeckSensor] = []
        for printer in coordinator.data.printers:
            if printer.printer_id in known_printers:
                continue
            known_printers.add(printer.printer_id)
            entities.extend(
                PrintDeckSensor(coordinator, printer, description)
                for description in SENSORS
            )
        if entities:
            async_add_entities(entities)

    async_add_new_printers()
    entry.async_on_unload(coordinator.async_add_listener(async_add_new_printers))


class PrintDeckSensor(PrintDeckEntity, SensorEntity):
    """One normalized PrintDeck sensor."""

    entity_description: PrintDeckSensorEntityDescription

    @property
    def available(self) -> bool:
        """Return whether this measurement is authoritative and present."""
        printer = self.printer
        if not super().available or printer is None:
            return False
        if (
            self.entity_description.requires_full_detail
            and printer.detail_level != "full"
        ):
            return False
        return not (
            self.entity_description.none_is_unavailable
            and self.entity_description.value_fn(printer) is None
        )

    @property
    def native_value(self) -> Any:
        """Return the current value without performing I/O."""
        printer = self.printer
        return None if printer is None else self.entity_description.value_fn(printer)
