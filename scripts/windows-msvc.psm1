Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Initialize-AwjMsvcToolchain {
    param([string]$CompilerDirectory = '')
    $PSNativeCommandUseErrorActionPreference = $false
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw '缺少 Windows C++ 工具链定位器。' }
    $vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $vs) { throw '没有找到 MSVC 工具链。' }
    $devcmd = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
    $env:PATH = (Split-Path $vswhere -Parent) + ';' + $env:PATH
    $environment = & $env:ComSpec /d /c "call `"$devcmd`" -no_logo -arch=amd64 -host_arch=amd64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw '初始化 MSVC x64 构建环境失败。' }
    foreach ($entry in $environment) {
        $separator = $entry.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable($entry.Substring(0,$separator), $entry.Substring($separator+1), 'Process')
        }
    }
    if (-not $CompilerDirectory) { $CompilerDirectory = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64' }
    $directory = [IO.Path]::GetFullPath($CompilerDirectory)
    $compiler = Join-Path $directory 'cl.exe'
    $linker = Join-Path $directory 'link.exe'
    $archive = Join-Path $directory 'lib.exe'
    $resource = Join-Path $env:WindowsSdkVerBinPath 'x64/rc.exe'
    foreach ($path in @($compiler,$linker,$archive,$resource)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "MSVC/Windows SDK 工具不存在：$path" }
    }
    $compilerOutput = @(& $compiler /? 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'MSVC 编译器身份探测失败。' }
    $banner = [string]$compilerOutput[0]
    if ($banner -notmatch '^Microsoft \(R\).*?([0-9]+\.[0-9]+\.[0-9]+).*?x64') {
        throw 'Windows 生产构建必须使用真正的 MSVC cl.exe，不能用 clang-cl 替代。'
    }
    $version = $Matches[1]
    $linkerOutput = @(& $linker /? 2>&1)
    # link /? 的帮助退出码为 1100；真实构建仍必须退出 0。
    if ($LASTEXITCODE -notin @(0,1100) -or [string]$linkerOutput[0] -notmatch '^Microsoft \(R\).*?([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)') {
        throw 'MSVC 链接器身份探测失败。'
    }
    $linkerVersion = $Matches[1]
    $global:LASTEXITCODE = 0
    [pscustomobject]@{
        compiler=$compiler; compiler_id='MSVC'; compiler_version=$version; compiler_banner=$banner
        compiler_sha256=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash.ToLowerInvariant()
        linker=$linker; linker_version=$linkerVersion
        linker_sha256=(Get-FileHash -LiteralPath $linker -Algorithm SHA256).Hash.ToLowerInvariant()
        archive=$archive; resource=$resource; visual_studio=$vs
    }
}

function Assert-AwjMsvcReleaseEvidence {
    param($Evidence, [string[]]$ExpectedTargets, [string]$MinimumCpu = '')
    if ($null -eq $Evidence) { throw '缺少 MSVC 构建证据。' }
    foreach ($field in @('compiler_id','compiler_version','compiler_sha256','linker_version','linker_sha256','flags_source')) {
        if ($Evidence.$field -isnot [string]) { throw "MSVC 证据字段必须为字符串：$field" }
    }
    if ($Evidence.flags_source -cne 'cmake-generated-target-profile') { throw 'MSVC 参数没有绑定生成器目标 profile。' }
    if ($Evidence.compiler_id -cne 'MSVC' -or
        $Evidence.compiler_version -cnotmatch '^19\.[0-9]+\.[0-9]+$' -or
        $Evidence.compiler_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $Evidence.linker_version -cnotmatch '^14\.[0-9]+\.[0-9]+\.[0-9]+$' -or
        $Evidence.linker_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $Evidence.targets -isnot [Array] -or $Evidence.targets.Count -ne $ExpectedTargets.Count) {
        throw '缺少匹配实际 MSVC 编译器、链接器和目标的构建证据。'
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($target in $Evidence.targets) {
        foreach ($field in @('name','configuration','runtime','profile_sha256','minimum_cpu')) {
            if ($target.$field -isnot [string]) { throw "MSVC 目标字段必须为字符串：$field" }
        }
        if ($target.name -cnotin $ExpectedTargets -or -not $seen.Add($target.name) -or
            $target.configuration -cne 'Release' -or $target.runtime -cne 'MultiThreaded' -or
            $target.ipo -isnot [bool] -or $target.ipo -ne $true -or
            $target.profile_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            $target.compile_flags -isnot [Array] -or $target.link_flags -isnot [Array]) {
            throw 'MSVC Release 目标、静态运行库或 IPO 证据无效。'
        }
        foreach ($flag in @($target.compile_flags) + @($target.link_flags)) {
            if ($flag -isnot [string]) { throw 'MSVC 优化参数必须为字符串数组。' }
        }
        foreach ($flag in @('/O2','/Ob2','/Oi','/Ot','/Gy','/Gw','/GF','/GL','/MT')) {
            if ($flag -cnotin $target.compile_flags) { throw "MSVC Release 优化降档或缺少参数：$flag" }
        }
        foreach ($flag in @('/LTCG','/OPT:REF','/OPT:ICF','/INCREMENTAL:NO')) {
            if ($flag -cnotin $target.link_flags) { throw "MSVC Release 链接优化降档或缺少参数：$flag" }
        }
        if (@($target.compile_flags | Where-Object { $_ -match '^/(Od|O1|Os|Ob[01]|(GL|Oi|Ot|Gy|Gw|GF)-|MDd?|fp:fast)$' }).Count -or
            @($target.link_flags | Where-Object { $_ -match '^/(LTCG:OFF|INCREMENTAL(:YES)?|OPT:(NOREF|NOICF))$' }).Count) {
            throw 'MSVC Release 包含覆盖优化、静态运行库或浮点契约的参数。'
        }
        if ($MinimumCpu -and $target.minimum_cpu -cne $MinimumCpu) { throw 'MSVC 目标最低 CPU 与交付 profile 不一致。' }
        if ($target.minimum_cpu -ceq 'AVX2' -and '/arch:AVX2' -cnotin $target.compile_flags) {
            throw 'MSVC 应用缺少实际 AVX2 参数。'
        }
    }
}

function Get-AwjMsvcBuildEvidence {
    param([string]$BuildDirectory, [string[]]$Targets, $Toolchain, [string]$MinimumCpu = '')
    if ((Get-FileHash -LiteralPath $Toolchain.compiler -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Toolchain.compiler_sha256 -or
        (Get-FileHash -LiteralPath $Toolchain.linker -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Toolchain.linker_sha256) {
        throw 'MSVC 编译器或链接器在构建期间变化。'
    }
    $records = @(
        foreach ($target in $Targets) {
            $path = Join-Path $BuildDirectory "awj-msvc-$target-Release.txt"
            $data = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
            foreach ($line in [IO.File]::ReadAllLines($path)) {
                if (-not $line) { continue }
                $separator = $line.IndexOf('=')
                if ($separator -lt 1 -or -not $data.TryAdd($line.Substring(0,$separator), $line.Substring($separator+1))) {
                    throw 'MSVC 目标 profile 存在缺失或重复字段。'
                }
            }
            # CMake 的 MSVC 版本附带第四段 .0；保留原值，仅对齐 banner 的三段比较。
            $compilerVersion = $data['compiler_version'] -replace '^(19\.[0-9]+\.[0-9]+)\.0$', '$1'
            if ($data['schema'] -cne '1' -or $data['target'] -cne $target -or
                $data['compiler_id'] -cne 'MSVC' -or $compilerVersion -cne $Toolchain.compiler_version -or
                [IO.Path]::GetFullPath($data['compiler']) -ine [IO.Path]::GetFullPath($Toolchain.compiler) -or
                [IO.Path]::GetFullPath($data['linker']) -ine [IO.Path]::GetFullPath($Toolchain.linker) -or
                $data['compiler_sha256'] -cne $Toolchain.compiler_sha256 -or
                $data['linker_sha256'] -cne $Toolchain.linker_sha256) {
                throw 'CMake 实际编译器/链接器与可信 MSVC 工具链不一致。'
            }
            $compileFlags = @($data['compile_flags'] -split '[; \t]+' | Where-Object { $_ })
            $linkFlags = @($data['link_flags'] -split '[; \t]+' | Where-Object { $_ })
            if ($data['ipo'] -ceq '1') {
                $compileFlags += @($data['ipo_compile_flags'] -split '[; \t]+' | Where-Object { $_ })
                $linkFlags += @($data['ipo_link_flags'] -split '[; \t]+' | Where-Object { $_ })
            }
            if ($data['runtime'] -ceq 'MultiThreaded') { $compileFlags += '/MT' }
            [pscustomobject]@{
                name=$target; configuration=$data['configuration']; runtime=$data['runtime']; ipo=($data['ipo'] -ceq '1')
                cmake_compiler_version=$data['compiler_version']
                minimum_cpu=$data['minimum_cpu']; compile_flags=@($compileFlags | Select-Object -Unique)
                link_flags=@($linkFlags | Select-Object -Unique)
                profile_sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
    )
    $evidence = [pscustomobject]@{
        compiler_id='MSVC'; compiler_version=$Toolchain.compiler_version; compiler_banner=$Toolchain.compiler_banner
        compiler_sha256=$Toolchain.compiler_sha256; linker_version=$Toolchain.linker_version; linker_sha256=$Toolchain.linker_sha256
        flags_source='cmake-generated-target-profile'; targets=$records
    }
    Assert-AwjMsvcReleaseEvidence $evidence $Targets $MinimumCpu
    return $evidence
}

Export-ModuleMember -Function Initialize-AwjMsvcToolchain, Assert-AwjMsvcReleaseEvidence, Get-AwjMsvcBuildEvidence
