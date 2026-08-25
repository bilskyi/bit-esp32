from server.context import build_messages, estimate_tokens


def test_estimate_tokens_grows_with_length():
    assert estimate_tokens("hello world") < estimate_tokens("hello world " * 10)


def test_estimate_tokens_charges_cyrillic_more_than_latin():
    # Cyrillic costs more tokens per character on Whisper/Llama tokenizers,
    # so an equal-length string must not be counted as equally cheap.
    assert estimate_tokens("привіт світ") > estimate_tokens("hello world")


def test_estimate_tokens_of_empty_string_is_zero():
    assert estimate_tokens("") == 0


def _history(n):
    out = []
    for i in range(n):
        out.append({"role": "user", "content": f"question {i} " + "x" * 200})
        out.append({"role": "assistant", "content": f"answer {i} " + "y" * 200})
    return out


def test_system_prompt_comes_first():
    msgs = build_messages("SYS", [], "hi", budget=2000)
    assert msgs[0] == {"role": "system", "content": "SYS"}


def test_current_user_message_comes_last():
    msgs = build_messages("SYS", _history(2), "hi there", budget=2000)
    assert msgs[-1] == {"role": "user", "content": "hi there"}


def test_history_kept_when_under_budget():
    msgs = build_messages("SYS", _history(1), "hi", budget=2000)
    assert len(msgs) == 4  # system + 2 history + current user


def test_oldest_turns_dropped_first_when_over_budget():
    msgs = build_messages("SYS", _history(10), "hi", budget=300)
    kept = [m["content"] for m in msgs]
    assert not any("question 0" in c for c in kept)
    assert any("answer 9" in c for c in kept)


def test_result_never_exceeds_budget_when_droppable():
    msgs = build_messages("SYS", _history(10), "hi", budget=300)
    assert sum(estimate_tokens(m["content"]) for m in msgs) <= 300


def test_system_and_current_user_survive_an_impossible_budget():
    msgs = build_messages("SYS", _history(10), "hi", budget=1)
    assert [m["role"] for m in msgs] == ["system", "user"]
    assert msgs[-1]["content"] == "hi"


def test_history_never_starts_with_a_dangling_assistant_reply():
    for budget in range(120, 600, 20):
        msgs = build_messages("SYS", _history(10), "hi", budget=budget)
        if len(msgs) > 2:
            assert msgs[1]["role"] == "user"
