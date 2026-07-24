param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('winmm', 'dbghelp', 'd3d12', 'wininet', 'winhttp')]
    [string]$Name
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$systemDll = Join-Path $env:SystemRoot "System32\$Name.dll"
$bin = Join-Path $project 'bin'
$work = Join-Path $bin "obj\$Name"

if (!(Test-Path $systemDll)) {
    throw "System DLL not found: $systemDll"
}

New-Item -ItemType Directory -Force $bin, $work | Out-Null

# Generate an export list from the actual System32 DLL on the build machine.
$dump = & dumpbin.exe /exports $systemDll | Out-String
if ($LASTEXITCODE) {
    throw "dumpbin failed for $systemDll"
}

$pattern = [regex]'(?m)^\s*(\d+)\s+[0-9A-Fa-f]+\s+(?:[0-9A-Fa-f]+\s+)?([A-Za-z_?][^\s=()]*)'
$exports = @(
    foreach ($match in $pattern.Matches($dump)) {
        [pscustomobject]@{
            Ordinal = [int]$match.Groups[1].Value
            Name = $match.Groups[2].Value
        }
    }
) | Sort-Object Ordinal -Unique

if ($exports.Count -eq 0) {
    throw "Could not parse exports from $systemDll"
}

$initPath = Join-Path $work 'proxy_init.c'
$asmPath = Join-Path $work 'proxy_stubs.asm'
$defPath = Join-Path $work 'proxy.def'

$init = @(
    '#define WIN32_LEAN_AND_MEAN',
    '#include <windows.h>',
    "FARPROC g_genericProxyExports[$($exports.Count)];",
    'BOOL initializeGenericProxy(void) {',
    '    wchar_t path[MAX_PATH];',
    '    UINT n = GetSystemDirectoryW(path, MAX_PATH);',
    "    if (!n || n + $($Name.Length + 5) >= MAX_PATH) return FALSE;",
    ('    lstrcatW(path, L"\\{0}.dll");' -f $Name),
    '    HMODULE module = LoadLibraryW(path);',
    '    if (!module) return FALSE;'
)

$asm = @(
    'option casemap:none',
    'EXTERN g_genericProxyExports:QWORD',
    '',
    '.code'
)
$definition = @("LIBRARY $Name", 'EXPORTS')

for ($i = 0; $i -lt $exports.Count; $i++) {
    $export = $exports[$i]
    $escaped = $export.Name.Replace('\', '\\').Replace('"', '\"')
    $stub = "ProxyExport$i"

    $init += ('    g_genericProxyExports[{0}] = GetProcAddress(module, "{1}");' -f $i, $escaped)
    $init += "    if (!g_genericProxyExports[$i]) return FALSE;"

    $asm += "$stub PROC"
    $asm += "    jmp QWORD PTR [g_genericProxyExports + $i*8]"
    $asm += "$stub ENDP"

    $definition += "$($export.Name)=$stub @$($export.Ordinal)"
}

$init += '    return TRUE;'
$init += '}'
$asm += 'END'

Set-Content -Encoding ASCII $initPath $init
Set-Content -Encoding ASCII $asmPath $asm
Set-Content -Encoding ASCII $defPath $definition

$coreObj = Join-Path $work 'nvcloak.obj'
$initObj = Join-Path $work 'proxy_init.obj'
$stubObj = Join-Path $work 'proxy_stubs.obj'
$output = Join-Path $bin "$Name.dll"

& ml64.exe /nologo /c "/Fo$stubObj" $asmPath
if ($LASTEXITCODE) { throw "ml64 failed for $Name" }

& cl.exe /nologo /O2 /c /DGENERIC_PROXY "/Fo$coreObj" (Join-Path $PSScriptRoot 'nvcloak.c')
if ($LASTEXITCODE) { throw "core compile failed for $Name" }

& cl.exe /nologo /O2 /c "/Fo$initObj" $initPath
if ($LASTEXITCODE) { throw "proxy initialization compile failed for $Name" }

& link.exe /nologo /DLL "/OUT:$output" "/DEF:$defPath" $coreObj $initObj $stubObj `
    kernel32.lib user32.lib ole32.lib uuid.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE) { throw "link failed for $Name" }

Remove-Item -ErrorAction SilentlyContinue (Join-Path $bin "$Name.exp"), (Join-Path $bin "$Name.lib")
Write-Host "Built $output ($($exports.Count) forwarded exports)"
