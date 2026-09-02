"""Asynchronous client for the PrintDeck Unified Printer API."""

from __future__ import annotations

import asyncio
import json
from collections.abc import Mapping
from dataclasses import dataclass
from time import monotonic
from typing import Any

from aiohttp import ClientError, ClientSession

from .const import API_VERSION

REQUEST_TIMEOUT_SECONDS = 10
MINIMUM_REQUEST_INTERVAL_SECONDS = 1.05


class PrintDeckApiError(Exception):
    """Base error returned by the PrintDeck client."""


class PrintDeckCannotConnectError(PrintDeckApiError):
    """The PrintDeck device could not be reached."""


class PrintDeckAuthenticationError(PrintDeckApiError):
    """The PrintDeck API token was rejected."""


class PrintDeckApiDisabledError(PrintDeckApiError):
    """The Unified Printer API is disabled on the device."""


class PrintDeckUnsupportedError(PrintDeckApiError):
    """The device does not expose the required API contract."""


class PrintDeckInvalidResponseError(PrintDeckApiError):
    """The device returned malformed or unexpected data."""


class PrintDeckNotFoundError(PrintDeckApiError):
    """The requested endpoint does not exist."""


class PrintDeckRateLimitedError(PrintDeckApiError):
    """The PrintDeck request limit was reached."""

    def __init__(self, retry_after: float) -> None:
        super().__init__("PrintDeck rate limit reached")
        self.retry_after = retry_after


@dataclass(frozen=True)
class PrintDeckInfo:
    """Stable identity and software information for one PrintDeck."""

    device_id: str
    firmware_version: str
    hardware: str
    hostname: str | None
    snapshot_supported: bool


@dataclass(frozen=True)
class PrintDeckPrinter:
    """Normalized state for one printer connected to PrintDeck."""

    printer_id: str
    name: str
    protocol: str
    manufacturer: str | None
    model: str | None
    selected: bool
    connection_state: str
    reachability: str
    detail_level: str
    stale: bool
    phase: str
    activity: str
    job_kind: str
    job_name: str | None
    progress_percent: float
    elapsed_seconds: int
    remaining_seconds: int
    current_layer: int
    total_layers: int
    nozzle_current_c: float | None
    nozzle_target_c: float | None
    bed_current_c: float | None
    bed_target_c: float | None
    chamber_current_c: float | None


def _mapping(value: Any, field: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise PrintDeckInvalidResponseError(f"{field} is not an object")
    return value


def _string(
    value: Any, field: str, *, nullable: bool = False, default: str | None = None
) -> str | None:
    if value is None and nullable:
        return None
    if value is None and default is not None:
        return default
    if not isinstance(value, str):
        raise PrintDeckInvalidResponseError(f"{field} is not a string")
    return value


def _boolean(value: Any, field: str, *, default: bool | None = None) -> bool:
    if value is None and default is not None:
        return default
    if not isinstance(value, bool):
        raise PrintDeckInvalidResponseError(f"{field} is not a boolean")
    return value


def _number(
    value: Any, field: str, *, nullable: bool = False, default: float | None = None
) -> float | None:
    if value is None and nullable:
        return None
    if value is None and default is not None:
        return default
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise PrintDeckInvalidResponseError(f"{field} is not a number")
    return float(value)


def _integer(value: Any, field: str, *, default: int = 0) -> int:
    if value is None:
        return default
    if isinstance(value, bool) or not isinstance(value, int):
        raise PrintDeckInvalidResponseError(f"{field} is not an integer")
    return value


def parse_info(payload: Mapping[str, Any]) -> PrintDeckInfo:
    """Validate and normalize the identity response."""
    if (
        payload.get("api_version") != API_VERSION
        or payload.get("product") != "PrintDeck"
    ):
        raise PrintDeckUnsupportedError("This is not a supported PrintDeck API")
    device_id_value = payload.get("device_id")
    if not isinstance(device_id_value, str) or not device_id_value.startswith(
        "printdeck-"
    ):
        raise PrintDeckUnsupportedError(
            "This PrintDeck firmware has no stable device ID"
        )
    device_id = device_id_value
    network = _mapping(payload.get("network", {}), "network")
    capabilities = _mapping(payload.get("capabilities", {}), "capabilities")
    return PrintDeckInfo(
        device_id=device_id,
        firmware_version=_string(
            payload.get("firmware_version"), "firmware_version", default="unknown"
        )
        or "unknown",
        hardware=_string(payload.get("hardware"), "hardware", default="PrintDeck")
        or "PrintDeck",
        hostname=_string(network.get("hostname"), "network.hostname", nullable=True),
        snapshot_supported=_boolean(
            capabilities.get("snapshot"), "capabilities.snapshot", default=False
        ),
    )


def parse_printer(printer_value: Any, status_value: Any) -> PrintDeckPrinter:
    """Validate and normalize one printer and its status."""
    printer = _mapping(printer_value, "printer")
    status = _mapping(status_value, "status")
    connection = _mapping(status.get("connection"), "status.connection")
    job = _mapping(status.get("job"), "status.job")
    temperatures = _mapping(status.get("temperatures", {}), "status.temperatures")

    printer_id = _integer(printer.get("id"), "printer.id")
    status_printer_id = _integer(status.get("printer_id"), "status.printer_id")
    if printer_id <= 0 or status_printer_id != printer_id:
        raise PrintDeckInvalidResponseError(
            "Printer identity does not match its status"
        )

    progress = _number(
        job.get("progress_percent"), "status.job.progress_percent", default=0.0
    )
    assert progress is not None
    return PrintDeckPrinter(
        printer_id=str(printer_id),
        name=_string(printer.get("name"), "printer.name") or f"Printer {printer_id}",
        protocol=_string(printer.get("protocol"), "printer.protocol", default="unknown")
        or "unknown",
        manufacturer=_string(
            printer.get("manufacturer"), "printer.manufacturer", nullable=True
        ),
        model=_string(printer.get("model"), "printer.model", nullable=True),
        selected=_boolean(printer.get("selected"), "printer.selected", default=False),
        connection_state=_string(
            connection.get("state"), "status.connection.state", default="unknown"
        )
        or "unknown",
        reachability=_string(
            connection.get("reachability"),
            "status.connection.reachability",
            default="unknown",
        )
        or "unknown",
        detail_level=_string(
            connection.get("detail_level"),
            "status.connection.detail_level",
            default="summary",
        )
        or "summary",
        stale=_boolean(
            connection.get("stale"), "status.connection.stale", default=True
        ),
        phase=_string(job.get("phase"), "status.job.phase", default="unknown")
        or "unknown",
        activity=_string(job.get("activity"), "status.job.activity", default="unknown")
        or "unknown",
        job_kind=_string(job.get("kind"), "status.job.kind", default="print")
        or "print",
        job_name=_string(job.get("name"), "status.job.name", nullable=True),
        progress_percent=max(0.0, min(100.0, progress)),
        elapsed_seconds=max(
            0, _integer(job.get("elapsed_seconds"), "status.job.elapsed_seconds")
        ),
        remaining_seconds=max(
            0, _integer(job.get("remaining_seconds"), "status.job.remaining_seconds")
        ),
        current_layer=max(
            0, _integer(job.get("current_layer"), "status.job.current_layer")
        ),
        total_layers=max(
            0, _integer(job.get("total_layers"), "status.job.total_layers")
        ),
        nozzle_current_c=_number(
            temperatures.get("nozzle_current_c"),
            "status.temperatures.nozzle_current_c",
            nullable=True,
        ),
        nozzle_target_c=_number(
            temperatures.get("nozzle_target_c"),
            "status.temperatures.nozzle_target_c",
            nullable=True,
        ),
        bed_current_c=_number(
            temperatures.get("bed_current_c"),
            "status.temperatures.bed_current_c",
            nullable=True,
        ),
        bed_target_c=_number(
            temperatures.get("bed_target_c"),
            "status.temperatures.bed_target_c",
            nullable=True,
        ),
        chamber_current_c=_number(
            temperatures.get("chamber_current_c"),
            "status.temperatures.chamber_current_c",
            nullable=True,
        ),
    )


def parse_snapshot(payload: Mapping[str, Any]) -> tuple[PrintDeckPrinter, ...]:
    """Validate and normalize a complete snapshot response."""
    if payload.get("api_version") != API_VERSION:
        raise PrintDeckUnsupportedError("Unsupported PrintDeck API version")
    values = payload.get("printers")
    if not isinstance(values, list):
        raise PrintDeckInvalidResponseError("printers is not a list")
    parsed: list[PrintDeckPrinter] = []
    for index, value in enumerate(values):
        item = _mapping(value, f"printers[{index}]")
        parsed.append(parse_printer(item.get("printer"), item.get("status")))
    return tuple(parsed)


def parse_legacy_snapshot(
    printers_payload: Mapping[str, Any], statuses_payload: Mapping[str, Any]
) -> tuple[PrintDeckPrinter, ...]:
    """Combine the two endpoints exposed by older Unified API firmware."""
    printers = printers_payload.get("printers")
    statuses = statuses_payload.get("statuses")
    if not isinstance(printers, list) or not isinstance(statuses, list):
        raise PrintDeckInvalidResponseError("Legacy printer response is incomplete")
    status_by_id: dict[int, Mapping[str, Any]] = {}
    for index, value in enumerate(statuses):
        status = _mapping(value, f"statuses[{index}]")
        status_by_id[_integer(status.get("printer_id"), "status.printer_id")] = status
    parsed: list[PrintDeckPrinter] = []
    for index, value in enumerate(printers):
        printer = _mapping(value, f"printers[{index}]")
        printer_id = _integer(printer.get("id"), "printer.id")
        status = status_by_id.get(printer_id)
        if status is None:
            raise PrintDeckInvalidResponseError("Printer status is missing")
        parsed.append(parse_printer(printer, status))
    return tuple(parsed)


class PrintDeckApiClient:
    """Rate-aware client for one PrintDeck device."""

    def __init__(self, session: ClientSession, host: str, token: str) -> None:
        self._session = session
        self._host = host
        self._token = token
        self._request_lock = asyncio.Lock()
        self._last_request_at = 0.0
        self._snapshot_supported: bool | None = None

    @property
    def host(self) -> str:
        """Return the configured host."""
        return self._host

    async def async_get_info(self) -> PrintDeckInfo:
        """Return and validate stable device information."""
        info = parse_info(await self._async_request("/v1/info"))
        self._snapshot_supported = info.snapshot_supported
        return info

    async def async_get_snapshot(self) -> tuple[PrintDeckPrinter, ...]:
        """Return all printer state using the fewest API requests available."""
        if self._snapshot_supported is not False:
            try:
                snapshot = parse_snapshot(await self._async_request("/v1/snapshot"))
            except PrintDeckNotFoundError:
                self._snapshot_supported = False
            else:
                self._snapshot_supported = True
                return snapshot
        printers = await self._async_request("/v1/printers")
        statuses = await self._async_request("/v1/printers/status")
        return parse_legacy_snapshot(printers, statuses)

    async def _async_request(self, path: str) -> Mapping[str, Any]:
        for attempt in range(2):
            try:
                return await self._async_request_once(path)
            except PrintDeckRateLimitedError as err:
                if attempt == 1:
                    raise
                await asyncio.sleep(err.retry_after)
        raise PrintDeckCannotConnectError("Request retry failed")

    async def _async_request_once(self, path: str) -> Mapping[str, Any]:
        async with self._request_lock:
            delay = (
                self._last_request_at + MINIMUM_REQUEST_INTERVAL_SECONDS - monotonic()
            )
            if delay > 0:
                await asyncio.sleep(delay)
            url = f"http://{self._host}{path}"
            try:
                async with self._session.get(
                    url,
                    headers={"Authorization": f"Bearer {self._token}"},
                    timeout=REQUEST_TIMEOUT_SECONDS,
                ) as response:
                    text = await response.text()
                    status = response.status
                    retry_after = response.headers.get("Retry-After", "1")
            except (ClientError, TimeoutError) as err:
                raise PrintDeckCannotConnectError(
                    "Cannot connect to PrintDeck"
                ) from err
            finally:
                self._last_request_at = monotonic()

            if status == 401:
                raise PrintDeckAuthenticationError("Invalid PrintDeck API token")
            if status == 403:
                raise PrintDeckApiDisabledError("Unified Printer API is disabled")
            if status == 404:
                raise PrintDeckNotFoundError("PrintDeck endpoint not found")
            if status == 429:
                try:
                    retry_seconds = max(1.0, float(retry_after))
                except ValueError:
                    retry_seconds = 1.0
                raise PrintDeckRateLimitedError(retry_seconds)
            if status >= 400:
                raise PrintDeckCannotConnectError(f"PrintDeck returned HTTP {status}")
            try:
                payload = json.loads(text)
            except (json.JSONDecodeError, TypeError) as err:
                raise PrintDeckInvalidResponseError(
                    "PrintDeck returned invalid JSON"
                ) from err
            return _mapping(payload, "response")
