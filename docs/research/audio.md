# Audio su `andromeda`: chi l'ha già fatto, e come portarlo

Ricerca del 6 novembre 2026, solo lettura: nessun comando sul telefono, nessun codice dei repository scaricati eseguito,
nessun codice del kernel scritto. I valori del Mi Mix 3 5G vengono dal device tree attivo del telefono
(`phone/<seriale>/live-fdt.dts`, fuori da git), dal kernel Xiaomi `andromeda-p-oss` e dalla configurazione audio di
PixelExperience per `andromeda`; gli altri fatti dai file citati in [Fonti](#8-fonti).

Livelli di prova usati sotto: **visto sull'hardware** (log o risultato descritto con data/run dagli autori),
**dichiarato** (lo dice il README, senza prova pubblicata), **solo codice** (driver presente, nessun risultato
dichiarato). Nessun risultato di altri è stato verificato da noi.

## Sommario

- **Lo stesso hardware audio è già stato acceso sotto XNU, sullo stesso SoC.** HakiOS per il POCO X3 Pro
  ([`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150), `vayu`, SM8150)
  avvia l'ADSP via TrustZone (PAS id 1, `0x8BE00000`, stessi item SMP2P 443/429), parla APR sul canale
  `apr_audio_svc`, apre la porta AFE **QUAT MI2S RX `0x1006` sugli stessi pin GPIO 137–140** che usiamo noi, e porta
  il suono di iOS all'altoparlante; legge l'ID del **WCD934x** sul **SLIMbus** (NGD `0x171c0000`, reset GPIO 143, IRQ
  GPIO 123: identici ai nostri) e registra dai **microfoni del codec** (porta `0x4001`, canali 135/136). Stato
  dichiarato: altoparlante e microfono "hardware", cuffie mute. **Ma il repository non ha licenza**: è solo un
  riferimento.
- **La differenza grossa è l'amplificatore.** Il nostro è un **TI TAS2557** (I2C SE8 `0x4c`, reset GPIO 59, IRQ
  GPIO 115), con un **DSP interno che va caricato** con il programma del produttore (`tas2557_uCDSP.bin`, nella
  partizione `vendor`) e con la calibrazione di fabbrica (`/persist/audio/tas2557_cal.bin`, 442 byte, già nel nostro
  backup). Nessun port XNU e nessun kernel Linux mainline ha un driver TAS2557; quello TI/Xiaomi è GPL-2.0 (solo
  documentazione). In compenso la protezione dell'altoparlante gira nel chip stesso, non nell'ADSP.
- **Auricolare e microfoni sono sul WCD9340**, non su un secondo amplificatore: auricolare = uscita EAR del codec
  (SLIMbus RX0, `RX INT0`), microfoni = ADC1 e ADC3 (SLIMbus TX7/TX8). Niente jack: le cuffie passano dal **USB-C
  analogico** (MBHC del codec + interruttore comandato da GPIO, `fsa4476-gpio-support`).
- **La parte iOS è risolta e riusabile con licenza.** HakiOS (`oosexgang/hakios-xnu-kernel`, kernel APSL 2.0, user
  space MIT) presenta a `mediaserverd` un **AudioServerPlugIn** (`HakiosAudio.driver`) con i dispositivi che
  `VirtualAudio.plugin` dell'iPhone X si aspetta (`Speaker`, `Hawking`, `Codec`, `Actuator`) e scrive i campioni in
  `/dev/hakios-audio`. È la stessa userland che usiamo (iPhone X, iOS 16.5 per HakiOS, 16.7.16 per ios-topaz che lo
  ha già incluso) ed è esattamente ciò che manca a noi: il crash di `mediaserverd` che ci ha fatto disattivare
  `VirtualAudio.plugin` nasce dall'assenza del dispositivo virtuale `vdef`. Due port (POCO X5 Pro e Galaxy S9) lo
  dichiarano funzionante sull'hardware.
- **Piano**: fase 0 senza hardware (plug-in + `/dev` che scarta, `VirtualAudio` riattivato: `mediaserverd` stabile,
  utile anche alla riproduzione video), poi sonda in sola lettura, poi **ADSP via PAS** (lo stesso meccanismo del
  modem per il Wi-Fi, ma senza `rmtfs`: è il miglior primo bersaglio PAS del progetto), poi APR, poi TAS2557 e tono di
  prova, poi iOS sull'altoparlante. Codec WCD9340 (microfoni, auricolare) e USB-C dopo. Difficoltà complessiva alta,
  ma ogni pezzo ha un riferimento funzionante sullo stesso SoC.

---

## 1. L'hardware audio di `andromeda`

Fonti: device tree attivo (EvolutionX 16, kernel Inkypen 4.14), kernel Xiaomi `andromeda-p-oss`
(`arch/arm64/boot/dts/qcom/andromeda-sm8150.dtsi`, `andromeda-audio-overlay.dtsi`, `audio-kernel/asoc/sm8150.c`),
configurazione audio di PixelExperience `device_xiaomi_andromeda` (`mixer_paths_tavil.xml`,
`mixer_paths_overlay_static.xml`, `audio_platform_info.xml`). Il file `mixer_tavil.xml` già presente nello
scratchpad è identico byte per byte a `configs/audio/mixer_paths_tavil.xml` di PixelExperience (`f74fef44`); da solo
è quello generico Qualcomm (altoparlanti WSA), le scelte vere di `andromeda` sono nell'overlay.

GIC: INTID = SPI + 32. Tile TLMM: NORTH `0x03900000`, SOUTH `0x03D00000`, EAST `0x03500000`, `0x1000` per pin
(`pinctrl-sm8150.c`).

### 1.1 Componenti

| Cosa | Valore | Da dove |
|---|---|---|
| Codec | **Qualcomm WCD9340 "tavil"** (`qcom,tavil-slim-pgd`), elemental address `00 01 50 02 17 02` (produttore `0x0217`, prodotto `0x0250`); IFD `00 00 50 02 17 02`. Il nodo `pahu` (WCD9360) c'è ma la sua scheda è `disabled` | `slim@171c0000/tavil_codec`; `sound-pahu status = "disabled"` |
| Scheda audio Android | `qcom,sm8150-asoc-snd-tavil` (`sm8150-tavil-snd-card`), attiva | `qcom,msm-audio-apr/sound-tavil` |
| Bus del codec | **SLIMbus**, NGD `slim@171c0000` (`0x2c000`), BAM `0x17184000`; IRQ SPI 163 / 164; SMMU SID `0x1b46`, `0x1b4d`, `0x1b50`; `qcom,ea-pc 0x2a0`. Il master del bus è l'ADSP: l'AP è un satellite (NGD) | `slim_aud` |
| SoundWire | solo verso gli altoparlanti WSA881x, che su questa scheda **non ci sono** (`qcom,wsa-max-devs = 0`) | `tavil_codec/swr_master`, `sound-tavil` |
| Secondo SLIMbus | `slim@17240000` (cell 3): audio Bluetooth del WCN3990 (`btfmslim_slave`), solo per l'offload SCO/A2DP di Android | `slim_qca` |
| Altoparlante | **TI TAS2557** (smart amp con DSP interno, I/V sense), I2C **SE8** `i2c@a80000` (GPIO 88 SDA, 89 SCL, NORTH, funzione `qup8`), indirizzo **`0x4c`**, reset **GPIO 59** (SOUTH), IRQ **GPIO 115** (SOUTH), `ti,i2s-bits = 16`, `ti,bypass-tmax = 0`; nessun pin di identificazione dell'altoparlante (→ firmware predefinito) | `qupv3_se8_i2c/tas2557@4c`; Xiaomi `andromeda-sm8150.dtsi` |
| Collegamento dell'altoparlante | **QUAT MI2S**: GPIO 137 SCK, 138 WS, 139 SD0 (dati verso l'amplificatore), 140 SD1 (ritorno I/V), funzione `qua_mi2s`, 8 mA; `rx-lines 0x1`, `tx-lines 0x2`. DAI link Android `QUAT_MI2S_RX` → codec `tas2557.1-004c` / `tas2557 ASI1`, scelto in `sm8150.c` quando la piattaforma è `HARDWARE_PLATFORM_ANDROMEDA` (sul Mi 9 è un CS35L41, sul K20 Pro un TFA9874) | `quat_mi2s_*_active`, `qcom,msm-dai-q6-mi2s-quat`, `sm8150.c:7081-7746` |
| Auricolare | uscita **EAR** del WCD9340: `SLIM RX0` → `RX INT0_1` → `RX INT0 DEM MUX = CLSH_DSM_OUT`, `EAR SPKR PA Gain = G_6_DB` | `mixer_paths_overlay_static.xml`, percorso `handset` |
| Microfoni | analogici sul WCD9340: **AMIC3 → ADC3** e **AMIC1 → ADC1** (`handset-mic`: DEC7/DEC8 → `SLIM TX7`/`TX8` → `SLIM_0_TX`, 2 canali); `speaker-mic` = ADC1. Il device tree instrada anche AMIC4. Mic bias 1/3/4 = 2,7 V, 2 = 1,8 V | overlay `handset-mic`, `speaker-mic`; `qcom,cdc-micbias*-mv` |
| Cuffie | **nessun jack 3,5 mm**: USB-C analogico con MBHC del codec (`qcom,msm-mbhc-usbc-audio-supported = 1`) e interruttore a GPIO (`qcom,fsa4476-gpio-support`): `usbc-analog-en1` GPIO 152, `usbc-analog-en2` GPIO 35, `uart-audio-sw` GPIO 25, `adc2-switch` GPIO 130; l'FSA4480 I2C è `disabled`. Cuffie USB digitali: offload ADSP (`usb_audio_qmi_dev`) | `sound-tavil`, `andromeda-audio-overlay.dtsi`, `fsa4480@43 status = "disabled"` |
| Prossimità | **a ultrasuoni (Elliptic Labs)**: `ro.vendor.audio.us.proximity = true`, percorsi `ultrasound-output` (EAR) e `ultrasound-input` (ADC1), modulo nell'ADSP, calibrazione `/persist/audio/us_cal_v2` | `getprop.txt`, overlay, `persist.img` |

### 1.2 ADSP (il DSP audio, dentro LPASS)

| Cosa | Valore | Da dove |
|---|---|---|
| Nodo | `qcom,lpass@17300000` (`qcom,pil-tz-generic`), **`pas-id 1`**, firmware `adsp`, `smem-id 0x1a7` (423, motivo del crash), `sysmon-id 1`, `ssctl-instance-id 0x14`, `qcom,signal-aop` (messaggio AOP "load_state adsp"), proxy `vdd_cx` = `cx.lvl` a livello `0x181` (TURBO = 384 + 1 di offset), proxy clock `xo` (RPMh CXO), timeout proxy 10 s | `qcom,lpass@17300000` |
| Interrupt | watchdog SPI 162 (fronte); da SMP2P `slave-kernel`: bit 0 err-fatal, 1 err-ready, 2 proxy-unvote, 3 stop-ack, 7 shutdown-ack; `force-stop` = bit 0 della nostra voce `master-kernel` | idem; `qcom,smp2p-adsp@1799000c` |
| SMP2P | item SMEM **443** (loro) e **429** (nostro), IRQ SPI 158, campanello APCS IPC bit 10 | `qcom,smp2p-adsp@1799000c` |
| GLINK | bordo `lpass`/`adsp`, SMEM, campanello APCS IPC bit **8**, IRQ SPI 156; canali `IPCRTR` (intents `0x800×5`, `0x2000×3`, `0x4400×2`), **`apr_audio_svc`** (`0x200×20`), `fastrpcglink-apps-dsp`, `glink_ssr` | `glink_adsp` |
| Memoria | `pil_adsp_region` **`0x8BE00000`, `0x2200000`** (34 MiB), `no-map`; dentro "PIL_REGION" `0x8B700000`–`0x9AB00000` della mappa UEFI di Aloha (riservata, fuori dalla finestra di XNU che parte da `0xC0000000`) | `reserved-memory`; `PlatformMemoryMapLib.c` |
| Buffer audio dell'AP | la memoria PCM che l'ADSP legge passa dall'SMMU delle applicazioni come **stream `0x1b21`**, `qcom,smmu-sid-mask 0xf` (i 4 bit bassi dello stream nei bit 32–35 di ogni indirizzo dato al DSP: buffer sotto i 4 GiB) | `qcom,msm-audio-ion` |
| Firmware | partizione **`modem`** (`sde52`, FAT, in Android `/vendor/firmware_mnt/image/`): `adsp.mdt` + `adsp.bNN` (su `vayu` `adsp.b02`–`b13`; il nostro elenco va letto). Partizione `dsp` (`sde48`): librerie FastRPC, non servono per l'audio | `partitions.txt`; `hakios-sm8150/pull-firmware.sh` |
| Caricamento in Android | `qcom,msm-adsp-loader`, `adsp-state 0`: lo carica il kernel all'avvio. Sotto il nostro UEFI va verificato se l'ADSP è mai partito (item 443) | `qcom,msm-adsp-loader` |

Mainline Linux conferma gli stessi numeri (`qcom,sm8150-adsp-pas`: `crash_reason_smem 423`, `pas_id 1`,
`load_state "adsp"`, `ssctl_id 0x14`; pd-mapper: dominio `msm/adsp/audio_pd`, servizio `avs/audio`, istanza 74).

### 1.3 Codec: alimentazioni, clock, pin

| Cosa | Valore | Da dove |
|---|---|---|
| Alimentazioni | `cdc-vdd-buck`, `buck-sido`, `vdd-tx-h`, `vdd-rx-h`, `vddpx-1`: **S4A 1,8 V** (`qcom,stub-regulator`: sempre accesa); `cdc-vdd-3v3`: **BOB** del PM8150L (RPMh `bobc1`) a 3,3 V | `tavil_codec`, `pm8150_s4`, `pm8150l_bob` |
| MCLK | **9,6 MHz** (`qcom,cdc-mclk-clk-rate`, `qcom,tavil-mclk-clk-freq`) da `audio_ext_clk_lnbb` = **RPMh `LN_BB_CLK2`** (`clock_rpmh` indice 2) | `tavil_codec clocks`, `audio_ext_clk_lnbb` |
| Reset | **GPIO 143** (SOUTH), attivo basso (`cdc_reset_active`: uscita alta, 8 mA) | `msm_cdc_pinctrl@143` |
| Interrupt | **GPIO 123** (SOUTH) → `wcd9xxx-irq` | `wcd9xxx-irq`, `wcd_intr_default` |
| GPIO interni del codec | GPIO 1 `us_euro_sw`, 4/5 `hph_en0/1` (il "hifi amp" di LINEOUT1/2, comando `HiFi Function`) | `wcd_pinctrl@5` |
| DSP interno del codec | `wcd-dsp-mgr@2`, firmware `cpe_9340` via `wcd_spi` (risveglio vocale): non serve | `wcd-dsp-mgr@2` |

### 1.4 Come Android lo usa (mixer e porte AFE)

| Uscita/ingresso | Porta AFE (ADSP) | Impostazioni |
|---|---|---|
| Altoparlante | `QUAT_MI2S_RX` = **`0x1006`** | `QUAT_MI2S_RX Audio Mixer MultiMedia1/5`; `QUAT_MI2S_RX Channels One`, `Format S24_LE`, `SampleRate KHZ_48`; TAS2557 **`Configuration 10`** (musica), `9` (voce). Con S24_LE la macchina usa slot da 32 bit: bit clock 48000 × 32 × 2 = 3,072 MHz (`get_mi2s_bits_per_sample`), clock AFE `QUAD_MI2S_IBIT` (`0x106`) |
| Ritorno I/V del TAS2557 | `QUAT_MI2S_TX` = `0x1007` | `Channels One`, `S24_LE`, 48 kHz |
| Auricolare | `SLIMBUS_0_RX` = `0x4000` | canale SLIMbus 144 (porta RX0 del codec; `rx_ch[] = {144…}` in `sm8150.c`) |
| Microfoni | `SLIMBUS_0_TX` = `0x4001` | TX7/TX8 → canali 135/136 (`tx_ch[] = {128…}`) |
| Cuffie USB-C | `SLIMBUS_6_RX` = `0x400C` | `RX INT1_2`/`INT2_2` → HPHL/HPHR |

`audio_platform_info.xml` lo conferma: `SND_DEVICE_OUT_SPEAKER` → `QUAT_MI2S_RX`, cuffie → `SLIMBUS_6_RX`,
microfoni → `SLIMBUS_0_TX`.

### 1.5 Il TAS2557 in dettaglio

Driver Android: `audio-kernel/asoc/codecs/tas2557/` (5510 righe, Texas Instruments 2016 + Xiaomi 2019, GPL-2.0).

- Registri a libro/pagina (registro 0 = pagina, registro 127 della pagina 0 = libro). ID del silicio:
  **libro 0, pagina 0, registro 3** (`0x80` PG1.0, `0x90` PG2.0, `0xa0` PG2.1). Reset software: libro 0 pagina 0
  registro 1 ← `0x01`.
- **Non suona senza firmware**: il driver carica sempre `tas2557_uCDSP.bin` (senza pin di identificazione
  dell'altoparlante è quello predefinito) con PLL, "programmi" (il codice del DSP del chip) e "configurazioni"
  (`Configuration 10` per la musica), poi la calibrazione di fabbrica `/mnt/vendor/persist/audio/tas2557_cal.bin`.
  Senza programma caricato `tas2557_enable()` rifiuta di accendere.
- File sul telefono: `/vendor/firmware/tas2557_uCDSP.bin` e `/vendor/etc/tas2557.ftcfg` nel `vendor` dinamico
  (come per la GPU, `docs/research/gpu.md` 6.3b); nel nostro `persist.img` ci sono già `audio/tas2557_cal.bin`
  (442 B), `tas2557_cal.txt`, `tas2557_chk.txt`.
- La protezione dell'altoparlante (temperatura, escursione) è l'algoritmo TI che gira **nel DSP del chip** con quella
  calibrazione: al contrario del TAS2562 di `vayu`, non dipende dall'ADSP.

---

## 2. Chi l'ha già fatto

| Progetto | Hardware | Cosa c'è | Livello di prova | Licenza |
|---|---|---|---|---|
| **HakiOS `vayu`** ([`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) `3e7be137`) | POCO X3 Pro, **SM8150**, **WCD934x su SLIMbus**, TAS2562 + TAS2564 su QUAT MI2S | ADSP PAS, GLINK, QRTR, APR, AFE MI2S, ADM/ASM, TAS256x, SMMU `0x1b21`, NGD SLIMbus + ID del WCD934x, init del codec, microfoni via SLIMbus TX, MBHC del jack | **dichiarato "hardware"** per altoparlante e microfono (README e `device.conf`: "speaker (TAS2562/TAS2564) and microphone work on hardware"), con riferimenti a run nei commenti ("run 202605: keyboard clicks audible at -20 dB"); cuffie **mute**; avvertenza degli autori: i risultati vengono da build precedenti alla ricostruzione dell'albero | **nessuna**: solo riferimento |
| **HakiOS comune** ([`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) `63fc2b46`, `drivers/audio/`, `userland/src/audio_plugin.c`) | POCO X5 Pro `redwood` (SM7325, AW88261 su TDM, macro LPASS + SoundWire) | `hakios.adsp.c` (ADSP PAS su IPCC), `hakios.sound.c` (AFE/ADM/ASM, SMMU), `hakios.guard.c` (limitatore al posto della protezione), `hakios.mic.c`, `hakios.audio.c` (`/dev/hakios-audio`), **`HakiosAudio.driver`** (AudioServerPlugIn) | redwood: **dichiarato funzionante** (`docs/devices/redwood.md`: "speakers and microphone through the audio DSP"); revisione del codice in `docs/driver-review/sound.md` con 16 difetti (volume non limitato, nodi `0666`, …) | **APSL 2.0** (kernel: `xnu/files/`, `drivers/`) + **MIT** (user space): riusabile |
| **Scion `starlte`** ([`Scion-Project/scion-starlte`](https://github.com/Scion-Project/scion-starlte) `e600c821`, `AUDIO.md`) | Galaxy S9, Exynos 9810, iOS 16.5 | driver ABOX + MAX98512, stessa catena iOS di HakiOS (`/dev/hakios-audio`, plug-in) | **visto sull'hardware** (run a1–b3, 3 novembre 2026: "iOS's sounds through `/dev/hakios-audio`", 0 frame persi), registrato in UniXNU `docs/devices/starlte.md` come `device-verified` | non dichiarata (GitHub: NOASSERTION) |
| **ios-topaz** ([`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz) `e4772911`, patch 0144–0146, 0935, `userland/audio/`) | Redmi Note 12 4G, SM6225, AudioReach/GPR, WCD937x | port di `TopazAudio` (topaz-woa) + **`RedwoodAudio.driver`** di HakiOS su **iOS 16.7.16 (20H392)**, la nostra build | **contraddittorio**: il README dice "sound" nello stato del 22 ottobre ma elenca l'audio tra le cose che non funzionano; `sweet-root-tweaks.py` disattiva ancora `VirtualAudio` | nessuna dichiarata (le parti HakiOS restano APSL/MIT) |
| **UniXNU-sheng** ([`UniXNU/UniXNU-sheng`](https://github.com/UniXNU/UniXNU-sheng) `ce0c600f`, `hal/ShengAudioPlugIn.c`, `package/ShengAudio*`) | SM8550, iPadOS 27, AudioReach, CS35L43 | servizio audio nel kernel + plug-in HAL; scoperta importante: **CoreAudio assume un clock host di 24 MHz** (correzione nel plug-in) | `implemented` (UniXNU `docs/devices/sheng.md`) | **BSD-2-Clause-Patent**: riusabile |
| **ios-flame** ([`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame) `745c4341`) | Pixel 4, SM8150, WCD9340 | nulla: "Not working yet: audio"; `HANDOFF.md` nota che senza dispositivo audio anche la riproduzione video si ferma (CoreAudio "no object with given ID 0", AQMEIO -66680) | — | MIT |
| **ios-sweet** ([`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) `45263bee`) | Redmi Note 10 Pro | nulla; la nostra `sweet-root-tweaks.py` ne eredita la disattivazione di `VirtualAudio.plugin` (analisi in `NOTES.md`) | — | MIT |
| Altri port HakiOS (`DBC1349/hackios-mi10ultra` `f81b29c4`, SM8250 + CS35L41; fork raphael di HeliumStudio-Dev e yfl520) | | il primo dichiara "ADSP audio ✅"; i fork raphael contengono solo i file comuni di redwood | dichiarato / solo codice | non dichiarata |
| **Linux sm8150-mainline** ([gitlab `sm8150-mainline/linux`](https://gitlab.com/sm8150-mainline/linux) ramo `sm8150/6.14`, `defb39db`) | Xiaomi Pad 5 (`nabu`): WCD9340 su SLIMbus + 4 CS35L41 su QUAT TDM | `sm8150.dtsi` con `slim-ngd@171c0000`, APR/q6afe/q6asm/q6adm; `sm8150-wcd9340.dtsi` (`slim217,250`, LN_BB_CLK2, 9,6 MHz, IRQ GPIO 123); `qcom,sm8150-sndcard` | solo codice (stato non dichiarato nel ramo) | GPL-2.0: solo documentazione |
| **Linux mainline** (`a91337fb`) | — | `qcom_q6v5_pas.c` (`sm8150_adsp_resource`), `qcom_pd_mapper.c`, `apr.c`, `qdsp6/q6afe.c`/`q6asm.c`/`q6adm.c`/`q6core.c`, `mfd/wcd934x.c`, `codecs/wcd934x.c`, `slimbus/qcom-ngd-ctrl.c`, `qcom/sdm845.c` (WCD934x su SLIMbus + QUAT MI2S, la macchina più vicina); **nessun driver TAS2557** (c'è `tas2562.c` per 2562/2564/2110) | — | GPL-2.0: solo documentazione |

Nessuno ha ancora fatto suonare **un TAS2557** o **l'uscita EAR/HPH del WCD934x** sotto XNU.

### 2.1 HakiOS `vayu`: cosa fa, passo per passo

`kernel/bsd/dev/vayu_audio.c` (3564 righe), `vayu_jack.c` (2908), `vayu_pas.c` (3677). Ogni passo ha il suo
boot-arg e parte spento (la stessa regola del nostro `CONTRIBUTING.md`).

| Passo | Cosa | Corrisponde da noi |
|---|---|---|
| `vayu_audio=1` | sola lettura: pin (funzione, pull, livello), SMEM (item 443: l'ADSP è già partito? item 423: le sue ultime parole); con `vayu_audio_i2c=1` l'ID degli amplificatori solo se i pin SDZ sono già alti | uguale, con GPIO 59/115/88/89 e l'ID del TAS2557 |
| `vayu_audio=2` | ADSP: `IS_CALL_AVAILABLE`, controllo di `adsp.mdt`/`.bNN`, eventuale arresto di un ADSP rimasto acceso, `INIT_IMAGE` (id 1) e `MEM_SETUP` (`0x8BE00000`), copia e rilettura, item GLINK 478/479 e SMP2P 429 creati, "adsp: on" all'AOP, `AUTH_AND_RESET`, attesa di "ready". Il voto `cx.lvl` non viene toccato (opzione `vayu_audio_cx=1`: solo verso l'alto, mai giù) | **identico** (stessi indirizzi, item e id) |
| `vayu_audio=3` | GLINK (`IPCRTR`, `apr_audio_svc`), HELLO QRTR, APR: `AVCS_CMD_ADSP_EVENT_GET_STATE` finché risponde 1, poi `AVCS_GET_VERSIONS` | identico |
| `vayu_jack=1` | NGD SLIMbus in modalità registri: QMI `SELECT_INSTANCE`/`POWER ACTIVE` al servizio 0x301 su QRTR, `MASTER_CAPABILITY`/`REPORT_SATELLITE`, indirizzo logico, `ADDR_QUERY` del codec, ID `0x0021–0x0024`, otto scritture di bring-up, rilettura | identico (stesso NGD, stessi indirizzi elementari) |
| `vayu_jack=2` | `LN_BB_CLK2` via RPMh, MCLK, tabelle di init, efuse, SVS; uscita cuffie su `0x400C` (canali 146/147) — **muta** | stesso codec; da noi il percorso utile è EAR (`0x4000`) e le cuffie sono su USB-C |
| `vayu_audio=4`, `vayu_sound=1–3` | porta `0x1006` (clock `0x106`, I2S SD0, 48 kHz), SMMU `0x1b21` con un contesto proprio (rimesso a posto dopo), ADM senza elaborazione, ASM PCM 16 bit, tono 1 kHz a −30 dBFS, poi l'amplificatore a basso volume con lettura dei guasti ogni 100 ms | stessa porta e stessi pin; amplificatore diverso |
| `vayu_sound=4/5` | flusso continuo da `/dev/hakios-audio` (iOS), limitatore e watcher che accende l'amplificatore solo quando c'è suono | da riprendere da HakiOS comune (licenza APSL) |
| `vayu_sound=6` | microfoni: `SLIMBUS_0_TX` `0x4001`, TX7/TX8, canali 135/136, ASM sessione 2, livelli per canale | **stesso percorso** del nostro `handset-mic` |

Il loro amplificatore TAS2562 non ha firmware: lo configurano a registri come il driver Android `tas256x.c`. Per il
TAS2557 questo non basta (sezione 1.5).

### 2.2 HakiOS comune e i port che lo hanno ripreso

- `hakios.adsp.c` (3306 righe): ADSP del POCO X5 Pro (IPCC invece della IPC APCS, `0x86700000`, 64 MB), con la
  struttura a 11 passi che `vayu` ha adattato all'SM8150.
- `hakios.sound.c` (1541): AFE/ADM/ASM, SMMU per lo stream del DSP, e `sound=4` = "quello che suona Darwin":
  porta e flusso sempre attivi, amplificatori accesi al primo campione non silenzioso e spenti dopo 3 s di silenzio,
  volume dal plug-in.
- `hakios.guard.c` (318): filtro passa-alto, tre medie della potenza e limitatore di picco, "in place of the DSP's
  speaker protection": per noi resta utile come limite di sicurezza anche con la protezione nel TAS2557.
- `hakios.audio.c` (362): `/dev/hakios-audio` (scrittura di frame 48 kHz 16 bit stereo, mai bloccante;
  `ioctl` `_IOW('r', 1, {below, muted})`); con `rec_sound=N` registra in memoria quello che iOS suona ("the only way
  to hear a machine one cannot listen to").
- `userland/src/audio_plugin.c` (1041, MIT): il plug-in (sezione 3).

### 2.3 Cosa insegna UniXNU-sheng (iPadOS 27, SM8550)

- **Clock host**: "CoreAudio's host clock frequency is a constant of 24 MHz (Apple's timer)"; con il timer a
  19,2 MHz i cicli I/O dell'audio server arrivavano il 25 % in ritardo. Il nostro timebase è **19,2 MHz**
  (`scripts/devices.py`). Il plug-in di HakiOS non corregge nulla e su redwood (anche 19,2 MHz) è dichiarato
  funzionante: **va verificato su `CoreAudio` di 20H392** (`AudioGetHostClockFrequency` nel dyld cache).
- iPadOS 27 usa `audiomxd` e arm64e; iOS 16.7.16 sull'iPhone X usa `mediaserverd` e arm64 (A11, niente PAC): i trucchi
  sui puntatori firmati di sheng non servono.
- Lì un contesto SMMU traducente per gli stream audio resettava la scheda (lo tiene l'hypervisor) e gli stream sono
  rimasti in pass-through: è lo stesso rischio che `docs/research/wifi-bluetooth.md` segnala per il SID `0x640`.

---

## 3. Il lato iOS: cosa serve a iOS 16 per suonare e registrare

1. **Un dispositivo nell'HAL di CoreAudio** (dentro `mediaserverd`). Sull'iPhone vero i dispositivi integrati
   arrivano da driver del kernel tramite IOAudio2, famiglia di Apple che il nostro kernel open source non ha. L'altra
   strada sono i plug-in in `/System/Library/Audio/Plug-Ins/HAL` (da lì arrivano Bluetooth, AirPlay, CarPlay, USB):
   un **AudioServerPlugIn** (CFPlugIn con la tabella `AudioServerPlugInDriverInterface`). È la strada di HakiOS,
   Scion, topaz e UniXNU.
2. **`VirtualAudio.plugin` deve esserci.** Tutto l'audio di iOS passa dal dispositivo virtuale, che costruisce le
   porte dai nomi (UID) dei dispositivi dell'iPhone X. Senza di essi il dispositivo predefinito `vdef` non esiste e
   `mediaserverd` va in SIGSEGV (`ApplyBeamFormerPreference ← VirtualAudio_PlugIn::SetPropertyData`, analisi di
   ios-sweet): per questo `scripts/sweet-root-tweaks.py` (funzione `audio()`) lo sposta in `HAL.disabled`. Senza
   dispositivo, però, anche video e suoni di sistema non hanno dove andare (ios-flame: AQMEIO -66680).
3. **Cosa presenta `HakiosAudio.driver`** (testo di `audio_plugin.c`):
   - `Speaker`: uscita stereo → `/dev/hakios-audio`, più un ingresso stereo silenzioso; con controllo di volume
     (48 dB lineari, 3 dB per ognuno dei 16 passi; iOS lo mette al massimo una volta e scala i campioni da sé);
   - `Hawking`: un ingresso muto, compagno obbligatorio (`vhaw`) senza il quale non nasce la porta altoparlante;
   - `Codec`: ingresso mono da `/dev/hakios-mic` con sorgente dati `imic` (microfono interno);
   - `Actuator`: uscita mono per il Taptic Engine (catena `d22_actuator_general`) → `/dev/hakios-haptic`;
   - formato unico 48 kHz, 16 bit, trasporto "built-in", clock = `mach_absolute_time()`; il suono passa prima per la
     catena di effetti dell'iPhone X (`d22_speaker_general` da `/Library/Audio/Tunings/D22`).

   Il nostro ADT presenta proprio un iPhone X (`D22AP`): le tabelle di routing che il plug-in soddisfa sono le nostre.
4. **Firma e caricamento**: `mediaserverd` deve accettare un bundle firmato ad-hoc. Con l'AMFI permissivo di
   ios-sweet è probabile (topaz carica anche ElleKit), da verificare. Il plug-in va compilato con l'SDK iPhoneOS
   (nel workflow macOS del kernel o con LLVM + SDK); HakiOS ha anche un binario precompilato, che non useremmo.
5. **Clock** (sezione 2.3): 24 MHz contro 19,2 MHz, da verificare prima di inseguire scatti e sottoruns.
6. **Root**: in `sweet-root-tweaks.py` andrà cambiata `audio()` (riattivare `VirtualAudio`, installare il plug-in;
   `StartRecorderServer=false` resta). Non modificato in questa ricerca.

Conclusione: per iOS non serve un IOAudioDevice nel kernel. Serve un nodo `/dev` semplice (lo schema
`/dev/hakios-audio`) e un plug-in in user space; il lavoro duro sta tutto sotto, tra ADSP e amplificatore.

---

## 4. Fattibilità e piano a fasi

Ogni driver parte spento e si accende con il suo boot-arg (nome proposto `andraudio=N`, come `andrgpu`). Prima la
sola lettura, poi le scritture. Nessuna partizione viene scritta: il firmware si legge in Android (con il permesso
dell'utente, come per la GPU) e resta in `inputs/` o nell'immagine di root locale, mai in git.
Difficoltà: B bassa, M media, A alta.

| Fase | Cosa | Fatto quando | Diff. | Rischi | Riferimenti |
|---|---|---|---|---|---|
| **A0** — iOS senza hardware | Nodo `/dev/andromeda-audio` (ABI di `/dev/hakios-audio`) che conta e scarta i frame e stampa nel log durata, ritmo e picchi; plug-in HakiOS adattato (nomi dei nodi; `/dev/…-mic` e `-haptic` silenziosi); `VirtualAudio.plugin` riattivato | `mediaserverd` non va più in crash per 10 minuti; il log mostra ~48000 frame/s quando si tocca la tastiera o cambia il volume; la riproduzione video non si ferma più su CoreAudio | B–M | firma del bundle; clock 24 MHz; altri crash di `mediaserverd` nascosti dal primo | `audio_plugin.c` (MIT), `hakios.audio.c` (APSL), analisi `mediaserverd` di ios-sweet |
| **A1** — sonda in sola lettura (`andraudio=1`) | SMEM: item 443/423/429 e la partizione GLINK (l'ADSP è partito sotto UEFI?); TLMM dei pin 25, 35, 59, 88, 89, 115, 123, 130, 137–140, 143, 152; SCM `IS_CALL_AVAILABLE` per PAS con id 1 (la nostra `cepheus_scm_discovery.h` interroga già il supporto PAS, oggi per l'id 13); voto RPMh di `LN_BB_CLK2` e BOB (cmd-db) | rapporto con i valori letti, nessuna scrittura | B | lettura di un blocco LPASS = external abort (HakiOS: LPASS è dell'ADSP): non leggerlo | `vayu_audio.c` passo 1, `andromeda_probe.c` |
| **A2** — TAS2557: chi sei (`andraudio=2`) | I2C via GPIO su 88/89 (lo schema `cepheus_i2c_bitbang.h` del touch, pin presi e restituiti come trovati); reset GPIO 59 alto (il chip esce in software shutdown, nessuno stadio d'uscita acceso), 20 ms, lettura di libro/pagina e del registro 3 (atteso `0xa0`); reset di nuovo basso | ID letto e pin rimessi come prima | B | scrittura TLMM (eccezione stretta, come il touch) | `tas2557-regmap.c` (doc), `vayu_audio.c` `ask_amplifiers` |
| **A3** — ADSP via TrustZone (`andraudio=3`) | AOP QMP "adsp on" (codice QMP del Mi 9 per la GPU), `INIT_IMAGE` (`.mdt`), `MEM_SETUP` `0x8BE00000`+`0x2200000`, copia dei segmenti in `pil_adsp_region` (mappata con `ml_io_map`), `AUTH_AND_RESET`, SMP2P 443/429; arresto pulito: bit 0 di `master-kernel` → stop-ack → `PAS_SHUTDOWN` → AOP "off"; mai riavviare un ADSP spento senza stop-ack | "ready" entro 10 s, nessun err-fatal per 60 s, arresto con stop-ack, una seconda prova nello stesso avvio riesce | **A** | prima PAS del progetto (condivisa con il Wi-Fi W1); hypervisor e protezione della memoria; firmware di decine di MB: non entra nel payload `GUACESP` da 34 MiB, va letto dalla root HFS+ (come `vayu` legge `/fw`) o dalla FAT della partizione `modem` in sola lettura (come topaz da `modem_a`) | `vayu_pas.c` (solo rif.), `hakios.adsp.c` (APSL), mainline `qcom_q6v5_pas.c`/`qcom_common.c`, topaz 0144 |
| **A4** — GLINK, QRTR, APR | item GLINK nella partizione SMEM apps↔ADSP, canali `IPCRTR` e `apr_audio_svc`, HELLO QRTR, elenco servizi, `AVCS_CMD_ADSP_EVENT_GET_STATE` = 1, versioni dei servizi (AFE 4, ASM 7, ADM 8) | "audio services up" e versioni nel log | M | GLINK/QRTR in overflow (difetti trovati nelle review HakiOS); pd-mapper forse necessario (`avs/audio`): `vayu` arriva a "up" senza | `hakios.adsp.c`, mainline `apr.c`, `q6core.c`, `qcom_pd_mapper.c` |
| **A5** — tono dall'ADSP, amplificatore spento | clock `QUAD_MI2S_IBIT` (`0x106`), porta `0x1006` I2S SD0 48 kHz (prima 16 bit stereo come `vayu`, poi S24_LE come Android), avvio; SCK/WS che commutano ai pin 137/138 (lettura del livello); SMMU `0x1b21` (prima solo lettura del matching, poi un contesto), ADM senza elaborazione, ASM PCM, tono 1 kHz −30 dBFS, eventi "write done" | eventi del DSP al ritmo di 48 kHz, pin che commutano | A | SMMU e hypervisor (reset della scheda su SM8550); buffer fisicamente sotto 4 GiB (la finestra di XNU parte da `0xC0000000` e oggi arriva a `0x120000000`: va allocato nella parte bassa) | `hakios.sound.c` `sound=1/2`, `vayu_audio.c` passo 4, mainline `q6afe.c`/`q6asm.c`/`q6adm.c` |
| **A6** — TAS2557 acceso, tono udibile | sul PC uno script nostro trasforma `tas2557_uCDSP.bin` in un elenco piatto di scritture (PLL, programma, `Configuration 10`) e lo mette nell'ADT come i `gpufw,<nome>` (SHA-256 controllato dal kernel); il kernel lo scrive via I2C, applica `tas2557_cal.bin`, imposta l'ASI come Android (48 kHz, S24_LE, mono), accende solo con la porta che gira, volume digitale basso, legge i guasti ogni 100 ms e spegne al primo; il limitatore di HakiOS sui campioni | tono udibile a volume basso, nessun guasto, spegnimento pulito | A | formato del firmware TI da reimplementare (il parser TI è GPL); firmware PG2.1 contro PG1.0; volume massimo mai oltre quello di Android | `tas2557-core.c` (doc), `mixer_paths_overlay_static.xml`, `hakios.guard.c` |
| **A7** — iOS sull'altoparlante | il nodo di A0 diventa il "sink" del flusso ASM (`hakios.sound.c` `sound=4`); volume dal plug-in; amplificatore acceso con il suono, spento dopo 3 s di silenzio | clic della tastiera, suonerie e Musica udibili; nessun frame perso per 10 minuti | M | clock 24 MHz; sottoruns; i 16 difetti della review HakiOS da correggere riprendendo il codice | `hakios.sound.c`, `audio_plugin.c`, Scion `AUDIO.md` |
| **A8** — codec WCD9340: ID e microfoni | NGD SLIMbus (QMI 0x301 su QRTR, modalità registri), ID `0x0021–0x0024`, bring-up, `LN_BB_CLK2` + MCLK 9,6 MHz, init; `SLIMBUS_0_TX` `0x4001`, TX7/TX8 (ADC3, ADC1, mic bias); `Codec` del plug-in | ID letto; livelli del microfono che seguono la voce; Memo Vocali registra | A | NGD condiviso con l'ADSP; il codec che sparisce dal bus dopo il cambio SIDO (run di `vayu`); tabelle di init GPL da reimplementare | `vayu_jack.c` (solo rif.), mainline `qcom-ngd-ctrl.c`, `mfd/wcd934x.c`, `codecs/wcd934x.c`, `sm8150-wcd9340.dtsi` |
| **A9** — auricolare | `SLIMBUS_0_RX` `0x4000` canale 144 → `RX INT0` → EAR PA; porta "ricevitore" di iOS | voce nell'auricolare | A | nessuno ha ancora fatto suonare un'uscita RX del WCD934x sotto XNU (`vayu`: cuffie mute) | `codecs/wcd934x.c` (EAR), overlay `handset` |
| **A10** — dopo | cuffie USB-C analogiche (MBHC + GPIO 152/35/25/130), audio Bluetooth (A2DP via HCI, non via `slim_qca`), prossimità a ultrasuoni (modulo Elliptic nell'ADSP + EAR + ADC1: riguarda la voce 13 della roadmap), `Actuator` → motore di vibrazione | — | A | — | `sm8150.c` (MBHC USB-C), `audio-kernel/dsp/elliptic` |

**Il primo traguardo più piccolo** è A0 (solo iOS, nessun rischio per l'hardware) seguito da A1/A2 (sola lettura e un
ID I2C). Il primo traguardo hardware vero è **A3: l'ADSP che dice "ready"**. È anche il miglior primo bersaglio PAS
del progetto: il modem del Wi-Fi ha bisogno in più di `rmtfs`, tftp e pd-mapper per restare vivo, l'ADSP no (secondo
`vayu`). Il codice SMEM/SMP2P/PAS/GLINK/QRTR scritto per l'audio serve poi così com'è al piano Wi-Fi (W1/W2).

Stima onesta: A0 pochi giorni; A1–A2 un avvio ciascuna; A3–A4 la parte più incerta (settimane, come per chi l'ha
fatto prima); A5–A7 dipendono soprattutto dall'SMMU e dal firmware del TAS2557; A8–A9 sono il lavoro che nemmeno
`vayu` ha chiuso.

---

## 5. Riuso: cosa si può prendere e cosa solo consultare

| Fonte | Licenza | Uso consentito |
|---|---|---|
| HakiOS `hakios-xnu-kernel`: `drivers/audio/hakios.audio.c`, `hakios.sound.c`, `hakios.guard.c`, `hakios.adsp.c`, `hakios.mic.c` | APSL 2.0 (come il nostro XNU) | **riuso con attribuzione**, mantenendo le intestazioni; numeri di redwood da sostituire con i nostri |
| HakiOS `userland/src/audio_plugin.c`, `audio_plugin.plist`, `hakios.audio.h` | MIT | **riuso** con copyright "the HakiOS authors" e testo MIT in `licenses/` |
| UniXNU-sheng `hal/ShengAudioPlugIn.c`, `package/ShengAudio*` | BSD-2-Clause-Patent | riuso possibile (per esempio la correzione del clock host), con attribuzione |
| ios-flame, ios-sweet | MIT | niente di audio da prendere; la nostra `sweet-root-tweaks.py` viene da lì |
| `hakios-sm8150` (`vayu_*.c`) | **nessuna** | **solo riferimento**: si leggono ordine dei passi e lezioni; i numeri li prendiamo dal nostro device tree e dal kernel Xiaomi, il codice lo scriviamo noi |
| ios-topaz (patch 0144–0146, 0935), topaz-woa | nessuna dichiarata | solo riferimento |
| Scion `starlte` | non dichiarata | solo come prova che la catena iOS funziona |
| Linux mainline, sm8150-mainline, kernel Xiaomi `audio-kernel` (incluso il driver TI TAS2557) | GPL-2.0 | **solo documentazione**: protocolli APR/AFE/ASM/ADM, NGD SLIMbus, registri del WCD934x, formato del firmware TAS2557; niente codice copiato in XNU |
| Firmware ADSP, `tas2557_uCDSP.bin`, `tas2557_cal.bin`, ACDB | del produttore | dal telefono dell'utente, fuori da git, mai pubblicati dai workflow |

Quando il codice verrà ripreso, `CREDITS.md` va aggiornato nello stesso cambiamento (non toccato da questa ricerca).
Voci proposte:

- **OOSEX GANG** (shimoku, vcvkk e collaboratori), HakiOS `hakios-xnu-kernel`: `/dev/hakios-audio`, percorso
  AFE/ADM/ASM, limitatore degli altoparlanti, plug-in `HakiosAudio.driver` (APSL 2.0 / MIT).
- **nomorecoolnicknames**, `hakios-sm8150`: riferimento per ADSP, APR, SLIMbus e WCD934x sull'SM8150 (nessuna
  licenza, nessun codice copiato).
- **UniXNU** (UniXNU-sheng): la scoperta del clock host a 24 MHz di CoreAudio.
- **MakrSas**, `ios-topaz`; **Scion-Project**, `scion-starlte`: riferimenti per il plug-in su 20H392 e per la prova
  sull'hardware.
- **Texas Instruments / Xiaomi**, driver `tas2557` (GPL-2.0), e il kernel Linux (`qdsp6`, `wcd934x`,
  `qcom-ngd-ctrl`, `qcom_q6v5_pas`): solo documentazione.
- **PixelExperience-Devices**, `device_xiaomi_andromeda`: percorsi del mixer di `andromeda`.

---

## 6. Rischi e domande aperte

1. **ADSP già acceso?** Sotto Android lo carica `adsp-loader`; sotto UEFI va letto l'item 443 prima di tutto. Un ADSP
   rimasto acceso da un kernel precedente va fermato (HakiOS lo fa); ripartire sopra uno stato SMEM vecchio ha
   resettato il SoC su topaz-woa.
2. **TrustZone e hypervisor**: `vayu` dimostra PAS sotto un UEFI Mu-Silicium sullo stesso SoC; il nostro è Aloha.
   Da confermare con A1 (`IS_CALL_AVAILABLE`) senza scrivere nulla.
3. **Firmware ADSP**: elenco esatto dei `.bNN` e dimensione da leggere dalla partizione `modem`; posto nella root
   HFS+ (letto dopo il montaggio) o lettura FAT diretta della partizione.
4. **SMMU `0x1b21`**: contesto proprio (`vayu`) contro pass-through (sheng); sullo stesso SoC l'hypervisor ha già
   trasformato un bypass in fault per il Wi-Fi (topaz). Prima leggere, poi scrivere.
5. **TAS2557**: il formato di `tas2557_uCDSP.bin` va reimplementato da zero (parser TI GPL); va capito quale
   programma usare con `Configuration 10` e come applicare la calibrazione. Senza firmware il chip non suona.
6. **Clock host 24 MHz** di CoreAudio contro il nostro timebase da 19,2 MHz (sezione 2.3).
7. **`VirtualAudio` riattivato** può svegliare altri crash di `mediaserverd` oggi nascosti; c'è sempre il ritorno a
   `HAL.disabled`.
8. **Uscite RX del WCD934x** (auricolare, cuffie): nessuno le ha fatte suonare sotto XNU.
9. **Sicurezza dell'altoparlante**: volume mai oltre il livello di Android, spegnimento al primo guasto, il limitatore
   di HakiOS anche con la protezione del chip; ogni prova udibile con l'utente presente.

---

## 7. Dati del telefono usati

`phone/e719dcab/live-fdt.dts` (nodi citati nelle tabelle), `info/getprop.txt` (proprietà audio, `us.proximity`),
`info/partitions.txt` (`modem` `sde52`, `dsp` `sde48`, `persist`, `vendor`), `backup/persist.img` (elenco di
`/audio`, letto con `debugfs` in sola lettura). Nessuno di questi file è entrato nel repository.

---

## 8. Fonti

Grazie agli autori: quasi tutto quello che sappiamo su questo hardware sotto XNU viene dal loro lavoro.

- **HakiOS SM8150**, nomorecoolnicknames:
  [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) commit
  `3e7be137a100cef19f1d7e83346b6e688be697d1` (nessuna licenza): `README.md`, `device.conf`, `pull-firmware.sh`,
  `kernel/bsd/dev/vayu_audio.c`, `vayu_jack.c`, `vayu_pas.c`, `kernel/bsd/sys/vayu_audio.h`.
- **HakiOS**, OOSEX GANG (shimoku, vcvkk e collaboratori):
  [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) commit
  `63fc2b4633bac3af8e57dadab9f5a8d4cb940438` (APSL 2.0 + MIT, file `LICENSE`): `README.md`, `LICENSE`,
  `drivers/audio/conf`, `hakios.adsp.c`, `hakios.audio.c`, `hakios.sound.c`, `hakios.guard.c`, `hakios.mic.c`,
  `hakios.speaker.c`, `xnu/files/bsd/sys/hakios.audio.h`, `userland/src/audio_plugin.c`, `audio_plugin.plist`,
  `docs/driver-review/sound.md`, `docs/devices/redwood.md`, `docs/devices/qemu.md`, `docs/getting-started/qemu.md`
  (IPSW iPhone X 16.5 `20F66`).
- **ios-topaz**, MakrSas: [`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz) ramo `topaz`, commit
  `e4772911e116ab4ab6c4ac1f38e6106fefe8b483` (commit dell'audio `fe7625f1`): `README.md`, `NOTES.md`,
  `xnu/patches/0144-topaz-audio-adsp.patch`, `0145-topaz-audio-codec.patch`, `0146-topaz-audio-stream.patch`,
  `0935-topaz-private-dev-redwood-audio-dev-topaz-audio-for-.patch`, `userland/audio/audio_plugin.c`,
  `redwood_audio.h`, `tools/sweet-root-tweaks.py`.
- **Scion**, Scion-Project: [`Scion-Project/scion-starlte`](https://github.com/Scion-Project/scion-starlte) commit
  `e600c8216ec8b43a0c7bd2f4982ac77ef2ce5e3b`, `AUDIO.md`; prova citata nel commit
  [`Scion-Project/scion-kernel@dc5fbb43`](https://github.com/Scion-Project/scion-kernel/commit/dc5fbb431909904560cc13f0dac3801143f6434c).
- **UniXNU** (organizzazione UniXNU): [`UniXNU/UniXNU`](https://github.com/UniXNU/UniXNU) commit
  `10edc1efeeabc8c180b1a3ad9f96b7c8e36a385e`: `docs/devices/sheng.md`, `starlte.md`, `evb10.md`, `cas.md`,
  `docs/devices/*.md` (stato "Audio" dei dispositivi); [`UniXNU/UniXNU-sheng`](https://github.com/UniXNU/UniXNU-sheng)
  commit `ce0c600fa015964d4ecb6af864fa2ab66efa915f` (BSD-2-Clause-Patent): `hal/ShengAudioPlugIn.c`,
  `package/ShengAudioService.cpp`, `package/ShengAudioAbi.h`, `LICENSE`.
- **ios-flame**, Nick Wylynko ([NWylynko](https://github.com/NWylynko)):
  [`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame) commit `745c4341af86089efb0df851fa8a27fb8face624`
  (MIT): `README.md`, `HANDOFF.md`, `tools/sweet-root-tweaks.py`.
- **ios-sweet**, dipa-studio: [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) commit
  `45263bee51df4a0f55cdf9c649af886399488ddb` (MIT): `NOTES.md` (analisi del crash di `mediaserverd` in
  `VirtualAudio`), `tools/sweet-root-tweaks.py`; da noi `scripts/sweet-root-tweaks.py`.
- **Altri port consultati**: [`DBC1349/hackios-mi10ultra`](https://github.com/DBC1349/hackios-mi10ultra) `f81b29c4`
  (`README.md`); [`HeliumStudio-Dev/hackios-xnu-kernel`](https://github.com/HeliumStudio-Dev/hackios-xnu-kernel)
  ramo `raphael-bringup` `57722468`; [`yfl520/hackios-raphael`](https://github.com/yfl520/hackios-raphael) `f128f1d8`
  (elenco dei file); [`WeiXiaoye/perseus-xnu`](https://github.com/WeiXiaoye/perseus-xnu) `2de092c3` (nessun file audio).
- **Kernel Xiaomi**, [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) ramo
  `andromeda-p-oss`, commit `74347f5521fb444936f5986ff0937264307d78fe` (GPL-2.0): `arch/arm64/boot/dts/qcom/andromeda-sm8150.dtsi`,
  `andromeda-audio-overlay.dtsi`, `audio-kernel/asoc/sm8150.c`, `audio-kernel/asoc/codecs/tas2557/` (Texas Instruments,
  Xiaomi: `tas2557-core.c`, `tas2557-regmap.c`, `tas2557-codec.c`, `tas2557.h`), `audio-kernel/asoc/codecs/wcd934x/wcd934x.c`,
  `audio-kernel/dsp/elliptic/`, `include/dt-bindings/clock/qcom,rpmh.h`, `include/dt-bindings/regulator/qcom,rpmh-regulator.h`.
- **PixelExperience-Devices**, [`device_xiaomi_andromeda`](https://github.com/PixelExperience-Devices/device_xiaomi_andromeda)
  commit `f74fef4409c362763a0443d46e13f020d9c1c57d`: `configs/audio/mixer_paths_tavil.xml` (= `mixer_tavil.xml` dello
  scratchpad), `mixer_paths_overlay_static.xml`, `mixer_paths_overlay_dynamic.xml`, `audio_platform_info.xml`;
  elenco dei blob del vendor (`tas2557_uCDSP.bin`, `tas2557.ftcfg`, `libultrasound.so`, ACDB `Forte_*`).
- **Linux mainline**, [`torvalds/linux`](https://github.com/torvalds/linux) commit
  `a91337fb5e8e0127c8b5be87be9f3027f95b48bc` (GPL-2.0, solo documentazione): `drivers/remoteproc/qcom_q6v5_pas.c`,
  `drivers/soc/qcom/qcom_pd_mapper.c`, `drivers/soc/qcom/apr.c`, `sound/soc/qcom/qdsp6/q6afe.c`, `q6asm.c`, `q6adm.c`,
  `q6routing.c`, `q6core.c`, `drivers/mfd/wcd934x.c`, `sound/soc/codecs/wcd934x.c`, `sound/soc/codecs/tas2562.c`,
  `drivers/slimbus/qcom-ngd-ctrl.c`, `sound/soc/qcom/sdm845.c`, `sm8250.c`, `arch/arm64/boot/dts/qcom/sm8150.dtsi`;
  `sound/soc/codecs/tas2557.c` non esiste. Copia locale di `drivers/pinctrl/qcom/pinctrl-sm8150.c` per i tile.
- **sm8150-mainline** (Caleb Connolly, Alexey Minnekhanov e collaboratori):
  [gitlab.com/sm8150-mainline/linux](https://gitlab.com/sm8150-mainline/linux) ramo `sm8150/6.14`, commit
  `defb39db31e6e2de8d7319c77ad697f9581070bd`: `arch/arm64/boot/dts/qcom/sm8150.dtsi`, `sm8150-wcd9340.dtsi`,
  `sm8150-xiaomi-nabu.dts`, `sm8150-xiaomi-cepheus.dts`.
- **Project Aloha**, Kancy Joe (`sunflower2333`) e collaboratori:
  [`mu_aloha_platforms`](https://github.com/Project-Aloha/mu_aloha_platforms) commit `0b798128`,
  `Platforms/SurfaceDuo1Pkg/Device/xiaomi-andromeda/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c` ("PIL_REGION").
- **Questo repository**: `ANDROMEDA.md`, `ROADMAP.md`, `CONTRIBUTING.md`, `CREDITS.md`, `docs/research/wifi-bluetooth.md`,
  `docs/research/gpu.md`, `scripts/sweet-root-tweaks.py`, `scripts/devices.py`, `xnu/cepheus_scm_discovery.h`,
  `xnu/cepheus_i2c_bitbang.h`, `xnu/cepheus_qmp_connect.h`.
- **Dati del telefono** (fuori da git): sezione 7.
