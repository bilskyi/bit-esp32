from server.persona import build_system_prompt


def test_caps_reply_length():
    prompt = build_system_prompt([]).lower().replace("\u2013", "-")
    assert "1-3" in prompt and "sentence" in prompt
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
