# Environmental History Characteristic (UUID: 12345678-1234-5678-1234-56789abcde16)

This characteristic shares the unified 8-byte `history_fragment_header_t` envelope used by the watering history endpoints. Clients send 20-byte `ble_history_request_t` commands and receive responses in 232-byte payload windows (8-byte header + up to 232 bytes of packed records). A "detailed" request simply re-packs hourly retention entries; higher-frequency sensor samples are not exposed. Monthly history remains internal to the firmware.

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `history_fragment_header_t + payload` | 8 B + <=232 B | Unified 8-byte header | Returns the most recent response slice |
| Write | `ble_history_request_t` | 20 B | None | Enforces >=50 ms between accepted commands |
| Notify | `history_fragment_header_t + payload` | 8 B + <=232 B | Unified 8-byte header | Mirrors read buffer for each accepted write |

## Request Format (`ble_history_request_t`)
| Offset | Field | Type | Meaning |
|--------|-------|------|---------|
| 0 | `command` | `uint8_t` | 0x01 detailed, 0x02 hourly, 0x03 daily, 0x04 trends, 0x05 clear |
| 1 | `start_time` | `uint32_t` | Inclusive Unix start (0 -> earliest stored) |
| 5 | `end_time` | `uint32_t` | Inclusive Unix end (0 -> now) |
| 9 | `data_type` | `uint8_t` | 0 detailed, 1 hourly, 2 daily (ignored for 0x04/0x05) |
| 10 | `max_records` | `uint8_t` | 1-100 (0 treated as 100) |
| 11 | `fragment_id` | `uint8_t` | 0-based fragment selector |
| 12 | `reserved[8]` | `uint8_t[8]` | Must be 0 |

## Response Envelope
Every read or notification begins with the shared 8-byte header below, followed by `fragment_size` bytes of packed records (<=232 bytes).

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `data_type` | `uint8_t` | Mirrors handler response; trends set 0x03 |
| 1 | `status` | `uint8_t` | 0 success, non-zero per Status Codes |
| 2 | `entry_count` | `uint16_t` (LE) | Records present in this fragment (<=232 / record size) |
| 4 | `fragment_index` | `uint8_t` | Echo of requested fragment index |
| 5 | `total_fragments` | `uint8_t` | Total fragments available for this cached response |
| 6 | `fragment_size` | `uint8_t` | Payload bytes appended (0 for header-only, max 232) |
| 7 | `reserved` | `uint8_t` | Always 0 |

The characteristic's internal buffer is updated with this header and payload slice so repeated reads deliver identical bytes.

Rate-limited writes respond with `status = 0x07`, `fragment_size = 0`, and no payload. Invalid fragment requests return `status = 0x06` and populate `total_fragments` to help clients resynchronise.

## Data Records
- **Detailed (`data_type=0`)**: 12 B each -> `uint32_t timestamp`, `int16_t temperature_c_x100`, `uint16_t humidity_pct_x100`, `uint32_t pressure_pa` (hPa ×100).<br>- **Hourly (`data_type=1`)**: 16 B each -> `uint32_t timestamp`, temperature avg/min/max (×100), average humidity (×100), average pressure in Pa.<br>- **Daily (`data_type=2`)**: 22 B each -> `uint32_t date_code` (YYYYMMDD as stored), temperature avg/min/max (×100), humidity avg/min/max (×100), average pressure in Pa, `uint16_t sample_count` (defaults to 24 when metadata missing).<br>- **Trends (`command=0x04`)**: single 24 B record summarising last 24 h deltas and slopes; firmware sets `data_type=0x03` and `entry_count=1` when available.

## Status Codes
- `0x00` success
- `0x01` invalid command / data type mismatch
- `0x02` invalid time range
- `0x03` no data
- `0x05` storage/packing failure
- `0x06` invalid fragment id
- `0x07` rate limited (command accepted but response suppressed)

## Rate Limiting
- Firmware enforces >=50 ms between accepted commands. Earlier writes receive a `status=0x07` header (no payload).
- After any accepted request the latest header + payload persist in the attribute value for long reads.

## Command Notes
1. `GET_DETAILED` / `GET_HOURLY` / `GET_DAILY`: honour `data_type`; firmware clamps `max_records` to 100 and fragments in 232 B pages.<br>2. `GET_TRENDS`: ignores `data_type` and `fragment_id`; returns either one trend record (`total_fragments=1`) or `status=0x03` when fewer than two hourly samples exist.<br>3. `CLEAR_HISTORY`: calls `env_history_reset_all()`, responds with metadata only (`entry_count=0`).

## Client Guidance
- Start multi-fragment downloads with `fragment_id=0` and iterate sequentially; any invalid index replies with `status=0x06` including the expected `total_fragments`.
- Treat `fragment_size` as authoritative when copying payload bytes; do not assume `entry_count * record_size` fits without truncation.
- Respect throttle windows; back off for at least 50 ms on status `0x07` before retrying.
- Enable notifications to receive responses proactively; otherwise poll via read after each write.

## Related Interfaces
- `21-environmental-data.md` - live sensor feed referenced by hourly aggregation.
- `bt_environmental_history_handlers.c` - firmware implementation of packing and trend generation.
        const response = await environmentalHistoryChar.readValue();
        return parseEnvironmentalHistoryResponse(response);
        
    } catch (error) {
        console.error('Error requesting environmental history:', error);
        throw error;
    }
}

function parseEnvironmentalHistoryResponse(value) {
    const dv = new DataView(value.buffer);
    const header = {
        dataType: dv.getUint8(0),
        status: dv.getUint8(1),
        recordCount: dv.getUint16(2, true), // LE16 entry count (<=232 / record size)
        fragmentIndex: dv.getUint8(4),
        totalFragments: dv.getUint8(5),
        fragmentSize: dv.getUint8(6)
    };
    if (header.status !== 0) {
        throw new Error(`Environmental history error: 0x${header.status.toString(16)}`);
    }
    const records = [];
    let offset = 8;
    for (let i = 0; i < header.recordCount; i++) {
        switch (header.dataType) {
            case 0: // detailed
                records.push({
                    timestamp: dv.getUint32(offset, true),
                    temperature: dv.getInt16(offset + 4, true) / 100,
                    humidity: dv.getUint16(offset + 6, true) / 100,
                    pressure: dv.getUint32(offset + 8, true)
                });
                offset += 12;
                break;
            case 1: // hourly
                records.push({
                    timestamp: dv.getUint32(offset, true),
                    tempAvg: dv.getInt16(offset + 4, true) / 100,
                    tempMin: dv.getInt16(offset + 6, true) / 100,
                    tempMax: dv.getInt16(offset + 8, true) / 100,
                    humidityAvg: dv.getUint16(offset + 10, true) / 100,
                    pressureAvg: dv.getUint32(offset + 12, true)
                });
                offset += 16;
                break;
            case 2: { // daily
                const dateCode = dv.getUint32(offset, true);
                records.push({
                    dateCode, // YYYYMMDD (not Unix epoch)
                    tempAvg: dv.getInt16(offset + 4, true) / 100,
                    tempMin: dv.getInt16(offset + 6, true) / 100,
                    tempMax: dv.getInt16(offset + 8, true) / 100,
                    humidityAvg: dv.getUint16(offset + 10, true) / 100,
                    humidityMin: dv.getUint16(offset + 12, true) / 100,
                    humidityMax: dv.getUint16(offset + 14, true) / 100,
                    pressureAvg: dv.getUint32(offset + 16, true),
                    sampleCount: dv.getUint16(offset + 20, true)
                });
                offset += 22;
                break;
            }
            case 3: // trends (single record expected)
                records.push({
                    tempChange24h: dv.getInt16(offset, true) / 100,
                    humidityChange24h: dv.getInt16(offset + 2, true) / 100,
                    pressureChange24h: dv.getInt32(offset + 4, true),
                    tempMin24h: dv.getInt16(offset + 8, true) / 100,
                    tempMax24h: dv.getInt16(offset + 10, true) / 100,
                    humidityMin24h: dv.getUint16(offset + 12, true) / 100,
                    humidityMax24h: dv.getUint16(offset + 14, true) / 100,
                    tempSlopePerHr: dv.getInt16(offset + 16, true) / 100,
                    humiditySlopePerHr: dv.getInt16(offset + 18, true) / 100,
                    pressureSlopePerHr: dv.getInt16(offset + 20, true),
                    sampleCount: dv.getUint16(offset + 22, true)
                });
                offset += 24;
                break;
        }
    }
    return { header, records };
}
```

### Getting Hourly Environmental Summaries

```javascript
async function getHourlyEnvironmentalSummaries(days = 7) {
    const endDate = new Date();
    const startDate = new Date(endDate.getTime() - (days * 24 * 60 * 60 * 1000));
    
    try {
        const startTime = Math.floor(startDate.getTime() / 1000);
        const endTime = Math.floor(endDate.getTime() / 1000);
        
        const command = new ArrayBuffer(20);
        const view = new DataView(command);
        
        view.setUint8(0, 0x02);          // GET_HOURLY command
        view.setUint32(1, startTime, true);
        view.setUint32(5, endTime, true);
        view.setUint8(9, 0x01);          // Hourly data type
    view.setUint8(10, 168);          // Max 7 days * 24 hours (clamped to 100 internally)
        view.setUint8(11, 0x00);         // Start fragment
        
        await environmentalHistoryChar.writeValue(command);
        
        // Handle multiple fragments if necessary
        let allRecords = [];
        let currentFragment = 0;
        let totalFragments = 1;
        
        do {
        const response = await environmentalHistoryChar.readValue();
        const { header, records } = parseEnvironmentalHistoryResponse(response);

        allRecords = allRecords.concat(records);
        totalFragments = header.totalFragments;
        currentFragment = header.fragmentIndex + 1;
            
            if (currentFragment < totalFragments) {
                // Request next fragment
                view.setUint8(11, currentFragment);
                await environmentalHistoryChar.writeValue(command);
            }
            
        } while (currentFragment < totalFragments);
        
        return allRecords;
        
    } catch (error) {
        console.error('Error getting hourly summaries:', error);
        throw error;
    }
}
```

### Environmental Trend Analysis

```javascript
async function getEnvironmentalTrends(days = 30) {
    try {
        const hourlyData = await getHourlyEnvironmentalSummaries(days);
        
        // Calculate trends
        const trends = {
            temperature: calculateTrend(hourlyData.map(r => r.tempAvg)),
            humidity: calculateTrend(hourlyData.map(r => r.humidityAvg)),
            pressure: calculateTrend(hourlyData.map(r => r.pressureAvg)),
            dailyPatterns: analyzeDailyPatterns(hourlyData),
            extremes: findExtremes(hourlyData)
        };
        
        return trends;
        
    } catch (error) {
        console.error('Error analyzing trends:', error);
        throw error;
    }
}

function calculateTrend(values) {
    if (values.length < 2) return { slope: 0, direction: 'stable' };
    
    // Simple linear regression
    const n = values.length;
    const sumX = (n * (n - 1)) / 2;
    const sumY = values.reduce((a, b) => a + b, 0);
    const sumXY = values.reduce((sum, y, x) => sum + x * y, 0);
    const sumX2 = (n * (n - 1) * (2 * n - 1)) / 6;
    
    const slope = (n * sumXY - sumX * sumY) / (n * sumX2 - sumX * sumX);
    
    return {
        slope: slope,
        direction: slope > 0.1 ? 'increasing' : 
                  slope < -0.1 ? 'decreasing' : 'stable',
        confidence: Math.min(Math.abs(slope) * 100, 100)
    };
}

function analyzeDailyPatterns(hourlyData) {
    const hourlyAverages = new Array(24).fill(0);
    const hourlyCounts = new Array(24).fill(0);
    
    hourlyData.forEach(record => {
        const hour = new Date(record.timestamp * 1000).getHours();
        hourlyAverages[hour] += record.tempAvg;
        hourlyCounts[hour]++;
    });
    
    // Calculate average for each hour
    for (let i = 0; i < 24; i++) {
        if (hourlyCounts[i] > 0) {
            hourlyAverages[i] /= hourlyCounts[i];
        }
    }
    
    return {
        hourlyAverages: hourlyAverages,
        peakHour: hourlyAverages.indexOf(Math.max(...hourlyAverages)),
        coolestHour: hourlyAverages.indexOf(Math.min(...hourlyAverages)),
        dailyRange: Math.max(...hourlyAverages) - Math.min(...hourlyAverages)
    };
}
```

## Fragmentation Support

Environmental history uses the unified 8-byte header for all responses and notifications.

### Fragment Management

```javascript
async function getAllEnvironmentalHistory(command) {
    let allRecords = [];
    let currentFragment = 0;
    let totalFragments = 1;
    
    // Set initial fragment ID
    const cmdView = new DataView(command);
    cmdView.setUint8(11, 0); // Start with fragment 0
    
    do {
        await environmentalHistoryChar.writeValue(command);
        
        // Wait for response
        const response = await environmentalHistoryChar.readValue();
        const { header, records } = parseEnvironmentalHistoryResponse(response);

        allRecords = allRecords.concat(records);
        totalFragments = header.totalFragments;
        currentFragment = header.fragmentIndex + 1;
        
        // Update fragment ID for next request
        if (currentFragment < totalFragments) {
            cmdView.setUint8(11, currentFragment);
            await new Promise(resolve => setTimeout(resolve, 100)); // Small delay
        }
        
    } while (currentFragment < totalFragments);
    
    return allRecords;
}
```

### Unified Header Usage


JavaScript example:
```javascript
// Enable unified 8B header for Environmental History
const cmd = new ArrayBuffer(20);
const v = new DataView(cmd);
v.setUint8(0, 0x02);               // GET_HOURLY
v.setUint32(1, startTime, true);
v.setUint32(5, endTime, true);
v.setUint8(9, 0x01);               // hourly
v.setUint8(10, 168);               // max records
v.setUint8(11, 0x00);              // fragment 0
// reserved bytes currently unused
await environmentalHistoryChar.writeValue(cmd);
```

## Data Export

### CSV Export

```javascript
function exportEnvironmentalHistoryCSV(records, dataType) {
    let headers, formatRecord;
    
    if (dataType === 0) {
        // Detailed records
        headers = 'Timestamp,Temperature_C,Humidity_Pct,Pressure_Pa,Pressure_hPa';
        formatRecord = (r) => [
            new Date(r.timestamp * 1000).toISOString(),
            r.temperature.toFixed(2),
            r.humidity.toFixed(2),
            r.pressure,
            (r.pressure / 100).toFixed(2)
        ].join(',');
    } else if (dataType === 1) {
        // Hourly records
        headers = 'Hour,Temp_Avg_C,Temp_Min_C,Temp_Max_C,Humidity_Avg_Pct,Pressure_Avg_hPa';
        formatRecord = (r) => [
            new Date(r.timestamp * 1000).toISOString(),
            r.tempAvg.toFixed(2),
            r.tempMin.toFixed(2),
            r.tempMax.toFixed(2),
            r.humidityAvg.toFixed(2),
            (r.pressureAvg / 100).toFixed(2)
        ].join(',');
    } else {
        // Daily records
        const formatDateCode = (code) => {
            const year = Math.floor(code / 10000);
            const month = Math.floor((code % 10000) / 100);
            const day = code % 100;
            return `${year}-${String(month).padStart(2, '0')}-${String(day).padStart(2, '0')}`;
        };
        headers = 'Date,Temp_Avg_C,Temp_Min_C,Temp_Max_C,Humidity_Avg_Pct,Humidity_Min_Pct,Humidity_Max_Pct,Pressure_Avg_hPa,Samples';
        formatRecord = (r) => [
            formatDateCode(r.dateCode),
            r.tempAvg.toFixed(2),
            r.tempMin.toFixed(2),
            r.tempMax.toFixed(2),
            r.humidityAvg.toFixed(2),
            r.humidityMin.toFixed(2),
            r.humidityMax.toFixed(2),
            (r.pressureAvg / 100).toFixed(2),
            r.sampleCount
        ].join(',');
    }
    
    const csv = [headers, ...records.map(formatRecord)].join('\n');
    return csv;
}
```

## Rate Limiting

- Minimum 1s between accepted commands (earlier requests return status 0x07 with header only)
- Maximum records per request: 100 (0 interpreted as 100)
- One active aggregation/packing buffer at a time (new request replaces previous)
- Notifications throttled to >=500 ms interval

## Error Handling

| Error Code | Description | Resolution |
|------------|-------------|------------|
| **0x01** | Invalid command | Check command type (0x01-0x05) |
| **0x02** | Invalid time range | Ensure start_time < end_time |
| **0x03** | No data found | Expand time range or check data collection |
| **(0x04)** | (Not used) | Reserved / not emitted by current firmware |
| **0x05** | Storage error | System storage issue, retry later |
| **0x06** | Invalid fragment | Check fragment_id continuity |
| **0x07** | Rate limited | Respect 1s between commands |

## Python Example

```python
import struct
import asyncio
from datetime import datetime, timedelta

async def get_daily_environmental_summary(client, days=30):
    """Get daily environmental summaries for specified number of days"""
    try:
        env_history_uuid = "12345678-1234-5678-1234-56789abcde16"
        
        # Calculate time range
        end_time = int(datetime.now().timestamp())
        start_time = int((datetime.now() - timedelta(days=days)).timestamp())
        
        # Prepare command for daily summaries
        command = struct.pack('<BIIBBB7x', 
                            0x03,        # GET_DAILY command
                            start_time,  # Start time
                            end_time,    # End time
                            0x02,        # Daily data type
                            days,        # Max records
                            0x00)        # Start fragment
        
        # Send command
        await client.write_gatt_char(env_history_uuid, command)
        await asyncio.sleep(0.5)  # Wait for processing
        
        # Read response
        response = await client.read_gatt_char(env_history_uuid)
        
        # Parse response header
        status, data_type, record_count, fragment_id, total_fragments = struct.unpack('<BBBBB3x', response[:8])
        
        if status != 0:
            raise Exception(f"Environmental history error: {status}")
        
        # Parse daily records
        records = []
        offset = 8
        
        for i in range(record_count):
            day_data = struct.unpack('<IhhhHHHIH', response[offset:offset+22])
            
            record = {
                'date': datetime.fromtimestamp(day_data[0]).strftime('%Y-%m-%d'),
                'temp_avg': day_data[1] / 100.0,
                'temp_min': day_data[2] / 100.0,
                'temp_max': day_data[3] / 100.0,
                'humidity_avg': day_data[4] / 100.0,
                'humidity_min': day_data[5] / 100.0,
                'humidity_max': day_data[6] / 100.0,
                'pressure_avg': day_data[7] / 100.0,  # Convert to hPa
                'sample_count': day_data[8]
            }
            records.append(record)
            offset += 22
        
        return records
        
    except Exception as e:
        print(f"Error getting environmental summary: {e}")
        return []
```

## Related Characteristics

- **[Environmental Data](21-environmental-data.md)** - Real-time environmental readings
- **[Compensation Status](23-compensation-status.md)** - Environmental impact on irrigation
- **[History Management](12-history-management.md)** - General system history management
- **[Growing Environment](14-growing-environment.md)** - Plant environment configuration
- **[Auto Calc Status](15-auto-calc-status.md)** - Environmental data in FAO-56 calculations

## Notes

- Internal retention (current build): hourly ~30 days (720), daily ~12 months (372), monthly (60) - monthly not exposed over BLE.
- "Detailed" view is a compact hourly projection (no extra temporal resolution).
- Daily record timestamp field encodes YYYYMMDD (not Unix); convert if needed.
- Min/max temperature in hourly records presently equal avg (future expansion).
- trend (0x04) request always returns at most one fragment with one record.
- Gas resistance omitted to conserve space.
- All times treated as UTC.
