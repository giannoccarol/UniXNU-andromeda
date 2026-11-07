# UniXNU sul Xiaomi Mi Mix 3 5G (`andromeda`)

Port di ricerca: kernel XNU di Apple avviato tramite UEFI sul Mi Mix 3 5G (Snapdragon 855 / SM8150). Parte dal port del
Mi 9 ([`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus)), che ha lo stesso chip, la stessa
famiglia di schermo (Samsung EA8076, command mode, 1080×2340) e lo stesso touch (ST FTS su I2C SE17, 0x49, IRQ GPIO122).
I file del Mi 9 restano nel repository come riferimento; la configurazione originale è in `board.cepheus.toml`.

Valutazione completa: [`../VALUTAZIONE-PORT.md`](../VALUTAZIONE-PORT.md).

Regole del progetto (crediti, onestà sullo stato, niente file Apple, sicurezza del telefono): [CONTRIBUTING.md](CONTRIBUTING.md).
Crediti: [CREDITS.md](CREDITS.md).

## Stato

Riepilogo di cosa è pronto da provare e di cosa manca: [docs/COSA-MANCA.md](docs/COSA-MANCA.md).

| Fase | Stato |
|---|---|
| 1. UEFI (Aloha `xiaomi-andromeda` + UniXNU/DarwinBoot) | **funziona sul telefono** (6 novembre 2026): menu UniXNU a schermo |
| 2. Kernel + ADT | **funziona sul telefono** (6 novembre 2026): XNU arriva al montaggio della root |
| 3. Sistema iOS su `userdata` | **funziona sul telefono** (6 novembre 2026): `launchd`, backboardd, SpringBoard; **schermata di blocco con l'orario visibile** |
| 4. Touch | **funziona** (7 novembre 2026): mancava `vdd_boost_vreg`, il regolatore sul pin 5 del PM8150B che Linux tiene sempre acceso e il bootloader lascia spento; `andrboost=1` lo accende prima del touch. Il firmware del controller parte senza impulso di reset (`sweettouchreset=0`). I tocchi arrivano a iOS e funzionano, anche con due dita (`SWEET HID: touch`); trascinamenti e pizzichi no: le posizioni cambiano tra dito giù e dito su, ma iOS non li riconosce (traccia `sweettouchtrace`). Da fare: questo, poi I2C via DMA |
| 4b. Tasti | **funzionano sul telefono** (6 novembre 2026): power, volume e blocco/sblocco schermo (PON, patch 0083 del Mi 9) |
| 5. Bluetooth, Wi-Fi, ricarica | **sonda letta sul telefono** (6 novembre 2026, `andrprobe=2`): **la ricarica funziona** in hardware (stato FULLON, 4,39 V, ~40 mA in ingresso alla batteria dal PC a 500 mA; il watchdog del caricatore non la spegne); l'icona di iOS è ancora un valore fisso. UART Bluetooth SE13 con firmware UART (`proto=2`), clock attivabile, pin 43–46 non ancora in funzione UART. I regolatori Wi-Fi/BT non sono visibili via SPMI dal processore principale: vanno votati via RPMh. Dettagli in `docs/research/ricarica.md` |
| 6. CPU | **driver pronto** (`xnu/andromeda_cpufreq.c`, `andrcpu=1`): tutti i cluster alla frequenza piena come ios-flame, con protezione termica (Gold/Prime a ≤1,4 GHz sopra 80 °C, di nuovo al massimo sotto 70 °C). Da misurare sul telefono |
| 7. Batteria per iOS | **driver pronto** (patch 0091, `sweetbattery=2`): percentuale, tensione, corrente e stato di carica veri dal PM8150B al posto del 100 % fisso. Da provare |
| 8. GPU | driver integrato (port di ios-flame): gradini `andrgpu=1..4`, copia a schermo con la GPU (F4, `andrgpupresent=1`, patch 0120) e librerie Turnip/Metal compilate (F5, workflow `gpu-userland-andromeda`). Firmware nel device tree Apple. Niente ancora provato sul telefono oltre il gradino 1 da preparare |
| Ritorno in fastboot | **scritto, non provato** (`andrfastboot=N`): dopo N secondi sync dei dischi, motivo "bootloader" nel PMIC e reset PSCI. Da provare solo con l'utente presente |
| Controllo a telefono acceso | **scritto e compilato, non provato** (`andrctl=1`, `scripts/andctl.py`): dal PC, via UDP sulla rete USB, avvia i driver a un livello e accende/spegne statistiche e risparmi senza riavviare. Vedi [sotto](#controllo-a-telefono-acceso-andrctl1) |
| Avvio | misurato: sistema montato a 43 s, SpringBoard a 73 s, schermata di blocco a **116 s** dal `fastboot boot` |

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
| 3 GiB | `Out of Resources`: nessun blocco libero così grande nella mappa UEFI. Causa trovata dopo (6 novembre): la RAM sopra `0x180000000` è una voce separata della mappa perché `RamPartitionDxe` di Aloha la aggiunge con capacità diverse; il PStore in questa build non esiste. La patch `patches/aloha-andromeda-ram-merge.patch` unisce le due voci (3 GiB da `0x140000000`) |
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

**Panic del 7 novembre (1): riga degli argomenti troppo lunga.** Sei avvii su sette, con il kernel nuovo e con
quello del 6 novembre, finiscono con `memmove` verso l'indirizzo 0 in `end_transaction` del journal HFS, alla prima
transazione dopo il mount. Causa: XNU 8796 legge 608 byte di argomenti (`BOOT_LINE_LENGTH`,
`pexpert/pexpert/arm64/boot.h`), DarwinBoot riempie un campo da 1024 (struttura di Darwin 23,
`unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/BootArgs.h`). Con 721 caratteri il testo dal carattere 612 diventa
`bootFlags` e `memSizeActual`, cioè `hw.memsize`; HFS calcola il buffer del journal come 128 KiB × (memsize / 256 MiB)
in un `int`, che va in overflow: dimensione negativa, `IOMallocData` restituisce NULL. Il calcolo sulle righe delle
sette immagini prevede esattamente gli esiti visti (l'unica che passava, `nocpu`, dava per caso un valore positivo;
la riga del 6 novembre, 419 caratteri, lasciava il campo a zero). Inoltre i caratteri dal 608 in poi non arrivano al
kernel: gli interruttori aggiunti in coda con `ANDROMEDA_ARGS` venivano ignorati. Correzione: argomenti sotto i 608
caratteri (i valori predefiniti `=0` non si passano più) e `scripts/build-payload.py` rifiuta righe più lunghe. In DarwinBoot, `scripts/patch-darwinboot-bootargs.py` (applicato da `scripts/firmware-in-container.sh`) rifiuta righe oltre 607 caratteri con un messaggio sullo schermo e scrive `bootFlags`/`memSizeActual` anche agli offset di XNU 8796 (0x2d0/0x2d8); `RAM_MERGE=0 ./build-uefi.sh` compila la UEFI senza la RAM unita. La UEFI così corretta (`ff3a5919…`) differisce da quella provata (`ea4f8073…`, ricompilata identica) solo per questa modifica; provata sul telefono il 7 novembre (iOS fino a SpringBoard) e ora predefinita. La UEFI precedente resta in `out/andromeda/fv/SM8150_EFI-unixnu-stock.fd`.

**Panic del 7 novembre (2): spazio per le mappature dei registri esaurito.** Con la GPU accesa
`kmem_alloc(…, 2097152, 0x100021): failed with 3` (`KERN_NO_SPACE`). È `io_map()` di XNU:
`ml_io_map()` prende gli indirizzi da `io_submap`, che su ARM è grande **8 MiB** (`osfmk/arm/io_map.c`), e ogni
mappatura è permanente. Ogni driver rimappava gli stessi blocchi (GCC 384 KiB, TLMM nord e sud 512 KiB ciascuno,
TLMM est), e la GPU chiede finestre da 2 MiB. Correzione: `andromeda_io_map()` (in `xnu/andromeda_probe.c`), usata
da tutti i driver andromeda, mappa con `ml_io_map_unmappable()` nello spazio dati di `kernel_map` (gigabyte) come fa
il driver GPU di ios-flame, e riusa una mappatura che copre già l'intervallo; il pool della GPU è mappato allo stesso
modo (write-combining). Ogni nuova mappatura lascia una riga `ANDROMEDA PROBE: io map …` con il totale.

**Touch, 7 novembre.** Con le diagnostiche in sola lettura (`sweettouchdiag`) il sensore risultava piatto: frame strength mai oltre ±22 con il dito sullo schermo, mentre chip, comandi e versioni AFE (tutte 0x2) erano in regola. Avviato in RAM, **TWRP** (kernel Xiaomi, driver `fts_521`) ha il touch funzionante con le stesse alimentazioni, gli stessi pin e la stessa sequenza (reset, `A0 00 01`); il suo driver però **ha aggiornato da solo il firmware del controller** da 0x54/config 0x2A a **0x50/config 0x25** (file `st_fts_e5.ftb`): scrittura permanente nel chip, non prevista. Con il firmware 0x50 il nostro driver legge solo `0xFF` da tutto ciò che serve il firmware (l'ID del chip, registro hardware, si legge), sia via GPIO sia con il controller I2C hardware SE17 (`sweettouchgpio=0`, come Linux). Differenza trovata: nel cmd-db `ldoc1` è un regolatore VRM e il nostro voto chiedeva solo l'accensione, senza tensione, mentre TWRP mostra `pm8150l_l1` a 1800000 uV. La patch 0136 vota 1800 mV + HPM + acceso prima del reset del chip.

**Touch, 7 novembre (2).** Con L1C a 1,8 V il firmware 0x50 non riparte comunque dopo l'impulso di reset (registro `0x20000024` = `01`, core M3 fermo; né il rilascio né il reset software via registro lo avviano). Riparte invece se il chip si accende con il reset già alto (prova "warm2": TWRP, riavvio in fastboot, nostro kernel senza impulso): evento `03`, informazioni di sistema identiche a quelle di TWRP (FW 0x50, config 0x25, CX `FFFF`, AFE 2/2/2). Anche così il sensore resta cieco: 45 frame senza tocchi. Il confronto dei frame (in TWRP con i file sysfs `ms_raw`/`ms_strength` del driver, in sola lettura) è netto: in TWRP il grezzo va da 99 a 688 (media 445, rumore ±7), da noi è piatto a circa -5800 (±25, rumore ±30). La parte analogica del chip non misura. La sequenza di accensione del pannello EA8076 della UEFI (`Panel_samsung_ea8076_fhd_amoled_cmd.xml`) è identica a quella di Linux, quindi il display è escluso. Prossima prova: lo stato di `fp_vdd_vreg` (GPIO92) e `vdd_boost_vreg` (pin 5 del PM8150B), due regolatori che Linux tiene sempre accesi senza un utilizzatore nel device tree; `sweettouchfpvdd=1` porta GPIO92 alto, `sweettouchreset=0` evita l'impulso di reset.

**Touch, 7 novembre (3).** Avvio a freddo (telefono spento da TWRP) con `sweettouchreset=0`: **il firmware parte** (evento `03`, controller pronto), quindi era l'impulso di reset a lasciarlo fermo. Il chip riporta di nuovo **FW 0x54/config 0x2A**, la versione originale, e non lo 0x50 che TWRP mostrava un'ora prima: l'aggiornamento fatto dal driver di TWRP non è sopravvissuto allo spegnimento completo, oppure il chip ha due immagini. Da capire. Stato dei regolatori prima che il nostro driver li tocchi: GPIO7 (`disp_vci_vreg`) alto, GPIO92 (`fp_vdd_vreg`) ingresso basso, GPIO37 basso, pin 5 del PM8150B (`vdd_boost_vreg`) configurato come ingresso con pull-down (`MODE_CTL` 00, `DIG_PULL_CTL` 04, `EN_CTL` 80), cioè spento, mentre Linux lo tiene sempre acceso. GPIO92 alto non cambia nulla: grezzo ancora piatto a -5800, forza massima 28. Prova successiva: `andrboost=1`.

**Touch, 7 novembre (4): funziona.** Con `andrboost=1` la sonda accende il pin 5 del PM8150B come fa Linux (`MODE_CTL` 01, `DIG_OUT_SOURCE_CTL` 80, `EN_CTL` 80, tutte rilette uguali) 20 s prima che il driver alimenti il controller. Il frame grezzo diventa quello di TWRP (da -275 a 207, media circa 43), il rumore della forza scende a ±7 e sotto il dito sale fino a 966; il controller manda gli eventi di contatto (`13`/`23`/`33`) e iOS riceve i tocchi: 13 tocchi completi e 40 posizioni nel primo minuto, l'utente conferma che funziona. Il regolatore `vdd_boost_vreg` (device tree: `regulator-fixed` su `pm8150b_gpios 5`, `regulator-always-on`, senza utilizzatori dichiarati) è quindi l'alimentazione della parte analogica del sensore. Restano da verificare: se `sweettouchfpvdd=1` serve ancora, il multitouch, e togliere `sweettouchraw`/`sweettouchdiag` dagli argomenti.

**Perché si bloccava.** Se il ramdisk non è un FAT `GUACESP`, `ShengBootMemoryDxe` cerca il payload leggendo pagina per
pagina tutta la memoria della mappa UEFI, comprese le zone `EfiReservedMemoryType`. Sul Mix 3 5G tra queste c'è
"Hypervisor" (`0x85700000`), che non è leggibile: il firmware si ferma prima di disegnare qualsiasi cosa. Sul Mi 9 non
succede perché c'è sempre un payload vero. Da sistemare nel modulo: leggere solo memoria normale (write-back).

**Perché 34 MiB.** Il bootloader carica il ramdisk a `0x80800000` e la zona "Kernel" della mappa finisce a
`0x82A00000`; da `0x83722000` inizia "HLOS 1", memoria che il firmware usa per sé. Il payload da 64 MiB del Mi 9
arriverebbe a `0x84800000`, sopra quella zona. 34 MiB con cluster da 512 byte restano un FAT32 valido.

## Controllo a telefono acceso (`andrctl=1`)

**Scritto e compilato, non ancora provato sul telefono.** Con l'argomento di boot `andrctl=1` il kernel
(`xnu/andromeda_ctl.c`, patch 0140) riceve comandi dal PC mentre iOS gira e avvia o accende i driver andromeda senza
riavviare e senza ricostruire l'immagine. Serve anche perché i 607 caratteri della riga di boot non bastano più per
tutti gli interruttori. Senza `andrctl=1` non si apre nessuna porta.

```bash
ANDROMEDA_ARGS="andrctl=1" ./build-boot.sh             # immagine con il ricevitore acceso (9 caratteri in più)
python3 scripts/udplog-punch.py phone/logs/xnu.log     # log, come sempre
python3 scripts/andctl.py status                       # stato di tutti i driver, una riga
python3 scripts/andctl.py gpu 1                        # GPU gradino 1 adesso
python3 scripts/andctl.py idle keys 50                 # un risparmio P1
```

Il PC deve avere `192.168.7.1` sull'interfaccia USB, come per il log. La risposta è una sola riga, `ok COMANDO: cosa
succede` oppure `no COMANDO: perché no`, e finisce anche nel log come `ANDROMEDA CTL: …` (`scripts/andstatus.py` mostra
l'ultima). `andctl.py` esce con 0 per `ok`, 1 per `no`, 2 se non arriva risposta. Le risposte passano il firewall del
PC (ufw) senza regole né sudo perché sono risposte al pacchetto partito dal PC.

| Comando | Cosa fa | Argomento di boot equivalente |
|---|---|---|
| `status`, `help` | stato di ogni driver (`-`, `wait:N` in attesa del suo ritardo, `run:A-B`, `done:N` = passato fino a N, `fail:N`), proprietario di PAS, consumi, risparmi, slider; elenco dei comandi | — |
| `gpu 1`–`4` | gradini della GPU fino a N | `andrgpu=N` |
| `bt 1`–`2` | Bluetooth B1 o B1+B2 | `andrbt=N` |
| `hap 1` | le due vibrazioni di prova | `andrhap=1` |
| `cam 1` | fotocamera C1 | `andrcam=1` |
| `wlan 1`–`2` | Wi-Fi W0 (sola lettura) o W0+W1 | `andrwlan=N` |
| `bl 1`–`2` | luminosità: prova o servizio | `andrbl=N` |
| `pwr 0`/`1` | righe `ANDROMEDA PWR` e conteggio idle spenti/accesi | `andrpwr=1` |
| `idle` | mostra i cinque risparmi P1 | — |
| `idle cpu 0`/`1`, `fb 0`/`1`, `usb 0`/`2`–`20`, `keys 0`/`20`–`200`, `touch 0`/`20`–`1000` | un risparmio P1, `0` = spento | `andridle` (bit 0), `andrfbpark`, `andrusbidle`, `andrkeysidle`, `andrtouchidle` |
| `slider 1` | avvia la lettura dello slider | `andrslider=1` |
| `slidercam 0`–`2` | polarità dello slider per `andromedad` (si prova senza riavviare) | `andrslidercam=N` |
| `fastboot` | ritorno in fastboot adesso | `andrfastboot=N` |

Regole, uguali a quelle degli argomenti di boot (stesso codice, stessi controlli):

- **Un gradino già passato non riparte, una corsa fallita non si ripete.** GPU e Wi-Fi proseguono dal gradino
  successivo (`gpu 3` dopo un avvio con `andrgpu=2` esegue solo il gradino 3; `wlan 2` dopo `wlan 1` esegue solo W1);
  la luminosità può passare dalla prova (`bl 1`) al servizio (`bl 2`); Bluetooth, fotocamera e vibrazione girano una
  volta per avvio. Una corsa di un argomento di boot ancora nel suo ritardo parte subito, al livello più alto dei due.
- **Limiti invariati**: soglie termiche della GPU (ricontrollate anche prima del gradino 4 quando prosegue una corsa
  già passata dal 3), tensione massima della vibrazione, valori dei risparmi entro gli
  stessi intervalli degli argomenti di boot (fuori intervallo il comando è rifiutato, non corretto).
- **GPU gradino 3 e Wi-Fi W1 mai nello stesso avvio.** Nel codice la risorsa in comune è **TrustZone PAS** (lo ZAP
  della GPU e l'avvio del modem), non la mailbox AOP QMP: la GPU andromeda non apre mai QMP. Il primo dei due che
  chiede PAS lo tiene per tutto l'avvio (`andromeda_pas_claim`); vale anche per gli argomenti di boot: con
  `andrgpu>=3` e `andrwlan=2` nella stessa riga parte la GPU e il Wi-Fi si rifiuta con un messaggio (prima la regola
  era solo scritta qui).
- **Una corsa hardware alla volta**: GPU, Bluetooth, fotocamera, vibrazione, Wi-Fi e il voto SE13 della sonda si danno il turno
  (`andromeda_ctl_begin`); una chiesta mentre un'altra gira aspetta (`ANDROMEDA CTL: bt waits for gpu …`). Vale
  anche per le corse degli argomenti di boot, che prima potevano sovrapporsi se i ritardi coincidevano.
- **Bluetooth e sonda**: il voto del clock SE13 di `andrprobe=2`, lo stesso bit che usa il Bluetooth, prende anch'esso
  il turno, così i due non si sovrappongono qualunque siano i ritardi.

Perimetro (cosa entra e cosa no):

- solo pacchetti arrivati su `ue0`, la rete USB (verificato con `IP_RECVIF`), da `192.168.7.1`; tutto il resto è
  scartato senza risposta (`ANDROMEDA CTL: dropped …` nel log);
- solo la lista chiusa qui sopra (`xnu/andromeda_ctl.h`, testata sul PC da `xnu/test_andromeda_ctl.c`): ASCII
  stampabile, al massimo 96 caratteri, parole fisse e piccoli numeri decimali; nessun indirizzo, registro, percorso o
  testo libero arriva a un driver;
- 8 comandi di fila, poi uno ogni 250 ms;
- un comando non esegue mai il lavoro nel ricevitore: avvia il thread del driver, che fa quello che farebbe al boot.

Cambi per gli avvii senza `andrctl` (dovuti al gestore, tutti più prudenti): la regola PAS e i turni delle corse
hardware qui sopra; la GPU che si ferma per la temperatura prima del gradino 3 scrive `not started (thermal refuse)` e
non conta come fallita; la riga finale della GPU dice fino a quale gradino è passata.

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

Tutti i progetti e le persone di cui usiamo il lavoro, con cosa usiamo di ciascuno: [CREDITS.md](CREDITS.md). In breve:
`ios-sweet` (dipa-studio), il port del Mi 9 di YangQi0408 (base di questo repository), il percorso SM8150 di
JohnLeeeeeee678 e wochgn, UniXNU e DarwinBoot, il firmware UEFI di Project Aloha (port `xiaomi-andromeda` di Kancy Joe /
`sunflower2333`), XNU di Apple, `darwin-xnu-build` di blacktop e `apfs-fuse` di Simon Gander.
