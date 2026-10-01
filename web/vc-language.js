// Browser text is Unicode; only the Russian H: README is converted to CP866.
// Footer translations: docs/translations-2026-10-01.md.
const texts = {
  en: Object.freeze({
    language: "en",
    footer: "Volkov Commander 4.99, machine-translated from 8086 assembly to WebAssembly. "
      + "Not an emulator. Files live in memory and vanish on reload. ",
    linux: "Linux version on GitHub.",
    fontCredit: "IBM VGA font: ",
    quit: "VC has quit. Press any key to start it again.",
    keyboard: "Keyboard",
  }),
  ru: Object.freeze({
    language: "ru",
    footer: "Volkov Commander 4.99, автоматически переведен с ассемблера 8086 в "
      + "WebAssembly. Это не эмулятор. Файлы живут в памяти и исчезают при обновлении страницы. ",
    linux: "Версия для Linux доступна на GitHub.",
    fontCredit: "Шрифт IBM VGA: ",
    quit: "VC завершил работу. Нажмите любую клавишу, чтобы запустить его снова.",
    keyboard: "Клавиатура",
  }),
  uk: Object.freeze({
    language: "uk",
    footer: "Volkov Commander 4.99, автоматично перекладений з асемблера 8086 на "
      + "WebAssembly. Це не емулятор. Файли живуть у пам'яті та зникають при оновленні сторінки. ",
    linux: "Версія для Linux доступна на GitHub.",
    fontCredit: "Шрифт IBM VGA: ",
    quit: "VC завершив роботу. Натисніть будь-яку клавішу, щоб запустити його знову.",
    keyboard: "Клавіатура",
  }),
};

// navigator.language is a BCP 47 tag, so a region/script suffix does not
// change the language. Unknown or unavailable preferences retain English.
export function pageText(languageTag) {
  const language = typeof languageTag === "string" ? languageTag.toLowerCase().split("-")[0] : "";
  return texts[language === "ru" || language === "uk" ? language : "en"];
}
