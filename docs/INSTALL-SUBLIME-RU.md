## Установка PDPy11 и Sublime Text (Си-версия)

Python, brew и pipx не нужны. Установка — это копирование четырёх файлов.

1. Скачайте и установите [Sublime Text](https://www.sublimetext.com/download). Запустите его один раз и закройте.
2. В Sublime Text выберите **Preferences / Browse Packages** и откройте папку `User`:
   * macOS: `~/Library/Application Support/Sublime Text/Packages/User`
   * Windows: `%APPDATA%\Sublime Text\Packages\User`
   * Linux: `~/.config/sublime-text/Packages/User`
3. Положите в неё файлы `pdpy11`, `PDPy11.sublime-syntax`, `PDPy11.sublime-color-scheme` и `pdpy11.sublime-build`.
4. На macOS и Linux сделайте компилятор исполняемым: `chmod +x pdpy11`. Если файл скачан браузером, на macOS ещё выполните `xattr -d com.apple.quarantine pdpy11`.
5. Откройте любой файл `.mac` в Sublime Text. Подсветка включится сама. Нажмите **Command+B** (на Windows и Linux — **Ctrl+B**): программа соберётся, рядом появятся `.bin` и `.lst`, а ошибки в панели внизу можно открывать щелчком.

В `pdpy11.sublime-build` компилятор указан как `$packages/User/pdpy11`. `$packages` — переменная самого Sublime Text, поэтому путь вписывать не нужно.

Если раньше вы ставили Python-версию, просто замените старый `Packages/User/pdpy11.sublime-build` новым. Обновление — замена файлов, удаление — удаление этих четырёх файлов.

### Запуск в эмуляторе

Чтобы после сборки запускался эмулятор, откройте `pdpy11.sublime-build` в папке `User` и в нём:

* закомментируйте строку `"cmd"`;
* раскомментируйте одну из строк `"shell_cmd"` для вашей системы;
* замените в ней путь к эмулятору на свой.

Эмулятор запускается, только если сборка прошла без ошибок.
