"""The survey's corpus is data, and data gets edited by hand.

A duplicated line, or nine questions in one language instead of ten, would
bias every distribution the script prints - and the number would look exactly
as authoritative as a correct one. Nothing else checks it.
"""

from collections import Counter

from scripts.emotion_survey import CORPUS


def test_the_corpus_is_thirty_questions_ten_per_language():
    assert len(CORPUS) == 30
    assert Counter(lang for lang, _ in CORPUS) == {"uk": 10, "ru": 10, "en": 10}


def test_no_question_appears_twice():
    questions = [question for _, question in CORPUS]
    assert len(set(questions)) == len(questions)


def test_every_language_is_one_the_server_can_speak():
    """A fourth language in the corpus would be answered in Ukrainian by the
    persona's own rule, so its rows would measure the wrong thing."""
    from server.lang import VOICES

    assert {lang for lang, _ in CORPUS} == set(VOICES)
