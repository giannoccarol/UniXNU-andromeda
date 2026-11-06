# GPU Adreno 640 sotto XNU su `andromeda`: stato dell'arte e piano

Ricerca del 6 novembre 2026, solo lettura: nessun comando sul telefono, nessun codice dei repository scaricati eseguito,
nessuna modifica allo stato git. I valori del Mi Mix 3 5G vengono dal device tree attivo (`phone/<seriale>/live-fdt.dts`,
fuori da git) e dai log in `phone/logs/`; gli altri fatti dai file citati in [Fonti](#10-fonti).

Legenda della certezza, usata in tutto il documento:

- **✔** letto nel sorgente, nei log, o misurato da altri su hardware uguale (SM8150 + Adreno 640);
- **~** dedotto: coerente con più indizi, non ancora visto su `andromeda`;
- **?** da verificare.

## Sommario

- **Esiste già un port con l'accelerazione GPU funzionante sulla nostra stessa base.** È
  [`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame) (Pixel 4): stesso SoC (SM8150), stessa Adreno 640
  (chip id `0x06040001`), stesso kernel ios-sweet xnu-8796 `SWEET` e stesso userland iOS 16.7.16 (20H392) dell'iPhone X.
  Lì backboardd, SpringBoard e le app disegnano con Metal sulla GPU: frontend di UniXNU → metal2vulkan → Turnip di Mesa
  → user client nel kernel. Ci sono screenshot nel repo; un secondo port (`ios-sunfish`, Adreno 618) ha replicato lo stack
  in modo indipendente. Licenza MIT/APSL. **Né il Mi 9 né noi lo avevamo ancora considerato.**
- **Il Mi 9 (`feat/adreno640`) non ha ancora acceso la GPU.**
  - Ha circa 6.500 righe di moduli GPU e 2.700 di test, quasi tutti indipendenti dal dispositivo.
  - Sul telefono ha verificato solo sonde in sola lettura (CX spento, tabelle ARC), la prova a tempo delle frequenze CPU
    e l'allocazione di memoria non-cacheable, dopo aver corretto un bug di XNU sopra i 4 GiB (patch 0088).
  - I suoi blocchi sono quasi tutti regole di prudenza che si è dato da solo (chi possiede i voti RPMh, prova del canale
    SCM), non limiti dell'hardware: ios-flame e HakiOS accendono la stessa GPU senza quelle prove.
- **HakiOS**: `vayu_adreno.c` (POCO X3 Pro, SM8150, A640) è un'implementazione completa e coerente con Linux. Però:
  - il funzionamento sull'hardware è solo dichiarato;
  - il repository non ha licenza;
  - lo userland passa per OpenGL ES (Mesa freedreno + iniezione in EAGL con indirizzi fissi di iOS 16.5).
- **UniXNU**:
  - il driver kernel è a7xx (Adreno 740) nello strato hardware;
  - ABI, user client, gestione memoria, SMMU 4K, Turnip "darwin" e frontend Metal sono riusabili;
  - punta a iPadOS 17/26/27 con i kext Apple: per iOS 16 conviene prendere la versione già adattata da ios-flame.
- **Classificazione**: l'Adreno 640 è **a6xx gen2** in Mesa, non gen1 (✔ `freedreno_devices.py`).
- **Firmware necessari**: `a640_gmu.bin`, `a630_sqe.fw` e `a640_zap.mdt` + `.b00`–`.b02` (o `a640_zap.elf`), in
  `/vendor/firmware` del telefono (partizione `vendor`, intatta). Vanno estratti dal nostro telefono o dalla ROM ufficiale
  e dati al kernel come proprietà dell'ADT, **mai in git**.
- **Il miglioramento visibile più rapido non è la GPU: sono le frequenze della CPU.** Oggi i core grandi girano a
  711/827 MHz invece di 2419/2841 MHz. Con le frequenze piene, ios-flame misurava 8–25 fps in software; con la GPU, dopo
  molte ottimizzazioni, ne ottiene 19–26.
  - Fase 1 del piano: frequenze CPU, ritmo del vsync e un `IOAcceleratorES` vuoto che ferma il polling di backboardd.
  - Fasi 2–7: il port di ios-flame, adattato e protetto con le salvaguardie del Mi 9, da fare insieme al Mi 9 (stesso SoC,
    stesso pannello, stesso kernel).

---

## 1. Il lavoro del Mi 9 (`origin/feat/adreno640`)

Ramo letto a `93cde1fd` (7 novembre 2026, 00:17 +0800, YangQi0408), 144 commit dopo la nostra base `35561c0`:
216 file, +14.769 righe. La documentazione è in cinese: `docs/cpu-gpu.md`, `gpu-acceptance.md` (1.276 righe di
diario), `gpu-one-shot.md`, `gpu-reference-projects.md`, `zap-platform-contract.md`, `rpmh-firmware-evidence.md`,
`scm-static-evidence.md`, `upstream-sync.md` e `panic-fastboot.md`. I messaggi dei commit hanno solo il titolo.

### 1.1 Cosa è implementato

76 moduli `xnu/cepheus_*` (~6.565 righe) e 64 test `xnu/test_*` (~2.668 righe), più script di controllo con i sorgenti
Android (`scripts/test-*-source.py`), sanitizer (ASan/UBSan) e CI. Per area:

| Area | File | Cosa fa |
|---|---|---|
| Alimentazione e clock | `cepheus_gpu_power.h`, `cepheus_gpu_power_txn.h`, `cepheus_arc*.h`, `cepheus_bcm.h` | Lettura del cmd-db (livelli ARC `cx.lvl`/`mx.lvl`/`gfx.lvl`, BCM). Transazione GDSC CX + RCG GMU a 200 MHz + clock, ma **solo in mock** |
| GMU | `cepheus_gmu_{firmware,loader,upload,upload_mmio,start,workspace,bringup,mmio,oob,slumber,park,retire_mmio}`, `cepheus_gpu_pdc.h`, `cepheus_pdc_mmio.h`, `cepheus_qmp_*` | Parsing a blocchi di `a640_gmu.bin`, upload in ITCM/DTCM, sequenze PDC/RSC A640v2, avvio del CM3 (`0xBABEFACE`), OOB keepalive, slumber/park, ACD via QMP |
| HFI | `cepheus_hfi*.h`, `cepheus_gmu_hfi_link.c` | Code HFI v2, PERF/BW table, CORE_FW_START, START, voto DCVS, coda di debug ed errori |
| SMMU | `cepheus_smmu_{plan,install,geometry,mmio,detach}.h`, `cepheus_gx_smmu_plan.h`, `cepheus_gpu_vm*.h` | Contesti GMU (SID 4/5, mask 0x400) e GPU (SID 0, 0x401), tabelle 4K a 39 bit, con controlli su slot e ASID |
| GX e CP | `cepheus_gx_{core,gbif,hwcg,llc,perfcounter,va_layout}.h`, `cepheus_cp_*`, `cepheus_sqe.h`, `cepheus_a640_pm4.h`, `cepheus_gpu_ring.h`, `cepheus_gpu_submit.h` | `a6xx_start` in sottoblocchi, CP_PROTECT, ring + SQE con CP fermo, ME_INIT, PM4 di prova (CP_MEM_WRITE + CACHE_FLUSH_TS) |
| ZAP e SCM | `cepheus_scm_discovery.h`, `cepheus_cp_unsecure_gate.h`, `scripts/inspect-zap-layout.py` | Analisi MDT/ELF della ZAP; ponte SMC nativo (`cepheus_gpu_scm_probe_once`) **non agganciato**; il CP non viene sbloccato senza una ZAP autenticata davvero |
| Memoria | `cepheus_gpu_memory.cpp`, patch `0087`/`0088` | Pool GPU dentro la finestra gestita da pmap, mappature NC verificate; **correzione di `pmap_sync_page_attributes_phys`**, che troncava gli indirizzi fisici sopra 4 GiB (bug di ios-sweet, trovato anche da ios-flame e Pong) |
| Temperatura | `cepheus_gpu_tsens.h`, `cepheus_gpu_thermal.h` | Sensori GPU (TSENS0 s15, TSENS1 s11 sul Mi 9) e blocco termico con limite di durata |
| CPU | `cepheus_soc.c` (`cephsoc`, `cephboost`) | Sonda OSM e prova di 120 s a 1,4 GHz |

I parametri di avvio che il codice legge sono `cephsoc`, `cephboost`, `cepharc`, `cephfuse`, `cephgx` e `cephmem`. In
`cepheus_gpu_compile.c` c'è scritto esplicitamente che i moduli GPU non hanno "startup hook o backend MMIO": a parte le
sonde e le prove di memoria, **nessuna riga di questo codice tocca la GPU**.

### 1.2 Cosa è stato verificato sul telefono (Mi 9)

| Data | Prova | Risultato | Fonte |
|---|---|---|---|
| ~01/11 | Sonda OSM | Silver 1632 MHz, Gold **710,4**, Prime **825,6** MHz (indice 0 della LUT), L3 1536 | `cpu-gpu.md`, run 41291850807 ✔ |
| ~01/11 | `cephboost`: Gold e Prime a 1,4016 GHz per 120 s, stessi mV, poi ripristino | Frequenze misurate 1402 MHz, ripristino ok | run 41292760384 ✔ |
| 02/11 | Sonda GPU in sola lettura (d336276) | `CX_GDSCR=GX_GDSCR=0x222001`, `CX_HW_CTRL=0x5ff4` (bit31=0, **CX spento**), clock GMU/AHB/CXO spenti, `gmu_blockers=0x7a` | `cpu-gpu.md` ✔ |
| 06/11 | Tabelle ARC dal cmd-db | CX raw `0/16/64/128/192/256/384/416`, senza un 48 esatto → `config=-1` (in correzione con `b04b259`, non ancora provata) | `gpu-acceptance.md` ✔ |
| 06/11 | `cephmem=1` con la correzione 0088 | Buffer NC allocato, mappato, pattern ok e rilasciato a PA > 4 GiB; niente panic | `gpu-acceptance.md` ✔ |
| 07/11 | `cephmem=2`: sei buffer del workspace GMU | ok, ma **senza installare l'SMMU, senza DMA e senza avviare il GMU** | `gpu-acceptance.md` ✔ |

**Mai provato sul telefono**: scritture di alimentazione, avvio del GMU, HFI, SMMU, chiamate SCM/PAS, CP, comandi GPU.
Lo dice anche il loro `gpu-one-shot.md`: "非本轮真机结果" (nessun risultato nuovo sul telefono).

### 1.3 Cosa li blocca

Elenco di `gpu-one-shot.md` e `gpu-acceptance.md`, confrontato con chi la stessa GPU l'ha già accesa:

| Blocco dichiarato | Stato | Come lo affrontano ios-flame e HakiOS |
|---|---|---|
| GDSC CX e clock GMU spenti | misurato ✔ | È lo stato normale dopo il bootloader. Entrambi tolgono `SW_COLLAPSE` al GDSC CX (GPUCC+`0x106c`), accendono i CBCR e impostano l'RCG del GMU (ios-flame su XO, HakiOS su GPLL0/3 a 200 MHz) ✔ |
| "Eredità" dei voti ARC/RPMh dell'apps DRV (di chi sono, e non si possono abbassare) | policy, irrisolto | Nessuno dei due vota ARC dalla CPU. I rail restano quelli lasciati dal bootloader; i livelli vanno al GMU via HFI PERF_TABLE, e il GX lo accende il GMU con OOB GPU_SET ✔ |
| Canale SCM per PAS13 (la ZAP) | ponte compilato, mai chiamato | `smc #0` diretto con SMCCC SIP: INIT_IMAGE, MEM_SETUP, AUTH_AND_RESET. Su ios-flame tutte e tre restituiscono 0 ✔ |
| Memoria PAS, XPU, banda crypto | non implementato | ios-flame scrive i segmenti nella regione `pil_gpu_region` (no-map) e non vota la banda crypto: funziona ✔ |
| Possesso reale di SMMU, DDR e LLCC; arresto verificato | policy | ios-flame sceglie a runtime un SMR e un context bank liberi. Non configura l'LLCC. Vota DDR e NoC al massimo solo dalla patch 0122 ✔ |
| Metal/IOAccelerator | assente | ios-flame: SweetAccelerator + SweetMetal2 (§4) ✔ |

L'approccio del Mi 9 è "fail-closed": nessuna scrittura senza prova di possesso. Questo ha prodotto risultati veri (il
bug 0088, il blocco termico, il rifiuto delle ZAP false del tipo "TAKEN TO MEAN"), ma finora nessun pixel. Le loro
salvaguardie (temperatura, PA a 64 bit, interruzione dopo un numero limitato di passi, niente ZAP finta) vanno tenute e
applicate **sopra** una sequenza già dimostrata, invece di aspettare prove formali che nessun port SM8150 ha avuto.

### 1.4 Cosa vale anche per `andromeda` e cosa è del Mi 9

`andromeda` è SM8150 v2.2 come il Mi 9, e l'FDT attivo ha gli stessi nodi GPU (✔ `live-fdt.dts`):

- `kgsl-3d0@2C00000`, `qcom,chipid = <0x6040001>`;
- `gmu@0x2C6A000`, `kgsl-smmu@0x02ca0000`;
- `pil_gpu_region` a `0x98f15000` (0x2000, no-map), `qcom,kgsl-hyp` con PAS 13 e `firmware-name "a640_zap"`;
- `cmd-db@c3f000c`;
- frequenze 257/345/427/499,2/585 MHz (bin 0) e +675 MHz (bin 1);
- quirk `secvid-set-once` e `cx-gdsc`.

| Pezzo del Mi 9 | Per `andromeda` |
|---|---|
| Sonda OSM e `cephboost` | **uguale** (indirizzi OSM del SoC) ✔ |
| Sensori e blocco termico GPU | uguale come SoC. La corrispondenza con le zone termiche va riletta dal nostro FDT (`tsens_tz_sensor12`, `gpuss-0/1-usr`) ~ |
| Parser ARC/BCM e sonda `cepharc` | **uguale**: legge il cmd-db a runtime ✔ |
| GMU, HFI, PDC/RSC A640v2, SMMU (SID 4/5 e 0), CP, GX, LLC | **uguale** (stesso SoC e revisione; stessi `iommus` nell'FDT) ✔ |
| Pool di memoria e correzione 0088 | **uguale**; la 0088 è già nel nostro albero ✔ |
| `cepheus_gpu_config.h` | **del Mi 9**: è legato allo SHA256 dell'FDT del Mi 9 (`bdd364a1…`). Va rigenerato dal nostro FDT (stesse frequenze, vettori di banda da confrontare) ✔ |
| Digest dei firmware fissati (`a640_gmu.bin 3ff0c027…`, `a630_sqe.fw a0e1b583…`, `a640_zap.elf 74a948e8…`) e test ZAP | **del Mi 9**: da sostituire con i digest dei nostri file. La ZAP ha la stessa struttura: 14.256 B, MDT 148+6712 (✔ alberi vendor `andromeda`) ? |
| Velocità del chip (fuse) | per chip: va letta sul nostro (`qfprom 0x784130`, bit 31:29) ? |

### 1.5 Cosa non hanno ancora guardato

`gpu-reference-projects.md` confronta HakiOS raphael (A640, GLES) e Pong (A730, Metal), ma **non ios-flame**: nel
ramo non c'è nessuna occorrenza di "flame", "Pixel 4" o `sweet_a6xx` (✔ `git grep`). È la prima cosa da segnalare a
YangQi0408 (§8).

---

## 2. HakiOS e Adreno 640

### 2.1 `nomorecoolnicknames/hakios-sm8150`: `kernel/bsd/dev/vayu_adreno.c`

Commit `3e7be137` (29/10/2026). Il repository ha due soli commit, **nessuna licenza**, nessuna issue.

`vayu_adreno.c` (3.851 righe) è l'adattamento all'A640 di `drivers/gpu/hakios.adreno.c` del repository principale, che è
per l'**A642L** del POCO X5 Pro (3.852 righe, ✔). Ricostruisce in 22 passi la sequenza Linux
(`a6xx_gmu.c`, `a6xx_hfi.c`, `a6xx_gpu.c`, `mdt_loader.c`, `qcom_scm`). Ogni passo si attiva con `vayu_adreno=N`.

| Passi | Cosa fa (registri) |
|---|---|
| 1–2 | Clock GCC (`0x71004`, `0x7100c`, `0x71018`, `0x71154`; voto PLL `0x52004`). Ricerca nel cmd-db di `gfx.lvl`, `cx.lvl`, `mx.lvl` e dei BCM. Fuse del bin a `0x784130`[31:29] |
| 3–5 | GPUCC `0x2c90000`: GDSC CX `+0x106c` (toglie SW_COLLAPSE), rifiuta se il GX `+0x100c` è già acceso. RCG GMU `+0x1120` su GPLL0/3 = 200 MHz. CBCR cxo `+0x109c`, ahb `+0x1078`, cx_gmu `+0x1098`; frequenze verificate con il contatore di debug |
| 6–10 | Solo letture di GMU, GBIF, RSCC (`0x2c8d000` = GMU+0x23000) e SMMU (IDR, SMR/S2CR); prova di scrittura nella DTCM con il CM3 in reset |
| 11–12 | SMC SIP: `0xC2000601` (is_available), `0x42000207` (PAS_IS_SUPPORTED) |
| 13–15 | SMMU contesto 1 per il GMU: SID 5, mask 0x400, 39 bit, granulo 4K su pagine XNU da 16K. Upload di `a640_gmu.bin` (ITCM e DTCM via registri). RSCC/PDC (`0xb290000`/`0xb490000`, offset `0x30090`). Rilascio del reset, attesa di `0xbabeface` o `0x100` a seconda della versione |
| 16–18 | HFI senza scambio di versione: PERF_TABLE, BW_TABLE (un solo livello, come Linux per l'a640), CORE_FW_START, START, GX_BW_PERF_VOTE. **OOB GPU_SET**: il GX lo accende il GMU |
| 19–21 | SMMU contesto 0 per la GPU: SID 0, mask 0x401. hw_init di Linux per l'a640 (senza HWCG né CP_PROTECT). `CP_SQE_CNTL=1`, `CP_ME_INIT` |
| 22 | ZAP: INIT_IMAGE `0x42000201`, MEM_SETUP `0x42000202` su `0x98f15000`, AUTH_AND_RESET `0x42000205`, poi `CP_SET_SECURE_MODE 0`. PAS_SHUTDOWN non viene chiamato ("hung the phone"). **Con un errore il codice assume che "la ZAP sia già caricata"**: è lo schema che il Mi 9 rifiuta, giustamente |
| keep | Allocatore VA, map/unmap, sottomissione via `CP_INDIRECT_BUFFER` e fence `CACHE_FLUSH_TS` con attesa a polling. Nessun recupero dopo un blocco, nessun IRQ, nessun voto BCM dal lato AP |

**Prove sull'hardware: solo dichiarate.**
- I commenti citano run del 23/10 con dettagli plausibili, ma non ci sono log né foto.
- `configs/vayu.config` tiene spenti `CONFIG_HAKIOS_ADRENO` e `CONFIG_GPU`, e il codice che collega `hakios_gpu_*` →
  `vayu_gpu_*` non è pubblicato.
- Lo stato GPU è passato da "partial" a "works" in 27 minuti, e le due PR verso il repository principale (#31, #35) sono
  state chiuse senza merge.

**Valore per noi.** Indirizzi, stream e regione ZAP di `vayu` coincidono con il nostro FDT. È un buon secondo riferimento,
indipendente da ios-flame, per controllare ogni valore.

### 2.2 `oosexgang/hakios-xnu-kernel`: lo userland OpenGL ES

Commit `63fc2b46` (06/11/2026), base `xnu-8796.121.2`. Licenza APSL 2.0 per `xnu/files` e `drivers/`, MIT per il resto.

- **Kernel**:
  - `drivers/gpu/hakios.gpu.cpp`: `HakiosGPU : IOService` e `HakiosGPUUserClient`. I selettori di `externalMethod`
    sono **i numeri delle ioctl msm DRM**: `0x09` GEM_CLOSE, `0x40` GET_PARAM, `0x41`–`0x4c`, più `0x7d` userptr,
    `0x7e` IOSurface→BO, `0x7f` show. `clientMemoryForType(handle)` fa da mmap.
  - `MiniIOMobileFramebuffer.cpp` (righe 2004–2036) pubblica un `IOAcceleratorES` vuoto ("sgx") solo perché CoreAnimation
    non aspetti 30 s. `MetalPluginName` c'è solo con `metal=1`.
  - Non esistono classi IOGPU, AGX* o IOAcceleratorESLegacy.
- **Mesa** 25.0.7 compilato per iOS (`tools/mk-mesa.sh`: `-Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm
  -Dosmesa=true`). Lo shim `tools/mesa/hakios-drm/hakios.drm.c` traduce libdrm in `IOConnectCallStructMethod`.
- **Collegamento con CoreAnimation** (percorso predefinito: OpenGL ES):
  - `userland/src/inject.c` viene caricato in backboardd con `DYLD_INSERT_LIBRARIES`;
  - `eagl.c` sostituisce `-[EAGLContext initWithAPI:properties:]`, `getMacroContextPrivate`, `texImageIOSurface:…` e
    `attachImage:toCoreSurface:…`;
  - così il renderer `EAGLServer` di CA disegna con Mesa;
  - gli indirizzi sono **fissi per iOS 16.5 (20F66) iPhone10,3** (`inject.c:25, 104, 335, 515`): per 20H392 vanno
    ricalcolati.
- **Metal**: `hakios-metal` emette PM4 propri ma accetta solo il chip id `0x06030500` (A642L,
  `include/hakios-metal/a6xx.hpp:30`). **Sull'A640 non funziona.**
- **Presentazione a schermo**: copia dalla CPU nel framebuffer del bootloader (5,5–7,9 ms a frame).

### 2.3 Firmware secondo HakiOS

`pull-firmware.sh` li prende via adb da `/vendor/firmware/`. Servono:
- `a640_gmu.bin`;
- `a630_sqe.fw`: l'A640 usa il microcode dell'A630; serve versione ≥ 0x190;
- `a640_zap.mdt` + `.bNN`: i segmenti si ricavano dai program header; PAS 13.

Nessun blob è in git (✔ su tutti i rami).

### 2.4 Cosa si può portare da HakiOS

| Pezzo | Valutazione |
|---|---|
| Sequenza di `vayu_adreno.c` (C, `bsd/dev`) | Alta portabilità: solo SoC, API standard di 8796 (`ml_io_map`, `kvtophys`, `PE_parse_boot_argn`). **Senza licenza**: per riusarla serve il permesso di nomorecoolnicknames; come riferimento per un confronto si può leggere |
| `hakios.gpu.cpp` + Mesa + `hakios-drm` + `eagl.c` | È la via **OpenGL ES** (§5.2, alternativa B). Gli offset di `eagl.c` vanno ricalcolati su 20H392 |
| `IOAcceleratorES` vuoto | Banale, e utile subito (§5.4) |
| hakios-metal | Non utilizzabile (bloccato sull'A642L) |

---

## 3. UniXNU (Adreno 740 e Metal)

Clone locale `unixnu` a `b6ae99c1` (21/10/2026); upstream `10edc1ef` (06/11/2026). Upstream ha rinominato i
sottomoduli: NativeAdreno → **Adreno740**, NativeMetal → **AdrenoMetal**, NativePrelink → XnuPrelink.

### 3.1 Come si aggancia a iOS

- **Scoperta di Metal** (✔ verificata sul binario da `scripts/audit-target-metal-discovery.py`):
  1. `_MTLRegisterDevices` chiama `IOServiceMatching("IOAcceleratorES")`;
  2. legge `MetalPluginName` e carica `/System/Library/Extensions/<nome>.bundle`;
  3. legge `MetalPluginClassName` (deve essere sottoclasse di `_MTLDevice`);
  4. chiama `initWithAcceleratorPort:` e poi `MTLAddDevice`, che controlla la conformità a `MTLDevice` e `MTLDeviceSPI`.
- **Classe nel kernel**: `AGXAccelerator : IOAcceleratorES` (`NativeAdrenoAccelerator.cpp:16`). Il nome è scelto perché i
  profili sandbox lo ammettono. Su iPadOS 17+ la base `IOAcceleratorES` viene dal kext Apple IOGPUFamily, caricato
  apposta.
- **Proprietà pubblicate**: `MetalPluginName="NativeAdrenoMetal"`, `MetalPluginClassName="NativeAdrenoMetalDevice"`.
- **User client** `AGXDeviceUserClient`, tipo di apertura `0x4147`:
  - selettori 1–16 (ABI NativeAdreno v1): Query, BoAlloc, BoMap, BoFree, Submit, WaitFence, GetTimestamp, GetStatus,
    Set/GetMetadata, Create/CloseQueue, BoCpuMap/Unmap, GetDeviceInfo, BoImport, SetPriority;
  - c'è anche un tunnel `{'NTNL', metodo, payload}`.
- **IOSurface**: nessun aggancio nel kernel. Il frontend importa le pagine della superficie con BoImport: le pagine del
  task vengono bloccate in memoria e mappate nell'SMMU come non-cacheable.
- **Patch XNU**: bypass MACF per l'apertura del servizio (`zzw-native-adreno-sandbox-grant.patch`) e montaggio del
  bundle sopra `/System/Library/Extensions`.

### 3.2 Verificato o dichiarato

La documentazione dichiara funzionanti, su sheng (A740), il cold start di GMU e HFI, il CP, la SMMU, la scoperta Metal e
il desktop composto dalla GPU. Le prove sono indirette: centinaia di run numerati nei commenti, tempi misurati, nessun
log grezzo, e la storia git è stata schiacciata il 20/10. Su iPadOS 27 risultano ancora un panic nella preemption e il
ritmo dei frame "Pending".

### 3.3 Percorso degli shader

Tutto avviene nel processo che usa Metal: frontend ObjC → `NativeMetalBridge.rs` → metal2vulkan (AIR → SPIR-V) →
Turnip (NIR → ir3) quando si crea la pipeline. Le cache sono su disco.

- **Turnip non è specifico dell'a7xx**: i template compilano A6XX e A7XX. Il backend kernel "darwin" prende chip id,
  GMEM, hbb e swizzle da `GetDeviceInfo`; `0x06040001` diventa 640 senza bisogno di casi speciali
  (`turnip-darwin-kmd.patch`).
- **AirNir** è solo a7xx (`MesaBackend.c:29`, controlla `fd_dev_gen != 7`) e solo offline: non serve.
- **Differenze ir3 rilevanti per l'A640**: niente `storage_16bit` né `scalar_alu`, 256 costanti compute, delay slot
  diversi. metal2vulkan rifiuta in modo esplicito le pipeline che chiedono feature mancanti; ios-flame mostra che a
  CoreAnimation basta.

### 3.4 Cosa si riusa per l'A640 (a6xx gen2)

| Componente | Classe | Motivo |
|---|---|---|
| ABI, Publish, UserClient, Memory, Vm, SmmuContext/Memory, Timer, Status | **riusabile** | IOKit e gestione memoria; la GPU entra solo come parametro |
| Ring, Submit, CpInit | **da adattare** | Struttura dei comandi A7XX (BR/BV, `CP_THREAD_CONTROL`); l'A640 usa `a6xx_submit` e un ME_INIT a 8 dword |
| `NativeAdrenoA740HwInit.h`, Irq, Gmu* | **da rigenerare/adattare** | Registri, HWCG, protect, finestre GMU e DTCM dell'A740 |
| Pwrup, Preempt, contatori A7XX | **solo a7xx** | Sull'A640 non servono |
| AdrenoFirmware (parser GMU, HFI "moderno") | riusabile | L'A640 usa HFI v2 non legacy |
| ArmSmmu4K | riusabile | MMU-500 stage-1 4K generico |
| AdrenoPM4 | parziale | Gli encoder Type4/7 sì; i blocchi Shader/VPC/Raster sono A740 |
| Build di Turnip "darwin", metal2vulkan | riusabili | Ricompilati per iOS 16.0 (ios-flame lo fa già) |
| Frontend AdrenoMetal | dipende dalla versione OS | Upstream ha offset QuartzCore di 24A446 (iPadOS 27): **non usare su iOS 16**. ios-flame ha la versione per iOS 16 (`_MTLDevice` = 528 byte) |
| Kext Apple + NativePrelink/XnuPrelink, GopFramebuffer | **non applicabili** | ios-sweet ha già IOSurface e IOMFB propri e non carica kext |

### 3.5 Classificazione dell'Adreno 640 (correzione)

| Voce | Valore |
|---|---|
| Generazione | **a6xx gen2** (Mesa `freedreno_devices.py`, `GPUId(640)`; Linux `a6xx_catalog.c`), non gen1. L'A630 è gen1 ✔ |
| Chip id | `0x06040001` ✔ (FDT) |
| GMEM | 1 MiB |
| UBWC / highest bank bit | 3.0 / 15 (FDT `qcom,ubwc-mode = 3`, `highest-bank-bit = 0x0f`) ✔ |
| Microcode | SQE `a630_sqe.fw`, senza AQE ✔ |
| HFI | v2 non legacy ✔ |
| RSCC | GMU+`0x23000` ✔ |
| PDC | `0xb290000`/`0xb490000` ✔ |

---

## 4. Port sulla stessa base: ios-flame (A640) e ios-topaz (A610)

### 4.1 `NWylynko/ios-flame`: la stessa GPU su iOS 16.7.16, già accelerata

Commit `745c4341` (22/10/2026, Nick Wylynko). MIT © dipa-studio / ios-flame contributors; i file XNU modificati sono APSL
2.0.

Patch ios-sweet 0001–0081 più 0082–0124 di flame. Tutto è compilato dentro il kernel, come da noi. Il README (tabella
"What works", 22/10) dichiara: 8 core a piena frequenza, Metal su Turnip via metal2vulkan (`sweetmtl=2`) per backboardd,
SpringBoard e app, shader compilati sul telefono, **19–26 fps** scorrendo le pagine della Home.

**Patch che ci interessano:**

| Patch | Contenuto | Per noi |
|---|---|---|
| `0086-flame-cpufreq` (`bsd/dev/sweet_cpufreq.c`, 92 righe) | OSM: `DCVS_PERF_STATE_DESIRED` (`+0x920`) di ogni cluster all'indice più alto valido per tutti i core. A55 1785, A76 **2419**, prime **2841** MHz; CPR e LMh restano all'hardware; `flamecpufreq=0/N` | **subito** (§5.4) |
| `0087`, `0121` | vsync a scadenze fisse; la notifica vsync passa il periodo (`a3`): con la GPU 15,8 → 21 fps | **subito** |
| `0094` | `sweet_smc` (SMC con x0..x7) e `/dev/pmem` | sì |
| `0095-flame-gpu-present` (1.837 righe: `bsd/dev/sweet_a6xx.c` 1.011, `sweet_gpu.c`) | Accensione completa dell'A640 e present via GPU | **base del nostro driver** |
| `0096`/`0097` | `SweetAccelerator : IOAcceleratorES` con `MetalPluginName`, `MetalPluginClassName`, `IOGLBundleName`, `model "Adreno 640"` (solo con `sweetmtl`); user client v1 | sì |
| `0102`, `0122` | RPMh generico; voti BCM MC0 e SH0 al massimo all'avvio della GPU (banda CPU a 8 thread 13,7 → 18–19,6 GB/s; `sweetddr=0`) | sì, in accordo col Mi 9 |
| `0103`, `0114` | AMFI: ogni cdhash è "trusted"; policy dyld | sì (il nostro AMFI è già permissivo) |
| `0104`, `0124` | IOSurface: bytesPerRow delle superfici planari; superfici a 10 bit come BGRA solo per WebContent | sì |
| `0105`–`0109`, `0113`, `0117`–`0120` | Riuso dei VA, tabelle, frequenza GPU, recupero (soft reset + ZAP con SCM `0x0a`), submit asincroni, unmap dopo la fence, contabilità del pool, statistiche | sì |
| `0115` | User client v2 = ABI NativeAdreno v1 (per Turnip); include la correzione di `pmap_sync_page_attributes_phys` e `+nosha3+nosm4` in `MakeInc.def` | sì; la correzione pmap l'abbiamo già (0088) |
| `0110`–`0112` | Angoli arrotondati disegnati dalla GPU | più avanti |

**Sequenza di accensione** (`a6_power_up` = `power_cx` → `gmu_boot` → `gmu_hfi` → `gpu_cp`; ✔ patch 0095 e HANDOFF
"GPU probe 1–6"):

1. **CX**
   - Clock GCC `0x71004/0x71154/0x7100c/0x71018`.
   - GDSC CX (GPUCC `0x106c`): toglie il collapse e attende PWR_ON.
   - RCG GMU `0x1120` su XO; CBCR ahb, cxo e cx_gmu.
   - **Nessun voto RPMh dalla CPU.**
2. **GMU**
   - SMMU `0x2ca0000`: SMR e context bank liberi scelti a runtime. SID 5, mask 0x400; SEP=7 (con SEP=0, fault sopra
     i 2 GB); MAIR0 `0xff44`.
   - Finestre: icache `0x4000`, dcache `0x44000`, log `0x60001000`, HFI `0x60005000`.
   - Tabelle RSCC, PDC_SEQ e PDC; l'handshake RSCC non riceve mai l'ack, ed è innocuo.
   - `a640_gmu.bin` in ITCM/DTCM. Risposta **`0xBABEFACE`**, firmware 2.0.1.5.
3. **HFI e GX**
   - cmd-db letto tramite `0x0c3f000c`; voti ARC calcolati come in `a6xx_gmu_rpmh_arc_votes_init`.
   - PERF_TABLE (257–585 MHz; GMU 200), BW_TABLE, CORE_FW_START, START, GX_BW_PERF_VOTE.
   - OOB GPU_SET → **GDSC GX acceso, `RBBM_STATUS` risponde**.
4. **CP**
   - SMMU per la GPU: SID 0, mask 0x401, 39 bit.
   - Sottoinsieme di hw_init (SECVID, UCHE, GMEM, UBWC hbb 15).
   - SQE (`a630_sqe.fw` senza la prima dword) via `CP_SQE_INSTR_BASE` `0x830/0x831`; ring `0x0800020c`; ME_INIT.
   - **Poi** ZAP con PAS (init, mem_setup, auth: tutte 0) e `CP_SET_SECURE_MODE 0`.
   - `CP_MEM_WRITE` → **`0xcafef00d`**. Accensione completa in **296 ms**.
5. **Present**
   - Blit 2D della GPU (`CP_BLIT`, scalato) dalla IOSurface 1125×2436 al pannello 1080×2280.
   - Stride di VIG0/VIG1 e flush CTL0 dell'MDP (`0xae00000`): 1,1–3 ms, asincrono dalla 0119.

**Firmware.** Vanno nell'ADT come proprietà `/chosen/gpufw,<nome>` (`tools/mkpayload.py --gpufw`), presi da
`vendor.img/firmware`. Non sono in git.

**Userland** (`phone/metal2`):
- **Struttura**:
  - `SweetMetal2.bundle` = frontend UniXNU adattato a iOS 16 + bridge metal2vulkan (Rust, pin `720ef1ce`) + Turnip
    (Mesa `85fda3b3` + patch UniXNU, `-Dfreedreno-kmds=darwin`, minimo iOS 16.0).
  - Firma ad hoc; installazione via SSH (`tools/flame-metal2-install.sh`).
- **Chi riceve un device**: backboardd sempre; gli altri processi solo se elencati in `/var/local/mtl2-clients`.
  SpringBoard è obbligatorio, altrimenti lo sblocco non funziona.
- **Impostazioni di CA per processo**: `CAEnableDeepFramebuffer=NO` (altrimenti CA sceglie `b3a8` e non disegna),
  compressione del framebuffer e delle IOSurface disattivate (`SweetMetal2.mm:164-166`).
- **Trappola principale (✔ HANDOFF, "The big trap")**:
  - SWEET non ha PPL, quindi la physmap mappa tutta la RAM gestita come cacheable.
  - Ogni vista WC/NC di una pagina gestita da XNU è un alias incoerente: scritture perse, letture vecchie.
  - **Soluzione**: i buffer GPU vengono da un pool di RAM **che XNU non gestisce** (Pixel 4:
    `0xc4000000`–`0xe0000000` + `0xe1000000`–`0x100000000`, 944 MB).
  - Lo stesso rischio riguarda `SweetSurface`, che mappa le IOSurface WC nel kernel.

**Prove.**
- Screenshot `docs/img/metal2-tour.jpg`: 13 schermate (blocco, Home, Impostazioni, Centro di Controllo, Safari…) disegnate
  con Metal.
- HANDOFF documenta le sonde 1–6 con i valori dei registri, Turnip `v2test`/`vktest` "ALL PASS" e le fasi Metal.
- Log e foto citati sono in `.gitignore`.
- Replica indipendente: `Yofuness/ios-sunfish` (A618) segna Metal funzionante.
- `nithinyadavamaraboina-online/IOS-26-Hot` (Realme X2 Pro, SM8150/A640) deriva da flame e cambia solo `ZAP_REGION`
  (commit `528e45209f`): indica che la regione ZAP è la differenza principale tra un SM8150 e l'altro ~.

**Numeri da tenere presenti** (✔ HANDOFF):

| Configurazione | Prestazioni |
|---|---|
| Rendering in software con 8 core a piena frequenza | **8–25 fps** interagendo, picchi 38. La copia sul pannello costa ~2,3 ms: il limite è il renderer software di CA |
| GPU, kernel 0116 | 17 fps |
| GPU, kernel 0121/0122 | 19–26 fps |

backboardd fa ~1,8 submit e ~10–15 ms di GPU per frame, in serie con il lavoro CPU di CA.

**Cosa resta rotto con Metal**:
- i widget della vista Oggi (chronod resta su CPU);
- in Safari il corpo sotto la prima riga di tile;
- i poster (AegirPoster);
- con un fault nel fetch di un IB il CP si blocca.

**Da cambiare per `andromeda`** (✔ dal nostro FDT):

| Parametro | Valore per noi |
|---|---|
| `ZAP_REGION` | `0x98f15000`, non `0x9a915000` |
| Framebuffer | `0x9c000000` (`cont_splash_region`, 36 MiB), non `0x9c400000` |
| Pannello | 1080×2340, quindi un'altra scala |
| Pipe MDP lasciata dal bootloader | da verificare ? |
| Pool GPU | in RAM non gestita da noi (§9) |
| Firmware | nel nostro ADT |

### 4.2 `MakrSas/ios-topaz`: Adreno 610 via Mesa (solo riferimento)

Ramo `topaz` a `e4772911` (22/10/2026). Redmi Note 12 4G, SM6225: Adreno 610, chip id `0x06010001`, **senza GMU**.
**Nessuna licenza**: solo riferimento, e per qualsiasi riuso serve il permesso di MakrSas.

**Patch 0900–0949**, importate in blocco con il commit `fe7625f1` ("now that HackiOS is open source"):

| Patch | Contenuto |
|---|---|
| `0901` | Driver kernel: `TopazAdreno.cpp` (sequenza dal driver Windows topaz-woa) + `RedwoodGPU.cpp`, il client msm-DRM di HakiOS/darwin-redwood "reused with the author's permission" |
| `0905` | Firmware Qualcomm **incorporato come array C nella patch**: da non copiare |
| `0919`/`0947` | `kern.exec_inject` (`DYLD_INSERT_LIBRARIES` per programmi scelti) |
| `0924`–`0949` | Superfici a schermo intero nel pool GPU, lette direttamente dal display (VIG0) |
| `0138` | `IOAcceleratorES` vuoto "sgx": evita i **30 s** di attesa di `CAMetalContextCreate` a ogni avvio di backboardd |
| `0948` | Modi display a 60 Hz: senza, CA distanziava i frame di 50 ms |

**Userland: OpenGL ES, non Metal.**
- `bbd_wrap.c` sostituisce `/usr/libexec/backboardd` e lo rilancia con `DYLD_INSERT_LIBRARIES=libtopaz-inject.dylib`.
- `eagl.c` sostituisce 7 metodi di `EAGLContext` con **indirizzi per 20H392**, la nostra stessa build.
- Mesa 25.0.7 freedreno (OSMesa, profilo ES) passa per `redwood-drm` → user client `RedwoodGPU`.
- Lo userland non si compila dal repo: mancano `eagl_gen.h`, `mkeagl.py`, `eagl_slots.txt` e `redwood_input.h`.

**Prove.** Il README dichiara tutta la UI su GPU a 25–35 fps. Nel repo ci sono solo i messaggi delle patch, nessun log
né screenshot: plausibile, non verificabile.

**Per noi.**
- L'accensione dell'A610 non si applica: l'A640 ha bisogno di GMU, RSC/PDC, HFI e OOB.
- Insegnano qualcosa l'`IOAcceleratorES` vuoto, i 60 Hz e lo schema di iniezione in backboardd (se servisse la via
  OpenGL ES).
- Gli indirizzi EAGL per 20H392 vanno ricavati da noi, perché non hanno licenza.

---

## 5. Cosa vuole iOS 16 dal kernel per usare una GPU

### 5.1 Come backboardd sceglie il renderer

✔ NOTES di ios-sweet, HANDOFF di ios-flame, log nostri.

**Ordine dei renderer.** `CA::WindowServer` (IOMFBServer su AppleCLCD) costruisce il renderer così:
1. **Metal**: `CAMetalContextCreate` → `MTLCreateSystemDefaultDevice`.
2. **OpenGL ES** (`EAGLServer`): solo se abilitato. HakiOS e topaz lo forzano iniettando codice.
3. Altrimenti il renderer software **`CA::OGL::SW`**: è quello che usiamo oggi.

**La scoperta di Metal** chiede al kernel:
- un servizio IOKit la cui catena di classi contenga **`IOAcceleratorES`**;
- con le proprietà `MetalPluginName` e `MetalPluginClassName`.

Nel nostro log backboardd cerca `IOAcceleratorES` **678 volte** in mezz'ora (sempre "0 found"), più `AGXAccelerator` e
`IOAcceleratorESLegacy` (✔ `phone/logs/merged-20261106-174054.log`).

**Il percorso Apple non serve.** Il driver Apple usa IOGPUFamily (`IOGPUDevice`, apertura tipo 1 con la versione API
nei 16 bit alti) + `AGXMetalA11.bundle`, che emette comandi per la GPU Apple: per un'Adreno è inutilizzabile. Non va
imitato né caricato.

### 5.2 Le superfici minime, per obiettivo

| Obiettivo | Kernel | Userland | Riferimento |
|---|---|---|---|
| **(0) Solo blit di present via GPU** | GPU accesa, import della IOSurface, blit e fence prima dello swap | niente | ios-flame 0095. **Guadagno piccolo**: la nostra copia costa 2,3–4,8 ms (Mi 9, `performance.md`) |
| **(a) Composizione GPU solo in backboardd (+SpringBoard)** | (0) + `IOAcceleratorES` con le proprietà Metal; user client ABI NativeAdreno v1 (BO, map, BoImport delle pagine del task, submit, fence, device info); pool GPU in RAM non gestita; IOSurface lineari (bpr corretti) | `SweetMetal2.bundle` (frontend + metal2vulkan + Turnip) nel root; `CAEnableDeepFramebuffer=NO`; compressione off; cache shader scrivibile; elenco client = backboardd, SpringBoard | ios-flame 0096/0115/0116 |
| **(b) Metal completo per le app** | come (a), con contabilità e limiti per processo, recupero, unmap dopo la fence | elenco client `*` (escluso chronod) | ios-flame 0117–0124 |
| **(B) Alternativa OpenGL ES** | user client stile msm DRM (HakiosGPU) | Mesa freedreno OSMesa + iniezione EAGL in backboardd con offset ricalcolati per 20H392 | HakiOS, topaz |

Per (a) e (b) il kernel è lo stesso: cambia solo la configurazione dello userland. **L'obiettivo minimo è (a).** La
via (B) ha due svantaggi: richiede di ricavare e mantenere offset privati di QuartzCore, e non dà Metal alle app. Ha senso
solo se Turnip o metal2vulkan si bloccano sull'A640.

### 5.3 Aspettative realistiche

Con la GPU, ios-flame arriva a 19–26 fps sulla Home, non a 60. Il guadagno rispetto al software a piena frequenza
(8–25) c'è, ma non è enorme: CA fa ancora molto lavoro sulla CPU, in serie.

Il valore della GPU sta altrove:
- toglie carico alla CPU (batteria e temperatura);
- dà Metal alle app (Safari, Mappe, giochi);
- regge animazioni complesse (sfocature, Centro di Controllo).

### 5.4 La via più corta per un miglioramento visibile (prima della GPU)

1. **Frequenze CPU.**
   - **Situazione**: Gold e Prime sono a 711/827 MHz (indice 0), Silver a 1632 MHz (✔ misura del Mi 9; stesso SoC). Oggi
     i core "piccoli" sono più veloci di quelli grandi, e lo scheduler di SWEET li tratta tutti allo stesso modo ~.
   - **Cosa fare**: portare `sweet_cpufreq.c` di ios-flame (indice più alto valido per tutti i core: 2419/2841 MHz),
     oppure il `cephboost` del Mi 9 esteso dalla prova di 120 s a permanente.
   - **Condizione**: è uno dei candidati più promettenti, ma **solo con il blocco termico TSENS del Mi 9 attivo**.
     La tensione la gestisce l'OSM/CPR in hardware, come in Linux (`clk-cpu-osm.c` scrive lo stesso registro).
2. **Ritmo del display.**
   - Oggi `sweetfb_hz=30`: passare a 60 Hz con le scadenze fisse (`sweetfb_fixedtimer=1` del Mi 9, o flame 0087).
   - Mandare il periodo del vsync nella callback di CA (flame 0121).
   - Esporre modi a 60 Hz (topaz 0948, da reimplementare).
   - Effetto atteso: meno frame distanziati di 50 ms.
3. **`IOAcceleratorES` vuoto**, senza `MetalPluginName`: un IOService pubblicato al boot.
   - Smette il polling di backboardd ed evita l'attesa di 30 s di `CAMetalContextCreate` a ogni avvio (topaz 0138,
     HakiOS `MiniIOMobileFramebuffer.cpp`).
   - Metal resta nil, e CA continua in software.
   - Dopo, diventerà la classe base del vero acceleratore.
4. **Voto DDR/NoC** (flame 0122): +35–40% di banda di memoria per la CPU. Il renderer software è legato alla memoria, ma
   servono scritture BCM su RPMh: va concordato con il lavoro ARC/BCM del Mi 9.
5. **Più core per il renderer software?**
   - **Non sappiamo** se `CA::OGL::SW` divide il lavoro tra thread ?.
   - Il nostro campionatore (`SMP backboardd t0..t3 pc/lr` nel log) più la simbolicazione sulla dyld cache (`ipsw dyld
     a2s`) possono dirlo. ios-flame ha fatto lo stesso con `tools/flame-profile.py`.
   - Se è un solo thread, conta soprattutto che giri su un A76 veloce (punto 1).

---

## 6. Firmware: dove sono e come prenderli senza metterli in git

### 6.1 File

| File | Ruolo | Dimensioni note per `andromeda` (✔ alberi vendor pubblici, solo elenco) |
|---|---|---|
| `a640_gmu.bin` | Firmware del GMU (formato a blocchi) | **37.680 B** (PixelExperience "fourteen") o **103.756 B** (MIUI V10.3.19): versioni diverse. Il driver deve accettare il formato giusto: HakiOS distingue `0xbabeface`/`0x100` secondo la versione ✔ |
| `a630_sqe.fw` | Microcode del CP (anche per l'A640) | 31.816 / 32.136 B; serve versione ≥ 0x190 ✔ |
| `a640_zap.mdt` + `a640_zap.b00`/`.b01`/`.b02` | ZAP shader firmato (PAS 13), forma divisa | 6.860 + 148 / 6.712 / 1.968 B ✔ |
| `a640_zap.elf` | La stessa ZAP in un solo ELF | 14.256 B; **identica per dimensione e struttura a quella del Mi 9** (SHA `74a948e8…`) ? |

La ZAP è firmata per la catena di fiducia dell'OEM: va usata **quella del nostro telefono**. Se il digest coincide con
quello del Mi 9 sapremo che Xiaomi usa la stessa su entrambi ?.

`a640_gmu.bin` e `a630_sqe.fw` dovrebbero esistere anche in `linux-firmware` (`qcom/`), che è ridistribuibile ~. Per
coerenza con il firmware vendor è comunque meglio usare quelli del telefono.

### 6.2 Dove sono sul telefono

- Sul nostro esemplare `/vendor/firmware/` sta nella partizione `vendor` (`sde53`). `modem` (`sde52`, FAT "NON-HLOS")
  contiene i firmware di modem, ADSP, CDSP e WLAN: la ZAP di solito non è lì, ma va controllato ~.
- Il vendor di EvolutionX ha i blob della ROM MIUI: l'impronta dichiarata è `V12.0.7.0.PEMEUXM` ✔ (`getprop.txt`).
- I backup in `phone/<seriale>/backup/` **non comprendono `vendor`**.
- `scripts/pull-phone-info.sh` ha bisogno di Android, che con iOS su `userdata` non parte più.

### 6.3 Come estrarli

Comandi **da far lanciare all'utente**; qui non è stato eseguito nulla. Hanno tutti in comune:
- **sola lettura** del telefono;
- destinazione `inputs/andromeda/gpufw/`, già esclusa da git (`.gitignore`: `inputs/`, `phone/`, `fw-*/`).

**A) Dalla ROM fastboot ufficiale (nessun telefono coinvolto).** Va scaricata dall'utente da Xiaomi. Il nome esatto del
pacchetto EEA V12.0.7.0 è da verificare ?.

```bash
mkdir -p "$SCRATCH/rom" inputs/andromeda/gpufw && cd "$SCRATCH/rom"
tar xzf andromeda_eea_global_images_V12.0.7.0.PEMEUXM_*.tgz --wildcards '*/images/vendor.img'
simg2img */images/vendor.img vendor.raw      # se è sparse; altrimenti è già ext4
debugfs -R 'ls -l /firmware' vendor.raw | grep -E 'a6[34]0|a630_sqe'
for f in a640_gmu.bin a630_sqe.fw a640_zap.mdt a640_zap.b00 a640_zap.b01 a640_zap.b02 a640_zap.elf; do
  debugfs -R "dump /firmware/$f $OLDPWD/inputs/andromeda/gpufw/$f" vendor.raw
done
```

**B) Dal telefono, con un recovery avviato solo in RAM.** Il comando è `fastboot boot`, mai `flash`.
- Montare `vendor` in sola lettura e copiare i file:
  `adb shell mount -o ro /dev/block/bootdevice/by-name/vendor /vendor`, poi
  `adb pull /vendor/firmware/<file> inputs/andromeda/gpufw/`.
- In alternativa, leggere l'intera partizione con `adb exec-out dd if=/dev/block/bootdevice/by-name/vendor` e poi
  procedere come in A.
- **Attenzione**: il recovery non deve toccare `userdata` (HFS+ di iOS). Niente "Format data" o "Wipe"; tenere il system
  in sola lettura.
- Questa via dà i file **effettivamente in uso sul telefono**, che sono preferibili.

**C) Come verifica**: confrontare gli SHA256 di A e B, e quelli con i digest del Mi 9.

Nei documenti va registrato **solo lo SHA256**, come fa il Mi 9 (`cpu-gpu.md`).

### 6.4 Come passarli al kernel

Come ios-flame: proprietà `/chosen/gpufw,<nome>` nell'ADT.
- Si aggiunge a `scripts/make-andromeda-adt.py` un'opzione `--gpufw DIR` (o una voce `[payload.gpufw]` in
  `board.toml`).
- In tutto sono ~60–150 KB, nel payload `GUACESP` da 34 MiB.
- Il kernel li legge con `PE_get_dt_property` e controlla gli SHA256 attesi: lo schema del Mi 9, con i nostri digest.
- Nessun lettore ext4 nel kernel, niente rete.

---

## 7. Piano a fasi per `andromeda`

Regole di metodo (da ROADMAP):
- ogni passo parte spento e si accende con il suo parametro;
- log via USB per ogni avvio;
- nessuna scrittura fuori da `userdata`;
- il Mi 9 viene informato a ogni passo valido per tutti.

| Fase | Obiettivo | Contenuto | Fatto quando (accettazione) | Difficoltà | Con il Mi 9 |
|---|---|---|---|---|---|
| **F0** Misure | Base di confronto | Benchmark di swipe ripetibile dalle righe `SWEET PERF` (Mi 9 `summarize-performance.py`); campionamento dei thread di backboardd (§5.4, punto 5) | Tre run con lo stesso gesto, fps medio e p90 registrati; noto quanti thread lavorano in `CA::OGL::SW` | bassa, 1–2 giorni | Script loro |
| **F1** Velocità senza GPU | Miglioramento visibile subito | (1) Frequenze OSM (flame 0086 o `cephboost` esteso) con blocco TSENS. (2) 60 Hz a scadenze fisse + periodo vsync (flame 0121). (3) `IOAcceleratorES` vuoto. (4) Facoltativo: voto DDR/NoC | `rate_khz` Gold ≥ 2,4 GHz e Prime ≥ 2,8 GHz nel log; il blocco termico scatta in una prova provocata; nel log niente più cicli `IOAcceleratorES -> 0 found`; fps F0 migliorati in modo misurabile; touch, tasti e spegnimento schermo invariati; 30 min senza panic né surriscaldamento | bassa-media, ~1 settimana | **Insieme**: stesso codice; concordare la politica termica |
| **F2** Firmware e sonde in sola lettura | Dati certi prima di scrivere | Estrazione (§6.3), SHA256 nei documenti, firmware nell'ADT. Sonde del Mi 9 già nel nostro ramo di sync (`cephsoc`, `cepharc`, `cephfuse`, `cephgx`) + equivalente della "probe 1" di flame (ID GMU/SMMU); lettura della pipe MDP che legge `0x9c000000` | Digest registrati; il kernel trova e verifica i 4–7 file; nel log GPUCC, ARC, bin del fuse e ID SMMU senza fault | bassa, giorni (serve l'utente per l'estrazione) | **Insieme**: stessi probe, si confrontano i valori |
| **F3** GPU accesa | Il GPU esegue comandi | Port di `sweet_a6xx.c`/`sweet_gpu.c` (flame 0095 senza il present, + 0094 `sweet_smc`, 0102) con: parametri dall'FDT (ZAP, cmd-db, SID), firmware dall'ADT, pool in RAM non gestita, guardie del Mi 9 (TSENS, PA a 64 bit, nessuna ZAP finta, limite ai passi). Avanza per gradini con `sweetgpu=N`: CX → GMU → HFI+GX → CP+ZAP → `CP_MEM_WRITE` → riempimento di un'area del framebuffer | Per gradino: `0xBABEFACE` e `HFI_CTRL_STATUS=1`; GDSC GX acceso e `RBBM_STATUS` letto; PAS init/mem/auth = 0; `0xcafef00d` scritto; rettangolo GPU visibile sul pannello. 10 avvii consecutivi senza fault SMMU; arresto (slumber) pulito | media, 1–2 settimane | **Da fare una volta per entrambi**: PR al loro ramo con i parametri da FDT |
| **F4** Present via GPU | Prima integrazione con IOSurface | Blit GPU IOSurface → framebuffer con fence (flame 0095 present, 0119 asincrono); ritorno alla copia CPU con un parametro | CRC uguali tra percorso CPU e GPU su N frame; tempo di present ≤ copia CPU; 30 min senza artefatti né fault | media, ~1 settimana | Insieme: stesso pannello EA8076 1080×2340 |
| **F5** Turnip sul telefono | Driver Vulkan funzionante | User client v2 (flame 0115, ABI NativeAdreno v1). Build di Turnip per iOS 16 (Mesa `85fda3b3` + patch UniXNU) in un workflow macOS; `v2test`/`vktest` nel root | `v2test` e `vktest` "ALL PASS" con UBWC, sysmem e GMEM forzato | media-alta, 1–2 settimane (toolchain Mac + Rust) | Insieme (workflow condiviso) |
| **F6** Composizione Metal | Obiettivo minimo (a) | `SweetAccelerator : IOAcceleratorES` (0096/0116), `SweetMetal2.bundle` installato da `build-root.sh`, impostazioni CA, elenco client = backboardd + SpringBoard; correzioni IOSurface 0104 | Blocco schermo e Home composti dalla GPU (log `MTL2`); fps ≥ F1 nello stesso benchmark; 30 min stabile; `sweetmtl=0` riporta al software | alta, 2–4 settimane | Insieme |
| **F7** Metal per le app e rifiniture | Obiettivo (b) | Elenco client `* -chronod`, Safari (0124), recupero (0108), contabilità del pool (0117/0122), statistiche (0120), angoli (0110–0112) | Impostazioni, Safari, Foto e Note corrette; 1 ora senza DeviceLost; temperatura sotto la soglia | alta, aperta | Insieme |
| *Alt-B* | Ripiego OpenGL ES | HakiosGPU + Mesa freedreno + iniezione EAGL con offset 20H392 ricavati da noi | Home composta via EAGLServer | alta | Solo se F5/F6 si bloccano |

**Note tecniche per il port (F3–F6):**
- **Numerazione.** Le patch di flame 0082–0124 si sovrappongono alle nostre e a quelle del Mi 9 (0082–0089): le portiamo
  in un intervallo a parte (es. `0200+`). Va tolto il doppione della correzione pmap: la nostra 0088 equivale alla parte
  pmap di flame 0115.
- **Compilatore.** flame ha dovuto aggiungere `+nosha3+nosm4` perché clang emetteva EOR3, che gli A76/A55 non hanno. Va
  verificato con il nostro Xcode 16.2 ?.
- **Cosa sostituire.** ios-flame parte dal suo shim, noi da Aloha + DarwinBoot. Gli indirizzi fissi di 0095 (ZAP,
  framebuffer, pool) vanno sostituiti con letture dall'FDT o dall'ADT.
- **`SweetSurface`.** Le mappature WC delle IOSurface nel kernel hanno lo stesso rischio di alias incoerente: vanno
  riviste prima di F4.

---

## 8. Lavoro comune con il Mi 9 (YangQi0408)

Il Mi 9 ha lo stesso SoC, lo stesso pannello (EA8076 1080×2340), lo stesso kernel e la stessa catena di avvio
(Aloha + DarwinBoot), e in FDT la stessa regione ZAP `0x98f15000`. **Tutto il piano vale per entrambi** quasi alla
lettera.

Da proporre:

1. **Segnalare ios-flame** con questo confronto. Il loro `gpu-reference-projects.md` non lo cita, ed è la prova su
   hardware più vicina a noi due.
2. **Un solo driver GPU per l'SM8150**: il port di `sweet_a6xx.c` con i parametri dall'FDT, sopra le loro salvaguardie
   (TSENS, workspace verificato, niente ZAP finta, arresto). Da offrire come PR su `feat/adreno640`. I loro moduli
   offline (HFI, PDC, CP_PROTECT, HWCG, LLC) servono come seconda fonte per controllare i valori di flame.
3. **Sonde**: far girare su `andromeda` le loro sonde in sola lettura (sono già nel nostro ramo di sync) e mandare loro i
   valori (CX, ARC, bin, SMMU): un secondo esemplare rafforza le loro prove.
4. **CPU**: concordare una sola politica OSM + termica, invece di `cephboost` (120 s, 1,4 GHz) da una parte e flame 0086
   (sempre al massimo) dall'altra.
5. **Firmware**: una tabella condivisa dei digest per dispositivo; i file restano fuori da git.
6. **Userland Metal**: un unico workflow macOS che produce `SweetMetal2.bundle` e Turnip per iOS 16, usato da entrambi.

---

## 9. Domande aperte e rischi

1. **Lo stato lasciato da Aloha UEFI.** ios-flame parte dall'ABL di Google con uno shim; noi passiamo da Aloha +
   DarwinBoot.
   - I rail CX/MX e i clock GCC della GPU risultano spenti sul Mi 9, come sul Pixel 4 prima della probe 1: si accendono.
   - L'UEFI potrebbe però lasciare contesti SMMU configurati in modo diverso.
   - Va controllato con le sonde di F2 ?.
2. **PAS 13 via SMC sotto Aloha + QHEE di Xiaomi.** Funziona su Pixel 4 (✔) e risulta dichiarato su raphael e vayu
   (Xiaomi) ~. Prima di tutto va fatta la sola query `is_call_available` (ponte del Mi 9).
   - Ogni SMC può non tornare.
   - PAS_SHUTDOWN su vayu bloccava il telefono: va evitato.
3. **Dove mettere il pool GPU.** XNU gestisce 1,5 GiB (`owned_memory_size`). Il pool deve stare in RAM convenzionale
   UEFI fuori da quella finestra.
   - Candidati: l'altra voce grande della mappa, tra DXE Heap `0xC0300000`–`0x140000000` e RAM partition
     `0x180000000`–`0x200000000`.
   - Va descritto nell'ADT o passato come parametro, controllato contro PStore e le zone runtime.
   - Si scontra con l'obiettivo "3 GiB a XNU" della ROADMAP: su 6 GB c'è posto per entrambi, ma va pianificato ?.
4. **Pipe MDP.** Quale pipe legge `0x9c000000` dopo Aloha, e se è sicuro cambiarne lo stride come fa flame. In
   alternativa la GPU scrive nel framebuffer con lo stride nativo, senza toccare l'MDP ?.
5. **Versione del firmware GMU** presente sul telefono (37.680 o 103.756 B): risposta `0xBABEFACE` o `0x100`, e formato
   a blocchi ?.
6. **Bin del chip.** Con bin 1 il livello a 675 MHz è disponibile; il valore va letto ?.
7. **Calore.** Il Mix 3 5G scalda (modem 5G, vetro). Con CPU al massimo e GPU accesa il blocco TSENS è obbligatorio,
   non opzionale; LMh in hardware è l'ultima difesa.
8. **Sicurezza.** L'user client GPU è in pratica un accesso DMA per qualsiasi processo, e AMFI e sandbox sono permissivi.
   Accettabile per un port di ricerca, ma va detto nel README.
9. **Licenze.**
   - ios-flame: MIT, con file XNU APSL 2.0. UniXNU AdrenoMetal e Adreno740: MIT. Mesa: MIT. HakiOS: APSL 2.0 e MIT.
   - **metal2vulkan è GPL-2.0-or-later OR LGPL-3.0-or-later** ed è linkato staticamente nel bundle: se distribuiamo il
     bundle compilato dobbiamo rispettarne la licenza.
   - **hakios-sm8150 e ios-topaz non hanno licenza**: solo lettura e confronto; per il riuso serve il permesso dei
     rispettivi autori.
10. **Multithreading del renderer software** (§5.4, punto 5) ?.
11. **Il Mi 9 accetterà un percorso meno "fail-closed"?** Se no, F3 si fa da noi con le loro guardie come opzione, e
    loro restano liberi di seguire il loro ritmo.

---

## 10. Fonti

Grazie agli autori: questo piano si regge sul loro lavoro.

- **Port Mi 9**, YangQi0408: [`YangQi0408/UniXNU-cepheus`](https://github.com/YangQi0408/UniXNU-cepheus), ramo
  `feat/adreno640` a `93cde1fda5bd43fa21158d0de978f17c49a1cfa1`.
  - Documenti: `docs/cpu-gpu.md`, `gpu-acceptance.md`, `gpu-one-shot.md`, `gpu-reference-projects.md`,
    `zap-platform-contract.md`, `rpmh-firmware-evidence.md`, `scm-static-evidence.md`, `upstream-sync.md`,
    `performance.md`.
  - Codice: `xnu/cepheus_*` (GPU, GMU, HFI, CP, GX, SMMU, QMP, PDC, SCM, ARC, BCM), `xnu/0086`–`0088`, `cepheus_soc.c`,
    `cepheus_gpu_compile.c`, `scripts/test-*`.
- **ios-flame**, Nick Wylynko ([NWylynko](https://github.com/NWylynko)):
  [`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame) a `745c4341af86`, MIT (© 2026 dipa-studio / ios-flame
  contributors), patch XNU APSL 2.0.
  - Documenti: `README.md`, `HANDOFF.md` (sezioni "ALL 8 CORES AT FULL CLOCK", "GPU probe 1–6", "The big trap", Metal
    steps 1–3, "WiFi at boot… frame pacing, DDR vote"), `NOTES.md`, `METAL-NEXT.md`, `CREDITS.md`.
  - Patch: `xnu/patches/0086`, `0087`, `0094`–`0097`, `0102`–`0124`.
  - Userland: `phone/metal2/SweetMetal2.mm`, `docs/img/metal2-tour.jpg`.
- **ios-sweet**, dipa-studio (damjankakak3): [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) a
  `45263bee`, MIT + APSL: `NOTES.md` (percorso GUI, `CA::OGL::SW`, patch 0046 `SweetSurface`/`SweetFramebuffer`).
- **ios-topaz**, MakrSas: [`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz) ramo `topaz` a `e4772911e116`,
  **senza licenza**: `README.md`, `CREDITS.md`, patch `0138`, `0901`, `0902`, `0905`, `0919`, `0924`–`0949`, `0948`,
  `userland/` (`bbd_wrap.c`, `eagl.c`). Driver Windows di riferimento [`MakrSas/topaz-woa`](https://github.com/MakrSas/topaz-woa).
- **HakiOS**, OOSEX GANG (shimoku / shimoku74, vcvkk, yehorkaa, dlz3388461878-cmd):
  [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) a `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`,
  APSL 2.0 + MIT.
  - Kernel: `drivers/gpu/hakios.adreno.c` (A642L), `drivers/gpu/hakios.gpu.cpp`,
    `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`.
  - Mesa: `tools/mk-mesa.sh`, `tools/mesa/hakios-drm/hakios.drm.c`.
  - Userland: `userland/src/inject.c`, `eagl.c`, `HakiosMetalCA.m`.
  - Metal: `hakios-metal/include/hakios-metal/a6xx.hpp`, `docs/reference/metal.md`, `metal-measurements.md`.
  - Revisione: `docs/driver-review/gpu-display.md`.
  - Il lavoro di vcvkk (darwin-redwood) è all'origine del driver adreno e del percorso EAGL.
- **HakiOS SM8150**, nomorecoolnicknames:
  [`nomorecoolnicknames/hakios-sm8150`](https://github.com/nomorecoolnicknames/hakios-sm8150) a
  `3e7be137a100cef19f1d7e83346b6e688be697d1`, **senza licenza**: `kernel/bsd/dev/vayu_adreno.c`, `configs/vayu.config`,
  `pull-firmware.sh`, `README.md`, `device.conf`.
- **HakiOS raphael** (Helium Studio, HeliumStudio-Dev), letto tramite il documento del Mi 9:
  `HeliumStudio-Dev/hackios-xnu-kernel` `57722468` (`xnu/files/bsd/dev/hakios.adreno.c`, `docs/RAPHAEL.md`).
  **Pong** (Chengdong-Yu), `Chengdong-Yu/UniXNU-pong` `e0ee6df1`, citato dallo stesso documento.
- **UniXNU** ([organizzazione UniXNU](https://github.com/UniXNU); bluseliu50, rpone/rponeawa, HuJK e collaboratori):
  - [`UniXNU/UniXNU`](https://github.com/UniXNU/UniXNU): `b6ae99c1` (clone locale) e `10edc1ef` (upstream), con
    `docs/drivers/native-adreno.md`, `xnu-drivers.md`, `kexts.md`, `scripts/audit-target-metal-discovery.py`,
    `build-turnip-darwin.py`, `build-adreno-metal-plugin.py`, `patches/xnu/zzw-*`, `zzy-*`.
  - Sottomoduli: **Adreno740** (ex NativeAdreno, MIT; `NativeAdrenoAccelerator.cpp`, `NativeAdrenoABI.h`,
    `NativeAdrenoUserClient.cpp`, `Ring`, `Submit`, `CpInit`, `Gmu*`), **AdrenoMetal** (ex NativeMetal, MIT;
    `NativeAdrenoMetalPlugin.mm`, `NativeBridgeMetalFrontend.mm`, `OriginalAcceleratorABI.h`), **AdrenoFirmware** e
    **XnuPrelink** (BSD-2-Clause-Patent), **AdrenoPM4**, **AirNir**, **ArmSmmu4K** (MIT).
  - Board: `UniXNU-sheng` (`package/GpuPowerProbe.cpp`), `UniXNU-sm8550-common`.
- **metal2vulkan**, steelbrain: [`steelbrain/metal2vulkan`](https://github.com/steelbrain/metal2vulkan),
  GPL-2.0-or-later OR LGPL-3.0-or-later (pin UniXNU/ios-flame `720ef1ce`).
- **Mesa** (freedreno, Turnip, ir3), MIT: `src/freedreno/common/freedreno_devices.py` (`GPUId(640)`, a6xx gen2), pin
  `85fda3b3` di UniXNU/ios-flame.
- **Linux** drm/msm (GPL-2.0): `a6xx_gmu.c`, `a6xx_hfi.c`, `a6xx_gpu.c`, `a6xx_catalog.c`; `drivers/clk/qcom/clk-cpu-osm.c`
  (msm-4.14). Kernel Xiaomi [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) ramo
  `andromeda-p-oss`: `arch/arm64/boot/dts/qcom/sm8150.dtsi`, `sm8150-gpu.dtsi`, `drivers/gpu/msm/`.
- **Altri port citati**:
  - [`Yofuness/ios-sunfish`](https://github.com/Yofuness/ios-sunfish) (A618, Metal);
  - [`nithinyadavamaraboina-online/IOS-26-Hot`](https://github.com/nithinyadavamaraboina-online/IOS-26-Hot)
    (SM8150/A640, `528e45209f`);
  - `RigoLigoRLC/unixnu-adreno6xx` (`86e0f1bd`) e `zzweiqing/UniXNU-nabu` (A640 su iPadOS 17).
- **Nomi dei file firmware** (solo l'elenco, nessun file scaricato):
  [`AndroidBlobs/vendor_xiaomi_andromeda`](https://github.com/AndroidBlobs/vendor_xiaomi_andromeda) (V10.3.19.0.PEMMIXM)
  e [`PixelExperience-Blobs/vendor_xiaomi_andromeda`](https://github.com/PixelExperience-Blobs/vendor_xiaomi_andromeda)
  (ramo `fourteen`).
- **Dati di `andromeda`**: FDT attivo (`phone/<seriale>/live-fdt.dts`), `phone/<seriale>/info/partitions.txt` e
  `getprop.txt`, log `phone/logs/merged-20261106-174054.log`, `board.toml`, `ANDROMEDA.md`, `ROADMAP.md` (tutti fuori
  da git, tranne gli ultimi tre).
