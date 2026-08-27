from server.persona import build_system_prompt


def test_caps_reply_length():
    """Brevity is the persona's whole job; the exact bound has been tightened.

    The spec asked for 1-3 sentences. Measured against gpt-oss that produced
    253 characters - fifteen seconds of speech, during which the device ignores
    its button - so the instruction now says one or two, with a word count.
    """
    prompt = build_system_prompt([]).lower().replace("\u2013", "-")
    assert "one or two short sentences" in prompt
    assert "thirty words" in prompt
    assert "never longer" in prompt


def test_instructs_to_answer_in_the_users_language():
    prompt = build_system_prompt([]).lower()
    assert "language" in prompt


def test_includes_remembered_facts():
    prompt = build_system_prompt(["Lives in Kyiv", "Works on embedded audio"])
    assert "Lives in Kyiv" in prompt
    assert "Works on embedded audio" in prompt


def test_omits_memory_section_when_there_are_no_facts():
    assert "remember" not in build_system_prompt([]).lower()


def test_stays_small_enough_to_leave_room_for_history():
    from server.context import estimate_tokens

    facts = [f"Fact number {i}" for i in range(5)]
    assert estimate_tokens(build_system_prompt(facts)) < 400


def test_asks_for_an_emotion_tag_naming_every_emotion():
    """The nine names have to match server.emotion, which has to match face.h.
    A name the prompt offers but the vocabulary does not know is a face that
    silently keeps its old expression."""
    from server.emotion import EMOTIONS

    prompt = build_system_prompt([])
    for name in EMOTIONS:
        assert f"[{name}]" in prompt, name


def test_puts_the_tag_rule_before_everything_else():
    """It has to be the very first token of the reply, so it is the first
    thing the model is told.

    Asserts the position and the subject, not the wording: the wording is
    deliberately tuned and a test pinned to it would have to be edited every
    time it is, which teaches everyone to edit the test rather than read it.
    """
    prompt = build_system_prompt([])
    first_rule = prompt.split("Rules:", 1)[1].strip().split("\n")[0]
    assert first_rule.startswith("-")
    assert "square brackets" in first_rule
    assert "before any words" in first_rule


def test_says_the_tag_is_not_to_be_spoken():
    prompt = build_system_prompt([]).lower()
    assert "never spoken" in prompt
