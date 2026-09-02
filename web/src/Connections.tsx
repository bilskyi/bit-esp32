// Зʼєднання: the one section on this whole rebuild with no code behind it
// at all - no MCP client, no integrations table, no /connect route, nothing
// this component could call even optimistically. See the plan's honest-data
// table: "Honest empty state, in her voice." So this file has no state, no
// effect, no API call - just the one thing that is true (nothing is
// connected yet) and the one thing worth saying well (what a connection
// would actually change about her). Nothing here is a button that goes
// nowhere: every control that would need a backend has been left out
// entirely rather than wired to do nothing.

/** Зʼєднання, told straight: she knows a great deal from memory and can act
 * on almost none of it. A connection - MCP or otherwise - is not "more
 * data", it is a new verb: the difference between reporting that no free
 * hour exists and going to go find one. */
function Connections() {
  return (
    <section className="view">
      <div className="wrap">
        <div className="head">
          <div className="hl">
            <p className="lab">Зʼєднання · нічого не підключено</p>
            <h1 style={{ fontSize: 18, marginTop: 2 }}>Поки я вмію мало.</h1>
            <p className="prose" style={{ fontSize: 13 }}>
              Я знаю про тебе багато — і майже нічого не можу з цим зробити назовні. Спитай, о
              котрій у тебе завтра зустріч, і я відповім з памʼяті. Попроси її перенести — і зможу
              лише сказати, що не вмію. Кожне зʼєднання, яке тут зʼявиться, — це не нова тема для
              розмови, а нове дієслово для мене.
            </p>
          </div>
        </div>

        <div className="grid2">
          <div className="stack">
            <div className="grp">
              <div className="grph">
                <h2>Підключено</h2>
                <span className="pill">0</span>
              </div>
              <div className="empty">
                <p className="prose" style={{ fontSize: 12 }}>
                  Чесно порожньо: немає MCP-клієнта на сервері, немає списку сервісів і немає
                  кнопки «Підключити», яка хоч щось би підключила. Я не малюю тут те, чого
                  насправді нема — навіть демонстрації заради.
                </p>
              </div>
            </div>

            <div className="grp" style={{ marginTop: 20 }}>
              <div className="grph">
                <h2>Виклики інструментів</h2>
                <span className="cnt">0</span>
              </div>
              <div className="empty">
                <p className="prose" style={{ fontSize: 12 }}>
                  Тут з&rsquo;являвся б кожен виклик інструмента — коли, з якими аргументами і що
                  повернулось, поряд зі score в інспекторі, так само чесно, як зараз показано
                  пошук у памʼяті. Порожньо не тому, що щось зламалось: мені просто ще нічим
                  користуватись.
                </p>
              </div>
            </div>
          </div>

          <div className="stack">
            <div className="grp">
              <p className="lab" style={{ marginBottom: 8 }}>
                Що зміниться з першим зʼєднанням
              </p>
              <ol className="steps">
                <li>
                  «Подивись, чи є вікно в четвер» перестане бути відмовою і стане тим, що я
                  справді перевірила.
                </li>
                <li>
                  Кожен виклик інструмента буде видно тут-таки, в інспекторі поряд зі score — те
                  саме чесне логування, що вже є для пошуку в памʼяті.
                </li>
                <li>
                  Я не вирішую сама, чим користуватись: кожен інструмент вмикається чи
                  вимикається окремим перемикачем, який ти бачиш і контролюєш.
                </li>
              </ol>
            </div>

            <div className="panel note pad" style={{ marginTop: 4 }}>
              <p className="lab" style={{ color: 'var(--accent)', marginBottom: 6 }}>
                Чому тут порожньо, а не «скоро»
              </p>
              <p className="prose" style={{ fontSize: 12 }}>
                MCP — протокол, яким асистент викликає зовнішні інструменти, — на сервері ще не
                підключений. Поле для адреси й кнопка «Додати» зʼявляться тут того дня, коли
                справді щось прийматимуть, а не раніше.
              </p>
            </div>
          </div>
        </div>
      </div>
    </section>
  )
}

export default Connections
