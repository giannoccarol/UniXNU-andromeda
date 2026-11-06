# Crediti

Questo port esiste grazie al lavoro di molte persone e progetti. Qui sotto c'è chi abbiamo usato, per cosa, e dove.
Se manca qualcuno o qualcosa è attribuito male, è un errore da correggere.

## Port e kernel XNU

| Progetto / autore | Cosa usiamo |
|---|---|
| **Apple**, [XNU](https://github.com/apple-oss-distributions/xnu) (`xnu-8796.141.3`, licenza APSL 2.0) | il kernel, compilato dai sorgenti pubblici |
| [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) (licenza MIT, copia in `licenses/ios-sweet-MIT.txt`) | piattaforma `SWEET` per XNU su Qualcomm: patch del kernel (CPU, GIC, UFS, framebuffer `AppleCLCD`, USB CDC-ECM e log in diretta, log persistente, tasti, HID), strumenti del sistema iOS (`mkiosroot.sh`, `sweet-root-tweaks.py`) e il primo iOS 16 su un telefono Android; la lettura SPMI tramite observer (`sweet_spmi.c`) è la base della sonda `xnu/andromeda_probe.c`; il servizio batteria `SweetBattery` (patch 0062) che la nostra patch 0091 collega al PM8150B |
| **Nick Wylynko** ([NWylynko](https://github.com/NWylynko)), [`ios-flame`](https://github.com/NWylynko/ios-flame) (Pixel 4, stesso SoC SM8150; licenza MIT, copia in `licenses/ios-flame-MIT.txt`) | metodo e layout dei registri OSM per portare i core alla frequenza piena (`sweet_cpufreq.c`, patch 0086): base di `xnu/andromeda_cpufreq.c`, a cui abbiamo aggiunto la protezione termica; **driver GPU Adreno 640**: `sweet_a6xx.c` e `sweet_gpu.c` (patch 0095, sequenza CX/GMU/HFI/GX/CP/ZAP, pool in RAM non gestita da XNU, firmware come proprietà ADT `gpufw,<nome>`), il ciclo SMC con ripresa da `sweet_pmem.c` (patch 0094) e le note di `HANDOFF.md`: base di `xnu/andromeda_gpu.c`, `xnu/andromeda_gpu_core.c` e dell'opzione `--gpufw` di `scripts/make-andromeda-adt.py` (file XNU sotto APSL 2.0); il servizio sostitutivo `AppleMobileFileIntegrity` (`SweetAMFI.cpp`, patch 0103 → la nostra 0096) e gli indirizzi `mac-address-*` in `/chosen` (`tools/mkadt.py`), che sbloccano LaunchServices e i fornitori di sfondi di PosterBoard; il piano Wi-Fi in `docs/research/wifi-flame.md` parte dal loro `wland` |
| [`blacktop/darwin-xnu-build`](https://github.com/blacktop/darwin-xnu-build) | la build di XNU sui runner macOS di GitHub |
| **YangQi0408**, [`UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus) (Xiaomi Mi 9) | **base di questo repository**: driver touch ST-FTS (`xnu/cepheus_touch.c`) e I2C via GPIO, voto RPMh per `ldoc1`, codice SoC/CPU (`cepheus_soc.c`, OSM, TSENS), generatore del device tree Apple (`mkadt.py`, `devices.py`), costruzione del sistema iOS (`rebuild-root.sh`, `prepare-ios-root.py`), payload e immagine di avvio, patch per Aloha/UniXNU/DarwinBoot, workflow del kernel; dal ramo `feat/adreno640`: correzione della cache per indirizzi sopra i 4 GiB, tasti PON, evento Home, spegnimento rapido dello schermo, base per la GPU Adreno 640 (GMU, HFI, SMMU, CP); analisi dello stallo di SpringBoard su LaunchServices |
| **JohnLeeeeeee678**, [`UniXNU-raphael`](https://github.com/JohnLeeeeeee678/UniXNU-raphael) (Redmi K20 Pro) | il percorso SM8150 "chiavi in mano" da cui deriva il port del Mi 9; indicazione della finestra di memoria per i telefoni da 6 GB |
| **wochgn**, [`UniXNU-guacamole`](https://github.com/wochgn/UniXNU-guacamole) (OnePlus 7 Pro) | il primo port SM8150 arrivato allo spazio utente di iOS; le patch per Aloha, UniXNU e DarwinBoot; la nota sulla finestra da 3 GiB |
| **sxl47**, [`UniXNU-cepheus`](https://github.com/sxl47/UniXNU-cepheus) | note sulla memoria e sul framebuffer degli SM8150 (consultate) |
| [UniXNU](https://github.com/UniXNU/UniXNU) (organizzazione UniXNU, licenza BSD-2-Clause-Patent) | gestore di avvio UEFI UniXNU, DarwinBoot, `ShengBootMemoryDxe`, script di build di firmware, payload e immagine di avvio |

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
| **YangQi0408**, [`UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus) ramo `feat/adreno640` | moduli GMU/HFI/CP/SMMU/QMP/PDC per l'A640 e le salvaguardie del Mi 9 |
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
| **Project Aloha**, Kancy Joe (`sunflower2333`): `Panel_samsung_ea8076_fhd_amoled_cmd.xml`, `PlatformMemoryMapLib.c` di `xiaomi-andromeda` | autorefresh e comandi del pannello EA8076, zona "Display Reserved" |

## Hardware del Mi Mix 3 5G

| Fonte | Cosa usiamo |
|---|---|
| **Xiaomi**, [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource), ramo `andromeda-p-oss` | device tree e driver: pannello, touch, audio, caricatore, sensori |
| **Nicholas Magill (`Inkypen79`)**: `kernel_xiaomi_andromeda`, `device_xiaomi_andromeda`, `vendor_xiaomi_andromeda`; [PixelExperience-Devices](https://github.com/PixelExperience-Devices) e PixelExperience-Blobs | device tree e blob Android aggiornati |
| `AndroidBlobs/vendor_xiaomi_andromeda`; **mauronofrio**, `android_device_xiaomi_andromeda` (TWRP) | blob e device tree consultati |
| **Kernel Linux**, `drivers/pinctrl/qcom/pinctrl-sm8150.c` (Qualcomm / The Linux Foundation) | gruppi (tile) dei pin GPIO dell'SM8150 |
| **Kernel Linux**, `drivers/clk/qcom/gcc-sm8150.c`, `include/linux/soc/qcom/geni-se.h`, `drivers/spmi/spmi-pmic-arb.c` e le intestazioni dei registri `smb5`/`fg-gen4` del kernel Qualcomm downstream (copia OnePlus, GPL-2.0) | solo documentazione: indirizzi dei clock QUP, registri GENI, protocollo dell'arbitro SPMI, registri di stato del caricatore e del fuel gauge PM8150B letti dalla sonda `xnu/andromeda_probe.c` (nessun codice copiato) |
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
