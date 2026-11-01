# Сборка инструментов на Apple Silicon

Использован OpenOrbis v0.5.4 с готовыми PS4-библиотеками. Его macOS-упаковщики
имеют архитектуру x86_64 и не запустились на этом Mac. Rosetta не устанавливалась.
Инструменты пересобраны нативно.

1. Установлены Homebrew `llvm`, `lld` и `go`. Профиль shell и системный clang
   не менялись. LLVM и LLD вызываются по абсолютному пути.
2. Архив OpenOrbis распакован в рабочую папку проекта. Системный установщик
   OpenOrbis не запускался.
3. `create-fself` собран из закреплённого commit:

```sh
cd create-fself/cmd/create-fself
go build -modfile=go-linux.mod -o /absolute/path/native-bin/create-fself .
```

4. Установлен локальный .NET SDK 8.0.427 osx-arm64 (в рабочую папку, без
   системного установщика). SHA512 архива проверен по Microsoft release metadata.
   В локальной копии двух проектов LibOrbisPkg заменён TargetFramework
   `netcoreapp3.0` на `net8.0`; код упаковщика не изменён.

```sh
/absolute/path/dotnet/dotnet build PkgTool.Core/PkgTool.Core.csproj \
  -c Release -o /absolute/path/native-bin/pkgtool
```

5. Wrapper для PS4_PKGTOOL:

```sh
#!/bin/sh
exec /absolute/path/dotnet/dotnet /absolute/path/pkgtool/PkgTool.Core.dll "$@"
```

6. Запускается `scripts/build.py` с переменными из README. Скрипт сохраняет
   имя `ld.lld` при запуске: разрешение симлинка в `lld` меняет выбор драйвера.

Generated font_atlas.h, noto_font.h и web_page.h включены в исходники.
Для новых release-ресурсов: python3 scripts/generate_release_assets.py (Pillow).
Noto Sans и OFL включены; иконка создана кодом проекта. Старый font_atlas.h
используется только экраном технической диагностики.

Проверки упаковщика не заменяют запуск на PS4. При изменении библиотек или
прошивки повторить проверку установки, изображения, звука и сети.
