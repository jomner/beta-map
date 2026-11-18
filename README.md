# beta-map

beta-map makes an isometric web map of a Minecraft Beta world. Every block is drawn as a 4-pixel cube, and you can
turn the map to see it from all four directions.

![The map](screenshot.png)

- **Fast.** A drawer written in C does the work. It uses little memory, and on each run it draws again only the
  parts of the world that changed.
- **A static website.** The output is a folder of files. You can put it on any web host, for example GitHub Pages
  (free).
- **Read-only.** beta-map only reads the world. It never changes it.
- **Live players (optional).** The map can show the players who are online, with their skin faces. A panel lists
  everyone who has ever joined, with search and their statistics.

## Worlds

beta-map reads worlds in the McRegion format (Beta 1.3 to Beta 1.8). These worlds have `region/r.X.Z.mcr` files.

## Make a map

You need Python 3.8 or newer. For the fast drawer you also need a C compiler and zlib:

- Debian, Ubuntu, Raspberry Pi OS: `sudo apt install gcc zlib1g-dev`
- macOS: `xcode-select --install`

Then:

1. Get a copy of your world folder: the folder with `level.dat` and `region/` in it. A backup is best. A world that
   the server is writing to also works, but it can catch chunks that are only half saved.
2. Run this command:

   ```
   python beta_map.py path/to/world site --title "My Server"
   ```

   On Windows, use `py` if `python` isn't found.
3. The `site` folder is now the whole website. To see it on your computer, run:

   ```
   python -m http.server 8000 --directory site
   ```

4. Open http://localhost:8000 in a browser.

Don't open `index.html` straight from the folder: browsers don't let a page on your disk load its own map files.

### The drawer

On the first run, `beta_map.py` builds the drawer from `beta-map-draw.c`, and it builds it again when
`beta-map-draw.c` changes (after an update). You don't have to do anything.

If there's no C compiler, `beta_map.py` draws in Python and shows a warning. Python draws the same map, but it's
much slower and needs much more memory. On Windows, use WSL to get the drawer.

The drawer keeps the tiles between runs. On each run it finds the region files that changed, and draws only those
regions and the tiles they touch. Times on a PC, for a world of 67 region files (each covers 512 x 512 blocks):

| Run | Time |
| --- | --- |
| First run | 34 seconds |
| Nothing changed | Less than 1 second |
| One region changed | 2.5 seconds |

If a run fails or you stop it, the website stays as it was.

On a very slow computer, use `--views 1`. The drawer then draws one direction instead of four: a quarter of the work
and of the disk space. The page then has no turn buttons.

### Options

| Option | What it does |
| --- | --- |
| `--title "Name"` | The name at the top left of the page |
| `--places places.txt` | Named places on the map, one per line: `overworld 120 -340 My Castle` (or `nether`). A line that starts with `#` is a comment |
| `--spawn X,Z` | Where the spawn marker goes, and where the map opens (default: the world's spawn point) |
| `--live URL` | The address of a `players.json` file, for live players (see below) |
| `--no-player-count` | Hide the "Visitors" counter (bottom right: how many players have ever joined, counted from the world's `players` folder) |
| `--every HOURS` | If you draw the map on a schedule (cron, Task Scheduler): how many hours apart, 1, 2, 3, 4, 6, 8, 12 or 24. The page then shows "Next update 6:30 PM (in 2h 5m)" in each viewer's own time zone. Keep it the same as your schedule |
| `--at HH:MM` | With `--every`: the time of one of the draws, in your computer's own time, like cron (default `00:00`). Example: cron `30 */6 * * *` is `--every 6 --at 00:30` |
| `--cache DIR` | Where the drawings are kept between runs (default: the output folder's name plus `-cache`) |
| `--views N` | How many of the four directions to draw, 1 to 4 (default 4) |
| `--png-level N` | Tile compression, 1 (fastest, the default) to 9 (smallest files, slower) |
| `--drawer PATH` | Use this drawer |
| `--no-drawer` | Draw in Python, even if there's a compiler |
| `--jobs N` | Python only: how many processes draw at the same time (default: your CPU cores minus one) |

### Your own changes to the page

Don't edit `index.html`. Each run copies a fresh `index.html` into the site, and an update of beta-map replaces it.

Put your changes in `custom.css` and `custom.js` in the site folder (next to `index.html`). The page loads them if
they're there, and beta-map never writes or replaces them, so they survive every run and every update. CSS can
restyle anything (colours, sizes, hiding a panel); `custom.js` runs after the page is set up, for anything else.

## Put the map on GitHub Pages

1. Make a new GitHub repository for the website.
2. Copy everything in your output folder into it, including the hidden `.nojekyll` file.
3. Commit and push.
4. In the repository's Settings > Pages, select "Deploy from a branch", your main branch, and `/ (root)`.

To update the map, run beta-map again into the same folder, then push again. Only the tiles that changed are new
files, so each push stays small.

If the repository grows too big over time, push the site as one fresh commit (`git checkout --orphan`) and
force-push it.

## Live players (optional)

The page reads a `players.json` file every 2 seconds:

```json
{"time": 1700000000, "players": [{"name": "Steve", "x": 12.5, "y": 70.0, "z": -40.2, "world": "overworld"}]}
```

- `time` is in Unix seconds. When it's more than 30 seconds old, the page says the server is offline.
- `world` is `overworld` or `nether`.
- Each player can also have `joined` (Unix seconds, for "Online for"), and `mined`, `placed` and `walked`
  (lifetime totals).

Something on your server has to write this file and serve it over HTTPS with an `Access-Control-Allow-Origin: *`
header. Then give its address to beta-map with `--live https://example.com/players.json`.

### The Players panel

The Players panel is at the bottom left (the "N online" line and the Visitors counter open it too). It lists
everyone who has ever joined: who's online first, then everyone else by play time. Click a name to see that
player's card. Click someone who is online to follow them. A link that ends in `#follow=Name` opens the map
following that player.

The list comes from two places:

- The world's `players` folder, read when the map is drawn (only with `--live`, and not with `--no-player-count`).
  So players from before you set up live players are listed too, without statistics.
- An optional `playtime.json` file next to `players.json`, read every minute:

  ```json
  {"players": [{"name": "Steve", "first": "2026-01-02 15:04", "last": "2026-03-04 18:30", "secs": 7200, "mined": 950, "placed": 1200, "walked": 15000}]}
  ```

  `first` and `last` are in UTC. Every field other than `name` is optional.

An optional `profiles.json` file next to `players.json` gives players a skin face and a colour:

```json
{"Steve": {"skin": "https://textures.minecraft.net/texture/...", "colour": "#ff8800"}}
```

### Server plugins

- **Betrock++:** `extras/livemap.lua` writes `players.json` and `playtime.json`. It needs the bpa_lua addon (the
  instructions are at the top of the file). It keeps its own record in `livemap-players.tsv` in the server folder,
  so the statistics count from when you install it.
- **Other servers** (Poseidon, Bukkit and so on) need a small plugin that writes the same files. Pull requests are
  welcome.

## How it works

- **Reading:** beta-map reads the chunks of each region file itself and finds every block that can be seen: one
  with a face open to the air or to a see-through block.
- **Drawing:** each visible block becomes a 4 x 4 pixel cube, with a lighter top and two darker sides. Regions are
  drawn back to front, so nearer blocks cover the ones behind. Water and glass are see-through, torches glow in
  their own layer, and flowers, rails, fences and signs are small shapes.
- **Four directions:** beta-map turns the world a quarter turn and draws it again, four times, for the turn buttons.
- **Tiles:** the pictures are cut into squares of 512 x 512 pixels, with smaller copies for zooming out. The page
  (built on [Leaflet](https://leafletjs.com/)) loads only the squares you're looking at.
- **Heights:** a hidden layer keeps the height of each column, so the coordinates under the cursor, the places and
  the players sit exactly on the 3D picture.
- **Minimap:** a top-down picture, one pixel per block.
- **Only the changes:** the drawer keeps each region's drawing in the cache, cut into tile pieces. When a region
  changes, it rebuilds only the tiles that region touches. Each tile address ends in `?v=` and the draw's id, so
  browsers never show an old tile.
- **Safe updates:** the drawer puts new tiles in `tiles/c/.next` first, and moves them into the site only at the end
  of a run.

## Tests

To test the drawer, run this on Linux or macOS, with a copy of a world (the test adds two region files and removes
them again):

```
bash tests/drawer-test.sh path/to/world-copy
```

It checks that the drawer makes the same pixels as Python, also after a Python run in between, a deleted site, a run
that fails halfway, and a change of `--views`. `tests/fuzz.py` tests the drawer with damaged region files (the
instructions are at the top of the file).

## Credits and licences

- beta-map: MIT licence (see `LICENSE`).
- [Monocraft](https://github.com/IdreesInc/Monocraft) font: SIL Open Font License, `web/Monocraft-OFL.txt`.
- [Leaflet](https://leafletjs.com/) 1.9.4: BSD 2-Clause, `web/Leaflet-LICENSE.txt`.

Minecraft is a trademark of Mojang. This project isn't affiliated with Mojang or Microsoft.
