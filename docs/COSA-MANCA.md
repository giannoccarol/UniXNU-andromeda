# Cosa manca: iOS 16 sul Xiaomi Mi Mix 3 5G (`andromeda`)

Stato al 6 novembre 2026, sera. Ramo `andromeda` del fork `giannoccarol/UniXNU-cepheus`.

iOS 16.7.16 parte dal `fastboot boot` fino alla schermata di blocco e alla home; funzionano tasti, USB (rete e log),
memoria UFS e ricarica (in hardware). Tutto il resto è descritto qui: cosa è pronto da provare, cosa serve da te, e
componente per componente il prossimo passo. I crediti di ogni lavoro ripreso o consultato sono in
[CREDITS.md](../CREDITS.md) e nella sezione "Fonti" di ciascuna ricerca in [docs/research/](research/).

---

## 1. Pronto da provare sul telefono

Quattro immagini, già costruite sul PC (in `out/andromeda/boot/`, fuori da git) con il kernel compilato su GitHub.
Ognuna si avvia **solo in RAM** con `fastboot boot`: nessuna scrittura sul telefono.

| Immagine | Cosa contiene | UEFI |
|---|---|---|
| `boot-andromeda-xnu.img` | tutto quello che segue nella tabella delle prove | quello già provato |
| `boot-andromeda-xnu-btbl.img` | la principale + prova Bluetooth (`andrbt=1`) e luminosità (`andrbl=1`) | quello già provato |
| `boot-andromeda-xnu-3g.img` | la principale con **~2,94 GiB** di memoria per iOS (`0xBC000000`) | nuovo, con la RAM unita |
| `boot-andromeda-xnu-fb.img` | la principale + ritorno automatico in fastboot dopo 5 minuti (`andrfastboot=300`) | quello già provato |

Il log via USB si legge con `scripts/udplog-ts.py` e si riassume con
`python3 scripts/andstatus.py <log> --follow`.

### Ordine delle prove

| # | Prova | Cosa fai tu | Cosa guardo nel log / sullo schermo |
|---|---|---|---|
| 1 | **Touch passo per passo** (`sweettouchexp=1`) | dalla schermata di blocco, tocchi lo schermo di continuo per 2 minuti e mezzo, telefono appoggiato | `SWEET TOUCH EXP: step N ended, finger-enter events in it: K`: il passo con K > 0 dice quale comando sblocca le dita |
| 2 | **Display intero** (`sweetfb_fit=1`) | guardi la barra in alto | ora e batteria non più tagliate; `SWEET FB: fit 1125x2436 -> 1080x2338` |
| 3 | **Sfondo della schermata di blocco** (servizio AMFI + indirizzi MAC) | guardi la schermata di blocco | sfondo vero invece dell'orologio rosso; `SWEET AMFI: trust cache query …: trusted` |
| 4 | **CPU con governatore** (`andrcpu=1 andrgov=1`) | niente | `ANDROMEDA CPU: gov gold 60s: avg … MHz …` ogni minuto; temperature sotto 80 °C |
| 5 | **Batteria vera** (`sweetbattery=2`) | guardi l'icona | percentuale vera; `SWEET BATTERY: PM8150B fuel gauge, …%` |
| 6 | **GPU gradino 1** (`andrgpu=1`, sola lettura) | niente | `ANDROMEDA GPU: probe: firmware … found + SHA-256 OK` (4 righe), `step 1 (probe) ok` |
| 7 | **Slider** (`andrslider=1`) | apri e chiudi lo slider qualche volta | `ANDROMEDA SLIDER: hall42=… hall114=… -> up (open)/down (closed)` |
| 8 | **Ritorno in fastboot** (`andrfastboot=N`) | resti vicino: se partisse Android, spegni subito tenendo premuto power | immagine `-fb`; dopo 5 minuti il telefono deve tornare da solo in fastboot (`ANDROMEDA PROBE: REBOOT …` nel log) |
| 9 | **Bluetooth B1 e luminosità** (immagine `-btbl`) | guardi lo schermo dopo 30 s | luminosità che cambia massimo → un quarto → massimo; `ANDROMEDA BT: B1 PASS` |
| 10 | **3 GiB di memoria** (immagine `-3g`) | niente | parte come la principale; se non parte, DarwinBoot mostra la mappa e basta riavviare con l'immagine principale |
| 11 | **GPU gradini 2 → 4**, uno per avvio | niente | `gmu: init result babeface BOOTED`, poi `zap: auth and reset rc 0`, poi `step 4 (CP_MEM_WRITE) PASS` |

Se la prova 8 funziona, i test successivi si possono fare anche senza di te: avvio, log, ritorno in fastboot,
kernel successivo.

---

## 2. Decisioni e passi che servono da te

| Cosa | Perché | Rischio |
|---|---|---|
| **Riscrivere il sistema iOS su `userdata`** con le ultime modifiche (`bluetoothd` e `fairplayd` tolti dall'avvio) | boot più veloce: oggi si riavviano 46 volte ciascuno | è una scrittura sulla partizione; i dati di iOS di prova si perdono |
| **Avvio senza PC** (`scripts/install-boot.sh <seriale>`) | il telefono parte in iOS da solo; un riavvio non finisce più in Android (che non troverebbe la sua `userdata`) | scrittura della partizione `boot`; il backup di Android c'è e `--restore` lo rimette |
| **Leggere la partizione `modem`** con TWRP in RAM, in sola lettura | firmware di Wi-Fi (`wlanmdsp.mbn`, `bdwlan.*`), DSP audio e DSP sensori | nessuno, come per la GPU |
| **Copia delle partizioni EFS del modem 5G** (`mdm1m9kefs*`) in sola lettura | backup prima di qualsiasi lavoro sul modem | nessuno |
| **Messaggio a MakrSas** (ios-topaz) per la licenza di `radio.c` | la parte radio del Wi-Fi di ios-flame deriva da lì; senza permesso va riscritta da ath10k | nessuno (testo pronto in [richieste-permesso.md](research/richieste-permesso.md)) |
| **Pannello di controllo a telefono acceso** | accendere/spegnere i driver senza riavviare | un servizio che riceve comandi dalla rete va progettato con cura (vedi §3, Diagnostica); da decidere insieme il perimetro |

---

## 3. Componente per componente

Legenda difficoltà: **B** bassa, **M** media, **A** alta, **MA** molto alta.

| Componente | Stato oggi | Prossimo passo | Diff. | Riferimenti principali |
|---|---|---|---|---|
| **Touch** | il chip risponde e segnala contatti (rumore del display), non le dita | prova passo per passo (§1, prova 1) | B–M | Mi 9 (YangQi0408), ios-flame (Nick Wylynko), driver Xiaomi `fts_521` |
| **Display** | funziona; immagine ridotta del 4 % invece di ritagliata (da provare) | frequenza 60 Hz e vsync (ios-flame 0121), poi composizione con la GPU | M | ios-sweet, ios-flame |
| **Schermata di blocco** | orologio rosso: PosterBoard senza fornitori di sfondi | servizio AMFI + indirizzi MAC (da provare, §1 prova 3) | B | ios-flame (patch 0103, `mkadt.py`) |
| **CPU** | pronto: piena velocità, governatore a carico, protezione termica | misure sul telefono, taratura | B–M | ios-flame, Mi 9, Linux schedutil/interactive |
| **Memoria** | 1,5 GiB; patch UEFI per ~3 GiB compilata | prova dell'immagine `-3g` | M | Project Aloha (RamPartitionDxe), DarwinBoot (UniXNU) |
| **Ricarica** | funziona in hardware (FULLON, ~40 mA dal PC); watchdog innocuo | corrente d'ingresso giusta con un caricatore da muro | M | [ricarica.md](research/ricarica.md): ios-mh2lm, Linux qcom_smbx |
| **Batteria per iOS** | pronta (`sweetbattery=2`) | temperatura vera dalla SRAM del fuel gauge | M | ios-sweet `SweetBattery` |
| **GPU** | driver di ios-flame integrato, gradino 1 in sola lettura | gradini 2–4, poi copia a schermo dalla GPU, Turnip, Metal | A | [gpu.md](research/gpu.md): ios-flame, UniXNU, Mesa, metal2vulkan, Mi 9 |
| **Bluetooth** | B1 pronto (accensione, versione, reset) | caricamento firmware/NVM, poi `bluetoothd` come controller | A | Linux btqca/hci_qca, renoir_xnu_bluethooth (daimile2), ios-flame 0102 |
| **Wi-Fi** | piano pronto ([wifi-flame.md](research/wifi-flame.md)) | firmware dalla partizione `modem`, poi `andrwlan=1..3`: avvio del modem e del firmware WLAN | MA | ios-flame `wland` (MIT), HakiOS (APSL), ios-topaz (solo riferimento) |
| **Luminosità** | DCS `0x51` pronto (`andrbl=1/2`) | collegarla al cursore di iOS senza l'attenuazione digitale lenta | M | ios-topaz (fatti), UniXNU-sheng, Linux dsi_host |
| **Audio** | ricerca fatta ([audio.md](research/audio.md)) | A0: audio "muto" + plug-in HAL per stabilizzare `mediaserverd` (sblocca anche i video), poi DSP audio via TrustZone | A | HakiOS (`HakiosAudio.driver` MIT, driver APSL), hakios-sm8150 (riferimento) |
| **Vibrazione** | registri letti in sola lettura | due impulsi di prova da 200 ms con te presente, poi via audio "Actuator" | M | Qualcomm `qti-haptics.c` (documentazione), HakiOS `redwood` |
| **Slider** | lettura pronta (`andrslider=1`) | evento HID per iOS (silenzioso o apertura fotocamera frontale) | B | Xiaomi `halls.c`, Demon000 (LineageOS) |
| **Sensori** (rotazione, luce, prossimità) | tutti dietro il DSP dei sensori (SLPI); la prossimità è a ultrasuoni nel DSP audio | avvio dello SLPI via TrustZone, poi QMI; dal lato iOS basta un servizio HID "accelerometro" | MA | [sensori-vibrazione.md](research/sensori-vibrazione.md): libssc, hexagonrpc, Scion |
| **Sospensione e consumo** | il telefono non dorme mai | misura in mA, meno polling, poi spegnere i core a schermo spento, poi il pannello | A | [sospensione.md](research/sospensione.md): HakiOS (`CPU_OFF`, APSL), Linux s2idle |
| **Fotocamere** | ricerca fatta; file di accensione dei sensori estratti (`phone/e719dcab/camera/`, fuori da git) | C1: accendere l'IMX363 e leggere il suo ID `0x0363` | A | [fotocamere.md](research/fotocamere.md): ios-sunfish e ios-redfin (MIT), HakiOS, Linux CAMSS (A. Gigena) |
| **App** | l'App Store non è percorribile (FairPlay); app **non cifrate** sì | installazione via copia in `/Applications` + registrazione in LaunchServices | M | [app-store.md](research/app-store.md): ios-flame, ios-apollo |
| **Internet via USB** | messo in secondo piano (il Wi-Fi è la priorità) | l'interfaccia USB come scheda di rete IOKit vera | M | ios-sweet NOTES, HakiOS |
| **Rete cellulare, SMS, dati** | ricerca fatta ([modem-gps-nfc.md](research/modem-gps-nfc.md)) | arriva dopo il Wi-Fi (stesso avvio del modem); dati 4G con QcomBaseband | MA | UniXNU QcomBaseband (HoshinoYumeka), UniXNU-OnePlus-Ace5 (tycykp) |
| **GPS** | ricerca fatta | client QMI LOC dopo l'avvio del modem, posizioni a `locationd` | A | iOSPlus10 (akshaynexus) |
| **Avvio senza PC** | script pronto | da fare con la tua conferma (§2) | B | — |
| **Velocità di avvio** | lock screen a ~116 s | sistema iOS con i demoni inutili tolti (§2); CPU veloce già dall'inizio | B | — |
| **Diagnostica** | `scripts/andstatus.py`: stato leggibile dal log | sul telefono, una riga di stato periodica unica | B | — |

---

## 4. Non realizzabile (o non ha senso provarci)

| Cosa | Perché |
|---|---|
| App dell'App Store (cifrate) | FairPlay: il kernel le avvia solo con il componente Apple legato all'hardware Apple (`mach_loader.c`, `text_crypter_create`). Nessun port ci riesce |
| iCloud, Apple ID di sistema, Face ID / Touch ID, Apple Pay | richiedono il Secure Enclave e i server Apple |
| NFC per iOS | `nfcd` vuole il controller NXP con firmware Apple e Secure Element |
| 5G (modem X50) | servono PCIe, MHI e un protocollo che nemmeno Linux ha funzionante |
| Chiamate VoLTE | l'IMS di iOS usa servizi QMI privati di Apple che il firmware Xiaomi non ha (il 2G/CSFB resta "molto difficile") |

Regole che restano ferme: non si usa l'identità di un iPhone vero (seriali, UDID, Apple ID altrui), l'IMEI non si tocca
mai, i file Apple e i dati del telefono non entrano nel repository.

---

## 5. Dove sono le cose

- Stato e istruzioni: [ANDROMEDA.md](../ANDROMEDA.md); ordine dei lavori: [ROADMAP.md](../ROADMAP.md).
- Ricerche: [docs/research/](research/) — `gpu.md`, `wifi-flame.md`, `wifi-bluetooth.md`, `ricarica.md`, `audio.md`,
  `fotocamere.md`, `sospensione.md`, `sensori-vibrazione.md`, `modem-gps-nfc.md`, `app-store.md`.
- Driver nostri: `xnu/andromeda_*.c` (sonda, CPU e governatore, GPU, Bluetooth, luminosità), patch `0089`–`0096`.
- Crediti: [CREDITS.md](../CREDITS.md). Il progetto si regge sul lavoro di molti: ios-sweet (dipa-studio), il port del Mi 9
  (YangQi0408), ios-flame (Nick Wylynko), UniXNU, Project Aloha, HakiOS (OOSEX GANG) e gli altri citati lì.
