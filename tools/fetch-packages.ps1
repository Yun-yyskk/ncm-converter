$ErrorActionPreference = 'Stop'
$pkgs = Join-Path $env:USERPROFILE '.nuget\packages'
$seen = @{}
function Download-Nupkg($id, $ver) {
  $tmp = Join-Path $env:TEMP ($id + '.' + $ver + '.nupkg')
  $url = "https://api.nuget.org/v3-flatcontainer/$($id.ToLower())/$($ver.ToLower())/$($id.ToLower()).$($ver.ToLower()).nupkg"
  for ($attempt = 1; $attempt -le 4; $attempt++) {
    try {
      Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing -TimeoutSec 1800
      return $true
    } catch {
      Write-Host "  attempt $attempt failed: $($_.Exception.Message)"
      if ($attempt -lt 4) { Start-Sleep -Seconds (3 * $attempt) }
    }
  }
  return $false
}
function Fetch($id, $ver) {
  $key = ($id.ToLower() + '/' + $ver.ToLower())
  if ($seen.ContainsKey($key)) { return }
  $seen[$key] = $true
  $dir = Join-Path $pkgs ($id.ToLower() + '\' + $ver.ToLower())
  $marker = Join-Path $dir '.nupkg.metadata'
  if (-not (Test-Path $marker)) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Write-Host "download $id $ver"
    $ok = Download-Nupkg $id $ver
    if (-not $ok) { Write-Host "  ABORT $id $ver"; return }
    $tmp = Join-Path $env:TEMP ($id + '.' + $ver + '.nupkg')
    $zip = $tmp + '.zip'
    Copy-Item $tmp $zip -Force
    Expand-Archive -Path $zip -DestinationPath $dir -Force
    Remove-Item $zip, $tmp -Force -ErrorAction SilentlyContinue
    Set-Content -Path $marker -Value '{"version":2}' -NoNewline
    $sz = (Get-ChildItem $dir -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB
    Write-Host ("  ok {0:N1} MB" -f $sz)
  } else {
    Write-Host "cached  $id $ver"
  }
  $nuspec = Join-Path $dir ($id + '.nuspec')
  if (Test-Path $nuspec) {
    try { $xml = [xml](Get-Content $nuspec -Raw) } catch { return }
    $deps = $xml.SelectNodes("//*[local-name()='dependency']")
    foreach ($d in $deps) {
      $did = $d.GetAttribute('id')
      $dver = $d.GetAttribute('version')
      if ($dver -match '^[\[\(]?\s*([0-9][0-9A-Za-z\.\-]*)') { $dver = $matches[1] }
      if ($did -and $dver) { Fetch $did $dver }
    }
  }
}
Fetch 'Microsoft.WindowsAppSDK' '1.8.260921001'
Write-Host '=== resolved packages ==='
$seen.Keys | Sort-Object
