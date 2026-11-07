# Crediti

Questo port esiste grazie al lavoro di molte persone e progetti. Qui sotto c'è chi abbiamo usato, per cosa, e dove.
Se manca qualcuno o qualcosa è attribuito male, è un errore da correggere.

## Port e kernel XNU

| Progetto / autore | Cosa usiamo |
|---|---|
| **Apple**, [XNU](https://github.com/apple-oss-distributions/xnu) (`xnu-8796.141.3`, licenza APSL 2.0) | il kernel, compilato dai sorgenti pubblici; per il controllo a telefono acceso (`xnu/andromeda_ctl.c`) l'API delle socket in kernel (`bsd/kern/kpi_socket.c`: `sock_receive` con i dati di controllo), `IP_RECVIF` (`ip_savecontrol` in `bsd/netinet/ip_input.c`) e `TUNABLE_WRITEABLE` (`osfmk/kern/startup.h`, usato dalla patch 0101) |
| [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) (licenza MIT, copia in `licenses/ios-sweet-MIT.txt`) | piattaforma `SWEET` per XNU su Qualcomm: patch del kernel (CPU, GIC, UFS, framebuffer `AppleCLCD`, USB CDC-ECM e log in diretta, log persistente, tasti, HID), strumenti del sistema iOS (`mkiosroot.sh`, `sweet-root-tweaks.py`) e il primo iOS 16 su un telefono Android; la lettura SPMI tramite observer (`sweet_spmi.c`) è la base della sonda `xnu/andromeda_probe.c`; il servizio batteria `SweetBattery` (patch 0062) che la nostra patch 0091 collega al PM8150B; il socket UDP in kernel del log in diretta (patch 0077, `sock_socket`/`sock_send` verso `192.168.7.1`) e la rete USB `ue0`/`192.168.7.2` (patch 0075) sono il modello del ricevitore di comandi `xnu/andromeda_ctl.c` (`andrctl=1`) |
| **Nick Wylynko** ([NWylynko](https://github.com/NWylynko)), [`ios-flame`](https://github.com/NWylynko/ios-flame) (Pixel 4, stesso SoC SM8150; licenza MIT, copia in `licenses/ios-flame-MIT.txt`) | metodo e layout dei registri OSM per portare i core alla frequenza piena (`sweet_cpufreq.c`, patch 0086): base di `xnu/andromeda_cpufreq.c`, a cui abbiamo aggiunto la protezione termica; **driver GPU Adreno 640**: `sweet_a6xx.c` e `sweet_gpu.c` (patch 0095, sequenza CX/GMU/HFI/GX/CP/ZAP, pool in RAM non gestita da XNU, firmware come proprietà ADT `gpufw,<nome>`, finestre dei registri mappate con `ml_io_map_unmappable()` fuori dall'`io_submap` da 8 MiB: lo stesso metodo ora è in `andromeda_io_map()` di `xnu/andromeda_probe.c`, usata da tutti i driver andromeda), il ciclo SMC con ripresa da `sweet_pmem.c` (patch 0094) e le note di `HANDOFF.md`: base di `xnu/andromeda_gpu.c`, `xnu/andromeda_gpu_core.c` e dell'opzione `--gpufw` di `scripts/make-andromeda-adt.py` (file XNU sotto APSL 2.0); il servizio sostitutivo `AppleMobileFileIntegrity` (`SweetAMFI.cpp`, patch 0103 → la nostra 0096) e gli indirizzi `mac-address-*` in `/chosen` (`tools/mkadt.py`), che sbloccano LaunchServices e i fornitori di sfondi di PosterBoard; il piano Wi-Fi in `docs/research/wifi-flame.md` parte dal loro `wland`; la politica dyld per i percorsi `@rpath` (patch 0114, copiata senza modifiche come nostra 0115), che fa partire le app con framework interni |
| [`blacktop/darwin-xnu-build`](https://github.com/blacktop/darwin-xnu-build) | la build di XNU sui runner macOS di GitHub |
| **YangQi0408**, [`UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus) (Xiaomi Mi 9) | **base di questo repository**: driver touch ST-FTS (`xnu/cepheus_touch.c`) e I2C via GPIO, voto RPMh per `ldoc1`, codice SoC/CPU (`cepheus_soc.c`, OSM, TSENS), generatore del device tree Apple (`mkadt.py`, `devices.py`), costruzione del sistema iOS (`rebuild-root.sh`, `prepare-ios-root.py`), payload e immagine di avvio, patch per Aloha/UniXNU/DarwinBoot, workflow del kernel; dal ramo `feat/adreno640`: correzione della cache per indirizzi sopra i 4 GiB, tasti PON, evento Home, spegnimento rapido dello schermo, base per la GPU Adreno 640 (GMU, HFI, SMMU, CP); analisi dello stallo di SpringBoard su LaunchServices |
| **JohnLeeeeeee678**, [`UniXNU-raphael`](https://github.com/JohnLeeeeeee678/UniXNU-raphael) (Redmi K20 Pro) | il percorso SM8150 "chiavi in mano" da cui deriva il port del Mi 9; indicazione della finestra di memoria per i telefoni da 6 GB |
| **wochgn**, [`UniXNU-guacamole`](https://github.com/wochgn/UniXNU-guacamole) (OnePlus 7 Pro) | il primo port SM8150 arrivato allo spazio utente di iOS; le patch per Aloha, UniXNU e DarwinBoot; la nota sulla finestra da 3 GiB |
| **sxl47**, [`UniXNU-cepheus`](https://github.com/sxl47/UniXNU-cepheus) | note sulla memoria e sul framebuffer degli SM8150 (consultate) |
| **STMicroelectronics** (AMG) e **Xiaomi**, driver Linux `fts_521` del touch ST-FTS FTM5 ([`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource), ramo `andromeda-p-oss`, `drivers/input/touchscreen/fts_521`, licenza GPL-2.0) | usato come documentazione del protocollo del controller, nessun codice copiato: comandi, eventi e indirizzi in `xnu/cepheus_touch.c` (lettura FIFO `0x86`, registri `0xFA`, scan `A0`, comandi di sistema `A4`), la sequenza di avvio di `fts.c` e, per `sweettouchdiag`, il formato delle informazioni di sistema (`readSysInfo`) e dei frame del sensore (`getMSFrame3`, `getFrameData`) |
| **TeamWin** ([TWRP](https://twrp.me), build ufficiale `twrp-3.7.0_9-0-andromeda` da [dl.twrp.me/andromeda](https://dl.twrp.me/andromeda/)) | strumento di confronto, nessun codice copiato: avviato solo in RAM, il suo kernel Xiaomi con il touch funzionante ci ha dato i valori di riferimento (regolatori `pm8150l_l1` a 1,8 V e `disp_vddio_vreg` accesi, stato dei pin 37/54/55/56/122, sequenza del driver `fts_521`) da cui vengono `sweettouchgpio=0` e il voto di tensione di L1C (patch 0136); il suo driver ha anche aggiornato da solo il firmware del controller touch da 0x54 a 0x50 (vedi ANDROMEDA.md). Il 7 novembre i frame del sensore letti dai suoi file sysfs `ms_raw` e `ms_strength` (sola lettura) hanno mostrato il riferimento di un sensore funzionante (grezzo da 99 a 688) contro il nostro piatto a -5800, e l'elenco dei suoi regolatori accesi ha indicato `fp_vdd_vreg` e `vdd_boost_vreg` da controllare: `vdd_boost_vreg` era l'alimentazione che mancava al touch (`andrboost=1`) |
| [UniXNU](https://github.com/UniXNU/UniXNU) (organizzazione UniXNU, licenza BSD-2-Clause-Patent) | gestore di avvio UEFI UniXNU, DarwinBoot, `ShengBootMemoryDxe`, script di build di firmware, payload e immagine di avvio; DarwinBoot riempie la struttura `boot_args` di Darwin 23, e il nostro `scripts/patch-darwinboot-bootargs.py` la adatta anche a XNU 8796 (riga di comando da 608 byte) |

## Firmware UEFI

| Progetto / autore | Cosa usiamo |
|---|---|
| **Project Aloha**, [`mu_aloha_platforms`](https://github.com/Project-Aloha/mu_aloha_platforms) | il firmware UEFI del Mi Mix 3 5G (`Platforms/SurfaceDuo1Pkg/Device/xiaomi-andromeda`): port di **Kancy Joe (`sunflower2333`)**, con **XiaoYeZi (`qaz6750`)**, **Gustave Monce (`gus33000`)**, **Rostislav Lastochkin (`remtrik`)** e gli altri contributori; mappa di memoria, driver estratti dal firmware del telefono, device tree |
| **WOA-Project**, [`mu_andromeda_platforms`](https://github.com/WOA-Project/mu_andromeda_platforms) (Gustave Monce) | la base Surface Duo da cui derivano Aloha e Mu-Silicium |
| **Microsoft**, [Project Mu](https://microsoft.github.io/mu/) e [`mu_crypto_release`](https://github.com/microsoft/mu_crypto_release); **TianoCore** [EDK II](https://github.com/tianocore/edk2) | il framework UEFI e il driver BaseCrypto precompilato |
| **Project Silicium**, [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) (Robotix22 e collaboratori); **Renegade Project**, [`edk2-msm`](https://github.com/edk2-porting/edk2-msm) | consultati per la valutazione dell'UEFI |

## HakiOS

| Progetto / autore | Cosa usiamo |
|---|---|
| **OOSEX GANG**, [`hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel), [`hakios-uefi`](https://github.com/oosexgang/hakios-uefi) (shimoku, vcvkk) | consultati: metodo di porting, gancio XNU per Mu-Silicium, driver Wi-Fi e audio come riferimento |
| **nomorecoolnicknames**, [`hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) (POCO X3 Pro) | riferimento per i driver SM8150 (GPU, audio, modem, Wi-Fi) |

## Ricerca: Wi-Fi, Bluetooth e rete (consultati)

Usati come riferimento per i piani in [docs/research/wifi-bluetooth.md](docs/research/wifi-bluetooth.md) e [docs/research/wifi-flame.md](docs/research/wifi-flame.md) (provenienza e licenza di ogni file di ios-flame nel §2). Il codice dei
progetti senza licenza dichiarata non viene copiato senza il permesso dei loro autori.

| Progetto / autore | Cosa ci insegna |
|---|---|
| **MakrSas**, [`ios-topaz`](https://github.com/MakrSas/ios-topaz) (Redmi Note 12, base `ios-sweet`; nessuna licenza dichiarata) e [`topaz-woa`](https://github.com/MakrSas/topaz-woa) | Wi-Fi funzionante su iOS 16.7.16 con un chip della famiglia WCN399x: avvio del modem via TrustZone, GLINK/QRTR, servizi host, WLFW, copy engine, WMI/HTT, scansione, WPA2, interfaccia Apple80211 per `wifid` (patch 0113-0130, 0900) |
| **Angelpro09xd**, [`ios-sapphire`](https://github.com/Angelpro09xd/ios-sapphire) | altro port sulla stessa base |
| **daimile2** e collaboratori, [`renoir_xnu_bluethooth`](https://github.com/daimile2/renoir_xnu_bluethooth) (licenze per file, vedi il loro `NOTICE.md`) | Bluetooth su base `ios-sweet` e iOS 20H392: `bluetoothd` come controller di tipo 0, BlueTool sostitutivo, trasporto UART |
| **OOSEX GANG**, HakiOS (`hakios-xnu-kernel`, `hakios-sm7325`) | stack Wi-Fi verso iOS (Apple80211, WLFW, ath11k) |
| **nomorecoolnicknames**, `hakios-sm8150` | parti SM8150: modem via TrustZone, `rmtfs`, pd-mapper, tftp, WLFW |
| **UniXNU**: [`Ath12kWifi`](https://github.com/UniXNU/Ath12kWifi), [`QcaBluetooth`](https://github.com/UniXNU/QcaBluetooth), [`QcomBaseband`](https://github.com/UniXNU/QcomBaseband) (HoshinoYumeka, [@chiba233](https://github.com/chiba233)) | Wi-Fi, Bluetooth e baseband su SM8550 / iPadOS |
| Kernel Linux (`ath10k` SNOC/QMI, `btqca`), Xiaomi `icnss` | protocolli e sequenze del chip |
| OpenIntelWireless (itlwm/AirportItlwm, zxystd e collaboratori), acidanthera (BrcmPatchRAM, BlueToolFixup), Darling | come Wi-Fi e Bluetooth si presentano ai sistemi Apple |

## Ricerca: ricarica e batteria (consultati)

Usati come riferimento per il piano in [docs/research/ricarica.md](docs/research/ricarica.md) (elenco completo con commit
nella sezione "Fonti"). Il codice dei progetti senza licenza dichiarata non viene copiato.

| Progetto / autore | Cosa ci insegna |
|---|---|
| **shenli99**, [`ios-mh2lm`](https://github.com/shenli99/ios-mh2lm) (LG G8X, stesso PMIC PM8150B, base `ios-sweet`; nessuna licenza dichiarata) | lettura del caricatore sul SID 2, watchdog del caricatore armato all'avvio, lettura della temperatura dal fuel gauge |
| **Casey (Caleb) Connolly** (Linaro), Robin Snyders (`Sr-0w`), Joel Selvaraj, Yassine Oudjana, map220v: `qcom_smbx.c`, `qcom_fg.c`, [`sm8150-mainline`](https://gitlab.com/sm8150-mainline/linux), `hotdog-linux-bringup` | registri e codifiche di SMB5 e fuel gauge su Linux mainline |
| **Teguh Sobirin** (ROCKNIX), `qcom_pm8150b_charger.c` | altro driver del caricatore PM8150B |
| **RigoLigoRLC**, [`unixnu-nativeqcomspmi`](https://github.com/RigoLigoRLC/unixnu-nativeqcomspmi); **rponeawa** e **bluseliu50**, `UniXNU-sheng` (`ShengBattery`) | libreria SPMI e modello di servizio batteria per XNU |
| Xiaomi / Qualcomm, kernel `andromeda-p-oss` (`qpnp-smb5.c`, `smb5-lib.c`, `qpnp-fg-gen4.c`); Project Aloha (`QcomChargerCfg.cfg`) | sequenza di carica del telefono e configurazione del caricatore UEFI |

## Ricerca: GPU (consultati)

Usati come riferimento per il piano in [docs/research/gpu.md](docs/research/gpu.md) (elenco completo con commit e file
nella sezione "Fonti").

| Progetto / autore | Cosa ci insegna |
|---|---|
| **Nick Wylynko** ([NWylynko](https://github.com/NWylynko)), [`ios-flame`](https://github.com/NWylynko/ios-flame) (Pixel 4, SM8150 + Adreno 640, base `ios-sweet`; MIT, patch XNU APSL 2.0) | accensione della stessa GPU sotto XNU, composizione Metal di backboardd/SpringBoard via UniXNU + metal2vulkan + Turnip, CPU a frequenza piena |
| **YangQi0408**, [`UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus) ramo `feat/adreno640` | moduli GMU/HFI/CP/SMMU/QMP/PDC per l'A640 e le salvaguardie del Mi 9; la conversione degli offset dei registri GMU (parole dalla base della GPU, `words * 4 - 0x6a000` in `cepheus_gmu_start.h`) con cui il 7 novembre abbiamo trovato e corretto la base sbagliata di `ANDR_GMU_REG` |
| **UniXNU** (bluseliu50, rponeawa, HuJK e collaboratori): Adreno740, AdrenoMetal, AdrenoFirmware, XnuPrelink, AdrenoPM4, ArmSmmu4K | interfaccia `IOAcceleratorES`, plugin Metal, gestione memoria GPU |
| **steelbrain**, [`metal2vulkan`](https://github.com/steelbrain/metal2vulkan) (GPL-2.0-or-later OR LGPL-3.0-or-later) | traduzione Metal → Vulkan |
| **Mesa** (freedreno, Turnip, ir3; MIT) | driver Vulkan per Adreno a6xx |
| **OOSEX GANG** (shimoku74, vcvkk, yehorkaa, dlz3388461878-cmd), HakiOS (APSL 2.0 + MIT); **nomorecoolnicknames**, `hakios-sm8150` (nessuna licenza); **HeliumStudio-Dev**, Chengdong-Yu | accensione Adreno e percorso OpenGL ES (ripiego) |
| **MakrSas**, `ios-topaz` (nessuna licenza) | `IOAcceleratorES` minimo per backboardd (Adreno 610) |
| Linux drm/msm (`a6xx_gmu.c`, `a6xx_hfi.c`, `a6xx_gpu.c`) e kernel Xiaomi `andromeda-p-oss` | sequenze e indirizzi della GPU SM8150 |

## Ricerche consultate (App Store e altre)

Le ricerche in `docs/research/` (App Store, fotocamere, audio, sospensione, sensori, modem) elencano nella loro sezione
"Fonti" ogni autore, progetto e commit consultato. Per l'App Store in particolare: OOSEX GANG / **vcvkk** (HakiOS, interfaccia
dell'App Store aperta sul POCO X5 Pro), **dipa-studio** (ios-sweet), gli autori di **IOS-26-Hot** (HotStore per `.ipa` non
cifrate), **Nick Wylynko** (ios-flame) e gli autori di **ios-apollo** (installazione di app non cifrate). Nessun codice copiato.
Per l'audio (`docs/research/audio.md`): OOSEX GANG (HakiOS, driver APSL e plug-in `HakiosAudio.driver` MIT),
**nomorecoolnicknames** (`hakios-sm8150`, audio del POCO X3 Pro: solo riferimento, nessuna licenza), UniXNU (UniXNU-sheng, clock
di CoreAudio), **MakrSas** (ios-topaz), Scion-Project (`starlte`), PixelExperience, Texas Instruments / Xiaomi (driver TAS2557,
GPL, solo documentazione) e Linux (q6dsp, WCD934x, SLIMbus).
Per le fotocamere (`docs/research/fotocamere.md`): **vcvkk** e OOSEX GANG (HakiOS, fotocamera su POCO X5 Pro), **Yofuness**
(`ios-sunfish`, MIT) e DenialOfGravity / Rory Axton (`ios-redfin`, MIT), **nomorecoolnicknames** (`hakios-sm8150`, solo
riferimento), **Andrew Gigena** (serie CAMSS SM8150 per Linux) e **Robin Snyders** (fotocamere OnePlus 7T Pro), i manutentori
di Qualcomm CAMSS in Linux e di libcamera, **Eileen Yoon** (Asahi), YangQi0408 (I²C via GPIO che riusiamo).
Per vibrazione, sensori e slider (`docs/research/sensori-vibrazione.md`): Xiaomi `drivers/halls/halls.c` e Qualcomm
`qti-haptics.c` (GPL, solo documentazione: GPIO dei sensori Hall e registri del blocco aptico letti da `andrslider`/`andrprobe`),
**Demon000** (Cosmin Tanislav, riscrittura LineageOS dei Hall, significato degli stati), **Nicholas Magill** (Inkypen79) e
**Sebastiano Barezzi** (kernel e KeyHandler), **nomorecoolnicknames** (`hakios-sm8150` `vayu`, solo riferimento), OOSEX GANG e
gli autori di Scion (vcvkk, dylanneve1, shimoku74), Dylan Van Assche (libssc), Richard Acayan (hexagonrpc), Caleb Connolly,
Andrew Gigena.
Per rete cellulare, GPS e NFC (`docs/research/modem-gps-nfc.md`): **HoshinoYumeka** (@chiba233, `UniXNU/QcomBaseband`, BSD-2),
**tycykp** (`UniXNU-OnePlus-Ace5`, BSD-2-Clause-Patent), OOSEX GANG (HakiOS), nomorecoolnicknames (`hakios-sm8150`, solo
riferimento), akshaynexus (iOSPlus10), Nick Wylynko (ios-flame), seemoo-lab, Mehta/Pathak, Andrew Gigena (IPA v4.1), Richard
Acayan, Stephan Gerhold, linux-msm/Linaro, libqmi/ModemManager e il wiki di postmarketOS.

## Bluetooth e luminosità (patch 0092/0093)

| Progetto / autore | Cosa usiamo |
|---|---|
| **Nick Wylynko** (NWylynko), `ios-flame` patch `0102-flame-rpmh-generic.patch` (commit `745c4341`, MIT) | il modo di dividere `rh_send` di `sweet_rpmh.c` in una richiesta generica (`rh_send_cmds`), ripreso nella patch 0092 |
| **dipa-studio**, `ios-sweet` (`45263bee`, MIT), `sweet_rpmh.c` della patch 0062 | il trasporto RPMh a cui la 0092 aggiunge `andromeda_rpmh_bt_rail` |
| **MakrSas**, [`ios-topaz`](https://github.com/MakrSas/ios-topaz) (`e4772911`, nessuna licenza: solo fatti, nessun codice copiato), patch 0104/0105/0110 | cosa si aspetta CoreBrightness di iOS 16.7.16 (classe `AppleARMBacklight`, `backlight-control`, `brightness-nits`, `IOMFBBrightnessLevel`), il rischio della digital dimming, DCS 0x51 via command DMA |
| **UniXNU contributors**, `UniXNU-sheng` `package/AppleARMBacklight.cpp` (`ce0c600f`, BSD-2-Clause-Patent; consultato, nessun codice copiato) | `IODisplayParameters` (min/max/value), `brightness`/`FadePeriod` via setProperties |
| **Kernel Linux** (GPL-2.0, solo documentazione): `qcom_geni_serial.c`, `qcom-geni-se.c`, `hci_qca.c`, `btqca.c/.h`, `drm/msm/dsi/dsi_host.c`, `clk-rcg2.c`, `gcc-sm8150.c`, `pinctrl-sm8150.c`, `qcom-rpmh-regulator.c`; Xiaomi `bluetooth-power.c` (`andromeda-p-oss`) | registri e sequenze GENI UART, impulsi WCN3990, comando EDL, DCS long write, RCG M/N, modi PMIC5 LDO, ordine delle alimentazioni |
| **Kernel Linux** (GPL-2.0, solo documentazione), fase B2 (`andrbt=2`): `btqca.c/.h` (`qca_uart_setup`, `qca_tlv_check_data`, `qca_tlv_send_segment`, `qca_download_firmware`), `hci_qca.c` (`qca_setup`, `qca_set_speed`, `qca_set_baudrate`); **Dmitry Baryshkov**, patch "Bluetooth: qca: fix ROM version reading on WCN3998 chips" (serie `wcn3990-pwrctl`, v2 del 6 gennaio 2026); Xiaomi `gcc-sm8150.c` (`andromeda-p-oss`) | nomi dei file dalla ROM version (anche il formato `0xX00Y` del WCN3998 e l'NVM "u" del WCN3991), intestazione TLV della patch, tag NVM 2/17/27 e byte della velocità, segmenti `EDL_PATCH_TLV_REQ` da 243 byte con i modi di download e l'ultimo segmento sempre confermato, cambio di velocità `0xFC48` con il controllo di flusso spento e l'evento vendor del WCN3990; il limite di 50 MHz (VDD_MIN) di `gcc_qupv3_wrap2_s3_clk_src`. Nessun codice copiato |
| **nomorecoolnicknames**, `hakios-sm8150` `vayu_bt.c` (`3e7be137`, nessuna licenza: solo fatti, nessun codice) | conferma dei byte dell'ack dei segmenti TLV (`04 FF 03 00 04 00`) e dei tag NVM 17/27/2 sullo stesso SoC |
| **Qualcomm**, firmware Bluetooth WCN399x distribuito da [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware) (`qca/crbtfw21.tlv` `BTFM.CHE.2.1.3.c7-00024-QCACHROM-1`, `qca/crnv21.bin`, `qca/crbtfw32.tlv` `BTFM.CHE.3.2.0.c2-00044-QCACHROMZ-1`, `qca/crnv32.bin`, `qca/crnv32u.bin`; licenza `LICENSE.QualcommAtheros_ath10k`, ridistribuibile; pacchetto Arch `linux-firmware-atheros 1:20261019-1`) | rampatch e NVM della fase B2, copiati in locale in `inputs/andromeda/btfw/` da `scripts/pull-bt-fw.sh` (fuori da git) e passati al kernel come `/chosen btfw,<nome>`; da sostituire con la copia della partizione `bluetooth` del telefono quando sarà estratta |
| **Project Aloha**, Kancy Joe (`sunflower2333`): `Panel_samsung_ea8076_fhd_amoled_cmd.xml`, `PlatformMemoryMapLib.c` di `xiaomi-andromeda` | autorefresh e comandi del pannello EA8076, zona "Display Reserved" |

## Slider e app Fotocamera (patch 0097, `phone/andromedad`)

| Progetto / autore | Cosa usiamo |
|---|---|
| **Nick Wylynko** (NWylynko), [`ios-flame`](https://github.com/NWylynko/ios-flame) (commit `745c4341`, MIT, copia in `licenses/ios-flame-MIT.txt`): `phone/tools/openapp.m`, `openapp-ents.plist`, `phone/tools/build.sh`, `tools/flame-install-ssh.sh` | il modo di aprire un'app dal suo bundle id con FrontBoardServices (`FBSOpenApplicationService`, poi `FBSSystemService`; opzioni `__UnlockDevice`/`__ActivateSuspended`; attesa della risposta) in `phone/andromedad/andromedad.m`; i tre entitlement `*.launchapplications` in `andromedad.entitlements`; compilazione con l'SDK iphoneos e firma ad hoc con entitlement in `build.sh`; il job aggiunto alla cache `System/Library/xpc/launchd.plist` (chiave `/System/Library/LaunchDaemons/<label>.plist`) in `install-root.sh`. Il ripiego su `LSApplicationWorkspace` e il resto del demone sono nostri |
| **dipa-studio**, `ios-sweet` (`45263bee`, MIT) | l'uso della cache dei job di launchd modificata, accettata con `launchd_unsecure_cache=1` (`sweet-root-tweaks.py`), la politica Sandbox permissiva (patch 0050) che lascia girare il demone, e il log della console (patch 0047/0077) che porta le righe `andromedad:` scritte su `/dev/console` nel log persistente e in quello via USB |
| **Demon000** (Cosmin Tanislav), riscrittura LineageOS dei Hall (`5fb68e49`); **Nicholas Magill** (Inkypen79) e **Sebastiano Barezzi**, `KeyHandler` di `device_xiaomi_andromeda` (Apache-2.0); **Xiaomi**, `drivers/halls/halls.c` (GPL-2.0) | solo fatti, nessun codice copiato: GPIO 42/114, (42, 114) = (0, 1) → `STATE_SLIDER_DOWN` = slider aperto (polarità 1 di `andrslidercam`), e l'idea stessa di aprire la fotocamera all'apertura dello slider |
| **Apple**, XNU `xnu-8796.141.3` (APSL 2.0), `bsd/sys/sysctl.h`, `bsd/kern/kern_newsysctl.c`, `bsd/kern/kern_synch.c` | KPI usate da `xnu/andromeda_slider.c` (`SYSCTL_NODE`/`SYSCTL_PROC`, `msleep1`/`wakeup`); dal commento di `sysctl.h` lo schema dell'OID sempre registrato che risponde `ENOENT` quando è spento |

## Fotocamera posteriore, passo C1 (patch 0098)

| Progetto / autore | Cosa usiamo |
|---|---|
| **YangQi0408**, `UniXNU-cepheus`: `cepheus_i2c_bitbang.h`, `cepheus_clock_status.h` | I²C via GPIO con attese limitate e controllo dei rami di clock, riusati senza modifiche |
| **dipa-studio**, `ios-sweet` (`45263bee`, MIT), `sweet_rpmh.c`; **Nick Wylynko**, `rh_send_cmds` (ios-flame 0102, arrivata con la 0092) | il trasporto RPMh a cui la 0098-rpmh aggiunge `andromeda_rpmh_cam_rail` |
| **nomorecoolnicknames**, `hakios-sm8150` `vayu_camera.c` (`3e7be137`, senza licenza: solo l'idea, nessun codice) | I²C via GPIO sui pin del CCI dello stesso SoC |
| **Yofuness**, `ios-sunfish` `docs/features/camera.md` (`a1b6eb8a`, MIT, solo consultato) | MCLK da XO a 19,2 MHz, ramo MCLK dopo il GDSC, un solo indirizzo per prova |
| **Xiaomi/Qualcomm**, `com.qti.sensormodule.andromeda_semco_imx363.bin` (dal vendor del telefono, fuori da git) | indirizzo, registro e valore dell'ID, sequenze di accensione e spegnimento con i ritardi |
| **Xiaomi** `andromeda-p-oss` (`74347f55`, GPL-2.0, solo documentazione): `cam_sensor_cmn_header.h`, `cam_sensor_util.c`, `rpmh-regulator.c` | numeri dei passi, cosa accende ogni passo, unità dei ritardi, scelta del modo dal carico |
| **Linux** (GPL-2.0, solo documentazione): `camcc-sm8150.c` (Satya Priya Kakitapalli), `gdsc.c`, `clk-rcg2.c`, `clk-branch.c`, `gcc-sm8150.c`, `pinctrl-sm8150.c`, `qcom-rpmh-regulator.c` | offset di registri, bit, valori grezzi dei modi PMIC5 |

## Audio senza hardware (fase A0, phone/andromeda-audio)

| Progetto / autore | Cosa usiamo |
|---|---|
| **OOSEX GANG** (shimoku, vcvkk e collaboratori, "the HakiOS authors"), [`hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) commit `63fc2b46`: `userland/src/audio_plugin.c`, `audio_plugin.plist`, `userland/build.sh` (MIT, copia in `licenses/hakios-MIT.txt`) | base di `phone/andromeda-audio/AndromedaAudio.c` e `Info.plist`: interfaccia AudioServerPlugIn, i dispositivi `Speaker`/`Hawking`/`Actuator`/`Codec` con stream, volume dell'altoparlante, sorgente `imic`, orologio su `mach_absolute_time()`, elenco delle proprietà mancanti; compilazione `clang -bundle` e installazione in `/System/Library/Audio/Plug-Ins/HAL`. Nostri: rimozione del lato kernel, log su `/dev/console` con rapporto periodico, esperimento del ricevitore, controllo del clock |
| **dipa-studio**, `ios-sweet` (`45263bee`, MIT): `NOTES.md`, patch 0047/0050 | analisi del crash di `mediaserverd` senza `vdef`; log persistente e Sandbox permissiva che portano le righe del plug-in nel log |
| **UniXNU contributors**, `UniXNU-sheng` (`ce0c600f`, BSD-2-Clause-Patent; consultato, nessun codice copiato): `hal/ShengAudioPlugIn.c` | il clock host a 24 MHz di CoreAudio contro un timer a 19,2 MHz, che il plug-in controlla al caricamento |
| **MakrSas**, `ios-topaz` (`e4772911`, nessuna licenza: solo fatti); **Scion-Project**, `scion-starlte` (`e600c821`, `AUDIO.md`) | prova che lo stesso plug-in è stato portato su 20H392 (topaz) e che la catena plug-in → VirtualAudio suona sull'hardware (Scion) |
| **Nick Wylynko**, `ios-flame` (`745c4341`, MIT): `HANDOFF.md` | la nota che senza dispositivo audio si ferma anche la riproduzione video |

## Wi-Fi e modem, primi passi (patch 0110)

| Progetto / autore | Cosa usiamo |
|---|---|
| **Nick Wylynko** (NWylynko), `ios-flame` (`745c4341`, MIT, copia in `licenses/ios-flame-MIT.txt`): `phone/wifi/smem.c`, `wland.c`, `scm.c` | base di `xnu/andromeda_wlan.c`: lettura e allocazione SMEM, mailbox QMP dell'AOP, item SMP2P in uscita, sequenza PAS `INIT_IMAGE`/`MEM_SETUP`/segmenti/`AUTH_AND_RESET`, qui eseguiti nel kernel invece che in user space |
| **nomorecoolnicknames**, `hakios-sm8150` `vayu_pas.c` (`3e7be137`, nessuna licenza: solo riferimento, nessun codice) | lettura del firmware del modem dal root via vnode, mappatura write-combining della regione riservata, regola "mai SHUTDOWN dopo un avvio senza conferma di arresto" |
| **dipa-studio**, `ios-sweet` (`45263bee`, MIT) | piattaforma `SWEET` e root in lettura-scrittura da cui il driver legge i file |
| **YangQi0408**, `UniXNU-cepheus` | la cautela "is-call-available prima di ogni chiamata, niente `PAS_SHUTDOWN`", condivisa con `andromeda_gpu.c` |

## Consumo e risvegli (patch 0100-0105)

| Progetto / autore | Cosa usiamo |
|---|---|
| **OOSEX GANG** (shimoku74, vcvkk e collaboratori), [`hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) `63fc2b46` (kernel APSL 2.0) | l'idea di non svuotare la L1 per set/way a ogni ingresso in idle sulle schede generiche: la 0101 fa la stessa scelta, a runtime con `andridle=1` e misurandone il costo; nessun codice copiato |
| **aTechGeekDev**, [`ios-apollo`](https://github.com/aTechGeekDev/ios-apollo) `1f75f381` (MIT) | dalle loro note (xnu 0153/0155) le idee di non mandare vsync a schermo spento e di rallentare il touch a schermo spento: 0102 (`andrfbpark`) e il cambiamento in `cepheus_touch.c` (`andrtouchidle`); nessun codice copiato |
| **dipa-studio**, `ios-sweet` `45263bee` (MIT) | i driver che le 0102-0104 modificano: `SweetFramebuffer.cpp`, `sweet_usb.c`, `sweet_keys.c` |
| **YangQi0408**, `UniXNU-cepheus` (`feat/adreno640`) | lo spegnimento del display della loro 0083, su cui si appoggia la 0102; il driver del touch |
| **Apple**, XNU `xnu-8796.141.3` (APSL 2.0) | `processor_info` (`PROCESSOR_CPU_LOAD_INFO`, `PROCESSOR_CPU_STAT64`), `cpu_idle()`, `platform_cache_idle_enter()`, `lck_mtx_sleep_deadline` |
| **Linux** (Danny Lin, commit `81188f585d02`, stati PSCI SM8150; `arch/arm64` `cpu_do_idle()`); USB-IF, *USB 2.0 Specification* §9.2.6.4 | solo documentazione: soglia dei 4 ms, idle di Linux senza manutenzione delle cache, tempi delle richieste di controllo che limitano `andrusbidle` a 20 ms |

## Vibrazione, passo V1 (patch 0135)

| Progetto / autore | Cosa usiamo |
|---|---|
| **Xiaomi / Qualcomm (The Linux Foundation)**, `drivers/input/misc/qti-haptics.c` e `andromeda-sm8150.dtsi` di `andromeda-p-oss` (`74347f55`, GPL-2.0, solo documentazione, nessun codice copiato) | mappa dei registri del blocco aptico del PM8150B; valori e ordine di inizializzazione, ampiezza VMAX e riproduzione; il limite di 2552 mV |
| **Nicholas Magill (Inkypen79)**, kernel EvolutionX in uso sul telefono (device tree attivo) | LRA, sinusoide, risonanza QWD, periodo 5102 µs |
| **nomorecoolnicknames**, [`hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) `vayu_haptic.c` (`3e7be137`, nessuna licenza: solo fatti e metodo, nessun codice copiato) | identità del blocco sullo stesso PMIC (TYPE `0x15`, SUBTYPE `0x09`); il metodo: elenco chiuso di registri, rilettura di ogni scrittura, `STATUS1` ogni 10 ms, `SC_DET` che ferma tutto, due impulsi a 2 s, spegnimento anche nel panic |
| **dipa-studio**, `ios-sweet` (`45263bee`, MIT), `sweet_spmi.c` | la lettura observer con cui `xnu/andromeda_probe.c` rilegge le scritture |
| **Kernel Linux**, `drivers/spmi/spmi-pmic-arb.c` (GPL-2.0, solo documentazione) | comando `EXT_WRITEL` sul canale dell'arbitro v5, bit di stato, tabella dei proprietari |
| **Apple**, XNU `xnu-8796.141.3` (APSL 2.0), `osfmk/kdp/kdp_callout.h` | `kdp_register_callout()` per spegnere il motore se il kernel va in panic durante un impulso |

## GPU: Turnip e Metal (fase F5, phone/turnip e phone/metal2)

Tutto viene scaricato durante la build a commit fissi; i binari viaggiano con `NOTICE.txt`, i testi delle licenze e, per
metal2vulkan, l'archivio del sorgente corrispondente completo.

| Progetto / autore | Cosa usiamo |
|---|---|
| **Nick Wylynko** (NWylynko), `ios-flame` `745c4341` (MIT) | sorgenti di `phone/turnip` (vktest, v2test, shader, patch 0100, `NativeAdrenoABI.h`) e di `phone/metal2` (`SweetMetal2.mm`, `Info.plist`, `m2vtest.c`, frontend adattato a iOS 16, bridge, patch 0100–0101); ricetta di build e schema di installazione |
| **UniXNU** (bluseliu50, rponeawa, HuJK e collaboratori), `UniXNU/UniXNU` `b6ae99c1` (BSD-2-Clause-Patent) | le 8 patch Mesa (backend darwin, MIT), i pin di Mesa e degli strumenti Python, `build-turnip-darwin.py`, `build-native-metal-bridge.py`; NativeMetal (frontend e bridge, MIT), NativeAdreno (ABI, MIT), patch 0001–0006 a metal2vulkan |
| **Anees Iqbal** (steelbrain), `metal2vulkan` `720ef1ce` (GPL-2.0-or-later OR LGPL-3.0-or-later) | traduzione Metal → Vulkan, linkata staticamente nel bundle; sorgente nell'artifact |
| **Sviluppatori Mesa** (freedreno, Turnip, ir3), `85fda3b3` (MIT) | driver Vulkan per Adreno a6xx |
| crate Rust (elenco in `THIRD-PARTY-CRATES.txt` dell'artifact) | dipendenze del bridge |

## App dell'utente (scripts/add-ipa.py, phone/andromeda-apps)

| Progetto / autore | Cosa usiamo |
|---|---|
| **Nick Wylynko** (NWylynko), [`ios-flame`](https://github.com/NWylynko/ios-flame) (`745c4341`, MIT): `phone/tools/lsreg.m`, `lsreg-ents.plist`, `openapp.m`, `openapp-ents.plist`, `phone/wifi/wland.c`, `HANDOFF.md` | installazione senza `installd` (bundle root:wheel in `/Applications`, app di sistema via `_LSPrivateRebuildApplicationDatabasesForSystemApps`, provata con YouTube su 20H392); chiamate LaunchServices ed entitlement di `andromeda-apps`; lettura di `kern.bootargs`; note su identificatore ≠ bundle id e `@rpath` |
| **dipa-studio**, `ios-sweet` (`45263bee`, MIT): `tools/mkinit.py`, patch 0035/0036/0050 | formato della firma ad hoc ripreso nel firmatario di `add-ipa.py`; AMFI che accetta le firme e attacca gli entitlement, Sandbox permissiva |
| **aTechGeekDev**, [`ios-apollo`](https://github.com/aTechGeekDev/ios-apollo) (MIT; `NOTES.md`, `tools/app-pack.sh`) | solo idee: app non cifrate in `/Applications` registrate da LaunchServices |
| **Nithin Amaraboina**, `IOS-26-Hot` (`hotstored.m`, MIT; nessun codice) | l'idea di rifiutare all'importazione i binari con `cryptid != 0` |
| `codesign` di Apple; `ldid` (Procursus) e `rcodesign` (Gregory Szorc) | solo il formato degli entitlement DER, nessun codice |
| **Apple**, XNU `xnu-8796.141.3` (APSL 2.0): `ubc_subr.c`, `kern_proc.c`, `mach_loader.c`, `cs_blobs.h`; `mach-o/loader.h` | regole del kernel su firme e cifratura usate nei controlli |

## GPU: copia a schermo con la GPU (fase F4, patch 0120)

| Progetto / autore | Cosa usiamo |
|---|---|
| **Nick Wylynko** (NWylynko), [`ios-flame`](https://github.com/NWylynko/ios-flame) (`745c4341`, MIT; file XNU sotto APSL 2.0): patch `0095-flame-gpu-present`, `0111-flame-gpu-display-mask`, `0119-flame-gpu-async-present`, `HANDOFF.md` | in `xnu/andromeda_gpu_present.c` e nella 0120: lo stream di registri del blit 2D, il percorso del present (superfici mappate per slot, pulizia della cache, IB + fence, ritorno alla copia CPU), la tabella a 4K e il flush del TLB dell'SMMU, l'immagine di appoggio nel pool |
| **Sviluppatori Mesa** (freedreno, Turnip), Mesa `e6dd90e0` (MIT): `a6xx.xml`, `tu_clear_blit.cc` | campi e allineamenti dei registri A2D; l'idea di scrivere una destinazione non allineata dall'indirizzo allineato sottostante con uno scostamento in x; nessun codice copiato |
| **dipa-studio**, `ios-sweet` (`45263bee`, MIT) | `SweetFramebuffer.cpp` e `SweetSurface` che la 0120 modifica |

## GPU: user client NativeAdreno e Metal (fasi F5/F6, patch 0200, `andrmtl=N`)

| Progetto / autore | Cosa usiamo |
|---|---|
| **Nick Wylynko** (NWylynko), [`ios-flame`](https://github.com/NWylynko/ios-flame) (commit `745c4341`, MIT, copia in `licenses/ios-flame-MIT.txt`; i file XNU modificati sono APSL 2.0): patch `0115-flame-gpu-v2-client-turnip` (`sweet_gpu.c` e il client v2 di `SweetAccelerator.cpp`), `0117-flame-gpu-pool-accounting`, le parti di `0109-flame-gpu-async` e `0113-flame-gpu-unmap-after-fence` che la 0115 usa, `HANDOFF.md` ("The big trap") | in `xnu/andromeda_gpu_mtl.c` / `.h`: il pool dei BO in RAM che XNU non gestisce, con la lista libera ordinata e fusa in unità da 16 KiB; una finestra GPU da 4 GB per connessione da 64 GB in su; le tabelle 4K sotto la L1 condivisa (`map_4k`, `pt_walk`, `a6_unmap`); il submit di fino a 16 IB avvolti come `a6xx_submit` (invalidazione CCU, IB, `CACHE_FLUSH_TS` che scrive la fence) con il controllo dello spazio nel ring; le attese a fette e il rilevamento del blocco senza progressi; i metodi dell'ABI (`v2Method`), l'attesa dell'ultima fence alla chiusura, i descrittori "zombie" e la contabilità del pool per client. Nostri: l'aggancio dopo il gradino 4, nessun reset (un blocco ferma Metal per l'avvio), i controlli più stretti (IB dentro un BO mappato del chiamante, import solo da RAM gestita da XNU, BO in sola lettura mappati in sola lettura, pool controllato pagina per pagina) e il test sull'host `xnu/test_andromeda_gpu_mtl.c` |
| **Nick Wylynko** (NWylynko), [`ios-flame`](https://github.com/NWylynko/ios-flame) (`745c4341`, MIT; file XNU APSL 2.0): patch `0096-flame-metal-accelerator`, `0097-flame-metal-userclient`, `0115-flame-gpu-v2-client-turnip`, `0116-flame-sweetmetal2-plugin-switch` | in `xnu/SweetAccelerator.cpp` (stessi nomi di classe): `IOAcceleratorES` e `SweetAccelerator` con la personalità su `IOResources` e `probe()` che rifiuta senza il parametro; le proprietà `MetalPluginName`/`IOGLBundleName` `SweetMetal2`, `MetalPluginClassName` `SweetMTL2Device`, `model`, `IOUserClientClass`; lo user client `SweetAcceleratorUserClient` (solo il client v2: `initWithTask` per tipo, `clientMemoryForType` write-combined per i BO, `externalMethod` con il tunnel, descrittori su memoria fisica e pin delle pagine del task, raccolta dei descrittori ancora mappati). Nostri: la pubblicazione rimandata al gradino 4, il livello 1 senza plug-in, la divisione tra lato C e "OS layer", i `static_assert` contro `NativeAdrenoABI.h` |
| **dipa-studio**, `ios-sweet` (`45263bee`, MIT) | il blocco `ARM64_BOARD_CONFIG_SWEET` di `iokit/KernelConfigTables.cpp` e `iokit/conf/files.arm64` a cui la 0200 aggiunge `SweetAccelerator`; le IOSurface (`SweetSurface`) che Metal importa |
| **UniXNU contributors**, NativeAdreno (oggi [`UniXNU/Adreno740`](https://github.com/UniXNU/Adreno740): `NativeAdrenoABI.h`, `NativeAdrenoPublish.cpp`; MIT, copia in `licenses/NativeAdreno-MIT.txt`), nella copia di ios-flame `phone/turnip/NativeAdrenoABI.h` (`745c4341`) | `xnu/NativeAdrenoABI.h` senza modifiche: l'ABI v1 (record, metodi, flag) che parla il backend darwin di Turnip; il tunnel `'NTNL'` e i tipi di apertura `0x4147`/2. `scripts/test-mtl-abi.py` controlla che sia identico al pin dello userland (`phone/turnip/build.sh`) |
| **Kernel Linux** drm/msm (GPL-2.0), `a6xx_gpu.c` `a6xx_submit`; **Sviluppatori Mesa** (freedreno, MIT), `adreno_pm4.xml`, `a6xx.xml` | solo documentazione, tramite ios-flame: l'involucro dei submit e i codici degli eventi CP (nessun codice copiato) |

## Hardware del Mi Mix 3 5G

| Fonte | Cosa usiamo |
|---|---|
| **Xiaomi**, [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource), ramo `andromeda-p-oss` | device tree e driver: pannello, touch, audio, caricatore, sensori |
| **Nicholas Magill (`Inkypen79`)**: `kernel_xiaomi_andromeda`, `device_xiaomi_andromeda`, `vendor_xiaomi_andromeda`; [PixelExperience-Devices](https://github.com/PixelExperience-Devices) e PixelExperience-Blobs | device tree e blob Android aggiornati |
| `AndroidBlobs/vendor_xiaomi_andromeda`; **mauronofrio**, `android_device_xiaomi_andromeda` (TWRP) | blob e device tree consultati |
| **Kernel Linux**, `drivers/pinctrl/qcom/pinctrl-sm8150.c` (Qualcomm / The Linux Foundation) | gruppi (tile) dei pin GPIO dell'SM8150 |
| **Kernel Linux**, `drivers/clk/qcom/gcc-sm8150.c`, `include/linux/soc/qcom/geni-se.h`, `drivers/spmi/spmi-pmic-arb.c`, `drivers/pinctrl/qcom/pinctrl-spmi-gpio.c` e le intestazioni dei registri `smb5`/`fg-gen4` del kernel Qualcomm downstream (copia OnePlus, GPL-2.0) | solo documentazione: indirizzi dei clock QUP, registri GENI, protocollo dell'arbitro SPMI, registri di stato del caricatore e del fuel gauge PM8150B e registri dei GPIO del PMIC (pin 5 del PM8150B, `vdd_boost_vreg`) letti dalla sonda `xnu/andromeda_probe.c`, con la sequenza di `direction_output` (modo uscita, livello alto, abilitazione) che `andrboost=1` ripete su quel solo pin (nessun codice copiato) |
| **Kernel Linux / Xiaomi** (`andromeda-p-oss`): `drivers/power/reset/msm-poweroff.c`, `drivers/input/misc/qpnp-power-on.c`, `include/linux/input/qpnp-power-on.h`; **Qualcomm** `QcomModulePkg` (ABL, nel nostro albero Aloha: `LinuxLoader.c`, `ShutdownServices.h`) | solo documentazione: sequenza del riavvio in fastboot (motivo in IMEM e nel PON) usata da `andrfastboot` in `xnu/andromeda_probe.c` |
| **Project Aloha**, Kancy Joe (`sunflower2333`): `PlatformMemoryMapLib.c` di `xiaomi-andromeda`; **Kernel Linux** drm/msm (`a6xx_gmu.c`, `a6xx_hfi.c`, `a6xx_gpu.c`, `gpucc-sm8150.c`) | solo documentazione: mappa della memoria UEFI per scegliere dove mettere la memoria della GPU, registri e sequenze dell'Adreno 640 |
| **Linux**: `kernel/sched/cpufreq_schedutil.c` (Rafael J. Wysocki, Intel), `drivers/cpufreq/cpufreq_interactive.c` (Mike Chan, Google), `drivers/cpufreq/cpu-boost.c` (The Linux Foundation); **Apple XNU** `processor_info`/`recount`, `ml_get_topology_info` | solo documentazione e idee per il governatore della CPU (`xnu/andromeda_cpu_governor.h`, `andrgov=1`): bersaglio 1,25 × occupazione × frequenza, livello "hispeed", attesa minima prima di scendere, frequenza minima dopo un tocco |
| **Project Aloha**, Kancy Joe (`sunflower2333`) e XiaoYeZi (`qaz6750`): `RamPartitionDxe.c` (derivato da ABL `Board.c` di Qualcomm / The Linux Foundation); **Project Mu / EDK II** (`Page.c`, `Gcd.c`) e ArmPkg `CpuMmuCommon.c` (TianoCore, Microsoft); **DarwinBoot** di UniXNU (bluseliu50, rpone) | `patches/aloha-andromeda-ram-merge.patch` modifica `RamPartitionDxe.c` perché la RAM sopra `0x180000000` si unisca a quella sotto in un'unica voce da 3 GiB; il resto è servito come documentazione per capire la mappa e `AllocateWindow` |
| **Danila Tikhonov**, driver mainline `panel-samsung-ams639rq08.c` | riferimento per il pannello Samsung EA8076 |
| [sm8150-mainline](https://gitlab.com/sm8150-mainline/linux) (Caleb Connolly, Alexey Minnekhanov e altri); postmarketOS `linux-postmarketos-qcom-sm8150` | riferimenti mainline per l'SM8150 |

## Strumenti

| Strumento / autore | Uso |
|---|---|
| **Simon Gander (`sgan81`)**, [apfs-fuse](https://github.com/sgan81/apfs-fuse) | lettura dei dischi APFS dell'IPSW |
| `hfsprogs` (porting Linux di `diskdev_cmds` di Apple) | creazione e controllo del volume HFS+ |
| AOSP `libsparse` (`img2simg`) e `mkbootimg` | immagine sparse per `fastboot` e immagine di avvio |
| mtools, dosfstools | volumi FAT del payload |
| **Giuseppe Tribulato**, [remotezip](https://github.com/gtsystem/python-remotezip); **Yogesh Khatri**, binding Python [`lzfse`](https://pypi.org/project/lzfse/); **Kirill Simonov**, PyYAML | estrazione del device tree dall'IPSW senza scaricarlo; verifica dei workflow |
| [ipsw.me](https://ipsw.me) | checksum dell'IPSW |

## Software Apple

iOS 16.7.16 (iPhone X, `20H392`) viene scaricato dai server di Apple da chi usa questo port, costruito in locale e
**mai ridistribuito**: né questo repository né i suoi workflow contengono o pubblicano file del sistema iOS. La licenza
di iOS ne consente l'uso solo su dispositivi Apple; questo è un progetto di ricerca personale.
