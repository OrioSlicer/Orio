# Frequently asked questions

### Is Orio free?
Yes, completely, and it will stay free. It is open source under the AGPL-3.0 license.

### Does Orio replace Bambu Studio?
It can. Orio does everything Bambu Studio does (it is built on it) and adds its own tools. You can also keep both: Orio has its own data folder (`%APPDATA%\Orio`) and does not touch Bambu Studio's.

### Can I open my Bambu Studio projects?
Yes. Bambu Studio 3MF projects open in Orio, and projects saved by Orio stay readable by Bambu Studio. Features only Orio knows about are simply ignored there. OrcaSlicer projects and profiles are read too.

### Does it work with non-Bambu printers?
Yes: 61 brands and about 350 printer models (Prusa, Creality, Elegoo, Voron, Anycubic, Qidi…) with OrcaSlicer's filament library. Printing to them works by exporting the G-code, or through their own network tools.

### Why does Windows say "Windows protected your PC"?
Orio is not code-signed: a code-signing certificate costs several hundred dollars a year. Click **More info**, then **Run anyway**. Every release publishes the SHA-256 checksum of the zip, and the source code is in this repository.

### Is Orio safe? Does it send my data anywhere?
Orio has no telemetry. It only connects to the internet to download a language you ask for, Bambu Lab's network plug-in (for Bambu printers) and printer/filament profile updates. In LAN mode, your files go straight from your PC to your printer.

### Why LAN mode and not the Bambu cloud?
LAN mode keeps everything local, works without an account and without internet, and is not affected by changes to the cloud. See the [LAN mode guide](LAN-MODE.md).

### Mac or Linux?
Not yet. Orio is Windows-only for now; a macOS version is on the [roadmap](ROADMAP.md).

### A feature is marked "Beta". Can I use it?
Yes. Beta features work, but they are still being polished: watch the first print, and please report what you notice.

### My part "disappeared" after I changed a setting!
It did not: the preview is just out of date. Orio shows a card in the middle of the view: click **Re-slice** (Ctrl+R), or tick **Auto-slice** at the bottom of the legend.

### How do I get another language?
*Preferences > Language*: languages other than English and French download in a few seconds.

### How can I help?
Star the repository, share your prints in [Show and tell](https://github.com/OrioSlicer/Orio/discussions/categories/show-and-tell), suggest ideas, report bugs with the 3MF project, or translate Orio (see [CONTRIBUTING](../CONTRIBUTING.md)).
