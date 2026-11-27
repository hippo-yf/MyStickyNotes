$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$repo=Join-Path $root '.sync-repo'
$repoArgs=@()
function gitok {
    $ErrorActionPreference='Continue'
    $output=& git @repoArgs @args 2>&1
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
$config=if(Test-Path "$root/notes.json"){Get-Content "$root/notes.json" -Raw -Encoding UTF8 | ConvertFrom-Json}
function setting($name,$default){
    if($null -eq $config -or $null -eq $config.PSObject.Properties[$name]){return $default}
    $value=$config.$name
    if($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)){throw "Invalid setting: $name"}
    return $value
}
function sshPath($path){
    if(![IO.Path]::IsPathRooted($path)){$path=Join-Path $root $path}
    return (Get-Item -LiteralPath $path -ErrorAction Stop).FullName.Replace('\','/')
}
$env:NOTE_SYNC_KEY=sshPath (setting 'sshKeyPath' 'ssh/id_ed25519')
$hostsPath=$config.knownHostsPath
if($null -eq $hostsPath -or ($hostsPath -is [string] -and $hostsPath.Length -eq 0)){
    $hostsPath=Join-Path $HOME '.ssh/known_hosts'
}elseif($hostsPath -isnot [string] -or [string]::IsNullOrWhiteSpace($hostsPath)){
    throw 'Invalid setting: knownHostsPath'
}
$env:NOTE_SYNC_HOSTS=sshPath $hostsPath
$env:GIT_TERMINAL_PROMPT='0'
$env:GIT_SSH_COMMAND='ssh -o ConnectTimeout=12 -o ServerAliveInterval=10 -o ServerAliveCountMax=2 -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=\"$NOTE_SYNC_HOSTS\"" -i "$NOTE_SYNC_KEY"'
$slug=setting 'giteeRepository' 'hippo-yf/my-sticky-notes-cache'
if($slug -notmatch '^[a-zA-Z0-9_-][a-zA-Z0-9._-]*/[a-zA-Z0-9_-][a-zA-Z0-9._-]*$'){throw 'Invalid giteeRepository: expected owner/repository'}
$url="git@gitee.com:$slug.git"
$state="$root/.last-sync"
if($slug -cne 'hippo-yf/my-sticky-notes-cache'){
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$id=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($slug))).Replace('-','').Substring(0,20)}finally{$sha.Dispose()}
    $repo="$root/.sync-repo-$id"
    $state="$root/.last-sync-$id"
}
if(!(Test-Path "$repo/.git")){
    gitok clone --quiet $url $repo
}
Set-Location -LiteralPath $repo
$gitPath=(Get-Item -LiteralPath $repo).FullName.Replace('\','/')
$repoArgs=@('-c',"safe.directory=$gitPath",'-c','core.autocrlf=false','-C',$repo,"--git-dir=$repo/.git","--work-tree=$repo")
if((gitok rev-parse --is-inside-work-tree) -ne 'true'){throw "Invalid sync working tree: $repo"}
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
        & git @repoArgs merge-base --is-ancestor $head $remoteHead
        if($LASTEXITCODE -ne 1){throw $pullError}
        & git @repoArgs merge-base --is-ancestor $remoteHead $head
        if($LASTEXITCODE -ne 1){throw $pullError}
        if(gitok status --porcelain){throw $pullError}
        gitok branch "saved-sync-$([guid]::NewGuid().ToString('N'))" $head
        gitok checkout --quiet --detach $remoteHead
    }
}else{
    $branch=(gitok symbolic-ref --short HEAD).Trim()
} 
$local="$root/notes.json"
if((gitok config --get remote.origin.url) -cne $url){throw 'Sync cache origin does not match giteeRepository'}
$old=if(Test-Path $state){([string](Get-Content $state -Raw)).Trim()}else{''}
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
Set-Content "$state.tmp" (hashfile $local)
if(Test-Path $state){[IO.File]::Replace("$state.tmp",$state,"$state.bak")}
else{[IO.File]::Move("$state.tmp",$state)}
if($downloaded){exit 10}
exit 0 
}catch{Add-Content "$root/sync.log" ("[{0:yyyy-MM-dd HH:mm:ss}] ERROR {1}" -f (Get-Date),$_.Exception.Message);exit 1}








