# UniXNU sul Xiaomi Mi Mix 3 5G (`andromeda`)

Port di ricerca: kernel XNU di Apple avviato tramite UEFI sul Mi Mix 3 5G (Snapdragon 855 / SM8150). Parte dal port del
Mi 9 ([`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus)), che ha lo stesso chip, la stessa
famiglia di schermo (Samsung EA8076, command mode, 1080×2340) e lo stesso touch (ST FTS su I2C SE17, 0x49, IRQ GPIO122).
I file del Mi 9 restano nel repository come riferimento; la configurazione originale è in `board.cepheus.toml`.

Valutazione completa: [`../VALUTAZIONE-PORT.md`](../VALUTAZIONE-PORT.md).

## Stato

| Fase | Stato |
|---|---|
| 1. UEFI (Aloha `xiaomi-andromeda` + UniXNU/DarwinBoot) | **funziona sul telefono** (6 novembre 2026): menu UniXNU a schermo |
| 2. Kernel + ADT | **funziona sul telefono** (6 novembre 2026): XNU arriva al montaggio della root |
| 3. Sistema iOS su `userdata` | da fare (cancella i dati Android) |
| 4. Touch | da fare: adattare `xnu/cepheus_touch.c` (reset GPIO54, alimentazioni L1C + GPIO37) |

## Compilare l'UEFI

Serve Docker. EDK II non compila da un percorso con spazi, quindi la build gira in un container Ubuntu 24.04 con la
cartella montata su `/work`.

```bash
git clone https://github.com/UniXNU/UniXNU unixnu && git -C unixnu checkout b6ae99c && git -C unixnu submodule update --init --recursive
git clone https://github.com/Project-Aloha/mu_aloha_platforms uefi && git -C uefi checkout 0b798128 && git -C uefi submodule update --init --recursive
./build-uefi.sh
```

Risultato in `out/andromeda/boot/`:

- `boot-andromeda-uefi-plain.img`: UEFI di Aloha senza UniXNU (test 1: il UEFI parte?);
- `boot-andromeda-uefi-unixnu.img`: UEFI con UniXNU e DarwinBoot (test 2: compare il menu UniXNU?).

Entrambe contengono un payload FAT `GUACESP` da 34 MiB senza kernel (`scripts/make-empty-payload.sh`): il menu UniXNU
non trova un sistema da avviare, ed è previsto. `VARIANTS="nome=moduli ..." ./build-uefi.sh` compila varianti con un
sottoinsieme dei moduli (`sheng`, `unixnu`, `unixnudxe`, `darwinboot`), per isolare un blocco.

## Risultati sul telefono (6 novembre 2026)

Telefono: Mi Mix 3 5G, `board-id 0x2d`, SM8150 v2.2, 6 GB, EvolutionX 16 (kernel Inkypen 4.14), bootloader sbloccato.

| Immagine | Risultato |
|---|---|
| UEFI Aloha semplice | parte in ~11 s; schermata di recupero (chiave inglese, cavo USB, QR code) ereditata dal firmware del Surface Duo; il PC vede il dispositivo USB `045e:066b` |
| UEFI + UniXNU, ramdisk vuoto | **schermo nero, bloccato** |
| UEFI semplice + solo `ShengBootMemoryDxe`, ramdisk vuoto | **bloccato** (nessun dispositivo USB): il colpevole è questo modulo |
| UEFI + UniXNU, payload FAT da 34 MiB | **menu UniXNU**; dopo 10 s torna al firmware (prima voce della configurazione di riserva: "Return to firmware") |

### Fase 2: primo avvio di XNU

| Finestra del kernel (`owned_memory_size`) | Risultato |
|---|---|
| 3 GiB | `Out of Resources`: nessun blocco libero così grande nella mappa UEFI (MPSS_EFS riservato a `0xC0000000`, PStore a `0x17FE00000`) |
| 2 GiB | `Not Found`: `AllocatePages(AllocateAddress)` di EDK II non accetta un intervallo che copre due voci della mappa (`Page.c`, "covers multiple entries"), mentre DarwinBoot misura lo spazio sommando voci adiacenti |
| **1,5 GiB** | **XNU parte**: 8/8 core (PSCI `CPU_ON`, SGI verificati), `AppleCLCD 1080x2340` a `0x9c000000` presentato come 1125×2436, inizializzazione BSD, rete USB CDC-ECM (`Sweet iOS (USB Ethernet)`, `1209:0001`; il telefono risponde al ping su `192.168.7.2`), driver UFS che legge `userdata` (`BSD root: ufsroot0`). La root non si monta perché `userdata` contiene ancora Android (`hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : 14df`) e il kernel riprova in ciclo: **è il punto d'arrivo previsto della fase 2** |

Sul telefono non viene scritto nulla: senza `ufsrw=1` il driver UFS rifiuta ogni scrittura, e il log persistente
(`sweetlog`) è spento.

**Log in diretta via USB.** Con `sweetusb=2` il kernel manda la console in UDP a `192.168.7.1:5140`; al primo
collegamento rimanda gli ultimi 64 KiB. Sul PC:

```bash
sudo bash -c 'nmcli device set enp0s20f0u1 managed no; ip addr replace 192.168.7.1/24 dev enp0s20f0u1 && ip link set enp0s20f0u1 up && iptables -I INPUT -i enp0s20f0u1 -p udp --dport 5140 -j ACCEPT'
python3 scripts/udplog.py phone/logs/xnu.log
```

Se l'indirizzo arriva dopo il primo invio, l'inizio dell'avvio va perso (il kernel non ritrasmette): per averlo intero,
`sudo scripts/watch-usb-log.sh` va lanciato prima di `fastboot boot`, così riassegna l'indirizzo appena compare
l'interfaccia (MAC `02:00:5e:77:ee:01`).

**Perché si bloccava.** Se il ramdisk non è un FAT `GUACESP`, `ShengBootMemoryDxe` cerca il payload leggendo pagina per
pagina tutta la memoria della mappa UEFI, comprese le zone `EfiReservedMemoryType`. Sul Mix 3 5G tra queste c'è
"Hypervisor" (`0x85700000`), che non è leggibile: il firmware si ferma prima di disegnare qualsiasi cosa. Sul Mi 9 non
succede perché c'è sempre un payload vero. Da sistemare nel modulo: leggere solo memoria normale (write-back).

**Perché 34 MiB.** Il bootloader carica il ramdisk a `0x80800000` e la zona "Kernel" della mappa finisce a
`0x82A00000`; da `0x83722000` inizia "HLOS 1", memoria che il firmware usa per sé. Il payload da 64 MiB del Mi 9
arriverebbe a `0x84800000`, sopra quella zona. 34 MiB con cluster da 512 byte restano un FAT32 valido.

## Provare sul telefono

`fastboot boot` carica l'immagine solo in RAM: non scrive nessuna partizione, e al riavvio torna Android.

```bash
adb reboot bootloader
fastboot getvar product          # deve rispondere: andromeda
fastboot boot out/andromeda/boot/boot-andromeda-uefi-unixnu.img
```

Per uscire tieni premuto il tasto di accensione per 10-15 secondi; per tornare in fastboot, accensione + volume giù.
Parti con la batteria sopra il 50%. Le variabili UEFI di Aloha sono emulate in RAM (`PcdEmuVariableNvModeEnable`): la
voce di avvio che UniXNU registra non viene scritta sul telefono.

## Fase 2: kernel e device tree Apple

Il kernel è lo stesso del Mi 9 (`xnu-8796`, `MACHINE_CONFIG=SWEET`, da `dipa-studio/ios-sweet`): stesso SoC, e le
aggiunte del Mi 9 (touch, SoC) partono solo con `sweettouch=1` / `cephsoc=1`, che `board.toml` lascia spenti. Si compila
solo su macOS, con il workflow manuale `kernel-andromeda` (runner `macos-15-intel` con Xcode 16.2: `macos-14` è stato
ritirato il 2 novembre 2026, e gli Xcode successivi al 16.2 non compilano `xnu-8796`): usa solo sorgenti pubblici, non scarica
nulla dall'IPSW, e l'artifact `andromeda-kernel` scade dopo 7 giorni.

```bash
gh workflow run kernel-andromeda.yml --ref andromeda
gh run download <run-id> -n andromeda-kernel -D inputs/andromeda     # -> inputs/andromeda/kernel.macho
scripts/get-base-d22ap.sh       # device tree dell'iPhone X: solo DeviceTree.d22ap.im4p dall'IPSW Apple (~33 KB)
./build-boot.sh                 # payload GUACESP 34 MiB + out/andromeda/boot/boot-andromeda-xnu.img
```

`get-base-d22ap.sh` ha bisogno dei pacchetti Python `remotezip` e `lzfse`; nel container si installano in un ambiente
temporaneo (`python3 -m venv /tmp/venv`). Il device tree Apple del Mix 3 5G (`--device andromeda` in `scripts/mkadt.py`)
si presenta come iPhone X (`D22AP`), con la memoria misurata sul telefono: `dram-base 0xC0000000`, 6 GiB, finestra
kernel da 3 GiB.

Il primo avvio di prova mostra il log del kernel a schermo. Senza il sistema iOS su `userdata` (fase 3) il kernel non
trova la partizione di root e si ferma lì: è il traguardo atteso della fase 2.

## Dati del telefono

`scripts/pull-phone-info.sh` legge via adb proprietà, memoria, id di SoC e scheda. Con il root legge anche il device tree
attivo, `/proc/iomem` e il backup di `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `boot`, `dtbo`, `vbmeta`. Sul
telefono non scrive nulla, a parte copie temporanee in `/data/local/tmp`, cancellate subito. Funziona solo se il telefono
collegato risulta `andromeda`. Il risultato va in `phone/<seriale>/`, escluso da git perché contiene dati che identificano
il telefono.

## Cosa cambia rispetto al Mi 9

- `board.toml`: dispositivo `xiaomi-andromeda`, 6 GB di RAM (valori `TODO_MEASURE` da confermare sul telefono).
- `scripts/build-firmware.py`: generico. Prende il dispositivo da `board.toml` e aggiunge da solo i quattro moduli
  UniXNU/DarwinBoot a `Device/xiaomi-andromeda/DXE.inc` e `DXE.dsc.inc` (la patch Aloha li aggiunge solo al OnePlus 7 Pro).
  Con `NO_DARWINBOOT=1` compila l'UEFI semplice.
- `scripts/build-bootimg.py`: generico. Aggiorna `/chosen/linux,initrd-*` in **tutti** i device tree della tabella:
  quella di Aloha per `andromeda` contiene 11 alberi base (SM8150 v1/v2, SM8150P v1/v2 e altre schede), e il bootloader
  sceglie quello che corrisponde al chip. `--no-payload` mette un ramdisk vuoto.
- `scripts/get-dtb.sh`: usa il device tree attivo del telefono (`inputs/live-fdt/andromeda.dtb`), se c'è, altrimenti
  quello di Aloha.
- `build/Dockerfile.firmware`, `build-uefi.sh`, `scripts/firmware-in-container.sh`: build in container.

## Crediti

- [UniXNU](https://github.com/UniXNU/UniXNU) e DarwinBoot.
- [`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus) (Mi 9): base di questo repository.
- [`JohnLeeeeeee678/UniXNU-raphael`](https://github.com/JohnLeeeeeee678/UniXNU-raphael) e
  [`wochgn/UniXNU-guacamole`](https://github.com/wochgn/UniXNU-guacamole): il percorso SM8150 da cui deriva il Mi 9.
- Project Aloha, [`mu_aloha_platforms`](https://github.com/Project-Aloha/mu_aloha_platforms): il UEFI `xiaomi-andromeda`
  di Kancy Joe (`sunflower2333`), con XiaoYeZi (`qaz6750`), Gustave Monce (`gus33000`), Rostislav Lastochkin (`remtrik`) e
  gli altri autori; basato su `mu_andromeda_platforms` di WOA-Project e su Project Mu di Microsoft.
- [`microsoft/mu_crypto_release`](https://github.com/microsoft/mu_crypto_release): driver BaseCrypto precompilato.
- Hardware del Mi Mix 3 5G: Xiaomi, [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource)
  ramo `andromeda-p-oss`; Nicholas Magill (`Inkypen79`) e PixelExperience per i device tree.
