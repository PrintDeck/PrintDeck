"""Shared entity model for PrintDeck."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import EntityDescription
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .api import PrintDeckPrinter
from .const import DOMAIN
from .coordinator import PrintDeckCoordinator


class PrintDeckEntity(CoordinatorEntity[PrintDeckCoordinator]):
    """Base class for an entity belonging to one configured printer."""

    _attr_has_entity_name = True

    def __init__(
        self,
        coordinator: PrintDeckCoordinator,
        printer: PrintDeckPrinter,
        description: EntityDescription,
    ) -> None:
        super().__init__(coordinator)
        self.entity_description = description
        self._printer_id = printer.printer_id
        self._attr_unique_id = (
            f"{coordinator.data.info.device_id}_{printer.printer_id}_{description.key}"
        )

    @property
    def printer(self) -> PrintDeckPrinter | None:
        """Return the current in-memory state of this entity's printer."""
        return self.coordinator.printer(self._printer_id)

    @property
    def available(self) -> bool:
        """Return whether this data point currently has authoritative data."""
        printer = self.printer
        return (
            super().available
            and printer is not None
            and (
                not printer.stale
                or bool(getattr(self.entity_description, "available_when_stale", False))
            )
        )

    @property
    def device_info(self) -> DeviceInfo:
        """Describe the printer as a child of its PrintDeck device."""
        printer = self.printer
        assert printer is not None
        info = self.coordinator.data.info
        return DeviceInfo(
            identifiers={(DOMAIN, f"{info.device_id}:{printer.printer_id}")},
            name=printer.name,
            manufacturer=printer.manufacturer or "PrintDeck",
            model=printer.model or printer.protocol,
            via_device=(DOMAIN, info.device_id),
            configuration_url=f"http://{self.coordinator.client.host}",
        )
