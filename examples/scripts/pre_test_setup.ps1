# Pre-Test Setup Script Example
# This script runs before test execution begins

param(
    [hashtable]$Context
)

Write-Host "=== Pre-Test Setup Script ===" -ForegroundColor Green

# Example: Set custom environment variables
$env:AUTOWATERING_TEST_MODE = "automated"
$env:ZEPHYR_EXTRA_MODULES = "modules/custom"

# Example: Create test-specific directories
$testDataDir = "test_data"
if (-not (Test-Path $testDataDir)) {
    New-Item -ItemType Directory -Path $testDataDir -Force
    Write-Host "Created test data directory: $testDataDir" -ForegroundColor Green
}

# Example: Copy test fixtures
if (Test-Path "fixtures") {
    Copy-Item -Path "fixtures\*" -Destination $testDataDir -Recurse -Force
    Write-Host "Copied test fixtures to $testDataDir" -ForegroundColor Green
}

# Example: Start test services (if needed)
# Start-Process -FilePath "test_service.exe" -WindowStyle Hidden

# Example: Validate test prerequisites
$prerequisites = @(
    @{ Name = "CMakeLists.txt"; Path = "CMakeLists.txt" },
    @{ Name = "Project Config"; Path = "prj.conf" },
    @{ Name = "Tests Directory"; Path = "tests" }
)

foreach ($prereq in $prerequisites) {
    if (Test-Path $prereq.Path) {
        Write-Host "✅ $($prereq.Name): Found" -ForegroundColor Green
    } else {
        Write-Host "❌ $($prereq.Name): Missing" -ForegroundColor Red
        throw "Missing prerequisite: $($prereq.Name)"
    }
}

# Example: Log test configuration
Write-Host "Test Configuration:" -ForegroundColor Cyan
Write-Host "  Test Suite: $($Context.TestSuite)" -ForegroundColor White
Write-Host "  Platform: $($Context.Platform)" -ForegroundColor White
Write-Host "  WSL Distribution: $($Context.DistroName)" -ForegroundColor White

# Example: Custom validation logic
if ($Context.TestSuite -eq "hardware" -and $Context.Platform -ne "hardware") {
    Write-Host "⚠️  Warning: Hardware test suite selected but platform is not hardware" -ForegroundColor Yellow
}

Write-Host "Pre-test setup completed successfully" -ForegroundColor Green
return $true