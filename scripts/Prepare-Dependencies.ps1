param([string]$Root = "$PSScriptRoot/../../.local/ofxr-autonomous-deps")
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
New-Item -ItemType Directory -Force $Root | Out-Null
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE) { throw "$Exe failed: $LASTEXITCODE" }
}
function Checkout([string]$Repo, [string]$Commit, [string]$Dest, [string[]]$Sparse) {
    if (!(Test-Path "$Dest/.git")) {
        Run git @('clone', '--filter=blob:none', '--no-checkout', "https://github.com/$Repo.git", $Dest)
    }
    if ($Sparse) { Run git (@('-C', $Dest, 'sparse-checkout', 'set') + $Sparse) }
    Run git @('-C', $Dest, 'checkout', '--detach', $Commit)
    if ((git -C $Dest rev-parse HEAD).Trim() -ne $Commit) { throw "Wrong revision: $Repo" }
}
$source = [IO.Path]::GetFullPath("$PSScriptRoot/..")
$env:GIT_LFS_SKIP_SMUDGE = '1'
Checkout GPUOpen-LibrariesAndSDKs/FidelityFX-SDK c6efa6bf7f2027b3ec94f28578bb5965eabb9e55 "$Root/FidelityFX" @('sdk','PrebuiltSignedDLL')
Run cmake @('-S', "$Root/FidelityFX/sdk", '-B', "$Root/build-fidelityfx", '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DFFX_ALL=OFF', '-DFFX_OF=ON', '-DFFX_API_BACKEND=DX12_X64', '-DFFX_BUILD_AS_DLL=OFF',
    "-DCMAKE_PROJECT_INCLUDE=$source/ffx_private_build.cmake")
Run cmake @('--build', "$Root/build-fidelityfx", '--config', 'Release', '--parallel', '4')
Checkout NVIDIA/DLSS 374959484e79a640feaba44c93ac8cfb0a03f5b5 "$Root/DLSS" @('include','lib/Windows_x86_64/x64','lib/Windows_x86_64/rel')
# NVIDIA stores runtime DLLs in LFS. Only retrieve the runtime the inherited build needs.
Run git @('-C', "$Root/DLSS", 'lfs', 'pull', '--include=lib/Windows_x86_64/rel/nvngx_dlssg.dll')
Checkout NVIDIA-RTX/Streamline b998246a3d499c08765c5681b229c9e6b4513348 "$Root/StreamlineSDK" @('include')
New-Item -ItemType Directory -Force "$Root/Streamline", "$Root/OpenVR/include", "$Root/runtime/fsr-optiscaler", "$Root/runtime/afw-beta6", "$Root/licenses" | Out-Null
Copy-Item "$Root/StreamlineSDK/include/*" "$Root/Streamline" -Recurse -Force
Invoke-WebRequest 'https://raw.githubusercontent.com/ValveSoftware/openvr/v2.5.1/headers/openvr.h' -OutFile "$Root/OpenVR/include/openvr.h"
if ((Get-FileHash "$Root/OpenVR/include/openvr.h").Hash -ne '94E5545370159C85F87CD6E15DD3739F7C919FC7A6E869F5E4ED463533A07ED0') { throw 'OpenVR hash mismatch' }
Copy-Item "$Root/FidelityFX/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll" "$Root/runtime/fsr-optiscaler/ofxr_amd_fidelityfx_framegeneration_dx12.dll" -Force

# Official modular SDK: headers and signed runtime, no rebuild of AMD's ML binary.
$amd = '7539ae8b6568ad29ed78a753404351daf3b5cca7'
Checkout GPUOpen-LibrariesAndSDKs/FidelityFX-SDK $amd "$Root/FSR24" @('Kits/FidelityFX/api/include','Kits/FidelityFX/framegeneration/include','Kits/FidelityFX/upscalers/include','Kits/FidelityFX/signedbin','Kits/FidelityFX/docs')
# Use the SDK's original header layout: includes use relative sibling paths.
Copy-Item "$Root/FSR24/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll" "$Root/runtime/ofxr_amd_fidelityfx_ml_dx12.dll" -Force
Copy-Item "$Root/FSR24/Kits/FidelityFX/docs/license.md" "$Root/licenses/AMD-FSR-2.4.txt" -Force
Copy-Item "$Root/FSR24/3rdpartynotice.md" "$Root/licenses/AMD-FSR-2.4-third-party.txt" -Force

# Required only by the inherited non-FSR routes; no installation/registration.
$afw = "$Root/afw-beta6.zip"
Invoke-WebRequest 'https://github.com/PureDark/UEVR/releases/download/UEVR_AFW_v1.0-beta.6/UEVR-nightly_AFW_v1.0-beta.6.zip' -OutFile $afw
if ((Get-FileHash $afw).Hash -ne 'C5EB5D3F6C356D20D817849C0CF5CC95356ED03AA99100B6237DBB3081DBBE2C') { throw 'AFW archive hash mismatch' }
Expand-Archive $afw "$Root/afw-unpacked" -Force
$dll = Get-ChildItem "$Root/afw-unpacked" -Filter PDAFWPlugin.dll -Recurse | Select-Object -First 1
if (!$dll -or (Get-FileHash $dll.FullName).Hash -ne '5F3CFC38903AC438241A3C47CB4BC3AC329CB39B056E33A63C20045FE15336F9') { throw 'AFW DLL hash mismatch' }
Copy-Item $dll.FullName "$Root/runtime/afw-beta6/PDAFWPlugin.dll" -Force
