# Vibrazione, sensori e slider magnetico sotto XNU su `andromeda`: hardware, lavori altrui, iOS e un piano

Ricerca del 6 novembre 2026 (ROADMAP, voce 13), solo lettura: nessun comando sul telefono, nessun codice dei repository
esterni eseguito, nessun file del progetto toccato oltre a questo. Le fonti principali sono:

- il device tree attivo del telefono (`phone/<seriale>/live-fdt.dts`, fuori da git: qui solo i valori che servono);
- le proprietà Android lette dal telefono (`phone/<seriale>/info/getprop.txt`);
- il kernel Xiaomi `andromeda-p-oss` (commit `74347f55`, clone parziale);
- le configurazioni dei sensori dei blob vendor (PixelExperience-Blobs `vendor_xiaomi_andromeda`, ramo `fourteen`,
  commit `63c49c5e`);
- i log dei nostri avvii (`phone/logs/`, traccia IOKit di backboardd);
- le stringhe del sistema iOS 16.7.16 che costruiamo in locale (`out/andromeda/root/ios16-root.img`: `locationd` e
  dyld shared cache, letti con `strings` e mai copiati nel repository);
- i progetti elencati in [Fonti](#9-fonti).

Legenda della certezza (la stessa di [ricarica.md](ricarica.md)):

- **✔** letto nel sorgente o nei dati del telefono, oppure riportato come visto funzionare dagli autori sul loro hardware;
- **~** dedotto: coerente con più indizi, non ancora visto su `andromeda`;
- **?** da verificare.

## Sommario

- **Vibrazione: il pezzo hardware più semplice dopo lo slider.**
  - L'hardware ✔: un motore lineare (LRA) pilotato dal blocco aptico del **PM8150B, SPMI SID 3, `0xC000`**, con il
    driver Qualcomm `qti-haptics.c`. Non ci sono chip esterni né alimentazioni da accendere.
  - Una vibrazione costante richiede circa otto scritture di configurazione, due di avvio e due di arresto, tutte sotto
    `0xD0`, quindi senza `SEC_ACCESS`.
  - Valori del telefono: il DT attivo dice **2800 mV / 5102 µs**, il kernel Xiaomi **2552 mV / 4878 µs**. Per le prime
    prove proponiamo **al massimo 2552 mV**.
  - HakiOS `vayu` (POCO X3 Pro, stesso PM8150B) ha già letto lo stesso blocco sul suo telefono ✔. Il suo codice però non
    ha licenza: ne usiamo solo i fatti.
- **Slider: due sensori di Hall su GPIO del processore principale, il compito più facile.**
  - Sono il **GPIO 42 (tile NORTH)** e il **GPIO 114 (SOUTH)** ✔, letti a polling dal bit `IN` del TLMM, come fa già il
    touch.
  - Il significato degli stati viene dal kernel e dal KeyHandler di Inkypen79 ~:
    - (42, 114) = (0, 1): slider **aperto**;
    - (1, 0): **chiuso**;
    - qualunque altra combinazione: in movimento.
  - iOS non ha uno slider: l'evento va inventato (sezione [3.5](#35-slider-che-evento-per-ios)).
- **Sensori: sono tutti dietro lo SLPI (Qualcomm Sensor Core), nessuno è sul processore principale ✔.**
  - I chip: accelerometro e giroscopio LSM6DSO (SPI), magnetometro AK0991x, barometro BMP285, luce TSL2584, SAR
    ADUX1050.
  - I bus I2C del processore principale portano solo audio, NFC, caricatore e touch.
  - La catena per arrivarci:
    1. avvio dello SLPI via TrustZone (PAS 12);
    2. GLINK → QRTR → QMI 0x190 con messaggi protobuf;
    3. un server FastRPC che passa allo SLPI la sua configurazione.
  - **Nessuno ce l'ha funzionante su SM8150**: non Linux mainline (WIP), non HakiOS `vayu` (parziale, non verificato).
    Difficoltà molto alta, mesi di lavoro.
- **Prossimità: non c'è un sensore.** È l'ultrasuono di Elliptic Labs: auricolare, microfono e un algoritmo nell'ADSP ✔.
  - Dipende dall'audio ([audio.md](audio.md), fase A10).
  - iOS la usa soprattutto in chiamata, che qui non c'è ancora.
- **Luce ambientale:** la TSL2584 sta dietro lo SLPI. La luminosità automatica richiede anche il controllo della
  luminosità (`andrbl`, in lavorazione).
- **Il lato iOS: ruotare lo schermo è realistico, CoreMotion completo no.**
  - Per ruotare le app basta un servizio HID "accelerometro" (pagina `0xFF00`, usage 3, eventi di tipo 13), che
    backboardd legge ✔.
  - Scion (HakiOS sul Galaxy S9) l'ha visto funzionare il 3 novembre 2026, insieme alla prossimità ✔.
  - La base nel kernel c'è già: `SweetHID` di ios-sweet sa costruire eventi accelerometro, luce e prossimità ✔.
  - Le app CoreMotion passano invece da `locationd`, che parla il protocollo del coprocessore di movimento Apple (SPU):
    emularlo non è realistico.
  - Le vibrazioni di iOS escono come audio sul dispositivo "Actuator": HakiOS `redwood` vibra così ✔.
- **Primi passi, i più piccoli:**
  1. una sonda in sola lettura per Hall, blocco aptico e stato dello SLPI, in un solo avvio;
  2. un impulso di prova del motore, con l'utente presente;
  3. lo slider come evento HID;
  4. poi, separato e lungo, l'esperimento di avvio dello SLPI.

## 1. L'hardware

### 1.1 Vibrazione: LRA sul PM8150B

Il nodo è `qcom,haptics@c000` sotto `qcom,pm8150b@3` ✔ FDT:

- `reg <0xc000 0x100>`, interruzioni `hap-sc-irq` e `hap-play-irq`;
- `qcom,actuator-type = "lra"`, forma d'onda `sine`, risonanza automatica `qwd`;
- `qcom,lra-allow-variable-play-rate`;
- **nessun `vdd-supply`**.

Su Android il driver è `qti-haptics.c` (`CONFIG_INPUT_QTI_HAPTICS=y` nel defconfig `andromeda_user_defconfig`,
compatible `qcom,haptics`), che espone un dispositivo input force-feedback. Il servizio `vendor.qti.vibrator` gira sul
telefono ✔ (getprop).

| Proprietà | DT attivo (EvolutionX, kernel Inkypen79) | Xiaomi `andromeda-p-oss` (`andromeda-sm8150.dtsi:591-593`) | base `pm8150b.dtsi:493-494` |
|---|---|---|---|
| `qcom,vmax-mv` | 2800 (`0xaf0`) | **2552** | 3400 |
| `qcom,play-rate-us` (periodo di risonanza) | 5102 (`0x13ee`, ~196 Hz) | **4878** (~205 Hz) | 6667 |
| `qcom,wf-vmax-mv` degli effetti | 3600 | 2552 | 3600 |

Il kernel Android del telefono usa 2800 mV. La regola del progetto ("mai oltre il kernel Android") ammette quindi fino a
2800. Per le prime prove proponiamo il valore più basso dei due, **2552 mV**, quello che Xiaomi ha scelto per questo
motore; 2800 resta il limite assoluto. Il periodo conta poco, perché in modalità VMAX la risonanza automatica (QWD) lo
insegue da sola ~.

**Registri** (`qti-haptics.c:78-176`, offset da `0xC000` sul SID 3) ✔:

| Offset | Nome | Uso |
|---|---|---|
| `0x04`/`0x05` | TYPE/SUBTYPE | identità (vedi HakiOS sotto) |
| `0x0A` | STATUS1 | bit 3 `SC_DET` (cortocircuito), bit 1 `BUSY` |
| `0x46` | EN_CTL1 | bit 7: modulo acceso |
| `0x48` | EN_CTL2 | bit 1 standby automatico, bit 0 freno |
| `0x4A` | EN_CTL3 | ponte H, PWM, limitazione di corrente, DAC |
| `0x4B` | AUTO_RES_CTRL | bit 7: risonanza automatica |
| `0x4C` / `0x4D` | CFG1 / CFG2 | 0 = LRA / 0 = sinusoide |
| `0x4E` | SEL | bit 5:4 sorgente: 0 VMAX (costante), 1 buffer, 2 audio, 3 PWM |
| `0x4F` | AUTO_RES_CFG | bit 7 QWD, bit 3 `CAL_EOP` |
| `0x51` | VMAX_CFG | bit 5:1 = mV / 116 |
| `0x52` | ILIM_CFG | `0x02` = 1000 mA |
| `0x54`/`0x55` | RATE_CFG1/2 | periodo / 5 µs, 12 bit |
| `0x59` | SC_CLR | bit 0: azzera il cortocircuito |
| `0x5A` | ZX_CFG | bit 2:0 antirimbalzo dello zero-crossing (3 = 80 µs) |
| `0x5C`, `0x5E`, `0x60-0x67` | BRAKE, WF_REPEAT, WF_S1-S8 | effetti brevi in buffer (non servono per la prima prova) |
| `0x70` | PLAY | bit 7: suona |
| `0xD0` | SEC_ACCESS | le scritture **sopra `0xD0`** richiedono `0xA5` qui: **non le facciamo mai** |

**Una vibrazione costante** (da 20 ms in su, ramo VMAX di `qti_haptics_load_constant_waveform()` e
`qti_haptics_playback()`, righe 621-680 e 940-990), preceduta da `qti_haptics_hw_init()` (righe 1043-1150) ✔:

1. **Configurazione.**
   - CFG1 = `0x00`;
   - ILIM_CFG = `0x02`;
   - EN_CTL3 = `0xFD`;
   - ZX_CFG[2:0] = 3;
   - RATE = periodo/5: 4878 µs dà `0x3CF` (`0xCF`, `0x03`);
   - CFG2 = `0x00`;
   - AUTO_RES_CFG (maschera `0x8F`) = `0x88`;
   - AUTO_RES_CTRL = `0xEC`.
2. **Ampiezza e sorgente.**
   - VMAX_CFG (maschera `0x3E`) = (mV/116) << 1: 2552 mV dà 22, cioè `0x2C`;
   - AUTO_RES_CTRL bit 7 a 1;
   - SEL (maschera `0x31`) = `0x00`, sorgente VMAX.
3. **Avvio.** EN_CTL1 = `0x80`, poi PLAY = `0x80`.
4. **Arresto.** Dopo la durata PLAY = `0x00`; un periodo + 1 ms dopo, EN_CTL1 = `0x00`.

Sul cortocircuito il driver azzera `SC_CLR`. Se ne arrivano più di 5 in un secondo spegne il modulo per sempre
(righe 768-830).

**Cosa ha già visto qualcun altro sullo stesso PMIC** ✔, nel commento di testa di `vayu_haptic.c` di HakiOS `vayu`,
corsa "164539", in sola lettura sul POCO X3 Pro:

- il canale SPMI del blocco è id `0x3c0` (SID 3, periferica `0xc0`), con **proprietario 0, cioè il processore
  principale**;
- TYPE `0x15`, SUBTYPE `0x09`;
- i registri lasciati da Android: VMAX_CFG `0x22`, RATE `0x3e8` = 5000 µs, EN_CTL3 `0xff`.

Su `andromeda` ci aspettiamo lo stesso ~. Lo dirà la fase [V0](#v-vibrazione).

### 1.2 Sensori: tutti dietro lo SLPI

**Lo SLPI e la sua infrastruttura** ✔ FDT:

| Cosa | Valore |
|---|---|
| Caricamento | `pil_ssc: qcom,ssc@5c00000`, `qcom,pil-tz-generic`, **PAS id `0x0c`**, firmware `"slpi"`, voti `vdd_cx`/`vdd_mx`, `qcom,signal-aop` |
| Memoria | `pil_slpi_region` **`0x97b00000`, 0x1400000** (`no-map`), dentro la zona `PIL_REGION` (`0x8B700000-0x9AB00000`, riservata) della mappa UEFI di Aloha |
| Avvio da Android | `qcom,msm-ssc-sensors` → `drivers/sensors/sensors_ssc.c` (Xiaomi/LF, GPL-2.0): `subsystem_get_with_fwname("slpi")` quando init scrive su `/sys/kernel/boot_slpi/boot` |
| Canale | GLINK `dsps` (remote-pid 3, trasporto SMEM, doorbell APCS bit 24), canali `IPCRTR` (QRTR), `fastrpcglink-apps-dsp`, `glink_ssr` |
| SMP2P | `qcom,smp2p-dsps`: SMEM 481/430, APCS bit 26 |
| Bus dei sensori | QUP SSC SE20-23 (`0x2680000`…), wrapper `qupv3_3` (`0x26c0000`), clock SSC (`qcom,scc@2b10000`), TLMM dello SLPI (`0x2b40000`, 14 pin): **tutti `disabled`** dal lato del processore principale |

Su Android girano `vendor.sensors` (`sscrpcd`), `vendor.sensors.qti`, `vendor.sensors-hal-1-0` e
`vendor.adsprpcd_sensorspd` ✔ (getprop). Fra i blob ci sono `sensors.ssc.so`, `libsnsapi.so`, `libsns_*` e
`libsns_registry_skel.so` ✔.

I sensori non hanno nodi nel DT: li descrive la configurazione JSON che lo SLPI legge dall'Android
(`/vendor/etc/sensors/config/sm8150_*.json`, filtrata per `hw_platform` "ANDROMEDA" e `soc_id` 339) ✔:

| Sensore | Chip | Bus SSC (`bus_type`/`bus_instance`) | Indirizzo | IRQ (`dri_irq_num`) | Alimentazioni | Note |
|---|---|---|---|---|---|---|
| Accelerometro + giroscopio | ST **LSM6DSO** | SPI, istanza 2, fino a 3,3 MHz | — | 132 | `sensor_vddio` | ODR 20-500 Hz, assi `+x +y +z` |
| Magnetometro | AKM **AK0991x** | I2C 1, 400 kHz | `0x0C` | 119 | `sensor_vddio` | assi `x=-y`, `y=+x`, `z=+z` |
| Barometro | Bosch **BMP285** | I2C 3 | `0x77` | — | `sensor_vddio` | |
| Luce ambientale | ams **TSL2584** | I2C 3 | `0x49` | 117 | `sensor_vdd` + `sensor_vddio` | solo ALS, niente prossimità (~, datasheet) |
| SAR (antenne) | ADI **ADUX1050** | I2C 3 | `0x2C` | 120 | `sensor_vdd_2` + `sensor_vddio` | serve solo al modem |
| Umidità/temperatura | Sensirion SHTW2 | ? | ? | — | — | c'è solo un `shtw2_0.json` generico (ANDROMEDA tra le piattaforme): presenza ? |

Le alimentazioni (`/pmic/client/sensor_vdd*`) e i pin (`sm8150_power_0.json`: GPIO 0-7 e 12-13, numerati sul TLMM dello
SLPI ~) sono gestiti dallo SLPI stesso, non dal processore principale ~.

**Nessun sensore sui bus del processore principale** ✔. I QUP attivi portano solo questi dispositivi:

- SE4: `fsa4480`, `aqt1000`, `smb1390` e `smb1355`;
- SE8: `tas2557`;
- SE9: `nq` (NFC);
- SE17: `fts` (touch).

**Prossimità: ultrasuoni Elliptic Labs, nell'ADSP**, non nell'SSC ✔:

- fra i blob vendor ci sono `elliptic_sensor.xml`, `sensors.elliptic.so`, `libultrasound.so` e `ultrasound.wav`;
- il kernel ha `audio-kernel/dsp/elliptic/` ("Copyright Elliptic Labs", GPL), con porte AFE ultrasoniche RX/TX e un
  parametro `SENSORHUB`;
- il comunicato di Elliptic Labs dice che INNER BEAUTY III sostituisce il sensore IR sul Mi MIX 3 5G;
- [audio.md](audio.md) §1.1 riporta `ro.vendor.audio.us.proximity = true`, i percorsi auricolare + ADC1 e la
  calibrazione in `persist`.

### 1.3 Slider: due sensori di Hall

Nel DT non c'è nessun nodo per i Hall. Il driver Xiaomi `drivers/halls/halls.c` (GPL v2, `CONFIG_HALLS=y`) fissa nel
codice `DEF_HALL_GPIO_1 = 42` e `DEF_HALL_GPIO_2 = 114` ✔:

- li configura come ingressi, con interruzione su entrambi i fronti;
- ricava lo stato: (h1, h2) = (0, 1) → 1, (1, 0) → 0, altro → 2;
- lo passa a user space con `/dev/halls`, dove lo legge il blob `sensors.halls.so`.

Lo stesso fatto è già annotato in [fotocamere.md](fotocamere.md) §1.5.

**Che cosa vuol dire ogni stato.** Il kernel di Inkypen79 per EvolutionX, quello che gira oggi sul telefono, usa la
riscrittura di LineageOS (Demon000 / Cosmin Tanislav, commit `5fb68e49`, 2019-05-20, scritta per il Mi MIX 3 `perseus`)
✔:

- legge gli stessi due GPIO e li trasforma in tasti input;
- 42=1 e 114=0 → `STATE_SLIDER_UP` → tasto 594;
- 42=0 e 114=1 → `STATE_SLIDER_DOWN` → tasto 595.

Il `KeyHandler` di `device_xiaomi_andromeda` (Apache-2.0) suona "close" sul 594, e sul 595 suona "open" e apre la
fotocamera ✔. Quindi lo stato 1 di Xiaomi (42=0, 114=1) è lo **slider aperto** (fotocamera frontale scoperta) e lo
stato 0 è **chiuso** ~. Il codice lo dice con certezza; il significato fisico è dedotto, ma con alta confidenza.

| Pin | Tile TLMM | `GPIO_CFG` (fisico) | `GPIO_IN_OUT` | Riattivazione PDC | Altro uso possibile del pin (disattivato nel DT) |
|---|---|---|---|---|---|
| 42 (Hall 1) | NORTH `0x03900000` | `0x0392A000` | `0x0392A004` (bit 0 = ingresso) | 48 | `qupv3_se4_2uart` (GPIO 41/42) |
| 114 (Hall 2) | SOUTH `0x03D00000` | `0x03D72000` | `0x03D72004` | 82 | `qupv3_se1_i2c` (GPIO 114/115) |

Gli indirizzi seguono `pinctrl-sm8150.c` (registri a `0x1000 * id` dentro il tile) e le stesse basi di
`xnu/cepheus_touch.c` ✔. Resta da confermare che i numeri del driver Xiaomi siano quelli del TLMM ~: vale se il gpiochip
del TLMM parte da 0, ed è la regola sui kernel Qualcomm 4.14. Lo confermerà la sonda, se i valori cambiano quando si
muove lo slider.

## 2. Lavori precedenti (XNU/iOS su hardware non Apple)

| Progetto | Cosa fa | Prova | Licenza | Cosa ci serve |
|---|---|---|---|---|
| **HakiOS `vayu`** (nomorecoolnicknames, [`hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) `3e7be13`, POCO X3 Pro, SM8150) | **Vibrazione** (`vayu_haptic.c`, 1296 righe): stesso blocco PM8150B; tre gradini `vayu_haptic=1/2/3` (sola lettura; configurazione e due impulsi; motore per iOS via `/dev/hakios-haptic`) | gradino 1 letto sul telefono (corsa 164539) ✔; i parametri del gradino 3 sono stati ritoccati dopo la corsa 132828 ~; nel README "built, not verified on hardware" | **nessuna** | fatti e metodo (elenco chiuso di 13 registri, rilettura, `SC_DET`, spegnimento anche sul percorso del panic): codice da non copiare |
| idem | **SLPI e sensori** (`vayu_slpi.c`, `vayu_pas.c`, `vayu_ssc.c` 2286 righe, `vayu_fastrpc.c`) | non verificato ✔ README; un commento cita una corsa in cui l'SSC rispondeva con un errore a ogni richiesta finché il formato del TLV non è stato corretto ~ | **nessuna** | i dettagli nella sezione [2.1](#21-hakios-vayu-la-strada-dello-slpi-percorsa-a-metà) |
| idem | **Lato iOS** (`motion.c`): `libhakios.motion.dylib` iniettata in backboardd; servizi HID virtuali (`IOHIDVirtualServiceClientCreateWithCallbacks`) `0xFF00`/3 per `BKOrientationManager` e `0xFF0C`/3 per CoreMotion; `IOHIDEventCreateAccelerometerEvent` in g | non verificato ? | **nessuna** | lo schema |
| **HakiOS `redwood`** (OOSEX GANG, [`hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) `63fc2b4`, POCO X5 Pro) | `hakios.haptic.c` (Awinic AW86927 via I2C); plug-in CoreAudio HAL `audio_plugin.c` con il dispositivo **"Actuator"** (catena `d22_actuator_general`) i cui campioni vanno a `/dev/hakios-haptic`; in `inject.c` un gancio (spento) su `-[BLControl waitForALSArrival]` | "Cameras, vibration: yes" su `redwood` ✔ (`docs/index.md`) | APSL-2.0 (`drivers/`), **MIT** (`userland/src`) | il **percorso iOS della vibrazione**: riusabile con attribuzione |
| **Scion** ([`Scion-Project/scion-kernel`](https://github.com/Scion-Project/scion-kernel), vcvkk, dylanneve1, shimoku74 e altri; Galaxy S9, fork di HakiOS) | `userland/src/inject.c`: servizi HID virtuali in backboardd per accelerometro `0xFF00`/3 (tipo 13), luce `0xFF00`/4 (tipo 12, lux), prossimità `0xFF00`/8 (tipo 14), giroscopio `0xFF00`/9, bussola `0xFF00`/`0x0A`; orientamento ricavato dall'accelerometro | "backboardd took face up, portrait and landscape and the proximity sensor, 3 November" ✔ (`scion-starlte.md`); vibrazione "felt" il 1° novembre (MAX77705) ✔ | MIT (userland), APSL-2.0 (kernel) | la prova più forte che **la rotazione via HID funziona su iOS 16** |
| **ios-sweet** (dipa-studio, nostra base) | patch 0058: `IOHIDEventService` minimo nel kernel, `SweetHIDButtons` e `SweetHIDTouch`, `IOHIDEvent.cpp` di IOHIDFamily-1915.140.3 con `accelerometerEvent`, `gyroEvent`, `compassEvent`, `ambientLightSensorEvent`, `proximityEventWithProbability`, `buttonEvent` | backboardd apre i nostri `SweetHIDButtons`/`SweetHIDTouch` (`open … type 1702065507` = `'esuc'`) ✔ nei nostri log | MIT + APSL (file IOHIDFamily) | **la base per sensori e slider nel kernel**, senza iniezioni |
| **ios-flame** (Nick Wylynko, Pixel 4, SM8150) | niente su sensori e vibrazione (in `HANDOFF.md` sono elencati come lavoro futuro); avverte che `pil_slpi_region` è protetta da XPU e va mappata Device-GRE, mai Normal (patch 0084) | ✔ | MIT | l'avvertenza sulla memoria dello SLPI |
| **UniXNU** | nessun codice per i sensori. `check-original-backboard-haptic-wait.py` (bluseliu50) mostra che il backboardd di iPadOS 27 aspetta per sempre un gruppo del controller aptico (`-[BKButtonHapticsController applyDefinitions:]`), "senza dire che sia la causa dello stallo". OnePlus 13T (IssacL04, `PaganiHaptics`, `PaganiSensors`): sperimentali, non verificati | ✔ | BSD-2-Clause-Patent | un indizio da tenere a mente (sezione [3.1](#31-vibrazione)) |
| **ios-apollo** (aTechGeekDev, SM8250) | ha ripreso la vibrazione AW8697 di HakiOS | trovato con una ricerca sul web, codice non letto da noi ? | MIT | — |
| **Linux / postmarketOS** | libssc (Dylan Van Assche), hexagonrpcd (Richard Acayan), `drv-ssc-*` in iio-sensor-proxy 3.9, `qcom_q6v5_pas` (`qcom,sm8150-slpi-pas`), `fastrpc.c`, QRTR | SSC funzionante su SDM845 (SHIFT6mq, OnePlus 6/6T); **su SM8150 "WIP"**, nessun dispositivo; WIP di Andrew Gigena (`2a070aeb`, ramo `andrew/6.16-sensors` di sm8150-mainline): nodo SLPI a `0x5c00000`, label glink `slpi`, VMID | GPL-3.0+ (libssc, hexagonrpc, iio-sensor-proxy), GPL-2.0 (kernel), LGPL-2.1+ (libqmi), BSD-3 (`qrtr-ns`) | solo documentazione del protocollo |
| **Linux, aptica** | serie `qcom-pmi8998-haptics.c` di Caleb Connolly (v7, 2022): mai integrata | ✔ patchwork | GPL-2.0 | conferma della sequenza |
| **Emulatori** | QEMUAppleSilicon emula l'AOP con endpoint HID SPU (AGPL-3.0: da evitare); qemu-t8030 nessuno; qemu-ios vecchi stub I2C | ✔ | varie | niente di riusabile |

### 2.1 HakiOS `vayu`: la strada dello SLPI percorsa a metà

Il codice non ha licenza: qui riassumiamo solo i fatti, con i riferimenti per chi li verifica.

- **Avvio** (`vayu_slpi.c:122-134`, `vayu_pas.c`) ✔:
  - PAS id 12; carve-out `0x97b00000`/`0x1400000`; SMP2P 481/430, bit APCS 26 (SMP2P) e 24 (GLINK). **Sono gli stessi
    valori del DT di `andromeda`** ✔. Solo il nome del firmware cambia: `slpi_vy` per loro, `slpi` per noi.
  - Prima i controlli `IS_CALL_AVAILABLE`/`PAS_IS_SUPPORTED`, poi `INIT_IMAGE` (1), `MEM_SETUP` (2), la copia dei
    segmenti con rilettura e infine `AUTH_AND_RESET` (5).
  - Prima del reset mandano un messaggio QMP all'AOP (`load_state slpi on`) e votano RPMh `lcx`/`lmx` al livello 384.
  - Nessuna interruzione: SMP2P e FIFO GLINK sono lette a polling ogni 1 ms.
- **Client SSC** ✔:
  - GLINK `IPCRTR` → QRTR → servizio QMI **0x190**, messaggio `0x0020` (TLV 0x01 = lunghezza u16 + protobuf); le
    indicazioni arrivano come `0x0021`/`0x0022`.
  - Lookup dei SUID con id 512/768 sul SUID `0xABAB…AB`, poi attributi (1/128), stream (513) e campioni (1025).
  - Chiedono `accel`, `gyro`, `mag`, `ambient_light`, `proximity` e `sensor_temperature`. Lo stream esiste solo per accel
    e gyro, a 50 Hz; i dati escono da `/dev/vayu-motion`.
- **Registry** ✔: lo SLPI legge la sua configurazione dall'Android tramite FastRPC inverso.
  - Il loro kernel fa da `apps_std` in sola lettura: serve un pacchetto `sensors.pak` e un `soc0` sintetico
    (`soc_id 339`).
  - Mette in bypass l'SMMU per gli SID `0x5a1-0x5a3`.
  - Il generatore del pacchetto non è pubblicato. Che senza registry lo SLPI non crei i driver è una loro ipotesi
    ("H-noreg").

Cosa vale anche per noi: SoC, PAS, memoria, SMP2P e GLINK sono identici; il resto è il protocollo. Cosa è solo loro: i
chip, il contenuto del registry, le matrici di montaggio.

## 3. Il lato iOS (16.7.16, identità iPhone X `D22AP`)

### 3.1 Vibrazione

- **Come vibra un iPhone X.** Il Taptic Engine è un'**uscita audio**.
  - `VirtualAudio.plugin` si aspetta, accanto a `Speaker`, `Hawking` e `Codec`, un dispositivo `Actuator`, su cui
    CoreHaptics/AVHapticPlayer, i feedback di UIKit e la vibrazione della suoneria suonano forme d'onda
    ([audio.md](audio.md) §3) ✔. Su HakiOS quel dispositivo lo pubblica il loro plug-in HAL.
  - Nel nostro sistema ci sono le tarature `Library/Audio/Tunings/D22/Haptics` e `D22/ButtonHaptics` ✔. Il percorso di
    HakiOS (dispositivo `Actuator` del plug-in → nodo del kernel → motore) funziona su `redwood` ✔.
  - Per noi: il plug-in della fase A0 di [audio.md](audio.md), con l'uscita `Actuator` collegata a un nodo
    `/dev/andromeda-haptic` e al driver del PM8150B.
- **Una vibrazione "semplice" alternativa?**
  - Nella dyld shared cache ci sono messaggi come "Using New Unified Haptic/Vibe Engine", "Device will not vibe as
    vibrator is not initialized" e "no vibrator support" ✔: esiste un ramo per dispositivi senza Taptic.
  - Che cosa lo attivi (un'altra identità del dispositivo? un servizio IOKit?) non lo sappiamo ?. Con l'identità D22 la
    strada naturale resta `Actuator`.
- **Cosa cerca già backboardd sul telefono** ✔ (traccia IOKit dei nostri avvii, `phone/logs/`):
  - una notifica su `IONameMatch AppleSPUHapticsAudioDriver`;
  - una su `IOPropertyMatch "Button Haptics Asset Interface" = 1`;
  - `aop-smart-cover`.
  - Nessuna di queste blocca SpringBoard oggi. Un servizio aptico pubblicato a metà potrebbe però svegliare un'attesa
    come quella che UniXNU ha trovato in iPadOS (`BKButtonHapticsController`) ?. Si pubblicano servizi aptici solo
    quando funzionano.

### 3.2 Rotazione e movimento

Ci sono due consumatori, ben distinti.

1. **backboardd → orientamento dell'interfaccia** ✔.
   - Usa `BKSAccelerometer`, `BKOrientationManager` e `IOHIDEventCreateDeviceOrientationEventWithUsage` (stringhe della
     shared cache).
   - Prende gli eventi da qualunque servizio con `HIDServiceSupport` e la coppia usage giusta: è lo stesso match con cui
     backboardd trova oggi i nostri servizi.
   - Serve un servizio `PrimaryUsagePage 0xFF00` / `PrimaryUsage 0x03` (`kHIDUsage_AppleVendor_Accelerometer`), con
     eventi `kIOHIDEventTypeAccelerometer` = 13 in g.
   - Gli altri valori, letti in IOHIDFamily ✔: orientamento 10, luce 12, prossimità 14, giroscopio 20, bussola 21,
     pressione 31; pagina `AppleVendorMotion` = `0xFF0C`, con `Orientation` = 6.
   - Scion lo fa con servizi virtuali iniettati in backboardd, e l'orientamento arriva ✔. Noi possiamo farlo **nel
     kernel**, come `SweetHIDButtons`, con `IOHIDEvent::accelerometerEvent()`, che è già nella patch 0058.
   - La schermata Home e quella di blocco dell'iPhone X non ruotano: la prova va fatta in un'app (Safari, Calcolatrice,
     Foto) ~.
2. **`locationd` → CoreMotion, bussola, passi e altitudine per le app** ✔ (stringhe di `locationd`).
   - Passa per `CLIoHidInterface` e `CLMotionCoprocessor` (`sendCommandPayload`, `CLSPU::PackedSensorBuffer`,
     `CLSensorFusionService{3,6,9}AxisSPU`), cioè il protocollo del coprocessore di movimento di Apple.
   - Emularlo è un lavoro di reverse engineering dal risultato incerto ?.
   - HakiOS registra anche `0xFF0C`/3 per `CLIoHidInterface`, senza prova che basti.
   - Obiettivo realistico: rotazione sì; `CMMotionManager`, bussola e passi forse in parte, molto dopo.

### 3.3 Prossimità

- **Come la usa iOS** ✔ (stringhe della shared cache):
  - backboardd la gestisce con `BKSHIDServicesRequestProximityDetectionMode` (dal telefono e da CoverSheet), e la stessa
    classe del driver Apple si chiama `AppleProxHIDEventDriver`;
  - il servizio va su `0xFF00`/`0x08`, con eventi di tipo 14.
- **Su `andromeda`**:
  - serve soprattutto **in chiamata**, che non abbiamo (ROADMAP 14);
  - l'ultrasuono richiede l'ADSP, le porte AFE ultrasoniche e la calibrazione Elliptic: viene dopo l'audio
    ([audio.md](audio.md), A10).
- **Nel frattempo** non serve niente. Un servizio fisso "lontano" va considerato solo se qualcosa lo aspetta ?.

### 3.4 Luce ambientale

- **Come la usa iOS**: tramite `BKSHIDServicesAmbientLightSensorExists` e `…EnableAutoBrightness` di backboardd e
  CoreBrightness (`ALSServiceReady`), con eventi di tipo 12 da `0xFF00`/`0x04` ✔.
- **Un'attesa all'avvio**: HakiOS aveva scritto un gancio per l'attesa di 5 s di `-[BLControl waitForALSArrival]`, poi
  disattivato ~. Va controllato nei nostri log quanto aspetta backboardd senza ALS ?.
- **Cosa serve**: la TSL2584 (dietro lo SLPI) e un controllo della luminosità che iOS sappia comandare; `andrbl`
  (`xnu/andromeda_backlight.c`, in lavorazione) ne è la metà.

### 3.5 Slider: che evento per iOS

iOS non ha uno slider, quindi l'evento va scelto. Opzioni, dalla più semplice:

1. **Solo nel log**, per la verifica dell'hardware (fase H0).
2. **Un servizio HID nel kernel** (`SweetHIDSlider`, sul modello di `SweetHIDButtons`) che pubblica uno stato a due
   posizioni. Il servizio non vincola la scelta dell'uso. Due candidati:
   - **l'interruttore suoneria/silenzioso**: sull'iPhone è anch'esso un interruttore a due posizioni.
     backboardd lo gestisce (`handleRingerSwitchEvent`, `_BKSHIDServicesGetRingerState` ✔); la coppia usage che si
     aspetta non la conosciamo ancora ? (la tabella HID ha `kHIDUsage_Tfon_RingEnable` = `0x2D` sulla pagina
     Telephony, ma è solo un candidato);
   - **un tasto consumer** già noto a iOS, letto da un piccolo gancio in SpringBoard che apre la fotocamera frontale
     quando si apre lo slider, come fa il `KeyHandler` di Android. Richiede un'iniezione in user space, e oggi non ne
     usiamo.
3. **Per la fotocamera** ([fotocamere.md](fotocamere.md)) lo stato dello slider basta leggerlo nel kernel.

Proposta: H1 pubblica lo stato e lo scrive nel log; la scelta fra suoneria e fotocamera la fa l'utente, dopo aver visto
quale coppia usage accetta backboardd.

## 4. Piano a fasi

Ogni gradino parte spento e si accende con un suo parametro di avvio. I nomi sono proposte; i parametri di sola lettura
possono stare in `andromeda_probe.c`, come `andrprobe`. La lettura dei Hall prevista dalla sonda C0 di
[fotocamere.md](fotocamere.md) va scritta **una volta sola** e condivisa.

### H. Slider

| Gradino | Parametro | Cosa fa | Scritture | Fatto quando | Difficoltà |
|---|---|---|---|---|---|
| **H0** | `andrhall=1` (o un livello di `andrprobe`) | ogni 200 ms per 2 minuti legge `GPIO_CFG` e `GPIO_IN_OUT` dei pin 42 e 114 e stampa una riga a ogni cambiamento (con funzione, pull e direzione del pin); l'utente apre e chiude lo slider | nessuna | i due bit si alternano (0,1)/(1,0) con i movimenti, e si sa quale è "aperto" | **bassa**, 1 giorno |
| **H1** | `andrhall=2` | polling a 20-50 ms con antirimbalzo; macchina a stati chiuso/movimento/aperto; servizio `SweetHIDSlider` registrato | nessuna (se i pin hanno già funzione GPIO e ingresso; se no, si imposta solo l'ingresso come fa Xiaomi, con un elenco chiuso di 2 pin) | stato corretto nel log per 20 aperture | bassa |
| **H2** | — | l'evento a iOS (sezione 3.5); più avanti l'interruzione TLMM (registri `INTR_CFG`/`INTR_STATUS` a `+0x8`/`+0xC`, interruzione riassuntiva del TLMM) al posto del polling | — | iOS reagisce (suoneria o fotocamera) | media |

### V. Vibrazione

| Gradino | Parametro | Cosa fa | Scritture | Fatto quando | Difficoltà / rischio |
|---|---|---|---|---|---|
| **V0** | `andrhaptic=1` | lettura via SPMI observer, come `andromeda_probe.c`, di `{3, 0xc0}` aggiunto all'elenco chiuso: TYPE/SUBTYPE, STATUS1 e tutti i registri da `0x46` a `0x70`; stampa APID e proprietario del canale | nessuna (solo il comando di lettura observer) | identità `0x15/0x09` ~ e proprietario 0 | **bassa**, rischio minimo |
| **V1** | `andrhaptic=2` | `hw_init` come `qti-haptics.c`, poi **due impulsi costanti da 200 ms a ≤ 2552 mV**, a 2 s di distanza, con STATUS1 letto ogni 10 ms; poi PLAY=0 ed EN_CTL1=0 | solo i 13 registri dell'elenco (`0x46-0x70`), ognuno riletto, sul canale del blocco dopo aver verificato che è nostro (proprietario 0), come `sp_write_pon_spare()`; mai sopra `0xD0` | l'utente sente due impulsi; nessun `SC_DET` | **bassa-media**; rischio basso (motore e limiti di Android; spegnimento anche sul percorso del panic) |
| **V2** | `andrhaptic=3` | nodo `/dev/andromeda-haptic` (mono int16 come `/dev/hakios-haptic`); tocchi da 35 ms e vibrazioni fino a 1,5 s con pausa uguale | come V1 | iOS vibra con la tastiera, gli avvisi e la suoneria | media; dipende dal plug-in audio (fase A0 di [audio.md](audio.md)) |
| **V3** | — | effetti brevi in buffer (WF_S1-S8 e freno, come gli effetti `wf_0`-`wf_5` del DT) per i "tick" di UIKit | in più: BRAKE, WF_REPEAT, WF_S1-S8 | tocchi più nitidi | media |

Per V1 serve un'**SPMI in scrittura generica**, limitata ai registri ammessi. Oggi `andromeda_probe.c` scrive solo
`PON_SPARE`: conviene spostare lettura e scrittura SPMI in un file comune (`andromeda_spmi.c`), condiviso con la
batteria e con il ritorno in fastboot.

### S. Sensori (SLPI)

| Gradino | Parametro | Cosa fa | Scritture | Fatto quando | Difficoltà |
|---|---|---|---|---|---|
| **S0** | `andrslpi=1` | rilevamento in sola lettura: voci SMEM 481/430 (SMP2P dello SLPI), presenza delle FIFO GLINK, `is-call-available`/`PAS_IS_SUPPORTED(12)` via SCM (solo domande, come fa già `pas_auth_available()` in `andromeda_gpu_core.c`); nessuna mappatura di `0x97b00000` | nessuna | sappiamo se TrustZone accetta il PAS 12 e in che stato lo SLPI è stato lasciato | bassa |
| **S1** | `andrslpi=2` | avvio dello SLPI: firmware `slpi.mdt/.bNN` preso dal telefono (`/vendor/firmware_mnt/image/`) e incorporato come quello della GPU (mai in git); voti RPMh `lcx`/`lmx` (patch 0092 generalizzata), QMP all'AOP (moduli `cepheus_qmp_*` del Mi 9), poi `INIT_IMAGE`, `MEM_SETUP`, copia, `AUTH_AND_RESET`; **mai `PAS_SHUTDOWN`** (bloccava `vayu`) | memoria di `pil_slpi_region` (riservata), chiamate SCM, voti RPMh; nessuna partizione | lo SLPI segnala "ready" in SMP2P | **alta** |
| **S2** | `andrslpi=3` | GLINK su SMEM, QRTR: arriva il servizio 400; lookup dei SUID `accel` e `ambient_light` e lettura degli attributi `NAME`/`VENDOR` | messaggi IPC | **"ID del sensore" nel log** (per esempio `lsm6dso`/`STMicro` ~) | **alta** |
| **S3** | `andrslpi=4` | server FastRPC inverso in sola lettura: configurazione (`/vendor/etc/sensors/config`, `sns_reg_config`), registry (dal backup di `persist`, **solo letto**) e `soc0` ("ANDROMEDA", 339); poi stream dell'accelerometro a 50 Hz | messaggi IPC; nessuna scrittura su `persist` | campioni plausibili (circa 1 g sull'asse verticale) | **molto alta** (SMMU per lo SLPI, protocollo FastRPC) |
| **S4** | `andrsensors=1` | servizio HID nel kernel `0xFF00`/3 con gli eventi accelerometro (segni e assi dalla configurazione), per backboardd | — | le app ruotano | media (dopo S3) |
| **S5** | — | luce (`0xFF00`/4, con `andrbl`), poi giroscopio e bussola; CoreMotion solo come ricerca | — | luminosità automatica | alta |

**Prossimità:** dopo l'audio (A10 di [audio.md](audio.md)).

**Strada scartata per ora:** leggere i sensori direttamente dal processore principale, accendendo clock SSC e QUP
SE20-23 e leggendo per esempio `WHO_AM_I` dell'LSM6DSO. Nel DT è tutto `disabled`, il blocco SSC ha una sua alimentazione
e probabilmente protezioni XPU, e un accesso a un blocco spento può bloccare il bus invece di dare un errore ?. Non è un
primo passo.

### Ordine consigliato

1. **H0 + V0** nello stesso avvio, in sola lettura: lo slider mosso dall'utente, e i registri del motore.
2. **V1** con l'utente presente: due impulsi.
3. **H1**, poi **S0** (anch'essa in sola lettura).
4. **V2**, quando c'è il plug-in audio A0.
5. **S1 → S3**, come progetto a parte (settimane o mesi), in coordinamento con il Mi 9 (stesso SLPI) e con il lavoro
   mainline su SM8150.

## 5. Regole di sicurezza per questi driver

- **Ogni gradino resta spento** senza il suo parametro di avvio.
- **Motore**:
  - mai oltre 2552 mV finché non si decide altrimenti, mai oltre 2800 mV (il kernel Android del telefono);
  - impulsi di prova di 1000 ms al massimo, vibrazioni di 3000 ms al massimo, con pausa;
  - `SC_DET` ferma tutto;
  - solo i registri dell'elenco, sempre riletti, mai sopra `0xD0`;
  - spegnimento (PLAY=0, EN_CTL1=0) a fine corsa e sul percorso del panic.
- **SPMI**: si scrive solo sul canale il cui proprietario è il processore principale, verificato prima di ogni
  scrittura (come `sp_write_pon_spare()`).
- **GPIO dei Hall**: solo letture. Se proprio serve, si imposta solo l'ingresso, e solo su 42 e 114.
- **SLPI**:
  - nessuna mappatura Normal di `pil_slpi_region` (XPU, avvertenza di ios-flame);
  - mai `PAS_SHUTDOWN`;
  - firmware, configurazione e registry presi dal telefono e tenuti fuori da git;
  - `persist` solo letto, da backup.

## 6. Riuso e licenze

| Fonte | Licenza | Come la usiamo |
|---|---|---|
| `qti-haptics.c`, `halls.c`, `sensors_ssc.c`, `audio-kernel/dsp/elliptic` (Xiaomi / Qualcomm / Elliptic Labs) | GPL-2.0 | solo documentazione: registri e sequenze, nessun codice copiato (come per `andromeda_probe.c`) |
| Riscrittura dei Hall di Demon000 e `KeyHandler` (LineageOS, Inkypen79) | GPL-2.0 / Apache-2.0 | solo il significato degli stati |
| `hakios-sm8150` (`vayu_*`, `motion.c`) | nessuna | solo fatti e metodo; per copiare codice serve il permesso dell'autore (vedi [richieste-permesso.md](richieste-permesso.md)) |
| HakiOS `audio_plugin.c` (userland) | MIT | riusabile con attribuzione (lo prevede già [audio.md](audio.md)) |
| HakiOS `hakios.haptic.c` (driver) | APSL-2.0 | struttura riusabile con attribuzione; l'hardware è diverso (AW86927) |
| Scion `inject.c` | MIT (da verificare file per file) | schema dei servizi HID; noi preferiamo il kernel |
| ios-sweet `SweetHID.cpp` e i file IOHIDFamily della patch 0058 | MIT / APSL-2.0 | base diretta dei servizi nel kernel |
| IOHIDFamily (Apple, tag 1915.140.3 e 1035.70.7) | APSL-2.0 | valori delle usage e dei tipi di evento |
| libssc, hexagonrpc, iio-sensor-proxy | GPL-3.0+ | solo documentazione del protocollo SSC/FastRPC |
| libqmi (`qmi-service-ssc.json`) | LGPL-2.1+ | solo documentazione (id del servizio e dei messaggi) |
| QEMUAppleSilicon `aop.c` | AGPL-3.0 | da evitare |

## 7. Domande aperte e rischi

- **Slider**:
  - i numeri 42/114 sono quelli del TLMM, e lo stato (0,1) vuol dire davvero "aperto"? Risponde H0;
  - che bias lascia l'UEFI sui due pin? Se l'uscita del Hall è open-drain, senza pull-up il valore fluttua ?.
- **Motore**:
  - il canale SPMI di `{3, 0xc0}` appartiene al processore principale su `andromeda`, come sul `vayu`?
  - quali valori lascia il bootloader? Risponde V0;
  - quale periodo di risonanza vale davvero per questo motore (4878 o 5102 µs)? Con QWD conta poco ~.
- **iOS e vibrazione**:
  - con il plug-in A0, `mediaserverd` usa davvero `Actuator` per i feedback di sistema con l'identità D22? Su `redwood`
    sì ✔, da noi ?;
  - c'è un'attesa di backboardd legata all'aptica che un servizio pubblicato a metà potrebbe sbloccare o bloccare ?.
- **Rotazione**: i segni degli assi dell'LSM6DSO rispetto alla convenzione Apple. HakiOS inverte tutti gli assi come
  ipotesi ~.
- **SLPI**:
  - TrustZone e l'hypervisor accettano il PAS 12 da XNU?
  - serve l'SMMU per FastRPC, e si può fare senza bypass?
  - lo SLPI crea i driver senza registry ("H-noreg")?
  - lo stesso lavoro serve al Mi 9 (stesso SoC, sensori diversi): va diviso con loro.
- **Luce e prossimità**:
  - quanto aspetta backboardd senza ALS all'avvio (`waitForALSArrival`)?
  - qualcosa di iOS aspetta un servizio di prossimità?

## 8. Crediti da aggiungere a `CREDITS.md`

Questo documento non modifica `CREDITS.md` (vincolo di questa ricerca). Quando il lavoro diventa codice, va aggiunta
una sezione "Ricerca: vibrazione, sensori, slider (consultati)" con:

- Xiaomi (MiCode `andromeda-p-oss`: `qti-haptics.c`, `halls.c`, `sensors_ssc.c`, DT);
- Qualcomm / The Linux Foundation;
- Elliptic Labs (driver ultrasuoni);
- Demon000 (Cosmin Tanislav, riscrittura dei Hall), Nicholas Magill (Inkypen79) e Sebastiano Barezzi (kernel e
  `KeyHandler` di `andromeda`), LineageOS;
- PixelExperience-Blobs (configurazioni dei sensori);
- nomorecoolnicknames (HakiOS `vayu`: aptica PM8150B, SLPI, SSC, `motion.c`);
- OOSEX GANG (shimoku, vcvkk e collaboratori: HakiOS, plug-in `Actuator`);
- Scion-Project (vcvkk, dylanneve1, shimoku74 e collaboratori);
- dipa-studio (ios-sweet `SweetHID`);
- Apple (IOHIDFamily);
- Nick Wylynko (avvertenza XPU di ios-flame);
- bluseliu50 / UniXNU (attesa aptica di backboardd);
- Dylan Van Assche (libssc, `drv-ssc-*`), Richard Acayan (hexagonrpc), Andrew Gigena (sm8150-mainline WIP), Caleb
  Connolly (aptica PMI8998), i manutentori di libqmi.

## 9. Fonti

**Telefono (fuori da git)**

- `phone/<seriale>/live-fdt.dts`:
  - `qcom,pm8150b@3` / `qcom,haptics@c000`;
  - `pil_ssc` (`qcom,ssc@5c00000`), `pil_slpi_region`, `ssc_sensors`, `glink_slpi` (`dsps`), `qcom,smp2p-dsps@1799000c`;
  - `qupv3_se20-23`, `qupv3_3`, `clock_scc`, `slpi_tlmm`;
  - i nodi `qupv3_se*_i2c` attivi;
  - `qupv3_se1_i2c_pins` e `qupv3_se4_2uart_pins` (GPIO 114/42).
- `phone/<seriale>/info/getprop.txt`: servizi `vendor.sensors*`, `vendor.qti.vibrator`, `vendor.adsprpcd_sensorspd`.
- `phone/logs/*.log` (6 novembre 2026): traccia IOKit di backboardd (`AppleSPUHapticsAudioDriver`,
  `Button Haptics Asset Interface`, `aop-smart-cover`, `HIDServiceSupport`, apertura di `SweetHIDButtons`/`SweetHIDTouch`).

**Kernel e blob Android**

- [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource), ramo `andromeda-p-oss`, commit
  `74347f5521fb`:
  - `drivers/input/misc/qti-haptics.c`;
  - `drivers/halls/halls.c` e `Kconfig`;
  - `drivers/sensors/sensors_ssc.c`;
  - `audio-kernel/dsp/elliptic/`, `audio-kernel/include/elliptic/`;
  - `arch/arm64/boot/dts/qcom/andromeda-sm8150.dtsi`, `pm8150b.dtsi`, `raphael-sm8150.dtsi` (AK09970 del K20 Pro, non
    di `andromeda`);
  - `arch/arm64/configs/andromeda_user_defconfig`.
- Kernel Linux `drivers/pinctrl/qcom/pinctrl-sm8150.c` (tile, layout dei registri, mappa PDC).
- [`Inkypen79/kernel_xiaomi_andromeda`](https://github.com/Inkypen79/kernel_xiaomi_andromeda) (`drivers/halls/halls.c`:
  `5fb68e496063` di Demon000, 2019-05-20; `6df91f3ec4d2` di Nicholas Magill; `75f5b5e2db09` di Sebastiano Barezzi) e
  [`Inkypen79/device_xiaomi_andromeda`](https://github.com/Inkypen79/device_xiaomi_andromeda) (`KeyHandler.java`).
- [`LineageOS/android_device_xiaomi_perseus`](https://github.com/LineageOS/android_device_xiaomi_perseus) (`KeyHandler`),
  [`LineageOS/android_kernel_xiaomi_sdm845`](https://github.com/LineageOS/android_kernel_xiaomi_sdm845).
- [`PixelExperience-Blobs/vendor_xiaomi_andromeda`](https://github.com/PixelExperience-Blobs/vendor_xiaomi_andromeda),
  ramo `fourteen`, `63c49c5e1614`:
  - `proprietary/vendor/etc/sensors/config/sm8150_{lsm6dso_0,ak991x_0,bmp285_0,tsl2584,adux1050_0,power_0}.json`,
    `shtw2_0.json`;
  - l'elenco dei blob (`sensors.ssc.so`, `sensors.halls.so`, `sensors.elliptic.so`, `elliptic_sensor.xml`,
    `libultrasound.so`, `sscrpcd`).
- [AndroidBlobs/vendor_xiaomi_andromeda](https://github.com/AndroidBlobs/vendor_xiaomi_andromeda) (elenco dei blob,
  confronto).
- Elliptic Labs, comunicato su INNER BEAUTY III e Mi MIX 3 5G (2019-05-02):
  <https://ellipticlabs.com/mfn_news/elliptic-labs-environmentally-friendly-ai-virtual-smart-sensor-inner-beauty-iii-enables-xiaomi-mi-mix-3-5g/>.

**XNU / iOS su hardware non Apple**

- [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) `3e7be13` (2026-10-29):
  - `kernel/bsd/dev/vayu_haptic.c`, `vayu_slpi.c`, `vayu_pas.c`, `vayu_ssc.c`, `vayu_fastrpc.c`;
  - `kernel/bsd/sys/vayu_*.h`, `userland/src/motion.c`;
  - `README.md` e `device.conf`.
- [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) `63fc2b4` (2026-11-06):
  - `drivers/haptic/hakios.haptic.c`, `userland/src/audio_plugin.c`, `userland/src/inject.c`;
  - `docs/index.md`, `docs/driver-review/camera-haptic-torch.md`, `docs/reference/drivers.md`;
  - `LICENSE`, `LICENSE.APSL`.
- [`Scion-Project/scion-kernel`](https://github.com/Scion-Project/scion-kernel) (`userland/src/inject.c`, pagina
  `starlte`); `UniXNU/docs/devices/starlte.md` (corse dell'1 e del 3 novembre 2026).
- [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) `45263be`: `xnu/patches/0058-sweet-hid-event-services.patch`
  (`SweetHID.cpp`, `IOHIDEvent.cpp`, `IOHIDEventTypes.h`, `IOHIDUsageTables.h`).
- [`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame) `745c434`: `HANDOFF.md`, `NOTES.md`, patch 0084 e 0085.
- [`UniXNU/UniXNU`](https://github.com/UniXNU/UniXNU) `10edc1e`: `scripts/check-original-backboard-haptic-wait.py`,
  `docs/devices/oneplus13t.md`.
- [`aTechGeekDev/ios-apollo`](https://github.com/aTechGeekDev/ios-apollo) (aptica AW8697, non verificata da noi).
- [`apple-oss-distributions/IOHIDFamily`](https://github.com/apple-oss-distributions/IOHIDFamily): `IOHIDEventTypes.h`
  (tag 1915.140.3), `AppleHIDUsageTables.h` (tag 1035.70.7), `IOHIDEventDriver.cpp`.

**Linux e postmarketOS (solo documentazione)**

- [libssc](https://codeberg.org/DylanVanAssche/libssc) (Dylan Van Assche); [hexagonrpc](https://github.com/linux-msm/hexagonrpc)
  (Richard Acayan); [iio-sensor-proxy](https://gitlab.freedesktop.org/hadess/iio-sensor-proxy) 3.9;
  [libqmi](https://gitlab.freedesktop.org/mobile-broadband/libqmi) (`data/qmi-service-ssc.json`);
  [linux-msm/qrtr](https://github.com/linux-msm/qrtr).
- [sm8150-mainline, commit `2a070aeb`](https://gitlab.com/sm8150-mainline/linux/-/commit/2a070aeb7730f28ca2b99ba29f82633098469aba)
  (Andrew Gigena, "sm8150 sensors debug").
- postmarketOS wiki: "Qualcomm Snapdragon Sensor Core", "Qualcomm Snapdragon 855 (SM8150)"; pmaports MR 4050.
- Caleb Connolly, serie "qcom-pmi8998-haptics" v7:
  <https://patchwork.kernel.org/project/linux-input/patch/20221015172915.1436236-3-caleb@connolly.tech/>.

**Sistema iOS (locale, mai nel repository)**

- `out/andromeda/root/ios16-root.img` (iOS 16.7.16 `20H392`), letto con 7-Zip e `strings`:
  - `usr/libexec/locationd`: `CLIoHidInterface`, `CLMotionCoprocessor`, `CLSensorFusionService*SPU`,
    `CLCompassCalibratorAKM`;
  - la dyld shared cache: `BKSAccelerometer`, `BKSHIDServicesRequestProximityDetectionMode`,
    `BKSHIDServicesAmbientLightSensorExists`, `AppleSPUHapticsAudioDriver`, `AppleProxHIDEventDriver`, i messaggi sulla
    vibrazione, `handleRingerSwitchEvent`;
  - le tarature `Library/Audio/Tunings/D22/Haptics` e `ButtonHaptics`.

**Documenti del progetto collegati**

- [audio.md](audio.md) (plug-in A0, `Actuator`, ultrasuoni A10), [fotocamere.md](fotocamere.md) (Hall e sonda C0),
  [ricarica.md](ricarica.md) (SPMI observer, PM8150B),
  [`../../xnu/andromeda_probe.c`](../../xnu/andromeda_probe.c), [`../../xnu/andromeda_gpu_core.c`](../../xnu/andromeda_gpu_core.c)
  (SCM PAS), [`../../ROADMAP.md`](../../ROADMAP.md) voce 13.
