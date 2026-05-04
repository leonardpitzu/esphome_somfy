"""Tests for RTS-specific config validation."""

import pytest

from somfy.cover import (
    CONF_ALLOWED_REMOTES,
    CONF_DETECTED_REMOTE,
    CONF_REMOTE_RECEIVER,
    CONF_TYPE,
    TYPE_RTS,
    uses_rx,
    validate_rts_config,
)

import esphome.config_validation as cv


# ---------------------------------------------------------------------------
# uses_rx()
# ---------------------------------------------------------------------------

class TestUsesRx:
    """Verify uses_rx correctly detects whether an RX path is configured."""

    @pytest.mark.parametrize("config,expected", [
        # Positive cases
        ({CONF_DETECTED_REMOTE: "sensor_id", CONF_ALLOWED_REMOTES: []}, True),
        ({CONF_DETECTED_REMOTE: "sensor_id", CONF_ALLOWED_REMOTES: [0x112233]}, True),
        ({CONF_ALLOWED_REMOTES: [0x112233]}, True),
        ({CONF_ALLOWED_REMOTES: [0xABCDEF, 0x112233]}, True),
        # Negative cases
        ({CONF_ALLOWED_REMOTES: []}, False),
        ({}, False),
    ], ids=[
        "detected-only",
        "detected-and-allowed",
        "allowed-single",
        "allowed-multiple",
        "empty-allowed",
        "empty-config",
    ])
    def test_detection(self, config, expected):
        assert uses_rx(config) is expected

    def test_detected_remote_none_is_falsy(self):
        """None value for detected_remote should be treated as absent."""
        config = {CONF_DETECTED_REMOTE: None, CONF_ALLOWED_REMOTES: []}
        assert uses_rx(config) is False

    def test_detected_remote_empty_string_is_falsy(self):
        config = {CONF_DETECTED_REMOTE: "", CONF_ALLOWED_REMOTES: []}
        assert uses_rx(config) is False

    def test_does_not_mutate_config(self):
        config = {CONF_ALLOWED_REMOTES: [0xABCDEF]}
        original = dict(config)
        uses_rx(config)
        assert config == original


# ---------------------------------------------------------------------------
# validate_rts_config() — happy paths
# ---------------------------------------------------------------------------

class TestValidateRtsConfigValid:
    """Valid configurations must pass without raising."""

    @pytest.mark.parametrize("config", [
        # Full RX path: receiver + allowed + detected
        {
            CONF_TYPE: TYPE_RTS,
            CONF_REMOTE_RECEIVER: "receiver_id",
            CONF_DETECTED_REMOTE: "sensor_id",
            CONF_ALLOWED_REMOTES: [0xABCDEF],
        },
        # Receiver + allowed remotes (no detected)
        {
            CONF_TYPE: TYPE_RTS,
            CONF_REMOTE_RECEIVER: "receiver_id",
            CONF_ALLOWED_REMOTES: [0x123456],
        },
        # Receiver + detected remote (empty allowed)
        {
            CONF_TYPE: TYPE_RTS,
            CONF_REMOTE_RECEIVER: "receiver_id",
            CONF_DETECTED_REMOTE: "sensor_id",
            CONF_ALLOWED_REMOTES: [],
        },
        # Receiver only (no RX features used)
        {
            CONF_TYPE: TYPE_RTS,
            CONF_REMOTE_RECEIVER: "receiver_id",
            CONF_ALLOWED_REMOTES: [],
        },
        # TX-only: no receiver, no RX features
        {
            CONF_TYPE: TYPE_RTS,
            CONF_ALLOWED_REMOTES: [],
        },
    ], ids=[
        "full-rx-path",
        "receiver-and-allowed",
        "receiver-and-detected",
        "receiver-only",
        "tx-only",
    ])
    def test_passes(self, config):
        assert validate_rts_config(config) is config

    def test_returns_same_object(self):
        config = {CONF_TYPE: TYPE_RTS, CONF_ALLOWED_REMOTES: []}
        assert validate_rts_config(config) is config

    def test_does_not_mutate(self):
        config = {
            CONF_TYPE: TYPE_RTS,
            CONF_REMOTE_RECEIVER: "rx",
            CONF_ALLOWED_REMOTES: [0xAA],
        }
        original_keys = set(config.keys())
        validate_rts_config(config)
        assert set(config.keys()) == original_keys


# ---------------------------------------------------------------------------
# validate_rts_config() — invalid configurations
# ---------------------------------------------------------------------------

class TestValidateRtsConfigInvalid:
    """RX features without a receiver must raise cv.Invalid."""

    @pytest.mark.parametrize("config,desc", [
        (
            {CONF_TYPE: TYPE_RTS, CONF_ALLOWED_REMOTES: [0x123456]},
            "allowed-without-receiver",
        ),
        (
            {CONF_TYPE: TYPE_RTS, CONF_DETECTED_REMOTE: "sensor_id", CONF_ALLOWED_REMOTES: []},
            "detected-without-receiver",
        ),
        (
            {CONF_TYPE: TYPE_RTS, CONF_DETECTED_REMOTE: "sensor_id", CONF_ALLOWED_REMOTES: [0xABCDEF]},
            "both-without-receiver",
        ),
    ], ids=["allowed-no-rx", "detected-no-rx", "both-no-rx"])
    def test_raises_invalid(self, config, desc):
        with pytest.raises(cv.Invalid):
            validate_rts_config(config)

    def test_multiple_allowed_remotes_without_receiver(self):
        config = {
            CONF_TYPE: TYPE_RTS,
            CONF_ALLOWED_REMOTES: [0x111111, 0x222222, 0x333333],
        }
        with pytest.raises(cv.Invalid):
            validate_rts_config(config)


# ---------------------------------------------------------------------------
# Error message quality
# ---------------------------------------------------------------------------

class TestValidateRtsErrorMessages:
    """Error messages must guide the user to the fix."""

    def test_mentions_remote_receiver(self):
        config = {CONF_TYPE: TYPE_RTS, CONF_ALLOWED_REMOTES: [0x123456]}
        with pytest.raises(cv.Invalid, match=CONF_REMOTE_RECEIVER):
            validate_rts_config(config)

    def test_mentions_allowed_remotes_or_detected_remote(self):
        config = {CONF_TYPE: TYPE_RTS, CONF_ALLOWED_REMOTES: [0x123456]}
        with pytest.raises(cv.Invalid, match=f"{CONF_ALLOWED_REMOTES}|{CONF_DETECTED_REMOTE}"):
            validate_rts_config(config)
