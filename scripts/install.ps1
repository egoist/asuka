# Installs the asuka CLI on Windows from its GitHub release, and the
# libraries it carries prebuilt for this computer:
#
#	irm https://asuka.egoist.dev/install.ps1 | iex
#
# $env:ASUKA_VERSION picks a release (default: the latest);
# $env:ASUKA_INSTALL the directory (default:
# %LOCALAPPDATA%\Programs\asuka\bin), which is added to the user's PATH.
# Windows PowerShell 5.1 and PowerShell 7; x64 and arm64.

# A function, and throw, not exit: run by iex, exit would close the
# shell it's run in.
function Install-Asuka {
	$ErrorActionPreference = 'Stop'
	$ProgressPreference = 'SilentlyContinue' # 5.1's progress bar slows a download down
	if ($PSVersionTable.PSVersion.Major -lt 6) {
		# Windows PowerShell 5.1 may not offer TLS 1.2 by itself.
		[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
	}

	$repo = 'egoist/asuka'
	$dir = $env:ASUKA_INSTALL
	if (-not $dir) { $dir = Join-Path $env:LOCALAPPDATA 'Programs\asuka\bin' }

	# The OS's architecture, not the process's (an x64 PowerShell runs
	# emulated on an arm64 PC).
	$arch = $null
	try { $arch = [string][System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture } catch {}
	if (-not $arch) {
		$arch = $env:PROCESSOR_ARCHITEW6432
		if (-not $arch) { $arch = $env:PROCESSOR_ARCHITECTURE }
	}
	switch -Regex ($arch) {
		'^(Arm64|ARM64)$' { $cpu = 'arm64' }
		'^(X64|AMD64)$' { $cpu = 'x64' }
		default { throw "asuka: no build for $arch (x64 and arm64 only)" }
	}
	$zip = "asuka-windows-$cpu.zip"
	$raw = "asuka-windows-$cpu.exe"

	if ($env:ASUKA_VERSION) {
		$base = "https://github.com/$repo/releases/download/v$($env:ASUKA_VERSION -replace '^v', '')"
	} else {
		$base = "https://github.com/$repo/releases/latest/download"
	}

	$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("asuka-" + [guid]::NewGuid())
	New-Item -ItemType Directory -Path $tmp | Out-Null
	try {
		$sums = Join-Path $tmp 'SHA256SUMS'
		Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums
		$hashes = @{}
		foreach ($line in Get-Content $sums) {
			$f = $line -split '\s+', 2
			if ($f.Count -eq 2) { $hashes[$f[1].TrimStart('*')] = $f[0].ToLower() }
		}
		# Zipped, a third of its size; a release before those (0.2.9 and
		# earlier) has the executable as it is.
		$asset = $zip
		if (-not $hashes.ContainsKey($asset)) { $asset = $raw }
		if (-not $hashes.ContainsKey($asset)) { throw "asuka: SHA256SUMS has no $zip or $raw" }
		Write-Host "asuka: downloading $asset"
		$download = Join-Path $tmp $asset
		Invoke-WebRequest -UseBasicParsing -Uri "$base/$asset" -OutFile $download
		$got = (Get-FileHash -Algorithm SHA256 $download).Hash.ToLower()
		if ($hashes[$asset] -ne $got) { throw "asuka: the download doesn't match its checksum" }
		$exe = Join-Path $tmp 'asuka.exe'
		if ($asset -eq $zip) {
			Expand-Archive -Path $download -DestinationPath (Join-Path $tmp 'unzipped')
			Move-Item -Force (Join-Path $tmp 'unzipped\asuka.exe') $exe
		} else {
			Move-Item -Force $download $exe
		}

		New-Item -ItemType Directory -Force -Path $dir | Out-Null
		$dest = Join-Path $dir 'asuka.exe'
		$old = "$dest.old"
		Remove-Item -Force -ErrorAction SilentlyContinue $old
		if (Test-Path $dest) {
			# A running asuka.exe can't be overwritten, but it can be renamed.
			Move-Item -Force $dest $old
		}
		Move-Item -Force $exe $dest
		Remove-Item -Force -ErrorAction SilentlyContinue $old
		Write-Host "asuka: installed $dest ($(& $dest version))"
		# This computer's prebuilt libraries (its own apps'), checked
		# against the checksums the CLI carries, so the first build doesn't
		# wait for them; if that fails, the first build fetches them.
		& $dest prebuilt fetch
		if ($LASTEXITCODE -ne 0) { Write-Host "asuka: the prebuilt libraries weren't fetched; the first build fetches them" }
	} finally {
		Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tmp
	}

	# The user's PATH, as the registry has it (not this session's, which has
	# the machine's too), and this session's.
	$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
	$parts = @()
	if ($userPath) { $parts = $userPath -split ';' | Where-Object { $_ } }
	if (-not ($parts | Where-Object { $_.TrimEnd('\') -ieq $dir.TrimEnd('\') })) {
		[Environment]::SetEnvironmentVariable('Path', (@($parts) + $dir) -join ';', 'User')
		Write-Host "asuka: added $dir to your PATH (new terminals have it)"
	}
	if (-not (($env:Path -split ';') | Where-Object { $_.TrimEnd('\') -ieq $dir.TrimEnd('\') })) {
		$env:Path = "$env:Path;$dir"
	}
}

Install-Asuka
