# Open in Admin Terminal — Windhawk Mod

**TL;DR:** Right-click any folder or drive in File Explorer and open an elevated terminal there — no more navigating after the fact.

## With this mod you can:

- Right-click a folder background and open an admin terminal in that location
- Right-click a folder item and open an admin terminal inside it
- Right-click a drive and open an admin terminal at its root
- Choose your preferred terminal: Windows Terminal, PowerShell 7, Windows PowerShell, Command Prompt, or a custom command
- Customize the context menu label, or let the mod use a smart default based on your terminal choice
- Optionally append the terminal name to a custom label (e.g. "Open elevated (Windows Terminal)")

## Preview

![Normal usage — right-click folder background or folder item to open elevated Windows Terminal](https://i.imgur.com/qEgGpvc.gif)

*Right-clicking a folder item to open an elevated terminal in that location (Windows Terminal)*

![Context menu with Windows Terminal option](https://i.imgur.com/HziEs16.png) ![Context menu with PowerShell 7 option](https://i.imgur.com/CCE3vlP.png)

![Mod settings showcase](https://i.imgur.com/8KgCdju.gif)

*Settings panel — choose terminal, customize label, toggle terminal name appending*

Screenshots may show earlier builds, but current releases use runtime classic-menu injection. The mod does not write Explorer context menu entries to the registry.

## Files

- `open-in-admin-terminal.wh.cpp` — Windhawk mod source

## How to use

1. Open Windhawk.
2. Create a new mod.
3. Paste in `open-in-admin-terminal.wh.cpp`.
4. Compile and enable it.

## Notes

- On Windows 11, Explorer may place this entry under `Show more options` depending on your context menu setup.
- The entry is injected only while Explorer's classic menu is open; disabling the mod leaves no registry cleanup behind.
- The mod intentionally targets filesystem folders and drive roots only.
- Routine diagnostics are quiet by default. Enable debug logging in the settings when troubleshooting target detection or launch behavior.

## Version log

- 1.9: Added quiet-by-default debug logging, improved menu placement, tightened filesystem target eligibility, and refreshed docs for classic-menu runtime injection.
- 1.8: Switched to direct Explorer classic-menu injection with no persistent registry writes.
