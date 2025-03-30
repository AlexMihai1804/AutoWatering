#ifndef BT_IRRIGATION_SERVICE_H
#define BT_IRRIGATION_SERVICE_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include "watering.h"
#include "rtc.h"  // Adăugăm header-ul RTC pentru sincronizarea timpului

/**
 * @brief Initialize the Bluetooth irrigation service
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_service_init(void);

/**
 * @brief Update valve status via Bluetooth
 * 
 * @param channel_id Channel ID
 * @param state Valve state (0=off, 1=on)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_valve_status_update(uint8_t channel_id, bool state);

/**
 * @brief Update flow data via Bluetooth
 * 
 * @param pulses Current pulse count
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_flow_update(uint32_t pulses);

/**
 * @brief Update system status via Bluetooth
 * 
 * @param status Current system status
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_system_status_update(watering_status_t status);

/**
 * @brief Update channel configuration via Bluetooth
 * 
 * @param channel_id Channel ID
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_channel_config_update(uint8_t channel_id);

/**
 * @brief Update pending tasks count via Bluetooth
 * 
 * @param count Number of pending tasks
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_queue_status_update(uint8_t count);

/**
 * @brief Update system configuration via Bluetooth
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_config_update(void);

/**
 * @brief Update statistics via Bluetooth
 * 
 * @param channel_id Channel ID
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_statistics_update(uint8_t channel_id);

/**
 * @brief Actualizează timpul RTC prin Bluetooth
 * 
 * @param datetime Structura cu noua dată și oră
 * @return 0 pe succes, cod de eroare negativ pe eșec
 */
int bt_irrigation_rtc_update(rtc_datetime_t *datetime);

/**
 * @brief Notifică clientul Bluetooth despre o alarmă
 * 
 * @param alarm_code Codul alarmei
 * @param alarm_data Date suplimentare despre alarmă
 * @return 0 pe succes, cod de eroare negativ pe eșec
 */
int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data);

/**
 * @brief Începe o sesiune de calibrare a senzorului de debit
 * 
 * @param start 1 pentru a începe, 0 pentru a opri
 * @param volume_ml Volumul de apă în ml pentru calibrare (la oprire)
 * @return 0 pe succes, cod de eroare negativ pe eșec
 */
int bt_irrigation_start_flow_calibration(uint8_t start, uint32_t volume_ml);

/**
 * @brief Actualizează istoricul de irigare
 * 
 * @param channel_id ID-ul canalului
 * @param entry_index Index-ul intrării în istoric (0 = cea mai recentă)
 * @return 0 pe succes, cod de eroare negativ pe eșec
 */
int bt_irrigation_history_update(uint8_t channel_id, uint8_t entry_index);

/**
 * @brief Actualizează diagnosticele sistemului
 * 
 * @return 0 pe succes, cod de eroare negativ pe eșec
 */
int bt_irrigation_diagnostics_update(void);

/**
 * @brief Execută o comandă directă asupra unui canal
 * 
 * @param channel_id ID-ul canalului
 * @param command Codul comenzii (0=închide, 1=deschide, 2=puls)
 * @param param Parametru suplimentar (ex: durata pulsului în secunde)
 * @return 0 pe succes, cod de eroare negativ pe eșec
 */
int bt_irrigation_direct_command(uint8_t channel_id, uint8_t command, uint16_t param);

#endif // BT_IRRIGATION_SERVICE_H
