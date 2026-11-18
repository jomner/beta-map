# beta-map

beta-map makes an isometric web map of a Minecraft Beta world. It shows each block as a small cube. You can see the
map from four directions.

![The map](screenshot.png)

- **Fast.** A drawer in C does the work. It uses little memory. It draws again only the parts of the world that
  changed.
- **A static website.** The output is a folder of files. You can put it on any web host, for example GitHub Pages.
- **Read-only.** beta-map does not change the world.
- **Live players (optional).** The map can show the players who are online. A panel shows all the players who
  joined, with their statistics.

## Worlds

beta-map reads worlds in the McRegion format (Beta 1.3 to Beta 1.8). These worlds have `region/r.X.Z.mcr` files.

## Make a map

You need Python 3.8 or newer. To make the drawer, you also need a C compiler and zlib:

- Debian, Ubuntu, Raspberry Pi OS: `sudo apt install gcc zlib1g-dev`
- macOS: `xcode-select --install`

Do the steps that follow:

1. Get a copy of your world folder. This is the folder that has `level.dat` and `region/` in it. A backup is
   best.
2. Run this command:

   ```
   python beta_map.py path/to/world site --title "My Server"
   ```

   On Windows, use `py` if `python` does not work.
3. The `site` folder is now the website. To see it on your computer, run this command:

   ```
   python -m http.server 8000 --directory site
   ```

4. Open http://localhost:8000 in a browser.

Do not open `index.html` directly from the folder. Browsers do not let a page on your disk load the map files.

### The drawer

On the first run, `beta_map.py` makes the drawer from `beta-map-draw.c`. It makes the drawer again when
`beta-map-draw.c` changes. You do not have to do this yourself.

If there is no C compiler, `beta_map.py` draws in Python and shows a warning. Python draws the same map, but it is
much slower and uses much more memory. On Windows, use WSL to get the drawer.

The drawer keeps the tiles between runs. On each run, it finds the region files that changed. It draws only these
regions and the tiles that they touch. Examples on a PC (67 region files):

| Run | Time |
| --- | --- |
| First run | 34 seconds |
| No change | Less than 1 second |
| One region changed | 2.5 seconds |

If a run fails or you stop it, the website does not change.

On a very slow computer, use `--views 1`. The drawer then draws one direction, not four. This is a quarter of the
work. The page then has no turn buttons.

### Options

| Option | Function |
| --- | --- |
| `--title "Name"` | The name at the top left of the page |
| `--places places.txt` | Names on the map. Write one place on each line: `overworld 120 -340 My Castle` (or `nether`). A line that starts with `#` is a comment |
| `--spawn X,Z` | The position of the spawn marker. The map opens there. The default is the spawn point of the world |
| `--live URL` | The address of a `players.json` file, for live players (see below) |
| `--no-player-count` | Hide the "Visitors" counter. This counter shows how many players joined the world |
| `--every HOURS` | How many hours there are between your scheduled draws: 1, 2, 3, 4, 6, 8, 12 or 24. The page then shows the time of the next update |
| `--at HH:MM` | With `--every`: the time of one of the draws, in the time of your computer (default `00:00`). For cron `30 */6 * * *`, use `--every 6 --at 00:30` |
| `--cache DIR` | The folder for the cache (default: the name of the output folder plus `-cache`) |
| `--views N` | How many directions to draw, 1 to 4 (default 4) |
| `--png-level N` | Tile compression, 1 (fastest, the default) to 9 (smallest files) |
| `--drawer PATH` | Use this drawer |
| `--no-drawer` | Draw in Python |
| `--jobs N` | Python only: how many processes draw at the same time (default: the number of CPU cores minus one) |

### Your changes to the page

Do not change `index.html`. Each run copies a new `index.html` into the site, and an update of beta-map replaces it.

Put your changes in `custom.css` and `custom.js` in the site folder. The page loads these files if they are there.
beta-map does not write or replace them.

## Put the map on GitHub Pages

1. Make a new GitHub repository for the website.
2. Copy all the files from your output folder into it. Include the hidden file `.nojekyll`.
3. Commit and push.
4. In Settings > Pages of the repository, select "Deploy from a branch", your main branch, and `/ (root)`.

To update the map, run beta-map again into the same folder. Then push again.

If the repository becomes too large, push the site as one new commit (`git checkout --orphan`) and force-push it.

## Live players (optional)

The page reads a `players.json` file every 2 seconds:

```json
{"time": 1700000000, "players": [{"name": "Steve", "x": 12.5, "y": 70.0, "z": -40.2, "world": "overworld"}]}
```

- `time` is in Unix seconds. If `time` is more than 30 seconds old, the page shows that the server is offline.
- `world` is `overworld` or `nether`.
- Each player can also have `joined` (Unix seconds), `mined`, `placed` and `walked` (lifetime totals).

Your server must write this file. It must also serve the file over HTTPS with the header
`Access-Control-Allow-Origin: *`. Give the address to beta-map with `--live https://example.com/players.json`.

### The Players panel

The Players panel is at the bottom left. It shows all the players who joined: first the players who are online,
then all other players. Click a name to see the card of that player. Click a player who is online to follow that
player. A link that ends in `#follow=Name` opens the map and follows that player.

The list comes from these sources:

- The `players` folder of the world. beta-map reads it when it draws the map (only with `--live`). These players
  show without statistics.
- An optional `playtime.json` file next to `players.json`. The page reads it every minute:

  ```json
  {"players": [{"name": "Steve", "first": "2026-01-02 15:04", "last": "2026-03-04 18:30", "secs": 7200, "mined": 950, "placed": 1200, "walked": 15000}]}
  ```

  `first` and `last` are in UTC. All fields other than `name` are optional.

An optional `profiles.json` file next to `players.json` gives each player a skin face and a colour:

```json
{"Steve": {"skin": "https://textures.minecraft.net/texture/...", "colour": "#ff8800"}}
```

### Server plugins

- **Betrock++:** `extras/livemap.lua` writes `players.json` and `playtime.json`. It needs the bpa_lua addon. The
  instructions are at the top of the file. The statistics start when you install it.
- **Other servers** (Poseidon, Bukkit): these need a plugin that writes the same files. Pull requests are welcome.

## How it works

- **Reading:** beta-map reads the chunks of each region file. It finds each block that has a face open to the air or
  to a transparent block.
- **Drawing:** each visible block is a cube of 4 x 4 pixels. Water and glass are transparent. Torches are in a
  separate layer.
- **Directions:** beta-map turns the world a quarter turn and draws it again, four times.
- **Tiles:** the pictures are cut into squares of 512 x 512 pixels, with smaller copies for zoom-out. The page
  ([Leaflet](https://leafletjs.com/)) loads only the squares that you see.
- **Heights:** a hidden layer keeps the height of each column. The page uses it to put the coordinates, the places
  and the players in the correct positions.
- **Minimap:** a top-down picture, one pixel for each block.
- **Changes only:** the drawer keeps the drawing of each region in the cache, cut into tile pieces. When a region
  changes, the drawer makes again only the tiles that this region touches.
- **Safe updates:** the drawer puts new tiles in `tiles/c/.next` first. It moves them into the site only at the end
  of a run.

## Tests

To test the drawer, run this command on Linux or macOS. Use a copy of a world.

```
bash tests/drawer-test.sh path/to/world-copy
```

The test makes sure that the drawer and Python make the same pixels. It also tests runs that fail, a deleted site and
a change of `--views`. `tests/fuzz.py` tests the drawer with damaged region files. The instructions are at the top of
the file.

## Credits and licences

- beta-map: MIT licence (see `LICENSE`).
- [Monocraft](https://github.com/IdreesInc/Monocraft) font: SIL Open Font License, `web/Monocraft-OFL.txt`.
- [Leaflet](https://leafletjs.com/) 1.9.4: BSD 2-Clause, `web/Leaflet-LICENSE.txt`.

Minecraft is a trademark of Mojang. This project is not affiliated with Mojang or Microsoft.
