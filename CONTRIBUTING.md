# Contributing to Orio

Thank you for helping! Here is how to do it well.

## Report a bug

Open an [issue](https://github.com/OrioSlicer/Orio/issues/new/choose) (also from Orio: *Help > Report issue*) and fill in the form:

- Orio version (*Help > About*), Windows version, printer model;
- what you did, what you expected, what happened;
- **the 3MF project** (*File > Save project*). Most bugs can't be reproduced without it;
- for a crash: the log files from `%APPDATA%\Orio\log` (plain text, so have a look before sharing: they may contain your file names).

One issue per bug, please.

## Suggest an idea

Ideas go to [Discussions > Ideas](https://github.com/OrioSlicer/Orio/discussions/categories/ideas), not to issues. Search first: if the idea exists, upvote it and add your use case. Real use cases are what decide priorities.

## Translate Orio

Orio's interface is translated with gettext files (the names still say "BambuStudio", inherited from the base):

- `bbl/i18n/<code>/BambuStudio_<code>.po`: the source of each language, editable with [Poedit](https://poedit.net/) (free);
- `languages/<code>/BambuStudio.mo`: the compiled file Orio downloads from *Preferences > Language*.

To improve a language: edit the `.po` file, check it in Poedit, and open a pull request with the `.po`. To add a new language, open a discussion first so we can set up its folder.

Orio's own additions are often the least translated strings: they are a great place to start.

## Code

Pull requests are welcome for bug fixes. For a new feature, open a discussion first: Orio keeps a consistent design, and a short conversation saves you work.

- Windows build: `build_win.bat` (Visual Studio 2022, CMake). Dependencies are built with the scripts of the `deps` folder.
- Keep projects compatible: a 3MF saved by Orio must still open in Bambu Studio.
- By contributing, you agree that your contribution is licensed under the AGPL-3.0, like the rest of Orio.

## Be kind

Be respectful and patient with everyone, beginners first. Disrespectful messages are removed.
