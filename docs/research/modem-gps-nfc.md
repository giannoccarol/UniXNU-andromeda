# Rete cellulare, GPS e NFC per `andromeda`: cosa serve e cosa è realistico

Ricerca del 6 novembre 2026, solo lettura: nessun comando sul telefono, nessun codice dei repository scaricati eseguito,
nessuna riga di codice scritta. Riguarda la voce 14 della [ROADMAP](../../ROADMAP.md) (fotocamere escluse). I valori
del Mi Mix 3 5G vengono dal device tree attivo del telefono (`phone/<seriale>/live-fdt.dts`) e da `info/getprop.txt`,
che restano fuori da git. Quelli del kernel Xiaomi vengono dal ramo `andromeda-p-oss`. I fatti sul lato iOS sono stati
letti in locale dall'IPSW 16.7.16 (`20H392`) e dalla root costruita da `build-root.sh`: `BuildManifest.plist`, il device
tree `d22ap`, i plist di launchd e i nomi di simboli e classi in CommCenter, locationd, gpsd, nfcd e nella dyld shared
cache. Da quei file riportiamo solo nomi. Nessun file Apple entra nel repository. Le fonti sono elencate in [Fonti](#9-fonti).

Livelli di prova usati sotto: **[visto]** = letto da noi in un file, nel device tree o in un binario; **[codice]** = c'è
nel codice di un altro progetto; **[dichiarato]** = README o wiki di un altro progetto, senza nostra verifica;
**[ipotesi]** = deduzione nostra, da verificare.

## Sommario

| Funzione | Giudizio | Perché, in breve |
|---|---|---|
| **Dati mobili 4G** | **molto difficile, ma realistico** | La strada iOS c'è: [`QcomBaseband`](https://github.com/UniXNU/QcomBaseband) di HoshinoYumeka ([@chiba233](https://github.com/chiba233)), BSD-2, porta CommCenter fino a LTE e dati IPv4 su un modem Qualcomm integrato (SC7180, iPadOS 17). Da noi mancano due pezzi: l'adattamento all'ABI di iOS 16 per l'iPhone X, e l'**IPA v4.1** dell'SM8150. Nessuno ha l'IPA v4.1 sotto XNU, e su `vayu` (SM8150) i dati via IPA bloccano il SoC |
| **SMS** | **molto difficile** (ma viene quasi gratis dopo il ponte dei dati) | CommCenter parla già WMS (QMI `0x05`) e il ponte lo fa passare. Non serve l'IPA. Nessuno l'ha mai provato sotto XNU. Su Linux SM8150 gli SMS funzionano col ramo IPA di Andrew Gigena |
| **Telefonate** | **molto difficile** su 2G/CSFB, **non realistico** in VoLTE con lo stack IMS di iOS | Serve tutto l'audio (ADSP, sessione voce CVD, WCD934x, amplificatori), che oggi non c'è. Il VoLTE di iOS gira sul processore principale e usa servizi QMI privati di Apple che il firmware Xiaomi non ha [ipotesi]. Su Linux il VoLTE è sperimentale anche su SDM845 |
| **5G (X50, SDX50M)** | **non realistico** | Servirebbero PCIe, MHI, ESOC, Sahara, EFS dell'X50 e l'offload IPA↔MHI. Nemmeno Linux mainline ha un X50 funzionante. Per il 4G non serve [ipotesi da verificare] |
| **GPS** | **fattibile** per una strada laterale, **molto difficile** in modo nativo | Il motore GNSS è nel modem (QMI LOC `0x10`). Un nostro client LOC può passare le posizioni a `locationd` tramite il servizio di simulazione (`com.apple.locationd.simulation`), come progetta iOSPlus10. Non servono SIM, IPA, audio né trasmissione. La via nativa dell'iPhone X è un'altra: `locationd` → "Eureka" → QMI **PDS** sul trasporto baseband Apple |
| **NFC** | **non realistico** per iOS | `nfcd` vuole il kext Apple `AppleStockholmControl` e il controller NXP PN80V con firmware Apple. Avremmo un NQ3xx (PN553) con firmware Xiaomi/NXP. Apple Pay è impossibile (Secure Enclave). Leggere tag fuori da iOS è possibile ma vale poco |

**Cosa viene gratis dal Wi-Fi.** Le fasi W0–W2 di [wifi-bluetooth.md](wifi-bluetooth.md#8-piano-a-fasi-per-andromeda)
sono proprio quelle che servono anche a cellulare e GPS: avvio del modem via TrustZone, SMP2P, GLINK `IPCRTR`, QRTR,
rmtfs, pd-mapper e tftp attivi per tutta la sessione. Con W2 si ottiene anche il primo dato decisivo, cioè l'**elenco
dei servizi QMI** annunciati dal firmware di questo telefono. Da quello dipende quasi tutto il resto (LOC o PDS, VOICE,
WMS, IMS). Il cellulare aggiunge sopra: IPA, il ponte verso CommCenter e, per la voce, l'audio.

**Ordine consigliato, dopo W2:** GPS (G0–G1), poi dati solo dal kernel (C2), poi il ponte iOS (C3) e gli SMS (C4).
Le telefonate (C5) vengono per ultime, dopo l'audio della voce 10 della ROADMAP.

---

## 1. Hardware e firmware

### 1.1 Modem integrato (MPSS, "X24")

Molti valori sono già in [wifi-bluetooth.md §1](wifi-bluetooth.md#1-lhardware-di-andromeda-fdt-attivo). Qui ci sono
quelli che servono alla parte cellulare.

| Cosa | Valore | Da dove |
|---|---|---|
| PAS | `qcom,mss@4080000`, `pil-tz-generic`, **`pas-id 4`**, firmware `modem` (`modem.mdt` + `.bNN`), regione `0x8E000000` + `0x9600000` (150 MiB), crash in SMEM 421, `ssctl-instance-id 0x12`, `sysmon-id 0`, `minidump-id 3`, voti proxy `vdd_cx`/`vdd_mss` a `0x181`, `qcom,signal-aop`, `qcom,poff-depends-on = "esoc0"` | FDT; mainline `qcom_q6v5_pas.c` (`mpss_resource_init`: stessi pas-id, SMEM e ssctl) |
| GLINK `modem` | `remote-pid 1`, APCS bit 12, SPI 449; canali `IPCRTR` (QRTR, cioè tutto il QMI), **`DS`** (glinkpkt `at_mdm0`: comandi AT), `DATA40_CNTL`, `DATA1`/`4`/`11` (canali dati vecchi, non usati con l'IPA), `fastrpcglink-apps-dsp`, `glink_ssr` | FDT `qcom,glink`, `qcom,glinkpkt` |
| SMP2P | `qcom,smp2p-modem@1799000c`, item 435/428, SPI 451; voci `master-kernel`/`slave-kernel`, **`ipa-1` in/out**, `wlan-1` in | FDT |
| rmtfs | `qcom,rmtfs_sharedmem`: 3 MiB, `client-id 1`, `guard-memory` | FDT |
| EFS del modem | `modemst1`, `modemst2`, `fsg`, `fsc` (con `persist`): contengono **IMEI**, calibrazione RF e NV. Copia di sicurezza in `phone/<seriale>/backup/` | `info/partitions.txt` |
| Firmware | partizione `modem` (NON-HLOS); versione `c3-46.0926.1-0531_2124_8bef12b` (`gsm.version.baseband`). Le configurazioni operatore (MBN di `modem_pr/mcfg`) le legge il modem stesso via tftp [ipotesi: stesso schema di `wlanmdsp.mbn`] | getprop |
| Android | `qcrild` (RIL), `ipacm`, `dataqti`/`dataadpl`, `imsqmidaemon`/`imsdatadaemon`/`ims_rtp_daemon` (IMS nel modem con aiuti dal processore principale), `mdm_helper` (avvio X50), `loc_launcher`/`gnss_service`, `ro.baseband = mdm`, `persist.radio.multisim.config = ssss` (una SIM), `persist.radio.calls.on.ims = 1` | getprop |
| SIM | al momento della lettura nessuna SIM (`gsm.sim.state = ABSENT`): per le prove ne serve una | getprop |

Lo stesso MPSS SM8150 si avvia già sotto XNU in due progetti: nel kernel con `vayu` (POCO X3 Pro) e dallo spazio
utente con ios-flame (Pixel 4, `wland up`, fino a WLFW). Le lezioni di ios-flame valgono uguali da noi: item SMP2P 428
creato **prima** dell'avvio; senza rmtfs il modem va in err-fatal dopo circa 40 s; i pacchetti di controllo QRTR sono
sempre di 20 byte; un solo buffer rmtfs per accensione, altrimenti `hyp_assign` si blocca in TrustZone.

### 1.2 Il modem 5G esterno: SDX50M ("X50") su PCIe

| Cosa | Valore | Da dove |
|---|---|---|
| Nodo | `qcom,mdm3`, `compatible = "qcom,ext-sdx50m"`, `mdm-link-info = "0305_01.01.00"`, `sysmon-id 0x14`, `ssctl-instance-id 0x10`, `esoc-skip-restart-for-mdm-crash`; il `model` del device tree è "SDX50M ANDROMEDA" | FDT; `sm8150-sdx50m.dtsi` di Xiaomi |
| GPIO di controllo | MDM2AP errfatal **53**, AP2MDM errfatal **141**, MDM2AP status **142**, AP2MDM status **135**, soft reset **GPIO 9 del PM8150L** | FDT `qcom,mdm3` |
| PCIe | `pcie1` `qcom,pcie@1c08000` (PARF `0x1C08000`, PHY `0x1C0E000`, DBI/BAR da `0x40000000` a `0x5FFFFFFF`), PERST **GPIO 102**, WAKE **GPIO 104**, SMMU SID base `0x1E00`, PHY ver `0x83d`; `pcie0` è `disabled` | FDT |
| MHI | `mhi_0` (`mhi,name = "esoc0"`), ID PCI `17cb:0305`–`0308`, regione `mhi_region` da 16 MiB (presa da `0xA3C00000`), finestra `0xA0000000`–`0xA4BFFFFF`. Canali: SAHARA 2/3, DIAG 4/5, EFS 10/11, QMI0/QMI1 14–17, IP_CTRL 18/19, **IPCR 20/21** (QRTR su MHI), DUN 32/33, IP_HW0 100/101 (spento nel dtsi Xiaomi), **IP_HW_MHIP_1 107/108** (offload IPA), RMNET_CTL 109/110 | FDT; `sm8150-sdx50m.dtsi` |
| Firmware | `0x305 → sdx50m/sbl1.mbn` via BHI, il resto via Sahara (`mdm_helper`/ks in Android). EFS dell'X50 nelle partizioni **`mdm1m9kefs1/2/3/c`**, che **non sono nel backup** | `drivers/bus/mhi/controllers/mhi_qcom.c` di Xiaomi; `info/partitions.txt` |
| IPA | `qcom,ipa-mhi-proxy` con doorbell nel BAR PCIe1 (`0x40300300`): i dati NR passano dall'IPA dell'SM8150 [ipotesi sul ruolo esatto in EN-DC] | FDT |

L'X50 fa solo NR in modalità NSA (EN-DC): l'ancora LTE resta il modem integrato. `poff-depends-on` riguarda l'ordine
di **spegnimento**. Per il 4G l'X50 dovrebbe poter restare spento [ipotesi: il firmware MPSS di questo telefono potrebbe
lamentarsi se non lo trova. Si vede in W1/C1].

### 1.3 IPA (motore dei pacchetti) e dati

| Cosa | Valore | Da dove |
|---|---|---|
| Nodo | `qcom,ipa@1e00000`: `ipa-base 0x1E00000` (`0x34000`), `gsi-base 0x1E04000` (`0x28000`), IRQ SPI 311 (IPA) e 432 (GSI), `qcom,ee = 0` | FDT |
| Versione | `qcom,ipa-hw-ver = 0x0f` = **`IPA_HW_v4_1`** (enum di `msm_ipa.h`: v4.0 = 14, v4.1 = 15, v4.2 = 16) | FDT + `include/uapi/linux/msm_ipa.h` Xiaomi |
| SMMU | contesti apps SMMU: AP **SID `0x520`** (IOVA `0x20000000`, 1 GiB), WLAN `0x521`, uC `0x522`; `qcom,arm-smmu`, `smmu-fast-map` | FDT |
| Firmware GSI | `qcom,ipa_fws`: **PAS id `0xf`**, `pil_ipa_fw_region 0x98F00000` (64 KiB), `pil_ipa_gsi_region 0x98F10000` (20 KiB) | FDT |
| Modem | `modem-cfg-emb-pipe-flt`, SMP2P `ipa-1` (il modem aspetta "setup ready"), `qcom,rmnet-ipa3` | FDT |

Su Linux mainline l'IPA v4.1 non c'è: ci sono v3.1, 3.5.1, 4.2, 4.5, 4.7, 4.9, 4.11, 5.0, 5.2 e 5.5. C'è però un ramo di
**Andrew Gigena** (`1aba6cd68608`, agosto 2025, GPL-2.0) con `ipa_data-v4.1.c`, `ipa_reg-v4.1.c` e
`qcom,sm8150-ipa`, entrato nel kernel postmarketOS per SM8150. Sotto XNU esistono l'IPA v4.2 di QcomBaseband (SC7180,
BSD-2) e l'IPA 4.11 di HakiOS (SM7325, APSL). Il v4.1 va scritto, partendo da quelle differenze di registri.
HakiOS segnala anche che l'hypervisor dà **un registro di match SMMU per stream**: va tenuto presente per i SID
`0x520`/`0x521`.

### 1.4 Servizi QMI necessari

Numeri da libqmi (`qmi-enums.h`). I client iOS sono nomi trovati nella dyld shared cache di 20H392 [visto].

| Servizio | ID | A cosa serve | Client iOS (20H392) | Android |
|---|---|---|---|---|
| CTL | `0x00` | allocazione dei client QMUX | `qmi::ClientRouter`, `QMuxClientIface` | QMUX/QRTR |
| WDS | `0x01` | sessione dati, indirizzi | `QMIDataCommandDriver`, `Mav16QMIDataContextIP` | dataqti |
| DMS | `0x02` | identità (IMEI **solo in lettura**), modo operativo (bassa potenza/online) | `QMIDMSCommandDriver` | qcrild |
| NAS | `0x03` | registrazione, segnale, reti | `QMINetworkRegistrationDriver`, `QMINasClient` | qcrild |
| QOS | `0x04` | flussi dati | `QMIQOSClient` | dataqti |
| WMS | `0x05` | **SMS** | `QMISMSCommandDriver` (namespace `qmi::wms`) | qcrild |
| PDS | `0x06` | GNSS "vecchio stile": **quello che usa `locationd` dell'iPhone X** | `CLQmiPdsManager` (locationd) | — |
| VOICE | `0x09` | chiamate, servizi supplementari | `EurekaCallCommandDriver`, `QMISuppServicesCommandDriver` | qcrild |
| UIM | `0x0B` | SIM: stato, ICCID/IMSI, APDU, eSIM | via QMUX; eSIM con il servizio Apple VINYL | qcrild |
| LOC | `0x10` | GNSS moderno: **quello che offre il firmware Android** | — | loc_launcher, gnss_service |
| SAR | `0x11` | stato SAR (potenza di trasmissione vicino al corpo) | non cercato | qcrild |
| IMS / IMSP / IMSA | `0x12` / `0x1F` / `0x21` | IMS nel modem (VoLTE "alla Android") | — (iOS fa IMS da sé) | imsqmidaemon |
| WDA | `0x1A` | formato dati (raw IP in QMAP) | via ponte | dataqti |
| PDC | `0x24` | configurazioni operatore (MBN) | `QMI.pdc` | qcrild |
| DSD | `0x2A` | scelta del sistema dati | `QMI.dsd` | dataqti |
| DPM | `0x2F` | porta dati hardware dell'IPA | via ponte | dataqti |
| IMSDCM | `0x302` | il modem chiede al processore principale la PDN `ims` | — | imsdatadaemon / 81voltd su Linux |
| Apple privati | `0xE1`–`0xEA`, `0x52` | audio, BSP, AWD, VINYL (eSIM), **mavims** (IMS), ELQM, Stewie, `ms` | `QMIBasebandSettingsDriver`, `QMIRTPCommandDriver`… | **non esistono** nel firmware Qualcomm/Xiaomi [ipotesi forte] |

BaseTrace (seemoo-lab) ha osservato che l'X55 dell'iPhone 12 non espone né LOC né i servizi IMS standard, e che iOS
usa PDS. Il firmware Apple e quello Android dello stesso chip offrono insiemi di servizi diversi. Il ponte deve
tradurre, o rispondere "non supportato", dove non coincidono.

### 1.5 Audio delle chiamate

In Android la voce non passa dal processore principale. Il vocoder del modem e la sessione voce CVD dell'ADSP
(`qcom,lpass@17300000`, `pas-id 1`, GLINK `lpass` canale `apr_audio_svc`) si parlano direttamente. Il percorso
continua sulle porte AFE SLIMbus fino al codec WCD934x e all'amplificatore (TAS2557). Il kernel Xiaomi ha
`dsp/q6voice.c` (MVM/CVS/CVP) e `asoc/msm-pcm-host-voice-v2.c` ("host PCM": il processore principale legge e scrive i
campioni della chiamata). Su Linux il pezzo equivalente è `q6voice` di Stephan Gerhold, fuori dall'albero principale e
assente dal kernel SM8150 di postmarketOS, più `q6voiced` (MIT) per aprire il percorso.

L'iPhone X fa diversamente. Il device tree `d22ap` ha `/arm-io/mca3/mca3a/audio-baseband-voice`
(`compatible = "audio-data,baseband-voice"`) e iOS ha il plug-in HAL `BasebandVoice.driver` [visto]: la voce arriva al
processore principale su I2S, e la elabora CoreAudio. Le strade sono due. La prima è una voce "hostless" tenuta tutta
nell'ADSP, con instradamento (altoparlante, capsula, BT) comandato dal nostro driver. La seconda passa dall'host PCM
verso `BasebandVoice`. Entrambe richiedono prima l'audio di base (ROADMAP, voce 10).

### 1.6 GNSS

Nessun nodo GNSS nel device tree del processore principale: motore, LNA e RF sono gestiti dal modem. Android usa QMI
LOC tramite `loc_launcher` e il HAL `gnss_service`. Il pd-mapper di Linux per `qcom,sm8150` elenca il servizio
`gps/gps_service` nel dominio radice del modem (`msm/modem/root_pd`). Il nostro pd-mapper lo prende già dai `.jsn`
della partizione `modem` (W2). Senza dati di assistenza il primo fix richiede 12–15 minuti all'aperto (wiki
postmarketOS). Con XTRA (orbite previste, scaricate da Internet e iniettate via LOC) scende molto. Su Qualcomm il GNSS
lavora anche con il modem in bassa potenza (senza trasmettere) [ipotesi, è il piano di iOSPlus10].

### 1.7 NFC

| Cosa | Valore | Da dove |
|---|---|---|
| Chip | `qcom,nq-nci` @ `0x28`. Famiglia NXP NQ: `nq-nci.h` elenca NQ310 (`0x40`), NQ330 (`0x51`), SN100. Android ha anche `secureelement-hal 1.2` e `qti_esepowermanager`, quindi c'è un **eSE** (con ogni probabilità NQ330 = PN553 + elemento sicuro; da leggere con CORE_INIT) | FDT, `nq-nci.h` Xiaomi, getprop |
| Bus | SE9 I2C `0xA84000` (`status ok`, SPI 354) | FDT |
| GPIO | IRQ **47**, VEN **36**, FIRM **48**, CLKREQ **113**. Il sorgente Xiaomi ha VEN **41**: vale il device tree attivo | FDT; `xiaomi-sdx50-sm8150-common.dtsi` |
| Linux | `nxp-nci` (I2C, `nxp,pn553` nel binding). È usato su Xperia kumano (SM8150, stessi IRQ 47 e FIRM 48) e su OnePlus 6/6T. Solo NCI: l'eSE non è esposto | mainline |

---

## 2. Lavoro già fatto da altri

### 2.1 Sotto XNU / iOS

| Progetto | Dispositivo | Cosa funziona | Prova | Licenza | Per noi |
|---|---|---|---|---|---|
| [`UniXNU/QcomBaseband`](https://github.com/UniXNU/QcomBaseband) `dd0199cf`, di **HoshinoYumeka ([@chiba233](https://github.com/chiba233))**, estratto da [`chiba233/Trogdor-ipadOS`](https://github.com/chiba233/Trogdor-ipadOS) | Chromebook SC7180 (Coachz), iPadOS 17.7.11 (21H461), ramo Apple **Mav16** (`J121AP`) | IMEI/ICCID in Impostazioni, registrazione LTE (in roaming), operatore e tacche, **dati IPv4** su `pdp_ip0` anche dopo riconnessioni. IPv6, APN e sessioni lunghe in sospeso. Non dichiarati SMS, chiamate e GPS (un iPad non telefona) | codice + log nel README | BSD-2-Clause | **il modello principale**: classi `AppleBaseband`/`AppleBasebandPCIControl`/`AppleBasebandPCIInterface` (18 canali, `ABPInterfaceProtocol`), ponte QMUX↔QRTR, endpoint SAHARA che **consuma e scarta** il firmware Apple, risposte ai servizi privati BSP/VINYL, PDP `pdp_ip0..3`, IPA v4.2/GSI/QMAP, copia patchata di CommCenter per il "ticket di posticipo" |
| [`tycykp/UniXNU-OnePlus-Ace5`](https://github.com/tycykp/UniXNU-OnePlus-Ace5) `aaa100d1` | SM8650, **la nostra stessa userland 20H392 D22** | Analisi: D22 → `mav17` → vendor 1 → `MCommandDriversFactory`/`QMICommandDriverFactory`, cioè **QMI e non ARI**; tabelle di dispatch del kernelcache originale (`AppleBasebandUserClient` di D20: 44 selettori, `AppleBasebandPCIUserClient` 13, `…PCIControlUserClient` 4); una sola SIM (`expectedSimCount = 1`). Nulla provato sull'hardware | codice + documenti | BSD-2-Clause-Patent | la mappa dell'ABI di iOS 16 che QcomBaseband (iOS 17) non copre |
| [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) `63fc2b46` (HakiOS, OOSEX GANG) | POCO X5 Pro (SM7325), iOS 16.5 | Modem avviato (~88 servizi QMI), rmtfs in sola lettura, IPA attivo, SIM, segnale, sessione WDS. I dati sono un'**interfaccia Ethernet `en1` con DHCP finto nel kernel**, senza CommCenter. Le tacche sono iniettate in SpringBoard. Mancano chiamate, SMS e IPv6 | codice + `docs/cellular.md` | APSL 2.0 + MIT | la scorciatoia "dati senza CommCenter" (fase C2) e l'ordine DPM → WDA → WDS. Nessun HakiOS ha un "QcomBaseband" |
| [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) `3e7be137` | POCO X3 Pro (**SM8150**, senza X50) | **Registrazione LTE sull'hardware**; "Mobile data (IPA) hangs the SoC". `vayu_cell.c` in sola lettura (UIM, NAS; la registrazione solo con `vayu_cell_register=1`). `vayu_nfc.c`: PN553 via NCI, scoperta dei tag non funzionante; FIRM mai alzato | README + codice | **nessuna** | stesso SoC: conferma che il nostro MPSS registra sotto XNU. Solo come riferimento, il codice non si copia |
| [`akshaynexus/iOSPlus10`](https://github.com/akshaynexus/iOSPlus10) (commit della posizione del 27/10/2026) | SM8450, **20H392 D22** | **Posizione simulata vista funzionare** (`CLSimulationManager` via `com.apple.locationd.simulation`, interruttore principale acceso). Il server di posizionamento Wi-Fi di Apple risponde anche a un telefono non attivato. Client QMI LOC nel kernel solo compilato | codice + log | MIT per `docs/`, `scripts/`, `tools/` (il codice della posizione è da verificare) | **il piano GPS** (G0–G1) |
| [`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame) `745c4341` | Pixel 4 (**SM8150**) | Modem avviato dallo spazio utente (`wland`), WLFW e Wi-Fi funzionanti. Cellulare e GPS "not working yet" | `HANDOFF.md` | MIT | lezioni di avvio del modem SM8150 (sezione 1.1) |
| Fork di ios-sweet (topaz, sapphire, renoir, mh2lm, cepheus…) e catalogo Portsmith | vari | nessuno dichiara SMS, chiamate, GPS o NFC | README | varie | — |

### 2.2 Linux (protocolli e stato su SM8150)

| Riferimento | Stato | Licenza |
|---|---|---|
| MPSS SM8150 in mainline (`qcom,sm8150-mpss-pas`, Sibi Sankar, `49076351a263`) | avvio del modem: c'è | GPL-2.0 (solo documentazione) |
| IPA v4.1, ramo `andrew/6.16-ipa` di Andrew Gigena, ora nel kernel postmarketOS SM8150 | dati "partial" su raphael; la pagina SoC SM8150 dice "can send/receive SMS (replies may be delayed)", dati e GPS "WIP" | GPL-2.0 (solo documentazione) |
| MHI `pci_generic.c` | **nessun ID `0x0305`** (SDX50M). Nessun X50 funzionante su mainline. Anche l'X55 del Pad 5 Pro 5G (enuma) si ferma all'accensione MHI (manca ESOC) | — |
| ModemManager ≥ 1.18 + libqmi ≥ 1.26 su QRTR (Eric Caruso, Aleksander Morgado e altri); `qrtr`, `rmtfs`, `tqftpserv`, `pd-mapper` di linux-msm (Bjorn Andersson, Linaro) | lo stack di riferimento (SDM845: OnePlus 6T con SMS e dati "works", chiamate "partial") | LGPL-2.1+/GPL-2.0+; **BSD-3** per linux-msm |
| GPS: ModemManager + QMI LOC + geoclue (wiki "GPS" di postmarketOS); MR ModemManager !1409 | funziona su SDM845 | GPL-2.0+ |
| VoLTE: [`81voltd`](https://gitlab.nura.eco/modem/81voltd) di Richard Acayan (servizio IMSDCM `0x302` sul processore principale, IMS dentro il modem) | "sperimentale", solo su alcuni SDM845 e alcuni operatori | GPL-2.0+ |
| Audio della voce: `q6voice` (Stephan Gerhold, fuori albero), `q6voiced` (MIT), `callaudiod` (Mobian, GPL-3.0+) | SDM845 e msm8916 | vedi colonna |
| NFC: `nxp-nci` + binding PN553 (Danila Tikhonov `05c9afb9bfa3`), nodi su kumano (Konrad Dybcio `6cef82a8a7d9`) e OnePlus 6 (David Heidelberg `50fc93244973`) | lettura tag con neard | GPL-2.0 |

---

## 3. Il lato iOS (20H392, `D22` = iPhone10,3, baseband Qualcomm "Mav17")

### 3.1 iOS sceglie lo stack Qualcomm (QMI), non quello Intel (ARI)

- `BuildManifest.plist` [visto]: `d22ap` → `Firmware/Mav17-9.00.02.Release.bbfw` (BbChipID `0x3B0E1`); `d221ap`
  (iPhone10,6, A1901) → `ICE17-6.01.02` (Intel XMM7480). Il nostro device tree si presenta come `D22AP`, quindi siamo sul
  ramo Qualcomm (MDM9655, X16, secondo il teardown iFixit dell'A1865).
- L'ADT originale `d22ap` [visto] ha `/baseband` (`compatible = "baseband,n41"`, con `device-imei` e `imeisv` riempiti
  da iBoot), `/arm-io/apcie/pci-bridge2/baseband-pcie`, `/product/baseband-chipset = "mav17"` e `gps-capable = 1`.
  **Non ha un nodo GPS.** Il nostro `andromeda.adt` conserva `/product` ma non ha `/baseband`.
- Nella dyld shared cache [visto] ci sono `libBasebandCommandDriversQMI.dylib` e `libQMIParserDynamic.dylib`, decine di
  `QMI…CommandDriver` (rete, SMS, dati, voce "Eureka", servizi supplementari, audio di chiamata, IMS RTP, Stewie)
  e varianti per generazione (`Mav13`, `Mav16`, `Mav20`). C'è anche `libIOACIPCBB.dylib` (Apple Converged IPC, il
  trasporto Intel). La scelta tra i due la fa il codice, non la presenza della libreria: per D22 è QMI (analisi Ace5;
  ARIstoteles: "Only Intel basebands support ARI, Qualcomm basebands use QMI").

### 3.2 Cosa si aspetta CommCenter

- **launchd** [visto]: `com.apple.CommCenter.plist` ha `_LimitLoadToDeviceTree = baseband`; la variante
  `CommCenter-noncellular.plist`, con lo stesso `Label`, ha `_LimitLoadFromDeviceTree = baseband`. Il nodo `/baseband`
  sceglie quale delle due parte. Oggi `scripts/sweet-root-tweaks.py` le toglie entrambe (crash-loop e blocco di Setup).
  Per il cellulare va tenuta la variante cellular, con `/baseband` nell'ADT.
- **Kernel** (Ace5, dal kernelcache originale): `AppleBasebandD20` (provider `baseband,n41`), `AppleBasebandPCIMAVControl`,
  `AppleBasebandPCI`, `AppleBasebandPCIInterface` (uno per canale, selezionato da `ABPInterfaceProtocol`),
  `AppleBasebandPCIPDPQMAPSkywalk` e `AppleBasebandPCIQMAPControl` (dati QMAP), `AppleBasebandPCIMAVPDPManager`. Nel
  nostro XNU nessuna di queste esiste: vanno **finte**, con gli stessi nomi, selettori e forme delle completion
  asincrone. QcomBaseband lo fa per iOS 17. Per iOS 16 le tabelle sono quelle di Ace5.
- **Canali** di `libPCITransport`: `pbl`, `sahara`, `diag`, `refs`, `qmux1`, `qmux2`, `rom`, `boot`, `ari`, `trace`,
  `at`, `dpl`, `mediactl`, `vcom`, `gnss`, `qdss`, `virtual_timesync`, `boot_logger`. Le cose vere passano da `qmux1`
  e `qmux2`, dove i frame QMUX di Apple vanno tradotti in QRTR (allocazione dei client CTL, transazioni, servizi). Dopo
  arrivano `pbl`/`sahara`, l'avvio.
- **Avvio** (ABM/BBUpdater): BBUpdater vuole caricare `Mav17.Release.bbfw` (nella root, in `/usr/standalone/update/`)
  via BHI + Sahara, con personalizzazione Apple. Il ponte deve **accettare e buttare via** quelle immagini, come fa
  QcomBaseband, mentre il modem Xiaomi gira col suo firmware. **Mai** un byte del firmware Apple verso l'MPSS o verso
  una partizione.
- **Servizi privati Apple**: BSP (`0xE2`: chip ID, seriale, attivazione, file PRI/GRI del carrier bundle che vanno
  confermati e scartati, altrimenti CommCenter resetta il baseband), VINYL (`0xE5`, eSIM). Tutto il resto risponde
  `QMI_ERR_NOT_SUPPORTED`.
- **Coda e tempi**: all'avvio si registrano 53 client. Con code corte si perdono risposte (VOICE persa = reset del
  baseband per `ATCS_TIMEOUT`, secondo QcomBaseband).
- **Attivazione**: in iPadOS 17 CommCenter blocca il contesto Internet senza un "postponement ticket" di Apple, e
  QcomBaseband patcha un ramo. Se valga anche per 20H392 con il nostro ramo `Hactivation` non si sa [da verificare].

### 3.3 IMS e VoLTE in iOS

Sugli iPhone lo stack IMS gira sul processore principale: `libIPTelephony` ("Bambi", con `BambiCall`, `IMSSMSManager`),
SIP su `pdp_ip1:5060` (Mehta e Pathak, HITB 2023). Il modem dà i servizi Apple per media e trasporto (`mavims`, `ms`,
`rtp`, `audio`). Il firmware Xiaomi non li ha [ipotesi forte; l'elenco dei servizi in W2 lo conferma o smentisce].
Il VoLTE "nativo" di iOS quindi **non è realistico**. L'alternativa è quella di Android e di 81voltd: IMS dentro il modem
(PDC con l'MBN dell'operatore, PDN `ims` aperta su richiesta IMSDCM), con la chiamata vista da CommCenter come una
normale chiamata VOICE. Non è mai stato provato, e CommCenter potrebbe provare a spegnere l'IMS del modem
(`QMINetworkRegistrationDriver::enableIms`) [ipotesi].

### 3.4 GPS: `locationd` e `gpsd`

- Su D22 [visto] `locationd` contiene `CLEurekaController`, `CLEurekaModemController`, `CLQmiPdsManager`,
  `CLQmiPdsMessenger`, `CLEurekaSupl`: il GNSS arriva **dal baseband, in QMI PDS**, sul trasporto `qmux`. Le maschere
  di log `/usr/lib/bbmasks/D22_GPS.dmc` e `D22_SUPL.dmc` (formato DIAG Qualcomm) vanno nella stessa direzione.
- `gpsd` [visto] è il demone per i chip GNSS separati (librerie `libfire*` e `libolaf`, `/dev/cu.gps` su UART,
  `AppleSPUGNSSDriverUserClient`, `libPCITransport`). Su D22 non dovrebbe servire [ipotesi: le stringhe
  `isMav21andNewerGnss` indicano che il canale `gnss` serve ai baseband più nuovi].
- Strade per noi: (a) **simulazione**: un nostro client QMI LOC passa i fix a `CLSimulationManager`
  (`appendSimulatedLocation:`). Le posizioni risultano `isSimulatedBySoftware`, la direzione di marcia si perde.
  iOSPlus10 l'ha vista funzionare sulla stessa userland. (b) **nativa**: ponte PDS↔LOC dentro QcomBaseband, molto più
  lavoro. Serve solo se il firmware non ha PDS. (c) **Wi-Fi**: una volta che il Wi-Fi funziona, il posizionamento Wi-Fi
  di Apple (`gs-loc.apple.com`) dà una posizione approssimata anche senza GNSS e senza attivazione (iOSPlus10;
  Rye e Levin, IEEE S&P 2024).

### 3.5 NFC: `nfcd`

[visto] `nfcd` → `NFDriver*` (`NFDriverSecureElementTransceive`, `NFDriverRFSettingsPushSignedSetting`…) → kext
`AppleStockholmControl` / `AppleStockholmControlUserClient` / `AppleStockholmSPMI`, più `attemptHammerfestRecovery`
e `com.apple.nfrestore`. Nell'ADT `d22ap` il controller è `/arm-io/uart3/stockholm` (`compatible = "nfc,primary,gpio"`,
GPIO wake, enable, `download_request`). È un NXP PN80V (iFixit) su UART, con firmware Apple, e l'elemento sicuro con il
firmware `SE/Stockholm4.RELEASE.sefw` personalizzato da Apple al restore. Per farlo funzionare servirebbero l'ABI
chiusa del kext, le estensioni NCI di Apple e i controlli di firmware. In cambio avremmo solo la lettura NDEF da app
CoreNFC: su iPhone X non c'è nemmeno la lettura in background, che parte da iPhone XS. Apple Pay è escluso: elemento
sicuro e Secure Enclave sono accoppiati in fabbrica (Apple Platform Security).

### 3.6 Rischi lato iOS da chiudere prima di accendere qualcosa

1. **BBUpdater** prova a scrivere firmware nel baseband: il ponte deve fare da pozzo (sezione 3.2).
2. **nfrestore** aggiorna il controller NFC: se mai si esponesse il chip, FIRM (GPIO 48) resta basso e nessun comando
   di download parte, come in `vayu_nfc.c`.
3. **Attivazione**: CommCenter con SIM può parlare con i server di attivazione Apple, mandando IMEI e ICCID di un
   telefono non Apple. Va tenuto offline o bloccato [da verificare cosa tenta 20H392].
4. **IMEI nell'ADT**: `device-imei` resta a zero. CommCenter lo legge dal modem (DMS), in sola lettura.

---

## 4. Conclusione per funzione

Difficoltà: B bassa, M media, A alta, MA molto alta. "Gratis" = già nelle fasi W0–W2 del Wi-Fi.

### 4.1 Dati mobili (4G) — molto difficile, realistico

- **Catena minima**: W0–W2 (gratis) → DMS/UIM/NAS: SIM pronta, modo online, registrazione → **IPA v4.1**: firmware GSI
  (PAS `0xf`), SMMU `0x520`, endpoint, handshake INIT_DRIVER con il modem, voce SMP2P `ipa` → DPM Open Port → WDA
  (raw IP/QMAP) → WDS (bind mux, start) → QMAP mux 1 → interfaccia di rete.
- **Due strade per iOS**: (A) **scorciatoia HakiOS**: `en1` Ethernet con DHCP finto, senza CommCenter. Internet
  funziona, ma niente operatore in Impostazioni né SMS. (B) **ponte CommCenter** (QcomBaseband adattato a 20H392 con
  le tabelle Ace5): `pdp_ip0..3`, Impostazioni → Cellulare, tacche vere, ed è la base per SMS e voce.
- **Rischio principale**: l'IPA. Su `vayu`, stesso SoC, blocca il SoC; nessuno ha il v4.1 sotto XNU. Poi il ticket di
  attivazione di CommCenter e le differenze di ABI tra iOS 16 e 17.

### 4.2 SMS — molto difficile, ma incrementale

- **Catena minima**: W0–W2 + ponte CommCenter (C3) + WMS passante. **Niente IPA.** Gli SMS su IMS (SMSoIP) richiedono
  l'IMS di iOS e restano fuori. Vanno quelli su NAS/SGs, che molte reti LTE usano ancora.
- Non provato da nessuno sotto XNU. Su Linux SM8150 funziona, con risposte in ritardo.

### 4.3 Telefonate — molto difficile (2G/CSFB), non realistico (VoLTE iOS)

- **Catena minima**: ponte CommCenter + VOICE + **audio completo** (ADSP PAS 1, APR, sessione CVD, AFE SLIMbus,
  WCD934x, capsula e altoparlante, poi HFP per il Bluetooth) + accordo con `BasebandVoice`/CoreAudio sul percorso. Dove
  2G e 3G sono spenti serve in più il VoLTE: IMS del modem + IMSDCM (come 81voltd) + PDC. Sperimentale anche su Linux.
- Le **chiamate d'emergenza** devono funzionare per forza, se si dichiarano chiamate: finché non sono provate, il port
  va detto "non adatto a telefonare".

### 4.4 5G (X50) — non realistico

PCIe RC con la sequenza PHY del device tree, MHI, ESOC (GPIO 53/135/141/142, reset sul PM8150L), Sahara con
`sdx50m/sbl1.mbn`, EFS `mdm1m9kefs*` e offload IPA↔MHI (MHIP). Nessun precedente, nemmeno su Linux. L'X50 va
lasciato spento e il 4G va verificato senza di lui.

### 4.5 GPS — fattibile (via simulazione)

- **Catena minima**: W0–W2 (gratis) → modem in **bassa potenza** (nessuna trasmissione, nessuna SIM) → client QMI LOC
  (registrazione eventi, avvio sessione, fix) → demone che chiama `CLSimulationManager` → app. Con il Wi-Fi: XTRA
  iniettato via LOC per un primo fix rapido, e posizionamento Wi-Fi di Apple.
- Prima va verificato nell'elenco dei servizi di W2 se c'è LOC (atteso) e se c'è PDS (non atteso).

### 4.6 NFC — non realistico per iOS

Un probe NCI in sola lettura (CORE_RESET/CORE_INIT, per sapere se è NQ310 o NQ330) è facile. Non porta niente a iOS.
Non è in programma.

### 4.7 Cosa viene dal lavoro Wi-Fi, e cosa no

| Pezzo | Wi-Fi | Cellulare | GPS |
|---|---|---|---|
| W1: PAS 4, AOP `load_state`, voti `mss`/`cx`, SMP2P 428/435, arresto pulito | sì | sì | sì |
| W2: GLINK `IPCRTR`, QRTR, rmtfs in RAM, pd-mapper (anche `gps/gps_service`), tftp | sì | sì (+ file `modem_pr/mcfg` via tftp [ipotesi]) | sì |
| Gestione dei riavvii del modem (SSR, topaz 0124) | sì | sì: un crash del modem spegne anche il Wi-Fi | sì |
| Client QMI generico (encode/decode TLV) | WLFW | UIM/NAS/WDS/WMS/VOICE | LOC |
| MSA, SMMU `0x640`, copy engine | sì | no | no |
| IPA v4.1, SMMU `0x520` | no | **sì** (solo dati) | no |
| Ponte CommCenter (QcomBaseband) | no | **sì** | solo per la via nativa |
| Audio ADSP/CVD | no | **sì** (voce) | no |
| `en1`/configd/IONetworkStack (N1 del Wi-Fi) | sì | sì per la strada A | no |

---

## 5. Piano a fasi (proposta, nessun codice scritto)

Come per il Wi-Fi: ogni driver parte spento con un suo boot-arg. Prima la sola lettura, e niente che trasmetta senza
un boot-arg esplicito. Rmtfs sempre in RAM: le scritture del modem non arrivano mai a `modemst1/2`, `fsg`, `fsc`.

| Fase | Obiettivo | Fatto quando | Diff. | Da chi |
|---|---|---|---|---|
| C0 | Dopo W2: elenco dei servizi QRTR del modem con le versioni; DMS Get Operating Mode (deve essere bassa potenza); UIM Card Status | rapporto: c'è LOC? PDS? VOICE, WMS, IMSA/IMSP, DPM, WDA, PDC? Il modem resta su senza X50? | B | `vayu_cell.c` (lettura), ios-flame |
| G0 | Client LOC in bassa potenza: fix NMEA o posizione nel log, all'aperto | latitudine e longitudine (arrotondate nel log) entro 15 min | M | libqmi `qmi-service-loc.json`, iOSPlus10 `location-plan.md` |
| G1 | Iniezione in `locationd` via `com.apple.locationd.simulation`; XTRA quando c'è il Wi-Fi | Mappe mostra il punto blu | M | iOSPlus10, `locsim` (udevsharold) come metodo |
| C1 | Con SIM e boot-arg esplicito: modo online, registrazione LTE | NAS "registered", PLMN, RSRP nel log; identificativi mascherati | M | `vayu_cell.c`, libqmi |
| C2 | IPA v4.1 + DPM/WDA/WDS + `en1` (strada A) | `ping` e Safari via rete mobile | **MA** | QcomBaseband `ipa/` (v4.2), HakiOS `hakios.ipa.c`/`hakios.cell.c`, ramo IPA v4.1 di Andrew Gigena (solo documentazione) |
| C3 | Ponte CommCenter per 20H392: classi AppleBaseband*, 18 canali, SAHARA a pozzo, QMUX↔QRTR, BSP/VINYL, PDP, `/baseband` nell'ADT, job cellular di CommCenter | Impostazioni → Cellulare mostra operatore e tacche; dati su `pdp_ip0` | **MA** | QcomBaseband, tabelle Ace5 |
| C4 | SMS (WMS passante) | un SMS inviato e uno ricevuto in Messaggi | A | QcomBaseband, libqmi WMS |
| C5 | Voce: prima audio ROADMAP 10, poi CVD (stile `q6voice`), poi VOICE; 2G/CSFB prima del VoLTE | chiamata in uscita e in entrata con audio in capsula | **MA** | Linux `q6voice`/`q6voiced`, 81voltd (solo documentazione) |
| N0 | (facoltativa) probe NCI in sola lettura | variante del chip nota | B | `vayu_nfc.c` come metodo, `nq-nci.c` |

---

## 6. Aspetti legali, regolamentari e di sicurezza

- **IMEI: mai modificarlo.** Sta nelle EFS del modem (`modemst*`/`fsg`) e si legge soltanto, con DMS, senza scriverlo in
  log, ADT o repository. 3GPP TS 22.016 vuole che l'IMEI non cambi dopo la produzione. Nel Regno Unito modificarlo è
  reato (Mobile Telephones (Re-programming) Act 2002), e così in India (Telecommunications Act 2023). Per l'Italia non
  abbiamo trovato una norma specifica: per noi è vietato comunque.
- **EFS e partizioni**: rmtfs serve copie in RAM. Nessuna scrittura su `modem`, `modemst1/2`, `fsg`, `fsc`,
  `persist`, `mdm1m9kefs*`. La regola del progetto (CONTRIBUTING §4) le esclude già. Prima di qualsiasi prova
  cellulare va fatto, in Android e con il consenso dell'utente, il backup mancante delle `mdm1m9kefs*`.
- **Radio certificata**: firmware del modem, calibrazione RF e NV restano quelle Xiaomi, caricate e autenticate da
  TrustZone. Bande, potenze e configurazioni RF non si toccano. Le prove radio si fanno solo con una SIM dell'utente e
  con il boot-arg di registrazione esplicito. Per la RED (2014/53/UE) contano le modifiche al software che governa
  la radio: qui quel software resta quello del produttore [ipotesi; non è consulenza legale].
- **SAR**: in Android `persist.radio.sar_sar_sensor = true`, cioè un sensore SAR segnala al modem quando il telefono è
  vicino al corpo (QMI SAR `0x11`). Sotto XNU nessuno manda lo stato SAR. Va verificato quale stato usa il modem di
  default prima di trasmettere a lungo [da verificare].
- **Emergenze**: finché il 112 non è provato, il port non va presentato come telefono.
- **Firmware Apple**: `Mav17.Release.bbfw`, `Stockholm4.RELEASE.sefw` e CommCenter (anche patchato) non entrano nel
  repository né nei workflow (CONTRIBUTING §3). Se servisse un patcher di CommCenter, si distribuisce lo strumento, come
  fanno QcomBaseband e renoir.
- **Licenze**: QcomBaseband (BSD-2) e Ace5 (BSD-2-Clause-Patent) si possono riusare con i crediti; HakiOS è APSL 2.0 +
  MIT; `hakios-sm8150` e ios-topaz non hanno licenza, quindi niente copie; i sorgenti Linux, 81voltd e il ramo IPA v4.1
  (GPL) si usano solo come documentazione; linux-msm (`rmtfs`, `pd-mapper`, `tqftpserv`, `qrtr`) è BSD-3.

---

## 7. Domande aperte

1. Il firmware MPSS di `andromeda` regge senza X50 acceso? (C0)
2. Quali servizi QMI annuncia? In particolare **PDS**, che decide la via nativa del GPS, e IMSA/IMSP per il VoLTE.
3. Lo stato di default del modem dopo l'avvio è bassa potenza? `vayu` deve chiedere esplicitamente l'online.
4. L'IPA v4.1 sotto XNU: perché su `vayu` blocca il SoC (SMMU, clock `RPMH_IPA_CLK`, ordine di avvio)?
5. 20H392 ha lo stesso blocco del "ticket di posticipo" di iPadOS 17? Che cosa manda CommCenter ai server Apple con
   una SIM inserita?
6. Le immagini dell'X50 e le MBN di `modem_pr/mcfg`: dove stanno esattamente? Da verificare in Android, in sola lettura.
7. Stato SAR di default (sezione 6).

## 8. Crediti

Questo documento è quasi tutto lavoro di altri: HoshinoYumeka (@chiba233) per QcomBaseband e Trogdor-ipadOS;
tycykp per l'analisi dell'ABI 20H392 (UniXNU-OnePlus-Ace5); OOSEX GANG per HakiOS; nomorecoolnicknames per
`hakios-sm8150`; akshaynexus per iOSPlus10; Nick Wylynko per ios-flame; seemoo-lab (Tobias Kröll, Jiska Classen,
Lucas Arnold, Matthias Hollick e collaboratori) per ARIstoteles e BaseTrace; Hardik Mehta e Rajanish Pathak per lo
studio sul VoLTE di iOS; Andrew Gigena per l'IPA v4.1; Richard Acayan e Alexander Couzens per 81voltd; Stephan Gerhold
per `q6voice`/`q6voiced`; Bjorn Andersson e Linaro per `qrtr`, `rmtfs`, `pd-mapper`, `tqftpserv`; Eric Caruso,
Aleksander Morgado e i contributori di libqmi/ModemManager; Sibi Sankar, Konrad Dybcio, David Heidelberg, Danila
Tikhonov, Dmitry Baryshkov per il supporto mainline; i contributori del wiki postmarketOS. Quando si riuserà codice
o dati da uno di questi, va aggiunta la voce in [CREDITS.md](../../CREDITS.md) nello stesso cambiamento.

## 9. Fonti

- **Dati del telefono** (fuori da git): `phone/<seriale>/live-fdt.dts` (nodi `qcom,mss@4080000`, `qcom,glink`,
  `qcom,glinkpkt`, `qcom,smp2p-modem@1799000c`, `qcom,rmtfs_sharedmem`, `qcom,mdm3`, `qcom,pcie@1c08000`,
  `qcom,mhi@0`, `mhi_region`, `qcom,ipa@1e00000`, `ipa_smmu_*`, `qcom,ipa_fws`, `qcom,ipa-mhi-proxy`, `nq@28`,
  `i2c@a84000`, `qcom,lpass@17300000`), `info/getprop.txt`, `info/partitions.txt`.
- **Kernel Xiaomi** [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) ramo
  `andromeda-p-oss`, commit `74347f5521fb444936f5986ff0937264307d78fe`: `arch/arm64/boot/dts/qcom/sm8150-sdx50m.dtsi`,
  `xiaomi-sdx50-sm8150-common.dtsi`, `drivers/bus/mhi/controllers/mhi_qcom.c`, `drivers/esoc/esoc-mdm-4x.c`,
  `drivers/nfc/nq-nci.h`, `include/uapi/linux/msm_ipa.h`, `audio-kernel/dsp/q6voice.c`,
  `audio-kernel/asoc/msm-pcm-host-voice-v2.c` (GPL-2.0, solo documentazione).
- **iOS 16.7.16 `20H392`** (letto in locale, mai ridistribuito): `BuildManifest.plist`, `DeviceTree.d22ap`
  (`out/andromeda/tree/scripts/base-d22ap.adt`), `com.apple.CommCenter*.plist`, `com.apple.gpsd.plist`,
  `com.apple.nfcd.plist`, `com.apple.locationd.plist`, nomi di simboli in `CommCenter`, `locationd`, `gpsd`, `nfcd`
  e nella dyld shared cache, `/usr/lib/bbmasks/`, `/usr/standalone/update/`.
- **QcomBaseband**, HoshinoYumeka: [`UniXNU/QcomBaseband`](https://github.com/UniXNU/QcomBaseband)
  `dd0199cf3300764ed52b23f4a47761f91fab21a2` (BSD-2-Clause): `README.md`, `UPSTREAM.json`,
  `platform/coachz/README.md`, `baseband/coachz_bb_channels.h`, `coachz_bb_sahara.h`, `coachz_bb_data.h`,
  `coachz_baseband.cpp`; origine [`chiba233/Trogdor-ipadOS`](https://github.com/chiba233/Trogdor-ipadOS)
  `58b5bc8bfc04482014ff403078435e581e58a5b0` (HEAD al 6/11/2026: `893b312121898409b5246d9e2efa5047d02e4d1f`). Guida
  [UniXNU `docs/porting/baseband.md`](https://unixnu.com/porting/baseband) e `docs/drivers/qcom-baseband.md` (UniXNU
  `10edc1efeeabc8c180b1a3ad9f96b7c8e36a385e`).
- **Ace5**, tycykp: [`tycykp/UniXNU-OnePlus-Ace5`](https://github.com/tycykp/UniXNU-OnePlus-Ace5)
  `aaa100d19f6fb14e5897a7e496edf7838d4d4e9f` (BSD-2-Clause-Patent): `docs/MODEM_APPLE_ABI.md`, `docs/MODEM_BRINGUP.md`.
- **HakiOS**, OOSEX GANG: [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel)
  `63fc2b4633bac3af8e57dadab9f5a8d4cb940438` (APSL 2.0 + MIT): `drivers/modem/hakios.mpss.c`, `hakios.qmi.c`,
  `hakios.nas.c`, `hakios.ipa.c`, `hakios.cell.c`, `docs/cellular.md`, `docs/devices/redwood.md`.
- **HakiOS SM8150**, nomorecoolnicknames: [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150)
  `3e7be137a100cef19f1d7e83346b6e688be697d1` (nessuna licenza): `README.md`, `kernel/bsd/dev/vayu_cell.c`,
  `vayu_nfc.c`, `vayu_modem.c`.
- **iOSPlus10**, akshaynexus: [`akshaynexus/iOSPlus10`](https://github.com/akshaynexus/iOSPlus10) (HEAD
  `146e889430bab755329dde7104cc0fa002f44bfb`; commit della posizione `7f7f158093`, `0ee662639f`, `bceff29b11`,
  `9ff0d2a48d`): `docs/location-plan.md`.
- **ios-flame**, Nick Wylynko: [`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame)
  `745c4341af86089efb0df851fa8a27fb8face624` (MIT): `README.md`, `HANDOFF.md` ("WiFi, part 1").
- **seemoo-lab**: [ARIstoteles](https://github.com/seemoo-lab/aristoteles) (MIT; T. Kröll, S. Kleber, F. Kargl,
  M. Hollick, J. Classen, ESORICS 2021, DOI 10.1007/978-3-030-88418-5_7); [BaseTrace](https://github.com/seemoo-lab/BaseTrace)
  (GPL-3.0; L. Arnold, M. Hollick, J. Classen, RAID 2024, DOI 10.1145/3678890.3678898).
- **VoLTE di iOS**: H. Mehta, R. Pathak, "Hacking into iOS VoLTE implementation", HITB 2023
  ([PDF](https://conference.hitb.org/hitbsecconf2023hkt/materials/D2T1%20-%20Hacking%20into%20iOS%20VOLTE%20implementation%20-%20Hardik%20Mehta%20&%20Rajanish%20Pathak.pdf)).
- **Posizionamento Wi-Fi Apple**: E. Rye, D. Levin, "Surveilling the Masses with Wi-Fi-Based Positioning Systems",
  IEEE S&P 2024 ([arXiv 2405.14975](https://arxiv.org/abs/2405.14975)).
- **Hardware iPhone X**: [teardown iFixit](https://www.ifixit.com/Teardown/iPhone+X+Teardown/98975) (A1865:
  MDM9655, WTR5975, PN80V); [modelli Apple](https://support.apple.com/en-us/108044);
  [Apple Platform Security, Secure Element e Secure Enclave](https://support.apple.com/guide/security/secc1f57e189/web).
- **Linux mainline** (torvalds/linux, master del 6/11/2026): `arch/arm64/boot/dts/qcom/sm8150.dtsi`,
  `drivers/remoteproc/qcom_q6v5_pas.c`, `drivers/net/ipa/`, `drivers/bus/mhi/host/pci_generic.c`, `drivers/gnss/`,
  `drivers/nfc/nxp-nci/`, `Documentation/devicetree/bindings/net/nfc/nxp,nci.yaml`, `drivers/soc/qcom/qcom_pd_mapper.c`.
- **IPA v4.1**, Andrew Gigena: [`sm8150-mainline/linux` ramo `andrew/6.16-ipa`](https://gitlab.com/sm8150-mainline/linux/-/commits/andrew%2F6.16-ipa)
  `1aba6cd68608`; kernel postmarketOS SM8150 [`gitlab.nura.eco/soc/qualcomm-sm8150/linux`](https://gitlab.nura.eco/soc/qualcomm-sm8150/linux).
- **postmarketOS** (dal 2026 si chiama Nura; pagine lette da copie web.archive.org): [Qualcomm Snapdragon 855 (SM8150)](https://wiki.postmarketos.org/wiki/Qualcomm_Snapdragon_855_(SM8150)),
  pagine di raphael, guacamole, hotdogb, flame, vayu, enchilada, fajita, beryllium, enuma e "GPS".
- **libqmi / ModemManager** (Aleksander Morgado e collaboratori): `src/libqmi-glib/qmi-enums.h`, `data/qmi-service-*.json`
  (LGPL-2.1+); ModemManager 1.18 (QRTR + IPA), MR !1409 (QMI LOC).
- **linux-msm**: [`qrtr`](https://github.com/linux-msm/qrtr), [`rmtfs`](https://github.com/linux-msm/rmtfs),
  [`pd-mapper`](https://github.com/linux-msm/pd-mapper), [`tqftpserv`](https://github.com/linux-msm/tqftpserv) (BSD-3).
- **Voce su Linux**: [`81voltd`](https://gitlab.nura.eco/modem/81voltd) (Richard Acayan, GPL-2.0+),
  [`q6voiced`](https://gitlab.nura.eco/nura/q6voiced) (Stephan Gerhold, MIT), `q6voice` in
  [msm8916-mainline/linux](https://github.com/msm8916-mainline/linux) `34810638eaaa`, [`callaudiod`](https://gitlab.com/mobian1/callaudiod).
- **Regole**: 3GPP TS 22.016 (IMEI);
  [UK Mobile Telephones (Re-programming) Act 2002, s.1](https://www.legislation.gov.uk/ukpga/2002/31/section/1);
  direttiva 2014/53/UE (RED).
