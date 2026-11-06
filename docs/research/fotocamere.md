# Fotocamere su `andromeda`: fattibilità, stato dell'arte e primi passi

Ricerca del 6 novembre 2026, in sola lettura. Nessun comando sul telefono e nessun codice del kernel scritto. I valori
del Mi Mix 3 5G vengono da tre fonti:

- il device tree attivo del telefono (`phone/<seriale>/live-fdt.dts`, fuori da git; qui riporto solo i valori che
  servono);
- le proprietà Android salvate da `scripts/pull-phone-info.sh` (`phone/<seriale>/info/getprop.txt`);
- il kernel Xiaomi `andromeda-p-oss`.

Gli altri fatti vengono dai file elencati in [Fonti](#8-fonti).

Legenda della certezza:

- **✔**: letto nel sorgente, nel device tree, oppure misurato da altri su hardware uguale;
- **~**: dedotto, coerente con più indizi, ma non ancora visto su `andromeda`;
- **?**: da verificare, con la sonda [C0](#c0-sonda-in-sola-lettura-andrcam1) o con i file del vendor.

Quando un risultato è di un altro progetto e l'ho solo letto nei suoi documenti, lo scrivo come **dichiarato** da loro.

## Sommario

- **Si può fare, e altri l'hanno già fatto sulla stessa base.** Quattro port XNU/iOS 16 hanno la fotocamera
  funzionante. Nessuno usa l'ISP di Apple: tutti leggono i frame grezzi dal percorso RDI (CSIPHY → CSID → IFE →
  memoria) e fanno l'elaborazione dell'immagine in software (✔ nel loro codice). I quattro port:
  - **HakiOS** sul POCO X5 Pro: dichiara la Camera.app con anteprima e foto.
  - **`ios-sunfish`** sul Pixel 4a (Titan 175 come noi): dichiara la Camera.app con anteprima e foto. Usa lo
    **stesso sensore principale, il Sony IMX363**.
  - **`ios-redfin`** sul Pixel 5: dichiara la Camera.app con anteprima e foto, anche lui con l'**IMX363**.
  - **`hakios-sm8150`** sul POCO X3 Pro, **stesso SoC SM8150**: dichiara foto dal sensore principale e video a
    ~7,6 fps.
- **L'hardware (✔ FDT, getprop).** Ci sono quattro sensori, tutti sul controller **CCI0**:
  - posteriori: principale **Sony IMX363** (modulo SEMCO) e tele **Samsung S5K3M3** (SEMCO, **non** S5K3M5);
  - nello slider: frontale **Sony IMX576** (Sunny) e profondità **OmniVision OV02A10** (Sunny).

  Le alimentazioni sono queste:
  - VIO 1,8 V da un regolatore acceso dal GPIO 34;
  - un GPIO per sensore (21, 29, 32, 33), che secondo Xiaomi accende insieme "DVDD e AVDD" (LDO esterni alimentati da
    BOB e da S8C a 1,352 V);
  - in più L17A, MCLK a 19,2 MHz e reset su GPIO.

  Lo slider ha **due sensori Hall sui GPIO 42 e 114**, che bastano leggere. Il flash è nel **PM8150L**.
- **Linux.** Il CAMSS per SM8150 non è in mainline. Ci sono due lavori che lo coprono:
  - la serie v3 di **Andrew Gigena**, pubblicata il 6/11/2026;
  - **Robin Snyders**, che dichiara le quattro fotocamere del OnePlus 7T Pro (SM8150) funzionanti con CAMSS,
    libcamera e il suo ISP software.

  IMX576 e OV02A10 hanno driver mainline, l'IMX363 solo fuori albero, l'S5K3M3 nessuno. È tutto GPL: per noi vale solo
  come documentazione.
- **La Camera di Apple non è realistica.** Su iOS 16 la catena è Camera.app → AVFoundation → `mediaserverd` →
  `H10ISP.mediacapture` → `AppleH10CamIn` → coprocessore ISP con firmware Apple. `cameracaptured` arriva solo con
  iOS 18. Imitare `AppleH10CamIn` vuol dire imitare il protocollo del firmware ISP, e nessuno ci ha provato oltre uno
  stub che registra le chiamate.
- **La Camera.app si può raggiungere in un altro modo.** HakiOS, sunfish e redfin mettono dentro `mediaserverd` una
  `FigCaptureDevice` propria. Però l'anteprima richiede la GPU, e oggi i nostri tweak spengono `StartRecorderServer`
  perché senza GPU `mediaserverd` va in crash.
- **La strada per noi è uno strumento nostro.** Un driver kernel spento per default (`andrcam=N`) prende i frame RAW10
  dal percorso RDI. In user space l'ISP software fa debayer, esposizione automatica e bilanciamento del bianco. Prima
  arriva uno strumento da riga di comando, poi un'app UIKit disegnata dalla CPU, installata come le altre app non
  cifrate.
- **Il primo traguardo (C1)** accende solo l'IMX363, legge `0x0016` = `0x0363` e lo scrive in una riga
  `ANDROMEDA CAM:`, poi spegne tutto. Prima viene la **C0**, una sonda in sola lettura su clock, GDSC, pin, Hall, PMIC
  e SMMU.
- **Rischi principali:**
  - la **SMMU e l'hypervisor**: su sunfish, una scrittura dell'IFE da uno stream non registrato ha riavviato il
    telefono;
  - la **memoria protetta**;
  - i **rail condivisi**: L3C con il DSI, BOB con l'audio;
  - la **sequenza di accensione**, che sta nel file del modulo nel vendor Android del telefono e non nel device tree.
- **Stima dei tempi:**
  - C0 + C1: 1-2 settimane;
  - primo frame grezzo: altre 3-6 settimane;
  - foto salvata e strumento: 1-2 settimane;
  - app: 1-2 settimane;
  - la Camera.app di Apple solo dopo la GPU.

---

## 1. L'hardware di `andromeda`

Il nome dei sensori non è nel device tree. Lo dà Android (✔ `getprop`):

- `persist.vendor.camera.mi.module.info` = `back_camera=semco_Sony_imx363_I;front_camera=sunny_Sony_imx576_I;`;
- `persist.vendor.camera.mi.module.infoext` = `back_camera_aux=semco_Samsung_s5k3m3_I;front_camera_aux=sunny_OV_ov02a10_I;`.

Coincide con le liste dei blob del vendor (`com.qti.sensormodule.*`) di tequilaOS e PixelExperience e con
[`VALUTAZIONE-PORT.md`](../../../VALUTAZIONE-PORT.md).

### 1.1 I quattro sensori

Nodi `qcom,cam-sensor@N` sotto `qcom,cci@ac4a000`. Sono uguali in `andromeda-sm8150-camera-sensor.dtsi` di Xiaomi e
sono comuni a tutte le revisioni di scheda: i file `andromeda-pinctrl-p*.dtsi` non toccano i pin della fotocamera ✔.

| Slot | Ruolo | Sensore | CSIPHY | Bus I²C (pin SDA/SCL) | MCLK | Reset / power-down | Abilitazione DVDD+AVDD | Orientamento | Accessori |
|---|---|---|---|---|---|---|---|---|---|
| `@0` | posteriore principale ("wide") | **Sony IMX363** 12 MP | 0 | CCI0 master 0 (GPIO 17/18) | MCLK0, GPIO 13 | `CAM_RESET_W` GPIO 28 | `CAM_VDD0_EN` GPIO 21 | roll 90, yaw 180 | attuatore AF `@0`, OIS `@0`, EEPROM `@0`, flash `@0` |
| `@1` | posteriore tele | **Samsung S5K3M3** 12 MP | 1 | CCI0 master 1 (GPIO 19/20) | MCLK1, GPIO 14 | `CAM_RESET_T` GPIO 30 | `CAM_VDD1_EN` GPIO 29 | roll 90, yaw 180 | attuatore `@1`, EEPROM `@1`, flash `@1` |
| `@2` | frontale (slider) | **Sony IMX576** 24 MP | 2 | CCI0 master 1 (GPIO 19/20) | MCLK2, GPIO 15 | `CAM_RESET_F` GPIO 12 | `CAM_VDD2_EN` GPIO 32 | roll 270, yaw 0 | EEPROM `@2`, flash `@2` (?) |
| `@3` | frontale di profondità ("ultra" nel DT) | **OmniVision OV02A10** 2 MP | 3 | CCI0 master 0 (GPIO 17/18) | MCLK3, GPIO 16 | `CAM_PWD_U` GPIO 23 (`gpio-standby`) | `CAM_VDD3_EN` GPIO 33 | roll 270, yaw 0 | EEPROM `@3` a `0xA2` (8 bit) |

Note ✔ dal DT e dal sorgente Xiaomi:

- **Il secondo GPIO di ogni sensore accende due LDO esterni.** Il DT lo chiama `gpio-vana`. Il commento Xiaomi in
  `andromeda-pinctrl.dtsi` dice "WIDE RESET DVDDEN AVDDEN 1.8AVDDEN", e la stessa cosa per tele, front e "ultra".
- **MCLK a 19,2 MHz** (`clock-rates = 19200000`) per tutti e quattro, cioè l'XO senza PLL.
- **Il controller CCI1** (`0xac4b000`) è `disabled`, perché i suoi pin 31-34 sono usati come GPIO di alimentazione.
- **Tutti i sensori stanno su CCI0**, divisi tra master 0 (IMX363, OV02A10) e master 1 (S5K3M3, IMX576). Due sensori
  sullo stesso master devono avere indirizzi I²C diversi.
- **Probabile svista Xiaomi:** `eeprom@1` (il tele) usa MCLK0 e il master 0, non MCLK1 e il master 1. Va tenuto
  presente se un giorno leggiamo quella EEPROM.
- **OIS:** il defconfig abilita `CONFIG_USE_ROHM_BU64753` (`drivers/media/platform/msm/Kconfig`). È con buona
  probabilità il controller OIS/AF del modulo principale (~). Il suo firmware (`andromeda_ois.prog`/`.coeff`) sta nel
  vendor.

**Indirizzi e ID dei sensori**, da verificare sul nostro telefono:

| Sensore | Registro e valore dell'ID | Indirizzo I²C a 7 bit | Fonte |
|---|---|---|---|
| IMX363 | `0x0016`-`0x0017` = **`0x0363`** | **0x1A** su Pixel 4a e Pixel 5 ✔ (loro misura); 0x10 nel driver per il Poco F1 | sunfish (cam2), redfin `0919`, driver di Joel Selvaraj |
| S5K3M3 | `0x0000`-`0x0001` = `0x30D3` | ? | una sola fonte fuori albero (`s5k-cis.c` del Galaxy S10): **?** |
| IMX576 | `0x0016`-`0x0017` = **`0x0576`** | 0x10 nell'esempio del binding | `imx576.c` mainline ✔ |
| OV02A10 | `0x02`-`0x03` = **`0x2509`** (pagina 0) | 0x3D nell'esempio del binding | `ov02a10.c` mainline ✔ |

Gli indirizzi veri di `andromeda`, insieme alla sequenza di accensione e alle tabelle dei registri, stanno nei file
`com.qti.sensormodule.<modulo>.bin` del vendor ([§1.7](#17-cosa-non-cè-nel-device-tree)).

### 1.2 Alimentazioni

| Nome nel DT | Regolatore | Tensione (DT) | Carico (DT) | Chi lo usa (DT) | Note |
|---|---|---|---|---|---|
| `cam_vio` | `camera_viodd_ldo`, regolatore fisso acceso dal **GPIO 34**, alimentato da **S4A** (1,8 V di sistema) | 1,8 V | 1 mA | i 4 sensori e le 4 EEPROM | rampa 135 µs ✔ |
| `cam_vana` | **BOB** del PM8150L (`bobc1`) | 3,008-4,0 V | 80 mA | i 4 sensori | BOB alimenta gli LDO AVDD esterni accesi dal GPIO del sensore (~). **Condiviso con l'audio** (codec): non si cambia ✔ |
| `cam_vdig` | **S8C** del PM8150L (`smpc8`) | 1,352 V fissi | 1,2 A | sensori 0-2 (non l'OV02A10) | nel DT lo usano solo le fotocamere ✔; con buona probabilità alimenta gli LDO DVDD esterni (~) |
| `cam_v_custom1` | **L17A** del PM8150 (`ldoa17`) | 2,856-3,008 V | 10 mA (`@0`), 1 mA (`@1`, `@2`) | sensori 0-2 | nel DT lo usano solo le fotocamere ✔; il suo ruolo è da leggere nel modulo (?) |
| `mipi-csi-vdd` | **L3C** del PM8150L (`ldoc3`) | 1,2 V | — | CSIPHY 0-3 | **condiviso** con DSI PHY, USB e PCIe: già acceso, **mai spegnerlo** ✔ |
| `cam_vaf` (AF) | regolatori fissi da 2,8 V accesi da **GPIO 26** (wide) e **GPIO 31** (tele), alimentati da BOB | 2,8 V | — | attuatori | |
| `cam_vaf` (OIS) | `camera_ois_regulator`, **GPIO 24** | 2,8 V | — | OIS `@0` | |
| `cam_clk` | GDSC `titan_top` | — | — | tutti | il genitore è **MMCX** (`VDD_MMCX_LEVEL`), lo stesso rail del display (~ già acceso) |

I nomi delle risorse RPMh sono ✔ nel DT: `bobc1`, `smpc8`, `ldoa17`, `ldoc3`. La patch 0092 (Bluetooth) ha già una
funzione che vota rail RPMh da un elenco chiuso, con la tensione esatta del DT (`andromeda_rpmh_bt_rail`, sul modello di
ios-flame 0102). Per la fotocamera basta aggiungere `smpc8` a 1352 mV e, se il modulo lo chiede, `ldoa17` a 2856 mV, il
minimo del DT.

### 1.3 Il sottosistema Titan 175

| Blocco | Indirizzo (DT ✔) | Note |
|---|---|---|
| camcc | `0x0ad00000` (+0x10000), `qcom,camcc-sm8150-v2` | `vdd_mm` = MMCX, `vdd_mx` = MX |
| GDSC `titan_top` | `0x0ad0c1bc` | bit 0 SW_COLLAPSE, bit 31 PWR_ON (mainline `camcc-sm8150.c`: `gdscr = 0xc1bc`) ✔ |
| RCG/CBCR MCLKn | camcc + `0x5004 + 0x20·n` / `0x501c + 0x20·n` | tabella mainline: 19,2 MHz da XO, 24 MHz da PLL2 (Regera, camcc + 0x2000) ✔ |
| CCI0: RCG / CBCR | camcc + `0xc108` / `0xc120` | 19,2 MHz da XO oppure 37,5 MHz da PLL0 (il DT vuole 37,5) ✔ |
| altri rami | `cpas_ahb` 0xc168, `camnoc_axi` 0xc18c, `core_ahb` 0xc1c8, `slow_ahb` RCG 0x7058 | ✔ mainline |
| GCC (rami della fotocamera) | `camera_ahb` 0x10b008, `camera_hf_axi` 0x10b030, `camera_sf_axi` 0x10b034, `camera_xo` 0x10b044 | ✔ `gcc-sm8150.c` |
| CCI0 | `0x0ac4a000`, IRQ SPI 460, `qcom,cci` | registri come `i2c-qcom-cci.c` (mainline, compatibile `msm8996-cci`) |
| CSIPHY 0-3 | `0x0ac65000` / `66000` / `67000` / `68000`, `qcom,csiphy-v1.1` | timer a 300 MHz, `cphy_rx` a 400 MHz |
| CSID0 / VFE0 | `0x0acb3000` / `0x0acaf000` (`csid175`, `vfe175`) | |
| CSID1 / VFE1 | `0x0acba000` / `0x0acb6000` | |
| CSID-lite0 / VFE-lite0 | `0x0acc8000` / `0x0acc4000` (`csid-lite175`, `vfe-lite175`) | il percorso usato da vayu e da sunfish |
| CSID-lite1 / VFE-lite1 | `0x0accf000` / `0x0accb000` | |
| CPAS / CAMNOC | `0x0ac40000` / `0x0ac42000` | voti AHB/AXI, QoS |
| ICP (A5), IPE, BPS, JPEG, FD, LRME | `0x0ac00000`… | **non servono** al percorso RDI; l'ICP vuole `CAMERA_ICP.elf` nella regione `camera_region` `0x8b700000` (5 MiB, `no-map`) |
| SMMU di sistema | `apps-smmu@0x15000000` (`qcom,qsmmu-v500`, `qcom,skip-init`) | **stream dell'IFE: `0x840` con maschera `0x620`** (8 SID: 0x840, 0x860, 0xa40, 0xa60, 0xc40, 0xc60, 0xe40, 0xe60); IOVA dell'IFE nel DT: `0x07400000` + `0xd8c00000` (fino a `0xE0000000`) ✔ |

`qcom,skip-init` vuol dire che Linux non reimposta la SMMU e tiene la configurazione lasciata dal firmware. Sotto XNU
la SMMU è come l'ha lasciata l'UEFI, con le voci di UFS, USB e display. **Non va toccato niente di esistente**
([§5](#5-rischi)).

### 1.4 Pin (TLMM, tile da `pinctrl-sm8150.c` ✔)

Indirizzi dei tile: NORTH `0x03900000`, EAST `0x03500000`, SOUTH `0x03d00000`, WEST `0x03100000`. Per ogni pin la
configurazione sta a `tile + 0x1000·pin` e lo stato a `+4`, come nella sonda `andromeda_probe.c`.

| GPIO | Tile | Funzione (DT Xiaomi) |
|---|---|---|
| 12 | NORTH | reset IMX576 |
| 13-16 | NORTH | MCLK0-3 (funzione `cam_mclk`, 4 mA) |
| 17 / 18 | NORTH | CCI0 M0 SDA / SCL (`cci_i2c`, pull-up) |
| 19 / 20 | NORTH | CCI0 M1 SDA / SCL |
| 21 | EAST | DVDD+AVDD dell'IMX363 |
| 23 | EAST | power-down dell'OV02A10 |
| 24 | EAST | regolatore OIS |
| 26 | EAST | VAF dell'attuatore wide |
| 28 | EAST | reset IMX363 |
| 29 | EAST | DVDD+AVDD dell'S5K3M3 |
| 30 | EAST | reset S5K3M3 |
| 31 | NORTH | VAF dell'attuatore tele |
| 32 | NORTH | DVDD+AVDD dell'IMX576 |
| 33 | NORTH | DVDD+AVDD dell'OV02A10 |
| 34 | NORTH | VIO di tutte le fotocamere |
| **42** | NORTH | **Hall 1 dello slider** |
| **114** | SOUTH | **Hall 2 dello slider** |

Con la fotocamera spenta, Android mette i pin di reset e di alimentazione a `output-low` con pull-down (stati
`cam_sensor_suspend_*`) ✔. Che cosa lasci l'UEFI lo dirà la C0 (?).

### 1.5 Lo slider: due sensori Hall

Il DT non ha nessun nodo Hall. È il driver Xiaomi `drivers/halls/halls.c` (`CONFIG_HALLS=y` nel defconfig
`andromeda_user_defconfig`) che fissa nel codice due ingressi, `DEF_HALL_GPIO_1 = 42` e `DEF_HALL_GPIO_2 = 114` ✔:

- li legge come ingressi, con un'interruzione su entrambi i fronti;
- `(h1, h2) = (0, 1)` → stato 1; `(1, 0)` → stato 0; ogni altra combinazione → stato 2 (in movimento) ✔;
- espone il risultato a user space con `/dev/halls` (misc device e ioctl).

Due punti sono ancora da verificare con la C0:

- che i numeri siano quelli del TLMM (~), cosa che vale se il gpiochip del TLMM parte da 0, come fanno di solito i
  driver Xiaomi che usano numeri fissi;
- quale dei due stati è "aperto" (?).

Leggere lo slider è il pezzo più facile di tutto questo lavoro: bastano due letture del bit `IN` dei pin. Può servire
anche fuori dalla fotocamera (ROADMAP §13).

### 1.6 Flash e torcia

`qcom,leds@d300` sul **PM8150L, SID 5** (`qcom,qpnp-flash-led-v2`) ✔:

- `flash_0` e `flash_1` fino a 1500 mA (per default 1000 mA per 1280 ms);
- `flash_2` fino a 750 mA;
- `torch_0/1/2` fino a 500 mA (per default 300 mA);
- `led_switch_0` con maschera `0x03`.

`camera-flash@0/@1` (le posteriori) usano i canali 0+1, cioè il doppio LED. `@2/@3` (le frontali) usano il canale 2 e
`led_switch_1`: se ci sia davvero un LED davanti è da verificare (?).

Accendere la torcia vuol dire **scrivere** registri del PMIC. Il nostro SPMI oggi sa solo leggere (canale observer di
ios-sweet). HakiOS ha `hakios.torch.c` (APSL). È un lavoro a parte e non serve alle foto.

### 1.7 Cosa non c'è nel device tree

Sul kernel downstream la sequenza di accensione di ogni modulo la manda lo user space Qualcomm (CamX), e la prende da
`/vendor/lib64/camera/com.qti.sensormodule.<vendor>_<sensore>.bin`. Lo stesso file contiene:

- l'indirizzo I²C;
- l'ordine e i ritardi di VIO/VANA/VDIG/MCLK/RESET;
- le tabelle di init e dei modi (risoluzione, frequenza, corsie);
- l'OTP e l'EEPROM.

Sunfish ha decodificato quello dell'IMX363 del Pixel 4a ✔:

- 10 modi da 82 registri, tutti RAW10 a 4 corsie;
- tra questi il **2016×1512 a 60 fps in binning 2×2, a 304 Mb/s per corsia**;
- `EXCK_FREQ` a 24 MHz;
- indirizzo 0x1A.

Il formato è "Parameter Parser", con un decodificatore MIT (`tools/sunfish-camtab.py`, derivato da `redfin-camtab.py`).

**Per noi:**

- **Prima della C1** servono i `.bin` del nostro vendor. Si prendono in sola lettura con `adb pull` da Android e vanno
  in `phone/<seriale>/camera/`, **fuori da git** perché sono file Xiaomi/Qualcomm.
- **Le tabelle decodificate** possono entrare nel device tree Apple al momento della build, come i firmware della GPU
  (`gpufw,<nome>` in `scripts/make-andromeda-adt.py`), senza finire nel repository.

---

## 2. Lo stato dell'arte

### 2.1 Linux mainline

- **CAMSS per SM8150: non c'è** (torvalds/linux `a91337fb5e8e`, dopo v7.4-rc1). Sono supportati sdm845, sm6150, sm8250
  e sc8280xp ✔.
  - La serie **"media: qcom: camss: Add SM8150 CAMSS support"** di **Andrew Gigena** è alla v3, 7 patch, pubblicata il
    2026-11-06 (patchwork serie 1204447 ✔). Contiene:
    - 4 CSIPHY tramite il nuovo `phy-qcom-mipi-csi2`;
    - 2 CSID + 2 CSID-lite;
    - 2 VFE + 2 VFE-lite con `vfe_ops_170`;
    - CCI `qcom,sm8150-cci` con fallback `qcom,msm8996-cci`.

    Nel DTS della serie gli indirizzi coincidono con quelli del nostro DT. Gigena l'ha provata sul Pixel 4 con un
    IMX363 e driver dei sensori fuori albero.
  - Ramo precedente su sm8150-mainline: `andrew/6.16-cameras`.
- **camcc:** `drivers/clk/qcom/camcc-sm8150.c` è in mainline da v6.12 (Satya Priya Kakitapalli, Qualcomm,
  `ea73b7aceff6`) ✔.
- **CCI:** `i2c-qcom-cci.c` va bene con il compatibile `msm8996-cci` ✔.
- **I file più vicini al Titan 175:**
  - **`camss-vfe-17x.c`** (sdm845, Robert Foss; rinominato da Bryan O'Donoghue perché "vfe-170 and vfe-175" stanno
    nello stesso file);
  - **`camss-csid-gen2.c`** (Jonathan Marek);
  - per il CSIPHY v1.1, la tabella `lane_regs_sc8280xp` di `camss-csiphy-3ph-1-0.c`.
- **Su Titan, mainline usa solo il percorso RDI**, cioè il Bayer grezzo in memoria ✔. Gli offset del write master sono
  in `camss-vfe-17x.c`: `VFE_BUS_WM_CFG(n) = 0x2208 + 0x100·n`, `IMAGE_ADDR 0x2214`, `STRIDE 0x2228`,
  `REG_UPDATE_CMD 0x4ac`. Sono gli stessi che usa vayu.

### 2.2 Sensori in Linux

| Sensore | Driver | Stato |
|---|---|---|
| IMX363 | `imx363.c` di **Joel Selvaraj** (GPL-2.0, ricavato da imx355): rami `sdm845-mainline` e `sdm670-mainline` (modifica di Robert Mader, Collabora), pmaports | **fuori da mainline**; 4032×3024 RAW10, MCLK 24 MHz |
| S5K3M3 | nessuno upstream; `s5k-cis.c` in `butztill/s10-linux` | ? |
| S5K3M5 (non è il nostro) | `s5k3m5.c` di **Vladimir Zapolskiy** (Linaro) | mainline v7.0 |
| IMX576 | `imx576.c` di **Himanshu Bhavani** e **Hardevsinh Palaniya** (Silicon Signals) | mainline v7.4-rc1 |
| OV02A10 | `ov02a10.c` di **Dongchun Zhu** (MediaTek) | mainline v5.11; 1600×1200, 1 corsia |

### 2.3 SM8150 con fotocamera su Linux

- **OnePlus 7T Pro (`hotdog`, SM8150), Robin Snyders** (`Sr-0w/hotdog-linux-bringup`, GPL-2.0). Dichiara che tutte e
  quattro le fotocamere catturano con CAMSS mainline e libcamera 0.7.2, con l'ISP software sulla GPU.

  Le sue patch 0055-0089 spiegano **i quattro errori che ha trovato su SM8150** ✔. Sono la lista dei controlli da fare
  da noi:
  1. **CSIPHY v1.1, non v1.0:** con la sequenza sdm845 il CSID riceve errori (`0064`).
  2. **IOVA dentro `0x07400000`-`0xE0000000`:** fuori da lì il write master segnala "fatto" senza scrivere niente
     (`0066`).
  3. **Voti di banda della NoC della fotocamera:** senza, il write master non ha strada verso la memoria (`0067`,
     `0075`).
  4. **`GCC_CAMERA_HF_AXI_CLK` acceso:** senza, la CAMNOC risponde "Disconnected target" (`0076`).
- **Pixel 4 (`flame`):** fotocamera dalla serie Gigena (IMX363, IMX481, IMX520, OV7251).
- **Xiaomi Pad 5 (`nabu`):** CCI e sensori, CAMSS parziale.
- **Mi Mix 3 e Mi Mix 3 5G:** nessun lavoro. La pagina wiki postmarketOS del Mix 3 (`perseus`) dice "N", e
  `andromeda` non ha nemmeno un DTS mainline.

### 2.4 libcamera

- **Il pipeline handler "simple" con l'ISP software (SoftISP) da v0.3.0** (2024) ✔:
  - debayer e statistiche di **Hans de Goede**;
  - IPA soft e integrazione di **Andrey Konovalov**, con CAMSS (`qcom-camss`) già tra i dispositivi con SoftISP;
  - algoritmi AWB, AGC, BLC, LUT e CCM di **Milan Zamazal**;
  - debayer su GPU (EGL/GLES) di **Bryan O'Donoghue** in v0.7.0;
  - in v0.8.0 gli algoritmi passano in `src/ipa/softisp/`.
- **Licenza:** LGPL-2.1-or-later. Solo gli shader di debayer sono BSD-2-Clause, derivati da McGuire 2008.
- **Per noi:** libcamera non gira su iOS, e non copiamo codice LGPL nel nostro strumento. È **il miglior riferimento di
  metodo** per un ISP minimo: statistiche per canale, AGC con target di luminanza, AWB grey-world, black level, gamma.

### 2.5 XNU/iOS su hardware non Apple

| Progetto (autore) | SoC / sensori | Licenza | Fotocamera |
|---|---|---|---|
| **HakiOS** (vcvkk, OOSEX GANG), `oosexgang/hakios-xnu-kernel` | SM7325 (redwood) | kernel APSL-2.0, user space MIT ✔ | **dichiarato:** Camera.app con anteprima, foto, fuoco, zoom, frontale. Kernel `drivers/camera/hakios.camera.c` → `/dev/hakios-camera` (frame `RWFRAME1` + RAW10); user space: ISP software `camera_isp.c` e una `FigCaptureDevice` per `BWFigCaptureDeviceVendor` dentro `mediaserverd` ✔ |
| **hakios-sm8150** (nomorecoolnicknames), `vayu_camera.c` (5334 righe) | **SM8150** (POCO X3 Pro); IMX582, S5K3T2 | **nessuna licenza** | **dichiarato:** foto dal sensore principale, video a ~7,6 fps. Rail via RPMh, **I²C bit-bang sui pin CCI**, CSIPHY v1.1, CSID-lite0 RDI0 → IFE-lite0 WM0, una sola voce SMMU `0x840`/`0x620` ✔ (dal codice) |
| **ios-sunfish** (Yofuness) | SM7150 (Titan 175); **IMX363** + IMX355 | MIT ✔ | **dichiarato:** Camera.app con anteprima a colori e foto HEIC, AE e AF. Il diario passo per passo in `docs/features/camera.md` è il riferimento migliore per i nostri gradini (§2.6) |
| **ios-redfin** (DenialOfGravity, Rory Axton), ramo `redfin-int` | SM7250; **IMX363**, IMX481, IMX355 | MIT ✔ | **dichiarato:** Camera.app con anteprima e foto. Patch `0919`-`0934-redfin-camera-*` (dalla sonda del sensore al flash) |
| ios-sapphire (Angelpro09xd) | SM6225 | nessuna | dichiarato: foto RAW con un'app propria |
| instantnoodle-xnu (Evsio0n) | SM8250 | APSL | dichiarato: anteprima delle 4 fotocamere |
| `Log1nYT/ios-sweet` | — | — | `SweetCamera.cpp`: **stub** di `AppleH10CamIn`, "NOT a camera driver"; registra solo i selettori ✔ |
| ios-sweet, ios-flame, ios-topaz, ios-mh2lm, UniXNU (33 repository), UniXNU-cepheus/raphael/guacamole | — | — | **nessun lavoro sulla fotocamera.** Il README di ios-flame la mette tra le cose che non funzionano ✔; UniXNU-sheng spegne `cameracaptured` |

### 2.6 Cosa ne ricaviamo

Il diario di sunfish (`docs/features/camera.md`, MIT) segue proprio i gradini che proponiamo. Le lezioni che valgono
anche per noi ✔ (misurate da loro su Titan 175):

- **Chip ID (M1).**
  - L'IMX363 risponde a **0x1A** e solo se interrogato da solo: interrogarlo dopo una NACK su 0x10 dava timeout.
  - L'ID si legge con MCLK a 19,2 MHz (XO).
  - Il ramo MCLK resta spento finché il GDSC `titan_top` non è acceso.
- **Prima di guardare i dati, verificarne il flusso.**
  - Il sensore conta i frame nel registro `0x0005`, che si può leggere prima di ricevere qualunque dato.
  - Il pattern di prova del CSID (CSID-lite TPG) dimostra che la parte CSID → IFE funziona senza sensore.
  - Il CSID conta byte e pacchetti: un frame 2016×1512 RAW10 vale esattamente 3 810 240 byte.
- **SMMU.** È stata la parte più costosa:
  - con stream non registrati il telefono **si riavvia** dopo qualche secondo (probabilmente l'hypervisor);
  - una voce SMR con maschera ha causato perdita di dati e riavvii;
  - il metodo che ha funzionato è una **banca "trappola"**: un contesto che fa fallire ogni scrittura e ne registra il
    SID, poi voci SMR **esatte** e mai voci già esistenti;
  - il write master alterna due SID (bit 10 dell'indirizzo);
  - serve anche `camera_sf_axi` acceso.
- **Memoria.** Su sunfish XNU non ha RAM sotto i 4 GiB e `IMAGE_ADDR` è a 32 bit: hanno usato un'area del framebuffer.
  Da noi la RAM unita per la finestra da 3 GiB parte da `0xC0300000` (ROADMAP §5). Se la finestra di XNU la include,
  c'è circa 1 GiB di RAM sotto i 4 GiB (~, da verificare nel log di DarwinBoot).
- **Pulizia.** Prima di smontare la SMMU va fatto il reset dell'IFE, e il SID va lasciato su una banca trappola, così
  eventuali scritture in ritardo non finiscono in RAM. L'RCG della CAMNOC va rimesso su XO prima di spegnere il PLL.

Due differenze con vayu, che usa il nostro stesso SoC:

- vayu fa l'**I²C via GPIO** sui pin del CCI. Noi abbiamo già un trasporto bit-bang nell'albero
  (`xnu/cepheus_i2c_bitbang.h`, dal port del Mi 9 di YangQi0408), quindi per la C1 si può fare a meno del controller
  CCI;
- vayu vota i rail via RPMh come la nostra patch 0092.

---

## 3. Il lato iOS

### 3.1 Cosa vuole lo stack di Apple

La catena su iOS 16:

1. Camera.app (`CAMCaptureEngine`) → AVFoundation → XPC verso il server di cattura.
2. Il server di cattura **su iOS 16 è `mediaserverd`** ✔:
   - nel nostro log di avvio c'è `exec mediaserverd` e nessuna riga `cameracaptured`;
   - `cameracaptured` compare per la prima volta in iOS 18.0 beta 1 (blacktop/ipsw-diffs).
3. Dentro `mediaserverd` il grafo di nodi `BW…` di CMCapture carica `/System/Library/MediaCapture/H10ISP.mediacapture`.
4. Il plug-in apre `AppleH10CamInUserClient` del kext **`AppleH10CameraInterface`** (iPhone X = A11 = ISP "H10") ✔.
5. Il kext comanda il **coprocessore ISP di Apple**, che esegue un firmware Apple (RTKit sui chip recenti) e pilota i
   sensori Sony scelti da Apple.

Lo user client `AppleH10CamIn` ha circa 90 selettori, documentati in parte dal gist di b1n4r1b01, senza header pubblici.
Il firmware restituisce frame **già elaborati** (YUV) insieme ai metadati 3A. Ogni sensore Apple ha un file di taratura
(setfile).

Asahi Linux l'ha ricostruito per l'ISP dei Mac M1 (driver `apple-isp` di Eileen Yoon e altri, GPL-2.0-only):

- 7 canali IPC su una memoria condivisa;
- circa 89 comandi `CISP_CMD_*`;
- un setfile per sensore.

È la prova di quanto sia ricca quell'interfaccia.

Intorno ci sono anche:

- l'encoder video AVE;
- `AppleJPEGDriver`;
- lo scaler M2;
- `IOSurface`;
- la GPU per i filtri e l'anteprima;
- il Secure Enclave per Face ID (Pearl).

Nel nostro sistema mancano tutti tranne `IOSurface`.

### 3.2 Perché non lo replichiamo

Per far funzionare `H10ISP.mediacapture` senza modifiche dovremmo:

- scrivere un `AppleH10CamIn` finto;
- imitare la semantica del firmware ISP: accensione, configurazione dei canali, pool di buffer, metadati, setfile;
- fare noi l'ISP intero, perché il plug-in si aspetta immagini già elaborate.

Senza header, senza firmware da studiare e con un plug-in che controlla anche ID dei moduli e taratura, non è
realistico.

Nessuno ci ha provato oltre lo stub di `Log1nYT/ios-sweet`. Corellium ha una fotocamera virtuale per iPhone 8 con
iOS 16, ma l'implementazione non è pubblica.

### 3.3 La via di mezzo: una `FigCaptureDevice` nostra dentro `mediaserverd`

È la via di HakiOS, sunfish e redfin ✔ (codice MIT). Funziona così:

- `camera_device.c` crea una `FigCaptureDevice` con le sue `FigCaptureStream` e la consegna a
  `BWFigCaptureDeviceVendor` di `mediaserverd`, al posto di quella che costruirebbe `H10ISP`;
- i formati imitano quelli dell'iPhone X (`D22/AVCaptureSession.plist`);
- il codice entra in `mediaserverd` dentro un plug-in che il demone carica comunque (un driver audio HAL).

Servono poi diversi ritocchi:

- nodi ADT `/arm-io/isp` e `/product/camera`, altrimenti MobileGestalt dice che la fotocamera non c'è;
- `StartRecorderServer` acceso;
- TCC;
- correzioni di IOSurface;
- texture YCbCr in Metal per l'anteprima.

**Per noi viene dopo la GPU.** `scripts/sweet-root-tweaks.py` spegne `StartRecorderServer` perché il pre-riscaldamento
dei filtri della fotocamera chiama una funzione OpenGL nulla senza GPU e manda `mediaserverd` in crash ✔. Per
l'anteprima sunfish ha dovuto aggiungere un "Metal bridge". Riusare il loro user space (MIT, con crediti) è possibile,
ma a fine percorso.

### 3.4 L'alternativa realistica: uno strumento nostro

Il kernel prende i frame RAW10 dal percorso RDI e li mette in un anello in memoria. Ne esce in due modi:

- **`/dev/andromeda-cam`**, un dispositivo a caratteri BSD (`cdevsw_add` + `devfs_make_node`) con permessi **0600 root**,
  non 0666 come in HakiOS (la loro revisione lo segnala come problema);
- in alternativa, un `IOUserClient` con `IOConnectMapMemory64` per non copiare i frame.

`read()` restituisce un'intestazione (dimensioni, stride, ordine Bayer, esposizione, guadagno, numero di sequenza) seguita
dal frame. I comandi (sensore, modo, esposizione, guadagno) passano da `write()`/`ioctl`, e il kernel li **limita** ai
valori del modulo.

In user space un ISP minimo, tutto sulla CPU:

1. spacchettamento RAW10 (4 pixel in 5 byte);
2. livello del nero (misurato sulle righe ottiche nere o preso dal modulo);
3. demosaicing bilineare (poi edge-aware);
4. bilanciamento del bianco grey-world;
5. matrice colore dai file di taratura del modulo (facoltativa);
6. gamma sRGB;
7. esposizione automatica: media verso il 18 %, agendo su tempo di integrazione (`0x0202`) e guadagno analogico
   (`0x0204`) nei sensori Sony.

Per il codice si può riusare, con crediti:

- `camera_isp.c` di HakiOS e di redfin (MIT, scritti proprio per iOS);
- come riferimenti di metodo, `demosaicing.cpp` di OpenCV (intestazione BSD-3, repository Apache-2.0),
  `camera_pipe` di Halide (MIT) e `finish.cpp` di hdr-plus (MIT).

Non si copiano libcamera (LGPL), Megapixels (GPL-3.0), LibRaw (LGPL/CDDL), né i driver Linux (GPL).

Un 2016×1512 si elabora in qualche decimo di secondo sugli 8 core (~). Sunfish dichiara un'anteprima a 30 fps dopo le
ottimizzazioni (ISP diviso in bande di righe).

### 3.5 Installare e lanciare il nostro codice

- **Firma.** AMFI accetta ogni cdhash (porting della patch di ios-flame, `xnu/0096`), quindi basta una firma ad hoc con
  gli entitlement (`ldid -S` oppure `codesign -s -`).
- **Compilazione:** `xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0`, oppure Theos.
- **Strumento da riga di comando.** Oggi sul telefono non c'è un SSH documentato nel progetto (?). Si può:
  - lanciarlo con launchd: un job nel `launchd.plist` del sistema, scritto dallo stesso tipo di script di
    `sweet-root-tweaks.py`;
  - fargli mandare l'immagine al PC via TCP sulla rete USB (`192.168.7.1`), come già fa il log;
  - fargli scrivere un file su `userdata`.
- **App.** Una `.app` non cifrata va copiata in `/Applications` e registrata in LaunchServices (`uicache`/`lsreg`), come
  da [ROADMAP «Installare app»](../../ROADMAP.md) e [app-store.md §4](app-store.md). Per aprire il nostro `/dev` le
  serve un entitlement che la tolga dalla sandbox, come quelli che TrollStore documenta (~, da provare).
- **Mostrare l'immagine:** `CGImageCreate` da RGBA → `UIImageView`, disegnato dalla CPU, senza Metal.
- **Salvare:** un PNG con ImageIO (`CGImageDestination`) nei documenti dell'app, oppure in Foto con `PHPhotoLibrary`.
  Foto richiede `assetsd`, che nel nostro log viene lanciato ✔, e il permesso Foto. Il JPEG senza `AppleJPEGDriver` è
  da provare (?). Il video H.264/HEVC senza AVE no.

---

## 4. Piano a fasi

Ogni gradino:

- è un **parametro di avvio** (`andrcam=N`), spento per default;
- scrive solo da un **elenco chiuso** di registri;
- usa le tensioni **del DT, mai oltre**;
- si prova con un solo `fastboot boot`;
- ha un test che dice "fatto" su righe `ANDROMEDA CAM:` del log USB.

| Gradino | Obiettivo | Cosa scrive | Prova di "fatto" | Difficoltà |
|---|---|---|---|---|
| **C0** | sonda in sola lettura | niente (solo letture, più il comando di lettura SPMI observer come in `andromeda_probe.c`) | stato di clock, GDSC, pin, Hall, PMIC, SMMU nel log | bassa, 1-2 giorni |
| **C1** | **chip ID dell'IMX363** | 1 o 2 rail RPMh, 4 GPIO, GDSC, MCLK0 su XO, I²C | `ANDROMEDA CAM: imx363 id 0x0363 addr 0x1a` | media, ~1 settimana |
| C2 | pattern di prova del CSID in memoria (senza sensore) | PLL0, RCG/rami camcc, AXI GCC, 1 voce SMMU nuova, CSID-lite0, IFE-lite0 | byte del CSID = byte attesi, buffer pieno, nessun fault | alta, 1-2 settimane |
| C3 | primo frame grezzo dell'IMX363 | + tabelle del modulo, PLL2/MCLK 24 MHz, CSIPHY0 v1.1, L3C già acceso | contatore `0x0005` che sale, pacchetti CSID, istogramma per canale, miniatura nel log | alta, 1-3 settimane |
| C4 | **foto salvata** | + `/dev/andromeda-cam` | PNG a colori dal nostro ISP | media, ~1 settimana |
| C5 | app "Fotocamera" | — (user space) | anteprima e scatto dall'interfaccia, salvataggio in Foto | media, 1-2 settimane |
| C6 | Camera.app di Apple (facoltativo) | ADT, tweak di `mediaserverd`, plug-in | anteprima nella Camera.app | molto alta, dopo la GPU |

### C0: sonda in sola lettura (`andrcam=1`)

Un file `xnu/andromeda_cam.c` sul modello di `andromeda_probe.c`: un thread kernel, `andrcamdelay=N` secondi dopo
l'avvio, un elenco chiuso di indirizzi. Legge:

1. **GCC**: `camera_ahb` (0x10b008), `camera_hf_axi` (0x10b030), `camera_sf_axi` (0x10b034), `camera_xo` (0x10b044).
   **I registri camcc si leggono solo se `camera_ahb` risulta acceso**: senza clock AHB una lettura può bloccare il
   bus (lo nota anche vayu).
2. **camcc**: il GDSC `titan_top` (0x0ad0c1bc); PLL0 (+0x0) e PLL2 (+0x2000) MODE/L; MCLK0-3 RCG CMD/CFG/M/N/D e CBCR;
   `cci_0` RCG/CBCR; `cpas_ahb`, `core_ahb`, `camnoc_axi`.
3. **TLMM**: configurazione e IN/OUT dei 20 pin di §1.4.
4. **Hall**: i GPIO 42 e 114 ogni 2 s per 2 minuti, con una riga a ogni cambiamento. Mentre gira, l'utente apre e chiude
   lo slider: così scopriamo quale stato è "aperto". È il primo risultato utile anche senza fotocamera.
5. **PMIC** (canale observer SPMI, già nella sonda, con l'elenco ammesso da allargare): stato, tensione e modo di
   PM8150L S8C, L3C, BOB e PM8150 L17A. Gli indirizzi delle periferiche sono da ricavare: per i LDO la regola della
   sonda (`0x40 + n - 1`) dà L3C = SID 4 `0x42` e L17A = SID 0 `0x50` (~); S8C e BOB sono da ricavare dal cmd-db o da
   Linux (?). Per il flash si leggono solo i registri di stato di PM8150L SID 5 `0xd3`.
6. **cmd-db**: gli indirizzi RPMh di `smpc8`, `ldoa17`, `bobc1`, `ldoc3` e il livello attuale di MMCX. Il parser c'è già
   nel codice GPU (`cepheus_arc*.h`).
7. **SMMU di sistema** (`0x15000000`), solo lettura, come la sonda `smmu` di sunfish:
   - `sCR0` (USFCFG: gli stream non registrati vanno in fault?);
   - `IDR0`/`IDR1` (numero di SMR e di contesti);
   - tutte le SMR/S2CR valide;
   - quali voci coprono `0x840`/`0x620`, se ce ne sono;
   - quali SMR e contesti sono liberi.

   Leggere qui è ancora da verificare sotto l'hypervisor di SM8150 (?). Su SM7150 sunfish l'ha fatto senza problemi, e
   vayu legge `IDR0`/`IDR1` della stessa SMMU.

**Fatto quando** il log mostra tutti i valori, nessuna lettura blocca il sistema e il telefono arriva a SpringBoard come
sempre. Rischio quasi nullo: le sole aree pericolose sono i registri camcc a clock spento, protetti dal punto 1.

### C1: primo traguardo, il chip ID dell'IMX363 (`andrcam=2`)

**Prerequisiti:**

- i `.bin` dei moduli dal vendor del telefono ([§1.7](#17-cosa-non-cè-nel-device-tree)), da cui ricavare indirizzo
  I²C, ordine e ritardi dell'accensione;
- i risultati della C0;
- `smpc8` (e, se il modulo lo chiede, `ldoa17`) aggiunti all'elenco RPMh della patch 0092, alle tensioni esatte del DT.

**Sequenza** (ordine provvisorio, da allineare al `powerSetting` del modulo; tra i passi attese larghe, ≥ 10 ms, finché
non si conoscono quelle vere):

1. Controlli in sola lettura: i pin 13, 17, 18, 21, 28, 34 sono come la C0 li aveva trovati, BOB e L3C sono accesi,
   `camera_ahb` è acceso. Se qualcosa non torna ci si ferma senza scrivere niente.
2. **VIO:** GPIO 34 alto (`camera_viodd_ldo`, 1,8 V da S4A).
3. **VDIG:** voto RPMh `smpc8` a 1352 mV. Poi **VANA/DVDD:** GPIO 21 alto (gli LDO esterni AVDD/DVDD da BOB e S8C).
   `ldoa17` a 2856 mV solo se il modulo lo prevede.
4. **GDSC `titan_top`:** SW_COLLAPSE = 0, attesa di PWR_ON (attesa limitata), poi `cpas_ahb` e `core_ahb`.
5. **MCLK0** su XO a 19,2 MHz (il valore del DT): RCG su sorgente 0, divisore 1, poi il CBCR. GPIO 13 in funzione
   `cam_mclk`.
6. **RESET:** GPIO 28 alto.
7. **Lettura I²C** su CCI0 M0, provando **un indirizzo per avvio** (prima quello del modulo, poi 0x1A, poi 0x10). Si
   scrive il puntatore di registro `0x0016` (2 byte), poi un repeated start e la lettura di 2 byte. **Nessun registro
   del sensore viene scritto.** Due trasporti possibili:
   - **C1a, bit-bang** (consigliato per primo): GPIO 17/18 in funzione GPIO con `cepheus_i2c_bitbang.h` a 100 kHz;
     niente clock `cci_0` né programmazione del CCI. È il metodo di vayu sullo stesso SoC;
   - **C1b, controller CCI**: `cci_0` su XO a 19,2 MHz, reset, temporizzazioni a 100 kHz, coda M0Q0 con
     `CCI_I2C_Mm_Qn_LOAD_DATA` / `EXEC_WORD_CNT`, `READ_DATA` (offset da `i2c-qcom-cci.c`, solo come documentazione).
     Serve dalla C3 in poi solo se il bit-bang si rivela troppo lento.
8. **Spegnimento sempre**, in ordine inverso e solo per i passi fatti: reset basso, MCLK spento e RCG su XO, GDSC
   collassato se l'abbiamo acceso noi, GPIO 21 e 34 bassi, voto `smpc8` a 0, pin rimessi come trovati.

Esempio di log:

```text
ANDROMEDA CAM: c1 start sensor 0 imx363 cci0.m0 addr 0x1a (bitbang)
ANDROMEDA CAM: vio gpio34 0->1; vdig smpc8 1352 mV on; vana/dvdd gpio21 0->1
ANDROMEDA CAM: gdsc titan_top 0x...->0x... pwr_on; mclk0 xo 19.2 MHz cbcr 0x1; reset gpio28 0->1
ANDROMEDA CAM: imx363 reg 0x0016 = 0x0363 addr 0x1a OK
ANDROMEDA CAM: c1 power-down done (8 steps undone)
```

**Fatto quando** si legge `0x0363` e il telefono continua come prima.

Con `andrcamsel=1..3` lo stesso gradino accende uno solo degli altri sensori:

- l'S5K3M3 su M1, ID `0x30D3` da confermare;
- l'IMX576 su M1, `0x0576`;
- l'OV02A10 su M0, `0x2509` a 0x3D, con il power-down sul GPIO 23 al posto del reset.

Questo conferma anche l'assegnazione degli slot.

Rischi della C1:

- **sequenza di accensione**: un ordine sbagliato di solito vuol dire solo nessuna risposta, ma le tensioni restano
  quelle del DT;
- **GDSC che non sale**: potrebbe voler dire MMCX troppo basso, lo stesso problema di "eredità" dei voti ARC della GPU;
- **bus I²C bloccato**: tutte le attese sono limitate, e il trasporto bit-bang lascia le linee solo a livello basso o
  rilasciate.

### C2: pattern di prova del CSID in memoria (`andrcam=3`)

Senza sensore, come lo step 3 di vayu e la c1/d di sunfish.

**Clock:**

- PLL0 di camcc (Trion, 1200 MHz);
- gli RCG `slow_ahb`, `camnoc_axi`, `cphy_rx`, `ife_lite_0_csid`, `ife_lite_0` con i loro rami;
- `camera_hf_axi` e `camera_sf_axi` del GCC.

**SMMU, in tre avvii separati:**

1. una voce SMR libera per `0x840`/`0x620` verso un contesto **trappola** (fault senza page walk): registra il SID e il
   FAR reali, e non scrive niente in RAM;
2. voci **esatte** per i SID osservati verso un contesto in bypass, con il buffer sotto i 4 GiB;
3. oppure verso un contesto stage-1 che mappa solo il buffer, con IOVA dentro `0x07400000`-`0xE0000000`.

Mai una voce SMR o un contesto già usati. Alla fine il SID si lascia su una banca trappola.

**Pattern di prova:** CSID-lite0 TPG (barre colorate RAW10) → RDI0 → IFE-lite0 WM0 → buffer. Si controlla il bit di
fine del write master, poi si invalida la cache e si fa un istogramma.

Se il write master dice "fatto" ma il buffer resta vuoto, i sospetti sono quelli di Snyders: voti NoC, AXI HF, IOVA
fuori intervallo.

### C3: il primo frame grezzo dell'IMX363 (`andrcam=4`)

- **Sensore:** init e il modo 2016×1512 a 4 corsie dalle tabelle del modulo.
- **MCLK a 24 MHz:** dal PLL2 (Regera, tabella mainline: 24 MHz = PLL2 early /10 × 1/4). L'alternativa, più rischiosa,
  è adattare `EXCK_FREQ` e i moltiplicatori a 19,2 MHz.
- **CSIPHY0 v1.1:** sequenza di `cam_csiphy_1_1_hwreg.h` di Xiaomi, oppure `lane_regs_sc8280xp`, come documentazione.
  Settle count calcolato dalla velocità per corsia con la formula di Linux.
- **Ricezione:** CSID-lite0 RX dalla PHY 0, VC 0, DT 0x2B (RAW10).

Verifiche, in quest'ordine:

1. il contatore del sensore `0x0005` sale;
2. lo stato delle corsie del CSIPHY;
3. pacchetti e byte del CSID, che devono essere esattamente 3 810 240;
4. il bit di fine del write master;
5. la media per canale Bayer;
6. una **miniatura** a 126×95 in grigio, stampata in esadecimale su righe `ANDROMEDA CAM: thumb`, circa 24 KB, che
   stanno nell'anello da 64 KiB del log USB.

Uno script sul PC la ricompone in PNG: è la prima "foto" senza user space. Le miniature restano in `phone/logs/`, fuori
da git, perché riprendono la casa dell'utente.

### C4: foto salvata (`andrcam=5` + strumento `andrcam`)

**Kernel:**

- `/dev/andromeda-cam` (0600) con l'anello e i comandi;
- il modo a piena risoluzione (4032×3024 RAW10, 15,2 MB) solo per lo scatto.

**Strumento `andrcam`:** in C, arm64, firmato ad hoc. Esegue:

1. AE in pochi frame;
2. lo scatto;
3. l'ISP di §3.4;
4. il PNG;
5. l'invio al PC via TCP sulla rete USB, oppure un file in `/var/mobile/Media`.

**Fatto quando** una foto a colori riconoscibile arriva sul PC.

### C5: l'app

L'app è UIKit, con un `UIImageView` aggiornato a 5-15 fps a 1008×756 (binning e scala) e disegnato dalla CPU.

**Funzioni:**

- lo scatto a piena risoluzione;
- il salvataggio in Foto;
- la **scelta automatica della fotocamera frontale quando si apre lo slider**, letta dai Hall: è il gesto tipico del
  Mix 3;
- la torcia, se nel frattempo facciamo le scritture PMIC del flash.

**Installazione:** in `/Applications`, registrata in LaunchServices ([§3.5](#35-installare-e-lanciare-il-nostro-codice)).

### C6 (facoltativo): la Camera.app di Apple

È il modello HakiOS/sunfish/redfin ([§3.3](#33-la-via-di-mezzo-una-figcapturedevice-nostra-dentro-mediaserverd)),
solo dopo la GPU. Prima va riacceso `StartRecorderServer`, e `mediaserverd` deve reggere il pre-riscaldamento dei filtri.

---

## 5. Rischi

| Rischio | Dove | Cosa può succedere | Come lo evitiamo |
|---|---|---|---|
| **SMMU e hypervisor** | C2-C5 | scritture DMA dell'IFE senza voce SMR → fault globali → riavvio (sunfish ✔). Una voce SMR sbagliata può dirottare stream di altri (UFS!) e **corrompere `userdata`** | dump in sola lettura (C0); banca trappola prima del bypass; solo SMR e contesti liberi, mai quelli esistenti; voci esatte; reset dell'IFE prima dello smontaggio; SID lasciato su una banca trappola |
| **Memoria protetta** | C2-C5 | `camera_region` (`0x8b700000`), `removed_regions` e le zone dell'hypervisor non sono leggibili né scrivibili da HLOS: un DMA lì fa scattare l'hypervisor | buffer solo in RAM di XNU (`kmem_alloc_contig` sotto i 4 GiB, ~) oppure in uno stage-1 che mappa solo il buffer; mai indirizzi dalla mappa UEFI riservata |
| **Coerenza delle cache** | C2-C5 | l'IFE scrive senza snooping: si leggono dati vecchi | invalidare la cache del buffer dopo ogni frame (come vayu) |
| **Sequenza di accensione** | C1, C3 | ordine sbagliato → nessuna risposta, nel caso peggiore il sensore in uno stato strano fino al riavvio | sequenza e ritardi dal `.bin` del modulo; tensioni esatte del DT; spegnimento sempre, in ordine inverso |
| **Rail condivisi** | C1-C5 | spegnere L3C toglie il DSI (schermo) e l'USB (log); toccare BOB disturba l'audio; S8C e L17A nel DT servono solo alle fotocamere | L3C e BOB mai votati né spenti (si controlla solo che siano accesi); voti solo a `smpc8`/`ldoa17`, sempre ritirati |
| **Bus bloccato** | C0-C3 | lettura di camcc, CSID o IFE con il loro clock spento → blocco del sistema | ogni accesso controlla prima il ramo di clock; ordine come in Linux |
| **CCI / I²C** | C1, C3 | coda CCI bloccata, bus tenuto basso da un sensore | trasporto bit-bang con attese limitate (C1a); il CCI solo dopo, con un reset a ogni sessione |
| **GDSC / MMCX** | C1 | il GDSC non sale se MMCX è troppo basso (problema simile ai voti ARC della GPU) | C0 legge il livello di MMCX; se serve, il voto si concorda con il lavoro sulla GPU |
| **Build del kernel** | tutti | ogni modifica passa dal workflow macOS: giri lunghi | un solo file nuovo, gradini attivabili da parametri di avvio, più prove per avvio (`andrcamsel`, indirizzi) |
| **Privacy** | C3-C5 | miniature e foto della casa dell'utente nei log | log e immagini solo in `phone/` (ignorata da git), mai nei workflow |
| **Licenze** | tutti | codice GPL (Linux, kernel Xiaomi) copiato in file APSL/MIT | Linux e Xiaomi solo come documentazione; riuso solo di codice MIT/APSL (HakiOS, sunfish, redfin), con crediti; vayu (senza licenza) solo come idee e misure citate, salvo permesso dell'autore; i `.bin` dei moduli fuori da git |

---

## 6. Domande aperte

- Gli indirizzi I²C dei quattro moduli di `andromeda`, e la loro sequenza di accensione (dai `.bin` del vendor).
- Che cosa alimenta davvero `cam_v_custom1` (L17A)? E S8C alimenta direttamente qualcosa, o solo gli LDO esterni?
- Il chip ID dell'S5K3M3: oggi c'è una sola fonte (`0x30D3`).
- Dopo l'UEFI, sotto XNU: lo stato dei pin, del GDSC e di MMCX; se BOB è in modo PWM o automatico; il contenuto della
  SMMU.
- Quale stato dei Hall corrisponde allo slider aperto, e i numeri `42`/`114` sono davvero quelli del TLMM?
- Il LED del flash `@2`/`@3` esiste sulle frontali?
- L'OIS (ROHM BU64753) va inizializzato o centrato per avere immagini nitide, o basta lasciarlo spento?
- La serie CAMSS di Gigena verrà accettata? Da seguire: le eventuali correzioni valgono anche per noi.
- Nel nostro iOS c'è un accesso a riga di comando (SSH o simile) per lanciare lo strumento senza passare da launchd?

---

## 7. Crediti da aggiungere a `CREDITS.md`

Questo documento si basa sul lavoro delle persone e dei progetti qui sotto. Per regola del progetto
([CONTRIBUTING.md](../../CONTRIBUTING.md)) vanno aggiunti a `CREDITS.md`, in una sezione "Ricerca: fotocamere
(consultati)". In questo cambiamento non l'ho fatto, perché la richiesta era di toccare solo questo file. Quando si
riprende codice (C1 in poi), la voce va completata con che cosa si usa esattamente.

- **Port XNU con la fotocamera:**
  - HakiOS: vcvkk, OOSEX GANG (shimoku74 e collaboratori);
  - Yofuness (`ios-sunfish`);
  - DenialOfGravity / Rory Axton (`ios-redfin`);
  - nomorecoolnicknames (`hakios-sm8150`, vayu);
  - Angelpro09xd (`ios-sapphire`);
  - Evsio0n (`instantnoodle-xnu`);
  - Log1nYT (stub `AppleH10CamIn`).
- **Linux:**
  - camss: Robert Foss, Jonathan Marek, Bryan O'Donoghue, Hitesh Patel, Wenmeng Liu;
  - camcc: Satya Priya Kakitapalli;
  - serie SM8150: Andrew Gigena;
  - bring-up SM8150: Robin Snyders (`Sr-0w`);
  - driver dei sensori: Joel Selvaraj, Robert Mader, Vladimir Zapolskiy, Himanshu Bhavani, Hardevsinh Palaniya,
    Dongchun Zhu, butztill;
  - nabu: xianlly.
- **libcamera:** Hans de Goede, Andrey Konovalov, Milan Zamazal, Bryan O'Donoghue.
- **Apple ISP e iOS:**
  - Eileen Yoon e il team Asahi Linux;
  - b1n4r1b01 e ifilipis (gist);
  - blacktop (`ipsw-diffs`);
  - Jonathan Levin (database degli entitlement);
  - Lakr233 (`vphone-cli`).
- **Kernel Xiaomi** (`andromeda-p-oss`, codice Qualcomm/The Linux Foundation e Xiaomi).

---

## 8. Fonti

Grazie a tutti gli autori: quasi tutto quello che c'è qui è lavoro loro.

### Dal telefono e dal nostro repository

- Device tree attivo (`phone/<seriale>/live-fdt.dts`, fuori da git). Nodi usati:
  - `qcom,cci@ac4a000` e i suoi `qcom,cam-sensor@0..3`, `qcom,actuator@0/1`, `qcom,ois@0`, `qcom,eeprom@0..3`;
  - `qcom,cci@ac4b000`, `qcom,csiphy@ac65000..ac68000`, `qcom,csid*`, `qcom,vfe*`, `qcom,cam-cpas@ac40000`,
    `qcom,camcc`;
  - `qcom,gdsc@0xad0c1bc`, `qcom,cam_smmu/msm_cam_smmu_ife`, `apps-smmu@0x15000000`;
  - `gpio-regulator@0..3`, `qcom,camera-flash@0..3`, `qcom,pm8150l@5/qcom,leds@d300`;
  - i regolatori RPMh `bobc1`, `smpc8`, `ldoa17`, `ldoc3`, i pinctrl `cam_*`/`cci*`, `camera_region`.
- `phone/<seriale>/info/getprop.txt`: `persist.vendor.camera.mi.module.info` e `…infoext`.
- `phone/logs/ios-20261106-171527.log`: `exec mediaserverd` (riga 635), `exec assetsd` (riga 3350), nessun
  `cameracaptured`.
- File del progetto:
  - `xnu/andromeda_probe.c`, `xnu/andromeda_bt.c`, `xnu/0092-andromeda-bt-rpmh.patch`;
  - `xnu/cepheus_i2c_bitbang.h` (YangQi0408, UniXNU-cepheus);
  - `scripts/sweet-root-tweaks.py` (`StartRecorderServer`), `scripts/make-andromeda-adt.py`;
  - `docs/research/gpu.md`, `docs/research/app-store.md`, `ROADMAP.md`, `ANDROMEDA.md`, `../VALUTAZIONE-PORT.md`.

### Kernel Xiaomi

[`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource), ramo `andromeda-p-oss`, commit
`74347f5521fb444936f5986ff0937264307d78fe` (codice di The Linux Foundation/Qualcomm e Xiaomi, GPL-2.0). File:

- `arch/arm64/boot/dts/qcom/andromeda-sm8150-camera-sensor.dtsi`, `andromeda-pinctrl.dtsi` (commenti "DVDDEN AVDDEN"),
  `andromeda-pinctrl-p*.dtsi`, `andromeda-sm8150.dtsi`;
- `drivers/halls/halls.c`, `drivers/halls/Kconfig`, `include/linux/halls.h`;
- `arch/arm64/configs/andromeda_user_defconfig` (`CONFIG_HALLS`, `CONFIG_USE_ROHM_BU64753`, `CONFIG_SPECTRA_CAMERA`);
- `drivers/media/platform/msm/Kconfig`, `drivers/input/misc/Kconfig` (AK09970, non abilitato).

### Linux mainline

[torvalds/linux](https://github.com/torvalds/linux) a `a91337fb5e8e0127c8b5be87be9f3027f95b48bc` (GPL-2.0, solo
documentazione).

- **Clock e pin:**
  - `drivers/clk/qcom/camcc-sm8150.c`: Satya Priya Kakitapalli (Qualcomm), `ea73b7aceff6`, v6.12;
  - `drivers/clk/qcom/gcc-sm8150.c`, `drivers/pinctrl/qcom/pinctrl-sm8150.c` (The Linux Foundation).
- **CCI:** `drivers/i2c/busses/i2c-qcom-cci.c` (The Linux Foundation, Linaro).
- **CAMSS** (`drivers/media/platform/qcom/camss/`): `camss-vfe-17x.c`, `camss-vfe.c` (`vfe_get_output_v2`),
  `camss-csid-gen2.c`, `camss-csiphy-3ph-1-0.c` (`lane_regs_sc8280xp`), `camss-vfe-480.c`. Commit:
  - SDM845, Robert Foss: `7319cdf189bb`, `eebe6d00e9bf`, `2f8b67195e48`, `705245679372`;
  - SM8250, Jonathan Marek: `b4436a18eedb`, `4edc8eae715c`, `53655d2a0ff2`;
  - Bryan O'Donoghue: SC8280XP `4f94419ef428`, rinomina `682f4968084a`, PHY CSI2 `2bc7bb2b8e68`;
  - Hitesh Patel, RDI: `fc02156a3efb`, `eed550a8191c`;
  - Wenmeng Liu, SM6150: `2759816f1682`.
- **Sensori** (`drivers/media/i2c/`):
  - `s5k3m5.c`: Vladimir Zapolskiy, `7d402666094c`;
  - `imx576.c`: Himanshu Bhavani e Hardevsinh Palaniya, `ee2e9bf41d3e`, correzione `d7e7bca71698`;
  - `ov02a10.c`: Dongchun Zhu, `91807efbe8ec`.

### Serie e alberi fuori da mainline

- **Andrew Gigena**, "media: qcom: camss: Add SM8150 CAMSS support" v3:
  - <https://patchwork.kernel.org/series/1204447/> (verificata il 2026-11-06), cover
    `20261106-sm8150-v7.4-rc1-camss-v3-0-0103e8a0bbcd@andrewgigena.dev`;
  - v2 serie 1203712, v1 serie 1202452;
  - rami `github.com/andrewgigena/linux` `v7.4-rc1/sm8150-camss-v{1,2,3}`;
  - ramo precedente `andrew/6.16-cameras` su [sm8150-mainline](https://gitlab.com/sm8150-mainline/linux).
- **Robin Snyders** (`Sr-0w`), [`hotdog-linux-bringup`](https://github.com/Sr-0w/hotdog-linux-bringup)
  `47087509c4289c2580c54f5433e55366b2e00445` (GPL-2.0):
  - `aports/device/testing/linux-oneplus-hotdog-mainline616/0055`, `0056`-`0063`, `0064`, `0066`, `0067`, `0074`,
    `0075`, `0076`, `0089`;
  - `aports/device/testing/device-oneplus-hotdog/90-hotdog-camera.conf`.
- **IMX363:** Joel Selvaraj, `imx363.c` in [sdm845-mainline](https://gitlab.com/sdm845-mainline/linux) (ramo
  `sdm845/7.1-dev`); variante sdm670-mainline (Robert Mader, Collabora); pmaports `0074-…`.
- **S5K3M3:** butztill, `s10-linux` `s5k-cis.c` @`d8135b2465b3` (unica fonte per l'ID).
- **xianlly**, `nabu-linux-camera`.
- **Wiki postmarketOS/Nura** (wiki.nura.eco): pagine OnePlus 7T Pro, Pixel 4, Mi Mix 3, SM8150.

### libcamera

[gitlab.freedesktop.org/camera/libcamera](https://gitlab.freedesktop.org/camera/libcamera) (LGPL-2.1-or-later).

- SoftISP v0.3.0:
  - Hans de Goede: `c683e819`, `783f92c0`, `3755d966`;
  - Andrey Konovalov: `c987946e`, `6362bd46`, `c50065c9`.
- Algoritmi di Milan Zamazal: `7fab2062`, CCM `38ec74fb`.
- Debayer su GPU, Bryan O'Donoghue: `f520b29f` (v0.7.0).
- Rinomina in `src/ipa/softisp/`: `ba33a822` (v0.8.0).
- File: `src/libcamera/software_isp/{debayer_cpu,debayer_egl,swstats_cpu,software_isp}.cpp`.

### Port XNU/iOS

- **HakiOS**, OOSEX GANG (vcvkk, shimoku74, oosexgang-developers):
  [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel)
  `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`, `LICENSE` (kernel APSL-2.0, resto MIT). File:
  - `drivers/camera/hakios.camera.c`, `xnu/files/bsd/sys/hakios.camera.h`;
  - `userland/src/camera_device.c`, `camera_isp.c`, `inject_media.c`;
  - `docs/driver-review/camera-haptic-torch.md`.
- **hakios-sm8150**, nomorecoolnicknames:
  [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150)
  `3e7be137a100cef19f1d7e83346b6e688be697d1` (nessuna licenza: solo idee e misure, citate). File:
  `kernel/bsd/dev/vayu_camera.c`, `README.md`, `tools/camtab.py`.
- **ios-sunfish**, Yofuness: [`Yofuness/ios-sunfish`](https://github.com/Yofuness/ios-sunfish)
  `a1b6eb8a6e0446310d69827918414a9ebc1f6d82` (MIT). File: `docs/features/camera.md`, `tools/sunfish-camtab.py`,
  `phone/tools/cam{rx,d,clk}.c`.
- **ios-redfin**, DenialOfGravity (Rory Axton): [`DenialOfGravity/ios-redfin`](https://github.com/DenialOfGravity/ios-redfin)
  ramo `redfin-int` `ef2252aec48e5bc3197d806d419ed3d597478fb5` (MIT). File:
  - `xnu/patches/0919-redfin-camera-sensor-probe.patch`, `0921`, `0922`, `0923`, `0926`, `0929`, `0931`-`0934`;
  - `phone/audio/camera_*.c`, `docs/redfin/port-plan.md`.
- **Altri port:**
  - Angelpro09xd, [`ios-sapphire`](https://github.com/Angelpro09xd/ios-sapphire) `4c2a9c64`;
  - Evsio0n, [`instantnoodle-xnu`](https://github.com/Evsio0n/instantnoodle-xnu) `1d05cda8`;
  - Log1nYT, [`ios-sweet`](https://github.com/Log1nYT/ios-sweet) `592f852b5efc9773709a906aed6755688cb07e82`,
    `xnu/patches/SweetCamera.cpp`.
- **Senza fotocamera:**
  - NWylynko, [`ios-flame`](https://github.com/NWylynko/ios-flame) `745c4341af86089efb0df851fa8a27fb8face624`
    (`README.md` riga 32, `HANDOFF.md`);
  - dipa-studio, [`ios-sweet`](https://github.com/dipa-studio/ios-sweet) `45263bee`;
  - organizzazione [UniXNU](https://github.com/UniXNU) (UniXNU-sheng `ce0c600f`).

### Sensori di `andromeda` (liste dei blob, solo nomi)

- tequilaOS, `platform_device_xiaomi_andromeda` ramo `tortilla` `306d9d810217653acc2a00e554db4138399c9a00`,
  `proprietary-files.txt`.
- PixelExperience-Blobs, `vendor_xiaomi_andromeda` ramo `fourteen` `63c49c5e`.
- PixelExperience-Devices, `kernel_xiaomi_andromeda` `93cc8c24`.
- Specifiche: [fonearena](https://www.fonearena.com/blog/276036/xiaomi-mi-mix-3-5g-price-specifications.html).

### Lo stack di Apple

- Asahi Linux, Eileen Yoon e collaboratori: [`AsahiLinux/linux`](https://github.com/AsahiLinux/linux) ramo `asahi`,
  `drivers/media/platform/apple/isp/` `6fdefe9fd4700b34034620a7cb9bd20e5e92c45c` (GPL-2.0-only);
  [`asahi-installer`](https://github.com/AsahiLinux/asahi-installer) `asahi_firmware/isp.py` `e93743d5` (MIT).
- b1n4r1b01, gist dei selettori `AppleH10CamIn`:
  <https://gist.github.com/b1n4r1b01/8a79465e89de8a70a3cb12680a304b17>.
- ifilipis, gist `H10ISP.mediacapture`: <https://gist.github.com/ifilipis/460880d243e9bf33312e125a8334e3d2>.
- blacktop, [`ipsw-diffs`](https://github.com/blacktop/ipsw-diffs) `iOS/17_5_1_21F90__vs_18_0_22A5282m`
  (`cameracaptured` nuovo in iOS 18).
- Jonathan Levin, database degli entitlement: <https://newosxbook.com/ent.php?exec=usr/sbin/applecamerad&osVer=iOS16>
  e `exec=usr/sbin/mediaserverd`.
- Corellium, fotocamera virtuale: <https://support.corellium.com/features/sensors/camera/>.
- Lakr233, [`vphone-cli`](https://github.com/Lakr233/vphone-cli) `a78483a30430b7b3ecdf51536881fd97a440da81` (MIT),
  `VPhoneGuestComponents/VCamCaptured`, `Research/Guest/virtual_camera_transport.md`.
- EthanArbuckle, `vcam-ios` `55560cb5` (nessuna licenza).
- Apple, [Platform Security Guide, Face ID](https://support.apple.com/guide/security/face-id-and-touch-id-security-sec067eb0c9e/web).
- Apple, [XNU `xnu-8796.141.3`](https://github.com/apple-oss-distributions/xnu/tree/xnu-8796.141.3):
  `iokit/Kernel/IOUserClient.cpp`, `bsd/sys/conf.h`, `bsd/miscfs/devfs/devfs.h`.

### Installare app e strumenti

- opa334, [TrollStore](https://github.com/opa334/TrollStore) (MIT, README per gli entitlement).
- ProcursusTeam, [`uikittools-ng`](https://github.com/ProcursusTeam/uikittools-ng) (`uicache.m`) e
  [`ldid`](https://github.com/ProcursusTeam/ldid) (AGPL-3.0, solo come strumento).
- [Theos](https://github.com/theos/theos).

### ISP software (riferimenti riusabili)

- OpenCV `9d4808be`, `modules/imgproc/src/demosaicing.cpp` (BSD-3 nell'intestazione, repository Apache-2.0).
- opencv_contrib `2f5266f0`, `modules/xphoto/src/grayworld_white_balance.cpp` (Apache-2.0).
- Halide `659675b1`, `apps/camera_pipe/camera_pipe_generator.cpp` (MIT).
- Tim Brooks, [`hdr-plus`](https://github.com/timothybrooks/hdr-plus) `ef4dd2ca`, `src/finish.cpp` (MIT).
- Dave Coffin, [dcraw](https://dechifro.org/dcraw/dcraw.c).
- Da non copiare:
  - Martijn Braam e altri, [Megapixels](https://gitlab.com/megapixels-org/Megapixels) (GPL-3.0);
  - [LibRaw](https://github.com/LibRaw/LibRaw) (LGPL/CDDL).
