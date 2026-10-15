# publicar.ps1 - incrementa version.txt, compila, sobe o .bin como Release no GitHub
# Uso (no terminal do ESP-IDF, na pasta do projeto):
#   .\publicar.ps1                  bump + build + commit/push + release
#   .\publicar.ps1 -Notas "texto"   idem, com notas na release
#   .\publicar.ps1 -SemBump         nao incrementa (rebuild da mesma versao; atualiza o .bin da release)
#   .\publicar.ps1 -SemUpload       so bump + build (nada vai pro GitHub)
param([switch]$SemBump, [switch]$SemUpload, [string]$Notas = "")
# Repo PUBLICO so de releases (o codigo fica no repo privado). Vazio = usa o origin deste projeto.
$RepoRelease = "sergiolbj/kartbox-v2-releases"
$ErrorActionPreference = "Continue"   # erros tratados via $LASTEXITCODE + throw (PS 5.1 trata stderr nativo como erro com Stop)
Set-Location $PSScriptRoot

$vf  = Join-Path $PSScriptRoot "version.txt"
$old = (Get-Content $vf -Raw).Trim()
if (-not ($old -match '^(\d+)\.(\d+)\.(\d+)(.*)$')) { throw "version.txt fora do padrao X.Y.Z[-sufixo]: '$old'" }
$new = $old
if (-not $SemBump) { $new = "{0}.{1}.{2}{3}" -f $Matches[1], $Matches[2], ([int]$Matches[3] + 1), $Matches[4] }

function Restaurar-Versao { [IO.File]::WriteAllText($vf, "$old`n") }

if (-not $env:IDF_PATH) { throw "IDF_PATH vazio: rode este script no terminal do ESP-IDF." }
if (-not $SemUpload) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw "GitHub CLI (gh) nao encontrado. Instale: winget install GitHub.cli ; depois: gh auth login" }
    & gh auth status 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "gh sem login. Rode: gh auth login" }
}

Write-Host "Versao: $old -> $new"
[IO.File]::WriteAllText($vf, "$new`n")

& python "$env:IDF_PATH\tools\idf.py" build
if ($LASTEXITCODE -ne 0) { Restaurar-Versao; throw "build falhou - version.txt voltou para $old" }

$bin = Join-Path $PSScriptRoot "build\kartbox.bin"
if (-not (Test-Path $bin)) { Restaurar-Versao; throw "build\kartbox.bin nao existe" }
$pd = Get-Content (Join-Path $PSScriptRoot "build\project_description.json") -Raw | ConvertFrom-Json
if ($pd.project_version -ne $new) { Write-Warning "versao gravada no bin ($($pd.project_version)) difere de $new" }
$mb = [math]::Round((Get-Item $bin).Length / 1MB, 2)
Write-Host "OK: kartbox.bin $mb MB, versao $($pd.project_version)"

if ($SemUpload) { Write-Host "SemUpload: nada enviado ao GitHub."; exit 0 }

# commit so do version.txt (nao mexe no resto do que voce alterou)
& git add version.txt
& git diff --cached --quiet -- version.txt
if ($LASTEXITCODE -ne 0) {
    & git commit version.txt -m "release v$new"
    if ($LASTEXITCODE -ne 0) { throw "git commit falhou" }
}
& git push
if ($LASTEXITCODE -ne 0) { throw "git push falhou (bin NAO enviado; version.txt ja esta commitado)" }

$asset = Join-Path $env:TEMP "kartbox-$new.bin"
Copy-Item $bin $asset -Force
$tag = "v$new"
$sha = (& git rev-parse HEAD).Trim()
$ra = @()
$alvo = @("--target", $sha)
if ($RepoRelease) { $ra = @("--repo", $RepoRelease); $alvo = @() }
if ([string]::IsNullOrWhiteSpace($Notas)) { $Notas = "Firmware KartBox $new" }

& gh release view $tag @ra 2>&1 | Out-Null
if ($LASTEXITCODE -eq 0) {
    & gh release upload $tag $asset --clobber @ra
} else {
    $pre = @()
    if ($new -match '-') { $pre = @("--prerelease") }
    & gh release create $tag $asset --title "KartBox $new" --notes $Notas @alvo @ra @pre
}
if ($LASTEXITCODE -ne 0) { throw "gh release falhou" }
Remove-Item $asset -Force -ErrorAction SilentlyContinue
Write-Host "Publicado: $tag"
& gh release view $tag @ra --json url --jq .url
