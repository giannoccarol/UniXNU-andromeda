# Wi-Fi e Bluetooth per `andromeda`: come hanno fatto gli altri, e un piano

Ricerca del 6 novembre 2026, solo lettura: nessun comando sul telefono, nessun codice dei repository scaricati eseguito.
Gli indirizzi del Mi Mix 3 5G vengono dal device tree attivo del telefono (`phone/<seriale>/live-fdt.dts`, che resta
fuori da git), gli altri fatti dai file citati in [Fonti](#11-fonti).

## Sommario

- **Il Wi-Fi del nostro chip funziona già sotto XNU con iOS 16.7.16, sulla nostra stessa base.**
  [`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz) (Redmi Note 12 4G, SM6225 + **WCN3988**) è un port di
  `dipa-studio/ios-sweet` con la stessa userland iPhone X **20H392** (16.7.16). Lì il WCN3988 (famiglia WCN3990,
  firmware sul DSP del modem, come il nostro) funziona: Impostazioni elenca le reti, entra in quelle WPA2, salva la
  password nel portachiavi, si ricollega dopo il riavvio, e Safari carica pagine HTTPS. Le patch sono 0113–0130 e 0900
  (`TopazModem.cpp` più la parte Apple80211 presa da HakiOS).
- **La parte SM8150 c'è già, sullo stesso SoC.** [`hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150)
  (POCO X3 Pro, `vayu`) avvia il modem via TrustZone, gli serve `rmtfs`, pd-mapper e tftp, e vede comparire WLFW. Si
  ferma per costruzione dopo `CAP`: non manda mai il file della scheda (BDF) né `CAL_REPORT`, quindi il firmware non
  arriva a `FW_READY` (sezione 3). Il loro Bluetooth usa **gli stessi identici blocchi del nostro**: SE13
  `0xC8C000`, GPIO 43–46, L7A/L2C/L11C.
- **La parte iOS è risolta in due modi.** HakiOS e topaz rispondono alle ioctl `SIOCGA80211`/`SIOCSA80211` nel kernel,
  dopo che IO80211.framework ha fallito ad aprire un user client ("Falling back to IOCTL path"). Il supplicant
  WPA2-PSK gira nel kernel. UniXNU (iPadOS 27) porta invece mac80211/cfg80211 di Linux e un `wpa_supplicant` in user
  space. Per iOS 16.7.16 la strada di HakiOS/topaz è già provata su 20H392.
- **Bluetooth: il blocco sono i comandi "Apple", e la soluzione esiste già per il nostro `bluetoothd`.**
  `vayu` risponde ai comandi vendor dal kernel, e `bluetoothd` ricomincia lo stesso in loop. Sia
  [`renoir_xnu_bluethooth`](https://github.com/daimile2/renoir_xnu_bluethooth) (base ios-sweet, **iOS 20H392**,
  QCA6490) sia UniXNU `QcaBluetooth` (iPadOS 27) modificano `bluetoothd` perché tratti il chip come "controller
  tipo 0" (dongle di terze parti, senza tabella di comandi vendor). Le capacità arrivano dai comandi HCI standard, con
  un BlueTool sostitutivo e il trasporto nativo `UART.SK`. Su renoir funzionano ricerca, abbinamento, riconnessione e
  A2DP AAC. Il loro patcher controlla l'hash SHA-256 del `bluetoothd` di 20H392, che è la nostra build.
- **Quick win: Internet dal PC via USB.** Il lato PC è NAT più eventualmente DHCP (sezione 9). Al telefono manca solo
  che configd veda l'interfaccia: un oggetto IOKit `IONetworkInterface` sotto un `IOEthernetController`, più
  `IONetworkStack` (stesso schema di HakiOS/topaz). ios-sweet ha già provato il resto (servizio manuale, eventi di
  link) e si è fermato esattamente lì.

---

## 1. L'hardware di `andromeda` (FDT attivo)

`ro.wlan.chip=39xx`, `persist.vendor.qcom.bluetooth.soc=cherokee`, `cnss-daemon` attivo in Android (con ogni
probabilità è lui che scarica BDF e calibrazione via QMI dallo user space: da verificare). I nodi `qcom,cnss-qca6390@a0000000` (PCIe) e `wil6210`
(60 GHz, `disabled`) sono del device tree comune SM8150 e qui non servono. GIC: INTID = SPI + 32.

### Wi-Fi (WCN3990 integrato, firmware nel dominio `wlan_pd` del modem)

| Cosa | Valore | Da dove |
|---|---|---|
| Registri WLAN (copy engine) | `0x18800000`, 8 MiB; CE0 a `+0x240000` (`0x18A40000`), registro dell'indirizzo RRI a `+0x24C004` | `qcom,icnss@18800000` `membase`; offset di `wcn3990_ce_regs` in ath10k `hw.c` |
| Finestre IOVA (Linux) | `smmu_iova_base` `0xA0000000` (256 MiB), `smmu_iova_ipa` `0xB0000000` (64 KiB) | `icnss` `reg` |
| Stream SMMU | apps SMMU `0x15000000` (`qcom,qsmmu-v500`, `qcom,skip-init`), **SID `0x640` mask `0x1`** | `icnss` `iommus = <&apps_smmu 0x640 0x1>` |
| Interrupt CE | 12 linee: SPI 414–425 (`0x19e`–`0x1a9`), INTID 446–457 | `icnss` `interrupts` |
| Memoria MSA | `pil_wlan_fw_region` **`0x8BC00000`**, `0x180000`, no-map; `qcom,wlan-msa-memory = 0x100000` (1 MiB), `qcom,wlan-msa-fixed-region` | `reserved-memory` |
| SMP2P "wlan" | ingressi `force-fatal-error` (bit 0), `early-crash-ind` (bit 1) da `smp2p-modem` | `qcom,smp2p_map_wlan_1_in` |
| `vdd-cx-mx` | PM8150 **L1A** 752 mV (RPMh `ldoa1`) | phandle `0xeb` |
| `vdd-1.8-xo` | PM8150 **L7A** 1,8 V (`ldoa7`) | `0xec` |
| `vdd-1.3-rfa` | PM8150L **L2C** 1,304 V (`ldoc2`) | `0xed` |
| `vdd-3.3-ch0` | PM8150L **L11C** 3,104–3,312 V (`ldoc11`) | `0xee` |
| Clock di riferimento | nessun consumatore nel DT Android; mainline vota `RPMH_RF_CLK2` (`cxo_ref_clk_pin`) | mainline `sm8150.dtsi` |

### Modem (dove gira il firmware WLAN) e infrastruttura comune

| Cosa | Valore |
|---|---|
| Modem PAS | `qcom,mss@4080000`: `pas-id 4`, firmware `modem`, `modem_region` **`0x8E000000` + `0x9600000`**, crash in SMEM item 421 (`0x1a5`), `ssctl-instance-id 0x12`, `qcom,signal-aop`, proxy `vdd_cx`/`vdd_mss` a livello `0x181` (TURBO), `qcom,poff-depends-on = "esoc0"` (SDX50M esterno del 5G) |
| SMEM | `0x86000000`, 2 MiB; hwlock TCSR mutex `0x1F40000`, lock 3 |
| SMP2P modem | item 435 (dal modem) / 428 (verso il modem), APCS IPC `0x17C0000C` bit 14, SPI 451 |
| GLINK modem | APCS bit 12, SPI 449; canale `IPCRTR`, intents `0x800×5, 0x2000×3, 0x4400×2` |
| AOP QMP | msgram `0x0C300000`, campanello APCS `0x17C0000C` |
| rmtfs | `qcom,rmtfs_sharedmem`: 3 MiB allocati dall'host (`reg <0 0x300000>`), client-id 1 |
| Partizioni | `modem` (NON-HLOS: `modem.mdt/.bNN`, `wlanmdsp.mbn`, `bdwlan.*`), `modemst1/2`, `fsg`, `fsc`, `persist`, `bluetooth` (montata come `bt_firmware`) |

### Bluetooth (WCN399x "cherokee" su UART) e NFC

| Cosa | Valore |
|---|---|
| Nodo | `vendor/bt_wcn3990`, `qca,wcn3990`: `vdd-core` **L7A** 1,8 V, `vdd-pa` **L2C** 1,304 V, `vdd-ldo` **L11C** 3,312 V (per `vayu` anche S4A 1,8 V come vddio). Nessun GPIO di enable: l'accensione è a impulsi sulla UART (`0xC0`, poi `0xFC`) |
| UART | `qupv3_se13_4uart` **`0xC8C000`** (`qcom,msm-geni-serial-hs`), QUPv3 wrapper 2 a `0xCC0000` (SE3 del wrapper); clock GCC id `0x73`/`0x7d`/`0x7e`; IRQ SPI 585 (`0x249`, INTID 617); risveglio GPIO 46, `qcom,wakeup-byte = 0xFD` |
| Pin | GPIO 43 CTS, 44 RTS, 45 TX, 46 RX, funzione `qup13` (stessi numeri e stesso blocco di `vayu`) |
| Audio BT | `btfmslim_codec: wcn3990` su `slim@17240000` (SCO/A2DP offload di Android; per iOS l'A2DP passa da HCI ACL) |
| NFC | `qcom,nq-nci` @`0x28` su SE9 I2C `0xA84000`; IRQ GPIO 47, VEN GPIO 36, FW GPIO 48, CLKREQ GPIO 113 (bassa priorità: `nfcd` di iOS vuole il chip Apple con Secure Element) |

---

## 2. HakiOS (POCO X5 Pro `redwood`, SM7325 + WCN6750)

Repository `oosexgang/hakios-xnu-kernel` (commit `63fc2b46`, 6 novembre 2026; kernel APSL 2.0) e device repo
`oosexgang/hakios-sm7325`. Lo stato dichiarato in `docs/devices/redwood.md`: **wifi works** ("radio, driver, a network
joined"), **bluetooth no** (solo una lettura dei tre pin, `hakios.bt.c`). La userland è iOS 16.5.

### Struttura

| Strato | File | Cosa fa |
|---|---|---|
| Avvio del DSP radio | `drivers/wifi/hakios.wifi.c` (4444 righe) | 16 passi con boot-arg `wifi_step=N`. Sul WCN6750 il firmware gira nel **WPSS**, un Hexagon separato e non nel modem. Il kernel copia `wpss.mdt/.b02–.b07` nella memoria riservata e chiede a TrustZone `INIT_IMAGE`/`MEM_SETUP`/`AUTH_AND_RESET` (PAS). Prima apre la mailbox QMP dell'AOP ("image loaded"). Poi SMEM, SMP2P (item suo e nostro), IPCC, GLINK (`IPCRTR`), QRTR e lookup del servizio WLFW. Opzionalmente scrive l'SMMU (`wifi_smmu=2`: un context bank con tabelle proprie per lo stream della radio). Ferma tutto con l'ordine di Linux (SMP2P "stop", poi `PAS_SHUTDOWN`): un DSP spento solo da TrustZone non ripartiva più |
| QMI WLFW | `hakios.wlfw.c` + `drivers/modem/hakios.qmi.c` | Prima uno "stand-in" a mano, poi il `qmi.c` vero di ath11k. Le tabelle dei messaggi vengono estratte dai sorgenti Linux a build time (`tools/mk-wlfw.sh`) e non sono nel repo. Misure sul telefono: regdb in 4 pezzi, BDF `bd_m20gl.elf` in 10, "init done" dopo 283 ms; con calibrazione a freddo "cal done" dopo 6,7 s; poi `WLAN_CFG` e modalità *mission* |
| Driver radio | `wifi/lx/` + `hakios.lx.c` | **ath11k di Linux 7.0.14 non modificato**, compilato sugli header LinuxKPI di FreeBSD 15.1 più uno strato proprio (`lx_base.c`: thread, work, memoria, pool DMA; `lx_bus.c`: bus AHB e anelli CE; `tools/mk-ath11k.sh`). CE, HTT e WMI sono quelli di ath11k |
| Al posto di mac80211 | `wifi/lx/src/lx_80211.c` | Scan, join e conversione 802.11 ↔ Ethernet scritti in proprio. **mac80211 non c'è** |
| Supplicant | `wifi/lx/src/lx_wpa.c` | **WPA2-PSK nel kernel** (PBKDF2, PRF, MIC, key unwrap, handshake a 4 vie e di gruppo). La crittografia del traffico la fa il firmware |
| Confine "stazione" | `hakios.sta.c`, `sys/hakios.sta.h` | Accensione, canali, scan, risultati, join/leave serializzati |
| Superficie iOS | `hakios.apple80211.c`, `hakios.wlan.cpp`, `drivers/net/hakios.net.c` | Vedi sotto |

### Come presentano il Wi-Fi a iOS (vale anche per noi)

1. **`en0`** creato con la KPI d'interfaccia BSD (come `if_fake`), framing Ethernet.
2. **Oggetti IOKit finti con i nomi giusti** (`hakios.wlan.cpp`): `IO80211Controller` (sottoclasse di
   `IOEthernetController` → `IONetworkController`) con `IOMACAddress` e `IOBuiltin`, figlio un `IONetworkInterface`
   (`HakiosWLAN`) con `IOInterfaceName`/`BSD Name` = `en0`, `IOInterfaceNamePrefix` `en`, `IOInterfaceType` 6,
   `IOInterfaceUnit`, `IOPrimaryInterface`, e **`IO80211InterfaceRole = "Infrastructure"`**: è la proprietà con cui
   wifid trova il dispositivo. `newUserClient` restituisce *unsupported*, così IO80211.framework ripiega sulle ioctl.
   `local-mac-address` va nel nodo `wlan` dell'ADT, perché MobileGestalt lo legge da lì; senza, lsd aspetta 5 s per
   ogni bundle e un rebuild di LaunchServices dura ore. La scoperta è del port `ios-sapphire`, patch 0157/0158.
3. **Ioctl Apple80211** su qualsiasi socket: `SIOCSA80211 0x802869c8`, `SIOCGA80211 0xc02869c9`, `struct apple80211req`
   {nome[16], tipo, valore, lunghezza, puntatore}. Tipi gestiti: 1 SSID, 2 AUTH_TYPE, 4 CHANNEL, 9 BSSID, 10 SCAN_REQ
   (5456 byte), 11 SCAN_RESULT (1164 byte, un BSS per chiamata, `EIO` alla fine), 12 CARD_CAPABILITIES (13 byte di bit:
   senza il bit WPA2 il framework non costruisce il join), 13 STATE, 15 OP_MODE, 16 RSSI, 19 POWER, 20 ASSOCIATE
   (592 byte: passphrase in chiaro con cipher 10 oppure PMK con cipher 6), 21 ASSOCIATE_RESULT (1 ok, 21 "invalid PMK",
   9 timeout, 2 altro), 22 DISASSOCIATE, 27/254 canali (4824 byte), 43/44 versione, 103 CURRENT_NETWORK, 207
   CHANNELS_INFO (10016 byte). I layout hanno `_Static_assert` letti da IO80211.framework 16.5.
4. **Eventi**: `kev_post_msg` con classe `KEV_IEEE80211_CLASS` (6), sottoclasse 1, codice = numero Apple80211
   (1 power, 2 SSID, 3 BSSID, 4 link con 32 byte di dati, 9 assoc done, 10 scan done). I primi 16 byte dei dati sono il
   nome dell'interfaccia.
5. **Portachiavi**: wifid salva la password con `WiFiSecuritySetPassword`, e securityd deve poter fare
   `kAKSWrapKey`/`kAKSUnwrapKey`. HakiOS (e topaz in `SweetKeyStore.cpp`) le risponde senza SEP.
6. **Credenziali di prova**: `fw/network` (SSID + passphrase) nell'immagine, mai come boot-arg (i boot-arg finiscono nei
   panic log).

Le review interne (`docs/driver-review/wifi-bringup.md`, `wifi-stack.md`) elencano difetti da non copiare: lunghezze
GLINK che vanno in overflow (loop infinito), estensioni SMEM non verificate, controlli QMP/QRTR che vanno in wrap. Lato
WPA: il messaggio 1 non autenticato sovrascrive le chiavi, nessun controllo "frame protetto" e nessun replay check sui
frame di gruppo. Chi riusa questo codice (topaz compreso) deve applicare le correzioni.

---

## 3. HakiOS SM8150 (`vayu`, POCO X3 Pro): il tentativo WCN3990 e il Bluetooth

`nomorecoolnicknames/hakios-sm8150`, commit `3e7be137` (29 ottobre 2026); nessuna licenza dichiarata nel repo. È lo
stesso SoC: i file `vayu_*` riportano accanto agli indirizzi le righe del loro DT. Stato: modem **funziona** ("the
modem boots, its remote file system is served, WLFW is announced"); Wi-Fi "WCN3990 WLAN DSP crashes at
`PC=0xb0193960` before FW_READY; WMI/HTT/CE not written"; Bluetooth "bluetoothd restarts in a loop on Apple vendor
commands" (etichetta di prova `TEST23-BT`).

### Cosa fa `vayu_wifi.c`

- `vayu_wifi=1`: pubblica il **pd-mapper** (SERVREG locator, QMI `0x40`, versione 1 istanza 1, domini dai `.jsn` della
  partizione modem: `msm/modem/wlan_pd` con `kernel/elf_loader`, `tms/servreg`, `wlan/fw`, istanza 180) e un server
  **tftp** su QRTR (servizio 4096). Il server è in sola lettura e prende i file da `fw/`, con i prefissi
  `/readonly/firmware/image/`, `/readonly/vendor/firmware/`, `/readonly/vendor/firmware_mnt/image/`; `/readwrite/` è
  tenuto in RAM.
- `vayu_wifi=2`: WLFW `ind_register` → `host_cap` → `msa_info` (MSA `0x8BC00000`, 1 MiB) → assegnazione SCM di ogni
  regione da HLOS a {MSS_MSA `0xF`, WLAN `0x18`, + WLAN_CE `0x19` se non sicura} (`vayu_pas.c: vayu_scm_assign_msa`,
  SIP `0x0C`/`0x16`) → `msa_ready` → `cap`. Poi **si ferma**. Il commento lo dice: "no BDF_DOWNLOAD, no CAL_*, no
  WLAN_CFG, no WLAN_MODE". Alla fine il thread rilascia l'handle QMI e `vayu_wifi_stop()` chiude anche pd-mapper e tftp.
- `vayu_rmtfs.c` serve `modemst1/2`, `fsg`, `fsc` dalla RAM (le scritture non arrivano mai alla UFS). Senza rmtfs il
  modem va in FATAL dopo circa 40 s.

### Perché non arriva a `FW_READY`, e perché va in crash

Confronto con ath10k mainline (`qmi.c`: `ath10k_qmi_event_server_arrive`, `ath10k_qmi_event_msa_ready`), con `icnss.c`
del kernel Xiaomi `andromeda-p-oss` e con topaz (`0122`):

1. **Certo: manca il passo che fa nascere `FW_READY`.** In mainline e in topaz l'ordine è: `ind_register` →
   `host_cap` (`daemon_support = 0`) → `msa_info` → 20 ms di pausa → assegnazione MSA → `msa_ready` → `cap` →
   **indicazione `MSA_READY` → `BDF_DOWNLOAD` (`bdwlan.bNN` dal `board_id` di `cap`; `bdwlan.bin` come ripiego) →
   `CAL_REPORT` (meta_data_len 0)** → indicazione `FW_READY` → `WLAN_CFG` (tabelle CE) → `WLAN_MODE` mission. In
   Android quel pezzo non sta in `icnss.c`: con ogni probabilità lo fa `cnss-daemon` dallo user space (attivo sul
   nostro telefono; da verificare). `vayu` non lo fa: il firmware resta ad aspettare. "WMI/HTT/CE not written" viene dopo e
   richiede la mission mode: topaz avverte che prima di quella un accesso ai registri CE blocca il bus.
2. **Probabile: il crash a `PC=0xb0193960`** è un indirizzo virtuale del Q6 nel dominio WLAN; senza simboli non si
   risolve. Il testo del crash (`file:riga`, di solito `err_qdi.c…wlan_process…`) sta nell'item SMEM 421: va letto
   prima di tutto (topaz `ITEM_MSS_CRASH 421`, `vayu_smem_census()`). Ipotesi, in ordine:
   - **L'host sparisce a metà avvio.** Dopo `cap` `vayu` chiude il client WLFW e poi i servizi tftp e pd-mapper, con
     l'MSA già assegnata al modem. Topaz invece "serve il modem per tutta la sessione" (`0122`) e rifà la procedura se
     il WLAN PD riparte (`0124`).
   - **Alimentazioni e clock spenti.** `icnss.c` di Android accende `vdd-cx-mx`/`xo`/`rfa`/`ch0` (L1A, L7A, L2C,
     L11C) *prima* di `ind_register` e le spegne a `FW_READY`. Mainline le accende solo a `hif_power_up` e vota
     `RF_CLK2`. `vayu_wifi.c` non vota nessuna alimentazione (lo fa solo `vayu_bt.c`, e `vayu_bt` registra che gli LDO
     non si leggono dall'arbitro SPMI delle app). Un firmware che tocca il chip RF spento può andare in assert.
   - **MSA**: se in quella prova c'era `vayu_msa_fixed_perm=1` (assegnazione saltata), una violazione XPU è fatale.
     Gli VMID usati sono quelli giusti, identici a `icnss.c` e a topaz.
   - **File**: `pull-firmware.sh` prende il *primo* `wlanmdsp.mbn`/`bdwlan.bin` che trova sotto `/vendor` e non i
     `bdwlan.bNN`. Un file non abbinato alla build del modem, o un `bdwlan` richiesto via tftp e assente, sono
     plausibili.
   - Da escludere col log tftp: richieste a cui nessuno risponde.

### Bluetooth su `vayu` (`vayu_bt.c`, 4467 righe): utile quasi riga per riga

Ha gli stessi registri del nostro: SE `0xC8C000`, wrapper `0xCC0000`, GCC SE3 (`0x1e4d4`/`0x1e4d8`), TLMM tile est
`0x03500000` + `0x1000·pin` per GPIO 43–46, voti RPMh `ldoa7`/`ldoc2`/`ldoc11`. Passi fatti funzionare:

1. lettura; UART GENI in FIFO a 2400 baud (divisore 250 da 19,2 MHz);
2. voti RPMh;
3. impulsi QCA `0xC0` a 2400, poi `0xFC` a 115200 (`hci_qca.c`: `qca_regulator_init`/`qca_send_power_pulse`);
4. versione SoC (EDL `01 00 FC 01 19`);
5. HCI;
6. download rampatch TLV (segmenti da 243 byte, ack `04 FF 03 00 04 00`) e NVM: tag 17 con IBS e baud, tag 27 deep
   sleep, tag 2 indirizzo BD. File `crbtfw*.tlv`/`crnv*.bin`; nome da `rom_ver`, che per WCN3998 è
   `((soc_ver & 0xF000) >> 7) | (soc_ver & 0xF)` in `btqca.c`;
7. consegna a iOS: tty `/dev/cu.bluetooth` + `/dev/uart.bluetooth` oppure kernel control `com.vayu.bt.uart`, che
   `bluetoothd` usa con `BT_HCI_TRANSPORT=H4:POSIX:<nome>,<baud>`;
8. stub di **BlueTool** (`userland/src/bluetool_stub.c`): XPC `com.apple.BlueTool`, risposta `result = 0`,
   `manufacturer 29`.

**Perché il loop.** Il `bluetoothd` dell'iPhone X è scritto per un Broadcom (nei simboli `bcm_vsc.c`,
`_BCM_CreateMultiAdvInstParser`). `vayu` ha trovato tre problemi, e li ha aggirati uno alla volta con uno "shim" che
risponde ai comandi OGF `0x3F` dal kernel:

- la catena d'inizializzazione si ferma dopo `0x0C31`, perché `0x0C5B` (Write_Default_Erroneous_Data_Reporting) è
  condizionato al bit 3 dell'ottetto 18 di Supported Commands: si riscrive la risposta `0x1002` e si risponde `0x0C5B`;
- `0xFD48` (multi-advertising Broadcom) con status 0 e payload vuoto fa scattare l'assert `bcm_vsc.c:642`; con status
  `0x01` (risposta reale del Qualcomm) passa;
- la modalità 2 (status 0 a qualsiasi comando vendor non elencato) **non basta**: `bluetoothd` ricomincia lo stesso.

Lezione: inventare risposte vendor dal kernel è fragile, perché i parser si aspettano payload precisi. Serve togliere
a `bluetoothd` il ramo Broadcom (sezione 4).

---

## 4. I port sulla stessa base ios-sweet: topaz, sapphire, renoir

### `MakrSas/ios-topaz`: il Wi-Fi WCN3988 funzionante (riferimento principale)

Commit `e4772911` (22 ottobre 2026). Kernel ios-sweet `xnu-8796`, userland iPhone X **16.7.16 (20H392)**. Nessuna
licenza nel repo; le patch derivate da HakiOS (`0900`) sono APSL 2.0, come dichiara `CREDITS.md`. Catena, tutta in
`iokit/Kernel/arm/TopazModem.cpp`, a polling, senza interrupt:

| Patch | Cosa | Per noi |
|---|---|---|
| 0113 | PAS del modem via TrustZone (SMC `0x42000200 \| cmd`: init_image `.mdt` → mem_setup → copia `.bNN` nella carveout → auth_and_reset), handshake SMP2P (item 435/428) | uguale; su SM8150 vanno aggiunti messaggio AOP QMP `load_state` e voti RPMh `mss.lvl`/`cx.lvl` (come `vayu_pas.c`) |
| 0115 | firmware letto **in sola lettura dalla partizione `modem_a`** | da noi partizione `modem`, tramite il driver UFS |
| 0118 | GLINK su SMEM + QRTR (`IPCRTR`, HELLO, elenco servizi) | uguale; mailbox APCS `0x17C0000C` bit 12 invece della IPC di SM6225 |
| 0121 | pd-mapper, tftp (`/readwrite` in RAM), rmtfs (EFS in sola lettura) | uguale (domini dai `.jsn` di `andromeda`) |
| 0122/0124 | WLFW fino a mission, sequenza completa con BDF e `CAL_REPORT`; tabelle CE/servizi/shadow di `snoc.c`; si rifà tutto se il WLAN PD riparte | **identico** (stessa famiglia WCN3990) |
| 0123/0125 | stream SMMU WLAN su un context bank **identità** (SCTLR.M=0: qui un S2CR "bypass" globale viene trasformato in fault, come nel *bypass quirk* di `arm-smmu-qcom.c`), copy engine a `membase+0x240000`, HTC, WMI TLV (SERVICE_READY → INIT → READY), scan passivo, HTT (anello RX, frag bank) | stessa cosa con SID `0x640` e `membase 0x18800000` |
| 0126 | API della radio per il resto del kernel | uguale |
| 0127/0128 | `sweetbusy`; nessun "busy" sulla radice del registro per il demone IOKit (IOKit non diventava mai quiescente e **wifid bloccava il boot**) | da prendere |
| 0129/0130 | stazione: VDEV_START, PEER_CREATE, AUTH/ASSOC come frame di gestione WMI, PEER_ASSOC + VDEV_UP, **handshake WPA2-PSK nel kernel** (SHA-1/PBKDF2/PRF/AES unwrap proprie), chiavi con VDEV_INSTALL_KEY, dati HTT in formato native wifi ↔ Ethernet; join anche con PMK già pronto (è ciò che wifid passa alla riconnessione) | uguale |
| 0900 | `en0`, `redwood_apple80211.c`/`redwood_sta.c`/`redwood_net.c`, `RedwoodWLAN.cpp`, **`MiniIONetworkStack.cpp`** (finto `IONetworkStack`, senza il quale l'InterfaceNamer di configd non pubblica mai `*QUIET*` e `SCNetworkInterfaceCopyAll()` aspetta 5 minuti), `kAKSWrapKey/UnwrapKey` | da prendere quasi intero |

Limiti dichiarati: solo WPA2-PSK, niente risparmio energetico né MAC casuale; `bluetoothd` in loop.
[`Angelpro09xd/ios-sapphire`](https://github.com/Angelpro09xd/ios-sapphire) (Redmi Note 13 4G, SM6225, WCN3988)
riprende topaz e aggiunge 0157/0158 (`IONetworkController` sotto il nodo `wlan`, `local-mac-address`) e 0294–0297
(rumore, limiti RSSI).

### `daimile2/renoir_xnu_bluethooth`: Bluetooth funzionante su 20H392

Commit `2b9d59c9` (4 novembre 2026), licenze per file (APSL/GPL-2.0/BSD-3). Mi 11 Lite 5G, SM7350, QCA6490; kernel
"XNU 8796.141.3, Renoir kernel143" su base **ios-sweet** (`ARM64_BOARD_CONFIG_SWEET`); `runtime_build: 20H392`. Stato
al 5 novembre: interruttore nativo, ricerca, abbinamento e riconnessione con chiavi salvate, **A2DP AAC-LC 44,1 kHz
che suona davvero**, inizializzazione del broadcast. HFP/SCO, sospensione e risparmio energetico ancora aperti.

| Pezzo | Dettaglio | Per noi |
|---|---|---|
| Kernel (0129–0143) | UART GENI in DMA + IRQ, H4 + IBS, sequenza `hci_qca`/`btqca` (versione, cambio velocità, rampatch, NVM, reset, indirizzo dall'ADT), **trasporto `UART.SK`** (kernel pipe Skywalk che `bluetoothd` usa nativamente), permesso `com.apple.private.skywalk.register-user-pipe` per la pipe audio A2DP | stessa base kernel: il trasporto e la parte Skywalk sono quasi copiabili; la parte chip viene da `vayu_bt.c` (WCN3998) |
| ADT | `/arm-io/bluetooth`: `device_type "bluetooth"`, `transport-encoding = 1` (H4), `transport-speed` (3 200 000), `local-mac-address` (dato dal proprietario, mai nel repo) | da aggiungere a `scripts/mkadt.py` |
| BlueTool | `renoir_bluetool.c`: `com.apple.BlueTool`, `RunBuiltinScript {script, btaddress}`; "boot"/"init" restituiscono il risultato **reale** dell'avvio del kernel (`sysctl kern.renoir_bt_boot`), "deepsleep" lo gestiscono NVM + IBS; gli script Broadcom sono rifiutati | da copiare |
| `bluetoothd` | `tools/patch-renoir-bluetoothd.py`: patch offline del binario 20H392 (**SHA-256 `8b7c0a0b…3a8f`**), codice nel padding dell'header `__TEXT`, `LC_LOAD_DYLIB` di `RenoirBTType0.dylib`; il controller diventa `THIRD_PARTY_DONGLE = 0`, con capacità prese da HCI reale (Supported Commands, LE features, `0x203B` per il numero di set di advertising) | se il nostro hash coincide, si riusa così com'è |
| Alimentazione | "dipende dal Wi-Fi che tiene i voti delle alimentazioni condivise" | da noi L7A/L2C/L11C sono condivise Wi-Fi/BT: serve un proprietario unico con conteggio dei voti |

---

## 5. UniXNU (SM8550, iPadOS 27)

| Repository | Cosa | Riusabile per noi |
|---|---|---|
| [`Ath12kWifi`](https://github.com/UniXNU/Ath12kWifi) (`28de7b91`) | ath12k + **mac80211 + cfg80211 di Linux 7.0.14** su XNU, header FreeBSD, **`wpa_supplicant` 2.12 in user space** (`userspace/driver_sheng.c` collega SME/WPA all'endpoint `org.unixnu.wifi.control`). Interfaccia scheda/modulo in `include/nw_abi.h` (`lx_platform`, `nw_board`, `nw_wifi_*`). Su `sheng`: toggle, scan, WPA2 (anche PMF), DHCP, traffico | l'idea dell'ABI scheda/modulo e il supplicant in user space (WPA3/PMF più avanti). Non il trasporto: PCIe + MHI non è SNOC/QMI su modem |
| `UniXNU-sheng` (`WifiProbe.cpp`, `WifiNet.cpp`) | `WifiNet` espone `en0` + Apple80211 per gli ABI di iPadOS 27 (richieste di associazione da 592 e 656 byte, evento asincrono da 540 byte); il nodo `/arm-io/wlan` soddisfa la condizione del job wifid | metodo per riconoscere i layout; per 16.7.16 valgono quelli verificati da topaz |
| [`QcaBluetooth`](https://github.com/UniXNU/QcaBluetooth) (`22ff97f9`) | `btqca.c` di Linux bloccato (da `ianchb/sm8550-mainline`), GENI in DMA, H4/IBS, **UART.SK** (derivato da renoir), BlueTool XPC, `QcaBTType0.dylib` e patch del `bluetoothd` di 24A446 per il "tipo zero" (svuota la tabella delle operazioni vendor, quattro hook e i due opcode vendor); scansione estesa `0x2041/0x2042`, indirizzo casuale `0x2005` prima dello scan. Funzionano abbinamento, A2DP, rumore e batteria degli AirPods; "Unavailable: features that need Apple vendor commands" | conferma l'approccio tipo 0; i `WcnPower.h` (voti indipendenti Wi-Fi/BT sulle alimentazioni condivise) sono il modello per noi |
| [`QcomBaseband`](https://github.com/UniXNU/QcomBaseband) (`dd0199cf`, HoshinoYumeka / chiba233, BSD-2) | ponte baseband: QMUX/QRTR, servizi AppleBaseband, IPA v4.2 per SC7180 | solo per la telefonia (in futuro); per il Wi-Fi conferma che la scheda deve fornire modem PAS, SMEM/SMP2P, GLINK/QRTR e rmtfs, cioè la stessa infrastruttura |

---

## 6. Altri progetti, e cosa insegnano sulle interfacce Apple

- **itlwm / AirportItlwm** (OpenIntelWireless, zxystd): Wi-Fi Intel su macOS. `itlwm` si presenta come Ethernet e prende
  le credenziali da un'app propria (HeliPort). `AirportItlwm` emula IO80211Family (sottoclassi di `IO80211Controller` e
  delle interfacce, gestione delle richieste `apple80211`, eventi `postMessage` SSID/BSSID/LINK/SCAN/ASSOC) con lo
  stack `net80211` di OpenBSD portato, supplicant WPA compreso, nel kernel. Lezioni: (a) Apple si aspetta che la WPA
  personale stia nel driver o nel firmware (sui Broadcom degli iPhone lo fa il firmware), quindi un supplicant nel
  kernel è "nativo" per wifid; (b) l'interfaccia è un insieme stabile di GET/SET numerati con strutture versionate,
  ricostruibile dai simboli del framework. Su iOS la kext IO80211Family non esiste nel nostro kernel: HakiOS ottiene lo
  stesso risultato con classi finte e il ripiego sulle ioctl.
- **BrcmPatchRAM / BlueToolFixup** (acidanthera): carica il firmware `.hcd` nei Broadcom USB con i VSC di PatchRAM
  (`0xFC2E` Download_Minidriver, poi `0xFC4C`/`0xFC4E` Write/Launch_RAM) e, da macOS 12, modifica `bluetoothd` perché
  accetti controller di terze parti. È l'equivalente macOS del "tipo 0".
  **IntelBluetoothFirmware** (OpenIntelWireless): caricamento del firmware Intel, più `IntelBTPatcher` per i comandi LE
  che il controller rifiuta, lo stesso tipo di problema che UniXNU risolve forzando la scansione estesa.
- **Linux `btbcm.c`** conferma i VSC Broadcom che il mondo Apple dà per scontati: `0xFC01` (BD_ADDR), `0xFC1C`/`0xFC1D`
  (scrittura/lettura parametri PCM per SCO), `0xFC2E` (minidriver), `0xFC5A` (prodotto USB), `0xFC6E` (feature del
  controller), `0xFC79` (configurazione estesa). Su un QCA i corrispondenti sono `0xFC14` (BD_ADDR),
  `0xFC48` (baud), `0xFC00` (EDL/patch), `0xFC0B` (NVM), `0xFC17` (log off) di `btqca.h`.
- **Darling** (macOS su Linux) non ha driver Wi-Fi o BT: usa la rete dell'host, e per noi non serve. Anche i progetti
  iOS-su-QEMU portano la rete con dispositivi virtuali (HakiOS: `redwood_virtio_net.c`), non con Apple80211: utili
  solo per provare la parte BSD/configd senza radio.

---

## 7. Le interfacce iOS in sintesi (16.7.16 / 20H392)

**Wi-Fi.** Il percorso provato su 20H392 da topaz (e su 16.5 da HakiOS):

| Requisito | Chi lo usa |
|---|---|
| Nodo ADT `wlan` con `local-mac-address` | MobileGestalt/lsd (altrimenti 5 s per ogni domanda) |
| `IO80211Controller` (← `IOEthernetController` ← `IONetworkController`) con `IOMACAddress`, `IOBuiltin`; figlio `IONetworkInterface` con `IOInterfaceName`, `BSD Name`, `IOInterfaceNamePrefix`, `IOInterfaceType` 6, `IOInterfaceUnit`, `IOPrimaryInterface`, `IO80211InterfaceRole = "Infrastructure"` | wifid (trova il device), SystemConfiguration (lo classifica come Wi-Fi) |
| `newUserClient` che rifiuta | IO80211.framework ripiega sulle ioctl |
| `IONetworkStack` apribile che rifiuta il naming | InterfaceNamer di configd → `*QUIET*` |
| Ioctl `SIOC[GS]A80211` con i tipi della sezione 2 | wifid, Impostazioni, barra di stato |
| Eventi `KEV_IEEE80211_CLASS` | wifid (fine scan, fine join, link) |
| `SIOCGIFMEDIA` e `KEV_DL_LINK_ON/OFF` | KernelEventMonitor/IPMonitor di configd (non instrada su un link spento) |
| Interfaccia "instradabile" dopo il join (servizio DHCP di configd) | wifid ("failed to become routable interface"), poi Impostazioni spunta la rete |
| `kAKSWrapKey`/`kAKSUnwrapKey` | securityd → portachiavi (password, riconnessione automatica con PMK) |
| IOKit quiescente (topaz 0128) | wifid non blocca l'avvio |

**Bluetooth.** `bluetoothd` di 20H392:

| Requisito | Fonte |
|---|---|
| Trasporto: nativo `UART.SK` (Skywalk kernel pipe, letto dalle proprietà di `/arm-io/bluetooth`) oppure kernel control/tty con `BT_HCI_TRANSPORT=H4:POSIX:…` (richiede di cambiare l'ambiente del job) | renoir, UniXNU / `vayu` |
| `com.apple.BlueTool` via XPC: `RunBuiltinScript` "boot"/"init"/"deepsleep" → `result` (0 ok; 10 e 13 fanno ripartire `bluetoothd`), `manufacturer`, `firmware` | renoir, `vayu` |
| Controller "tipo 0": niente tabella di operazioni vendor Broadcom, capacità da Supported Commands/LE Features | renoir (20H392), UniXNU (24A446) |
| Portachiavi funzionante (chiavi di abbinamento) | UniXNU (`AppleKeyStoreState`), topaz `SweetKeyStore` |
| Pipe audio A2DP: permesso Skywalk user-pipe | renoir 0140 |

**I comandi vendor che `bluetoothd`/BlueTool mandano all'avvio**, per quanto risulta dalle fonti:

1. *BlueTool* (script Broadcom): caricamento PatchRAM (`0xFC2E`, `0xFC4C`, `0xFC4E`), baud (`0xFC18`), BD_ADDR
   (`0xFC01`), modalità sleep (`0xFC27`). Con BlueTool sostituito non partono.
2. *`bluetoothd`*: VSC del ramo Broadcom (`bcm_vsc.c`), tra cui `0xFD48` (crea un'istanza multi-adv) e quelli di
   lettura versione/feature (`0xFC79`/`0xFC6E`, da confermare con una traccia).
3. *Comandi standard condizionati*: `0x0C31` (flow control host→controller), `0x0C5B` (dipende dall'ottetto 18 bit 3),
   LE `0x2005`, `0x2035`, `0x203B`, `0x2041/0x2042`.

Come trattarli: (a) **tipo 0**, consigliato: elimina 1 e 2 alla fonte; (b) tradurre solo gli equivalenti diretti
(BD_ADDR → `0xFC14` o tag NVM 2; baud → `0xFC48`; sleep → IBS); (c) come ultima risorsa rispondere `0x01` (Unknown
HCI Command), che manda i parser sul ramo d'errore, invece di status 0 con payload vuoto, che fa scattare gli assert.
Per avere l'elenco esatto: traccia H4 del kernel (`vayu_bt_trace=1`) e analisi statica del binario sul PC (Ghidra o
`ipsw`), senza eseguire nulla sul telefono.

---

## 8. Piano a fasi per `andromeda`

Ogni driver parte spento e si accende col suo boot-arg (metodo della ROADMAP). Le prove passano prima per la sola
lettura, poi per la scrittura. Prima di ogni prova va letto il crash in SMEM 421. Difficoltà: B bassa, M media, A alta.

### Rete subito: Internet del PC via USB (parallela a tutto)

| Fase | Obiettivo | Fatto quando | Diff. | Da chi |
|---|---|---|---|---|
| N0 | NAT sul PC (sezione 9) | `tcpdump` sul PC vede uscire il traffico del telefono | B | ios-sweet `NOTES.md` |
| N1 | iOS vede l'interfaccia USB: ifnet rinominato `en1`, `IOEthernetController` + `IONetworkInterface` (come `HakiosWLAN`, ma Ethernet e non built-in), `MiniIONetworkStack`, quiescenza IOKit (topaz 0128); eventi link già in 0081 | log `SWEET NET` (patch 0080): configd aggiunge indirizzo e route di default su `en1`; query DNS visibili sul PC; Safari carica una pagina | B–M | HakiOS `hakios.wlan.cpp`, topaz 0900 `MiniIONetworkStack.cpp`, ios-sweet 0075/0076/0080/0081 |

### Wi-Fi

| Fase | Obiettivo | Fatto quando | Diff. | Da chi |
|---|---|---|---|---|
| W0 | Ricognizione in sola lettura: stato RPMh di `ldoa1/ldoa7/ldoc2/ldoc11` e `RF_CLK2` (cmd-db), SMMU per SID `0x640`, SMEM (modem mai partito?), mappa UEFI per un pool di memoria fuori dalla finestra XNU. Firmware dal telefono (in Android, una volta): `modem.mdt/.bNN`, `wlanmdsp.mbn`, `bdwlan.*`, `.jsn` dalla partizione `modem`, fuori da git | rapporto con valori letti, nessuna scrittura | B | HakiOS passi 1–5, `vayu_bt` passo 1 |
| W1 | Modem PAS: messaggio AOP QMP `load_state modem on`, voti `mss.lvl`/`cx.lvl`, `INIT_IMAGE` → copia in `0x8E000000` → `AUTH_AND_RESET`, SMP2P 428/435; arresto pulito (SMP2P stop → `PAS_SHUTDOWN`) | bit "ready" nell'item 435, nessun err-fatal per 60 s, riavvio della prova senza reset del telefono | A | `vayu_pas.c`/`vayu_modem.c` (stesso SoC), topaz 0113/0115/0116/0117, ordine di arresto di HakiOS |
| W2 | GLINK `IPCRTR` + QRTR; servizi host: rmtfs (14) da `modemst1/2/fsg/fsc` in RAM, pd-mapper (`0x40`), tftp (4096) con `/readwrite` in RAM, **tutti attivi per tutta la sessione** | servizio `0x45` annunciato; modem stabile più di 5 minuti; il log tftp mostra `wlanmdsp.mbn` letto | M | `vayu_rmtfs.c`, `vayu_wifi.c` (pd-mapper/tftp), topaz 0118/0121; correzioni GLINK/QRTR delle review HakiOS |
| W3 | WLFW fino a mission: `host_cap` (`daemon_support=0`), MSA a `0x8BC00000` con assegnazione SCM (`0xF`, `0x18`, `0x19`), **BDF `bdwlan.bNN` dal `board_id`**, `CAL_REPORT`, `FW_READY`, `WLAN_CFG` (tabelle CE di `snoc.c`), `WLAN_MODE` 0; alimentazioni e `RF_CLK2` votate prima, come `icnss.c` | log "WLAN ON (mission)"; i registri a `0x18800000` rispondono; nessun crash del WLAN PD | M | topaz 0122/0124 (identico), `vayu_scm_assign_msa`, `icnss.c` |
| W4 | SMMU: context bank identità per SID `0x640`; CE (12 pipe, `membase+0x240000`, RRI in DDR), HTC, WMI TLV (`INIT` con `resource_config` WCN3990), HTT RX; scan passivo | elenco BSS nel log (BSSID mascherati); poi richiesta di scan su ordine | A | topaz 0123/0125/0126, ath10k `ce.c`/`htc.c`/`wmi-tlv.c`/`hw.c` |
| W5 | Stazione: VDEV/PEER, AUTH/ASSOC, WPA2-PSK nel kernel (con le correzioni di `wifi-stack.md`: chiavi candidate, KEYS persistente, filtro sui frame non protetti, replay check sui frame di gruppo), dati HTT ↔ Ethernet; MAC da file privato | con `fw/network` di prova: lease DHCP e `ping` dal kernel | A | topaz 0129/0130, HakiOS `lx_wpa.c` (rivisto) |
| W6 | Superficie iOS: `en0`, Apple80211 ioctl, classi IOKit, `local-mac-address` nel nodo `wlan` dell'ADT, AKS wrap/unwrap, eventi | Impostazioni elenca reti, entra in WPA2, salva la password, si ricollega dopo un riavvio (ramo PMK); Safari HTTPS | M | topaz 0900 (= HakiOS `hakios.apple80211.c`/`sta.c`/`net.c`/`wlan.cpp`), ios-sapphire 0157/0158/0294–0297 |
| W7 | Rifinitura: interrupt CE (INTID 446–457) invece del polling, risparmio energetico, rekey, roaming, WPA3 (eventuale passaggio a mac80211 + `wpa_supplicant` come UniXNU), coesistenza con BT | ore di traffico senza errori; batteria accettabile | A | UniXNU `Ath12kWifi` (modello), HakiOS `lx_bus.c` (IRQ) |

### Bluetooth

| Fase | Obiettivo | Fatto quando | Diff. | Da chi |
|---|---|---|---|---|
| B0 | Lettura: wrapper QUP2, SE13, GPIO 43–46, RCG/branch GCC, LDO | stessi valori trovati da `vayu` (o le differenze spiegate) | B | `vayu_bt.c` passo 1 |
| B1 | Accensione + UART + HCI: voti `ldoa7/ldoc2/ldoc11` (+ S4A), UART a 2400, `0xC0`/`0xFC`, 115200, versione EDL, `HCI_Reset` | `Read_Local_Version` con manufacturer 29, `soc_id` letto | M | `vayu_bt.c` passi 2–7, `hci_qca.c` |
| B2 | Firmware: `crbtfwXX.tlv` + `crnvXX.bin` dalla partizione `bluetooth`, NVM (IBS, BD_ADDR da file privato), baud alto con `0xFC48`, reset | `lmp_subver` cambiato; reset a 3,2 Mbaud | M | `vayu_bt.c` passo 8, `btqca.c`, renoir 0136/0142 |
| B3 | Trasporto e BlueTool: `UART.SK` + nodo ADT `/arm-io/bluetooth` (oppure kernel control `vayu` come ripiego), BlueTool che restituisce il risultato reale | `bluetoothd` resta vivo più di 10 minuti, senza loop | M | renoir 0137/0141/0143 + `renoir_bluetool.c`; `vayu` kctl |
| B4 | Comandi vendor: patch tipo 0 del `bluetoothd` 20H392 (controllo hash) + dylib delle capacità; ripiego: shim VSC con le regole di `vayu` | interruttore in Impostazioni acceso; la ricerca mostra i dispositivi | M–A | renoir `patch-renoir-bluetoothd.py`/`renoir_bt_type0.c`; UniXNU `native-ios27.md` (metodo) |
| B5 | Abbinamento e profili: chiavi nel portachiavi, A2DP (permesso Skywalk user-pipe), poi HFP/SCO (instradamento SCO su HCI invece che su SLIMbus da verificare) | cuffie abbinate che si ricollegano; A2DP che suona | A | renoir 0140, UniXNU QcaBluetooth |
| B6 | Energia e coesistenza: sleep IBS, risveglio da GPIO 46, voti condivisi con il Wi-Fi contati | BT acceso senza consumo anomalo; Wi-Fi e BT insieme | M–A | UniXNU `WcnPower.h`, renoir `docs/STATUS.md` (lavoro rimasto) |

**Ordine consigliato.** Prima N0–N1, perché è la rete subito e il lavoro su configd serve poi anche al Wi-Fi. Poi B0–B3:
hardware già mappato da `vayu`, nessun modem richiesto. Intanto W0–W3, il percorso critico, perché il modem SM8150 è
la parte più rischiosa. Poi W4–W6 (copia ragionata di topaz) e B4–B5.

---

## 9. Quick win: Internet del PC via USB CDC-ECM

**Lato PC** (Linux; da eseguire in `bash`, perché la shell predefinita è fish). `USBIF` è l'interfaccia del telefono
(`ANDROMEDA.md`: `enp0s20f0u1`, MAC host `02:00:5e:77:ee:01`), `UPLINK` quella che va verso Internet:

```bash
USBIF=enp0s20f0u1; UPLINK=wlan0
sudo nmcli device set "$USBIF" managed no
sudo ip addr replace 192.168.7.1/24 dev "$USBIF" && sudo ip link set "$USBIF" up
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 192.168.7.0/24 -o "$UPLINK" -j MASQUERADE -m comment --comment andromeda
sudo iptables -A FORWARD -i "$USBIF" -o "$UPLINK" -j ACCEPT -m comment --comment andromeda
sudo iptables -A FORWARD -i "$UPLINK" -o "$USBIF" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT -m comment --comment andromeda
# solo se il servizio iOS è in DHCP (port=0: nessun DNS locale, niente conflitto con systemd-resolved)
sudo dnsmasq --no-daemon --port=0 --interface="$USBIF" --bind-interfaces \
  --dhcp-range=192.168.7.2,192.168.7.2,255.255.255.0,1h \
  --dhcp-option=option:router,192.168.7.1 --dhcp-option=option:dns-server,1.1.1.1,8.8.8.8
# verifica: sudo tcpdump -ni "$USBIF" 'port 53 or icmp or tcp port 443'
```

Le regole non sopravvivono al riavvio del PC; si tolgono cercando il commento `andromeda`. Con firewalld/nftables vale
lo stesso (masquerade sulla zona dell'uplink, `forward` dall'interfaccia USB). La regola `INPUT` per UDP 5140 del log
dal vivo resta com'è.

**Lato telefono (kernel).** Il kernel ha già `ue0` con `192.168.7.2/24` (0075), mbuf corretti (0076), stato del link
per configd (0081) e la traccia di route e indirizzi (0080). La prova di ios-sweet (immagine v15, servizio manuale "USB
Ethernet" con router `192.168.7.1` e DNS `1.1.1.1`) non ha prodotto né DNS né traffico in uscita. La causa indicata
dalle loro note è che configd non vede `ue0`, perché manca l'oggetto IOKit. Cosa serve:

1. ifnet chiamato `en1`, lasciando `en0` al Wi-Fi; l'InterfaceNamer e SCNetworkInterface ragionano su prefisso e unità;
2. `IOEthernetController` con `IOMACAddress` (il MAC del lato telefono, inventato e "locally administered") e figlio
   `IONetworkInterface` con `IOInterfaceName`/`BSD Name` `en1`, `IOInterfaceNamePrefix` `en`, `IOInterfaceType` 6,
   `IOInterfaceUnit` 1, `IOBuiltin` falso. Lo schema è quello di `hakios.wlan.cpp`, ma senza `IO80211Controller` e
   senza `IO80211InterfaceRole`;
3. `MiniIONetworkStack` (topaz 0900), altrimenti `SCNetworkInterfaceCopyAll()` aspetta 5 minuti;
4. quiescenza IOKit (topaz 0128);
5. servizio: DHCP (dnsmasq qui sopra) oppure il `preferences.plist` manuale di ios-sweet v15, con `DeviceName en1`.
   Senza Wi-Fi né cellulare, IPMonitor rende primaria l'Ethernet e mDNSResponder prende il DNS dal servizio.

Ripiego senza configd, solo per i demoni: route di default aggiunta dal kernel e DNS scritto in
`State:/Network/Global/DNS` da un piccolo tool con l'entitlement di scrittura su SCDynamicStore. Le app che
controllano la raggiungibilità vogliono comunque il percorso via configd.

Fatto quando: `SWEET NET: configd route add default 192.168.7.1` nel log, DNS e TLS visibili nel `tcpdump`, Safari
apre una pagina HTTPS.

---

## 10. Domande aperte e rischi

1. **Il modem interno del 5G parte senza l'SDX50M?** `pil_modem` ha `qcom,poff-depends-on = "esoc0"` e Android fa girare
   `mdm_helper`. Da verificare in W1 (il LTE del modem interno dovrebbe essere indipendente; il Wi-Fi sì).
2. **Firmware giusti**: `wlanmdsp.mbn` e `bdwlan.bNN` devono venire dalla stessa build NON-HLOS della partizione
   `modem`. Il `board_id` arriva da `cap`. `vayu` ne prende il primo che trova: meglio leggere direttamente la
   partizione, come topaz 0115.
3. **Alimentazioni e `RF_CLK2` dopo il UEFI di Aloha**: chi le tiene accese? I voti RPMh di XNU vanno coordinati con
   quelli del UEFI (`vayu_bt_giveback=0`: non togliere voti che non sono nostri).
4. **Hypervisor e SCM**: `vayu` dimostra PAS e `MEM_PROT_ASSIGN` sotto UEFI sull'SM8150. Va confermato che
   l'hypervisor trasforma il bypass S2CR in fault anche per il SID `0x640`, come per topaz.
5. **Memoria per TrustZone, modem e CE**: buffer contigui sotto 2^35 (CE), condivisi in write-combined. Una volta
   assegnata al modem (rmtfs, MSA), quella memoria HLOS non deve più toccarla: meglio un pool fuori dalla finestra XNU,
   da scegliere nella mappa UEFI (`ShengBootMemoryDxe` e la finestra da 1,5 GiB sono già delicati).
6. **Il crash `PC=0xb0193960` di `vayu`**: servono il loro log e il testo dell'item SMEM 421 per sapere quale ipotesi
   della sezione 3 è quella giusta.
7. **`bluetoothd`**: controllare che il nostro binario abbia lo SHA-256 atteso dal patcher renoir
   (`8b7c0a0b…3a8f`). Noi parliamo di iOS 16.7.16 e l'immagine di ios-sweet è 20H392: il README di topaz conferma
   che sono la stessa build.
8. **SCO e chiamate**: su WCN399x l'SCO va di default su SLIMbus (`btfmslim`); per HFP via HCI serve un'impostazione
   vendor o NVM da trovare.
9. **Indirizzi MAC e BD**: in Android stanno in `persist`. Vanno letti in sola lettura e tenuti fuori da git e dai log,
   come fanno HakiOS e renoir.
10. **Licenze**: `ios-topaz` e `hakios-sm8150` non dichiarano una licenza (le parti derivate da XNU/HakiOS sono APSL).
    Prima di copiare codice conviene chiedere agli autori o riscrivere seguendo il codice come riferimento. `btqca.c`
    e `hci_qca.c` sono GPL-2.0 (renoir e UniXNU tengono le licenze file per file). Il patcher di `bluetoothd` modifica
    un binario Apple: si distribuisce lo strumento, non il binario.
11. **Sicurezza**: il codice derivato da HakiOS va preso con le correzioni delle loro review (overflow GLINK, SMEM e
    QRTR; regole WPA). Il supplicant nel kernel non fa WPA3.

---

## 11. Fonti

Grazie agli autori: il piano è quasi tutto lavoro loro.

- **HakiOS**, OOSEX GANG: [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) commit
  `63fc2b4633bac3af8e57dadab9f5a8d4cb940438` (APSL 2.0): `drivers/wifi/hakios.wifi.c`, `hakios.wlfw.c`, `hakios.lx.c`,
  `hakios.sta.c`, `hakios.apple80211.c`, `hakios.wlan.cpp`, `drivers/bluetooth/hakios.bt.c`, `wifi/lx/src/*`,
  `tools/mk-ath11k.sh`, `docs/driver-review/wifi-bringup.md`, `wifi-stack.md`, `docs/devices/redwood.md`,
  `userland/src/wifipw_test.c`. Device repo [`oosexgang/hakios-sm7325`](https://github.com/oosexgang/hakios-sm7325)
  `04fbfff9` (`device.conf`, `pull-firmware.sh`).
- **HakiOS SM8150**, nomorecoolnicknames: [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150)
  `3e7be137a100cef19f1d7e83346b6e688be697d1`: `README.md`, `device.conf`, `pull-firmware.sh`,
  `kernel/bsd/dev/vayu_wifi.c`, `vayu_bt.c`, `vayu_modem.c`, `vayu_rmtfs.c`, `vayu_pas.c`, `userland/src/bluetool_stub.c`.
- **ios-topaz**, MakrSas: [`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz) ramo `topaz`
  `e4772911e116ab4ab6c4ac1f38e6106fefe8b483`: `README.md`, `CREDITS.md`, `xnu/patches/0113`–`0130`, `0900`; dati
  hardware da [`MakrSas/topaz-woa`](https://github.com/MakrSas/topaz-woa) (driver Windows `ModemPas.c`, `Wlfw.c`,
  `ce.c`, `htc.c`, `wmi.c`, `scan.c`, `assoc.c`, citati nelle patch).
- **ios-sapphire**, Angelpro09xd: [`Angelpro09xd/ios-sapphire`](https://github.com/Angelpro09xd/ios-sapphire)
  `4c2a9c64`: `README.md` (patch 0150, 0157, 0158, 0294–0297).
- **Renoir XNU Bluetooth**, daimile2 e collaboratori:
  [`daimile2/renoir_xnu_bluethooth`](https://github.com/daimile2/renoir_xnu_bluethooth)
  `2b9d59c921c9ec803462b2be8deb6980f7297c22`: `README.md`, `docs/ARCHITECTURE.md`, `docs/STATUS.md`, `docs/PATCHES.md`,
  `NOTICE.md`, `config/boot-contract.json`, `config/dependencies.json`, `userland/renoir_bluetool.c`,
  `tools/patch-renoir-bluetoothd.py`, `kernel/base/bsd/dev/renoir_wifi.c`.
- **UniXNU**: [`UniXNU/Ath12kWifi`](https://github.com/UniXNU/Ath12kWifi) `28de7b91` (`README.md`, `include/nw_abi.h`),
  [`UniXNU/QcaBluetooth`](https://github.com/UniXNU/QcaBluetooth) `22ff97f9` (`README.md`, `native-ios27.md`,
  `NOTICE.md`), [`UniXNU/QcomBaseband`](https://github.com/UniXNU/QcomBaseband) `dd0199cf` (di HoshinoYumeka,
  [@chiba233](https://github.com/chiba233), da Trogdor-ipadOS), [`UniXNU/UniXNU-sheng`](https://github.com/UniXNU/UniXNU-sheng)
  `ce0c600f` (`reports/ipados27-wifi-association.md`, `ipados27-bluetooth-*.md`), [`UniXNU/UniXNU`](https://github.com/UniXNU/UniXNU)
  `10edc1ef` (`docs/drivers/ath12k-wifi.md`, `qca-bluetooth.md`, `docs/porting/network.md`); riferimenti Linux di
  `ianchb/sm8550-mainline`.
- **ios-sweet**, dipa-studio: [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) `45263bee`: `NOTES.md`
  (rete USB, v15, configd), `xnu/patches/0075`–`0081`.
- **Linux mainline** (torvalds/linux, `master` del 6 novembre 2026): `drivers/net/wireless/ath/ath10k/snoc.c`, `qmi.c`,
  `hw.c`, `hw.h`; `drivers/bluetooth/btqca.c`, `btqca.h`, `btbcm.c`; `arch/arm64/boot/dts/qcom/sm8150.dtsi`,
  `sm8150-hdk.dts`. Kernel Xiaomi [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource)
  ramo `andromeda-p-oss`: `drivers/soc/qcom/icnss.c`, `icnss_qmi.c` (in quel ramo ath10k non ha `snoc.c`: Android usa
  qcacld-3.0).
- **macOS, per il confronto** (dalla documentazione pubblica dei progetti, non dal codice): itlwm/AirportItlwm,
  IntelBluetoothFirmware/IntelBTPatcher (OpenIntelWireless, zxystd e collaboratori), BrcmPatchRAM/BlueToolFixup
  (acidanthera), Darling.
- **Dati del telefono**: `phone/<seriale>/live-fdt.dts` e `info/getprop.txt`, `info/partitions.txt` (fuori da git).
