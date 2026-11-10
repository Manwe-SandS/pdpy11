<p align="center">
  <img src="logo.png" alt="pdpy11" width="240">
</p>

# PDPy11 (версия на Си)

[English](README.md) · **Русский**

Кросс-ассемблер для PDP-11 и родственных машин (Электроника БК-0010 / БК-0011М, УКНЦ, Союз-Неон, Электроника-60).

Это перенос [PDPy11](https://github.com/pdpy11/pdpy11) 2.0.4 с Python на переносимый однопоточный ISO C11 без внешних
зависимостей. Для корректных программ он создаёт побайтно такие же выходные файлы, как версия на Python, работает
примерно в 100 раз быстрее и расходует примерно в 5 раз меньше памяти.

(c) Ivanq & Manwe 2018-2026. Распространяется на условиях GNU GPLv3 или (по вашему выбору) любой более поздней версии.

## Релизы

Готовые программы для всех платформ выложены на странице
[**Releases**](https://github.com/Manwe-SandS/pdpy11/releases/latest)
(контрольные суммы лежат в файле `SHA256SUMS`):

| Платформа | Скачать |
|---|---|
| Windows 64 бита | [pdpy11-v2.1.1-windows-x64.zip](https://github.com/Manwe-SandS/pdpy11/releases/download/v2.1.1/pdpy11-v2.1.1-windows-x64.zip) |
| Windows 32 бита | [pdpy11-v2.1.1-windows-x86.zip](https://github.com/Manwe-SandS/pdpy11/releases/download/v2.1.1/pdpy11-v2.1.1-windows-x86.zip) |
| macOS (Apple silicon и Intel) | [pdpy11-v2.1.1-macos-universal.zip](https://github.com/Manwe-SandS/pdpy11/releases/download/v2.1.1/pdpy11-v2.1.1-macos-universal.zip) |
| Linux x86-64 | [pdpy11-v2.1.1-linux-x64.tar.gz](https://github.com/Manwe-SandS/pdpy11/releases/download/v2.1.1/pdpy11-v2.1.1-linux-x64.tar.gz) |
| Linux ARM64 | [pdpy11-v2.1.1-linux-arm64.tar.gz](https://github.com/Manwe-SandS/pdpy11/releases/download/v2.1.1/pdpy11-v2.1.1-linux-arm64.tar.gz) |

В каждом архиве лежат компилятор, конвертер исходников `convert-to`, файлы для Sublime Text (подсветка синтаксиса,
цветовая схема, система сборки), [INSTALL.md](INSTALL.md) с пошаговой инструкцией по установке (по-русски:
[docs/INSTALL-SUBLIME-RU.md](docs/INSTALL-SUBLIME-RU.md)) и документация.

## Сборка из исходников

```
make                 # macOS / Linux / MinGW; или: cc -O2 -std=c11 -o pdpy11 src/*.c -lm
make test            # набор тестов
```

Windows с MSVC: запустите `build.bat` из Developer Command Prompt. Для вывода звука в Windows нужна библиотека `winmm`
(`build.bat` и Makefile под MinGW подключают её автоматически).

## Использование

```
pdpy11 [-o outfile] [--bin | --raw | --sav | --wav | --turbo-wav] [--link N] [--play | --play-turbo]
       [--lst] [--implicit-bin] [--charset encoding] [--source-charset encoding]
       [--report-format graphical|bare] [-Wxxx] infile [infile ...]
```

| Ключ | Значение |
|---|---|
| `-o outfile` | имя выходного файла |
| `--bin`, `--raw`, `--sav`, `--wav`, `--turbo-wav` | формат выходного файла (`.bin` с заголовком БК, «сырой» образ, `.sav` для RT-11, WAV с записью для ленты БК, WAV с записью в турбо-формате) |
| `--link N` | адрес компоновки по умолчанию (восьмеричный, по умолчанию `1000`); `.link` в исходном тексте его переопределяет |
| `--play`, `--play-turbo` | проиграть программу через звуковой выход в ленточном формате БК (обычном или турбо) |
| `--implicit-bin` | записать `<исходник>.bin`, если ни исходный текст, ни командная строка не задают выходной файл |
| `--lst` | создать файл листинга |
| `--charset`, `--source-charset` | кодировка строк в выходном файле / кодировка исходных файлов (UTF-8, UTF-16, CP1251, CP866, KOI8-R определяются автоматически) |

Формат выходного файла можно выбрать и в исходном тексте:

```asm
make_bin   ["file.bin"]
make_raw   ["file"]
make_sav   ["file.sav" [, final_address]]
make_wav        ["file.wav" [, "имя на ленте"]]     ; "~speaker" вместо имени файла проигрывает звук
make_turbo_wav  ["file.wav" [, "имя на ленте"]]
play_wav        ["имя на ленте"]                    ; проигрывает вместо записи файла
play_turbo_wav  ["имя на ленте"]
```

Звук проигрывает система: PlaySound в Windows, `afplay` в macOS, `paplay` или `aplay` в Linux.

Язык и командная строка описаны в [docs/SYNTAX-RU.md](docs/SYNTAX-RU.md).
Текстовые макросы, условную компиляцию и блоки повторения подробно описывает [docs/MACROS-RU.md](docs/MACROS-RU.md).
Конвертер `convert-to` (входит в каждый архив релиза, исходный текст — `tools/convert-to.c`) переписывает исходные
тексты, написанные для других ассемблеров, в стиле PDPy11, раскрывая макросы и условную компиляцию: см.
[docs/CONVERT-TO-RU.md](docs/CONVERT-TO-RU.md).

В `tests/DIFFERENCES.md` перечислены намеренные отличия от версии на Python.
