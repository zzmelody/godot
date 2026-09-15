-- Canonical production entry for the bridge example.
local asset = "res://assets/generated/bridge/r1/bridge.scn"
return {
    {
        name = "generate_bridge",
        operation = "run-recipe",
        source = "res://native/third_party/godot/cooker/examples/bridge.luau",
        output = asset,
        seed = 42,
        parameters = {span = 8, width = 2.6, rise = 1.25, segments = 24},
        limits = {time_ms = 1500, max_instances = 2000},
        if_missing = true,
    },
    {name = "validate_bridge", operation = "validate-resource", source = asset, type = "PackedScene"},
    {
        name = "manifest_bridge",
        operation = "asset-manifest",
        output = "res://assets/generated/bridge/r1/asset.manifest.json",
        asset_id = "bridge",
        revision = 1,
        kind = "model",
        entry = "bridge.scn",
        files = {"bridge.scn"},
        source = "cooker-example",
        generator = "Cooker Luau Recipe API v2",
        if_missing = true,
    },
}
