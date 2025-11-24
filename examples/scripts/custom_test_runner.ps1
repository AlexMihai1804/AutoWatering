# Custom Test Runner Example
# This script demonstrates how to create a custom test execution workflow

param(
    [string]$TestType = "unit",
    [string]$Environment = "development",
    [switch]$GenerateReport,
    [switch]$SendNotifications
)

Write-Host "=== Custom Test Runner ===" -ForegroundColor Green
Write-Host "Test Type: $TestType" -ForegroundColor Cyan
Write-Host "Environment: $Environment" -ForegroundColor Cyan

# Configuration based on environment
$configs = @{
    development = "examples/configurations/development_config.json"
    ci_cd = "examples/configurations/ci_cd_config.json"
    minimal = "examples/configurations/minimal_config.json"
}

$configFile = $configs[$Environment]
if (-not $configFile -or -not (Test-Path $configFile)) {
    Write-Host "❌ Configuration not found for environment: $Environment" -ForegroundColor Red
    exit 1
}

Write-Host "Using configuration: $configFile" -ForegroundColor Green

# Test type to suite mapping
$testSuites = @{
    unit = "unit"
    integration = "integration"
    hardware = "hardware"
    all = "all"
    smoke = "quick"
    regression = "all"
}

$testSuite = $testSuites[$TestType]
if (-not $testSuite) {
    Write-Host "❌ Unknown test type: $TestType" -ForegroundColor Red
    Write-Host "Available types: $($testSuites.Keys -join ', ')" -ForegroundColor Yellow
    exit 1
}

# Platform selection based on test type
$platformMap = @{
    unit = "native"
    integration = "both"
    hardware = "hardware"
    all = "both"
    smoke = "native"
    regression = "both"
}

$platform = $platformMap[$TestType]

try {
    Write-Host "Starting test execution..." -ForegroundColor Green
    
    # Pre-execution setup
    if (Test-Path "examples/scripts/pre_test_setup.ps1") {
        Write-Host "Running pre-test setup..." -ForegroundColor Yellow
        $setupResult = & "examples/scripts/pre_test_setup.ps1" -Context @{
            TestSuite = $testSuite
            Platform = $platform
            Environment = $Environment
        }
        
        if (-not $setupResult) {
            throw "Pre-test setup failed"
        }
    }
    
    # Main test execution
    $testArgs = @(
        "-TestSuite", $testSuite,
        "-Platform", $platform,
        "-ConfigFile", $configFile
    )
    
    if ($Environment -eq "development") {
        $testArgs += "-VerboseOutput"
    }
    
    Write-Host "Executing: .\run_tests_wsl.ps1 $($testArgs -join ' ')" -ForegroundColor Cyan
    $testResult = & ".\run_tests_wsl.ps1" @testArgs
    $testSuccess = $LASTEXITCODE -eq 0
    
    # Post-execution cleanup
    if (Test-Path "examples/scripts/post_test_cleanup.ps1") {
        Write-Host "Running post-test cleanup..." -ForegroundColor Yellow
        $cleanupResult = & "examples/scripts/post_test_cleanup.ps1" -Context @{
            TestSuite = $testSuite
            Platform = $platform
            Environment = $Environment
            TestSuccess = $testSuccess
            ExecutionTime = (Get-Date) - $startTime
        }
    }
    
    # Generate custom report
    if ($GenerateReport) {
        Write-Host "Generating custom report..." -ForegroundColor Yellow
        Generate-CustomReport -TestType $TestType -Environment $Environment -Success $testSuccess
    }
    
    # Send notifications
    if ($SendNotifications) {
        Write-Host "Sending notifications..." -ForegroundColor Yellow
        Send-CustomNotifications -TestType $TestType -Environment $Environment -Success $testSuccess
    }
    
    # Final status
    if ($testSuccess) {
        Write-Host "✅ Custom test runner completed successfully" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "❌ Custom test runner completed with errors" -ForegroundColor Red
        exit 1
    }
    
} catch {
    Write-Host "❌ Custom test runner failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

function Generate-CustomReport {
    param(
        [string]$TestType,
        [string]$Environment,
        [bool]$Success
    )
    
    $reportData = @{
        TestType = $TestType
        Environment = $Environment
        Success = $Success
        Timestamp = Get-Date
        ReportVersion = "1.0"
    }
    
    # Add performance data if available
    if (Test-Path "performance_report.json") {
        $perfData = Get-Content "performance_report.json" | ConvertFrom-Json
        $reportData.Performance = $perfData
    }
    
    # Add test results if available
    if (Test-Path "test_results") {
        $testFiles = Get-ChildItem "test_results" -Filter "*.json" | Measure-Object
        $reportData.TestFiles = $testFiles.Count
    }
    
    $reportPath = "custom_test_report_$(Get-Date -Format 'yyyyMMdd_HHmmss').json"
    $reportData | ConvertTo-Json -Depth 10 | Out-File -FilePath $reportPath -Encoding UTF8
    
    Write-Host "Custom report generated: $reportPath" -ForegroundColor Green
}

function Send-CustomNotifications {
    param(
        [string]$TestType,
        [string]$Environment,
        [bool]$Success
    )
    
    $status = if ($Success) { "SUCCESS" } else { "FAILURE" }
    $color = if ($Success) { "good" } else { "danger" }
    
    # Example: Console notification
    Write-Host "📧 Notification: $TestType tests in $Environment environment: $status" -ForegroundColor $(if ($Success) { "Green" } else { "Red" })
    
    # Example: File-based notification (for integration with other systems)
    $notification = @{
        TestType = $TestType
        Environment = $Environment
        Status = $status
        Timestamp = Get-Date
        Message = "$TestType tests in $Environment environment completed with status: $status"
    }
    
    $notificationPath = "notifications/test_notification_$(Get-Date -Format 'yyyyMMdd_HHmmss').json"
    New-Item -ItemType Directory -Path "notifications" -Force | Out-Null
    $notification | ConvertTo-Json | Out-File -FilePath $notificationPath -Encoding UTF8
    
    Write-Host "Notification saved: $notificationPath" -ForegroundColor Green
}