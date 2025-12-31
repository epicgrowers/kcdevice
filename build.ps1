#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Build script for KC-Device firmware supporting ESP32-S3 and ESP32-C6 targets

.DESCRIPTION
    This script simplifies building, flashing, and monitoring the KC-Device firmware
    for different ESP32 chip variants. It automatically manages separate build
    directories and SDK configurations for each target.

.PARAMETER Target
    The ESP32 chip target: 's3' (ESP32-S3) or 'c6' (ESP32-C6)

.PARAMETER Action
    The action to perform: 'build', 'flash', 'monitor', 'clean', 'fullclean', 'menuconfig', 'test', or 'all'

.PARAMETER Port
    COM port for flashing (e.g., COM3). If not specified, idf.py will auto-detect

.EXAMPLE
    .\build.ps1 -Target s3 -Action build
    Build firmware for ESP32-S3

.EXAMPLE
    .\build.ps1 -Target c6 -Action all -Port COM3
    Build and flash firmware for ESP32-C6 on COM3

.EXAMPLE
    .\build.ps1 -Target s3 -Action monitor -Port COM3
    Monitor serial output from ESP32-S3 on COM3
#>

param(
    [Parameter(Mandatory=$true, Position=0)]
    [ValidateSet('s3', 'c6', 'S3', 'C6')]
    [string]$Target,

    [Parameter(Mandatory=$false, Position=1)]
    [ValidateSet('build', 'flash', 'monitor', 'clean', 'fullclean', 'menuconfig', 'test', 'all')]
    [string]$Action = 'build',

    [Parameter(Mandatory=$false)]
    [string]$Port = ""
)

# Normalize target to lowercase
$Target = $Target.ToLower()

# Configuration mapping
$config = @{
    's3' = @{
        Name = 'ESP32-S3'
        BuildDir = 'build_s3'
        SdkConfig = 'sdkconfig.s3'
        ChipTarget = 'esp32s3'
        Architecture = 'Xtensa'
    }
    'c6' = @{
        Name = 'ESP32-C6'
        BuildDir = 'build_c6'
        SdkConfig = 'sdkconfig.c6'
        ChipTarget = 'esp32c6'
        Architecture = 'RISC-V'
    }
}

$targetConfig = $config[$Target]

function Test-RuntimeConfiguration {
    param(
        [string]$RepoRoot,
        [string]$Target
    )

    $runtimeDir = Join-Path $RepoRoot "config/runtime"
    $servicesFile = Join-Path $runtimeDir "services.json"
    $apiKeysFile = Join-Path $runtimeDir "api_keys.json"
    $errors = @()

    if (-not (Test-Path $servicesFile)) {
        $errors += "Missing runtime config file: $servicesFile"
    } else {
        try {
            $servicesJson = Get-Content -Path $servicesFile -Raw -Encoding UTF8 | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $errors += "Failed to parse ${servicesFile}: $($_.Exception.Message)"
        }

        $httpEnabled = $false
        $mqttEnabled = $false
        $mdnsEnabled = $false
        $timeSyncEnabled = $false

        if ($servicesJson -and $servicesJson.services) {
            $servicesSection = $servicesJson.services
            $httpEnabled = [bool]$servicesSection.enable_http_server
            $mqttEnabled = [bool]$servicesSection.enable_mqtt
            $mdnsEnabled = [bool]$servicesSection.enable_mdns
            $timeSyncEnabled = [bool]$servicesSection.enable_time_sync

            if ($httpEnabled -and $Target -eq 'c6') {
                Write-Warning "HTTPS dashboard is enabled in services.json but the ESP32-C6 build disables it at runtime."
            }

            if ($httpEnabled -and (-not $servicesSection.https_port -or $servicesSection.https_port -le 0)) {
                $errors += "services.json must provide a positive https_port when enable_http_server is true."
            }
        } else {
            $errors += "services.json is missing the 'services' object."
        }

        if ($mqttEnabled) {
            if (-not $servicesJson.mqtt) {
                $errors += "services.json is missing the 'mqtt' object while MQTT service is enabled."
            } else {
                $mqttSection = $servicesJson.mqtt

                if ([string]::IsNullOrWhiteSpace($mqttSection.broker_uri)) {
                    $errors += "mqtt.broker_uri must be a non-empty string when MQTT is enabled."
                }

                if ([string]::IsNullOrWhiteSpace($mqttSection.username)) {
                    $errors += "mqtt.username must be provided when MQTT is enabled."
                }

                if ([string]::IsNullOrWhiteSpace($mqttSection.password)) {
                    $errors += "mqtt.password must be provided when MQTT is enabled."
                }
            }

            if (-not $servicesJson.telemetry) {
                $errors += "services.json is missing the 'telemetry' object while MQTT service is enabled."
            } else {
                $telemetrySection = $servicesJson.telemetry

                if ($null -eq $telemetrySection.publish_interval_sec -or $telemetrySection.publish_interval_sec -lt 0 -or $telemetrySection.publish_interval_sec -gt 86400) {
                    $errors += "telemetry.publish_interval_sec must be between 0 and 86400 seconds."
                }

                $backoffFields = @('busy_backoff_ms', 'client_backoff_ms', 'idle_delay_ms')
                foreach ($field in $backoffFields) {
                    $value = $telemetrySection.$field
                    if ($null -eq $value -or $value -lt 0 -or $value -gt 600000) {
                        $errors += "telemetry.$field must be between 0 and 600000 milliseconds."
                    }
                }
            }
        }

        if ($mdnsEnabled -or $httpEnabled) {
            if (-not $servicesJson.dashboard) {
                $errors += "services.json is missing the 'dashboard' object while HTTP/mDNS services are enabled."
            } else {
                $dashboardSection = $servicesJson.dashboard
                $hostname = $dashboardSection.mdns_hostname
                $instanceName = $dashboardSection.mdns_instance_name

                if ([string]::IsNullOrWhiteSpace($hostname)) {
                    $errors += "dashboard.mdns_hostname must be provided when the dashboard or mDNS is enabled."
                } elseif (-not [regex]::IsMatch($hostname.ToLowerInvariant(), '^[a-z0-9\-]+$')) {
                    $errors += "dashboard.mdns_hostname may only contain lowercase letters, numbers, and hyphens."
                } elseif ($hostname.Length -gt 32) {
                    $errors += "dashboard.mdns_hostname must be 32 characters or fewer."
                }

                if ([string]::IsNullOrWhiteSpace($instanceName)) {
                    $errors += "dashboard.mdns_instance_name must be provided when the dashboard or mDNS is enabled."
                } elseif ($instanceName.Length -gt 63) {
                    $errors += "dashboard.mdns_instance_name must be 63 characters or fewer (mDNS label limit)."
                }
            }
        }

        if ($timeSyncEnabled) {
            if (-not $servicesJson.time_sync) {
                $errors += "services.json is missing the 'time_sync' object while time synchronization is enabled."
            } else {
                $timeSection = $servicesJson.time_sync
                if ([string]::IsNullOrWhiteSpace($timeSection.timezone)) {
                    $errors += "time_sync.timezone must be provided when time synchronization is enabled."
                }

                if ($null -eq $timeSection.timeout_sec -or $timeSection.timeout_sec -le 0 -or $timeSection.timeout_sec -gt 120) {
                    $errors += "time_sync.timeout_sec must be between 1 and 120 seconds."
                }

                if ($null -ne $timeSection.retry_attempts) {
                    if ($timeSection.retry_attempts -lt 0 -or $timeSection.retry_attempts -gt 5) {
                        $errors += "time_sync.retry_attempts must be between 0 and 5."
                    }
                }

                if ($null -ne $timeSection.retry_delay_sec) {
                    if ($timeSection.retry_delay_sec -lt 1 -or $timeSection.retry_delay_sec -gt 300) {
                        $errors += "time_sync.retry_delay_sec must be between 1 and 300 seconds."
                    }
                }
            }
        }
    }

    if (-not (Test-Path $apiKeysFile)) {
        $errors += "Missing runtime key file: $apiKeysFile"
    } else {
        try {
            $apiKeysJson = Get-Content -Path $apiKeysFile -Raw -Encoding UTF8 | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $errors += "Failed to parse ${apiKeysFile}: $($_.Exception.Message)"
        }

        if ($apiKeysJson -and $apiKeysJson.api_keys) {
            $validKeys = @()
            foreach ($entry in $apiKeysJson.api_keys) {
                if (-not $entry.value -or [string]::IsNullOrWhiteSpace($entry.value)) {
                    continue
                }

                if ($entry.value -eq 'REPLACE_WITH_PROVISIONING_KEY') {
                    continue
                }

                if ($entry.type -and $entry.type -notin @('cloud', 'dashboard', 'custom')) {
                    $errors += "API key '$($entry.name)' has unsupported type '$($entry.type)'."
                }

                $validKeys += $entry
            }

            if ($validKeys.Count -eq 0) {
                $errors += "api_keys.json must contain at least one non-placeholder key value."
            }
        } else {
            $errors += "api_keys.json is missing the 'api_keys' array."
        }
    }

    if ($errors.Count -gt 0) {
        Write-Host "" 
        Write-Host "Runtime configuration validation failed:" -ForegroundColor Red
        foreach ($err in $errors) {
            Write-Host "  - $err" -ForegroundColor Red
        }
        Write-Host ""
        return $false
    }

    Write-Host "Runtime configuration validation passed." -ForegroundColor Green
    return $true
}

# Display header
Write-Host ""
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  KC-Device Build Script" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  Target:       $($targetConfig.Name) ($($targetConfig.Architecture))" -ForegroundColor Yellow
Write-Host "  Build Dir:    $($targetConfig.BuildDir)" -ForegroundColor Yellow
Write-Host "  SDK Config:   $($targetConfig.SdkConfig)" -ForegroundColor Yellow
Write-Host "  Action:       $Action" -ForegroundColor Yellow
if ($Port) {
    Write-Host "  Port:         $Port" -ForegroundColor Yellow
}
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""

$actionsRequiringValidation = @('build', 'flash', 'all', 'test')
if ($actionsRequiringValidation -contains $Action) {
    if (-not (Test-RuntimeConfiguration -RepoRoot $PSScriptRoot -Target $Target)) {
        exit 1
    }
}

# Build the idf.py command
$idfCmd = "idf.py"
$idfArgs = @(
    "-B", $targetConfig.BuildDir,
    "-D", "SDKCONFIG=$($targetConfig.SdkConfig)"
)

# Add port if specified
if ($Port) {
    $idfArgs += @("-p", $Port)
}

# Execute based on action
switch ($Action) {
    'build' {
        Write-Host "Building firmware for $($targetConfig.Name)..." -ForegroundColor Green
        & $idfCmd @idfArgs build
    }
    'flash' {
        Write-Host "Flashing firmware to $($targetConfig.Name)..." -ForegroundColor Green
        & $idfCmd @idfArgs flash
    }
    'monitor' {
        Write-Host "Starting serial monitor for $($targetConfig.Name)..." -ForegroundColor Green
        Write-Host "Press Ctrl+] to exit" -ForegroundColor Yellow
        & $idfCmd @idfArgs monitor
    }
    'clean' {
        Write-Host "Cleaning build directory for $($targetConfig.Name)..." -ForegroundColor Green
        & $idfCmd @idfArgs clean
    }
    'fullclean' {
        Write-Host "Performing full clean for $($targetConfig.Name)..." -ForegroundColor Green
        & $idfCmd @idfArgs fullclean
    }
    'menuconfig' {
        Write-Host "Opening menuconfig for $($targetConfig.Name)..." -ForegroundColor Green
        & $idfCmd @idfArgs menuconfig
    }
    'test' {
        Write-Host "Building firmware for $($targetConfig.Name) prior to tests..." -ForegroundColor Green
        & $idfCmd @idfArgs build
        if ($LASTEXITCODE -ne 0) {
            break
        }

        $testDir = Join-Path $PSScriptRoot "test"
        if (-not (Test-Path $testDir)) {
            Write-Warning "Test directory '$testDir' not found. Skipping tests."
            break
        }

        $pythonCmd = Get-Command python -ErrorAction SilentlyContinue
        if (-not $pythonCmd) {
            Write-Host "Python executable not found in PATH. Install Python to run tests." -ForegroundColor Red
            $LASTEXITCODE = 1
            break
        }

        $pythonExe = $pythonCmd.Source

        & $pythonExe "-c" "import importlib.util, sys; sys.exit(0 if importlib.util.find_spec('pytest') else 1)"
        $pytestAvailable = ($LASTEXITCODE -eq 0)

        if ($pytestAvailable) {
            Write-Host "Running pytest suite from '$testDir'..." -ForegroundColor Green
            & $pythonExe "-m" "pytest" $testDir
            break
        }

        Write-Warning "pytest is not installed in the active environment. Running standalone test scripts instead."
        $testScripts = Get-ChildItem -Path $testDir -Filter 'test_*.py' -File | Sort-Object Name
        if ($testScripts.Count -eq 0) {
            Write-Warning "No test_*.py files found in '$testDir'."
            break
        }

        foreach ($script in $testScripts) {
            Write-Host "Executing $($script.Name)..." -ForegroundColor Green
            & $pythonExe $script.FullName
            if ($LASTEXITCODE -ne 0) {
                break
            }
        }
    }
    'all' {
        Write-Host "Building and flashing firmware for $($targetConfig.Name)..." -ForegroundColor Green
        & $idfCmd @idfArgs build
        if ($LASTEXITCODE -eq 0) {
            Write-Host ""
            Write-Host "Build successful! Flashing..." -ForegroundColor Green
            & $idfCmd @idfArgs flash
            if ($LASTEXITCODE -eq 0) {
                Write-Host ""
                Write-Host "Flash successful! Starting monitor..." -ForegroundColor Green
                Write-Host "Press Ctrl+] to exit" -ForegroundColor Yellow
                & $idfCmd @idfArgs monitor
            }
        }
    }
}

# Check exit code
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Red
    Write-Host "  ERROR: Command failed with exit code $LASTEXITCODE" -ForegroundColor Red
    Write-Host "================================================================" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "================================================================" -ForegroundColor Green
Write-Host "  SUCCESS: $Action completed for $($targetConfig.Name)" -ForegroundColor Green
Write-Host "================================================================" -ForegroundColor Green
Write-Host ""
