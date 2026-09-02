"""Config flow for the PrintDeck integration."""

from __future__ import annotations

import logging
from collections.abc import Mapping
from typing import Any
from urllib.parse import urlsplit

import voluptuous as vol
from homeassistant import config_entries
from homeassistant.config_entries import ConfigEntry, ConfigFlowResult
from homeassistant.const import CONF_HOST
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.selector import (
    TextSelector,
    TextSelectorConfig,
    TextSelectorType,
)
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import (
    PrintDeckApiClient,
    PrintDeckApiDisabledError,
    PrintDeckApiError,
    PrintDeckAuthenticationError,
    PrintDeckCannotConnectError,
    PrintDeckInfo,
    PrintDeckInvalidResponseError,
    PrintDeckUnsupportedError,
)
from .const import CONF_TOKEN, DEFAULT_HOST, DOMAIN

_LOGGER = logging.getLogger(__name__)


def _normalize_host(value: str) -> str:
    host = value.strip()
    if "://" in host:
        parsed = urlsplit(host)
        if parsed.scheme != "http" or not parsed.netloc or parsed.path not in ("", "/"):
            raise ValueError("PrintDeck requires a local HTTP host without a path")
        host = parsed.netloc
    return host.rstrip("/").rstrip(".")


def _data_schema(defaults: Mapping[str, Any] | None = None) -> vol.Schema:
    values = defaults or {}
    return vol.Schema(
        {
            vol.Required(
                CONF_HOST, default=values.get(CONF_HOST, DEFAULT_HOST)
            ): TextSelector(TextSelectorConfig(type=TextSelectorType.TEXT)),
            vol.Required(CONF_TOKEN, default=values.get(CONF_TOKEN, "")): TextSelector(
                TextSelectorConfig(type=TextSelectorType.PASSWORD)
            ),
        }
    )


def _token_schema() -> vol.Schema:
    return vol.Schema(
        {
            vol.Required(CONF_TOKEN): TextSelector(
                TextSelectorConfig(type=TextSelectorType.PASSWORD)
            )
        }
    )


class PrintDeckConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Handle setup, discovery, reauthentication and reconfiguration."""

    VERSION = 1

    def __init__(self) -> None:
        self._discovered_host: str | None = None

    async def _async_validate(self, host: str, token: str) -> PrintDeckInfo:
        client = PrintDeckApiClient(async_get_clientsession(self.hass), host, token)
        return await client.async_get_info()

    async def _async_entry_from_input(
        self, user_input: dict[str, Any], *, reconfigure: bool = False
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        try:
            host = _normalize_host(user_input[CONF_HOST])
            token = user_input[CONF_TOKEN].strip()
            if not host or not token:
                raise ValueError
            info = await self._async_validate(host, token)
        except ValueError:
            errors["base"] = "invalid_input"
        except PrintDeckAuthenticationError:
            errors["base"] = "invalid_auth"
        except PrintDeckApiDisabledError:
            errors["base"] = "api_disabled"
        except PrintDeckCannotConnectError:
            errors["base"] = "cannot_connect"
        except PrintDeckUnsupportedError:
            errors["base"] = "unsupported_firmware"
        except PrintDeckInvalidResponseError:
            errors["base"] = "invalid_response"
        except PrintDeckApiError:
            errors["base"] = "cannot_connect"
        except Exception:
            _LOGGER.exception("Unexpected exception while connecting to PrintDeck")
            errors["base"] = "unknown"
        else:
            await self.async_set_unique_id(info.device_id)
            data = {CONF_HOST: host, CONF_TOKEN: token}
            if reconfigure:
                self._abort_if_unique_id_mismatch()
                return self.async_update_reload_and_abort(
                    self._get_reconfigure_entry(), data_updates=data
                )
            self._abort_if_unique_id_configured(updates={CONF_HOST: host})
            suffix = info.device_id.removeprefix("printdeck-")[-6:].upper()
            return self.async_create_entry(title=f"PrintDeck {suffix}", data=data)

        return self.async_show_form(
            step_id="reconfigure" if reconfigure else "user",
            data_schema=_data_schema(user_input),
            errors=errors,
            description_placeholders={
                "configuration_url": f"http://{user_input.get(CONF_HOST, DEFAULT_HOST)}"
            },
        )

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Handle manually adding a PrintDeck."""
        if user_input is not None:
            return await self._async_entry_from_input(user_input)
        return self.async_show_form(
            step_id="user",
            data_schema=_data_schema(),
            description_placeholders={"configuration_url": f"http://{DEFAULT_HOST}"},
        )

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        """Handle a PrintDeck announced through mDNS."""
        device_id = discovery_info.properties.get("id")
        if not device_id or not device_id.startswith("printdeck-"):
            return self.async_abort(reason="invalid_discovery")
        await self.async_set_unique_id(device_id)
        self._abort_if_unique_id_configured(updates={CONF_HOST: discovery_info.host})
        self._discovered_host = discovery_info.host.rstrip(".")
        suffix = device_id.removeprefix("printdeck-")[-6:].upper()
        self.context["title_placeholders"] = {"name": f"PrintDeck {suffix}"}
        self.context["configuration_url"] = f"http://{self._discovered_host}"
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Ask only for the API token after automatic discovery."""
        assert self._discovered_host is not None
        errors: dict[str, str] = {}
        if user_input is not None:
            complete_input = {
                CONF_HOST: self._discovered_host,
                CONF_TOKEN: user_input[CONF_TOKEN],
            }
            result = await self._async_entry_from_input(complete_input)
            if result["type"] != "form":
                return result
            errors = result.get("errors", {})
        return self.async_show_form(
            step_id="zeroconf_confirm",
            data_schema=_token_schema(),
            errors=errors,
            description_placeholders={
                "name": self.context.get("title_placeholders", {}).get(
                    "name", "PrintDeck"
                ),
                "host": self._discovered_host,
                "configuration_url": f"http://{self._discovered_host}",
            },
        )

    async def async_step_reauth(
        self, entry_data: Mapping[str, Any]
    ) -> ConfigFlowResult:
        """Start reauthentication after a rejected API token."""
        self.context["title_placeholders"] = {"name": self._get_reauth_entry().title}
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Replace a rejected API token."""
        entry = self._get_reauth_entry()
        errors: dict[str, str] = {}
        if user_input is not None:
            token = user_input[CONF_TOKEN].strip()
            try:
                info = await self._async_validate(entry.data[CONF_HOST], token)
            except PrintDeckAuthenticationError:
                errors["base"] = "invalid_auth"
            except PrintDeckApiDisabledError:
                errors["base"] = "api_disabled"
            except PrintDeckUnsupportedError:
                errors["base"] = "unsupported_firmware"
            except PrintDeckApiError:
                errors["base"] = "cannot_connect"
            else:
                await self.async_set_unique_id(info.device_id)
                self._abort_if_unique_id_mismatch()
                return self.async_update_reload_and_abort(
                    entry, data_updates={CONF_TOKEN: token}
                )
        return self.async_show_form(
            step_id="reauth_confirm", data_schema=_token_schema(), errors=errors
        )

    async def async_step_reconfigure(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Change the host or API token from the integration UI."""
        entry: ConfigEntry = self._get_reconfigure_entry()
        if user_input is not None:
            return await self._async_entry_from_input(user_input, reconfigure=True)
        return self.async_show_form(
            step_id="reconfigure",
            data_schema=_data_schema(entry.data),
            description_placeholders={
                "configuration_url": f"http://{entry.data[CONF_HOST]}"
            },
        )
