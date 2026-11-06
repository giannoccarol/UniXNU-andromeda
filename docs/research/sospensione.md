# Sospensione e consumo sotto XNU su `andromeda`: cosa tiene sveglio il telefono, e un piano

Ricerca del 6 novembre 2026 (voce 12 di [ROADMAP.md](../../ROADMAP.md)), solo lettura: nessun comando sul telefono,
nessun codice dei repository consultati eseguito, nessun codice copiato. I valori del Mi Mix 3 5G vengono dal device tree
attivo del telefono (`phone/<seriale>/live-fdt.dts`, fuori da git) e dal log USB del 6 novembre
(`phone/logs/merged-20261106-174054.log`, fuori da git); il resto dai file citati in [Fonti](#8-fonti).

Legenda della certezza, la stessa di [ricarica.md](ricarica.md):

- **✔** letto nel sorgente, nel device tree o nel nostro log (o misurato da altri su hardware uguale);
- **~** dedotto: coerente con più indizi, non ancora visto su `andromeda`;
- **?** da verificare con la sonda della [fase P0](#p0-sonde-e-misura-di-partenza).

## Sommario

- **Oggi il telefono non dorme mai, nemmeno un core.** Nel kernel SWEET nessuno chiama PSCI `CPU_SUSPEND` né `CPU_OFF`
  (✔ ricerca nel codice di ios-sweet, del Mi 9 e nostro). Un core inattivo esegue solo `WFI`; il cluster, la LLCC e
  l'AOSS (il sottosistema sempre acceso che spegne CX e oscillatore) non scendono mai. In più, ogni ingresso in idle
  svuota la cache L1 per set/way (`platform_cache_idle_enter()` sulle piattaforme non Apple, ✔ `osfmk/arm/caches.c`):
  HakiOS lo ha tolto perché "lento, sbagliato con più processori, e sotto un hypervisor ogni istruzione è una trap".
- **Molte sveglie, tutte da polling.** Il kernel SWEET non ha interruzioni periferiche (SPI): solo il timer e gli IPI
  (✔ patch 0004/0017 di ios-sweet). Per questo tutto interroga a tempo: USB ogni ~1,1 ms (~880 risvegli al secondo,
  ✔ misurato dal nostro log), vsync del framebuffer a 60 Hz anche a schermo spento, touch ogni ~15,5 ms con I2C a
  bit-bang in attesa attiva (✔ log), tasti ogni 20 ms. Con `andrcpu=1` e senza `andrgov=1` i tre cluster restano alla
  frequenza (e tensione) massima.
- **Lo schermo "spento" è solo nero.** `RequestPowerChange(0)` di backboardd scrive un fotogramma nero
  (ios-sweet 0079, Mi 9 0083), ma il pannello resta acceso e, molto probabilmente, l'MDP continua a rinfrescarlo dalla
  RAM a 60 Hz (autorefresh lasciato dal firmware, ✔ su Pixel 4 per ios-flame, ~ da noi).
- **Il SoC sa dormire, e il device tree dice come.** Stati PSCI del telefono (✔ FDT `qcom,lpm-levels`, uguali al
  mainline): core in "rail power collapse" `0x40000004` (uscita 0,9 ms sui Silver, 1,5 ms sui Gold/Prime); cluster
  intero con LLCC spenta e AOP in sonno `0x4100c244` (uscita 6,6 ms), accettato solo quando tutti i core dormono
  (Danny Lin, commit Linux `b2e3f897684c`). PSCI `SYSTEM_SUSPEND` non c'è (✔ misurato da ios-flame sullo stesso SoC):
  anche Linux su SM8150 fa solo *suspend-to-idle* (tutti i core nello stato più profondo + voti RPMh di sonno).
- **Per il sonno profondo servono tre cose che oggi mancano**: (1) i voti RPMh di sonno/risveglio sul DRV2 dell'`apps_rsc`
  (TCS SLEEP e WAKE, scritti dall'ultimo core che si spegne, più l'ora del prossimo risveglio nel TCS di controllo);
  (2) le sorgenti di risveglio nel PDC (tasto power e cavo arrivano entrambi dall'arbitro SPMI, SPI 481 = pin PDC 1;
  USB sui pin 6/8/9); (3) un percorso di ritorno in XNU (che esiste già: `resume_idle_cpu` → `arm_init_idle_cpu` →
  `cpu_idle_exit(TRUE)`, pensato da Apple per i core spenti in idle).
- **Nessun port iOS su Android dorme davvero.** ios-apollo scrive che la CPU non sospende mai e che da bloccato il
  telefono consuma quasi come in uso. HakiOS: sonno di sistema disattivato di proposito perché `IOCPUSleepKernel()` si
  bloccava. UniXNU: `sleep-disabled = 1` nel device tree Apple. ios-sweet, ios-flame, Mi 9: nessun lavoro sul sonno.
  Ci sono però pezzi utili: lo spegnimento del pannello via DSI di ios-raphael (SM8150), la riduzione dei risvegli di
  ios-apollo, il `CPU_OFF` e il driver GIC con SPI di HakiOS.
- **Misurare si può già**, con una sola scrittura da approvare: `andromeda_battery_sample()` legge IBATT (488,28 µA per
  LSB), ma con il cavo collegato il caricatore alimenta il telefono e IBATT non è il consumo. Sospendendo l'ingresso
  (`USBIN_CMD_IL` `0x1340` bit 0, la stessa scrittura dell'`input_suspend` Android) il telefono va a batteria e il log
  USB resta. In alternativa: cavo staccato e log persistente.
- **Piano a fasi**, ognuna dietro un parametro di avvio spento di default: **P0** sonde e misura di partenza; **P1**
  meno risvegli, frequenze basse e niente svuotamento della cache in idle; **P2** core spenti (prima `CPU_OFF` dei core
  1-7 a schermo spento, poi `CPU_SUSPEND` in idle); **P3** schermo spento davvero (autorefresh fermo, DCS `0x28`/`0x10`);
  **P4** sorgenti di risveglio, cluster `0x4100c244`, voti RPMh di sonno: uno "standby" alla Linux con il tasto power e
  il cavo che svegliano; **P5** (dopo) sonno di sistema vero di iOS, dark wake, idle profondo anche a schermo acceso.

## 0. Cosa sappiamo già dal telefono

Dal log USB del 6 novembre (dalle 17:40, tre avvii, ~65 minuti in tutto) e dal device tree attivo:

| Fatto | Valore | Fonte |
|---|---|---|
| Thread USB (gadget a polling, `ud_sleep_ms(1)`) | 30000 giri in ~34 s → **~1,13 ms a giro, ~880 risvegli/s** | ✔ log: righe `SWEET USB: ue0 rx … tx … frames` a +28,0 s e +61,9 s |
| Touch (ST-FTS, I2C a bit-bang, `touch_wait(10)`) | 3000 letture in ~46,5 s → **15,5 ms a ciclo**: ~5 ms oltre l'attesa, in buona parte attesa attiva (`delay()`) | ✔ log `SWEET TOUCH (cepheus): polls=…`; ~ ripartizione |
| Schermo spento a lungo | da +3528,9 s (`SWEET FB: display off`) a fine log (+3946 s): **~7 minuti senza tentativi di sonno di sistema** (nessun `IOCPUSleepKernel`, nessun panic), cavo collegato | ✔ log |
| Watchdog APSS (`qcom,wdt@17c10000`) | non armato sotto XNU: ~29 minuti di funzionamento continuo (secondo dei tre avvii nel log) senza che nessuno lo accarezzi (Android lo usa con abbaio a 11 s e `qcom,wakeup-enable`) | ~ dedotto dal log; ✔ FDT |
| USB lasciato acceso dal firmware | GDSC `usb30_prim` acceso, 7 clock (master, sleep, utmi, aggre-axi, cfg-noc-axi, ahb2phy, clkref) accesi | ✔ log +133,9 s |
| Corrente batteria con il cavo del PC | IBATT `0xFFB3` = -37,6 mA (entra corrente), VBATT 4,386 V, ingresso 500 mA | ✔ [ricarica.md](ricarica.md) §0 |
| Ingresso del caricatore | `USBIN_CMD_IL` (`0x1340`) = `0x00`: non sospeso | ✔ sonda `andrprobe=2`, riga `PM8150B USBIN cfg sid2 0x1300: 40=00` |
| Batteria | 3800 mAh (`qcom,nom-batt-capacity-mah = <0xed8>`) | ✔ FDT |

Conti utili: con 3800 mAh, una giornata intera a schermo spento richiede una media **sotto ~158 mA**; tre giorni
**sotto ~53 mA**. Un passo del SOC monotono (1/255) vale ~14,9 mAh.

## 1. Come fanno Apple e XNU

Sorgenti: `apple-oss-distributions/xnu` `xnu-8796.141.3` (la stessa versione del nostro kernel; copia locale con
`config/MasterVersion` 22.6.0). Tutto ✔ letto nel sorgente, salvo dove indicato.

### 1.1 Idle di un core

- Lo scheduler, quando un processore non ha lavoro, arriva a `cpu_idle()` (`osfmk/arm64/cpu.c`). Se `idle_enable` è vero
  (lo è sempre, salvo il boot-arg `jtag`) e il prossimo timer non è troppo vicino (`SetIdlePop()`), esegue:
  callback di piattaforma `cpu_idle_notify(cpu, TRUE, &timeout)` → `platform_cache_idle_enter()` (solo non Apple) →
  `dsb sy` → `wfi` (o `arm64_retention_wfi()` sui chip Apple con stato di ritenzione) → `cpu_idle_exit(FALSE)`.
- La callback viene da `ml_processor_register()`: `processor_idle` e `idle_timer` di `ml_processor_info_t`
  (`machine_routines.c`). Sui telefoni Apple la riempie `AppleARMSMP.cpp` con `gPMGR->enterCPUIdle()` /
  `exitCPUIdle()` / `updateCPUIdle()`: è il driver PMGR (chiuso) che decide quanto spegnere il core. Il campo
  `powergate_latency` diventa `cpu_idle_latency`, che `SetIdlePop()` usa per **svegliare il core in anticipo della sua
  latenza d'uscita**.
- **Il ritorno da un core spento è già previsto.** `cpu_machine_idle_init()` mette in ogni `cpu_data` il gestore di reset
  `resume_idle_cpu`; se il core si spegne in `wfi`, al risveglio parte dal vettore di reset (`LowResetVectorBase` in
  `start.s`), trova il suo `cpu_data` dall'MPIDR, salta a `resume_idle_cpu` → `arm_init_idle_cpu()` (`arm_init.c`) →
  `cpu_idle_exit(TRUE)` → `Idle_load_context()`. Il thread di idle non ha stato da salvare: **non serve salvare
  registri**, solo rimettere in piedi MMU, eccezioni, timer.
- Sui chip non Apple `platform_cache_idle_enter()` chiama `FlushPoU_Dcache()`, che pulisce e invalida per set/way la L1
  (la macro `DCACHE_SET_WAY` scorre i livelli da 0 a LoUIS compreso): su Cortex-A76 sono ~1024 operazioni `dc cisw` a
  ogni ingresso in idle, e la cache si svuota ogni volta. Su A76/A55 la coerenza è hardware: non serve. HakiOS l'ha
  tolto (patch di `osfmk/arm64/cpu.c`, `#if !defined(APPLE_ARM64_ARCH_FAMILY) && !defined(GENERIC_PSCI_SMP)`),
  scrivendo che sotto un hypervisor ogni istruzione set/way è una trap (✔ per HakiOS, ? per il nostro QHEE).

### 1.2 Core spenti a caldo

- `processor_exit()` / `processor_start()` (`osfmk/kern/processor.c`; esiste anche `processor_exit_from_user()`) tolgono e
  rimettono un core. Apple ha anche `sched_perfcontrol_update_powered_cores()` (`sched_prim.c`), con cui il controllore
  delle prestazioni (CLPC, chiuso) accende e spegne i core secondo il carico.
- Il core che esce passa da `cpu_sleep()`: segna `SleepState`, rimette come gestore di reset `start_cpu`, pulisce la cache
  (`CleanPoC_Dcache()`), poi `PE_cpu_machine_quiesce()` (piattaforme IOCPU "classiche", come la nostra: chiama
  `IOCPU::quiesceCPU()`) oppure `PE_cpu_down()` + `arm64_prepare_for_sleep()` (AppleARMSMP). Al riavvio
  `cpu_machine_init()` chiama `PE_cpu_machine_init(cpu, !started)`: per un core che rientra, `initCPU(false)`.

### 1.3 Sonno di sistema

- In iOS decide `powerd` (spazio utente) quando lo schermo è spento e nessuno tiene asserzioni; chiede il sonno a
  `IOPMrootDomain`. Il kernel lo consente se `checkSystemSleepAllowed()` non trova ostacoli: `gSleepDisabledFlag`
  (anche dalla proprietà `sleep-disabled` di `/defaults` nel device tree Apple, letta a fine avvio), `CONFIG_SLEEP`
  (acceso nel nostro `MASTER.arm64.sweet`, che deriva dalla configurazione macOS), asserzioni CPU, ecc.
- Poi `IOPlatformExpert::sleepKernel()` → `IOCPUSleepKernel()`: nella versione IOCPU (`IOCPU.cpp`, la nostra) chiama
  `haltCPU()` su ogni core secondario e per ultimo sul core di avvio; nella versione AppleARMSMP usa `processor_exit()`.
  Il core di avvio arriva a `ml_arm_sleep()` (`cpu.c`): aspetta che tutti gli altri abbiano il `cpu_sleep_token`, salva
  `wake_abstime`, poi `arm64_prepare_for_sleep(true)`.
- `arm64_prepare_for_sleep` (`machine_routines_asm.s`): sui chip Apple imposta i registri di spegnimento e fa `wfi`; sulla
  macchina virtuale Apple (`APPLEVIRTUALPLATFORM`) fa **PSCI `CPU_OFF` sui secondari e `SYSTEM_SUSPEND` sul core di avvio**
  (via `hvc`); sulle piattaforme generiche, compresa SWEET, è solo un ciclo di `wfi`. È un precedente utile: XNU stesso
  usa PSCI per dormire.
- Al risveglio `cpu_timebase_init(FALSE)` ricalcola `rtclock_base_abstime` perché `mach_absolute_time()` non avanzi
  durante il sonno, che il contatore hardware si sia fermato o no.
- `IOPMrootDomain` ha un watchdog del sonno/risveglio (`gSwdPanic = 1`): 35 o 180 s a seconda della configurazione
  (`IOServicePMPrivate.h`), poi panic. Utile per la diagnosi, ma è un rischio in più.
- **Dark wake**: risvegli brevi a schermo spento (timer, notifiche), gestiti da `IOPMrootDomain` (`gDarkWakeFlags`).
  Il nostro kernel è compilato come macOS (`XNU_TARGET_OS_OSX`, ✔ NOTES.md di ios-sweet), quindi usa i valori macOS
  (`kDarkWakeFlagUserWakeWorkaround` su arm64). Non serve all'inizio: si può considerare ogni risveglio "completo".

### 1.4 Lo schermo

- backboardd decide schermo acceso/spento (tasto, blocco automatico) e lo chiede a `IOMobileFramebuffer`: selettore 12,
  `RequestPowerChange(stato)` (✔ commento della patch 0079 di ios-sweet e nostro log `SWEET FB: display off/on`). Sui
  telefoni Apple il driver del display spegne pannello e pipeline; la luminosità passa da un servizio
  `AppleARMBacklight` con `IODisplayParameters` (✔ ios-apollo 0154, ripreso da ios-spes).

### 1.5 Cosa fa oggi la piattaforma SWEET

| Pezzo | Comportamento | Fonte |
|---|---|---|
| `SweetCPU` (IOCPU classico) | accende i core con PSCI `CPU_ON` (`0xC4000003`, `smc`); `ml_processor_info_t` senza `processor_idle` né `powergate_latency` (= 0); `quiesceCPU()`, `haltCPU()` e `initCPU(false)` vanno in **panic** ("not implemented"); `startCPU()` rifiuta un secondo avvio | ✔ ios-sweet 0018, 0065, 0078 |
| Idle | `wfi` semplice, con svuotamento set/way della L1 a ogni ingresso | ✔ `cpu.c`, `caches.c` (nessuna patch SWEET lo tocca) |
| Interruzioni | solo PPI del timer virtuale (INTID 19) e SGI 1 abilitate sul redistributor; gli SPI andrebbero a `cdp->interrupt_handler`, che nessuno imposta | ✔ ios-sweet 0004, 0017 |
| Driver | tutti a polling: USB 1 ms (0073/0075), tasti 20 ms (0060), touch 10 ms + I2C a bit-bang (nostro `cepheus_touch.c`), batteria 30 s (0062) | ✔ codice |
| Framebuffer | timer vsync a 60 Hz (`SWEET_FB_VSYNC_US 16667`) e notifiche vsync ai client **anche a schermo spento**; spento = un fotogramma nero, pannello alimentato | ✔ ios-sweet 0046, 0079; Mi 9 0083 |
| Sonno di sistema | consentito dal kernel (`CONFIG_SLEEP`, nessun `sleep-disabled` nel nostro ADT); se arrivasse, `IOCPUSleepKernel()` → `SweetCPU::haltCPU()` → panic | ✔ codice; ? se `powerd` lo chieda a batteria |
| Frequenze | `andrcpu=1` senza `andrgov`: tutti i cluster all'indice OSM più alto, sempre | ✔ `board.toml`, `xnu/andromeda_cpufreq.c` |

## 2. Il lato SM8150

### 2.1 PSCI

- Device tree: `psci { compatible = "arm,psci-1.0"; method = "smc"; }` (✔ FDT). ios-flame ha misurato sul Pixel 4 (stesso
  SoC): **PSCI 1.1, `CPU_ON` e `SYSTEM_RESET` sì, `SYSTEM_RESET2` e `SYSTEM_SUSPEND` no** (✔ `NOTES.md`); ios-sweet lo stesso
  sull'SM7150. Su `andromeda` non è stato ancora chiesto (?): [P0](#p0-sonde-e-misura-di-partenza).
- Funzioni che ci servono (SMC64 dove serve un indirizzo): `PSCI_VERSION` `0x84000000`, `CPU_SUSPEND` `0xC4000001`
  (x1 stato, x2 punto d'ingresso fisico, x3 contesto), `CPU_OFF` `0x84000002`, `CPU_ON` `0xC4000003`, `AFFINITY_INFO`
  `0xC4000004`, `PSCI_FEATURES` `0x8400000A`, `PSCI_SET_SUSPEND_MODE` `0x8400000F`, `PSCI_STAT_RESIDENCY` `0xC4000010`,
  `PSCI_STAT_COUNT` `0xC4000011` (le ultime due facoltative: se ci sono, il firmware conta da solo tempo e ingressi per
  stato).
- `PSCI_FEATURES(CPU_SUSPEND)`: bit 0 = modalità OS-initiated (OSI) disponibile, bit 1 = formato esteso dello stato (Linux
  `psci.c`, `psci_has_osi_support()`). Gli stati Qualcomm (bit 30 = "perde il contesto") sono nel formato esteso (~).
- `CPU_OFF` funziona sui SoC Qualcomm: HakiOS lo usa per spegnere i core prima di tornare al loader (`psci_smp.c`); il
  core rientra con `CPU_ON` come al primo avvio.

### 2.2 Stati di idle (FDT attivo, `qcom,lpm-levels`, `qcom,use-psci`)

| Livello | Etichetta | Parametro PSCI | Ingresso / uscita / residenza minima (µs) |
|---|---|---|---|
| core Silver (cpu0-3), 0 | `wfi` | (nessuna chiamata: `wfi`) | 57 / 43 / 100 |
| core Silver, 1 | `rail-pc` | modo 4 → **`0x40000004`** | 355 / 909 / 3934 |
| core Gold/Prime (cpu4-7), 0 | `wfi` | — | 57 / 43 / 83 |
| core Gold/Prime, 1 | `rail-pc` | **`0x40000004`** | 241 / 1461 / 4488 |
| cluster `L3`, 0 | `l3-wfi` | modo 1 | 48 / 51 / 99 |
| cluster `L3`, 1 | `llcc-off` | modo `0xc24` << 4, `qcom,notify-rpm`, `qcom,is-reset` → **`0x4100c244`** | 3263 / 6562 / 9987 |

✔ FDT; gli stessi valori sono nel mainline (`sm8150.dtsi`, commit `81188f585d02` di Danny Lin, "valori del kernel
Qualcomm 4.14 `LA.UM.8.1.r1-15600-sm8150.0`"). La composizione di `0x4100c244` è nel commit `b2e3f897684c` di Danny Lin:
stato cluster `0xc24 << 4` + bit di reset `1 << 30` + livello di affinità `1 << 24` + stato del core `0x4`. Nello stesso
commit: in questo stato **la LLCC è spenta e l'AOP entra in sonno**, e lo stato **non si può usare come stato di idle
semplice, perché vale solo se tutti i core del cluster dormono**; in Linux lo si raggiunge con il driver cpuidle a domini
PSCI in modalità OSI (l'ultimo core che va in idle chiede il cluster). Nella discussione sulla prima versione (LKML,
gennaio 2021) Danny Lin riferiva che lo stesso valore usato come stato piatto "non funziona per niente", e funziona con i
domini. Esiste anche uno stato `0x40000003` (C3), non usato nemmeno da Qualcomm "per problemi di throttling termico".

Due conseguenze per XNU:

- **Timer.** I `rail-pc` hanno `qcom,use-broadcast-timer` (mainline: `local-timer-stop`): con il core spento il suo
  timer non lo sveglia (✔ FDT). Serve un timer esterno: il timer di sistema a memoria `arm,armv7-timer-mem` a
  `0x17c20000`, frame 0 a `0x17c21000`, SPI 8 (fisico) e SPI 6 (virtuale) (✔ FDT). Quando scende tutto il cluster, il
  risveglio a tempo lo fa l'AOSS: Linux scrive l'istante del prossimo timer nel TCS di controllo del RSC (§2.4).
- **GIC al ritorno.** Con il GIC in modalità sicura (il nostro caso: `GICD_TYPER.SecurityExtn = 1`, ✔ ios-flame) Linux,
  all'uscita da uno stato che spegne il core, rimette solo i registri di sistema della CPU interface
  (`gic_cpu_sys_reg_enable()`/`gic_cpu_sys_reg_init()` in `irq-gic-v3.c`); il redistributor lo gestisce il firmware.
  Anche XNU dovrà rifare `ICC_SRE/PMR/BPR1/CTLR/IGRPEN1` e riprogrammare il comparatore del timer virtuale
  (`arm_init_idle_cpu()` chiama solo `fiq_context_init()` → `_enable_virtual_timer()`).

### 2.3 RPMh: chi tiene accesi CX, MX, XO e la DDR

- Il RSC delle applicazioni: FDT `apps_rsc: mailbox@18220000` (`qcom,tcs-drv`, `qcom,drv-id = <2>`, TCS a `0x18220d00`,
  IRQ SPI 5) con `qcom,tcs-config` = ACTIVE 2, SLEEP 3, WAKE 3, CONTROL 1 (✔ FDT; il mainline lo ha corretto agli stessi
  numeri con il commit `17ac8af678b6` di Maulik Shah). Il display ha il suo RSC (`disp_rsc` a `0xaf20000`, DRV 0:
  SLEEP 1, WAKE 1, ACTIVE 2). Il downstream ha anche `system_pm` (`qcom,system-pm`) sul canale `apps_rsc`.
- Ogni DRV ha tre insiemi di voti: attivo, sonno, risveglio. I TCS ACTIVE mandano subito; quando il sottosistema dorme
  il sequencer del RSC manda i TCS SLEEP, al risveglio i TCS WAKE. **Se il set di sonno è vuoto, restano i voti attivi**:
  quelli che l'UEFI ha messo sullo stesso DRV 2 (l'`RpmhDxe` di Aloha gira sul processore applicativo) e i nostri
  (L1C del touch). Con quei voti l'AOSS non può spegnere CX né l'oscillatore (~).
- Linux (`drivers/soc/qcom/rpmh-rsc.c`, `rpmh.c`): l'ultimo core che va giù (notifica CPU PM, o del dominio del cluster)
  chiama `rpmh_flush()`, che scrive nei TCS SLEEP e WAKE i valori in cache di ogni client e poi
  `rpmh_rsc_write_next_wakeup()`: l'istante del prossimo timer, in cicli del contatore di sistema, in
  `RSC_DRV_CTL_TCS_DATA_LO` (`+0x40`) e `_HI` (`+0x38`, bit 31 = valido) della base del DRV, cioè `0x18220040` e
  `0x18220038` (~ indirizzi calcolati). Durante la sospensione di sistema scrive il massimo (nessun risveglio a tempo).
- **Cosa non sappiamo:** i valori votati dall'UEFI. Il port del Mi 9 ha analizzato `RpmhDxe.efi` e non ha trovato un modo
  di sola lettura per sapere quali voti sono stati completati sul DRV ([../rpmh-firmware-evidence.md](../rpmh-firmware-evidence.md)).
  Quindi il set di risveglio non può "ripristinare l'UEFI": deve contenere voti nostri, scelti per essere sufficienti
  (§5, P4).
- Gli indirizzi delle risorse (`cx.lvl`, `mx.lvl`, `xo.lvl`, BCM `MC0`, `SH0`, `ACV`, `MM0`…) si leggono dal cmd-db
  (`qcom,cmd-db@c3f000c`, ✔ FDT), che il nostro `sweet_rpmh.c` (ios-sweet 0062, Mi 9) già interpreta.

### 2.4 AOSS/AOP e i contatori del sonno

- L'AOP (processore sempre acceso) decide il sonno del SoC quando tutti i master dormono: **aosd** (sonno dell'AOSS),
  **cxsd** (CX spento), **ddr** (DDR in self-refresh). I contatori sono nella RAM messaggi dell'AOP: mainline
  `sram@c3f0000`, `qcom,rpmh-stats`, tre record da `+0x48` (commit `47cb6a068409` di Maulik Shah; struttura
  `sleep_stats` = tipo, conteggio, ultimo ingresso, ultima uscita, durata accumulata in `qcom_stats.c`). Il downstream
  del telefono descrive la stessa zona come `qcom,rpm-stats@c300000` con l'offset letto da `0xc3f0004` e
  `qcom,num-records = <3>`; in più `qcom,rpmh-master-stats@b221200`, i conteggi del sonno per master (APSS, modem, ADSP,
  CDSP, SLPI, GPU, display) (✔ FDT). **Sono la prova che il SoC dorme davvero**, e si leggono in sola lettura.
- I sottosistemi remoti (modem, ADSP, CDSP, SLPI) sotto XNU non vengono avviati; i loro DRV dovrebbero essere fermi (~,
  da confermare con i contatori per master). Il canale QMP verso l'AOP (`qcom,qmp-aop@c300000`) serve a Linux per dire
  all'AOP lo stato dei sottosistemi e alcune soglie (?): da capire solo se l'AOSS non scende.

### 2.5 Sorgenti di risveglio: PDC

- Con il cluster e l'AOSS spenti il GIC non vede niente: le interruzioni che devono svegliare passano dal PDC
  (`qcom,pdc-sm8150` a `0xb220000`, ✔ FDT), che le tiene e le ripresenta al GIC. Mappa (mainline `qcom,pdc-ranges`):
  pin 0-93 = SPI 480-573, pin 94-124 = SPI 609-639, pin 125 = SPI 63. Registri (`qcom-pdc.c`, versione 2.7/3.0):
  `IRQ_ENABLE_BANK` a `+0x10`, `IRQ_CFG[n]` a `+0x110` (tipo: livello/fronte, polarità).

| Sorgente | Percorso | Note |
|---|---|---|
| **Tasto power** | PM8150 PON (SID 0, periferica `0x08`, KPDPWR) → arbitro SPMI → **SPI 481 = pin PDC 1** | serve abilitare l'interruzione nella periferica PMIC (registri standard `INT_SET_TYPE` `+0x11`, `INT_POLARITY_HIGH/LOW` `+0x12/0x13`, `INT_LATCHED_CLR` `+0x14`, `INT_EN_SET` `+0x15`), nell'arbitro (abilitazione per APID) e nel PDC |
| **Cavo/caricatore** | PM8150B USBIN (SID 2, `0x1300`, `INT_RT_STS` bit 4) → stesso SPI 481 | stesso percorso; la ricarica continua da sola durante il sonno (✔ [ricarica.md](ricarica.md)) |
| **USB dati** | DWC3: `pwr_event` SPI 130, `hs_phy_irq` SPI 131; `dp_hs_phy_irq` pin PDC 9, `dm_hs_phy_irq` pin 8, `ss_phy_irq` pin 6 | ✔ mainline `sm8150.dtsi` |
| **A tempo** | prossimo timer nel TCS di controllo del RSC (§2.3); in alternativa l'allarme RTC del PM8150 | per i risvegli di iOS a schermo spento |
| GPIO (sensore di Hall dello slider, ecc.) | TLMM → PDC (`wakeup-parent = <&pdc>`) | facoltativo, più avanti |

### 2.6 Cosa tiene sveglio il telefono oggi

| Consumatore | Prova | Effetto | Fase |
|---|---|---|---|
| Polling USB ogni ~1,1 ms | ✔ log | un core non resta mai inattivo più di ~1 ms: sotto la residenza minima di `rail-pc` (3,9-4,5 ms) | P1 (adattivo), poi IRQ |
| Vsync a 60 Hz e notifiche a backboardd anche a schermo spento | ✔ codice | 60 risvegli/s nel kernel + 60 in spazio utente | P1 |
| Touch ogni ~15,5 ms con attese attive | ✔ log/codice | parte di un core occupata di continuo | P1 (fermo a schermo spento) |
| Tasti ogni 20 ms | ✔ codice | 50 risvegli/s | P1 (più lento), P4 (IRQ) |
| Cluster alla frequenza massima (`andrcpu=1`, niente `andrgov`) | ✔ `board.toml` | tensione alta anche in `wfi`; HakiOS su `vayu` stima ~800 mA oltre i 500 mA del PC con le righe più alte | P1 |
| Svuotamento set/way della L1 a ogni idle | ✔ codice | costo a ogni idle (trap? ✔ per HakiOS) e cache fredda al risveglio | P1 |
| Nessun `CPU_SUSPEND`/`CPU_OFF` | ✔ codice | core e cluster sempre alimentati | P2, P4 |
| MDP in autorefresh, DSI attivo, pannello acceso | ✔ su Pixel 4 (ios-flame); ~ qui | ~600 MB/s di lettura dalla DDR a 60 Hz (1080×2340×4 B), DSI in alta velocità | P3 |
| USB: GDSC e 7 clock accesi, link attivo con il PC | ✔ log | CX attivo; con il cavo al PC niente sonno profondo | P4 (prove a cavo staccato) |
| UFS sempre attivo (il driver di ios-sweet non usa hibern8) | ✔ codice; ? capacità `MASK_AUTO_HIBERN8_SUPPORT` | link e PHY UFS sempre accesi | P1 (auto-hibern8, `AHIT` `0x18`) |
| Voti RPMh attivi dell'UEFI, set di sonno vuoto | ~ | AOSS sempre sveglio | P4 |
| Flush del log persistente ogni 10 s | ✔ ios-sweet 0069 | scritture UFS periodiche | lasciato (serve alle prove) |

### 2.7 Come sospende Linux su SM8150 (documentazione)

- Senza `SYSTEM_SUSPEND` il driver PSCI non registra la sospensione su RAM (`psci_init_system_suspend()` controlla
  `PSCI_FEATURES(SYSTEM_SUSPEND)`, ✔ `psci.c`): resta il *suspend-to-idle* (s2idle). Linux congela i processi, sospende i
  dispositivi (che armano le loro interruzioni di risveglio, `enable_irq_wake()` → PDC), poi ogni core entra nello stato
  cpuidle più profondo; in modalità OSI l'ultimo porta giù il dominio del cluster (`0x4100c244`), e la notifica al
  dominio fa scrivere a `rpmh-rsc` i set di sonno/risveglio (con il risveglio a tempo al massimo). L'AOP vede tutti i
  master fermi: aosd, cxsd, DDR in self-refresh.
- Android (kernel 4.14 del telefono) fa lo stesso con `lpm-levels` (lo stato del cluster con `qcom,notify-rpm`) e
  `system_pm`; tiene anche il watchdog APSS armato con `qcom,wakeup-enable`.
- Per noi conta lo schema, non il codice (GPL-2.0, non lo copiamo): **ultimo core giù → flush RPMh → stato del cluster;
  risveglio da PDC**.

## 3. Misurare

### 3.1 Cosa c'è

`andromeda_battery_sample()` (`xnu/andromeda_probe.c`, usato da `sweetbattery=2`) legge in sola lettura via SPMI il
fuel gauge del PM8150B: SOC monotono `0x4009` (0-255), VBATT `0x41A0` (122,07 µV/LSB), IBATT `0x41A2` (488,28 µA/LSB,
positivo in scarica), con doppia lettura contro i campioni a metà. Restituisce mA con il segno di `IOPMPowerSource`
(positivo = in carica) e stampa solo le prime tre volte e poi una ogni 120 chiamate (cioè ogni ~ora, con il servizio
batteria a 30 s).

### 3.2 Il problema del cavo, e quattro modi di aggirarlo

Con il cavo collegato il caricatore alimenta il telefono e IBATT misura solo la corrente netta della batteria (oggi
-37,6 mA, quasi carica): **non è il consumo**.

1. **Ingresso sospeso, log USB acceso (consigliato).** Bit 0 di `USBIN_CMD_IL` (`0x1340`, PM8150B SID 2): è la scrittura
   di `smblib_set_usb_suspend()` del kernel Android (`smb5-lib.c`; registro in `smb5-reg.h`), cioè l'`input_suspend` di
   Android. Il telefono va a batteria, la linea dati USB resta (il VBUS c'è comunque, e ios-sweet 0074 forza già la
   sessione valida nel wrapper Qualcomm) (~). È una scrittura PMIC: va nell'elenco chiuso delle scritture ammesse, con
   conferma dell'utente, durata massima (es. 10 minuti) e ripristino automatico, anche su errore. Rischio: se il kernel si
   blocca con l'ingresso sospeso il telefono resta a batteria fino al riavvio (? se il registro sopravvive a un reset).
2. **Cavo staccato, log persistente.** Il ring di 4 MiB in fondo a `userdata` (ios-sweet 0047, flush ogni 10 s) tiene le
   righe; si rileggono al ricollegamento (il log USB rimanda gli ultimi 64 KiB, se il gadget rienumera: ?) o dal recovery
   con `scripts/pull-log.sh`. È l'unico modo per P4 (con il cavo al PC il SoC non dorme).
3. **Finestre lunghe.** Differenza di SOC in ore: un passo vale ~14,9 mAh, quindi a 50 mA serve almeno mezz'ora per vedere
   due passi. Va bene come conferma, non per confrontare passi vicini.
4. **Misuratore USB-C esterno**, con la batteria piena e il caricatore in fine carica: la corrente d'ingresso
   approssima il consumo (perdite del caricatore comprese). Solo confronti relativi.

### 3.3 Gli altri contatori (tutti in sola lettura)

| Cosa | Come | Dice |
|---|---|---|
| Idle per core | `processor_info(PROCESSOR_CPU_LOAD_INFO)`, già usato dal governatore | % di tempo inattivo |
| Risvegli per core | `processor_info(PROCESSOR_CPU_STAT64)`: `timer_cnt` (in `rtclock_intr`), `ipi_cnt` | risvegli/s |
| Stati PSCI | `PSCI_STAT_COUNT`/`PSCI_STAT_RESIDENCY`, se ci sono (?) | ingressi e tempo per stato, contati dal firmware |
| Sonno del SoC | record aosd/cxsd/ddr a `0xc3f0000 + 0x48`; master stats a `0xb221200` | se e quanto ha dormito l'AOSS |
| Domini e clock | bit 31 dei GDSCR (acceso) e dei CBCR (`CLK_OFF`) di USB, UFS, MDSS | cosa resta acceso |
| MDP | `PP_AUTOREFRESH_CONFIG` del pingpong 0: `0xae71000 + 0x30` (FDT `qcom,sde-pp-off`, Linux `dpu_hw_pingpong.c`) | autorefresh acceso? ogni quanti frame? |
| Watchdog APSS | `0x17c10000` (FDT), bit di abilitazione | conferma che è spento |

Una riga sola ogni 10 s, mediata, per confrontare le fasi; per esempio
`ANDROMEDA PWR: t=600 disp=off ibatt_ma=212 vbat_mv=4120 soc=81 wake/s=c0:950,c1:12,... idle%=c0:41,... pc=0 aosd=0 cxsd=0`.

## 4. Cosa hanno fatto gli altri port

| Port | Sonno e consumo | Fonte |
|---|---|---|
| **ios-sweet** (dipa-studio, Redmi Note 10 Pro, SM7150) | display "spento" = nero con il pannello alimentato (0079: "the panel itself stays powered (AMOLED black pixels are off)"); `SweetCPU`: halt/quiesce/resume non implementati; PSCI `SYSTEM_SUSPEND` assente (misurato) | ✔ patch 0018/0065/0079, `NOTES.md` |
| **ios-flame** (Nick Wylynko, Pixel 4, SM8150) | PSCI 1.1 senza `SYSTEM_SUSPEND` (misurato); l'ABL lascia il pannello command mode in autorefresh, e fare "kick" in autorefresh blocca la pipeline; nessun lavoro sul sonno nei documenti letti | ✔ `NOTES.md`, `HANDOFF.md` |
| **UniXNU-cepheus** (YangQi0408, Mi 9, SM8150) | 0083: a schermo spento un solo azzeramento del framebuffer invece della dissolvenza; il timer vsync resta; "nessuna verifica completa di sospensione e ripresa" | ✔ patch 0083, `docs/gpu-acceptance.md` |
| **ios-raphael** (Gsjsjzhznsz, Redmi K20 Pro, SM8150) | comandi DCS via il DSI0 (`0xae94000`): luminosità `0x51`, display off/on `0x28`/`0x29`, "spegnimento immediato: un fotogramma nero + vero sonno del pannello", in turni di prova (t21/t22); non nella versione stabile | ✔ `README.md`, patch 0098 (senza licenza: solo riferimento) |
| **ios-apollo / project-BadApple** (aTechGeekDev, Mi 10T, SM8250) | "Sleep / standby" allo 0 %: la CPU non sospende mai, e da bloccato il telefono consuma quasi come in uso (README). Misurato a schermo spento: core al 40-70 % e 1,5-2,3 GHz; poi touch a 100 ms a schermo spento, niente notifiche vsync a schermo spento, voto DDR che segue il carico (0153, 0155) | ✔ `README.md`, `NOTES.md` |
| **HakiOS** (OOSEX GANG: shimoku74, vcvkk e altri) | sonno di sistema **disattivato** (`gSleepDisabledFlag = 1`, `syssleep=1` per riattivarlo): `powerd` chiede il sonno pochi secondi dopo il blocco, e `IOCPUSleepKernel()` con un core ne usciva subito in ciclo, con più core si fermava per sempre; idle senza svuotamento set/way; `CPU_OFF` solo per tornare al loader; driver GICv3 con SPI (`gic_v3_irq.c`) | ✔ `xnu/hakios.patch`, `psci_smp.c`, `gic_v3_irq.c` (APSL 2.0) |
| **HakiOS SM8150** (nomorecoolnicknames, POCO X3 Pro) | "la batteria si scarica in fretta"; dal PC (500 mA) 31 % → 18 % in una notte, le righe di frequenza più alte "assorbono ~800 mA più di così" | ✔ `README.md`, `vayu_cpufreq.c` (senza licenza: solo riferimento) |
| **UniXNU** (organizzazione UniXNU) | `sleep-disabled = 1` in `[adt.defaults.u32]` (evb10, elden); "tenere spenti sospensione e power management delle CPU al primo ingresso in XNU"; il PMGR di UniXNU-sheng rifiuta (panic) `enterCPUIdle`, `disableCPUCore` e simili | ✔ `device-records/*.toml`, `ShengPlatformPower.cpp` |
| Linux mainline SM8150 (Danny Lin, Maulik Shah, Bjorn Andersson e altri) | idle dei core e del cluster con domini PSCI in OSI, contatori del sonno RPMh, RSC agganciato al dominio del cluster | ✔ commit citati in [Fonti](#8-fonti) |

**In sintesi:** nei progetti letti nessuno ha il sonno profondo sotto XNU su Qualcomm. Chi ci ha provato con il percorso
di sistema di iOS (HakiOS) si è bloccato in `IOCPUSleepKernel()`; gli altri lo hanno spento. Il lavoro utile da riprendere
come idea (con i crediti) è: meno polling e niente vsync a schermo spento (ios-apollo), il pannello che dorme via DSI
(ios-raphael), `CPU_OFF` e SPI nel GIC (HakiOS, codice APSL compatibile con le nostre patch di XNU), lo schema
"ultimo core → RPMh → cluster" (Linux, solo come documentazione).

## 5. Piano

Regole di sempre (CONTRIBUTING): ogni passo parte spento con un suo parametro di avvio; prove solo con `fastboot boot`;
le scritture fuori dalla sola lettura (PMIC, RPMh, PDC, DSI) vanno in elenchi chiusi e si accendono con la conferma
dell'utente. Nomi proposti: `andrpwr` (sonde e misura), `andridle` (idle e core), `andrdisp` (schermo), `andrsleep`
(standby).

Ordine scelto, e perché diverso da "prima `CPU_SUSPEND` in idle": **spegnere i core 1-7 con `CPU_OFF` a schermo spento
è più semplice dell'idle profondo** (un core spento non ha timer, quindi niente timer di broadcast, niente SPI) e porta
allo stesso risparmio quando il telefono è fermo. Lascia un solo core vivo, il core 0, che diventa l'unico "ultimo core"
per il cluster: P4 si semplifica molto.

### P0. Sonde e misura di partenza

- **Cosa** (`andrpwr=1`, sola lettura): PSCI (`PSCI_VERSION`, `PSCI_FEATURES` di `CPU_SUSPEND`, `CPU_OFF`,
  `SET_SUSPEND_MODE`, `SYSTEM_SUSPEND`, `STAT_COUNT`, `STAT_RESIDENCY`: chiamate SMC di interrogazione, senza effetti);
  `PP_AUTOREFRESH_CONFIG`; GDSC/CBCR di USB, UFS, MDSS; capacità UFS (`CAP` bit 23) e `AHIT`; PDC `IRQ_ENABLE_BANK` e
  `IRQ_CFG[1]`; record aosd/cxsd/ddr e master stats; watchdog APSS; ogni 10 s la riga `ANDROMEDA PWR` con media di IBATT
  e risvegli per core.
- **Misure** (`andrpwr=2`, con l'unica scrittura `USBIN_CMD_IL` bit 0 per 10 minuti, se l'utente la approva): consumo a
  schermo acceso sulla schermata di blocco e a schermo spento, con `andrcpu=1` e con `andrcpu=1 andrgov=1`.
- **Prove senza codice nuovo:** (a) staccare e riattaccare il cavo durante un avvio normale: il log USB riprende?
  (b) telefono bloccato, cavo staccato, 10 minuti: `powerd` chiede il sonno (panic `SWEET CPU: CPU halt/suspend is not
  implemented` nel log persistente)? Se sì, mettere `sleep-disabled = 1` in `/defaults` dell'ADT (come UniXNU) fino a P5.
- **Fatto quando:** righe `ANDROMEDA PWR: psci=1.1 osi=? stat=? autorefresh=… pdc1=… aosd=… cxsd=…` e una tabella di
  mA per stato in questo documento.
- **Rischi:** quasi nulli per le letture (l'MMIO dell'AOP e del PDC lo legge anche Linux); la sospensione dell'ingresso
  vedi §3.2.

### P1. Idle pulito e meno risvegli

- **Cosa** (`andridle=1` e parametri dei driver):
  - niente `platform_cache_idle_enter()` su SWEET (stessa scelta di HakiOS);
  - `andrgov=1` di default; a schermo spento tutti i cluster (e la L3, banco OSM `l3` a `0x18321000`) all'indice più basso;
  - framebuffer: a schermo spento timer vsync fermo e nessuna notifica (come ios-apollo 0153), ripartenza al `RequestPowerChange(1)`;
  - touch fermo a schermo spento (o a 100 ms, come ios-apollo 0155); tasti a 50-100 ms a schermo spento;
  - USB adattivo: 1 ms quando c'è traffico, 20-50 ms dopo un secondo di silenzio (il PC ritenta da solo);
  - UFS auto-hibern8 (`AHIT`, `0x18` dell'UFSHCI) se la capacità c'è, con un tempo di 5-10 ms (passo separato).
- **Fatto quando:** risvegli/s per core e mA a schermo spento, prima e dopo ogni voce, nella riga `ANDROMEDA PWR`.
- **Rischi:** USB più lento quando riparte; touch che si perde il primo tocco dopo l'accensione; nessun rischio
  hardware (sono solo tempi e frequenze già usati).

### P2. Core spenti

- **P2a, hotplug** (`andridle=2`): a schermo spento `processor_exit()` dei core 7, 6, 5, 4 (poi 3, 2, 1);
  `SweetCPU::quiesceCPU()` → PSCI `CPU_OFF`; a schermo acceso `processor_start()` → `startCPU()` (secondo `CPU_ON`
  ammesso, `initialized` azzerato) → `initCPU(false)` (senza panic: rifà GIC, timer, SGI di prova). `AFFINITY_INFO`
  per controllare che il core sia davvero spento. Il percorso di XNU (`cpu_sleep()` → `PE_cpu_machine_quiesce()`) c'è già.
- **P2b, idle profondo** (`andridle=3`, a schermo acceso): sul modello di Apple, `powergate_latency` = latenza d'uscita
  (909/1461 µs) e, al posto di `wfi` in `cpu_idle()` per SWEET, `CPU_SUSPEND(0x40000004)` quando il prossimo timer
  (`rtcPop`) è oltre la residenza minima (3934/4488 µs). Punto d'ingresso come `sweet_secondary_entry` (0065), che
  porta a `resume_idle_cpu`; al ritorno: `ICC_*`, comparatore del timer, `timer_resync_deadlines()`. Serve il timer di
  broadcast (frame 0 a `0x17c21000`, SPI 8): quindi prima un livello minimo di SPI nel GIC (abilitazione, gruppo 1,
  instradamento; modello: `gic_v3_irq.c` di HakiOS). Prima un solo core (il 7) con il frame tutto suo, poi tutti.
- **Fatto quando:** P2a `ANDROMEDA IDLE: cpu7 off (psci 0) … on (cpu_on 0, sgi ok)` per 100 cicli di schermo senza
  errori, e mA a schermo spento; P2b `ANDROMEDA IDLE: cpu7 pc=N fail=0 late_us_max=…` e, se c'è, `PSCI_STAT_COUNT`
  in crescita.
- **Rischi:** core che non torna (`CPU_ON` dopo `CPU_OFF`) → riavvio; timer persi in P2b (un core che dorme oltre la
  scadenza: per questo il broadcast c'è dal primo giorno e si registra il ritardo massimo); IPI verso un core spento;
  `CPU_SUSPEND` che torna con `DENIED`/`INVALID_PARAMETERS` (va trattato come un `wfi`). La pulizia delle cache per lo
  spegnimento la fa il firmware PSCI.

### P3. Schermo spento davvero

- **P3a:** il fotogramma nero c'è già; il resto è in P1 (vsync fermo).
- **P3b, autorefresh** (`andrdisp=1`): dopo il nero e un fotogramma di attesa, leggere e salvare
  `PP_AUTOREFRESH_CONFIG`, poi spegnerne il bit di abilitazione; il pannello command mode tiene l'immagine nella sua
  memoria e la DDR smette di essere letta a 60 Hz. Riaccensione: rimettere esattamente il valore salvato, **mai un
  "kick"** (ios-flame: autorefresh + kick blocca la pipeline).
- **P3c, il pannello dorme** (`andrdisp=2`): comandi DCS sul DSI0 (`0xae94000`, FDT `qcom,mdss_dsi_ctrl0`), quelli del
  device tree del telefono per l'EA8076: spegnimento `05 … 28` poi `05 … 10` con 120 ms di attesa
  (`qcom,mdss-dsi-off-command`, modo LP); accensione `11` (sleep out), 120 ms, `29`. Il meccanismo (DMA comandi del
  controller DSI) è documentato in Linux drm/msm ed è stato fatto sullo stesso SoC da ios-raphael (0098): lo riscriviamo,
  non lo copiamo (nessuna licenza). Stesso canale della luminosità (`0x51`, ROADMAP voce 9).
- **P3d (più avanti):** PLL e PHY del DSI, GDSC dell'MDSS, alimentazioni del pannello (vci 3,0 V, vddio 1,8 V): è un
  driver del display intero (sequenza di accensione completa del device tree). La vddio sul GPIO37 è condivisa con il
  touch: non si spegne.
- **Fatto quando:** `ANDROMEDA DISP: off autorefresh=0x… dcs28=ok dcs10=ok` / `on dcs11=ok dcs29=ok`, immagine giusta
  dopo 100 cicli, e mA a schermo spento in calo rispetto a P1.
- **Rischi:** pannello che non torna (serve il riavvio); errore DSI che blocca il controller; il TE che si ferma con il
  pannello in sonno (per questo l'autorefresh si spegne prima); dopo lo sleep out il pannello potrebbe volere parte della
  sequenza di accensione del fornitore (?).

### P4. Sorgenti di risveglio, cluster e voti RPMh: standby

Il modello è il *suspend-to-idle* di Linux, non il sonno di sistema di iOS: a schermo spento iOS resta "sveglio", ma il
core 0 è l'unico acceso e, quando non ha niente da fare, porta giù tutto il cluster. Non serve che i nostri driver
sappiano gestire le notifiche di sonno di `IOPMrootDomain`.

- **P4a, risveglio** (`andrsleep=1`): SPI 481 nel GIC e pin 1 nel PDC; interruzione KPDPWR abilitata nel PON e
  nell'arbitro; il gestore azzera il latch PMIC (`INT_LATCHED_CLR`) e passa l'evento al driver dei tasti. Prova:
  `CPU_SUSPEND(0x40000004)` del core 0 con i core 1-7 spenti (P2a), svegliato solo dal tasto.
- **P4b, cluster** (`andrsleep=2`): se `PSCI_FEATURES` dice OSI, `SET_SUSPEND_MODE(OSI)`; altrimenti modalità coordinata
  dalla piattaforma, che con i core 1-7 in `CPU_OFF` dovrebbe accettare la richiesta del core 0 (~, da provare: Danny Lin
  ha visto lo stato piatto fallire con gli altri core accesi). Il core 0 chiede `0x4100c244` solo se il prossimo timer è
  oltre ~10 ms (residenza 9987 µs), dopo aver fermato il polling USB.
- **P4c, voti RPMh** (`andrsleep=3`): dal cmd-db gli indirizzi di `xo.lvl`, `cx.lvl`, `mx.lvl` e dei BCM; all'avvio voti
  attivi nostri, alti e sicuri (per non sotto-alimentare clock lasciati accesi dall'UEFI); nel set di sonno XO a 0, CX/MX
  in ritenzione, BCM a 0 tranne quelli da tenere vivi; nel set di risveglio gli stessi voti attivi; prima dello stato del
  cluster il prossimo timer in `RSC_DRV_CTL_TCS_DATA_LO/HI`. Prima prova con il solo XO, poi CX/MX, poi la DDR.
- **P4d, cavo e USB:** USBIN come sorgente (stesso SPI); pin PDC 6/8/9 del PHY per il collegamento USB. Le prove di
  consumo si fanno a cavo staccato, con il log persistente scritto (flush forzato) prima di ogni standby.
- **Fatto quando:** aosd e cxsd che crescono (contatori dell'AOP); tasto power e inserimento del cavo che svegliano entro
  1 s con lo schermo che torna; mA medi a schermo spento con l'obiettivo di stare sotto ~53 mA (tre giorni).
- **Rischi:** **blocchi al risveglio** (si esce solo con 10-15 s di tasto power, reset del PMIC): ogni passo con un
  limite di cicli e un contatore nel log persistente; **voti RPMh sbagliati** → SoC che si pianta o si riavvia: valori
  presi dal mainline e dall'evidenza del Mi 9, uno per volta; **log USB perso** (il SoC non dorme con il cavo al PC):
  log persistente + rilettura; **watchdog**: l'APSS è spento (§0), il watchdog del caricatore morde senza fermare la
  ricarica su questo esemplare ([ricarica.md](ricarica.md)), il watchdog del sonno di `IOPMrootDomain` non entra in gioco
  finché non si usa il sonno di sistema; **risvegli troppo frequenti** di iOS (timer dei demoni) che annullano il
  guadagno: si contano in P1.

### P5. Dopo

- Sonno di sistema vero di iOS: `SweetCPU::haltCPU()` con `CPU_OFF` per i secondari e, sul core 0, lo stesso standby di
  P4 dentro `arm64_prepare_for_sleep()` (come fa `APPLEVIRTUALPLATFORM` con PSCI); risveglio a tempo per il dark wake
  (TCS di controllo o allarme RTC); ogni driver nostro deve fermarsi e ripartire con il power management di IOKit. Prima
  di questo, `sleep-disabled = 1` resta la difesa (HakiOS ha visto il blocco).
- Idle profondo dei core a schermo acceso (P2b esteso a tutti i core, con il broadcast per più core) e cluster anche
  durante l'uso.
- Risveglio con lo slider (sensore di Hall via PDC), AOD con i comandi `doze` del pannello.

## 6. Riepilogo per fase

| Fase | Parametro | Scritture | Riga di prova | Misura | Rischio principale |
|---|---|---|---|---|---|
| P0 | `andrpwr=1/2` | solo `0x1340` bit 0 (con conferma) | `ANDROMEDA PWR: psci=… osi=… aosd=…` | mA di partenza per stato | quasi nullo |
| P1 | `andridle=1`, `andrgov=1`, parametri driver | nessuna nuova (frequenze OSM già in uso) | risvegli/s per core | mA a schermo spento | latenze di USB e touch |
| P2a | `andridle=2` | PSCI `CPU_OFF`/`CPU_ON` | `ANDROMEDA IDLE: cpuN off/on` | mA a schermo spento | core che non torna |
| P2b | `andridle=3` | PSCI `CPU_SUSPEND`, GIC SPI, timer a memoria | `pc=N fail=0 late_us_max=…` | mA a schermo acceso | timer persi, blocchi |
| P3 | `andrdisp=1/2` | MDP autorefresh, DSI DCS | `ANDROMEDA DISP: off … on …` | mA a schermo spento | pannello che non torna |
| P4 | `andrsleep=1..3` | PMIC (interruzioni), PDC, GIC, RPMh | aosd/cxsd che crescono | mA medi in standby | blocchi al risveglio, voti RPMh |
| P5 | — | — | `IOCPUSleepKernel exit` | ore di standby | il percorso di sonno di iOS |

## 7. Domande aperte

- PSCI di `andromeda`: versione, OSI, formato esteso, `STAT_COUNT`/`STAT_RESIDENCY`? (P0)
- L'hypervisor Qualcomm intrappola le istruzioni set/way, come scrive HakiOS? (P1: tempo di `FlushPoU_Dcache()`)
- L'autorefresh dell'MDP è acceso, e con che conteggio di frame? (P0)
- Il pin PDC 1 è già abilitato dal firmware? L'interruzione KPDPWR è già configurata dall'UEFI? (P0)
- Il gadget USB rienumera dopo un distacco? (P0)
- `powerd` chiede il sonno a batteria, cavo staccato? (P0)
- `USBIN_CMD_IL` sopravvive a un reset? (da leggere all'avvio dopo un test interrotto)
- Il set di risveglio deve contenere qualcosa per i clock lasciati accesi dall'UEFI (MDSS, USB, UFS)? Quali livelli di CX
  servono? (P4c, partendo dai voti di Linux per gli stessi clock)
- Il pannello EA8076 dopo `sleep in`/`sleep out` senza reset vuole di nuovo i comandi del fornitore? (P3c)
- L'AOSS ha bisogno di messaggi QMP sullo stato dei sottosistemi remoti per scendere? (solo se aosd resta a 0)

## 8. Fonti

Crediti: tutto quello che segue è stato **consultato**; nessun codice copiato in questo documento. Le idee riprese nel
piano (indicate sopra con l'autore) sono di: Danny Lin (stati e composizione `0x4100c244`, cluster con OSI),
Maulik Shah e i manutentori Linux di `rpmh-rsc` (flush all'ultimo core, prossimo risveglio), OOSEX GANG/HakiOS (niente
set/way in idle, sonno di sistema disattivato, `CPU_OFF`, SPI nel GIC), aTechGeekDev/ios-apollo (meno risvegli a
schermo spento), Gsjsjzhznsz/ios-raphael (DCS del pannello su SM8150), Nick Wylynko/ios-flame (PSCI 1.1, autorefresh),
dipa-studio/ios-sweet (piattaforma SWEET), YangQi0408/UniXNU-cepheus (0083, evidenza RPMh), UniXNU (`sleep-disabled`).
`CREDITS.md` non è stato modificato in questo cambiamento (per indicazione); la riga generica "Ricerche consultate"
copre già `docs/research/` e rimanda a questa sezione. Quando un'idea diventerà codice, la voce va aggiunta lì nello
stesso cambiamento.

- **Apple**, XNU [`apple-oss-distributions/xnu`](https://github.com/apple-oss-distributions/xnu) `xnu-8796.141.3`
  (APSL 2.0; copia locale, `config/MasterVersion` 22.6.0): `osfmk/arm64/cpu.c` (`cpu_idle`, `cpu_idle_exit`,
  `cpu_sleep`, `ml_arm_sleep`, `cpu_machine_idle_init`, `cpu_timebase_init`), `osfmk/arm64/machine_routines_asm.s`
  (`arm64_prepare_for_sleep`), `osfmk/arm64/machine_routines.c` (`ml_processor_register`, `fiq_context_init`),
  `osfmk/arm64/caches_asm.s` (`FlushPoU_Dcache`, `DCACHE_SET_WAY`), `osfmk/arm64/start.s` (`resume_idle_cpu`),
  `osfmk/arm/caches.c`, `osfmk/arm/rtclock.c` (`SetIdlePop`), `osfmk/arm/arm_init.c` (`arm_init_idle_cpu`),
  `osfmk/arm/cpu_common.c`, `osfmk/arm/cpu_data_internal.h`, `osfmk/kern/processor.c`, `osfmk/kern/sched_prim.c`,
  `iokit/Kernel/IOCPU.cpp`, `iokit/Kernel/arm/AppleARMSMP.cpp`, `iokit/Kernel/IOPMrootDomain.cpp`,
  `iokit/Kernel/IOServicePMPrivate.h`, `iokit/Kernel/IOPlatformExpert.cpp`, `config/MASTER*`.
- **ios-sweet**, dipa-studio: [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) `45263bee` (MIT):
  `xnu/patches/0004-sweet-machine-config.patch`, `0006-…-master-config…`, `0017-…-virtual-timer-irq`,
  `0018-sweet-boot-cpu-driver.patch`, `0046-sweet-iosurface-iomfb.patch`, `0047`, `0056`, `0060-sweet-keys-touch-drivers.patch`,
  `0062-sweet-touch-power-battery.patch`, `0065-sweet-smp-psci.patch`, `0069-sweet-ufs-faster.patch`,
  `0073-sweet-usb-device-ep0.patch`, `0074`, `0075-sweet-usb-ecm.patch`, `0077-sweet-usb-live-log.patch`,
  `0078-sweet-smp-8-cores.patch`, `0079-sweet-display-power.patch`, `0081-sweet-usb-link-state.patch`; `NOTES.md`.
- **ios-flame**, Nick Wylynko ([NWylynko](https://github.com/NWylynko)): [`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame)
  `745c4341` (MIT): `NOTES.md` (PSCI 1.1, GIC, watchdog), `HANDOFF.md` (autorefresh dell'ABL, 20 ottobre 2026),
  `xnu/patches/0083-flame-display-keys-watchdog.patch`.
- **UniXNU-cepheus**, YangQi0408: [`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus) (`master`
  `35561c07`, ramo `feat/adreno640`): patch 0083 (prestazioni, display spento, tasti PON), `docs/gpu-acceptance.md`,
  `docs/rpmh-firmware-evidence.md` (copia nel nostro `docs/`); il nostro `xnu/cepheus_touch.c`, `xnu/cepheus_soc.c`.
- **ios-raphael**, Gsjsjzhznsz: [`Gsjsjzhznsz/ios-raphael`](https://github.com/Gsjsjzhznsz/ios-raphael) `1da7f473`
  (nessuna licenza dichiarata: solo riferimento): `README.md`, `xnu/patches/0098-raphael-display-brightness-off.patch`;
  progetto successivo [`Gsjsjzhznsz/hackios-xnu-kernel`](https://github.com/Gsjsjzhznsz/hackios-xnu-kernel) `94c36a1b`.
- **ios-apollo / project-BadApple**, aTechGeekDev: [`aTechGeekDev/ios-apollo`](https://github.com/aTechGeekDev/ios-apollo)
  `1f75f381` (MIT): `README.md` (tabella "Sleep / standby", "Power and battery"), note di lavoro (xnu 0153, 0155).
- **HakiOS**, OOSEX GANG (shimoku74, vcvkk e collaboratori):
  [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`
  (kernel APSL 2.0, resto MIT): `xnu/hakios.patch` (hunk di `iokit/Kernel/IOPMrootDomain.cpp` con `gSleepDisabledFlag`
  e `syssleep`, hunk di `osfmk/arm64/cpu.c` su `platform_cache_idle_enter`, `CPU_OFF` in `start.s`),
  `xnu/files/osfmk/arm64/psci_smp.c`, `xnu/files/osfmk/arm64/gic_v3_irq.c`, `xnu/files/iokit/Kernel/GenericARMPE.cpp`,
  `docs/boot-args.md`.
- **HakiOS SM8150**, nomorecoolnicknames: [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150)
  `3e7be137` (nessuna licenza: solo riferimento): `README.md`, `kernel/bsd/dev/vayu_cpufreq.c` (nota sulla porta SDP).
- **UniXNU**: [`UniXNU/UniXNU`](https://github.com/UniXNU/UniXNU) (copia locale `10edc1e`, oggi `b9365195`; BSD-2-Clause-Patent):
  `docs/porting/power.md`, `docs/.vitepress/device-records/evb10.toml`, `elden.toml`, `cas.toml`;
  [`UniXNU/UniXNU-sheng`](https://github.com/UniXNU/UniXNU-sheng) `ce0c600f` (rponeawa, bluseliu50 e collaboratori):
  `package/ShengPlatformPower.cpp`, `package/CpuFreqProbe.cpp`.
- **Linux mainline** ([`torvalds/linux`](https://github.com/torvalds/linux) `a91337fb`, GPL-2.0, solo documentazione):
  `drivers/soc/qcom/rpmh-rsc.c`, `rpmh.c`, `rpmh-internal.h`, `qcom_stats.c` (Linux Foundation, Qualcomm Innovation
  Center); `drivers/irqchip/qcom-pdc.c`, `irq-gic-v3.c`; `drivers/cpuidle/cpuidle-psci.c`, `cpuidle-psci-domain.c`
  (Ulf Hansson e altri); `drivers/firmware/psci/psci.c`; `drivers/ufs/core/ufshcd.c`, `drivers/ufs/host/ufs-qcom.c`,
  `include/ufs/ufshci.h`; `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_pingpong.c`, `catalog/dpu_5_0_sm8150.h`;
  `drivers/clocksource/arm_arch_timer.c`, `arm_arch_timer_mmio.c`; `arch/arm64/boot/dts/qcom/sm8150.dtsi`.
  Commit: `81188f585d02` e `b2e3f897684c` (Danny Lin, stati PSCI e idle profondo del cluster, dicembre 2020 - gennaio 2021,
  applicati da Bjorn Andersson), `17ac8af678b6` (Maulik Shah, configurazione TCS), `47cb6a068409` (Maulik Shah,
  contatori del sonno RPMh), `2ffa0ca4d37a` (Maulik Shah / Ulf Hansson, `apps_rsc` nel dominio del cluster). Discussione
  LKML "[PATCH 2/3] arm64: dts: qcom: sm8150: Add PSCI idle states" (Danny Lin, Bjorn Andersson, 20 dicembre 2020 -
  4 gennaio 2021).
- **Kernel Qualcomm downstream** (copia OnePlus, GPL-2.0, solo documentazione): `smb5-reg.h` (`USBIN_CMD_IL_REG`,
  `USBIN_SUSPEND_BIT`), `smb5-lib.c` (`smblib_set_usb_suspend`).
- **Dati del telefono** (fuori da git): `phone/<seriale>/live-fdt.dts` (`qcom,lpm-levels`, `psci`, `apps_rsc`, `disp_rsc`,
  `system_pm`, `pdc`, `arm,armv7-timer-mem`, `qcom,rpm-stats`, `qcom,rpmh-master-stats`, `qcom,wdt`, `qcom,qmp-aop`,
  `qcom,mdss_mdp`, `qcom,mdss_dsi_ctrl0`, pannello `qcom,mdss_dsi_samsung_fhd_ea8076_cmd`, batteria);
  `phone/logs/merged-20261106-174054.log`.
- **Questo repository**: `xnu/andromeda_probe.c` (`andromeda_battery_sample()`), `xnu/andromeda_cpufreq.c`,
  `xnu/cepheus_soc.c`, `board.toml`, `scripts/pull-log.sh`, [ricarica.md](ricarica.md).
