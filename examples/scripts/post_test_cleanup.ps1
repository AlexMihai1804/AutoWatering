# Post-Test Cleanup Script Example
# This script runs after test execution completes

param(
    [hashtable]$Context
)

Write-Host "=== Post-Test Cleanup Script ===" -ForegroundColor Green

# Example: Archive test results
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$archiveDir = "test_archives\$timestamp"

if (Test-Path "test_results") {
    New-Item -ItemType Directory -Path $archiveDir -Force
    Copy-Item -Path "test_results\*" -Destination $archiveDir -Recurse -Force
    Write-Host "Archived test results to: $archiveDir" -ForegroundColor Green
}

# Example: Generate custom reports
if ($Context.TestSuccess) {
    Write-Host "✅ Tests completed successfully" -ForegroundColor Green
    
    # Example: Generate success notification
    $successReport = @{
        Status = "SUCCESS"
        TestSuite = $Context.TestSuite
        Platform = $Context.Platform
        ExecutionTime = $Context.ExecutionTime.TotalSeconds
        Timestamp = Get-Date
    }
    
    $successReport | ConvertTo-Json | Out-File -FilePath "$archiveDir\success_report.json" -Encoding UTF8
} else {
    Write-Host "❌ Tests completed with errors" -ForegroundColor Red
    
    # Example: Generate failure report
    $failureReport = @{
        Status = "FAILURE"
        TestSuite = $Context.TestSuite
        Platform = $Context.Platform
        ExecutionTime = $Context.ExecutionTime.TotalSeconds
        Timestamp = Get-Date
        ErrorDetails = "Check logs for detailed error information"
    }
    
    $failureReport | ConvertTo-Json | Out-File -FilePath "$archiveDir\failure_report.json" -Encoding UTF8
}

# Example: Clean up temporary files
$tempFiles = @("*.tmp", "*.temp", "temp_*")
foreach ($pattern in $tempFiles) {
    Get-ChildItem -Path . -Filter $pattern -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue
}
Write-Host "Cleaned up temporary files" -ForegroundColor Green

# Example: Clean up test data directory
if (Test-Path "test_data") {
    Remove-Item -Path "test_data" -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "Cleaned up test data directory" -ForegroundColor Green
}

# Example: Stop test services (if started in pre-test)
# Get-Process -Name "test_service" -ErrorAction SilentlyContinue | Stop-Process -Force

# Example: Send notifications (if configured)
if ($Context.Config -and $Context.Config.ContainsKey("notifications")) {
    $notifications = $Context.Config.notifications
    
    if (($Context.TestSuccess -and $notifications.on_success) -or 
        (-not $Context.TestSuccess -and $notifications.on_failure)) {
        
        # Example: Send email notification (pseudo-code)
        # Send-EmailNotification -Recipients $notifications.email_recipients -Status $Context.TestSuccess
        
        # Example: Send Slack notification (pseudo-code)
        # Send-SlackNotification -Webhook $notifications.slack_webhook -Status $Context.TestSuccess
        
        Write-Host "Notifications sent" -ForegroundColor Green
    }
}

# Example: Performance analysis
if ($Context.PerformanceReport) {
    $perf = $Context.PerformanceReport
    
    Write-Host "Performance Summary:" -ForegroundColor Cyan
    Write-Host "  Total Duration: $($perf.ExecutionSummary.TotalDurationSeconds) seconds" -ForegroundColor White
    Write-Host "  Success Rate: $($perf.TestSummary.SuccessRate)%" -ForegroundColor White
    
    # Example: Performance threshold checking
    if ($perf.ExecutionSummary.TotalDurationSeconds -gt 1800) { # 30 minutes
        Write-Host "⚠️  Warning: Test execution exceeded 30 minutes" -ForegroundColor Yellow
    }
    
    if ($perf.TestSummary.SuccessRate -lt 95) {
        Write-Host "⚠️  Warning: Test success rate below 95%" -ForegroundColor Yellow
    }
}

# Example: Log cleanup summary
Write-Host "Cleanup Summary:" -ForegroundColor Cyan
Write-Host "  Archive Directory: $archiveDir" -ForegroundColor White
Write-Host "  Test Status: $(if ($Context.TestSuccess) { 'SUCCESS' } else { 'FAILURE' })" -ForegroundColor White
Write-Host "  Execution Time: $($Context.ExecutionTime.ToString('mm\:ss'))" -ForegroundColor White

Write-Host "Post-test cleanup completed successfully" -ForegroundColor Green
return $true