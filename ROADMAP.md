# Roadmap: iOS sul Xiaomi Mi Mix 3 5G (`andromeda`)

Obiettivo: il port XNU/iOS più stabile e completo su SM8150. Oggi (6 novembre 2026) iOS 16.7.16 arriva a SpringBoard
sul telefono; la grafica è disegnata dalla CPU e non c'è input.

Il principio guida è non partire da zero: l'SM8150 è il chip con più lavoro fatto in questa scena, sparso in tre posti
che nessuno ha ancora messo insieme:

| Fonte | Cosa dà | Come la usiamo |
|---|---|---|
| [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) + [UniXNU](https://github.com/UniXNU/UniXNU) | kernel `SWEET`, avvio, USB, UFS, framebuffer, sistema iOS | è la base attuale |
| [`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus) (Mi 9), ramo `feat/adreno640` | touch, tasto power, Home, CPU (OSM), temperatura (TSENS), lavoro sulla **stessa GPU** (Adreno 640) | lo allineiamo periodicamente al nostro ramo |
| [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) (POCO X3 Pro, HakiOS) | driver SM8150 di GPU (Mesa freedreno), audio (ADSP, WCD934x), modem, Wi-Fi (non funzionante) | fonte di registri e sequenze di accensione: è un altro kernel, va adattato |

## Ordine di lavoro

| # | Componente | Stato | Difficoltà |
|---|---|---|---|
| 1 | Touch | driver adattato, **in prova** | bassa |
| 2 | Allineamento al ramo `feat/adreno640` del Mi 9 | da fare | media |
| 3 | Tasti, Home, blocco e spegnimento schermo | da fare (arriva col punto 2) | bassa |
| 4 | Avvio senza PC (partizione `boot`) | pronto, dopo il touch | bassa |
| 5 | Memoria: da 1,5 a 2,5-3 GiB | da fare | media |
| 6 | **Batteria e ricarica** | da fare | media-alta |
| 7 | **GPU** (Adreno 640) | da fare, in parallelo col Mi 9 | alta |
| 8 | Frequenze CPU e temperatura | da fare (in parte col punto 2) | media |
| 9 | Luminosità | da fare | media |
| 10 | Audio | da fare | alta |
| 11 | Wi-Fi e Bluetooth | ricerca | molto alta |
| — | App Store, iCloud, telefonate, impronta | non realizzabile (Secure Enclave, server Apple) | — |

### 1. Touch
- **Stato**: kernel con il driver ST-FTS del Mi 9 reso configurabile (`sweettouchrst=54`, `sweettouchvddio=37`, tile
  TLMM per pin, elenco chiuso di pin ammessi). Stessi parametri confermati sul Mi 9: I2C via GPIO, voto RPMh per `ldoc1`,
  60 s di attesa.
- **Fatto quando**: tocchi e trascinamenti arrivano a iOS (log `SWEET TOUCH`, risposta dell'interfaccia).
- **Dopo**: I2C via DMA GPI invece che via GPIO (più veloce, meno CPU), multitouch e gesti.

### 2. Allineamento al Mi 9 (`feat/adreno640`, 144 commit dopo la nostra base)
- **Cosa prendiamo**: correzione della cache per indirizzi fisici sopra i 4 GiB (la nostra finestra passa `0x100000000`),
  tasti solo PON, evento Home, spegnimento rapido dello schermo (patch 0083), sonda e politica delle frequenze OSM, blocco
  termico TSENS, base GPU (GMU, HFI, SMMU, CP).
- **Come**: unione del loro ramo nel nostro, mantenendo le nostre aggiunte (pin del touch, finestra di memoria, script
  generici). Il workflow del kernel replica il loro elenco di file e di test. Ogni novità resta spenta finché non la
  attiviamo con il suo parametro.
- **Fatto quando**: il kernel unito compila e riavvia fino a SpringBoard con touch e tasti.

### 3. Tasti, Home, blocco e schermo
- Power e volume via PMIC (PON, comune all'SM8150), Home come gesto o tasto, blocco con spegnimento del pannello.
- **Fatto quando**: power blocca e sblocca, lo schermo si spegne e si riaccende, il gesto Home esce dalle app.

### 4. Avvio senza PC
- `fastboot flash boot` con l'immagine provata. Il recovery e il fastboot restano raggiungibili; il ritorno ad Android è
  `fastboot flash boot` con `phone/<seriale>/backup/boot.img` più `fastboot -w`.

### 5. Più memoria
- DarwinBoot vuole la finestra del kernel dentro una sola voce della mappa UEFI. Unendo nella mappa di Aloha "DXE Heap"
  (`0xC0300000-0x140000000`) e "RAM Partition" (`0x140000000-0x17FE00000`) si ottiene una voce da ~3 GiB.
- **Come**: patch della mappa di `xiaomi-andromeda`, prova del solo UEFI (menu UniXNU), poi finestra 2,5 GiB, poi 3.

### 6. Batteria e ricarica
- **Perché prima di tutto il resto**: sotto XNU oggi la batteria si scarica e basta (anche sul POCO X3 Pro di HakiOS):
  senza ricarica il telefono non è usabile da nessuno.
- **Come**: (a) lettura in sola lettura via SPMI dei registri PM8150B (caricatore SMB5, fuel gauge Gen4) sotto XNU, per
  capire cosa lascia acceso il bootloader e se un watchdog del caricatore ferma la ricarica; (b) driver minimo che tiene
  viva la ricarica con i limiti del bootloader (stessi valori del kernel Android `qpnp-smb5`); (c) livello e stato della
  batteria a iOS (`IOPMPowerSource`, oggi fisso al 100 %); (d) protezioni: temperatura, tensione massima.
- **Riferimenti**: `drivers/power/supply/qcom/qpnp-smb5.c` e `qpnp-fg-gen4.c` del kernel Xiaomi `andromeda-p-oss`,
  `hakios.battery.c` di HakiOS.

### 7. GPU (Adreno 640)
- **Situazione**: nessuno ha ancora la GPU sotto XNU su SM8150. Il Mi 9 (stessa GPU) sta portando su GMU, HFI, SMMU e
  CP, ma CX e GX non sono ancora alimentati; HakiOS dichiara la resa accelerata via Mesa freedreno (OpenGL ES) sul POCO X3
  Pro; UniXNU ha Metal funzionante sull'Adreno 740 (SM8550).
- **Come**: (a) allinearsi al Mi 9 e contribuire lì, perché ogni passo vale per entrambi; (b) usare HakiOS per le
  sequenze di accensione (GMU, firmware `a640_gmu.bin`/`a640_sqe.fw` e shader ZAP firmato, presi dal telefono e mai messi
  in git); (c) obiettivo intermedio: composizione accelerata di SpringBoard; obiettivo finale: Metal (sul modello
  UniXNU `AdrenoMetal`, adattato da a7xx ad a6xx).

### 8. Frequenze CPU e temperatura
- I core grandi girano a ~711/827 MHz invece di 2,4/2,8 GHz (misura del Mi 9). Si alzano dentro la tabella OSM del
  firmware, senza toccare tensioni, solo con il blocco termico TSENS attivo.

### 9. Luminosità
- Pannello Samsung EA8076 (command mode): luminosità via comandi DCS sul DSI. Riferimento: driver mainline
  `panel-samsung-ams639rq08.c` e il device tree del telefono.

### 10. Audio
- ADSP (caricamento firmware), codec WCD934x su SLIMbus, amplificatore TI TAS2557 (driver nuovo: HakiOS ha TAS2562/2564),
  plug-in audio per iOS.

### 11. Wi-Fi e Bluetooth: vedere come hanno fatto gli altri
- **Wi-Fi WCN3990** (integrato, firmware sul DSP del modem, famiglia ath10k SNOC). Da studiare:
  HakiOS (`hakios.wifi.c`, `hakios.wlfw.c`, `hakios.apple80211.c`: QMI WLFW verso il firmware del modem, anelli CE,
  interfaccia Apple80211 per iOS) che funziona sul POCO X5 Pro (SM7325, WCN6750); UniXNU `Ath12kWifi` sull'SM8550; il
  tentativo `vayu` sullo stesso WCN3990, che si ferma prima di `FW_READY`. La parte verso iOS (Apple80211) è riusabile; la
  parte hardware va scritta per ath10k SNOC partendo dal driver Linux.
- **Bluetooth WCN3998** su UART: l'HCI funziona, ma `bluetoothd` di iOS manda comandi vendor Apple che il chip non conosce
  (problema documentato su `vayu`): serve uno strato che li traduca o li risponda.

## Installare app (.ipa)
- **App dell'App Store: no.** Sono cifrate con FairPlay e legate a un Apple ID; la decifratura richiede componenti Apple
  (driver FairPlay del kernel, Secure Enclave) che qui non esistono.
- **App non cifrate** (compilate da sé, open source, o già decifrate) con codice **arm64**: probabilmente sì, perché la
  verifica delle firme è permissiva. Senza App Store né `installd` raggiungibile, l'installazione passa dal filesystem:
  copia del `.app` in `/Applications` e registrazione presso LaunchServices (come fa `uicache`). Da verificare.
- **Limiti**: le app che usano Metal non disegnano finché non c'è la GPU; quelle che richiedono iCloud, notifiche push o
  acquisti non funzionano; la rete per ora c'è solo via USB (si può condividere quella del PC).

## Metodo
- **Ciclo di prova**: ogni avvio raccoglie il log via USB (`scripts/watch-usb-log.sh` + `scripts/udplog.py`), e uno
  script ne estrae le tappe (montaggio, `launchd`, SpringBoard, driver, panic) per confrontarle con l'avvio precedente.
- **Ogni driver nuovo parte spento** e si accende con il suo parametro: si prova senza ricompilare.
- **Build del kernel più veloci** con una cache dei file compilati nel workflow.
- **Mai scritture fuori da `userdata`**: bootloader, recovery, modem e `persist` non si toccano; i backup sono in
  `phone/` (fuori da git).
- **Restituire**: le correzioni valide per tutti gli SM8150 vanno proposte al port del Mi 9 e agli altri.
