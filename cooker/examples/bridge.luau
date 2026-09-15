-- A CPU-generated reusable bridge asset, not a game-world behavior.
-- No IO, engine objects, Python, scene strings, or external Lua interpreter.
local p = cooker.parameters
local span = p.span or 8
local width = p.width or 2.6
local rise = p.rise or 1.25
local segments = p.segments or 24
assert(span >= 2 and span <= 30, "span must be 2..30")
assert(width >= 1 and width <= 8, "width must be 1..8")
assert(rise >= 0 and rise <= 4, "rise must be 0..4")
assert(segments >= 4 and segments <= 64 and segments % 1 == 0, "segments must be an integer 4..64")

local stone = cooker.material({color = {0.60, 0.53, 0.42}, roughness = 0.82})
local trim = cooker.material({color = {0.31, 0.20, 0.08}, metallic = 0.7, roughness = 0.38})
local iron = cooker.material({color = {0.06, 0.095, 0.10}, metallic = 0.8, roughness = 0.45})
local box = cooker.primitive("box", {size = {1, 1, 1}})
local cap = cooker.primitive("sphere", {radius = 0.10, segments = 12, rings = 6})

local function height(x)
    return 0.35 + rise * (1 - (2 * x / span) ^ 2)
end
local vertices, indices, uvs = {}, {}, {}
local function quad(a, b, c, d)
    local base = #vertices
    for _, v in {a, b, c, d} do table.insert(vertices, v) end
    for _, i in {1, 2, 3, 1, 3, 4} do table.insert(indices, base + i) end
    for _, uv in {{0, 0}, {1, 0}, {1, 1}, {0, 1}} do table.insert(uvs, uv) end
end

local z0, z1 = -width / 2, width / 2
local thickness = 0.32
for i = 0, segments - 1 do
    local x0, x1 = -span / 2 + span * i / segments, -span / 2 + span * (i + 1) / segments
    local y0, y1 = height(x0), height(x1)
    quad({x0, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y0, z1})
    quad({x0, y0 - thickness, z1}, {x1, y1 - thickness, z1}, {x1, y1 - thickness, z0}, {x0, y0 - thickness, z0})
    quad({x0, y0, z1}, {x1, y1, z1}, {x1, y1 - thickness, z1}, {x0, y0 - thickness, z1})
    quad({x0, y0 - thickness, z0}, {x1, y1 - thickness, z0}, {x1, y1, z0}, {x0, y0, z0})
end
for _, side in {-1, 1} do
    local x = side * span / 2
    local za, zb = z0 * side, z1 * side
    quad({x, height(x), za}, {x, height(x) - thickness, za}, {x, height(x) - thickness, zb}, {x, height(x), zb})
end
local deck = cooker.mesh({vertices = vertices, indices = indices, uvs = uvs})
local instances = {{asset = deck, material = stone}}
for _, side in {-1, 1} do
    local z = side * (width / 2 - 0.13)
    for i = 0, segments do
        local x = -span / 2 + span * i / segments
        local y = height(x)
        local tall = i % 4 == 0
        local post = tall and 0.09 or 0.045
        table.insert(instances, {asset = box, material = iron, position = {x, y + 0.45, z}, scale = {post, 0.9, post}})
        if tall then table.insert(instances, {asset = cap, material = trim, position = {x, y + 0.97, z}}) end
        if i < segments then
            local nx = x + span / segments
            local dy = height(nx) - y
            local angle = math.atan2(dy, nx - x)
            local length = math.sqrt((nx - x) ^ 2 + dy ^ 2)
            for _, offset in {0.18, 0.89} do
                table.insert(instances, {asset = box, material = trim,
                    position = {(x + nx) / 2, (y + height(nx)) / 2 + offset, z},
                    rotation = {0, 0, angle}, scale = {length + 0.01, 0.055, 0.07}})
            end
        end
    end
end
-- Same mesh/material pairs are batched as MultiMesh by the native host.
return cooker.scene(instances)
