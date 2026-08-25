from server.costs import Usage


def test_starts_empty():
    u = Usage()
    assert u.turns == 0 and u.audio_seconds == 0.0 and u.tts_chars == 0


def test_records_a_turn():
    u = Usage()
    u.add_turn(audio_seconds=2.5, prompt_tokens=300, completion_tokens=40, tts_chars=80)
    assert u.turns == 1
    assert u.audio_seconds == 2.5
    assert u.prompt_tokens == 300
    assert u.completion_tokens == 40
    assert u.tts_chars == 80


def test_accumulates_across_turns():
    u = Usage()
    u.add_turn(audio_seconds=1.0, prompt_tokens=100, completion_tokens=10, tts_chars=20)
    u.add_turn(audio_seconds=2.0, prompt_tokens=200, completion_tokens=20, tts_chars=30)
    assert u.turns == 2
    assert u.audio_seconds == 3.0
    assert u.prompt_tokens == 300


def test_total_tokens_sums_prompt_and_completion():
    u = Usage()
    u.add_turn(audio_seconds=0, prompt_tokens=100, completion_tokens=25, tts_chars=0)
    assert u.total_tokens == 125


def test_is_empty_until_a_turn_is_recorded():
    u = Usage()
    assert u.is_empty
    u.add_turn(audio_seconds=0, prompt_tokens=1, completion_tokens=1, tts_chars=1)
    assert not u.is_empty
