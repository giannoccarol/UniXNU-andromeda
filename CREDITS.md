# Crediti

Questo port esiste grazie al lavoro di molte persone e progetti. Qui sotto c'è chi abbiamo usato, per cosa, e dove.
Se manca qualcuno o qualcosa è attribuito male, è un errore da correggere.

## Port e kernel XNU

| Progetto / autore | Cosa usiamo |
|---|---|
| **Apple**, [XNU](https://github.com/apple-oss-distributions/xnu) (`xnu-8796.141.3`, licenza APSL 2.0) | il kernel, compilato dai sorgenti pubblici |
| [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) (licenza MIT, copia in `licenses/ios-sweet-MIT.txt`) | piattaforma `SWEET` per XNU su Qualcomm: patch del kernel (CPU, GIC, UFS, framebuffer `AppleCLCD`, USB CDC-ECM e log in diretta, log persistente, tasti, HID), strumenti del sistema iOS (`mkiosroot.sh`, `sweet-root-tweaks.py`) e il primo iOS 16 su un telefono Android; la lettura SPMI tramite observer (`sweet_spmi.c`) è la base della sonda `xnu/andromeda_probe.c` |
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

Usati come riferimento per il piano in [docs/research/wifi-bluetooth.md](docs/research/wifi-bluetooth.md). Il codice dei
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

## Hardware del Mi Mix 3 5G

| Fonte | Cosa usiamo |
|---|---|
| **Xiaomi**, [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource), ramo `andromeda-p-oss` | device tree e driver: pannello, touch, audio, caricatore, sensori |
| **Nicholas Magill (`Inkypen79`)**: `kernel_xiaomi_andromeda`, `device_xiaomi_andromeda`, `vendor_xiaomi_andromeda`; [PixelExperience-Devices](https://github.com/PixelExperience-Devices) e PixelExperience-Blobs | device tree e blob Android aggiornati |
| `AndroidBlobs/vendor_xiaomi_andromeda`; **mauronofrio**, `android_device_xiaomi_andromeda` (TWRP) | blob e device tree consultati |
| **Kernel Linux**, `drivers/pinctrl/qcom/pinctrl-sm8150.c` (Qualcomm / The Linux Foundation) | gruppi (tile) dei pin GPIO dell'SM8150 |
| **Kernel Linux**, `drivers/clk/qcom/gcc-sm8150.c`, `include/linux/soc/qcom/geni-se.h`, `drivers/spmi/spmi-pmic-arb.c` e le intestazioni dei registri `smb5`/`fg-gen4` del kernel Qualcomm downstream (copia OnePlus, GPL-2.0) | solo documentazione: indirizzi dei clock QUP, registri GENI, protocollo dell'arbitro SPMI, registri di stato del caricatore e del fuel gauge PM8150B letti dalla sonda `xnu/andromeda_probe.c` (nessun codice copiato) |
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
