"""Data coordinator for the PrintDeck integration."""

from __future__ import annotations

import logging
from dataclasses import dataclass

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import (
    PrintDeckApiClient,
    PrintDeckApiDisabledError,
    PrintDeckApiError,
    PrintDeckAuthenticationError,
    PrintDeckInfo,
    PrintDeckPrinter,
    PrintDeckRateLimitedError,
    PrintDeckUnsupportedError,
)
from .const import DEFAULT_UPDATE_INTERVAL, DOMAIN

_LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True)
class PrintDeckCoordinatorData:
    """Last complete snapshot received from PrintDeck."""

    info: PrintDeckInfo
    printers: tuple[PrintDeckPrinter, ...]


class PrintDeckCoordinator(DataUpdateCoordinator[PrintDeckCoordinatorData]):
    """Poll one aggregate endpoint and fan its state out to all entities."""

    def __init__(
        self, hass: HomeAssistant, entry: ConfigEntry, client: PrintDeckApiClient
    ) -> None:
        super().__init__(
            hass,
            _LOGGER,
            config_entry=entry,
            name=DOMAIN,
            update_interval=DEFAULT_UPDATE_INTERVAL,
            always_update=False,
        )
        self.client = client
        self.info: PrintDeckInfo | None = None

    async def _async_setup(self) -> None:
        try:
            self.info = await self.client.async_get_info()
        except PrintDeckAuthenticationError as err:
            raise ConfigEntryAuthFailed from err
        except PrintDeckApiError as err:
            raise UpdateFailed(str(err)) from err

    async def _async_update_data(self) -> PrintDeckCoordinatorData:
        assert self.info is not None
        try:
            printers = await self.client.async_get_snapshot()
        except PrintDeckAuthenticationError as err:
            raise ConfigEntryAuthFailed from err
        except PrintDeckRateLimitedError as err:
            raise UpdateFailed("PrintDeck request limit reached") from err
        except PrintDeckApiDisabledError as err:
            raise UpdateFailed("Unified Printer API is disabled") from err
        except PrintDeckUnsupportedError as err:
            raise UpdateFailed("PrintDeck API is not supported") from err
        except PrintDeckApiError as err:
            raise UpdateFailed(str(err)) from err
        return PrintDeckCoordinatorData(info=self.info, printers=printers)

    def printer(self, printer_id: str) -> PrintDeckPrinter | None:
        """Return one printer from the in-memory snapshot."""
        if self.data is None:
            return None
        return next(
            (
                printer
                for printer in self.data.printers
                if printer.printer_id == printer_id
            ),
            None,
        )
