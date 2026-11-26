# Makar launcher: everything that can be opened, built or recorded, from anywhere (through makar.cmd).
# makar help lists it all.
param([Parameter(Position = 0)][string]$Command = "help", [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)

$ErrorActionPreference = "Stop"
$Project = Split-Path $PSScriptRoot -Parent
$Uproject = Join-Path $Project "Makar.uproject"
$Engine = if ($env:MAKAR_UE) { $env:MAKAR_UE } else { "C:\Program Files\Epic Games\UE_5.8\Engine" }
$Editor = Join-Path $Engine "Binaries\Win64\UnrealEditor.exe"
$Recordings = Join-Path $Project "Saved\Recordings"

$Maps = [ordered]@{
  ocean    = @{ Path = "/Game/Maps/Ocean";    Text = "la mer des Caraibes et l'ile (carte par defaut)" }
  shipyard = @{ Path = "/Game/Maps/Shipyard"; Text = "le chantier naval : generateur de navires, menu, vent, visite a bord" }
}
$Aliases = @{ mer = "ocean"; chantier = "shipyard"; navires = "shipyard"; ships = "shipyard" }

function Show-Help {
  @"

  makar <commande> [options]        (depuis n'importe quel dossier)

  OUVRIR
    makar ocean        [-res 1600x900] [-full]   $($Maps.ocean.Text)
    makar shipyard     [-res 1600x900] [-full]   $($Maps.shipyard.Text)   (alias : chantier)
    makar editor       [ocean|shipyard]           l'editeur Unreal sur une carte (ocean par defaut)
    makar videos                                  le dossier des enregistrements video
    makar folder                                  le dossier du projet

  CONSTRUIRE ET VERIFIER
    makar build                                   compile le code C++ (ferme d'abord le jeu et l'editeur)
    makar check [types] [graines] [vents]         banc d'essai des voiles (ex. : makar check FRIGATE 1,2 0,90,180)
    makar record [options]                        enregistre une video du chantier, hors ecran
        -name fregate  -ship "Frigate 1 184 40"  -view "-130 -8 75"  -seconds 6  -fps 30
        -at "3 Makar.Wind 20 25"  (commande a un instant donne, repetable)
        navire : "<Sloop|Brigantine|Frigate|Galleon> <graine> <vent d'ou, degres> <force, m/s>"
        vue    : "<lacet> <tangage> <distance, m> [x y z]"

  AUTRE
    makar stop                                    ferme le jeu et l'editeur (ex. : pour jouer)
    makar log                                     les dernieres lignes du journal du jeu
    makar help                                    cette aide

"@ | Write-Host
}

function Get-Option([string]$Name, $Default) {
  for ($i = 0; $i -lt $Rest.Count; $i++) {
    if ($Rest[$i] -ieq "-$Name") { if ($i + 1 -lt $Rest.Count) { return $Rest[$i + 1] } }
  }
  return $Default
}
function Get-Options([string]$Name) {
  $values = @()
  for ($i = 0; $i -lt $Rest.Count; $i++) { if ($Rest[$i] -ieq "-$Name" -and $i + 1 -lt $Rest.Count) { $values += $Rest[$i + 1] } }
  return $values
}
function Test-Flag([string]$Name) { return [bool]($Rest | Where-Object { $_ -ieq "-$Name" }) }

function Start-Game([string]$Map) {
  $res = (Get-Option "res" "1600x900") -split "x"
  $argList = @("`"$Uproject`"", $Maps[$Map].Path, "-game", "-nosplash")
  if (Test-Flag "full") { $argList += "-fullscreen" } else { $argList += @("-windowed", "-ResX=$($res[0])", "-ResY=$($res[1])") }
  Start-Process -FilePath $Editor -ArgumentList $argList | Out-Null
  Write-Host "Lancement : $Map ($($Maps[$Map].Text))"
}

function Invoke-Build {
  $build = Join-Path $Engine "Build\BatchFiles\Build.bat"
  $log = Join-Path $env:TEMP "makar_build.txt"
  Write-Host "Compilation..."
  & $build MakarEditor Win64 Development -Project="$Uproject" -WaitMutex *> $log
  if ($LASTEXITCODE -ne 0) {
    Write-Host "ECHEC de la compilation :" -ForegroundColor Red
    Select-String -Path $log -Pattern "error" | Select-Object -First 15 | ForEach-Object { Write-Host "  $($_.Line)" }
    return $false
  }
  Write-Host "Compilation reussie." -ForegroundColor Green
  return $true
}

function Test-Running {
  if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) {
    Write-Host "Le jeu ou l'editeur est ouvert : ferme-le d'abord (ou 'makar stop')." -ForegroundColor Yellow
    return $true
  }
  return $false
}

function Invoke-Record {
  $name = Get-Option "name" ("rec_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
  $ship = Get-Option "ship" "Frigate 1 184 25"
  $view = Get-Option "view" ""
  $seconds = Get-Option "seconds" "6"
  $fps = Get-Option "fps" "30"
  $res = (Get-Option "res" "1280x720") -split "x"
  $cmds = @("Makar.Ship $ship")
  if ($view) { $cmds += "Makar.At 0 Makar.View $view" }
  foreach ($a in (Get-Options "at")) { $cmds += "Makar.At $a" }
  $cmds += "Makar.Record $seconds $fps $name quit"
  $log = "MakarRecord.log"
  Write-Host "Enregistrement '$name' ($seconds s a $fps i/s, hors ecran)..."
  $p = Start-Process -FilePath $Editor -ArgumentList "`"$Uproject`"", $Maps.shipyard.Path, "-game", "-RenderOffscreen", "-unattended", "-nosound", "-nosplash", "-windowed", "-ResX=$($res[0])", "-ResY=$($res[1])", "-log=$log", "-ExecCmds=`"$($cmds -join ',')`"" -PassThru
  if (-not $p.WaitForExit(1800000)) { Stop-Process -Id $p.Id -Force; Write-Host "Trop long : arrete." -ForegroundColor Red; return }
  $dir = Join-Path $Recordings $name
  $frames = @(Get-ChildItem $dir -Filter *.bmp -ErrorAction SilentlyContinue).Count
  if ($frames -eq 0) { Write-Host "Aucune image enregistree (voir Saved\Logs\$log)." -ForegroundColor Red; return }
  $ffmpeg = & py -c "import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())" 2>$null
  if (-not $ffmpeg) { Write-Host "ffmpeg introuvable : py -m pip install --user imageio-ffmpeg" -ForegroundColor Red; return }
  $mp4 = Join-Path $Recordings "$name.mp4"
  & $ffmpeg -y -loglevel error -framerate $fps -i "$dir\frame_%05d.bmp" -c:v libx264 -pix_fmt yuv420p -crf 20 $mp4
  $step = [Math]::Max(1, [Math]::Floor($frames / 12))
  $sheet = Join-Path $Recordings "${name}_planche.png"
  & $ffmpeg -y -loglevel error -framerate $fps -i "$dir\frame_%05d.bmp" -vf "select='not(mod(n\,$step))',drawtext=fontfile='C\:/Windows/Fonts/arial.ttf':text='%{pts\:hms}':x=10:y=10:fontsize=36:fontcolor=yellow:box=1:boxcolor=black@0.5,scale=640:-1,tile=4x3" -frames:v 1 -update 1 $sheet
  Remove-Item $dir -Recurse -Force
  Write-Host "Video : $mp4" -ForegroundColor Green
  Write-Host "Planche : $sheet"
}

function Invoke-Check {
  $types = if ($Rest.Count -gt 0 -and $Rest[0]) { $Rest[0] } else { "SLOOP,BRIGANTINE,FRIGATE,GALLEON" }
  $seeds = if ($Rest.Count -gt 1) { $Rest[1] } else { "1,2" }
  $winds = if ($Rest.Count -gt 2) { $Rest[2] } else { "0,45,90,135,180,225,270,315" }
  $env:MAKAR_VERIFY_SAILS = "1"; $env:SAILCHECK_TYPES = $types; $env:SAILCHECK_SEEDS = $seeds; $env:SAILCHECK_WINDS = $winds
  $script = (Join-Path $PSScriptRoot "sail_check.py") -replace '\\', '/'
  Write-Host "Banc d'essai des voiles : $types, graines $seeds, vents $winds (hors ecran, plusieurs minutes)..."
  $p = Start-Process -FilePath $Editor -ArgumentList "`"$Uproject`"", $Maps.shipyard.Path, "-RenderOffscreen", "-unattended", "-nosound", "-nosplash", "-log=MakarSailCheck.log", "-ExecCmds=`"py $script`"" -PassThru
  $p.WaitForExit() | Out-Null
  Remove-Item Env:MAKAR_VERIFY_SAILS
  $checks = Select-String -Path (Join-Path $Project "Saved\Logs\MakarSailCheck.log") -Pattern "SAILCHECK EShipType" | ForEach-Object { $_.Line } | Select-Object -Skip 1
  $pass = @($checks | Where-Object { $_ -match ": PASS" }).Count
  foreach ($c in $checks) {
    if ($c -match "SAILCHECK EShipType::(\w+) seed (\d+) wind (\d+) trim (\d+)%: (\w+) fit fails (\d+), cut ([\d.]+)%, inside (\d+), crossings (\d+)") {
      $color = if ($matches[5] -eq "PASS") { "Gray" } else { "Red" }
      Write-Host ("{0,-10} graine {1} vent {2,3}  {3}  echecs {4}  reduit {5}%  dedans {6}  croisements {7}" -f $matches[1], $matches[2], $matches[3], $matches[5], $matches[6], $matches[7], $matches[8], $matches[9]) -ForegroundColor $color
    }
  }
  $color = if ($pass -eq $checks.Count) { "Green" } else { "Red" }
  Write-Host "$pass sur $($checks.Count) reussis" -ForegroundColor $color
}

$c = $Command.ToLower()
if ($Aliases.ContainsKey($c)) { $c = $Aliases[$c] }
switch ($c) {
  { $_ -in "help", "-h", "--help", "/?", "aide", "list", "liste" } { Show-Help }
  { $Maps.Contains($_) } { Start-Game $c }
  "editor" {
    $map = if ($Rest.Count -gt 0 -and $Maps.Contains($Rest[0].ToLower())) { $Rest[0].ToLower() } else { "ocean" }
    Start-Process -FilePath $Editor -ArgumentList "`"$Uproject`"", $Maps[$map].Path | Out-Null
    Write-Host "Ouverture de l'editeur sur $map"
  }
  "build" { if (-not (Test-Running)) { Invoke-Build | Out-Null } }
  "check" { if (-not (Test-Running)) { Invoke-Check } }
  "record" { if (-not (Test-Running)) { Invoke-Record } }
  "videos" { New-Item -ItemType Directory -Force $Recordings | Out-Null; Invoke-Item $Recordings }
  "folder" { Invoke-Item $Project }
  "stop" {
    $procs = Get-Process UnrealEditor, UnrealEditor-Cmd, LiveCodingConsole -ErrorAction SilentlyContinue
    if ($procs) { $procs | Stop-Process -Force; Write-Host "Jeu et editeur fermes." } else { Write-Host "Rien d'ouvert." }
  }
  "log" {
    $log = Get-ChildItem (Join-Path $Project "Saved\Logs\*.log") | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    Write-Host "== $($log.Name)"; Get-Content $log.FullName -Tail 40
  }
  default { Write-Host "Commande inconnue : $Command" -ForegroundColor Red; Show-Help; exit 1 }
}

