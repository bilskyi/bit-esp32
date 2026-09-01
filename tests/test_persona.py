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


def test_the_device_default_reproduces_the_measured_prompt_exactly():
    """The golden test. BASE was measured (RESUME.md); an attempt to reword it
    made the emotion spread worse on three runs of emotion_survey.py. The
    assembly must reproduce it byte for byte, or the measurement no longer
    describes what ships."""
    from server.persona import BASE, build_system_prompt
    from server.roles import DEVICE_DEFAULT

    assert build_system_prompt([], DEVICE_DEFAULT, spoken=True) == BASE


def test_the_default_arguments_are_the_device_defaults():
    from server.persona import BASE, build_system_prompt

    assert build_system_prompt([]) == BASE


def test_a_screen_reply_allows_markdown_when_the_role_does():
    from server.persona import build_system_prompt
    from server.roles import WEB_DEFAULT

    prompt = build_system_prompt([], WEB_DEFAULT, spoken=False).lower()
    assert "markdown, lists and headings are fine" in prompt
    assert "up to 6 sentences" in prompt


def test_a_spoken_reply_never_allows_markdown_even_if_the_role_does():
    """TTS reads asterisks and hyphens out loud. Whatever the role says,
    a reply that will be spoken gets the no-markdown rule."""
    from server.persona import build_system_prompt
    from server.roles import WEB_DEFAULT

    prompt = build_system_prompt([], WEB_DEFAULT, spoken=True).lower()
    assert "no lists, no headings, no markdown" in prompt
    assert "markdown, lists and headings are fine" not in prompt


def test_a_custom_persona_section_replaces_only_the_opening():
    from server.emotion import EMOTIONS
    from server.persona import build_system_prompt
    from server.roles import Role, SPEAKABLE_LANGUAGES

    coach = Role(id=1, name="Coach", prompt="You are a blunt running coach.",
                 max_sentences=2, markdown_allowed=False,
                 languages=SPEAKABLE_LANGUAGES, pinned_mood=None)
    prompt = build_system_prompt([], coach, spoken=True)
    assert prompt.startswith("You are a blunt running coach.")
    assert "voice companion" not in prompt
    # The invariants survive a custom persona.
    for name in EMOTIONS:
        assert f"[{name}]" in prompt, name
    assert "Reply only in Ukrainian, Russian or English." in prompt


def test_narrowing_the_languages_narrows_the_rule():
    from server.persona import build_system_prompt
    from server.roles import Role

    ukrainian_only = Role(id=1, name="UA", prompt=None, max_sentences=2,
                          markdown_allowed=False, languages=("uk",), pinned_mood=None)
    prompt = build_system_prompt([], ukrainian_only, spoken=True)
    assert "Reply only in Ukrainian." in prompt
    assert "Russian" not in prompt.split("Reply only in", 1)[1].split("\n", 1)[0]


def test_two_languages_read_as_a_pair():
    from server.persona import build_system_prompt
    from server.roles import Role

    pair = Role(id=1, name="Pair", prompt=None, max_sentences=2,
                markdown_allowed=False, languages=("uk", "en"), pinned_mood=None)
    assert "Reply only in Ukrainian or English." in build_system_prompt([], pair, spoken=True)


def test_a_pinned_mood_becomes_a_rule():
    from server.persona import build_system_prompt
    from server.roles import Role, SPEAKABLE_LANGUAGES

    sleepy = Role(id=1, name="Sleepy", prompt=None, max_sentences=2,
                  markdown_allowed=False, languages=SPEAKABLE_LANGUAGES,
                  pinned_mood="sleepy")
    prompt = build_system_prompt([], sleepy, spoken=True)
    assert "[sleepy]" in prompt
    assert "you feel sleepy" in prompt.lower()


def test_no_pinned_mood_adds_no_mood_rule():
    from server.persona import BASE, build_system_prompt
    from server.roles import DEVICE_DEFAULT

    # Not "you feel" bare: BASE's tag rule itself says "how you feel about
    # it", so that phrase is unavoidably present. What must be absent is the
    # pinned-mood rule's distinct wording.
    assert "right now you feel" not in build_system_prompt([], DEVICE_DEFAULT, spoken=True).lower()
    assert build_system_prompt([], DEVICE_DEFAULT, spoken=True) == BASE


def test_one_sentence_reads_as_one_sentence():
    from server.persona import build_system_prompt
    from server.roles import Role, SPEAKABLE_LANGUAGES

    terse = Role(id=1, name="Terse", prompt=None, max_sentences=1,
                 markdown_allowed=False, languages=SPEAKABLE_LANGUAGES, pinned_mood=None)
    assert "Answer in one sentence." in build_system_prompt([], terse, spoken=False)
