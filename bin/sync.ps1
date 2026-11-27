$ErrorActionPreference='Stop'
$root=$PSScriptRoot
function gitok {
    $ErrorActionPreference='Continue'
    $output=& git @args 2>&1
    $code=$LASTEXITCODE
    $lines=@($output | ForEach-Object { "$_" -replace '\x1b\[[0-9;]*m','' })
    foreach($line in $lines){
        if($line -match '^\s*(remote:\s*)?(warning|警告)\s*[:：]'){
            Add-Content "$root/sync.log" ("[{0:yyyy-MM-dd HH:mm:ss}] WARN {1}" -f (Get-Date),$line.Trim())
        }
    }
    if($code){
        $detail=($output | Where-Object { $_ -is [System.Management.Automation.ErrorRecord] } | ForEach-Object { "$_" -replace '\x1b\[[0-9;]*m','' }) -join ' | '
        throw "git $($args[0]) failed ($code) $detail"
    }
    $lines
} 
function hashfile($p){if(Test-Path $p){(Get-FileHash $p).Hash}else{''}}
try {
$log=Get-Item "$root/sync.log" -ErrorAction SilentlyContinue
if($log -and $log.Length -gt 1MB){[IO.File]::Copy($log.FullName,"$root/sync.previous.log",$true);[IO.File]::WriteAllText($log.FullName,"")}
$key=(Resolve-Path "$root/ssh/id_ed25519").Path.Replace('\','/')
$hosts=(Resolve-Path "$root/ssh/gitee_known_hosts").Path.Replace('\','/')
$env:GIT_TERMINAL_PROMPT="0"
$env:GIT_SSH_COMMAND="ssh -o ConnectTimeout=12 -o ServerAliveInterval=10 -o ServerAliveCountMax=2 -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=`"$hosts`" -i `"$key`""
$repo=Join-Path $root '.sync-repo'
$url='git@gitee.com:hippo-yf/my-sticky-notes-cache.git'
if(!(Test-Path "$repo/.git")){
    gitok clone --quiet $url $repo
}
Set-Location $repo
gitok config --local core.autocrlf false
$refs=gitok ls-remote --symref origin HEAD
$branch=$null
foreach($ref in $refs){
    if("$ref" -match '^ref: refs/heads/(.+)\s+HEAD$'){ $branch=$Matches[1];break }
}
if($branch){
    # Recover a rejected push without discarding its local commit or the editable note.
    try { gitok pull --ff-only origin $branch }
    catch {
        $pullError=$_

        $head=gitok rev-parse HEAD
        gitok fetch origin $branch
        $remoteHead=gitok rev-parse FETCH_HEAD
        & git merge-base --is-ancestor $head $remoteHead
        if($LASTEXITCODE -ne 1){throw $pullError}
        & git merge-base --is-ancestor $remoteHead $head
        if($LASTEXITCODE -ne 1){throw $pullError}
        if(gitok status --porcelain){throw $pullError}
        gitok branch "saved-sync-$([guid]::NewGuid().ToString('N'))" $head
        gitok checkout --quiet --detach $remoteHead
    }
}else{
    $branch=(gitok symbolic-ref --short HEAD).Trim()
} 
$local="$root/notes.json"
$old=if(Test-Path "$root/.last-sync"){([string](Get-Content "$root/.last-sync" -Raw)).Trim()}else{''}
$downloaded=$false
$a=hashfile $local
$b=hashfile 'notes.json'
if($old -and $a -and !$b){throw 'Remote notes.json is missing. Local note preserved.'}
if($a -and $b -and $a -ne $b -and $a -ne $old -and $b -ne $old){Copy-Item notes.json "$root/notes.remote.json" -Force;throw 'Concurrent edits. Remote copy saved as notes.remote.json.'}
if($a -and $a -ne $old -and $a -ne $b){Copy-Item $local notes.json;gitok add -- notes.json;gitok -c user.name=StickyNotes -c user.email=notes@localhost commit --quiet -m 'Update notes'}
elseif($b -and $a -ne $b){
    $start=New-Object Diagnostics.ProcessStartInfo
    $start.FileName=Join-Path $root 'StickyNotes.exe'
    $start.Arguments='--validate-note'
    $start.WorkingDirectory=$repo
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $validator=[Diagnostics.Process]::Start($start)
    try{
        if(!$validator.WaitForExit(60000)){$validator.Kill();throw 'Remote validation timed out'}
        if($validator.ExitCode -ne 0){throw 'Remote notes.json failed application validation'}
    }finally{$validator.Dispose()} 
    Copy-Item notes.json "$local.incoming" -Force
    if(Test-Path $local){[IO.File]::Replace("$local.incoming",$local,"$local.bak")}
    else{[IO.File]::Move("$local.incoming",$local)}
    $downloaded=$true
}
if($a -and $a -ne $old -and $a -ne $b){
    gitok push --quiet origin "HEAD:refs/heads/$branch"
}elseif($a -and $a -eq $b){
    # A prior push may have failed after the local commit was created.
    $head=gitok rev-parse HEAD
    if(!($refs -match "^$head\s+HEAD$")){gitok push --quiet origin "HEAD:refs/heads/$branch"}
}
$state="$root/.last-sync"
Set-Content "$state.tmp" (hashfile $local)
if(Test-Path $state){[IO.File]::Replace("$state.tmp",$state,"$state.bak")}
else{[IO.File]::Move("$state.tmp",$state)}
if($downloaded){exit 10}
exit 0 
}catch{Add-Content "$root/sync.log" ("[{0:yyyy-MM-dd HH:mm:ss}] ERROR {1}" -f (Get-Date),$_.Exception.Message);exit 1}






