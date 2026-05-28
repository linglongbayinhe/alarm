param(
    [string]$FontPath = "D:\Projects\AI\alarm_esp+app\font_09_SourceHanSansSC\OTF\SimplifiedChinese\SourceHanSansSC-Normal.otf",
    [string]$SymbolsPath = "$PSScriptRoot\font_symbols.txt",
    [string]$OutputPath = "$PSScriptRoot\..\ui\ui_font_source_han_sans_sc_normal_16.c"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $FontPath)) {
    throw "Font file not found: $FontPath"
}
if (-not (Test-Path -LiteralPath $SymbolsPath)) {
    throw "Symbol file not found: $SymbolsPath"
}

$symbols = (Get-Content -LiteralPath $SymbolsPath -Encoding UTF8 |
    Where-Object { $_ -notmatch '^\s*#' }) -join ''

if ([string]::IsNullOrWhiteSpace($symbols)) {
    throw "No symbols found in $SymbolsPath"
}

lv_font_conv `
    --bpp 4 `
    --size 16 `
    --no-compress `
    --font $FontPath `
    --symbols $symbols `
    --range 32-127 `
    --format lvgl `
    --lv-include lvgl.h `
    --lv-font-name ui_font_source_han_sans_sc_normal_16 `
    --output $OutputPath
