from server.memory.summarise import extract_facts
from tests.fakes import FakeLLM


async def test_returns_empty_for_empty_history():
    llm = FakeLLM()
    assert await extract_facts(llm, []) == []
    assert llm.prompts == []


async def test_parses_a_json_array_of_facts():
    llm = FakeLLM()
    llm.completions = ['["Lives in Kyiv", "Building an ESP32 project"]']
    facts = await extract_facts(llm, [{"role": "user", "content": "hi"}])
    assert facts == ["Lives in Kyiv", "Building an ESP32 project"]


async def test_tolerates_prose_around_the_json():
    llm = FakeLLM()
    llm.completions = ['Sure! Here you go:\n["Likes short answers"]\nHope that helps.']
    assert await extract_facts(llm, [{"role": "user", "content": "hi"}]) == ["Likes short answers"]


async def test_caps_at_five_facts():
    llm = FakeLLM()
    llm.completions = ['["a","b","c","d","e","f","g"]']
    assert len(await extract_facts(llm, [{"role": "user", "content": "hi"}])) == 5


async def test_returns_empty_when_the_model_returns_nonsense():
    llm = FakeLLM()
    llm.completions = ["I could not find any facts."]
    assert await extract_facts(llm, [{"role": "user", "content": "hi"}]) == []


async def test_drops_blank_and_non_string_entries():
    llm = FakeLLM()
    llm.completions = ['["Real fact", "", 42, null, "  "]']
    assert await extract_facts(llm, [{"role": "user", "content": "hi"}]) == ["Real fact"]


async def test_conversation_text_reaches_the_model():
    llm = FakeLLM()
    llm.completions = ["[]"]
    await extract_facts(llm, [{"role": "user", "content": "I live in Lviv"}])
    assert "I live in Lviv" in llm.prompts[0][-1]["content"]
