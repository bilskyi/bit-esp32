import pytest

from server.emotion import (
    DEFAULT,
    EMOTIONS,
    HEAD_LIMIT,
    LeadingTag,
    from_text,
    split_tag,
    strip_tags,
)


def test_the_vocabulary_matches_the_firmware():
    """face.h enumerates exactly these. A name here that is not there arrives
    as an unknown emotion and the face keeps its old expression."""
    assert EMOTIONS == frozenset(
        {
            "neutral",
            "happy",
            "excited",
            "curious",
            "confused",
            "surprised",
            "sad",
            "annoyed",
            "sleepy",
        }
    )
    assert DEFAULT in EMOTIONS


# ------------------------------------------------------------------ split_tag


@pytest.mark.parametrize("name", sorted(EMOTIONS))
def test_every_emotion_is_recognised_as_a_leading_tag(name):
    assert split_tag(f"[{name}] Привіт.") == (name, "Привіт.")


def test_tag_is_recognised_without_a_space_after_it():
    assert split_tag("[happy]Привіт.") == ("happy", "Привіт.")


def test_leading_whitespace_before_the_tag_is_tolerated():
    assert split_tag("  \n[sad] Вибач.") == ("sad", "Вибач.")


def test_tag_case_is_ignored():
    assert split_tag("[Happy] Hi.") == ("happy", "Hi.")


def test_an_unknown_tag_is_removed_but_names_no_emotion():
    """The model was given nine names. If it invents one, the important thing
    is that the brackets never reach TTS - edge-tts will happily pronounce
    "smug"."""
    assert split_tag("[smug] Привіт.") == (None, "Привіт.")


def test_text_with_no_tag_is_returned_untouched():
    assert split_tag("Привіт, як справи?") == (None, "Привіт, як справи?")


def test_a_bracket_that_never_closes_is_left_alone():
    text = "[this is not a tag, it just keeps going and going and going"
    assert split_tag(text) == (None, text)


def test_only_the_first_tag_is_taken():
    assert split_tag("[happy] [sad] Привіт.") == ("happy", "[sad] Привіт.")


def test_empty_input():
    assert split_tag("") == (None, "")


# ----------------------------------------------------------------- strip_tags


def test_stray_tags_are_stripped_anywhere_in_the_text():
    """Insurance. A tag in the middle of a sentence would otherwise be read
    aloud, and the handoff already records the model emitting characters
    nobody expected."""
    assert strip_tags("Все добре [happy] а в тебе?") == "Все добре а в тебе?"


def test_stripping_leaves_ordinary_text_alone():
    text = "Два по два — чотири. Просто."
    assert strip_tags(text) == text


def test_stripping_collapses_the_space_it_leaves_behind():
    assert strip_tags("Так. [excited] Авжеж!") == "Так. Авжеж!"


def test_stripping_does_not_eat_numbers_in_brackets():
    assert strip_tags("Пункт [1] важливий.") == "Пункт [1] важливий."


# ------------------------------------------------------------------ from_text


def test_a_question_back_is_curious():
    assert from_text("А в тебе як?") == "curious"


def test_an_exclamation_is_excited():
    assert from_text("Авжеж, зробимо!") == "excited"


@pytest.mark.parametrize(
    "text",
    [
        "Вибач, я не розчув.",
        "Извини, я не расслышал.",
        "Sorry, I did not catch that.",
        "Не знаю.",
        "I don't know.",
    ],
)
def test_apologies_and_ignorance_are_sad(text):
    assert from_text(text) == "sad"


def test_an_apology_that_also_asks_is_still_sad():
    """Both signals fire on the "did not catch" reply, which ends in a
    question. The apology is the more specific one."""
    assert from_text("Вибач, я не розчув. Повтори, будь ласка?") == "sad"


def test_a_plain_statement_is_neutral():
    assert from_text("У Києві зараз близько двадцяти градусів.") == "neutral"


def test_the_heuristic_ignores_tags_left_in_the_text():
    assert from_text("[happy] Все добре!") == "excited"


def test_the_heuristic_always_returns_something_the_firmware_knows():
    for text in ["", "?", "!", "...", "Привіт", "1234"]:
        assert from_text(text) in EMOTIONS


# ----------------------------------------------------------------- LeadingTag


def feed_all(sniffer: LeadingTag, deltas: list[str]) -> str:
    out = "".join(sniffer.feed(d) for d in deltas)
    return out + sniffer.flush()


def test_a_tag_split_across_deltas_is_still_found():
    """The tag arrives token by token, so it is almost never whole in one
    delta - "[", "hap", "py", "]" is the normal case, not the edge case."""
    s = LeadingTag()
    assert feed_all(s, ["[", "hap", "py", "] При", "віт."]) == "Привіт."
    assert s.emotion == "happy"


def test_the_tag_never_reaches_the_output_even_one_character_of_it():
    s = LeadingTag()
    out = feed_all(s, ["[su", "rpri", "sed] О!"])
    assert "[" not in out and "]" not in out
    assert out == "О!"


def test_text_flows_through_when_there_is_no_tag():
    s = LeadingTag()
    assert feed_all(s, ["При", "віт, ", "як справи?"]) == "Привіт, як справи?"
    assert s.emotion is None


def test_nothing_is_held_back_once_the_head_is_resolved():
    """Whatever is buffered while sniffing delays the first sentence, and the
    first sentence is the whole latency budget. Past the head, deltas pass
    straight through."""
    s = LeadingTag()
    s.feed("[happy] Перше речення. ")
    assert s.resolved
    assert s.feed("Друге.") == "Друге."


def test_a_long_head_with_no_closing_bracket_gives_up_and_emits_it():
    s = LeadingTag()
    text = "[the model started rambling instead of tagging and never stopped"
    assert feed_all(s, [text]) == text
    assert s.emotion is None


def test_an_unclosed_bracket_resolves_within_the_head_limit():
    """It must not hold the whole reply hostage waiting for a bracket that is
    never coming. Bounded by HEAD_LIMIT, not by a number picked to pass."""
    s = LeadingTag()
    out = ""
    consumed = 0
    for ch in "[" + "x" * (HEAD_LIMIT * 3):
        out += s.feed(ch)
        consumed += 1
        if s.resolved:
            break
    assert s.resolved
    assert consumed <= HEAD_LIMIT
    assert out == "[" + "x" * (consumed - 1)  # every character comes back out


def test_flush_releases_a_head_shorter_than_the_limit():
    s = LeadingTag()
    assert feed_all(s, ["Так."]) == "Так."


def test_flush_on_an_empty_stream():
    s = LeadingTag()
    assert feed_all(s, []) == ""
    assert s.emotion is None


def test_an_unknown_tag_resolves_to_no_emotion_and_is_still_removed():
    s = LeadingTag()
    assert feed_all(s, ["[smug] Ага."]) == "Ага."
    assert s.emotion is None
    assert s.resolved


def test_the_names_match_the_firmware_exactly():
    """The one invariant spanning both halves of the repo.

    The device matches these names by substring and ignores anything else, so
    a rename on one side does not raise - the face just quietly stops changing.
    Read the C rather than trusting a comment.
    """
    import pathlib
    import re

    face_c = pathlib.Path(__file__).resolve().parents[1] / "firmware" / "main" / "face.c"
    assert face_c.exists(), face_c

    table = re.search(
        r"EMO_NAME\[FACE_EMO_COUNT\]\s*=\s*\{(.*?)\};", face_c.read_text(), re.S
    )
    assert table, "could not find EMO_NAME in face.c"

    in_firmware = set(re.findall(r'"([a-z]+)"', table.group(1)))
    assert in_firmware == set(EMOTIONS)
