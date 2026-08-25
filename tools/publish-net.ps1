<#
.SYNOPSIS
    Publishes a quantised net as a content-addressed GitHub release.

.DESCRIPTION
    A net is 50 MB, so it is gitignored and cannot travel with the source. But
    an NNUE build embeds one, which means CI runners, OpenBench workers and
    anyone building from a fresh clone all need a way to GET the exact net a
    given commit expects. This uploads it somewhere all three can reach.

    The release tag is derived from the net's own SHA-256 - `net-` plus the
    first twelve hex digits - so a tag can never come to mean a different file.
    That is the whole point: `make net-fetch` pins NET_TAG *and* NET_SHA256 and
    refuses a download that hashes differently, and a net that is silently the
    wrong one scores plausibly while losing Elo, which is the failure mode
    invariant 8 exists to prevent.

    Three assets go up, under fixed names the build system relies on:

        net.nnue           what EVAL=nnue embeds
        net.nnue.vectors   10,000 (FEN, expected_int) pairs from the exporter
        net.nnue.sha256    the hash, for a human checking by hand

    The vectors are not optional. Without them the release workflow cannot
    prove that the binary it is about to publish reproduces the quantised
    reference exactly, and "we did not check" is not a state this repository
    tolerates for an evaluation.

    Re-running is safe. A tag that already exists is verified rather than
    replaced, and only missing assets are uploaded.

    AUTHENTICATION: needs a token with `contents: write` on the repository, in
    $env:GH_TOKEN or $env:GITHUB_TOKEN. If the GitHub CLI is installed and
    logged in, `gh auth token` is used automatically. Create one at
    https://github.com/settings/tokens (classic: `repo`; fine-grained:
    Contents read/write on this repository).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\publish-net.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\publish-net.ps1 `
        -Net external\nets\cand.nnue -UpdateMakefile

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\publish-net.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [string]$Net,
    [string]$Repo,
    # Rewrites NET_TAG and NET_SHA256 in the Makefile to the net just
    # published, which is the change a release has to carry anyway.
    [switch]$UpdateMakefile,
    [switch]$DryRun
)

. "$PSScriptRoot\common.ps1"
$ErrorActionPreference = 'Stop'

# Windows PowerShell 5.1 still negotiates SSLv3/TLS1.0 by default, which
# api.github.com refuses outright.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not $Net) { $Net = Join-Path $RepoRoot 'external\nets\net.nnue' }

Write-Section 'Net'

if (-not (Test-Path $Net)) {
    Write-Fail "No net at $Net"
    Write-Host "  Export one first:  make nnue-export" -ForegroundColor Yellow
    exit 1
}
$netPath = (Resolve-Path $Net).Path
$vectors = "$netPath.vectors"

if (-not (Test-Path $vectors)) {
    Write-Fail "No equivalence vectors beside the net ($vectors)"
    Write-Host "  The release workflow verifies every published binary against these." -ForegroundColor Yellow
    Write-Host "  Re-export to produce them:  make nnue-export" -ForegroundColor Yellow
    exit 1
}

$sha = (Get-FileHash -Algorithm SHA256 -Path $netPath).Hash.ToLower()
$tag = 'net-' + $sha.Substring(0, 12)
$sizeMB = [math]::Round((Get-Item $netPath).Length / 1MB, 1)

Write-Ok "$netPath ($sizeMB MB)"
Write-Ok "sha256 $sha"
Write-Ok "tag    $tag"

# The sidecar the exporter writes. Regenerated rather than trusted: it is a
# claim about the file, and the file is right here to check it against.
$shaFile = "$netPath.sha256"
Write-TextNoBom $shaFile ("{0}  {1}`n" -f $sha, (Split-Path -Leaf $netPath))

# The manifest, if the exporter left one. Its architecture fields make the
# release page say what the net actually is instead of just how big it is.
$manifest = $null
$manifestPath = Join-Path (Split-Path -Parent $netPath) 'net.json'
if (Test-Path $manifestPath) {
    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.sha256 -and $manifest.sha256 -ne $sha) {
        # Not fatal: the manifest may simply be older than the net beside it.
        # But it is exactly the sort of drift worth saying out loud.
        Write-Warn2 "net.json records sha256 $($manifest.sha256), not this net - not attaching it"
        $manifest = $null
    }
}

# ------------------------------------------------------------------- repo --
if (-not $Repo) {
    $origin = (& git -C $RepoRoot remote get-url origin 2>$null)
    if (-not $origin) {
        Write-Fail "No git remote 'origin'. Pass -Repo owner/name."
        exit 1
    }
    # Both remote spellings: https://github.com/owner/name(.git) and
    # git@github.com:owner/name(.git)
    if ($origin -match 'github\.com[:/](?<owner>[^/]+)/(?<name>[^/]+?)(\.git)?\s*$') {
        $Repo = "$($Matches.owner)/$($Matches.name)"
    } else {
        Write-Fail "Could not read owner/name out of origin ($origin). Pass -Repo owner/name."
        exit 1
    }
}
Write-Ok "repo   $Repo"

# ------------------------------------------------------------------ token --
$token = $env:GH_TOKEN
if (-not $token) { $token = $env:GITHUB_TOKEN }
if (-not $token) {
    $ghCmd = Get-Command gh -ErrorAction SilentlyContinue
    if ($ghCmd) {
        $token = (& $ghCmd.Source auth token 2>$null)
        if ($LASTEXITCODE -ne 0) { $token = $null }
    }
}
if (-not $token -and -not $DryRun) {
    Write-Fail 'No GitHub token.'
    Write-Host '  Set one for this session:' -ForegroundColor Yellow
    Write-Host '      $env:GH_TOKEN = "ghp_..."' -ForegroundColor Yellow
    Write-Host '  It needs contents:write on the repository. See the help in this file.' -ForegroundColor Yellow
    exit 1
}
$token = ($token | Out-String).Trim()

$apiHeaders = @{
    Authorization          = "Bearer $token"
    Accept                 = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
    'User-Agent'           = 'stormbreaker-publish-net'
}

function Get-ReleaseByTag([string]$Tag) {
    try {
        return Invoke-RestMethod -Method Get -Headers $apiHeaders `
            -Uri "https://api.github.com/repos/$Repo/releases/tags/$Tag"
    } catch {
        if ($_.Exception.Response -and $_.Exception.Response.StatusCode.value__ -eq 404) { return $null }
        throw
    }
}

function New-NetRelease([string]$Tag) {
    $lines = @(
        "Quantised network for [StormBreaker](https://github.com/$Repo).",
        '',
        '```',
        $sha,
        '```',
        ''
    )
    if ($manifest) {
        $lines += @(
            '| Field | Value |',
            '|---|---|',
            "| Architecture | $($manifest.features) -> $($manifest.hidden)x2 -> $($manifest.output_buckets) |",
            "| Activation | $($manifest.activation) |",
            "| Feature set | $($manifest.feature_set) |",
            "| Quantisation | QA $($manifest.qa), QB $($manifest.qb), scale $($manifest.scale) |",
            "| Format version | $($manifest.format_version) |",
            "| Trainer tag | $($manifest.tag) |",
            ''
        )
    }
    $lines += @(
        'Embedded at build time, not loaded at run time. To build against it:',
        '',
        '```sh',
        "make net-fetch NET_TAG=$Tag NET_SHA256=$sha",
        'make nnue',
        '```',
        '',
        'The tag is the first twelve hex digits of the SHA-256 above, so it',
        'always names this exact file. `net.nnue.vectors` holds the 10,000',
        'reference positions `<engine> nnue verify` checks the C inference',
        'against, which is why the release workflow can prove a published',
        'binary matches the quantised reference exactly.'
    )

    $body = @{
        tag_name = $Tag
        name     = $Tag
        body     = ($lines -join "`n")
        draft    = $false
        # Keeps nets out of the "Latest release" slot, which belongs to the
        # engine. A net is a build input, not something a user downloads.
        prerelease = $true
    } | ConvertTo-Json -Depth 4

    return Invoke-RestMethod -Method Post -Headers $apiHeaders `
        -Uri "https://api.github.com/repos/$Repo/releases" `
        -ContentType 'application/json' -Body $body
}

function Send-Asset($Release, [string]$Path, [string]$Name) {
    $len = (Get-Item $Path).Length
    $existing = $Release.assets | Where-Object { $_.name -eq $Name }

    if ($existing) {
        if ($existing.size -eq $len) {
            Write-Ok "$Name already uploaded"
            return
        }
        Write-Warn2 "$Name is there at $($existing.size) bytes, expected $len - replacing it"
        Invoke-RestMethod -Method Delete -Headers $apiHeaders `
            -Uri "https://api.github.com/repos/$Repo/releases/assets/$($existing.id)" | Out-Null
    }

    Write-Host "  uploading $Name ($([math]::Round($len / 1MB, 1)) MB)..." -ForegroundColor Cyan
    $uploadHeaders = $apiHeaders.Clone()
    $uploadHeaders['Content-Type'] = 'application/octet-stream'
    Invoke-RestMethod -Method Post -Headers $uploadHeaders `
        -Uri "https://uploads.github.com/repos/$Repo/releases/$($Release.id)/assets?name=$Name" `
        -InFile $Path | Out-Null
    Write-Ok "$Name uploaded"
}

# ---------------------------------------------------------------- publish --
Write-Section "Publishing $tag"

if ($DryRun) {
    Write-Warn2 'DryRun: nothing will be uploaded'
    Write-Host "  would create release $tag on $Repo with:" -ForegroundColor Yellow
    Write-Host "      net.nnue          <- $netPath" -ForegroundColor Yellow
    Write-Host "      net.nnue.vectors  <- $vectors" -ForegroundColor Yellow
    Write-Host "      net.nnue.sha256   <- $shaFile" -ForegroundColor Yellow
} else {
    $release = Get-ReleaseByTag $tag
    if ($release) {
        Write-Ok "release $tag already exists - checking its assets"
    } else {
        $release = New-NetRelease $tag
        Write-Ok "created release $tag"
    }

    # Fixed asset names, whatever the local files are called: NET_URL in the
    # Makefile ends in /net.nnue, and the workflow fetches /net.nnue.vectors.
    Send-Asset $release $netPath 'net.nnue'
    Send-Asset $release $vectors 'net.nnue.vectors'
    Send-Asset $release $shaFile 'net.nnue.sha256'
}

# ------------------------------------------------------------------- pin --
Write-Section 'The pin'

$makefile = Join-Path $RepoRoot 'Makefile'
$pinTag = "NET_TAG    ?= $tag"
$pinSha = "NET_SHA256 ?= $sha"

if ($UpdateMakefile -and -not $DryRun) {
    $text = Get-Content $makefile -Raw
    # Scriptblock evaluators, not replacement strings. A replacement string
    # would have to be escaped for `$`, and escaping it the obvious way (with
    # [regex]::Escape, which is for patterns) writes the pin out as
    # NET_TAG    \?= ... - a Makefile that breaks at the next build, not here.
    $text = [regex]::Replace($text, '(?m)^NET_TAG\s*\?=.*$',    { $pinTag })
    $text = [regex]::Replace($text, '(?m)^NET_SHA256\s*\?=.*$', { $pinSha })
    Write-TextNoBom $makefile $text
    Write-Ok 'Makefile updated'
    Write-Host '  Commit it: the pin is what says which net a release embeds.' -ForegroundColor Cyan
    Write-Host '  Record WHY this net was adopted in docs/EXPERIMENTS.md, beside its SPRT.' -ForegroundColor Cyan
} else {
    Write-Host '  Put these in the Makefile (or re-run with -UpdateMakefile):' -ForegroundColor Cyan
    Write-Host ''
    Write-Host "      $pinTag" -ForegroundColor White
    Write-Host "      $pinSha" -ForegroundColor White
    Write-Host ''
    Write-Host '  Then record WHY this net was adopted in docs/EXPERIMENTS.md,' -ForegroundColor Cyan
    Write-Host '  beside the SPRT that adopted it.' -ForegroundColor Cyan
}

Write-Host ''
Write-Ok "https://github.com/$Repo/releases/tag/$tag"
