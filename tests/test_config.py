from server.config import Settings


def _settings(**kw):
    return Settings(_env_file=None, **kw)


def test_defaults_match_the_spec():
    s = _settings()
    assert s.max_context_tokens == 2000
    assert s.session_timeout_s == 60
    assert s.log_level == "info"
    assert s.sample_rate == 16000
    assert s.max_tokens == 150


def test_environment_overrides_defaults(monkeypatch):
    monkeypatch.setenv("MAX_CONTEXT_TOKENS", "500")
    monkeypatch.setenv("SESSION_TIMEOUT_S", "12")
    s = Settings()
    assert s.max_context_tokens == 500
    assert s.session_timeout_s == 12


def test_utterance_cap_is_derived_from_the_session_timeout():
    s = _settings(session_timeout_s=10)
    assert s.max_utterance_bytes == 10 * 16000 * 2


def test_auth_is_disabled_when_no_device_token_is_set():
    assert _settings().auth_required is False


def test_auth_is_required_once_a_device_token_is_set():
    assert _settings(device_token="s3cret").auth_required is True
