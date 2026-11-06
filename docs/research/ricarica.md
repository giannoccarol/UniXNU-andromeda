# Batteria e ricarica sotto XNU su `andromeda`: cosa ferma la ricarica, e un piano

Ricerca del 6 novembre 2026, solo lettura: nessun comando sul telefono, nessun codice dei repository scaricati eseguito.
I valori del Mi Mix 3 5G vengono dal device tree attivo del telefono (`phone/<seriale>/live-fdt.dts`, fuori da git), dal
kernel Xiaomi `andromeda-p-oss` e dall'UEFI di Aloha che avviamo; gli altri fatti dai file citati in [Fonti](#8-fonti).

Legenda della certezza, usata in tutto il documento:

- **✔** letto nel sorgente (o misurato da altri su hardware uguale);
- **~** dedotto: coerente con più indizi, non ancora visto su `andromeda`;
- **?** da verificare con la sonda della [fase (a)](#5a-sonda-in-sola-lettura).

Tutti gli indirizzi del caricatore e del fuel gauge sono sul **PM8150B, SPMI SID 2** (✔ FDT: `qcom,pm8150b@2`,
`reg = <0x02 0x00>`).

## Sommario

- **Il caricatore carica da solo.** Lo SMB5 del PM8150B è una macchina a stati hardware (trickle → pre → fast →
  taper → fine → ricarica automatica). Il driver Linux non "fa" la ricarica: fissa i limiti, abilita il comando di
  carica e risponde agli eventi. Senza software la ricarica continua, **se qualcuno l'ha lasciata abilitata e nessuna
  protezione la ferma**.
- **Con buona probabilità a fermarla è il nostro UEFI.** L'UEFI di Aloha `xiaomi-andromeda` contiene i DXE Qualcomm
  `PmicDxe`, `QcomChargerDxe`, `ChargerExDxe` e l'app `QcomChargerApp`, con `QcomChargerCfg.cfg` che dice
  `EnableChargerWdog = 1` ("Enable Charger WDOG during charging and Disable before exiting", riga 143). Il DXE **arma il
  watchdog del caricatore appena viene caricato** (stringa `QcomChargerDxe:: %a Enabled WDOG.` nel binario), ma solo
  l'app lo disarma quando esce, e il gestore di avvio Mu di Aloha **non lancia mai `QcomChargerApp`**. XNU parte quindi
  con il watchdog armato in modalità "il morso disabilita la ricarica": abbaio dopo 128 s, morso 1 s dopo. Nessuno lo
  accarezza, e la ricarica si ferma (~).
- **La prova più vicina: un port XNU sullo stesso PMIC.** Su LG G8X (SM8150 + PM8150B, UEFI Mu-Silicium con gli stessi
  DXE Qualcomm), `shenli99/ios-mh2lm` legge sotto XNU `0x1651 = 0x62`, `0x1653 = 0x8C` (watchdog armato, morso che
  disabilita la ricarica) e `0x1042 = 0` (comando di carica spento) (✔). In una sessione più vecchia, partita con la
  ricarica già abilitata, la batteria si è caricata a ~2,3 A per 65 campioni (~130 s), poi lo stato è passato a 7
  ("disabilitata") col cavo ancora inserito e il comando ancora a 1 (✔). Sono i tempi del watchdog (~).
- **Seconda causa possibile, non alternativa: il PC dà 500 mA.** Con il cavo al PC (porta SDP), per il log USB, il
  limite d'ingresso è 500 mA, sotto il consumo del telefono con la grafica disegnata dalla CPU. HakiOS `vayu` (stesso
  SoC) annota un calo dal 31 % al 18 % in una notte su porta PC (✔, `vayu_cpufreq.c:684-685`). La sonda le distingue.
- **Leggere la batteria è facile, e si fa in sola lettura.** SOC monotono a `0x4009`/`0x400A` (0-255), tensione a
  `0x41A0` (122,07 µV/LSB), corrente a `0x41A2` (488,28 µA/LSB, positiva in scarica), stato della carica a `0x1006`,
  cavo a `0x1310` bit 4 (✔). La temperatura della batteria sta nella SRAM del fuel gauge (parola 328, 0,25 °C/LSB): per
  leggerla serve una richiesta DMA, cioè delle scritture (✔).
- **Il lato XNU c'è a metà.** `sweet_spmi.c` di ios-sweet (patch 0062) sa già leggere il PMIC tramite il canale
  observer dell'arbitro SPMI v5, ma accetta solo il SID 0. `SweetBattery.cpp` è scritto per il PM6150 + QG del Redmi
  Note 10 Pro, non per il nostro PM8150B + FG Gen4, ed è comunque spento (`sweetbattery` non è nei parametri di
  `board.toml`). iOS vede quindi un 100 % fisso.
- **Il piano, a fasi, ognuna dietro un parametro di avvio:**
  - (a) una sonda in sola lettura che stampa i registri all'avvio e ogni N secondi sul log USB;
  - (b) un driver minimo che **accarezza** il watchdog (`0x1643`) e riabilita il comando di carica (`0x1042`), senza
    cambiare tensioni né correnti;
  - (c) valori veri a `IOPMPowerSource`;
  - (d) regole di sicurezza: tensione massima 4,40 V, niente scritture fuori da un elenco chiuso, stop sopra 45 °C.
- **Esperimento rapido in parallelo:** compilare l'UEFI con `EnableChargerWdog = 0` (è un file di testo nel nostro
  albero Aloha). Se con quell'UEFI la ricarica tiene più di 3 minuti, l'ipotesi è confermata senza toccare il kernel.

## 1. L'hardware (FDT attivo)

### SPMI e PMIC

| Cosa | Valore (✔ FDT) |
|---|---|
| Arbitro SPMI | `qcom,spmi-pmic-arb` a `/soc/qcom,spmi@c440000`, `qcom,ee = 0`, `qcom,channel = 0` |
| Regioni | `core` `0x0C440000`+`0x1100`, `chnls` `0x0C600000`+`0x2000000`, `obsrvr` `0x0E600000`+`0x100000`, `intr` `0x0E700000`+`0xA0000`, `cnfg` `0x0C40A000`+`0x26000` (uguali al `sweet` SM7150 e al `sm-t733` di UniXNU) |
| IRQ dell'arbitro | GIC SPI `0x1E1` (`periph_irq`) |
| PM8150 | SID 0 e 1 (PON `0x0800`, ADC5 `0x3100`, SDAM `0xB100` usata dal FG come `fg_sdam`) |
| **PM8150B** | **SID 2** (caricatore, fuel gauge, ADC5, Type-C, PD-PHY, BCL, Qnovo) e SID 3 (LPG/LED, aptica) |
| PM8150L | SID 4 e 5 |

### Blocchi del PM8150B (SID 2)

| Blocco | Base | Nodo FDT / nota |
|---|---|---|
| REVID | `0x0100` | `qcom,revid@100` (sottotipo PMIC atteso `0x20` = PM8150B, ✔ `qpnp-revid.h:187`) |
| PON (secondario) | `0x0800` | non usato per i tasti |
| Temp alarm | `0x2400` | `qcom,spmi-temp-alarm` |
| ADC5 (VADC) | `0x3100` (+ cal `0x3700`) | `vadc@3100`, riferimento 1875 mV |
| ADC_TM5 | `0x3500` | monitora `wp_therm` |
| Qnovo5 | `0xB000` | `qcom,sdam-qnovo@b000` (FET su GPIO8) |
| GPIO | `0xC000` | GPIO6 `smb_stat`, GPIO7 `power_good`, GPIO8 Qnovo, **GPIO12 boost OTG** |
| **CHGR** | `0x1000` | `qcom,chgr@1000` |
| **DCDC** | `0x1100` | `qcom,dcdc@1100` |
| **BATIF** | `0x1200` | `qcom,batif@1200` |
| **USBIN** | `0x1300` | `qcom,usb@1300` |
| DCIN | `0x1400` | `qcom,dc@1400` (non c'è ricarica wireless) |
| **TYPEC** | `0x1500` | `qcom,typec@1500` |
| **MISC** (watchdog) | `0x1600` | `qcom,misc@1600`, interrupt `wdog-snarl`, `wdog-bark`, `aicl-fail`, `aicl-done`, `smb-en`, `temp-change`, `temp-change-smb` |
| USB PD-PHY | `0x1700` | `qcom,qpnp-pdphy` |
| BCL | `0x1D00` | `qcom,bcl-v5` |
| **FG BATT_SOC** | `0x4000` | `qcom,fg-batt-soc@4000` |
| **FG BATT_INFO** | `0x4100` | `qcom,fg-batt-info@4100` |
| **FG RRADC** | `0x4200` | `qcom,fg-rradc@4200` |
| **FG MEM_IF** | `0x4300` | `qcom,fg-memif@4300` |
| FG DMA (finestre SRAM) | `0x4400`-`0x49FF` | non nel FDT: costanti `GEN4_FG_DMA0..5_BASE` (✔ `fg-reg.h`) |

### Il caricatore (`qcom,qpnp-smb5`, valori decodificati)

| Proprietà | Valore | Significato |
|---|---|---|
| `qcom,fv-max-uv` | 4 450 000 | tensione di fine carica massima: **4,45 V** |
| `qcom,fcc-max-ua` | 3 700 000 | corrente di carica massima: 3,7 A |
| `qcom,usb-icl-ua` | 2 800 000 | limite d'ingresso USB massimo: 2,8 A |
| `qcom,wd-bark-time-secs` | 16 | sotto Linux il watchdog abbaia ogni 16 s e Linux lo accarezza |
| `qcom,sw-jeita-enable` | presente | JEITA via software: Linux **spegne** il JEITA hardware |
| `qcom,auto-recharge-soc` | 99 | ricarica automatica sotto il 99 % |
| `qcom,chg-term-src` / `qcom,chg-term-current-ma` | 1 (ADC) / −220 | fine carica a 220 mA, misurata dall'ADC |
| `qcom,charger-temp-max`, `qcom,smb-temp-max` | 800 | 80,0 °C (decimi di grado) |
| `qcom,sec-charger-config` | 1 | secondo caricatore = solo charge pump (SMB1390) |
| `qcom,thermal-mitigation*` | 16 livelli | limiti d'ingresso per livello termico (3,0 A → 0,5 A) |
| `qcom,distinguish-qc-class-ab`, `qcom,lpd-disable` | presenti | QC classe A/B; rilevamento liquidi spento |
| `io-channels` | ADC5 `0x1E`, `0x08`, `0x07`, `0x99`, `0x83`, `0x09` | `mid_voltage`, `usb_in_voltage`, `usb_in_current`, `sbux_res`, `vph_voltage`, `chg_temp` |

### Fuel gauge e profili batteria

`qpnp,fg` (`qcom,fg-gen4`): cutoff 3400 mV, vuoto 3100 mV, corrente di terminazione di sistema −300 mA, cutoff 200 mA,
`qcom,fg-force-load-profile` (Linux ricarica il profilo a ogni avvio), `qcom,rapid-soc-dec-en`. `qcom,battery-data`
(`batt-id-range-pct = 15`) contiene due profili, **tutti e due con `batt-id-kohm = 100`**:

| Profilo | `battery-type` | Checksum | Capacità nominale | FV | Corrente rapida |
|---|---|---|---|---|---|
| `3811462_xiaomi_bm3gcos_3700mah_slave_jan3rd2019` (Coslight) | `e5g_cos` | `0xB9FD` | 3800 mAh | 4,45 V | 3,7 A |
| `alium_860_89032_0000_3600mah_averaged_masterslave_sep24th2018` | `itech_3700mah` | `0x1538` | 3800 mAh | 4,45 V | 3,7 A |

JEITA dei due profili (identico), usato dal JEITA software di Linux:

| Temperatura batteria | FCC | FV |
|---|---|---|
| 0,0-5,0 °C | 370 mA | 4,45 V |
| 5,1-10,0 °C | 1,11 A | 4,45 V |
| 10,1-15,0 °C | 1,85 A | 4,45 V |
| 15,1-45,0 °C | 3,70 A | 4,45 V |
| 45,1-58,0 °C | 1,85 A | **4,10 V** |
| fuori da 0-58 °C | 0 | — |

L'UEFI carica invece nel FG il profilo **Alium** (`BATTERY.PROVISION` e `BATTERY.PROVISION1` sono lo stesso file: checksum
`1538`, 3600 mAh, `Float_Voltage,4350`). Se nel telefono c'è la batteria Coslight, il FG gira con il profilo sbagliato
finché Android non lo ricarica ([domanda aperta 3](#7-domande-aperte)).

### Charge pump, caricatore parallelo, OTG

- **SMB1390** (charge pump ÷2) su I²C QUP SE4 (`i2c@890000`), indirizzo `0x10`, `status = "ok"`; lo slave `0x18` è
  `disabled`. Registri I²C a 16 bit (`CORE_STATUS1 0x1006`, `CORE_CONTROL1 0x1020` bit 0 `CMD_EN_SWITCHER`, ✔
  `smb1390-charger-psy.c:29-66`). Serve solo con ingressi a 9 V e più (HVDCP3, PD PPS). Si accende dal PM8150B
  (`MISC_SMB_EN_CMD 0x1648`) più comandi I²C: sotto XNU nessuno lo fa, ed è giusto così.
- **SMB1355** (caricatore parallelo) `0x0C` sullo stesso bus, `status = "ok"`, ma `sec-charger-config = 1` dice "solo
  CP": Linux non lo usa. Se sia montato fisicamente non si sa.
- **Boost OTG**: regolatore `ext_5v_boost` su **GPIO12 del PM8150B** (attivo alto, default `output-low`), più
  `DCDC_CMD_OTG 0x1140`. Non va mai acceso.
- `extcon_usb1`: VBUS letto su GPIO10 del PM8150 (`0x5be` è il pinctrl GPIO del PM8150), ID su TLMM 101.

## 2. Come carica lo SMB5 (Linux `qpnp-smb5.c`, `smb5-lib.c`)

### 2.1 Una macchina a stati hardware

`BATTERY_CHARGER_STATUS_1` (`0x1006`, bit 2:0) riporta lo stato (✔ `smb5-reg.h`):

| Valore | Stato | | Valore | Stato |
|---|---|---|---|---|
| 0 | INHIBIT (batteria già vicino a FV) | | 4 | TAPER (tensione costante) |
| 1 | TRICKLE | | 5 | TERMINATE (finita) |
| 2 | PRE_CHARGE | | 6 | PAUSE |
| 3 | FULLON (corrente costante) | | 7 | **DISABLE** |

L'hardware passa da solo da uno stato all'altro, termina a `ITERM` e riparte da solo (`CHGR_CFG2 0x1051` bit 2:1: per
tensione o per SOC, soglia SOC in `0x107D` = SOC×255/100). Linux interviene solo su limiti ed eventi. Prova sul campo:
nella sessione `m19` di ios-mh2lm il PMIC è passato da 7 a 3 (FULLON, −2,3 A) all'inserimento del cavo **senza alcuna
scrittura di XNU** (✔ `BATTERY-CHARGING-HISTORY.md`).

### 2.2 Cosa resta configurato quando parte XNU

La catena di avvio con `fastboot boot` è: XBL + UEFI di Xiaomi → ABL → la nostra immagine, che contiene l'UEFI di Aloha
→ UniXNU/DarwinBoot → XNU. L'UEFI di Aloha **ripete tutta la fase DXE**, compresi i driver Qualcomm del PMIC e del
caricatore:

- `APRIORI.inc` righe 58, 97, 98: `PmicDxeLa`, `QcomChargerDxeLA`, `ChargerExDxe`; `DXE.inc` righe 74-76 e 91: gli stessi
  più `QcomChargerApp`; `DXE.inc` 209-220: `QcomChargerCfg.cfg` e i due `BATTERY.PROVISION` (✔, Aloha `0b798128`).
- `uefiplat.cfg` riga 171: `DefaultChargerApp = "QcomChargerApp"`. Nel codice di Aloha però nessuno lo legge né lancia
  l'app: nessun riferimento a `ChargerApp` fuori dai `.inc` e dai `.cfg` (✔ ricerca nel nostro albero). Il gestore di
  avvio di Qualcomm dovrebbe lanciarla, quello di Mu no (~, da verificare).
- `QcomChargerCfg.cfg` (versione 31, file Qualcomm per "SDM855" con una sezione Xiaomi `[PlatformE5G]`):

| Chiave | Valore | Nota |
|---|---|---|
| `EnableChargerWdog` (r. 143) | **1** | "0: no WDOG; 1: enable during charging and disable before exiting; 2: enable and leave enabled when exiting" |
| `LoadBatteryProfile` (r. 172), `FgCondRestart` (r. 213) | TRUE, TRUE | l'UEFI ricarica il profilo (Alium) nel FG |
| `[PlatformE5G]` `ChgFvMax` / `ChgFccMax` (r. 334-335) | **4400 mV / 1000 mA** | le altre sezioni (MTP, QRD): 4350 mV / 2000 mA |
| JEITA E5G | freddo duro 0 °C, freddo morbido 15 °C, caldo morbido 45 °C, caldo duro 58 °C | `JeitaHardLimitEnable = TRUE`; in zona morbida FCC −50 mA, FV −300 mV a caldo |
| `HwAfpHotLimit` (r. 349) | 62 °C | il PBS spegne il telefono (AFP) se la batteria supera 62 °C "if SW is stuck" |
| `EmergencyShutdownVbatt` (r. 133) | 3200 mV | |
| `DieTempSrc`, `DieTempCompSrc`, `SkinTempSrc` | TRUE, TRUE, FALSE | regolazione termica hardware sulla temperatura del die |
| `ChargerHotInC` / `ChargerTooHotInC` | 80 / 90 °C | |

- Stringhe nel binario `QcomChargerDxeLA.efi` che usiamo (✔ `strings`): `ChargerPlatform_Init`,
  `QcomChargerDxe:: %a Enabled WDOG.`, `ChargerLib_Exit`, `ChargerLib:: %a WDOG Bite happened, Toggling charging command
  to re-enable charging.`, `ChargerLib:: %a Disabling charging`, `ChargerPlatform_LoadProfile`, `PlatformE5G`.
- Cosa fa il DXE (~, ipotesi da fonti pubbliche, da confermare con la sonda `andrprobe`): la configurazione dice
  "enable during charging and disable before exiting", le stringhe del binario parlano di watchdog abilitato e di
  comando di carica riattivato dopo un morso, e ios-mh2lm legge sul G8X (stesso PMIC, stesso DXE Qualcomm) dopo l'UEFI
  `0x1651 = 0x62` e `0x1653 = 0x8C`. Decodificati con i campi di `smb5-reg.h` (kernel Linux/Xiaomi):
  - `0x1651`: IRQ di abbaio (bit 6), bit 5, timer acceso all'inserimento del cavo (bit 1);
  - `0x1653`: "il morso disabilita la ricarica" (bit 7), abbaio 128 s (bit 3:2), morso al valore minimo (bit 1:0);
  - a disarmarlo alla fine dovrebbe essere `QcomChargerApp`, che il gestore di avvio di Aloha non lancia;
  - se l'UEFI ricarica il profilo del fuel gauge, il comando di carica `0x1042` può restare a 0.
- **ABL** (LinuxLoader, `QcomModulePkg/Library/BootLib/UpdateCmdLine.c` nel nostro albero Aloha) legge solo
  `ChargerEx` (carica a telefono spento, presenza del caricatore, tensione) e non scrive registri del caricatore (✔).

**Stato atteso al passaggio a XNU** (~, da confermare con la sonda): `0x1651 = 0x62`, `0x1653 = 0x8C`, `0x1042` a 0 o a
1 a seconda del percorso del profilo, FV/FCC da `[PlatformE5G]` (`0x1070 = 0x50` = 4,40 V, `0x1061 = 0x14` = 1,0 A) o da
`[PlatformMTP]` (`0x4B` = 4,35 V, `0x28` = 2,0 A), JEITA hardware acceso con le soglie E5G.

### 2.3 Cosa può fermare la ricarica senza driver

| # | Causa | Dove si vede | Firma nella sonda | Cosa fa Linux | Certezza |
|---|---|---|---|---|---|
| 1 | **Morso del watchdog** con `BITE_WDOG_DISABLE_CHARGING_CFG` | `0x1651`/`0x1653`; stato `0x1006` | `wd=62/8c`, stato 3 per ~129 s dall'inserimento, poi 7 con `en=1` e ingresso valido | lascia il watchdog acceso ma col morso innocuo (bit 7 di `0x1653` a 0), abbaio a 16 s, accarezza `0x1643` nell'IRQ di abbaio | ~ (✔ su G8X) |
| 2 | **Comando di carica spento** | `0x1042` bit 0 | `en=0`, stato 7 | `vote(chg_disable, DEFAULT_VOTER, false)` → scrive 1 | ~ (✔ su G8X) |
| 3 | Ingresso sospeso | `0x1340` bit 0; `0x110B` bit 6 | `path` bit 6 | `usb_icl` ≤ 25 mA → sospende, altrimenti no | ? |
| 4 | Limite d'ingresso basso (SDP 100/500 mA, AICL) | `0x1107`, `0x1108`, `0x1366`, `0x1342`, APSD `0x1308` | `icl=500`, `ib` ≈ 0 o positivo col PC | vota l'ICL in base ad APSD, Type-C Rp e PD (SDP 500, CDP 1500, DCP 1600, Rp 1,5/3 A) | ✔ |
| 5 | JEITA hardware | `0x100D` bit 3:2 (duro), 5:4 (morbido); FG `0x4106` | `s7` | spegne il JEITA hardware e usa il suo software | ✔ |
| 6 | Sovratensione batteria | `0x1007` bit 1; `0x1210` bit 2 | `s2` bit 1 | segnala e sospende | ✔ |
| 7 | Batteria/termistore mancante | `0x1210` bit 4/5; FG `0x4107` | `bat` | — | ✔ |
| 8 | Temperatura del die / connettore | `0x1606` (bit 6 regolazione attiva), `0x1607`, `0x1609` | `die` | regolazione termica, `THERMREG_SRC` | ✔ |
| 9 | Timer di sicurezza della carica rapida | `0x10A2` (192/384/768/1536 min) | stato 7 o errore dopo ore | imposta 768 min | ✔ registro, ? bit di stato |
| 10 | INHIBIT (batteria già vicina a FV) | `0x1051` bit 0, `0x1072` | stato 0 | normale | ✔ |
| 11 | Input crollato / AICL fallito | `0x1310` bit 0, `0x1610` bit 2, `0x1380` bit 7 | `in` | AICL con riavvio periodico 12 s | ✔ |

Nota sul punto 1: con il bit 1 di `0x1651` il timer parte **all'inserimento del cavo**. Se il cavo è già inserito durante
l'avvio, il morso può arrivare prima che XNU abbia caricato qualsiasi driver (UEFI + avvio fino a SpringBoard superano
facilmente 129 s). Il driver deve quindi sia accarezzare sia riaccendere. L'UEFI stesso, dopo un morso, "fa il toggle
del comando di carica" (stringa nel binario): `0x1042` a 0, poi a 1.

### 2.4 Cosa fa Linux all'avvio e durante il funzionamento

`smb5_init_hw()` (`qpnp-smb5.c:2606-3061`), in ordine; riassunto, non elenco completo:

1. Legge FCC/FV/ICL già programmati (`smblib_get_charge_param`) e i limiti dal DT; vota FCC ≤ 3,7 A, FV ≤ 4,45 V.
2. Regolazione termica (`MISC_THERMREG_SRC_CFG 0x1670`, `BATIF_ADC_CHANNEL_EN 0x1282`).
3. HVDCP (`USBIN_OPTIONS_1 0x1362`): autenticazione e HVDCP accesi, modalità autonoma spenta.
4. Type-C (`smb5_configure_typec`, 2300-2423): APSD acceso, HVDCP detect spento, `TYPE_C_CFG 0x1358` bit 7 a 0, IRQ
   Type-C, try.SNK, VCONN a software, `USBIN_LOAD_CFG 0x1365`.
5. `smblib_icl_override(HW_AUTO_MODE)`: `0x1366` bit 0 a 1, `0x1342` a 0, `0x1365` bit 4 a 0.
6. Limite OTG, voti ICL/DC, `DCIN_LOAD_CFG 0x1465` bit 5 a 0.
7. AICL (`0x1380`: riavvio periodico + AICL + sospensione al crollo), `AICL_RERUN_TIME 0x1661 = 1` (12 s).
8. **Abilita la ricarica**: `vote(chg_disable_votable, DEFAULT_VOTER, false)` → `CHARGING_ENABLE_CMD 0x1042` bit 0 = 1
   (`smb5-lib.c:1776`).
9. VBUS a controllo software (`DCDC_OTG_CFG 0x1153` bit 1 a 0).
10. **Watchdog** (2842-2873): `0x1653` ← abbaio `ilog2(16/16) = 0` (16 s) | morso 8 s (`0x3`) | snarl 8 s (`0x70`), con
    **`BITE_WDOG_DISABLE_CHARGING_CFG` a 0** → `0x73`. `0x1651` ← timer all'inserimento + IRQ di abbaio, AFP e timer
    sempre acceso a 0.
11. Soglie ITERM (ADC), opzioni per caricatori "float", INHIBIT, timer di sicurezza 768 min (`0x10A2 = 2`), ricarica
    automatica a SOC 99 % (`0x107D`), **JEITA hardware spento** (`JEITA_EN_CFG 0x1090` a 0), `DCDC_ENG_SDCDC_CFG5`,
    `TYPE_C_DEBUG_ACC_SNK_CFG`.

Durante il funzionamento:

- l'IRQ di abbaio scrive `BARK_BITE_WDOG_PET 0x1643 = 1` (`wdog_bark_irq_handler`, `smb5-lib.c:7146-7160`);
- APSD e Type-C impostano l'ICL (`smblib_set_icl_current`, 1483-1560):
  - SDP ≤ 500 mA in modalità USB51 (`0x1366` bit 1, override `0x1342`);
  - gli altri tipi in modalità HC con `USBIN_CURRENT_LIMIT_CFG 0x1370` e override dopo APSD (`0x1365` bit 4);
  - poi AICL;
- JEITA software a passi (tabella della [sezione 1](#fuel-gauge-e-profili-batteria));
- mitigazione termica; Qnovo; charge pump.

Codifiche del PM8150B (✔ `smb5_pm8150b_params`, `qpnp-smb5.c:117-207`; confermate dalla serie mainline SMB5):

| Registro | Codifica |
|---|---|
| FCC `0x1061` | raw × 50 mA (0-8 A): 1,0 A = `0x14`, 2,0 A = `0x28`, 3,7 A = `0x4A` |
| FV `0x1070` | 3,60 V + raw × 10 mV (max 4,79 V): 4,35 V = `0x4B`, **4,40 V = `0x50`**, 4,45 V = `0x55` |
| ICL `0x1370`, stato `0x1106`/`0x1107`/`0x1108` | raw × 50 mA (0-5 A): 500 mA = `0x0A`, 1,5 A = `0x1E` |
| OTG `0x1152` | 500 mA + raw × 500 mA |
| JEITA CC comp `0x1092`/`0x1093` | raw × 25 mA |
| AICL 5 V `0x1381` | 4,0 V + raw × 100 mV |

**Attenzione:** la prima versione della serie mainline SMB5 usava le codifiche SMB2 (7,5 mV e 25 mA per passo). Su un
OnePlus 7T Pro ha scritto FV `0x7D` = 4,85 V e FCC `0x4E` = 3,9 A, e la batteria è arrivata a 4,527 V prima che se ne
accorgessero (✔ `hotdog-linux-bringup`, `2026-08-04-mainline616-smb5-parameters.md`). Da qui la regola: ogni scrittura
di un limite va riletta e confrontata con la tensione misurata dal FG.

### 2.5 Le scritture minime per tenere viva la ricarica

Ordinate dalla meno alla più invasiva:

1. **Accarezzare il watchdog**: `0x1643 ← 0x01` almeno ogni 60 s (abbaio a 128 s se la configurazione è quella
   dell'UEFI). Non disattiva nessuna protezione: se XNU si blocca, il morso spegne comunque la ricarica.
2. **Riaccendere il comando di carica**: se `0x1042` bit 0 = 0, oppure stato 7 con ingresso valido e nessun guasto,
   scrivere `0x1042 ← 0x00` e poi `0x1042 ← 0x01` (lo stesso toggle che fa l'UEFI dopo un morso, e che mainline fa
   dopo `CHGR_CFG2`: commit `1fba78714201`).
3. Solo se serve: togliere la sospensione dell'ingresso (`0x1340` bit 0 ← 0).
4. Solo se serve: ICL per caricatori da muro (DCP/Type-C 1,5 A) quando la sonda mostra 100/500 mA con un DCP.
5. Mai, nelle prime fasi: FV, FCC, JEITA, timer, HVDCP, Type-C, OTG, charge pump.

In alternativa al punto 1 si può **spegnere** il watchdog come mainline (`0x1653 ← 0x00`; `0x1651` bit 7, 6, 1 a 0) o
renderlo innocuo come Linux (`0x1653` bit 7 a 0). È più semplice, ma toglie la protezione contro un XNU bloccato. Per
noi accarezzare è meglio, ed è la stessa scelta di ios-mh2lm.

### 2.6 SMB1390, HVDCP, PD: da lasciare spenti

Per caricare a 5 V bastano il buck del PM8150B e l'ICL scelto da APSD e dalla corrente annunciata via Type-C. HVDCP
(QC2/3) e PD servono per salire di tensione e accendere la charge pump: è complessità e rischio in più, senza beneficio
finché la carica base non funziona. Un caricatore USB-C che annuncia Rp 3 A dà fino a 15 W a 5 V senza PD; l'hardware
lo legge in `TYPE_C_SNK_STATUS 0x1506`.

## 3. Leggere lo stato della batteria

### 3.1 Fuel gauge Gen4: registri diretti (sola lettura)

| Grandezza | Registri (SID 2) | Conversione | Fonte |
|---|---|---|---|
| SOC monotono (MSOC) | `0x4009`, copia `0x400A` (devono coincidere) | 0-255: 0 → 0 %, 255 → 100 %, altrimenti `round((raw−1)·98/253)+1` | ✔ `fg-util.c:848-905` |
| SOC pronto | `0x4010` (`BATT_SOC_INT_RT_STS`) bit 1 `SOC_READY`, bit 5 `MSOC_EMPTY` | se `SOC_READY` = 0, `0x4009` non è valido | ✔ `fg-reg.h` |
| Algoritmo acceso | `0x4046` bit 7 `FG_ALGORITHM_EN`; stato `0x4006` | solo lettura | ✔ |
| Tensione batteria | `0x41A0` LSB, `0x41A1` MSB; copia `0x41A6`/`0x41A7` | u16 × **122,070 µV** | ✔ `fg-util.c:1026-1065` |
| Corrente batteria | `0x41A2` LSB, `0x41A3` MSB; copia `0x41A8`/`0x41A9` | s16 × **488,281 µA**; **positiva = scarica** (convenzione del PMIC, che Linux riporta così) | ✔ `fg-util.c:982-1023` |
| ESR | `0x41A4`/`0x41A5` (copia `0x41AA`) | maschera MSB 6 bit | ✔ |
| Stato JEITA del FG | `0x4106`: bit 7 troppo caldo, 6 caldo, 5 freddo, 4 troppo freddo, 0 temperatura disponibile | | ✔ |
| Batteria mancante | `0x4107`: bit 2 HW, 1 algoritmo, 0 comparatore | | ✔ |
| Stato FG | `0x4109`: bit 7 reset del watchdog del FG, bit 0 terminazione | | ✔ |
| RRADC: batteria mancante | `0x4210` bit 0 | | ✔ |
| RRADC: temperatura grezza | `0x4288` LSB, `0x4289` MSB (2 bit) | **scala non verificata**: Linux Gen4 la definisce ma non la legge | ? |

La lettura "doppia con copia" del FG serve a scartare un campione a metà aggiornamento: è quello che fa Linux, e
sostituisce bene il `readWord` ripetuto di `SweetBattery`.

### 3.2 Temperatura e capacità: SRAM del FG via DMA (servono scritture)

La temperatura vera della batteria nel Gen4 è la parola SRAM **328** (`BATT_TEMP_WORD`), 10 bit con segno, **0,25 °C/LSB**
(✔ `qpnp-fg-gen4.c:654-673`). Il FG Gen4 espone la SRAM in sei finestre DMA (✔ `fg-memif.c:1169-1206`): la parola 328
cade nella partizione 4 (parole 300-405, base `0x4800 + 0x20`), quindi a **`0x4858`/`0x4859`** (~, calcolato:
`0x4820 + (328−300)×2`). Per leggerla:

1. `MEM_IF_MEM_ARB_CFG 0x4340` bit 0 ← 1 (richiesta di arbitraggio);
2. `MEM_IF_MEM_INTF_CFG 0x4350`: bit 7 ← 1, bit 5 ← 0 (accesso DMA, non IMA);
3. attesa di `MEM_IF_INT_RT_STS 0x4310` bit 3 (`GEN4_MEM_GNT`), dopo 40 µs, al massimo 50 × 10 ms;
4. lettura di `0x4858`-`0x4859`;
5. rilascio: `0x4350` bit 7 e 5 ← 0, `0x4340` bit 0 ← 0;
6. errori in `MEM_IF_DMA_STS 0x4370` (bit 1 scrittura, bit 2 lettura), da azzerare con `0x4371` bit 0.

ios-mh2lm l'ha fatto sul G8X: primo `DMA WRITE_ERROR` azzerato, poi 28,75-30,5 °C coerenti (✔ `BATTERY-FG-DMA-RECOVERY.md`).
Sul PM8150B v1 serve in più l'attesa di "ALG active" (`PM8150B_V1_DMA_WA`): va letta la revisione in REVID (`0x0100`-`0x0105`).
Con lo stesso meccanismo si leggono anche capacità nominale (parola 271), capacità appresa (285), cicli (291) e
integrità del profilo (299), utili più avanti per `MaxCapacity` e `CycleCount`.

### 3.3 Stato del caricatore e dell'ingresso

Bastano poche letture. `INT_RT_STS` (offset `0x10` di ogni blocco) è lo stato in tempo reale degli interrupt; l'ordine
dei bit è quello della tabella IRQ di `qpnp-smb5.c` ed è confermato dagli `interrupts` del FDT (✔):

- `0x1010` CHGR: 0 errore, 1 cambio stato, …, 6 allarme VPH, 7 calo VPH in pre-carica;
- `0x1110` DCDC: 5 limitazione di corrente d'ingresso, 7 switcher "power ok";
- `0x1210` BATIF: 0 temperatura, 2 sovratensione, 3 batteria bassa, 4 termistore o ID mancante, 5 terminale mancante,
  6 sovracorrente del buck, 7 sovratensione VPH;
- `0x1310` USBIN: 0 crollo, 2 sottotensione, 3 sovratensione, **4 cavo inserito**, 6 cambio sorgente, 7 cambio ICL;
- `0x1610` MISC: **0 snarl, 1 abbaio**, 2 AICL fallito, 3 AICL fatto, 4 `smb-en`, 5 `imp-trigger`, 6/7 cambio
  temperatura.

Poi `0x1006` (stato), `0x1007` (errori), `0x100D` (JEITA), `0x110B` (percorso: bit 0 ingresso valido, 4 usa USBIN,
6 USBIN sospeso), `0x1307`/`0x1308` (APSD: bit 0 SDP, 1 OCP, 2 CDP, 3 DCP, 4 float, 5 QC2, 6 QC3), `0x1506`
(Rp 0,5/1,5/3 A), `0x150B` (CC collegato, orientamento, modo sorgente).

### 3.4 ADC5 come riserva

L'ADC5 del PM8150B (`0x3100`) ha i canali del FDT: `vph_pwr 0x83` (÷3), `usb_in_v_div_16 0x08` (÷16),
`usb_in_i_uv 0x07`, `chg_temp 0x09`, `die_temp 0x06`, `conn_therm 0x4F`, `smb1390_therm 0x0E`, `wp_therm 0x4D`,
`chg_mid 0x1E` (÷6). **Non è in sola lettura**: una conversione richiede `CH_SEL 0x3144`, `EN_CTL1 0x3146` bit 7,
`CONV_REQ 0x3147` bit 7, attesa di `STATUS1 0x3108` bit 0 (EOC), dati in `0x3150`/`0x3151`, riferimento 1875 mV,
fondo scala `0x70E4` (✔ `qcom-spmi-adc5.c:35-96`, `qcom-vadc-common.h:50`). Serve per tensione e corrente d'ingresso e
per le temperature del caricatore e del connettore, non per la batteria (quella la dà il FG). ios-mh2lm ha già un
provider ADC5 (0155) validato sul G8X. Da tenere per la fase (c).

## 4. Cosa c'è già (XNU e altri port)

### 4.1 ios-sweet: `sweet_spmi.c` (patch 0062)

`bsd/dev/sweet_spmi.c` (✔ `0062-sweet-touch-power-battery.patch`, righe 472-616):

- mappa `core 0x0C440000` (0x2000), `cnfg 0x0C40A000` (0x1000) e `obsrvr 0x0E600000` (0x10000, EE 0);
- accetta solo l'arbitro v5 (`0x5…`-`0x6…`) con 1-512 APID;
- trova l'APID scorrendo la tabella `core + 0x900 + 4·n` (bit 19:8 = PPID), e se ce ne sono più di uno preferisce quello
  con proprietario EE 0 in `cnfg + 0x700 + 4·n` (bit 2:0);
- legge un byte con il comando `EXT_READL` scritto a `obs + 0x80·apid`: `(1 << 27) | ((addr & 0xff) << 4)`, BC = 0;
- attende `STATUS +0x08` (bit 0 fatto, bit 1-3 errore: fallito, negato, perso), dato in `RDATA0 +0x18`;
- un timeout "avvelena" quell'APID fino al riavvio;
- **limiti**: `sid != 0` → errore; periferiche ammesse solo `0x10`-`0x13` e `0x48`; nessuna scrittura.

Il formato coincide con Linux (✔ `drivers/spmi/spmi-pmic-arb.c` del ramo Xiaomi): `PMIC_ARB_CMD 0x00`, `STATUS 0x08`,
`WDATA0 0x10`, `RDATA0 0x18`, `EXT_WRITEL = 0`, `EXT_READL = 1`, `fmt_cmd_v2 = (opc << 27) | ((addr & 0xff) << 4) |
(bc & 7)`. Per v5 il canale observer è a `0x10000·ee + 0x80·apid` dalla base `obsrvr`. Il canale di scrittura è a
**`0x10000·apid` dalla base `chnls` (`0x0C600000`)** ed è ammesso solo se il proprietario in `cnfg` è il nostro EE
(`"disallowed SPMI write"`, righe 1015-1023). Per scrivere: `WDATA0` prima, poi `CMD` con BC = lunghezza − 1, poi
attesa dello `STATUS`. Questo percorso di scrittura è già provato su SM8150 da HakiOS `vayu_wled.c:96-300` (PM8150L
SID 5) con controllo del proprietario, elenco chiuso dei registri, rilettura e tre tentativi su "dropped".

### 4.2 `sweet_keys.c` (patch 0060) e PON

Stesso schema in sola lettura: legge `PON_RT 0x0810` del SID 0 (PPID `0x008`), cioè power e volume giù, più il GPIO2
del PM6150L (`0x4C1`, solo `sweet`). Sul Mi 9 il PON del PM8150 è all'APID 249 (`docs/root-image.md:247-248` del port
cepheus), e `sweetkeyspononly=1` (in `board.toml`) evita la parte GPIO. Mappa e lock sono suoi: due driver che usano
APID diversi non si disturbano, perché ogni canale observer è indipendente. Un futuro driver del caricatore deve usare
lo stesso lock di `sweet_spmi.c` per tutte le sue letture e scritture.

### 4.3 `SweetBattery.cpp` e cosa legge iOS

`iokit/Kernel/arm/SweetBattery.cpp` (0062, righe 618-938) è una sottoclasse di `IOPMPowerSource` registrata su
`IOResources`.

- **Senza parametro** pubblica 100 %, alimentazione esterna presente, non in carica: è lo stato attuale su `andromeda`.
- **Con `sweetbattery=1`** legge ogni 30 s il QG del **PM6150 sul SID 0**:
  - controlla che `0x4804` = `0x0D` (tipo QG) e che `0x4808` bit 0 sia a 1;
  - tensione da `0x48C0`, che `poll()` converte con `× 194637 / 10⁶`;
  - corrente da `0x48C2`;
  - poi `0x1310`, `0x110B`, `0x1006`, `0x1007`, `0x100D`;
  - SOC stimato da una curva di tensione; temperatura fissa a 25 °C.

  Su `andromeda` il controllo del tipo fallisce e resta il 100 % fisso: **non va acceso così com'è**.
- Proprietà pubblicate: `BatteryInstalled`, `CurrentCapacity`/`MaxCapacity` in mAh (5020, la `sweet`),
  `ExternalConnected`, `ExternalChargeCapable`, `IsCharging`, `AtWarnLevel`, `AtCriticalLevel` (sempre false),
  `Voltage`, `Amperage`, `TimeRemaining` (−1), `CycleCount`, `FullyCharged`, `AppleRawExternalConnected`,
  `AppleRawCurrentCapacity`, `AppleRawMaxCapacity`, `DesignCapacity`, `InstantAmperage`, `Temperature` (centesimi di
  °C), più il dizionario legacy `IOBatteryInfo`. Poi `updateStatus()`.
- Le chiavi sono quelle di XNU (✔ `iokit/IOKit/pwr_mgt/IOPM.h` di `xnu-8796.101.5`, righe 579-609, 638-650, 847-853):
  - `ExternalConnected`, `ExternalChargeCapable`, `BatteryInstalled`, `IsCharging`, `FullyCharged`;
  - `AtWarnLevel`, `AtCriticalLevel`;
  - `CurrentCapacity`, `MaxCapacity`, `DesignCapacity`, `TimeRemaining`, `Amperage`, `Voltage`, `CycleCount`;
  - `AdapterInfo`, `ErrorCondition`, `Temperature`, `ChargeStatus`, `BatteryHealth`;
  - `AdapterDetails` (`Watts`, `Current`, `FamilyCode`, `AdapterVoltage`, `Description`).
- Convenzioni: `Amperage` in mA, **positiva in carica** (il contrario del FG); `Voltage` in mV; `Temperature` in
  centesimi di grado; powerd calcola la percentuale come `CurrentCapacity × 100 / MaxCapacity`.

### 4.4 ios-mh2lm (LG G8X): stesso PMIC, stesso problema

[`shenli99/ios-mh2lm`](https://github.com/shenli99/ios-mh2lm) (`c4473d54`, base ios-sweet, documenti in cinese, **senza
licenza**: solo riferimento, niente codice copiato) è il lavoro più vicino al nostro:

- `xnu/patches/0115-mh2lm-battery-sid2.patch`: estende `sweet_spmi_read` a `sid ≤ 15` e alle periferiche `0x10`-`0x13`,
  `0x16`, `0x40`-`0x43`, `0x48`. `SweetBattery` legge MSOC `0x4009` solo se `0x4010` bit 1, `VBATT 0x41A0`, `0x4107`,
  `0x1006`, `0x1310`. Attenzione: il commento tratta `0x4009` come "già in %", mentre per Linux è 0-255.
- Patch 0150-0155 (descritte in `docs/BATTERY-SPMI-TRANSPORT.md`): trasporto condiviso con permessi per APID e
  scrittura con rilettura in un elenco chiuso (`0x1042`, `0x1043`, `0x1061`, `0x1070`, `0x1340`, `0x1342`, `0x1365`,
  `0x1366`, `0x1370`, `0x1380`), sessione DMA del FG, provider ADC5.
- Risultati sul G8X: watchdog `0x62/0x8c` con `en0`, ancora senza carezza (`BATTERY-DRIVER-NEXT.md:52`). La sessione
  `m19` ha caricato per 65 campioni, poi è passata a 7 col cavo inserito (`BATTERY-CHARGING-HISTORY.md`). Il firmware
  delle immagini riuscite e di quelle non riuscite è identico byte per byte: la differenza è nello stato lasciato
  all'avvio, non nel firmware.

### 4.5 UniXNU

- [`RigoLigoRLC/unixnu-nativeqcomspmi`](https://github.com/RigoLigoRLC/unixnu-nativeqcomspmi) (`ce3ccf0d`,
  BSD-2-Clause-Patent): libreria SPMI v5/v7 con `read`, `write` e `update8` da 1-8 byte. Tiene la tabella dei canali e
  dei proprietari e rifiuta la scrittura sui canali di altri EE (`NativeQcomSpmi.cpp:66-112`, `177-242`). La
  configurazione `sm-t733` ha le nostre stesse regioni. È la candidata migliore a sostituire `sweet_spmi.c` se serve un
  trasporto completo, e la licenza lo permette.
- [`UniXNU/UniXNU-sheng`](https://github.com/UniXNU/UniXNU-sheng) `package/ShengBattery.cpp` (SM8550, BSD-2-Clause-Patent):
  una sottoclasse `IOPMPowerSource` pulita, con work loop, timer, `TimeRemaining` e `AtCriticalLevel`. I dati vengono
  dal battery manager del DSP (pmic_glink), che sull'SM8150 non c'è: va riusata solo la parte iOS.
- Il core UniXNU non ha un driver di batteria; `docs/porting/power.md:143-181` è una guida generica.

### 4.6 HakiOS e hakios-sm8150

- [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) `63fc2b46`:
  - `drivers/power/hakios.battery.c/.cpp` è per il POCO X5 Pro (`redwood`, SM7325): i dati arrivano dal battery
    manager del DSP audio e da `QcomChargerDxe`, che il loader HakiOS tiene vivo sul core 0. **Non vale per l'SM8150.**
  - `hakios.pmic.c` è un inventario SPMI in sola lettura.
  - `hakios.battguard.c` (`battery_stop=`, `battery_stop_poll=`) ferma GPU, Wi-Fi e DSP e torna al loader dopo tre
    letture sotto soglia. È un'idea da riprendere.
  - La mappa `AdapterDetails`/`FamilyCode` di `hakios.battery.cpp:102-113` (SDP `0xE0004000`, DCP `…4004`,
    CDP `…4005`, Type-C `…4009`, PD `…400A`) è utile per iOS.
- [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) `3e7be137` (`vayu`, senza
  licenza; permesso già chiesto, vedi `richieste-permesso.md`):
  - lo stato dichiarato è "PM8150B over SPMI; does not charge under XNU and drains quickly, fuel gauge unverified";
  - il loro driver di batteria **non è pubblico**;
  - `vayu_cpufreq.c:683-715` legge `0x1310` bit 4, `0x1308` bit 0 (SDP) e `0x4009`/`0x400A` con `(raw·100+127)/255`, e
    abbassa la CPU quando il telefono è su porta PC sotto il 50 %;
  - i loro DXE dell'UEFI (`vayuPkg`) sono gli stessi Qualcomm.
- [`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus): nessun lavoro su batteria e caricatore in
  nessun ramo; `sweet_spmi.c` non è stato esteso. Quello che faremo vale identico per il Mi 9.

### 4.7 Linux mainline e la serie SMB5

- `drivers/power/supply/qcom_smbx.c`, già `qcom_pmi8998_charger.c` (Casey Connolly), supporta solo PMI8998/PM660:
  - spegne il watchdog all'avvio (righe 859-863);
  - accarezza `0x643` nell'IRQ di abbaio (765-773); `2f0dec120078` (Robin Snyders) ha corretto la carezza, che
    dimenticava la base del caricatore;
  - `1fba78714201` (Fabricio Akio, ottobre 2026): sul Moto X4 il bootloader lascia il watchdog armato "to disable
    charging when it bites", e la correzione riscrive `CHARGING_ENABLE_CMD` dopo `CHGR_CFG2`.
- **Serie SMB5 per PM8150B**: v1 di Casey Connolly (giugno 2025, non accettata), v2 di Robin Snyders con Casey Connolly
  e Joel Selvaraj (12 agosto 2026), provata su OnePlus 7T Pro (SM8150). Ordine all'avvio:
  1. sospende l'ingresso (`0x1340` = 1) e spegne la carica (`0x1042` = 0), "per non ereditare i limiti del bootloader";
  2. programma FV/FCC con le codifiche giuste e ICL a 500 mA;
  3. spegne il watchdog (`0x1653 = 0`; `0x1651` bit 7, 6, 1 a 0);
  4. accende AICL;
  5. riaccende la carica e l'ingresso.
- FG: nessun driver PM8150B in mainline. `qcom_fg.c` di sm8150-mainline (Caleb Connolly, Joel Selvaraj, Yassine
  Oudjana; PM8150B aggiunto da map220v) usa la base `0x4000` e le stesse letture della sezione 3.1.

## 5. Piano

Ogni fase parte spenta e si accende con il suo parametro, come da `ROADMAP.md`. Nessuna fase scrive fuori dal PMIC, e
fino alla (b) non si scrive nemmeno lì.

### 5.0 Esperimento UEFI (opzionale, rapido, nessuna modifica al kernel)

Variante dell'UEFI con `EnableChargerWdog = 0` in `Platforms/SurfaceDuo1Pkg/Device/xiaomi-andromeda/Binaries/RawFiles/QcomChargerCfg.cfg`
(per esempio una variabile d'ambiente letta da `scripts/build-firmware.py`, che modifica una copia prima della build).

- Effetto atteso (~): il DXE non arma il watchdog.
- Variante B: anche `LoadBatteryProfile = FALSE`. L'UEFI non ricarica il profilo Alium e lascia nel FG quello caricato
  da Android (giusto per la batteria montata), e forse evita lo spegnimento della carica nel percorso del profilo.
- Prova: avvio con caricatore da muro, dopo 5 minuti si passa al cavo del PC. Il kernel rimanda gli ultimi 64 KiB di
  log, quindi si legge anche la storia; serve la sonda (a), o almeno il livello della batteria prima e dopo.
- Limiti: il comportamento del DXE con 0 è dedotto, non provato. Senza watchdog un XNU bloccato non ferma la carica; la
  terminazione hardware, il JEITA hardware e l'AFP a 62 °C restano attivi.

È un'ottima verifica dell'ipotesi, ma la soluzione definitiva resta nel kernel (fase b).

### 5.a Sonda in sola lettura

**Parametri:**

- `sweetchg=1` accende la sonda;
- `sweetchgpoll=<s>` imposta il periodo (2-300 s, predefinito 10);
- `sweetchgdump=1` aggiunge un dump completo all'avvio e a ogni cambio di stato del cavo.

Nessuna scrittura al PMIC.

**Codice:**

1. In `sweet_spmi.c`: ammettere il **SID 2** e un elenco chiuso di periferiche per la lettura (`0x01` REVID, `0x10`-`0x16`,
   `0x24`, `0x31`, `0x40`-`0x42`, `0x43` solo `0x00`-`0x5F` e `0x70`-`0x7F`, escludendo `RD_DATA` dell'IMA `0x4367`-`0x436A`).
   Aggiungere una funzione che restituisce APID, proprietario e flag IRQ di un PPID, per stampare a chi appartengono i
   canali. Il resto (lock, avvelenamento, `EXT_READL`) resta com'è.
2. Un servizio IOKit nuovo, oppure una modalità `andromeda` di `SweetBattery` (per esempio `sweetbattery=2`), che parte
   subito al matching su `IOResources` (prima di `launchd`) con un `IOTimerEventSource`.
3. **All'avvio**, una riga per gruppo:
   - REVID `0x0100`-`0x0105` (tipo/sottotipo, revisione);
   - tipo e sottotipo (`+0x04`/`+0x05`) di `0x10`-`0x16` e `0x40`-`0x43`;
   - APID e proprietario dei canali `0x210`, `0x213`, `0x216`, `0x241`, `0x243`;
   - configurazione: `0x1051`, `0x1061`, `0x1070`, `0x1072`, `0x107D`, `0x1090`-`0x109B`, `0x10A2`, `0x1153`,
     `0x1340`, `0x1342`, `0x1358`, `0x1360`, `0x1362`, `0x1363`, `0x1365`, `0x1366`, `0x1370`, `0x1380`, `0x1381`,
     `0x1440`, `0x1544`, `0x1648`, `0x1651`, `0x1653`, `0x1661`, `0x1670`, `0x1690`, `0x4046`, `0x4162`-`0x4165`.
4. **Ogni N secondi**, una riga compatta (~150 byte, quindi 64 KiB bastano per ~70 minuti di storia):

   ```text
   SWEET CHG t=123 rt=13:10 path=95 apsd=08/01 rp=04 st=3 en=1 s2=00 s7=00 icl=1500/1500 fcc=1000 fv=4400
             wd=62/8c misc=00 die=04/00 bat=00 msoc=a2(64%) soc_rdy=1 vb=4012mV ib=-912mA fgts=01 sys=00
   ```

   Più una riga `SWEET CHG EVENT` a ogni cambio di stato (st, en, cavo, abbaio, APSD, JEITA), con il tempo dall'avvio.
5. **Come leggere i risultati:**

| Osservazione | Significato | Prossimo passo |
|---|---|---|
| `wd=62/8c`, `en=0`, `st=7` col cavo | UEFI: watchdog armato, carica spenta | fase b1 |
| `wd=62/8c`, `en=1`, `st=3` per ~129 s dall'inserimento, poi `st=7` con `en=1` | morso del watchdog | fase b1 (carezza + toggle) |
| `st=3`/`4` stabile, `ib < 0` (corrente in carica), `vb` che sale | carica funzionante | solo la fase c |
| `st=3` ma `ib` ≈ 0 o positivo col PC; `icl=500` | ICL SDP sotto i consumi | caricatore da muro o Rp USB-C; poi b2 |
| `path` bit 6 o `0x1340` bit 0 | ingresso sospeso | b2 |
| `s7` bit 3 o 2, `fgts` bit 7 o 4 | JEITA duro: troppo caldo o troppo freddo | nessuna scrittura; aspettare |
| `st=0` con `vb` vicino a FV | INHIBIT: batteria piena | normale |
| `fv > 4450` o `fcc > 3700` | limiti fuori dal profilo | non accendere la fase b; capire chi li ha scritti |

6. **Prove** (mai lasciato incustodito, telefono su una superficie non infiammabile, batteria sopra il 30 %, un misuratore
   USB tra caricatore e telefono per avere la corrente d'ingresso senza fidarsi dei registri):
   - T1 avvio senza cavo, poi cavo PC al minuto 3;
   - T2 avvio col cavo PC;
   - T3 avvio con caricatore da muro USB-A 5 V/2 A, passaggio al PC dopo 5 minuti;
   - T4 caricatore USB-C (Rp 3 A);
   - T5 sessione lunga oltre 10 minuti.

### 5.b Driver minimo

`sweetchgctl=<livello>`, predefinito 0. Ogni livello include i precedenti. Le scritture passano da una
`sweet_spmi_write()` nuova che:

- accetta solo coppie (SID, registro, maschera) di un elenco chiuso compilato nel kernel;
- verifica che il proprietario del canale in `cnfg + 0x700 + 4·apid` sia EE 0;
- usa il canale `chnls + 0x10000·apid` (mappato una volta per APID): `WDATA0`, poi `CMD = (0 << 27) | ((addr & 0xff) << 4)`,
  poi `STATUS` con un timeout;
- **rilegge sempre** dall'observer e confronta;
- segnala ogni scrittura nel log (`SWEET CHG WRITE addr=… old=… new=… ok=…`);
- al primo errore disattiva tutte le scritture fino al riavvio.

| Livello | Scritture ammesse | Logica | Fatto quando |
|---|---|---|---|
| **b1** | `0x1643 = 0x01` (carezza), `0x1042` bit 0 (toggle 0 → 1) | ogni 10 s: lettura e controlli di sicurezza ([5.d](#5d-regole-di-sicurezza)); carezza ogni 30 s se `0x1651` bit 1 o 0 è a 1; se `en=0`, oppure `st=7` con `0x110B` bit 0 = 1 e nessun guasto, toggle; al massimo 1 toggle al minuto e 10 per avvio, poi solo log | caricatore da muro: `st` 3/4 per più di 10 min, `ib < 0`, MSOC che sale, nessun passaggio a 7 dopo 129 s |
| **b2** | `0x1340` bit 0 (solo a 0); ICL: `0x1370` ≤ `0x1E` (1,5 A), `0x1366` bit 0, `0x1342` bit 0, `0x1365` bit 4, come `smblib_set_icl_current` in modo HC | solo se la sonda mostra ingresso sospeso, o ICL a 100/500 mA con APSD DCP o Rp ≥ 1,5 A; con un SDP resta l'automatico dell'hardware | DCP: `icl` ~1,5 A e AICL fatto (`0x1610` bit 3) |
| **b3** | FCC `0x1061` ≤ `0x28` (2,0 A); finestra DMA del FG (`0x4340` bit 0, `0x4350` bit 7/5, `0x4371` bit 0) per la temperatura | FCC sopra 1,0 A solo con temperatura letta e valida tra 15 e 40 °C; altrimenti si torna al valore trovato | temperatura coerente (20-35 °C a riposo); FCC rispettata (`ib` ≤ FCC) |

**FV non si scrive in nessun livello.** Se l'UEFI lascia un FV ≤ 4,40 V (`0x50`), va bene così; se è sopra 4,45 V
(`0x55`), il driver spegne la carica (`0x1042 = 0`) e lo segnala. La tensione di fine carica di 4,45 V del profilo
Linux non serve: 4,40 V è più conservativa e fa bene alla batteria.

Il watchdog **non** si spegne e il suo tempo non si cambia: lo si accarezza. Se XNU va in panic o si blocca, al
massimo 129 s dopo la ricarica si ferma da sola. È il comportamento giusto per un kernel ancora in sviluppo.

### 5.c iOS: `IOPMPowerSource` con valori veri

Partire da `SweetBattery` (proprietà già giuste per iOS 16.7.16), con una modalità `andromeda` che legge il SID 2.
Come modello di pulizia del codice si può guardare `ShengBattery.cpp`.

| Proprietà | Sorgente | Note |
|---|---|---|
| `CurrentCapacity` / `MaxCapacity` / `DesignCapacity` | MSOC (3.1) × 3800 / 100; `MaxCapacity = DesignCapacity = 3800` | il FDT dà 3800 mAh per entrambi i profili; più avanti la capacità appresa (parola 285) |
| `Voltage` | `0x41A0` (con copia) | mV |
| `Amperage`, `InstantAmperage` | −`0x41A2` | mA, **positiva in carica** per iOS |
| `ExternalConnected`, `AppleRawExternalConnected` | `0x1310` bit 4 | |
| `ExternalChargeCapable` | `0x110B` bit 0 e non bit 6 | |
| `IsCharging` | cavo e `0x1006` ∈ {1, 2, 3, 4} | |
| `FullyCharged` | `0x1006` = 5, oppure 0 con MSOC ≥ 99 % | |
| `Temperature` | parola SRAM 328 (b3) × 25 | centesimi di °C; finché non c'è: 2500 e `SweetBatteryTemperatureEstimated = true` |
| `AdapterDetails` | APSD `0x1308`, Rp `0x1506`, ICL `0x1107` | `FamilyCode` come HakiOS; `Watts = 5 V × ICL` |
| `AtWarnLevel` | MSOC ≤ 10 % | |
| `AtCriticalLevel` | MSOC ≤ 2 % **e** `vb` < 3450 mV in due letture valide di fila | prima fase: sempre false, come oggi (un dato sbagliato non deve spegnere iOS) |
| `TimeRemaining`, `CycleCount` | −1; poi la parola 291 | |

Aggiornamento ogni 30 s, e subito a ogni cambio di cavo o di stato della carica. Se una lettura non è valida si tiene
l'ultima buona; senza nessuna lettura buona, 100 % come oggi, mai 0 %. Un "battguard" come quello di HakiOS è
opzionale: sotto il 5 % senza cavo, spegnimento pulito invece di lasciar scendere la batteria a 3,2 V.

### 5.d Regole di sicurezza

1. **Elenco chiuso**: nessuna scrittura fuori dalle coppie (registro, maschera) del livello acceso; mai `SHIP_MODE 0x1240`,
   `DCDC_CMD_OTG 0x1140` (e il GPIO12 del boost), `MISC_SMB_EN_CMD 0x1648` (SMB1390), `USBIN_ADAPTER_ALLOW 0x1360` e
   HVDCP `0x1362`, `TYPE_C_MODE_CFG 0x1544`, `0x1651`/`0x1653`, il JEITA `0x1090`-`0x109B`, il timer `0x10A2`, i
   registri del FG `0x4046`/`0x4048` e la SRAM in scrittura, PBS, SDAM, PON, i registri ≥ `0xD0` di ogni blocco
   (`SEC_ACCESS`).
2. **Limiti mai superati da noi**: FV non scritto (accettato ≤ 4,40 V, carica spenta sopra 4,45 V); FCC ≤ 2,0 A
   (Linux 3,7 A: solo riferimento); ICL ≤ 1,5 A senza PD.
3. **Codifiche verificate**: dopo ogni scrittura di un limite, rilettura. Se `vb` supera FV di 50 mV, oppure se la
   corrente di carica supera la FCC del 20 % per due letture, la carica si spegne.
4. **Temperatura**: carica solo tra 10 e 45 °C, quando la temperatura è disponibile (isteresi 2 °C); sempre stop con il
   JEITA duro (`0x100D` bit 3/2, `0x4106` bit 7/4), con il die in `SHDN`/`RST` (`0x1607` bit 3/2) o sopra gli 80 °C
   del FDT. Il **JEITA hardware lasciato dall'UEFI non si spegne** (Linux lo spegne solo perché lo sostituisce con il
   suo).
5. **Guasti**: `BAT_OV` (`0x1007` bit 1, `0x1210` bit 2), batteria o termistore mancanti (`0x1210` bit 4/5, `0x4107`),
   VPH OV, buck OC: carica spenta, nessun toggle fino al riavvio.
6. **Non combattere l'hardware**: se l'hardware rispegne la carica, al massimo 10 toggle per avvio, poi solo log.
7. **Watchdog accarezzato, non spento** (5.b).
8. **Tutto spento per default**, attivabile per livelli, con il log di ogni scrittura; la fase b solo dopo aver letto
   la sonda su almeno T1-T3.
9. **Prove fisiche sorvegliate**: misuratore USB, mai di notte, stop se il telefono scotta (sopra 40 °C al tatto o
   come lettura).

## 6. Tabella dei registri (PM8150B, SID 2)

`R` = letto dalla sonda, `W1`/`W2`/`W3` = scrivibile dal livello b1/b2/b3, `—` = mai scritto.

| Indirizzo | Nome | Bit / codifica | Uso | Cert. |
|---|---|---|---|---|
| `0x0100`-`0x0105` | REVID | revisione 1-4, tipo `0x51`, sottotipo PMIC (`0x20` = PM8150B) | R | ✔ / ~ (tipo) |
| `0x1006` | BATTERY_CHARGER_STATUS_1 | 2:0 stato (tabella 2.1) | R | ✔ |
| `0x1007` | STATUS_2 | 1 BAT_OV; 7 ingresso limitato, 6 timer di sicurezza scaduto, 4 terminale mancante (layout SMB2) | R | ✔ (bit 1) / ? (altri) |
| `0x100B` | STATUS_5 | 2 trickle, 1 pre, 0 fullon abilitati | R | ✔ |
| `0x100D` | STATUS_7 | 5 caldo morbido, 4 freddo morbido, 3 troppo caldo, 2 troppo freddo, 1/0 AFP | R | ✔ |
| `0x1010` | CHGR INT_RT_STS | 0 errore, 1 cambio stato, 6 VPH alarm | R | ✔ |
| `0x1042` | CHARGING_ENABLE_CMD | 0: 1 = carica abilitata | R, W1 | ✔ |
| `0x1043` | CHARGING_PAUSE_CMD | 0 | R | ✔ |
| `0x1051` | CHGR_CFG2 | 2:1 ricarica (SOC/VBAT), 0 INHIBIT | R | ✔ |
| `0x1061` | FAST_CHARGE_CURRENT_CFG | raw × 50 mA | R, W3 (≤ `0x28`) | ✔ |
| `0x1070` | FLOAT_VOLTAGE_CFG | 3,60 V + raw × 10 mV | R, — | ✔ |
| `0x1072` | CHARGE_INHIBIT_THRESHOLD | 1:0 FV−50/100/200/300 mV | R | ✔ |
| `0x107D` | CHARGE_RCHG_SOC_THRESHOLD | SOC × 255/100 | R | ✔ |
| `0x1090` | JEITA_EN_CFG | 3 FV caldo, 2 FV freddo, 1 CC caldo, 0 CC freddo (4 limiti duri, layout SMB2) | R | ✔ / ? (bit 4) |
| `0x1092`/`0x1093` | JEITA_CCCOMP hot/cold | raw × 25 mA | R | ✔ |
| `0x1094`-`0x109B` | soglie JEITA morbide/dure | codici ADC del termistore, big endian | R | ✔ (indirizzi) |
| `0x10A2` | FAST_CHARGE_SAFETY_TIMER | 0/1/2/3 = 192/384/768/1536 min | R | ✔ |
| `0x1106`/`0x1107`/`0x1108` | ICL max / ICL / AICL ICL | raw × 50 mA | R | ✔ |
| `0x110A` | AICL_STATUS | 6 soft ilimit, 0 AICL fatto | R | ✔ |
| `0x110B` | POWER_PATH_STATUS | 6 USBIN sospeso, 4 usa USBIN, 3 usa DCIN, 0 ingresso valido | R | ✔ |
| `0x1110` | DCDC INT_RT_STS | 5 limitazione ingresso, 7 switcher ok | R | ✔ |
| `0x1140` | DCDC_CMD_OTG | 0 OTG | R, — | ✔ |
| `0x1153` | DCDC_OTG_CFG | 1 sorgente OTG | R | ✔ |
| `0x1210` | BATIF INT_RT_STS | 7 VPH OV, 6 buck OC, 5 terminale mancante, 4 termistore/ID mancante, 3 batt. bassa, 2 OV, 0 temp | R | ✔ |
| `0x1240` | SHIP_MODE | 0 | — (mai) | ✔ |
| `0x1307` | APSD_STATUS | 0 rilevamento fatto, 1 QC, 4 enumerazione fatta, 6 timeout HVDCP | R | ✔ |
| `0x1308` | APSD_RESULT_STATUS | 0 SDP, 1 OCP, 2 CDP, 3 DCP, 4 float, 5 QC2, 6 QC3 | R | ✔ |
| `0x1310` | USBIN INT_RT_STS | 4 cavo, 3 OV, 2 UV, 0 crollo, 7 cambio ICL | R | ✔ |
| `0x1340` | USBIN_CMD_IL | 0 sospendi | R, W2 (solo a 0) | ✔ |
| `0x1342` | CMD_ICL_OVERRIDE | 0 | R, W2 | ✔ |
| `0x1358` | TYPE_C_CFG | 7 BC1.2 su CC | R | ✔ |
| `0x1360` | USBIN_ADAPTER_ALLOW_CFG | 0 = solo 5 V | R, — | ✔ |
| `0x1362` | USBIN_OPTIONS_1 | 6 autenticazione HVDCP, 5 autonomo, 3 rilevamento BC1.2, 2 HVDCP | R, — | ✔ |
| `0x1363` | USBIN_OPTIONS_2 | 2:0 opzioni float | R | ✔ |
| `0x1365` | USBIN_LOAD_CFG | 4 override ICL dopo APSD | R, W2 | ✔ |
| `0x1366` | USBIN_ICL_OPTIONS | 2 USB3, 1 USB51 (500 mA), 0 modo HC | R, W2 | ✔ |
| `0x1370` | USBIN_CURRENT_LIMIT_CFG | raw × 50 mA | R, W2 (≤ `0x1E`) | ✔ |
| `0x1380` | USBIN_AICL_OPTIONS | 7 sospendi al crollo, 4 riavvio periodico, 3 ADC, 2 AICL | R | ✔ |
| `0x1440` | DCIN_CMD_IL | 0 sospendi DCIN | R | ✔ |
| `0x1506` | TYPE_C_SNK_STATUS | 3 Rp std, 2 Rp 1,5 A, 1 Rp 3 A | R | ✔ |
| `0x150B` | TYPE_C_MISC_STATUS | 0 CC collegato, 1 orientamento, 6 modo sorgente | R | ✔ |
| `0x1544` | TYPE_C_MODE_CFG | ruolo | R, — | ✔ |
| `0x1606` | TEMP_RANGE_STATUS | 6 regolazione termica attiva, 5 TLIM, 3/2/1 sopra/dentro/sotto | R | ✔ |
| `0x1607`/`0x1609` | DIE / CONNECTOR_TEMP_STATUS | 3 SHDN, 2 RST, 1 UB, 0 LB | R | ✔ |
| `0x1610` | MISC INT_RT_STS | 0 snarl, 1 abbaio, 2 AICL fallito, 3 AICL fatto, 4 smb-en | R | ✔ |
| `0x1643` | BARK_BITE_WDOG_PET | 0: scrivere 1 = carezza | W1 | ✔ |
| `0x1648` | MISC_SMB_EN_CMD | 0 CP, 1 CP FPF, 2 STAT, 3/4 override | R, — | ✔ |
| `0x1651` | WD_CFG | 7 AFP al morso, 6 IRQ abbaio, 5 IRQ morso (~), 1 timer all'inserimento, 0 timer sempre | R, — | ✔ / ~ (bit 5) |
| `0x1653` | SNARL_BARK_BITE_WD_CFG | 7 **il morso disabilita la carica**; 6:4 snarl (0 = 62,5 ms, 7 = 8 s); 3:2 abbaio 16·2ⁿ s; 1:0 morso (3 = 8 s, 0 = 1 s ~) | R, — | ✔ / ~ |
| `0x1661` | AICL_RERUN_TIME_CFG | 1 = 12 s | R | ✔ |
| `0x1670` | MISC_THERMREG_SRC_CFG | sorgenti della regolazione termica | R | ✔ |
| `0x4006` | FG_ALG_STS | | R | ✔ |
| `0x4009`/`0x400A` | MSOC / copia | 0-255 | R | ✔ |
| `0x4010` | BATT_SOC INT_RT_STS | 1 SOC_READY, 5 MSOC_EMPTY | R | ✔ |
| `0x4046` | BATT_SOC_EN_CTL | 7 algoritmo FG | R, — | ✔ |
| `0x4106` | BATT_TEMP_STS | 7/6/5/4 JEITA del FG, 0 disponibile | R | ✔ |
| `0x4107` | SYS_BATT | 2/1/0 batteria mancante | R | ✔ |
| `0x4109` | FG_STS | 7 reset WD del FG, 0 terminazione | R | ✔ |
| `0x4162`-`0x4165` | soglie JEITA del FG | | R | ✔ (indirizzi) / ? (scala) |
| `0x41A0`-`0x41A3` | VBATT / IBATT | 122,070 µV; s16 × 488,281 µA | R | ✔ |
| `0x41A6`-`0x41A9` | copie VBATT / IBATT | | R | ✔ |
| `0x4210` | RRADC INT_RT_STS | 0 batteria mancante | R | ✔ |
| `0x4288`/`0x4289` | RRADC BATT_TEMP | 10 bit, scala ignota | R | ? |
| `0x4310` | MEM_IF INT_RT_STS | 3 `GEN4_MEM_GNT`, 2 DMA XCP | R | ✔ |
| `0x4340` | MEM_ARB_CFG | 0 richiesta | W3 | ✔ |
| `0x4350` | MEM_INTF_CFG | 7 richiesta di accesso, 5 IACS | W3 | ✔ |
| `0x4370`/`0x4371` | DMA_STS / DMA_CTL | 1/2 errori; 0 azzera | R, W3 | ✔ |
| `0x4858`/`0x4859` | SRAM parola 328 (DMA4) | s10 × 0,25 °C | R dopo DMA | ✔ formula / ~ indirizzo |

## 7. Domande aperte

1. **Cosa c'è davvero nei registri quando parte XNU**: `0x1651`/`0x1653`, `0x1042`, FV/FCC/ICL. Risponde la sonda (a) in
   T1-T3.
2. **Quale sezione della configurazione UEFI viene applicata**: `[PlatformE5G]` (4,40 V / 1 A) o `[PlatformMTP]`
   (4,35 V / 2 A)? Dipende dal tipo di piattaforma che Aloha presenta. Si vede da `0x1070`/`0x1061`.
3. **Quale batteria c'è nel telefono** (Coslight `e5g_cos` o Alium): i due profili hanno lo stesso ID da 100 kΩ, e l'UEFI
   carica sempre Alium. Da leggere una volta in Android (`/sys/class/power_supply/bms/battery_type`, lo fa l'utente)
   oppure dalla parola SRAM 299 via DMA.
4. **Il percorso del profilo spegne davvero la carica (`0x1042 = 0`) anche su `andromeda`?** Sul G8X lo stato iniziale
   cambiava da un avvio all'altro, con lo stesso firmware.
5. **Bit 5 di `0x1651`, codifica del morso in `0x1653`, e un registro di stato del morso**: c'è, o si deduce solo da
   "stato 7 con `en=1`"? Il dump completo di MISC (`sweetchgdump=1`) prima e dopo un morso può rispondere.
6. **Bit di `0x1007` sullo SMB5** (timer di sicurezza scaduto?): Linux SMB5 definisce solo il bit 1.
7. **ICL in modalità automatica** con DCP e Rp 1,5/3 A: quanto sceglie l'hardware da solo, senza driver? Se basta, la
   b2 non serve.
8. **Proprietari dei canali SPMI del SID 2**: ci aspettiamo EE 0 (Android li scrive dall'APPS). La sonda li stampa;
   se uno non è nostro, quella scrittura è vietata.
9. **Scala di `0x4288`** e revisione del PM8150B (serve la correzione DMA della v1?).
10. **Consumo del telefono sotto XNU** contro i 500 mA del PC: va misurato con il misuratore USB. Può darsi che con il PC
    non si arrivi mai al pareggio, e allora per le prove lunghe serve un caricatore.
11. **APSD con il nostro USB in modalità device**: il rilevamento BC1.2 usa D+/D− mentre il nostro stack USB tiene la
    PHY; il PC viene visto come SDP o CDP? (CDP darebbe 1,5 A.)
12. **L'AFP del PBS a 62 °C e il JEITA hardware sono davvero programmati dall'UEFI di Aloha?** Le soglie
    `0x1094`-`0x109B` sono codici ADC del termistore: vanno convertite con la tabella del termistore (beta 4250, pull-up
    100 kΩ) per un confronto con la configurazione.
13. **Il DXE con `EnableChargerWdog = 0`** (5.0) disarma davvero il watchdog? Il comportamento viene da sorgenti non
    ufficiali, non provato.
14. Le stesse correzioni valgono per il Mi 9 (`cepheus`, UEFI Aloha con gli stessi DXE) e per il POCO X3 Pro: da
    proporre ai rispettivi port quando funzionano.

## 8. Fonti

Grazie a tutti gli autori: quasi tutto quello che c'è qui è lavoro loro.

- **Kernel Xiaomi** (codice di The Linux Foundation/Qualcomm e Xiaomi, GPL-2.0):
  [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) ramo `andromeda-p-oss`
  `74347f5521fb444936f5986ff0937264307d78fe`:
  - `drivers/power/supply/qcom/`: `qpnp-smb5.c`, `smb5-lib.c`, `smb5-lib.h`, `smb5-reg.h`, `qpnp-fg-gen4.c`,
    `fg-util.c`, `fg-memif.c`, `fg-reg.h`, `fg-core.h`, `smb1390-charger-psy.c`;
  - `drivers/spmi/spmi-pmic-arb.c`, `drivers/iio/adc/qcom-spmi-adc5.c`, `qcom-vadc-common.h`;
  - `include/linux/qpnp/qpnp-revid.h`.
- **Project Aloha**, Kancy Joe (`sunflower2333`) e gli altri autori:
  [`Project-Aloha/mu_aloha_platforms`](https://github.com/Project-Aloha/mu_aloha_platforms) `0b798128`.
  - File: `Platforms/SurfaceDuo1Pkg/Device/xiaomi-andromeda/APRIORI.inc`, `DXE.inc`,
    `Binaries/RawFiles/QcomChargerCfg.cfg`, `uefiplat.cfg`, `BATTERY.PROVISION`, `BATTERY.PROVISION1`; stringhe di
    `Binaries/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.efi`; `QcomModulePkg/Library/BootLib/UpdateCmdLine.c`.
  - I binari e la configurazione sono di Qualcomm e Xiaomi.
- **ios-sweet**, dipa-studio: [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) `45263bee`:
  `xnu/patches/0060-sweet-keys-touch-drivers.patch` (`sweet_keys.c`), `0062-sweet-touch-power-battery.patch`
  (`sweet_spmi.c`, `sweet_spmi.h`, `SweetBattery.cpp`), `README.md`, `NOTES.md`.
- **ios-mh2lm**, shenli99 (LG G8X; senza licenza, solo riferimento):
  [`shenli99/ios-mh2lm`](https://github.com/shenli99/ios-mh2lm) `c4473d5455f8f07ab6b7bb81e8ac55f2fb0164c6`:
  - patch: `xnu/patches/0115-mh2lm-battery-sid2.patch`, `0116-mh2lm-battery-publish.patch`;
  - documenti: `docs/BATTERY-CHARGING-HISTORY.md`, `BATTERY-DRIVER-NEXT.md`, `BATTERY-SPMI-TRANSPORT.md`,
    `BATTERY-FG-DMA-RECOVERY.md`, `BATTERY-HARDWARE-INTERFACES.md`, `BATTERY-RUNTIME.md`.
- **Linux mainline** (torvalds/linux): `drivers/power/supply/qcom_smbx.c` di Casey (Caleb) Connolly, Linaro.
  - Commit: `5ec53bcc7fce` (rinomina, Casey Connolly), `2f0dec120078` (Robin Snyders), `1fba78714201` (Fabricio Akio;
    maintainer Sebastian Reichel).
  - Serie SMB5: v1 di Casey Connolly (`20250619-smb2-smb5-support-v1`, con le revisioni di Dmitry Baryshkov e Luca
    Weiss) e v2 di Robin Snyders con Casey Connolly e Joel Selvaraj (`20260812-submit-qcom-smbx-send-v1-v2`).
- **hotdog-linux-bringup**, Robin Snyders (`Sr-0w`): `docs/evidence/2026-08-04-mainline616-smb5-parameters.md`,
  `2026-08-19-pd-hard-reset-charge-gap.md`, patch `0020` e `0155`.
- **sm8150-mainline** ([gitlab.com/sm8150-mainline/linux](https://gitlab.com/sm8150-mainline/linux), ramo `sm8150/6.17`):
  `qcom_fg.c` (Caleb Connolly, Joel Selvaraj, Yassine Oudjana; PM8150B di map220v, `b1c33086fa6f`), `pm8150b.dtsi`.
- **ROCKNIX**: `qcom_pm8150b_charger.c` di Teguh Sobirin.
- **HakiOS**, OOSEX GANG (shimoku74, vcvkk, oosexgang-developers):
  [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`:
  `drivers/power/hakios.battery.c`, `hakios.battery.cpp`, `hakios.battguard.c`, `hakios.pmic.c`,
  `drivers/input/hakios.buttons.c`, `xnuloader/loader.c`, `docs/devices/redwood.md`.
- **HakiOS SM8150**, nomorecoolnicknames:
  [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) `3e7be137`: `README.md`,
  `device.conf`, `kernel/bsd/dev/vayu_cpufreq.c`, `vayu_wled.c`, `vayu_haptic.c`, `uefi/files/.../vayuPkg/Include/DXE.inc`.
- **UniXNU**:
  - [`RigoLigoRLC/unixnu-nativeqcomspmi`](https://github.com/RigoLigoRLC/unixnu-nativeqcomspmi) `ce3ccf0d` (RigoLigoRLC);
  - [`RigoLigoRLC/unixnu-sm-t733`](https://github.com/RigoLigoRLC/unixnu-sm-t733) `4379151e`;
  - [`UniXNU/UniXNU-sheng`](https://github.com/UniXNU/UniXNU-sheng) `516a7804` (`package/ShengBattery.cpp`,
    `ShengBattmgr.c`, di rponeawa e bluseliu50);
  - [`UniXNU/UniXNU`](https://github.com/UniXNU/UniXNU) `10edc1ef` (`docs/porting/power.md`).
- **UniXNU-cepheus**, YangQi0408: [`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus) rami
  `master`, `feat/adreno640` (`docs/root-image.md`, `docs/panic-fastboot.md`).
- **Apple**: [`apple-oss-distributions/xnu`](https://github.com/apple-oss-distributions/xnu) `xnu-8796.101.5`,
  `iokit/IOKit/pwr_mgt/IOPM.h`.
- **Windows su SM8150**: `erdilS/Port-Windows-11-Xiaomi-Pad-5` issue #389 (Meari-Prototype): FV codificato male dai driver
  del Surface Duo, ingresso sospeso, batteria che si scarica.
- **Dati del telefono**: `phone/<seriale>/live-fdt.dts` (fuori da git).
