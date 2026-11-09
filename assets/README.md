# Assets

## Icons (`icons/mdi--*.svg`)

The browser's own interface (tabs, toolbar, window buttons, address bar) uses these icons.

### Where they come from

- **Set:** Material Design Icons (MDI), by [Pictogrammers](https://pictogrammers.com/).
- **Downloaded from:** <https://icon-sets.iconify.design/mdi/> (Iconify), on 9 November 2026.
- **License:** Apache License 2.0. The icons belong to Pictogrammers and the other contributors of the set. The license
  text is at <https://www.apache.org/licenses/LICENSE-2.0>. The icons are used as they were downloaded, with the file
  names Iconify gives them.

### What the `mdi--` in the file names means

Iconify names a file `<set>--<icon>.svg`: the part before `--` is the short name of the icon set (`mdi` = Material
Design Icons), the part after it is the name of the icon inside that set. So `mdi--chevron-left.svg` is the icon
`chevron-left` of the set `mdi`. Other Iconify sets would have another prefix (`lucide--`, `tabler--` ...), so icons of
different sets can live in the same folder without clashing.

### Rules for the files

- 24 x 24 `viewBox`, one colour, drawn with `fill="currentColor"`: the colour comes from the CSS `color` of the place
  where the icon is shown, so the same file works in the light and the dark theme.
- The icons are put into the interface's page when the program is built (`tools/EmbedIcons.cpp`): the page writes `{{icon:chevron-left}}` where `mdi--chevron-left.svg` is to be.
- Use only icons of the one set (MDI, filled style), so the interface looks the same everywhere.
- To add an icon: download it from Iconify as SVG with the same set, keep the file name Iconify gives, put it in `icons/`, and add a row to
  the table below.

### Which icon is used for what

| File | Used for |
|---|---|
| `mdi--chevron-left.svg` | Back |
| `mdi--chevron-right.svg` | Forward |
| `mdi--reload.svg` | Reload |
| `mdi--close.svg` | Close a tab, close the window |
| `mdi--add.svg` | New tab |
| `mdi--minimize.svg` | Minimize the window |
| `mdi--crop-square.svg` | Maximize the window |
| `mdi--image-filter-none.svg` | Restore the window (two squares) |
| `mdi--security.svg` | Secure (https) page |
| `mdi--security-off.svg` | Page that is not secure (http) |
| `mdi--public.svg` | Generic site icon (a globe) |
| `mdi--hamburger-menu.svg` | Menu button |
| `mdi--star.svg` / `mdi--star-outline.svg` | Bookmarked / not bookmarked |
| `mdi--home.svg` | Home |
| `mdi--tray-download.svg` | Downloads |
| `mdi--settings.svg` | Settings |
| `mdi--search.svg` | Search, find in page |
| `mdi--history.svg` | History |
| `mdi--fullscreen-exit.svg` | Leave full screen |
| `mdi--volume-high.svg` / `mdi--volume-mute.svg` | A tab is playing sound / is muted |
| `mdi--content-copy.svg` | Copy the address |
| `mdi--open-in-new.svg` | Open in a new window |
| `mdi--arrow-down-drop.svg` | Drop-down arrow |
| `mdi--warning.svg` | Certificate or security warning |
| `mdi--tune.svg` | Site permissions and settings (the button at the left of the address) |
