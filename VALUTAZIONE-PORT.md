# Valutazione del port: XNU / iOS sul Xiaomi Mi Mix 3 5G (`andromeda`)

Data: 5 novembre 2026. Ricerca solo in lettura su sorgenti pubblici: nessun comando eseguito sul telefono.

## In breve

- **Il port è fattibile come progetto di ricerca, e il Mi Mix 3 5G è un candidato migliore del previsto.** Oggi nessuno lo
  supporta, ma:
  - il **Mi 9 (`cepheus`)** ha lo stesso chip (SM8150), la stessa famiglia di schermo (Samsung EA8076 AMOLED, command
    mode, 1080×2340) e lo stesso touch (ST FTS su I2C SE17, indirizzo 0x49, IRQ GPIO122). Con UniXNU arriva già alla
    home di iOS, con tasto power e touch funzionanti;
  - il **UEFI per il Mix 3 5G esiste già** ed è mantenuto (Project Aloha, `xiaomi-andromeda`). Sotto Windows ci
    funzionano schermo, GPU e Wi-Fi.
- **Strada consigliata: UniXNU, partendo dal port del Mi 9**, non HakiOS. HakiOS ha senso solo se vuoi a tutti i costi
  non toccare i dati Android, ma richiede molto più lavoro (vedi sotto).
- **Primo traguardo realistico:** home di iOS con touch, grafica disegnata dalla CPU (lenta), avvio temporaneo via
  `fastboot boot`. Stima: **3-6 settimane part-time** per chi ha esperienza di kernel/embedded.
- **Ricerca aperta, nessuna garanzia:** GPU, audio, Wi-Fi, ricarica della batteria, rete cellulare. Su SM8150 nessuno ha
  ancora un Wi-Fi funzionante sotto XNU.
- **Non funzioneranno mai:** App Store, Apple ID, iCloud, iMessage, telefonate (manca il Secure Enclave), impronta digitale
  (gira nel TEE Qualcomm).

## Stato del lavoro (5 novembre 2026)

- **Bootloader:** già sbloccato, con una ROM custom installata. La fase 0 si riduce a backup, raccolta dati e test di
  `fastboot boot`.
- **Fase 1 (UEFI): completata sul telefono il 6 novembre 2026.** Il menu UniXNU compare a schermo. Il repository di
  lavoro è [`UniXNU-andromeda/`](UniXNU-andromeda/ANDROMEDA.md), ramo `andromeda`, derivato dal port del Mi 9; i
  risultati di ogni test sono lì.
- **Due ostacoli trovati e aggirati:**
  - con un ramdisk vuoto il modulo `ShengBootMemoryDxe` legge la zona protetta dell'hypervisor e blocca il telefono
    a schermo nero: le immagini di prova ora contengono un payload FAT, come quelle vere;
  - il payload da 64 MiB del Mi 9 invaderebbe memoria usata dal firmware: sul Mix 3 5G si usano 34 MiB.
- **Fase 2 (kernel e device tree Apple): completata il 6 novembre 2026.** XNU parte sul telefono e arriva al montaggio della partizione di sistema (8 core, schermo, rete USB, lettura di `userdata`); si ferma lì perché il sistema iOS non è ancora installato. Ostacolo risolto: la finestra di memoria del kernel deve stare in una sola voce della mappa UEFI, quindi 1,5 GiB sul Mix 3 5G. Dettagli in `UniXNU-andromeda/ANDROMEDA.md`. Device tree Apple del Mix 3 5G generato (identità iPhone X,
  memoria misurata). Il kernel si compila su GitHub Actions nel fork pubblico
  [giannoccarol/UniXNU-cepheus](https://github.com/giannoccarol/UniXNU-cepheus), ramo `andromeda`, workflow
  `kernel-andromeda`. Il runner `macos-14` è stato ritirato da GitHub il 2 novembre 2026; si usa `macos-15-intel` con
  Xcode 16.2, come il port del Mi 9 (gli Xcode successivi non compilano `xnu-8796`). Il repository privato
  `giannoccarol/UniXNU-andromeda` è rimasto ai primi tre commit e non serve più.
- **Dati misurati sul telefono:** pannello `dsi_samsung_fhd_ea8076_cmd_display`, touch ST FTS (`0x49`, IRQ GPIO122,
  reset GPIO54), `board-id 0x2d`, SM8150 v2.2, 6 GB (finestra da 3 GiB a `0xC0000000` valida), framebuffer a
  `0x9C000000`. Backup di `persist`, `modemst1/2`, `fsg`, `fsc`, `boot`, `dtbo`, `vbmeta` in
  `UniXNU-andromeda/phone/` (escluso da git).
- **Due correzioni rispetto alla valutazione iniziale:**
  - il commit di Aloha usato dal Mi 9 (`0b798128`) è del 19 ottobre 2026, non la release 2402.86;
  - il `android-andromeda.dtb` di Aloha non è il device tree del telefono, ma una tabella di 11 alberi base generici:
    il bootloader sceglie quello giusto e applica la `dtbo`. Lo script del Mi 9 aggiornava solo il primo (un APQ8016),
    ora li aggiorna tutti.

## Le due strade: HakiOS e UniXNU

| | HakiOS ([oosexgang](https://github.com/oosexgang/hakios-xnu-kernel)) | UniXNU ([UniXNU](https://github.com/UniXNU/UniXNU)) |
|---|---|---|
| Telefoni SM8150 (stesso chip del Mix 3 5G) | 1 port della community: POCO X3 Pro (`vayu`) | Mi 9 (`cepheus`), K20 Pro (`raphael`), OnePlus 7 Pro (`guacamole`), Galaxy Note10+ 5G, Xiaomi Pad 5 (`nabu`) |
| UEFI per `andromeda` | no: l'aggancio HakiOS è solo per Mu-Silicium, che non ha `andromeda` (va creato un `andromedaPkg`, 3-7 giorni) | **sì**: Project Aloha `xiaomi-andromeda`, più le patch UniXNU (2-4 giorni) |
| Kernel | driver per dispositivo (`vayu_*`, ~2 MB di C con gli indirizzi del POCO) | kernel SM8150 condiviso (build `SWEET` di `ios-sweet`), cambia solo il profilo del dispositivo |
| Cosa viene scritto sul telefono | niente: il sistema sta in RAM | il sistema iOS va nella partizione `userdata`: **i dati e le app Android vengono cancellati** (il sistema Android resta) |
| Schermo e touch del Mix 3 5G | diversi da `vayu` (LCD video mode, touch Novatek su SPI): da riscrivere | uguali al Mi 9: driver touch da adattare, non da riscrivere |
| Stato dei port SM8150 | `vayu`: GPU, touch, audio, LTE visti funzionare su build vecchie; Wi-Fi no; dopo l'ultimo aggiornamento mai riavviato | Mi 9: home di iOS, power, touch; grafica solo CPU, GPU spenta |
| Maturità del codice | repository di 2 commit, caricato in un giorno | repository di 1-3 settimane, cambiano ogni giorno |

**Perché UniXNU:** per arrivare alla home bastano un profilo del dispositivo (indirizzi di memoria, schermo, nome della
scheda) e l'adattamento del touch del Mi 9. Con HakiOS bisognerebbe creare il UEFI e poi riscrivere schermo e touch,
perché il port SM8150 esistente è per un telefono con hardware diverso.

## Hardware: cosa si riusa e cosa va scritto

Fonte principale: kernel Xiaomi `andromeda-p-oss` (unico ramo: il telefono non ha mai superato Android 9) e i device tree
di PixelExperience. "Base" indica il port da cui partire.

| Componente | Mi Mix 3 5G | Base più vicina | Lavoro |
|---|---|---|---|
| SoC, CPU | SM8150 (Snapdragon 855), Kryo 485 | tutti i port SM8150 | nessuno: kernel condiviso |
| RAM | 6 GB LPDDR4X; buco di memoria a `0xBCC00000-0xBFFFFFFF` | K20 Pro / Mi 9 nelle versioni da 6 GB | basso: finestra di memoria (`owned_memory_size = 0x80000000`), da verificare sul nodo `/memory` del telefono |
| Storage | UFS 2.1 | tutti i port SM8150 | basso: stesso controller |
| Schermo | Samsung EA8076 AMOLED, DSI command mode, 1080×2340, 60 Hz | **Mi 9** (stessa famiglia EA8076, command mode) | basso: framebuffer lasciato acceso dal bootloader. La luminosità (comandi DCS, niente WLED) si fa più avanti |
| Touch | ST FTS (driver `fts_521`), I2C SE17, 0x49, IRQ GPIO122, reset GPIO54, alimentazioni PM8150L L1 + regolatore su GPIO37 | **Mi 9**: stesso chip, bus, indirizzo e IRQ; cambiano reset (GPIO12) e alimentazione (GPIO84) | basso-medio: adattare `cepheus_touch.c` |
| Tasti | PM8150 PON + GPIO | tutti | basso |
| Slider magnetico | 2 sensori di Hall su GPIO42 e GPIO114 | nessuno | basso, ma in iOS non c'è un uso naturale |
| GPU | Adreno 640 | `vayu` (HakiOS, accelerata con Mesa); Mi 9: GPU non ancora accesa | alto |
| Audio | codec WCD934x + **un** amplificatore TI TAS2557; niente jack (audio analogico via USB-C) | `vayu` ha WCD934x ma amplificatori TAS2562/2564 | alto |
| Prossimità | ultrasonica (Elliptic Labs) tramite il DSP audio | nessuno | molto alto: dipende dall'audio |
| Wi-Fi | WCN399x integrato (ICNSS) | nessuno funzionante su SM8150 (`vayu`: il firmware crasha) | molto alto |
| Bluetooth | WCN399x su UART SE13 | `vayu`: `bluetoothd` si riavvia in loop | alto |
| Batteria, ricarica | PM8150B (fuel gauge interno) + charge pump SMB1390 | `vayu`: non carica sotto XNU | alto, e incide sulla sicurezza d'uso |
| Vibrazione | PM8150B LRA | `vayu` (non verificato) | medio |
| Modem LTE | modem interno SM8150 | `vayu` (HakiOS): registrazione LTE sì, dati mobili bloccano il chip; UniXNU: niente baseband | molto alto |
| Modem 5G | SDX50M (X50) esterno su PCIe/MHI | nessuno | non realistico |
| Fotocamere | IMX363 + S5K3M3 (retro), IMX576 + OV02A10 (fronte) | nessuno con questi sensori | molto alto |
| Impronta | FPC posteriore, gestita nel TEE | nessuno | non realistico |
| NFC | NXP su I2C SE9 | `vayu` (non funziona) | alto, e inutile senza Apple Pay |
| Sensori | hub SLPI (LSM6DSO, AK0991x, …) | `vayu` (non verificato) | alto |

**Varianti di scheda:** esistono revisioni P0-P2.22 con alcuni GPIO diversi (es. NFC). Prima di fissare i pin, leggi il
`board-id` dal device tree del tuo telefono.

## UEFI

- **Dove:** [`Project-Aloha/mu_aloha_platforms`](https://github.com/Project-Aloha/mu_aloha_platforms),
  `Platforms/SurfaceDuo1Pkg/Device/xiaomi-andromeda`. Il nome "AndromedaPkg" nello stesso repository è il Surface Duo e
  non c'entra con questo telefono.
- **Contenuto:** 349 driver estratti dal firmware del Mix 3 5G, mappa di memoria, device tree Android reale
  (`android-andromeda.dtb`), schermo 1080×2340 sul framebuffer a `0x9C000000`.
- **Rispetto al K20 Pro:** cambiano quattro voci della mappa di memoria (regione PIL più piccola, un blocco "Reserved" a
  `0x9AB00000` ancora segnato TODO, posizione di MPSS_EFS), più alcuni driver nelle liste di avvio. FD a `0x9FC00000`,
  3 MiB, identico.
- **Patch UniXNU:** sono identiche nei port Mi 9, K20 Pro e OnePlus 7 Pro e si applicano. Però aggiungono i driver
  UniXNU/DarwinBoot solo alla cartella del OnePlus: per il Mix 3 5G vanno aggiunte a mano 4 righe in `DXE.inc` e
  `DXE.dsc.inc`. Controlla poi che i driver siano davvero nel firmware compilato.
- **Build:** solo RELEASE. La DEBUG non entra nei 3 MiB, e allargare l'FD sovrascrive heap e tabelle delle pagine.
- **Release già pronte:** Aloha ha pubblicato `xiaomi-andromeda_NOSB.img` (2402.86, marzo 2024), ma senza UniXNU. Utile
  solo come primo test che il UEFI si avvii sul tuo telefono.

## Piano a fasi

**Fase 0: preparazione** (1-2 settimane, quasi tutte di attesa)
1. Sblocca il bootloader con Mi Unlock. Cancella i dati, e Xiaomi impone un periodo di attesa.
2. Scarica la ROM fastboot ufficiale EEA **V12.0.7.0.PEMEUXM** (MIUI 12, Android 9: l'ultima). È la via di ritorno.
   Controlla `fastboot getvar product` (deve dire `andromeda`) e `fastboot getvar anti`.
3. Fai il backup delle partizioni con IMEI e calibrazioni: `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`.
4. Con un root temporaneo (Magisk) estrai `/sys/firmware/fdt`, `/proc/iomem`, `/proc/meminfo`, `board-id` e la riga di
   comando del kernel (contiene il nome del pannello).
5. Verifica che `fastboot boot` funzioni avviando un'immagine innocua (es. TWRP).

*Fatto quando:* hai backup, dati del telefono e ROM fastboot, e `fastboot boot` funziona.

**Fase 1: UEFI** (2-4 giorni). Compila Aloha `xiaomi-andromeda` NOSB RELEASE con le patch UniXNU e le 4 righe DXE. Avvia
con `fastboot boot`. *Fatto quando:* vedi il menu UniXNU sullo schermo.

**Fase 2: kernel e device tree Apple** (3-10 giorni). Kernel SM8150 condiviso (la build `ios-sweet` gira su un runner
macOS di GitHub Actions). Profilo `andromeda` in `devices.py` copiato dal Mi 9, con memoria da 6 GB e framebuffer
verificato; poi ADT e payload. *Fatto quando:* il log del kernel arriva a schermo fino a `launchd`.

**Fase 3: sistema iOS** (2-5 giorni). Immagine HFS+ costruita **da un IPSW scaricato da Apple** (iPhone X, 16.7.x) con
gli script del port, scritta su `userdata`. **Cancella i dati Android.** *Fatto quando:* compare la home di iOS (grafica
via CPU).

**Fase 4: touch** (3-10 giorni). Adatta `cepheus_touch.c` ai GPIO e alle alimentazioni del Mix 3 5G. *Fatto quando:* la
home si usa col dito.

**Fase 5: oltre** (mesi, nessuna garanzia). Nell'ordine: luminosità, ricarica, GPU (idee dal driver Adreno 640 di HakiOS
`vayu`), audio, Wi-Fi.

## Rischi

- **Dati Android:** lo sblocco del bootloader e la fase 3 li cancellano. Per tornare ad Android basta `fastboot -w`
  (riparte da zero) oppure la ROM fastboot.
- **Partizioni da non toccare mai:** `xbl`, `abl` e le altre partizioni di avvio. Lì si rischia il brick. Tutto il piano
  usa solo `fastboot boot` (in RAM) e `userdata`.
- **Batteria:** sotto XNU con ogni probabilità non si carica e si scarica in fretta. Fai sessioni brevi partendo sopra
  il 50%.
- **Codice giovanissimo:** i repository hanno giorni o settimane di vita, autori anonimi o con pseudonimo, stati dichiarati
  e non verificati. Prima di avviare un binario, leggi cosa fa.
- **Immagini iOS già pronte:** alcuni port pubblicano il sistema iOS già assemblato come release. Non scaricarle: sono
  software Apple ridistribuito senza permesso e binari non verificabili. Costruisci la tua dall'IPSW ufficiale.
- **Licenza Apple:** la licenza di iOS ne consente l'uso solo su dispositivi Apple, e questi port saltano l'attivazione.
  Resta un esperimento personale da non distribuire.
- **HakiOS e il token "$HackiOS":** il sito promuove una criptovaluta che finanzia il team. Non serve a nulla per il
  port: non comprarla.
- **Sicurezza:** con il bootloader sbloccato il telefono è meno protetto. Non usarlo per dati sensibili.

## Stima dello sforzo

| Fase | Durata (part-time, con esperienza di kernel/embedded) | Incertezza |
|---|---|---|
| 0. Preparazione | 1-2 settimane (attesa di Xiaomi) | bassa |
| 1. UEFI | 2-4 giorni | media |
| 2. Kernel + ADT | 3-10 giorni | media-alta |
| 3. Sistema iOS | 2-5 giorni | media |
| 4. Touch | 3-10 giorni | media |
| **Fino alla home con touch** | **3-6 settimane** | |
| 5. GPU, audio, Wi-Fi, ricarica | mesi | molto alta |

Senza esperienza di kernel e bring-up i tempi si moltiplicano: ogni passo si verifica solo sul telefono, guardando log
sullo schermo.

## Crediti e fonti

L'elenco completo e aggiornato di chi usiamo nel port (con cosa usiamo di ciascuno) è in
[`UniXNU-andromeda/CREDITS.md`](UniXNU-andromeda/CREDITS.md). Oltre alle fonti qui sotto, dalla fase 1 in poi: il ramo
`feat/adreno640` del port del Mi 9 di YangQi0408, `blacktop/darwin-xnu-build`, `sgan81/apfs-fuse`, `hfsprogs`, AOSP
`libsparse`, `gtsystem/python-remotezip`, il binding `lzfse` di Yogesh Khatri, ipsw.me e il kernel Linux
(`pinctrl-sm8150.c`).

**Boot e UEFI**
- Project Aloha, [`mu_aloha_platforms`](https://github.com/Project-Aloha/mu_aloha_platforms): Kancy Joe (`sunflower2333`,
  autore del port `xiaomi-andromeda`), XiaoYeZi (`qaz6750`), Gustave Monce (`gus33000`), Rostislav Lastochkin (`remtrik`)
  e altri; basato su `mu_andromeda_platforms` di WOA-Project. Anche `Project-Aloha/UEFIFirmwareBackup` e
  `Project-Aloha/Andromeda`.
- Project Silicium, [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) (Robotix22 e collaboratori).
- Renegade Project, [`edk2-porting/edk2-msm`](https://github.com/edk2-porting/edk2-msm). Microsoft, Project Mu.

**XNU su telefoni Qualcomm**
- [UniXNU](https://github.com/UniXNU/UniXNU) e DarwinBoot.
- [`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus) (Mi 9: touch ST FTS, memoria,
  framebuffer); [`sxl47/UniXNU-cepheus`](https://github.com/sxl47/UniXNU-cepheus).
- [`JohnLeeeeeee678/UniXNU-raphael`](https://github.com/JohnLeeeeeee678/UniXNU-raphael) (K20 Pro: profili per
  dispositivo, root HFS+).
- [`wochgn/UniXNU-guacamole`](https://github.com/wochgn/UniXNU-guacamole) (OnePlus 7 Pro: primo port SM8150).
- `dipa-studio/ios-sweet` (kernel `SWEET`), `blacktop/darwin-xnu-build`.
- HakiOS: [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel),
  [`oosexgang/hakios-uefi`](https://github.com/oosexgang/hakios-uefi);
  [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) (POCO X3 Pro).

**Hardware del Mi Mix 3 5G**
- Xiaomi, [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) ramo `andromeda-p-oss`.
- Nicholas Magill (`Inkypen79`): `kernel_xiaomi_andromeda`, `device_xiaomi_andromeda`, `vendor_xiaomi_andromeda`;
  PixelExperience-Devices / PixelExperience-Blobs `*_xiaomi_andromeda`.
- `AndroidBlobs/vendor_xiaomi_andromeda`; `mauronofrio/android_device_xiaomi_andromeda` (TWRP).
- Danila Tikhonov, driver mainline `panel-samsung-ams639rq08.c` (stessa famiglia di pannello).
- [sm8150-mainline](https://gitlab.com/sm8150-mainline/linux) (Caleb Connolly, Alexey Minnekhanov e altri); postmarketOS
  `linux-postmarketos-qcom-sm8150`.
- Specifiche: [FoneArena](https://www.fonearena.com/blog/276036); stato
  Windows: [Gizchina, 20/05/2022](https://www.gizchina.com/tech/xiaomi-mix-3-5g-with-windows-11-successfully-enables-gpu-acceleration).
- Firmware: [Ximitime, ROM EEA](https://ximitime.com/hyperos/andromeda/stable/eea/).
