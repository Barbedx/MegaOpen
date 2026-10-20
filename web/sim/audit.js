'use strict';
/* ============ СИМУЛЯТОР: екран «Аудит» ============
   Матеріал рецензії дизайну, не частина продукту: лише в tools/build_sim.py.
   Іде ПІСЛЯ скриптів сторінки: реєструє SCR.audit і додає посилання внизу Плати. */
(()=>{
const AUDIT=`# Аудит web UI — MegaOpen, env:t2can (2026-10-16)

Що перевірено в репо (лише читання):
- \`KeyEngine.cpp:87\`: encCw/encCcw читають тільки матриця і /api/test (\`WebUi.cpp:775\`); у ручок свої коди.
- \`BoardProfile.h:131\`: матриця на T-2CAN дає #error.
- \`CarTask.cpp:200\`: при CAN2 сніфер вимкнено.
- \`Features.h:96\`: CAN2 разом із LINK не збирається.
- \`DisplayAssets.h:386\`: сторінка читає поля linkTx*, яких API не віддає.
- \`DisplayAssets.h:280,662\`: cols і bar захардкоджені (26 і 10).
- \`WebUi.cpp:1533\`: /paint малює info як меню.
- \`WebUi.cpp:806\`: порожній пароль стирає збережений.
- \`CanboxAssets.h:378\`: кнопка клімату пише cmd28=1 у NVS.
- Маршруту /api/config немає.

## Що є

| Сторінка | Для чого | Розмір | Вердикт |
|---|---|---|---|
| / «клавіші» | матриця 3×3, таймінги, маршрути, WiFi, mDNS, стан, тест, лог | 24.6 kB | розібрати: ~60% мертве на t2can |
| /macros | 8 карток макросів | 13.7 kB | лишити, 2 баги |
| /display «екран» | реєстрація, консоль панелі, медіа, AMS, ANCS, мапа керма | 53.5 kB | розібрати: ~75% бенч |
| /canbox | емітер, тюнінг Raise, сирі кадри, 4 відправники клавіш, клімат | 30.6 kB | майже все в Сервіс |
| /vehicle «сигнали» | підміна сигналів, сценарій, TWAI панелі, мертвий сніфер | 26.8 kB | розібрати |
| /car «CAN A» | швидкість, запис, мітки, файли | 10.8 kB | лишити, найкраща сторінка |
| /knobs «ручки» | енкодери, підсвітка | 20.6 kB | лишити, вирізати прозу |
| /res | драбина KEY1/KEY2 | 13.9 kB | на t2can не зібрано |
| /wiggle | пошук пайки | 3.1 kB | у Сервіс |
| /update | ElegantOTA | lib | у Плату, назад звідти не вийти |

## Дублі

- Лог-панель: /, /display, /canbox, /vehicle — 4 копії, у кожній свій рівень логу.
- Емітер увімк/вимк і стан: /canbox #emon/#emoff і /vehicle #emon/#emoff.
- Підміна сигналів: /vehicle (за id) і /canbox Клімат (за name).
- Текст ГУ на щиток: /display mediaSource=hu (NVS) і /canbox huTextToDisplay (RAM). Можуть конфліктувати.
- Годинник: /display HHMM, «Зараз», autoTime — три шляхи.
- Відправити клавішу: / /api/test, /display /api/display/key, /canbox (сітка, kcode, перебір, каталог), /macros ефект key.
- Коди енкодера: / encCw/encCcw/encInverted і /knobs cw/ccw/навпаки.
- Таймінги імпульсу: / pulse/repeat і /res pulse/gap/repeat.
- Вимкнути макрос: чекбокс «увімкнено» і тригер off.
- /api/settings пишуть /, /display і /vehicle, кожна свій шматок.
- POST /api/canbox cmd28/doorsWithIg/steeringInverted і /api/canbox/tuning.
- Стан: /api/status (шапка) і /api/health — обидва лише на /.
- /api/media scrollMs = scrollMsTitle; autoTime/bleMode є і в /api/media, і в /api/settings.
- /api/can/selftest і /api/can/loop (wiggle).
- /car «Оновити список» робить те саме, що автооновлення кожні 10 с.
- /vehicle «Приклад» дублює текст, уже вставлений у textarea.

## Сміття / бенч-інструменти

Прибрати з t2can:
- / матриця: сітка 3×3, редактор слота, «Зберегти мапу», «Перечитати», «Дефолти Мегана».
- / 5 таймінгів (дебаунс, дебаунс енкодера, long, repeat, автоповтор) — їх живить тільки матриця.
- / encCw, encCcw, encInverted.
- Шапка ghost, чип M у лог-панелі.
- /vehicle кнопки ролі (409), «Дозволити передачу» (409), рядок плати V (/api/link не зібрано).
- /display pwrbytes (статичний текст), «claude top» за замовчуванням, лінк TX/RX/кільце (бекенду немає).
- /canbox розділ-вказівник «Сигнали», huTextToDisplay.
- /knobs проза про проводку DevKit, есе про ефекти, таблиця пінів (показує GPIO-1).
- /api/display/fsgeom: геометрію вже відомо.

Перенести в прихований Сервіс:
- Консоль панелі: power, text, menu, fullscreen, confirm, info, msgbox, selectbutton, styled, highlight, popup, abort, caps.
- Медіа-покинь (title/artist/state/time), «Намалювати раз», «сплеш зараз».
- Канал тексту ГУ: huTitleCmd/huArtistCmd/huTitleFirst/huForget.
- canbox: повідомлення, тюнінг протоколу, сирий кадр, код клавіші, перебір, каталог, кільце UART.
- Підміна сигналів, сценарій, «Зафіксувати все», клімат.
- TWAI шини панелі: стан, тайминги, ECC, сирий потік, піни.
- /wiggle, живий лог, рівень логу, тестове натискання.

## Не туди покладено

- WiFi (скан, SSID, пароль) на / «клавіші» → Плата.
- mDNS на / → Плата.
- Стан ланцюгів (health) на / → Огляд.
- Режим BLE (з перезавантаженням) на /display в AMS → Плата.
- Мапа керма AFFA→RAV4 внизу /display, хоча на t2can це і є мапа клавіш → Кнопки.
- Попап натискань на /display → Кнопки.
- «UART увімк» і «Імпульс енкодера» (насправді імпульс UART-клавіші) серед таймінгів матриці → Кнопки.
- Тип панелі і funcReg серед тестових кнопок → Екран, блок перезавантаження.
- /vehicle «Шина CAN» змішує TWAI (CAN B, панель) з таблицею ID CAN A → TWAI у Сервіс, ID на Авто.
- Одиниці пального і поїздки сховані в тюнінгу протоколу → Авто.
- Рівень логу всієї плати в кожній лог-панелі → Сервіс.
- Підсвітка в групі «входи» → Кнопки › Ручки.
- /update без навігації → Плата.

## Зламане / без бекенду

- /display лінк TX/RX/кільце: полів немає в API, тому завжди «0, кільце ок» — хибне все добре.
- /display «Застосувати» (mapply) шле cols=26 і bar=10 і ламає 8-колонкові розкладки info/text.
- /api/media/paint: info малюється як меню.
- /canbox клімат «Застосувати» назавжди пише cmd28=1, зовнішня t° пропадає; «Зняти фіксації» не повертає.
- /vehicle таблиця ID порожня: сніфер вимкнено, /api/can/sniff не викликається.
- /vehicle сортування ID: помилка пріоритету (VehicleAssets.h:298).
- /vehicle кнопка сирого потоку не читає .rawStream.
- /vehicle /api/link, /link/sim, /link/filter дають 404.
- /vehicle «Шина авто» і «Дозволити передачу» отримують 409, сторінка мовчить.
- /macros поле коду для ефекту action сховане при першому рендері.
- /macros назва вставляється у value без екранування.
- / WiFi: порожній пароль затирає збережений; збереження одразу перезавантажує, без підтвердження.
- / keymap, defaults, settings, WiFi ігнорують HTTP-відповідь: 503 виглядає як успіх.
- / посилання на /res у тексті стану — 404 на t2can.
- /knobs правий енкодер показує GPIO-1, підказка про pull-up хибна.
- /display blesave завжди каже «перезавантажую».
- /display #layout перезаписується кожну секунду, навіть у фокусі.
- /canbox таблиця повідомлень перебудовується кожні 4 с, клік губиться.
- Підказка AMS «у C3 одне радіо», але плата S3.
- Пам'ять каже «No WebSockets», а /ws підключено (WebUi.cpp:139) і ним користуються 4 сторінки. Розв'язати до редизайну.

## Налаштування без UI

- apName, mdnsName: маршруту немає.
- uartBaud: тільки для читання, POST ігнорує.
- wifi2 (друга мережа) і хмара: лише POST /api/cloud.
- BLE: /api/ble (стан) і /api/ble/unbond (забути).
- Повторна реєстрація панелі: /api/display/resync.
- Запис: autostart, ring, fill, fileMB, autoProtect, mode (/api/cap/settings, /api/cap/mode).
- Чистка записів: /api/cap/delete_older, /api/cap/format.
- Логи на флеші: /api/logs, /api/logs/file?n= (8 завантажень).
- Factory reset: /api/factory?confirm=factory.
- Сніфер /api/can/sniff; TWAI timing і mode.
- serialLog; glassHoldMs.
- /api/knobs/cal: на t2can pot=-1, не потрібно.
- Не показуються: /api/media hu* (вивчений канал), source*; /api/display phase/lastLoss/sessionsLost; /api/can2 restarts/beginErr/errFlags; /api/canbox uartAcks/uartDeferred/baud.

## Що не використовується

- /api/keymap GET/POST/defaults на t2can: матриці немає.
- /api/display/fsgeom, raw, notify, clear, release: жодна сторінка не викликає.
- /api/can/level, pintest, txwatch, pads, alerts, last, mode, selftest, probe: лише curl на стенді.
- /api/link*, /api/can/role на t2can: мертві.
- /res і /api/res* на t2can: не зібрано.
- /api/cloud: бекенд не розгорнуто.
- /api/logs/format: лише для аварій.

## v2: що змінено за відгуком (2026-10-16)

- Шапка: «онлайн» прибрано, чип з’являється лише коли плата не відповідає. Прошивка, збірка, пам’ять, IP — рядком унизу.
- Широкий екран: меню зліва, картки колонками. Телефон: вкладки внизу.
- Огляд: «Музика» показує джерело (ГУ, iPhone, нема). «Телефон» лише коли BLE увімкнено. Точка доступу — лише як резерв.
- Екран: 4 розкладки з коду з живим малюнком; 11 стилів і параметри під розкладкою; редактор 48×48; годинник від ГУ (0x9D) або iPhone (CTS); сповіщення сховані без AMS; виправлена підказка реєстрації.
- Кнопки: рядок на кнопку (клік, утримання), підсвітка натискання наживо, ▶ = як натискання; колонка «Резистор»; пресет прибрано; ручка разом зі своїм кільцем; ноги згорнуто.
- Авто: «Сигнали», «У ГУ», «Підміна», «Сценарій» і «Клімат» злиті в мапу RAV4 → ГУ: тест ▶ і прив’язка до ID CAN A. 🔒 замість «захищено».
- Плата: запасна мережа в /api/wifi; роль BLE з поясненням і «вимкнено»; «Налаштування у файл»; «Журнал запусків» з поясненням; «Режим розробника» показує Сервіс.
- Сервіс: консоль з полями під команду; «Клавіша в CAN B»; «покинь» стало «Тестовий трек»; ECC і піни пояснені; лог опитуванням, без WebSocket.

## Бекенд під v2 (план 2026-10-16; що з нього зроблено — ledger у docs/design/web-ui.md)

Видалити:
- /ws, обробник WebSocket, 4 клієнти на старих сторінках, префікси K/L/T/V/J у LogBus. Замість: GET /api/logs?since=N.
- /api/signals (GET, POST), /api/signals/unpinall, /api/emitter, POST /api/canbox (cmd28, doorsWithIg, steeringInverted, huTextToDisplay), масштаби steerDiv, steerClamp, rpmMul, rpmDiv, tripFields, histLen, radarInvert. Замість: /api/map і /api/map/test; NVS-блоб «decode» замість decode_defaults.inc (немає блоба — заводська таблиця).
- /api/cloud wifi2. Замість: ssid2, pass2 у /api/wifi.
- /api/test (дубль /api/display/key), /api/keymap*, /api/link*, /api/can/role, /api/display/fsgeom і raw, /api/can selftest, loop, level, pintest, txwatch, pads, alerts, probe; /res на t2can.
- g_lastKeyLine (пишеться, ніхто не читає). Замість: поле last у GET /api/affamap.
- Перезапуск як побічний ефект /api/settings, /api/wifi, /api/display. Замість: одне POST /api/reboot.
- Старі сторінки *Assets.h, коли нова готова.

Нове, і що воно замінює:
- /api/map, /api/map/test: 6 маршрутів сигналів і емітера.
- /api/logs?since: /ws.
- /api/reboot: 4 приховані перезапуски.
- /api/display/key?wire=1: параметр; KeySource::Wire уже є в бібліотеці.
- /api/media bits=: параметр, своя картинка 48×48.
- /api/config: єдине нове без заміни; можна відкласти.

Знайдено попутно:
- doorsLen=1 за замовчуванням: апка бачить відчинений капот (hu-decoder.md:59). Треба 2.
- EncCw → 0x85, а на стенді 0x85 = попередній (raise-protocol.md:367).
- Стрілки меню: стара сторінка шле 0x0C, правильно 0x0B.
- Напруги АКБ у профілі Toyota нема (лише 0x41 02 з VW). 7D 05, 06, 09 апка читає, прошивка не шле.
- Старих сторінок з малюванням не знайдено: у /display лише перегляд 48×48. Редактор — нова річ.

`;
function md(src){const out=[];let list=false,tbl=null;const inl=s=>esc(s).replace(/`([^`]+)`/g,'<code>$1</code>');
 const endL=()=>{if(list){out.push('</ul>');list=false}},endT=()=>{if(tbl){out.push('<div class="tw"><table>'+tbl.join('')+'</table></div>');tbl=null}};
 for(const raw of src.split('\n')){const l=raw.trimEnd();
  if(l.startsWith('|')){const c=l.replace(/^\||\|$/g,'').split('|').map(x=>x.trim());if(c.every(x=>/^:?-+:?$/.test(x)))continue;endL();
   if(!tbl)tbl=['<tr>'+c.map(x=>`<th>${inl(x)}</th>`).join('')+'</tr>'];else tbl.push('<tr>'+c.map(x=>`<td>${inl(x)}</td>`).join('')+'</tr>');continue}
  endT();if(l.startsWith('- ')){if(!list){out.push('<ul>');list=true}out.push('<li>'+inl(l.slice(2))+'</li>');continue}
  endL();if(l.startsWith('## '))out.push('<h3>'+inl(l.slice(3))+'</h3>');else if(l.startsWith('# '))out.push('<h2>'+inl(l.slice(2))+'</h2>');else if(l)out.push('<p>'+inl(l)+'</p>')}
 endL();endT();return out.join('')}
document.head.insertAdjacentHTML('beforeend',`<style>
.md{padding:4px 14px 14px;font-size:14px}
.md h2{font-size:18px;text-transform:none;letter-spacing:0;color:var(--text);padding:10px 0 4px}
.md h3{font-size:13px;text-transform:uppercase;letter-spacing:.07em;color:var(--mute);margin:18px 0 6px}
.md ul{margin:4px 0;padding-left:18px}.md li{margin:3px 0}
.md table{font-size:13px}.md td,.md th{padding:6px}
.links{display:flex;gap:16px;justify-content:center;padding:10px 0 4px;font-weight:700}
@media (min-width:900px){main>.links{column-span:all}}
</style>`);
SCR.audit={title:'Аудит',hidden:true,render(m){m.innerHTML='<section class="card"><div class="md">'+md(AUDIT)+'</div></section><div class="links"><a href="#board">← Плата</a></div>'}};
const board=SCR.board.render;
SCR.board.render=async function(m){await board.call(this,m);m.insertAdjacentHTML('beforeend','<div class="links"><a href="#audit">Аудит UI</a></div>')};
})();
