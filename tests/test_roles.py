import pytest

from server.roles import DEVICE_DEFAULT, WEB_DEFAULT, Role, Roles


@pytest.fixture
async def roles(tmp_path):
    r = Roles(f"sqlite+aiosqlite:///{tmp_path}/test.db")
    await r.init()
    await r.ensure_defaults()
    yield r
    await r.close()


async def test_ensure_defaults_creates_one_role_per_surface(roles):
    names = sorted(role.name for role in await roles.all())
    assert names == ["Device default", "Web default"]


async def test_ensure_defaults_is_idempotent(roles):
    await roles.ensure_defaults()
    assert len(await roles.all()) == 2


async def test_the_device_surface_starts_on_the_device_default(roles):
    active = await roles.active_for("esp32")
    assert active.name == "Device default"
    assert active.prompt is None
    assert active.max_sentences == DEVICE_DEFAULT.max_sentences
    assert active.markdown_allowed is False


async def test_the_web_surface_starts_on_the_web_default(roles):
    active = await roles.active_for("web")
    assert active.name == "Web default"
    assert active.max_sentences == WEB_DEFAULT.max_sentences
    assert active.markdown_allowed is True


async def test_an_unknown_surface_falls_back_to_the_web_default(roles):
    active = await roles.active_for("carrier-pigeon")
    assert active.name == "Web default"


async def test_a_created_role_round_trips(roles):
    created = await roles.create(
        name="Coach",
        prompt="You are a blunt running coach.",
        max_sentences=3,
        markdown_allowed=False,
        languages=("uk", "en"),
        pinned_mood="excited",
    )
    fetched = await roles.get(created.id)
    assert fetched == created
    assert fetched.languages == ("uk", "en")
    assert fetched.pinned_mood == "excited"


async def test_creating_a_duplicate_name_is_refused(roles):
    await roles.create(name="Coach", prompt=None, max_sentences=2,
                       markdown_allowed=False, languages=("uk",), pinned_mood=None)
    with pytest.raises(ValueError, match="name"):
        await roles.create(name="Coach", prompt=None, max_sentences=4,
                           markdown_allowed=True, languages=("en",), pinned_mood=None)


async def test_a_language_outside_the_three_voices_is_refused(roles):
    with pytest.raises(ValueError, match="language"):
        await roles.create(name="Deutsch", prompt=None, max_sentences=2,
                           markdown_allowed=False, languages=("de",), pinned_mood=None)


async def test_an_empty_language_set_is_refused(roles):
    with pytest.raises(ValueError, match="language"):
        await roles.create(name="Silent", prompt=None, max_sentences=2,
                           markdown_allowed=False, languages=(), pinned_mood=None)


async def test_a_mood_outside_the_nine_faces_is_refused(roles):
    with pytest.raises(ValueError, match="mood"):
        await roles.create(name="Smug", prompt=None, max_sentences=2,
                           markdown_allowed=False, languages=("uk",), pinned_mood="smug")


async def test_update_changes_only_the_fields_given(roles):
    created = await roles.create(name="Coach", prompt="Blunt.", max_sentences=3,
                                 markdown_allowed=False, languages=("uk",), pinned_mood=None)
    updated = await roles.update(created.id, max_sentences=5)
    assert updated.max_sentences == 5
    assert updated.prompt == "Blunt."
    assert updated.name == "Coach"


async def test_update_validates_languages_too(roles):
    created = await roles.create(name="Coach", prompt=None, max_sentences=3,
                                 markdown_allowed=False, languages=("uk",), pinned_mood=None)
    with pytest.raises(ValueError, match="language"):
        await roles.update(created.id, languages=("fr",))


async def test_update_of_an_unknown_role_is_none(roles):
    assert await roles.update(999, max_sentences=2) is None


async def test_set_active_switches_the_surface(roles):
    created = await roles.create(name="Terse", prompt=None, max_sentences=1,
                                 markdown_allowed=False, languages=("uk",), pinned_mood=None)
    assert await roles.set_active("esp32", created.id) is True
    assert (await roles.active_for("esp32")).name == "Terse"


async def test_set_active_to_an_unknown_role_is_refused(roles):
    assert await roles.set_active("esp32", 999) is False
    assert (await roles.active_for("esp32")).name == "Device default"


async def test_deleting_the_active_role_falls_back_to_the_surface_default(roles):
    created = await roles.create(name="Terse", prompt=None, max_sentences=1,
                                 markdown_allowed=False, languages=("uk",), pinned_mood=None)
    await roles.set_active("esp32", created.id)
    assert await roles.delete(created.id) is True
    assert (await roles.active_for("esp32")).name == "Device default"


async def test_a_built_in_default_cannot_be_deleted(roles):
    device = next(r for r in await roles.all() if r.name == "Device default")
    with pytest.raises(ValueError, match="built-in"):
        await roles.delete(device.id)


async def test_deleting_an_unknown_role_is_false(roles):
    assert await roles.delete(999) is False


async def test_reverting_the_device_role_restores_the_measured_prompt(roles):
    device = next(r for r in await roles.all() if r.name == "Device default")
    await roles.update(device.id, prompt="You are a pirate.")
    assert (await roles.active_for("esp32")).prompt == "You are a pirate."
    await roles.update(device.id, prompt=None)
    assert (await roles.active_for("esp32")).prompt is None
