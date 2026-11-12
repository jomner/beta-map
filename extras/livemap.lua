-- beta-map live player dots, for Betrock++ (https://github.com/OpenBetaGroup/Betrock, a C++ Beta 1.7.3 server) with
-- its bpa_lua addon: put this file in the server's lua_scripts folder and make a "livemap" folder in the server folder.
-- Every 2 s it writes livemap/players.json. Serve that one folder over HTTPS (with an
-- "Access-Control-Allow-Origin: *" header, since the map page is on another site) and pass its URL to
-- beta_map.py --live. Everyone online is shown.

local DIR = "livemap"
local OUT = DIR .. "/players.json"
local EVERY = 40                 -- ticks: 2 s

local online = {}                -- player handle -> true once joined
local tick = 0

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
function OnBlockBreak(p, w, tool, x, y, z) learn(w, x, z) end
function OnBlockPlace(p, w, x, y, z) learn(w, x, z) end
function OnBlockUse(p, w, x, y, z) if y >= 0 and y <= 127 then learn(w, x, z) end end

local function json(s) return '"' .. s:gsub('[%c"\\]', function(c) return string.format("\\u%04x", c:byte()) end) .. '"' end

local function write()
    local items = {}
    for i = 0, server.getPlayerCount() - 1 do
        local p = server.getPlayerAt(i)
        local e = p and online[p] and player.getEntity(p)
        if e then
            local pos = entity.getPosition(e)
            items[#items + 1] = string.format('{"name":%s,"x":%.1f,"y":%.1f,"z":%.1f,"world":"%s"}',
                json(player.getUsername(p)), pos.x, pos.y, pos.z, worldName(entity.getWorld(e)))
        end
    end
    local f = io.open(OUT .. ".tmp", "w")
    if not f then return end                           -- no livemap folder yet
    f:write('{"time":', os.time(), ',"players":[', table.concat(items, ","), ']}')
    f:close()
    os.rename(OUT .. ".tmp", OUT)
end

function OnPlayerJoin(p) online[p] = true end

function OnPlayerLeave(p) online[p] = nil end

function OnServerTick()
    tick = tick + 1
    if tick % EVERY ~= 0 then return end
    write()
end

log.info("Live map loaded (" .. OUT .. ")")
