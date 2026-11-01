# What Windows itself knows about a media session: the same API the flyout and the lock screen read. Run by
# steps_media.mjs with Windows PowerShell, which can load WinRT types. Prints one line of JSON.
#   powershell -File media_session.ps1 AppId [read|play|pause|next|previous|seek SECONDS]
param(
    [Parameter(Mandatory)][string]$App,
    [string]$Action = 'read',
    [double]$Seconds = 0
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1
function Await($operation, [Type]$type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($operation))
    [void]$task.Wait(5000)
    $task.Result
}

[void][Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
[void][Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties, Windows.Media.Control, ContentType = WindowsRuntime]
$manager = Await ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])
$session = $manager.GetSessions() | Where-Object { $_.SourceAppUserModelId -like "*$App*" } | Select-Object -First 1
if (-not $session) {
    [pscustomobject]@{ found = $false; apps = @($manager.GetSessions() | ForEach-Object { $_.SourceAppUserModelId }) } | ConvertTo-Json -Compress
    exit 0
}

switch ($Action) {
    'play' { $accepted = Await $session.TryPlayAsync() ([bool]) }
    'pause' { $accepted = Await $session.TryPauseAsync() ([bool]) }
    'next' { $accepted = Await $session.TrySkipNextAsync() ([bool]) }
    'previous' { $accepted = Await $session.TrySkipPreviousAsync() ([bool]) }
    'seek' { $accepted = Await $session.TryChangePlaybackPositionAsync([long]($Seconds * 10000000)) ([bool]) }
}

$media = Await $session.TryGetMediaPropertiesAsync() ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties])
$playback = $session.GetPlaybackInfo()
$timeline = $session.GetTimelineProperties()
[pscustomobject]@{
    found     = $true
    app       = $session.SourceAppUserModelId
    current   = $manager.GetCurrentSession().SourceAppUserModelId
    title     = $media.Title
    artist    = $media.Artist
    album     = $media.AlbumTitle
    thumbnail = $null -ne $media.Thumbnail
    status    = "$($playback.PlaybackStatus)"
    next      = $playback.Controls.IsNextEnabled
    previous  = $playback.Controls.IsPreviousEnabled
    seekable  = $playback.Controls.IsPlaybackPositionEnabled
    position  = $timeline.Position.TotalSeconds
    end       = $timeline.EndTime.TotalSeconds
    accepted  = $accepted
} | ConvertTo-Json -Compress
