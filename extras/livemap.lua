-- beta-map live players, for Betrock++ (https://github.com/OpenBetaGroup/BetrockPlusPlus, a C++ Beta 1.7.3 server) with
-- its bpa_lua addon: put this file in the server's lua_scripts folder and make a "livemap" folder in the server folder.
-- Serve that one folder over HTTPS (with an "Access-Control-Allow-Origin: *" header, since the map page is on another
-- site) and pass the URL of its players.json to beta_map.py --live. It writes:
--   livemap/players.json   every 2 s: everyone online, where they are, and their lifetime stats (the dots)
--   livemap/playtime.json  every minute: everyone it has ever seen, for the page's players list and cards: first and
--                          last time online (UTC), time played, and blocks mined, placed and walked
-- Its own record of those is livemap-players.tsv in the server folder (not served). Counting starts when this file
-- is installed; players from before show in the list (from the world's player files) without stats.

local DIR = "livemap"
local OUT = DIR .. "/players.json"
local LIST = DIR .. "/playtime.json"
local RECORD = "livemap-players.tsv"
local EVERY = 40                 -- ticks: 2 s
local SAVE_EVERY = 1200          -- ticks: 60 s

local online = {}                -- player handle -> true once joined
-- This session, for the dot and "Online for": name, when they joined, when play time was last added, and where they
-- were at the last sample (walked is the straight-line distance between the 2-s samples; a jump of over 100 blocks is
-- a teleport and not counted).
local stats = {}                 -- player handle -> { name, joined, counted, x, z, w }
local TELEPORT = 100
local tick = 0

-- Everyone ever seen, by lower-case name (names aren't case sensitive in the game). Times are Unix seconds.
local everyone = {}              -- lower name -> { name, first, last, secs, mined, placed, walked }
local dirty = false
do
    local f = io.open(RECORD, "r")
    if f then
        for line in f:lines() do
            local n, a, l, s, m, p, w = line:match("^([^\t]+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t(%d+)\t([%d.]+)$")
            if n then
                everyone[n:lower()] = { name = n, first = tonumber(a), last = tonumber(l), secs = tonumber(s),
                                        mined = tonumber(m), placed = tonumber(p), walked = tonumber(w) }
            end
        end
        f:close()
    end
end
local function record(name)
    local k = name:lower()
    local t = everyone[k]
    if not t then
        local now = os.time()
        t = { name = name, first = now, last = now, secs = 0, mined = 0, placed = 0, walked = 0 }
        everyone[k] = t; dirty = true
    end
    t.name = name
    return t
end

local function json(s) return '"' .. s:gsub('[%c"\\]', function(c) return string.format("\\u%04x", c:byte()) end) .. '"' end
local function utc(t) return os.date("!%Y-%m-%d %H:%M", t) end

-- play time up to now for everyone online, so the saved record and the list are current
local function countTime()
    local now = os.time()
    for p, s in pairs(stats) do
        if online[p] then
            local t = record(s.name)
            t.secs = t.secs + math.max(0, now - s.counted); t.last = now
            s.counted = now; dirty = true
        end
    end
end

local function save()
    countTime()
    if not dirty then return end
    local lines, items = {}, {}
    for _, t in pairs(everyone) do
        lines[#lines + 1] = string.format("%s\t%d\t%d\t%d\t%d\t%d\t%.1f", t.name, t.first, t.last, t.secs, t.mined, t.placed, t.walked)
        items[#items + 1] = string.format('{"name":%s,"first":"%s","last":"%s","secs":%d,"mined":%d,"placed":%d,"walked":%d}',
            json(t.name), utc(t.first), utc(t.last), t.secs, t.mined, t.placed, math.floor(t.walked))
    end
    table.sort(lines); table.sort(items)
    local f = io.open(RECORD .. ".tmp", "w")
    if f then
        f:write(table.concat(lines, "\n"), "\n"); f:close()
        os.rename(RECORD .. ".tmp", RECORD)
        dirty = false
    end
    f = io.open(LIST .. ".tmp", "w")
    if f then                                          -- (no livemap folder yet: just the record)
        f:write('{"time":', os.time(), ',"players":[', table.concat(items, ","), ']}'); f:close()
        os.rename(LIST .. ".tmp", LIST)
    end
end

-- Which world handle is the Nether: the addon API doesn't say, but the Nether has a bedrock ceiling at y 127 and the
-- Overworld never does. Only ever looked at inside a block event, where the game has that chunk loaded (reading a
-- block at a player's spot from the tick crashed the server while the player was still joining). Until a world has
-- had a block event it counts as the Overworld.
local isNether = {}
local function learn(w, x, z)
    local k = tostring(w)
    if isNether[k] == nil and w then isNether[k] = world.getBlock(w, x, 127, z).id == 7 end
end
local function worldName(w) return isNether[tostring(w)] and "nether" or "overworld" end
function OnBlockBreak(p, w, tool, x, y, z)
    learn(w, x, z)
    local s = stats[p]; if s then local t = record(s.name); t.mined = t.mined + 1; dirty = true end
end
function OnBlockPlace(p, w, x, y, z)
    learn(w, x, z)
    local s = stats[p]; if s then local t = record(s.name); t.placed = t.placed + 1; dirty = true end
end
function OnBlockUse(p, w, x, y, z) if y >= 0 and y <= 127 then learn(w, x, z) end end

local function joined(p)
    local now, name = os.time(), player.getUsername(p)
    stats[p] = { name = name, joined = now, counted = now }
    record(name).last = now; dirty = true
end

local function write()
    local items = {}
    for i = 0, server.getPlayerCount() - 1 do
        local p = server.getPlayerAt(i)
        local e = p and online[p] and player.getEntity(p)
        if e then
            local pos, wn = entity.getPosition(e), worldName(entity.getWorld(e))
            if not stats[p] then joined(p) end
            local s = stats[p]
            local t = record(s.name)
            if s.x and s.w == wn then
                local d = math.sqrt((pos.x - s.x) ^ 2 + (pos.z - s.z) ^ 2)
                if d < TELEPORT and d > 0 then t.walked = t.walked + d; dirty = true end
            end
            s.x, s.z, s.w = pos.x, pos.z, wn
            items[#items + 1] = string.format('{"name":%s,"x":%.1f,"y":%.1f,"z":%.1f,"world":"%s","joined":%d,"mined":%d,"placed":%d,"walked":%d}',
                json(s.name), pos.x, pos.y, pos.z, wn, s.joined, t.mined, t.placed, math.floor(t.walked))
        end
    end
    local f = io.open(OUT .. ".tmp", "w")
    if not f then return end                           -- no livemap folder yet
    f:write('{"time":', os.time(), ',"players":[', table.concat(items, ","), ']}')
    f:close()
    os.rename(OUT .. ".tmp", OUT)
end

function OnPlayerJoin(p) online[p] = true; joined(p) end

function OnPlayerLeave(p)
    countTime()
    online[p] = nil; stats[p] = nil
    save()
end

function OnServerTick()
    tick = tick + 1
    if tick % SAVE_EVERY == 0 then save() end
    if tick % EVERY ~= 0 then return end
    write()
end

function OnUnload() save() end

save()                           -- playtime.json right away (the list loads before the first minute is up)
log.info("Live map loaded (" .. OUT .. " and " .. LIST .. ", record in " .. RECORD .. ")")
