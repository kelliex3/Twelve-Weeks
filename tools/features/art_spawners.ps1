# Adds art-only spawners to a level pak so their art groups (actor packs) load with the level:
# each "name=artgroup" argument clones spawner -Src as <name> with ArtGroup <artgroup>,
# AutoSpawn on (spawned 500 m below the origin; art of non-spawning spawners is not loaded). spawn-object(<artgroup>, ...) can then create copies
# at runtime (the cut multiplayer's armor meshes). Used by build_dll.py on the staged mod pak.
param([string]$Pak, [string]$Src, [string[]]$Add)
$srcName = $Src
$u = if ($env:T2R_SCENE_EDITOR) { $env:T2R_SCENE_EDITOR } else { Join-Path $PSScriptRoot "../local/SceneEditor/unpacked" }
Set-Location $u
[Reflection.Assembly]::LoadFrom("$u\Newtonsoft.Json.dll") | Out-Null
$a = [Reflection.Assembly]::LoadFrom("$u\TLOUSceneEditor.dll")
$pr = $a.GetType("TLOUSceneEditor.Core.PackageReader")
$pw = $a.GetType("TLOUSceneEditor.Core.PackageWriter")
$c = $pr::ParsePackage([IO.File]::ReadAllBytes($Pak), $false)
$list = $c.Spawners
$byName = @{}; foreach ($s in $list) { $byName[$s.SpawnerName] = $s }
$from = $byName[$srcName]
if (-not $from) { throw "no spawner $srcName" }
foreach ($e in ($Add -join ",") -split ",") {
    $name, $art = $e -split "=", 2
    if ($byName.ContainsKey($name)) { continue }
    $n = $from.Clone()
    $n.SpawnerName = $name
    $n.ArtGroup = $art
    $n.AutoSpawn = $false
    $n.Position = [Numerics.Vector3]::new(0, -500, 0)
    $list.Add($n)
    "art spawner $name ($art)"
}
$pw::BuildPackage($Pak, $list)
