param(
    [string]$Repository=(Split-Path $PSScriptRoot -Parent),
    [ValidateSet('All','Message','Packet','File')][string]$Component='All',
    [string]$VisualStudio='',
    [ValidatePattern('^[0-9]+\.[0-9]+(?:\.[0-9]+)?$')][string]$VCToolsVersion='14.44',
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$')][string]$WindowsSdkVersion='10.0.26100.0'
)
$ErrorActionPreference='Stop'
$Repository=(Resolve-Path -LiteralPath $Repository).Path
if([string]::IsNullOrWhiteSpace($VisualStudio)){
    if(-not [string]::IsNullOrWhiteSpace($env:VSINSTALLDIR)){
        $VisualStudio=$env:VSINSTALLDIR
    }else{
        $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if(!(Test-Path -LiteralPath $vswhere -PathType Leaf)){
            throw 'Cannot locate Visual Studio. Pass -VisualStudio with the installation folder.'
        }
        $VisualStudio=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
        if($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($VisualStudio)){
            throw 'No Visual Studio C++ installation found. Pass -VisualStudio explicitly.'
        }
    }
}
$VisualStudio=(Resolve-Path -LiteralPath $VisualStudio).Path
$devShell=Join-Path $VisualStudio 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll'
if(!(Test-Path -LiteralPath $devShell -PathType Leaf)){
    throw 'The selected Visual Studio installation has no developer shell module.'
}
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $VisualStudio -SkipAutomaticLocation -DevCmdArguments "-arch=x86 -host_arch=x64 -vcvars_ver=$VCToolsVersion -winsdk=$WindowsSdkVersion"
$env:VSLANG='1033'
$helperProfiles=@(
    @{name='Message';source='research/architecture/section8b-helper-empty-batch';target='build/section8b-helper-empty-batch/b5/sexyz.exe';hash='F6C7E6079AA68F3C10A902A8CC90257C8F0BA158FCEEC76EDA0C9E2CBB7DFB91';guard='EnsureMessageHelper.ps1'},
    @{name='Packet';source='research/architecture/rc10-packet-helper';target='build/rc10-helper-packet/b5/SFXQWKTransfer.exe';hash='D813790C72BA1EDDA5F2E920719798E05A0DF4D722C2E557A32A1678C800F998';guard='EnsurePacketHelper.ps1'},
    @{name='File';source='research/architecture/section10-file-helper';target='build/section10-helper-file/b5/SFXFileTransfer.exe';hash='8B87D172E85FE23AA792A984E69DC305292A74D125F7414B7607218B487479A4';guard='EnsureFileHelper.ps1'}
)
$names=@('sexyz','xmodem','zmodem','ringbuf','nopen','date_str','telnet','crc16','crc32','datewrap','dirwrap','filewrap','genwrap','ini_file','netwrap','os_info','sockwrap','str_list','threadwrap','xpdatetime','xpevent','xpprintf')
foreach($helperProfile in $helperProfiles){
    if($Component -ne 'All' -and $Component -ne $helperProfile.name){continue}
    $material=Join-Path $Repository $helperProfile.source
    $target=Join-Path $Repository $helperProfile.target
    $output=Join-Path $Repository ('build/qualified-helpers/'+$helperProfile.name)
    if(!(Test-Path -LiteralPath $target)){
        if(Test-Path -LiteralPath $output){throw "Preserve previous output and choose a fresh source checkout: $output"}
        New-Item -ItemType Directory -Path $output -Force|Out-Null
        $compileSource=Join-Path $material 'source'
        if($helperProfile.name -eq 'Message'){
            # These upstream files expose __FILE__ in diagnostic text.
            # Keep the reported paths relative to the upstream source tree.
            $compileSource=Join-Path $output 'source-for-build'
            Copy-Item -LiteralPath (Join-Path $material 'source') -Destination $compileSource -Recurse
            foreach($legacyName in @('sexyz','xmodem','zmodem')){
                $generated=Join-Path $compileSource "src/sbbs3/$legacyName.c"
                $original=[IO.File]::ReadAllBytes($generated)
                $virtualFile="src/sbbs3/$legacyName.c"
                $directive=[Text.Encoding]::ASCII.GetBytes("#line 1 `"$virtualFile`"`r`n")
                [IO.File]::WriteAllBytes($generated,($directive+$original))
            }
        }
        $directories=@('src/sbbs3','src/xpdev','src/hash','src/smblib')|ForEach-Object{Join-Path $compileSource $_}
        $flags=@('/nologo','/O2','/MT','/W3','/Brepro','/showIncludes','/DWIN32','/D_CONSOLE','/DSBBS_EXPORTS','/DRINGBUF_EVENT','/DRINGBUF_MUTEX','/DHAS_INTTYPES_H','/DHAS_STDINT_H','/DWINVER=0x601','/D_WIN32_WINNT=0x601','/D_CRT_SECURE_NO_DEPRECATE','/D_CRT_NONSTDC_NO_DEPRECATE','/DNDEBUG',"/I$material")
        $flags+=@($directories|ForEach-Object{"/I$_"})
        $objects=@()
        Push-Location $output
        try{
            foreach($name in $names){
                $helperInput=$directories|ForEach-Object{Join-Path $_ "$name.c"}|Where-Object{Test-Path -LiteralPath $_}|Select-Object -First 1
                if(!$helperInput){throw "Missing source: $name"}
                $object=Join-Path $output "$name.obj";$objects+=$object
                & cl.exe @flags /c $helperInput "/Fo$object" > "$name.compile.log" 2>&1
                if($LASTEXITCODE){throw "Helper compilation failed; see $output/$name.compile.log"}
            }
            $binary=Split-Path $target -Leaf
            & link.exe /nologo /Brepro /INCREMENTAL:NO /SUBSYSTEM:CONSOLE,6.01 /MACHINE:X86 "/OUT:$binary" @objects ws2_32.lib iphlpapi.lib > link.log 2>&1
            if($LASTEXITCODE){throw "Helper link failed; see $output/link.log"}
            if((Get-FileHash -LiteralPath $binary).Hash -ne $helperProfile.hash){throw "Rebuilt helper identity differs. Preserve output; use the documented compiler/SDK. $output"}
            New-Item -ItemType Directory -Path (Split-Path $target) -Force|Out-Null
            Copy-Item -LiteralPath $binary -Destination $target
        }finally{Pop-Location}
    }
    if((Get-FileHash -LiteralPath $target).Hash -ne $helperProfile.hash){throw 'Existing helper differs; never overwrite an unknown image'}
    New-Item -ItemType Directory -Path $output -Force|Out-Null
    & (Join-Path $Repository ('cmake/'+$helperProfile.guard)) -Repository $Repository -Stamp (Join-Path $output 'qualified-input.stamp')
    Write-Output ($helperProfile.name+' helper: exact qualified image verified')
}
