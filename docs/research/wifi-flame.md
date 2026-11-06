# Il Wi-Fi di ios-flame su `andromeda`: piano di port

Ricerca del 6 novembre 2026, solo lettura: nessun comando sul telefono, nessun codice di ios-flame eseguito, nessuna
modifica allo stato git. Completa il piano generale [wifi-bluetooth.md](wifi-bluetooth.md) (fasi W0–W6, tabella
dell'hardware) con il port che oggi è **il più vicino a noi**:
[`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame) di **Nick Wylynko**, Pixel 4 (`flame`). Ha lo stesso SoC
(SM8150), lo stesso chip Wi-Fi (WCN3990, firmware nel dominio WLAN del modem), la stessa base ios-sweet (`xnu-8796`,
`SWEET`) e la stessa userland dell'iPhone X 16.7.16 (20H392). I valori di `andromeda` vengono dal device tree attivo
(`phone/<seriale>/live-fdt.dts`, fuori da git), dalla mappa UEFI di Aloha e dalle copie locali delle partizioni; gli
altri fatti dai file citati in [Fonti](#7-fonti).

Legenda della certezza, la stessa di [gpu.md](gpu.md):

- **✔** letto nel sorgente o nei dati del telefono, oppure misurato da altri su hardware uguale;
- **~** dedotto: coerente con più indizi, non ancora visto su `andromeda`;
- **?** da verificare.

## Sommario

- **Su flame il Wi-Fi funziona davvero, dalla schermata Impostazioni.** ✔ README e HANDOFF (22 ottobre 2026, kernel
  0123/0124): scansione, WPA2-PSK, connessione automatica all'avvio, riconnessione dopo la perdita del link, `wifid` su
  `en0` con il DHCP di iOS, Safari in HTTPS. Restano aperte la rete etichettata "Unsecured" finché `wifid` non la salva
  da sé, il join di un'altra rete da Impostazioni e il riavvio del modem senza riavviare il telefono.
- **L'architettura è diversa da topaz e HakiOS: la radio gira in user space.** Il kernel offre solo tre cose:
  - `/dev/pmem`: memoria fisica e chiamate SMC per i driver in user space (patch 0094);
  - voti RPMh dallo user space (0102);
  - `en0` più le richieste Apple80211 di `wifid`, collegate a un demone tramite `/dev/wlan` (0123).

  Tutto il resto lo fa il demone `wland` (`phone/wifi/`, ~5.900 righe): avvio del modem via TrustZone, SMP2P, GLINK e
  QRTR, i servizi rmtfs, pd-mapper e tftp, WLFW fino alla modalità *mission*, copy engine, HTC, WMI, HTT, scansione,
  join e handshake WPA2.
- **Licenze: si può riusare quasi tutto, tranne la radio.**
  - Il codice di `wland` fino a WLFW è di flame e ricade sotto la sua MIT. HANDOFF mostra che è stato scritto prima di
    leggere topaz.
  - I file del kernel sono APSL 2.0. La superficie Apple80211 (`redwood_*`, `RedwoodWLAN`, `MiniIONetworkStack`) viene
    da HakiOS, che la pubblica sotto APSL 2.0.
  - **`radio.c` è un port di `TopazModem` di ios-topaz** (copy engine, WMI, HTT, stazione, WPA2), e ios-topaz non ha
    licenza. Va riscritto da ath10k (ISC), oppure serve il permesso scritto di MakrSas.
- **Su `andromeda` cambiano pochi valori.** Restano uguali SMEM, SMP2P, GLINK, QMP, MSA, registri WLAN, SID SMMU
  `0x640` e i quattro LDO con le stesse tensioni. Cambiano:
  - la regione del modem: **`0x8E000000` + `0x9600000`** invece di `0x8DC00000` + `0xB400000`;
  - il nome del file della scheda: `bdwlan.bNN` scelto dal `board_id` di WLFW invece di `bdwlan-flame.bin`;
  - i prefissi tftp e la tabella pd-mapper, che vanno presi dai nostri file;
  - il buffer rmtfs: proponiamo `MPSS_EFS` `0xC0000000` (3 MiB), riservato da Aloha;
  - il pool dei copy engine, che va messo fuori dalla nostra finestra XNU (sul Pixel è `0xE0000000`).
- **Firmware: tutto nella partizione `modem`.** La copia locale del `vendor` (quella estratta per la GPU) **non contiene
  né `wlanmdsp.mbn` né `bdwlan.*`** ✔. La mappa tftp del vendor dice che il modem legge `/readonly/firmware` dalla
  partizione `modem`, e `/readwrite` da `persist` ✔. Va estratta una volta, in sola lettura, con TWRP avviato in RAM
  (§4.2).
- **Piano.** Seguiamo il metodo di flame (driver in user space, un passo per avvio, un solo avvio del modem per ogni
  accensione del telefono) e la sicurezza del nostro progetto:
  - `/dev/pmem` spento di default, con elenco chiuso di indirizzi e di SMC;
  - mai `PAS_SHUTDOWN`;
  - log nel flusso USB in diretta, tramite il tee `sweetnull` di ios-sweet.

  W0–W3 (modem e WLFW) si possono fare subito con il codice MIT di flame. W4–W5 aspettano la riscrittura o il permesso
  per `radio.c`.

---

## 1. Come funziona il Wi-Fi in ios-flame

### 1.1 Chi fa cosa

```
 wifid / Impostazioni / configd (iOS 20H392, invariati)
        │ SIOCGA80211/SIOCSA80211, eventi KEV 6/1, IOKit                        ── kernel ──
 redwood_apple80211.c ─ redwood_sta.c ─ flame_sta.c ─ flame_wlan.c (/dev/wlan) ─ redwood_net.c (en0)
 RedwoodWLAN.cpp (IO80211Controller + IONetworkInterface), MiniIONetworkStack.cpp, quiescenza IOKit
        │ /dev/wlan: un messaggio per read/write (flamewlan.h)
 wland (user space, root) ── bridge.c ─ radio.c (CE/HTC/WMI/HTT, WPA2) ─ wlfw.c ─ rmtfs/pdmapper/tftp
        │                                   link.c (GLINK IPCRTR + QRTR) ─ glink.c ─ smem.c ─ qmi.c
        │ /dev/pmem: pread/pwrite su indirizzi fisici, PMEM_ALLOC, PMEM_SMC, PMEM_RPMH
 hardware: SMEM, APCS IPC, AOP QMP, RPMh, TrustZone (PAS, hyp_assign), apps SMMU, registri WLAN 0x18800000
```

| Strato | Dove | Cosa fa (✔ sorgente) |
|---|---|---|
| Accesso all'hardware | kernel `bsd/dev/sweet_pmem.c` (patch 0094) | `pread`/`pwrite` con offset = indirizzo fisico. La RAM gestita da XNU passa per la physmap, con pulizia e invalidazione della cache prima e dopo. Il resto (MMIO, carve-out) è mappato come memoria device, solo accessi a 32 bit. `PMEM_ALLOC` dà memoria contigua azzerata; `PMEM_SMC` esegue una SMC con x0..x7; `PMEM_LIST`. Solo root (0600); `sweetpmem=0` lo spegne (**da loro è acceso di default**) |
| Voti RPMh | kernel `sweet_rpmh.c` (ios-sweet 0062, estesa da flame 0102) | `PMEM_RPMH`: da 1 a 4 comandi `(offset, valore)` su una risorsa VRM o ARC presa per nome da cmd-db; con la 0120 anche le BCM |
| Superficie iOS | kernel, patch 0123 | §1.3 |
| Orchestrazione | `wland.c` (827 righe) | Riga di comando (`smem`, `smp2p`, `cmddb`, `levels`, `vote`, `bcm`, `qmp`, `wlan-power`, `smmu`, `boot`, `shutdown`, `net`, `up`, `auto`). Contiene il lettore di cmd-db, la mailbox AOP QMP, l'elemento SMP2P in uscita con l'ack SSR, l'avvio PAS del modem, il ciclo `net`, il lavoro di avvio `auto` e il join da `wlan.conf` |
| SMEM | `smem.c` | Tabella delle partizioni `$TOC` (v12), elementi cached e uncached, allocazione sotto il mutex TCSR 3 |
| GLINK, QRTR | `glink.c`, `link.c` | FIFO SMEM (elementi 478/479/480), campanello APCS bit 12, canale `IPCRTR`, intent e riassemblaggio; QRTR con noi come nodo 1 e il modem come nodo 0; HELLO, NEW_SERVER, RESUME_TX |
| QMI | `qmi.c` | Formato TLV di `qmi_encdec.c`; buffer da 8 KiB (il BDF arriva a 6.182 byte) |
| Servizi host | `rmtfs.c` (servizio 14), `pdmapper.c` (0x40), `tftp.c` (4096) | EFS del modem da file immagine; mappa dei domini di protezione; immagini lette dal modem (`wlanmdsp.mbn`) e una zona `/readwrite` |
| SCM | `scm.c` | `hyp_assign` (`MEM_PROT_ASSIGN`, SIP `0x0C`/`0x16`) |
| WLFW | `wlfw.c` (344) | Sequenza di ath10k `qmi.c`, poi `WLAN_CFG` con le tabelle CE di `snoc.c` e `WLAN_MODE` mission |
| Radio | `radio.c` (2.092) | SMMU identità per il SID `0x640`, 12 copy engine a polling, HTC, WMI TLV, anello HTT RX, scansione passiva su 37 canali, stazione (VDEV, PEER, AUTH/ASSOC come frame di gestione WMI), handshake WPA2 a 4 vie e di gruppo con CommonCrypto, dati HTT native-wifi ↔ Ethernet, riconnessione |
| IP senza `wifid` | `ipnet.c` | `utun`, ARP, client DHCP, `netsvc` verso configd. È il ripiego quando manca `/dev/wlan` |
| Ponte verso il kernel | `bridge.c`, `flamewlan.h` | Il protocollo di `/dev/wlan` |

### 1.2 La sequenza di `wland up`, nell'ordine

Ogni passo è "✔ sorgente + HANDOFF"; tra parentesi il file Linux che flame dichiara di seguire.

1. **SMMU** (`radio.c: smmu_wlan_identity`): trova uno SMR e un context bank liberi sulla apps SMMU `0x15000000` e ci
   manda lo stream `0x640/0x1` con la traduzione spenta (CBAR `0x1f000`, CBA2R AArch64, SCTLR `0xe0`, M=0). È lo
   stesso modo in cui ABL fa girare i suoi stream. Va fatto **prima** che il firmware WLAN tocchi la memoria
   dell'host.
2. **Alimentazioni** (`wlan_power`):
   - LDO via RPMh VRM, in HPM e accesi: `ldoa1` 752 mV, `ldoa7` 1800 mV, `ldoc2` 1304 mV, `ldoc11` 3104 mV;
   - `rfclka2` acceso;
   - **messaggio QMP `{class: clock, res: qdss, val: 1}`**: senza il clock QDSS il firmware va in eccezione quando
     parte la calibrazione (PFR `EX:wlan_process:0x1:WLAN RT:0x78:PC=0xb01976e8`).
3. **SMP2P** (`smp2p_create`): crea l'elemento in uscita 428 ("master-kernel") **prima** di avviare il modem, poi suona
   il bit 14 dell'APCS. Altrimenti il modem non segnala mai "ready".
4. **Avvio del modem** (`boot_modem`; `qcom_q6v5_pas`, `mdt_loader`):
   1. voti `cx.lvl` e `mss.lvl` al livello più alto di cmd-db;
   2. `INIT_IMAGE` (PAS 4) con intestazione ELF + segmento hash in un buffer `PMEM_ALLOC`;
   3. `MEM_SETUP` della regione;
   4. copia di `modem.bNN` all'indirizzo fisico di ogni segmento (rifiuta i segmenti fuori dalla regione, **senza
      rilocare**);
   5. QMP `{class: image, res: load_state, name: modem, val: on}`;
   6. `AUTH_AND_RESET`;
   7. attesa del bit 1 "ready" (senza bit 0 "err-fatal") nell'elemento 435, al massimo 30 s.
5. **GLINK + QRTR** (`link_up`), poi i servizi, che restano attivi **per tutta la sessione**:
   - **rmtfs**: buffer da 2 MiB con 4 KiB di guardia per lato, `hyp_assign` HLOS → HLOS+MSS **una sola volta per
     accensione** (l'indirizzo è salvato in `/var/local/fw/rmtfs/.buffer`); file `modemst1/2`, `fsg`, `fsc` in
     `/var/local/fw/rmtfs`;
   - **pd-mapper**;
   - **tftp**: `/readonly/firmware/image/` → `/var/local/fw/modem`; `/readwrite/` → `/var/local/fw/rw`; blocchi al
     massimo da 1024 byte.
6. **WLFW** (`wlfw_start`, QMI 0x45):
   1. `IND_REGISTER` (fw_ready, msa_ready, pin_connect, client id ath10k);
   2. `HOST_CAP` (`daemon_support=0`);
   3. `MSA_INFO` (`0x8BC00000`, 1 MiB);
   4. dopo 20 ms, `hyp_assign` di ogni regione MSA a {MSS_MSA `0xF`, WLAN `0x18`, + WLAN_CE `0x19` se non è sicura};
   5. `MSA_READY`, poi `CAP` (chip, board, soc, versione, build id);
   6. attesa dell'indicazione MSA_READY;
   7. **`BDF_DOWNLOAD`** in segmenti da 6.144 byte (`file_id 0`; un errore CRC sull'ultimo segmento non è fatale);
   8. **`MAC_ADDR` (0x33)** con un indirizzo amministrato localmente, poi **`CAL_REPORT`**;
   9. `FW_READY`, `WLAN_CFG` (12 CE, 21 servizi, 12 registri shadow), `WLAN_MODE` 0 (mission).
7. **Radio** (`radio_start`, 100 ms dopo mission: prima di allora un accesso ai CE blocca il bus):
   - copy engine a `0x18A40000` + `0x1000·n`, indirizzo RRI in DDR a `+0xC004`, registri shadow a `0x18832000`;
   - anelli e buffer in un **pool di RAM che XNU non gestisce** (`0xE0000000`, 16 MiB, mappato come device: nessuna
     manutenzione della cache);
   - HTC READY, WMI SERVICE_READY → INIT → READY, HTT (senza l'anello RX nessun beacon arriva alla scansione);
   - lista di canali e scansione passiva.
8. **Ponte**: `/dev/wlan` aperto, HELLO dopo la prima scansione (il kernel crea `en0`), poi `wifid` comanda scansioni e
   join. Senza `/dev/wlan`: `utun` + DHCP propri (`ipnet.c`) e join da `/var/local/wlan.conf` (`ssid=`, `psk=`, solo
   sul telefono).
9. **All'avvio**: il job launchd `org.iosonpixel.wland` esegue `wland auto`:
   - al massimo una volta per accensione (timbro `kern.boottime` in `/var/local/wland.boot`);
   - spento da `/var/local/wland.off` o `sweetwifi=0`;
   - alza da sé il proprio limite jetsam a 2048 MB, perché launchd lo uccideva mentre caricava il firmware.

**Tempi misurati da flame** ✔ (HANDOFF "WiFi works", corsa wifi12):

| Momento | Tempo |
|---|---|
| Mission mode | 18,0 s dall'avvio di `wland` |
| WMI READY | +170 ms |
| Scansione | 3 s |
| Auth + assoc + 4 vie | 170 ms |
| DHCP | 4 s |
| Join + DHCP da `wland auto` | ~23 s |
| Riavvio del modem | ~70 s, quasi tutti per i 3,7 MB di `wlanmdsp.mbn` via tftp |
| CPU di `wland` a riposo | ~1,6 % di un core |
| Velocità | ~5,7 Mbit/s |

### 1.3 La superficie verso iOS (patch 0123)

- **`en0`** (`redwood_net.c`): KPI di interfaccia BSD, framing Ethernet, `SIOCGIFMEDIA` 802.11. Lo stato del link va a
  configd.
- **Apple80211** (`redwood_apple80211.c`): `SIOCSA80211 0x802869c8` e `SIOCGA80211 0xc02869c9`, eventi
  `KEV_IEEE80211_CLASS`. Layout e tipi sono quelli di [wifi-bluetooth.md §2](wifi-bluetooth.md) e §7.
- **Stazione** (`redwood_sta.c`): un thread esegue tutte le chiamate, una alla volta. `flame_sta.c` le traduce in
  richieste per `wland`.
- **`/dev/wlan`** (`flame_wlan.c`, `sys/flame_wlan.h` = `flamewlan.h`):
  - dal kernel a `wland`: `SCAN`, `JOIN` (SSID, BSSID, passphrase o PMK in esadecimale), `LEAVE`, `TX` (frame
    Ethernet);
  - da `wland` al kernel: `HELLO`, `BSS_BEGIN`/`BSS`/`SCAN_DONE`, `JOIN_DONE`, `LEFT`, `LINK`, `RX`;
  - scan e join bloccano chi chiama fino alla risposta (20 s e 40 s);
  - `sweetwifid=0` spegne il dispositivo.
- **IOKit**:
  - `RedwoodWLAN.cpp`: `IO80211Controller` con `IOMACAddress`, e un `IONetworkInterface` con
    `IO80211InterfaceRole = "Infrastructure"`. Nessun user client, quindi IO80211.framework ripiega sulle ioctl;
  - `MiniIONetworkStack.cpp`: senza, l'InterfaceNamer di configd non pubblica `*QUIET*`;
  - quiescenza del registro (topaz 0128, in `IOService.cpp`/`IOStartIOKit.cpp`): altrimenti `wifid` blocca l'avvio.
- **Portachiavi**: `kAKSWrapKey`/`kAKSUnwrapKey` in `SweetKeyStore.cpp` (da HakiOS).
- **ADT** (`tools/mkadt.py`):
  - nodo `/arm-io/wlan`: launchd avvia `wifid` solo se esiste (`_LimitLoadToDeviceTree`);
  - `/chosen` `mac-address-wifi0`, `-bluetooth0`, `-ethernet0` (amministrati localmente). **Senza, MobileGestalt aspetta
    5 s per ogni richiesta dell'indirizzo Wi-Fi**: installd impiega 5 s per bundle e LaunchServices non finisce mai di
    registrare le estensioni (HANDOFF "Black wallpaper fixed").
- **configd**: il `preferences.plist` dell'immagine non aveva servizi di rete. Flame lo ha spostato da parte, e configd
  ha creato la configurazione predefinita con un servizio Wi-Fi su `en0`.

### 1.4 Cosa ha insegnato a flame (da non riscoprire)

✔ HANDOFF "WiFi, part 1" e "WiFi works". **Ognuno di questi punti era necessario.**

- **SMP2P**:
  - l'elemento 428 deve esistere prima che il modem parta;
  - prima di un riavvio va azzerato il vecchio "ready" del modem;
  - dopo ogni riavvio del modem il suo RESTART_DONE va ricopiato nel nostro RESTART_ACK.
- **rmtfs**: senza, il modem va in err-fatal dopo ~40 s, e nell'elemento 421 non c'è nessuna stringa.
- **QRTR**: i pacchetti di controllo sono sempre di 20 byte. Un `RESUME_TX` con il layout sbagliato blocca tutti i
  client dopo ~8 pacchetti.
- **GLINK**:
  - gli id degli intent sono per canale;
  - i pacchetti grandi richiedono intent da 30 ms l'uno, quindi i blocchi tftp sono limitati a 1024 byte.
- **Alimentazioni e clock QDSS prima dell'avvio del modem.**
- **Buffer QMI da 8 KiB**: con 4 KiB i segmenti del BDF perdevano il TLV dei dati in silenzio.
- **MSA e rmtfs vanno assegnati una sola volta per accensione.** Riprendere l'MSA mentre il WLAN gira, o rifare
  l'`hyp_assign` di rmtfs dopo qualche riavvio, è **una SMC che non ritorna e blocca una CPU**: si riavvia il telefono.
- **Pool non gestito da XNU.** SWEET non ha PPL: la physmap mappa tutta la RAM gestita come cacheable. Ogni vista WC
  o NC di una pagina gestita è un alias incoerente, con scritture perse e letture vecchie ("The big trap").
- **Jetsam**: launchd uccide `wland` durante il caricamento del firmware se non alza il proprio limite.

---

## 2. Provenienza e licenze, file per file

Cosa dichiarano i progetti (✔ letto):

- **ios-flame**:
  - `LICENSE`: MIT, "Copyright (c) 2026 dipa-studio" (quella di ios-sweet);
  - `CREDITS.md`: le aggiunte di flame sono MIT ("the ios-flame contributors"), tranne `xnu/patches/`, che resta APSL
    2.0 come i file XNU;
  - `phone/wifi/radio.c` "is a userland port of its TopazModem radio (its patches 0123-0130)";
  - la patch 0123 contiene la 0900 e la 0128 di topaz;
  - `phone/wifi/` "follows" ath10k (ISC), i driver Linux SMEM/SMP2P/GLINK/QRTR/remoteproc (GPL-2.0), e rmtfs,
    pd-mapper e tqftpserv di Bjorn Andersson;
  - nessun file di `phone/wifi/` ha un'intestazione di licenza.
- **ios-topaz** (MakrSas, `e4772911`):
  - **nessun file `LICENSE`** nell'albero;
  - `CREDITS.md` dichiara APSL 2.0 solo per le parti derivate da HakiOS in `0900-*` e `0901-*`…`0947-*`
    (`redwood_apple80211`, `redwood_sta`, `redwood_net`, …);
  - le patch `0113`–`0130` (`TopazModem.cpp`) non sono coperte, e `TopazModem` dice di essere un port di
    `MakrSas/topaz-woa` (driver Windows, licenza non vista);
  - l'oggetto della 0900 è "from darwin-redwood, PRIVATE".
- **HakiOS** (OOSEX GANG, `63fc2b46`): `LICENSE` mette sotto **APSL 2.0** "xnu/hakios.patch and everything under
  xnu/files/ and drivers/", e il resto sotto MIT.
- **Confronto diretto** dei file `redwood_*` di flame con i `hakios.*` di HakiOS, rinominando
  `hakios`/`redwood`:

  | File | Righe diverse |
  |---|---|
  | `redwood_apple80211.c` | 34 su 739, solo commenti ("iOS" ↔ "Darwin") e percorsi degli include |
  | `redwood_sta.c` | 8 su 371 |
  | `MiniIONetworkStack.cpp` | 4 su 114 |
  | `RedwoodWLAN.cpp` | 54 (HakiOS si è evoluto dopo) |
  | `redwood_net.c` | 184 (HakiOS si è evoluto dopo) |

  Sono lo stesso codice di HakiOS ✔.

Decisione per ogni pezzo. **Riuso** vuol dire: copiamo o adattiamo mantenendo intestazione e licenza, con la voce in
`CREDITS.md` nello stesso cambiamento. **Riferimento** vuol dire: leggiamo, poi riscriviamo da fonti con licenza chiara.

| Pezzo (ios-flame) | Autore / origine | Licenza | Per noi |
|---|---|---|---|
| `phone/wifi/wland.c`, `smem.c/h`, `glink.c/h`, `link.c/h`, `qmi.c/h`, `rmtfs.c/h`, `pdmapper.c/h`, `tftp.c/h`, `scm.c/h`, `pmem.c/h` | Nick Wylynko. Scritti fino a WLFW il 21/10 tra le 05:30 e le 06:55, **prima** di leggere topaz (HANDOFF "Reference repos fetched", 18:30) ✔. Seguono come documentazione Linux e i demoni di Andersson | MIT (ios-flame) | **Riuso** con l'avviso MIT (c'è già `licenses/ios-flame-MIT.txt`) e un'intestazione che dice da dove vengono. La tabella del pd-mapper e i percorsi vanno sostituiti con i nostri (§3) |
| `phone/wifi/wlfw.c/h` | Nick Wylynko, ordine di ath10k `qmi.c` | MIT; le tabelle CE, servizi e shadow sono dati di ath10k `snoc.c` (ISC) | **Riuso**, più l'avviso ISC di ath10k (Qualcomm Atheros) in `licenses/` per le tabelle, per prudenza |
| `phone/wifi/bridge.c/h`, `flamewlan.h`, `ipnet.c/h`, `build.sh` | Nick Wylynko | MIT | **Riuso** (`ipnet.c` solo come ripiego) |
| `phone/wifi/ath.c/h` | Nick Wylynko, prima prova; `build.sh` non la compila ✔ | MIT | Non serve |
| **`phone/wifi/radio.c/h`** | Port in user space di `TopazModem` (topaz 0123/0125/0126/0129/0130), a sua volta port di `topaz-woa` | **Nessuna licenza per la parte di MakrSas**: la MIT di flame copre solo il lavoro di flame | **Solo riferimento.** Due strade: (a) permesso esplicito (o una licenza nel repository) da MakrSas, più la conferma di Nick; (b) riscrittura da ath10k (`ce.c`, `htc.c`, `wmi-tlv.c/h`, `htt_rx.c`, `htt_tx.c`, `hw.c`; ISC), usando topaz e flame solo per sapere *cosa* serve. Lo stesso vale per il pezzo SMMU di questo file: lo riscriviamo dal codice USB di flame (0090, APSL) e da `arm-smmu` |
| `xnu/patches/0094`: `bsd/dev/sweet_pmem.c` | Nick Wylynko | APSL 2.0 (file del kernel di flame) | **Riuso** con crediti. Il ciclo SMC con ripresa l'abbiamo già preso per la GPU (`xnu/andromeda_gpu.c`). Da noi va spento di default e limitato (§5.2) |
| `xnu/patches/0091`/`0094`: `sweet_files.c` (server di file via USB) | Nick Wylynko | APSL 2.0 | **Riuso** facoltativo, come canale per i file (§5.2) |
| `xnu/patches/0102`: `PMEM_RPMH`, `sweet_rpmh_write` | `sweet_rpmh.c` di ios-sweet (dipa-studio, patch 0062) esteso da flame | MIT (ios-sweet) / APSL 2.0 | **Riuso** con crediti a entrambi |
| `0123`: `redwood_apple80211.c`, `redwood_sta.c`, `redwood_net.c`, `sys/redwood_sta.h`, `sys/redwood_net.h`, `RedwoodWLAN.cpp`, `MiniIONetworkStack.cpp` | HakiOS (OOSEX GANG; nato in "darwin-redwood" di vcvkk), passato per topaz 0900, invariato in flame | **APSL 2.0** (HakiOS `LICENSE`, topaz `CREDITS.md`) | **Riuso** con crediti a OOSEX GANG, MakrSas e Nick Wylynko. Meglio partire dalla versione pubblicata da HakiOS, dove la licenza è scritta, e applicare le correzioni delle loro review (`docs/driver-review/`). Serve una copia dell'APSL 2.0 in `licenses/` (oggi non c'è) |
| `0123`: `sys/redwood_input.h` | topaz (5 righe) | APSL secondo la 0900 | Banale: si riscrive |
| `0123`: `flame_sta.c` | Nick Wylynko, da `topaz_sta.c` (colla di MakrSas nella 0900) | APSL 2.0 secondo flame. Topaz dichiara APSL la 0900, ma elenca come derivati HakiOS solo i `redwood_*` ~ | **Riuso** con crediti a Nick e MakrSas. Per avere la certezza, chiederlo a MakrSas insieme a `radio.c` (sono 242 righe) |
| `0123`: `flame_wlan.c`, `sys/flame_wlan.h` | Nick Wylynko | APSL 2.0 | **Riuso** |
| `0123`: `IOService.cpp`, `IOStartIOKit.cpp` (quiescenza) | topaz 0128 | Modifiche a file di XNU, quindi "Modifications" APSL 2.0 (§1.10 e §2.2) | Poche righe: si riscrivono, con crediti a MakrSas |
| `0123`: `SweetKeyStore.cpp` (wrap/unwrap) | HakiOS, via topaz 0900 | APSL 2.0 | **Riuso** con crediti |
| `tools/mkadt.py`: nodo `wlan`, `mac-address-*` | Nick Wylynko | MIT | L'idea, nel nostro `scripts/mkadt.py` |
| Job launchd `org.iosonpixel.wland` | Nick Wylynko (modificato a mano sul telefono, non nel repository) | – | L'idea, nel nostro `sweet-root-tweaks.py` |

**Nota per `CREDITS.md`** (da aggiungere nello stesso cambiamento che porta il primo file; non lo fa questo
documento). Nella tabella "Ricerca: Wi-Fi, Bluetooth e rete" serve una riga per **Nick Wylynko, `ios-flame`**:
"Wi-Fi WCN3990 funzionante su iOS 16.7.16 con il demone `wland` (modem via TrustZone, rmtfs/pd-mapper/tftp,
GLINK/QRTR, WLFW, radio) e le patch 0094/0102/0123 (`/dev/pmem`, RPMh, `en0` + Apple80211 per `wifid`); piano in
`docs/research/wifi-flame.md`". Quando si copia codice, la riga va spostata nella sezione del codice usato, con i
nomi dei file.

---

## 3. Cosa cambia su `andromeda`

### 3.1 Valori fissi di flame contro il nostro FDT

I valori di `andromeda` sono ✔ dal nostro `live-fdt.dts`, citati per nodo. Intestazione dell'albero: `model = "SDX50M
ANDROMEDA"`, `qcom,msm-id = <0x153 0x20000>`, `qcom,board-id = <0x2d 0x01>`.

| Cosa | ios-flame (fisso nel codice) | `andromeda` | Azione |
|---|---|---|---|
| SMEM | `0x86000000`, 2 MiB (`smem.h`); mutex TCSR `0x1F40000` + `0x1000·3` | `smem` `<0x86000000 0x200000>`; `hwlocks = <tcsr_mutex 3>` su `0x1F40000`, passo `0x1000` | Uguale |
| SMP2P modem | in 435 / out 428, APCS `0x17C0000C` bit 14 | `qcom,smem = <0x1b3 0x1ac>` (435/428), `qcom,ipc` bit `0x0e`, SPI `0x1c3` (451); voci `master-kernel`, `slave-kernel`, `ipa`, `wlan` | Uguale. Il significato dei bit di `slave-kernel` è quello di `qcom,mss`: 0 err-fatal, 1 err-ready, 2 proxy-unvote, 3 stop-ack, 7 shutdown-ack |
| SMP2P "wlan" (in) | Non usato | `qcom,smp2p_map_wlan_1_in`: bit 0 force-fatal-error, bit 1 early-crash-ind | Da aggiungere al monitor dello stato (solo lettura) |
| GLINK modem | Elementi 478/479/480, APCS bit 12 | `glink_modem`: `mboxes = <apcs 0x0c>`, SPI `0x1c1` (449); `IPCRTR` con intent `0x800×5, 0x2000×3, 0x4400×2` | Uguale |
| AOP QMP | `0x0C300000`, APCS bit 0 | `qmp-aop@c300000` (`irq-mask 0x01`); clock `qcom,aopclk` "qdss_clk" | Uguale, compreso il messaggio QDSS |
| cmd-db | Puntatore letto a `0x0C3F000C` | Stesso SoC; Aloha mappa "AOP CMD DB" a `0x85F20000` | Uguale ~; in W0 si controlla che ci siano `ldoa1`, `ldoa7`, `ldoc2`, `ldoc11`, `rfclka2`, `cx.lvl`, `mss.lvl` |
| Modem PAS | `PAS_MODEM 4`; **`MPSS_PA 0x8DC00000`, `MPSS_SIZE 0xB400000`** | `qcom,mss@4080000`: `qcom,pas-id 4`, firmware `modem`, `modem_region` **`<0x8e000000 0x9600000>`**, `smem-id 0x1a5` (421), `ssctl-instance-id 0x12`, `qcom,signal-aop`, watchdog SPI `0x10a` | **Si cambia la regione.** Si controlla offline che tutti i `PT_LOAD` di `modem.mdt` cadano in `0x8E000000`–`0x97600000`. Se l'immagine è rilocabile (bit 27 dei `p_flags`) e l'indirizzo più basso è diverso, va applicata la regola di rilocazione di `mdt_loader` (vayu la implementa in `vayu_pas.c`), che flame non ha |
| Voti proxy del modem | `cx.lvl` e `mss.lvl` al **livello più alto** di cmd-db | `vdd_cx` → `cx.lvl`, `vdd_mss` → `mss.lvl`, `qcom,vdd_*-uV-uA = <0x181 …>` (livello 385, cioè il primo corner ≥ TURBO) | **Diverso da flame**: votiamo il primo livello di cmd-db ≥ 385, come Android. Il più alto può superare il voto di Android (regola 4 di CONTRIBUTING) ? |
| MSA WLAN | `0x8BC00000`, `0x100000` | `pil_wlan_fw_region` `<0x8bc00000 0x180000>`, `qcom,wlan-msa-memory = 0x100000`, `qcom,wlan-msa-fixed-region` | Uguale |
| Registri WLAN | `0x18800000`; CE `+0x240000`; shadow `+0x32000`; RRI `+0x24C004` | `icnss` `membase <0x18800000 0x800000>` | Uguale |
| SMMU WLAN | SID `0x640` mask `0x1` su `0x15000000` | `iommus = <apps_smmu 0x640 0x01>`; `apps-smmu@0x15000000`: `qcom,qsmmu-v500`, `qcom,skip-init` | Valori uguali. **È diverso lo stato lasciato dal firmware**: lì ABL, qui Aloha + DarwinBoot (§5.5) |
| IRQ dei CE | Non usati (polling) | `icnss` `interrupts`: SPI `0x19e`–`0x1a9` (414–425) | Polling come flame; interrupt in W7 |
| Alimentazioni WLAN | `ldoa1` 752, `ldoa7` 1800, `ldoc2` 1304, `ldoc11` 3104 mV, HPM | `vdd-cx-mx` → `ldoa1` `0xb7980` (752 mV); `vdd-1.8-xo` → `ldoa7` 1,8 V; `vdd-1.3-rfa` → `ldoc2` 1,304 V; `vdd-3.3-ch0` → `ldoc11`, `qcom,vdd-3.3-ch0-config = <0x2f5d00 0x328980>` (3,104–3,312 V) | **Stessi valori**, dentro i limiti di Android (regola 4 di CONTRIBUTING). `ldoa7`, `ldoc2` e `ldoc11` sono condivisi con il Bluetooth: serve un solo proprietario dei voti |
| Clock RF | `rfclka2` VRM acceso | Nessun consumatore nel DT Android; mainline vota `RPMH_RF_CLK2` | Come flame ~ |
| BDF | `bdwlan-flame.bin` (nome di Google), `file_id 0`, segmenti da 6.144 byte | Xiaomi: `bdwlan.*` nella partizione `modem` ~ | Nome scelto dal **`board_id` restituito da `CAP`** (regola di topaz 0122, la stessa convenzione cnss): `0xFF` → `bdwlan.bin`; `< 0x100` → `bdwlan.b%02x`; altrimenti `bdwlan.%03x`; se manca, `bdwlan.bin`. **Il `qcom,board-id <0x2d 0x01>` del FDT non c'entra**: identifica la scheda per ABL. Il `board_id` WLAN viene dall'OTP del chip e si vede solo in W3 |
| MAC WLAN | `02:50:49:58:45:4c`, fisso | Android punta a `/mnt/vendor/persist/wlan_mac.bin` (link nel vendor ✔), ma il file **non c'è** nel nostro backup di `persist` ✔ | Un indirizzo nostro, amministrato localmente e diverso da quello di flame, uguale a `mac-address-wifi0` dell'ADT. In alternativa: niente `MAC_ADDR`, e si usa quello che il firmware riporta in `WMI READY` ("fw MAC") ~ |
| Prefissi tftp | Solo `/readonly/firmware/image/` (nome in minuscolo) e `/readwrite/` | `vendor/rfs/msm/mpss` ✔: `readonly/firmware` → `/vendor/firmware_mnt` (= partizione `modem`); `readonly/vendor/firmware` → `/vendor/firmware`; `readwrite` → `/mnt/vendor/persist/rfs/msm/mpss`; `shared` → `persist/rfs/shared`; `hlos` → `persist/hlos_rfs/shared` | Si aggiunge `/readonly/vendor/firmware/` (come vayu) e si registra ogni RRQ. `/readwrite` parte da una copia di `persist/rfs/msm/mpss` (un solo file, `server_check.txt`) |
| pd-mapper | Tabella fissa dai `.jsn` di flame: `root_pd` (servreg, gps) e `wlan_pd` (`kernel/elf_loader`, `tms/servreg`, `wlan/fw`), istanza 180 | Dai nostri `modemr.jsn`/`modemuw.jsn` (da estrarre). Vayu, stesso SoC, ha trovato gli stessi domini ✔ | Si genera la tabella dai nostri `.jsn` sul PC, offline |
| rmtfs | 2 MiB da `PMEM_ALLOC` (RAM gestita), `hyp_assign` HLOS → HLOS+MSS | `qcom,rmtfs_sharedmem`: `reg <0x0 0x300000>` (3 MiB, allocati dall'host), `client-id 1`, `qcom,guard-memory`. Aloha riserva **`MPSS_EFS` `0xC0000000`, `0x300000`** ✔ | **Proposta: il buffer in `MPSS_EFS`.** È fuori da XNU in ogni configurazione, perché è "Reserv" nella mappa PEI e non può finire nella finestra. La dimensione è quella del DT; 4 KiB di guardia per lato. Partizioni: `modemst1`, `modemst2`, `fsg` (8 MiB ciascuna) e `fsc` (256 KiB), **copie** dal nostro backup |
| Pool CE/HTT/WMI | `0xE0000000`, 16 MiB, sotto i 4 GiB, fuori da XNU | La finestra XNU (1,5 GiB) è dentro uno dei due tratti liberi da ≥ 1,5 GiB (§3.2) | Pool scelto e **controllato dal kernel** (§5.2); sotto i 4 GiB solo se il firmware chiede memoria host WMI senza il servizio 179 |
| Firmware WLAN | `WLAN.HL.3.2.0.c9-00554`, chip `0x30224` ✔ | Android: `vendor.wlan.firmware.version = FW:3.0.0.9.227.1 HW:HW_VERSION=40060000` ✔ (`getprop.txt`) | Versione diversa: in W4 si confrontano la mappa dei servizi WMI e la versione ABI ? |
| Modem 5G | Nessuno | `SDX50M` esterno: `ro.baseband = mdm`, `qcom,poff-depends-on = "esoc0"`. Nel FDT attivo non c'è un nodo esoc ✔ | Si presume che l'MSS interno parta senza l'SDX50M ~ (§5.5) |

### 3.2 Le regioni di memoria e la finestra di XNU

XNU gestisce 1,5 GiB (`owned_memory_size`). DarwinBoot li prende in **una sola** voce libera della mappa UEFI. Sulla
mappa attuale di Aloha le voci abbastanza grandi sono due:

- "DXE Heap" `0xC0300000`–`0x140000000`;
- la RAM che `RamPartitionDxe` aggiunge a `0x180000000`–`0x200000000`.

Due nostri log (`phone/logs/ios-20261106-171527.log`, `touch-20261106-173316.log`) mostrano il buffer DMA USB del
kernel a `0x183b58000` e `0x183b7c000`: è memoria gestita, quindi oggi la finestra sembra nel tratto alto ~. Il
lavoro in corso sulla fusione delle voci di RAM (ROADMAP §5) **sposterà questo confine**: `0x140000000`–`0x200000000` diventerebbe un'unica voce da 3 GiB. Per questo
nessun indirizzo "libero" può essere fisso: il kernel deve controllarlo a ogni avvio con `pmap_valid_address`, come fa
già `xnu/andromeda_gpu.c` per il pool GPU.

| Regione | Indirizzo | Usata da | Dentro la finestra XNU? | Da `wland` |
|---|---|---|---|---|
| SMEM | `0x86000000`, 2 MiB | Tutti i processori | Mai (UEFI "SMEM", riservata) ✔ | Device, 32 bit |
| AOP msg RAM | `0x0C300000` | AOP (QMP) | MMIO | Device |
| cmd-db | Dati a `0x85F20000` | AOP | Mai (UEFI "AOP CMD DB") ✔ | Sola lettura |
| **Hypervisor** | `0x85700000`, 6 MiB | QHEE | Mai | **Mai leggerla**: blocca il firmware (ANDROMEDA.md, `ShengBootMemoryDxe`) ✔ |
| MSA WLAN | `0x8BC00000`, 1 MiB | MSS + WLAN dopo `hyp_assign` | Mai (UEFI "PIL_REGION" `0x8B700000`–`0x9AB00000`) ✔ | Mai dopo l'assegnazione |
| Modem | `0x8E000000`, 150 MiB | MSS dopo `AUTH_AND_RESET` | Mai (PIL_REGION) ✔ | Scrittura dei segmenti prima dell'auth |
| rmtfs | `0xC0000000`, 3 MiB (proposta) | HLOS + MSS, condivisa | Mai ("MPSS_EFS", Reserv) ✔ | Device |
| Pool GPU | `0x140000000` + 16 MiB (`andrgpupool`) | GPU | Oggi no; **dopo la fusione della RAM sì** ? | – |
| Pool WLAN | Da scegliere in W0 | DMA WLAN (SMMU identità) + `wland` | Deve essere no, controllato | Device |
| PStore | `0x17FE00000`, 2 MiB | UEFI | Mai | Da evitare |

**Regola**: nessuna pagina gestita da XNU passa mai al modem o al WLAN in modo esclusivo. Se l'`hyp_assign` di una
pagina gestita togliesse l'accesso a HLOS, XNU andrebbe in fault la prima volta che la tocca. Flame assegna a HLOS+MSS
il buffer rmtfs in RAM gestita, e funziona solo perché HLOS mantiene l'accesso: da noi lo evitiamo.

---

## 4. Firmware

### 4.1 Cosa serve e dove sta

| File | Ruolo | Dove, su `andromeda` |
|---|---|---|
| `modem.mdt` + `modem.b00`…`modem.bNN` | Immagine dell'MSS (PAS 4), forma divisa | Partizione `modem` (`sde52`, FAT "NON-HLOS"), `image/` ~. Vayu prende gli stessi file da `/vendor/firmware_mnt/image/`; Aloha (`uefipil.cfg`) carica `\image\adsp` e gli altri dalla stessa partizione FAT ✔ |
| `wlanmdsp.mbn` | Dominio utente WLAN, chiesto dal modem via tftp | `modem`, `image/` ~. **Non è nel vendor** ✔ |
| `bdwlan.bin` / `bdwlan.bNN` | Board data (BDF) per il `board_id` di `CAP` | `modem`, `image/` ~. **Non è nel vendor** ✔ |
| `modemr.jsn`, `modemuw.jsn` (e simili) | Domini e servizi per il pd-mapper | `modem`, `image/` ? |
| `modemst1.img`, `modemst2.img`, `fsg.img`, `fsc.img` | EFS del modem (IMEI, calibrazioni), serviti da rmtfs | **Già nel backup** `phone/e719dcab/backup/` (8 + 8 + 8 MiB + 256 KiB), con `SHA256SUMS` ✔. Si usano copie, mai le partizioni |
| `readwrite/server_check.txt` | Zona `/readwrite` del modem | `persist`, `rfs/msm/mpss/` (5 byte) ✔, dal backup `persist.img` |
| `WCNSS_qcom_cfg.ini`, `wlan_mac.bin` | Configurazione del driver Android (qcacld) | Solo link nel vendor ✔; non servono |

Riscontri già fatti in locale, senza toccare il telefono:

- **`vendor`**: copia estratta per la GPU ([gpu.md §6.3b](gpu.md)), elencata con `debugfs`. `/firmware` contiene GPU,
  IPA, camera e OIS, più `wlan/qca_cld/` con due soli link. **Niente di WLAN o modem** ✔. `/rfs/msm/mpss/` dà la mappa
  dei percorsi tftp (§3.1) ✔. In `/bin` ci sono gli originali Qualcomm (`rmt_storage`, `tftp_server`, `pd-mapper`,
  `cnss-daemon`, `qrtr-ns`, `mdm_helper`, `ks`): solo come conferma, non si eseguono.
- **`persist`** (backup): esistono `rfs/`, `hlos_rfs/`, `bluetooth/`; manca `wlan_mac.bin` ✔. Sono stati letti solo i
  nomi dei file, non il loro contenuto.

### 4.2 Estrazione in sola lettura (piano: comandi da far lanciare all'utente)

Come per la GPU: TWRP avviato **solo in RAM**, nessuna scrittura sul telefono, destinazione `inputs/andromeda/wlanfw/`
(esclusa da git: `inputs/`). **Il recovery non deve toccare `userdata`**, dove ora c'è l'HFS+ di iOS: niente "Format
data" né "Wipe". Shell `bash` sul PC (quella predefinita è fish).

```bash
# 1. telefono in fastboot, TWRP 3.7.0 ufficiale in RAM (fastboot boot, mai flash)
fastboot getvar product                    # deve rispondere: andromeda
TWRP=/percorso/twrp-3.7.0-andromeda.img    # lo stesso TWRP ufficiale usato per gpu.md §6.3b
fastboot boot "$TWRP"
# 2. copia dell'intera partizione modem (FAT "NON-HLOS"), sola lettura
W=inputs/andromeda/wlanfw; mkdir -p "$W"
adb exec-out 'dd if=/dev/block/bootdevice/by-name/modem bs=4M 2>/dev/null' > "$W/modem.img"
adb shell 'sha256sum /dev/block/bootdevice/by-name/modem'      # deve coincidere con:
sha256sum "$W/modem.img"
# 3. sul PC, senza montare: elenco ed estrazione con mtools (caratteri jolly tra apici, li espande mtools)
mdir -i "$W/modem.img" ::/image | grep -Ei 'modem|wlan|bdwlan|jsn'
for pat in 'modem.*' 'wlanmdsp.mbn' 'bdwlan.*' '*.jsn'; do
  mcopy -n -i "$W/modem.img" "::/image/$pat" "$W/" || echo "nessun file per $pat"
done
(cd "$W" && sha256sum -- * > SHA256SUMS)
# 4. copia di lavoro della zona /readwrite dal backup di persist (sul PC, sola lettura)
mkdir -p "$W/rw" && debugfs -R "rdump /rfs/msm/mpss $W/rw" phone/e719dcab/backup/persist.img
```

- **Verifica incrociata (facoltativa)**: `NON-HLOS.bin` della ROM fastboot Xiaomi V12.0.7.0.PEMEUXM, la build da cui
  vengono i nostri blob (`ro.vendor_dlkm.build.fingerprint`). Si confrontano gli SHA256 dei singoli file.
- **Cosa entra nei documenti**: solo gli SHA256 dei file, mai i file, in questa tabella, da compilare dopo
  l'estrazione:

| File | Byte | SHA256 |
|---|---|---|
| `modem.mdt` | ? | ? |
| `wlanmdsp.mbn` | ? | ? |
| `bdwlan.*` (ognuno) | ? | ? |
| `modemr.jsn` / `modemuw.jsn` | ? | ? |

- **Cosa non entra nei documenti**: gli SHA256 delle immagini EFS. Identificano il telefono e restano nel
  `SHA256SUMS` locale.

### 4.3 Controlli offline prima del primo avvio

Uno script nostro in `scripts/`, che legge i file come dati (`python3 -I`) e non esegue niente:

1. **`modem.mdt`**: elenco dei program header (tipo, `p_paddr`, `p_memsz`, flag rilocabile, indice del segmento
   hash). Fatto quando tutti i `PT_LOAD` stanno in `0x8E000000`–`0x97600000`, oppure quando c'è un offset di
   rilocazione calcolato. Controlla anche che esistano tutti i `modem.bNN` necessari.
2. **`.jsn`**: tabella `servizio → dominio, istanza` per `pdmapper.c`, da confrontare con quella di flame.
3. **`bdwlan.*`**: elenco dei `board_id` disponibili. Quale serve si sa solo in W3, dalla risposta `CAP`.
4. **Coerenza della build**: `wlanmdsp.mbn` e `bdwlan.*` devono venire dalla stessa partizione `modem` di `modem.mdt`.
   Vayu prendeva "il primo che trova".

---

## 5. Piano a fasi

### 5.1 Scelte

- **Driver in user space, come flame**: per W0–W5 si cambia `wland`, non il kernel, che da noi si compila solo sui
  runner macOS. Il kernel serve una volta per `/dev/pmem` e una per la superficie iOS. La radio nel kernel si
  considera solo in W7.
- **Un passo per avvio, comandato da un parametro**:
  - `andrwlan=N` dice a `wland` fin dove arrivare (0 o assente: non fa niente);
  - `andrpmem=1` accende `/dev/pmem` (da flame è acceso di default; da noi no, regola 4 di CONTRIBUTING);
  - `andrwifid=1` accende `/dev/wlan`, `en0` e Apple80211.
- **Un solo avvio del modem per accensione, e mai `PAS_SHUTDOWN`.** Flame non ha mai provato il riavvio con la radio
  attiva; su vayu `PAS_SHUTDOWN` bloccava il telefono; il nostro codice GPU lo evita già. Ogni nuova prova è un
  riavvio del telefono. Con un modem che si avvia una volta sola, i riavvii costano poco più dell'SSH.
- **Log**: launchd manda lo stdout di `wland` su `/dev/null`. Con `sweetnull=backboardd,SpringBoard,wland` (patch 0048
  di ios-sweet, già nel nostro kernel) quelle righe arrivano al log USB in diretta come `|wland| …`. In più c'è
  `/var/local/wland.log`.
- **Privacy nei log**:
  - SSID e BSSID mascherati (flame maschera solo i BSSID);
  - mai la passphrase;
  - mai il contenuto delle immagini EFS.

### 5.2 Prerequisiti (nessuna radio)

| Passo | Cosa | Fatto quando | Ripiego |
|---|---|---|---|
| P1 | Firmware estratti e controlli offline (§4.2–4.3) | Elenco di `image/` e SHA256 in §4.2; rapporto dei program header di `modem.mdt` | ROM fastboot Xiaomi |
| P2 | `wland` per iOS: codice in **`userland/wland/`** (non `phone/`, che da noi è escluso da git) dai file MIT di flame. Build con un workflow su macOS: `xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0`, firma ad hoc. Nessun file dell'IPSW | Artifact `wland` firmato; `wland` senza argomenti stampa l'uso | – |
| P3 | Kernel K1: `xnu/andromeda_pmem.c`, port di `sweet_pmem.c` (0094) + `PMEM_RPMH` (0102) dietro `andrpmem=1`. **Più stretto di flame**: (a) elenco chiuso di intervalli fisici: SMEM, PIL_REGION, MPSS_EFS, pool WLAN, `0x18800000`+8 MiB, `0x15000000`+1 MiB, `0x17C0000C`, `0x0C300000`+1 MiB, `0x85F20000`+128 KiB, `0x1F40000`+`0x20000`; niente Hypervisor, niente RAM gestita fuori da `PMEM_ALLOC`; (b) SMC ammesse: `0xC2000601` (is-call-available), `0x42000201/202/205/207` (PAS init, mem_setup, auth, is_supported), `0x42000C16` (`MEM_PROT_ASSIGN`), **non** `0x42000206` (shutdown); (c) un solo lock per le SMC, condiviso con `andr_smc` della GPU; (d) `PMEM_POOL`: il kernel sceglie il pool WLAN e rifiuta ogni pagina che `pmap_valid_address` dice gestita, come `andrgpupool`. Meglio ancora: un unico allocatore delle carve-out per GPU e WLAN | `ANDROMEDA PMEM: on, N ranges, pool 0x… size 0x…` nel log; un `pread` fuori elenco restituisce `EPERM` | Senza `andrpmem` nessun nodo: il kernel di oggi resta com'è |
| P4 | Canale per i file: il server di file di flame (`sweet_files.c`, 0091/0094, APSL) dietro `andrfiles=1`, oppure i file messi nell'immagine (P5) | Un `put` e un `get` di prova via `ue0` | Ricostruire l'immagine |
| P5 | Root iOS (§5.4) e ADT | Il job `org.andromeda.wland` compare nel log `exec wland` | `ANDROMEDA_WLAN=0` ricostruisce l'immagine senza |

### 5.3 Passi sul telefono

Ogni riga è un avvio, e ogni passo ripete quelli prima (con `andrwlan=4` girano anche 1–3). Le righe di log sono quelle
del codice di flame, che porteremo con le stesse stringhe; quelle di W0.a e W1.a (`scm: …`, `ANDROMEDA PMEM: …`) sono
nuove, nostre.

| Passo | `andrwlan` | Cosa | Riga di log che lo prova | Ripiego |
|---|---|---|---|---|
| **W0.a** Ricognizione | 1 | Sola lettura: `smem` (partizioni, elementi 421/428/435/478–480), `cmddb` (le sette risorse), SMMU (`sCR0`/`ID0`/`ID1`, SMR che corrispondono a `0x640`, SMR e bank liberi), magia QMP (`0x4d41494c`, v1), finestra XNU e controllo del pool (dal kernel) | `smem: …` con la partizione apps↔modem; `ldoa1 slave 4 addr …` …; `smmu: sCR0 … N SMRs, M context banks`, nessuno SMR già su `0x640`; `ANDROMEDA PMEM: XNU 0x…+0x…, pool ok` | Una lettura che blocca: pressione lunga, poi si toglie l'intervallo dall'elenco e lo si annota |
| **W1.a** TrustZone, solo domande | 2 | `is-call-available` per PAS init, mem, auth e `MEM_PROT_ASSIGN`; `PAS_IS_SUPPORTED(4)`. Fatto prima: lo ZAP della GPU (`andrgpu=3`) è la prima prova di PAS sotto Aloha | `scm: avail 42000201 -> 1` (×4), `scm: pas 4 supported -> 1` | Una risposta "0" netta: ci si ferma (nessun avvio del modem con questo firmware o UEFI) |
| **W1.b** Alimentazioni, SMP2P, SMMU | 3 | `wlan_power`, `smp2p_create`, banco SMMU identità per `0x640` | `wlan power: ldoa1 752 mV on -> 0` (×4), `wlan power: rfclka2 on -> 0`, `qmp: "{class: clock, res: qdss, val: 1}" -> acked`, `smp2p: outbound item 428 at … ready, modem kicked`, `smmu: WLAN 0x640/0x1 -> CBn via SMRm: … -> ok` | RPMh in timeout: lo slot si mette in quarantena (`sweet_rpmh.c`), si riavvia. SMMU "NOT as written": l'hypervisor riscrive gli S2CR, e si passa a tabelle S1 identità (come `wifi_smmu=2` di HakiOS) |
| **W1.c** Avvio del modem | 4 | PAS con la nostra regione, poi 60 s di osservazione | `boot: init image (… bytes metadata at …) -> 0/0`, `boot: mem setup 8e000000+9600000 -> 0/0`, `boot: N bytes of segments loaded`, `boot: auth and reset -> 0/0`, `boot: modem ready after …00 ms`, `smp2p: modem state 2 ready`, e nessun `ERR-FATAL` per 60 s | `rc ≠ 0`: ci si ferma. Err-fatal: lettura dell'elemento 421 (`wland item 421`), niente shutdown, riavvio. "Segment outside the region": rilocazione (§4.3) |
| **W2** GLINK, QRTR, servizi | 5 | `link_up`, rmtfs (buffer in MPSS_EFS), pd-mapper, tftp, per tutta la sessione | `qrtr: < ctrl type … HELLO`, `rmtfs: serving at 1:10, buffer c0001000`, `rmtfs: open "/boot/modem_fs1" -> 1`, `pd-mapper: "…wlan…" -> 1 domain(s)`, `tftp: read "/readonly/firmware/image/wlanmdsp.mbn" (octet)` senza `-> not found`, `tftp: read done (…)`, `qrtr:   server service 0x45 …`, `wlfw: server at 0:…`; modem stabile oltre 5 minuti | Err-fatal dopo ~40 s: rmtfs non raggiunto. Client fermi dopo ~8 pacchetti: `RESUME_TX`. `not found`: prefisso o nome, e si aggiunge il percorso visto nel log. `PD FAILURE "…"` dal pd-mapper: si legge il motivo |
| **W3** WLFW fino a mission | 6 | Sequenza del §1.2, punto 6, con il nostro BDF | `wlfw: chip 0x… family 0x… board 0x… soc 0x… fw 0x… "…"` (si annota il `board_id`), `wlfw: < MSA READY`, `wlfw: board data sent (…/bdwlan.bXX, …)`, `wlfw: cal report -> 0`, `wlfw: < FW READY`, `wlfw: wlan config sent, mission mode on` | `PD FAILURE "…EX:wlan_process…"`: alimentazioni, QDSS o RF clock. Errore 48 sul BDF: buffer QMI. CRC sull'ultimo segmento: non è fatale. Nessun FW READY: si prova `bdwlan.bin`. Mai riprendere l'MSA |
| **W4** Radio, scansione | 7 | CE, HTC, WMI, HTT, scansione passiva (riscrittura di `radio.c`, §2) | `ce: 12 rings programmed, rx buffers posted`, `*** HTC READY on ce… ***`, `wmi: fw … abi …`, `wmi: INIT (… chunks, … bytes host memory) sent`, `*** WMI READY (fw MAC …) ***`, `*** HTT VERSION_CONF … ***`, `scan: channel list (37) ok, vdev ok, passive scan ok`, `*** scan 1 done (…): N networks … ***` | Il bus si blocca: accesso ai CE prima della mission (attesa di 100 ms). Nessun beacon: anello HTT RX. `INIT` rifiutato o `mem_req` a 32 bit: confronto della mappa dei servizi (FW 3.0 contro 3.2) e pool sotto i 4 GiB |
| **W5** Join con `utun` | 8 | `/var/local/wlan.conf` di prova (solo sul telefono); WPA2-PSK, DHCP, `ping` | `join: …`, `join: associated, waiting for the 4-way handshake`, `*** WPA2 keys installed (group key yes, …) ***`, `*** joined '…' ***`, `*** dhcp: ACK …, lease … s ***`; `nettest`/`ping` dal telefono | `message 3 MIC wrong`: passphrase. Nessun messaggio 1: AUTH/ASSOC. Nel nostro WPA vanno messe le correzioni della review HakiOS: chiavi candidate, filtro dei frame non protetti, controllo replay sui frame di gruppo |
| **W6** iOS | 8 + `andrwifid=1` | Kernel K2 (`flame_wlan.c`, `flame_sta.c`, `redwood_*` da HakiOS, `RedwoodWLAN`, `MiniIONetworkStack`, quiescenza, AKS wrap/unwrap) + ADT `wlan` + MAC; `sweetnull` con anche `wifid` | `bridge: /dev/wlan open: en0 and wifid`, `bridge: hello: the kernel makes en0`, `\|wifid\| … Falling back to IOCTL path`, `bridge: scan done, N networks for wifid`, DHCP di configd su `en0`. Impostazioni elenca le reti, entra in WPA2, salva la password e si ricollega dopo il riavvio (ramo PMK) | `andrwifid=0`: `wland` torna al suo `utun` (W5) |
| **W7** Rifinitura | – | Job all'avvio senza `andrwlan` limitante, riconnessione, CPU, interrupt CE (INTID 446–457), radio nel kernel, coesistenza BT | Ore di traffico senza errori | [wifi-bluetooth.md §8](wifi-bluetooth.md) |

**Ordine e dipendenze.**

- **Si possono fare subito**, con il codice MIT di flame: P1–P5 e W0–W3.
- **W1.a** conviene dopo `andrgpu=3`: se lo ZAP passa, PAS e la ripresa delle SMC sotto Aloha sono provati.
- **W4–W5 aspettano `radio.c`**: la riscrittura da ath10k, oppure il permesso di MakrSas. **La richiesta va fatta
  subito**: un'issue su `MakrSas/ios-topaz` che chieda una licenza (MIT o APSL) per `0113`–`0130` e per `topaz_sta.c`,
  più una conferma a Nick Wylynko per `radio.c`.
- **W6** non dipende dal modem per la parte IOKit. Con `andrwifid=1` e senza `wland`, `wifid` parte ma non vede reti.
  Si può provare insieme al lavoro N1 sulla rete USB di [wifi-bluetooth.md](wifi-bluetooth.md), che usa la stessa
  coppia `IONetworkInterface` + `MiniIONetworkStack`.

### 5.4 Cosa serve sul root iOS

**`scripts/sweet-root-tweaks.py`**: una funzione `wlan(root)` che lavora **solo con `ANDROMEDA_WLAN=1`**, così che di
default l'immagine non cambi.

- **Job launchd** `org.andromeda.wland` in `System/Library/xpc/launchd.plist`:
  - `ProgramArguments [/private/var/local/bin/wland, auto]`;
  - `RunAtLoad`, senza `KeepAlive`, `UserName root`;
  - `StandardOutPath` e `StandardErrorPath` su `/dev/null`, per il tee `sweetnull`.
- **Directory** `/private/var/local/{bin,fw/modem,fw/rmtfs,fw/rw}`: root:wheel, 0700.
- **`wifid` resta nel launchd.plist.** Parte comunque solo se c'è il nodo ADT `wlan`. Oggi `bluetoothd` e
  `fairplayd.H2` sono parcheggiati, e `wifid` non va aggiunto a quell'elenco.
- **configd**: se l'immagine ha un `preferences.plist` senza servizi di rete (il caso di flame), lo si sposta da parte,
  e configd crea il servizio Wi-Fi da solo.
- **Credenziali**: mai `wlan.conf` dallo script.

**`scripts/rebuild-root.sh`**: un passo facoltativo, `WLAN_STAGE=DIR` (con `RMTFS_DIR=phone/<seriale>/backup`), dopo le
modifiche del root.

- Controlla gli SHA256 dei file contro i loro `SHA256SUMS`, poi copia:
  - `wland` in `bin/`;
  - i file di §4.1 in `fw/modem/` (nomi in minuscolo, come si aspetta `tftp.c`);
  - le **copie** delle EFS in `fw/rmtfs/`, con permessi 0600;
  - `rw/` dal backup di `persist`.
- Stampa `staged N files`.
- **L'immagine così prodotta contiene dati del telefono (IMEI nelle EFS)**: resta locale, come già oggi, e non va mai
  caricata in un workflow.

**`scripts/mkadt.py`** (dispositivo `andromeda`):

- nodo `/arm-io/wlan`: `device_type "wlan"`, `compatible "andromeda,wcn3990"`;
- in `/chosen`: `mac-address-wifi0`, `mac-address-bluetooth0`, `mac-address-ethernet0`, amministrati localmente e
  diversi da quelli di flame. Non vanno ricavati dal seriale.

Le proprietà MAC conviene aggiungerle **subito**, indipendentemente dal Wi-Fi. Su flame la loro assenza costava 5 s per
bundle e lasciava LaunchServices a metà: potrebbe c'entrare con lo stallo di SpringBoard su LaunchServices che abbiamo
ereditato dal Mi 9 ?.

**`board.toml`**: per le prove, `andrpmem=1 andrwlan=N sweetnull=backboardd,SpringBoard,wland`, più `andrwifid=1` in
W6. Mai di default.

### 5.5 Rischi

1. **Crash del modem.**
   - Cause note:
     - rmtfs mancante (~40 s);
     - servizi che spariscono a metà, come in vayu;
     - LDO, RF clock o QDSS spenti all'inizio della calibrazione;
     - firmware non abbinati, cioè `wlanmdsp.mbn` o `bdwlan` di un'altra build NON-HLOS;
     - segmenti caricati all'indirizzo sbagliato.
   - Effetto: modem morto fino al riavvio, perché non usiamo `PAS_SHUTDOWN`.
   - Diagnosi: elemento SMEM 421, PFR del pd-mapper, bit di `slave-kernel` e della voce SMP2P `wlan`.
   - **SDX50M**: con `ro.baseband = mdm` l'MSS interno potrebbe aspettarsi il modem esterno. Per il dominio WLAN non
     dovrebbe servire ~, ma il FDT attivo non ha il nodo esoc, quindi non sappiamo come Android lo gestisce ?.
2. **TrustZone sotto Aloha invece che dopo ABL.**
   - Da flame, la catena è ABL di Google → shim → XNU. Da noi è ABL Xiaomi → `fastboot boot` di Aloha (che carica
     `ScmDxeLA`, `TzDxeLA` e `PILDxe`) → DarwinBoot → XNU.
   - Indizi a favore ~:
     - TrustZone e hypervisor sono quelli del telefono;
     - nell'elenco `[AUTO]` del PIL di Aloha (`uefipil.cfg`) c'è solo `ABL`: l'UEFI non avvia il modem;
     - Windows su SM8150, a cui Aloha è destinato, avvia i DSP via PAS dopo `ExitBootServices`;
     - vayu ha mostrato PAS e `MEM_PROT_ASSIGN` sotto un UEFI Mu.
   - Rischio residuo: ogni SMC può non tornare e bloccare una CPU; serve la pressione lunga. Per questo:
     - prima le sole domande (W1.a);
     - lo ZAP della GPU come banco di prova;
     - un lock unico per tutte le SMC;
     - nessuna SMC ripetuta su MSA e rmtfs.
3. **Memoria fuori dalla finestra XNU da 1,5 GiB (§3.2).**
   - MSA, modem, SMEM e MPSS_EFS sono riservati nella mappa PEI e non si muovono ✔.
   - Il pool WLAN e quello GPU stanno in RAM convenzionale, e la fusione della RAM in corso può farli finire dentro
     la finestra. Il controllo `pmap_valid_address` a ogni avvio è obbligatorio, e conviene un solo allocatore delle
     carve-out.
   - Se il firmware chiede memoria host WMI a 32 bit, il pool deve stare sotto i 4 GiB e fuori da XNU. Oggi sarebbe
     il tratto basso di "DXE Heap"; va confermato in W0 ?.
   - Mai viste WC su pagine gestite (la "big trap" di flame).
4. **SMMU dopo Aloha.**
   - L'UEFI usa i suoi stream (UFS, USB, display) e può aver lasciato SMR e context bank occupati. L'hypervisor può
     trasformare un S2CR "bypass" in fault (topaz) o riscrivere i registri.
   - Con la traduzione spenta, il DMA del WLAN raggiunge tutta la RAM: un puntatore sbagliato del firmware corrompe
     XNU. Più avanti servono tabelle S1 limitate al pool.
5. **RPMh condiviso.**
   - `sweet_rpmh.c` usa un solo slot TCS e lo mette in quarantena dopo un errore.
   - I voti devono convivere con quelli dell'UEFI, della GPU (BCM) e in futuro del BT (`ldoa7`/`ldoc2`/`ldoc11`). Non
     si tolgono voti che non sono nostri.
6. **Firmware diverso da quello di flame** (3.0.0.9 contro 3.2.0): versione ABI WMI, servizi, richieste di memoria
   host. Si vede solo in W4.
7. **Sicurezza.**
   - `/dev/pmem` e il server di file danno a root l'accesso fisico alla macchina: spenti di default, elenco chiuso.
   - Il supplicant fa solo WPA2-PSK.
   - Va detto nel README: è un port di ricerca, non va usato con dati personali.
8. **Licenze.** `radio.c` blocca W4–W5 finché manca una licenza o la riscrittura (§2). Riscrivere costa circa 2.000
   righe, ma ath10k (ISC) le documenta tutte.
9. **Strumenti.** Per compilare `wland` serve macOS con l'SDK iOS, cioè un workflow come quello del kernel.
10. **Privacy.**
    - Le immagini EFS (IMEI), i MAC, gli SSID e i log con nomi di rete non vanno mai in git né nei workflow.
    - `wlan.conf` resta solo sul telefono.

---

## 6. In breve, per chi riprende il lavoro

- **Cosa prendiamo da flame**: l'architettura e il codice di `wland` fino a WLFW (MIT), `/dev/pmem` e RPMh (APSL),
  `/dev/wlan` + `flame_sta` (APSL), le correzioni elencate in §1.4.
- **Cosa prendiamo da HakiOS** (via topaz e flame, APSL): la superficie Apple80211.
- **Cosa non copiamo**:
  - `radio.c` e le parti di topaz senza licenza;
  - i valori fissi del Pixel (regione del modem, BDF, MAC, pool, prefissi tftp, tabella pd-mapper);
  - `/dev/pmem` acceso di default e aperto a qualsiasi indirizzo e SMC;
  - il voto di `cx.lvl`/`mss.lvl` al livello più alto (da noi: il primo ≥ 385, come Android);
  - gli SSID in chiaro nei log.
- **Primo passo concreto**: P1, l'estrazione della partizione `modem`. Poi P3 (kernel K1) e W0.a.

---

## 7. Fonti

Grazie agli autori: questo piano è quasi tutto lavoro loro.

- **ios-flame**, Nick Wylynko ([NWylynko](https://github.com/NWylynko)):
  [`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame) a `745c4341af86089efb0df851fa8a27fb8face624`
  (22 ottobre 2026). MIT (© 2026 dipa-studio / ios-flame contributors), patch XNU APSL 2.0. Clone locale in sola lettura.
  - Documenti: `README.md`, `HANDOFF.md` (sezioni "WiFi IN SETTINGS", "WiFi at boot, rejoin…", "WiFi works: scan, WPA2
    join, DHCP, internet", "WiFi, part 1", "Reference repos fetched", "Black wallpaper fixed", "The big trap"),
    `NOTES.md` ("Flame patches"), `CREDITS.md`, `LICENSE`, `DEVICE.md`, `AGENTS.md`.
  - User space: `phone/wifi/` (`wland.c`, `smem.c/h`, `glink.c/h`, `link.c/h`, `qmi.c/h`, `rmtfs.c/h`, `pdmapper.c/h`,
    `tftp.c/h`, `scm.c/h`, `wlfw.c/h`, `radio.c/h`, `ipnet.c/h`, `bridge.c/h`, `flamewlan.h`, `pmem.c/h`, `ath.c/h`,
    `build.sh`).
  - Kernel: `xnu/patches/0091-flame-usb-files.patch`, `0094-flame-pmem-files-read.patch`, `0102-flame-rpmh-generic.patch`,
    `0123-flame-en0-apple80211-wifid-over-wland.patch`.
  - Strumenti: `tools/mkadt.py` (nodo `wlan`, `mac-address-*`), `tools/flame-files.py`, `tools/flame-cmd.py`,
    `tools/patch-launchd-plist.py`.
- **ios-topaz**, MakrSas: [`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz) a
  `e4772911e116ab4ab6c4ac1f38e6106fefe8b483`, **senza licenza**.
  - `CREDITS.md`; `xnu/patches/0113`, `0122` (regola dei nomi `bdwlan`), `0123`, `0128`, `0900` (oggetto "from
    darwin-redwood, PRIVATE").
  - `TopazModem.cpp` si dichiara port di [`MakrSas/topaz-woa`](https://github.com/MakrSas/topaz-woa), non letto.
- **HakiOS**, OOSEX GANG: [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) a
  `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`.
  - Licenze: `LICENSE` (APSL 2.0 per `xnu/files/` e `drivers/`, MIT per il resto), `LICENSE.APSL`, `NOTICE`, `README.md`.
  - File confrontati con flame: `drivers/wifi/hakios.apple80211.c`, `hakios.sta.c`, `hakios.wlan.cpp`,
    `drivers/net/hakios.net.c`, `xnu/files/iokit/Kernel/MiniIONetworkStack.cpp`, `xnu/files/bsd/sys/hakios.sta.h`,
    `hakios.net.h`.
  - Revisioni da applicare a ciò che riusiamo: `docs/driver-review/wifi-bringup.md`, `wifi-stack.md` (già riassunte in
    [wifi-bluetooth.md §2](wifi-bluetooth.md)).
  - Il lavoro di vcvkk ("darwin-redwood") è all'origine di questi file.
- **HakiOS SM8150**, nomorecoolnicknames:
  [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) a
  `3e7be137a100cef19f1d7e83346b6e688be697d1`, **senza licenza**: `pull-firmware.sh` (file del modem in
  `/vendor/firmware_mnt/image/`), `kernel/bsd/dev/vayu_pas.c` (regola di rilocazione di `mdt_loader`), `vayu_rmtfs.c`
  (buffer da 3 MB).
- **ios-sweet**, dipa-studio: [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) a `45263bee`, MIT:
  `sweet_rpmh.c` (patch 0062, base di flame 0102), tee `sweetnull` (patch 0048).
- **Project Aloha** (Kancy Joe / `sunflower2333`, XiaoYeZi / `qaz6750` e collaboratori):
  [`mu_aloha_platforms`](https://github.com/Project-Aloha/mu_aloha_platforms) a `0b798128`, dispositivo
  `xiaomi-andromeda`: `Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c` (MPSS_EFS, PIL_REGION, SMEM, Hypervisor,
  DXE Heap, RAM Partition), `DXE.inc`/`APRIORI.inc` (`ScmDxeLA`, `TzDxeLA`, `PILDxe`), `Binaries/RawFiles/uefipil.cfg`.
- **Linux** (citato dalle intestazioni di flame e da [wifi-bluetooth.md](wifi-bluetooth.md); non riletto qui): ath10k
  `snoc.c`, `qmi.c`, `ce.c`, `htc.c`, `wmi-tlv.c`, `hw.c` (ISC); `soc/qcom/smem.c`, `smp2p.c`, `qcom_aoss.c`,
  `mdt_loader.c`, `rpmsg/qcom_glink_native.c`, `qcom_glink_smem.c`, `net/qrtr/`, `remoteproc/qcom_q6v5_pas.c`,
  `firmware/qcom_scm.c` (GPL-2.0, solo documentazione). **Bjorn Andersson** (Linaro): `rmtfs`, `pd-mapper`,
  `tqftpserv`, protocolli seguiti da flame.
- **Dati di `andromeda`** (fuori da git):
  - `phone/e719dcab/live-fdt.dts`: nodi `/`, `qcom,mss@4080000`, `qcom,icnss@18800000`, `reserved-memory`
    (`pil_wlan_fw_region`, `modem_region`, `smem`), `qcom,smp2p-modem@1799000c`, `qcom,glink/modem`, `qcom,qmp-aop@c300000`,
    `qcom,aopclk`, `apps-smmu@0x15000000`, `qcom,rmtfs_sharedmem@0`, `hwlock@1f40000`, regolatori `ldoa1`, `ldoa7`,
    `ldoc2`, `ldoc11`, `cx.lvl`, `mss.lvl`;
  - `info/getprop.txt` (`ro.wlan.chip`, `vendor.wlan.firmware.version`, `ro.baseband`), `info/partitions.txt`;
  - `backup/` (solo nomi e dimensioni; `persist.img` elencato con `debugfs`);
  - copia della partizione logica `vendor` di [gpu.md §6.3b](gpu.md) (elenco di `/firmware`, `/rfs`, `/bin`, `/etc/init`);
  - `phone/logs/ios-20261106-171527.log` e `touch-20261106-173316.log` (indirizzo DMA USB).
- **Questo repository**: [wifi-bluetooth.md](wifi-bluetooth.md), [gpu.md](gpu.md), `CREDITS.md`, `CONTRIBUTING.md`,
  `ANDROMEDA.md`, `ROADMAP.md` §5, `xnu/andromeda_gpu.c` e `andromeda_gpu_core.c` (pool controllato, `andr_smc`,
  nessun `PAS_SHUTDOWN`), `scripts/sweet-root-tweaks.py`, `scripts/rebuild-root.sh`, `scripts/mkadt.py`.
