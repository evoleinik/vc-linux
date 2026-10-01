## footer ru

Volkov Commander 4.99, автоматически переведен с ассемблера 8086 в
WebAssembly. Это не эмулятор. Файлы живут в памяти и исчезают при
обновлении страницы. Версия для Linux доступна на GitHub.

## footer uk

Volkov Commander 4.99, автоматично перекладений з асемблера 8086 на
WebAssembly. Це не емулятор. Файли живуть у пам'яті та зникають при
оновленні сторінки. Версія для Linux доступна на GitHub.

## readme ru

Volkov Commander в вашем браузере.

Это настоящий файловый менеджер Volkov Commander 4.99.
Его код на ассемблере 8086 автоматически переведен в WebAssembly.
Это не эмулятор.

Диск H: - это ваша песочница. Эти файлы живут только в памяти.
Обновите страницу, чтобы отменить все изменения и начать заново.
Командной оболочки нет. Команда cd работает.
Linux-версия работает с реальными файлами: github.com/evoleinik/vc-linux

Полезные клавиши:
  F3          Просмотр файла. Esc закрывает просмотр.
  F5          Копировать в другую панель.
  F6          Переименовать или переместить.
  F7          Создать каталог.
  F8          Удалить выбранные файлы.
  Ctrl-[      Вставить путь из левой панели в командную строку.
  Ctrl-]      Вставить путь из правой панели в командную строку.
  Ctrl-I      Вставить имена выбранных файлов в командную строку.
  Ctrl-H      Показать или скрыть скрытые файлы.
  Ctrl-L      Включить или скрыть информационную панель.
  Ctrl-Q      Включить панель быстрого просмотра.
  Alt-letter  Поиск по первым буквам имени файла.
  Alt-F10     Открыть дерево каталогов.
  Tab         Сменить панель.
  F10         Выход.

Введите GWBASIC для запуска BASIC. Попробуйте PRINT 2+2, а затем SYSTEM
для возврата.
В каталоге GAMES лежат двенадцать общедоступных BASIC-игр Дэвида Аля.
Откройте GAMES, выберите файл .BAS и нажмите Enter, чтобы поиграть.
Введите BOOTLOGO для запуска Logo с черепашьей графикой.
Ctrl+Pause или Ctrl+Shift+B останавливает программу.

## readme uk

Volkov Commander у вашому браузері.

Це справжній файловий менеджер Volkov Commander 4.99.
Його код на асемблері 8086 автоматично перекладено на WebAssembly.
Це не емулятор.

Диск H: - це ваша пісочниця. Ці файли живуть лише в пам'яті.
Оновіть сторінку, щоб скасувати всі зміни та почати спочатку.
Командної оболонки немає. Команда cd працює.
Версія для Linux працює з реальними файлами: github.com/evoleinik/vc-linux

Корисні клавіші:
  F3          Перегляд файлу. Esc закриває перегляд.
  F5          Скопіювати на іншу панель.
  F6          Перейменувати або перемістити.
  F7          Створити каталог.
  F8          Видалити вибрані файли.
  Ctrl-[      Вставити шлях з лівої панелі в командний рядок.
  Ctrl-]      Вставити шлях з правої панелі в командний рядок.
  Ctrl-I      Вставити імена вибраних файлів у командний рядок.
  Ctrl-H      Показати або сховати приховані файли.
  Ctrl-L      Увімкнути або сховати інформаційну панель.
  Ctrl-Q      Увімкнути панель швидкого перегляду.
  Alt-letter  Пошук за першими літерами імені файлу.
  Alt-F10     Відкрити дерево каталогів.
  Tab         Змінити панель.
  F10         Вихід.

Введіть GWBASIC для запуску BASIC. Спробуйте PRINT 2+2, а потім SYSTEM
щоб повернутися.
У каталозі GAMES лежать дванадцять загальнодоступних BASIC-ігор Девіда Аля.
Відкрийте GAMES, виберіть файл .BAS та натисніть Enter, щоб пограти.
Введіть BOOTLOGO для запуску Logo з черепашою графікою.
Ctrl+Pause або Ctrl+Shift+B зупиняє програму.

## notes

CP866 check was done mechanically: decoded all 256 bytes of CP866 in Python
and compared every character in the translations against that set (plus
ASCII). Result:

- Russian footer and Russian README: every character is a valid CP866 byte.
  No em dashes, curly quotes or ellipsis characters appeared in either draft,
  so no cleanup was needed on that front.
- One Russian README line ("Версия для Linux работает с реальными
  файлами: ...") was exactly 76 characters in the first draft, one over the
  limit. Reworded to "Linux-версия работает с реальными файлами: ..." (72
  characters) to clear it. All other lines in both Russian texts are under
  76 characters already.
- Ukrainian footer and Ukrainian README are NOT CP866-safe. CP866 has no
  code point at all for і (U+0456) or ґ (U+0491) - confirmed by decoding
  every byte 0x00-0xFF under CP866. It does have Ё ё, Є є, Ї ї and Ў ў, so
  it covers Russian plus a few extra Cyrillic letters, but not the full
  Ukrainian alphabet.
- і shows up 4 times in the Ukrainian footer and 45 times in the Ukrainian
  README above (words like "справжній", "файли", "інформаційну",
  "командний", "каталозі"). і is one of the most common letters in
  Ukrainian, so a natural, correctly worded translation cannot avoid it.
  Forcing і out would mean either wrong Ukrainian or visibly stilted
  phrasing - both texts above are written as normal, correct Ukrainian and
  will need real support for і (and, in other copy, ґ) to display right.
- ґ did not happen to appear in either Ukrainian text, so that specific gap
  did not bite here, but it is missing from CP866 the same way і is, and
  it would show up in other Ukrainian copy (e.g. "ґрунт", "ґудзик").
- The real fix is CP1125 ("DOS Ukrainian" / "RUSCII"), not CP866. Python's
  cp1125 codec is installed and working here: it keeps Ё ё, drops Ў ў, and
  adds Ґ ґ Є є І і Ї ї - the full Ukrainian alphabet. If the in-browser H:
  drive renders text through the same DOS code page the emulated Volkov
  Commander uses, the Ukrainian README/footer need to be shipped as CP1125
  bytes, not squeezed into CP866. The only CP866-native alternative is the
  Latin-i-for-Cyrillic-і visual substitution hack, which you already ruled
  out, so there is no actual way to write correct Ukrainian in CP866.
- All key names, commands and file names (F3, F5, F6, F7, F8, F10, Esc, Tab,
  Ctrl-[, Ctrl-], Ctrl-I, Ctrl-H, Ctrl-L, Ctrl-Q, Alt-letter, Alt-F10,
  Ctrl+Pause, Ctrl+Shift+B, cd, GWBASIC, PRINT 2+2, SYSTEM, GAMES, .BAS,
  BOOTLOGO, Logo, GitHub, github.com/evoleinik/vc-linux, H:, 4.99) were kept
  in Latin letters exactly as given in both languages.
