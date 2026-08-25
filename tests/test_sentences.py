from server.sentences import SentenceSplitter


def test_emits_sentence_once_terminator_followed_by_space():
    s = SentenceSplitter()
    assert s.feed("Hello there. How are") == ["Hello there."]


def test_withholds_incomplete_tail_until_terminated():
    s = SentenceSplitter()
    s.feed("Hello there. How are")
    assert s.feed(" you? Fine") == ["How are you?"]


def test_flush_returns_unterminated_remainder():
    s = SentenceSplitter()
    s.feed("A complete one. A dangling tail")
    assert s.flush() == ["A dangling tail"]


def test_flush_returns_nothing_when_buffer_empty():
    s = SentenceSplitter()
    s.feed("All done. ")
    assert s.flush() == []


def test_does_not_split_decimal_numbers():
    s = SentenceSplitter()
    assert s.feed("It uses 3.5 GB of") == []


def test_splits_cyrillic_on_question_mark():
    s = SentenceSplitter()
    assert s.feed("Привіт, як справи? Далі") == ["Привіт, як справи?"]


def test_treats_ellipsis_as_single_boundary():
    s = SentenceSplitter()
    assert s.feed("Ну... добре") == ["Ну..."]


def test_keeps_closing_quote_with_sentence():
    s = SentenceSplitter()
    assert s.feed('He said "go." Then left') == ['He said "go."']


def test_emits_multiple_sentences_from_one_chunk():
    s = SentenceSplitter()
    assert s.feed("One. Two! Three? ") == ["One.", "Two!", "Three?"]


def test_strips_surrounding_whitespace_from_emitted_sentences():
    s = SentenceSplitter()
    assert s.feed("One.    Two. ") == ["One.", "Two."]
