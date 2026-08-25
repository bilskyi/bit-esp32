from server.lang import detect_language, voice_for


def test_detects_ukrainian_by_unique_letters():
    assert detect_language("Привіт, як твої справи?") == "uk"


def test_detects_russian_by_function_words_without_unique_letters():
    assert detect_language("Привет, как дела?") == "ru"


def test_detects_russian_by_unique_letters():
    assert detect_language("Этот вопрос был бы сложным") == "ru"


def test_detects_english():
    assert detect_language("What is the weather like today?") == "en"


def test_technical_latin_terms_do_not_flip_cyrillic_text():
    assert detect_language("Використай WebSocket і FastAPI для цього") == "uk"


def test_empty_text_falls_back_to_default():
    assert detect_language("") == "uk"


def test_voice_for_each_language_matches_locale_prefix():
    assert voice_for("uk").startswith("uk-UA-")
    assert voice_for("ru").startswith("ru-RU-")
    assert voice_for("en").startswith("en-US-")


def test_voice_for_unknown_language_falls_back_to_default():
    assert voice_for("de").startswith("uk-UA-")
