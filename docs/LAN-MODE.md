# Using Orio with a Bambu Lab printer in LAN mode

Orio talks to your printer directly over your local network: no Bambu account, no cloud. Your files never leave your home.

> **What you need:** the printer and the PC on the **same local network** (same router), Orio installed, and 5 minutes.

## 1. Switch the printer to LAN mode

On the printer's screen:

1. Open **Settings** (gear icon) > **Network** (WLAN on some models).
2. Turn on **LAN Only Mode**.
3. Write down the **Access Code** (8 characters) and the **IP address** shown on the same screen.

> On recent firmware a **Developer Mode** switch appears next to LAN Only Mode. Orio's basic features (sending, printing, monitoring) do not need it. The **G-code console** may need it to send commands. Turn it on only if you need it: it lets any program on your network control the printer.

## 2. Install the network plug-in (once)

Bambu printers are driven through Bambu Lab's own network plug-in, the same one Bambu Studio uses. It is not included in the Orio download.

1. Start Orio. When it asks you to install the network plug-in, accept (**Install Network Plug-in**).
2. Orio downloads it from Bambu Lab and asks for a restart. Restart Orio.

You do **not** need to log in to a Bambu account for LAN mode: close the login window if it appears.

## 3. Add the printer

1. Open the **Printers** tab and click the printer name at the top left: the list of printers opens.
2. Click **+ Bind with Access Code**.
3. Enter the printer's **IP address** and **Access Code** from step 1. Orio checks them and says *"IP and Access Code Verified!"*. Your printer now shows under **My Device** with "(LAN)" after its name.

Orio connects and shows the live view, temperatures and AMS. Next time, just pick the printer in that list.

**Still not working?** Check that the PC and the printer are on the same network, and see **Can't find my devices?** at the bottom of the list.

## 4. Print

1. In **Prepare**, choose your printer model and plate, place your parts.
2. Click **Slice**, then **Print**.
3. Pick the printer, check the AMS slots (click a slot to change its filament), then **Send**.

The window closes at once and the transfer goes on in the background: you can start the next print right away. To send every plate of a project to several printers at once: **Print > Print all**.

## What works in LAN mode

| Feature | LAN mode |
|---|:---:|
| Send and start a print, AMS mapping | Yes |
| Live camera, temperatures, fans, progress | Yes |
| Pause / resume / stop | Yes |
| Exclude a failed part during the print (also on P1S) | Yes |
| Send all plates to several printers (Print all) | Yes |
| G-code console | Yes (Developer Mode may be needed) |
| Printing from outside your home | No (needs the Bambu cloud) |
| MakerWorld, cloud history, firmware updates from the app | No |

Firmware updates are still possible from the printer itself or with an SD card, as documented by Bambu Lab.

## Troubleshooting

- **"Connection failed"**: check the IP and Access Code on the printer screen again. The Access Code changes when LAN mode is turned off and on.
- **The printer drops after a while**: Orio reconnects by itself; if a setting was lost meanwhile, Orio tells you.
- **A firewall or VPN blocks the connection**: allow Orio on private networks, and disconnect the VPN while printing.
- Still stuck? Ask in [Discussions > Q&A](https://github.com/OrioSlicer/Orio/discussions/categories/q-a) with your printer model and firmware version.
