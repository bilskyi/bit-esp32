import pytest

from server.emotion import (
    DEFAULT,
    EMOTIONS,
    GUESSABLE,
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


@pytest.mark.parametrize(
    "text",
    [
        "Не зрозумів, про що ти.",
        "Не понял вопрос, уточни.",
        "I'm not sure what you mean.",
    ],
)
def test_not_understanding_the_question_is_confused(text):
    """Different from sadness: sadness apologises for the answer, confusion
    asks for the question again."""
    assert from_text(text) == "confused"


@pytest.mark.parametrize(
    "text",
    ["Ого, не знав такого.", "Ничего себе!", "Wow, that is new to me."],
)
def test_a_reaction_is_surprised(text):
    assert from_text(text) == "surprised"


def test_ogo_does_not_hide_inside_an_ordinary_word():
    """"ого" is a bare three characters, and it happens to be a substring of
    some of the commonest words in an ordinary Ukrainian reply: "нічого"
    (nothing), "нікого" (nobody), "когось" (someone). Matched by substring
    instead of by word boundary, _SURPRISED would fire on all three -
    stealing an everyday statement into a reaction it never made."""
    assert from_text("Нічого страшного, спробуємо ще раз.") != "surprised"
    assert from_text("Я нікого не бачив.") != "surprised"


def test_ogo_does_not_steal_a_reply_that_is_actually_happy():
    """The precedence case: _SURPRISED is checked before _HAPPY, so if "ого"
    matched inside "нічого" by substring, this reply would be misread as
    surprise even though it literally contains "дякую" (thanks). This is
    the test that proves the substring trap is gone rather than merely
    moved further down the chain."""
    assert from_text("Дякую, нічого не потрібно.") == "happy"


def test_hyphenated_ordinal_ogo_is_not_mistaken_for_surprise():
    """A related residual of the same trap, lower frequency: \\b alone treats
    the hyphen in a spelled-out ordinal as a word boundary too, exactly like
    a space would. "21-ого" ("the 21st") is a date, not a reaction."""
    assert from_text("Зустріч 21-ого числа.") != "surprised"


def test_utochny_does_not_hide_inside_a_longer_word():
    """The same substring trap _SURPRISED had, found in _CONFUSED: "уточни"
    is six characters and sits inside "уточнити" and "уточнив", two ordinary
    conjugations of the same verb. Neither sentence below asks a confused
    question - they state, in the past and infinitive, that clarifying
    happened or is wanted - so matching by substring stole two plain
    statements into confusion they never expressed."""
    assert from_text("Хочу уточнити деталі замовлення.") != "confused"
    assert from_text("Я уточнив розклад: потяг о шостій.") != "confused"


def test_utochny_still_matches_as_a_standalone_imperative():
    """The word boundary closes the trap without closing the word itself:
    "уточни" on its own, as the imperative "clarify", is exactly the signal
    _CONFUSED is meant to catch."""
    assert from_text("Уточни, будь ласка, деталі.") == "confused"


def test_no_way_does_not_fire_mid_sentence():
    """"no way" keeps intact word boundaries, so \\b cannot fix it - the
    ambiguity is meaning, not spelling. Anchoring it to a sentence start
    rules out this ordinary use of the phrase, which sits mid-clause rather
    than opening the sentence."""
    assert from_text("There is no way to tell from here.") != "surprised"


def test_nado_zhe_still_reads_as_surprise_when_it_opens_a_sentence():
    """A documented residual, not a bug nobody noticed: "надо же" can mean
    the interjection ("well I never") or, just as literally, "[we] also
    need to". Anchoring to a sentence start rules out the mid-sentence
    literal reading but not this one, because the literal reading can also
    open a sentence - "Надо же ещё раз перевірити документи" means "[we]
    also need to check the documents again", not surprise, and nothing
    short of reading the rest of the reply can tell the two apart. Pinned
    here so a future attempt to "fix" this with a smarter anchor does not
    do it by accident without measuring what it costs elsewhere."""
    assert from_text("Надо же ещё раз проверить документы.") == "surprised"


def test_shcho_same_stays_a_semantic_residual_not_a_matching_bug():
    """"що саме" ("what exactly") is left as a plain substring on purpose:
    "Ось що саме сталося вчора" is an ordinary declarative that happens to
    contain the phrase, and no \\b or anchoring can tell it apart from an
    actually confused question - the words are the marker and the words are
    also ordinary. Fixing this needs the sentence's meaning, which from_text
    does not have."""
    assert from_text("Ось що саме сталося вчора.") == "confused"


@pytest.mark.parametrize(
    "text",
    ["Привіт! Радий тебе чути.", "Спасибо, что спросил.", "Thanks, glad to help."],
)
def test_greetings_and_thanks_are_happy(text):
    assert from_text(text) == "happy"


def test_an_apology_beats_not_understanding():
    """Both signals fire here and the apology is the more specific fact, for
    the same reason it already beats the question mark."""
    assert from_text("Вибач, не зрозумів питання.") == "sad"


def test_an_apology_beats_surprise():
    """_SORRY is checked before _CONFUSED and _SURPRISED both, and this is
    the surprise half of that precedence - nothing pins it elsewhere.
    "Вибач" is the more specific fact: the reply is apologising, not
    reacting."""
    assert from_text("Вибач, ого, я забув документи.") == "sad"


def test_confusion_beats_surprise():
    """_CONFUSED sits above _SURPRISED for the same reason it sits above the
    question mark: asking for the question again is a more specific fact
    than a bare reaction, even when both signals are literally present."""
    assert from_text("Ого, не зрозумів, повтори.") == "confused"


def test_not_understanding_beats_the_question_mark():
    """It ends in a question and is not curiosity - it is a request for the
    question again."""
    assert from_text("Не зрозумів, що саме ти маєш на увазі?") == "confused"


def test_surprise_beats_the_exclamation_mark():
    assert from_text("Ого!") == "surprised"


def test_a_greeting_beats_the_exclamation_mark():
    """"Привіт!" is warmth before it is excitement."""
    assert from_text("Привіт!") == "happy"


def test_a_thanks_beats_the_question_mark():
    """_HAPPY moved above the "?" rule when this branch extended the
    heuristic, and nothing pinned the consequence: "Дякую, а в тебе як?"
    used to read as curious and now reads as happy. For a companion that
    thanks and then asks back, happy is the more likely intended reading -
    the thanks is the point of the sentence, the question back is
    politeness - so this is pinned as the emotion the change was meant to
    produce, not an accidental regression."""
    assert from_text("Дякую, а в тебе як?") == "happy"


def test_the_heuristic_names_what_it_cannot_produce():
    """annoyed and sleepy are a decision, not an omission.

    annoyed cannot be read off the assistant's own reply - the reply is polite
    by construction, so a rule inferring irritation from it either never fires
    or fires in the wrong place. sleepy must not be read off it at all: the
    firmware falls asleep on its own timer after 90 s of idle and a server
    guessing sleepiness from words would fight that timer. Both stay reachable
    the way they were always meant to be, through the model's tag.

    Asserted rather than left implicit, so a tenth name added to EMOTIONS
    forces a decision instead of passing silently.
    """
    assert GUESSABLE < EMOTIONS
    assert EMOTIONS - GUESSABLE == {"annoyed", "sleepy"}


def test_every_guess_lands_inside_the_advertised_set():
    corpus = [
        "",
        "?",
        "!",
        "...",
        "1234",
        "Привіт",
        "У Києві зараз близько двадцяти градусів.",
        "А в тебе як?",
        "Авжеж, зробимо!",
        "Вибач, я не розчув.",
        "Не зрозумів, про що ти.",
        "Ого, не знав такого.",
        "Спасибо, что спросил.",
        "Goodnight, sleep well.",
        "I have asked you this three times already.",
    ]
    for text in corpus:
        assert from_text(text) in GUESSABLE, text


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
