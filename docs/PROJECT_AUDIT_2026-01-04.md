# AutoWatering – Audit tehnic (2026-01-04)

Scop: notițe pentru viitor (nu implementare), cu probleme observate + îmbunătățiri recomandate + inventar de TODO-uri relevante.

Context:

- Firmware Zephyr 4.3 (nRF52840 / Arduino Nano 33 BLE)
- Arhitectura declarată în `docs/system-architecture.md`

---

## 1) Concluzii rapide (Top 10)

1. **Monolit BLE:** `src/bt_irrigation_service.c` are ~12k linii → mentenanță grea, risc de regresii.
2. **Inițializări duplicate** (același subsistem init din mai multe locuri) → boot instabil, ordine neclară, debugging dificil.
3. **Mutex cu `K_FOREVER` în zone critice** → risc de "hang" dacă un thread moare cu mutex-ul luat.
4. **Variabile globale/statice accesate cross-thread fără atomic/mutex** → risc de race conditions.
5. **Cuplare strânsă NVS ↔ domain** (`nvs_config.c` include/știe prea multe tipuri de domeniu) → testabilitate scăzută.
6. **Error handling inconsistent** (unele init-uri doar print warning și continuă) → sistem poate rula "degradat" fără semnalare structurată.
7. **Cod mort / `__attribute__((unused))`** în `main.c` și BLE → poluare, confuzie.
8. **TODO-uri în cod fără issue/urmărire** → risc să rămână nerezolvate.
9. **Indexare fragilă de atribute BLE (`ATTR_IDX_*`)** → orice inserare cere renumerotare manuală.
10. **Docs lint noise** (multe erori markdown) → scade semnalul/claritatea documentației.

---

## 2) Probleme critice (detalii + dovezi în cod)

### 2.1 Monolit BLE (mentenanță)

- Fișier: `src/bt_irrigation_service.c` (~12,605 linii)
- Simptome:
  - include-uri masive (BLE depinde de aproape toate modulele de domeniu)
  - logică de notificări + fragmentare + handlers + business logic în același loc

Recomandare:

- Separare în module (doar ca direcție, nu implementare aici):
  - `bt_notification_manager.c` (buffer pool + throttling + retry)
  - `bt_*_handlers.c` (watering / rain / env / config / history)

### 2.2 Inițializări duplicate (ordine neclară)

Onboarding init duplicat:

- `src/main.c:404` – `onboarding_state_init()`
- `src/watering.c:349` – `onboarding_state_init()`

Rain sensor/integration/history init duplicat (în 3-4 locuri):

- `src/main.c:434` – `rain_sensor_init()`
- `src/watering.c:364` – `rain_sensor_init()`
- `src/sensor_manager.c:387` – `rain_sensor_init()`
- `src/watering_monitor.c:1180` – `rain_sensor_init()`

- `src/main.c:665` – `rain_integration_init()`
- `src/watering.c:371` – `rain_integration_init()`
- `src/sensor_manager.c:394` – `rain_integration_init()`
- `src/watering_monitor.c:1187` – `rain_integration_init()`

- `src/main.c:656` – `rain_history_init()`
- `src/watering.c:377` – `rain_history_init()`
- `src/sensor_manager.c:402` – `rain_history_init()`
- `src/watering_monitor.c:1194` – `rain_history_init()`

Valve init duplicat:

- `src/main.c:356` – `valve_init()` (via wrapper)
- `src/watering.c:334` – `valve_init()`

Recomandare:

- Alege "single source of truth" pentru init (ideal `main.c`), iar restul modulelor să fie idempotente (guard `static bool initialized`).

### 2.3 Mutex-uri cu `K_FOREVER` în zone sensibile

Exemple (non-exhaustive):

- `src/bt_irrigation_service.c:580` / `608` / `746` / `795` / `845` – `notification_mutex` cu `K_FOREVER`
- `src/bt_irrigation_service.c:2412` / `2436` / `2474` etc – `reset_async_mutex` cu `K_FOREVER`
- `src/watering.c:393` (și alte linii) – `system_state_mutex` cu `K_FOREVER`
- `src/watering_log.c:38` / `59` / `79` / `130` – `log_mutex` cu `K_FOREVER`
- `src/rain_sensor.c` / `src/rain_integration.c` / `src/rain_history.c` – multe lock-uri `K_FOREVER`

Notă pozitivă:

- `src/watering_tasks.c` are deja comentarii și lock-uri cu timeout (ex: `src/watering_tasks.c:514`), deci există precedent în repo.

Recomandare:

- În special în BLE handlers: înlocuire cu timeout + fallback (log + fail-fast) în loc de hang.

---

## 3) Inventar TODO-uri concrete în cod (candidați de issue / trebuie legate de issue-uri)

### BLE / status tracking

- `src/bt_irrigation_service.c:832` – "Implement automatic fragmentation for generic notifications"
- `src/bt_irrigation_service.c:11569-11570` – "Add timestamp tracking to watering module" (rain/temp)
- `src/bt_irrigation_service.c:11741-11742` – "Add timestamp tracking" (variantă)
- `src/bt_irrigation_service.c:11782-11783` – "Add timestamp tracking" (variantă)
- `src/bt_irrigation_service.c:11886` – "Add remaining time tracking"

### Reset / wipe

- `src/reset_controller.c:150` – "Add verification logic (check NVS keys absent)"

Recomandare practică:

- Fiecare TODO de mai sus ar trebui:
  1) să aibă issue dedicat (sau subtask într-un issue),
  2) să fie legat prin referință (ex: `TODO(#123): ...`) ca să nu se piardă.

---

## 4) Observații despre GitHub issues (multe sunt, de fapt, TODO-uri de produs)

În documentație există o listă de roadmap issues care sunt, în esență, "TODO-uri" la nivel de proiect (nu e rău, doar trebuie conectate la cod):

- `docs/FEATURES_FULL.md:15-19` – Issues #1-#14 (batch B0/B1-B2/B4/B5/B7)
- `docs/FEATURES.md:176-181` – aceeași listă (repetată)

Recomandare:

- Pentru fiecare issue de roadmap:
  - adaugă în descriere link către TODO-urile din cod (fișier + line), dacă există;
  - sau, dacă nu există TODO în cod, definește "Definition of Done" clar.

---

## 5) Alte îmbunătățiri recomandate (fără cod acum)

### 5.1 Decuplare NVS de domeniu

- Motiv: `src/nvs_config.c` include tipuri și structuri de domeniu (watering, onboarding etc.), ceea ce crește cuplarea.
- Direcție: strat de "persistence adapters" / API de serializare pe module.

### 5.2 Standardizare error handling

- Propunere: "system degraded flags" (bitmask) care marchează subsisteme eșuate la init; expus prin BLE pentru diagnoză.

### 5.3 Reducere cod mort

- `src/main.c` conține multe funcții `__attribute__((unused))` (posibil rămase din debugging).
- Recomandare: ori șterse, ori protejate prin Kconfig `CONFIG_AUTOWATERING_DIAGNOSTICS`.

### 5.4 Curățare documentație (markdown lint)

- Sunt multe erori MD032/MD034/MD022 în docs (ex: `docs/FEATURES_FULL.md`).
- Recomandare: normalizează list spacing + transformă URL-urile în link-uri `[text](url)`.

---

## 6) Backlog sugerat (ordine de atac)

1. Eliminare init duplicat + idempotency guards (stabilitate boot)
2. Timeout-uri în loc de `K_FOREVER` (prevenire hang)
3. Split `bt_irrigation_service.c` (mentenanță)
4. Atomic/mutex pentru variabile cross-thread (race)
5. Decuplare NVS ↔ domain (testabilitate)
6. Cleanup docs / cod mort (hygiene)
